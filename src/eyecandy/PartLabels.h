// SPDX-License-Identifier: MIT
#ifndef PART_LABELS_H
#define PART_LABELS_H

#include <Arduino.h>
#include "JumperlOS.h"
#include "JumperlessDefines.h"
#include "States.h"            // MAX_PARTS

// Ambient part-pin labels (Guides-Simplification plan, workstream A).
//
// One composed GraphicOverlay ("_PARTS_") marks every part pin with a single
// LED at the OUTER EDGE of its row - the hole a chip body can never cover -
// in its pin-class color, leaving the inner four LEDs showing live net color
// so Highlighting / MeasureMode coexist by construction. Label TEXT goes to
// the OLED via tap-to-inspect (a breadboard row is one pixel-column of the
// text raster; per-row text is geometrically impossible).
//
// AUTO-HIDE (Kevin's ruling, 2026-08-24): markers show only while the part is
// active or highlighted - just placed/changed (bloom), probe tap on one of
// its rows, its net highlighted/brightened, step-viewer emphasis, or an
// active warning (warnings ALWAYS show, full column). An idle board stays
// dark.
//
// Warn, never block: wiring that contradicts a pin's class paints the whole
// column in the warning color and prints one PARTWARN line. Rails always obey
// the user; nothing is ever gated or forced.
//
// The service composes ON CHANGE only (fingerprint compare per pass, ~50 Hz);
// every post is clear-first requestLedShow(-1). NOT in the inner set: it
// freezes during menus/probe mode while core 1 keeps rendering the already-
// registered overlay.
class PartLabels : public Service {
public:
    static PartLabels& getInstance();

    PartLabels(const PartLabels&) = delete;
    PartLabels& operator=(const PartLabels&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "PartLabels"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    uint32_t periodUs() const override { return 20000; }   // 50 Hz listen cadence

    // Synchronous recompose + post, for callers that change state while this
    // service can't run (SlotManager preview enter/exit inside the menu loop).
    void recomposeNow();

    // Step-viewer emphasis: these breadboard nodes render edge + inward
    // neighbor, brightened, and their owning parts count as visible.
    // count = 0 (or nodes = nullptr) clears.
    void setEmphasis(const int16_t* nodes, int count);
    void clearEmphasis() { setEmphasis(nullptr, 0); }

    bool hasWarnings() const { return warnActiveMask != 0; }

    static const int MAX_EMPHASIS_NODES = 16;

private:
    PartLabels() = default;
    ~PartLabels() = default;

    // --- change detection ---
    uint32_t lastFingerprint = 0;
    uint32_t partHash[MAX_PARTS] = {0};
    uint32_t partsFoldLast = 0;
    uint32_t netlistHash = 0;
    uint32_t powerHash = 0;

    // --- visibility (auto-hide) ---
    unsigned long bloomUntilMs[MAX_PARTS] = {0};     // placed/changed
    unsigned long inspectUntilMs[MAX_PARTS] = {0};   // probe-tapped
    uint32_t highlightVisMask = 0;                   // net highlighted/brightened
    int lastHighlightedNet = -2, lastBrightenedNet = -2, lastBrightenedNode = -2;

    // --- warnings ---
    uint32_t warnActiveMask = 0;                     // bit per part
    uint8_t  warnReason[MAX_PARTS] = {0};            // PartWarnReason
    int8_t   warnPin[MAX_PARTS] = {0};               // pin index of the announce

    // --- emphasis ---
    int16_t emphasisNodes[MAX_EMPHASIS_NODES] = {0};
    int emphasisCount = 0;

    // --- tap-to-inspect listen state (MeasureMode's pattern) ---
    int lastSwitchPosition = -2;
    unsigned long switchStableTime = 0;
    unsigned long lastPositiveMs = 0;
    int lastInspectNode = -1;

    bool overlayAddWarned = false;

    void listenForInspectTap(unsigned long now);
    void evaluateWarnings();
    uint32_t computeHighlightVisMask();
    uint32_t computeVisMask(unsigned long now);
    void compose(uint32_t visMask);
    bool refresh(bool force);   // hash -> warn -> vis -> compose-on-change
};

extern PartLabels& partLabels;

#endif // PART_LABELS_H
