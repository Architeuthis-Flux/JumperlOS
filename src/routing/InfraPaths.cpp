// SPDX-License-Identifier: MIT
//
// InfraPaths implementation. See InfraPaths.h for the concept.
//
// Structure:
//   - a registry of the node pairs currently owned by infra functions
//     (the isEphemeralConnection precedent: pair-keyed side list, so no
//     bookkeeping is needed at any of the bridge-array mutation sites),
//   - raw bridge add/remove helpers that bypass addConnection/removeConnection
//     on purpose (their markDirty/undo/FakeGpio/refresh side effects are all
//     wrong for system bridges mutated mid-rebuild),
//   - the function table and the convergence loop.

#include "InfraPaths.h"

#include "FileParsing.h"   // checkIfBridgeExists-style helpers not needed; addBridge decl for wrapper paths
#include "JumperlessDefines.h"
#include "Peripherals.h"   // gpioDef, setDacXvoltage, getDacVoltage
#include "Probing.h"       // probeGpioPowerHwClaim/Release, bufferPowerConnected
#include "States.h"
#include "config.h"

extern void refreshLocalConnections(int ledShowOption, int fillUnused, int clean);
extern volatile bool refreshInProgress;        // Commands.cpp
extern volatile bool refreshLocalInProgress;   // Commands.cpp

// ===========================================================================
// Active-pair registry
// ===========================================================================

#define INFRA_MAX_ACTIVE_PAIRS 8

struct ActivePair {
    int16_t a, b;
    int8_t fn; // index into s_functions
};

static ActivePair s_active[INFRA_MAX_ACTIVE_PAIRS];
static int s_activeCount = 0;

static inline bool pairMatches(const ActivePair& p, int n1, int n2) {
    return (p.a == n1 && p.b == n2) || (p.a == n2 && p.b == n1);
}

bool infraIsBridge(int n1, int n2) {
    for (int i = 0; i < s_activeCount; i++) {
        if (pairMatches(s_active[i], n1, n2)) return true;
    }
    return false;
}

bool infraOwnsNode(int node) {
    for (int i = 0; i < s_activeCount; i++) {
        if (s_active[i].a == node || s_active[i].b == node) return true;
    }
    return false;
}

static void registryAdd(int fnIdx, int a, int b) {
    if (s_activeCount >= INFRA_MAX_ACTIVE_PAIRS) return; // table sized to never hit this
    s_active[s_activeCount].a = (int16_t)a;
    s_active[s_activeCount].b = (int16_t)b;
    s_active[s_activeCount].fn = (int8_t)fnIdx;
    s_activeCount++;
}

static void registryRemove(int a, int b) {
    for (int i = 0; i < s_activeCount; i++) {
        if (pairMatches(s_active[i], a, b)) {
            for (int j = i; j < s_activeCount - 1; j++) s_active[j] = s_active[j + 1];
            s_activeCount--;
            return;
        }
    }
}

// ===========================================================================
// Raw bridge mutation (bypasses addConnection/removeConnection on purpose)
// ===========================================================================

// True if any bridge that is NOT ours touches this node. This is the single
// definition of "the user claimed this node" for bridge-based claims.
// Conservative like the old findUnusedRoutableGpio: ephemeral measurement
// bridges count too (they mean the node is electrically in use right now).
static bool nonInfraBridgeTouches(int node) {
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numBridges; i++) {
        int n1 = c.bridges[i][0], n2 = c.bridges[i][1];
        if (n1 != node && n2 != node) continue;
        if (infraIsBridge(n1, n2)) continue;
        return true;
    }
    return false;
}

static int findBridgeIndex(int a, int b) {
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numBridges; i++) {
        if ((c.bridges[i][0] == a && c.bridges[i][1] == b) ||
            (c.bridges[i][0] == b && c.bridges[i][1] == a)) {
            return i;
        }
    }
    return -1;
}

