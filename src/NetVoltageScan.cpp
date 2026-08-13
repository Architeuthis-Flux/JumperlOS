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

#ifndef OG_JUMPERLESS

#include "CH446Q.h"
#include "FakeGpio.h" // tdmInputs (to avoid stealing the FakeGPIO ADC)
#include "NetsToChipConnections.h" // numberOfPaths (includes duplicate paths)
#include "MatrixState.h"
#include "Peripherals.h"
#include "RotaryEncoder.h" // encoderServiceYield() - serviced during our waits
#include "InfraPaths.h"
#include "RouteSafety.h"
#include "States.h"
#include "externVars.h" // pauseCore2 / core2busy / lastUserInputMs
#include "WaveGen.h"
#include "config.h"
#include "hardware/adc.h"

extern WaveGen wavegen;
extern volatile bool refreshInProgress; // Commands.h
extern volatile int probeActive;
extern int& inClickMenu;          // Menus.h
extern volatile int& inPadMenu;   // Probing.h
// Defined in Graphics.cpp - ant geometry continuity check for the report
extern void printAntPathContinuity(Stream* out);

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
static float pathCurrent_mA[MAX_BRIDGES];
static bool pathCurrentValid[MAX_BRIDGES];

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

// Tap failure tallies for the debug print (why nodes go stale)
static uint32_t tapFailNoRoute = 0;
static uint32_t tapFailAdcBusy = 0;
static uint32_t tapFailDrift = 0;
static uint32_t tapOk = 0;

// ============================================================================
// Hang-proof ADC read
// ============================================================================
//
// readAdc()'s adc_read() busy-waits with NO timeout, and its 100ms lock-steal
// means two cores can end up driving the ADC state machine at once - which
// corrupts it and hangs adc_read() forever (this wedged the whole board in
// testing). This variant is conversion-only: the CALLER must hold the
// readingADC lock. Each conversion is manually triggered with a microsecond
// timeout so a collision costs one discarded sample, not a core.

