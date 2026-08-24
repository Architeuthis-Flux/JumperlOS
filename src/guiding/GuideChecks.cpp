// SPDX-License-Identifier: MIT
// Guide verification checks (task 7). Design authority:
// CodeDocs/DESIGN_GUIDED_PLACEMENT.md §5 - the verify matrix and its honest
// limits. Seam contract in GuidedFlow.h (begin / poll / abort).
//
// Shape: one static CheckState driven from guideTick's STEP_VERIFY. Every
// check is a polled sub-state - nothing here blocks except the documented
// i2cScan reuse (§5.2 says reuse it; it is a bounded one-shot) - and every
// wait returns GUIDE_CHECK_RUNNING so the guide loop pumps serviceInner()
// between polls, which keeps the INA219 poll (Peripherals, CRITICAL) and
// the one-shot tap service (scan core) alive underneath us.
//
// Electrical safety rules (the brief's non-negotiables):
//  - stimulus checks (continuity / vf) REFUSE to run when any involved node
//    already carries a non-infra bridge - we measure only isolated rows and
//    never energize something the user wired;
//  - every ephemeral connection is removed and DAC0 / rails restored to
//    globalState.power truth on EVERY exit path (pass, fail, timeout,
//    user-abort mid-check) - checkTeardown() is the single funnel and
//    guideCheckAbort() calls it unconditionally;
//  - INA watchdog: any |I| > 50 mA while a stimulus is live aborts the
//    check immediately;
//  - stimulus DAC/rail writes are save=0, so the slot keeps the guide's
//    safe 0 V state, and infra's probe-power evaluation (which reads STATE
//    truth via getDacVoltage) never sees DAC0 enter the probe window - the
//    buffer feed cannot migrate onto our stimulus mid-check.
//
// Measurement plumbing: node voltages come from NVSCAN's one-shot tap API
// (requestNodeTap / requestPairTap, serviced on the scan core - see
// NetVoltageScan.h). Discipline: a request must SUCCEED before its result
// is ever polled, so a result left in the slots by an aborted check can
// never be read as fresh by the next one - and guideCheckAbort() calls
// cancelOneShotTap(), so the abandoned REQUEST doesn't fire later either.

#include "GuideScript.h"

// ---------------------------------------------------------------------------
// Part values, bands, display formatting - BOTH TARGETS
// ---------------------------------------------------------------------------
// Deliberately ABOVE the OG_JUMPERLESS guard: these are pure functions with no
// hardware in them, the `guideband` debug command runs on either board, and
// building them in the OG env is what catches signature drift (that env has
// caught it before - task 2). Contracts are in GuidedFlow.h.

static PartValueKind kindFromTypeStr(const char* typeStr) {
    if (typeStr == nullptr) return PartValueKind::NONE;
    if (strcmp(typeStr, "resistor") == 0) return PartValueKind::OHMS;
    if (strcmp(typeStr, "capacitor") == 0) return PartValueKind::FARADS;
    if (strcmp(typeStr, "inductor") == 0) return PartValueKind::HENRIES;
    return PartValueKind::NONE;
}

ParsedPartValue parsePartValue(const char* s, const char* typeStr) {
    ParsedPartValue r = { PartValueKind::NONE, -1.0f };
    if (s == nullptr || s[0] == '\0') return r;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (end == s) return r;      // no numeric prefix at all: "NE555", "SSD1306"
    if (!(v > 0.0f)) return r;   // 0 and negatives are not part values
    while (*end == ' ') end++;

    const PartValueKind typeKind = kindFromTypeStr(typeStr);
    float mult = 1.0f;
    bool sawMult = false;
    switch (*end) {
        case 'p': mult = 1e-12f; sawMult = true; break;
        case 'n': mult = 1e-9f;  sawMult = true; break;
        case 'u': mult = 1e-6f;  sawMult = true; break;
        case (char)0xB5: mult = 1e-6f; sawMult = true; break;  // bare micro sign
        case (char)0xC2:                                       // UTF-8 'µ'
            if ((unsigned char)end[1] == 0xB5) { end++; mult = 1e-6f; sawMult = true; }
            break;
        case 'k': case 'K': mult = 1e3f; sawMult = true; break;
        // THE m/M QUIRK. On a resistor BOTH mean mega: that is the shipped
        // convention the deleted parseResistanceOhms documented ("1m = meg
        // here"), and milliohms are an explicit non-goal - nobody writes a
        // 1 milliohm part into a breadboard project. On a capacitor or
        // inductor `m` is milli (10mH is real) and `M` is meaningless.
        case 'm': mult = (typeKind == PartValueKind::OHMS) ? 1e6f : 1e-3f;
                  sawMult = true; break;
        case 'M': if (typeKind != PartValueKind::OHMS) return r;
                  mult = 1e6f; sawMult = true; break;
        default: break;
    }
    if (sawMult) end++;
    while (*end == ' ') end++;

    // Optional unit token. Anything ELSE trailing is a reject: "2k2" / "4R7"
    // infix notation are documented non-goals (no shipped file uses them) and
    // silently misparsing them would be worse than refusing.
    PartValueKind unitKind = PartValueKind::NONE;
    if (*end != '\0') {
        if (*end == 'F' || *end == 'f')      { unitKind = PartValueKind::FARADS;  end += 1; }
        else if (*end == 'H' || *end == 'h') { unitKind = PartValueKind::HENRIES; end += 1; }
        else if (*end == 'V' || *end == 'v') { unitKind = PartValueKind::VOLTS;   end += 1; }
        else if (*end == 'R' || *end == 'r') { unitKind = PartValueKind::OHMS;    end += 1; }
        else if ((end[0] == 'o' || end[0] == 'O') && (end[1] == 'h' || end[1] == 'H') &&
                 (end[2] == 'm' || end[2] == 'M')) { unitKind = PartValueKind::OHMS; end += 3; }
        else return r;
        if (*end == 's' || *end == 'S') end++;   // "ohms"
        while (*end == ' ') end++;
        if (*end != '\0') return r;              // trailing junk
    }

    if (unitKind != PartValueKind::NONE && typeKind != PartValueKind::NONE &&
        unitKind != typeKind) {
        return r;                                 // "10uF" declared a resistor
    }
    PartValueKind kind = (unitKind != PartValueKind::NONE) ? unitKind : typeKind;
    if (kind == PartValueKind::NONE) return r;    // "24C02" on an ic
    r.kind = kind;
    r.v = v * mult;
    return r;
}

void formatOhms(float r, char* out, size_t n) {
    if (out == nullptr || n == 0) return;
    if (!(r >= 0.0f)) { snprintf(out, n, "?"); return; }
    if (r < 1000.0f)          snprintf(out, n, "%.0f", (double)r);
    else if (r < 10000.0f)    snprintf(out, n, "%.2fk", (double)(r / 1000.0f));
    else if (r < 100000.0f)   snprintf(out, n, "%.1fk", (double)(r / 1000.0f));
    else if (r < 1000000.0f)  snprintf(out, n, "%.0fk", (double)(r / 1000.0f));
    else                      snprintf(out, n, "%.2fM", (double)(r / 1000000.0f));
}

bool guideResistorBand(float rNom, uint8_t tolAuthorPct, float* lo, float* hi) {
    if (!(rNom > 0.0f)) return false;
    // tol_author covers the PART (5-10 % parts plus drift); tol_meas covers
    // the METER, and it grows with R because the shunt's one-LSB floor is a
    // bigger fraction of a smaller current (invest-measurement.md §1.8).
    const float tolAuthor = (tolAuthorPct > 0) ? (float)tolAuthorPct : 15.0f;
    const float tolMeas = (rNom <= 10000.0f) ? 5.0f
                        : (rNom <= 100000.0f) ? 10.0f : 25.0f;
    const float t = tolAuthor + tolMeas;
    if (lo) *lo = rNom * (1.0f - t / 100.0f);
    if (hi) *hi = rNom * (1.0f + t / 100.0f);
    return true;
}

float guideStimulusVolts(float rNom) {
    // 5 V at >= 20k lifts the current off the shunt floor (47k: ~105 uA
    // instead of 70 uA). Rows and the buffered +/-8 V sense path are rated for
    // it and the worst-case misplacement current at >= 20k is < 0.25 mA.
    return (rNom >= 20000.0f) ? 5.0f : 3.3f;
}

void guideBandReport(const char* value, const char* typeStr, int tolPct, Stream* out) {
    if (out == nullptr) return;
    ParsedPartValue p = parsePartValue(value, typeStr);
    const char* kindName = (p.kind == PartValueKind::OHMS)    ? "ohms"
                         : (p.kind == PartValueKind::FARADS)  ? "farads"
                         : (p.kind == PartValueKind::HENRIES) ? "henries"
                         : (p.kind == PartValueKind::VOLTS)   ? "volts" : "none";
    if (p.kind != PartValueKind::OHMS) {
        out->printf("GUIDEBAND value=%s type=%s kind=%s v=%.6g band=none\n\r",
                    (value != nullptr) ? value : "",
                    (typeStr != nullptr && typeStr[0]) ? typeStr : "-",
                    kindName, (double)p.v);
        return;
    }
    float lo = 0, hi = 0;
    guideResistorBand(p.v, (uint8_t)((tolPct > 0 && tolPct < 100) ? tolPct : 0), &lo, &hi);
    const float tolAuthor = (tolPct > 0 && tolPct < 100) ? (float)tolPct : 15.0f;
    const float tolMeas = (p.v <= 10000.0f) ? 5.0f : (p.v <= 100000.0f) ? 10.0f : 25.0f;
    char loStr[12], hiStr[12], nomStr[12];
    formatOhms(lo, loStr, sizeof(loStr));
    formatOhms(hi, hiStr, sizeof(hiStr));
    formatOhms(p.v, nomStr, sizeof(nomStr));
    out->printf("GUIDEBAND value=%s type=%s kind=ohms ohms=%.4g nom=%s tol=%.0f "
                "lo=%.4g hi=%.4g band=%s-%s stim=%.1f\n\r",
                (value != nullptr) ? value : "",
                (typeStr != nullptr && typeStr[0]) ? typeStr : "-",
                (double)p.v, nomStr, (double)(tolAuthor + tolMeas),
                (double)lo, (double)hi, loStr, hiStr,
                (double)guideStimulusVolts(p.v));
}

#if !defined(OG_JUMPERLESS)

#include "AdcRing.h"         // the always-on ring: Option 1's sense sampler
#include "Apps.h"            // i2cScan (§5.2: reuse, real signature checked)
#include "Commands.h"        // refreshLocalConnections, waitCore2
#include "InfraPaths.h"      // infraIsBridge (user-claim test), infraOwnsNode
#include "JumperlessDefines.h"
#include "NetVoltageScan.h"  // the one-shot tap request API
#include "PartPlacement.h"   // partPinNode
#include "Peripherals.h"     // currentSenseState, inaShuntCurrent_mA, INA1, gpioDef
#include "States.h"          // globalState
#include "config.h"
#include "hardware/gpio.h"

// The router's own verdict on what it could not route. Reset at the top of
// every assignment pass (NetsToChipConnections.cpp), so after a
// refreshLocalConnections + waitCore2 this list names exactly the bridges
// THIS refresh failed to place - which is how a check tells "the sense leg
// did not route" from "the sense leg routed and read 0 V".
extern int numberOfUnconnectablePaths;
extern int unconnectablePaths[10][2];

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class CkPhase : uint8_t {
    IDLE,
    TAP_REQUEST,  // posting a one-shot tap (retries while a stale one drains)
    TAP_WAIT,     // polling the tap result
    OFS_SETTLE,   // chain live, DAC0 held at 0 V, pre-sample wait
    OFS_SAMPLE,   // zero-stimulus shunt offset (invest-measurement.md §1.4)
    STIM_SETTLE,  // energized, waiting before sampling the shunt
    STIM_SAMPLE,  // collecting shunt-register samples
    V_READ,       // reading the two sense channels (ring bridges or taps)
    PRES_CHARGE,  // presence: rows tied to the stimulus, charging
    PRES_UNROUTE, // presence: strand the charge, then tap row by row
    OSC_COUNT,    // counting edges on the ephemeral GPIO route
    RAIL_SETTLE,  // transient power applied, 100 ms wait (rail_sane)
    RAIL_REF,     // rail_sane: tapping the RAIL itself, before any row (§3)
    DONE,
};

// How the check reads rowA/rowB while the stimulus is held.
enum class SenseMode : uint8_t {
    RING,    // Option 1: the sense legs are ephemeral ADC bridges routed WITH
             // the stimulus chain; the sampler reads the ring channels direct
    TAPS,    // fallback: T2's sequential-same-ADC one-shot taps
    CURRENT, // §1.9 last resort: no voltage at all, judge I against a wide
             // plausibility band derived from R_nom
};

// Max rows a multi-row check iterates (part pins for presence, class-tagged
// pins across all placed parts for rail_sane).
#define CK_MAX_ROWS 32

// rowClass, as stored by collectClassRows and read by rail_sane's gate 2:
//   1 = power fed from the TOP rail (and the documented default for a power
//       pin whose feeding supply cannot be named - see railRowClassFor)
//   2 = ground
//   3 = power fed from the BOTTOM rail
#define CK_ROWCLASS_TOP 1
#define CK_ROWCLASS_GND 2
#define CK_ROWCLASS_BOT 3

// The hint buffer, named so ckHint() below can assert against it.
#define CK_HINT_MAX 96

struct CheckState {
    bool active = false;
    GuideCheck check = GuideCheck::NONE;
    const GuideStep* step = nullptr;
    const GuideScript* script = nullptr;
    bool powerApplied = false;

    unsigned long startMs = 0;
    uint32_t timeoutMs = 1500;
    CkPhase phase = CkPhase::IDLE;

    // rows under test (presence / rail_sane iterate; continuity / vf use A+B)
    int rows[CK_MAX_ROWS];
    uint8_t rowClass[CK_MAX_ROWS]; // rail_sane: CK_ROWCLASS_TOP / _GND / _BOT
    int numRows = 0;
    int rowIdx = 0;

    // tap bookkeeping (single or pair, via the TAP_ phases)
    bool tapIsPair = false;
    int tapN1 = -1, tapN2 = -1;
    unsigned long tapBackoffUntilMs = 0;
    int tapHardFails = 0;          // consecutive -2 results
    // Sequential-same-ADC pair (tapCyclePair): which of the two nodes is
    // being tapped, the channel node A landed on (node B's hint), A's held
    // reading, and whether B had to fall back to a different channel.
    int tapSeqPhase = 0;           // 0 = node A, 1 = node B
    int tapSeqAdc = -1;
    float tapSeqV1 = 0;
    bool tapSeqAdcFallback = false;
    int tapFailNode = -1;          // node whose tap hard-failed (noroute@N)

    // stimulus chain (continuity / vf). FIVE legs now - the ground-side
    // topology's three plus Option 1's two sense bridges (see chainBegin).
    bool chainLive = false;        // teardown owes ephemeral removal + refresh
    bool chainAdded[5] = {false, false, false, false, false};
    // The exact endpoints each leg was added with, so teardown removes what
    // was added without re-deriving which topology this check built.
    int legNode[5][2] = {{-1,-1},{-1,-1},{-1,-1},{-1,-1},{-1,-1}};
    int chainRowA = -1, chainRowB = -1;
    float stimVolts = 0;
    unsigned long stimAppliedMs = 0;
    float bandLo = 0, bandHi = 0;  // resolved band for this run (OHMS / volts)

    // measurement (§1.4)
    SenseMode senseMode = SenseMode::TAPS;
    int senseAdcA = -1, senseAdcB = -1;  // ring channels held for the check
    float rNom = -1.0f;            // parsed part value in ohms, -1 = none
    uint8_t tolAuthorPct = 0;      // per-part `tol:`, 0 = the 15 % default
    bool bandFromAuthor = false;   // explicit min/max survived the legacy guards
    float inaSum = 0;
    int inaCount = 0;
    uint32_t inaLastMs = 0;
    float iOfs_mA = 0;             // zero-stimulus shunt current
    float iPart_mA = 0;            // I_raw - I_ofs
    float vSenseA = 0, vSenseB = 0;
    uint32_t ina1LastCheckMs = 0;  // rate limit for the INA1 source watchdog
    bool reSettleToTaps = false;   // STIM_SETTLE is re-arming for a degrade,
                                   // not for the first current sample
    float vOfsA = 0, vOfsB = 0;    // the sense pair at the zero-stimulus point
    bool haveVofs = false;
    bool haveVsense = false;
    char detail[112];              // the terminal's long-form line (§4)

