// SPDX-License-Identifier: MIT
/*
 * Net Voltage Scan implementation.
 *
 * See NetVoltageScan.h for the concept. The important mechanics:
 *
 * Sense routes are computed FRESH on every poll from the live crosspoint
 * occupancy (lastChipXY) and the fabric map (chipStates[].xMap/yMap), then
 * closed, read, and opened again within one call. No persistent channel
 * state exists, so rerouting can never leave a stale tap behind.
 *
 * Route shapes (ADC0-3 live on chip K x8-11, chip K y0-7 are the lanes to
 * breadboard chips A-H, y0 on each breadboard chip is BOUNCE_NODE):
 *   - row on chips A-H:  (rowChip, laneToK, rowY) + ADC crosspoint on K.
 *     Fallback when that lane is taken by routing: row -> L lane -> the
 *     L<->K interchip lane -> a free K y bounce.
 *   - node on a K x pin (rows 29/59, AREF, buffer in): bounce off a free
 *     K y lane.
 *   - node on an I/J/L x pin (nano header, ISENSE, UART, GPIO 1-8, rows
 *     30/60): through a breadboard chip's BOUNCE_NODE y0 over to its K lane.
 */

#include "NetVoltageScan.h"
#include "CoreMailbox.h" // core1req::allIdle() (T2.2b)

#ifndef OG_JUMPERLESS

#include "CH446Q.h"
#include "FakeGpio.h" // tdmInputs (to avoid taking over the FakeGPIO ADC)
#include "NetsToChipConnections.h" // numberOfPaths (includes duplicate paths)
#include "MatrixState.h"
#include "Peripherals.h"
#include "RotaryEncoder.h" // encoderServiceYield() - serviced during our waits
#include "InfraPaths.h"
#include "RouteSafety.h"
#include "States.h"
#include "externVars.h" // core-1 frame hold / core2busy / lastUserInputMs
#include "USBAudio.h"   // usbAudioOwnsAdc
#include "AdcRing.h"    // T2.1: taps read fresh sweeps off the ring
#include "WaveGen.h"
#include "config.h"
#include "hardware/adc.h"

extern WaveGen wavegen;
extern volatile bool refreshInProgress; // Commands.h
extern volatile int probeActive;
extern volatile int& inClickMenu; // Menus.h
extern volatile int& inPadMenu;   // Probing.h
// Defined in Graphics.cpp - ant geometry continuity check for the report,
// and the ant on/off flip tally (prints flips-since-last-print, then resets)
extern void printAntPathContinuity(Stream* out);
extern void printAntFlipStats(Stream* out);

// The encoder poll is single-owner on this core and internally throttled to
// 2kHz, so calling it during our blocking waits is free and keeps the
// clickwheel responsive no matter how long a tap takes.
static inline void waitServicingEncoder(unsigned long us) {
    unsigned long start = micros();
    while (micros() - start < us) {
        encoderServiceYield();
        tight_loop_contents();
    }
}

float nodeVoltage[NODE_VOLTAGE_MAX];
uint32_t nodeVoltageMs[NODE_VOLTAGE_MAX];
NetCurrentInfo netCurrentInfo[NET_CURRENT_INFO_COUNT];
static float pathCurrent_mA[MAX_BRIDGES];   // smoothed estimate (the EMA)
static bool pathCurrentValid[MAX_BRIDGES];
// What consumers see: the smoothed estimate gated by a post-EMA per-path
// deadband with hysteresis (0 while below the band). Separate from the EMA
// so the deadband never corrupts the filter state it gates.
static float pathShown_mA[MAX_BRIDGES];
static bool pathShownOn[MAX_BRIDGES];   // hysteresis latch
// EMA continuity: seeded once per routing epoch, NOT per freshness window.
// Gating the EMA on last-pass validity made any >freshness pause (menus,
// scan re-enable) inject one raw full-amplitude sample into the display.
static bool pathEmaSeeded[MAX_BRIDGES];

static const unsigned long kScanIntervalUs = 5000;    // min gap between taps
// Sample validity window. Generous on purpose: probing/scrolling preempts
// the scanner (user-input gate), so a tight window aged the currents out
// MID-INTERACTION - highlight displays flipped from "V + mA" back to plain
// "Net N / row X" while the user was actively looking at them. Routing
// changes still wipe samples instantly via the fingerprint reset.
static const unsigned long kVoltageFreshMs = 5000;
static const int kMaxScanNodes = 128;

static int scanNodes[kMaxScanNodes];
static int scanNodeCount = 0;
static int scanIndex = 0;
// Consecutive non-busy failures on the CURRENT round-robin node (one
// immediate retry before advancing - the ant-coherence rule; see the
// round-robin tap slot).
static uint8_t nodeRetryCount = 0;
static bool adcNodePresent[5]; // ADC0..ADC4 bridged into a net by the user
static uint32_t lastFingerprint = 0;
static unsigned long lastScanUs = 0;

// Auto-zero: tapping GND through the scan path itself measures the shared
// ADC/calibration bias (~10-30mV), which otherwise shows up as phantom
// 0.2-0.5mA currents across low-resistance paths. Tracked per ADC channel.
// Only refreshed while GND is not part of any user net - people build real
// circuits on these and we must not add even a stub to their ground.
static float scanZeroOffset[4] = {0, 0, 0, 0};
static uint32_t scanZeroMs[4] = {0, 0, 0, 0};
static bool gndInUse = false;

// Early-vs-late tap reading difference per node, for the floating check and
// the debug print.
static float nodeDrift[NODE_VOLTAGE_MAX];

// Measured known sources. fillKnownSources() used to write rail/DAC
// SETPOINTS into nodeVoltage[] while the other end of every path was
// ADC-measured - the systematic offset between "5.00V commanded" and what
// the sense path actually reads (rail droop, DAC/ADC calibration skew,
// 220mV seen on a real board) turned directly into phantom mA on every
// rail-fed path. Tapping each in-use source through the SAME sense path
// ~1/s makes both endpoints share the channel's systematics, so a
// zero-current path really reads dv~=0. Judgment call flagged in review:
// this deliberately reverses isKnownSourceNode()'s rail/DAC exclusion and
// taps them while in use - justified because a momentary high-Z tap is
// exactly what the scanner already does to every user node. GND stays
// untouched (it IS the zero reference; the auto-zero below handles it).
static const int kSourceNodes[4] = {TOP_RAIL, BOTTOM_RAIL, DAC0, DAC1};
static bool sourceInUse[4];
static float sourceMeasured[4];
static float sourceSetpointAtMeasure[4];
static uint32_t sourceMeasuredMs[4] = {0, 0, 0, 0};
// Attempt time, distinct from success time: a source whose tap keeps
// failing (lanes busy) must back off, not hog every tap slot away from
// the user-node round-robin.
static uint32_t sourceAttemptMs[4] = {0, 0, 0, 0};
// Measured values are trusted for a few refresh periods, then the setpoint
// takes back over (a paused scan must not serve week-old rail readings).
static const uint32_t kSourceMeasureFreshMs = 2500;
static const uint32_t kSourceMeasureIntervalMs = 1000;

// Last fastConnectPath refusal seen by a tap on this core, with the fabric
// snapshot taken AT the refusal (invest-vf-noroute.md §6). PLAIN statics on
// purpose: they are written and read only on the scan core, and the
// background scan taps through the same senseNodeVoltage - so the one-shot
// service copies them into its own volatiles immediately after its tap
// returns, before the scan can overwrite them. Core 0 never reads these.
static int lastRouteFailNode = -1;
static int lastRouteFailAdc = -1;
static int lastRouteFailRc = 0;
static uint16_t lastRouteFailKfree = 0;
static uint16_t lastRouteFailKxbusy = 0;
static int lastRouteFailChip = -1;
static uint16_t lastRouteFailXbusy = 0;
static uint8_t lastRouteFailBounceOk = 0;

static void noteRouteFail(int node, int adc, int rc) {
    lastRouteFailNode = node;
    lastRouteFailAdc = adc;
    lastRouteFailRc = rc;
    fastPathFailSnapshot(node, &lastRouteFailKfree, &lastRouteFailKxbusy,
                         &lastRouteFailChip, &lastRouteFailXbusy,
                         &lastRouteFailBounceOk);
}

// Tap failure tallies for the debug print (why nodes go stale)
static uint32_t tapFailNoRoute = 0;
static uint32_t tapFailAdcBusy = 0;
static uint32_t tapFailDrift = 0;
static uint32_t tapFailRingStale = 0; // ring couldn't serve a FRESH window
static uint32_t tapOk = 0;
static uint32_t tapPairOk = 0; // of tapOk, taps that read a path's both ends
static uint32_t tapSeqPairOk = 0;  // sequential same-channel pairs (fallback)
static uint32_t tapPairAsym = 0;   // pairs refused for unequal route hops