static void rawAddPair(int fnIdx, int a, int b) {
    ConnectionState& c = globalState.connections;
    bool registered = infraIsBridge(a, b);
    int existing = findBridgeIndex(a, b);
    if (existing >= 0) {
        // Pair already present (legacy slot leftover the scrub missed, or a
        // user add that dedupe-merged). Take it over: force dup=0 so the
        // resistance stays a single known path.
        c.bridges[existing][2] = 0;
        if (!registered) registryAdd(fnIdx, a, b);
        return;
    }
    if (c.numBridges >= MAX_BRIDGES) return;
    c.bridges[c.numBridges][0] = (int16_t)a;
    c.bridges[c.numBridges][1] = (int16_t)b;
    c.bridges[c.numBridges][2] = 0; // NEVER duplicated: known resistance
    c.bridgeColors[c.numBridges] = 0xFFFFFFFF;
    c.numBridges++;
    if (!registered) registryAdd(fnIdx, a, b);
    // No markDirty (never serialized), no undo record (not a user action),
    // no refresh (we run at the head of one).
}

static void rawRemovePair(int a, int b) {
    registryRemove(a, b);
    ConnectionState& c = globalState.connections;
    int idx = findBridgeIndex(a, b);
    if (idx < 0) return;
    for (int i = idx; i < c.numBridges - 1; i++) {
        c.bridges[i][0] = c.bridges[i + 1][0];
        c.bridges[i][1] = c.bridges[i + 1][1];
        c.bridges[i][2] = c.bridges[i + 1][2];
        c.bridgeColors[i] = c.bridgeColors[i + 1];
    }
    c.numBridges--;
    c.bridgeColors[c.numBridges] = 0xFFFFFFFF;
    // Keep the ephemeral side-list's bridgeIndex map consistent, exactly as
    // removeEphemeralConnection does - a live measure-mode ephemeral bridge
    // can sit above ours in the array.
    globalState.adjustEphemeralIndicesAfterBridgeRemoval(idx);
}

// ===========================================================================
// Function/candidate table
// ===========================================================================

struct InfraCandidate {
    const char* name;
    // Fill out[] with the pair(s) this candidate would use RIGHT NOW (live
    // config/state reads). Returns pair count, 0 = not applicable.
    int (*resolvePairs)(InfraPair out[2]);
    bool (*viable)(void);
    void (*onActivate)(void);   // idempotent
    void (*onDeactivate)(void);
};

struct InfraFunction {
    const char* name;
    bool (*enabled)(void);
    const InfraCandidate* candidates;
    int numCandidates;
    // Runtime:
    int activeCandidate; // -1 = none
    int activeResource;  // resolved node for scan-style candidates, else -1
    InfraPair activePairs[2];
    int activePairCount;
};

// --- probe_power -----------------------------------------------------------

static bool s_probePowerOn = false;

// gpioDef index the GPIO candidate resolved to during this evaluation (the
// 8->1 scan communicates its winner to onActivate through here), and the
// currently ACTIVE claim.
static int s_gpioResolvedIdx = -1;
static int s_gpioActiveIdx = -1;

#if !defined(OG_JUMPERLESS)

static bool dacVoltageInProbeWindow(int dacNum) {
    // Same window the old setDacXvoltage swap used: a user parking a DAC
    // outside [2.80, 3.90] V has claimed it for their own purposes.
    float v = getDacVoltage(dacNum);
    return v >= 2.80f && v <= 3.90f;
}

static void probeDacActivate(int dacNum) {
    // Park the DAC at the calibrated measure-mode level. checkProbePower
    // stays false: the voltage-claim nudge must never re-enter evaluation.
    // Skip when already parked - onActivate re-runs on EVERY rebuild
    // (idempotent re-assert), and an unconditional set would hit the MCP4728
    // over I2C and dirty the state each time.
    float target = jumperlessConfig.calibration.measure_mode_output_voltage;
    if (fabsf(getDacVoltage(dacNum) - target) > 0.005f) {
        if (dacNum == 0) {
            setDac0voltage(target, 1, 0, false);
        } else {
            setDac1voltage(target, 1, 0, false);
        }
    }
    bufferPowerConnected = true;
}

