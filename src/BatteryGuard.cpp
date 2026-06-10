// SPDX-License-Identifier: GPL-2.0-or-later

/* Battery-Guard
 *
 * The Battery-Guard combines several functions.
 * - Calculate the battery open circuit voltage, Voltage drop compensation
 * - Calculate the battery internal DC-Pulse resistance
 * - Limit the power drawn from the battery, if the battery voltage is close to the stop threshold.
 * - Help to periodically recharge the battery to 100% SoC, "Recharge Helper"
 *
 * Basic principle of the feature: "Battery open circuit voltage"
 * As soon as we know the battery internal resistance we calculate the open circuit voltage.
 * open circuit voltage = battery voltage - battery current * resistance.
 *
 * Basic principle of the feature: "Battery internal resistance"
 * In general, we try to use steady state values to calculate the battery internal resistance.
 * We always search for 2 consecutive values and build an average for voltage and current.
 * First, we search for a start value.
 * Second, we search for a sufficient current change (trigger).
 * Third, we search for min and max values in a time frame of 15 seconds
 * After the time, we calculate the resistance from the min and max difference.
 * Note: We need high load changes to get sufficient calculation results. About 100W on 24VDC or 180W on 48VDC.
 * The resistance on LifePO4 batteries is not a fixed value, it depends from temperature, charge and time
 * after a load change.
 *
 * Basic principle of the function: "Stop-Voltage Limiter"
 * If the battery voltage is close to the stop threshold, the battery limiter will calculate a maximum power limit
 * to keep the battery voltage above the voltage threshold.
 * The inverter is only switched-off when the threshold is exceeded and the inverter output cannot be reduced any further.
 *
 * Basic principle of the function: "Recharge Helper"
 * After some days we start to increase the battery start/stop thresholds, to make it more easier for the sun to
 * fully charge the battery. When we reach 100% SoC we switch back to the normal thresholds and start a new period.
 * Especially useful during winter to support the battery SoC calibration.
 *
 * 01.08.2024 - 0.1 - first version. "Stop-Voltage Limiter"
 * 09.12.2024 - 0.2 - add of feature "Recharge Helper"
 * 11.12.2024 - 0.3 - add of feature "Battery DC-Puls Resistance" and "Open Circuit Voltage"
 * 14.02.2025 - 0.4 - now works with all battery providers, accept different time stamps for voltage and current
 * 26.09.2025 - 0.5 - improvements "Recharge Helper" and use of shared_mutex to avoid data races
 * 27.10.2025 - 0.6 - "Recharge Helper" supports also SoC values
 * 02.02.2026 - 0.7 - improved logging and error handling, use startup epoch as a fallback for SoC full epoch
 */

#include <frozen/map.h>
#include <battery/Controller.h>
#include <Configuration.h>
#include <Utils.h>
#include <LogHelper.h>
#include "BatteryGuard.h"


// Can be useful if we detect 'Stop-Voltage Limiter' regulation problems
//#define OPTION_CORRECTION_FACTOR


#undef TAG
static const char* TAG = "battery";
static const char* SUBTAG = "Guard";

// runtime data key
static constexpr const char* DC_PULSE_RESISTANCE = "dc_pulse_resistance";
static constexpr const char* DC_PULSE_RESISTANCE_COUNTS = "dc_pulse_resistance_counts";
static constexpr const char* FALLBACK_SOC_EPOCH = "fallback_soc_epoch";

static constexpr frozen::string missing = "programmer error: missing status text";
static constexpr const char* TEXT_NODATA = "No data";
static constexpr float MAXIMUM_VOLTAGE_RESOLUTION = 0.020f;     // 20mV
static constexpr float MAXIMUM_CURRENT_RESOLUTION = 0.200f;     // 200mA
static constexpr float MAXIMUM_MEASUREMENT_TIME_PERIOD = 4000;  // 4 seconds
static constexpr float MAXIMUM_V_I_TIME_STAMP_DELAY = 1000;     // 1 second
static constexpr size_t MINIMUM_RESISTANCE_CALC = 5;            // minimum number of calculations to use the calculated resistance
static constexpr float INVERTER_EFF = 0.95f;                    // inverter efficiency
static constexpr size_t OUTDATED_TIME = 30 * 1000;              // 30 seconds

BatteryGuardClass BatteryGuard; // singleton instance


/*
 * Initialize the battery guard
 */
void BatteryGuardClass::init(Scheduler& scheduler) {

    // init the fast loop
    scheduler.addTask(_fastLoopTask);
    _fastLoopTask.setCallback(std::bind(&BatteryGuardClass::fastLoop, this));
    _fastLoopTask.setIterations(TASK_FOREVER);
    _fastLoopTask.enable();

    // init the slow loop
    scheduler.addTask(_slowLoopTask);
    _slowLoopTask.setCallback(std::bind(&BatteryGuardClass::slowLoop, this));
    _slowLoopTask.setIterations(TASK_FOREVER);
    _slowLoopTask.setInterval(60*1000);
    _slowLoopTask.enable();

    Runtime.registerProvider(this); // read runtime data on startup

    updateSettings(BatteryGuardClass::UpdateSource::STARTUP);
}


/*
 * Update the settings of the 'Battery Guard'. This function should be called, if DPL or battery configuration has changed
 * UpdateSource: source of the update request (default: STARTUP)
 * Hint: We can not use MQTT changeable values here because this function isn't called from MQTT.
 *       MQTT changeable values are only used in the "fastLoop" and "slowLoop".
 */
void BatteryGuardClass::updateSettings(UpdateSource const source) {

    std::unique_lock<std::shared_mutex> lock(_mutex);

    // check if BatteryGuard is enabled and battery provider is configured
    auto const& config = Configuration.get();
    _useBatteryGuard = config.BatteryGuard.Enabled && config.Battery.Enabled;

    // set the "Open Circuit Voltage" state
    _useCurrentCompensation = _useBatteryGuard && config.BatteryGuard.VoltageDropCompensationEnabled;
    _resistanceFromConfig = config.BatteryGuard.InternalResistance / 1000.0f; // mOhm -> Ohm
    if (source != UpdateSource::DPL) {
        _oState = (_useCurrentCompensation) ? OState::START : OState::OFF;
        _lastOCMillis = millis(); // we have 30sec time to calculate the open circuit voltage
    }
    if ((source == UpdateSource::STARTUP) || (source == UpdateSource::BATTERY)) {
        _analyzedResolutionV = 0.10f;
        _analyzedResolutionI = 1.0f;
        _analyzedPeriod.reset(10000);
        _analyzedUIDelay.reset(5000);
    }

    // set the "Stop-Voltage Limiter" state and calculate the lowest "lower power limit"
    // and the sum of the "upper power limit" from all battery powered inverters
    _useStopVoltageLimiter =  _useCurrentCompensation && config.BatteryGuard.LowVoltageLimiterEnabled;
    _lState = (_useStopVoltageLimiter) ? LState::IDLE : LState::OFF;
    if (_lState != LState::OFF && config.BatteryGuard.RechargeHelperEnabled && !config.BatteryGuard.UseVoltageThresholds) {
        _lState = LState::ERROR; // combination of "Stop-Voltage Limiter" and "Recharge Helper" with "SoC thresholds" doesn't make sense
    }
    _lowerPowerLimitOneInverter = 9999;
    _upperPowerLimitFromConfig = 0;
    for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& invConfig = config.PowerLimiter.Inverters[i];
        if (invConfig.Serial == 0ULL) { break; }
        if (!invConfig.IsGoverned) { continue; }
        if (invConfig.PowerSource == PowerLimiterInverterConfig::InverterPowerSource::Battery) {
            _upperPowerLimitFromConfig += invConfig.UpperPowerLimit;
            if (invConfig.LowerPowerLimit < _lowerPowerLimitOneInverter) { _lowerPowerLimitOneInverter = invConfig.LowerPowerLimit; }
        }
    }
    if (_lowerPowerLimitOneInverter == 9999) { _lowerPowerLimitOneInverter = 50; }    // default value
    if (_upperPowerLimitFromConfig == 0) { _upperPowerLimitFromConfig = 800; }      // default value
    _stopLimitPower = gUpperPowerLimitUsed();

    // set the "Recharge Helper" state
    _useRechargeHelper = _useBatteryGuard && config.BatteryGuard.RechargeHelperEnabled;
    if ((source != UpdateSource::BATTERY)) {
        if (_useRechargeHelper) {
            _hState = HState::START;
        } else {
            resetRechargeHelper();
            _hState = HState::OFF;
        }
    }

    // check if the "Stop-Voltage Limiter" was already active before
    if (_limitActive) {
        _limitStartTime.tm_year = 0;
        _limitStopTime.tm_year = 0;
        _limitActive = false;
        _insideLimitArea = false;
    }
}