// Pairwise differential taps. debug.net_scan_pair_taps selects the
// STRATEGY (Kevin's A/B ask, flashy-ants session - flip it live with
// `~ debug.net_scan_pair_taps = N`, no reboot):
//   0 = off (plain round-robin only)
//   1 = SEQUENTIAL pair, the DEFAULT: both ends back-to-back on ONE
//       channel (~3ms apart). One route at a time on one ADC - "how we
//       had it originally" - channel offsets/gain cancel in dv entirely
//       (same channel), only the 3ms of drift between dwells survives.
//   2 = TRUE pair: both ends in one dwell on two channels, hop-matched
//       routes only (falls back to sequential when refused). Zero time
//       skew, but per-channel GAIN mismatch turns into an error
//       proportional to V (1% @ 3V = 30mV); mitigated by alternating
//       which node rides which channel (pairSwapPhase) so it averages
//       out of the EMAs to first order.
// Either way the aligned delta lands here, per path; computePathCurrents
// prefers it over subtracting two round-robin node samples taken up to a
// full scan cycle apart.
static float pathPairDv[MAX_BRIDGES];
static uint32_t pathPairDvMs[MAX_BRIDGES];
static int pairPathIndex = 0;
static bool pairSwapPhase = false;

// ============================================================================
// Hang-proof ADC read
// ============================================================================
//
// readAdc()'s adc_read() busy-waits with NO timeout, and its 100ms lock takeover
// means two cores can end up driving the ADC state machine at once - which
// corrupts it and hangs adc_read() forever (this wedged the whole board in
// testing). This variant is conversion-only: the CALLER must hold the
// readingADC lock. Each conversion is manually triggered with a microsecond
// timeout so a collision costs one discarded sample, not a core.

static bool readScanAdcVoltage(int channel, int samples, float* volts) {
    if (adcRingActive()) {
        // T2.1: n fresh sweeps after this instant (the tap has settled by
        // now), off the ring - no converter, no lock; ~21 us per sample.
        // Fresh-or-fail: on timeout/resync/stop the ring hands back
        // best-available HISTORY, which describes whatever was on this
        // channel before our tap - feeding that into nodeVoltage[] is how
        // stale data becomes phantom current. Fail the tap instead; the
        // node just stays stale one more scan cycle.
        bool fresh = false;
        int raw = adcRingMeanAfterStrict(channel, adcRingSweeps(), samples,
                                         (uint32_t)samples * 25u + 400u, &fresh);
        if (!fresh) {
            tapFailRingStale++;
            return false;
        }
        float reading = ((float)raw) * (adcSpread[channel] / 4095.0f);
        if (channel != 4 && channel != 5) reading -= adcZero[channel];
        *volts = reading;
        return true;
    }
    adc_select_input(channel);
    uint32_t sum = 0;
    int good = 0;
    for (int i = 0; i < samples; i++) {
        hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
        unsigned long start = micros();
        while (!(adc_hw->cs & ADC_CS_READY_BITS)) {
            if (micros() - start > 100) break; // conversion is ~2us; 100us = corrupted
        }
        if (!(adc_hw->cs & ADC_CS_READY_BITS)) continue;
        sum += (adc_hw->result & 0xFFF);
        good++;
        delayMicroseconds(6);
    }

    if (good == 0) return false;
    // Same scaling readAdcVoltage() applies (calibrated spread + zero).
    float reading = ((float)(sum / good)) * (adcSpread[channel] / 4095.0f);
    if (channel != 4 && channel != 5) {
        reading -= adcZero[channel];
    }
    *volts = reading;
    return true;
}

// ============================================================================
// The tap itself (via RouteSafety fastConnectPath)
// ============================================================================

// Threshold for the floating-node check below. A driven node reads the same
// early and late in the tap (measured: +/-0.02V); a floating node's charge
// gets bumped by the charge kick of crosspoint switching and then decays through the
// ADC front end while tapped, so the two reads diverge (measured: 0.04-0.2V).
// Rejecting those keeps undriven nets from faking currents. Oscillating
// signals (PWM etc.) also get rejected, which is correct - this scanner is
// DC-only.
static const float kFloatingDriftVolts = 0.05f;

// Momentarily close a validated sense route, read the voltage, and open it.
// Returns false when no route exists or the node looks floating.
static bool senseNodeVoltage(int node, int adc, float* volts) {
    // Take the ADC lock BEFORE closing any crosspoints, so a busy ADC costs
    // nothing but this spin - which services the encoder the whole time.
    // NEVER take the lock over on timeout: that's how the ADC state machine gets corrupted
    // and hangs a core. Core 0's probe poller reads almost back-to-back; a
    // ~2ms window catches the gap between its reads.
    const bool ringLive = adcRingActive();   // T2.1: no converter to own - skip the lock
    unsigned long waitStart = micros();
    while (!ringLive && __atomic_test_and_set(&readingADC, __ATOMIC_ACQUIRE)) {
        if (micros() - waitStart > 2000) {
            tapFailAdcBusy++;
            return false;
        }
        encoderServiceYield();
        tight_loop_contents();
    }

    FastPathHandle handle;
    int adcNode = ADC0 + adc;
    // 500us/hop: normal handshakes complete in microseconds, so this only
    // matters when the PIO handshake is sick - and then the whole tap must
    // stay well under waitCore2()'s 25ms, or refreshConnections() times out
    // and rebuilds chipStates while we're still mid-tap (torn route state,
    // possible short). Budget: 2ms lock spin + 4x500us connect + unwind +
    // ~0.6ms dwell (80us settle + 500us ring hold) + 4x1ms disconnect
    // ~= 11ms hard worst case.
    int rc = fastConnectPath(node, adcNode, &handle, 500);
    if (rc != 0) {
        if (!ringLive) __atomic_clear(&readingADC, __ATOMIC_RELEASE);
        noteRouteFail(node, adc, rc);
        tapFailNoRoute++;
        return false;
    }

    waitServicingEncoder(80); // CH446Q settle (same value TDM landed on)
    float early = 0.0f, late = 0.0f;
    bool ok;
    if (ringLive) {
        // One dwell sliced from ring history: hold the tap ~500us while the
        // ring records (a sweep lands every ~21us), then read an EARLY
        // window (the 8 sweeps after settle) and a LATE window (the newest
        // 8) out of the same hold. Twice the samples per window of the old
        // read-wait-read at about the same tap length, and both windows are
        // guaranteed inside the tap. The floating-drift check below compares
        // the two ends of the dwell exactly as before.
        uint32_t gen = adcRingGeneration();
        uint32_t s0 = adcRingSweeps();
        waitServicingEncoder(500);
        uint32_t s1 = adcRingSweeps();
        // >= 17 sweeps elapsed keeps early and late disjoint; a generation
        // bump means a resync restarted the sweep phase mid-dwell.
        ok = (adcRingActive() && adcRingGeneration() == gen && (s1 - s0) >= 17);
        if (ok) {
            // Early = sweeps s0+1..s0+8 (sweep s0 was mid-flight at capture,
            // its sample can predate the settle end by one sweep period -
            // same "+1" convention as adcRingMeanAfter). Late = newest 8.
            int rawEarly = adcRingMeanWindow(adc, s0 + 9, 8);
            int rawLate = adcRingMeanWindow(adc, s1, 8);
            // Re-check: a resync while summing voids the channel mapping.
            ok = (adcRingActive() && adcRingGeneration() == gen);
            float scale = adcSpread[adc] / 4095.0f;
            float zero = (adc != 4 && adc != 5) ? adcZero[adc] : 0.0f;
            early = (float)rawEarly * scale - zero;
            late = (float)rawLate * scale - zero;
        }
        if (!ok) tapFailRingStale++;
    } else {
        ok = readScanAdcVoltage(adc, 4, &early);
        if (ok) {
            waitServicingEncoder(250); // give a floating net time to visibly decay
            ok = readScanAdcVoltage(adc, 4, &late);
        }
    }
    fastDisconnectPath(&handle);
    if (!ringLive) __atomic_clear(&readingADC, __ATOMIC_RELEASE);
    if (!ok) {
        // Ring-path failures already counted themselves as ringstale; the
        // legacy converter path failing (no good conversions) is adcbusy.
        if (!ringLive) tapFailAdcBusy++;
        return false;
    }
    if (node >= 0 && node < NODE_VOLTAGE_MAX) {
        nodeDrift[node] = late - early;
    }
    if (fabsf(late - early) > kFloatingDriftVolts) {
        tapFailDrift++;
        return false; // floating
    }
    tapOk++;
    *volts = late;
    return true;
}

