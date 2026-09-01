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
// every post is clear-first requestLedShow(-1). IN the inner set since
// 2026-08-29 (Kevin: "activate the parts bloom in probing"), so taps bloom
// during probe mode and the menus too - the OLED part card alone stays
// gated to non-modal contexts.
class PartLabels : public Service {
public:
    static PartLabels& getInstance();

    PartLabels(const PartLabels&) = delete;
    PartLabels& operator=(const PartLabels&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "PartLabels"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    // 200 Hz: the inspect listener reads the Probing service's cache, which
    // holds an accepted row for ONE ~10 ms refresh window per 500 ms held -
    // a consumer must sample faster than that window or it misses taps
    // (20 ms here made tap-to-inspect a coin flip). The tick body is a few
    // compares; the heavy recompose work stays fingerprint-gated.
    uint32_t periodUs() const override { return 5000; }

    // Inner set (Kevin, 2026-08-29: "activate the parts bloom in probing"):
    // probe mode and the menus pump serviceInner(), so membership here is
    // what lets a probe tap bloom a part's labels DURING probing instead of
    // only after it exits. The OLED part card stays gated to non-modal
    // contexts (listenForInspectTap) - probe mode owns that screen; the
    // LED bloom, the pin highlight and the PARTPIN line run everywhere.
    bool inInnerSet() const override { return true; }

    // Synchronous recompose + post, for callers that change state while this
    // service can't run (SlotManager preview enter/exit inside the menu loop).
    void recomposeNow();

    // Step-viewer emphasis: these breadboard nodes render edge + inward
    // neighbor, brightened, and their owning parts count as visible.
    // count = 0 (or nodes = nullptr) clears.
    void setEmphasis(const int16_t* nodes, int count);
    void clearEmphasis() { setEmphasis(nullptr, 0); }

    // Part highlight - the encoder scroll's part focus and the select tap.
    // pinIdx -1 = the WHOLE part (every pin gets its role-color dot pair);
    // pinIdx >= 0 focuses one pin (brightened, and an unwired pin's row is
    // painted whole so it lights up like a wired one). holdMs bounds how
    // long it shows; clearHighlighting() also clears it.
    void setPartHighlight(int partIdx, int pinIdx, unsigned long holdMs);
    void clearPartHighlight();
    bool partHighlightActive() const { return hlPart >= 0; }
    int partHighlightPart() const { return hlPart; }

    // Standing displays retire (Kevin's ruling, 2026-08-27: overlays are
    // informative but GO AWAY on board clear / parts-app exit): current
    // warnings mute until they change or re-fire, inspect/bloom windows
    // drop, any part highlight clears. The warn FLAGS survive - only the
    // standing paint retires.
    void clearTransients();

    // The cached-test-data line for the part card ("hFE 370  0.59V" /
    // "Vf 1.95V" / "1.0k"). False when the part was never tested.
    static bool partTestSummary(const PartDefinition& p, char* buf, size_t len);

    bool hasWarnings() const { return warnActiveMask != 0; }

    // part_safety: would bridging node1-node2 put wrong-way power on a placed
    // part, at the configured level? true = refused (announced on serial +
    // OLED). addBridgeToState is the only caller - see the .cpp note.
    bool connectionRefused(int node1, int node2);

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

    // --- warn display muting (clearTransients) ---
    uint32_t warnMutedMask = 0;                      // muted-standing bits
    uint8_t  warnMutedReason[MAX_PARTS] = {0};       // unmute when it changes

    // --- part highlight ---
    int8_t hlPart = -1;                              // -1 = none
    int8_t hlPin = -1;                               // -1 = whole part
    unsigned long hlUntilMs = 0;

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
