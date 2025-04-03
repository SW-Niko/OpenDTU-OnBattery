// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Locking policy:
 * - Public getters take a shared_lock internally.
 * - Mutating public methods take an exclusive lock internally.
 * - Do NOT call blocking/external code while holding _mutex. Fetch external data before acquiring _mutex.
 *   The only exception is data from the configuration or data used just for visualization.
 * - Flags marked as atomic may be read lock-free.
 */

#pragma once

#include <Arduino.h>
#include <shared_mutex>
#include <atomic>
#include <array>
#include <optional>
#include <ctime>
#include <utility>
#include <frozen/string.h>
#include <solarcharger/Controller.h>
#include <TaskSchedulerDeclarations.h>
#include <ArduinoJson.h>
#include "Statistic.h"

using CHARGER_STATE = SolarChargers::Stats::StateOfOperation;

// structure to hold external data needed for the surplus calculation
struct ExternalData {
    uint32_t battCurrentMillis = 0;                         // last update time of battery current [ms]
    std::optional<float> battCurrent = std::nullopt;        // battery current [A]
    std::optional<float> battVoltage = std::nullopt;        // battery voltage [V]
    std::optional<float> battSoC = std::nullopt;            // battery SoC [%]
    std::optional<float> battNomCapacity = std::nullopt;    // battery nominal capacity [Ah]
    std::optional<float> battNomVolt = std::nullopt;        // battery nominal voltage [V]
    std::optional<float> chaFloatVoltage = std::nullopt;    // charger float voltage [V]
    std::optional<float> chaOutputPower = std::nullopt;     // charger output power [W]
    std::optional<CHARGER_STATE> chaSoO;                    // charger state of operation
    uint16_t invNowPower = 0;                               // inverter power [W]
    uint32_t invNowMillis = 0;                              // inverter power update time [ms]
    int16_t targetPower = 0;                                // the target power from the zero calculation [W]
};


class SurplusClass {
    public:
        SurplusClass() = default;
        ~SurplusClass() = default;
        SurplusClass(const SurplusClass&) = delete;
        SurplusClass& operator=(const SurplusClass&) = delete;
        SurplusClass(SurplusClass&&) = delete;
        SurplusClass& operator=(SurplusClass&&) = delete;

        // public methods with exclusive lock
        void init(Scheduler& scheduler);
        void updateSettings(void);
        uint16_t calculateSurplus(uint16_t const requestedPower, uint16_t const nowPower, uint32_t const nowMillis);
        void stopSurplus(void);

        // public getter methods with shared lock
        bool isSurplusEnabled(void) const { std::shared_lock<std::shared_mutex> lock(_mutex); return _useSurplus && (_stageIEnabled || _stageIIEnabled); }
        void serializeInfo(JsonObject const& start) const;

    private:
        enum class State : uint8_t {
            OFF, STARTUP, ERROR, IDLE, TRY_MORE, WAIT_CHARGING, LIMIT_CHARGING, REDUCE_POWER, IN_TARGET, MAXIMUM_POWER,
            REQUESTED_POWER, BULK_POWER
        };
        enum class ReturnState : uint8_t {
            ERR_TIME = 0, ERR_CHARGER = 1, ERR_BATTERY = 2, ERR_SOLAR_POWER = 3, OK_STAGE_I, OK_STAGE_II
        };
        enum class ErrorState : uint8_t {
            NO_ERROR, NO_DPL, NO_BATTERY_SOC, NO_BATTERY_INVERTER, NO_CHARGER_STATE, NO_BATTERY_CAPACITY
        };

        void loop(void); // unique lock and shared lock
        ErrorState checkSurplusRequirements(void);

        // NOTE: All private methods below EXPECTS the caller to hold a shared lock (only reads)
        // or exclusive lock (for writes).
        // It intentionally does not acquire the lock itself to avoid double-locking.
        // Ensure all callers acquire a proper lock before calling.
        frozen::string const& gStatusText(SurplusClass::State const state) const;
        frozen::string const& gExitText(SurplusClass::ReturnState const status) const;
        frozen::string const& gQualityText(float const qNr) const;
        frozen::string const& gErrorText(SurplusClass::ErrorState const status) const;
        String gDatumText(tm const& start, tm const& stop) const;
        void printReport(void);
        uint16_t calcBulkMode(uint16_t const requestedPower, ExternalData const& exVal);
        uint16_t calcSlopePower(uint16_t const requestedPower, int32_t const surplusPower, ExternalData const& exVal);
        uint16_t calcAbsorptionMode(uint16_t const requestedPower, ExternalData const& exVal);
        uint16_t returnFromSurplus(uint16_t const requestedPower, uint16_t const exitPower, SurplusClass::ReturnState const status);
        std::optional<uint16_t> gSolarPower(ExternalData const& exVal) const;
        int16_t gTimeToSunset(void);
        uint16_t gUpperPowerLimitSum(void) const;
        void triggerStageState(bool stageI, bool stageII);
        void resetSurplus(void);
        void exitSurplus(void);
        uint16_t gUpperPowerLimit(void) const;
        bool isStateIdleOrAbove() const {
            State state = _surplusState.load();
            return !((state == State::OFF) || (state == State::STARTUP) || (state == State::ERROR));
        }