// Pairwise differential tap: close node1->adcA AND node2->adcB, one settle,
// then read both channels' early/late windows out of the same ring dwell.
// Ring-only (the shared-sweep trick IS the point); callers fall back to
// single taps when the ring is down. The two sequential fastConnectPaths
// can't collide (lastChipXY is updated per route, laneOk demands
// hardware-free lanes), and each route passes the same wouldShort
// validation as any tap. Both readings keep the per-channel floating-drift
// check - one floating end voids the whole tap.
static bool pairSenseTap(int node1, int node2, int adcA, int adcB,
                         float* v1, float* v2) {
    FastPathHandle h1, h2;
    int rc1 = fastConnectPath(node1, ADC0 + adcA, &h1, 500);
    if (rc1 != 0) {
        noteRouteFail(node1, adcA, rc1);
        tapFailNoRoute++;
        return false;
    }
    // Snapshot BEFORE unwinding h1: the whole point of the pair diagnostic is
    // what the fabric looked like with node1's route already claimed, which is
    // exactly the pressure that starves node2 (report §4).
    int rc2 = fastConnectPath(node2, ADC0 + adcB, &h2, 500);
    if (rc2 != 0) {
        noteRouteFail(node2, adcB, rc2);
        fastDisconnectPath(&h1);
        tapFailNoRoute++;
        return false;
    }
    // Hop matching (Kevin's ruling, flashy-ants session): dv only cancels
    // route-dependent systematics when both ends take routes of the same
    // SHAPE. A 2-hop GND route paired with a 4-hop bounced row route keeps
    // the asymmetry in every sample - refuse it here and let the caller's
    // sequential same-channel fallback tap both ends through their natural
    // routes on ONE channel instead.
    int hops1 = 0, hops2 = 0;
    for (int h = 0; h < 4; h++) {
        if (h1.path.chip[h] >= 0 && h1.path.x[h] >= 0 && h1.path.y[h] >= 0) hops1++;
        if (h2.path.chip[h] >= 0 && h2.path.x[h] >= 0 && h2.path.y[h] >= 0) hops2++;
    }
    if (hops1 != hops2) {
        fastDisconnectPath(&h2);
        fastDisconnectPath(&h1);
        tapPairAsym++;
        return false;
    }
    waitServicingEncoder(80); // CH446Q settle, both routes closed
    uint32_t gen = adcRingGeneration();
    uint32_t s0 = adcRingSweeps();
    waitServicingEncoder(500);
    uint32_t s1 = adcRingSweeps();
    bool ok = (adcRingActive() && adcRingGeneration() == gen && (s1 - s0) >= 17);
    float earlyA = 0, lateA = 0, earlyB = 0, lateB = 0;
    if (ok) {
        // Window arithmetic as in senseNodeVoltage: early = s0+1..s0+8,
        // late = the newest 8 - both channels sliced from the SAME sweeps.
        int rawEarlyA = adcRingMeanWindow(adcA, s0 + 9, 8);
        int rawEarlyB = adcRingMeanWindow(adcB, s0 + 9, 8);
        int rawLateA = adcRingMeanWindow(adcA, s1, 8);
        int rawLateB = adcRingMeanWindow(adcB, s1, 8);
        ok = (adcRingActive() && adcRingGeneration() == gen);
        float scaleA = adcSpread[adcA] / 4095.0f;
        float zeroA = (adcA != 4 && adcA != 5) ? adcZero[adcA] : 0.0f;
        float scaleB = adcSpread[adcB] / 4095.0f;
        float zeroB = (adcB != 4 && adcB != 5) ? adcZero[adcB] : 0.0f;
        earlyA = (float)rawEarlyA * scaleA - zeroA;
        lateA = (float)rawLateA * scaleA - zeroA;
        earlyB = (float)rawEarlyB * scaleB - zeroB;
        lateB = (float)rawLateB * scaleB - zeroB;
    }
    fastDisconnectPath(&h2);
    fastDisconnectPath(&h1);
    if (!ok) {
        tapFailRingStale++;
        return false;
    }
    if (node1 >= 0 && node1 < NODE_VOLTAGE_MAX) nodeDrift[node1] = lateA - earlyA;
    if (node2 >= 0 && node2 < NODE_VOLTAGE_MAX) nodeDrift[node2] = lateB - earlyB;
    if (fabsf(lateA - earlyA) > kFloatingDriftVolts ||
        fabsf(lateB - earlyB) > kFloatingDriftVolts) {
        tapFailDrift++;
        return false;
    }
    tapOk++;
    tapPairOk++;
    *v1 = lateA;
    *v2 = lateB;
    return true;
}

// ============================================================================
// One-shot tap requests (guide checks) - see the header contract
// ============================================================================
//
// Slots are written on core 0 (payload first, __dmb(), then the reqSeq bump)
// and consumed here (payload first, __dmb(), then doneSeq = reqSeq).
// reqSeq != doneSeq means a request is in flight; the poll side treats only
// doneSeq == reqSeq as complete, so a result can never be read against the
// wrong request. Single request in flight by contract. NO Serial I/O here -
// this runs on the scan core (dual-core USB hazard); failures speak through
// the result code and the tap counters.

static volatile int oneShotN1 = -1;
static volatile int oneShotN2 = -1;
static volatile bool oneShotIsPair = false;
static volatile int8_t oneShotCode = -2;
static volatile float oneShotV1 = 0.0f, oneShotV2 = 0.0f;
static volatile float oneShotDrift1 = 0.0f;
static volatile uint32_t oneShotReqSeq = 0;
static volatile uint32_t oneShotDoneSeq = 0;
static volatile bool oneShotCancel = false; // core-0 set, scan-core cleared
static uint32_t tapOneShot = 0; // serviced one-shots (the i! taps line)
// Preferred-ADC hint (core 0 writes with the request payload) and the
// channel actually granted (scan core writes with the result payload) - see
// the header's PREFERRED-ADC HINT note.
static volatile int8_t oneShotPreferAdc = -1;
static volatile int8_t oneShotAdcUsed = -1;
// Why the last one-shot returned -2, for core 0's diagnostic print. Copied
// out of the plain lastRouteFail* statics inside the service pass, before
// the background scan can overwrite them.
static volatile int16_t oneShotFailNode = -1;
static volatile int8_t oneShotFailAdc = -1;
static volatile int8_t oneShotFailRc = 0;
static volatile uint16_t oneShotFailKfree = 0;
static volatile uint16_t oneShotFailKxbusy = 0;
static volatile int8_t oneShotFailChip = -1;
static volatile uint16_t oneShotFailXbusy = 0;
static volatile uint8_t oneShotFailBounceOk = 0;
// Sequential pair fallback (ring down / no second channel free / a pair
// route refusal - see the Option 0 latch): one node per service pass, so a
// pass never exceeds the single-tap ~11ms worst case - two full taps back to
// back would flirt with waitCore2()'s 25ms ceiling.
static int oneShotPairPhase = 0;
static float oneShotPairV1 = 0.0f;
// Option 0 (invest-vf-noroute.md §8): a pairSenseTap that failed on ROUTING
// (no drift bump) latches here for the rest of THIS request, so the next
// service pass skips the pair branch and runs the one-node-per-pass path -
// which needs only one free route at a time and releases it between nodes.
// Core 0 clears it with each new request; the scan core sets it.
static volatile bool oneShotPairRouteFailed = false;

bool requestNodeTap(int node, int preferAdc) {
    if (oneShotReqSeq != oneShotDoneSeq) return false; // in flight
    if (node <= 0 || node >= NODE_VOLTAGE_MAX) return false;
    oneShotN1 = node;
    oneShotN2 = -1;
    oneShotIsPair = false;
    oneShotPairPhase = 0;
    oneShotPairRouteFailed = false;
    oneShotPreferAdc = (int8_t)((preferAdc >= 0 && preferAdc < 4) ? preferAdc : -1);
    // A cancel raised against the PREVIOUS request must not carry into this
    // one. It can survive when the scan core completed that request in the
    // same window the cancel was posted (both writers are core 0, so this
    // clear cannot race the set).
    oneShotCancel = false;
    __dmb();
    oneShotReqSeq = oneShotReqSeq + 1;
    return true;
}

int nodeTapResult(float* volts, float* drift, int* adcUsed) {
    if (oneShotReqSeq != oneShotDoneSeq) return 0; // pending
    if (oneShotIsPair) return -2;                  // kind mismatch
    __dmb();
    if (volts) *volts = oneShotV1;
    if (drift) *drift = oneShotDrift1;
    if (adcUsed) *adcUsed = oneShotAdcUsed;
    return oneShotCode;
}