// DAC1: preferred - 2 crosspoints, same chip K as BUFFER_IN.
static int rpDac1(InfraPair out[2]) { out[0] = {ROUTABLE_BUFFER_IN, DAC1}; return 1; }
static bool viDac1(void) { return !nonInfraBridgeTouches(DAC1) && dacVoltageInProbeWindow(1); }
static void actDac1(void) { probeDacActivate(1); }
static void deactDac(void) { /* nothing to undo: the DAC keeps its voltage */ }

// GPIO 8->1: the fallback. 4 crosspoints via the L->K interchip lane, so
// only used when both the preferred DAC is claimed. The scan prefers the
// high end because users reach for GPIO 1 first.
static int rpGpio(InfraPair out[2]) {
    // Keep the current claim if it is still the best free pin; otherwise
    // re-scan 8->1. (Re-picking each evaluation gives switch-back within
    // the GPIO class too - freeing GPIO_8 hops the claim back up.)
    for (int i = 7; i >= 0; i--) {
        int node = gpioDef[i][1];
        if (globalState.config.gpioPythonOwned[i]) continue;
        if (globalState.config.gpioPwmEnabled[i]) continue;
        if (jumperlessConfig.top_oled.enabled &&
            (node == jumperlessConfig.top_oled.gpio_sda ||
             node == jumperlessConfig.top_oled.gpio_scl)) continue;
        if (infraOwnsNode(node) && i != s_gpioActiveIdx) continue;
        if (nonInfraBridgeTouches(node)) continue;
        s_gpioResolvedIdx = i;
        out[0] = {ROUTABLE_BUFFER_IN, (int16_t)node};
        return 1;
    }
    s_gpioResolvedIdx = -1;
    return 0;
}
static bool viGpio(void) { return s_gpioResolvedIdx >= 0; }
static void actGpio(void) {
    // The pin-level work (save dir/state, 12mA SIO output-high, droop V0
    // seeding) lives in Probing - it owns the droop model and the pin
    // bookkeeping that refreshConnections' setGPIO pass re-asserts.
    if (s_gpioActiveIdx != s_gpioResolvedIdx && s_gpioActiveIdx >= 0) {
        probeGpioPowerHwRelease(s_gpioActiveIdx);
    }
    probeGpioPowerHwClaim(s_gpioResolvedIdx);
    s_gpioActiveIdx = s_gpioResolvedIdx;
    // Keep a free DAC1 parked at the measure-mode voltage so the (rare)
    // DAC-swap fallback in checkSwitchPosition reads the level the
    // thresholds were calibrated against. Same skip-if-parked guard as
    // probeDacActivate (this re-runs every rebuild).
    float target = jumperlessConfig.calibration.measure_mode_output_voltage;
    if (!nonInfraBridgeTouches(DAC1) &&
        fabsf(getDacVoltage(1) - target) > 0.005f) {
        setDac1voltage(target, 1, 0, false);
    }
    bufferPowerConnected = true;
}
static void deactGpio(void) {
    if (s_gpioActiveIdx >= 0) {
        probeGpioPowerHwRelease(s_gpioActiveIdx);
        s_gpioActiveIdx = -1;
    }
}

// DAC0: last resort - keeps probe power alive when DAC1 AND every GPIO are
// claimed (the old swap chain would have landed here too).
static int rpDac0(InfraPair out[2]) { out[0] = {ROUTABLE_BUFFER_IN, DAC0}; return 1; }
static bool viDac0(void) { return !nonInfraBridgeTouches(DAC0) && dacVoltageInProbeWindow(0); }
static void actDac0(void) { probeDacActivate(0); }

static const InfraCandidate s_probePowerCandidates[] = {
    { "DAC1", rpDac1, viDac1, actDac1, deactDac },
    { "GPIO", rpGpio, viGpio, actGpio, deactGpio },
    { "DAC0", rpDac0, viDac0, actDac0, deactDac },
};

