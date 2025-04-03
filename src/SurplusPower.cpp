// SPDX-License-Identifier: GPL-2.0-or-later

/* Surplus-Power-Mode
 * The Surplus mode tries to use excessive solar power as early as possible.
 * But avoids to waste solar energy needed to fully charge the battery by sunset.
 *
 * Surplus power is available when the solar power is higher as the power to fully charge the battery.
 * Then Surplus is compared with the household consumption and the higher value is feed into the grid.
 * (1) SurplusPower = SolarPower - BatteryPower
 * (2) AC-Power = maximum( SurplusPower, HouseholdConsumption )
 * (1)+(2) AC-Power = maximum( SolarPower - BatteryPower, HouseholdConsumption )
 *
 * Basic principle of Surplus-Stage-I (MPPT in bulk mode):
 * In bulk mode the MPPT acts like a current source.
 * In this mode we get reliable maximum solar power information and can use it for calculation.
 * We do not use all solar power for the inverter. We must reserve power for the battery to reach fully charge
 * of the battery until end of the day.
 * The calculation of the "reserve battery power" is based on actual SoC, average solar power
 * and remaining time to sunset.
 *
 * Basic principle of Surplus-Stage-II (MPPT in absorption/float mode):
 * In absorption- and float-mode the MPPT acts like a voltage source with current limiter.
 * In these modes we don't get reliable information about the maximum solar power or current.
 * To find the maximum solar power we increase the inverter power, we go higher and higher, step by step,
 * until we reach the maximum solar power. On this point the MPPT current limiter will kick in.
 * When we reduce the inverter power and check if the current into the battery is close to 0A.
 * A kind of approximation control loop.
 *
 * Basic principle of regulation quality indication (Excellent - Good - Bad - Very Bad):
 * To give an hint, if regulation can handle your system, we included regulation quality indication.
 * We count every power step polarity change ( + to -  and - to +) until we reach the target.
 * Normally only one polarity change is necessary to reach the target.
 * If we need sometimes more .. no problem, but if we are permanent above 2 we have a problem and can
 * not regulate the surplus power on this particular system.
 *
 * 10.08.2024 - 1.00 - first version, Stage-II (absorption-/float-mode)
 * 30.11.2024 - 1.10 - add of Stage-I (bulk-mode) and minor improvements of Stage-II
 * 04.02.2025 - 1.20 - use solar charger class, add of report and minor improvements
 * 25.02.2025 - 1.30 - new feature: optional slope mode in Stage-I
 *                   - improvement: use battery and inverter to calculate the total solar charger power
 * 01.06.2025 - 1.40 - improvement: use avarage solar power for calculation. Better result on cloudy days.
 *                   - new feature: add configuration an getter functions for the report data
 * 15.06.2025 - 1.50 - improvement: use the battery current for the surplus stage-II regulation
 *                   - improvement: full VE.Direct support is no longer required
 * 01.08.2025 - 1.60 - improvement: use the power limiter reload config way to reload the surplus config
 * 01.11.2025 - 1.70 - new feature: add mutex locks for thread safety and info view
 * 18.03.2026 - 1.80 - improvement: add tweaking values into the WebUI
 */

#include <battery/Controller.h>
#include <frozen/map.h>
#include <Configuration.h>
#include <SunPosition.h>
#include <LogHelper.h>
#include <defaults.h>
#include "SurplusPower.h"

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
static const char* SUBTAG = "Surplus";


static constexpr frozen::string missing = "programmer error: missing status text";
constexpr uint32_t MINUTES_IN_HOUR = 60;        // 60 minutes in one hour
constexpr uint32_t SECONDS_IN_MINUTE = 60;      // 60 seconds in one minute
constexpr uint32_t MILLIS_IN_SECOND = 1000;     // 1000 milliseconds in one second
constexpr uint32_t MILLIS_IN_MINUTE = 60*1000;  // 60000 milliseconds in one minute
constexpr int32_t RESERVE_POWER_MAX = 100000;   // Default value, battery reserve power [W]
constexpr int16_t NO_DATA = -1;                 // Default value, means no data available
constexpr float EFF_INVERTER = 0.94f;           // means 6% power loss on the inverter [%]
constexpr float EFF_CABLE = 0.99f;              // means 1% power loss on the cable [%]
constexpr float ABSORPTION_SOC = 99.5f;         // 99.5%, seems to be a good value for all charger [%]
constexpr float SLOPE_MAX_SOC = 98.0f;          // If enabled the slope mode will be used up to this SoC value [%]


SurplusClass Surplus; // singleton instance


/*
 * Initialize the surplus class
 */
void SurplusClass::init(Scheduler& scheduler) {

    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&SurplusClass::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.setInterval(1 * MILLIS_IN_MINUTE);
    _loopTask.enableDelayed(5 * MILLIS_IN_SECOND);

    updateSettings();
}


/*
 * Configures some basic parameters needed to calculate surplus power.
 * Note: This function must be called during Start-Up, DPL-Write-Config and Battery-Write-Config.
 */