/*
 * Normal loop, fetches the battery values (voltage, current and measurement time stamp) from the battery provider
 */
void BatteryGuardClass::fastLoop(void) {

    if ((!_useBatteryGuard) || !Battery.getStats()->isVoltageValid() || !Battery.getStats()->isCurrentValid()) {
        return; // not enabled or stats not valid, we abort
    }

    // read external state without holding our mutex
    auto const u2Value = Battery.getStats()->getVoltage();
    auto const u2Time = Battery.getStats()->getLastVoltageUpdate();
    auto const i2Value = Battery.getStats()->getChargeCurrent();
    auto const i2Time = Battery.getStats()->getLastCurrentUpdate();
    auto const nowSoC = Battery.getStats()->getSoC();

    std::unique_lock<std::shared_mutex> lock(_mutex);

    if ( (_u1Data.timeStamp == u2Time) && (_i1Data.timeStamp == i2Time) ) { return; } // same time stamp again, we abort

    if (u2Time == i2Time) {
        // the simple handling: voltage und current have the same time stamp
        _i1Data = { i2Value, i2Time, true };
        _u1Data = { u2Value, u2Time, true };
        _analyzedUIDelay.addNumber(0.0f); // first shared data
        updateBatteryValues(u2Value, i2Value, i2Time, nowSoC);
        return;
    }

    // the special handling: voltage and current time stamp are different
    // Note: In worst case, this will add a delay of 1 measurement period
    if (i2Time != _i1Data.timeStamp) {

        // check if new U1 data is available and check if we have to use U1 or U2
        Data uxData = _u1Data;
        if ((u2Time != _u1Data.timeStamp) && ((millis() - u2Time) > (millis() - i2Time))) {
            uxData = { u2Value, u2Time, true };
        }

        if (uxData.valid && _i1Data.valid) {

            // check if Ux time stamp is closer to I1 or I2 time stamp
            if ((i2Time - uxData.timeStamp) < (uxData.timeStamp - _i1Data.timeStamp)) {
                _analyzedUIDelay.addNumber(static_cast<float>(i2Time - uxData.timeStamp));
                updateBatteryValues(uxData.value, i2Value, uxData.timeStamp, nowSoC); // we use the older time stamp
            } else {
                _analyzedUIDelay.addNumber(-static_cast<float>(uxData.timeStamp - _i1Data.timeStamp));
                updateBatteryValues(uxData.value, _i1Data.value, _i1Data.timeStamp, nowSoC); // we use the older time stamp
            }
            _u1Data.valid = false;
        }

        _i1Data = { i2Value, i2Time, true }; // store the next I1 data
    }

    if (u2Time != _u1Data.timeStamp) { _u1Data = { u2Value, u2Time, true }; } // store the next U1 data
}


/*
 * Slow periodical tasks, will be called once a minute
 */
void BatteryGuardClass::slowLoop(void) {

    if (!_useBatteryGuard ) { return; } // fast exit to avoid locking

    if (_useCurrentCompensation || _useRechargeHelper) {

        // read external state without holding our mutex
        auto epochFull = Battery.getStats()->getSoCFullEpoch().value_or(0);
        time_t epochNow;
        if (!Utils::getEpoch(&epochNow, 5)) { epochNow = 0 ; }

        std::unique_lock<std::shared_mutex> lock(_mutex);

        if (_useCurrentCompensation) {
            if ((millis() - _lastOCMillis) > OUTDATED_TIME) {
                _openCircuitVoltageAVG.reset();
                _oState = OState::ERROR;
                if (_useStopVoltageLimiter) { _lState = LState::ERROR; }
            }
        }

        if (_useRechargeHelper) {
            calculateRechargeHelper(epochFull, epochNow);
        }

    } // end of unique lock

    if (DTU_LOG_IS_DEBUG) {

        std::shared_lock<std::shared_mutex> lock(_mutex);

        DTU_LOGD("");
        DTU_LOGD("------------- Battery Guard Report (every minute) -------------");
        printCompensationReport();
        printLimiterReport();
        printRechargeReport();
        DTU_LOGD("---------------------------------------------------------------");
        DTU_LOGD("");
    } // end of shared lock
}



// Open Circuit Voltage | Open Circuit Voltage | Open Circuit Voltage | Open Circuit Voltage | Open Circuit Voltage | Open Circuit Voltage

/*
 * Update the battery guard with new values from the battery provider. (voltage[V], current[A], millisStamp[ms])
 * Note: Just call the function if new values are available. Current flow into the battery must be positive.
 */
void BatteryGuardClass::updateBatteryValues(float const volt, float const current, uint32_t const millisStamp, float const nowSoC ) {
    if (volt <= 0.0f) { return; }

    // analyse the measurement period
    if ((_battMillis != 0) && (millisStamp > _battMillis)) {
        _analyzedPeriod.addNumber(millisStamp - _battMillis);
    }

    // analyse the voltage and current resolution
    auto resolution = std::abs(volt - _battVoltage);
    if ((resolution >= 0.001f) && (resolution < _analyzedResolutionV)) { _analyzedResolutionV = resolution; }
    resolution = std::abs(current - _battCurrent);
    if ((resolution >= 0.001f) && (resolution < _analyzedResolutionI)) { _analyzedResolutionI = resolution; }

    // store the values
    // Note: 'Voltage Drop Compensation', 'Stop Voltage Limiter' and the 'Recharge Helper' use this data
    _battMillis = millisStamp;
    _battVoltage = volt;
    _battCurrent = current;
    _battVoltageAVG.addNumber(_battVoltage);

    if (_useCurrentCompensation) {
        calculateResistance(_battVoltage, _battCurrent, nowSoC);
        calculateOpenCircuitVoltage(_battVoltage, _battCurrent);
    }
}


/*
 * Calculate the battery open circuit voltage.
 */
void BatteryGuardClass::calculateOpenCircuitVoltage(float const nowVoltage, float const nowCurrent) {

    // Resistance must be present and the current flow into the battery must have a positive sign
    auto oResistor = gResistanceUsed();
    if (oResistor.has_value()) {
        _openCircuitVoltageAVG.addNumber(nowVoltage - nowCurrent * oResistor.value());
        _lastOCMillis = millis();
        _oState = OState::ACTIVE;
    } else {
        _oState = OState::ERROR;
    }
}


/*
 * The battery open circuit voltage or nullopt if value is not valid or not enabled
 */
std::optional<float> BatteryGuardClass::getOpenCircuitVoltage(void) const {
    if (!_useCurrentCompensation) { return std::nullopt; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);
    return gOpenCircuitVoltage();
}


/*
 * The battery open circuit voltage or nullopt if value is not valid or not enabled
 */
std::optional<float> BatteryGuardClass::gOpenCircuitVoltage(void) const {
    if ((_openCircuitVoltageAVG.getCounts() > 0) && ((millis() - _lastOCMillis) <= OUTDATED_TIME)) {
        return _openCircuitVoltageAVG.getAverage();
    }
    return std::nullopt;
}


/*
 * True if the measurement resolution, period and time delay between voltage and current is sufficient
 * Requirement: Voltage <= 20mV, Current <= 200mA, Period <= 4s, Time delay <= 1s
 */
bool BatteryGuardClass::isResolutionOK(void) const {
    return (_analyzedResolutionV <= MAXIMUM_VOLTAGE_RESOLUTION)
        && (_analyzedResolutionI <= MAXIMUM_CURRENT_RESOLUTION)
        && (_analyzedPeriod.getAverage() <= MAXIMUM_MEASUREMENT_TIME_PERIOD)
        && (std::abs(_analyzedUIDelay.getAverage()) <= MAXIMUM_V_I_TIME_STAMP_DELAY);
}



// DC-Pulse Resistance | DC-Pulse Resistance | DC-Pulse Resistance | DC-Pulse Resistance | DC-Pulse Resistance | DC-Pulse Resistance

/*
 * Calculate the battery DC-Pulse-Resistance based on the voltage measurement position of the battery provider.
 * Note: The calculation will not work, if the performance of the battery provider is not good enough
 * or if the power difference is not big enough or too slow.
 */