    // oscillates
    int oscGpioIdx = -1;           // gpioDef index of the ephemeral route; -1 = tap fallback
    int oscTarget = -1;            // remembered for teardown (never deref step there)
    bool oscPreTapPending = false; // the safety sample, before any GPIO route
    // Why the tap fallback was entered - it decides what the verdict may
    // honestly claim, and it is quoted back to the user. 0 = no routable GPIO
    // was free, 1 = the project's rails exceed the GPIO domain, 2 = the target
    // itself read above it.
    uint8_t oscFallbackReason = 0;
    bool oscLastLevel = false, oscHaveLevel = false;
    uint32_t oscEdges = 0;
    unsigned long oscSettleUntilMs = 0;
    unsigned long oscWindowStartMs = 0;
    uint32_t oscWindowMs = 500;
    float oscVLo = 99.0f, oscVHi = -99.0f;
    int oscTapOks = 0, oscTapFloats = 0;

    // rail_sane transient (this check applied power; teardown restores)
    bool railTransient = false;
    float railMeasured = 0;        // §3: the TOP rail's OWN measured voltage
    bool railRefPending = false;   // the in-flight tap is a rail's, not a row's
    int railAdc = -1;              // channel the TOP rail tap landed on - every
                                   // rowClass 1 row is compared on the SAME one
    // ...and the same two for the BOTTOM rail. A project may power parts from
    // either rail (or both), so rowClass 3 rows get their own reference
    // measurement and their own preferred channel - the same-channel gain
    // cancellation of §3 is per-rail or it is not cancellation at all.
    float railMeasuredBot = 0;
    int railAdcBot = -1;
    uint8_t railRefWhich = 0;      // which rail the in-flight ref tap is: 0 top, 1 bottom
    bool railTopRefDone = false;
    bool railBotRefDone = false;
    float railWorstDelta = 0;
    float railWorstV = 0;
    int railWorstRow = -1;

    // presence bookkeeping (charge-retention - see the PRESENCE case)
    int presLegsAdded = 0;         // ISENSE_MINUS->rows[i] legs live, i < this
    bool presStranded = false;     // row legs removed, charge stranded
    bool presGrounded = false;     // 2-leg variant: rowB held at GND
    float presMinHold = 99.0f;
    int presMinHoldRow = -1;

    // outcome
    int result = GUIDE_CHECK_RUNNING;
    char val[24];
    char hint[CK_HINT_MAX];
};

static CheckState ck;

// A hint that does not FIT is worse than no hint: finishCheck strncpy's into
// ck.hint and GuidedFlow prints the result on the terminal AND on the 128x32
// OLED, so an over-long one is severed mid-word at the reader
// ("...so presence means nothi"). Two of the H1 wave's four new hints did
// exactly that and shipped through a bench run, because the evidence block
// only ever showed `val=`, never the sentence.
//
// Wrap a hint literal in ckHint() and the compiler proves it fits. The array
// size is deduced, so this costs nothing at runtime and cannot be fooled by a
// concatenation. Existing hints top out at 88 bytes; keep new ones there too -
// the cap is the buffer, but the OLED is the real reader.
template <size_t N>
static constexpr const char* ckHint(const char (&s)[N]) {
    static_assert(N <= CK_HINT_MAX,
                  "guide check hint is longer than ck.hint[] - it would be cut "
                  "mid-word on the terminal and the OLED");
    return s;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// "The user claimed this node": any non-infra bridge touches it (infra
// bridges are system-owned and invisible by construction - InfraPaths.h).
// Ephemeral measurement bridges live in the same array and count too: they
// mean the node is electrically in use right now.
static bool nodeHasUserBridge(int node) {
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numBridges; i++) {
        int n1 = c.bridges[i][0], n2 = c.bridges[i][1];
        if (n1 != node && n2 != node) continue;
        if (infraIsBridge(n1, n2)) continue;
        return true;
    }
    return false;
}

// A node that SOURCES or SINKS on its own - the two rail outputs, the board
// ground, the DAC outputs, and the shunt's own terminals.
//
// WHY THE STIMULUS CHECKS MUST REFUSE THESE AS rowA/rowB. The chain is
// DAC0 -> rowA -> [part] -> rowB -> ISENSE_PLUS -> [2 ohm shunt] ->
// ISENSE_MINUS -> GND. Put a live rail on rowB and that is the rail across
// 2 ohm to ground; the file's own bench figure for the return path is ~130
// ohm (see chainComplete), so 5 V lands at ~38 mA and 3.3 V at ~25 mA - both
// UNDER the 50 mA INA watchdogs, so nothing stops it, and it flows for the
// ~250-300 ms the zero-stimulus leak gate needs to notice. Pre-power it is
// worse in a different way: the rail op-amp sinking in parallel with the
// shunt produces a confident, wrong verdict.
//
// nodeHasUserBridge() cannot cover this. In COMPACT placement partPinNode()
// returns pin.connect verbatim, and expandOnePart SUPPRESSES the leg's bridge
// for exactly that case (`node == pin.connect`), so a compact part whose last
// pin is `connect: TOP_RAIL` presents rowB == TOP_RAIL with no bridge on it at
// all. globalDoNotIntersects catches the rowA side by accident ({TOP_RAIL,DAC0}
// is in the table) but has no ISENSE_PLUS entry, so the rowB side routes; and
// "stimulus chain routing failed" is the wrong thing to tell a user about a
// refusal this check should be making on purpose.
static bool nodeIsDrivenSource(int node) {
    return node == TOP_RAIL || node == BOTTOM_RAIL || node == GND ||
           node == DAC0 || node == DAC1 ||
           node == ISENSE_PLUS || node == ISENSE_MINUS;
}

// Any bridge at all touches the node (used for the oscillates GPIO pick -
// an infra-claimed GPIO is just as unavailable as a user-bridged one).
static bool nodeHasAnyBridge(int node) {
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numBridges; i++) {
        if (c.bridges[i][0] == node || c.bridges[i][1] == node) return true;
    }
    return false;
}

// rowA/rowB for the stimulus checks: explicit n1/n2 when both are given
// (author's choice on a verify step), else the part's first and last listed
// pin rows (the 2-lead case the matrix describes; vf: rowA = anode = first).
static bool resolveRowPair(const GuideStep& st, int* rowA, int* rowB) {
    if (st.n1 >= 0 && st.n2 >= 0) {
        *rowA = st.n1;
        *rowB = st.n2;
        return true;
    }
    if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
        const PartDefinition& p = globalState.parts.parts[st.partIdx];
        if (p.numPins >= 2) {
            int a = partPinNode(p, p.pins[0]);
            int b = partPinNode(p, p.pins[p.numPins - 1]);
            if (a >= 1 && b >= 1 && a != b) {
                *rowA = a;
                *rowB = b;
                return true;
            }
        }
    }
    return false;
}

// Did THIS refresh refuse to route the pair (n1,n2)? Order-insensitive.
// unconnectablePaths[] holds up to 10 entries and is rebuilt every routing
// pass, so it is only meaningful immediately after refreshLocalConnections().
static bool routeRefused(int n1, int n2) {
    int n = numberOfUnconnectablePaths;
    if (n > 10) n = 10;   // the producer has one unguarded push site
    for (int i = 0; i < n; i++) {
        int a = unconnectablePaths[i][0], b = unconnectablePaths[i][1];
        if ((a == n1 && b == n2) || (a == n2 && b == n1)) return true;
    }
    return false;
}

// Read BOTH sense channels out of ONE ring dwell (Option 1's sampler). Same
// fresh-window discipline as pairSenseTap: a generation bump or too few
// sweeps means the window is not ours, and best-available history describes
// whatever was on the channel before - fail the read instead of trusting it.
// No fastConnectPath anywhere: the sense legs are already closed as ordinary
// ephemeral bridges, so this is a pure memory read of ~600 us.
//
// Core 0. The ring's readers are documented as either-core safe (the sample
// counter is under a claimed SIO spinlock) and nothing here touches the
// converter, the ADC lock or a crosspoint.
//
// KNOWN RESIDUAL - THIS PATH IMPLEMENTS NEITHER OF §1.6's GAIN-CANCELLATION
// OPTIONS. invest-measurement.md §1.6 offers two ways to keep per-channel
// divider mismatch out of a SUBTRACTED pair: swap the channels across two pair
// taps and average, or read both nodes sequentially on the SAME channel. The
// TAPS fallback does the second (see tapCyclePair). This one does neither: it
// reads two DIFFERENT channels, and the two-point offset correction in
// evaluateContinuity/evaluateVf cancels OFFSET, not GAIN - a gain error scales
// with the reading, so it survives the subtraction.
//
// Sized, not hand-waved: after the per-channel adcSpread[]/adcZero[]
// calibration the residual mismatch is ~1-2 % of each node's voltage, which at
// a real part (330 ohm: 3.0 V on rowA, 0.86 V on rowB) is ~38 mV of error on a
// 2.14 V drop - under 2 % of R. That is inside the meter budget the tol_meas
// ladder already carries and nowhere near the +/-20-25 % bands, so it changes
// no verdict; it is disclosed here and in the report's §6 error model because
// §6's whole job is accuracy honesty. Fixing it properly would mean a second
// dwell with the channels swapped (two ring reads per sample point) - cheap,
// but it buys accuracy nobody is currently short of.
static bool ringReadPair(int chA, int chB, float* vA, float* vB,
                         float* driftA, float* driftB) {
    if (chA < 0 || chB < 0) return false;
    if (!adcRingActive()) {
        // Legacy START_ONCE path (the D-menu A/B toggle). No shared-sweep
        // trick available; two ordinary reads of a DC-held node are fine.
        *vA = readAdcVoltage(chA, 8);
        *vB = readAdcVoltage(chB, 8);
        if (driftA) *driftA = 0.0f;
        if (driftB) *driftB = 0.0f;
        return true;
    }
    uint32_t gen = adcRingGeneration();
    uint32_t s0 = adcRingSweeps();
    delayMicroseconds(500);
    uint32_t s1 = adcRingSweeps();
    if (!adcRingActive() || adcRingGeneration() != gen || (s1 - s0) < 17) return false;
    int rawEarlyA = adcRingMeanWindow(chA, s0 + 9, 8);
    int rawEarlyB = adcRingMeanWindow(chB, s0 + 9, 8);
    int rawLateA  = adcRingMeanWindow(chA, s1, 8);
    int rawLateB  = adcRingMeanWindow(chB, s1, 8);
    if (!adcRingActive() || adcRingGeneration() != gen) return false;
    // Channels 4 and 5 carry no zero calibration (they are not on the +/-8 V
    // divider path) - the same guard pairSenseTap applies. Unreachable while
    // the acquire mask is 0x0F, and that is exactly why it is written down:
    // a future mask widening would otherwise subtract a meaningless offset.
    const float scaleA = adcSpread[chA] / 4095.0f;
    const float zeroA = (chA != 4 && chA != 5) ? adcZero[chA] : 0.0f;
    const float scaleB = adcSpread[chB] / 4095.0f;
    const float zeroB = (chB != 4 && chB != 5) ? adcZero[chB] : 0.0f;
    float earlyA = (float)rawEarlyA * scaleA - zeroA;
    float earlyB = (float)rawEarlyB * scaleB - zeroB;
    *vA = (float)rawLateA * scaleA - zeroA;
    *vB = (float)rawLateB * scaleB - zeroB;
    if (driftA) *driftA = *vA - earlyA;
    if (driftB) *driftB = *vB - earlyB;
    return true;
}

// Terminal-state funnel: teardown FIRST (before anything can early-return),
// then record the outcome. Every terminal path goes through here.
static void checkTeardown(void);
// Option 1 -> one-shot taps, bridges and channels and all (defined below,
// called from chainBegin above it).
static void dropSenseLegsToTaps(bool energized);

static int finishCheck(int result, const char* hintText, const char* valFmt, ...) {
    checkTeardown();
    ck.result = result;
    ck.phase = CkPhase::DONE;
    va_list args;
    va_start(args, valFmt);
    vsnprintf(ck.val, sizeof(ck.val), valFmt, args);
    va_end(args);
    if (hintText != nullptr) {
        strncpy(ck.hint, hintText, sizeof(ck.hint) - 1);
        ck.hint[sizeof(ck.hint) - 1] = '\0';
    }
    return result;
}

// The terminal's long-form line (§4). Set before finishCheck; printed by
// GuidedFlow on PASS AND FAIL. Spaces are fine here - unlike ck.val, this is
// never a machine token.
static void setDetail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ck.detail, sizeof(ck.detail), fmt, args);
    va_end(args);
}

// Remove everything this check may have put on the board. Idempotent; the
// single teardown funnel for pass / fail / timeout / abort. DAC and rails
// go back to globalState.power TRUTH (0 V before power_on commits; the
// committed file values afterwards) - stimulus writes were save=0, so state
// never moved.
static void checkTeardown(void) {
    String err;
    bool removed = false;
    // FIRST, and before the refresh below. A pending one-shot tap request
    // outlives the check that posted it: serviceOneShotTap() sits at the head
    // of serviceNetVoltageScan() ungated by the scan's own enable/menu/idle
    // rules, so the moment this teardown's refreshLocalConnections+waitCore2
    // releases core1busy/refreshInProgress the scan core closes a real sense
    // route on a board nobody is measuring. guideCheckAbort() has cancelled it
    // for the user-abort path since wave 1; the three terminal exits that only
    // reach finishCheck() - the INA0 watchdog, the INA1 source watchdog and the
    // overall timeout - can all fire with ck.phase == TAP_WAIT and had no
    // cancel at all. This is that single choke point, so every exit inherits
    // it. Guarded on a request actually being in flight (NetVoltageScan.cpp),
    // so the ordinary pass path - where the tap that produced the value has
    // already been retired - pays nothing.
    cancelOneShotTap();
    if (ck.chainLive) {
        // Stimulus dies FIRST, then the routes open. (Presence-abort mid-
        // charge discharges its rows through the still-closed legs here -
        // fine, an aborted check owes no stranded charge.)
        setDac0voltage(globalState.power.dac0, 0, 0);
        // The three stimulus legs. PRESENCE still builds the OLD top-side
        // chain (DAC0->ISENSE_PLUS, ISENSE_MINUS->rowA, rowB->GND) because it
        // charges rather than measures current; continuity/vf build the
        // ground-side one (DAC0->rowA, rowB->ISENSE_PLUS, ISENSE_MINUS->GND).
        // chainHeadA/B remember which pair each leg actually used so teardown
        // never has to re-derive the topology.
        if (ck.chainAdded[0]) {
            removed |= globalState.removeEphemeralConnection(ck.legNode[0][0], ck.legNode[0][1], err, false, 0);
        }
        if (ck.chainAdded[1]) {
            removed |= globalState.removeEphemeralConnection(ck.legNode[1][0], ck.legNode[1][1], err, false, 0);
        }
        if (ck.chainAdded[2]) {
            removed |= globalState.removeEphemeralConnection(ck.legNode[2][0], ck.legNode[2][1], err, false, 0);
        }
        // Option 1's two sense bridges.
        if (ck.chainAdded[3]) {
            removed |= globalState.removeEphemeralConnection(ck.legNode[3][0], ck.legNode[3][1], err, false, 0);
        }
        if (ck.chainAdded[4]) {
            removed |= globalState.removeEphemeralConnection(ck.legNode[4][0], ck.legNode[4][1], err, false, 0);
        }
        if (!ck.presStranded) {
            for (int i = 0; i < ck.presLegsAdded; i++) {
                removed |= globalState.removeEphemeralConnection(ISENSE_MINUS, ck.rows[i],
                                                                err, false, 0);
            }
        }
        ck.presLegsAdded = 0;
        ck.chainLive = false;
        for (int i = 0; i < 5; i++) ck.chainAdded[i] = false;
    }
    // Option 1 holds its two sense channels for the whole check (unlike the
    // one-shot taps, which acquire and release per tap). Hand them back on
    // EVERY exit path - a mid-check abort included, which is why this sits in
    // the teardown funnel and not at the end of the happy path.
    if (ck.senseAdcA >= 0 || ck.senseAdcB >= 0) {
        infraReleaseAdc(INFRA_ADC_GUIDE);
        ck.senseAdcA = ck.senseAdcB = -1;
    }
    if (ck.oscGpioIdx >= 0) {
        removed |= globalState.removeEphemeralConnection(ck.oscTarget,
                                                         gpioDef[ck.oscGpioIdx][1],
                                                         err, false, 0);
        ck.oscGpioIdx = -1;
    }
    if (removed) {
        // One refresh for however many removals actually happened (a partial
        // chain from a failed setup still gets its survivors cleaned off the
        // hardware - per-removal applyRouting would skip the refresh when a
        // later pair was never added).
        refreshLocalConnections(1, 0, 0);
        waitCore2();
    }
    if (ck.railTransient) {
        setTopRail(globalState.power.topRail, 0, 0);
        setBotRail(globalState.power.bottomRail, 0, 0);
        setDac0voltage(globalState.power.dac0, 0, 0);
        setDac1voltage(globalState.power.dac1, 0, 0);
        ck.railTransient = false;
    }
}