void SurplusClass::updateSettings(void) {

    // read external states without holding our mutex
    auto capacity = Battery.getStats()->getNominalCapacity().value_or(0);

    std::unique_lock<std::shared_mutex> lock(_mutex);

    const auto& configPL = Configuration.get().PowerLimiter;
    _useSurplus.store(configPL.SurplusEnabled);

    if (!_useSurplus.load()) {
        exitSurplus();
        return;
    }

    // Surplus is enabled, we'll try to start it if it's not already running.
    if ((_surplusState.load() == State::OFF) || (_surplusState.load() == State::ERROR) || (_surplusState.load() == State::IDLE)) {
        _surplusState.store(State::STARTUP);
    }

    // configuring parameters to use stage-I
    _surplusUpperPowerLimit = gUpperPowerLimit();
    _slopeFactor = (configPL.SurplusSlopeDecreaseRate == 0) ? -_surplusUpperPowerLimit / 100 : configPL.SurplusSlopeDecreaseRate;
    if (_slopeFactor >= 0) { _slopeFactor = -1; }
    if (configPL.SurplusTweakEnabled) {
        // use the configured tweak values
        _batterySafetyPercent = configPL.SurplusSafetyFactor;
        _sunsetSafetyMinutes = configPL.SurplusSafetyMinutes;

        _slopeEnabled = configPL.SurplusSlopeEnabled;
        _slopeTarget = configPL.SurplusSlopeTarget;
    } else {
        // use default values
        _batterySafetyPercent = SURPLUS_SAFETY_FACTOR;
        _sunsetSafetyMinutes = SURPLUS_SAFETY_MINUTES;

        _slopeEnabled = SURPLUS_SLOPE_ENABLED;
        _slopeTarget = SURPLUS_SLOPE_TARGET;
    }

    // configuring parameters to use stage-II
    // Note: I have no idea how the different BMS versions detect a fully charged battery and set the SoC to 100%.
    // However ... The surplus regulation tries to hold the battery current between the upper and lower target.
    // If the upper target is too high we might not reach SoC calibration
    _upperTarget = std::max(static_cast<float>(capacity) / 50.0f, 2.0f); // 2% of the capacity, minimum 2A
    _lowerTarget = -0.25f; // -0.25A

    // after entering the absorption/float mode we wait until the current is below the charge target, then we are close
    // to fully charged and can start to use the surplus power
    _chargeTarget = 4 * _upperTarget;

    // calculate the power steps for the approximation regulation
    // the power step size should be above the hysteresis, otherwise one step may have no effect
    _powerStepSize = _surplusUpperPowerLimit / 40;
    _powerStepSize = std::max(_powerStepSize, static_cast<int16_t>(configPL.TargetPowerConsumptionHysteresis + 4));
    _batteryReserve = RESERVE_POWER_MAX;
}


/*
 * Periodical tasks, will be called once a minute
 * todo: call from the DPL loop task?
 */
void SurplusClass::loop(void) {

    if (!_useSurplus.load()) { return; } // fast exit to avoid locking

    // read external states without holding our mutex
    auto errorState = checkSurplusRequirements();

    {
        std::unique_lock<std::shared_mutex> lock(_mutex);

        // we check whether the requirements for using surplus are met
        _stageIEnabled = true;
        _stageIIEnabled = true;
        _errorState = errorState;
        switch (_errorState) {

            case ErrorState::NO_BATTERY_SOC:
            case ErrorState::NO_BATTERY_CAPACITY:
                // we cannot use surplus stage-I without battery Capacity and SoC information
                _stageIEnabled = false;
                [[fallthrough]];

            case ErrorState::NO_ERROR:
                // all requirements are met, we can use (stage_I) and stage_II
                if ((_surplusState.load() == State::STARTUP) || (_surplusState.load() == State::ERROR)) {
                    _surplusState.store(State::IDLE);
                }
                break;

            default:
                // we cannot use surplus, we stop it
                exitSurplus();
                _surplusState.store(State::ERROR);
                break;
        }
    } // end of unique lock


    if (DTU_LOG_IS_DEBUG) {

        std::shared_lock<std::shared_mutex> slock(_mutex);
        printReport();
    } // end of shared lock
}


/*
 * Check some requirements regarding configuration and system hardware
 */
SurplusClass::ErrorState SurplusClass::checkSurplusRequirements(void) {
    const auto& config = Configuration.get();

    // Configuration: check if DPL is enabled
    if (!config.PowerLimiter.Enabled) { return ErrorState::NO_DPL; }

    // Hardware: check if we have battery powered inverter(s)
    if (gUpperPowerLimitSum() <= 0) { return ErrorState::NO_BATTERY_INVERTER; }

    // Hardware: check if we get "bulk" and "absorption/float" information from at least one solar charger
    if (!SolarCharger.getStats()->getStateOfOperation().has_value()) { return ErrorState::NO_CHARGER_STATE; }

    // Hardware: check if we get the SoC from the battery provider. Note: We need this information only for stage-I
    if (!Battery.getStats()->isSoCValid()) { return ErrorState::NO_BATTERY_SOC; }

    // Configuration: check if we have a valid battery capacity. Note: We need this information only for stage-I
    if (Battery.getStats()->getNominalCapacity().value_or(0) <= 0) { return ErrorState::NO_BATTERY_CAPACITY; }

    return ErrorState::NO_ERROR; // all requirements are met
}


/*
 * Returns the "surplus power" or the "requested power", whichever is higher
 * requestedPower:  The next inverter ac-power based on calculation like "Zero feed throttle"
 * nowPower:        The actual producing power of all battery powered inverters
 * nowMillis:       Millis of the last power update of all battery powered inverters
 */