void BatteryGuardClass::calculateResistance(float const nowVoltage, float const nowCurrent, float const nowSoC) {

    // lambda function to avoid nested if-else statements and code doubling
    auto cleanExit = [&](RState state) -> void {

        if (_lastResistanceLogState == state) { return; } // no change, we abort without logging
        _lastResistanceLogState = state;
        DTU_LOGV("Resistance calculation state: %s", gResistanceStateText(state).data());
        if (state > _rStateMax) { _rStateMax = state; }
    };

    // check the resolution and the calculation frequency
    if (!isResolutionOK()) { return cleanExit(RState::RESOLUTION); }
    if ((millis() - _lastDataInMillis) < 900 ) { return cleanExit(RState::TIME); }
    _lastDataInMillis = millis();
    if (!_minMaxAvailable) { _rState = RState::IDLE; }

    // Check if we are in a useable voltage or SoC range to make the resistance calculation meaningful
    if ((nowSoC <= 15.0f) || nowSoC >= 90.0f) {
        if ((nowVoltage <= Configuration.get().PowerLimiter.VoltageStopThreshold)
            || (nowVoltage >= Configuration.get().Battery.NominalVoltage * 1.02f)) {
            return cleanExit(RState::OUTOF_RANGE);
        }
    }

    // check for the trigger event (sufficient current change)
    auto const minDiffCurrent = 4.0f; // seems to be a good value for all battery providers
    if (!_triggerEvent && _minMaxAvailable && (std::abs(nowCurrent - _pMinVolt.second) > (minDiffCurrent/2.0f))) {
        _lastTriggerMillis = millis();
        _triggerEvent = true;
        _rState = RState::TRIGGER;
    }

    // we evaluate min and max values in a time duration of 15 sec after the trigger event
    if (!_triggerEvent || (_triggerEvent && (millis() - _lastTriggerMillis) < 15*1000)) {

        // we use the measurement resolution to decide if two consecutive values are almost identical
        auto minVoltage = (_triggerEvent) ? 0.2f : std::max(_analyzedResolutionV * 3.0f, 0.01f);
        auto minCurrent = std::max(_analyzedResolutionI * 3.0f, 0.2f);

        // after the first-pair-after-the-trigger, we check if the current is stable
        // if the current is not stable we break the calculation because we have again a power transition
        // which influences the quality of the calculation
        if (_pairAfterTriggerAvailable && (std::abs(_checkCurrent - nowCurrent) > minCurrent)) {
            _firstOfTwoAvailable = false;
            _minMaxAvailable = false;
            _triggerEvent = false;
            _pairAfterTriggerAvailable = false;
            return cleanExit(RState::SECOND_BREAK);
        }

        // we must avoid to use measurement values during any power transitions
        // to solve this problem, we check whether two consecutive measurements are almost identical
        if (_firstOfTwoAvailable && (std::abs(_pFirstVolt.first - nowVoltage) <= minVoltage) &&
            (std::abs(_pFirstVolt.second - nowCurrent) <= minCurrent)) {

            auto avgVolt = std::make_pair((nowVoltage+_pFirstVolt.first)/2.0f, (nowCurrent+_pFirstVolt.second)/2.0f);
            if (!_minMaxAvailable || !_triggerEvent) {
                _pMinVolt = _pMaxVolt = avgVolt;
                _minMaxAvailable = true;
                _rState = RState::FIRST_PAIR; // we have the first pair (before the trigger event)
            } else {
                if (avgVolt.first < _pMinVolt.first) { _pMinVolt = avgVolt; }
                if (avgVolt.first > _pMaxVolt.first) { _pMaxVolt = avgVolt; }
                _pairAfterTriggerAvailable = true;
                _checkCurrent = nowCurrent;
                _rState = RState::SECOND_PAIR; // we have the second pair (after the trigger event)
            }
        }
        _pFirstVolt = { nowVoltage, nowCurrent }; // preparation for the next two consecutive values
        _firstOfTwoAvailable = true;
        return cleanExit(_rState);
    }

    // reset conditions for the next calculation
    _firstOfTwoAvailable = false;
    _minMaxAvailable = false;
    _triggerEvent = false;
    _pairAfterTriggerAvailable = false;

    // now we have minimum and maximum values and we can try to calculate the resistance
    // we need a minimum power difference to get a sufficiently good result (failure < 20%)
    // SmartShunt: 40mV and 4A (about 100W on VDC=24V, Ri=12mOhm)
    auto minDiffVoltage = std::max(_analyzedResolutionV * 5.0f, 0.04f);
    auto diffVolt = _pMaxVolt.first - _pMinVolt.first;
    auto diffCurrent = std::abs(_pMaxVolt.second - _pMinVolt.second);   // can be negative
    if ((diffVolt >= minDiffVoltage) && (diffCurrent >= minDiffCurrent)) {
        float resistor = diffVolt / diffCurrent;
        auto reference = (_resistanceFromConfig != 0.0f) ? _resistanceFromConfig : _resistanceFromCalcAVG.getAverage();
        if ((reference != 0.0f) && ((resistor > reference * 2.0f) || (resistor < reference / 2.0f))) {
            _rState = RState::TOO_BAD; // safety feature: we try to keep out bad values from the average
        } else {
            _resistanceFromCalcAVG.addNumber(resistor);
            _rState = RState::CALCULATED;
        }
    } else {
        _rState = RState::DELTA_POWER;
    }

    return cleanExit(_rState);
}


/*
 * The calculated battery internal resistance or nullopt if value is not valid
 */
std::optional<float> BatteryGuardClass::getCalculatedResistance(void) const {
    if (!_useCurrentCompensation) { return std::nullopt; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);

    // we use the calculated value if we have 5 valid calculations minimum
    if (_resistanceFromCalcAVG.getCounts() >= MINIMUM_RESISTANCE_CALC) {
        return _resistanceFromCalcAVG.getAverage();
    }
    return std::nullopt;
}


/*
 * The battery internal resistance, calculated or configured or nullopt if neither value is valid
 */
std::optional<float> BatteryGuardClass::gResistanceUsed(void) const {

    // we use the calculated value if we have 5 calculations minimum
    if (_useCurrentCompensation) {
        if (_resistanceFromCalcAVG.getCounts() >= MINIMUM_RESISTANCE_CALC) { return _resistanceFromCalcAVG.getAverage(); }
        if (_resistanceFromConfig > 0.0f) { return _resistanceFromConfig; } // Changed condition to strictly greater than 0.0f
    }
    return std::nullopt;
}


/*
 * Prepare data to be written into the runtime file
 */
void BatteryGuardClass::serializeRT(JsonObject obj) const {
    std::shared_lock<std::shared_mutex> lock(_mutex);

    // DC-Pulse Resistance
    obj[DC_PULSE_RESISTANCE] = _resistanceFromCalcAVG.getAverage();
    auto counts = static_cast<uint16_t>(_resistanceFromCalcAVG.getCounts());
    auto factor = static_cast<uint16_t>(_resistanceFromCalcAVG.getFactor());
    if (counts > factor) { counts = factor; } // limit the counts to the factor
    obj[DC_PULSE_RESISTANCE_COUNTS] = counts;

    // Recharge Helper, SoC fallback time
    obj[FALLBACK_SOC_EPOCH] = _fallbackSoCEpoch;
}


/*
 * Read the data from the runtime file
 */
void BatteryGuardClass::deserializeRT(JsonObject obj) {

    // if runtime data is not available, we exit and use the initialization values
    // This can happen, if the device is started for the first time or if the runtime file is corrupted
    if (obj.isNull()) { return; }

    std::unique_lock<std::shared_mutex> lock(_mutex);

    // DC-Pulse Resistance
    float resistance =  obj[DC_PULSE_RESISTANCE] | 0.0f;
    uint16_t counts = obj[DC_PULSE_RESISTANCE_COUNTS] | 0;
    _resistanceFromCalcAVG.reset();
    if (resistance != 0.0f) {
        for (uint16_t idx = 0; idx < counts; ++idx) {
            _resistanceFromCalcAVG.addNumber(resistance);
        }
    }

    // Recharge Helper, SoC fallback time
    _fallbackSoCEpoch = obj[FALLBACK_SOC_EPOCH] | 0L;
}


/*
 * Prints the "Voltage Drop Compensation" report
 */
