// SPDX-License-Identifier: MIT
// Ambient part-pin labels. Contract and the auto-hide model: PartLabels.h.
//
// Composition rules (all painting into ONE overlay, "_PARTS_"):
//  - pin marker  = the single OUTER-EDGE cell of the pin's column (overlay
//    row 1 for the top half, row 10 for the bottom - the hole a DIP/axial
//    body can never cover), pin-class color, pin 1 in the marker color.
//  - warning     = the FULL 5-cell column in the warning color; occluding
//    live net color is justified exactly when the wiring contradicts the
//    label (and warnings ignore auto-hide).
//  - emphasis    = edge + inward-neighbor cells, brightened - the step
//    viewer's "these rows" pointer.
// Every post is clear-first requestLedShow(-1): the overlay renderer skips
// transparent pixels rather than painting black, so a cell that went dark is
// otherwise never cleared (the documented drag-trail bug; Highlighting and
// the old guide both learned this the hard way).
//
// Geometry comes from partPinNode() ONLY (PartPlacement.h: the sole geometry
// authority). Colors and the paint helper are absorbed from the old guide
// renderer (GuidedFlow.cpp), which A-M3 deletes.

#include "PartLabels.h"

#include "Commands.h"          // requestLedShow
#include "GraphicOverlays.h"   // graphicOverlayState, MAX_OVERLAY_PIXELS
#include "Highlighting.h"      // highlighting (net highlight/brighten state)
#include "NetManager.h"        // findNodeInNet
#include "PartPlacement.h"     // partPinNode
#include "Peripherals.h"       // railHwVolts, getDacHardwareVoltage
#include "Probing.h"           // probing, switchPosition
#include "ReadingDisplay.h"
#include "boards/board.h"      // currentBoard().caps.ledsPerRow (OG gate)

PartLabels& PartLabels::getInstance() {
    static PartLabels instance;
    return instance;
}
PartLabels& partLabels = PartLabels::getInstance();

static const char* PARTS_OVERLAY = "_PARTS_";

// Dim class colors + pin-1 marker (the guide's §4.1 palette) + the warning
// red. Kept dim on purpose: these sit next to live net colors.
static const uint32_t LBL_COLOR_POWER  = 0x1A0000;
static const uint32_t LBL_COLOR_GND    = 0x001A02;
static const uint32_t LBL_COLOR_SIGNAL = 0x000818;
static const uint32_t LBL_COLOR_NC     = 0x040404;
static const uint32_t LBL_COLOR_PIN1   = 0x180800;
static const uint32_t LBL_COLOR_WARN   = 0x2A0004;

// Visibility windows (Kevin's auto-hide ruling): a changed part blooms for a
// few seconds; a tapped part stays lit while its reading is on the OLED.
static const unsigned long LBL_BLOOM_MS   = 5000;
static const unsigned long LBL_INSPECT_MS = 4000;

// Tap listen thresholds - MeasureMode's numbers, on the select side of the
// switch (MeasureMode owns position 0; inspect listens on position 1).
static const unsigned long LBL_SWITCH_DEBOUNCE_MS = 300;
// Longer than justReadProbe's 500 ms duplicate window: quiet past this means
// the probe really lifted, not that a held row is being rate-limited.
static const unsigned long LBL_LIFT_MS = 700;

enum PartWarnReason : uint8_t { WARN_NONE = 0, WARN_VCC_TO_GND, WARN_GND_TO_HOT, WARN_SELF_SHORT };

static const char* warnReasonName(uint8_t r) {
    switch (r) {
        case WARN_VCC_TO_GND: return "vcc_to_gnd";
        case WARN_GND_TO_HOT: return "gnd_to_hot";
        case WARN_SELF_SHORT: return "self_short";
        default:              return "none";
    }
}

static const char* pinClassName(uint8_t pinClass) {
    switch (pinClass) {
        case 1: return "power";
        case 2: return "gnd";
        case 3: return "nc";
        default: return "signal";
    }
}

static uint32_t pinClassColor(uint8_t pinClass) {
    switch (pinClass) {
        case 1: return LBL_COLOR_POWER;
        case 2: return LBL_COLOR_GND;
        case 3: return LBL_COLOR_NC;
        default: return LBL_COLOR_SIGNAL;
    }
}