uint16_t SurplusClass::calculateSurplus(uint16_t const requestedPower, uint16_t const nowPower, uint32_t const nowMillis) {

    if (!_useSurplus.load() || !isStateIdleOrAbove()) { return requestedPower; } // fast exit to avoid locking

    // fetch external data without holding our mutex
    struct ExternalData exData {};
    if (Battery.getStats()->isVoltageValid() && (Battery.getStats()->getVoltageAgeSeconds() <= 30)) {
        exData.battVoltage = Battery.getStats()->getVoltage();
    }
    if (Battery.getStats()->isSoCValid() && (Battery.getStats()->getSoCAgeSeconds() <= 30)) {
        exData.battSoC = Battery.getStats()->getSoC();
    }
    if (Battery.getStats()->isCurrentValid() && (millis() - Battery.getStats()->getLastCurrentUpdate() <= 30 * MILLIS_IN_SECOND)) {
        exData.battCurrent = Battery.getStats()->getChargeCurrent();
    }
    exData.battCurrentMillis = Battery.getStats()->getLastCurrentUpdate();
    exData.battNomCapacity = Battery.getStats()->getNominalCapacity();
    exData.battNomVolt = Battery.getStats()->getNominalVoltage();
    exData.chaFloatVoltage = SolarCharger.getStats()->getFloatVoltage();
    exData.chaOutputPower = SolarCharger.getStats()->getOutputPowerWatts();
    exData.chaSoO = SolarCharger.getStats()->getStateOfOperation();
    exData.invNowPower = nowPower;
    exData.invNowMillis = nowMillis;

    exData.targetPower = Configuration.get().PowerLimiter.TargetPowerConsumption;

    std::unique_lock<std::shared_mutex> lock(_mutex);

    if (!exData.chaSoO.has_value()) {
        return returnFromSurplus(requestedPower, requestedPower, ReturnState::ERR_CHARGER);
    }

    if (_stageIEnabled && (exData.chaSoO.value() == CHARGER_STATE::Bulk)) {
        return calcBulkMode(requestedPower, exData);
    }

    if (_stageIIEnabled && ((exData.chaSoO.value() == CHARGER_STATE::Absorption) || (exData.chaSoO.value() == CHARGER_STATE::Float))) {
        return calcAbsorptionMode(requestedPower, exData);
    }

    // solar charger in wrong operation state
    triggerStageState(false, false);
    return requestedPower;
}


/*
 * Calculates the surplus-power for stage-I (bulk-mode)
 * requestedPower:  The power based on actual calculation of "Zero feed throttle" [W]
 * exData:          The external data needed for the surplus calculation
 */
uint16_t SurplusClass::calcBulkMode(uint16_t const requestedPower, ExternalData const& exData) {

    // check if we get the SoC from the battery provider
    if (!exData.battSoC.has_value()) {
        return returnFromSurplus(requestedPower, requestedPower, ReturnState::ERR_BATTERY);
    }
    auto actSoC = exData.battSoC.value();

    // Different to "Zero-Feed-Throttle" the "Surplus-Mode" is not in a hurry. We always wait 5 sec before
    // we do the next calculation or 2 sec after any inverter power change.
    // In the meantime we use the surplus power from the last calculation
    // Note: we use a fixed time of 2 sec, but it would be better if the battery provider can give the delay time information
    if ((_stageIActive) && (((millis() - _lastCalcMillis) < 5*MILLIS_IN_SECOND) || ((millis() - exData.invNowMillis) < 2*MILLIS_IN_SECOND))) {

        auto backPower = static_cast<uint16_t>(_surplusPower);
        if (_slopeEnabled && (actSoC <= SLOPE_MAX_SOC)) { backPower = calcSlopePower(requestedPower, _surplusPower, exData); }

        return returnFromSurplus(requestedPower ,backPower, ReturnState::OK_STAGE_I);
    }

    // check if there is enough time left until sunset
    if (gTimeToSunset() <= 0) {
        _batteryReserve = RESERVE_POWER_MAX;
        _surplusState.store(State::IDLE);
        triggerStageState(false, false);
        return returnFromSurplus(requestedPower, requestedPower, ReturnState::OK_STAGE_I);
    }

    // if we are not in surplus stage-I yet, we start it now
    if (_surplusState.load() != State::BULK_POWER) {
        resetSurplus();
        triggerStageState(true, false);
        _surplusState.store(State::BULK_POWER);
    }

    // get the solar power, we need it for the calculation of the surplus power and the battery reserve
    auto oSolarPower = gSolarPower(exData);
    if (!oSolarPower.has_value()) {
        return returnFromSurplus(requestedPower, requestedPower, ReturnState::ERR_SOLAR_POWER); // we aboard
    }
    _avgSolSlow.addNumber(oSolarPower.value());
    _avgSolFast.addNumber(oSolarPower.value());

    // calculating the battery reserve power after the system start and in a fixed period of 1 minute
    // It is not necessary to do it more often, because the battery reserve power does not change that quickly
    if ((_lastReserveCalcMillis == 0) || ((millis() - _lastReserveCalcMillis) > (MILLIS_IN_MINUTE))) {

        // to calculate the maximum battery energie we need the nominal battery voltage,
        // or the float voltage, or the actual battery voltage
        auto voltage = exData.battNomVolt.value_or(0.0f);
        if (voltage <= 0.0f) { voltage = exData.chaFloatVoltage.value_or(0.0f); }
        if (voltage <= 0.0f) { voltage = exData.battVoltage.value_or(0.0f); }
        if (voltage <= 0.0f) {
            return returnFromSurplus(requestedPower, requestedPower, ReturnState::ERR_BATTERY); // we aboard
        }

        // time to sunset including safety time
        auto durationNowToSunset = gTimeToSunset();
        if (durationNowToSunset > 0) {
            // calculation of the power, we want to reserve for the battery
            auto timeFactor = std::max(static_cast<float>(durationNowToSunset), 10.0f) / 60.0f; // not less than 10 minutes [hour]
            auto socFactor = std::max(ABSORPTION_SOC - actSoC, 1.0f) / 100.0f;          // not less than 1% SoC difference
            auto batteryFactor = 1.0f + _batterySafetyPercent / 100.0f;                 // safety factor, 20% = 1.2
            auto energyCapacity = exData.battNomCapacity.value_or(0.0f) * voltage;      // battery energy capacity [Wh]
            _batteryReserve = energyCapacity * socFactor / timeFactor * batteryFactor;

            // we avoid a too low battery reserve if the battery is nearly full
            _batteryReserve = std::max(_batteryReserve, static_cast<int32_t>(energyCapacity * 0.04f));

            // print debug information
            if (DTU_LOG_IS_DEBUG) {
                DTU_LOGD("Battery capacity: %.0fWh, SoC-Factor: %.1f, Time: %.2fh, Safety-Factor: %.2f",
                    energyCapacity, socFactor, timeFactor, batteryFactor);
            }
        } else {
            // time is over and the battery is not fully charged so we use the maximum reserve power
            _batteryReserve = RESERVE_POWER_MAX;
        }
        _lastReserveCalcMillis = millis();
    }

    // surplus power, including inverter power loss.
    // We use the lower value of two different average solar power values to get a better result on cloudy days
    _solarPowerFiltered = std::min(_avgSolFast.getAverage(), _avgSolSlow.getAverage());
    _surplusPower = (_solarPowerFiltered - _batteryReserve) * EFF_INVERTER;
    _surplusPower = std::max(_surplusPower, 0); // avoid negative values
    _surplusPower = std::min(_surplusPower, static_cast<int32_t>(_surplusUpperPowerLimit));

    // slope power
    auto backPower = static_cast<uint16_t>(_surplusPower);
    if (_slopeEnabled && (actSoC <= SLOPE_MAX_SOC)) {
        backPower = calcSlopePower(requestedPower, _surplusPower, exData);
        _slopeActive = true; // slope mode is active
    } else {
        _slopePower = 0;
        _slopeActive = false; // slope mode is not active
    }

    _lastCalcMillis = millis();

    // print debug information
    if (DTU_LOG_IS_DEBUG && ((millis() - _lastDebugPrint) > (10 * MILLIS_IN_SECOND))) {
        _lastDebugPrint = millis();
        DTU_LOGD("Solar: %iW, Solar-Slow: %iW, Solar-Fast: %iW, Solar-Filter: %iW, Battery reserve: %iW",
            _avgSolSlow.getLast(), _avgSolSlow.getAverage(), _avgSolFast.getAverage(), _solarPowerFiltered, _batteryReserve);
    }
    return returnFromSurplus(requestedPower, backPower, ReturnState::OK_STAGE_I);
}