void BatteryGuardClass::printCompensationReport(void) const {
    DTU_LOGD("");
    DTU_LOGD("Quality of the battery data: %s", (isResolutionOK()) ? "Sufficient" : "Not sufficient");
    DTU_LOGD("Voltage resolution: %0.0fmV, Current resolution: %0.0fmA",
        _analyzedResolutionV * 1000.0f, _analyzedResolutionI * 1000.0f);

    DTU_LOGD("Measurement period: %0.0fms, V-I time stamp delay: %0.0fms",
        _analyzedPeriod.getAverage(), _analyzedUIDelay.getAverage());
    DTU_LOGD("");

    DTU_LOGD("Voltage Drop Compensation: %s", (_useCurrentCompensation) ? "Enabled" : "Disabled");
    if (_useCurrentCompensation) {
        DTU_LOGD("State: %s", gOpenVoltageStateText(_oState).data());
        DTU_LOGD("Open circuit voltage: %0.3fV, Actual battery voltage: %0.3fV", _openCircuitVoltageAVG.getAverage(), _battVoltage);

        auto oResistance = gResistanceUsed();
        if (!oResistance.has_value()) {
            DTU_LOGD("Resistance neither calculated (5 times) nor configured");
        } else {
            auto resCalc = (_resistanceFromCalcAVG.getCounts() >= MINIMUM_RESISTANCE_CALC) ?
                _resistanceFromCalcAVG.getAverage() * 1000.0f : 0.0f;
            DTU_LOGD("Resistance in use: %0.1fmOhm [Calculated: %0.1fmOhm, Configured: %0.1fmOhm]",
                oResistance.value() * 1000.0f, resCalc, _resistanceFromConfig * 1000.0f);
        }

        DTU_LOGD("Resistance calc.: %0.1fmOhm_avg [Min: %0.1fmOhm, Max: %0.1fmOhm, Amount: %i]",
            _resistanceFromCalcAVG.getAverage()*1000.0f, _resistanceFromCalcAVG.getMin()*1000.0f,
            _resistanceFromCalcAVG.getMax()*1000.0f, _resistanceFromCalcAVG.getCounts());

        DTU_LOGD("Resistance calculation state: %s", gResistanceStateText(_rStateMax).data());
    }
}


/*
 * Returns a string according to current text number
 */
frozen::string const& BatteryGuardClass::getQualityText(BatteryGuardClass::QText tNr) const {
    static const frozen::map<QText, frozen::string, 5> texts = {
        { QText::NODATA, "No data" },
        { QText::EXCELLENT, "Excellent" },
        { QText::GOOD, "Good" },
        { QText::BAD, "Bad" },
        { QText::VERYBAD, "Very bad" }
    };

    auto iter = texts.find(tNr);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}


/*
 * Returns a string according to resistance calculation state
 */
frozen::string const& BatteryGuardClass::gOpenVoltageStateText(BatteryGuardClass::OState tNr) const {
    static const frozen::map<OState, frozen::string, 4> texts = {
        { OState::OFF, "Off" },
        { OState::START, "Starting-Up, wait..." },
        { OState::ERROR, "Error" },
        { OState::ACTIVE, "Active" }
    };

    auto iter = texts.find(tNr);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}


/*
 * Returns a string according to resistance calculation state
 */
frozen::string const& BatteryGuardClass::gResistanceStateText(BatteryGuardClass::RState tNr) const {
    static const frozen::map<RState, frozen::string, 11> texts = {
        { RState::IDLE, "Idle" },
        { RState::RESOLUTION, "Battery data insufficient" },
        { RState::OUTOF_RANGE, "Voltage or SoC out of range" },
        { RState::TIME, "Measurement time too fast" },
        { RState::FIRST_PAIR, "Start data available" },
        { RState::TRIGGER, "Trigger event" },
        { RState::SECOND_PAIR, "Collecting data after trigger" },
        { RState::SECOND_BREAK, "Second power change after trigger" },
        { RState::DELTA_POWER, "Power difference not high enough" },
        { RState::TOO_BAD, "Resistance out of safety range" },
        { RState::CALCULATED, "Resistance calculated" }
    };

    auto iter = texts.find(tNr);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}



// Low Voltage Power Limiter | Low Voltage Power Limiter | Low Voltage Power Limiter | Low Voltage Power Limiter | Low Voltage Power Limiter

/*
 * Returns true if the battery voltage is below the voltage stop threshold.
 * inStateStop: true if current battery state is stop
 * Returns nullopt if the "Stop-Voltage Limiter" is disabled or we are in an error state and can not check the voltage.
 * We check two conditions:
 * (1) "battery voltage is equal or below the stop threshold" and "battery power limit is equal the lower power limit"
 * (2) "battery voltage is equal or below the stop threshold" for more as 2 minutes
 */
std::optional<bool> BatteryGuardClass::isStopThresholdReached(bool const inStateStop) {

    if (!_useStopVoltageLimiter) { return std::nullopt; } // fast exit to avoid locking

    std::unique_lock<std::shared_mutex> lock(_mutex);

    auto stopThreshold = gVoltageStopThresholdUsed();
    if ((_lState == LState::OFF)
        || (_lState == LState::ERROR)
        || !isBatteryVoltageValid()
        || (stopThreshold <= 0.0f)) { return std::nullopt; } // we abort, voltage based limiting is not possible

    bool stopReached = false;
    bool emergencyStop = false;
    auto avgOCVoltage = gOpenCircuitVoltage();
    auto avgVoltage = avgOCVoltage.has_value() ? avgOCVoltage.value() : _battVoltageAVG.getAverage();

    // we use the average voltage and the instantaneous voltage to filter out short voltage drops
    if ((avgVoltage > stopThreshold) || (_battVoltage > stopThreshold)) {
        _lastAboveStopMillis = millis();
    } else {

        // we switch the inverter off when the power limit cannot be reduced further.
        // Note: It takes some time from the activation of the new inverter limit until we receive a
        // new valid battery voltage. we give the loop 15 seconds to react
        if ((_lState == LState::LIMIT_MAX) && (15*1000 < (millis() - _lastAboveStopMillis))) {
            stopReached = true;
        }

        // safety feature: We also stop if the battery voltage is below the stop threshold for more than 2 minutes
        // regardless of the actual inverter power limit, e.g. if we cannot reduce power for any reason
        if (2*60*1000 < (millis() - _lastAboveStopMillis)) {
            if (_lState != LState::IDLE ) { emergencyStop = true; } // we are not in IDLE, we have to record an emergency stop
            stopReached = true;
        }
    }

    // update values only during battery state change to avoid retriggering,
    // because values ​​must be buffered up to the next stop event
    if (stopReached && !inStateStop) {
        getLocalTime(&_stopTime, 5);
        _lastStopState = (emergencyStop) ? LState::STOP_EMERGENCY : LState::STOP_NORMAL;
        DTU_LOGI("State: %s, Average battery voltage: %0.3fV, Battery voltage: %0.3fV",
            gLimiterStateText(_lState).data(), avgVoltage, _battVoltage);

        _lState = _lastStopState;
    }

    return stopReached;
}


/*
 * Returns the maximum possible power based on the features 'Stop-Voltage Limiter' and 'Recharge Helper'
 * requestedPower:  power we want to draw next from the battery + charger [W]
 * nowPower:        power we draw right now from the battery + charger [W]
 * nowMillis:       last time stamp of adjusting the inverter limit [millis()]
 * dischargeOk:     battery discharge allowed
 */
uint16_t BatteryGuardClass::calculatePowerLimit(uint16_t const requestedPower, uint16_t const nowPower, uint32_t const nowMillis) {

    if (!_useRechargeHelper && !_useStopVoltageLimiter) { return requestedPower; } // fast exit to avoid locking

    std::unique_lock<std::shared_mutex> lock(_mutex);

    auto backPower = requestedPower;

    // recharge helper: check if we have to limit the power
    if (_useRechargeHelper && _oPowerLimit.has_value()) {
        backPower = std::min(backPower, _oPowerLimit.value());
    }

    // low voltage power limiter: check if we have to limit the power
    if (_useStopVoltageLimiter) {
        backPower = calculateStopVoltagePowerLimit(backPower, nowPower, nowMillis);
    }
    return backPower;
}


/*
 * Returns the maximum possible inverter power before the battery voltage falls below the "battery stop threshold"
 * or the "requested power". Whichever is lower. The calculation includes also the power from the solar charger.
 * requestedPower:  power we want to draw next from the battery + charger [W]
 * nowPower:        power we draw right now from the battery + charger [W]
 * nowMillis:       time stamp of adjusting the inverter limit [millis()]
 */