// ---------------------------------------------------------------------------
// Tap sub-machine (shared by presence / voltage / vf / rail_sane / osc-fallback)
// ---------------------------------------------------------------------------
// Returns 0 while in progress, 1 with a value, -1 floating, -2 hard failure
// (consecutive route/ADC refusals - the node has no reachable sense route).
// Transient -2 results re-request with a short backoff; ck.timeoutMs bounds
// the whole affair from the caller's side.

static const int kTapHardFailLimit = 8;

// preferAdc / adcUsed: rail_sane compares every row against the RAIL's own
// reading, so both taps want the same channel - that is what makes "ADC gain
// error genuinely cancels" true rather than aspirational (§1.6/§3). A hint is
// never a reservation; adcUsed reports what was actually granted.
static int tapCycleNode(int node, float* outV, float* outDrift,
                        int preferAdc = -1, int* adcUsed = nullptr) {
    if (ck.phase == CkPhase::TAP_REQUEST) {
        if (millis() < ck.tapBackoffUntilMs) return 0;
        if (requestNodeTap(node, preferAdc)) ck.phase = CkPhase::TAP_WAIT;
        // else: a stale in-flight tap is draining - retry next poll
        return 0;
    }
    float v, d;
    int usedAdc = -1;
    int r = nodeTapResult(&v, &d, &usedAdc);
    if (adcUsed) *adcUsed = usedAdc;
    if (r == 0) return 0;
    if (r == -2) {
        ck.phase = CkPhase::TAP_REQUEST;
        ck.tapBackoffUntilMs = millis() + 30;
        if (++ck.tapHardFails >= kTapHardFailLimit) return -2;
        return 0;
    }
    ck.tapHardFails = 0;
    ck.phase = CkPhase::TAP_REQUEST; // ready for the next row
    if (outV) *outV = v;
    if (outDrift) *outDrift = d;
    return r;
}

// The A/B pair used by vf: TWO SEQUENTIAL ONE-NODE TAPS, preferring the same
// ADC channel for both - not the simultaneous pair tap this used to request.
//
// Why (invest-vf-noroute.md §2/§4 + the wave-2 ruling): a simultaneous pair
// needs two DISJOINT sense routes closed at once, and the stimulus chain
// always terminates on the SAME chip as the rows under test - so on a
// populated board the first route takes the last free chip-K y row and the
// second starves. Every single tap in the bench session routed while the pair
// failed 24 times running. Sequential is also the more accurate measurement:
// the stimulus is DC and held for the whole check, so nothing moves between
// the two taps, and reading both nodes on the SAME channel makes that
// channel's zero offset and gain cancel exactly in vA - vB (a simultaneous
// two-channel read instead turns per-channel gain mismatch into error
// proportional to V - invest-measurement.md §1.6).
//
// Cost: ~11 ms per tap, two taps instead of one dwell. The preferred-ADC hint
// is best-effort; a fallback to a different channel is reported, not fatal.
//
// Same return contract as before: 0 in progress, 1 both values, -1 floating,
// -2 hard failure. On -2, ck.tapFailNode names the node that could not be
// reached (the caller reports noroute@<node>) and one diagnostic line plus
// the tap counters go to Serial - core 0 only, the scan core must never print.
static int tapCyclePair(int n1, int n2, float* outV1, float* outV2) {
    int node = (ck.tapSeqPhase == 0) ? n1 : n2;
    if (ck.phase == CkPhase::TAP_REQUEST) {
        if (millis() < ck.tapBackoffUntilMs) return 0;
        // Node A takes whatever is free; node B asks for A's channel.
        int prefer = (ck.tapSeqPhase == 1) ? ck.tapSeqAdc : -1;
        if (requestNodeTap(node, prefer)) ck.phase = CkPhase::TAP_WAIT;
        // else: a stale in-flight tap is draining - retry next poll
        return 0;
    }
    float v, d;
    int adcUsed = -1;
    int r = nodeTapResult(&v, &d, &adcUsed);
    if (r == 0) return 0;
    if (r == -2) {
        ck.phase = CkPhase::TAP_REQUEST;
        ck.tapBackoffUntilMs = millis() + 30;
        if (++ck.tapHardFails >= kTapHardFailLimit) {
            ck.tapFailNode = node;
            // report §6's "one print that settles it": Kfree with <=1 bit set
            // (or bits whose d-side lane is busy) IS route starvation;
            // node=-1 rc=0 with ringstale moving is the other candidate.
            OneShotTapFail f;
            oneShotTapFailInfo(&f);
            Serial.printf("TAP noroute node=%d->ADC%d rc=%d Kfree=0x%02X "
                          "Kxbusy=0x%04X %cxbusy=0x%04X bounceOk=0x%02X\n",
                          f.node, f.adc, f.rc, (unsigned)f.kFreeYMask,
                          (unsigned)f.kXBusyMask,
                          (f.chip >= 0 && f.chip < 12) ? (char)('A' + f.chip) : '?',
                          (unsigned)f.xBusyMask, (unsigned)f.bounceOkMask);
            printTapCounters(&Serial);
            ck.tapSeqPhase = 0;
            return -2;
        }
        return 0;
    }
    ck.tapHardFails = 0;
    ck.phase = CkPhase::TAP_REQUEST;
    if (r == -1) {                 // floating: one bad end voids the pair
        ck.tapFailNode = node;
        ck.tapSeqPhase = 0;
        return -1;
    }
    if (ck.tapSeqPhase == 0) {     // node A landed - hold it, go get node B
        ck.tapSeqV1 = v;
        ck.tapSeqAdc = adcUsed;
        ck.tapSeqPhase = 1;
        return 0;
    }
    ck.tapSeqPhase = 0;
    if (adcUsed != ck.tapSeqAdc && !ck.tapSeqAdcFallback) {
        // Not a failure - the difference just carries the two channels'
        // offset/gain mismatch instead of cancelling it. Say so once, and
        // NEVER in `val` (the parseable field stays a plain voltage).
        ck.tapSeqAdcFallback = true;
        Serial.printf("  tap: rows %d/%d read on different ADCs (%d/%d) - "
                      "offset cancellation lost\n",
                      n1, n2, ck.tapSeqAdc, adcUsed);
    }
    if (outV1) *outV1 = ck.tapSeqV1;
    if (outV2) *outV2 = v;
    return 1;
}

// ---------------------------------------------------------------------------
// Begin
// ---------------------------------------------------------------------------

// Add one leg and remember its endpoints for the teardown funnel.
static bool addLeg(int slot, int n1, int n2) {
    String err;
    ck.legNode[slot][0] = n1;
    ck.legNode[slot][1] = n2;
    ck.chainAdded[slot] = globalState.addEphemeralConnection(n1, n2, err, false, 0);
    return ck.chainAdded[slot];
}

// Build + energize the continuity/vf stimulus chain: GROUND-SIDE SHUNT plus
// Option 1's sense bridges, ALL FIVE LEGS IN ONE refreshLocalConnections.
//
//   DAC0 --- rowA --- [part] --- rowB --- ISENSE_PLUS
//                                          [2 ohm shunt R1, INA0]
//                                        ISENSE_MINUS --- GND
//   rowA --- ADC(chA)          rowB --- ADC(chB)      (high-Z sense legs)
//
// WHY THE SHUNT MOVED TO THE GROUND SIDE (invest-measurement.md §1.2, proved
// on the bench 2026-08-21 before this was written). The CURR_SENSE- net
// carries a constant board-internal sink: with the OLD top-side chain
// (DAC0->ISENSE_PLUS, ISENSE_MINUS->rowA, rowB->GND) and NO part, INA0 read
// 2.319 mA - every milliamp of it through the shunt, which is why the old
// code had to measure and subtract a baseline, and why that subtraction
// systematically UNDER-reported part current (under load the CURR_SENSE- node
// sits lower, so the sink draws less than its unloaded baseline). With the
// shunt on the ground side the same empty chain reads 0.000 mA: the sink now
// hangs DOWNSTREAM of the shunt, where it can only split current that has
// already been counted. Positive control from the same session: bridging
// rowA to rowB read 16.17 mA, so the ISENSE_MINUS->GND leg really does route
// and really does close the loop. The BASE_SETTLE / BASE_SAMPLE / CHAIN_GROW
// baseline dance and its second refresh are gone with it.
//
// WHY THE SENSE LEGS RIDE THE SAME REFRESH (invest-vf-noroute.md §8 Option 1).
// The one-shot tap builder plans a sense route ALONE, in three tiers, after
// the stimulus chain has already claimed the escape lanes of the very chip
// the rows under test live on - task 2 measured that starving with
// bounceOk=0x00, i.e. not one of chips A-H could serve as a bounce. Adding
// rowA->ADCx / rowB->ADCy as ordinary ephemeral bridges hands the whole
// problem to the FULL router, which plans stimulus and sense together, has
// far more route shapes, and may lawfully share the target net's own lanes.
// ADC bridges are exempt from fillUnusedPaths duplication, and the refresh
// passes fillUnused=0 anyway. Cost: 5 of the 8 ephemeral slots and two ADC
// channels held for the whole check.
//
// No current flows in the sense legs (the ADC path is a ~1M divider), so
// every ohm of stimulus-path resistance drops out of V_rowA - V_rowB. That is
// the whole 4-wire fix: R = (V_A - V_B) / I_part contains no crosspoint
// count, no crosspoint resistance, no DAC output impedance and no sink.
static bool chainBegin(int rowA, int rowB, float stimulusVolts) {
    ck.chainRowA = rowA;
    ck.chainRowB = rowB;
    ck.stimVolts = stimulusVolts;
    ck.chainLive = true; // set BEFORE the adds so teardown always runs

    // Channels FIRST - the user-claimed-ADC exclusion has to pick before the
    // bridges are authored. infraAcquireAdc's keep-what-you-own rule is gated
    // on the candidate mask, so masking chA's bit out of the second call is
    // what makes one user hold two channels. Mask 0x0F: ADC0-3 only (ADC4 is
    // not on the calibrated tap path), allowSharedTdm=false (we are routing a
    // BRIDGE onto the channel - riding along on TDM's would be a real
    // electrical conflict, not a shared read).
    ck.senseAdcA = infraAcquireAdc(INFRA_ADC_GUIDE, 0x0F, false);
    if (ck.senseAdcA >= 0) {
        ck.senseAdcB = infraAcquireAdc(INFRA_ADC_GUIDE,
                                       (uint8_t)(0x0F & ~(1u << ck.senseAdcA)), false);
    }
    if (ck.senseAdcA < 0 || ck.senseAdcB < 0) {
        // Both free channels are user-bridged (the 555 file does exactly this
        // with ADC0/ADC1). Fall back to task 2's sequential-same-ADC one-shot
        // taps, which acquire ONE channel per tap under NVSCAN - §1.6 calls
        // that the preferred same-ADC mode anyway, so this is a degrade in
        // routing robustness, not in accuracy.
        infraReleaseAdc(INFRA_ADC_GUIDE);
        ck.senseAdcA = ck.senseAdcB = -1;
        ck.senseMode = SenseMode::TAPS;
    } else {
        ck.senseMode = SenseMode::RING;
    }

    bool ok = addLeg(0, DAC0, rowA) &&
              addLeg(1, rowB, ISENSE_PLUS) &&
              addLeg(2, ISENSE_MINUS, GND);
    if (ok && ck.senseMode == SenseMode::RING) {
        ok = addLeg(3, rowA, ADC0 + ck.senseAdcA) &&
             addLeg(4, rowB, ADC0 + ck.senseAdcB);
    }
    if (!ok) return false;

    refreshLocalConnections(0, 0, 0); // LED option 0: no visual disruption
    waitCore2();

    // The router's own verdict, read while it is still about THIS refresh.
    // A stimulus leg that could not be placed is a hard, honest setup failure.
    if (routeRefused(DAC0, rowA) || routeRefused(rowB, ISENSE_PLUS) ||
        routeRefused(ISENSE_MINUS, GND)) {
        return false;
    }
    // A SENSE leg that could not be placed is different: the old check had no
    // sense route at all and still ran, so refusing here would make a check
    // unrunnable where it used to work (§1.9's standing rule). Degrade to the
    // taps instead and say so once.
    if (ck.senseMode == SenseMode::RING &&
        (routeRefused(rowA, ADC0 + ck.senseAdcA) ||
         routeRefused(rowB, ADC0 + ck.senseAdcB))) {
        Serial.printf("  sense: chain routed but a sense bridge did not "
                      "(rows %d/%d) - falling back to one-shot taps\n\r", rowA, rowB);
        dropSenseLegsToTaps(false);
    }

    // save=0: state truth stays at the guide's safe 0 V - the slot cannot
    // persist the stimulus, and infra's DAC0 probe-power candidate stays
    // non-viable (it reads state) for the whole check.
    // Held at ZERO first: OFS_SAMPLE wants the zero-stimulus offset of the
    // whole isense path, which is also the leak test the ground-side topology
    // has to keep passing.
    setDac0voltage(0.0f, 0, 0);
    ck.stimAppliedMs = millis();
    ck.inaSum = 0;
    ck.inaCount = 0;
    ck.inaLastMs = currentSenseState.lastUpdatedMs;
    ck.phase = CkPhase::OFS_SETTLE;
    return true;
}

// Does this rail_sane run owe a reference tap, and for which rail? Only
// power-class rows are compared against a measured rail; a GND-only run needs
// no reference at all (the breadboard minus rails are hard ground - §3 step 3).
// A mixed-supply project owes BOTH, and they are taken one after the other.
static bool railSaneNeedsTopRef(void) {
    for (int i = 0; i < ck.numRows; i++) {
        if (ck.rowClass[i] == CK_ROWCLASS_TOP) return true;
    }
    return false;
}

static bool railSaneNeedsBotRef(void) {
    for (int i = 0; i < ck.numRows; i++) {
        if (ck.rowClass[i] == CK_ROWCLASS_BOT) return true;
    }
    return false;
}

static bool railSaneNeedsRef(void) {
    return railSaneNeedsTopRef() || railSaneNeedsBotRef();
}

// Is another reference tap still owed? Drives the RAIL_REF re-entry.
static bool railSaneRefOutstanding(void) {
    return (railSaneNeedsTopRef() && !ck.railTopRefDone) ||
           (railSaneNeedsBotRef() && !ck.railBotRefDone);
}

// Give up on Option 1 mid-check and hand the voltage half back to the one-shot
// taps. This is NOT just a mode flag - three things have to come apart or the
// fallback is worse than the failure it handles:
//
//  1. the two sense BRIDGES come off the fabric. Leaving them would point a
//     live ephemeral bridge at an ADC node that a tap is about to
//     fastConnectPath onto - the one thing the tap router's wouldShort
//     validation exists to prevent;
//  2. the two CHANNELS go back to the pool. The tap acquires under
//     INFRA_ADC_NVSCAN and would otherwise be starved of exactly the two
//     channels we proved were free;
//  3. the ring-measured OFFSET pair is discarded. The taps read both nodes on
//     ONE channel, so subtracting a delta captured on two different channels
//     would inject the very mismatch the two-point correction removes.
//
// The stimulus drops for the refresh (no live voltage while crosspoints move,
// the same rule chainComplete used to follow) and comes back after.
// `energized`: whether the stimulus is currently applied. Explicit rather
// than inferred from ck.phase, because chainBegin calls this BEFORE the first
// setDac0voltage and inferring would energize the chain early.
static void dropSenseLegsToTaps(bool energized) {
    String err;
    bool removed = false;
    if (energized) setDac0voltage(0.0f, 0, 0);
    for (int slot = 3; slot <= 4; slot++) {
        if (!ck.chainAdded[slot]) continue;
        removed |= globalState.removeEphemeralConnection(ck.legNode[slot][0],
                                                         ck.legNode[slot][1],
                                                         err, false, 0);
        ck.chainAdded[slot] = false;
    }
    if (removed) {
        refreshLocalConnections(0, 0, 0);
        waitCore2();
        // No routeRefused() check after THIS refresh, unlike chainBegin's.
        // Deliberate: this pass only REMOVES bridges, and removing bridges can
        // only relax fabric pressure - a stimulus leg that routed with the two
        // sense legs present cannot fail to route without them. The file
        // otherwise treats unconnectablePaths[] as a per-refresh contract, so
        // the one place that skips reading it says why.
    }
    infraReleaseAdc(INFRA_ADC_GUIDE);
    ck.senseAdcA = ck.senseAdcB = -1;
    ck.haveVofs = false;
    ck.senseMode = SenseMode::TAPS;
    ck.tapHardFails = 0;
    ck.tapSeqPhase = 0;
    if (energized) {
        setDac0voltage(ck.stimVolts, 0, 0);
        // Re-arm the settle rather than tapping into a just-restored stimulus.
        // I_part was sampled at the PRE-DROP operating point and the taps are
        // about to sample voltage at the post-restore one, so the two halves
        // of R = dV / I only belong together if the operating point really is
        // identical - which needs the DAC's step to have finished. It is a
        // DC resistive load and the DAC settles in microseconds, so this is
        // cheap insurance rather than a fix for an observed fault; STIM_SETTLE
        // costs 60 ms once, on a rung that only runs when something already
        // went wrong.
        ck.stimAppliedMs = millis();
        ck.phase = CkPhase::STIM_SETTLE;
        ck.reSettleToTaps = true;
    }
}

