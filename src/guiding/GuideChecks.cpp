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

#if !defined(OG_JUMPERLESS)

#include "Apps.h"            // i2cScan (§5.2: reuse, real signature checked)
#include "Commands.h"        // refreshLocalConnections, waitCore2
#include "InfraPaths.h"      // infraIsBridge (user-claim test), infraOwnsNode
#include "JumperlessDefines.h"
#include "NetVoltageScan.h"  // the one-shot tap request API
#include "PartPlacement.h"   // partPinNode
#include "Peripherals.h"     // currentSenseState, setDacXvoltage, setTop/BotRail, gpioDef
#include "States.h"          // globalState
#include "config.h"
#include "hardware/gpio.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class CkPhase : uint8_t {
    IDLE,
    TAP_REQUEST,  // posting a one-shot tap (retries while a stale one drains)
    TAP_WAIT,     // polling the tap result
    BASE_SETTLE,  // baseline chain live (no GND return), pre-sample wait
    BASE_SAMPLE,  // collecting the dead-part INA baseline
    CHAIN_GROW,   // add the rowB->GND return leg, re-energize
    STIM_SETTLE,  // full chain live, waiting before sampling the INA
    STIM_SAMPLE,  // collecting INA readings (continuity) / pair tap (vf)
    PRES_CHARGE,  // presence: rows tied to the stimulus, charging
    PRES_UNROUTE, // presence: strand the charge, then tap row by row
    OSC_COUNT,    // counting edges on the ephemeral GPIO route
    RAIL_SETTLE,  // transient power applied, 100 ms wait (rail_sane)
    DONE,
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

    // stimulus chain (continuity / vf)
    bool chainLive = false;        // teardown owes ephemeral removal + refresh
    bool chainAdded[3] = {false, false, false};
    int chainRowA = -1, chainRowB = -1;
    float stimVolts = 0;           // chainComplete re-applies after the grow
    unsigned long stimAppliedMs = 0;
    float bandLo = 0, bandHi = 0;  // resolved min/max for this run

    // INA sampling
    float inaSum = 0;
    int inaCount = 0;
    uint32_t inaLastMs = 0;
    float inaBaseline = 0;         // dead-part chain current (see chainBegin)

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

// "10k" / "4.7k" / "470" / "1M" -> ohms, -1 when unparseable. Feeds the
// derived continuity band when the author gave no min/max.
static float parseResistanceOhms(const char* s) {
    if (s == nullptr || s[0] == '\0') return -1.0f;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (end == s || v <= 0.0f) return -1.0f;
    while (*end == ' ') end++;
    char suffix = *end;
    if (suffix == 'k' || suffix == 'K') v *= 1000.0f;
    else if (suffix == 'M' || suffix == 'm') v *= 1000000.0f; // "1m" = meg here
    return v;
}

// Terminal-state funnel: teardown FIRST (before anything can early-return),
// then record the outcome. Every terminal path goes through here.
static void checkTeardown(void);

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
        if (ck.chainAdded[0]) {
            removed |= globalState.removeEphemeralConnection(DAC0, ISENSE_PLUS, err, false, 0);
        }
        if (ck.chainAdded[1]) {
            removed |= globalState.removeEphemeralConnection(ISENSE_MINUS, ck.chainRowA, err, false, 0);
        }
        if (ck.chainAdded[2]) {
            removed |= globalState.removeEphemeralConnection(ck.chainRowB, GND, err, false, 0);
        }
        if (!ck.presStranded) {
            for (int i = 0; i < ck.presLegsAdded; i++) {
                removed |= globalState.removeEphemeralConnection(ISENSE_MINUS, ck.rows[i],
                                                                err, false, 0);
            }
        }
        ck.presLegsAdded = 0;
        ck.chainLive = false;
        ck.chainAdded[0] = ck.chainAdded[1] = ck.chainAdded[2] = false;
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