bool requestPairTap(int n1, int n2) {
    if (oneShotReqSeq != oneShotDoneSeq) return false;
    if (n1 <= 0 || n1 >= NODE_VOLTAGE_MAX) return false;
    if (n2 <= 0 || n2 >= NODE_VOLTAGE_MAX) return false;
    oneShotN1 = n1;
    oneShotN2 = n2;
    oneShotIsPair = true;
    oneShotPairPhase = 0;
    oneShotPairRouteFailed = false;
    oneShotPreferAdc = -1;   // a stale hint must not leak into a pair service
    oneShotCancel = false;   // see requestNodeTap
    __dmb();
    oneShotReqSeq = oneShotReqSeq + 1;
    return true;
}

int pairTapResult(float* v1, float* v2) {
    if (oneShotReqSeq != oneShotDoneSeq) return 0;
    if (!oneShotIsPair) return -2;
    __dmb();
    if (v1) *v1 = oneShotV1;
    if (v2) *v2 = oneShotV2;
    return oneShotCode;
}

void oneShotTapFailInfo(OneShotTapFail* info) {
    if (!info) return;
    __dmb();
    info->node = oneShotFailNode;
    info->adc = oneShotFailAdc;
    info->rc = oneShotFailRc;
    info->kFreeYMask = oneShotFailKfree;
    info->kXBusyMask = oneShotFailKxbusy;
    info->chip = oneShotFailChip;
    info->xBusyMask = oneShotFailXbusy;
    info->bounceOkMask = oneShotFailBounceOk;
}

// Core-0 cancel for a request that no longer has an owner. Without it an
// aborted check's pending tap sat in the mailbox and fired LATER, unowned:
// it still closed real routes and burned a cadence slot, at a moment nothing
// was expecting hardware activity (the gate-closed abort path - the guide
// aborts while the scan core is refusing to service).
//
// GUARDED on a request actually being in flight: guideCheckBegin() calls
// guideCheckAbort() unconditionally to clear any prior run, so an
// unconditional flag-set would arm a cancel that ate the NEXT check's first
// tap - one wasted cadence interval per check, systematically.
void cancelOneShotTap(void) {
    if (oneShotReqSeq == oneShotDoneSeq) return; // nothing pending to cancel
    oneShotCancel = true;
    __dmb();
}

// Serviced at the head of serviceNetVoltageScan(), BEFORE the enable/menu/
// user-input gates: a one-shot IS user-requested work (the guide is waiting
// on it), bounded at one tap per pass. The hardware-safety gates stay - they
// exist for route/ADC integrity, not politeness. Shares lastScanUs with the
// scheduled scan so the two tap kinds interleave at the same cadence.
static void serviceOneShotTap(void) {
    if (oneShotReqSeq == oneShotDoneSeq) return; // nothing pending
    // Cancel BEFORE the tap starts (cancelOneShotTap, core 0): consume the
    // request without touching hardware and retire the sequence so the
    // mailbox is free for the next owner. -2 is the poller's documented
    // transient code, so the one race that can still happen - a cancel
    // arriving while this pass already ran - costs the caller at most one
    // benign retry. Not counted in tapOneShot: no tap was serviced.
    if (oneShotCancel) {
        oneShotCancel = false;
        oneShotPairPhase = 0;
        oneShotCode = -2;
        __dmb();
        oneShotDoneSeq = oneShotReqSeq;
        return;
    }
    if (core1FramesHeld()) return;
    if (!core1req::allIdle()) return;
    if (core1busy || refreshInProgress) return;
#if USB_AUDIO_ENABLE
    if (usbAudioOwnsAdc) return;
#endif
    if (probeActive != 0) return;
    if (wavegen.isRunning()) return;
    if (micros() - lastScanUs < kScanIntervalUs) return;

    core2busy = true;
    lastScanUs = micros();
    __dmb(); // request payload visible before we read it
    int n1 = oneShotN1, n2 = oneShotN2;
    bool isPair = oneShotIsPair;
    int prefer = oneShotPreferAdc;
    bool complete = true;
    int8_t code = -2;

    // A fresh window for the route-failure latch: anything left over from the
    // background scan's taps must not be attributed to this one-shot.
    lastRouteFailNode = -1;
    lastRouteFailAdc = -1;
    lastRouteFailRc = 0;
    lastRouteFailKfree = 0;
    lastRouteFailKxbusy = 0;
    lastRouteFailChip = -1;
    lastRouteFailXbusy = 0;
    lastRouteFailBounceOk = 0;

    // Honor the caller's preferred channel first (exact offset/gain
    // cancellation when two readings get subtracted - see the header), then
    // fall back to any free channel. A hint is never a reservation: nothing
    // is held between passes, so the channel can legitimately be gone.
    int adcA = -1;
    if (prefer >= 0 && prefer < 4) {
        adcA = infraAcquireAdc(INFRA_ADC_NVSCAN, (uint8_t)(1u << prefer), true);
    }
    if (adcA < 0) adcA = infraAcquireAdc(INFRA_ADC_NVSCAN, 0x0F, true);
    if (adcA >= 0) {
        if (!isPair) {
            uint32_t driftBefore = tapFailDrift;
            float v;
            if (senseNodeVoltage(n1, adcA, &v)) {
                oneShotV1 = v - scanZeroOffset[adcA];
                code = 1;
            } else {
                code = (tapFailDrift != driftBefore) ? -1 : -2;
            }
            oneShotDrift1 = (n1 > 0 && n1 < NODE_VOLTAGE_MAX) ? nodeDrift[n1] : 0.0f;
        } else {
            // Single-dwell differential when the ring is live and a second
            // channel frees up; otherwise one node per pass (see above).
            bool pairDone = false;
            if (oneShotPairPhase == 0 && adcRingActive() &&
                !oneShotPairRouteFailed) {
                int adcB = infraAcquireAdc(INFRA_ADC_NVSCAN,
                                           (uint8_t)(0x0F & ~(1u << adcA)), true);
                if (adcB >= 0) {
                    uint32_t driftBefore = tapFailDrift;
                    float va, vb;
                    if (pairSenseTap(n1, n2, adcA, adcB, &va, &vb)) {
                        oneShotV1 = va - scanZeroOffset[adcA];
                        oneShotV2 = vb - scanZeroOffset[adcB];
                        code = 1;
                        pairDone = true;
                    } else if (tapFailDrift != driftBefore) {
                        code = -1;          // floating - a real verdict, deliver it
                        pairDone = true;
                    } else {
                        // Option 0 (invest-vf-noroute.md §8): the pair needs
                        // TWO simultaneous disjoint sense routes and just
                        // proved it cannot get them. Latch, and retire the
                        // pass WITHOUT a result - the next pass takes the
                        // one-node-per-pass path, which needs one route at a
                        // time. Deferring to the next pass matters: the failed
                        // pairSenseTap already spent its connect/unwind time,
                        // and the ~11ms per-pass budget is held by never
                        // running two taps in one pass.
                        oneShotPairRouteFailed = true;
                        complete = false;
                        pairDone = true;
                    }
                }
            }
            if (!pairDone) {
                uint32_t driftBefore = tapFailDrift;
                float v;
                int node = (oneShotPairPhase == 0) ? n1 : n2;
                if (senseNodeVoltage(node, adcA, &v)) {
                    v -= scanZeroOffset[adcA];
                    if (oneShotPairPhase == 0) {
                        oneShotPairV1 = v;
                        oneShotPairPhase = 1;
                        complete = false; // n2 taps on the next pass
                    } else {
                        oneShotV1 = oneShotPairV1;
                        oneShotV2 = v;
                        code = 1;
                    }
                } else {
                    code = (tapFailDrift != driftBefore) ? -1 : -2;
                }
            }
        }
    }
    infraReleaseAdc(INFRA_ADC_NVSCAN); // releases every channel we hold
    core2busy = false;

    if (complete) {
        tapOneShot++;
        oneShotAdcUsed = (int8_t)adcA;
        if (code == -2) {
            // Copy the at-refusal snapshot out of the scan-core statics NOW,
            // while nothing else has run on this core. node == -1 here means
            // no fastConnectPath was refused at all - then -2 came from ADC
            // exhaustion (adcA < 0) or a stale ring window, which is exactly
            // the #1-vs-#2 discriminator of report §6.
            oneShotFailNode = (int16_t)lastRouteFailNode;
            oneShotFailAdc = (int8_t)lastRouteFailAdc;
            oneShotFailRc = (int8_t)lastRouteFailRc;
            oneShotFailKfree = lastRouteFailKfree;
            oneShotFailKxbusy = lastRouteFailKxbusy;
            oneShotFailChip = (int8_t)lastRouteFailChip;
            oneShotFailXbusy = lastRouteFailXbusy;
            oneShotFailBounceOk = lastRouteFailBounceOk;
        }
        oneShotCode = code;
        __dmb(); // result payload lands before the done stamp
        oneShotDoneSeq = oneShotReqSeq;
    }
}