// Which rail feeds this power pin? `pin.connect` is the authority and it works
// in BOTH placement modes: compact returns pin.connect as the node itself,
// expanded returns the footprint row which applyPartPlacement has already
// bridged to that same rail, so the tap reads the rail's potential either way.
//
// RESIDUAL, DELIBERATE: a power pin that connects to a plain ROW (fed from
// somewhere the parts table cannot see - a regulator's output, another part)
// stays class 1 and is compared to the top rail, which is the documented
// default at the `target:` branch of the RAIL_SANE case. Narrowing that would
// silently delete coverage from every project that passes today; the bug this
// fixes is a pin the file EXPLICITLY names the bottom rail for.
static uint8_t railRowClassFor(const PartDefinition& p, const PartPin& pin, int node) {
    if (pin.pinClass == 2) return CK_ROWCLASS_GND;
    (void)p;
    if (pin.connect == BOTTOM_RAIL || node == BOTTOM_RAIL) return CK_ROWCLASS_BOT;
    return CK_ROWCLASS_TOP;
}

// Collect the class-tagged rows of every placed part (rail_sane).
static void collectClassRows(void) {
    ck.numRows = 0;
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
            const PartPin& pin = p.pins[j];
            if (pin.pinClass != 1 && pin.pinClass != 2) continue;
            int node = partPinNode(p, pin);
            if (node < 1) continue;
            if (ck.numRows < CK_MAX_ROWS) {
                ck.rows[ck.numRows] = node;
                ck.rowClass[ck.numRows] = railRowClassFor(p, pin, node);
                ck.numRows++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// oscillates: the GPIO gate
// ---------------------------------------------------------------------------
// The frequency half of this check counts edges on an RP2350 GPIO, which means
// closing a crosspoint from the target node straight onto a 3.3 V pin (the
// routable bank is GPIO 20-27, taken to L.X4-X11 with NO level shifters).
// Nothing used to look at what the target sits at first.
//
// TWO GATES, and they are gates for different reasons:
//
//  1. THE SUPPLY CEILING - deterministic, and the one that actually protects
//     the shipped content. The 555 project runs `power: topRail: 5.0` and a
//     CMOS 555 substitute swings near-rail, so its OUT node reaches ~5 V. No
//     sample can be trusted to catch that (see gate 2), but the project's own
//     declared rails bound it exactly, for free, before any hardware moves.
//
//  2. THE ONE-SHOT TAP - the controller's ruling, kept because it catches what
//     the rails cannot: a node fed from outside the board.
//
// WHY THE TAP ALONE IS NOT ENOUGH, which is a correction to the ruling's
// letter. A tap is a ~500 us DC dwell (NetVoltageScan: early/late windows
// eight ring sweeps apart). This check's own band is 0.3-30 Hz, i.e. half
// cycles of 16 ms to 1.6 s, so a single tap on an oscillating node samples
// whichever half cycle it happens to land in and reads a clean, stable level
// there. On the very signal this check exists to measure it is a coin flip:
// high proves danger, low proves nothing. So the tap is used as a LOWER bound
// only, and the rails supply the UPPER one. Burst-sampling was considered and
// rejected - it turns a coin flip into a weighted coin flip; the ceiling is
// the deterministic half.
//
// Either refusal is EXPLAINED and then falls through to the existing high-Z
// tap fallback, which is safe at 5 V (the sense path is the +/-8 V divider).
//
// WHAT THIS COSTS, PLAINLY. The shipped 555 runs `power: topRail: 5.0`, so its
// final step - the project's marquee verification, `min: 0.3, max: 30` on the
// OUT pin - now takes gate 1 on EVERY run and can no longer be measured. The
// tap fallback still proves the pin is SWINGING, but not how fast, so that
// step now reports UNVERIFIED (GUIDE_CHECK_SKIPPED, val=unmeasured) instead of
// "1.4 Hz, inside the band". That is a real loss of verification for the one
// shipped project that uses this check, and it is the honest reading: the
// alternative was a PASS that silently waived the author's band, which is
// worse than an admitted gap. Warn-class still advances the build.
//
// THE WAY BACK, NOT BUILT TONIGHT. The fallback already takes repeated taps
// through the window; timestamping the level TRANSITIONS between them would
// give a real frequency estimate over the slow end of the band. Budget: a tap
// is ~11 ms plus poll overhead, so the effective sample rate is tens of Hz and
// Nyquist puts the usable ceiling at a few Hz - the 555's own ~1.4 Hz blink is
// comfortably measurable, 30 Hz is not. That would restore real verification
// for the common case and leave only the fast end unmeasured, which the
// verdict could then say precisely. Wants its own bench session with a real
// 555 on the board.
static const float kOscGpioMaxV = 3.6f;   // the FT pad's 3.3V domain, with margin
static const uint32_t kOscPreTapBudgetMs = 300; // room for the safety tap + retries

// The highest magnitude this project can put on a node, from the rails alone.
// Live power truth AND the script's own values: power_on has normally
// committed by the time an oscillates step runs, but a re-verify from a
// browsed-back step can reach here before that.
static float oscSupplyCeiling(void) {
    float c = fabsf(globalState.power.topRail);
    if (fabsf(globalState.power.bottomRail) > c) c = fabsf(globalState.power.bottomRail);
    if (ck.script != nullptr && ck.script->hasPower) {
        if (fabsf(ck.script->topRail) > c) c = fabsf(ck.script->topRail);
        if (fabsf(ck.script->bottomRail) > c) c = fabsf(ck.script->bottomRail);
    }
    return c;
}

// Pick a free routable GPIO and close the edge-counting route onto it.
// Returns true when ck.oscGpioIdx names a live route. The candidate filter is
// the probe-power scan's skip list, high end first (users reach for GPIO 1):
// not MicroPython-owned, no PWM, not the top-OLED pins, no bridge of any kind,
// config direction INPUT (the refresh's setGPIO pass re-asserts config, so an
// output-configured pin would drive the net - never pick one).
static bool oscTryGpioRoute(int target) {
    ck.oscGpioIdx = -1;
    for (int i = 7; i >= 0; i--) {
        int node = gpioDef[i][1];
        if (globalState.config.gpioPythonOwned[i]) continue;
        if (globalState.config.gpioPwmEnabled[i]) continue;
        if (globalState.config.gpioDirection[i] == 0) continue; // output
        if (jumperlessConfig.top_oled.enabled &&
            (node == jumperlessConfig.top_oled.gpio_sda ||
             node == jumperlessConfig.top_oled.gpio_scl)) continue;
        if (infraOwnsNode(node)) continue;
        if (nodeHasAnyBridge(node)) continue;
        ck.oscGpioIdx = i;
        break;
    }
    if (ck.oscGpioIdx < 0) return false;
    String err;
    if (!globalState.addEphemeralConnection(target, gpioDef[ck.oscGpioIdx][1],
                                            err, true, 0)) {
        ck.oscGpioIdx = -1; // nothing landed; no teardown owed
        return false;
    }
    gpio_set_dir(gpioDef[ck.oscGpioIdx][0], false); // input, belt+braces
    return true;
}

void guideCheckBegin(const GuideCheckRun& run) {
    guideCheckAbort(); // clears any prior run, teardown included

    ck = CheckState();
    ck.active = true;
    ck.step = run.step;
    ck.script = run.script;
    ck.powerApplied = run.powerApplied;
    ck.check = run.step->check;
    ck.timeoutMs = (run.step->timeoutMs > 0) ? run.step->timeoutMs : 1500;
    ck.startMs = millis();
    ck.val[0] = '\0';
    ck.hint[0] = '\0';
    ck.detail[0] = '\0';
    ck.phase = CkPhase::TAP_REQUEST;

    const GuideStep& st = *run.step;

    switch (ck.check) {

        case GuideCheck::PRESENCE: {
            // Charge-retention presence. The design's passive drift recipe
            // is UNDETECTABLE on this hardware for the case that matters
            // most: a fully-discharged bare row has no charge for the tap's
            // kick-and-decay to see, so an EMPTY row reads a stable 0.00V
            // and passed as "present" (bench-caught). Active variant with
            // the same primitives: charge the row(s) through the
            // DAC0->ISENSE stimulus, strand the charge, then tap. Verdict =
            // retention under the tap's 1M dwell load (tau = C x 1M): a
            // part-scale cap holds (stable high = present); a bare row
            // drains at sub-ms tau (drift mid-drain / stable-low drained =
            // absent). Still presence-HINT only (§5.2), default warn.
            //
            // IC carve-out (§5.2: "never hard-fail"): pF-scale pin
            // capacitance reads exactly like a bare row, so a correctly
            // seated IC would warn-fail every auto-synthesized place step.
            // Report it honestly as unverifiable instead - real
            // verification arrives at power_on / verify steps.
            //
            // The carve-out is keyed on the EXACT string "ic", which is the
            // only value the parts format documents for a chip (States.h:
            // resistor|capacitor|diode|led|bjt|fet|ic). An author who types
            // something else - "IC", "dip", "74hc00", or nothing - and then
            // writes an explicit `check: presence` on that step gets the
            // real stimulus instead: 3.3 V through the ~122 ohm DAC0/ISENSE
            // chain into an UNPOWERED chip's pins, i.e. current through the
            // die's ESD clamps. That is bounded by construction (charge is
            // <50 ms, the 50 mA INA watchdog aborts, and teardown drops the
            // DAC to 0 V) and is the same exposure a vf/continuity check on
            // any part accepts - so it is documented, not blocked. The
            // default-by-type path cannot reach it on its own:
            // guideDefaultCheckForPart (GuidedFlow.cpp) picks PRESENCE only
            // for typeStr "capacitor" or "ic", so landing here with some
            // other typeStr takes a deliberate author action - an explicit
            // `check: presence` on the step, or a per-part `verify:`
            // override. Widening the carve-out to a fuzzy match would be
            // worse: it would silently skip real 2-lead checks whose
            // typeStr merely contained "ic".
            if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts &&
                strcmp(globalState.parts.parts[st.partIdx].typeStr, "ic") == 0) {
                finishCheck(GUIDE_CHECK_SKIPPED,
                            "unpowered IC can't be electrically confirmed - real "
                            "verification happens at power_on",
                            "ic_unverified");
                break;
            }
            // Row set + protocol. A 2-leg part gets the GROUNDED variant:
            // rowB held at GND during charge, so a capacitor charges ACROSS
            // its plates and rowA then holds ground-referenced charge with
            // the full part capacitance (tau = C x 1M >> dwell) - the case
            // that actually detects a real cap. (Tied-charge - both legs at
            // 3.3V - leaves an isolated cap with only STRAY ground-referenced
            // charge and it reads like a bare row.) Single-target and >2-row
            // parts keep the tied variant: weaker, honest-limits documented.
            int presRowB = -1;
            if (st.target >= 1) {
                ck.rows[ck.numRows++] = st.target;
            } else if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
                const PartDefinition& p = globalState.parts.parts[st.partIdx];
                // Ephemeral budget: 1 chain head + N row legs must fit
                // MAX_EPHEMERAL_CONNECTIONS (8) - probe at most 7 rows.
                for (int j = 0; j < p.numPins && j < MAX_PART_PINS &&
                                ck.numRows < 7; j++) {
                    int node = partPinNode(p, p.pins[j]);
                    if (node >= 1) ck.rows[ck.numRows++] = node;
                }
                if (ck.numRows == 2) {
                    ck.presGrounded = true;
                    presRowB = ck.rows[1];
                    ck.numRows = 1; // tap rowA only; rowB is the return
                }
            }
            if (ck.numRows == 0) {
                finishCheck(GUIDE_CHECK_UNSUPPORTED, "presence needs a target or a part", "norows");
                break;
            }
            // Now a stimulus check: the full refusal rules apply.
            {
                bool inUse = false;
                for (int i = 0; i < ck.numRows; i++) {
                    if (nodeHasUserBridge(ck.rows[i])) inUse = true;
                }
                if (presRowB >= 1 && nodeHasUserBridge(presRowB)) inUse = true;
                if (inUse) {
                    finishCheck(GUIDE_CHECK_SKIPPED, "check skipped (rows in use)", "skip");
                    break;
                }
                // Same supply refusal as continuity/vf: presence charges its
                // rows from DAC0 through the shunt, so a rail or GND among them
                // is the same short with the same 25-38 mA under the watchdog.
                int badSupply = -1;
                for (int i = 0; i < ck.numRows && badSupply < 0; i++) {
                    if (nodeIsDrivenSource(ck.rows[i])) badSupply = ck.rows[i];
                }
                if (badSupply < 0 && presRowB >= 1 && nodeIsDrivenSource(presRowB)) {
                    badSupply = presRowB;
                }
                if (badSupply >= 0) {
                    finishCheck(GUIDE_CHECK_SKIPPED,
                                ckHint("can't charge a supply node - it holds its "
                                       "own voltage, so presence means nothing"),
                                "supply@%d", badSupply);
                    break;
                }
                if (nodeHasUserBridge(DAC0) || nodeHasUserBridge(ISENSE_PLUS) ||
                    nodeHasUserBridge(ISENSE_MINUS)) {
                    finishCheck(GUIDE_CHECK_SKIPPED,
                                "check skipped (DAC0/ISENSE in use)", "skip");
                    break;
                }
            }
            // Charge time is bounded (<50ms) - a reversed electrolytic sees
            // reverse 3.3V only for that moment; inrush 3.3V/~122ohm stays
            // well under the 50mA watchdog.
            ck.stimVolts = 3.3f;
            ck.chainRowA = ck.rows[0];
            ck.chainRowB = presRowB;
            ck.chainLive = true; // BEFORE the adds: teardown always runs
            {
                // PRESENCE keeps the OLD top-side chain deliberately: it
                // CHARGES a row and then strands the charge, so the shunt is
                // not in the measurement at all and the ground-side move
                // (which exists to keep the CURR_SENSE- sink out of the
                // ammeter) buys it nothing. Its legs are removed by the same
                // funnel through legNode[].
                String err;
                bool ok = addLeg(0, DAC0, ISENSE_PLUS);
                if (ck.presGrounded) {
                    ok = ok && addLeg(1, ISENSE_MINUS, ck.chainRowA);
                    ok = ok && addLeg(2, ck.chainRowB, GND);
                } else {
                    for (int i = 0; ok && i < ck.numRows; i++) {
                        ok = globalState.addEphemeralConnection(ISENSE_MINUS, ck.rows[i],
                                                                err, false, 0);
                        if (ok) ck.presLegsAdded = i + 1;
                    }
                }
                if (!ok) {
                    finishCheck(GUIDE_CHECK_FAIL, "charge chain routing failed", "setup");
                    break;
                }
                refreshLocalConnections(0, 0, 0);
                waitCore2();
            }
            setDac0voltage(3.3f, 0, 0); // save=0: state truth stays safe
            ck.stimAppliedMs = millis();
            ck.phase = CkPhase::PRES_CHARGE;
            // Two refreshes + N taps need room beyond the 1500ms default.
            {
                uint32_t need = 800 + 250u * (uint32_t)ck.numRows;
                if (ck.timeoutMs < need) ck.timeoutMs = need;
            }
            break;
        }

        case GuideCheck::CONTINUITY:
        case GuideCheck::VF: {
            // The INA poll runs at 50 ms, not 10 - so the §1.4 sample counts
            // (4 offset + 8 loaded) cost ~600 ms on their own, plus two
            // settles and the chain refresh. Raise the floor the way the
            // presence case does rather than let a tight authored timeout
            // false-fail a measurement that was going fine.
            if (ck.timeoutMs < 1400) ck.timeoutMs = 1400;
            int rowA, rowB;
            if (!resolveRowPair(st, &rowA, &rowB)) {
                finishCheck(GUIDE_CHECK_UNSUPPORTED,
                            "needs n1+n2 or a part with two pins", "norows");
                break;
            }
            // The refusal rule: stimulus only into ISOLATED rows, and only
            // through unclaimed DAC0/ISENSE. GND stays legal - rowB->GND
            // carries only our own loop current (rowB is isolated).
            if (nodeHasUserBridge(rowA) || nodeHasUserBridge(rowB)) {
                finishCheck(GUIDE_CHECK_SKIPPED, "check skipped (rows in use)", "skip");
                break;
            }
            // ...and only into rows, never into a supply. Deliberately AFTER
            // the rows-in-use test: an EXPANDED part's rail pin arrives here as
            // its own bridged footprint row, which is already "in use", and
            // that message stays the one a user sees for it.
            if (nodeIsDrivenSource(rowA) || nodeIsDrivenSource(rowB)) {
                int bad = nodeIsDrivenSource(rowA) ? rowA : rowB;
                finishCheck(GUIDE_CHECK_SKIPPED,
                            ckHint("can't stimulate a supply node - a rail/GND/DAC "
                                   "drives its own current"),
                            "supply@%d", bad);
                break;
            }
            if (nodeHasUserBridge(DAC0) || nodeHasUserBridge(ISENSE_PLUS) ||
                nodeHasUserBridge(ISENSE_MINUS)) {
                finishCheck(GUIDE_CHECK_SKIPPED,
                            "check skipped (DAC0/ISENSE in use)", "skip");
                break;
            }
            // CONTINUITY BANDS ARE OHMS NOW (invest-measurement.md §2.3) -
            // the check measures resistance, so an author's min/max name
            // ohms, not milliamps. That is a silent semantic flip for any
            // file still carrying the old mA numbers, so TWO guards catch
            // them; the shipped projects had theirs stripped in the same
            // wave, and provisioning refreshes those files on a version bump.
            if (ck.check == GuideCheck::CONTINUITY) {
                if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
                    const PartDefinition& p = globalState.parts.parts[st.partIdx];
                    ParsedPartValue pv = parsePartValue(p.value, p.typeStr);
                    if (pv.kind == PartValueKind::OHMS) ck.rNom = pv.v;
                    ck.tolAuthorPct = p.tol;
                }
                bool authored = (st.max > st.min);
                bool legacy = false;
                if (authored) {
                    if (ck.rNom > 0.0f) {
                        // GUARD 1 (value parses): explicit bounds that do not
                        // BRACKET the nominal value are not ohms. 555 R3's old
                        // "5.0-15.0" mA would read as 5-15 ohms and fail a real
                        // 330 - and a max<5 test alone would miss it.
                        legacy = !(st.min <= ck.rNom && ck.rNom <= st.max);
                    } else {
                        // GUARD 2 (value-less part): no real resistor band
                        // lives below 5 ohms, while every old mA band does.
                        legacy = (st.max < 5.0f);
                    }
                }
                if (authored && !legacy) {
                    ck.bandLo = st.min;
                    ck.bandHi = st.max;
                    ck.bandFromAuthor = true;
                } else {
                    if (legacy) {
                        Serial.printf("  min/max look like legacy mA (%.3g-%.3g) - "
                                      "ignoring, using the value-derived ohm band\n\r",
                                      (double)st.min, (double)st.max);
                    }
                    if (ck.rNom > 0.0f) {
                        guideResistorBand(ck.rNom, ck.tolAuthorPct, &ck.bandLo, &ck.bandHi);
                    } else {
                        // §2.4: no value, no usable bounds (a jumper wire) -
                        // "something conducts" is the only honest verdict.
                        ck.bandLo = 0.0f;
                        ck.bandHi = 100000.0f;
                    }
                }
                // §1.5's measurable ceiling. At 5 V the loop still delivers
                // ~25 uA (one useful multiple of the shunt's 5 uA LSB) at
                // ~200k; past 470k there is nothing to measure, so say so
                // instead of guessing. This replaces the old 0.15 mA floor.
                if (ck.rNom > 470000.0f) {
                    finishCheck(GUIDE_CHECK_UNSUPPORTED,
                                "resistance too large to measure - placed unverified",
                                "toohighR");
                    break;
                }
                if (!chainBegin(rowA, rowB, guideStimulusVolts(ck.rNom))) {
                    finishCheck(GUIDE_CHECK_FAIL, "stimulus chain routing failed", "setup");
                }
            } else { // VF - min/max stay VOLTS, untouched by the ohm flip
                ck.bandLo = (st.max > st.min) ? st.min : 0.5f;
                ck.bandHi = (st.max > st.min) ? st.max : 2.9f;
                if (!chainBegin(rowA, rowB, 3.0f)) { // keep stimulus <= 3.3 V
                    finishCheck(GUIDE_CHECK_FAIL, "stimulus chain routing failed", "setup");
                }
            }
            break;
        }

        case GuideCheck::VOLTAGE: {
            if (st.target < 1) {
                finishCheck(GUIDE_CHECK_UNSUPPORTED, "voltage needs target:", "norows");
                break;
            }
            // Deliberate deviation from §5.1's "reuse nodeVoltage[] when
            // < 250 ms old": the scan smooths through an alpha-0.3 EMA and
            // is input-paused exactly while the guide is being driven, so a
            // timestamp-fresh sample can be one EMA step from a stale seed
            // - the bench served 0.77V for a row a DAC held at 2.5V.
            // Timestamp-fresh is not value-fresh; a one-shot tap costs
            // <=11ms and reads the row NOW. Always tap.
            ck.rows[ck.numRows++] = st.target;
            break;
        }

        case GuideCheck::OSCILLATES: {
            if (st.target < 1) {
                finishCheck(GUIDE_CHECK_UNSUPPORTED, "oscillates needs target:", "norows");
                break;
            }
            ck.oscTarget = st.target;
            ck.oscWindowMs = (ck.timeoutMs > 500) ? ck.timeoutMs : 500;
            // window IS the schedule, plus room for the safety tap in front
            ck.timeoutMs = ck.oscWindowMs + 1000 + kOscPreTapBudgetMs;
            // The GPIO route is decided in the POLL now, behind the two gates
            // documented at oscTryGpioRoute: a deterministic supply-ceiling
            // test and one high-Z sample of the target. Both refusals land on
            // the tap fallback below, which is where the poll takes us if
            // either says no (or if no GPIO is free at all).
            ck.oscPreTapPending = true;
            ck.oscWindowStartMs = millis();
            ck.phase = CkPhase::TAP_REQUEST;
            break;
        }

        case GuideCheck::I2C_ACK: {
            // ================= THE I2C CHECK'S BRIDGE CONTRACT =================
            // (rewritten in the H1 wave; the old comment here claimed the
            // opposite of what the code did, twice over.)
            //
            // WHAT IT ROUTES. n1 -> RP_GPIO_26 (SDA) and n2 -> RP_GPIO_27 (SCL),
            // as ORDINARY EPHEMERAL LEGS through addLeg()/checkTeardown() - the
            // same funnel every other check uses. Those two nodes are the ones
            // Wire1 is moved onto below (RP pins 26/27), and they are the SAME
            // NODES as RP_GPIO_7/RP_GPIO_8 (137/138 - JumperlessDefines.h aliases
            // both names to one node), which is why a project that already wired
            // its device to RP_GPIO_7/8 needs nothing added.
            //
            // WHAT IT BORROWS. If the pair is ALREADY a bridge (the shipped
            // i2cscrn and eeprom projects commit exactly 8<->137 / 7<->138 at
            // their place step), addEphemeralConnection returns true WITHOUT
            // tracking anything (States.cpp:801) and removeEphemeralConnection
            // then finds no ephemeral record and removes nothing
            // (States.cpp:875). So a committed bridge is borrowed for the scan
            // and survives it, untouched, by construction.
            //
            // WHAT IT RESTORES. Only what it actually added. chainLive is set
            // BEFORE the adds so a half-built pair still tears down.
            //
            // WHAT IT DOES NOT DO. It does not touch GPIO pull configuration.
            // A real bus needs pull-ups: either the project's own
            // `config: gpio: pulls:` (which is how i2cscrn/eeprom get RP 26/27
            // pulled up, re-asserted by every refresh's setGPIO pass) or
            // physical resistors. An author wiring an arbitrary n1/n2 pair owns
            // that the same way they own the device's power.
            //
            // WHY i2cScan IS CALLED WITH sdaRow/sclRow = -1. It is a bus
            // scanner here and nothing else: -1 rows take its "no bridges of my
            // own" branch, and leaveConnections=1 skips its removal tail as
            // well. It used to be called with the real rows and
            // leaveConnections=0, which made it delete the project's OWN
            // committed SDA/SCL bridges (and markDirty, so the run file was
            // rewritten without them) every single time this check ran - while
            // its add branch was unreachable for external scans, so it had
            // never added them in the first place. Bridge lifetime belongs to
            // the check, not to an app helper.
            //
            // Still the one documented BLOCKING check (~1-3 s including
            // i2cScan's own OLED/LED UI).
            // ===================================================================
            if (st.n1 < 1 || st.n2 < 1) {
                finishCheck(GUIDE_CHECK_UNSUPPORTED, "i2c needs n1: (SDA) + n2: (SCL)", "norows");
                break;
            }
            {
                const bool haveSda = globalState.hasConnection(st.n1, RP_GPIO_26);
                const bool haveScl = globalState.hasConnection(st.n2, RP_GPIO_27);
                ck.chainLive = true;   // BEFORE the adds: teardown always runs
                // Short-circuit is deliberate and safe: if leg 0 fails, leg 1
                // is never attempted and its chainAdded[] stays false, so
                // teardown removes exactly the one that landed. checkTeardown
                // clears chainAdded[] wholesale afterwards, so a later check
                // cannot inherit either slot.
                if (!addLeg(0, st.n1, RP_GPIO_26) ||
                    !addLeg(1, st.n2, RP_GPIO_27)) {
                    finishCheck(GUIDE_CHECK_FAIL,
                                ckHint("could not route n1/n2 to the I2C pins"),
                                "setup");
                    break;
                }
                if (!haveSda || !haveScl) {
                    refreshLocalConnections(0, 0, 0);
                    waitCore2();
                    // addLeg() returning true means the pair entered STATE, not
                    // that the router placed it - chainBegin makes the same
                    // distinction right after its own refresh. Without this, a
                    // router refusal reaches the user as "no I2C ack - is the
                    // device powered?" and sends them hunting a power fault on
                    // a bus that was never connected. routeRefused() reads
                    // unconnectablePaths[], which is rebuilt every routing pass
                    // and is only meaningful immediately after the refresh -
                    // hence right here, not later.
                    if (routeRefused(st.n1, RP_GPIO_26) ||
                        routeRefused(st.n2, RP_GPIO_27)) {
                        finishCheck(GUIDE_CHECK_FAIL,
                                    ckHint("the router could not reach the I2C pins - "
                                           "the bus was never connected"),
                                    "refused");
                        break;
                    }
                    Serial.printf("  i2c: routed %s%s%s to RP 26/27 for the scan "
                                  "(removed again afterwards)\n\r",
                                  haveSda ? "" : "SDA",
                                  (!haveSda && !haveScl) ? " + " : "",
                                  haveScl ? "" : "SCL");
                }
            }
            int nDevices = i2cScan(/*sdaRow=*/-1, /*sclRow=*/-1, 26, 27,
                                   /*leaveConnections=*/1, /*internalScan=*/0);
            // A bus whose SDA cannot rise ACKs EVERYTHING: the master reads its
            // own low as an ack from every address in turn, so the scan comes
            // back with ~126 "devices" on a bus that has none. Passing on that
            // is the worst outcome available here - it certifies wiring that
            // does not work, and it is the DEFAULT bench case, because the
            // RP2350's internal pull-up (~50 k) cannot pull a routed
            // breadboard net up inside a 100 kHz bit time. Real modules bring
            // their own ~4.7 k and work fine; a bare header does not.
            // Eight is well above any breadboard bus and far below a flood.
            if (nDevices > 8) {
                finishCheck(GUIDE_CHECK_FAIL,
                            ckHint("every address answered - SDA is stuck low "
                                   "(no pull-up on the bus?)"),
                            "%dghost", nDevices);
                break;
            }
            bool pass = (nDevices > 0);
            finishCheck(pass ? GUIDE_CHECK_PASS : GUIDE_CHECK_FAIL,
                        pass ? nullptr
                             : "no I2C ack - is the device powered? (i2c is a powered-only check)",
                        "%ddev", nDevices);
            break;
        }

        case GuideCheck::RAIL_SANE: {
            if (st.target >= 1) {
                // DOCUMENTED ASSUMPTION: an explicit `target:` on a
                // rail_sane step is checked as VCC-class - "within 0.2 V of
                // the top rail". A step is a single row here, and the format
                // has no way to say which class that row is, so one of the
                // two bands has to be the default; VCC is the useful one
                // (a GND row is what collectClassRows() finds from the
                // parts' own pin classes, and a "is this row at 0 V" check
                // is what `voltage` with min/max exists for). Authors who
                // want the GND band write `check: voltage` with
                // min: -0.15 / max: 0.15 instead. Mirrored in the States.h
                // guide: format comment.
                //
                // One exception, and it is not a second default: if the target
                // IS the bottom rail hole row, compare it to the BOTTOM rail.
                // "VCC-class" was always shorthand for "the rail that feeds
                // it", and naming BOTTOM_RAIL says which one that is.
                ck.rows[ck.numRows] = st.target;
                ck.rowClass[ck.numRows] = (st.target == BOTTOM_RAIL)
                                              ? CK_ROWCLASS_BOT : CK_ROWCLASS_TOP;
                ck.numRows++;
            } else {
                collectClassRows();
            }
            if (ck.numRows == 0) {
                // Nothing class-tagged to verify - an honest no-op pass, so
                // auto-synthesized power_on steps in partless projects flow.
                finishCheck(GUIDE_CHECK_PASS, "no power/gnd-class pins to verify", "norows");
                break;
            }
            // Raise the floor the way presence and continuity/vf do, now that
            // numRows is final. rail_sane had been living on the bare 1500 ms
            // default, which was fine while it took ONE reference tap; a
            // mixed-supply project takes TWO, and at the file's own ~250 ms per
            // tap a four-row build needs ~1600 ms and would have started
            // intermittently reporting `timeout` on exactly the project shape
            // the bottom-rail fix exists to serve. 400 ms of headroom covers
            // the 100 ms RAIL_SETTLE and the poll overhead around it. A floor,
            // not an override - an author's longer timeout: still wins.
            {
                uint32_t taps = (uint32_t)ck.numRows
                              + (railSaneNeedsTopRef() ? 1u : 0u)
                              + (railSaneNeedsBotRef() ? 1u : 0u);
                uint32_t need = 400 + 250u * taps;
                if (ck.timeoutMs < need) ck.timeoutMs = need;
            }
            if (ck.step->type == GuideStepType::POWER_ON && !ck.powerApplied) {
                // The stimulus of this check IS the power application - the
                // step's COMMIT applies it for real only after a pass. save=0
                // transient; teardown returns the rails to state truth (0 V).
                if (!ck.script->hasPower) {
                    finishCheck(GUIDE_CHECK_UNSUPPORTED,
                                "project has no power: section - nothing to apply", "nopower");
                    break;
                }
                setTopRail(ck.script->topRail, 0, 0);
                setBotRail(ck.script->bottomRail, 0, 0);
                setDac0voltage(ck.script->dac0, 0, 0);
                setDac1voltage(ck.script->dac1, 0, 0);
                ck.railTransient = true;
                ck.stimAppliedMs = millis();
                ck.phase = CkPhase::RAIL_SETTLE; // §5.2: wait 100 ms
            } else if (railSaneNeedsRef()) {
                // Rails already live: skip the settle, go straight to the
                // reference tap (§3 step 1).
                ck.phase = CkPhase::RAIL_REF;
            }
            // else: GND-class rows only - no reference tap is owed.
            break;
        }

        default:
            // NONE never reaches here (STEP_VERIFY short-circuits it).
            finishCheck(GUIDE_CHECK_PASS, nullptr, "pass");
            break;
    }
}