static bool enProbePower(void) {
    return s_probePowerOn && jumperlessConfig.dacs.auto_connect_probe > 0;
}

#else // OG_JUMPERLESS: no routable buffer

static const InfraCandidate s_probePowerCandidates[] = {};
static bool enProbePower(void) { return false; }

#endif

// --- table -----------------------------------------------------------------

static InfraFunction s_functions[] = {
    { "probe_power", enProbePower, s_probePowerCandidates,
      (int)(sizeof(s_probePowerCandidates) / sizeof(s_probePowerCandidates[0])),
      -1, -1, {}, 0 },
    // oled_i2c / serial_1 / serial_2 land here in the lock-connection
    // migration milestone.
};
static const int s_numFunctions = (int)(sizeof(s_functions) / sizeof(s_functions[0]));

// Per-function forced candidate (test/calibration), -1 = automatic. Sized
// for the full table (probe + oled + serial_1 + serial_2) so new rows can't
// silently index past it - and every slot must init to -1, not 0.
static int8_t s_forced[4] = {-1, -1, -1, -1};
static_assert(sizeof(s_forced) >= sizeof(s_functions) / sizeof(s_functions[0]),
              "grow s_forced alongside the function table");

// ===========================================================================
// Convergence
// ===========================================================================

static volatile bool s_nudgePending = false;

static void teardownFunction(InfraFunction& fn) {
    if (fn.activeCandidate < 0) return;
    fn.candidates[fn.activeCandidate].onDeactivate();
    for (int p = 0; p < fn.activePairCount; p++) {
        rawRemovePair(fn.activePairs[p].a, fn.activePairs[p].b);
    }
    fn.activeCandidate = -1;
    fn.activeResource = -1;
    fn.activePairCount = 0;
}

static bool samePairs(const InfraFunction& fn, const InfraPair* pairs, int n) {
    if (fn.activePairCount != n) return false;
    for (int i = 0; i < n; i++) {
        if (fn.activePairs[i].a != pairs[i].a || fn.activePairs[i].b != pairs[i].b) {
            return false;
        }
    }
    return true;
}