/*
 * Calculates the surplus-power in stage-II (absorption-/float-mode)
 * requestedPower:  The power based on actual calculation like "Zero feed throttle"
 * exData:          The external data needed for surplus calculation
 */
uint16_t SurplusClass::calcAbsorptionMode(uint16_t const requestedPower, ExternalData const& exVal) {

    // check if battery current ist valid
    if (!exVal.battCurrent.has_value()) {
        return returnFromSurplus(requestedPower, requestedPower, ReturnState::ERR_BATTERY); // we aboard
    }
    auto pActI = std::make_pair(exVal.battCurrentMillis, exVal.battCurrent.value());

    // the regulation loop "Inverter -> Solar Charger -> Battery" needs time. And different to
    // "Zero-Feed-Throttle" the "Surplus-Mode Stage-II" is not in a hurry. We always wait some seconds before
    // we do the next calculation. We also check if last and actual current have the same timesstamp,
    // because it makes no sense to do the calculation again.
    // In the meantime we use the surplus power from the last calculation
    if ((_stageIIActive) && (((millis() - _lastCalcMillis) < (_regulationTime * MILLIS_IN_SECOND))
        || ((millis() - exVal.invNowMillis) < 2 * MILLIS_IN_SECOND) || (_pLastI.first == pActI.first))) {
        _pLastI = pActI; // update the last current
        return returnFromSurplus(requestedPower, _surplusPower, ReturnState::OK_STAGE_II);
    }

    auto lastState = _surplusState.load();
    auto lastSurplusPower = _surplusPower;
    if ((_surplusState.load() == State::IDLE) || (_surplusState.load() == State::BULK_POWER)) {
        // starting the stage-II, if stage-I was active before, we can start stage-II maybe with the identical power?
        _surplusPower = std::max(_surplusPower, static_cast<int32_t>(requestedPower));
        resetSurplus();
        _surplusState.store(State::LIMIT_CHARGING);
        triggerStageState(false, true); // for the report
    }

    // we filter the battery current to avoid dips due to MPPT behavior of the solar chargers
    // But we also accept a delay of 4 seconds in case of current drop due to clouds or other reasons
    auto batteryCurrent = 0.0f;
    if ((pActI.first - _pLastI.first) > (4 * MILLIS_IN_SECOND)) {
        // if measurement frequency is higher than 4 seconds, we do not filter and use the actual current
        batteryCurrent = pActI.second;
    } else {
        // we use the maximum of the last current and the actual current
        batteryCurrent = std::max(pActI.second, _pLastI.second);
    }
    _pLastI = pActI; // update the last current

    // state machine: hold, increase or decrease the surplus power
    int16_t addPower = 0;
    if ((batteryCurrent < _lowerTarget) && (_surplusState.load() != State::REQUESTED_POWER)) {
        // currently we discharge the battery, what is ok if the requested power is higher as the surplus power
        // in all other cases we must reduce the surplus power
        // Note: Maybe we can also use the battery power instead of the fixed step down size?
        addPower = -_powerStepSize;
        _surplusState.store(State::REDUCE_POWER);
    } else {
        // currently we charge the battery, next state depends from the actual state
        switch (_surplusState.load()) {

            case State::REQUESTED_POWER:
                // during last regulation step the requested power was higher as the surplus power
                // we keep that state until the exit condition is met
                if ((batteryCurrent >= _lowerTarget) || (requestedPower <= _surplusPower)) {
                    _surplusPower = std::max(_surplusPower, static_cast<int32_t>(requestedPower));
                    _surplusState.store(State::LIMIT_CHARGING);
                }
                break;

            case State::LIMIT_CHARGING:
                // the battery is not fully charged
                // we increase the surplus power until we are below the charge target
                if (batteryCurrent > _chargeTarget) {
                    addPower = _powerStepSize;
                } else {
                    _surplusState.store(State::WAIT_CHARGING);
                }
                break;

            case State::WAIT_CHARGING:
                // the battery is almost fully charged
                // we keep the state and the actual surplus power until we are below the upper target
                if ((batteryCurrent <= _upperTarget)) {
                    _lastInTargetMillis = millis();
                    _avgTargetCurrent.reset();
                    _surplusState.store(State::IN_TARGET);
                } else {
                    if (batteryCurrent > _chargeTarget) {
                        addPower = _powerStepSize;
                        _surplusState.store(State::LIMIT_CHARGING);
                    }
                }
                break;

            case State::REDUCE_POWER:
                // we are in the target range (batteryCurrent >= _lowerTarget),
                // after reducing the surplus power, now we use the maximum available solar power
                _lastInTargetMillis = millis();
                _avgTargetCurrent.reset();
                _surplusState.store(State::IN_TARGET);
                break;

            case State::TRY_MORE:
                // we keep that state and increase the surplus power step by step
                // as long as we do not hit the lower target (REDUCE_POWER)
                // or reach the maximum surplus power (MAXIMUM_POWER)
                addPower = _powerStepSize;
                break;

            case State::IN_TARGET:
            case State::MAXIMUM_POWER:
                // we are in the target range and keep that state for 1 minute
                _avgTargetCurrent.addNumber(pActI.second);
                if ((millis() - _lastInTargetMillis) > MILLIS_IN_MINUTE) {
                    if (_avgTargetCurrent.getAverage() >= 0.0f) {
                        // we are in the target range and the battery is fully charged, so we try to increase the surplus power
                        addPower = _powerStepSize;
                        _surplusState.store(State::TRY_MORE);
                    } else {
                        // we are in the target range, but energy flowing out of the battery, so we reduce the surplus power
                        addPower = -_powerStepSize;
                        _surplusState.store(State::REDUCE_POWER);
                    }
                }

                // regulation quality: we reached the target
                if (_qualityCounter != 0) {
                    _qualityAVG.addNumber(_qualityCounter);
                    _qualityCounter = 0;
                }
                break;

            default:
                _surplusState.store(State::IDLE);

        }
    }

    // regulation time in seconds, depending from the addPower. If we increase the surplus power
    // we need more time, because the solar charger needs time to check if more solar power is available
    _regulationTime = (addPower > 0) ? 10 : 4;
    _surplusPower += addPower;

    // we do not go below 0W or above the surplus upper power limit
    _surplusPower = std::max(_surplusPower, 0);
    if (_surplusPower >= _surplusUpperPowerLimit) {
        _surplusPower = _surplusUpperPowerLimit;
        if (_surplusState.load() != State::MAXIMUM_POWER) {
            _lastInTargetMillis = millis();
            _avgTargetCurrent.reset();
            _surplusState.store(State::MAXIMUM_POWER);
        }
    }

    // if the requested power is higher than the surplus power, we keep the last surplus power
    // and set the state to "REQUESTED_POWER" to avoid useless regulation steps
    auto backPower = static_cast<uint16_t>(_surplusPower);
    if (requestedPower > backPower) {
        _qualityCounter = 0;
        _surplusPower = lastSurplusPower;
        _surplusState.store(State::REQUESTED_POWER);
    } else {
        // regulation quality: we count the polarity changes (increasing or decreasing of the surplus power)
        if (((_lastAddPower < 0) && (addPower > 0)) || ((_lastAddPower > 0) && (addPower < 0))) {
            _qualityCounter++;
        }
        _lastAddPower = addPower;
    }

    _lastCalcMillis = millis();

    // print the debug information, but only if state has changed or after 30sec
    if (DTU_LOG_IS_DEBUG && ((_surplusState.load() != lastState) || (_surplusPower != lastSurplusPower) ||
        ((millis() - _lastDebugPrint) > 30 * MILLIS_IN_SECOND))) {

        _lastDebugPrint = millis();
        DTU_LOGD("State transmission: %s --> %s, Surplus: %iW, Requested: %iW",
            gStatusText(lastState).data(), gStatusText(_surplusState.load()).data(), _surplusPower, requestedPower);
        DTU_LOGD("Current-Now: %0.3fA, Current-Avg: %0.3fA, Power step: %iW, Settle time: %is",
            batteryCurrent, _avgTargetCurrent.getAverage(), addPower, _regulationTime);
    }
    return returnFromSurplus(requestedPower, backPower, ReturnState::OK_STAGE_II);
}