// ---------------------------------------------------------------------------
// Poll
// ---------------------------------------------------------------------------

// The rail target a VCC-class row is compared against: the transient values
// while this check applied them, state truth otherwise. §3 keeps this only as
// the SETPOINT the measured rail is gated against - rows are compared to the
// measurement, not to this.
static float railSaneTopTarget(void) {
    return ck.railTransient ? ck.script->topRail : globalState.power.topRail;
}

// The same for the BOTTOM rail. The power_on transient already APPLIES
// script->bottomRail (see the setBotRail call in the RAIL_SANE begin case) -
// it just had nothing that ever measured or referenced it.
static float railSaneBotTarget(void) {
    return ck.railTransient ? ck.script->bottomRail : globalState.power.bottomRail;
}

// The reference measurement and the channel a row of this class is compared on.
static float railSaneRefFor(uint8_t rowClass) {
    return (rowClass == CK_ROWCLASS_BOT) ? ck.railMeasuredBot : ck.railMeasured;
}

static int railSaneRefAdcFor(uint8_t rowClass) {
    return (rowClass == CK_ROWCLASS_BOT) ? ck.railAdcBot : ck.railAdc;
}

static float railSaneSetpointFor(uint8_t rowClass) {
    return (rowClass == CK_ROWCLASS_BOT) ? railSaneBotTarget() : railSaneTopTarget();
}

