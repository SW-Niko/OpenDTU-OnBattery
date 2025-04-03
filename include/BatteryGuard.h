// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Locking policy:
 * - Public getters having the prefix 'get' take a shared lock internally. getOpenCircuitVoltage(), getCalculatedResistance(), etc.
 * - Public mutating methods take an exclusive lock internally.
 * - Private getters having the prefix 'g' do not take a lock. gOpenCircuitVoltage(), gCalculatedResistance(), etc.
 * - Private methods are called with the appropriate lock held by the public methods.
 * - Fetching external data is done before acquiring _mutex. The only exception is data from the configuration or
 *   data used just for visualization.
 * - Flags marked as atomic may be read lock-free.
 */

#pragma once

#include <shared_mutex>
#include <atomic>
#include <frozen/string.h>
#include <TaskSchedulerDeclarations.h>
#include <ArduinoJson.h>
#include "Statistic.h"


class BatteryGuardClass {
    public:
        BatteryGuardClass() = default;
        ~BatteryGuardClass() = default;
        BatteryGuardClass(const BatteryGuardClass&) = delete;
        BatteryGuardClass& operator=(const BatteryGuardClass&) = delete;
        BatteryGuardClass(BatteryGuardClass&&) = delete;
        BatteryGuardClass& operator=(BatteryGuardClass&&) = delete;

        // public methods without lock
        void init(Scheduler& scheduler);

        // public methods with unique lock
        enum class UpdateSource : uint8_t { BATTERY, DPL, BATTERY_GUARD, STARTUP };
        void updateSettings(UpdateSource const source = UpdateSource::STARTUP);
        std::optional<bool> isStopThresholdReached(bool const inStateStop);
        uint16_t calculatePowerLimit(uint16_t const requestedPower, uint16_t const nowPower, uint32_t const nowMillis);
        void deserializeRTD(JsonObject const& obj);

        // public getter methods with shared lock
        void serializeInfo(JsonObject const& start) const;
        std::optional<float> getCalculatedResistance(void) const;
        std::optional<float> getOpenCircuitVoltage(void) const;
        std::optional<float> getVoltageStopThreshold(void) const;
        std::optional<float> getVoltageStartThreshold(void) const;
        std::optional<float> getSoCStopThreshold(void) const;
        std::optional<float> getSoCStartThreshold(void) const;
        bool isUseOfExcessiveSolarPowerAllowed(void) const;
        void serializeRTD(JsonObject const& obj) const;

    private:
        void fastLoop(void);    // unique lock
        void slowLoop(void);    // unique lock and shared lock

        Task _fastLoopTask;                                 // Task (get the battery values)
        Task _slowLoopTask;                                 // every minute (recharge helper and print the reports)
        std::atomic<bool> _useBatteryGuard = false;         // "Battery Guard" On/Off
        std::atomic<bool> _useCurrentCompensation = false;  // "Battery Current Compensation" On/Off
        std::atomic<bool> _useStopVoltageLimiter = false;   // "Stop-Voltage Limiter" On/Off
        std::atomic<bool> _useRechargeHelper = false;       // "Recharge Helper" On/Off
        mutable std::shared_mutex _mutex;                   // mutex to protect the shared data


        // Open circuit voltage: private members and methods
        enum class OState : uint8_t { OFF, START, ERROR, ACTIVE };

        void calculateOpenCircuitVoltage(float const nowVoltage, float const nowCurrent);
        std::optional<float> gOpenCircuitVoltage(void) const;
        bool isBatteryVoltageValid() const { return ((_battVoltage > 0.0f) && (millis() - _battMillis) < 30*1000); }
        void updateBatteryValues(float const voltage, float const current, uint32_t const millisStamp, float const nowSoC);
        bool isResolutionOK(void) const;
        void printCompensationReport(void) const;
        frozen::string const& gOpenVoltageStateText(OState tNr) const;

