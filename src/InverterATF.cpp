// SPDX-License-Identifier: GPL-2.0-or-later

/* Inverter-ATF (Inverter - Adaptive Transfer Function)
 *
 * The linear transfer function "Limit = m * Power" of the inverter is not always linear. Especially at low power values
 * (below 10%) and high power values (above 90%) in combination with 24VDC, the HM-xxx inverter often shows a non linear behavior.
 * The negative effects are: Oscillations of the power, reduced efficiency and forcing the inverter into standby mode.
 * The problem can be solved with an adaptive transfer function (ATF).
 *
 * 30.01.2025 - 0.1 - First version
 * 06.03.2025 - 0.2 - Double search to find the left and right value and keep the array increasing
 * 30.10.2025 - 0.3 - Use 'Runtime Data' to store the correction table, add shared mutex for thread safety
 * 27.11.2025 - 0.4 - Improved range checks and averaging of new values
 * 07.01.2026 - 0.5 - Improved validation checks and speed optimizations
 * 01.06.2026 - 0.6 - Improved handling of the 'Runtime Data'
 * 01.10.2026 - 0.7 - Use cache for ATF data arrays to allow safe transfer between instances, allow 4 inverters
 * 25.10.2026 - 0.8 - Added detection of the flicker zone
 */

#include <LogHelper.h>
#include "Configuration.h"
#include "InverterATF.h"

#undef TAG
static const char* TAG = "ATF";
#define SUBTAG _serialPrefix

// runtime data key
static constexpr const char* DATA = "data";

static constexpr float MAX_ACCEPTABLE_FACTOR = 1.15f;   // 15% maximum factor for the power value compared to the nominal inverter power
static constexpr float MIN_ACCEPTABLE_POWER = 1.0f;     // 1W minimum acceptable power value
static constexpr float MAX_LINEAR_DIFF_P = 0.20f;       // +20% of the linear calculated power
static constexpr float MAX_LINEAR_DIFF_N = 0.12f;       // -12% of the linear calculated power
static constexpr float MAX_LINEAR_DIFF_A = 0.06f;       // 6% of the nominal power as absolute difference
static constexpr float MAX_OK_DIFF = 0.015f;            // 1.5% difference of 2 power values are acceptable for validation
static constexpr float AVG_LIMIT= 0.06f;                // 6% average limit for the power values
static constexpr float MAX_ACCEPTABLE_LIMIT = 68.0f;    // above 68% we accept the maximum acceptable power
static constexpr float MIN_FZD_P = 0.02f;               // 2% tolerance for FZ calculation
static constexpr float REDUCTION_FZ = 0.05f;            // 5% reduction for the FZ calculation

// cache for ATF data arrays to allow safe transfer between instances
struct ATFCache {
    std::unique_ptr<float[]> realPower = nullptr;
    uint64_t serial = 0;
    uint16_t nomPower = 0;
};
static constexpr uint8_t MG_CACHE_SIZE = 4;
static ATFCache mgCache[MG_CACHE_SIZE];


/*
 * destructor, frees the ATF data memory and unregisters the runtime provider if necessary
 */
InverterATF::~InverterATF() {

    std::unique_lock<std::shared_mutex> ulock(_mutex);

    // if not already done, unregister the runtime provider
    auto doLog = false;
    if (_registered.exchange(false)) {
        Runtime.unregisterProvider(getIdRT());
        _registered.store(false);
        doLog = true;
    }

    // if not already done, free the memory of the ATF data array
    if (_realPower != nullptr) {
        _realPower.reset();
        _realPower = nullptr;
        doLog = true;
    }

    if (doLog) {
        DTU_LOGD("deactivation of Inverter-ATF done");
    }
}


/*
 * activate ATF data array with linear values and request it from the runtime file
 * or move it back from the cache
 */