// ---------------------------------------------------------------------------
// Measurement helpers (continuity / vf)
// ---------------------------------------------------------------------------

// One FRESH shunt sample, on a Peripherals-poll tick we have not counted yet.
// The 50 ms poll (CURRENT_SENSE_POLL_INTERVAL_MS) re-asks the chip from
// serviceInner, which the guide loop pumps between check polls; we only watch
// the stamp. Same pattern the old current_mA averaging used - the field read
// is the new part. That 50 ms is why the §1.4 sample counts cost ~600 ms and
// why this check raises its own timeout floor to 1400 ms.
static bool inaAccumulateFresh(void) {
    if (!currentSenseState.active) return false;
    if (currentSenseState.lastUpdatedMs == ck.inaLastMs) return false;
    if (currentSenseState.lastUpdatedMs <= ck.stimAppliedMs) return false;
    ck.inaLastMs = currentSenseState.lastUpdatedMs;
    ck.inaSum += inaShuntCurrent_mA();
    ck.inaCount++;
    return true;
}

// "105uA" / "4.20mA" - three significant figures either side of 1 mA.
static void formatCurrent(float mA, char* out, size_t n) {
    float a = fabsf(mA);
    if (a < 1.0f) snprintf(out, n, "%.0fuA", (double)(mA * 1000.0f));
    else          snprintf(out, n, "%.2fmA", (double)mA);
}

// The signed scan-estimated current through one live path, when the net
// voltage scan happens to know it. Used ONLY as a printed diagnostic.
static bool scanCurrentFor(int n1, int n2, float* out) {
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numPaths && i < MAX_BRIDGES; i++) {
        int a = c.paths[i].node1, b = c.paths[i].node2;
        if (!((a == n1 && b == n2) || (a == n2 && b == n1))) continue;
        if (!pathCurrentKnown(i)) return false;
        *out = pathCurrentSigned_mA(i);
        return true;
    }
    return false;
}

// invest-measurement.md §1.7's two cross-checks. NEITHER can fail a part -
// they print, and that is all.
static void senseCrosschecks(void) {
    const float i0 = fabsf(ck.iPart_mA);

    // (1) INA1 (0x41), whose 2 ohm R57 sits in DAC0's OUTPUT path, sees the
    // total current DAC0 sources. In this topology that should be the part
    // current; a persistent excess means something outside the part path is
    // drawing from the stimulus. Read-only - INA1's configuration is as
    // untouchable as INA0's, and nothing else consumes its conversion flag in
    // steady state.
    //
    // Gated on there BEING a part current to compare against: on an open row
    // i0 is ~0 and any board-internal draw on DAC0's rail trips the test, so
    // an ungated version printed "current is leaking" under every honest
    // `open` verdict (caught on the bench, 2.17 mA vs 0.00 mA on empty rows).
    // "Nothing flows through the part" is what val=open already says.
    float i1 = fabsf(INA1.getCurrent_mA());
    if (i0 >= 0.5f && INA1.getLastError() == 0 && (i1 - i0) > (0.1f * i0 + 0.2f)) {
        Serial.printf("  note: INA1 sees %.2f mA leaving DAC0 but INA0 measured "
                      "%.2f mA through the part - current is leaking outside the "
                      "part path\n\r", (double)i1, (double)i0);
    }

    // (2) The net scan's own Ohm's-law estimate for the rowB leg, LOG ONLY and
    // only where it has the resolution to mean anything (>= 2 mA).
    //
    // TASK #32 IS OPEN (CodeDocs/DEV_MERGE_HANDOFF.md): scan-vs-INA agreement
    // was re-proven at exactly ONE operating point after the pair-tap
    // hardening, +/-6 % per-route structure remains, and the regression window
    // (rows 34-79) was never bisected. The handoff says to leave #32 open
    // until Kevin agrees the number holds - so this disagreement NEVER fails a
    // part, and the 25 % threshold is a conversation starter, not a spec.
    float iScan = 0;
    if (i0 >= 2.0f && scanCurrentFor(ck.chainRowB, ISENSE_PLUS, &iScan)) {
        float d = fabsf(fabsf(iScan) - i0);
        if (d > 0.25f * i0) {
            Serial.printf("  note: net-scan estimates %.2f mA on the rowB leg vs "
                          "INA0's %.2f mA (>25%% apart; task #32 is open - "
                          "diagnostic only)\n\r", (double)iScan, (double)i0);
        }
    }
}

// R = (V_A - V_B) / I_part, and the honest verdicts around it (§4).
static void evaluateContinuity(void) {
    const GuideStep& st = *ck.step;
    const char* partName = "R";
    if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
        partName = globalState.parts.parts[st.partIdx].name;
    }
    char iStr[16];
    formatCurrent(ck.iPart_mA, iStr, sizeof(iStr));
    char loStr[12], hiStr[12];
    formatOhms(ck.bandLo, loStr, sizeof(loStr));
    formatOhms(ck.bandHi, hiStr, sizeof(hiStr));

    // §1.9's last resort: no voltage at all. Judge the current against a WIDE
    // plausibility band derived from R_nom assuming 0-600 ohm of unknown path,
    // and mark the value `~` so it can never be read as a measured resistance.
    if (!ck.haveVsense) {
        // The only caller that reaches here already gated on rNom > 0 (the
        // degrade at the TAPS -2 site), but the divide is right here and a
        // future caller should not have to know that.
        if (!(ck.rNom > 0.0f)) {
            setDetail("%s: R not measured and no nominal value to judge %s against",
                      partName, iStr);
            finishCheck(GUIDE_CHECK_FAIL,
                        "no sense route to the rows, and no value: to judge the "
                        "current against",
                        "noroute@%d",
                        (ck.tapFailNode > 0) ? ck.tapFailNode : ck.chainRowA);
            return;
        }
        float iLo = ck.stimVolts / (1.4f * ck.rNom + 600.0f) * 1000.0f;
        float iHi = 1.4f * ck.stimVolts / ck.rNom * 1000.0f;
        bool ok = (fabsf(ck.iPart_mA) >= iLo && fabsf(ck.iPart_mA) <= iHi);
        char nomStr[12];
        formatOhms(ck.rNom, nomStr, sizeof(nomStr));
        setDetail("%s: R NOT measured (no sense route) - %s, plausible band "
                  "%.3g-%.3g mA for %s", partName, iStr, (double)iLo, (double)iHi, nomStr);
        char hint[96];
        snprintf(hint, sizeof(hint),
                 "R not measured (no sense route) - current %s %s for %s",
                 iStr, ok ? "is plausible" : "is NOT plausible", nomStr);
        finishCheck(ok ? GUIDE_CHECK_PASS : GUIDE_CHECK_FAIL, hint, "~%s", iStr);
        return;
    }

    // Two-point: the offset operating point's own delta comes off, so a
    // constant additive error in either channel (and the INA's register
    // zero, already out via I_part) cancels exactly. A resistor is ohmic,
    // so the slope between the two points IS its resistance.
    const float vOfs = ck.haveVofs ? (ck.vOfsA - ck.vOfsB) : 0.0f;
    const float vPart = (ck.vSenseA - ck.vSenseB) - vOfs;
    // The measurable floor: one useful multiple of the shunt's 5 uA LSB. Below
    // it the division is noise over noise, and "open" is the true answer for
    // every part a guide step actually places.
    if (fabsf(ck.iPart_mA) < 0.025f) {
        // Don't quote a band on a step that does not enforce one - naming
        // "band 8.00k-12.0k" next to an open reads as "your resistor is the
        // wrong value" when the actual message is "there is no resistor here".
        if (st.bandAdvisory) {
            setDetail("%s: open (%s at %.1fV across the rows)", partName, iStr,
                      (double)ck.stimVolts);
        } else {
            setDetail("%s: open (%s at %.1fV across the rows; band %s-%s)",
                      partName, iStr, (double)ck.stimVolts, loStr, hiStr);
        }
        finishCheck(GUIDE_CHECK_FAIL,
                    "no conduction - part missing, or a leg not seated?", "open");
        return;
    }
    const float rMeas = vPart / (ck.iPart_mA / 1000.0f);
    // Hand the reading to the part BEFORE any verdict: a companion script wants
    // what is on the board whether or not the author cared about the band, and
    // whether or not this particular resistor turned out to be the wrong one.
    if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
        globalState.parts.parts[st.partIdx].measuredOhms = rMeas;
    }
    char rStr[12];
    formatOhms(rMeas, rStr, sizeof(rStr));

    // `enforce: false` - the measurement IS the step. Only the two PLACEMENT
    // verdicts still fail: open (already returned above - a leg not seated) and
    // short (both legs the same side of the ravine). The value is reported and
    // never judged, so a user who reaches for a 22k when the file says 47k
    // builds a working circuit and the script computes from what they used.
    if (st.bandAdvisory) {
        if (rMeas < 5.0f) {
            setDetail("%s: %s - reads as a short (%s @ %.1fV)", partName, rStr,
                      iStr, (double)ck.stimVolts);
            finishCheck(GUIDE_CHECK_FAIL,
                        "reads as a short - are the legs bridging the ravine?", "short");
            return;
        }
        if (ck.rNom > 0.0f) {
            char nomStr[12];
            formatOhms(ck.rNom, nomStr, sizeof(nomStr));
            setDetail("%s: %s measured (value: says %s - not enforced, %s @ %.1fV)",
                      partName, rStr, nomStr, iStr, (double)ck.stimVolts);
        } else {
            setDetail("%s: %s measured (%s @ %.1fV)", partName, rStr, iStr,
                      (double)ck.stimVolts);
        }
        finishCheck(GUIDE_CHECK_PASS, nullptr, "%s", rStr);
        return;
    }

    const bool pass = (rMeas >= ck.bandLo && rMeas <= ck.bandHi);
    setDetail("%s: %s (band %s-%s, %s @ %.1fV)", partName, rStr, loStr, hiStr,
              iStr, (double)ck.stimVolts);
    if (pass) {
        finishCheck(GUIDE_CHECK_PASS, nullptr, "%s", rStr);
        return;
    }
    // A dead short reads as a short, not as "wrong value" - the two need
    // different things done about them.
    if (rMeas < 5.0f) {
        finishCheck(GUIDE_CHECK_FAIL,
                    "reads as a short - are the legs bridging the ravine?", "short");
        return;
    }
    char hint[96];
    if (ck.rNom > 0.0f && !ck.bandFromAuthor) {
        char nomStr[12];
        formatOhms(ck.rNom, nomStr, sizeof(nomStr));
        const float tolMeas = (ck.rNom <= 10000.0f) ? 5.0f
                            : (ck.rNom <= 100000.0f) ? 10.0f : 25.0f;
        const float tolTotal = ((ck.tolAuthorPct > 0) ? (float)ck.tolAuthorPct : 15.0f)
                               + tolMeas;
        snprintf(hint, sizeof(hint), "reads %s, expected %s +/-%.0f%% - wrong part?",
                 rStr, nomStr, (double)tolTotal);
    } else {
        snprintf(hint, sizeof(hint), "reads %s, expected %s-%s - wrong part?",
                 rStr, loStr, hiStr);
    }
    finishCheck(GUIDE_CHECK_FAIL, hint, "%s", rStr);
}

// Vf = V_A - V_B, now reported WITH the current it was measured at (§1.10).
static void evaluateVf(void) {
    const GuideStep& st = *ck.step;
    const char* partName = "LED";
    if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
        partName = globalState.parts.parts[st.partIdx].name;
    }
    // Same two-point correction as continuity: at 0 V stimulus a diode is
    // off, so the offset delta is the two channels' mismatch (plus the
    // few mV the DAC0 zero-offset drops across whatever is there) and
    // taking it off is exactly right.
    const float vf = (ck.vSenseA - ck.vSenseB) -
                     (ck.haveVofs ? (ck.vOfsA - ck.vOfsB) : 0.0f);
    char iStr[16];
    formatCurrent(ck.iPart_mA, iStr, sizeof(iStr));
    setDetail("%s vf %.2fV @ %s (band %.2f-%.2fV)", partName, (double)vf, iStr,
              (double)ck.bandLo, (double)ck.bandHi);
    const bool conducting = (fabsf(ck.iPart_mA) > 0.5f);
    const bool inBand = (vf >= ck.bandLo && vf <= ck.bandHi);
    // `enforce: false` waives the BAND only. "No current" still fails: on a
    // diode that verdict means missing-or-backwards, which is a placement
    // error, and it is the one thing this check exists to catch.
    if (conducting && st.bandAdvisory) {
        setDetail("%s vf %.2fV @ %s (measured, not enforced)", partName,
                  (double)vf, iStr);
        finishCheck(GUIDE_CHECK_PASS, nullptr, "%.2fV", (double)vf);
    } else if (conducting && inBand) {
        finishCheck(GUIDE_CHECK_PASS, nullptr, "%.2fV", (double)vf);
    } else if (!conducting) {
        // Full stimulus across the gap, no current: missing and reversed are
        // electrically identical from here - §5.2's honest "flip it?".
        finishCheck(GUIDE_CHECK_FAIL,
                    "no current - LED missing or reversed (flip it?)",
                    "%.2fV", (double)vf);
    } else {
        finishCheck(GUIDE_CHECK_FAIL, "Vf outside the expected band",
                    "%.2fV", (double)vf);
    }
}