// ~4x brightness, per-channel clamped (the guide's pulse brighten).
static uint32_t lblBrighten(uint32_t c) {
    uint32_t r = ((c >> 16) & 0xFF) * 4; if (r > 0xFF) r = 0xFF;
    uint32_t g = ((c >> 8) & 0xFF) * 4;  if (g > 0xFF) g = 0xFF;
    uint32_t bl = (c & 0xFF) * 4;        if (bl > 0xFF) bl = 0xFF;
    return (r << 16) | (g << 8) | bl;
}

// FNV-1a fold - the fingerprint primitive.
static inline uint32_t fnv1a(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 16777619u;
    }
    return h;
}
static const uint32_t FNV_BASIS = 2166136261u;

// The hardware-truth rail/DAC volts, falling back to the state's setpoint
// before the first write (railHwVolts seeds at -100; LEDs.cpp's pattern).
static float railTruth(int rail) {
    return (railHwVolts[rail] > -99.0f)
               ? railHwVolts[rail]
               : (rail == 0 ? globalState.power.topRail : globalState.power.bottomRail);
}
static float dacTruth(int dac) {
    float v = getDacHardwareVoltage(dac);
    return (v > -99.0f) ? v : (dac == 0 ? globalState.power.dac0 : globalState.power.dac1);
}

// Does net `netNum` (validated) contain `node`? nodes[] terminates at 0.
static bool netContainsNode(int netNum, int node) {
    if (netNum <= 0 || netNum >= MAX_NETS) return false;
    if (globalState.connections.nets[netNum].number != netNum) return false;
    for (int n = 0; n < MAX_NODES && globalState.connections.nets[netNum].nodes[n] != 0; n++) {
        if (globalState.connections.nets[netNum].nodes[n] == node) return true;
    }
    return false;
}

// The validated net for a breadboard node, or -1 (the partsReassertNetNames
// guard: findNodeInNet's gpio/adc fallbacks return NODE values).
static int validNetForNode(int node) {
    int netNum = findNodeInNet(node);
    if (netNum <= 0 || netNum >= MAX_NETS) return -1;
    if (globalState.connections.nets[netNum].number != netNum) return -1;
    return netNum;
}

// Scratch overlay buffer: static, core-0 single-writer.
static uint32_t lblScratch[MAX_OVERLAY_PIXELS];

// ---------------------------------------------------------------------------
// Tap-to-inspect
// ---------------------------------------------------------------------------

void PartLabels::listenForInspectTap(unsigned long now) {
    if (switchPosition != lastSwitchPosition) {
        lastSwitchPosition = switchPosition;
        switchStableTime = now;
    }
    // Inspect listens in SELECT mode only (position 1) - MeasureMode owns the
    // measure side, so there is no tap-precedence collision by construction.
    if (switchPosition != 1 || (now - switchStableTime) < LBL_SWITCH_DEBOUNCE_MS) {
        lastInspectNode = -1;
        return;
    }

    // ONE positive cached reading IS the tap (Highlighting's consumption
    // pattern, the working precedent): the Probing service refreshes this
    // cache from justReadProbe, whose contract emits a held row once per
    // 500 ms with -1 between - a wait-for-N-stable-reads scheme on this
    // cache can never reach its threshold.
    int node = probing.getLastProbeReading();
    if (node < 1 || node > 60) {
        if (lastInspectNode != -1 && (now - lastPositiveMs) > LBL_LIFT_MS)
            lastInspectNode = -1;   // probe lifted: next tap announces again
        return;
    }
    lastPositiveMs = now;
    if (node == lastInspectNode) return;

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            if (partPinNode(p, pin) != node) continue;

            lastInspectNode = node;
            inspectUntilMs[i] = now + LBL_INSPECT_MS;

            char line2[24];
            snprintf(line2, sizeof(line2), "pin %d %s", pin.pinNumber, pinClassName(pin.pinClass));
            ReadingDisplay::show(p.name, node, pin.name, line2);

            Serial.print("\r\nPARTPIN row=");
            Serial.print(node);
            Serial.print(" part=");
            Serial.print(p.name);
            Serial.print(" pin=");
            Serial.print(pin.pinNumber);
            Serial.print(" label=");
            Serial.print(pin.name);
            Serial.print(" class=");
            Serial.print(pinClassName(pin.pinClass));
            Serial.print(" net=");
            Serial.println(validNetForNode(node));
            Serial.flush();
            return;
        }
    }
    // Unlabeled row: do nothing - Highlighting's net identify already covers
    // it. Remember the node so one tap prints at most one lookup attempt.
    lastInspectNode = node;
}