bool InverterATF::initATF(uint16_t const nomPower, uint64_t const serial) {

    std::unique_lock<std::shared_mutex> ulock(_mutex);

    if (_realPower != nullptr) { return true; } // already initialized

    // check if the inverter is listed in the configuration and get its name
    auto isNameAvailable = false;
    auto const& config = Configuration.get();
    for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& inv = config.Inverter[i];
        if (inv.Serial == 0ULL) { break; }
        if (inv.Serial != serial) { continue; }
        if (inv.Name[0] == '\0') { break; }

        // if available, use the name for logging purposes
        strncpy(_serialPrefix, inv.Name, sizeof(_serialPrefix) - 1);
        _serialPrefix[sizeof(_serialPrefix) - 1] = '\0';
        isNameAvailable = true;
        break;
    }

    // if no name was available in the configuration, use the serial
    if (!isNameAvailable) {
        strncpy(_serialPrefix, makeSerialString(serial).c_str(), sizeof(_serialPrefix) - 1);
        _serialPrefix[sizeof(_serialPrefix) - 1] = '\0';
    }

    // check if there is ATF data in the cache
    bool useDataFromCache = false;
    for (auto i = 0; i < MG_CACHE_SIZE; ++i) {
        if ((mgCache[i].realPower != nullptr) && (mgCache[i].serial == serial)) {
            useDataFromCache = true;
            break;
        }
    }

    _serial = serial;
    uint16_t useThisNomPower = 0;
    if (useDataFromCache) {
        useThisNomPower = moveDataBackFromCache();
        _state = InitState::CACHE_INIT;
    } else {
        useThisNomPower = nomPower;
        if (useThisNomPower != 0) {

            // allocate memory for the ATF data array
            _realPower = std::make_unique<float[]>(_size);
            if (_realPower == nullptr) {
                DTU_LOGE("unable to allocate memory for Inverter-ATF");
                return false;
            }
            _state = InitState::DEFAULT_INIT;
        }
    }

    if (useThisNomPower == 0) { return false; }

    _nomInvPower = useThisNomPower;
    _minAcceptablePower = MIN_ACCEPTABLE_POWER;
    _maxAcceptablePower = std::truncf(_nomInvPower * MAX_ACCEPTABLE_FACTOR);
    _absDiffPower = _nomInvPower * MAX_LINEAR_DIFF_A;
    _okDiffPower = _nomInvPower * MAX_OK_DIFF;
    _avgLimit = _nomInvPower * AVG_LIMIT;
    _minDiffFZ = _nomInvPower * MIN_FZD_P;

    // initialize the FZD data arrays
    for (auto i = 0; i < _sizeFZ; ++i) {
        _minFZ[i] = 0.0f;
        _maxFZ[i] = 0.0f;
    }

    // register this provider to be able to store the ATF data in the runtime file
    snprintf(_serialRT, sizeof(_serialRT), "atf_%s", makeSerialString(serial).c_str());
    Runtime.registerProvider(this, false);
    _registered.store(true);

    if (!useDataFromCache) {
        // init the ATF data array with 1% power steps
        auto powerStep = static_cast<float>( _nomInvPower ) / (_size - 1);
        for (auto idx = 0; idx < _size; ++idx) {
            _realPower[idx] = powerStep * idx;
        }
        Runtime.requestReadOnNextLoop(getIdRT());
    }

    _ready.store(true); // indicates that ATF is active and memory is allocated

    DTU_LOGD("activation of Inverter-ATF done");
    return true;
}


/*
 * returns the maximum number of inverters currently supported by ATF feature
 */
uint16_t InverterATF::getATFMaxSupportedInverter() const { return MG_CACHE_SIZE; }


/*
 * move ATF data into the temporary cache
 */