// ============================================================================
// Scan list / known sources
// ============================================================================

static bool isKnownSourceNode(int node) {
    return node == GND || node == TOP_RAIL || node == BOTTOM_RAIL ||
           node == DAC0 || node == DAC1;
}

static bool isScannable(int node) {
    if (node <= 0 || node >= NODE_VOLTAGE_MAX) return false;
    if (isKnownSourceNode(node)) return false;
    if (node >= ADC0 && node <= ADC4) return false; // filled from adcReadings[]
    if (node == ADC7_PROBE) return false;           // hardwired to the buffer
    return true;
}

// A path endpoint the pair tap may close a sense route onto: any scannable
// user node, the rails/DACs (measured sources), and - flashy-ants session,
// 2026-08-24 - GND itself. Kevin's ruling: the ADCs are high-impedance, so
// a momentary sense stub onto a live ground costs nothing, and the
// GND-ended paths were EXACTLY the flip fingerprint (every flipping path
// was single-ended; every pair-tapped path was rock solid across four i!
// captures). The measured ground potential rides in pathPairDv only;
// nodeVoltage[GND] stays pinned at 0 (recordTapVoltage skips GND). ADC
// endpoints still measure themselves through adcReadings[].
static bool pairTapEligible(int node) {
    if (node <= 0 || node >= NODE_VOLTAGE_MAX) return false;
    if (node >= ADC0 && node <= ADC4) return false;
    if (node == ADC7_PROBE) return false;
    return true;
}

// Full path count including the stacked duplicate entries fillUnusedPaths()
// appends (duplicate == 1). globalState.connections.numPaths only counts the
// originals; sendPaths() and we both need the whole set.
static int livePathCount() {
    int n = numberOfPaths;
    if (n > MAX_BRIDGES) n = MAX_BRIDGES;
    if (n < 0) n = 0;
    return n;
}

static uint32_t connectionsFingerprint() {
    int numPaths = livePathCount();
    uint32_t f = (uint32_t)globalState.connections.numNets * 7919u +
                 (uint32_t)numPaths * 104729u;
    for (int i = 0; i < numPaths; i++) {
        const pathStruct& p = globalState.connections.paths[i];
        f = f * 31u + (uint32_t)(p.node1 * 3 + p.node2 * 5 + p.net * 7);
    }
    return f;
}

static void rebuildScanList() {
    scanNodeCount = 0;
    gndInUse = false;
    for (int i = 0; i < 5; i++) adcNodePresent[i] = false;
    for (int i = 0; i < 4; i++) sourceInUse[i] = false;

    for (int i = 1; i < MAX_NETS; i++) {
        netStruct& net = globalState.connections.nets[i];
        if (net.number <= 0 || net.specialFunction == EMPTY_NET) continue;
        if (net.virtual_net) continue;
        for (int n = 0; n < MAX_NODES; n++) {
            int node = net.nodes[n];
            if (node <= 0) break;
            if (node == GND) gndInUse = true;
            if (node >= ADC0 && node <= ADC4) adcNodePresent[node - ADC0] = true;
            for (int k = 0; k < 4; k++) {
                if (node == kSourceNodes[k]) sourceInUse[k] = true;
            }
            if (!isScannable(node)) continue;
            bool dup = false;
            for (int k = 0; k < scanNodeCount; k++) {
                if (scanNodes[k] == node) {
                    dup = true;
                    break;
                }
            }
            if (!dup && scanNodeCount < kMaxScanNodes) {
                scanNodes[scanNodeCount++] = node;
            }
        }
    }
    if (scanIndex >= scanNodeCount) scanIndex = 0;
    nodeRetryCount = 0;   // new scan list: no retry debt carries over
}

// The commanded voltage for source k (kSourceNodes order). What
// fillKnownSources() used exclusively before the sources were measured.
static float sourceSetpoint(int k) {
    switch (k) {
    case 0: return globalState.power.topRail;
    case 1: return globalState.power.bottomRail;
    case 2: return globalState.power.dac0;
    default: return globalState.power.dac1;
    }
}

static void fillKnownSources() {
    uint32_t ms = millis();
    nodeVoltage[GND] = 0.0f;
    nodeVoltageMs[GND] = ms;
    // Rails/DACs: prefer the fresh MEASURED value (tapped through the same
    // sense path as every other node, so per-channel systematics cancel in
    // the dv), falling back to the setpoint when the measurement is stale
    // or the user has since changed the voltage.
    for (int k = 0; k < 4; k++) {
        float setpoint = sourceSetpoint(k);
        float v = setpoint;
        if (sourceMeasuredMs[k] != 0 &&
            (ms - sourceMeasuredMs[k]) < kSourceMeasureFreshMs &&
            fabsf(sourceSetpointAtMeasure[k] - setpoint) < 0.01f) {
            v = sourceMeasured[k];
        }
        nodeVoltage[kSourceNodes[k]] = v;
        nodeVoltageMs[kSourceNodes[k]] = ms;
    }
    // ADCs the user bridged into nets already measure their node directly;
    // updateLazyAdcReadings() keeps adcReadings[] fresh on this core.
    for (int a = 0; a <= 4; a++) {
        if (adcNodePresent[a]) {
            nodeVoltage[ADC0 + a] = adcReadings[a];
            nodeVoltageMs[ADC0 + a] = ms;
        }
    }
}

static inline bool voltageFresh(int node, uint32_t ms);

// Route a fresh (already zero-corrected) tap value to wherever this node's
// voltage lives: rails/DACs to the measured-source slot (fillKnownSources
// rewrites their nodeVoltage[] from it every pass), everything else to the
// per-node EMA. Smoothing matches on both branches: alpha 0.3 while the
// previous value is fresh, seed otherwise.
static void recordTapVoltage(int node, float volts) {
    // GND stays PINNED at 0 (fillKnownSources re-asserts it every pass): a
    // pair tap may MEASURE ground now (see pairTapEligible), but its value
    // rides in pathPairDv only - writing it here would wobble the zero
    // reference every non-pair consumer leans on.
    if (node == GND) return;
    uint32_t ms = millis();
    for (int k = 0; k < 4; k++) {
        if (node != kSourceNodes[k]) continue;
        if (sourceMeasuredMs[k] != 0 &&
            (ms - sourceMeasuredMs[k]) < kSourceMeasureFreshMs &&
            fabsf(sourceSetpointAtMeasure[k] - sourceSetpoint(k)) < 0.01f) {
            volts = sourceMeasured[k] + 0.3f * (volts - sourceMeasured[k]);
        }
        sourceMeasured[k] = volts;
        sourceSetpointAtMeasure[k] = sourceSetpoint(k);
        sourceMeasuredMs[k] = ms;
        sourceAttemptMs[k] = ms; // a pair tap counts as this source's refresh
        return;
    }
    if (node <= 0 || node >= NODE_VOLTAGE_MAX) return;
    if (voltageFresh(node, ms)) {
        nodeVoltage[node] += 0.3f * (volts - nodeVoltage[node]);
    } else {
        nodeVoltage[node] = volts;
    }
    nodeVoltageMs[node] = ms;
}

// ============================================================================
// Per-path current
// ============================================================================

static inline bool voltageFresh(int node, uint32_t ms) {
    return nodeVoltageMs[node] != 0 && (ms - nodeVoltageMs[node]) < kVoltageFreshMs;
}

static int pathCrosspoints(const pathStruct& p) {
    int count = 0;
    for (int h = 0; h < 4; h++) {
        if (p.chip[h] != -1 && p.x[h] >= 0 && p.y[h] >= 0) count++;
    }
    return count;
}

static volatile uint32_t computeGeneration = 0;