// ---------------------------------------------------------------------------
// Warnings (warn, never block - certainties only)
// ---------------------------------------------------------------------------

void PartLabels::evaluateWarnings() {
    uint32_t newMask = 0;
    uint8_t newReason[MAX_PARTS] = {0};
    int8_t  newPin[MAX_PARTS] = {0};

    float top = railTruth(0), bot = railTruth(1);
    float d0 = dacTruth(0), d1 = dacTruth(1);

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        // Two passes so self_short doesn't depend on pin-list order: first
        // resolve every power/gnd pin's net, then judge.
        int pinNet[MAX_PART_PINS];
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int node = partPinNode(p, p.pins[j]);
            pinNet[j] = (node >= 1 && node <= 60) ? validNetForNode(node) : -1;
        }
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            int netNum = pinNet[j];
            if (netNum < 0) continue;

            if (pin.pinClass == 1) {          // power-class pin
                if (netContainsNode(netNum, GND)) {
                    newMask |= (1u << i); newReason[i] = WARN_VCC_TO_GND; newPin[i] = (int8_t)j;
                    break;
                }
                bool shorted = false;
                for (int k = 0; k < p.numPins && k < MAX_PART_PINS; k++) {
                    if (k != j && p.pins[k].pinClass == 2 && pinNet[k] == netNum) {
                        shorted = true;
                        break;
                    }
                }
                if (shorted) {
                    newMask |= (1u << i); newReason[i] = WARN_SELF_SHORT; newPin[i] = (int8_t)j;
                    break;
                }
            } else if (pin.pinClass == 2) {   // gnd-class pin
                bool hot = (netContainsNode(netNum, TOP_RAIL) && fabsf(top) > 0.25f) ||
                           (netContainsNode(netNum, BOTTOM_RAIL) && fabsf(bot) > 0.25f) ||
                           (netContainsNode(netNum, DAC0) && fabsf(d0) > 0.25f) ||
                           (netContainsNode(netNum, DAC1) && fabsf(d1) > 0.25f);
                if (hot) {
                    newMask |= (1u << i); newReason[i] = WARN_GND_TO_HOT; newPin[i] = (int8_t)j;
                    break;
                }
            }
        }
    }

    // Edge-triggered announce: one line per part per new warning, never a
    // per-tick stream. Cleared warnings clear silently (the LEDs say it).
    for (int i = 0; i < MAX_PARTS; i++) {
        bool wasOn = (warnActiveMask >> i) & 1;
        bool isOn = (newMask >> i) & 1;
        if (isOn && (!wasOn || warnReason[i] != newReason[i])) {
            const PartDefinition& p = globalState.parts.parts[i];
            int node = (newPin[i] >= 0 && newPin[i] < p.numPins)
                           ? partPinNode(p, p.pins[newPin[i]]) : -1;
            Serial.print("\r\nPARTWARN part=");
            Serial.print(p.name);
            Serial.print(" pin=");
            Serial.print((newPin[i] >= 0 && newPin[i] < p.numPins) ? p.pins[newPin[i]].pinNumber : -1);
            Serial.print(" row=");
            Serial.print(node);
            Serial.print(" reason=");
            Serial.println(warnReasonName(newReason[i]));
            Serial.flush();
            char hint[32];
            snprintf(hint, sizeof(hint), "%s!", warnReasonName(newReason[i]));
            ReadingDisplay::show(p.name, node, hint);
        }
    }

    warnActiveMask = newMask;
    memcpy(warnReason, newReason, sizeof(warnReason));
    memcpy(warnPin, newPin, sizeof(warnPin));
}

// ---------------------------------------------------------------------------
// Visibility (auto-hide)
// ---------------------------------------------------------------------------

uint32_t PartLabels::computeHighlightVisMask() {
    uint32_t mask = 0;
    int hlNet = highlighting.highlightedNet;
    int brNet = highlighting.brightenedNet;
    int brNode = highlighting.brightenedNode;
    int brNodeNet = (brNode > 0) ? validNetForNode(brNode) : -1;
    if (hlNet <= 0 && brNet <= 0 && brNodeNet <= 0) return 0;

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int node = partPinNode(p, p.pins[j]);
            if (node < 1 || node > 60) continue;
            int netNum = validNetForNode(node);
            if (netNum < 0) continue;
            if (netNum == hlNet || netNum == brNet || netNum == brNodeNet) {
                mask |= (1u << i);
                break;
            }
        }
    }
    return mask;
}