bool InverterATF::moveATFDataToCache(void) {

    std::unique_lock<std::shared_mutex> ulock(_mutex);

    // check if ATF data is available before moving it to the cache
    if (_realPower == nullptr) {
        DTU_LOGW("ATF data not available, skipping move to cache");
        return false;
    }

    // check if cache is available and find the first empty slot
    for (size_t i = 0; i < MG_CACHE_SIZE; ++i) {
        if (mgCache[i].realPower == nullptr) {

            // move the ATF data to the cache
            mgCache[i].realPower = std::move(_realPower);
            mgCache[i].serial = _serial;
            mgCache[i].nomPower = _nomInvPower;

            // indicate that the ATF is no longer active in this instance
            _realPower = nullptr;
            _state = InitState::OFF;
            _ready.store(false);
            DTU_LOGV("ATF data moved to temporary cache");
            return true;
        }
    }

    DTU_LOGW("ATF cache full, skipping move to cache");
    return false;
}


/*
 * move ATF data from the cache back to this instance
 */
uint16_t InverterATF::moveDataBackFromCache(void) {

    if (_realPower != nullptr) {
        DTU_LOGW("ATF data already available, skipping move");
        return 0;
    }

    for (size_t i = 0; i < MG_CACHE_SIZE; ++i) {

        if ((mgCache[i].realPower != nullptr) && (mgCache[i].serial == _serial)) {
            _realPower = std::move(mgCache[i].realPower);
            auto power = mgCache[i].nomPower; // store the nominal power from the cache before clearing it
            mgCache[i].realPower = nullptr;
            mgCache[i].serial = 0;
            mgCache[i].nomPower = 0; // clear nominal power from cache
            DTU_LOGV("ATF data moved back to instance");
            return power; // return the nominal power restored from cache
        }
    }

    DTU_LOGW("no ATF data available in temporary cache");
    return 0;
}


/*
 * clear the entire temporary cache, delete ATF data
 */
void InverterATF::freeATFCache(void) {
    std::unique_lock<std::shared_mutex> ulock(_mutex);

    // iterate through the cache and free the allocated data
    for (size_t i = 0; i < MG_CACHE_SIZE; ++i) {
        if (mgCache[i].realPower == nullptr) { continue; }

        ESP_LOGV(TAG, "[%s] unused ATF data removed from temporary cache", makeSerialString(mgCache[i].serial).c_str());
        mgCache[i].realPower.reset();
        mgCache[i].realPower = nullptr;
        mgCache[i].serial = 0;
        mgCache[i].nomPower = 0;
    }
}


/*
 * update the ATF data array with a new data pair, power [W] and limit [%].
 * checks whether the values ​​are valid, calculates an average value, and ensures the consistency of the data.
 */