static int tapCycleNode(int node, float* outV, float* outDrift) {
    if (ck.phase == CkPhase::TAP_REQUEST) {
        if (millis() < ck.tapBackoffUntilMs) return 0;
        if (requestNodeTap(node)) ck.phase = CkPhase::TAP_WAIT;
        // else: a stale in-flight tap is draining - retry next poll
        return 0;
    }
    float v, d;
    int r = nodeTapResult(&v, &d);
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

// Build + energize the continuity/vf stimulus chain, in TWO stages:
//
//   stage 1 (baseline): DAC0 -> ISENSE_PLUS (INA0's shunt is the ammeter),
//                       ISENSE_MINUS -> rowA - and NO GND return, so no
//                       loop exists through the part yet
//   stage 2 (chainComplete): + rowB -> GND, the real measurement
//
// Why the baseline: the CURR_SENSE- net carries a constant board-internal
// load - bench-measured ~2.3 mA at 3.3 V on V5 r7 with the shunt fed and
// NOTHING routed downstream (identical on every row tried; scales with the
// stimulus voltage; suspect: the U12 DAC_1-repurpose path hanging on
// CURR_SENSE). Raw INA current is therefore part current PLUS that sink -
// enough to false-pass an empty row against a ~2 mA band. Stage 1 measures
// the sink at the exact stimulus voltage each run; the check evaluates
// (total - baseline). All ephemeral (LED option 0), one refresh per stage;
// the teardown funnel cleans up any partial chain.
static bool chainBegin(int rowA, int rowB, float stimulusVolts) {
    ck.chainRowA = rowA;
    ck.chainRowB = rowB;
    ck.stimVolts = stimulusVolts;
    ck.chainLive = true; // set BEFORE the adds so teardown always runs
    String err;
    ck.chainAdded[0] = globalState.addEphemeralConnection(DAC0, ISENSE_PLUS, err, false, 0);
    ck.chainAdded[1] = ck.chainAdded[0] &&
        globalState.addEphemeralConnection(ISENSE_MINUS, rowA, err, false, 0);
    if (!ck.chainAdded[1]) return false;
    refreshLocalConnections(0, 0, 0); // LED option 0: no visual disruption
    waitCore2();
    // save=0: state truth stays at the guide's safe 0 V - the slot cannot
    // persist the stimulus, and infra's DAC0 probe-power candidate stays
    // non-viable (it reads state) for the whole check.
    setDac0voltage(stimulusVolts, 0, 0);
    ck.stimAppliedMs = millis();
    ck.inaSum = 0;
    ck.inaCount = 0;
    ck.inaLastMs = currentSenseState.lastUpdatedMs;
    ck.phase = CkPhase::BASE_SETTLE;
    return true;
}

// Stage 2: stimulus off for the topology change (no live voltage while the
// refresh rewires crosspoints), add the GND return, re-energize.
static bool chainComplete(void) {
    setDac0voltage(globalState.power.dac0, 0, 0);
    String err;
    ck.chainAdded[2] =
        globalState.addEphemeralConnection(ck.chainRowB, GND, err, false, 0);
    if (!ck.chainAdded[2]) return false;
    refreshLocalConnections(0, 0, 0);
    waitCore2();
    setDac0voltage(ck.stimVolts, 0, 0);
    ck.stimAppliedMs = millis();
    ck.inaSum = 0;
    ck.inaCount = 0;
    ck.inaLastMs = currentSenseState.lastUpdatedMs;
    ck.phase = CkPhase::STIM_SETTLE;
    return true;
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
                String err;
                ck.chainAdded[0] =
                    globalState.addEphemeralConnection(DAC0, ISENSE_PLUS, err, false, 0);
                bool ok = ck.chainAdded[0];
                if (ck.presGrounded) {
                    ck.chainAdded[1] = ok &&
                        globalState.addEphemeralConnection(ISENSE_MINUS, ck.chainRowA, err, false, 0);
                    ck.chainAdded[2] = ck.chainAdded[1] &&
                        globalState.addEphemeralConnection(ck.chainRowB, GND, err, false, 0);
                    ok = ck.chainAdded[2];
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
            // Band: author's min/max when meaningful; else derived from the
            // part's value ("10k" -> I = 3.3 / (R + ~120 ohm path), +/-40%);
            // else a permissive default. INA path floor: below ~0.15 mA the
            // shunt reading is noise - refuse honestly rather than guess.
            if (ck.check == GuideCheck::CONTINUITY) {
                if (st.max > st.min) {
                    ck.bandLo = st.min;
                    ck.bandHi = st.max;
                } else {
                    float ohms = -1.0f;
                    if (st.partIdx >= 0 && st.partIdx < globalState.parts.numParts) {
                        ohms = parseResistanceOhms(
                            globalState.parts.parts[st.partIdx].value);
                    }
                    if (ohms > 0.0f) {
                        float i_mA = 3.3f / (ohms + 120.0f) * 1000.0f;
                        if (i_mA < 0.15f) {
                            finishCheck(GUIDE_CHECK_UNSUPPORTED,
                                        "resistance too large for the INA path", "toohighR");
                            break;
                        }
                        ck.bandLo = 0.6f * i_mA;
                        ck.bandHi = 1.4f * i_mA + 0.5f;
                    } else {
                        ck.bandLo = 0.2f;
                        ck.bandHi = 50.0f;
                    }
                }
                if (!chainBegin(rowA, rowB, 3.3f)) {
                    finishCheck(GUIDE_CHECK_FAIL, "stimulus chain routing failed", "setup");
                }
            } else { // VF
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
            }
            // else: rails already live (or a verify step) - measure directly.
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
// while this check applied them, state truth otherwise.
static float railSaneTopTarget(void) {
    return ck.railTransient ? ck.script->topRail : globalState.power.topRail;
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
            // Shared stimulus staging: baseline (no GND return) -> grow ->
            // loaded sample. Only the evaluation at the end differs.
            if (ck.phase == CkPhase::BASE_SETTLE) {
                if (millis() - ck.stimAppliedMs >= 60) ck.phase = CkPhase::BASE_SAMPLE;
                break;
            }
            if (ck.phase == CkPhase::BASE_SAMPLE) {
                // Average 4 FRESH INA polls (Peripherals re-asks the chip
                // every 10 ms from serviceInner - we just watch the stamp).
                if (currentSenseState.active &&
                    currentSenseState.lastUpdatedMs != ck.inaLastMs &&
                    currentSenseState.lastUpdatedMs > ck.stimAppliedMs) {
                    ck.inaLastMs = currentSenseState.lastUpdatedMs;
                    ck.inaSum += currentSenseState.current_mA;
                    ck.inaCount++;
                }
                if (ck.inaCount >= 4) {
                    ck.inaBaseline = ck.inaSum / ck.inaCount;
                    ck.phase = CkPhase::CHAIN_GROW;
                }
                break;
            }
            if (ck.phase == CkPhase::CHAIN_GROW) {
                if (!chainComplete()) {
                    finishCheck(GUIDE_CHECK_FAIL, "stimulus chain routing failed", "setup");
                }
                break;
            }
            if (ck.phase == CkPhase::STIM_SETTLE) {
                if (millis() - ck.stimAppliedMs >= 60) {
                    ck.phase = (ck.check == GuideCheck::CONTINUITY)
                                   ? CkPhase::STIM_SAMPLE
                                   : CkPhase::TAP_REQUEST; // vf pair-taps A/B
                }
                break;
            }
            if (ck.check == GuideCheck::CONTINUITY) {
                if (currentSenseState.active &&
                    currentSenseState.lastUpdatedMs != ck.inaLastMs &&
                    currentSenseState.lastUpdatedMs > ck.stimAppliedMs) {
                    ck.inaLastMs = currentSenseState.lastUpdatedMs;
                    ck.inaSum += currentSenseState.current_mA;
                    ck.inaCount++;
                }
                if (ck.inaCount >= 4) {
                    float i_mA = fabsf(ck.inaSum / ck.inaCount - ck.inaBaseline);
                    bool pass = (i_mA >= ck.bandLo && i_mA <= ck.bandHi);
                    finishCheck(pass ? GUIDE_CHECK_PASS : GUIDE_CHECK_FAIL,
                                pass ? nullptr
                                     : (i_mA < ck.bandLo
                                            ? "current below band - part missing or wrong value?"
                                            : "current above band - wrong value or a short?"),
                                "%.2fmA", i_mA);
                }
                break;
            }
            // ESCALATION PATH, if the bench ever noroutes here again:
            // invest-vf-noroute.md §8 Option 1 - have chainBegin/chainComplete
            // add the SENSE legs as ephemeral bridges too (rowA->ADC2,
            // rowB->ADC3, after infraAcquireAdc picks channels), so the full
            // router plans stimulus and sense together in one
            // refreshLocalConnections. It has far more route shapes than the
            // tap builder's three tiers, may lawfully share the GND net's
            // existing lanes, and ADC bridges are exempt from duplication;
            // the sampler then reads the ring channels directly, with no
            // fastConnectPath at all, and a routing failure becomes an honest
            // "stimulus chain routing failed" at setup. Costs: 5 of 8
            // ephemeral slots, ADC channels held for the whole check, and the
            // user-bridged-ADC exclusion has to pick channels first. Not built
            // now because the sequential-same-ADC taps below need only ONE
            // route at a time, which is what the bench actually starved on.
            float vA, vB;
            int r = tapCyclePair(ck.chainRowA, ck.chainRowB, &vA, &vB);
            if (r == 0) break;
            if (r == -2) {
                // Per-node, like the presence case: an undifferentiated
                // "noroute" is what made three bench retries say nothing.
                finishCheck(GUIDE_CHECK_FAIL, "no sense route to the rows",
                            "noroute@%d",
                            (ck.tapFailNode > 0) ? ck.tapFailNode : ck.chainRowA);
                break;
            }
            if (r == -1) {
                finishCheck(GUIDE_CHECK_FAIL,
                            "no stable reading across the part - is it seated?", "float");
                break;
            }
            float vf = vA - vB;
            // Part current = raw INA minus the dead-part baseline (the
            // board-internal CURR_SENSE sink - see chainBegin).
            float i_mA = currentSenseState.active
                             ? fabsf(currentSenseState.current_mA - ck.inaBaseline)
                             : 0.0f;
            bool conducting = (i_mA > 0.5f);
            bool inBand = (vf >= ck.bandLo && vf <= ck.bandHi);
            if (conducting && inBand) {
                finishCheck(GUIDE_CHECK_PASS, nullptr, "%.2fV", vf);
            } else if (!conducting) {
                // Full stimulus across the gap, no current: missing and
                // reversed are electrically identical from here - §5.2's
                // honest "flip it?".
                finishCheck(GUIDE_CHECK_FAIL,
                            "no current - LED missing or reversed (flip it?)",
                            "%.2fV", vf);
            } else {
                finishCheck(GUIDE_CHECK_FAIL, "Vf outside the expected band",
                            "%.2fV", vf);
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
            if (ck.phase == CkPhase::RAIL_SETTLE) {
                if (millis() - ck.stimAppliedMs >= 100) ck.phase = CkPhase::TAP_REQUEST;
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
                int r = tapCycleNode(row, &v, &d);
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
                    ok = (fabsf(v - railSaneTopTarget()) <= 0.2f); // VCC: rail +/- 0.2 V
                }
                if (!ok) {
                    finishCheck(GUIDE_CHECK_FAIL,
                                (ck.rowClass[ck.rowIdx] == 2)
                                    ? "gnd-class row is off 0V - miswired?"
                                    : "power-class row is off the rail voltage - miswired?",
                                "%.2fV@%d", v, row);
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
                    // successful power-up. The value string still carries
                    // what was actually measured.
                    finishCheck(GUIDE_CHECK_PASS, nullptr, "%.2fV@%d", v, row);
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

void guideCheckAbort(void) {}

#endif // OG_JUMPERLESS
