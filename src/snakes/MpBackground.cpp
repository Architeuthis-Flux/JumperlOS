// SPDX-License-Identifier: MIT
// Background MicroPython callback runner. Contract: MpBackground.h.

#include "MpBackground.h"

#include "Python_Proper.h"   // isMicroPythonInitialized

extern "C" {
int jl_bg_active(void);
int jl_bg_service_tick(uint32_t now_ms);
void jl_gpio_release_all_pins(void);
}

MpBackgroundService& MpBackgroundService::getInstance() {
    static MpBackgroundService instance;
    return instance;
}
MpBackgroundService& mpBackgroundService = MpBackgroundService::getInstance();

ServiceStatus MpBackgroundService::service() {
    if (!isMicroPythonInitialized()) {
        // Interpreter teardown killed the root pointer with it; the pins were
        // already released by the deinit path. Just forget the edge.
        wasActive = false;
        return ServiceStatus::IDLE;
    }

    bool nowActive = jl_bg_active() != 0;
    if (wasActive && !nowActive) {
        // Deactivation transition (bg_stop or one-strike): the pin claims
        // that survived script exit for this callback are released now, and
        // the C++ owners (refreshConnections' pull re-assert, DisplayService)
        // get their hardware back.
        jl_gpio_release_all_pins();
    }
    wasActive = nowActive;
    if (!nowActive) {
        return ServiceStatus::IDLE;
    }

    return jl_bg_service_tick(millis()) ? ServiceStatus::BUSY
                                        : ServiceStatus::IDLE;
}