void InverterATF::setATFData(float const power, float const limit, uint32_t const time) {

    if (!_ready.load()) { return; }

    std::unique_lock<std::shared_mutex> ulock(_mutex);
    if (_realPower == nullptr) { return; }

    // identical timestamp check and minimum power check, skip processing and avoid annoying logs
    if ((time == _lastMeterTime) || (power < _minAcceptablePower)) { return; }
    _lastMeterTime = time;

    DTU_LOGD("Limit: %0.1f%%, Power: %0.1fW, Timestamp: %u", limit, power, time);

    // safety range check, new values must be in an predefined range compared to the linear value
    auto linearPower = (_nomInvPower / 100.0f) * limit;
    auto minPower = _minAcceptablePower;
    if (linearPower > (minPower + _absDiffPower)) {
        minPower = std::min(linearPower - _absDiffPower, linearPower * (1.0f - MAX_LINEAR_DIFF_N));
    }
    auto maxPower = _maxAcceptablePower;
    if (limit < MAX_ACCEPTABLE_LIMIT) {
        maxPower = std::max(linearPower + _absDiffPower, linearPower * (1.0f + MAX_LINEAR_DIFF_P));
    }
    if ((limit <= 0.0f) || (limit > 100.0f) || (power < minPower) || (power > maxPower)) {

        // store the fault values
        _faultCounter++;
        _faultPower = static_cast<uint16_t>(power);
        _faultPowerMin = static_cast<uint16_t>(minPower);
        _faultPowerMax = static_cast<uint16_t>(maxPower);
        _faultLimit = limit;

        DTU_LOGW("safety range check failed [Power: %0.1fW, Min: %0.1fW, Max: %0.1fW], skip processing", power, minPower, maxPower);
        return; // out of range, ignore the new values
    }

    // average check: we check if new values are nearly identical with one of the 2 cached values
    // this avoids acceptance of faulty values during rapid or transient changes
    // Hint: We use 2 cached values to be more flexible on ping-pong changes
    auto useAverage = true;
    auto avgPower = power;
    for (auto i = 0; i < 2; ++i) {
        if (limit != _cachedLimit[i]) { continue; }
        if (fabs(power - _cachedPower[i]) > _okDiffPower) { continue; }

        // we have a match, we use the average of the new value and the cached value
        useAverage = false;
        avgPower = (_cachedPower[i] + avgPower) / 2.0f;
        break;
    }

    // update the cache
    _cachedLimit[1] = _cachedLimit[0];
    _cachedLimit[0] = limit;
    _cachedPower[1] = _cachedPower[0];
    _cachedPower[0] = power;
    _cache = { 0, 0.0f } ; // reset the getATFPower() cache to force recalculation

    auto idxL = static_cast<uint8_t>(limit); // remove decimal part
    auto idxR = idxL;
    if  ((limit - idxL) != 0.0f) { idxR = idxL + 1; } // we are between two indexes

    // Note: additional range checks are not necessary, because limit is between 0.1% and 100%
    // so idxL is between 0 and 100, and idxR is between 1 and 100
    auto powerL = _realPower[idxL];
    auto powerR = _realPower[idxR];
    auto newL = avgPower;
    DTU_LOGV("old values left: %u%%, %0.1fW, old values right: %u%%, %0.1fW", idxL, powerL, idxR, powerR);

    // check the observation flicker zone
    bool updateData = false;
    if (checkFZ(power, limit)) {

        // the limit is inside of the observation flicker zone
        if (idxL == idxR) { // we are exactly at an index, not between two indexes
            uint8_t idxFZ = limitToIndex(limit);
            if ((_minFZ[idxFZ] != 0.0f) && (_maxFZ[idxFZ] != 0.0f)) {
                newL = (_minFZ[idxFZ] + _maxFZ[idxFZ]) / 2.0f;
                updateData = true;
            } else if (_minFZ[idxFZ] != 0.0f) {
                newL = _minFZ[idxFZ];
                updateData = true;
            }
        }
    } else {

        // the limit is outside of the observation flicker zone
        updateData = true;
        if (idxL != idxR ) {
            // we are between two indexes, so we calculate the new power values for the left and right index
            float oldPower = powerL + (powerR - powerL) * (limit - idxL);
            if (oldPower <= 0.0f) { oldPower = 1.0f; } // avoid division by zero
            auto diffFactor = avgPower / oldPower - 1.0f;

            // we calculate the index values (left and right)
            if (powerL > 1.0f) {
                newL = powerL * ( 1.0f + diffFactor * (1.0f - (limit - idxL)));
            } else {
                if (idxL != 0) { newL = avgPower / 2; } // if the left power is too small, we just set it to half of the new power
            }
            auto newR = powerR * ( 1.0f + diffFactor * (1.0f - (idxR - limit)));

            _realPower[idxR] = makeAveragePower(newR, _realPower[idxR], useAverage);
        }
    }

    if (updateData) {
        _realPower[idxL] = makeAveragePower(newL, _realPower[idxL], useAverage);
        DTU_LOGV("new values left: %u%%, %0.1fW, new values right: %u%%, %0.1fW, average necessary: %s",
            idxL, _realPower[idxL], idxR, _realPower[idxR], (useAverage) ? "yes" : "no");
    } else {
        DTU_LOGV("no update necessary for limit: %0.1f", limit);
    }

    // we keep the array in an equal or increasing order to avoid problems if we calculate
    // the limit from the power. We check the left and the right side of the array
    // beginning from the closer changed index
    auto idxStart = ((limit - idxL) <= 0.5f) ? idxL : idxR;

    DTU_LOGV("array increasing check starting from index: %u%%", idxStart);

    // check if the right side from the start index is equal or increasing
    for (auto idx = idxStart; idx + 1 < _size; ++idx) {
        if (_realPower[idx + 1] < _realPower[idx]) {
            _realPower[idx + 1] = _realPower[idx];
        } else {
            if (idx > idxStart + 1) { break; } // we can stop if we are at least 2 indexes away from the start
        }
    }

    // check if the left side from the start index is equal or decreasing
    for (auto idx = idxStart; idx > 0; --idx) {
        if (_realPower[idx - 1] > _realPower[idx]) {
            _realPower[idx - 1] = _realPower[idx];
        } else {
            if (idx < idxStart - 1) { break; } // we can stop if we are at least 2 indexes away from the start
        }
    }
}