uint16_t BatteryGuardClass::calculateStopVoltagePowerLimit(uint16_t const requestedPower, uint16_t const nowPower, uint32_t const lastMillis) {

    auto stopVoltage = gVoltageStopThresholdUsed();
    auto upperPowerLimit = gUpperPowerLimitUsed();
    auto lastLimit = _stopLimitPower; // keep the last limit


    // we use a lambda function to avoid nested if-else statements
    auto cleanUp = [&](uint16_t backPower, LState const newState, bool quality) -> uint16_t {

        // _insideLimitArea | _limitActive  |  Description
        // ----------------------------------------------------------------------------------
        //      false       |     false     |  Limit area not reached and limiting not active
        //      true        |     false     |  Limit area reached but limiting not active
        //      true        |     true      |  Limit area reached and limiting active
        bool lastLimitAreaReached = _insideLimitArea;
        _insideLimitArea = (_stopLimitPower < upperPowerLimit) ? true : false;
        if (!lastLimitAreaReached && _insideLimitArea) {
            getLocalTime(&_limitStartTime, 5); // we start limiting
            _limitStopTime.tm_year = 0;
        }
        if (lastLimitAreaReached && !_insideLimitArea) {
            getLocalTime(&_limitStopTime, 5);  // we stop limiting
        }
        _limitActive = (backPower < requestedPower) ? true : false;

        // we do not modify the requested power, if the power is already below the lower power limit
        // to avoid problems with other power regulation strategies
        if (requestedPower < _lowerPowerLimitOneInverter) { backPower = requestedPower; }

        // handle the regulation quality and the safety counter
        if (quality) { handleQualityCounter(); }

        // we just log if "state has changed" or "enough time (10s) has passed"
        if ((newState != _lState) || ((millis() - _lastPrint) >= 10 * 1000)) {
            _lastPrint = millis();
            DTU_LOGD("State: %s, Allowed power %iW, [Limit: %iW, Requested: %iW]",
                gLimiterStateText(newState).data(), backPower, _stopLimitPower, requestedPower);

            DTU_LOGV("Battery voltage: %0.3fV, Distance to target: %0.0fmV, Target range: [%0.3fV-%0.3fV]", _battVoltage,
                (_battVoltage - stopVoltage - _halfOfTargetRange) * 1000.0f, stopVoltage, stopVoltage + 2 * _halfOfTargetRange);
        }

        _lState = newState;
        return backPower;
    };


    // if the requested power is 0W, we can stop any limiting activity
    if (requestedPower == 0) {
        if (_lState == LState::STOP_NORMAL || _lState == LState::STOP_EMERGENCY) {
            _stopLimitPower = upperPowerLimit;
            return cleanUp(requestedPower, LState::IDLE, true); // we stopped before, now we can go to IDLE
        }
        return cleanUp(requestedPower, _lState, false); // nothing to do
    }

    // we need a sufficient resolution a valid battery voltage and resistance to calculate a power limit
    if (!isResolutionOK() || !isBatteryVoltageValid() || !gResistanceUsed().has_value()) {
        return cleanUp(requestedPower, LState::ERROR, false);
    }

    // We must wait for new battery voltage readings after adjusting the inverter limit.
    // In case of outdated values we use the last limit to avoid false limit calculation and to save processing time
    // To avoid the millis() rollover problem we only compare time durations and not points in time.
    auto nowMillis = millis();
    if ((nowMillis - _battMillis) > (nowMillis - lastMillis + _analyzedPeriod.getAverage())) {
        if ((_lState == LState::IDLE) || (_lState == LState::ERROR)) {
            return cleanUp(requestedPower, _lState, false);
        } else {
            return cleanUp(std::min(requestedPower, _stopLimitPower), LState::OUTDATED_MEASUREMENT, false);
        }
    }

    // the minimum size of the target voltage range depends on the resolution the hysteresis and the resistance
    _halfOfTargetRange = std::max(_analyzedResolutionV * 1.5f, 0.01f);
    auto resistor = gResistanceUsed().value();
    float minTarget = Configuration.get().PowerLimiter.TargetPowerConsumptionHysteresis / _battVoltage * resistor;
    _halfOfTargetRange = std::max(_halfOfTargetRange, minTarget);


    // handle the regulation quality
    if (_qualityCounter > 0) {
        _qualityTarget.addNumber(_battVoltage - stopVoltage - _halfOfTargetRange);

        // correction factor: we calculate a new value based on the last regulation attempt
        #ifdef OPTION_CORRECTION_FACTOR
        if (_qualityCounter == 1) {
            float CF = _cfHelp1 / (_cfHelp2 * (_battVoltage - _cfHelp2));
            if ((CF >= 0.5f) && (CF <= 2)) { _cfAVG.addNumber(CF); }

            DTU_LOGV("Correction Factor: Average: %0.3f (Min: %0.3f, Max: %0.3f, Last: %0.3f, Amount: %i)",
                _cfAVG.getAverage(), _cfAVG.getMin(), _cfAVG.getMax(), _cfAVG.getLast(), _cfAVG.getCounts());
        }
        #endif
    }

    auto checkV = _battVoltage; // we use the instantaneous voltage for the limit calculation
    if (!_limitActive || (_limitActive && ((checkV <= stopVoltage) || (checkV >= (stopVoltage + _halfOfTargetRange * 2.0f))))) {

        // we calculate the new power limit based on the current inverter AC power, the battery power and the charger power
        auto batteryDischargePower = _battVoltage * -_battCurrent; // discharge current is negative
        auto chargerPower = std::max(static_cast<float>(nowPower) / INVERTER_EFF - batteryDischargePower, 0.0f);
        auto batteryCellVoltage = _battVoltage - _battCurrent * resistor;
        auto newBatteryCurrent = (batteryCellVoltage - (stopVoltage + _halfOfTargetRange)) / resistor;

        // a negative value here means we must charge the battery to reach the target voltage
        auto newBatteryPower = (stopVoltage + _halfOfTargetRange) * newBatteryCurrent;

        auto stopLimitPower = (chargerPower + newBatteryPower) * INVERTER_EFF;
        if (stopLimitPower < _lowerPowerLimitOneInverter) {
            _stopLimitPower = _lowerPowerLimitOneInverter;
        } else if (stopLimitPower > upperPowerLimit) {
            _stopLimitPower = upperPowerLimit;
        } else {
            _stopLimitPower = static_cast<uint16_t>(stopLimitPower); // only convert from float into uint16_t inside the valid range
        }

        // correction factor: preparing values for the next calculation just makes sense if we don't modified the calculated power value
        #ifdef OPTION_CORRECTION_FACTOR
        _cfHelp1 = (newBatteryPower - _stopLimitPower) * resistor / INVERTER_EFF;
        _cfHelp2 = checkV;
        #endif
    }

    // safety feature: if we are still below the stop voltage after 3 regulation attempts, we use the lowest power value
    if (_safetyCounter >= 3) {
        _stopLimitPower = _lowerPowerLimitOneInverter;
        _safetyHitsCounter++;
    }

    // now we have all necessary data to determine the next limit
    // we have to consider 7 different conditions

    // 1. No need to limit the battery power. We can leave the limiter
    if (_stopLimitPower >= upperPowerLimit) {
        return cleanUp(requestedPower, LState::IDLE, true);
    }

    // 2. We use the maximum limit, more limiting is not possible
    if (_stopLimitPower <= _lowerPowerLimitOneInverter) {
        return cleanUp(_stopLimitPower, LState::LIMIT_MAX, true);
    }

    // 3. The requested power is below the actual limit
    if (_stopLimitPower > requestedPower) {
        return cleanUp(requestedPower, LState::REQUESTED_POWER_BELOW_LIMIT, true);
    }

    // 4. We can keep the last limit
    if (_stopLimitPower == lastLimit) {
        return cleanUp(_stopLimitPower, LState::LIMITING_TARGET, true);
    }

    // 5. We must reduce the limit
    if (_stopLimitPower < lastLimit) {
        _safetyCounter++;   // safety feature: we count the attemps to reach a voltage above the stop threshold
        _qualityCounter++;  // quality feature: we count the necessary regulation steps to reach the target
        _lastStepDownTime = nowMillis;
        return cleanUp(_stopLimitPower, LState::LIMITING_DOWN, false);
    }

    // 6. We can increase the limit but we have to wait until the end of the 30 sec timeout
    if ((_stopLimitPower > lastLimit) && ((nowMillis - _lastStepDownTime) <= 30 * 1000)) {
        _stopLimitPower = lastLimit;
        return cleanUp(_stopLimitPower, LState::LIMITING_TIMEOUT, true);
    }

    // 7. We can increase the limit. Note: we only reach this line if the timeout is over
    _qualityCounter++;  // quality feature: we count the necessary regulation steps to reach the target
    return cleanUp(_stopLimitPower, LState::LIMITING_UP, false);
}


