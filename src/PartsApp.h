// SPDX-License-Identifier: MIT
#ifndef PARTS_APP_H
#define PARTS_APP_H

// Parts picker / placement app (Guides-Simplification plan, workstream B).
// This header starts life carrying only the sanctioned probe-button reader;
// the picker and placement flows land here in B-M2/M3.

// Probe-button polarity: the probe_revision>3 swap lives in exactly TWO
// sanctioned implementations - the MicroPython wrappers (jl_probe_button_*,
// JumperlessMicroPythonAPI.cpp) and this one; raw
// probeButton.getButtonPress() is unswapped. Post-swap semantics here:
// 0 = none, 1 = CONNECT/confirm, 2 = REMOVE/back.
// NOTE: getButtonPress(true) CONSUMES the press whatever its value - call
// once per loop pass and dispatch on the returned local (Menus.cpp:1046's
// eaten-press bug is why this warning exists).
int partsProbeButton(void);

#endif // PARTS_APP_H