static void computePathCurrents() {
    computeGeneration = computeGeneration + 1;
    for (int i = 0; i < MAX_NETS; i++) netCurrentInfo[i].valid = false;

    int numPaths = livePathCount();
    float rXpoint = jumperlessConfig.calibration.crosspoint_resistance;
    if (rXpoint < 1.0f) rXpoint = 1.0f;
    uint32_t ms = millis();

    for (int i = 0; i < numPaths; i++) {
        pathCurrentValid[i] = false;
        const pathStruct& p = globalState.connections.paths[i];
        if (p.skip || p.net <= 0 || p.net >= MAX_NETS) continue;
        if (p.pathType == VIRTUAL) continue;
        if (p.duplicate != 0) continue; // stacked copies fold into the main path
        int n1 = p.node1, n2 = p.node2;
        if (n1 <= 0 || n1 >= NODE_VOLTAGE_MAX) continue;
        if (n2 <= 0 || n2 >= NODE_VOLTAGE_MAX) continue;
        if (!voltageFresh(n1, ms) || !voltageFresh(n2, ms)) continue;
        int xp = pathCrosspoints(p);
        if (xp <= 0) continue;

        // Total conductance of this connection = the main path in parallel
        // with its stacked duplicates (fillUnusedPaths appends them with
        // duplicate == 1 and the same node pair).
        // ponytail: O(paths^2) duplicate lookup, fine at MAX_BRIDGES=128;
        // index duplicates by node pair if this ever grows.
        float g = 1.0f / ((float)xp * rXpoint);
        for (int j = 0; j < numPaths; j++) {
            const pathStruct& q = globalState.connections.paths[j];
            if (q.duplicate == 1 && q.net == p.net && !q.skip &&
                ((q.node1 == n1 && q.node2 == n2) ||
                 (q.node1 == n2 && q.node2 == n1))) {
                int xq = pathCrosspoints(q);
                if (xq > 0) g += 1.0f / ((float)xq * rXpoint);
            }
        }

        float v1 = nodeVoltage[n1];
        float v2 = nodeVoltage[n2];
        float dv = v1 - v2;
        // Pair-tapped paths carry a better delta: both ends read from the
        // same ring sweeps in one dwell, instead of two round-robin samples
        // taken up to a full scan cycle apart on different channels.
        if (pathPairDvMs[i] != 0 && (ms - pathPairDvMs[i]) < kVoltageFreshMs) {
            dv = pathPairDv[i];
        }
        // Smooth the RAW current estimate - no deadband before the filter.
        // The old pre-EMA hard deadband zeroed dv below 35mV, so a reading
        // parked AT the cliff toggled the filter input between 0 and the
        // full value on every pass and the EMA wandered through the ants'
        // on/off band - the "sparkle". EMA alpha 0.25 at 20Hz converges in
        // ~200ms - fast enough to track real changes, slow enough to iron
        // out shot noise.
        float i_mA = dv * g * 1000.0f;
        if (pathEmaSeeded[i]) {
            i_mA = pathCurrent_mA[i] + 0.25f * (i_mA - pathCurrent_mA[i]);
        }
        pathEmaSeeded[i] = true;
        pathCurrent_mA[i] = i_mA;
        pathCurrentValid[i] = true;

        // Deadband AFTER smoothing, per path, with hysteresis. The noise
        // floor is constant in VOLTS (ADC jitter + residual bias), so the
        // current cutoff scales with the path's conductance - the same
        // 35mV that used to gate dv, now applied to the smoothed current.
        // Enter at 1.25x, drop at 0.75x: every borderline case latches
        // solid-on or solid-off, never toggling.
        float iDead_mA = 0.035f * g * 1000.0f;
        if (pathShownOn[i]) {
            if (fabsf(i_mA) < iDead_mA * 0.75f) pathShownOn[i] = false;
        } else {
            if (fabsf(i_mA) > iDead_mA * 1.25f) pathShownOn[i] = true;
        }
        pathShown_mA[i] = pathShownOn[i] ? i_mA : 0.0f;

        NetCurrentInfo& info = netCurrentInfo[p.net];
        if (!info.valid || fabsf(pathShown_mA[i]) > info.current_mA) {
            info.valid = true;
            info.current_mA = fabsf(pathShown_mA[i]);
            info.voltage = 0.5f * (v1 + v2);
            if (dv >= 0) { // same delta the current came from (pair or nodes)
                info.fromNode = n1;
                info.toNode = n2;
            } else {
                info.fromNode = n2;
                info.toNode = n1;
            }
        }
    }
}

// ============================================================================
// Debug print
// ============================================================================

static int pickScanAdc();
static bool anySourceInUse(void);

// Print one node's current sense route (dry run of the same builder the
// taps use, against live occupancy).
static void printSenseRoute(int node, int adc, Stream* out) {
    out->printf("[nvscan] route %d:", node);
    pathStruct r;
    if (!planFastPath(node, ADC0 + adc, &r)) {
        out->println(" (none - lanes busy, short, or node unknown)");
        return;
    }
    for (int h = 0; h < 4; h++) {
        if (r.chip[h] < 0 || r.x[h] < 0 || r.y[h] < 0) continue;
        out->printf(" %c(x%d,y%d)", 'A' + r.chip[h], r.x[h], r.y[h]);
    }
    out->printf(" -> ADC%d\n", adc);
}

// The tap tallies alone, no config gate: a guide check's hard tap failure
// needs these whether or not the user has the current display on.
void printTapCounters(Stream* out) {
    if (!out) out = &Serial;
    int m = jumperlessConfig.debug.net_scan_pair_taps;
    out->printf("[nvscan] taps ok:%lu (pair:%lu seqpair:%lu asym:%lu mode:%s) noroute:%lu adcbusy:%lu drift:%lu ringstale:%lu oneshot:%lu\n",
                tapOk, tapPairOk, tapSeqPairOk, tapPairAsym,
                m == 2 ? "pair" : (m == 1 ? "seq" : "off"),
                tapFailNoRoute, tapFailAdcBusy,
                tapFailDrift, tapFailRingStale, tapOneShot);
}

static void printScanStats(Stream* out) {
    uint32_t ms = millis();
    out->print("[nvscan] nodes:");
    for (int k = 0; k < scanNodeCount; k++) {
        int node = scanNodes[k];
        out->printf(" %d=", node);
        if (voltageFresh(node, ms)) {
            out->printf("%.2fV(d%+.2f)", nodeVoltage[node], nodeDrift[node]);
        } else {
            out->printf("stale(d%+.2f)", nodeDrift[node]);
        }
    }
    out->println();
    if (anySourceInUse()) {
        out->print("[nvscan] sources:");
        for (int k = 0; k < 4; k++) {
            if (!sourceInUse[k]) continue;
            out->printf(" %d=set%.2fV", kSourceNodes[k], sourceSetpoint(k));
            if (sourceMeasuredMs[k] != 0 &&
                millis() - sourceMeasuredMs[k] < kSourceMeasureFreshMs) {
                out->printf("/meas%.2fV", sourceMeasured[k]);
            } else {
                out->print("/stale");
            }
        }
        out->println();
    }
    printTapCounters(out);
    // Sense routes as the taps would compute them right now (peek only -
    // a debug print must not take pool ownership)
    uint8_t freeMask = infraFreeAdcMask(0x0F);
    int adc = freeMask ? __builtin_ctz(freeMask) : -1;
    if (adc >= 0) {
        for (int k = 0; k < scanNodeCount; k++) {
            printSenseRoute(scanNodes[k], adc, out);
        }
        if (!gndInUse) {
            printSenseRoute(GND, adc, out); // the auto-zero tap
        }
    }
    // Crosspoint occupancy per chip (8 y rows of x bitmasks, hex)
    out->print("[nvscan] xy");
    for (int c = 0; c < 12; c++) {
        out->printf(" %c:", 'A' + c);
        for (int y = 0; y < 8; y++) {
            out->printf("%x,", lastChipXY[c].connected[y]);
        }
    }
    out->println();
    int numPaths = livePathCount();
    for (int i = 0; i < numPaths; i++) {
        const pathStruct& p = globalState.connections.paths[i];
        if (pathCurrentValid[i]) {
            // Shown (gated) value first - it is what the display and the HIL
            // zero-load check consume; the raw EMA rides along unsuffixed so
            // the mA regex can't match it. "pair" = the delta comes from a
            // same-dwell differential tap of both ends.
            bool pairFresh = pathPairDvMs[i] != 0 &&
                             (millis() - pathPairDvMs[i]) < kVoltageFreshMs;
            out->printf("[nvscan] path %d net %d  %d->%d  %dxp dup%d  %+.2f mA (ema %+.2f)%s\n",
                        i, p.net, p.node1, p.node2, pathCrosspoints(p),
                        p.duplicate, pathShown_mA[i], pathCurrent_mA[i],
                        pairFresh ? " pair" : "");
        } else if (p.net > 0 && !p.skip) {
            out->printf("[nvscan] path %d net %d  %d-%d  %dxp dup%d  (no data)\n",
                        i, p.net, p.node1, p.node2, pathCrosspoints(p),
                        p.duplicate);
        }
    }
    // Ant geometry self-check: every path's pixel sequence must be
    // physically continuous or the flow illusion is broken.
    printAntPathContinuity(out);
    printAntFlipStats(out);
    out->flush();
}

// ============================================================================
// Public API
// ============================================================================

bool nodeVoltageValid(int node) {
    if (node <= 0 || node >= NODE_VOLTAGE_MAX) return false;
    return voltageFresh(node, millis());
}