int guideCheckPoll(char* valOut, size_t valLen) {
    if (!ck.active) {
        snprintf(valOut, valLen, "idle");
        return GUIDE_CHECK_UNSUPPORTED;
    }
    if (ck.result != GUIDE_CHECK_RUNNING) {
        snprintf(valOut, valLen, "%s", ck.val);
        return ck.result;
    }

    // INA watchdog: a live stimulus pushing > 50 mA means something is very
    // wrong (a short, a mis-seated part) - kill it NOW, fail the check.
    if (ck.chainLive && currentSenseState.active &&
        fabsf(currentSenseState.current_mA) > 50.0f) {
        float i = currentSenseState.current_mA;
        finishCheck(GUIDE_CHECK_FAIL,
                    "overcurrent - stimulus aborted (short or mis-seated part?)",
                    "%.0fmA", i);
        snprintf(valOut, valLen, "%s", ck.val);
        return ck.result;
    }

    // ...AND THE HALF THE GROUND-SIDE MOVE MADE INVISIBLE. INA0 now sits in the
    // RETURN path, so it counts what comes back through rowB and the shunt.
    // Current that leaves DAC0 into rowA and reaches ground WITHOUT passing
    // rowB - a mis-seated leg landing in a grounded row is the realistic case -
    // never crosses the shunt and the watchdog above cannot see it. The old
    // top-side chain saw every milliamp DAC0 sourced; this one does not, and
    // that narrowing arrived silently with the topology change.
    //
    // INA1 (0x41) closes it for free: its 2 ohm R57 is in DAC0's OUTPUT path,
    // so it measures exactly the current the other one misses.
    //
    // RATE-LIMITED ON PURPOSE. This poll runs at multi-kHz; an I2C transaction
    // per pass would cost more than the gap it closes (and would fight the
    // Peripherals poll for the bus). Once per CURRENT_SENSE_POLL_INTERVAL_MS is
    // the same cadence the chip is converting at, so it cannot miss a fault for
    // longer than the primary watchdog's own sample period.
    // The 50 here MIRRORS Peripherals.cpp's file-static
    // CURRENT_SENSE_POLL_INTERVAL_MS; it is deliberately a literal and must
    // not be "fixed" into a reference to that constant, which has internal
    // linkage and would turn this into a link error. If the poll interval ever
    // moves, move this with it (and the two comments that quote it above).
    if (ck.chainLive && (millis() - ck.ina1LastCheckMs) >= 50) {
        ck.ina1LastCheckMs = millis();
        float i1 = INA1.getCurrent_mA();
        if (INA1.getLastError() == 0 && fabsf(i1) > 50.0f) {
            finishCheck(GUIDE_CHECK_FAIL,
                        "overcurrent leaving DAC0 - stimulus aborted (a leg in a "
                        "grounded row bypasses the shunt)",
                        "src%.0fmA", (double)i1);
            snprintf(valOut, valLen, "%s", ck.val);
            return ck.result;
        }
    }

    // Overall timeout (oscillates sized its own window into this).
    if (millis() - ck.startMs > ck.timeoutMs) {
        finishCheck(GUIDE_CHECK_FAIL, "check timed out", "timeout");
        snprintf(valOut, valLen, "%s", ck.val);
        return ck.result;
    }

    switch (ck.check) {

        case GuideCheck::PRESENCE: {
            if (ck.phase == CkPhase::PRES_CHARGE) {
                if (millis() - ck.stimAppliedMs >= 40) ck.phase = CkPhase::PRES_UNROUTE;
                break;
            }
            if (ck.phase == CkPhase::PRES_UNROUTE) {
                // Strand the charge: remove the charge leg(s) while the DAC
                // is STILL driving - dropping it first would discharge every
                // row through the still-closed chain and erase the signal.
                // Deliberate exception to stimulus-off-during-refresh: the
                // only routes changing are our own legs on rows verified
                // isolated, and the chain head stays put. The grounded
                // variant keeps rowB->GND in place - that return is what
                // makes a cap's retained charge ground-referenced.
                String err;
                if (ck.presGrounded) {
                    if (ck.chainAdded[1]) {
                        globalState.removeEphemeralConnection(ISENSE_MINUS, ck.chainRowA,
                                                              err, false, 0);
                        ck.chainAdded[1] = false;
                    }
                } else {
                    for (int i = 0; i < ck.presLegsAdded; i++) {
                        globalState.removeEphemeralConnection(ISENSE_MINUS, ck.rows[i],
                                                              err, false, 0);
                    }
                }
                ck.presStranded = true;
                refreshLocalConnections(0, 0, 0);
                waitCore2();
                setDac0voltage(globalState.power.dac0, 0, 0); // stimulus off
                ck.rowIdx = 0;
                ck.phase = CkPhase::TAP_REQUEST;
                break;
            }
            // Retention verdict per row. Under the tap's 1M dwell load,
            // tau = C x 1M sorts the cases: a part-scale cap (>= ~10nF,
            // tau >= 10ms) HOLDS through the dwell and reads stable-high; a
            // bare row (~0.1-1nF, tau ~ 0.1-1ms) drains - caught mid-drain
            // it drift-rejects (-1, "decay"), fully drained it reads a
            // stable near-0 ("nocharge"). Both mean no part-scale retention.
            // (Bench-calibrated: the first mapping called drift "present"
            // and a bare row passed - a stranded bare row arrives at the
            // tap still charged via tiny off-leakage and visibly drains
            // ACROSS the dwell.)
            float v, d;
            int r = tapCycleNode(ck.rows[ck.rowIdx], &v, &d);
            if (r == 0) break;
            if (r == -2) {
                finishCheck(GUIDE_CHECK_FAIL, "no sense route to the row",
                            "noroute@%d", ck.rows[ck.rowIdx]);
                break;
            }
            if (r == -1) {
                finishCheck(GUIDE_CHECK_FAIL,
                            "charge drained at bare-row speed - is the part leg seated?",
                            "decay@%d", ck.rows[ck.rowIdx]);
                break;
            }
            if (v < 0.4f) {
                finishCheck(GUIDE_CHECK_FAIL,
                            "row held no charge - is the part leg seated?",
                            "nocharge@%d", ck.rows[ck.rowIdx]);
                break;
            }
            if (v < ck.presMinHold) {
                ck.presMinHold = v;
                ck.presMinHoldRow = ck.rows[ck.rowIdx];
            }
            ck.rowIdx++;
            if (ck.rowIdx >= ck.numRows) {
                finishCheck(GUIDE_CHECK_PASS, nullptr, "hold%.2fV@%d",
                            ck.presMinHold, ck.presMinHoldRow);
            }
            break;
        }

        case GuideCheck::CONTINUITY:
        case GuideCheck::VF: {
            // THE MEASUREMENT MACHINE (invest-measurement.md §1.4, with the
            // Option-1 fold). Stages, all polled:
            //
            //   OFS_SETTLE   chain live, DAC0 held at 0 V
            //   OFS_SAMPLE   4 fresh shunt samples -> I_ofs, plus the sense
            //                pair at that same zero point; |I_ofs| < 1 mA or
            //                the isense path is leaking
            //   (energize)   DAC0 -> V_stim, save=0
            //   STIM_SETTLE  60 ms
            //   STIM_SAMPLE  8 fresh shunt samples -> I_raw; I_part = raw-ofs
            //   V_READ       the two sense channels, one ring dwell
            //   compute      two-point: R = dV / I_part, Vf = dV, where both
            //                dV and I_part are differences between the
            //                energized and zero-stimulus operating points
            //
            // Current comes from INA0's SHUNT-VOLTAGE register, not its
            // current register: 10 uV/LSB across the 2 ohm R1 is 5 uA/LSB,
            // six times finer than the calibrated 30.5 uA/LSB current LSB
            // that turned the bench's 47k (70 uA = 2 counts) into val=0.03mA.
            // The check NEVER touches INA0's configuration - the Peripherals
            // poll owns the chip's cadence and its conversion flag.
            if (ck.phase == CkPhase::OFS_SETTLE) {
                if (millis() - ck.stimAppliedMs >= 30) {
                    ck.phase = CkPhase::OFS_SAMPLE;
                    ck.inaSum = 0;
                    ck.inaCount = 0;
                    ck.inaLastMs = currentSenseState.lastUpdatedMs;
                }
                break;
            }
            if (ck.phase == CkPhase::OFS_SAMPLE) {
                inaAccumulateFresh();
                if (ck.inaCount >= 4) {
                    ck.iOfs_mA = ck.inaSum / ck.inaCount;
                    // THE ZERO POINT IS A REAL OPERATING POINT, not just a
                    // tare. Bench measurement while building this: DAC0's
                    // "0 V" output actually sits at about -73 mV, and through
                    // a low-resistance part that is a genuine -0.40 mA - the
                    // part really is conducting it. So the offset sample is
                    // the FIRST of two operating points, and the sense pair
                    // gets read here too (RING mode only - the tap mode reads
                    // both nodes on one channel, which cancels the channel
                    // offset by itself). R and Vf then come out of the
                    // DIFFERENCE between the two points, which cancels the
                    // INA's register zero AND the two channels' offsets
                    // exactly, instead of subtracting a current that was
                    // never an artifact.
                    if (ck.senseMode == SenseMode::RING) {
                        float dA = 0, dB = 0;
                        ck.haveVofs = ringReadPair(ck.senseAdcA, ck.senseAdcB,
                                                   &ck.vOfsA, &ck.vOfsB, &dA, &dB);
                    }
                    // The leak gate survives, at the level that means what it
                    // says. The old top-side chain read 2.319 mA here (the
                    // CURR_SENSE- sink) and the ground-side one reads 0.000 mA
                    // with no part; the worst honest reading is the DAC0
                    // zero-offset through a dead short, ~0.4 mA. 1 mA sits
                    // clear of that and still catches the sink by 2x.
                    if (fabsf(ck.iOfs_mA) > 1.0f) {
                        setDetail("isense reads %.2f mA with the stimulus at 0 V "
                                  "(expected under 1)", (double)ck.iOfs_mA);
                        finishCheck(GUIDE_CHECK_FAIL,
                                    "isense path is leaking - something else is "
                                    "driving the sense chain",
                                    "leak%.2fmA", (double)ck.iOfs_mA);
                        break;
                    }
                    setDac0voltage(ck.stimVolts, 0, 0); // save=0 as always
                    ck.stimAppliedMs = millis();
                    ck.phase = CkPhase::STIM_SETTLE;
                }
                break;
            }
            if (ck.phase == CkPhase::STIM_SETTLE) {
                if (millis() - ck.stimAppliedMs >= 60) {
                    if (ck.reSettleToTaps) {
                        // A mid-check degrade restored the stimulus and is
                        // waiting out the DAC step before the taps sample
                        // voltage - I_part is already measured and stands.
                        ck.reSettleToTaps = false;
                        ck.phase = CkPhase::TAP_REQUEST;
                        break;
                    }
                    ck.phase = CkPhase::STIM_SAMPLE;
                    ck.inaSum = 0;
                    ck.inaCount = 0;
                    ck.inaLastMs = currentSenseState.lastUpdatedMs;
                }
                break;
            }
            if (ck.phase == CkPhase::STIM_SAMPLE) {
                inaAccumulateFresh();
                if (ck.inaCount >= 8) {
                    ck.iPart_mA = ck.inaSum / ck.inaCount - ck.iOfs_mA;
                    ck.phase = (ck.senseMode == SenseMode::RING)
                                   ? CkPhase::V_READ
                                   : CkPhase::TAP_REQUEST;
                }
                break;
            }

            // ---- the voltage half -------------------------------------
            if (ck.senseMode == SenseMode::RING) {
                // Option 1's sampler: the sense legs are already closed as
                // ordinary bridges, so this is a pure ring read - no
                // fastConnectPath, no tier search, nothing that can starve.
                float dA = 0, dB = 0;
                if (!ringReadPair(ck.senseAdcA, ck.senseAdcB,
                                  &ck.vSenseA, &ck.vSenseB, &dA, &dB)) {
                    // A non-fresh window is transient (a resync, a lapped
                    // dwell). Retry; the check timeout bounds the loop, and
                    // eight misses in a row means the ring is not serving us.
                    if (++ck.tapHardFails >= kTapHardFailLimit) {
                        Serial.println("  sense: ring would not serve a fresh "
                                       "window - falling back to one-shot taps");
                        dropSenseLegsToTaps(true);
                    }
                    break;
                }
                ck.tapHardFails = 0;
                // ROUTED-OR-NOT, measured. rowA is wired straight to DAC0, so
                // a sense leg that really landed MUST have followed the
                // stimulus up.
                if (ck.vSenseA < 0.25f && ck.stimVolts >= 1.0f) {
                    Serial.printf("  sense: rowA reads %.3f V under a %.1f V "
                                  "stimulus - the sense bridge is not on the row; "
                                  "falling back to one-shot taps\n\r",
                                  (double)ck.vSenseA, (double)ck.stimVolts);
                    dropSenseLegsToTaps(true);
                    break;
                }
                // rowB gets a gate too - but only where current makes one
                // POSSIBLE. This closes the blind spot the report discloses:
                // rowB sits just above the 2 ohm shunt, so ~0 V is its correct
                // reading and a channel whose bridge never landed reads the
                // same, which means a falsely-zero vB silently inflates R by
                // the whole return path.
                //
                // What separates the two cases is current. The return path
                // below the part measures ~130 ohm on this fabric (bench: 12d
                // read vB = 1.847 V at 13.98 mA), so at >= 2 mA a genuinely
                // routed rowB reads >= ~260 mV - two orders of magnitude clear
                // of the 20 mV floor below.
                //
                // THE THRESHOLD MUST BE CURRENT-CONDITIONAL, not fixed: at
                // 47k / 5 V the honest vB is only ~14 mV, so an unconditional
                // gate would false-trip exactly the high-resistance case the
                // 5 V escalation exists to serve. Below 2 mA the verdict is
                // `open` regardless of vB, so the residual window costs
                // nothing.
                if (fabsf(ck.iPart_mA) >= 2.0f && fabsf(ck.vSenseB) < 0.020f) {
                    Serial.printf("  sense: rowB reads %.4f V while %.2f mA "
                                  "flows through it - the sense bridge is not on "
                                  "the row; falling back to one-shot taps\n\r",
                                  (double)ck.vSenseB, (double)ck.iPart_mA);
                    dropSenseLegsToTaps(true);
                    break;
                }
                ck.haveVsense = true;
            } else if (ck.senseMode == SenseMode::TAPS) {
                float vA, vB;
                int r = tapCyclePair(ck.chainRowA, ck.chainRowB, &vA, &vB);
                if (r == 0) break;
                if (r == -2) {
                    // §1.9: the old continuity check needed NO voltage tap,
                    // so refusing here would make a check unrunnable where it
                    // used to run. Degrade to a current-only plausibility
                    // verdict instead - flagged with a leading `~` in val so
                    // nobody mistakes it for a measured resistance.
                    if (ck.check == GuideCheck::CONTINUITY && ck.rNom > 0.0f) {
                        ck.senseMode = SenseMode::CURRENT;
                        break;
                    }
                    finishCheck(GUIDE_CHECK_FAIL, "no sense route to the rows",
                                "noroute@%d",
                                (ck.tapFailNode > 0) ? ck.tapFailNode : ck.chainRowA);
                    break;
                }
                if (r == -1) {
                    finishCheck(GUIDE_CHECK_FAIL,
                                "no stable reading across the part - is it seated?",
                                "float");
                    break;
                }
                ck.vSenseA = vA;
                ck.vSenseB = vB;
                ck.haveVsense = true;
            }

            // ---- diagnostics (§1.7) - never a verdict, only a hint -----
            senseCrosschecks();

            // ---- the verdict -------------------------------------------
            if (ck.check == GuideCheck::CONTINUITY) {
                evaluateContinuity();
            } else {
                evaluateVf();
            }
            break;
        }

        case GuideCheck::VOLTAGE: {
            float v, d;
            int r = tapCycleNode(ck.rows[0], &v, &d);
            if (r == 0) break;
            if (r == -2) {
                finishCheck(GUIDE_CHECK_FAIL, "no sense route to the row", "noroute");
                break;
            }
            if (r == -1) {
                finishCheck(GUIDE_CHECK_FAIL, "row reads floating (undriven?)", "float");
                break;
            }
            const GuideStep& st = *ck.step;
            bool hasBand = (st.max > st.min);
            bool pass = !hasBand || (v >= st.min && v <= st.max);
            finishCheck(pass ? GUIDE_CHECK_PASS : GUIDE_CHECK_FAIL,
                        pass ? nullptr : "voltage outside the expected band",
                        "%.2fV", v);
            break;
        }

        case GuideCheck::OSCILLATES: {
            if (ck.oscPreTapPending) {
                // GATE 1: the supply ceiling, before any hardware moves.
                const float ceiling = oscSupplyCeiling();
                if (ceiling > kOscGpioMaxV) {
                    ck.oscPreTapPending = false;
                    ck.oscFallbackReason = 1;
                    Serial.printf("  osc: this project drives its rails at "
                                  "%.2f V - the frequency check drives a 3.3 V "
                                  "pin and will not connect to a node that can "
                                  "swing that high. Falling back to high-Z taps "
                                  "(frequency not measured).\n\r", (double)ceiling);
                    ck.oscWindowStartMs = millis();
                    ck.phase = CkPhase::TAP_REQUEST;
                    break;
                }
                // GATE 2: one high-Z sample of the target itself. A high
                // reading proves danger; a low one proves only that this
                // instant was low, which is why gate 1 exists.
                float v, d;
                int r = tapCycleNode(ck.oscTarget, &v, &d);
                if (r == 0) break;
                ck.oscPreTapPending = false;
                ck.tapHardFails = 0;
                if (r == 1 && fabsf(v) > kOscGpioMaxV) {
                    ck.oscFallbackReason = 2;
                    Serial.printf("  osc: target sits at %.2f V - the frequency "
                                  "check drives a 3.3 V pin and will not connect "
                                  "to it. Falling back to high-Z taps (frequency "
                                  "not measured).\n\r", (double)v);
                    ck.oscWindowStartMs = millis();
                    ck.phase = CkPhase::TAP_REQUEST;
                    break;
                }
                // r == -1 (drift-rejected: the node is MOVING, which is what
                // this check wants) or -2 (no sense route) leave the voltage
                // unknown - the ceiling already bounds it, so take the GPIO.
                if (oscTryGpioRoute(ck.oscTarget)) {
                    ck.oscSettleUntilMs = millis() + 20;
                    ck.phase = CkPhase::OSC_COUNT;
                    break;
                }
                // No GPIO free: the pre-existing tap fallback. Repeated taps
                // through the window; a tap sees a DC level (ok) or
                // drift-rejects (the scanner is DC-only - a moving signal
                // LOOKS floating to it). Both levels observed across ok taps =
                // oscillating; frequency unmeasurable this way - "osc".
                ck.oscWindowStartMs = millis();
                ck.phase = CkPhase::TAP_REQUEST;
                break;
            }
            if (ck.oscGpioIdx >= 0) {
                // GPIO edge counting. Sample once per poll; the guide loop
                // runs this at multiple kHz, good to a few hundred Hz - the
                // 555-blinker regime the matrix calls the money shot.
                if (millis() < ck.oscSettleUntilMs) break;
                bool level = gpio_get(gpioDef[ck.oscGpioIdx][0]);
                if (!ck.oscHaveLevel) {
                    ck.oscHaveLevel = true;
                    ck.oscLastLevel = level;
                    ck.oscWindowStartMs = millis();
                    break;
                }
                if (level != ck.oscLastLevel) ck.oscEdges++;
                ck.oscLastLevel = level;
                if (millis() - ck.oscWindowStartMs >= ck.oscWindowMs) {
                    float f = (float)ck.oscEdges / 2.0f /
                              ((float)ck.oscWindowMs / 1000.0f);
                    const GuideStep& st = *ck.step;
                    bool hasBand = (st.max > st.min);
                    bool pass = hasBand ? (f >= st.min && f <= st.max)
                                        : (ck.oscEdges >= 2);
                    finishCheck(pass ? GUIDE_CHECK_PASS : GUIDE_CHECK_FAIL,
                                pass ? nullptr
                                     : (ck.oscEdges == 0
                                            ? "no edges seen - not oscillating (or slower than the window)"
                                            : "frequency outside the expected band"),
                                "%.1fHz", f);
                }
                break;
            }
            // Tap fallback: no GPIO was free. Repeated taps through the
            // window; both levels seen = oscillating, frequency unknown.
            float v, d;
            int r = tapCycleNode(ck.oscTarget, &v, &d);
            if (r == 1) {
                ck.oscTapOks++;
                if (v < ck.oscVLo) ck.oscVLo = v;
                if (v > ck.oscVHi) ck.oscVHi = v;
            } else if (r == -1) {
                ck.oscTapFloats++; // a moving signal drift-rejects (DC-only scanner)
            } else if (r == -2) {
                finishCheck(GUIDE_CHECK_FAIL, "no sense route to the row", "noroute");
                break;
            }
            if (millis() - ck.oscWindowStartMs >= ck.oscWindowMs) {
                if (ck.oscTapOks >= 2 && (ck.oscVHi - ck.oscVLo) > 1.0f) {
                    // BOTH LEVELS SEEN. What that is worth depends entirely on
                    // what the author asked for, and this used to ignore the
                    // question: it returned PASS unconditionally, never reading
                    // st.min/st.max - the band is evaluated ONLY on the GPIO
                    // edge-counting path above. While the fallback was reached
                    // only when no GPIO happened to be free that was academic.
                    // The supply-ceiling gate made it the PRIMARY path for the
                    // one shipped project that uses this check, so a 555
                    // blinking at 500 Hz - or at 0.05 Hz - would have come back
                    // ok=1 against `min: 0.3, max: 30`.
                    //
                    // A guide that cannot measure something must say so; it may
                    // never call it good. So: an authored band that was never
                    // evaluated is SKIPPED, not PASS. Warn-class still advances
                    // the step (SKIPPED never "measured" anything, so GuidedFlow
                    // proceeds on the confirm already given) - the difference is
                    // that the step reads UNVERIFIED instead of CONFIRMED, and
                    // the hint says which.
                    //
                    // With no band the author only asked "is it oscillating?",
                    // and both levels across the window answers exactly that.
                    // That stays a PASS.
                    const GuideStep& stO = *ck.step;
                    const bool bandAuthored = (stO.max > stO.min);
                    const char* why =
                        (ck.oscFallbackReason == 1) ? "rails exceed the 3.3V GPIO domain"
                        : (ck.oscFallbackReason == 2) ? "target sits above the 3.3V GPIO domain"
                                                      : "no routable GPIO was free";
                    setDetail("osc: %s - saw both levels, span %.2fV; frequency "
                              "not measured", why, (double)(ck.oscVHi - ck.oscVLo));
                    if (bandAuthored) {
                        finishCheck(GUIDE_CHECK_SKIPPED,
                                    ckHint("oscillating, but the frequency could not be "
                                           "measured - band NOT checked"),
                                    "unmeasured");
                    } else {
                        finishCheck(GUIDE_CHECK_PASS,
                                    ckHint("both levels seen - oscillating; frequency "
                                           "not measured"),
                                    "osc");
                    }
                } else if (ck.oscTapOks == 0 && ck.oscTapFloats >= 2) {
                    finishCheck(GUIDE_CHECK_FAIL,
                                "row never read stable - floating, or oscillating too fast for taps",
                                "float");
                } else {
                    finishCheck(GUIDE_CHECK_FAIL,
                                "single DC level seen - not oscillating",
                                "%.2fVspan", (ck.oscTapOks > 0) ? (ck.oscVHi - ck.oscVLo) : 0.0f);
                }
            }
            break;
        }

        case GuideCheck::RAIL_SANE: {
            // §3: MEASURE THE RAIL, THEN COMPARE THE ROWS TO THE MEASUREMENT.
            //
            // The old rule compared every row against the rail SETPOINT, but
            // the net-scan work already proved this board's rails run ~220 mV
            // below setpoint (`sources: 101=set5.00V/meas4.79V`). So a row at
            // 4.74 V on a rail that really measures 4.78 V - a HEALTHY board -
            // failed a +/-0.2 V setpoint test. Two gates now:
            //   1. the rail itself vs its setpoint, generously (the BOARD check)
            //   2. each power row vs the MEASURED rail, tightly, on the SAME
            //      ADC channel so gain error genuinely cancels (the WIRING check)
            if (ck.phase == CkPhase::RAIL_SETTLE) {
                if (millis() - ck.stimAppliedMs >= 100) {
                    ck.phase = railSaneNeedsRef() ? CkPhase::RAIL_REF
                                                  : CkPhase::TAP_REQUEST;
                }
                break;
            }
            if (ck.phase == CkPhase::RAIL_REF) {
                // Hand the wheel to the tap sub-machine, which owns
                // TAP_REQUEST/TAP_WAIT; railRefPending is what says the tap
                // now in flight is a RAIL's, not a row's, and railRefWhich
                // says which rail. Top first when both are owed, so the common
                // single-rail project behaves exactly as it always did.
                ck.railRefWhich = (railSaneNeedsTopRef() && !ck.railTopRefDone) ? 0 : 1;
                ck.railRefPending = true;
                if (ck.railRefWhich == 0) ck.railAdc = -1;
                else                      ck.railAdcBot = -1;
                ck.phase = CkPhase::TAP_REQUEST;
                break;
            }
            if (ck.railRefPending) {
                const bool isBot = (ck.railRefWhich == 1);
                const int refNode = isBot ? BOTTOM_RAIL : TOP_RAIL;
                const char* refName = isBot ? "bottom rail" : "rail";
                const float setpoint = isBot ? railSaneBotTarget()
                                             : railSaneTopTarget();
                float v, d;
                int adcUsed = -1;
                int r = tapCycleNode(refNode, &v, &d, -1, &adcUsed);
                if (r == 0) break;
                if (r == -2 || r == -1) {
                    // The reference tap is an IMPROVEMENT, not a prerequisite:
                    // refusing here would fail a check that used to pass. Fall
                    // back to the setpoint comparison and say so once.
                    Serial.printf("  rail: could not tap the %s itself "
                                  "(%s) - comparing rows against the %.2fV "
                                  "setpoint instead\n\r", refName,
                                  (r == -2) ? "no sense route" : "reads floating",
                                  (double)setpoint);
                    if (isBot) { ck.railMeasuredBot = setpoint; ck.railAdcBot = -1;
                                 ck.railBotRefDone = true; }
                    else       { ck.railMeasured = setpoint;    ck.railAdc = -1;
                                 ck.railTopRefDone = true; }
                    ck.railRefPending = false;
                    ck.tapHardFails = 0;
                    if (railSaneRefOutstanding()) ck.phase = CkPhase::RAIL_REF;
                    break;
                }
                if (isBot) { ck.railMeasuredBot = v; ck.railAdcBot = adcUsed;
                             ck.railBotRefDone = true; }
                else       { ck.railMeasured = v;    ck.railAdc = adcUsed;
                             ck.railTopRefDone = true; }
                ck.railRefPending = false;
                // GATE 1, the board check. 0.25 V + 5 % is wide on purpose:
                // it is asking "is the supply alive and roughly right", not
                // "is it calibrated". fabsf on both sides, so a negative
                // bottom-rail setpoint is gated the same way.
                if (fabsf(v - setpoint) > (0.25f + 0.05f * fabsf(setpoint))) {
                    setDetail("%s: meas %.2fV, set %.2fV - outside "
                              "0.25V+5%% of setpoint", refName,
                              (double)v, (double)setpoint);
                    finishCheck(GUIDE_CHECK_FAIL,
                                isBot ? "the bottom rail is off - check power"
                                      : "the rail itself is off - check power",
                                "rail%.2fV", (double)v);
                    break;
                }
                if (railSaneRefOutstanding()) ck.phase = CkPhase::RAIL_REF;
                break;
            }
            int row = ck.rows[ck.rowIdx];
            const uint8_t rowCls = ck.rowClass[ck.rowIdx];
            float v;
            bool have = false;
            // Always tap - the scan's <250ms window is poisoned by
            // construction here: rails flipped ~100ms ago, so any "fresh"
            // sample is pre-power or one EMA step converged (the same
            // timestamp-fresh != value-fresh deviation as VOLTAGE above).
            {
                float d;
                // The preferred channel is PER RAIL: a bottom-rail row read on
                // the channel the TOP rail happened to land on cancels nothing.
                int r = tapCycleNode(row, &v, &d, railSaneRefAdcFor(rowCls));
                if (r == 0) break;
                if (r == -2) {
                    finishCheck(GUIDE_CHECK_FAIL, "no sense route to the row",
                                "noroute@%d", row);
                    break;
                }
                if (r == -1) {
                    finishCheck(GUIDE_CHECK_FAIL,
                                "power-class row reads floating", "float@%d", row);
                    break;
                }
                have = true;
            }
            if (have) {
                bool ok;
                const float refV = railSaneRefFor(rowCls);
                if (rowCls == CK_ROWCLASS_GND) {
                    ok = (fabsf(v) <= 0.15f); // GND-class: 0 +/- 0.15 V
                } else {
                    // GATE 2, the wiring check: relative to the MEASURED rail
                    // THAT FEEDS THIS PIN, through the same sense-path class
                    // and the same channel.
                    float tol = 0.03f * fabsf(refV);
                    if (tol < 0.15f) tol = 0.15f;
                    float delta = fabsf(v - refV);
                    ok = (delta <= tol);
                    if (ck.railWorstRow < 0 || delta > fabsf(ck.railWorstDelta)) {
                        ck.railWorstDelta = delta;
                        ck.railWorstV = v;
                        ck.railWorstRow = row;
                    }
                }
                if (!ok) {
                    setDetail("%s: meas %.2fV (set %.2fV); row %d reads %.2fV",
                              (rowCls == CK_ROWCLASS_BOT) ? "bottom rail" : "rail",
                              (double)refV, (double)railSaneSetpointFor(rowCls),
                              row, (double)v);
                    finishCheck(GUIDE_CHECK_FAIL,
                                (rowCls == CK_ROWCLASS_GND)
                                    ? "gnd-class row is off 0V - miswired?"
                                    : ((rowCls == CK_ROWCLASS_BOT)
                                           ? "row is off the BOTTOM rail - miswired?"
                                           : "row is off the rail - miswired?"),
                                "%.2fV@%d", (double)v, row);
                    break;
                }
                ck.rowIdx++;
                if (ck.rowIdx >= ck.numRows) {
                    // Hint deliberately nullptr. §5.2's honest limit -
                    // "voltage-sanity, not a rail ammeter" - is a DOC
                    // caveat, not a bench message: GuidedFlow only prints
                    // guideCheckHint() when a check FAILS (`if (!pass &&
                    // hint...)`), and a rail_sane pass is the common case in
                    // every power_on step. Surfacing it on pass would print
                    // a disclaimer to the terminal AND the OLED on every
                    // successful power-up. The detail line below is NOT a
                    // disclaimer - it is the measurement, which §4 wants on
                    // pass as well as fail.
                    if (ck.railWorstRow > 0) {
                        if (ck.railBotRefDone && ck.railTopRefDone) {
                            // Mixed supply: name both measurements, or the
                            // line silently attributes a bottom-rail row's
                            // delta to the top rail.
                            setDetail("rails: top %.2fV (set %.2fV) bot %.2fV "
                                      "(set %.2fV); worst row delta %.2fV @%d",
                                      (double)ck.railMeasured,
                                      (double)railSaneTopTarget(),
                                      (double)ck.railMeasuredBot,
                                      (double)railSaneBotTarget(),
                                      (double)ck.railWorstDelta, ck.railWorstRow);
                        } else if (ck.railBotRefDone) {
                            setDetail("bottom rail: meas %.2fV (set %.2fV); worst "
                                      "row delta %.2fV @%d",
                                      (double)ck.railMeasuredBot,
                                      (double)railSaneBotTarget(),
                                      (double)ck.railWorstDelta, ck.railWorstRow);
                        } else {
                        setDetail("rail: meas %.2fV (set %.2fV); worst row "
                                  "delta %.2fV @%d", (double)ck.railMeasured,
                                  (double)railSaneTopTarget(),
                                  (double)ck.railWorstDelta, ck.railWorstRow);
                        }
                        // §4 wants `4.74V@8` - the POWER row, which is what
                        // the check is really about. Iteration order put the
                        // gnd-class row last and it reported `-0.00V@9`, a
                        // true but useless number.
                        finishCheck(GUIDE_CHECK_PASS, nullptr, "%.2fV@%d",
                                    (double)ck.railWorstV, ck.railWorstRow);
                    } else {
                        setDetail("rail: %d gnd-class row(s) within 0.15V of 0",
                                  ck.numRows);
                        finishCheck(GUIDE_CHECK_PASS, nullptr, "%.2fV@%d",
                                    (double)v, row);
                    }
                }
            }
            break;
        }

        default:
            finishCheck(GUIDE_CHECK_PASS, nullptr, "pass");
            break;
    }

    snprintf(valOut, valLen, "%s", (ck.result != GUIDE_CHECK_RUNNING) ? ck.val : "");
    return ck.result;
}