        OState _oState = OState::OFF;                       // shared data, holds the actual calculation state
        WeightedAVG<float> _openCircuitVoltageAVG {5};      // shared data, average battery open circuit voltage [V]
        uint32_t _lastOCMillis = 0;                         // last time of open circuit voltage calculation [ms]
        float _analyzedResolutionV = 0;                     // shared data, resolution of the battery voltage [V]
        float _analyzedResolutionI = 0;                     // shared data, resolution of the battery current [A]
        WeightedAVG<float> _analyzedPeriod {20};            // shared data, measurement period [ms]
        WeightedAVG<float> _analyzedUIDelay {20};           // shared data, delay between voltage and current [ms]
        float _battVoltage = 0.0f;                          // shared data, actual battery voltage [V]
        float _battCurrent = 0.0f;                          // shared data, actual battery current [A]
        uint32_t _battMillis = 0;                           // battery measurement time stamp [ms]
        WeightedAVG<float> _battVoltageAVG {5};             // shared data, average battery voltage [V]

        struct Data { float value; uint32_t timeStamp; bool valid; };
        Data _i1Data {0.0f, 0, false };                     // buffer the last current data [A, millis, true/false]
        Data _u1Data {0.0f, 0, false };                     // buffer the last voltage data [V, millis, true/false]


        // Calculate the "Battery DC-Puls resistance": private members and methods
        enum class RState : uint8_t { IDLE, RESOLUTION, OUTOF_RANGE, TIME, FIRST_PAIR, TRIGGER, SECOND_PAIR, SECOND_BREAK,
            DELTA_POWER, TOO_BAD, CALCULATED };

        void calculateResistance(float const nowVoltage, float const nowCurrent, float const nowSoC);
        std::optional<float> gResistanceUsed(void) const;
        frozen::string const& gResistanceStateText(RState state) const;

        RState _rStateMax = RState::IDLE;                   // shared data, holds the maximum calculation state
        RState _lastResistanceLogState = RState::IDLE;      // to avoid log flooding
        float _resistanceFromConfig = 0.0f;                 // shared data, configured battery resistance [Ohm]
        WeightedAVG<float> _resistanceFromCalcAVG {10};     // shared data, calculated battery resistance [Ohm]
        RState _rState = RState::IDLE;                      // shared data, state machine, holds the actual calculation state
        bool _firstOfTwoAvailable = false;                  // true after to got the first of two values
        bool _minMaxAvailable = false;                      // true if minimum and maximum values are available
        bool _triggerEvent = false;                         // true if we have sufficient current change
        bool _pairAfterTriggerAvailable = false;            // true if after the trigger the first second pair is available
        std::pair<float,float> _pFirstVolt = {0.0f,0.0f};   // first of two voltages and related current [V,A]
        std::pair<float,float> _pMaxVolt = {0.0f,0.0f};     // maximum voltage and related current [V,A]
        std::pair<float,float> _pMinVolt = {0.0f,0.0f};     // minimum voltage and related current [V,A]
        float _checkCurrent = 0.0f;                         // used to check the current [A] after the trigger
        uint32_t _lastTriggerMillis = 0;                    // last millis from the first min/max values [ms]
        uint32_t _lastDataInMillis = 0;                     // last millis for data in [ms]


        // Low Voltage Power Limiter: private members and methods
        enum class LState : uint8_t {
            OFF, IDLE, ERROR, LIMITING_DOWN, LIMITING_UP, LIMITING_TIMEOUT, LIMITING_TARGET, OUTDATED_MEASUREMENT,
            REQUESTED_POWER_BELOW_LIMIT, LIMIT_MAX, STOP_NORMAL, STOP_EMERGENCY, STOP_DPL
        };
        enum class QText : uint8_t { NODATA, EXCELLENT, GOOD, BAD, VERYBAD };

        uint16_t calculateStopVoltagePowerLimit(uint16_t const requestedPower, uint16_t const nowPower, uint32_t const nowMillis);
        uint16_t gUpperPowerLimitUsed(void) const;
        float gVoltageStopThresholdUsed(void) const;
        const char* gLimiterQuality() const;
        void handleQualityCounter(void);
        String gLimiterTime() const;
        String gDischargeStopTime() const;
        void printLimiterReport(void);
        frozen::string const& getQualityText(QText tNr) const;
        frozen::string const& gLimiterStateText(LState state) const;