void printNetVoltageScanStats(Stream* out) {
    if (!jumperlessConfig.display.net_currents) {
        out->println("[nvscan] net current scan is off ('i' to enable)");
        return;
    }
    printScanStats(out);
}

float netCurrent_mA(int netIndex) {
    if (netIndex <= 0 || netIndex >= MAX_NETS) return 0.0f;
    if (!netCurrentInfo[netIndex].valid) return 0.0f;
    return netCurrentInfo[netIndex].current_mA;
}

bool pathCurrentKnown(int pathIndex) {
    if (pathIndex < 0 || pathIndex >= MAX_BRIDGES) return false;
    return pathCurrentValid[pathIndex];
}

uint32_t netScanComputeGeneration(void) {
    return computeGeneration;
}

float pathCurrentSigned_mA(int pathIndex) {
    if (!pathCurrentKnown(pathIndex)) return 0.0f;
    return pathShown_mA[pathIndex]; // deadband-gated: solid 0 below the band
}

static int pickScanAdc() {
    // Pool-arbitrated: ADC0-3, per-tap acquire (released right after the
    // tap so measure mode / TDM aren't starved between taps). TDM's channel
    // stays available as a shared last resort - both consumers tap
    // sequentially on this core and disconnect after each read.
    return infraAcquireAdc(INFRA_ADC_NVSCAN, 0x0F, true);
}

static bool anySourceInUse(void) {
    return sourceInUse[0] || sourceInUse[1] || sourceInUse[2] || sourceInUse[3];
}

// The next in-use source due a measurement tap: least-recently measured of
// those past the refresh interval. A setpoint change re-arms immediately -
// the old measurement no longer describes the new command (fillKnownSources
// already fell back to the setpoint the moment it moved).
static int dueSourceIndex(void) {
    int best = -1;
    uint32_t ms = millis();
    for (int k = 0; k < 4; k++) {
        if (!sourceInUse[k]) continue;
        if (sourceAttemptMs[k] != 0 &&
            (ms - sourceAttemptMs[k]) < kSourceMeasureIntervalMs) {
            continue; // attempted recently (success or not) - back off
        }
        bool setpointMoved =
            sourceMeasuredMs[k] != 0 &&
            fabsf(sourceSetpointAtMeasure[k] - sourceSetpoint(k)) >= 0.01f;
        if (sourceMeasuredMs[k] != 0 && !setpointMoved &&
            (ms - sourceMeasuredMs[k]) < kSourceMeasureIntervalMs) {
            continue;
        }
        if (best < 0 ||
            (int32_t)(sourceMeasuredMs[k] - sourceMeasuredMs[best]) < 0) {
            best = k;
        }
    }
    return best;
}

// Sequential same-channel pair: both ends tapped back-to-back on ONE ADC,
// a few ms apart (each dwell ~1.2ms) - versus SECONDS apart in the plain
// round-robin, which is what made the single-ended paths' ants flip while
// every true-pair path sat rock solid (the four-capture i! fingerprint).
// Channel offsets cancel in dv by construction (same ADC), and a same-chip
// pair - whose two routes contend for the chip's one SF lane, so a true
// two-channel pair cannot route - naturally gets the same route SHAPE for
// both ends: hop-matched per Kevin's ruling. This is also the scan's
// "single ADC when space is tight" mode (Kevin, 2026-08-24): it needs only
// ONE free chip-K row where a true pair needs two.
static bool seqPairTap(int pathIdx, int node1, int node2, int adcA) {
    float v1, v2;
    if (!senseNodeVoltage(node1, adcA, &v1)) return false;
    if (!senseNodeVoltage(node2, adcA, &v2)) return false;
    recordTapVoltage(node1, v1 - scanZeroOffset[adcA]);
    recordTapVoltage(node2, v2 - scanZeroOffset[adcA]);
    // dv straight from the raw pair: the shared channel's zero offset is
    // common to both ends and cancels in the subtraction.
    pathPairDv[pathIdx] = v1 - v2;
    pathPairDvMs[pathIdx] = millis();
    tapSeqPairOk++;
    return true;
}

// One pair-tap slot: the next path with a tappable end gets its tap - both
// ends at once on two channels when a second one is free, else a sequential
// same-channel pair, else the one tappable end single-ended (so the slot
// still gathers something). Returns false when no path qualifies and the
// caller should fall back to the plain node round-robin. adcA is already
// acquired by the caller; the second channel is freed with it by the
// caller's infraReleaseAdc (which releases every channel this user owns).
// Pair taps still run serially, one path per tap - a pair holds 2 of the 4
// routable channels for its ~1.2ms dwell, less than the full 0x0F mask a
// plain tap can acquire.
static bool pairTapSlot(int adcA) {
    int numPaths = livePathCount();
    if (numPaths <= 0) return false;
    for (int tries = 0; tries < numPaths; tries++) {
        int i = pairPathIndex % numPaths;
        pairPathIndex = (pairPathIndex + 1) % numPaths;
        const pathStruct& p = globalState.connections.paths[i];
        if (p.skip || p.net <= 0 || p.net >= MAX_NETS) continue;
        if (p.pathType == VIRTUAL || p.duplicate != 0) continue;
        bool e1 = pairTapEligible(p.node1);
        bool e2 = pairTapEligible(p.node2);
        if (!e1 && !e2) continue;
        if (e1 && e2) {
            // Mode 2 = true two-channel pair first (see the strategy legend
            // at pathPairDv). Mode 1 (default) skips straight to sequential.
            if (jumperlessConfig.debug.net_scan_pair_taps == 2) {
                int adcB = infraAcquireAdc(INFRA_ADC_NVSCAN,
                                           (uint8_t)(0x0F & ~(1u << adcA)), true);
                if (adcB >= 0) {
                    // Alternate which node rides which channel each pass so
                    // a per-channel gain mismatch averages out of the EMAs.
                    pairSwapPhase = !pairSwapPhase;
                    int a1 = pairSwapPhase ? adcB : adcA;
                    int a2 = pairSwapPhase ? adcA : adcB;
                    float v1, v2;
                    if (pairSenseTap(p.node1, p.node2, a1, a2, &v1, &v2)) {
                        v1 -= scanZeroOffset[a1];
                        v2 -= scanZeroOffset[a2];
                        recordTapVoltage(p.node1, v1);
                        recordTapVoltage(p.node2, v2);
                        pathPairDv[i] = v1 - v2;
                        pathPairDvMs[i] = millis();
                        return true;
                    }
                    // Pair refused (asymmetric hops, lane contention) or its
                    // dwell failed - sequential below still gets a coherent
                    // dv where single-ended could not.
                }
            }
            // The default path: sequential same-channel pair on adcA
            // (offsets cancel entirely, ~3ms end to end, one K row).
            if (seqPairTap(i, p.node1, p.node2, adcA)) return true;
            // Both pair shapes failed (route/drift) - single-ended below,
            // so a contended path still gets one node refreshed.
        }
        int node;
        if (e1 && e2) {
            pairSwapPhase = !pairSwapPhase;
            node = pairSwapPhase ? p.node1 : p.node2;
        } else {
            node = e1 ? p.node1 : p.node2;
        }
        float volts;
        if (senseNodeVoltage(node, adcA, &volts)) {
            recordTapVoltage(node, volts - scanZeroOffset[adcA]);
        }
        return true;
    }
    return false;
}