const char* guideCheckHint(void) {
    return ck.hint;
}

const char* guideCheckDetail(void) {
    return ck.detail;
}

void guideCheckAbort(void) {
    checkTeardown();
    ck.active = false;
    ck.result = GUIDE_CHECK_RUNNING;
    ck.phase = CkPhase::IDLE;
    // Hand the tap mailbox back. The request-before-poll discipline
    // (tapCycle* only polls after ITS OWN request succeeded) already made a
    // surviving result unreadable as fresh, but the REQUEST itself outlived
    // us: after a gate-closed abort it stayed pending and the scan core ran
    // it whenever its gates reopened - routes closing on a board nobody was
    // measuring. cancelOneShotTap() is a no-op when nothing is pending, so
    // guideCheckBegin's unconditional abort costs the next check nothing.
    //
    // REDUNDANT SINCE THE H1 WAVE, KEPT AS DOCUMENTATION: checkTeardown()
    // above now cancels at the funnel, which is where the timeout and the two
    // INA watchdogs get theirs. A second call is free (the guard sees nothing
    // pending) and this comment is where the reason is written down.
    cancelOneShotTap();
}

#else // OG_JUMPERLESS ---------------------------------------------------------

// The OG has no NVSCAN fabric, no INA sense chain on the crossbar, and no
// routable-GPIO bank for edge counting - every check reports unsupported
// and the guide treats that as warn-class (note + continue), per the brief.

void guideCheckBegin(const GuideCheckRun&) {}

int guideCheckPoll(char* valOut, size_t valLen) {
    snprintf(valOut, valLen, "unsupported");
    return GUIDE_CHECK_UNSUPPORTED;
}

const char* guideCheckHint(void) {
    return "electrical checks need V5 hardware";
}

const char* guideCheckDetail(void) { return ""; }

void guideCheckAbort(void) {}

#endif // OG_JUMPERLESS