uint32_t PartLabels::computeVisMask(unsigned long now) {
    // Highlight intersection is recomputed only when the highlight state
    // moves - it costs a parts x pins net scan.
    if (highlighting.highlightedNet != lastHighlightedNet ||
        highlighting.brightenedNet != lastBrightenedNet ||
        highlighting.brightenedNode != lastBrightenedNode) {
        lastHighlightedNet = highlighting.highlightedNet;
        lastBrightenedNet = highlighting.brightenedNet;
        lastBrightenedNode = highlighting.brightenedNode;
        highlightVisMask = computeHighlightVisMask();
    }

    uint32_t mask = highlightVisMask | warnActiveMask;
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        if (now < bloomUntilMs[i] || now < inspectUntilMs[i]) mask |= (1u << i);
    }
    // Emphasized nodes surface their owning parts too.
    for (int e = 0; e < emphasisCount; e++) {
        int node = emphasisNodes[e];
        for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
            const PartDefinition& p = globalState.parts.parts[i];
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                if (partPinNode(p, p.pins[j]) == node) { mask |= (1u << i); break; }
            }
        }
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

void PartLabels::compose(uint32_t visMask) {
    memset(lblScratch, 0, sizeof(lblScratch));
    bool any = false;

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        if (!((visMask >> i) & 1)) continue;
        const PartDefinition& p = globalState.parts.parts[i];
        bool warned = (warnActiveMask >> i) & 1;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            int node = partPinNode(p, pin);
            if (node < 1 || node > 60) continue;
            int col = (node - 1) % 30;              // 0-based
            bool topHalf = (node <= 30);
            if (warned && j == warnPin[i]) {
                // The contradicted pin: full 5-cell column, warning color.
                int rowBase = topHalf ? 0 : 5;
                for (int r = 0; r < 5; r++) lblScratch[(rowBase + r) * 30 + col] = LBL_COLOR_WARN;
            } else {
                // Ordinary marker: the outer-edge cell only (row 1 / row 10).
                int edgeRow = topHalf ? 0 : 9;
                lblScratch[edgeRow * 30 + col] =
                    (pin.pinNumber == 1) ? LBL_COLOR_PIN1 : pinClassColor(pin.pinClass);
            }
            any = true;
        }
    }

    // Emphasis: edge + inward neighbor, brightened. Painted last so the step
    // viewer's pointer reads over ordinary markers.
    for (int e = 0; e < emphasisCount; e++) {
        int node = emphasisNodes[e];
        if (node < 1 || node > 60) continue;
        int col = (node - 1) % 30;
        bool topHalf = (node <= 30);
        uint32_t base = LBL_COLOR_SIGNAL;
        for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
            const PartDefinition& p = globalState.parts.parts[i];
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                if (partPinNode(p, p.pins[j]) == node) {
                    base = (p.pins[j].pinNumber == 1) ? LBL_COLOR_PIN1
                                                      : pinClassColor(p.pins[j].pinClass);
                    break;
                }
            }
        }
        int edgeRow = topHalf ? 0 : 9;
        int inward = topHalf ? 1 : 8;
        lblScratch[edgeRow * 30 + col] = lblBrighten(base);
        lblScratch[inward * 30 + col] = lblBrighten(base);
        any = true;
    }

    if (any) {
        if (graphicOverlayState.addOverlay(PARTS_OVERLAY, 1, 1, 30, 10, lblScratch) < 0 &&
            !overlayAddWarned) {
            overlayAddWarned = true;
            Serial.println("\r\n  (part labels unavailable - all overlay slots are full)");
        }
    } else {
        graphicOverlayState.removeOverlay(PARTS_OVERLAY);
    }
    requestLedShow(-1);   // clear-first, always (the drag-trail rule)
}

// ---------------------------------------------------------------------------
// Service tick
// ---------------------------------------------------------------------------

ServiceStatus PartLabels::service() {
    // OG has one LED per row and no overlay fabric: the whole feature lies
    // dormant behind the runtime capability, no #ifdef fork.
    if (board::currentBoard().caps.ledsPerRow < 5) return ServiceStatus::IDLE;

    if (board::currentBoard().caps.hasProbePads) {
        listenForInspectTap(millis());
    }

    return refresh(false) ? ServiceStatus::BUSY : ServiceStatus::IDLE;
}

