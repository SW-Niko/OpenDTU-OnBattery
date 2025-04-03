// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Locking policy:
 * - Public getters having the prefix 'get' take a shared lock internally.
 * - Public mutating methods take an exclusive lock internally.
 * - Private methods are called with the appropriate lock held by the public methods.
 * - Fetching external data is done before acquiring _mutex. The only exception is data from the configuration
 * - Flags marked as atomic may be read lock-free.
 */

#pragma once
#include <Arduino.h>
#include <cmath>
#include <shared_mutex>
#include <atomic>
#include <ArduinoJson.h>
#include "RuntimeData.h"


class InverterATF : public InterfaceProviderRT {
    public:
        InverterATF() = default;
        ~InverterATF();
        InverterATF(const InverterATF&) = delete;
        InverterATF& operator=(const InverterATF&) = delete;
        InverterATF(InverterATF&&) = delete;
        InverterATF& operator=(InverterATF&&) = delete;

        // indicates whether ATF is active, and can be used for further operations
        // this method uses atomic read
        bool isATFActive() const { return _ready.load(); }

        // activate the ATF data structure
        // this method uses a unique lock
        bool initATF(uint16_t const nomPower, uint64_t const serial);

        // move the ATF data into a temporary cache
        // returns true if the data was successfully moved, false otherwise
        // this method uses a unique lock
        bool moveATFDataToCache(void);

        // free the temporary cache, delete ATF data
        // this method uses a unique lock
        void freeATFCache(void);

        // update the ATF data with a current power and limit pair
        // this method uses a unique lock
        void setATFData(float const power, float const limit, uint32_t const time);

        // print the ATF report
        // this method uses a shared lock
        void printATFReport(void) const;

        // returns the power for the given limit calculated according to the ATF data
        // this method uses a shared/unique lock
        uint16_t getATFPower(float const limit) const;

        // returns the limit for the given power calculated according to the ATF data
        // this method uses a shared lock
        float getATFLimit(uint16_t const power);

        // returns the maximum number of inverters currently supported by ATF feature
        // this method requires no lock
        uint16_t getATFMaxSupportedInverter() const;

        // serialize and deserialize the ATF data to the runtime file
        // derived from InterfaceProviderRT
        // these methods use a shared/unique lock respectively
        String getIdRT() const override { return _serialRT; }
        void serializeRT(JsonObject obj) const override;
        void deserializeRT(JsonObject obj) override;

    private:
        // calculate the average power based on the new value, the old value, and a weighting factor
        float makeAveragePower(float newValue, float const oldValue, bool const factor);

        // build the serial string from the serial number
        String makeSerialString(uint64_t serial) const;

        // move ATF data from the cache back to this instance
        // returns the nominal power if the data was successfully moved, 0 otherwise
        uint16_t moveDataBackFromCache(void);

        // check if the given power and limit are inside the flicker zone
        bool checkFZ(float const power, float const limit);

        // convert an index to the corresponding limit [%] within the flicker zone
        float indexToLimit(uint8_t const index) const { return _startFZ + static_cast<float>(index) / 10.0f; };
        uint8_t limitToIndex(float const limit) const { return static_cast<uint8_t>(std::round((limit - _startFZ) * 10.0f)); }

        enum class InitState : uint8_t { OFF, DEFAULT_INIT, RTD_INIT, CACHE_INIT };

        std::atomic<bool> _ready = false;               // false: ATF not active and no memory allocated for the data array
        std::atomic<bool> _registered = false;          // false: the provider is not registered in the RuntimeProvider
        InitState _state = InitState::OFF;              // indicates how the ATF data array was initialized
        uint64_t _serial = 0;                           // serial number of the inverter as integer, used for identification
        char _serialRT[36] = "unknown";                 // serial number of the inverter as hex string, used for serialization
        char _serialPrefix[32] = "unknown";             // prefix used for logging, typically includes the inverter serial number
        static constexpr uint8_t _size = 101;           // Fixed size of the ATF data array, never NEVER change the size!
        std::unique_ptr<float[]> _realPower = nullptr;  // ATF data array, index 0-100%

        uint16_t _nomInvPower = 0;                      // Inverter nominal power [W]
        float _minAcceptablePower = 0.0f;               // minimum acceptable power for the validation check
        float _maxAcceptablePower = 0.0f;               // maximum acceptable power for the validation check, calculated from the nominal power [W]
        float _absDiffPower = 0;                        // maximum absolute difference, used for power value checks [W]
        float _okDiffPower = 0.0f;                      // maximum acceptable difference for power value checks [W]
        float _avgLimit = 0.0f;                         // average limit for the power values, used for smoothing
        float _cachedLimit[2] = { 0.0f, 0.0f };         // cache for the last two limit values used for averaging
        float _cachedPower[2] = { 0.0f, 0.0f };         // cache for the last two power values used for averaging
        uint32_t _lastMeterTime = 0;                    // last timestamp of the power value [ms]

        mutable std::pair<uint16_t, float> _cache;      // cache to avoid recalculations, first = power, second = limit
        mutable std::shared_mutex _mutex;               // mutex to protect the shared data

        uint16_t _faultCounter = 0;                      // counts the number of safety range check faults
        uint16_t _faultPower = 0;                        // last fault power value [W]
        uint16_t _faultPowerMin = 0;                     // minimum acceptable power at the time of the last fault [W]
        uint16_t _faultPowerMax = 0;                     // maximum acceptable power at the time of the last fault [W]
        float _faultLimit = 0.0f;                        // limit at the time of the last fault [%]

        static constexpr float _startFZ = 14.0f;        // start of the observation flicker zone [%]
        static constexpr uint8_t _sizeFZ = 21;          // fixed size of the FZ data array, never NEVER change the size!
        float _minFZ[_sizeFZ] = { 0.0f };               // observation FZ data array, minimum values, 14% - 16%, 0.1% steps, 21 values
        float _maxFZ[_sizeFZ] = { 0.0f };               // observation FZ data array, maximum values, 14% - 16%, 0.1% steps, 21 values
        bool _activeFZ = false;                         // true: flicker zone detected and active
        uint8_t _lowerIdxFZ = 0;                        // lower bound of the flicker zone
        uint8_t _upperIdxFZ = 0;                        // upper bound of the flicker zone
        float _minDiffFZ = 0.0f;                        // minimum flicker difference [W]
        uint8_t _lastNormalCheckFZ = 0;                 // last count of the normal flicker check
};