/*
 * Returns the actual available slope power [W] or the requested power whichever is higher.
 * requestedPower:  The actual "Zero feed throttle power" [W]
 * surplusPower:    The actual "surplus power" [W]
 */
uint16_t SurplusClass::calcSlopePower(uint16_t const requestedPower, int32_t const surplusPower, ExternalData const& exVal) {

    // calculate the decrease of the slope power, not faster as once in a second
    if ((millis() - _slopeLastMillis) > MILLIS_IN_SECOND) {
        auto timeDiff = millis() - _slopeLastMillis;
        if ((_slopeLastMillis != 0) && (timeDiff <= 30 * MILLIS_IN_SECOND)) {
            _slopePower += _slopeFactor * static_cast<int32_t>(timeDiff / MILLIS_IN_SECOND);
            _slopePower = std::max(_slopePower, 0); // avoid negative values
        }
        _slopeLastMillis = millis();
    }

    // remove the "DPL" target power
    auto consumption = static_cast<int32_t>(requestedPower) + exVal.targetPower;
    consumption = std::max(consumption, 0); // avoid negative values

    // keep the slope power in the range (consumption - _slopeTarget) < (_slopePower) < (surplusPower)
    _slopePower = std::max(_slopePower, consumption - _slopeTarget);
    _slopePower = std::min(_slopePower, surplusPower);
    return static_cast<uint16_t>(_slopePower);
}