/*
 * Check if the regulation quality counter must be added to the quality statistic
 */
void BatteryGuardClass::handleQualityCounter(void) {
    if (_qualityCounter != 0) {
        _qualityAVG.addNumber(_qualityCounter);
        _qualityCounter = 0;
    }

    // stores the maximum safety counter
    if (_safetyCounter > _safetyCounterMax) { _safetyCounterMax = _safetyCounter; }
    _safetyCounter = 0; // we can reset the safety counter
}


/*
 * Returns a string according to the actual limiter quality
 */
const char*  BatteryGuardClass::gLimiterQuality(void) const {
    if (_safetyHitsCounter > 5) { return getQualityText(QText::VERYBAD).data(); }

    auto qualityAVG = _qualityAVG.getAverage();
    QText quality = QText::BAD;
    if (qualityAVG == 0.0f) quality = QText::NODATA;
    if ((qualityAVG > 0.0f) && (qualityAVG <= 1.2f)) quality = QText::EXCELLENT;
    if ((qualityAVG > 1.2f) && (qualityAVG <= 2.0f)) quality = QText::GOOD;
    return getQualityText(quality).data();
}


/*
 * Returns a string according to the active time of the limiter
 */
String  BatteryGuardClass::gLimiterTime(void) const {
    String result;
    result.reserve(32);
    if (_limitStartTime.tm_year < (2000 - 1900)) {
        result.concat(TEXT_NODATA);
    } else {
        char time[32];
        strftime(time, sizeof(time), "%d-%h %H:%M", &_limitStartTime);
        result.concat(time);
        if (_limitStopTime.tm_year < (2000 - 1900)) {
            result.concat(" - ongoing");
        } else {
            strftime(time, sizeof(time), " - %H:%M", &_limitStopTime);
            result.concat(time);
        }
    }
    return result;
}


/*
 * Returns a string according to the stop time of the limiter
 */
String BatteryGuardClass::gDischargeStopTime(void) const {
    String text;

    if (_stopTime.tm_year < (2000 - 1900)) {
        text = TEXT_NODATA;
    } else {
        char time[32];
        strftime(time, sizeof(time), "%d-%h %H:%M", &_stopTime);
        text = time;
    }
    return text;
}


/*
 * Returns the currently valid upper power limit.
 * The upper power limit sum from all battery powered inverters or the DPL configured value, whichever is lower.
 */
uint16_t BatteryGuardClass::gUpperPowerLimitUsed(void) const {
    return std::min(_upperPowerLimitFromConfig, Configuration.get().PowerLimiter.TotalUpperPowerLimit);
}


/*
 * Returns the currently valid battery voltage stop threshold.
 * From the 'Recharge Helper' or from the DPL configuration
 */
float BatteryGuardClass::gVoltageStopThresholdUsed(void) const {
    return _oVoltageStopThreshold.has_value() ? _oVoltageStopThreshold.value() : Configuration.get().PowerLimiter.VoltageStopThreshold;
}


/*
 * Prints the "Low Voltage Power Limiter" report
 */
void BatteryGuardClass::printLimiterReport(void)
{
    DTU_LOGD("");
    DTU_LOGD("Stop-Voltage Limiter: %s", (_useStopVoltageLimiter) ? "Enabled" : "Disabled");
    if (_useStopVoltageLimiter) {
        DTU_LOGD("State: %s", gLimiterStateText(_lState).data());
        DTU_LOGD("Limiting active: %s", _limitActive ? "Yes" : "No");
        DTU_LOGD("Inside limiting area: %s", _insideLimitArea ? "Yes" : "No");
        DTU_LOGD("Actual limit: %iW [%iW-%iW]", _stopLimitPower, _lowerPowerLimitOneInverter, gUpperPowerLimitUsed());
        DTU_LOGD("Regulation quality: %s", gLimiterQuality());
        DTU_LOGD("Regulation quality counter: %0.2f avg [Max: %0.0f, Amount: %i]", _qualityAVG.getAverage(),
            _qualityAVG.getMax(), _qualityAVG.getCounts());
        DTU_LOGD("Safety counter: %i, Use lower power limit: %i", _safetyCounterMax, _safetyHitsCounter);
        DTU_LOGD("Last active time: %s", gLimiterTime().c_str());

        if (_lastStopState != LState::OFF) {
            DTU_LOGD("Last discharge stop time: %s, Reason: %s", gDischargeStopTime().c_str(), gLimiterStateText(_lastStopState).data());
        }

        auto stopThreshold = gVoltageStopThresholdUsed();
        DTU_LOGD("Target voltage range: %0.3fV - %0.3fV", stopThreshold, stopThreshold + _halfOfTargetRange * 2.0f);
        DTU_LOGD("Delta to target voltage: %0.0fmV_avg [Min: %0.0fmV, Max: %0.0fmV, Amount: %i]",
            _qualityTarget.getAverage() * 1000.0f, _qualityTarget.getMin() * 1000.0f,
            _qualityTarget.getMax() * 1000.0f, _qualityTarget.getCounts());

        #ifdef OPTION_CORRECTION_FACTOR
            DTU_LOGD("Correction Factor: %0.3f avg [Min: %0.3f, Max: %0.3f, Amount: %i]",
                _cfAVG.getAverage(), _cfAVG.getMin(), _cfAVG.getMax(), _cfAVG.getCounts());
        #endif
    }
}


/*
 * Returns a string according to the limiter state
 */
frozen::string const& BatteryGuardClass::gLimiterStateText(BatteryGuardClass::LState state) const {
    static const frozen::map<LState, frozen::string, 13> texts = {
        { LState::OFF, "Off" },
        { LState::IDLE, "Idle" },
        { LState::ERROR, "Error" },
        { LState::LIMITING_DOWN, "Limit decrease" },
        { LState::LIMITING_UP, "Limit increase" },
        { LState::LIMITING_TIMEOUT, "Limit timeout" },
        { LState::LIMITING_TARGET, "In target range" },
        { LState::OUTDATED_MEASUREMENT, "Waiting for newer battery values" },
        { LState::REQUESTED_POWER_BELOW_LIMIT, "Requested power below limit" },
        { LState::LIMIT_MAX, "Maximum limit" },
        { LState::STOP_NORMAL, "Stopped by voltage limiter" },
        { LState::STOP_EMERGENCY, "Stopped by overtime" },
        { LState::STOP_DPL, "Stopped by DPL" }
    };

    auto iter = texts.find(state);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}



// Recharge Helper | Recharge Helper | Recharge Helper | Recharge Helper | Recharge Helper | Recharge Helper | Recharge Helper

/*
 * The recharge helper supports the battery to calibrate the SoC (100% SoC)
 * States:  Explanation of the state machine
 * Off:     We do nothing
 * Error:   We do nothing because we have an error (config error or no SoC time)
 * Start:   We must calculate new values, but we don't wait for the 12:00 time trigger
 * Idle:    We do nothing, we use the DPL thresholds and limits
 * Stage 1: We increase the battery start/stop voltage thresholds. Change every day at 12:00
 * Stage 2: We reduce the maximum inverter power. Change every day at 12:00
 * Stage 3: We keep the maximum start/stop-thresholds and the minimum inverter power
 */