/*
 * we calculate a weighted average of 20% if:
 *  - averaging is requested, or
 *  - the difference between the new and old value is below the average limit.
 * in all other cases, we just use the new value
 */
float InverterATF::makeAveragePower(float newValue, float const oldValue, bool const average) {

    auto overrideAverage = (std::abs(newValue - oldValue) < _avgLimit) ? true : average;
    auto back = (overrideAverage) ? (newValue + oldValue * 4.0f) / 5.0f : newValue;
    if (back > _maxAcceptablePower) { back = _maxAcceptablePower; }
    return back;
}


/*
 * Get the power [W] of the given limit [%]
 * If the limit is between two values, then we do a linear interpolation
 * Note: We cache the last limit and power value to speed up repeated calls with the same limit
 */
uint16_t InverterATF::getATFPower(float const limit) const {

    if (!_ready.load()) { return 0; }

    {
        std::shared_lock<std::shared_mutex> slock(_mutex);

        if (_realPower == nullptr) { return 0; }
        if (limit <= 0.0f) { return 0; }
        if (limit >= 100.0f) { return static_cast<uint16_t>(_realPower[_size - 1]); }

        // shortcut, check the cache, if the new limit is the same as the last limit
        if (limit == _cache.second) { return _cache.first; }

    } // end of shared lock

    // cache miss -> unique lock to compute and update cache
    std::unique_lock<std::shared_mutex> ulock(_mutex);
    if (_realPower == nullptr) { return 0; }

    // re-check under exclusive lock
    if (limit == _cache.second) { return _cache.first; }

    // limit is between 0 and 100, so we can safely convert to uint8_t
    // idx is between 0 and 99, so idx+1 is between 1 and 100, safe to access the array without further range checks
    auto idx = static_cast<uint8_t>(limit); // remove decimal part
    float power = 0.0f;
    if (limit == static_cast<float>(idx)) {
        power = _realPower[idx]; // exact match
    } else {
        float m = (_realPower[idx + 1] - _realPower[idx]);   // 1% step
        power = _realPower[idx] + m * (limit - static_cast<float>(idx));
    }
    _cache.first = static_cast<uint16_t>(power + 0.5f); // round to nearest integer
    _cache.second = limit;                              // store the new limit in the cache

    DTU_LOGD("given limit: %0.1f%%, calculated power: %uW", _cache.second, _cache.first);

    return _cache.first;
}


/*
 * Get the limit [%] of the given power [W]
 * If the limit is between two index values, then we do a linear interpolation
 * Note: We do not cache the result here, because the function is called on the end of the power limiter loop
 * and the next step will be an ATF data update anyway.
 */