        std::atomic<State> _surplusState = State::OFF;      // state machine
        int32_t _surplusPower = 0;                          // actual surplus power [W]
        uint16_t _surplusUpperPowerLimit = 0;               // upper power limit [W]
        ErrorState _errorState = ErrorState::NO_ERROR;      // error state
        Task _loopTask;                                     // task to print the report
        uint32_t _lastDebugPrint = 0;                       // last millis we printed the debug logging
        uint16_t _lastLoggingPower = 0;                     // the last logged surplus or slope power
        std::array<uint8_t, static_cast<size_t>(ReturnState::ERR_SOLAR_POWER) + 1> _errorCounter {0}; // counts all detected errors
        std::atomic<bool> _useSurplus = false;              // "Surplus" On/Off
        mutable std::shared_mutex _mutex;                   // mutex to protect the shared data

       // to handle stage-I (bulk mode)
        bool _stageIEnabled = false;                        // surplus-stage-I enable / disable
        bool _stageIActive = false;                         // true if stage-I is active
        int16_t _batterySafetyPercent = 20;                 // battery reserve power safety factor [%] (20 = 20%)
        int16_t _sunsetSafetyMinutes = 60;                  // time between absorption start and sunset [minutes] (60 = 1h)
        int32_t _batteryReserve = 0;                        // battery reserve power [W]
        uint32_t _lastReserveCalcMillis = 0;                // last millis we calculated the battery reserve power
        WeightedAVG<uint16_t> _avgSolSlow {30};             // the average helps by cloudy weather (4%) [W]
        WeightedAVG<uint16_t> _avgSolFast {5};              // the average helps by cloudy weather (20%) [W]
        uint16_t _solarPowerFiltered = 0;                   // filtered solar power used for calculation [W]
        tm _stageITimeStart {};                             // last time we enter stage-I
        tm _stageITimeStop {};                              // last time we exit from stage-I

        // to handle the slope power (bulk mode)
        bool _slopeEnabled = false;                         // slope mode enable / disable
        bool _slopeActive = false;                          // true if slope mode is active
        int16_t _slopeTarget = -20;                         // power target, on top of the requested power [W]
        int16_t _slopeFactor = -10;                         // slope decrease factor [W/s]
        int32_t _slopePower = 0;                            // actual slope power [W]
        uint32_t _slopeLastMillis = 0;                      // last millis we calculated the decrease of the slope power

        // to handle stage-II (absorption- and float-mode)
        bool _stageIIEnabled = false;                       // surplus-stage-II enable / disable
        bool _stageIIActive = false;                        // true if stage-II is active
        int16_t _powerStepSize = 0;                         // approximation step size [W]
        float _lastBatteryCurrent = 0.0f;                   // last battery current [A]
        uint32_t _lastInTargetMillis = 0;                   // last millis we hit the target
        uint32_t _lastCalcMillis = 0;                       // last millis we calculated the surplus power
        uint32_t _lastUpdate = 0;                           // last millis we updated the battery current
        uint32_t _regulationTime = 5;                       // time to regulate the surplus power [s]
        WeightedAVG<float> _avgTargetCurrent {4};           // average battery current for the target range[A]
        float _lowerTarget = 0.0f;                          // lower target for the battery current target range [A]
        float _upperTarget = 0.0f;                          // upper target for the battery current target range [A]
        float _chargeTarget = 0.0f;                         // target for the battery charge current [A]
        std::pair<uint32_t,float> _pLastI = {0, 0.0f};      // first of two voltages and related current [V,A]
        tm _stageIITimeStart {};                            // last time we enter stage-II
        tm _stageIITimeStop {};                             // last time we exit from stage-II

        // to handle the quality counter (absorption- and float-mode)
        int8_t _qualityCounter = 0;                         // quality counter
        WeightedAVG<float> _qualityAVG {10};                // quality counter average
        int16_t _lastAddPower = 0;                          // last power step
};

extern SurplusClass Surplus;
