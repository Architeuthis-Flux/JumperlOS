// SPDX-License-Identifier: MIT
// Parts picker / placement app (Guides-Simplification plan, workstream B).
// B-M2/M3 grow this file; for now it holds the sanctioned revision-swapped
// probe-button reader, moved verbatim from GuidedFlow.cpp's guideProbeButton
// so the reader outlives the modal guide runner (A-M3 deletes that file).

#include "PartsApp.h"

#include "Probing.h"   // probeButton
#include "config.h"    // jumperlessConfig.hardware.probe_revision

int partsProbeButton(void) {
    int bPress = probeButton.getButtonPress(true);
    if (jumperlessConfig.hardware.probe_revision > 3) {
        if (bPress == 1) bPress = 2;
        else if (bPress == 2) bPress = 1;
    }
    return bPress;
}