float InverterATF::getATFLimit(uint16_t const power) {

    if (!_ready.load()) { return 0.0f; }

    std::shared_lock<std::shared_mutex> slock(_mutex);
    if (_realPower == nullptr) { return 0.0f; }

    // range checks
    if (power <= 0) { return 0.0f; }
    if (power >= static_cast<uint16_t>(_realPower[_size - 1])) { return 100.0f; }
    auto fPower = static_cast<float>(power);

    // check if the power is within the active flicker zone
    if (_activeFZ && (fPower >= _minFZ[_lowerIdxFZ]) && (fPower <= _maxFZ[_upperIdxFZ])) {

        auto mid = (_minFZ[_lowerIdxFZ] + _maxFZ[_upperIdxFZ]) / 2.0f;
        auto fzIndex = (fPower <= mid) ? _lowerIdxFZ : _upperIdxFZ;
        auto fzLimit = indexToLimit(fzIndex);

        // note: to be sure the flicker zone is still correct, we must allow to use the flicker zone from time to time
        if (_lastNormalCheckFZ <= 10) {
            fzLimit = (fPower <= mid) ? fzLimit - 0.1f : fzLimit + 0.1f;
            ++_lastNormalCheckFZ;
        } else {
            _lastNormalCheckFZ = 0;
        }
        fzLimit = std::clamp(fzLimit, 0.0f, 100.0f);

        DTU_LOGD("given power: %uW, flicker zone limit: %0.1f%%", power, fzLimit);
        return fzLimit;
    }

    // search from min W to max W (index 1 to 100) to find the left index
    auto equalFound = false;
    uint8_t idxLeft = 0;
    for (uint8_t idx = 1; idx < _size; ++idx) {
        if (fPower < _realPower[idx]) {
            idxLeft = idx - 1;
            break;
        } else if (fPower == _realPower[idx]) {
            idxLeft = idx;
            equalFound = true;
            break;
        }
    }
    if (idxLeft >= _size) { idxLeft = _size - 1; } // safety check
    auto limit = static_cast<float>(idxLeft);

    // linear interpolation is not necessary if
    // the left power value matches the given power or if we reached the next-to-last of the array
    if (!equalFound && (idxLeft < _size - 1)) {

        // linear interpolation between the left and right value
        auto const& powLeft = _realPower[idxLeft];
        auto const& powRight = _realPower[idxLeft + 1];

        // safety check to avoid division by zero and use of invalid values
        if ( powLeft < powRight) {
            auto m = 1.0f / (powRight - powLeft);
            limit = limit + m * (fPower - powLeft);
            if (limit < 0.0f) { limit = 0.0f; }
            if (limit > 100.0f) { limit = 100.0f; }
        }
        // else: we have identical power values, so we cannot interpolate and just use the left limit
    }
    limit = roundf(limit * 10.0f) / 10.0f; // round to 1 decimal place

    DTU_LOGD("given power: %uW, calculated limit: %0.1f%%", power, limit);
    return limit;
}


/*
 * Check if the given power falls within the flicker zone (FZ) defined by the limit.
 * The FZ is assumed to be defined between _startFZ and (_startFZ + (_sizeFZ - 1) * 0.1f).
 * Returns true if the power is within the FZ, false otherwise.
 */