/*
 * Returns the available solar power [W]. Specified by the solar chargers or calculated from the inverters and battery power
 */
std::optional<uint16_t> SurplusClass::gSolarPower(ExternalData const& exData) const {

    // calculate the solar power, based on the inverters power and the battery power
    auto available = false;
    auto fromBatAndInv = 0.0f;
    if (exData.battCurrent.has_value() && exData.battVoltage.has_value()) {
        fromBatAndInv = exData.invNowPower / EFF_INVERTER / EFF_CABLE + exData.battCurrent.value() * exData.battVoltage.value();
        fromBatAndInv = std::max(fromBatAndInv, 0.0f); // avoid negative values
        available = true;
    }

    // get the solar power from the solar chargers
    auto fromCharger = 0.0f;
    if (exData.chaOutputPower.has_value()) {
        fromCharger = std::max(exData.chaOutputPower.value(), 0.0f); // avoid negative values
        available = true;
    }

    // any solar power information available?
    if (!available) { return std::nullopt; }

    // if both values are available, we have to decide which one we use
    // we do not know if the solar charger power contains the power from all installed solar chargers
    // we do not know if the inverter power information is precise or if additional AC chargers are active
    if ((fromCharger > 0.0f) && ((std::abs(fromBatAndInv - fromCharger) / fromCharger) < 0.1f)) {

        // if the difference is less than 10%, we use the more precise solar power information from the charger
        return static_cast<uint16_t>(fromCharger);
    }

    // else we use the higher value
    return static_cast<uint16_t>(std::max(fromBatAndInv, fromCharger));
}


/*
 * Print the logging information if logging is enabled and surplus power has changed
 */
uint16_t SurplusClass::returnFromSurplus(uint16_t const requestedPower, uint16_t const exitPower, SurplusClass::ReturnState const exitState) {

    if ((exitState != SurplusClass::ReturnState::OK_STAGE_I) && (exitState != SurplusClass::ReturnState::OK_STAGE_II)) {
        auto fault = static_cast<uint8_t>(exitState);
        if (fault < _errorCounter.size()) { _errorCounter.at(fault)++; }
        DTU_LOGW("%s", gExitText(exitState).data());
        return requestedPower; // return the requested power on any detected fault
    }

    if (exitPower <= requestedPower) { return requestedPower; } // No logging, if surplus power is not above the requested power
    if (exitPower == _lastLoggingPower) { return exitPower; } // to avoid logging of useless information
    _lastLoggingPower = exitPower;

    if (DTU_LOG_IS_DEBUG) {
        if (exitState == SurplusClass::ReturnState::OK_STAGE_I) {
            DTU_LOGD("State: %s, Surplus: %iW, Slope: %iW, Requested: %iW, Returned: %iW",
                gStatusText(_surplusState.load()).data(), _surplusPower, _slopePower, requestedPower, exitPower);
        }
        if (exitState == SurplusClass::ReturnState::OK_STAGE_II) {
            DTU_LOGD("State: %s, Surplus: %iW, Requested: %iW, Returned: %iW",
                gStatusText(_surplusState.load()).data(), _surplusPower, requestedPower, exitPower);
        }
    }
    return exitPower;
}


/*
 * Returns time to sunset including safety factor in minutes if actual time is between 0:00 and sunset
 * or 0 if actual time is between sunset and 24:00 or on error (e.g. no time or sunset information available)
 */
int16_t SurplusClass::gTimeToSunset(void) {

    struct tm actTime, sunsetTime;
    if (getLocalTime(&actTime, 5) && SunPosition.sunsetTime(&sunsetTime)) {
        int16_t timeToSunset = sunsetTime.tm_hour * 60 + sunsetTime.tm_min
            - (actTime.tm_hour * 60 + actTime.tm_min) - _sunsetSafetyMinutes;
        if (timeToSunset < 0) timeToSunset = 0;
        return timeToSunset;
    }
    // on failure return 0 (caller may log or update error counters under lock)
    return 0;
}


/*
 * Returns the upper power limit sum of all battery powered inverters
 */
uint16_t SurplusClass::gUpperPowerLimitSum(void) const {

    uint16_t upperPowerLimitSum = 0;
    for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& invConfig = Configuration.get().PowerLimiter.Inverters[i];
        if (invConfig.Serial == 0ULL) { break; }
        if (!invConfig.IsGoverned) { continue; }
        if (invConfig.PowerSource == PowerLimiterInverterConfig::InverterPowerSource::Battery) {
            upperPowerLimitSum += invConfig.UpperPowerLimit;
        }
    }
    return upperPowerLimitSum;
}


/*
 * Returns the upper power limit of the surplus power mode
 */
uint16_t SurplusClass::gUpperPowerLimit(void) const {
    const auto limit = Configuration.get().PowerLimiter.SurplusPowerLimit;
    return (limit == 0) ? gUpperPowerLimitSum() : limit;
}


/*
 * Logs the start-time and the stop-time of the stages and handles the active state
 */
