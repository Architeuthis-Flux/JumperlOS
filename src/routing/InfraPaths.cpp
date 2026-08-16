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
#include "MCP4728.h"       // MCP4728::invalidateCache (the park's set-once shadow)
#include "Peripherals.h"   // gpioDef, setDacXvoltage, getDacVoltage
#include "Probing.h"       // probeGpioPowerHwClaim/Release
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
    // Optional: true when a USER connection sits on this function's own
    // service node(s). The function then YIELDS - tears down and stands
    // aside - instead of fighting or blocking the user (it re-converges
    // automatically once the user disconnects). nullptr = never yields.
    bool (*userOverridden)(void);
    // Optional: maps a walk position (0..numCandidates-1) to the candidate
    // index tried at that position, so a function can re-order its
    // preference at runtime from config without renumbering candidates
    // (infraForceCandidate callers keep their fixed indices). nullptr =
    // table order.
    int (*order)(int walk);
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
    // outside [2.80, 3.90] V has claimed it for their own purposes. The
    // user-claim latch covers save=0 writes (MicroPython's dac_set() default
    // moves only the hardware, and judging from state alone let the feed
    // re-park a DAC the user had just set - a silent revert); the state check
    // still covers persisted writes and boot.
    if (dacUserClaimed(dacNum)) return false;
    float v = getDacVoltage(dacNum);
    return v >= 2.80f && v <= 3.90f;
}

// onActivate re-runs on EVERY rebuild, so the park is called far more often
// than the DAC actually needs writing. The set-once guarantee lives in the
// MCP4728 driver now: it shadows the last register word per channel and
// setDacXvoltage() goes through its cached write, so an identical park is
// zero I2C. Blind hardware writes that used to hide behind an in-window
// state value (selfTestNormalizeHardware, calibrateDacs, WaveGen streaming
// DAC0, MicroPython dac_set(save=False)) all pass through the same driver
// and move the shadow, so the next park writes exactly when it must - the
// old "park epoch" that forced one unconditional re-write is gone. The
// bump entry point survives as a cache invalidation for anything that
// moves the chip behind the driver's back.
void infraDacParkEpochBump(void) {
    MCP4728::invalidateCache(0);
    MCP4728::invalidateCache(1);
}

static void parkDacAtMeasureTarget(int dacNum) {
    // A DAC the user has parked outside the probe window is theirs: never
    // write it. Without this, the nudge that a user dac_set() fires
    // re-activated the still-current DAC feed - parking it at 3.33 V ON TOP
    // OF the value the user had just written - and only then relocated the
    // feed, so the user's DAC read back at the park voltage (HIL routing test:
    // "DAC0 2.5V reads 3.18V"). The evaluation will find the DAC non-viable
    // and move the feed; the DAC keeps the user's voltage meanwhile.
    if (dacUserClaimed(dacNum)) return;
    // Park the DAC at the calibrated measure-mode level. checkProbePower
    // stays false: the voltage-claim nudge must never re-enter evaluation.
    // Clamp to the tip-drive sanity band (configManager also validates on
    // load): parking above 3.3V logic clips the pad ladder's high end.
    float target = jumperlessConfig.calibration.measure_mode_output_voltage;
    if (target < 3.0f || target > 3.6f) target = 3.33f;
    // save only when the STATE value actually moves (setDacVoltage marks the
    // state dirty unconditionally); the hardware decision is the driver's.
    bool stateOff = fabsf(getDacVoltage(dacNum) - target) > 0.005f;
    if (dacNum == 0) {
        setDac0voltage(target, stateOff ? 1 : 0, 0, false);
    } else {
        setDac1voltage(target, stateOff ? 1 : 0, 0, false);
    }
}

static void probeDacActivate(int dacNum) {
    parkDacAtMeasureTarget(dacNum);
}

