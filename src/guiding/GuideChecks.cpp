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

#include "GuidedFlow.h"

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
    uint8_t rowClass[CK_MAX_ROWS]; // rail_sane: 1 = power, 2 = gnd
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
    float vOfsA = 0, vOfsB = 0;    // the sense pair at the zero-stimulus point
    bool haveVofs = false;
    bool haveVsense = false;
    char detail[112];              // the terminal's long-form line (§4)

    // oscillates
    int oscGpioIdx = -1;           // gpioDef index of the ephemeral route; -1 = tap fallback
    int oscTarget = -1;            // remembered for teardown (never deref step there)
    bool oscLastLevel = false, oscHaveLevel = false;
    uint32_t oscEdges = 0;
    unsigned long oscSettleUntilMs = 0;
    unsigned long oscWindowStartMs = 0;
    uint32_t oscWindowMs = 500;
    float oscVLo = 99.0f, oscVHi = -99.0f;
    int oscTapOks = 0, oscTapFloats = 0;

    // rail_sane transient (this check applied power; teardown restores)
    bool railTransient = false;
    float railMeasured = 0;        // §3: the rail's OWN measured voltage
    bool railRefPending = false;   // the in-flight tap is the rail's, not a row's
    int railAdc = -1;              // channel the rail tap landed on - every
                                   // row is then compared on the SAME one
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
    char hint[96];
};

