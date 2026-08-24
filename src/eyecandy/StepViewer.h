// SPDX-License-Identifier: MIT
#ifndef STEP_VIEWER_H
#define STEP_VIEWER_H

#include <Arduino.h>
#include "JumperlOS.h"

// Non-blocking guide-step viewer (Guides-Simplification A-M4).
//
// The old blocking guide ORDERED the user through its steps; the viewer just
// lets them BROWSE the step texts on the OLED while everything stays ambient.
// While armed, the wheel scrolls steps (that is its browse job here - row
// scroll comes back the moment the viewer is off), the current step's rows
// glow through PartLabels' emphasis, and the cursor persists as
// parts.guideStep via the ordinary idle auto-save. Nothing is captured
// beyond wheel TURNS: clicks still open the menu, every OLED consumer still
// wins the panel (the retained screen is the idle screen and yields via
// notePanelTakenByOther), and there is no ordering, no checks, no gates.
//
// Off-ramps: `z steps off`, board clear (parts.clear() zeroes guideSource,
// which auto-disarms), or any context switch away from the armed project.
// Serial twins (every gesture has one): `z steps`, `z steps next|prev|<n>`,
// `z steps on|off`.
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

    void showStep(bool announce);
    void applyEmphasis();
};

extern StepViewer& stepViewer;

#endif // STEP_VIEWER_H
