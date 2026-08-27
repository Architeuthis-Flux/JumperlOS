// SPDX-License-Identifier: MIT
#ifndef PARTS_APP_H
#define PARTS_APP_H

#include "States.h"   // PartDefinition (the part card)

// Parts picker / placement app (Guides-Simplification workstream B, M2/M3).
//
// The top-level "Parts" menu row lands here (menuTree.h row -> Menus.cpp
// getActionCategory "Parts" branch -> runApp(-1, "Parts") -> apps[] row -
// ONE unit, rename all four together). The app is a browse + tap-to-place
// flow over the flash parts DB (src/partdb/): pick a class, pick a part,
// tap the row where pin 1 goes, and the app EXITS - labels bloom and
// everything after is ambient (PartLabels/DisplayService take over).
//
// Machine lines (HIL): `PARTPICK level=<class|part> n=<count>` per picker,
// `PARTDB place ok=<NAME> row=<N>` on commit, `PARTDB place refused
// reason="..."` on a geometry refusal. Serial twins: while the tap prompt
// is up, a typed row number + enter places without the probe; any other
// serial byte cancels out of the app (the runPicker convention).
void partsAppLauncher(void);
void partsTestLauncher(void);
void partsAutoLauncher(void);

// The part card (Kevin's spec, 2026-08-27): name / type / cached test data /
// "E - 17  B - 18  C - 19" (LEDs label polarity: "A+ - 21  K- - 51").
// focusPin brackets one pin ([B - 18]); -1 = the whole part. 128x32, the
// BJT-card idiom (four 5pt rows). The encoder scroll's part focus calls it.
void partsShowPartCard(const PartDefinition& p, int focusPin);

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