void infraEvaluate(void) {
    s_nudgePending = false;

    for (int f = 0; f < s_numFunctions; f++) {
        InfraFunction& fn = s_functions[f];

        if (!fn.enabled()) {
            teardownFunction(fn);
            continue;
        }

        // First viable candidate in preference order wins. This IS auto
        // switch-back: a more-preferred candidate becoming viable takes over
        // on the next rebuild, unconditionally. (Rebuilds are user-action
        // driven; "moves only to a strictly-more-preferred viable candidate"
        // cannot oscillate.)
        int chosen = -1;
        InfraPair pairs[2];
        int pairCount = 0;
        for (int walk = 0; walk < fn.numCandidates; walk++) {
            int cIdx = walk;
#if !defined(OG_JUMPERLESS)
            // DEPRECATED debug.probe_power_gpio (defaults false now): as a
            // one-release transition override it reorders the probe-power
            // walk to GPIO-first, its old meaning. Remove with the flag.
            if (f == 0 && jumperlessConfig.debug.probe_power_gpio) {
                static const int gpioFirst[] = {1, 0, 2}; // GPIO, DAC1, DAC0
                cIdx = gpioFirst[walk];
            }
#endif
            if (s_forced[f] >= 0 && cIdx != s_forced[f]) continue;
            pairCount = fn.candidates[cIdx].resolvePairs(pairs);
            if (pairCount <= 0) continue;
            if (!fn.candidates[cIdx].viable()) continue;
            chosen = cIdx;
            break;
        }

        if (chosen < 0) {
            if (fn.activeCandidate >= 0) {
                teardownFunction(fn);
            }
            // Rate-limited warning: this retries at every rebuild + tick.
            static uint32_t lastWarnMs = 0;
            if (millis() - lastWarnMs > 5000) {
                lastWarnMs = millis();
                Serial.printf("[infra] %s: no viable candidate (all resources claimed)\n\r",
                              fn.name);
            }
            continue;
        }

        if (chosen == fn.activeCandidate && samePairs(fn, pairs, pairCount)) {
            // Same candidate, same pairs - but the ARRAY may have lost them
            // (nodes_clear() / clearAllConnections wipes every bridge, ours
            // included, while the registry rides through). Re-add any
            // missing row before the idempotent hardware re-assert.
            for (int p = 0; p < pairCount; p++) {
                if (findBridgeIndex(pairs[p].a, pairs[p].b) < 0) {
                    rawAddPair(f, pairs[p].a, pairs[p].b);
                }
            }
            fn.candidates[chosen].onActivate(); // idempotent re-assert
            continue;
        }

        // Relocate: release-early. Both callbacks run before this rebuild's
        // sendPaths, so the function sees a brief (~ms) gap until the new
        // pair's crosspoints close - same glitch class as the old DAC swap.
        // Holding the old source instead would parallel two sources at
        // different voltages through the crossbar, which is worse.
        teardownFunction(fn);
        for (int p = 0; p < pairCount; p++) {
            rawAddPair(f, pairs[p].a, pairs[p].b);
            fn.activePairs[p] = pairs[p];
        }
        fn.activePairCount = pairCount;
        fn.activeCandidate = chosen;
#if !defined(OG_JUMPERLESS)
        fn.activeResource =
            (&fn == &s_functions[0] && chosen >= 0 && fn.activePairCount > 0)
                ? (fn.activePairs[0].a == ROUTABLE_BUFFER_IN ? fn.activePairs[0].b
                                                             : fn.activePairs[0].a)
                : -1;
#endif
        fn.candidates[chosen].onActivate();
    }

#if !defined(OG_JUMPERLESS)
    if (s_functions[0].activeCandidate < 0) {
        bufferPowerConnected = false;
    }
    // Compatibility view: ~59 sites still read probePowerDAC ("which DAC is
    // the probe's"). Keep it pointing at the DAC infra is using (or would
    // prefer for fallbacks) until the reader sweep retires it.
    {
        int src = infraProbePowerSource();
        if (src == DAC0) probePowerDAC = 0;
        else if (src == DAC1) probePowerDAC = 1;
        else probePowerDAC = 1; // GPIO-fed or off: DAC1 is the preferred fallback
    }
#endif
}

// ===========================================================================
// Probe power queries / control
// ===========================================================================

int infraProbePowerSource(void) {
    return s_functions[0].activeCandidate >= 0 ? s_functions[0].activeResource : -1;
}

int infraProbePowerGpioIdx(void) {
    return s_gpioActiveIdx;
}

void infraSetProbePowerEnabled(bool on) {
    s_probePowerOn = on;
}

bool infraProbePowerWanted(void) {
    return s_probePowerOn;
}

// ===========================================================================
// Nudges
// ===========================================================================

void infraNudge(void) {
    s_nudgePending = true;
    if (!refreshInProgress && !refreshLocalInProgress) {
        // Evaluation runs at the head of this refresh and clears the flag.
        refreshLocalConnections(0, 1, 0);
    }
    // else: the in-flight rebuild already ran its evaluation - the sticky
    // flag makes infraServiceTick retry once it finishes. (The
    // refreshLocalPending re-run is commented out in Commands.cpp, so we
    // cannot rely on it.)
}

void infraServiceTick(void) {
    if (s_nudgePending) {
        if (!refreshInProgress && !refreshLocalInProgress) {
            refreshLocalConnections(0, 1, 0);
        }
        return;
    }
#if !defined(OG_JUMPERLESS)
    // Viability re-check for claims that never touch the bridge array:
    // MicroPython taking the claim pin or PWM enabled on it mid-claim.
    if (s_gpioActiveIdx >= 0 &&
        (globalState.config.gpioPythonOwned[s_gpioActiveIdx] ||
         globalState.config.gpioPwmEnabled[s_gpioActiveIdx])) {
        infraNudge();
    }
#endif
}