// DAC0: preferred - 2 crosspoints, same chip K as BUFFER_IN, and the ONLY
// feed the switch-position sensing can see: INA219 @0x41's 2-ohm shunt
// (R57) is hardwired in DAC_0's output path, so the probe LED's supply
// current registers on checkProbeCurrent() only when DAC0 sources the
// buffer. (A DAC1 feed routes identically but is invisible to INA1 -
// hardware-observed as the switch never changing position.)
static int rpDac0(InfraPair out[2]) { out[0] = {ROUTABLE_BUFFER_IN, DAC0}; return 1; }
static bool viDac0(void) { return !nonInfraBridgeTouches(DAC0) && dacVoltageInProbeWindow(0); }
static void actDac0(void) { probeDacActivate(0); }
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
    // (No DAC0 pre-park here any more: it existed for the DAC-swap sensing
    // fallback, which was unreachable and is deleted. When DAC0 becomes the
    // feed again - by preference order or by the user releasing it - its
    // own onActivate parks it, and setDac0voltage's release-nudge already
    // re-evaluates the moment a user write comes back inside the window.)
}
static void deactGpio(void) {
    if (s_gpioActiveIdx >= 0) {
        probeGpioPowerHwRelease(s_gpioActiveIdx);
        s_gpioActiveIdx = -1;
    }
}

// (No DAC1 candidate: DAC1 stays the user's general-purpose routable DAC.
// DAC0 is both the servo'd measure-mode output AND the feed - they are the
// same wire, and the tip test stores measure_mode_output_voltage as a DAC0
// setting. The fall-through is DAC0 -> GPIO8..GPIO1; with all of those
// claimed the function stands down until one frees up.)

// Candidate INDICES are fixed (0 = DAC0, 1 = GPIO) - the self test, the
// calibration apps and Probing pin them through infraForceCandidate. The
// PREFERENCE between them is config: dacs.probe_power_source = 0 walks
// DAC0 then GPIO (the INA-sensed 2-crosspoint feed first), 1 walks GPIO
// then DAC0 (the ~170-ohm 4-crosspoint feed first, keeping DAC0 free for
// the user). Either way it always falls through to the other.
static const InfraCandidate s_probePowerCandidates[] = {
    { "DAC0", rpDac0, viDac0, actDac0, deactDac },
    { "GPIO", rpGpio, viGpio, actGpio, deactGpio },
};

bool infraProbePowerGpioFirst(void) {
    return jumperlessConfig.dacs.probe_power_source == 1;
}

static int ordProbePower(int walk) {
    return infraProbePowerGpioFirst() ? (1 - walk) : walk;
}

// A user bridge on ROUTABLE_BUFFER_IN itself (any shape - a row, an ADC, a
// DAC they placed by hand) means they want the buffer: yield the feed.
static bool ovProbePower(void) { return nonInfraBridgeTouches(ROUTABLE_BUFFER_IN); }

static bool enProbePower(void) {
    return s_probePowerOn && jumperlessConfig.dacs.auto_connect_probe > 0;
}

#else // OG_JUMPERLESS: no routable buffer

static const InfraCandidate s_probePowerCandidates[] = {};
static bool enProbePower(void) { return false; }
void infraDacParkEpochBump(void) { /* no probe feed to re-park on OG */ }
bool infraProbePowerGpioFirst(void) { return false; }

#endif

// --- oled_i2c --------------------------------------------------------------
//
// The top OLED's I2C lock bridges: sda_row<->gpio_sda + scl_row<->gpio_scl,
// resolved from live config each evaluation (the pins/rows are menu-editable
// at runtime - a change simply resolves new pairs and the diff rewires).
// Replaces handleLockedConnections' OLED block and its clones. viable() is
// unconditionally true, preserving the old unconditional-add semantics;
// user-conflict handling on the rows is future work.

extern bool oledUsingHardwiredPins; // oled.cpp

static int rpOledI2c(InfraPair out[2]) {
    out[0] = {(int16_t)jumperlessConfig.top_oled.sda_row,
              (int16_t)jumperlessConfig.top_oled.gpio_sda};
    out[1] = {(int16_t)jumperlessConfig.top_oled.scl_row,
              (int16_t)jumperlessConfig.top_oled.gpio_scl};
    return 2;
}
static bool viAlways(void) { return true; }
static void actNone(void) {}
static void deactNone(void) {}

static const InfraCandidate s_oledCandidates[] = {
    { "config_pins", rpOledI2c, viAlways, actNone, deactNone },
};

// A user claiming the OLED's GPIO pins wins them (the ROW side is fair
// game for extra I2C devices and never yields).
static bool ovOledI2c(void) {
    return nonInfraBridgeTouches(jumperlessConfig.top_oled.gpio_sda) ||
           nonInfraBridgeTouches(jumperlessConfig.top_oled.gpio_scl);
}