static bool readScanAdcVoltage(int channel, int samples, float* volts) {
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
// gets bumped by crosspoint switching injection and then decays through the
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
    // NEVER steal the lock: that's how the ADC state machine gets corrupted
    // and hangs a core. Core 0's probe poller reads almost back-to-back; a
    // ~2ms window catches the gap between its reads.
    unsigned long waitStart = micros();
    while (__atomic_test_and_set(&readingADC, __ATOMIC_ACQUIRE)) {
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
    // ~1ms reads + 4x1ms disconnect ~= 11ms hard worst case.
    int rc = fastConnectPath(node, adcNode, &handle, 500);
    if (rc != 0) {
        __atomic_clear(&readingADC, __ATOMIC_RELEASE);
        tapFailNoRoute++;
        return false;
    }

    waitServicingEncoder(80); // CH446Q settle (same value TDM landed on)
    float early = 0.0f, late = 0.0f;
    bool ok = readScanAdcVoltage(adc, 4, &early);
    if (ok) {
        waitServicingEncoder(250); // give a floating net time to visibly decay
        ok = readScanAdcVoltage(adc, 4, &late);
    }
    fastDisconnectPath(&handle);
    __atomic_clear(&readingADC, __ATOMIC_RELEASE);
    if (!ok) {
        tapFailAdcBusy++;
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

    for (int i = 1; i < MAX_NETS; i++) {
        netStruct& net = globalState.connections.nets[i];
        if (net.number <= 0 || net.specialFunction == EMPTY_NET) continue;
        if (net.virtual_net) continue;
        for (int n = 0; n < MAX_NODES; n++) {
            int node = net.nodes[n];
            if (node <= 0) break;
            if (node == GND) gndInUse = true;
            if (node >= ADC0 && node <= ADC4) adcNodePresent[node - ADC0] = true;
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
}

static void fillKnownSources() {
    uint32_t ms = millis();
    nodeVoltage[GND] = 0.0f;
    nodeVoltageMs[GND] = ms;
    nodeVoltage[TOP_RAIL] = globalState.power.topRail;
    nodeVoltageMs[TOP_RAIL] = ms;
    nodeVoltage[BOTTOM_RAIL] = globalState.power.bottomRail;
    nodeVoltageMs[BOTTOM_RAIL] = ms;
    nodeVoltage[DAC0] = globalState.power.dac0;
    nodeVoltageMs[DAC0] = ms;
    nodeVoltage[DAC1] = globalState.power.dac1;
    nodeVoltageMs[DAC1] = ms;
    // ADCs the user bridged into nets already measure their node directly;
    // updateLazyAdcReadings() keeps adcReadings[] fresh on this core.
    for (int a = 0; a <= 4; a++) {
        if (adcNodePresent[a]) {
            nodeVoltage[ADC0 + a] = adcReadings[a];
            nodeVoltageMs[ADC0 + a] = ms;
        }
    }
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

static void computePathCurrents() {
    for (int i = 0; i < MAX_NETS; i++) netCurrentInfo[i].valid = false;

    int numPaths = livePathCount();
    float rXpoint = jumperlessConfig.calibration.crosspoint_resistance;
    if (rXpoint < 1.0f) rXpoint = 1.0f;
    uint32_t ms = millis();

    for (int i = 0; i < numPaths; i++) {
        bool wasValid = pathCurrentValid[i];
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
        // Deadband: below the resolvable voltage delta the "current" is just
        // measurement noise (a floating net parked at the ADC bias reads
        // +/-20mV of jitter, which would fake fractional mA over a stacked
        // low-resistance path). Report a solid 0 instead.
        float dv = v1 - v2;
        if (fabsf(dv) < 0.035f) dv = 0.0f;
        float i_mA = dv * g * 1000.0f;
        // Smooth across recomputes: near the dv deadband the raw value
        // toggles between 0 and the real current on every pass (the node
        // voltages refresh round-robin), which made the current ants and
        // the highlight readout flicker. EMA alpha 0.25 at 20Hz converges
        // in ~200ms - fast enough to track real changes, slow enough to
        // iron out the toggle.
        if (wasValid) {
            i_mA = pathCurrent_mA[i] + 0.25f * (i_mA - pathCurrent_mA[i]);
        }
        pathCurrent_mA[i] = i_mA;
        pathCurrentValid[i] = true;

        NetCurrentInfo& info = netCurrentInfo[p.net];
        if (!info.valid || fabsf(i_mA) > info.current_mA) {
            info.valid = true;
            info.current_mA = fabsf(i_mA);
            info.voltage = 0.5f * (v1 + v2);
            if (v1 >= v2) {
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
    out->printf("[nvscan] taps ok:%lu noroute:%lu adcbusy:%lu drift:%lu\n",
                tapOk, tapFailNoRoute, tapFailAdcBusy, tapFailDrift);
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
            out->printf("[nvscan] path %d net %d  %d->%d  %dxp dup%d  %+.2f mA\n",
                        i, p.net, p.node1, p.node2, pathCrosspoints(p),
                        p.duplicate, pathCurrent_mA[i]);
        } else if (p.net > 0 && !p.skip) {
            out->printf("[nvscan] path %d net %d  %d-%d  %dxp dup%d  (no data)\n",
                        i, p.net, p.node1, p.node2, pathCrosspoints(p),
                        p.duplicate);
        }
    }
    // Ant geometry self-check: every path's pixel sequence must be
    // physically continuous or the flow illusion is broken.
    printAntPathContinuity(out);
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

float pathCurrentSigned_mA(int pathIndex) {
    if (!pathCurrentKnown(pathIndex)) return 0.0f;
    return pathCurrent_mA[pathIndex];
}

static int pickScanAdc() {
    // Pool-arbitrated: ADC0-3, per-tap acquire (released right after the
    // tap so measure mode / TDM aren't starved between taps). TDM's channel
    // stays available as a shared last resort - both consumers tap
    // sequentially on this core and disconnect after each read.
    return infraAcquireAdc(INFRA_ADC_NVSCAN, 0x0F, true);
}

void serviceNetVoltageScan(void) {
    static bool wasEnabled = false;
    if (!jumperlessConfig.display.net_currents) {
        if (wasEnabled) {
            // Leave nothing stale behind for the display consumers.
            memset(nodeVoltageMs, 0, sizeof(nodeVoltageMs));
            for (int i = 0; i < MAX_BRIDGES; i++) pathCurrentValid[i] = false;
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
    //   - pauseCore2: core 0 wants core 1 quiesced (flash write / XIP erase)
    //   - sendAllPathsCore2: a full crossbar refresh is pending, so the
    //     connections we'd read are mid-change on core 0
    //   - core1busy / refreshInProgress: core 0 is mid-rebuild of the
    //     netlist/chipStates we read and whose xStatus lanes we claim. The
    //     100ms input preempt below misses MicroPython/app/script-driven
    //     refreshes, so check the flags directly.
    if (pauseCore2) return;
    if (sendAllPathsCore2 != 0) return;
    if (core1busy || refreshInProgress) return;

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
        for (int i = 0; i < MAX_BRIDGES; i++) pathCurrentValid[i] = false;
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
    if (scanNodeCount > 0 && probeActive == 0 && !wavegen.isRunning() &&
        micros() - lastScanUs > kScanIntervalUs) {
        core2busy = true;
        lastScanUs = micros();
        int adc = pickScanAdc();
        if (adc >= 0) {
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
            } else {
                int node = scanNodes[scanIndex];
                // Advance round-robin unless the ADC was busy (retry then)
                uint32_t busyBefore = tapFailAdcBusy;
                float volts;
                bool ok = senseNodeVoltage(node, adc, &volts);
                if (ok || tapFailAdcBusy == busyBefore) {
                    scanIndex = (scanIndex + 1) % scanNodeCount;
                }
                if (ok) {
                    volts -= scanZeroOffset[adc];
                    // Smooth across scan cycles: shot-to-shot ADC noise is
                    // +/-20mV, which is whole milliamps over a 30-ohm path.
                    if (voltageFresh(node, millis())) {
                        nodeVoltage[node] += 0.3f * (volts - nodeVoltage[node]);
                    } else {
                        nodeVoltage[node] = volts;
                    }
                    nodeVoltageMs[node] = millis();
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
void serviceNetVoltageScan(void) {}
void serviceNetVoltageScanDebug(void) {}
void printNetVoltageScanStats(Stream* out) { out->println("net current scan: V5 only"); }

#endif // OG_JUMPERLESS