// ===========================================================================
// Slot-load migration
// ===========================================================================

void infraScrubLoadedBridges(void) {
    ConnectionState& c = globalState.connections;
    int writeIdx = 0;
    for (int readIdx = 0; readIdx < c.numBridges; readIdx++) {
        int n1 = c.bridges[readIdx][0];
        int n2 = c.bridges[readIdx][1];
        bool drop = false;

        // ROUTABLE_BUFFER_IN is system-reserved: any bridge touching it in a
        // saved slot is a stale power claim from firmware that persisted
        // them (or a hand-edited slot). Evaluation re-adds the live one.
        if (n1 == ROUTABLE_BUFFER_IN || n2 == ROUTABLE_BUFFER_IN) {
            drop = true;
#if !defined(OG_JUMPERLESS)
            // Stale GPIO claim: the pin's saved output-high direction rode
            // along in the slot. Put it back to input unless MicroPython
            // owns it now (then dir/pulls are Python's business).
            int other = (n1 == ROUTABLE_BUFFER_IN) ? n2 : n1;
            for (int i = 0; i < 8; i++) {
                if (gpioDef[i][1] != other) continue;
                if (i == s_gpioActiveIdx) break; // the LIVE claim, not stale
                if (!globalState.config.gpioPythonOwned[i]) {
                    globalState.config.gpioDirection[i] = 1; // input
                    gpio_set_dir(gpioDef[i][0], false);
                }
                break;
            }
#endif
        }

        // Active lock-function pairs are re-added fresh by evaluation; a
        // saved copy from older firmware would otherwise double up.
        if (!drop && infraIsBridge(n1, n2)) {
            drop = true;
        }

        if (drop) continue;
        if (writeIdx != readIdx) {
            c.bridges[writeIdx][0] = c.bridges[readIdx][0];
            c.bridges[writeIdx][1] = c.bridges[readIdx][1];
            c.bridges[writeIdx][2] = c.bridges[readIdx][2];
            c.bridgeColors[writeIdx] = c.bridgeColors[readIdx];
        }
        writeIdx++;
    }
    if (writeIdx != c.numBridges) {
        c.numBridges = writeIdx;
        for (int i = writeIdx; i < MAX_BRIDGES; i++) c.bridgeColors[i] = 0xFFFFFFFF;
        c.syncNetsFromBridges();
    }
}

// ===========================================================================
// Test / calibration override + status
// ===========================================================================

static bool s_systemBridgeAllowance = false;
void infraSetSystemBridgeAllowance(bool allow) { s_systemBridgeAllowance = allow; }
bool infraSystemBridgeAllowed(void) { return s_systemBridgeAllowance; }

int infraForceCandidate(const char* fnName, int candIdx) {
    for (int f = 0; f < s_numFunctions; f++) {
        if (strcmp(s_functions[f].name, fnName) != 0) continue;
        if (candIdx >= s_functions[f].numCandidates) return -1;
        s_forced[f] = (int8_t)(candIdx < 0 ? -1 : candIdx);
        return 0;
    }
    return -1;
}

void infraPrintStatus(Stream* out) {
    for (int f = 0; f < s_numFunctions; f++) {
        InfraFunction& fn = s_functions[f];
        out->printf("%-12s %s", fn.name, fn.enabled() ? "on " : "off");
        if (fn.activeCandidate >= 0) {
            out->printf("  -> %s", fn.candidates[fn.activeCandidate].name);
            if (fn.activeResource >= 0) out->printf(" (node %d)", fn.activeResource);
            for (int p = 0; p < fn.activePairCount; p++) {
                out->printf("  [%d-%d]", fn.activePairs[p].a, fn.activePairs[p].b);
            }
        } else {
            out->print("  -> (none)");
        }
        if (s_forced[f] >= 0) out->printf("  FORCED:%d", s_forced[f]);
        out->println("\r");
    }
}