void SurplusClass::triggerStageState(bool stageI, bool stageII) {

    if (stageI && !_stageIActive) {
        _stageIActive = true;
        getLocalTime(&_stageITimeStart, 10);
        _stageITimeStop = {0, 0, 0, 0, 0, 0, 0, 0, 0}; // reset stop time
    }
    if (!stageI && _stageIActive) {
        _stageIActive = false;
        _slopeActive = false;
        getLocalTime(&_stageITimeStop, 10);
    }
    if (stageII && !_stageIIActive) {
        _stageIIActive = true;
        getLocalTime(&_stageIITimeStart, 10);
        _stageIITimeStop = {0, 0, 0, 0, 0, 0, 0, 0, 0}; // reset stop time
    }
    if (!stageII && _stageIIActive) {
        _stageIIActive = false;
        getLocalTime(&_stageIITimeStop, 10);
    }

    if (!_stageIActive && !_stageIIActive) {
        _surplusPower = _slopePower = 0;
    }
}


/*
 * Stops the "surplus power" mode, e.g. due to stop threshold or AC-Charger is active
 */
void SurplusClass::stopSurplus(void) {

    std::unique_lock<std::shared_mutex> lock(_mutex);

    if (_stageIActive || _stageIIActive) {
        triggerStageState(false, false);
        _batteryReserve = RESERVE_POWER_MAX;
        _surplusState.store(State::IDLE);
    }
}


/*
 * Exit the "surplus power" mode, e.g. due to disabled surplus power or disabled DPL or system error
 */
void SurplusClass::exitSurplus(void) {
    if (_surplusState.load() != State::OFF) {
        _stageIEnabled = _stageIIEnabled = false;
        resetSurplus();
        triggerStageState(false, false);
        _surplusState.store(State::OFF);
    }
}


/*
 * Reset some surplus data
 * Note: This function is called on system start-up and when we exit solar-surplus
 */
void SurplusClass::resetSurplus(void) {
    // reset stage-I
    _batteryReserve = RESERVE_POWER_MAX;
    _lastReserveCalcMillis = 0;
    _slopePower = 0;
    _slopeLastMillis = 0;
    _avgSolSlow.reset();
    _avgSolFast.reset();

    // reset stage-II
    _lastInTargetMillis = 0;
    _qualityCounter = 0;
    _qualityAVG.reset();
    _avgTargetCurrent.reset();
}


/*
 * Prints the report of solar-surplus
 */
void SurplusClass::printReport(void) {
    DTU_LOGD("");
    DTU_LOGD("---------------- Surplus Report (every minute) ----------------");

    DTU_LOGD("Surplus: %s, State: %s", Configuration.get().PowerLimiter.SurplusEnabled ? "Enabled" : "Disabled",
        gStatusText(_surplusState.load()).data());
    DTU_LOGD("Surplus power: %iW [0W - %iW]", _surplusPower, _surplusUpperPowerLimit);
    DTU_LOGD("Regulation quality: %s", gQualityText(_qualityAVG.getAverage()).data());
    DTU_LOGD("Requirements: %s", gErrorText(_errorState).data());

    for (auto const& error : _errorCounter) {
        if (error > 0) {
            DTU_LOGD("Errors since start-up, Time: %i, Solar charger: %i, Battery: %i, Solar power: %i",
                _errorCounter.at(static_cast<uint8_t>(ReturnState::ERR_TIME)),
                _errorCounter.at(static_cast<uint8_t>(ReturnState::ERR_CHARGER)),
                _errorCounter.at(static_cast<uint8_t>(ReturnState::ERR_BATTERY)),
                _errorCounter.at(static_cast<uint8_t>(ReturnState::ERR_SOLAR_POWER)));
            break;
        }
    }
    DTU_LOGD("");

    // stage-I report
    DTU_LOGD("1) Stage-I (Bulk): %s / %s",
        (_stageIEnabled) ? "Enabled" : "Disabled",
        (_stageIActive) ? "Active" : "Not active");

    if (_stageIActive) {
        DTU_LOGD("Solar power: %iW, Reserved power: %iW, Surplus: %iW", _solarPowerFiltered, _batteryReserve, _surplusPower);
        DTU_LOGD("Safety factor: %i%%, Safety time: %imin", _batterySafetyPercent, _sunsetSafetyMinutes);

        DTU_LOGD("Slope Mode: %s, Slope Power: %iW [0W - %iW]",
            (_slopeEnabled) ? ((_slopeActive) ? "Active" : "Not Active") : "Disabled", _slopePower, _surplusPower);
        DTU_LOGD("Slope Target: %iW, Slope decrease factor: %iW/s", _slopeTarget, _slopeFactor);

        auto timeToAbsorption = gTimeToSunset();
        DTU_LOGD("Time remaining until exit/sunset: %02i:%02i", timeToAbsorption / 60, timeToAbsorption % 60);
    }

    DTU_LOGD("Last active time: %s", gDatumText(_stageITimeStart, _stageITimeStop).c_str());

    DTU_LOGD("");

    // stage-II report
    DTU_LOGD("2) Stage-II (Absorption/Float): %s / %s",
        (_stageIIEnabled) ? "Enabled" : "Disabled",
        (_stageIIActive) ? "Active" : "Not active");

    if (_stageIIActive) {
        DTU_LOGD("Lower Target: %0.1fA, Upper Target: %0.1fA, Charge Target: %0.1fA", _lowerTarget, _upperTarget, _chargeTarget);

        DTU_LOGD("Regulation power step: %uW", _powerStepSize);

        DTU_LOGD("Regulation quality: Average: %0.2f, Max: %0.0f, Amount: %i",
            _qualityAVG.getAverage(), _qualityAVG.getMax(), _qualityAVG.getCounts());
    }

    DTU_LOGD("Last active time: %s", gDatumText(_stageIITimeStart, _stageIITimeStop).c_str());

    DTU_LOGD("---------------------------------------------------------------");
    DTU_LOGD("");
}