static bool enOledI2c(void) {
    // Exact gate from the old handleLockedConnections: either lock flag, and
    // never when the panel sits on hardwired pins (no bridges needed at all).
    return (jumperlessConfig.top_oled.lock_connection == 1 ||
            globalState.config.oledLockConnection == 1) &&
           !oledUsingHardwiredPins;
}

// --- serial_1 --------------------------------------------------------------

static int rpSerial1(InfraPair out[2]) {
    out[0] = {RP_UART_TX, NANO_D0};
    out[1] = {RP_UART_RX, NANO_D1};
    return 2;
}

static const InfraCandidate s_serial1Candidates[] = {
    { "uart0_d0_d1", rpSerial1, viAlways, actNone, deactNone },
};

static bool ovSerial1(void) {
    return nonInfraBridgeTouches(RP_UART_TX) || nonInfraBridgeTouches(RP_UART_RX);
}

static bool enSerial1(void) {
    return jumperlessConfig.serial_1.lock_connection == 1;
}

// --- serial_2 --------------------------------------------------------------
// TODO (carried over from handleLockedConnections): determine node mappings
// for serial_2 / the routable UART before enabling this row.

static bool enSerial2(void) { return false; }

// --- table -----------------------------------------------------------------

static InfraFunction s_functions[] = {
    { "probe_power", enProbePower,
#if !defined(OG_JUMPERLESS)
      ovProbePower, ordProbePower,
#else
      nullptr, nullptr,
#endif
      s_probePowerCandidates,
      (int)(sizeof(s_probePowerCandidates) / sizeof(s_probePowerCandidates[0])),
      -1, -1, {}, 0 },
    { "oled_i2c", enOledI2c, ovOledI2c, nullptr, s_oledCandidates, 1, -1, -1, {}, 0 },
    { "serial_1", enSerial1, ovSerial1, nullptr, s_serial1Candidates, 1, -1, -1, {}, 0 },
    { "serial_2", enSerial2, nullptr, nullptr, nullptr, 0, -1, -1, {}, 0 },
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

        // USER OVERRIDE: a user connection on the function's own service
        // node wins, always. Yield - tear the infra path down FIRST so the
        // user's connection routes cleanly in this same rebuild - and stand
        // aside until they disconnect (re-convergence is automatic).
        // EXCEPT under a forced candidate: forcing is a deliberate
        // test/calibration act that owns the plumbing, and those flows add
        // their own instrumentation bridges on the service node (the tip
        // test's droop phase loads BUFFER_IN through the ISENSE shunt -
        // yielding there tore down the forced GPIO feed mid-measurement).
        if (fn.userOverridden && s_forced[f] < 0 && fn.userOverridden()) {
            if (fn.activeCandidate >= 0) {
                Serial.printf("[infra] %s yielded to a user connection on its node\n\r",
                              fn.name);
                teardownFunction(fn);
            }
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
            // The function's order hook (probe_power: dacs.probe_power_source)
            // decides which candidate this walk position tries. Candidate
            // indices never move, so a forced index means the same thing
            // under either order.
            int cIdx = fn.order ? fn.order(walk) : walk;
            if (cIdx < 0 || cIdx >= fn.numCandidates) continue;
            // (debug.probe_power_gpio is parsed but IGNORED: it was briefly
            // honored as a GPIO-first transition override, but every board
            // that ran the old GPIO-power firmware has true persisted in
            // config.txt - which silently kept the 4-crosspoint GPIO feed
            // and its droop model in play. dacs.probe_power_source is the
            // deliberate, validated replacement.)
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
    // Compatibility view: ~59 sites still read probePowerDAC ("which DAC is
    // the probe's"). Keep it pointing at the DAC infra is using (or would
    // prefer for fallbacks) until the reader sweep retires it.
    {
        // Feed is DAC0 or a GPIO now (no DAC1 candidate); DAC0 is always
        // the answer for "which DAC is the probe's" - it is the sensed
        // path (INA1) and the servo'd measure-mode output.
        (void)infraProbePowerSource();
        probePowerDAC = 0;
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

static void pairPathStats(int a, int b, int* paths, int* dups, int* xp);

float infraProbeDroopOhms(void) {
    float calibrated = jumperlessConfig.calibration.probe_droop_ohms;
    if (calibrated > 0.0f) return calibrated;

    // Uncalibrated fallback - two answers, because two consumers:
    //
    // LEGACY classifier (debug.probe_switch_agree = 0): the "empirical 30 ohm"
    // that made the ADC7 droop-mA land on the DAC0-calibrated mA thresholds.
    // It is a reverse fit, not a resistance (the self test measures ~170-185
    // ohm on this feed) - ~40 mV of droop reads as ~1.4 mA through it - and
    // the legacy GPIO branch is tuned to it. Keep it exactly while that
    // branch decides, or SELECT under a GPIO feed drops below threshold_low.
    //
    // AGREEMENT classifier (= 1): nobody divides by R for a verdict any more
    // (the GPIO feed is judged by the feed-side blink, droop is a statistic),
    // so report the physical estimate: pad + N x crosspoint_resistance, N
    // counted from the ACTUAL routed feed path (exact - infra pairs are never
    // duplicated), 4 when it isn't routed yet.
    if (!jumperlessConfig.debug.probe_switch_agree) return 30.0f;
    int xp = 4;
#if !defined(OG_JUMPERLESS)
    if (s_functions[0].activeCandidate >= 0 && s_functions[0].activePairCount > 0) {
        int paths, dups, routedXp;
        pairPathStats(s_functions[0].activePairs[0].a, s_functions[0].activePairs[0].b,
                      &paths, &dups, &routedXp);
        if (routedXp > 0) xp = routedXp;
    }
#endif
    return jumperlessConfig.calibration.probe_pad_ohms +
           (float)xp * jumperlessConfig.calibration.crosspoint_resistance;
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
    // Switch-back safety net: rebuilds are user-action driven, but some
    // ways a MORE-preferred candidate becomes viable again change no bridge
    // and fire no nudge - MicroPython gpio_release_pin() / PWM stop under
    // GPIO-first (hardware-observed on the flag's first HIL run), and under
    // DAC0-first a dac_set() that brings DAC0 back inside the window when it
    // had been outside by persisted STATE rather than by the user-claim latch
    // (booted with dac0 = 0.8 V saved: no latch, so the release-nudge in
    // setDac0voltage never fires - hardware-observed the same day). So every
    // ~500 ms: if the walk's FIRST candidate is not the active one and it
    // resolves + is viable right now, ask for a rebuild. The rebuild's own
    // evaluation makes the final call; this only makes sure one happens.
    {
        InfraFunction& fn = s_functions[0];
        if (fn.enabled() && s_forced[0] < 0 && fn.activeCandidate >= 0) {
            int first = fn.order ? fn.order(0) : 0;
            if (first != fn.activeCandidate && first >= 0 && first < fn.numCandidates) {
                InfraPair probe[2];
                if (fn.candidates[first].resolvePairs(probe) > 0 &&
                    fn.candidates[first].viable()) {
                    infraNudge();
                }
            }
        }
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

        // Stale power claims from firmware that persisted them: only the
        // FEED SHAPES (a DAC or routable GPIO bridged to BUFFER_IN) are
        // system artifacts - evaluation re-adds the live one. Any OTHER
        // bridge touching BUFFER_IN is deliberate user data (the function
        // yields to it at evaluation time) and survives the load.
        int other = -1;
        if (n1 == ROUTABLE_BUFFER_IN) other = n2;
        else if (n2 == ROUTABLE_BUFFER_IN) other = n1;
        bool feedShape = (other == DAC0 || other == DAC1 ||
                          (other >= RP_GPIO_1 && other <= RP_GPIO_8));
        if (other >= 0 && feedShape) {
            drop = true;
#if !defined(OG_JUMPERLESS)
            // Stale GPIO claim: the pin's saved output-high direction rode
            // along in the slot. Put it back to input unless MicroPython
            // owns it now (then dir/pulls are Python's business).
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

// ===========================================================================
// Ephemeral ADC resource pool
// ===========================================================================
//
// Cross-core note: TDM and the scan acquire on core 1, measure mode on
// core 0. Owner slots are single-byte writes; the worst race is two users
// grabbing the same just-freed channel in the same instant, which costs one
// electrically-overlapped tap - the same exposure the old uncoordinated
// per-consumer scans had, minus their systematic collisions.

static volatile uint8_t s_adcOwner[5] = {INFRA_ADC_NONE, INFRA_ADC_NONE,
                                         INFRA_ADC_NONE, INFRA_ADC_NONE,
                                         INFRA_ADC_NONE};

bool infraAdcUserClaimed(int adcChannel) {
    if (adcChannel < 0 || adcChannel > 4) return true; // invalid = occupied
    int adcNode = ADC0 + adcChannel;
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numBridges; i++) {
        int n1 = c.bridges[i][0], n2 = c.bridges[i][1];
        if (n1 != adcNode && n2 != adcNode) continue;
        // FakeGPIO input bridges are the TDM's own plumbing, not a user claim.
        if (IS_FAKE_GP_IN(n1) || IS_FAKE_GP_IN(n2)) continue;
        // A measure-mode ephemeral bridge is pool-tracked via its owner slot,
        // not treated as a user claim.
        if (globalState.isEphemeralConnection(n1, n2)) continue;
        return true;
    }
    return false;
}

int infraAcquireAdc(InfraAdcUser user, uint8_t candidateMask, bool allowSharedTdm) {
    // Already own a still-viable channel? Keep it (TDM's keep-if-free rule).
    for (int adc = 0; adc < 5; adc++) {
        if (s_adcOwner[adc] == user && (candidateMask & (1u << adc)) &&
            !infraAdcUserClaimed(adc)) {
            return adc;
        }
    }
    int sharedFallback = -1;
    for (int adc = 0; adc < 5; adc++) {
        if (!(candidateMask & (1u << adc))) continue;
        if (infraAdcUserClaimed(adc)) continue;
        if (s_adcOwner[adc] == INFRA_ADC_NONE) {
            s_adcOwner[adc] = user;
            return adc;
        }
        if (allowSharedTdm && s_adcOwner[adc] == INFRA_ADC_TDM && sharedFallback < 0) {
            sharedFallback = adc; // ride along, ownership stays TDM's
        }
    }
    return sharedFallback;
}

void infraReleaseAdc(InfraAdcUser user) {
    for (int adc = 0; adc < 5; adc++) {
        if (s_adcOwner[adc] == user) s_adcOwner[adc] = INFRA_ADC_NONE;
    }
}

uint8_t infraFreeAdcMask(uint8_t candidateMask) {
    uint8_t free = 0;
    for (int adc = 0; adc < 5; adc++) {
        if (!(candidateMask & (1u << adc))) continue;
        if (infraAdcUserClaimed(adc)) continue;
        if (s_adcOwner[adc] != INFRA_ADC_NONE) continue;
        free |= (1u << adc);
    }
    return free;
}

int infraForceCandidate(const char* fnName, int candIdx) {
    for (int f = 0; f < s_numFunctions; f++) {
        if (strcmp(s_functions[f].name, fnName) != 0) continue;
        if (candIdx >= s_functions[f].numCandidates) return -1;
        s_forced[f] = (int8_t)(candIdx < 0 ? -1 : candIdx);
        return 0;
    }
    return -1;
}

// How many routed paths carry this exact node pair, how many of them are
// duplicates, and how many crosspoints the (first) one closes. Infra pairs
// are added dup=0 and skipped by fillUnusedPaths, so the answer for a live
// feed must be paths:1 dup:0 with xp:2 (DAC0, same chip K) or xp:4 (GPIO,
// L->K interchip). Printed so a human - and the HIL suite - can see the
// single-path invariant instead of trusting it.
static void pairPathStats(int a, int b, int* paths, int* dups, int* xp) {
    *paths = 0; *dups = 0; *xp = 0;
    ConnectionState& c = globalState.connections;
    for (int i = 0; i < c.numPaths; i++) {
        const pathStruct& p = c.paths[i];
        bool match = (p.node1 == a && p.node2 == b) || (p.node1 == b && p.node2 == a);
        if (!match) continue;
        (*paths)++;
        if (p.duplicate != 0) (*dups)++;
        if (*xp == 0) {
            // Same predicate sendAllPaths uses to decide a hop is real.
            for (int j = 0; j < 4; j++) {
                if (p.chip[j] != -1 && p.x[j] != -1 && p.y[j] != -1) (*xp)++;
            }
        }
    }
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
#if !defined(OG_JUMPERLESS)
        if (f == 0) {
            out->printf("  order:%s", infraProbePowerGpioFirst() ? "GPIO>DAC0" : "DAC0>GPIO");
            if (fn.activeCandidate >= 0 && fn.activePairCount > 0) {
                int paths, dups, xp;
                pairPathStats(fn.activePairs[0].a, fn.activePairs[0].b, &paths, &dups, &xp);
                out->printf("  paths:%d dup:%d xp:%d", paths, dups, xp);
            }
        }
#endif
        out->println("\r");
    }
}
