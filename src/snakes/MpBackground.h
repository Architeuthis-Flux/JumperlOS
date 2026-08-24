// SPDX-License-Identifier: MIT
#ifndef MP_BACKGROUND_H
#define MP_BACKGROUND_H

#include <Arduino.h>
#include "JumperlOS.h"

// Background MicroPython callback runner (Guides-Simplification workstream D;
// prior art: the Temporal-Replay badge's MicroPythonMatrixService).
//
// A script registers a callback with jumperless.bg_start(cb, interval_ms);
// this service ticks it from the main loop after the script ends. Each tick
// is one short bounded call into the interpreter (modjumperless.c's
// jl_bg_service_tick: interval-gated, nlr-protected, ONE-STRIKE - a raising
// callback prints its traceback and deactivates).
//
// NOT in the inner set, deliberately twice over: the scheduler is the
// foreground guard (ticks can never land while a foreground script or REPL
// command owns core 0), and the service pauses in probe mode/menus - Kevin's
// "it can pause when in probe mode". It claims nothing exclusively.
//
// Pin-claim survival: gpio claims made by a script whose background callback
// is ACTIVE survive script exit (the release in
// jl_exit_micropython_restore_entry_state is skipped); this service releases
// them on the callback's deactivation transition (bg_stop / one-strike /
// interpreter teardown).
class MpBackgroundService : public Service {
public:
    static MpBackgroundService& getInstance();

    MpBackgroundService(const MpBackgroundService&) = delete;
    MpBackgroundService& operator=(const MpBackgroundService&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "MpBackground"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    uint32_t periodUs() const override { return 5000; }

private:
    MpBackgroundService() = default;
    ~MpBackgroundService() = default;
    bool wasActive = false;
};

extern MpBackgroundService& mpBackgroundService;

#endif // MP_BACKGROUND_H