bool InverterATF::checkFZ(float const power, float const limit) {

    // we assume that the FZ (flicker zone) is between 14% and 16% for all kind of inverters
    if ((limit < _startFZ) || (limit > (_startFZ + (_sizeFZ - 1) * 0.1f))) { return false; }

    auto idx = limitToIndex(limit);

    // if the minimum FZ value is not set, initialize it with the current power
    if (_minFZ[idx] == 0.0f) {
        _minFZ[idx] = power;
        _maxFZ[idx] = 0.0f;
        return true; // just one value set, cannot determine range yet
    }

    // if the minimum FZ value is set but the maximum is not, we try to determine the maximum based on the current power
    if (_maxFZ[idx] == 0.0f) {

        if (power >= (_minFZ[idx] + _minDiffFZ)) {
            _maxFZ[idx] = power;
        }
        else if (power <= (_minFZ[idx] - _minDiffFZ)) {
            _maxFZ[idx] = _minFZ[idx];
            _minFZ[idx] = power;
        }
        else {
            // the current power is not sufficiently different from the existing minimum, so we just average it
            _minFZ[idx] = (_minFZ[idx] + power) / 2.0f;
            return true; // just one value set, cannot determine range yet
        }
    } else {

        // if both minimum and maximum FZD values are set, we adjust them based on the current power
        // the slowly reduction of the other value helps to repair wrong or no longer valid values
        auto min = std::abs(_minFZ[idx] - power);
        auto max = std::abs(_maxFZ[idx] - power);
        if ( min < max) {
            _minFZ[idx] = (_minFZ[idx] + power) / 2.0f;
            _maxFZ[idx] = _maxFZ[idx] * (1.0f - REDUCTION_FZ);
        } else {
            _maxFZ[idx] = (_maxFZ[idx] + power) / 2.0f;
            _minFZ[idx] = _minFZ[idx] * (1.0f + REDUCTION_FZ);
        }
    }

    // check if the FZ range is too narrow and reset if necessary
    if ((std::abs(_maxFZ[idx] - _minFZ[idx])) < (_minDiffFZ)) {
        _minFZ[idx] = _maxFZ[idx] = 0.0f;
    }

    // find the lower and the upper bound of the FZ range
    uint8_t _found = 0;
    _lowerIdxFZ = _upperIdxFZ = 0;
    for (auto i = 0; i < _sizeFZ; ++i) {
        if ((_minFZ[i] != 0.0f) && (_maxFZ[i] != 0.0f)) {
            _lowerIdxFZ = i;
            ++_found;
            break;
        }
    }
    for (auto i = _sizeFZ - 1; i >= 0; --i) {
        if ((_minFZ[i] != 0.0f) && (_maxFZ[i] != 0.0f)) {
            _upperIdxFZ = i;
            ++_found;
            break;
        }
    }

    // set the active flag if both bounds are valid
    _activeFZ = (2 == _found);
    return true;
}


/*
 * Serialize the ATF data array into the runtime file
 */
void InverterATF::serializeRT(JsonObject obj) const {

    if (!_ready.load()) { return; } // if ATF is not active, we do not store any data

    std::shared_lock<std::shared_mutex> slock(_mutex);
    if (_realPower == nullptr) { return; }

    JsonArray data = obj[DATA].to<JsonArray>();
    for (auto idx = 0; idx < _size; ++idx) {
	    data.add(roundf(_realPower[idx] * 10.0f) / 10.0f); // round to 1 decimal place
    }
}


/*
 * Deserialize ATF data array from the runtime file
 */
void InverterATF::deserializeRT(JsonObject obj) {

    if (!_ready.load()) { return; } // if ATF is not active, we do not load any data

    // if runtime data is not available, we exit and use the initialization values
    // This can happen, if the device is started for the first time or if the runtime file is corrupted
    if (obj.isNull()) { return; }

    std::unique_lock<std::shared_mutex> ulock(_mutex);
    if (_realPower == nullptr) { return; }

    if ((_state == InitState::RTD_INIT) || (_state == InitState::CACHE_INIT)) { return; } // already initialized

    // if the data from the runtime file does not exist or the size is not identical,
    // then we do not copy any values and keep the initialization values
    JsonArray data = obj[DATA];
    if (data.size() == _size) {
        auto idx = 0;
        for (JsonVariantConst value : data) {
            float newValue = value.as<float>();

            // safety range check
            newValue = std::clamp(newValue, 0.0f, _maxAcceptablePower);
            _realPower[idx] = newValue;
            ++idx;
        }
        _state = InitState::RTD_INIT;
    }
}


/*
 * convert the serial to String
 */
String InverterATF::makeSerialString(uint64_t serial) const {
    char string[32];
    snprintf(string, sizeof(string), "%0x%08x",
    static_cast<uint32_t>((serial >> 32) & 0xFFFFFFFF),
    static_cast<uint32_t>(serial & 0xFFFFFFFF));
    return String(string);
}


/*
 * print the ATF report
 */
