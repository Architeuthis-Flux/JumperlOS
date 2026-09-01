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
void partsRemoveLauncher(void);   // Parts > Remove Parts, straight into the
                                  // remove flow (not the class picker)

// The ONE yes/no gesture set (Kevin, 2026-08-28): probe CONNECT or a short
// encoder click = yes, probe REMOVE or a hold = no - consistent everywhere,
// so prompts never spend OLED lines on a button legend. Shows `text`
// (multiline ok) on the OLED and blocks for the answer. Serial twins:
// y/Y = yes, n/N = no, any other byte = -1 (cancel, the picker convention).
int partsConfirmYesNo(const char* text);

// Remove every part record (bridges, net names, guide progress), no
// confirmation - Parts > Remove's All stop asks first and then calls this;
// the `x` command calls it directly (clearing the board clears its parts
// too, Kevin's ruling 2026-08-27), with refresh=false because x runs its
// own full refresh right after and paying for two fabric rebuilds doubled
// x's latency. Returns how many parts were removed.
int partsClearAllRecords(bool refresh = true);

// Appends " <part> <pin>" (the "7447 LT" idiom every highlight surface
// uses) to a node's display name when a placed part's pin sits on that
// row; any other node leaves the buffer untouched. The probe's connect /
// clear taps compose their node names through this (Kevin, 08:38: "along
// with the node, we should also show the pin label").
void partsAppendPinLabel(int node, char* buf, size_t cap);

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

// Tier-3 vector identification (DESIGN_IC_IDENTIFICATION.md 5.2): run
// every same-footprint partdb candidate's truth-table vectors against the
// dipN chip anchored at baseRow (bottom half) whose rails were measured at
// gndRow/vddRow. One entry per candidate TRIED (its rails landed on the
// measured ones and it has vectors): verdict 1 = every checked step
// agreed, 0 = a step disagreed (failStep says which), -1 = refused
// (resources / wiring / overcurrent - says nothing about the part).
// rotated = the orientation the rails forced (1 = pin 1 top-right), which
// is exactly the pin-1 answer a placement needs. Returns entries written,
// <0 = bad args. V5 only.
struct VectorIdentifyResult {
    uint16_t recIdx;    // partdb record index
    uint8_t rotated;    // 1 = pin 1 top-right
    int8_t verdict;     // 1 pass, 0 fail, -1 refused
    int8_t failStep;    // first disagreeing step when verdict == 0;
                        // -2 = the icc band refused it (record's
                        // iccMin10/iccMax10 vs the measured feed)
    int16_t icc10;      // measured feed current during the run, mA*10;
                        // -1 = not measured (board-powered / refused
                        // before power-up)
};
// fpMeasured (nullable): a measured Tier-1 clamp string (the part_fingerprint
// fp= alphabet) - candidates whose authored fingerprint conflicts hard are
// skipped BEFORE being powered.
int partsVectorIdentify(int baseRow, int width, int gndRow, int vddRow,
                        VectorIdentifyResult* out, int maxOut,
                        const char* fpMeasured = nullptr,
                        int* triedOut = nullptr);

#endif // PARTS_APP_H