void serviceNetVoltageScan(void) {
    // Guide one-shot taps first: they serve even while the scan is disabled
    // or input/menu-paused (see the helper's comment for the gate rationale).
    serviceOneShotTap();

    static bool wasEnabled = false;
    if (!jumperlessConfig.display.net_currents) {
        if (wasEnabled) {
            // Leave nothing stale behind for the display consumers.
            memset(nodeVoltageMs, 0, sizeof(nodeVoltageMs));
            for (int i = 0; i < MAX_BRIDGES; i++) {
                pathCurrentValid[i] = false;
                pathEmaSeeded[i] = false;
                pathShownOn[i] = false;
                pathShown_mA[i] = 0.0f;
                pathPairDvMs[i] = 0;
            }
            for (int i = 0; i < MAX_NETS; i++) netCurrentInfo[i].valid = false;
            wasEnabled = false;
        }
        return;
    }
    wasEnabled = true;

    // Menus own the LEDs and the user's attention - pause completely so
    // core 2 iterates as fast as possible for clickwheel responsiveness.
    // Voltages go stale and rebuild within ~100ms of the menu closing.
    if (inClickMenu != 0 || inPadMenu != 0) return;

    // We run OUTSIDE the core_sync/core2busy LED window now (see the call
    // site after core_sync_release() in main.cpp), so gate ourselves:
    //   - the core-1 frame hold: core 0 wants core 1 quiesced (flash write / XIP erase)
    //   - a path send pending / in flight (core1req mailbox): a crossbar
    //     refresh is pending, so the connections we'd read are mid-change
    //   - core1busy / refreshInProgress: core 0 is mid-rebuild of the
    //     netlist/chipStates we read and whose xStatus lanes we claim. The
    //     100ms input preempt below misses MicroPython/app/script-driven
    //     refreshes, so check the flags directly.
    if (core1FramesHeld()) return;
    if (!core1req::allIdle()) return;
    if (core1busy || refreshInProgress) return;
#if USB_AUDIO_ENABLE
    // The USB microphone holds the ADC for as long as the host has it open,
    // and its round-robin sweep is useless for a 5 ms connect->settle->read
    // tap. Stand down entirely rather than sneaking taps into the few moments
    // the audio path lets go of the converter (config-save yields, resyncs) -
    // those are exactly the moments the ADC is being reconfigured, and a
    // sample taken there fed "fresh" phantom currents into the display.
    // Voltages age out through the normal freshness window; the scan picks
    // up again within one cycle of the mic closing.
    if (usbAudioOwnsAdc) return;
#endif

    // User input preempts the scan outright: the wheel/probe/serial always
    // win, and Core 0 work that follows input (refreshes, waitCore2) never
    // collides with a tap's core2busy window. Voltages go slightly stale
    // and catch up within a scan cycle once the user pauses.
    if ((uint32_t)(millis() - lastUserInputMs) < 100 ||
        isEncoderButtonPhysicallyPressed()) {
        return;
    }

    uint32_t fingerprint = connectionsFingerprint();
    if (fingerprint != lastFingerprint) {
        lastFingerprint = fingerprint;
        rebuildScanList();
        // Routing changed - old samples may describe merged/split nets.
        memset(nodeVoltageMs, 0, sizeof(nodeVoltageMs));
        for (int i = 0; i < MAX_BRIDGES; i++) {
            pathCurrentValid[i] = false;
            pathEmaSeeded[i] = false;
            pathShownOn[i] = false;
            pathShown_mA[i] = 0.0f;
            pathPairDvMs[i] = 0;
        }
        for (int i = 0; i < MAX_NETS; i++) netCurrentInfo[i].valid = false;
    }

    fillKnownSources();

    // One tap per pass. Skip while the probe owns raw crosspoint scanning or
    // the wavegen owns the ADC/DAC path.
    //
    // core2busy brackets ONLY the tap (the crosspoint/ADC hardware work), not
    // the bookkeeping above/below: raising it for the whole service body put
    // the flag at a high duty cycle on every core-1 pass, which starved
    // OledGui::renderNow()'s core2busy gate and made pauseCore2ForFlash()/
    // waitCore2() waiters burn their timeouts. pauseCore2ForFlash still sees
    // the tap window (same contract as updateLazyAdcReadings); the pure-CPU
    // bookkeeping needs no flag - idleOtherCore() is what actually parks this
    // core for flash safety.
    if ((scanNodeCount > 0 || anySourceInUse()) && probeActive == 0 &&
        !wavegen.isRunning() && micros() - lastScanUs > kScanIntervalUs) {
        core2busy = true;
        lastScanUs = micros();
        int adc = pickScanAdc();
        if (adc >= 0) {
            int srcDue;
            // Refresh this channel's zero offset (a tap on GND) every second,
            // but NEVER touch GND while a user circuit is connected to it -
            // the last captured offset stays in use (it's a near-static ADC
            // bias, typically grabbed once at boot with an empty board).
            if (!gndInUse && millis() - scanZeroMs[adc] > 1000) {
                float zero;
                if (senseNodeVoltage(GND, adc, &zero)) {
                    scanZeroOffset[adc] = zero;
                    scanZeroMs[adc] = millis();
                }
            } else if ((srcDue = dueSourceIndex()) >= 0) {
                // A rail/DAC measurement tap - same sense path, same zero
                // offset as the user nodes, so fillKnownSources' measured
                // values cancel the channel's systematics in every dv.
                // (Pair taps refresh most in-use sources on their own; this
                // slot covers the ones they can't reach.)
                sourceAttemptMs[srcDue] = millis();
                float volts;
                if (senseNodeVoltage(kSourceNodes[srcDue], adc, &volts)) {
                    recordTapVoltage(kSourceNodes[srcDue], volts - scanZeroOffset[adc]);
                }
            } else if (jumperlessConfig.debug.net_scan_pair_taps != 0 &&
                       adcRingActive() && pairTapSlot(adc)) {
                // Pair-tap slot took the pass: one path's ends were tapped
                // together (or its single tappable end, when a second
                // channel/route wasn't available).
            } else if (scanNodeCount > 0) {
                int node = scanNodes[scanIndex];
                uint32_t busyBefore = tapFailAdcBusy;
                float volts;
                bool ok = senseNodeVoltage(node, adc, &volts);
                if (ok) {
                    // Smoothing lives in recordTapVoltage: shot-to-shot ADC
                    // noise is +/-20mV, whole milliamps over a 30-ohm path.
                    recordTapVoltage(node, volts - scanZeroOffset[adc]);
                    nodeRetryCount = 0;
                    scanIndex = (scanIndex + 1) % scanNodeCount;
                } else if (tapFailAdcBusy != busyBefore) {
                    // Channel contention, not the node's fault: retry next
                    // pass without burning a retry.
                } else if (++nodeRetryCount >= 2) {
                    // Ant coherence (flashy-ants session, fix 3): a failed
                    // tap used to wait a FULL round-robin cycle for its next
                    // try, leaving its paths stale while siblings refreshed
                    // - per-path ant churn desync was the visible result.
                    // One immediate retry bounds the age spread; a node that
                    // fails twice running (a genuinely floating leg
                    // drift-fails forever, by design) yields so it cannot
                    // starve the rest of its net.
                    nodeRetryCount = 0;
                    scanIndex = (scanIndex + 1) % scanNodeCount;
                }
            }
        }
        infraReleaseAdc(INFRA_ADC_NVSCAN); // per-tap: don't hold between taps
        core2busy = false;
    }

    // The duplicate-folding pass is O(paths^2) - up to ~0.5ms on a full
    // board. 20Hz is plenty for ants/highlight display, so don't pay it on
    // every core 2 iteration.
    static unsigned long lastComputeMs = 0;
    if (millis() - lastComputeMs >= 50) {
        lastComputeMs = millis();
        computePathCurrents();
    }

    // debug.net_voltage_scan printing happens on core 0 (see
    // serviceNetVoltageScanDebug) - Serial I/O from this core races core 0's
    // USB servicing and can wedge the board.
}

// Core 0 only: the once-a-second debug.net_voltage_scan report. Reads
// scanner state written on core 1 - torn values are cosmetic, and the route
// dry-runs (planFastPath) are read-only.
void serviceNetVoltageScanDebug(void) {
    if (!jumperlessConfig.debug.net_voltage_scan) return;
    if (!jumperlessConfig.display.net_currents) return;
    static unsigned long lastDebugPrintMs = 0;
    if (millis() - lastDebugPrintMs < 1000) return;
    lastDebugPrintMs = millis();
    printScanStats(&Serial);
}

#else // OG_JUMPERLESS: single-LED rows and a different fabric - feature off

float nodeVoltage[NODE_VOLTAGE_MAX];
uint32_t nodeVoltageMs[NODE_VOLTAGE_MAX];
NetCurrentInfo netCurrentInfo[NET_CURRENT_INFO_COUNT];

bool nodeVoltageValid(int) { return false; }
float netCurrent_mA(int) { return 0.0f; }
bool pathCurrentKnown(int) { return false; }
float pathCurrentSigned_mA(int) { return 0.0f; }
uint32_t netScanComputeGeneration(void) { return 0; }
// One-shot taps are a V5 primitive (the OG has no scanner fabric here);
// refusing the request tells the caller immediately, and the guide checks
// report themselves unsupported on OG before ever asking.
bool requestNodeTap(int, int) { return false; }
int nodeTapResult(float*, float*, int* adcUsed) {
    if (adcUsed) *adcUsed = -1;
    return -2;
}
bool requestPairTap(int, int) { return false; }
int pairTapResult(float*, float*) { return -2; }
void oneShotTapFailInfo(OneShotTapFail* info) {
    if (info) *info = OneShotTapFail();
}
void cancelOneShotTap(void) {}   // nothing can be in flight here
void serviceNetVoltageScan(void) {}
void serviceNetVoltageScanDebug(void) {}
void printNetVoltageScanStats(Stream* out) { out->println("net current scan: V5 only"); }
void printTapCounters(Stream* out) { out->println("net current scan: V5 only"); }

#endif // OG_JUMPERLESS