void InverterATF::printATFReport(void) const {

    if (!_ready.load()) { return; }

    std::shared_lock<std::shared_mutex> slock(_mutex);
    if (_realPower == nullptr) { return; }

    DTU_LOGI("");
    DTU_LOGI("------------------------- ATF Report --------------------------");
    DTU_LOGI("Inverter name: %s, Nominal power: %uW", _serialPrefix, _nomInvPower);
    DTU_LOGI("Data source: Initialized by %s values", (_state == InitState::RTD_INIT) ? "runtime" : (_state == InitState::DEFAULT_INIT) ? "default" : "cache");
    DTU_LOGI("Data safety range faults: %u, Last fault: %uW [Min: %uW, Max: %uW, Limit: %0.1f%%]",
        _faultCounter, _faultPower, _faultPowerMin, _faultPowerMax, _faultLimit);

    bool error = false;
    for (auto idx = 1; idx < _size; ++idx) {
        if (_realPower[idx-1] > _realPower[idx]) { error = true; break; }
    }
    DTU_LOGI("Data array increasing: %s", error ? "No" : "Yes");

    // search for biggest difference between real power values and their expected linear progression
    float maxDifferenceWatt = 0.0f;
    auto maxDiffLimit = 0;
    for (auto idx = 1; idx < _size; ++idx) {
        float expected = static_cast<float>(idx) / (_size - 1) * _nomInvPower;
        float difference = std::abs(_realPower[idx] - expected);
        if (difference > maxDifferenceWatt) {
            maxDifferenceWatt = difference;
            maxDiffLimit = idx;
        }
    }
    DTU_LOGI("Maximum deviation ATF <-> Linear: %0.1fW [limit: %u%%]", maxDifferenceWatt, maxDiffLimit);

    if (!_activeFZ) {
        DTU_LOGI("Flicker zone detected: No");
    } else {
        DTU_LOGI("Flicker zone detected: Yes [%0.1f%% - %0.1f%%]", indexToLimit(_lowerIdxFZ), indexToLimit(_upperIdxFZ));
    }
    DTU_LOGI("Flicker zone minimum power difference: %0.1fW", _minDiffFZ);

    DTU_LOGV("");
    DTU_LOGV("ATF data details");
    size_t columnWidth = 5;
    size_t columnMaxNr = 20;
    size_t rowMaxNr = 5;
    char cBuffer[columnMaxNr * columnWidth + 10];

    for (auto row = 0; row < rowMaxNr; ++row) {
        for (auto kind = 0; kind < 2; ++kind) {
            char *cBuf = cBuffer;
            auto remaining = sizeof(cBuffer);
            for (auto idx = 1; idx <= columnMaxNr; ++idx) {
                auto globalIdx = row * columnMaxNr + idx;
                if (globalIdx >= _size) break;

                if (kind == 0) { snprintf(cBuf, remaining, "%4i%%", static_cast<int>(globalIdx)); } // Limit
                else { snprintf(cBuf, remaining, "%5.0f", _realPower[globalIdx]); } // Power

                cBuf += columnWidth;
                remaining -= columnWidth;
            }
            DTU_LOGV("%s", cBuffer);
        }
    }

    DTU_LOGD("");
    DTU_LOGD("Flicker zone details");
    columnWidth = 6;
    columnMaxNr = 11;
    rowMaxNr = 2;
    char cBufferFZ[columnMaxNr * columnWidth + 10];
    for (auto row = 0; row < rowMaxNr; ++row) {
        for (auto kind = 0; kind < 3; ++kind) {
            char *cBuf = cBufferFZ;
            auto remaining = sizeof(cBufferFZ);
            for (auto idx = 0; idx < columnMaxNr; ++idx) {
                auto index = row * columnMaxNr + idx;
                if (index >= _sizeFZ) break;

                if (kind == 0) { snprintf(cBuf, remaining, "%5.1f%%", indexToLimit(index)); } // Limit
                else if (kind == 1) { snprintf(cBuf, remaining, "%6.1f", _maxFZ[index]); } // Maximum FZ value
                else if (kind == 2) { snprintf(cBuf, remaining, "%6.1f", _minFZ[index]); } // Minimum FZ value

                cBuf += columnWidth;
                remaining -= columnWidth;
            }
            DTU_LOGD("%s", cBufferFZ);
        }
    }

    DTU_LOGI("---------------------------------------------------------------");
    DTU_LOGI("");
}