        LState _lState = LState::OFF;                       // shared data, state machine
        uint16_t _stopLimitPower = 0;                       // shared data, actual power limiter value [W]
        tm _limitStartTime;                                 // shared data, limiting start time
        tm _limitStopTime;                                  // shared data, limiting stop time
        WeightedAVG<float> _qualityAVG {10};                // shared data, regulation quality (average)
        uint32_t _lastAboveStopMillis = 0;                  // last time the battery voltage was above the threshold
        LState _lastStopState = LState::OFF;                // last stop state
        tm _stopTime;                                       // buffer the stop time
        uint16_t _lowerPowerLimitOneInverter = 0;           // the lowest power limit from all battery powered inverters [W]
        uint16_t _upperPowerLimitFromConfig = 0;            // from the DPL config [W]
        uint32_t _lastStepDownTime = 0;                     // last time the power was reduced by the limiter
        uint32_t _lastBatteryMillis = 99;                   // initialization with 0 can lead to problem during start up [millis()]
        uint32_t _lastPrint = 0;                            // last time the limiter report was printed [millis()]
        size_t _safetyCounter = 0;                          // counts the power reductions steps
        size_t _safetyCounterMax = 0;                       // stores the maximum value of _safetyCounter
        bool _limitActive = false;                          // true if power is actual limited by the limiter
        bool _insideLimitArea = false;                      // true if limit < _upperPowerLimitConfig
        size_t _qualityCounter = 0;                         // counts the power regulation steps
        size_t _safetyHitsCounter = 0;                      // counts the power regulation steps to the lower power limit
        float _halfOfTargetRange = 0.0f;                    // target range [V]
        WeightedAVG<float> _qualityTarget {10};             // regulation quality (average)
        float _cfHelp1 = 0.0f;                              // buffer calculation result (for next correction factor calculation)
        float _cfHelp2 = 0.0f;                              // buffer calculation result (for next correction factor calculation)
        WeightedAVG<float> _cfAVG {5};                      // correction factor (average)


        // Recharge Helper: private members and methods
        enum class HState : uint8_t { OFF, ERROR, START, IDLE, STAGE1, STAGE2, STAGE3 };

        void calculateRechargeHelper(time_t const fullEpoch, time_t const nowEpoch);
        void resetRechargeHelper(void);
        bool gRechargeTimeTrigger(time_t const nowEpoch);
        bool thresholdsValid(float startMinDPL, float stopMinDPL, float startMax, float stopMax) const;
        void printRechargeReport(void) const;
        std::optional<uint16_t> gDaysSinceLastFullyCharged(time_t epochFull, time_t epochNow);
        frozen::string const& gRechargeStateText(HState const state) const;

        HState _hState = HState::OFF;                       // shared data, state machine
        uint16_t _dayCounter = 0;                           // shared data, days since battery was fully charged
        std::optional<uint16_t> _oPowerLimit;               // shared data, battery powered inverter max power, std::nullopt if not active
        std::optional<float> _oVoltageStopThreshold;        // shared data, battery voltage stop threshold, std::nullopt if not active
        std::optional<float> _oVoltageStartThreshold;       // shared data, battery voltage start threshold, std::nullopt if not active
        std::optional<float> _oSoCStartThreshold;           // shared data, battery SoC start threshold, std::nullopt if not active
        std::optional<float> _oSoCStopThreshold;            // shared data, battery SoC stop threshold, std::nullopt if not active
        bool _configError = false;                          // true if a configuration error was detected
        bool _lastTimeTrigger = false;                      // true if the trigger was already activated within the time window.
        time_t _fallbackSoCEpoch = 0;                       // fallback epoch if the 100% SoC epoch is not available
};

extern BatteryGuardClass BatteryGuard;
