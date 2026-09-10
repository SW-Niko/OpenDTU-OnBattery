// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <powermeter/Provider.h>
#include <TaskSchedulerDeclarations.h>
#include <memory>
#include <mutex>

namespace PowerMeters {

class Controller {
public:
    void init(Scheduler& scheduler);

    void updateSettings();

    float getPowerTotal() const;
    uint32_t getLastUpdate() const;
    bool isDataValid() const;

    // get the configured settling time
    uint32_t getSettlingTime() const;

    // the power measurement will be paused for the specified duration in milliseconds.
    void setPause(uint32_t duration) const;

    // stop the pause and resume to normal operation (before the specified duration elapses).
    void stopPause() const;

    // check if the pause feature is supported by the current provider
    bool isPauseSupported() const;

private:
    void loop();

    Task _loopTask;
    mutable std::mutex _mutex;
    std::unique_ptr<Provider> _upProvider = nullptr;
};

} // namespace PowerMeters

extern PowerMeters::Controller PowerMeter;
