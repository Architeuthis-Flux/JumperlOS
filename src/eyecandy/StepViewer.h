// SPDX-License-Identifier: MIT
#ifndef STEP_VIEWER_H
#define STEP_VIEWER_H

#include <Arduino.h>
#include "JumperlOS.h"

// Non-blocking guide-step viewer (Guides-Simplification A-M4; grip rules
// re-cut after the first bench pass, 2026-08-24).
//
// The old blocking guide ORDERED the user through its steps; the viewer just
// lets them BROWSE the step texts on the OLED while everything stays ambient.
//
// Bench law (the first flash was "a mess"): the viewer may only consume the
// wheel when the wheel has NO OTHER JOB and its own screen is the panel
// content - so what the wheel does is always what the OLED shows. Concretely
// it takes a turn only when no menu is up, nothing is BLOCKING, probe mode
// is off, and no net is highlighted (a probe tap hands the wheel to net
// scroll until the highlight times out). While armed but yielded (a reading
// or toast took the panel), the FIRST turn reclaims the screen without
// moving the cursor; turns after that scroll. Wheel turns are silent on
// serial - `VIEWER` machine lines come from `z steps` commands and
// state transitions only, never per-detent.
//
// Off-ramps: click-and-HOLD while the steps screen is showing (the physical
// exit - runPicker's HELD-cancels vocabulary), `z steps off`, board clear
// (parts.clear() zeroes guideSource, which auto-disarms), or any context
// switch away from the armed project. Serial twins: `z steps`,
// `z steps next|prev|<n>`, `z steps on|off`.
class StepViewer : public Service {
public:
    static StepViewer& getInstance();

    StepViewer(const StepViewer&) = delete;
    StepViewer& operator=(const StepViewer&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "StepViewer"; }
    // HIGH + registered BEFORE Highlighting: within a priority, registration
    // order is execution order, and the viewer must see wheel turns first
    // while it is armed. The inactive body is two loads.
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }

    // Parse `sourcePath` (the CANONICAL wiring) and arm the viewer at
    // `cursor` (clamped). Returns the step count, 0 when the guide has no
    // steps (viewer stays off), or -1 when the source cannot be read.
    int arm(const char* sourcePath, int cursor);
    void disarm();
    // NOT isActive(): Service::isActive() is a virtual with scheduler
    // semantics (last-status), and shadowing it here would change them.
    bool isArmed() const { return active; }
    int stepCount() const;
    int cursorIndex() const { return cursor; }

    // The `z steps ...` command body. rest = "" | "next" | "prev" | "<n>" |
    // "on" | "off". Prints its own machine lines.
    void command(const String& rest);

    // The armed step table (nullptr when not armed) - `z check step <k>`
    // reads the step + the script's power: values through this.
    const struct GuideScript* armedScript() const;

private:
    StepViewer() = default;
    ~StepViewer() = default;

    bool active = false;
    int cursor = 0;
    char armedSource[96] = {0};
    // Hold-to-exit edge latch: set when a HELD exit fires, cleared when the
    // button returns to rest (arm() also resets it). Without it the encoder
    // state machine's re-promotion (buttonHoldStart is unchanged while the
    // user keeps holding) would re-trigger the exit path every pass.
    bool holdLatch = false;

    void showStep(bool announce);
    void applyEmphasis();
};

extern StepViewer& stepViewer;

#endif // STEP_VIEWER_H