// The whole change-detection -> warning -> visibility -> compose pipeline.
// force = compose even when nothing hashed differently (the preview hooks:
// state was swapped under us while a BLOCKING menu froze this service, and
// core 1 renders overlays during preview - the stale overlay must be
// replaced synchronously).
bool PartLabels::refresh(bool force) {
    unsigned long now = millis();

    // Per-part hashes: geometry + pin table. A changed part blooms.
    uint32_t partsFold = FNV_BASIS;
    for (int i = 0; i < MAX_PARTS; i++) {
        uint32_t h = FNV_BASIS;
        if (i < globalState.parts.numParts) {
            const PartDefinition& p = globalState.parts.parts[i];
            h = fnv1a(h, (uint32_t)(uint16_t)p.baseRow);
            h = fnv1a(h, ((uint32_t)p.footprint << 16) | ((uint32_t)p.placement << 8) |
                          (uint32_t)(p.placed ? 1 : 0));
            h = fnv1a(h, (uint32_t)p.numPins);
            for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
                const PartPin& pin = p.pins[j];
                h = fnv1a(h, ((uint32_t)(uint8_t)pin.pinNumber << 24) |
                              ((uint32_t)(uint8_t)pin.offset << 16) |
                              ((uint32_t)(uint16_t)pin.connect));
                h = fnv1a(h, pin.pinClass);
            }
        }
        if (h != partHash[i]) {
            if (partHash[i] != 0) bloomUntilMs[i] = now + LBL_BLOOM_MS;
            else if (i < globalState.parts.numParts) bloomUntilMs[i] = now + LBL_BLOOM_MS;
            partHash[i] = h;
        }
        partsFold = fnv1a(partsFold, h);
    }

    // Netlist: authored bridges (the durable topology, not the paths cache).
    uint32_t nl = FNV_BASIS;
    nl = fnv1a(nl, (uint32_t)(uint16_t)globalState.connections.numBridges);
    for (int i = 0; i < globalState.connections.numBridges && i < MAX_BRIDGES; i++) {
        nl = fnv1a(nl, ((uint32_t)(uint16_t)globalState.connections.bridges[i][0] << 16) |
                        (uint32_t)(uint16_t)globalState.connections.bridges[i][1]);
    }

    // Power: hardware-truth SETPOINTS, quantized to mV - never a live ADC
    // reading (a jittering fingerprint would clear-repaint at 50 Hz).
    uint32_t pw = FNV_BASIS;
    pw = fnv1a(pw, (uint32_t)(int32_t)(railTruth(0) * 1000.0f));
    pw = fnv1a(pw, (uint32_t)(int32_t)(railTruth(1) * 1000.0f));
    pw = fnv1a(pw, (uint32_t)(int32_t)(dacTruth(0) * 1000.0f));
    pw = fnv1a(pw, (uint32_t)(int32_t)(dacTruth(1) * 1000.0f));

    // Contradictions re-derive only when their inputs moved (netlist, power,
    // or the parts table itself - a part placed onto a hot net must warn).
    if (nl != netlistHash || pw != powerHash || partsFold != partsFoldLast) {
        netlistHash = nl;
        powerHash = pw;
        partsFoldLast = partsFold;
        evaluateWarnings();
    }

    uint32_t visMask = computeVisMask(now);

    uint32_t fp = FNV_BASIS;
    fp = fnv1a(fp, partsFold);
    fp = fnv1a(fp, nl);
    fp = fnv1a(fp, pw);
    fp = fnv1a(fp, visMask);
    fp = fnv1a(fp, warnActiveMask);
    fp = fnv1a(fp, (uint32_t)emphasisCount);
    for (int e = 0; e < emphasisCount; e++) fp = fnv1a(fp, (uint32_t)(uint16_t)emphasisNodes[e]);
    fp = fnv1a(fp, slotManager.isPreviewMode() ? 1u : 0u);

    if (force || fp != lastFingerprint) {
        lastFingerprint = fp;
        compose(visMask);
        return true;
    }
    return false;
}

void PartLabels::recomposeNow() {
    if (board::currentBoard().caps.ledsPerRow < 5) return;
    refresh(true);
}

void PartLabels::setEmphasis(const int16_t* nodes, int count) {
    if (nodes == nullptr || count <= 0) {
        emphasisCount = 0;
        return;
    }
    if (count > MAX_EMPHASIS_NODES) count = MAX_EMPHASIS_NODES;
    memcpy(emphasisNodes, nodes, count * sizeof(int16_t));
    emphasisCount = count;
}