static CheckState ck;

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
    const float scaleA = adcSpread[chA] / 4095.0f, zeroA = adcZero[chA];
    const float scaleB = adcSpread[chB] / 4095.0f, zeroB = adcZero[chB];
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
        infraReleaseAdc(INFRA_ADC_SCAN);
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
    ck.senseAdcA = infraAcquireAdc(INFRA_ADC_SCAN, 0x0F, false);
    if (ck.senseAdcA >= 0) {
        ck.senseAdcB = infraAcquireAdc(INFRA_ADC_SCAN,
                                       (uint8_t)(0x0F & ~(1u << ck.senseAdcA)), false);
    }
    if (ck.senseAdcA < 0 || ck.senseAdcB < 0) {
        // Both free channels are user-bridged (the 555 file does exactly this
        // with ADC0/ADC1). Fall back to task 2's sequential-same-ADC one-shot
        // taps, which acquire ONE channel per tap under NVSCAN - §1.6 calls
        // that the preferred same-ADC mode anyway, so this is a degrade in
        // routing robustness, not in accuracy.
        infraReleaseAdc(INFRA_ADC_SCAN);
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

// Does this rail_sane run owe a reference tap? Only power-class rows are
// compared against the measured rail; a GND-only run needs no reference (the
// breadboard minus rails are hard ground - §3 step 3).
static bool railSaneNeedsRef(void) {
    for (int i = 0; i < ck.numRows; i++) {
        if (ck.rowClass[i] == 1) return true;
    }
    return false;
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
    }
    infraReleaseAdc(INFRA_ADC_SCAN);
    ck.senseAdcA = ck.senseAdcB = -1;
    ck.haveVofs = false;
    ck.senseMode = SenseMode::TAPS;
    ck.tapHardFails = 0;
    ck.tapSeqPhase = 0;
    if (energized) {
        setDac0voltage(ck.stimVolts, 0, 0);
        ck.phase = CkPhase::TAP_REQUEST;
    }
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
                ck.rowClass[ck.numRows] = pin.pinClass;
                ck.numRows++;
            }
        }
    }
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
            ck.timeoutMs = ck.oscWindowMs + 1000; // window IS the schedule
            // Free routable GPIO, the probe-power scan's skip list (high end
            // first - users reach for GPIO 1): not MicroPython-owned, no PWM,
            // not the top-OLED pins, no bridge of any kind, config direction
            // INPUT (the refresh's setGPIO pass re-asserts config, so an
            // output-configured pin would drive the net - never pick one).
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
            if (ck.oscGpioIdx >= 0) {
                String err;
                if (!globalState.addEphemeralConnection(st.target,
                                                        gpioDef[ck.oscGpioIdx][1],
                                                        err, true, 0)) {
                    ck.oscGpioIdx = -1; // nothing landed; no teardown owed
                    // fall back to taps below
                } else {
                    gpio_set_dir(gpioDef[ck.oscGpioIdx][0], false); // input, belt+braces
                    ck.oscSettleUntilMs = millis() + 20;
                    ck.phase = CkPhase::OSC_COUNT;
                    break;
                }
            }
            // Tap fallback (no GPIO free): repeated taps through the window.
            // A tap sees a DC level (ok) or drift-rejects (the scanner is
            // DC-only - a moving signal LOOKS floating to it). Both levels
            // observed across ok taps = oscillating; frequency unmeasurable
            // this way - reported honestly as "osc".
            ck.oscWindowStartMs = millis();
            ck.phase = CkPhase::TAP_REQUEST;
            break;
        }

        case GuideCheck::I2C_ACK: {
            // §5.2: reuse i2cScan (Apps.cpp) - author supplies sda/scl via
            // n1/n2. This is the one documented BLOCKING check (~1-3 s
            // including its own UI); it manages its own bridges and removes
            // them (leaveConnections=0), so no teardown is owed here.
            if (st.n1 < 1 || st.n2 < 1) {
                finishCheck(GUIDE_CHECK_UNSUPPORTED, "i2c needs n1: (SDA) + n2: (SCL)", "norows");
                break;
            }
            int nDevices = i2cScan(st.n1, st.n2, 26, 27, /*leaveConnections=*/0,
                                   /*internalScan=*/0);
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
                ck.rows[ck.numRows] = st.target;
                ck.rowClass[ck.numRows] = 1;
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

// ---------------------------------------------------------------------------
// Measurement helpers (continuity / vf)
// ---------------------------------------------------------------------------

// One FRESH shunt sample, on a Peripherals-poll tick we have not counted yet.
// The 10 ms poll re-asks the chip from serviceInner (which the guide loop
// pumps between check polls); we only watch the stamp. Same pattern the old
// current_mA averaging used - the field read is the new part.
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
        setDetail("%s: open (%s at %.1fV across the rows; band %s-%s)",
                  partName, iStr, (double)ck.stimVolts, loStr, hiStr);
        finishCheck(GUIDE_CHECK_FAIL,
                    "no conduction - part missing, or a leg not seated?", "open");
        return;
    }
    const float rMeas = vPart / (ck.iPart_mA / 1000.0f);
    char rStr[12];
    formatOhms(rMeas, rStr, sizeof(rStr));
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
    if (conducting && inBand) {
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
            //   OFS_SAMPLE   4 fresh shunt samples -> I_ofs; |I_ofs| < 0.3 mA
            //                or the isense path is leaking
            //   (energize)   DAC0 -> V_stim, save=0
            //   STIM_SETTLE  60 ms
            //   STIM_SAMPLE  8 fresh shunt samples -> I_raw; I_part = raw-ofs
            //   V_READ       the two sense channels, one ring dwell
            //   compute      R = (V_A - V_B) / I_part   |   Vf = V_A - V_B
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
                // stimulus up. (rowB deliberately gets no such gate: ~0 V is
                // its correct value in this topology and an unrouted channel
                // reads the same - see the report's §1 limitation note.)
                if (ck.vSenseA < 0.25f && ck.stimVolts >= 1.0f) {
                    Serial.printf("  sense: rowA reads %.3f V under a %.1f V "
                                  "stimulus - the sense bridge is not on the row; "
                                  "falling back to one-shot taps\n\r",
                                  (double)ck.vSenseA, (double)ck.stimVolts);
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
                    finishCheck(GUIDE_CHECK_PASS,
                                "tap fallback: both levels seen; frequency not measured (no free GPIO)",
                                "osc");
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
                // now in flight is the RAIL's, not a row's.
                ck.railRefPending = true;
                ck.railAdc = -1;
                ck.phase = CkPhase::TAP_REQUEST;
                break;
            }
            if (ck.railRefPending) {
                float v, d;
                int adcUsed = -1;
                int r = tapCycleNode(TOP_RAIL, &v, &d, -1, &adcUsed);
                if (r == 0) break;
                if (r == -2 || r == -1) {
                    // The reference tap is an IMPROVEMENT, not a prerequisite:
                    // refusing here would fail a check that used to pass. Fall
                    // back to the setpoint comparison and say so once.
                    Serial.printf("  rail: could not tap the rail itself "
                                  "(%s) - comparing rows against the %.2fV "
                                  "setpoint instead\n\r",
                                  (r == -2) ? "no sense route" : "reads floating",
                                  (double)railSaneTopTarget());
                    ck.railMeasured = railSaneTopTarget();
                    ck.railAdc = -1;
                    ck.railRefPending = false;
                    ck.tapHardFails = 0;
                    break;
                }
                ck.railMeasured = v;
                ck.railAdc = adcUsed;
                ck.railRefPending = false;
                // GATE 1, the board check. 0.25 V + 5 % is wide on purpose:
                // it is asking "is the supply alive and roughly right", not
                // "is it calibrated".
                const float setpoint = railSaneTopTarget();
                if (fabsf(v - setpoint) > (0.25f + 0.05f * fabsf(setpoint))) {
                    setDetail("rail: meas %.2fV, set %.2fV - outside "
                              "0.25V+5%% of setpoint", (double)v, (double)setpoint);
                    finishCheck(GUIDE_CHECK_FAIL,
                                "the rail itself is off - check power",
                                "rail%.2fV", (double)v);
                }
                break;
            }
            int row = ck.rows[ck.rowIdx];
            float v;
            bool have = false;
            // Always tap - the scan's <250ms window is poisoned by
            // construction here: rails flipped ~100ms ago, so any "fresh"
            // sample is pre-power or one EMA step converged (the same
            // timestamp-fresh != value-fresh deviation as VOLTAGE above).
            {
                float d;
                int r = tapCycleNode(row, &v, &d, ck.railAdc);
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
                if (ck.rowClass[ck.rowIdx] == 2) {
                    ok = (fabsf(v) <= 0.15f); // GND-class: 0 +/- 0.15 V
                } else {
                    // GATE 2, the wiring check: relative to the MEASURED rail,
                    // through the same sense-path class and the same channel.
                    float tol = 0.03f * fabsf(ck.railMeasured);
                    if (tol < 0.15f) tol = 0.15f;
                    float delta = fabsf(v - ck.railMeasured);
                    ok = (delta <= tol);
                    if (ck.railWorstRow < 0 || delta > fabsf(ck.railWorstDelta)) {
                        ck.railWorstDelta = delta;
                        ck.railWorstV = v;
                        ck.railWorstRow = row;
                    }
                }
                if (!ok) {
                    setDetail("rail: meas %.2fV (set %.2fV); row %d reads %.2fV",
                              (double)ck.railMeasured, (double)railSaneTopTarget(),
                              row, (double)v);
                    finishCheck(GUIDE_CHECK_FAIL,
                                (ck.rowClass[ck.rowIdx] == 2)
                                    ? "gnd-class row is off 0V - miswired?"
                                    : "row is off the rail - miswired?",
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
                        setDetail("rail: meas %.2fV (set %.2fV); worst row "
                                  "delta %.2fV @%d", (double)ck.railMeasured,
                                  (double)railSaneTopTarget(),
                                  (double)ck.railWorstDelta, ck.railWorstRow);
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
