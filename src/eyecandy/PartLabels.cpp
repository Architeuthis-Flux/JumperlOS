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
#include "partdb/PartDb.h"     // partdbResolveDriver - overvolt warn is display-only
#include "Peripherals.h"       // railHwVolts, getDacHardwareVoltage
#include "Probing.h"           // probing, switchPosition
#include "ReadingDisplay.h"
#include "guiding/GuideScript.h"     // formatOhms
#include "sensing/PartClassify.h"    // PartType (the cached-test summary)
#include "PartsApp.h"                // partsShowPartCard (the tap's card)
#include "boards/board.h"      // currentBoard().caps.ledsPerRow (OG gate)
#include "Colors.h"            // termColorLikeLed - PARTPIN wears its dot color
#include "JumperlOS.h"         // jOS.isUiModal - the card yields to modal UIs

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

enum PartWarnReason : uint8_t { WARN_NONE = 0, WARN_VCC_TO_GND, WARN_GND_TO_HOT, WARN_SELF_SHORT, WARN_POWER_OVERVOLT, WARN_PINS_UNVERIFIED, WARN_VEE_TO_HOT, WARN_VCC_TO_NEG };

static const char* warnReasonName(uint8_t r) {
    switch (r) {
        case WARN_VCC_TO_GND: return "vcc_to_gnd";
        case WARN_GND_TO_HOT: return "gnd_to_hot";
        case WARN_SELF_SHORT: return "self_short";
        case WARN_POWER_OVERVOLT: return "power_overvolt";
        case WARN_PINS_UNVERIFIED: return "pins_unverified";
        case WARN_VEE_TO_HOT: return "vee_to_hot";
        case WARN_VCC_TO_NEG: return "vcc_to_neg";
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

// The dot color for one pin: ROLE first (identification writes the role as
// the pin NAME - E/B/C, A/K, G/D/S, W - single letters by construction), in
// the standing card palette (PartsApp's ruling: warm = current enters, cool
// = current leaves, so E/A/S red, B/G/W yellow, C/K/D blue). Everything
// else falls back to pin-class, with pin 1 keeping its marker color. This
// replaces the every-dot-is-signal-blue read Kevin called out (2026-08-27).
static uint32_t pinDotColor(const PartPin& pin) {
    const char* n = pin.name;
    if (n[0] != '\0' && n[1] == '\0') {
        switch (n[0]) {
            case 'E': case 'A': case 'S': return 0x2A0000;  // PARTS_ROLE_E/A
            case 'B': case 'G': case 'W': return 0x201400;  // PARTS_ROLE_B
            case 'C': case 'K': case 'D': return 0x00062A;  // PARTS_ROLE_C/K
            default: break;
        }
    }
    if (pin.pinClass == 0 && pin.pinNumber == 1) return LBL_COLOR_PIN1;
    return pinClassColor(pin.pinClass);
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

// The validated net for a breadboard node, or -1. The index check is the
// partsReassertNetNames guard: a net number has to name a live net. (The old
// comment here said findNodeInNet's gpio/adc fallbacks "return NODE values" -
// they returned NET numbers that collided with the queried node, which is why
// every unconnected part leg on a low row read as wired. Fixed at the root in
// NetManager.cpp, 2026-08-28.)
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
            // A select tap highlights JUST this pin - never the whole-part
            // landing the encoder scroll does (Kevin's spec, 2026-08-27).
            // An unwired pin's row lights through the overlay this way, and
            // the same part card the scroll shows keeps the display in one
            // language (part first, [pin] bracketed, LED polarity labels).
            // In a modal context (probe mode, a menu) the OLED belongs to
            // that surface - the LED bloom and the serial line still fire,
            // which is the whole point of running in the inner set.
            setPartHighlight(i, j, LBL_INSPECT_MS);
            if (!jOS.isUiModal()) partsShowPartCard(p, j);

            Serial.print("\r\nPARTPIN row=");
            Serial.print(node);
            Serial.print(" part=");
            Serial.print(p.name);
            Serial.print(" pin=");
            Serial.print(pin.pinNumber);
            Serial.print(" label=");
            termColorLikeLed(pinDotColor(pin), &Serial);   // the dot's own hue
            Serial.print(pin.name);
            changeTerminalColor(-1, false, &Serial, true);
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

// A power-CLASS pin whose name says it is the NEGATIVE supply (VEE, VSS,
// V-) is EXPECTED on GND in single-supply use - the 4051 bench board wears
// VEE->GND as its standard hookup (2026-08-31, Kevin hit a false
// "vcc_to_gnd!" doing exactly that). Only positive supplies warn on GND.
static bool isNegativeSupplyPin(const char* name) {
    if (name == nullptr) return false;
    // case-insensitive compare against the short list partdb actually uses
    char up[12];
    int n = 0;
    while (name[n] != '\0' && n < 11) {
        char c = name[n];
        up[n] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        n++;
    }
    up[n] = '\0';
    return strcmp(up, "VEE") == 0 || strcmp(up, "VSS") == 0 ||
           strcmp(up, "V-") == 0 || strcmp(up, "-V") == 0 ||
           strcmp(up, "GND") == 0; // a gnd-NAMED pin misclassed as power
}


// The net a pin sits on - the LIVE net for evaluateWarnings, or the MERGED
// net a proposed bridge would create for the part_safety gate (both nets
// plus the two bridge nodes). One judgment routine serves both, so the warn
// rules and the refusal rules can never drift apart.
struct NetView {
    int netA, netB, nodeA, nodeB;
    bool has(int node) const {
        if (node <= 0) return false;
        if (node == nodeA || node == nodeB) return true;
        if (netA > 0 && netContainsNode(netA, node)) return true;
        if (netB > 0 && netContainsNode(netB, node)) return true;
        return false;
    }
};

// The strongest source on the net (rail or DAC), by magnitude. A net that
// carries both a rail and a DAC is its own problem; judging by the hotter
// one keeps the old per-source "any of them hot" behavior.
static bool netSourceVolts(const NetView& v, float top, float bot, float d0, float d1, float* out) {
    bool found = false;
    float best = 0.0f;
    struct { int node; float volts; } src[4] = {
        {TOP_RAIL, top}, {BOTTOM_RAIL, bot}, {DAC0, d0}, {DAC1, d1}};
    for (int i = 0; i < 4; i++) {
        if (v.has(src[i].node) && (!found || fabsf(src[i].volts) > fabsf(best))) {
            best = src[i].volts;
            found = true;
        }
    }
    *out = best;
    return found;
}

// Judge ONE pin of ONE part against the net it sits on. Power pins split by
// POLARITY (Kevin, 2026-09-01): VCC/VDD expect the most positive supply, so
// GND or a negative source under them is wrong-way power; VEE/VSS/V- expect
// the most negative, so GND (single supply) and a negative rail (bipolar)
// are both right and only a POSITIVE source is the fault.
static uint8_t judgePartPin(const PartDefinition& p, int j, const NetView& v,
                            float top, float bot, float d0, float d1) {
    const PartPin& pin = p.pins[j];
    float src = 0.0f;
    bool hasSrc = netSourceVolts(v, top, bot, d0, d1, &src);
    if (pin.pinClass == 1) {                          // power-class pin
        if (!isNegativeSupplyPin(pin.name)) {
            if (v.has(GND))              return WARN_VCC_TO_GND;
            if (hasSrc && src < -0.25f)  return WARN_VCC_TO_NEG;
            // A positive supply sharing a net with one of the part's own
            // gnd-class pins is a self short. NOT for VEE/VSS/V-: the
            // single-supply hookup ties those to GND together with VSS (the
            // 4051's VEE+VSS on GND), which the polarity rule above already
            // calls right - the loop here fired self_short on it anyway.
            for (int k = 0; k < p.numPins && k < MAX_PART_PINS; k++) {
                if (k != j && p.pins[k].pinClass == 2 &&
                    v.has(partPinNode(p, p.pins[k]))) {
                    return WARN_SELF_SHORT;
                }
            }
        } else {
            if (hasSrc && src > 0.25f)   return WARN_VEE_TO_HOT;
        }
        // 3.3V panels riding a hot rail: at 4V the bench panel NACKed at
        // random byte offsets until the display cycled lost/alive. Only
        // display-driver parts get this - plenty of logic is happy at 5V,
        // but every panel we drive is a 3.3V part.
        if (partdbResolveDriver(p) != nullptr && hasSrc && src > 3.6f) {
            return WARN_POWER_OVERVOLT;
        }
    } else if (pin.pinClass == 2) {                   // gnd-class pin
        if (hasSrc && fabsf(src) > 0.25f)  return WARN_GND_TO_HOT;
    }
    return WARN_NONE;
}

void PartLabels::evaluateWarnings() {
    uint32_t newMask = 0;
    uint8_t newReason[MAX_PARTS] = {0};
    int8_t  newPin[MAX_PARTS] = {0};

    float top = railTruth(0), bot = railTruth(1);
    float d0 = dacTruth(0), d1 = dacTruth(1);

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        // Placement couldn't electrically confirm which leg is which: the
        // part wears the warn column until a later verify clears the flag
        // (RAM-only, PartsApp sets it on the assumed-order fallback).
        if (p.pinsUnverified && p.placed) {
            newMask |= (1u << i);
            newReason[i] = WARN_PINS_UNVERIFIED;
            newPin[i] = 0;
        }
        // Two passes so self_short doesn't depend on pin-list order: first
        // resolve every power/gnd pin's net, then judge.
        int pinNet[MAX_PART_PINS];
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int node = partPinNode(p, p.pins[j]);
            pinNet[j] = (node >= 1 && node <= 60) ? validNetForNode(node) : -1;
        }
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int netNum = pinNet[j];
            if (netNum < 0) continue;
            NetView live = { netNum, -1, 0, 0 };
            uint8_t r = judgePartPin(p, j, live, top, bot, d0, d1);
            if (r != WARN_NONE) {
                newMask |= (1u << i); newReason[i] = r; newPin[i] = (int8_t)j;
                break;
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
            // The OLED card reads as words, not a machine token: underscores
            // become spaces ("vcc to gnd!"), and some display fonts have no
            // '_' glyph anyway. The serial trace above keeps the token.
            for (char* c = hint; *c != '\0'; c++) {
                if (*c == '_') *c = ' ';
            }
            ReadingDisplay::show(p.name, node, hint);
        }
    }

    // A muted warning un-mutes the moment it clears or changes reason - the
    // mute retires THIS standing complaint, not the part's right to warn.
    for (int i = 0; i < MAX_PARTS; i++) {
        if (!((warnMutedMask >> i) & 1)) continue;
        bool isOn = (newMask >> i) & 1;
        if (!isOn || newReason[i] != warnMutedReason[i]) warnMutedMask &= ~(1u << i);
    }

    warnActiveMask = newMask;
    memcpy(warnReason, newReason, sizeof(warnReason));
    memcpy(warnPin, newPin, sizeof(warnPin));
}

// part_safety gate ([routing] part_safety: 0 off / 1 power / 2 all). Called
// from addBridgeToState - the one path every USER connection takes (slot
// loads and undo go through addConnection directly and are NOT gated: a saved
// state is the user's business). Judges every placed part's pins against the
// net the bridge WOULD create. true = refused, already announced.
bool PartLabels::connectionRefused(int node1, int node2) {
    int level = jumperlessConfig.routing.part_safety;
    if (level <= 0) return false;
    NetView v = { validNetForNode(node1), validNetForNode(node2), node1, node2 };
    float top = railTruth(0), bot = railTruth(1);
    float d0 = dacTruth(0), d1 = dacTruth(1);
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            int node = partPinNode(p, p.pins[j]);
            if (!v.has(node)) continue;           // this pin isn't on the merged net
            uint8_t r = judgePartPin(p, j, v, top, bot, d0, d1);
            if (r == WARN_NONE) continue;
            // Refuse only what this bridge CREATES: a pin already sitting on a
            // faulted net (a standing PARTWARN the user chose to keep) must
            // not veto every unrelated wire that touches that net. Judge the
            // pin against its own pre-bridge net; same verdict = not ours.
            {
                int pinNet = (node >= 1 && node <= 60) ? validNetForNode(node) : -1;
                NetView before = { pinNet, -1, node, -1 };
                if (judgePartPin(p, j, before, top, bot, d0, d1) == r) continue;
            }
            bool power = (r == WARN_VCC_TO_GND || r == WARN_VCC_TO_NEG ||
                          r == WARN_VEE_TO_HOT || r == WARN_GND_TO_HOT ||
                          r == WARN_SELF_SHORT);
            if (level == 1 && !power) continue;   // "power" lets non-power warns through
            Serial.printf("\r\nPARTDB connect refused part=\"%s\" pin=\"%s\" row=%d reason=\"%s\"\r\n",
                          p.name, p.pins[j].name, node, warnReasonName(r));
            Serial.flush();
            char words[32];
            snprintf(words, sizeof(words), "%s", warnReasonName(r));
            for (char* c = words; *c != '\0'; c++) {
                if (*c == '_') *c = ' ';
            }
            ReadingDisplay::show(p.name, node, words, "refused");
            return true;
        }
    }
    return false;
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

    // Muted warnings (clearTransients) don't FORCE standing visibility -
    // they still paint their pulse pair when the part shows for other
    // reasons. An expired part highlight drops here.
    if (hlPart >= 0 && now > hlUntilMs) { hlPart = -1; hlPin = -1; }
    uint32_t mask = highlightVisMask | (warnActiveMask & ~warnMutedMask);
    if (hlPart >= 0) mask |= (1u << hlPart);
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

    // Warn pulse phase: the pair blinks so it can never be mistaken for
    // routing (Kevin, 2026-08-27: the standing full column "looks like
    // routing"). refresh() folds this phase into the fingerprint while a
    // warned part is visible, so the blink actually repaints.
    bool pulseOn = ((millis() / 400) & 1) != 0;

    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        if (!((visMask >> i) & 1)) continue;
        const PartDefinition& p = globalState.parts.parts[i];
        bool warned = (warnActiveMask >> i) & 1;
        bool focused = (i == hlPart);
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            int node = partPinNode(p, pin);
            if (node < 1 || node > 60) continue;
            int col = (node - 1) % 30;              // 0-based
            bool topHalf = (node <= 30);
            int edgeRow = topHalf ? 0 : 9;
            int inward = topHalf ? 1 : 8;
            uint32_t c = pinDotColor(pin);
            if (warned && j == warnPin[i]) {
                // The contradicted pin: edge + inward pair, warning color,
                // PULSING - pointed, and unmistakably not a net.
                uint32_t wc = pulseOn ? lblBrighten(LBL_COLOR_WARN) : LBL_COLOR_WARN;
                lblScratch[edgeRow * 30 + col] = wc;
                lblScratch[inward * 30 + col] = wc;
            } else if (focused && hlPin < 0) {
                // Whole-part highlight: every pin wears its role pair, bright.
                lblScratch[edgeRow * 30 + col] = lblBrighten(c);
                lblScratch[inward * 30 + col] = lblBrighten(c);
            } else if (focused && j == hlPin) {
                // The focused pin. An unwired row has no net to light up, so
                // the overlay paints its whole column in the role color -
                // "the highlighted node lights up the whole row" holds with
                // no wires at all. Wired rows get the net machinery's row
                // gradient; the overlay adds only the bright role pair.
                if (validNetForNode(node) < 0) {
                    int rowBase = topHalf ? 0 : 5;
                    for (int r = 0; r < 5; r++)
                        lblScratch[(rowBase + r) * 30 + col] = c;
                }
                lblScratch[edgeRow * 30 + col] = lblBrighten(c);
                lblScratch[inward * 30 + col] = lblBrighten(c);
            } else {
                // Ordinary marker: the outer-edge cell only (row 1 / row 10),
                // role-colored (a highlighted part's other pins land here).
                lblScratch[edgeRow * 30 + col] = c;
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
                    base = pinDotColor(p.pins[j]);
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
    fp = fnv1a(fp, warnMutedMask);
    fp = fnv1a(fp, ((uint32_t)(uint8_t)hlPart << 8) | (uint32_t)(uint8_t)hlPin);
    // the warn pulse repaints only while a warned part is actually visible
    if (warnActiveMask & visMask) fp = fnv1a(fp, (uint32_t)((now / 400) & 1));
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

void PartLabels::setPartHighlight(int partIdx, int pinIdx, unsigned long holdMs) {
    if (partIdx < 0 || partIdx >= globalState.parts.numParts || partIdx >= MAX_PARTS) {
        clearPartHighlight();
        return;
    }
    const PartDefinition& p = globalState.parts.parts[partIdx];
    if (pinIdx >= p.numPins || pinIdx >= MAX_PART_PINS) pinIdx = -1;
    hlPart = (int8_t)partIdx;
    hlPin = (int8_t)pinIdx;
    hlUntilMs = millis() + holdMs;
    if (board::currentBoard().caps.ledsPerRow >= 5) refresh(true);
}

void PartLabels::clearPartHighlight() {
    if (hlPart < 0) return;
    hlPart = -1;
    hlPin = -1;
    hlUntilMs = 0;
    if (board::currentBoard().caps.ledsPerRow >= 5) refresh(true);
}

void PartLabels::clearTransients() {
    // Blooms stay: a just-placed part's 5 s flash is the ambient handoff
    // the placement flow exits INTO - it self-expires and was never the
    // standing-overlay complaint.
    for (int i = 0; i < MAX_PARTS; i++) inspectUntilMs[i] = 0;
    // mute what's currently complaining; evaluateWarnings un-mutes on change
    warnMutedMask = warnActiveMask;
    memcpy(warnMutedReason, warnReason, sizeof(warnMutedReason));
    hlPart = -1;
    hlPin = -1;
    hlUntilMs = 0;
    if (board::currentBoard().caps.ledsPerRow >= 5) refresh(true);
}

bool PartLabels::partTestSummary(const PartDefinition& p, char* buf, size_t len) {
    if (buf == nullptr || len == 0) return false;
    buf[0] = '\0';
    char ohms[12];
    switch ((PartType)p.lastTestType) {
        case PartType::BJT_NPN:
        case PartType::BJT_PNP:
            snprintf(buf, len, "hFE %.0f  %.2fV",
                     (double)p.lastTestValue2, (double)p.lastTestValue);
            return true;
        case PartType::LED:
        case PartType::DIODE:
            snprintf(buf, len, "Vf %.2fV", (double)p.lastTestValue);
            return true;
        case PartType::ZENER:
            snprintf(buf, len, "Vf %.2fV Vz %.1fV",
                     (double)p.lastTestValue, (double)p.lastTestValue2);
            return true;
        case PartType::RESISTOR:
            formatOhms(p.lastTestValue, ohms, sizeof(ohms));
            snprintf(buf, len, "%s measured", ohms);
            return true;
        case PartType::POT:
            formatOhms(p.lastTestValue, ohms, sizeof(ohms));
            snprintf(buf, len, "pot %s", ohms);
            return true;
        case PartType::CAPACITOR:
            if (p.lastTestValue <= 0.0f) break;   // detect-only: no number to show
            formatFarads(p.lastTestValue, ohms, sizeof(ohms));
            snprintf(buf, len, "%s measured", ohms);
            return true;
        case PartType::NFET:
        case PartType::PFET:
            snprintf(buf, len, "%cFET %.2fV",
                     ((PartType)p.lastTestType == PartType::NFET) ? 'N' : 'P',
                     (double)p.lastTestValue);
            return true;
        default:
            break;
    }
    if (p.measuredOhms > 0.0f) {
        formatOhms(p.measuredOhms, ohms, sizeof(ohms));
        snprintf(buf, len, "%s measured", ohms);
        return true;
    }
    return false;
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