void BatteryGuardClass::calculateRechargeHelper(time_t const fullEpoch, time_t const nowEpoch) {

    if (_hState == HState::OFF) { return; }

    // check config errors
    auto const& config = Configuration.get();

    _configError = false;
    if (config.PowerLimiter.IgnoreSoc && !config.BatteryGuard.UseVoltageThresholds) {
        _configError = true; // invalid configuration, if DPL use voltage (ignores SoC) we also need voltage thresholds
    } else {
        if (config.BatteryGuard.UseVoltageThresholds) {
            _configError = !thresholdsValid(
                config.PowerLimiter.VoltageStartThreshold,
                config.PowerLimiter.VoltageStopThreshold,
                config.BatteryGuard.MaxVoltageStartThreshold,
                config.BatteryGuard.MaxVoltageStopThreshold);
        } else {
            _configError = !thresholdsValid(
                config.PowerLimiter.BatterySocStartThreshold,
                config.PowerLimiter.BatterySocStopThreshold,
                config.BatteryGuard.MaxSoCStartThreshold,
                config.BatteryGuard.MaxSoCStopThreshold);
        }
    }
    if (config.BatteryGuard.UpperPowerLimit >= gUpperPowerLimitUsed()) { _configError = true; }

    auto oDay = gDaysSinceLastFullyCharged(fullEpoch, nowEpoch);

    // Check if there is a configuration error or local time is invalid or day is not available.
    if (_configError || (!oDay.has_value()) || (nowEpoch == 0)) {
        resetRechargeHelper();
        _hState = HState::ERROR;
        return;
    }

    // all errors gone. If we reach this line, we can start again
    if ((_hState == HState::ERROR)) { _hState = HState::START; }

    // if the battery reached 100% SoC, we can reset and start from beginning
    _dayCounter = oDay.value();
    if ((_dayCounter == 0) && (_hState == HState::STAGE1 || _hState == HState::STAGE2 || _hState == HState::STAGE3)) {
        _hState = HState::START;
    }

    // we are not in a start state or don't get the 12:00 time trigger, we abort
    if ((_hState != HState::START) && !gRechargeTimeTrigger(nowEpoch)) { return; }

    // special handling for starting up, because we change values not before 12:00 o'clock
    uint16_t day = _dayCounter;
    if ((_hState == HState::START) && (day > 0)) {
        struct tm nowTime;
        localtime_r(&nowEpoch, &nowTime);
        if ((nowTime.tm_hour < 12)) { day = day - 1; }
    }

    // Idle: We do not alter any values
    uint16_t startDay = 0;
    uint16_t stopDay = config.BatteryGuard.DurationIdle;
    if (day < stopDay) {
        if (_hState != HState::IDLE) { resetRechargeHelper(); }
        _hState = HState::IDLE;
        return;
    }

    // Stage 1: We increase the battery start/stop-thresholds every day
    // and skip stage 1 if DurationStage1 is 0
    startDay += config.BatteryGuard.DurationIdle;
    stopDay += config.BatteryGuard.DurationStage1;
    if ((day < stopDay) && (config.BatteryGuard.DurationStage1 > 0)) {

        // calculate the battery start/stop thresholds based on the actual day
        float dayFactor = static_cast<float>(day - startDay + 1) / static_cast<float>(config.BatteryGuard.DurationStage1);
        if (config.BatteryGuard.UseVoltageThresholds) {
            auto const& startMin = config.PowerLimiter.VoltageStartThreshold;
            auto const& startMax = config.BatteryGuard.MaxVoltageStartThreshold;
            _oVoltageStartThreshold = startMin + (startMax - startMin) * dayFactor;
            auto const& stopMin = config.PowerLimiter.VoltageStopThreshold;
            auto const& stopMax = config.BatteryGuard.MaxVoltageStopThreshold;
            _oVoltageStopThreshold = stopMin + (stopMax - stopMin) * dayFactor;
        } else {
            auto const& startMin = config.PowerLimiter.BatterySocStartThreshold;
            auto const& startMax = config.BatteryGuard.MaxSoCStartThreshold;
            _oSoCStartThreshold = startMin + (startMax - startMin) * dayFactor;
            auto const& stopMin = config.PowerLimiter.BatterySocStopThreshold;
            auto const& stopMax = config.BatteryGuard.MaxSoCStopThreshold;
            _oSoCStopThreshold = stopMin + (stopMax - stopMin) * dayFactor;
        }
        _hState = HState::STAGE1;
        return;
    }

    // Stage 1 is over, we set the maximum thresholds
    if (config.BatteryGuard.UseVoltageThresholds) {
        _oVoltageStartThreshold = config.BatteryGuard.MaxVoltageStartThreshold;
        _oVoltageStopThreshold = config.BatteryGuard.MaxVoltageStopThreshold;
    } else {
        _oSoCStartThreshold = config.BatteryGuard.MaxSoCStartThreshold;
        _oSoCStopThreshold = config.BatteryGuard.MaxSoCStopThreshold;
    }

    // Stage 2: We reduce the upper power inverter limit every day
    // and skip stage 2 if DurationStage2 is 0
    startDay += config.BatteryGuard.DurationStage1;
    stopDay += config.BatteryGuard.DurationStage2;
    if ((day < stopDay) && (config.BatteryGuard.DurationStage2 > 0)) {

        // calculate the upper power limit based on the actual day
        float dayFactor = static_cast<float>(day - startDay + 1) / static_cast<float>(config.BatteryGuard.DurationStage2);
        auto const& powerMin = config.BatteryGuard.UpperPowerLimit;
        auto maxUpperPowerLimit = gUpperPowerLimitUsed();
        _oPowerLimit = maxUpperPowerLimit - (maxUpperPowerLimit - powerMin) * dayFactor;
        _hState = HState::STAGE2;
        return;
    }

    // Stage 3: we use the maximum start/stop-thresholds and the minimum upper power limit until the battery is full
    _oPowerLimit = config.BatteryGuard.UpperPowerLimit;
    _hState = HState::STAGE3;
    return;
}


/*
 * Returns the current voltage start threshold or nullopt if not active
 */