/*
 * Returns a string corresponding to the current Surplus state
 */
frozen::string const& SurplusClass::gStatusText(SurplusClass::State const state) const
{
    static const frozen::map<State, frozen::string, 12> texts = {
        { State::OFF, "Off" },
        { State::STARTUP, "Starting-Up, wait..." },
        { State::ERROR, "Error detected" },
        { State::IDLE, "Idle, Surplus not active" },
        { State::TRY_MORE, "Try more power" },
        { State::WAIT_CHARGING, "Wait for charging" },
        { State::LIMIT_CHARGING, "Limit charging" },
        { State::REDUCE_POWER, "Reduce power" },
        { State::IN_TARGET, "Use maximum solar power" },
        { State::MAXIMUM_POWER, "Use maximum surplus power" },
        { State::REQUESTED_POWER, "Use requested power" },
        { State::BULK_POWER, "Reserve battery power" }
    };

    auto iter = texts.find(state);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}


/*
 * Returns a string corresponding to the surplus exit reason
 */
frozen::string const& SurplusClass::gExitText(SurplusClass::ReturnState const status) const
{
    static const frozen::map<ReturnState, frozen::string, 6> texts = {
        { ReturnState::OK_STAGE_I, "Stage-I" },
        { ReturnState::OK_STAGE_II, "Stage-II" },
        { ReturnState::ERR_TIME, "Error, local time or sunset time not available" },
        { ReturnState::ERR_CHARGER, "Error, solar charger data is not available" },
        { ReturnState::ERR_BATTERY, "Error, battery data is not available" },
        { ReturnState::ERR_SOLAR_POWER, "Error, solar power data is not available" }
    };

    auto iter = texts.find(status);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}


/*
 * Returns a string corresponding to the system requirement error state
 */
frozen::string const& SurplusClass::gErrorText(SurplusClass::ErrorState const status) const
{
    static const frozen::map<ErrorState, frozen::string, 6> texts = {
        { ErrorState::NO_ERROR, "All fulfilled" },
        { ErrorState::NO_DPL, "DPL not enabled" },
        { ErrorState::NO_BATTERY_SOC, "Battery SoC missing" },
        { ErrorState::NO_BATTERY_INVERTER, "No battery powered inverter" },
        { ErrorState::NO_CHARGER_STATE, "Solar charger state missing" },
        { ErrorState::NO_BATTERY_CAPACITY, "Battery capacity not configured" }
    };

    auto iter = texts.find(status);
    if (iter == texts.end()) { return missing; }
    return iter->second;
}


/*
 * Returns a string corresponding to the regulation quality index
 */
frozen::string const& SurplusClass::gQualityText(float const qNr) const
{
    static const frozen::map<uint8_t, frozen::string, 5> texts = {
        { 0, "No data" },
        { 11, "Excellent" },
        { 16, "Good" },
        { 25, "Bad" },
        { 100, "Very bad" }
    };

    auto quality = static_cast<uint8_t>(qNr * 10);  // convert from [0.0 - 10.0] to [0 - 100]
    if (quality > 100) { quality = 100; }           // avoid values above 100
    for (auto const& [key, value] : texts) {
        if (quality <= key) { return value; }
    }
    return missing;
}


/*
 * Returns a string corresponding to the start and stop time
 */
String SurplusClass::gDatumText(tm const& start, tm const& stop) const
{
    String result;
    result.reserve(32);
    if (start.tm_year < (2000 - 1900)) {
        result.concat("No data");
    } else {
        char time[22];
        strftime(time, sizeof(time), "%d-%h %H:%M", &start);
        result.concat(time);
        if ((stop.tm_hour < start.tm_hour) || ((stop.tm_hour == start.tm_hour) && (stop.tm_min < start.tm_min))) {
            result.concat(" - ongoing");
        } else {
            strftime(time, sizeof(time), " - %H:%M", &stop);
            result.concat(time);
        }
    }
    return result;
}


/*
 * Serialize the Solar-Surplus information into a Arduino-Json object
 */
void SurplusClass::serializeInfo(JsonObject const& start) const {

    std::shared_lock<std::shared_mutex> lock(_mutex);

    // Basic configuration status
    start["enabled"] = _useSurplus.load();
    start["requirements_check"] = gErrorText(_errorState).data();
    start["state"] = gStatusText(_surplusState.load()).data();
    start["surplus_power"] = (_slopePower != 0) ? _slopePower : ((_surplusPower != 0) ? _surplusPower : NO_DATA);
    start["max_surplus_power"] = _surplusUpperPowerLimit;

    auto const& stage1 = start["bulk_mode"].to<JsonObject>();
    stage1["enabled"] = _stageIEnabled;
    stage1["state"] = _stageIActive ? "Active" : "Not active";
    stage1["slope_enabled"] = _slopeEnabled;
    stage1["slope_power"] = (_slopeActive) ? _slopePower : NO_DATA;
    stage1["max_slope_power"] = (_slopeActive) ? _surplusPower : NO_DATA;
    stage1["battery_reserve_power"] = (_batteryReserve == RESERVE_POWER_MAX) ? NO_DATA : _batteryReserve;
    stage1["duration"] = gDatumText(_stageITimeStart, _stageITimeStop).c_str();

    auto const& stage2 = start["absorption_mode"].to<JsonObject>();
    stage2["enabled"] = _stageIIEnabled;
    stage2["state"] = _stageIIActive ? "Active" : "Not active";
    stage2["regulation_quality"] = gQualityText(_qualityAVG.getAverage()).data();
    stage2["duration"] = gDatumText(_stageIITimeStart, _stageIITimeStop).c_str();
}