std::optional<float> BatteryGuardClass::getVoltageStartThreshold(void) const {
    if (!_useRechargeHelper) { return std::nullopt; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _oVoltageStartThreshold;
}


/*
 * Returns the current voltage stop threshold or nullopt if not active
 */
std::optional<float> BatteryGuardClass::getVoltageStopThreshold(void) const {
    if (!_useRechargeHelper) { return std::nullopt; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _oVoltageStopThreshold;
}


/*
 * Returns the current SoC start threshold or nullopt if not active
 */
std::optional<float> BatteryGuardClass::getSoCStartThreshold(void) const {
    if (!_useRechargeHelper) { return std::nullopt; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _oSoCStartThreshold;
}


/*
 * Returns the current SoC stop threshold or nullopt if not active
 */
std::optional<float> BatteryGuardClass::getSoCStopThreshold(void) const {
    if (!_useRechargeHelper) { return std::nullopt; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);
    return _oSoCStopThreshold;
}


/*
 * Returns true if use of excessive solar power is allowed
 * Note: Used to temporary disable for example 'Full Solar-Passthrough" or 'Surplus'
 */
bool BatteryGuardClass::isUseOfExcessiveSolarPowerAllowed(void) const {
    if (!_useRechargeHelper) { return true; } // fast exit to avoid locking

    std::shared_lock<std::shared_mutex> lock(_mutex);

    if (Configuration.get().BatteryGuard.ExcessiveSolarPowerDisabled
    && ((_hState == HState::STAGE1) || (_hState == HState::STAGE2) || (_hState == HState::STAGE3))) {
         return false;
    }
    return true;
}


/*
 * Returns the days since the battery was fully charged
 * Note: If the local time is not available we return std::nullopt
 *       If the time from the battery stats is not available we use the startup epoch as a fallback
 */
std::optional<uint16_t> BatteryGuardClass::gDaysSinceLastFullyCharged(time_t epochFull, time_t epochNow) {
    std::optional<uint16_t> oDay = std::nullopt;

    if (epochNow == 0) { return oDay; }

    if (epochFull != 0) {
        // the 100% SoC epoch is available, we can reset the fallback epoch
        _fallbackSoCEpoch = 0;
    } else {
        // the 100% SoC epoch is not available, we use the now epoch as a fallback
        if (0 == _fallbackSoCEpoch) { _fallbackSoCEpoch = epochNow; }
        epochFull = _fallbackSoCEpoch;
    }

    // start day counting from midnight
    tm timeFull;
    localtime_r(&epochFull, &timeFull);
    timeFull.tm_hour = timeFull.tm_min = timeFull.tm_sec = 0;
    epochFull = mktime(&timeFull);

    if (epochNow >= epochFull) { // guard against negative day deltas
        oDay = difftime(epochNow, epochFull) / (60 * 60 * 24);
    }
    return oDay;
}


/*
 * Reset the thresholds and the power limit
 */
void BatteryGuardClass::resetRechargeHelper(void) {
    _oPowerLimit = std::nullopt;
    _oVoltageStartThreshold = std::nullopt;
    _oVoltageStopThreshold = std::nullopt;
    _oSoCStartThreshold = std::nullopt;
    _oSoCStopThreshold = std::nullopt;
}


/*
 * Time trigger returns true every day at 12:00
 */
bool BatteryGuardClass::gRechargeTimeTrigger(time_t const nowEpoch) {

    tm nowTime;
    localtime_r(&nowEpoch, &nowTime);

    if ((nowTime.tm_year != 0) && (nowTime.tm_hour == 12) && (nowTime.tm_min <= 5)) {
        if (_lastTimeTrigger == false) {
            _lastTimeTrigger = true;
            return true;
        }
        return false;
    }
    _lastTimeTrigger = false;
    return false;
}


/*
 * Returns true if the configured thresholds are in a valid order
 */
bool BatteryGuardClass::thresholdsValid(float startMinDPL, float stopMinDPL, float startMax, float stopMax) const {
    if (( stopMinDPL > 0.0f ) && ( startMinDPL > 0.0f )
    && ( startMinDPL > stopMinDPL )
    && ( stopMax > stopMinDPL )
    && ( startMax > startMinDPL )
    &&( startMax > stopMax )) {
        return true;
    }

    return false;
}


/*
 * Print the 'Recharge Helper' report
 */
void BatteryGuardClass::printRechargeReport(void) const {
    auto const& config = Configuration.get();
    DTU_LOGD("");
    DTU_LOGD("Recharge Helper: %s", (_useRechargeHelper) ? "Enabled" : "Disabled");
    if (_useRechargeHelper) {
        DTU_LOGD("State: %s", gRechargeStateText(_hState).data());
        DTU_LOGD("Configuration error: %s", _configError ? "Yes" : "No");
        DTU_LOGD("Using 100%% SoC time from: %s", _fallbackSoCEpoch ? "Fallback" : "Battery");
        DTU_LOGD("Time since the start of the cycle: %i days", _dayCounter);
        DTU_LOGD("Start day of state 'Increase Thresholds': %i", config.BatteryGuard.DurationIdle);
        DTU_LOGD("Start day of state 'Decrease Power': %i", config.BatteryGuard.DurationIdle + config.BatteryGuard.DurationStage1);
        DTU_LOGD("Start day of state 'Keep Limits': %i",
            config.BatteryGuard.DurationIdle + config.BatteryGuard.DurationStage1 + config.BatteryGuard.DurationStage2);

        if (config.BatteryGuard.UseVoltageThresholds) {
            DTU_LOGD("Voltage Start Threshold: %0.3fV [%0.2fV-%0.2fV]", _oVoltageStartThreshold.value_or(0.0f),
                config.PowerLimiter.VoltageStartThreshold, config.BatteryGuard.MaxVoltageStartThreshold);
            DTU_LOGD("Voltage Stop Threshold: %0.3fV [%0.2fV-%0.2fV]", _oVoltageStopThreshold.value_or(0.0f),
                config.PowerLimiter.VoltageStopThreshold, config.BatteryGuard.MaxVoltageStopThreshold);
        } else {
            DTU_LOGD("SoC Start Threshold: %0.2f%% [%i%%-%i%%]", _oSoCStartThreshold.value_or(0.0f),
                config.PowerLimiter.BatterySocStartThreshold, config.BatteryGuard.MaxSoCStartThreshold);
            DTU_LOGD("SoC Stop Threshold: %0.2f%% [%i%%-%i%%]", _oSoCStopThreshold.value_or(0.0f),
                config.PowerLimiter.BatterySocStopThreshold, config.BatteryGuard.MaxSoCStopThreshold);
        }

        DTU_LOGD("Power Limit: %iW [%iW-%iW]", _oPowerLimit.value_or(0), gUpperPowerLimitUsed(), config.BatteryGuard.UpperPowerLimit);
        DTU_LOGD("DPL Use Voltage Thresholds Only: %s", config.BatteryGuard.UseVoltageThresholds ? "Yes" : "No");
    }
}


/*
 * Returns a string according to the current 'Recharge Helper' state
 */
frozen::string const& BatteryGuardClass::gRechargeStateText(BatteryGuardClass::HState const state) const {
    static const frozen::map<HState, frozen::string, 7> texts = {
        { HState::OFF, "Off"},
        { HState::START, "Starting-Up, wait..."},
        { HState::ERROR, "Error"},
        { HState::IDLE, "Idle" },
        { HState::STAGE1, "Increase-Thresholds" },
        { HState::STAGE2, "Decrease-Power" },
        { HState::STAGE3, "Keep Limits" }
    };

    auto iter = texts.find(state);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}


/*
 * Serialize the 'Battery Guard' informations into a JSON object
 */
void BatteryGuardClass::serializeInfo(JsonObject const& start) const {

    // read external state without holding our mutex
    auto useVoltageThresholds = Configuration.get().BatteryGuard.UseVoltageThresholds;

    // The data is locked for reading.
    std::shared_lock<std::shared_mutex> lock(_mutex);

    // 'Battery Guard' On/Off
    start["enabled"] = _useBatteryGuard.load();

    // 'Battery Current Compensation' informations
    auto const& compensation = start["compensation"];
    compensation["enabled"] = _useCurrentCompensation.load();
    compensation["state"] = gOpenVoltageStateText(_oState).data();
    compensation["used_resistance"] = gResistanceUsed().value_or(0.0f) * 1000.0f; // mOhm
    compensation["open_circuit_voltage"] = gOpenCircuitVoltage().value_or(0.0f); // V

    // 'Stop Voltage Limiter' informations
    auto const& limiter = start["limiter"];
    limiter["enabled"] = _useStopVoltageLimiter.load();
    limiter["state"] = gLimiterStateText(_lState).data();
    limiter["power_limit"] = (_lState > LState::ERROR) ? _stopLimitPower : 0; // W
    limiter["quality"] = gLimiterQuality();
    limiter["active_time"] = gLimiterTime();
    limiter["stop_time"] = gDischargeStopTime();
    limiter["stop_reason"] = (_lastStopState != LState::OFF) ? gLimiterStateText(_lastStopState).data() : TEXT_NODATA;

    // 'Recharge Helper' informations
    auto const& recharge = start["recharge"];
    recharge["enabled"] = _useRechargeHelper.load();
    recharge["state"] = gRechargeStateText(_hState).data();
    recharge["day"] = _dayCounter;
    recharge["use_voltage_thresholds"] = useVoltageThresholds;
    recharge["start_threshold"] = _oVoltageStartThreshold.value_or(0.0f); // V
    recharge["stop_threshold"] = _oVoltageStopThreshold.value_or(0.0f); // V
    recharge["soc_start_threshold"] = _oSoCStartThreshold.value_or(0.0f); // %
    recharge["soc_stop_threshold"] = _oSoCStopThreshold.value_or(0.0f); // %
    recharge["power_limit"] = _oPowerLimit.value_or(0); // W

    // Internal resistance (configured and calculated)
    auto const& values = start["values"];
    values["internal_resistance_calculated"] = (_resistanceFromCalcAVG.getCounts() >= MINIMUM_RESISTANCE_CALC) ? true : false;
    values["resistance_calculated"] = _resistanceFromCalcAVG.getAverage() * 1000.0f; // mOhm
    values["resistance_configured"] = _resistanceFromConfig * 1000.0f; // mOhm

    // Get resistance calculation details
    values["resistance_calculation_count"] = _resistanceFromCalcAVG.getCounts();
    values["resistance_calculation_state"] = gResistanceStateText(_rStateMax).data();

    // Resolution and timing information
    values["voltage_resolution"] = _analyzedResolutionV * 1000.0f; // mV
    values["current_resolution"] = _analyzedResolutionI * 1000.0f; // mA
    values["measurement_time_period"] = _analyzedPeriod.getAverage(); // milliseconds
    values["v_i_time_stamp_delay"] = _analyzedUIDelay.getAverage(); // milliseconds

    // Limits for the resistance calculation
    auto const& limits = start["limits"];
    limits["max_voltage_resolution"] = MAXIMUM_VOLTAGE_RESOLUTION * 1000.0f; // mV
    limits["max_current_resolution"] = MAXIMUM_CURRENT_RESOLUTION * 1000.0f; // mA
    limits["max_measurement_time_period"] = MAXIMUM_MEASUREMENT_TIME_PERIOD; // milliseconds
    limits["max_v_i_time_stamp_delay"] = MAXIMUM_V_I_TIME_STAMP_DELAY; // milliseconds
    limits["min_resistance_calculation_count"] = MINIMUM_RESISTANCE_CALC;
}
