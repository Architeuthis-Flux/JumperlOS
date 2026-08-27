// SPDX-License-Identifier: MIT
// Part-identification measurement primitives. See PartMeasure.h for the
// fixture discipline and the bench-measured facts (2026-08-26) this encodes.
//
// The GuideChecks lessons, kept:
//   - every leg goes through legAdd() and comes back out through legsClear()
//     or the partScanEnd() funnel - batch removals, ONE refresh
//   - refreshLocalConnections(0,0,0) only (bypass send: no LED work, no
//     clean escalation, no CH446Q reset pulse); never refreshConnections()
//   - after every build, ephRefused() checks the router's refusal list -
//     a state-side add that the fabric refused must fail loudly, not read 0V
//   - GPIO changes write globalState.config + gpioState alongside the raw
//     SDK calls so the setGPIO() pass at any refresh tail agrees with us
//   - INA0 is read as shuntVoltage_mV / 2 (5uA/LSB) from currentSenseState
//     with our own pollCurrentSense() pump - never reconfigure the chip

#include "PartMeasure.h"

#include <Arduino.h>
#include "hardware/gpio.h"
#include "JumperlessDefines.h"
#include "Peripherals.h"
#include "Commands.h"
#include "routing/States.h"
#include "routing/InfraPaths.h"
#include "routing/NetsToChipConnections.h"  // unconnectablePaths
#include "remembering/FileParsing.h"  // add/removeBridgeFromState (the lift)
#include "configManager.h"

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

// Per-leg add chatter for bring-up; refusals and add failures always print.
static bool partScanDebug = false;

static bool bridgeExistsInState(int a, int b) {
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        int n1 = globalState.connections.bridges[i][0];
        int n2 = globalState.connections.bridges[i][1];
        if ((n1 == a && n2 == b) || (n1 == b && n2 == a)) return true;
    }
    return false;
}

static bool nodeHasAnyBridgePM(int node) {
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        if (globalState.connections.bridges[i][0] == node ||
            globalState.connections.bridges[i][1] == node) return true;
    }
    return false;
}

// Stage one leg (no refresh). A pair already present as a regular bridge is
// refused - never adopt user wiring.
static bool legAdd(ScanSession& s, int a, int b) {
    if (s.nEph >= (uint8_t)(sizeof(s.ephA) / sizeof(s.ephA[0]))) {
        s.ephAddFailed = true;
        return false;
    }
    if (bridgeExistsInState(a, b)) {
        s.ephAddFailed = true;
        return false;
    }
    String err;
    if (!globalState.addEphemeralConnection(a, b, err, false, 0)) {
        Serial.print("\r\nPARTSCAN add-fail ");
        Serial.print(a); Serial.print("-"); Serial.print(b);
        Serial.print(" "); Serial.print(err);
        Serial.flush();
        s.ephAddFailed = true;
        return false;
    }
    if (partScanDebug) {
        Serial.print("\r\nPARTSCAN eph ");
        Serial.print(a); Serial.print("-"); Serial.print(b);
        Serial.flush();
    }
    s.ephA[s.nEph] = (int16_t)a;
    s.ephB[s.nEph] = (int16_t)b;
    s.nEph++;
    return true;
}

static void refreshQuiet(void) {
    refreshLocalConnections(0, 0, 0);
    waitCore2();
}

// Remove every staged leg, one refresh. The teardown funnel.
static void legsClear(ScanSession& s) {
    s.ephAddFailed = false;
    if (s.nEph == 0) return;
    String err;
    for (int i = 0; i < s.nEph; i++)
        globalState.removeEphemeralConnection(s.ephA[i], s.ephB[i], err, false, 0);
    s.nEph = 0;
    refreshQuiet();
}

// Did the refresh we just ran refuse any staged leg? Only meaningful right
// after refreshQuiet() - the list is rebuilt every routing pass.
static int ephRefused(ScanSession& s) {
    int n = numberOfUnconnectablePaths;
    if (n > 10) n = 10;
    int refused = 0;
    for (int i = 0; i < s.nEph; i++) {
        for (int u = 0; u < n; u++) {
            int a = unconnectablePaths[u][0], b = unconnectablePaths[u][1];
            if ((a == s.ephA[i] && b == s.ephB[i]) ||
                (a == s.ephB[i] && b == s.ephA[i])) {
                refused++;
                Serial.print("\r\nPARTSCAN REFUSED ");
                Serial.print(s.ephA[i]); Serial.print("-");
                Serial.print(s.ephB[i]);
                Serial.flush();
                break;
            }
        }
    }
    return refused;
}

// Build the staged legs: refresh + refusal check. False = tear back down.
static bool legsBuild(ScanSession& s) {
    if (s.ephAddFailed) {
        // a leg never even entered the session (slots/table full, or the
        // pair collided with a bridge) - measuring on the incomplete
        // fixture read a real diode as EMPTY (review finding: a lost
        // ISENSE_MINUS->GND leg made every servo step read ~0mA)
        legsClear(s);
        return false;
    }
    refreshQuiet();
    if (ephRefused(s) > 0) {
        legsClear(s);
        return false;
    }
    return true;
}

// Fresh INA0 conversion (see header). Two in a row so a conversion that
// straddled the caller's stimulus edge is discarded.
static float inaFreshMa(uint32_t maxWaitMs = 150) {
    unsigned long seen = currentSenseState.lastUpdatedMs;
    unsigned long start = millis();
    while (millis() - start < maxWaitMs) {
        Peripherals::getInstance().pollCurrentSense();
        if (currentSenseState.lastUpdatedMs != seen) break;
        delayMicroseconds(2000);
    }
    return currentSenseState.shuntVoltage_mV / 2.0f;
}

static float inaSettledMa(void) {
    (void)inaFreshMa();
    return inaFreshMa();
}

// The roving GPIO, config-consistent. pull: 0 none, 1 up, -1 down.
static void rovingOut(ScanSession& s, bool high) {
    if (s.gpioIdx < 0) return;
    int pin = gpioDef[s.gpioIdx][0];
    globalState.config.gpioDirection[s.gpioIdx] = 0;
    gpioState[s.gpioIdx] = high ? 1 : 0;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, high ? HIGH : LOW);
    gpio_set_input_enabled(pin, false);  // E9: buffer off, we never read it
}

static void rovingIn(ScanSession& s, int pull) {
    if (s.gpioIdx < 0) return;
    int pin = gpioDef[s.gpioIdx][0];
    globalState.config.gpioDirection[s.gpioIdx] = 1;
    globalState.config.gpioPulls[s.gpioIdx] = (pull == 1) ? 1 : (pull == -1) ? 0 : 2;
    gpioState[s.gpioIdx] = (pull == 1) ? 3 : (pull == -1) ? 4 : 2;
    pinMode(pin, (pull == 1) ? INPUT_PULLUP
                             : (pull == -1) ? INPUT_PULLDOWN : INPUT);
    gpio_set_input_enabled(pin, false);
}

static int rovingNode(ScanSession& s) {
    return (s.gpioIdx >= 0) ? gpioDef[s.gpioIdx][1] : -1;
}

// Claim one free routable GPIO into the session (the oscTryGpioRoute
// filter), saving its config for restoreRovingGpio. False = none free.
static bool claimRovingGpio(ScanSession& s) {
    s.gpioIdx = -1;
    for (int gi = 7; gi >= 0; gi--) {  // high end first, like oscTryGpioRoute
        int node = gpioDef[gi][1];
        if (globalState.config.gpioPythonOwned[gi]) continue;
        if (globalState.config.gpioPwmEnabled[gi]) continue;
        if (jumperlessConfig.top_oled.enabled &&
            (node == jumperlessConfig.top_oled.gpio_sda ||
             node == jumperlessConfig.top_oled.gpio_scl)) continue;
        if (infraOwnsNode(node)) continue;
        if (nodeHasAnyBridgePM(node)) continue;
        s.gpioIdx = gi;
        break;
    }
    if (s.gpioIdx < 0) return false;
    s.savedDir = globalState.config.gpioDirection[s.gpioIdx];
    s.savedPull = globalState.config.gpioPulls[s.gpioIdx];
    s.savedFloat = globalState.config.gpioReadFloating[s.gpioIdx];
    s.savedState = gpioState[s.gpioIdx];
    globalState.config.gpioReadFloating[s.gpioIdx] = 0;
    gpioReadFloating[s.gpioIdx] = 0;
    gpio_set_input_enabled(gpioDef[s.gpioIdx][0], false);
    return true;
}

static void restoreRovingGpio(ScanSession& s) {
    if (s.gpioIdx < 0) return;
    gpio_set_input_enabled(gpioDef[s.gpioIdx][0], true);
    globalState.config.gpioDirection[s.gpioIdx] = s.savedDir;
    globalState.config.gpioPulls[s.gpioIdx] = s.savedPull;
    globalState.config.gpioReadFloating[s.gpioIdx] = s.savedFloat;
    gpioReadFloating[s.gpioIdx] = s.savedFloat;
    gpioState[s.gpioIdx] = s.savedState;
    s.gpioIdx = -1;
}

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

int partScanBegin(ScanSession& s, const int* rows, int nRows, float iLimit_mA) {
    if (s.active) return -2;
    if (rp2040.cpuid() != 0) return -2;
    if (nRows < 2 || nRows > 3 || rows == nullptr) return -1;
    for (int i = 0; i < nRows; i++) {
        int r = rows[i];
        // Rows 29/30/59/60 are welcome: they exist as chip K/L x-pins
        // rather than y-lines, but DAC/GND/ISENSE/ADC legs all route to
        // them fine (bench, 2026-08-27: ADC0 on row 59 read the DAC to
        // within the normal fabric drop, and a 7-seg's common anode on 59
        // lit segments through DAC0 legs). The old refusal here made any
        // part with a pin on those columns electrically invisible.
        if (r < 1 || r > 60) return -1;
        for (int j = 0; j < i; j++)
            if (rows[j] == r) return -1;
    }
    // Never energize user wiring - by briefly REMOVING it (Kevin's ruling:
    // "if the part is wired in, just briefly unwire it to test"). Every
    // user bridge touching a DUT row, or the measurement path (ISENSE pair
    // / DAC0 - bench: a UART_TX->ISENSE_MINUS bridge left ~2.4mA standing
    // through INA0), is lifted with its duplicate count remembered and
    // restored by partScanEnd. Lifting only ever ISOLATES; with the DUT
    // rows bridge-free, nothing can power them but the part itself from
    // another lifted row - the powered-row check below still stands guard.
    s.nLift = 0;
    {
        const int liftCap = (int)(sizeof(s.liftA) / sizeof(s.liftA[0]));
        for (int i = 0; i < globalState.connections.numBridges; i++) {
            int n1 = globalState.connections.bridges[i][0];
            int n2 = globalState.connections.bridges[i][1];
            bool touches = (n1 == ISENSE_PLUS || n2 == ISENSE_PLUS ||
                            n1 == ISENSE_MINUS || n2 == ISENSE_MINUS ||
                            n1 == DAC0 || n2 == DAC0);
            for (int r = 0; !touches && r < nRows; r++)
                if (n1 == rows[r] || n2 == rows[r]) touches = true;
            if (!touches) continue;
            if (globalState.isEphemeralConnection(n1, n2)) continue;
            if (infraIsBridge(n1, n2)) continue;   // never touch infra's own
            if (s.nLift >= liftCap) return -3;     // too wired to briefly unwire
            s.liftA[s.nLift] = (int16_t)n1;
            s.liftB[s.nLift] = (int16_t)n2;
            s.liftDup[s.nLift] = globalState.connections.bridges[i][2];
            s.nLift++;
        }
        if (s.nLift > 0) {
            String err;
            for (int i = 0; i < s.nLift; i++)
                removeBridgeFromState(s.liftA[i], s.liftB[i], false);
            (void)err;
        }
    }

    // Park the probe power feed for the whole session, whatever its source
    // (a DAC0 feed shares the sweep's stimulus source; any feed keeps the
    // tip energized against a DUT row). This used to REFUSE under a DAC0
    // feed - which read as "part testing is broken" until the user switched
    // the source to GPIO. The teardown costs nothing: infraEvaluate runs at
    // the head of the very next refresh (the lift's, or the first leg
    // build), and partScanEnd's one restore refresh re-adds the feed.
    s.probePowerParked = true;
    s.probePowerRestore = infraProbePowerWanted();
    infraSetProbePowerEnabled(false);

    s.nRows = nRows;
    for (int i = 0; i < nRows; i++) s.rows[i] = rows[i];
    s.nEph = 0;
    s.ephAddFailed = false;
    s.iLimit_mA = iLimit_mA;
    s.dac0Restore = globalState.power.dac0;
    if (s.nLift > 0) refreshQuiet();   // the lift lands before any leg does

    // ADC channels: one per row, distinct (mask out what we already hold -
    // infraAcquireAdc's keep-what-you-own rule returns the held channel
    // again otherwise; the GuideChecks two-channel pattern). Channels whose
    // ADC node carries user wiring are never candidates - a fed lane reads
    // its feed, not the row (the census's poisoned-lane lesson; the
    // powered-row check below backstops anything subtler).
    for (int i = 0; i < 3; i++) s.adcCh[i] = -1;
    uint8_t mask = 0x0F;
    for (int c = 0; c < 4; c++)
        if (nodeHasAnyBridgePM(ADC0 + c)) mask &= (uint8_t)~(1u << c);
    for (int i = 0; i < nRows; i++) {
        int ch = infraAcquireAdc(INFRA_ADC_SCAN, mask, false);
        if (ch < 0) {
            // through the full teardown funnel: the lift already landed on
            // the fabric (refreshQuiet above), and a bare return here made
            // the briefly-unwired user bridges PERMANENT - markDirty had
            // already queued the deletion for the next auto-save
            partScanEnd(s);
            return -2;
        }
        mask &= (uint8_t)~(1u << ch);
        s.adcCh[i] = ch;
    }

    // One roving GPIO for pulls and gate duty (3-row sessions).
    s.gpioIdx = -1;
    if (nRows == 3) {
        if (!claimRovingGpio(s)) {
            partScanEnd(s);   // same funnel: the lift must go back
            return -5;
        }
    }

    s.active = true;

    // Gross-voltage sanity through momentary sense legs: a rail reaching a
    // DUT row through the part itself. One row at a time - all three rows
    // of a sip3 share a chip, and even three simultaneous ADC legs can
    // exhaust its lanes (bench: refused on chip-D rows). (0..2.6V is
    // indistinguishable from the floating lane bias here - the wiring gate
    // above is what actually guarantees cold rows.)
    bool powered = false;
    for (int i = 0; i < nRows; i++) {
        legAdd(s, s.rows[i], ADC0 + s.adcCh[i]);
        if (!legsBuild(s)) {
            partScanEnd(s);
            return -7;
        }
        delay(3);
        float v = readAdcVoltage(s.adcCh[i], 8);
        if (v > 2.9f || v < -0.5f) powered = true;
        legsClear(s);
    }
    if (powered) {
        partScanEnd(s);
        return -4;
    }
    partScanDischarge(s);
    return 0;
}

void partScanEnd(ScanSession& s) {
    restoreRovingGpio(s);
    setDac0voltage(s.dac0Restore, 0, 0, false);
    if (s.nEph > 0) {
        String err;
        for (int i = 0; i < s.nEph; i++)
            globalState.removeEphemeralConnection(s.ephA[i], s.ephB[i], err, false, 0);
        s.nEph = 0;
    }
    s.ephAddFailed = false;
    // put the briefly-lifted user wiring back, duplicate stacking and all,
    // so the one refresh below re-routes the user's world in one pass -
    // the parked probe power feed re-adds in the same pass (infraEvaluate
    // at the refresh head reads the restored flag)
    bool unParking = s.probePowerParked;
    if (unParking) {
        infraSetProbePowerEnabled(s.probePowerRestore);
        s.probePowerParked = false;
    }
    for (int i = 0; i < s.nLift; i++)
        addBridgeToState(s.liftA[i], s.liftB[i], s.liftDup[i], false);
    if (s.active || s.nRows > 0 || s.nLift > 0 || unParking) {
        refreshLocalConnections(1, 0, 0);  // plain redraw, never -1 (blanks)
        waitCore2();
    }
    s.nLift = 0;
    infraReleaseAdc(INFRA_ADC_SCAN);
    for (int i = 0; i < 3; i++) s.adcCh[i] = -1;
    s.nRows = 0;
    s.active = false;
}

void partScanDischarge(ScanSession& s) {
    if (!s.active) return;
    for (int i = 0; i < s.nRows; i++) legAdd(s, s.rows[i], GND);
    refreshQuiet();   // refusals here are harmless - discharge is best-effort
    delay(5);
    legsClear(s);
}

// ---------------------------------------------------------------------------
// junction map (3-row sessions)
// ---------------------------------------------------------------------------

void partScanJunctionMap(ScanSession& s, float v[3][3]) {
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) v[a][b] = 0.0f;
    if (!s.active || s.nRows != 3) return;

    // diagonal: floating readings, one sense leg at a time. The lane bias
    // is high-impedance, so give a just-discharged row time to drift back.
    partScanDischarge(s);
    delay(60);
    for (int i = 0; i < 3; i++) {
        legAdd(s, s.rows[i], ADC0 + s.adcCh[i]);
        if (!legsBuild(s)) continue;
        delay(30);
        v[i][i] = readAdcVoltage(s.adcCh[i], 8);
        legsClear(s);
    }

    // pairs: pull-up + sense on a (roving GPIO + ADC leg), GND leg on b,
    // third row untouched. 0.6V forward / 3.2V blocked - both outside E9's
    // band, and the roving pin's input buffer is off anyway.
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 3; b++) {
            if (a == b) continue;
            rovingIn(s, 0);
            legAdd(s, s.rows[a], ADC0 + s.adcCh[a]);
            legAdd(s, s.rows[a], rovingNode(s));
            legAdd(s, s.rows[b], GND);
            if (!legsBuild(s)) continue;   // leaves v[a][b] = 0 (distinct: real reads land 0.55+ or 3+)
            rovingIn(s, 1);
            delay(5);
            v[a][b] = readAdcVoltage(s.adcCh[a], 8);
            rovingIn(s, 0);
            legsClear(s);
        }
    }
    partScanDischarge(s);
}

// ---------------------------------------------------------------------------
// Kelvin servo + derived measurements (2- and 3-row)
// ---------------------------------------------------------------------------

// Build the drive fixture: DAC0->A, B->shunt->GND, sense legs on both.
static bool driveBuild(ScanSession& s, int idxA, int idxB) {
    setDac0voltage(0.0f, 0, 0, false);
    legAdd(s, DAC0, s.rows[idxA]);
    legAdd(s, s.rows[idxB], ISENSE_PLUS);
    legAdd(s, ISENSE_MINUS, GND);
    legAdd(s, s.rows[idxA], ADC0 + s.adcCh[idxA]);
    legAdd(s, s.rows[idxB], ADC0 + s.adcCh[idxB]);
    if (!legsBuild(s)) return false;
    delay(5);
    return true;
}

static void driveDown(ScanSession& s) {
    setDac0voltage(0.0f, 0, 0, false);
    delay(10);   // let the loop discharge anything capacitive
    legsClear(s);
}

bool partScanServo(ScanSession& s, int idxA, int idxB, float iTarget_mA,
                   float vMax, float* vPart, float* iPart) {
    if (vPart) *vPart = 0.0f;
    if (iPart) *iPart = 0.0f;
    if (!s.active || idxA < 0 || idxA >= s.nRows || idxB < 0 ||
        idxB >= s.nRows || idxA == idxB) return false;
    if (iTarget_mA > s.iLimit_mA) return false;
    if (vMax > 6.5f) vMax = 6.5f;
    if (!driveBuild(s, idxA, idxB)) return false;

    float v = 0.0f;
    bool conducting = false;
    bool reached = false;
    while (v < vMax - 0.001f) {
        v += conducting ? 0.08f : 0.30f;
        if (v > vMax) v = vMax;
        setDac0voltage(v, 0, 0, false);
        float i = inaSettledMa();
        if (i > s.iLimit_mA) {          // watchdog: straight off
            setDac0voltage(0.0f, 0, 0, false);
            if (iPart) *iPart = i;
            driveDown(s);
            return false;
        }
        if (i > 0.05f) conducting = true;
        if (i >= iTarget_mA) {
            if (vPart)
                *vPart = readAdcVoltage(s.adcCh[idxA], 8) -
                         readAdcVoltage(s.adcCh[idxB], 8);
            if (iPart) *iPart = i;
            reached = true;
            break;
        }
    }
    if (!reached) {
        if (vPart)
            *vPart = readAdcVoltage(s.adcCh[idxA], 8) -
                     readAdcVoltage(s.adcCh[idxB], 8);
        if (iPart) *iPart = inaSettledMa();
    }
    driveDown(s);
    return reached;
}

bool partScanResistance(ScanSession& s, int idxA, int idxB, float* ohms,
                        float* linearity) {
    if (ohms) *ohms = 0.0f;
    if (linearity) *linearity = 0.0f;
    float v1, i1, v2, i2;
    if (!partScanServo(s, idxA, idxB, 0.5f, 4.5f, &v1, &i1)) {
        if (!partScanServo(s, idxA, idxB, 0.05f, 4.5f, &v1, &i1))
            return false;               // >~90k: caller reports detect-only
        if (i1 < 0.02f) return false;
        if (ohms) *ohms = v1 / (i1 * 0.001f);
        if (linearity) *linearity = 1.0f;  // single point - unknown
        return true;
    }
    float r1 = v1 / (i1 * 0.001f);
    float target2 = (r1 > 700.0f) ? 2.0f : 5.0f;
    if (!partScanServo(s, idxA, idxB, target2, 4.5f, &v2, &i2)) {
        if (ohms) *ohms = r1;
        if (linearity) *linearity = 1.0f;
        return true;
    }
    float r2 = v2 / (i2 * 0.001f);
    if (ohms) *ohms = r2;
    if (linearity && r1 > 0.01f) *linearity = r2 / r1;
    return true;
}

bool partScanCapDetect(ScanSession& s, int idxA, int idxB, float vStep,
                       float* decayMs) {
    if (decayMs) *decayMs = 0.0f;
    if (!s.active) return false;
    if (!driveBuild(s, idxA, idxB)) return false;
    setDac0voltage(0.0f, 0, 0, false);
    delay(30);                          // start discharged
    setDac0voltage(vStep, 0, 0, false);
    float first = inaSettledMa();       // ~25-60ms after the step
    if (first < 0.10f) {
        driveDown(s);
        return false;                   // nothing charged
    }
    bool decayed = false;
    unsigned long t0 = millis();
    while (millis() - t0 < 600) {
        float i = inaSettledMa();
        if (i < 0.03f) {
            if (decayMs) *decayMs = (float)(millis() - t0);
            decayed = true;
            break;
        }
    }
    driveDown(s);
    return decayed;
}

// ---------------------------------------------------------------------------
// BJT / FET support (3-row sessions)
// ---------------------------------------------------------------------------

float partScanCalibratePull(ScanSession& s, int idx, bool up) {
    if (!s.active || s.nRows != 3 || idx < 0 || idx >= s.nRows) return -1.0f;
    if (s.gpioIdx < 0) return -1.0f;
    // DAC0 -> ISENSE+ -> shunt -> ISENSE- -> row: the shunt sits in the
    // feed, so INA0 reads the pull current exactly. Pulldown: row at 1.2V,
    // delta = 1.2/R. Pull-up: row at 0.3V, delta = 3.0/R (better signal).
    // Both keep the pad far from the E9 band (buffer is off regardless).
    rovingIn(s, 0);
    legAdd(s, DAC0, ISENSE_PLUS);
    legAdd(s, ISENSE_MINUS, s.rows[idx]);
    legAdd(s, s.rows[idx], rovingNode(s));
    legAdd(s, s.rows[idx], ADC0 + s.adcCh[idx]);
    if (!legsBuild(s)) return -1.0f;
    float vHold = up ? 0.3f : 1.2f;
    setDac0voltage(vHold, 0, 0, false);
    delay(5);
    float iOff = inaSettledMa();
    rovingIn(s, up ? 1 : -1);
    delay(5);
    float iOn = inaSettledMa();
    float vRow = readAdcVoltage(s.adcCh[idx], 8);
    rovingIn(s, 0);
    setDac0voltage(0.0f, 0, 0, false);
    legsClear(s);
    float d_mA = fabsf(iOn - iOff);
    if (d_mA < 0.008f) return -1.0f;    // below a usable resolution
    float vAcross = up ? (3.3f - vRow) : vRow;
    if (vAcross < 0.05f) return -1.0f;
    return vAcross / (d_mA * 0.001f);
}

bool partScanHfe(ScanSession& s, int eIdx, int bIdx, int cIdx, bool pnp,
                 float* i_mA, float* vb, float* vbe) {
    if (i_mA) *i_mA = 0.0f;
    if (vb) *vb = 0.0f;
    if (vbe) *vbe = 0.0f;
    if (!s.active || s.nRows != 3 || s.gpioIdx < 0) return false;
    // PNP: DAC feeds the emitter, collector returns through the shunt,
    // base sinks through the roving pulldown. NPN: DAC feeds the collector,
    // emitter returns through the shunt, base sources through the pull-up.
    // 1.2V drive keeps every biased node under the E9 band. The driven
    // row's voltage is taken from the DAC setpoint (bench: -77mV offset,
    // sub-ohm source) so the fixture stays at 5 legs.
    int drvIdx = pnp ? eIdx : cIdx;
    int shuntIdx = pnp ? cIdx : eIdx;
    setDac0voltage(0.0f, 0, 0, false);
    rovingIn(s, 0);
    legAdd(s, DAC0, s.rows[drvIdx]);
    legAdd(s, s.rows[shuntIdx], ISENSE_PLUS);
    legAdd(s, ISENSE_MINUS, GND);
    legAdd(s, s.rows[bIdx], rovingNode(s));
    legAdd(s, s.rows[bIdx], ADC0 + s.adcCh[bIdx]);
    if (!legsBuild(s)) return false;
    // ground the base first (through its pull... the junction clamps it at
    // ~0.65V from the driven rail once the drive comes up - never near 2V)
    rovingIn(s, pnp ? -1 : 1);
    setDac0voltage(1.2f, 0, 0, false);
    delay(8);
    // Two settled reads, keep the larger: this is steady DC, and a read
    // that lands stale-low (a conversion straddling the fixture settling)
    // once flipped an E/C orientation vote on the bench (hFE 509 with the
    // roles swapped). A stale read only ever under-reports here.
    float i = inaSettledMa();
    float i2 = inaSettledMa();
    if (i2 > i) i = i2;
    float vBase = readAdcVoltage(s.adcCh[bIdx], 8);
    rovingIn(s, 0);
    setDac0voltage(0.0f, 0, 0, false);
    legsClear(s);
    if (i_mA) *i_mA = i;
    if (vb) *vb = vBase;
    if (vbe) *vbe = fabsf(1.123f - vBase);  // 1.2V set - 77mV bench offset
    return true;
}

bool partScanFetProbe(ScanSession& s, int gIdx, int dIdx, int sIdx,
                      bool gateHigh, float* id_mA) {
    if (id_mA) *id_mA = 0.0f;
    if (!s.active || s.nRows != 3 || s.gpioIdx < 0) return false;
    // gate hard-driven by the roving pin, source through the shunt to GND,
    // drain from DAC0 at 0.6V (current-limited channel test, ~4mA max into
    // a dead short through the ~150R loop).
    setDac0voltage(0.0f, 0, 0, false);
    rovingOut(s, gateHigh);
    legAdd(s, s.rows[gIdx], rovingNode(s));
    legAdd(s, DAC0, s.rows[dIdx]);
    legAdd(s, s.rows[sIdx], ISENSE_PLUS);
    legAdd(s, ISENSE_MINUS, GND);
    if (!legsBuild(s)) {
        rovingIn(s, 0);
        return false;
    }
    setDac0voltage(0.6f, 0, 0, false);
    delay(5);
    float i = inaSettledMa();
    setDac0voltage(0.0f, 0, 0, false);
    rovingOut(s, false);   // always discharge the gate after
    delay(2);
    rovingIn(s, 0);
    legsClear(s);
    if (id_mA) *id_mA = i;
    return true;
}

// ---------------------------------------------------------------------------
// whole-board census (the Auto scan's first pass)
// ---------------------------------------------------------------------------

int partScanCensus(uint8_t* rowFlags, float* v0dbg, float* v1dbg,
                   bool (*abortCheck)(void),
                   void (*progress)(int row, int state)) {
    if (rowFlags == nullptr) return -2;
    if (rp2040.cpuid() != 0) return -2;
    static ScanSession s;   // a claims-only session: no DUT rows, just legs
    if (s.active) return -2;
    for (int i = 0; i <= 60; i++) rowFlags[i] = 0;
    s.nEph = 0;
    s.ephAddFailed = false;
    s.nLift = 0;

    // A CLEAN measurement lane, or no scan at all. A user bridge on an ADC
    // node - or anything else feeding the lane - poisons every reading:
    // bench, 2026-08-27, ~5V on a claimed lane censused the WHOLE board at
    // 4.8V (45 phantom hits clustered into two giant "chips"). Structurally
    // skip user-wired ADC channels, then trust nothing: discharge each
    // candidate lane to GND, release it, and reject any lane something
    // pulls back up (a floating lane stays near zero for milliseconds).
    int ch = -1;
    {
        uint8_t mask = 0x0F;
        for (int c = 0; c < 4; c++)
            if (nodeHasAnyBridgePM(ADC0 + c)) mask &= (uint8_t)~(1u << c);
        while (mask != 0) {
            int cand = infraAcquireAdc(INFRA_ADC_SCAN, mask, false);
            if (cand < 0) break;
            legAdd(s, ADC0 + cand, GND);
            bool built = legsBuild(s);
            if (built) delay(2);
            legsClear(s);
            delay(2);
            float open = built ? readAdcVoltage(cand, 4) : 99.0f;
            if (built && open < 1.0f && open > -1.0f) {
                ch = cand;
                break;
            }
            Serial.print("\r\nPARTSCAN lane ADC");
            Serial.print(cand);
            if (!built) {
                Serial.println(" discharge refused - rejected");
            } else {
                Serial.print(" reads ");
                Serial.print(open, 2);
                Serial.println("V after discharge - the lane is being FED, rejected");
            }
            infraReleaseAdc(INFRA_ADC_SCAN);
            mask &= (uint8_t)~(1u << cand);
        }
    }
    if (ch < 0) return -6;   // no clean lane; the caller says so loudly
    if (!claimRovingGpio(s)) {
        infraReleaseAdc(INFRA_ADC_SCAN);
        return -2;
    }

    // Prime the measurement lane: the FIRST poke reads its ring sample off
    // a cold ADC lane and lands marginally low - row 1 census-flagged 3/3
    // scans on this bench (v0 2.0-2.1 against the 2.2 threshold) while a
    // real 2-lead identify read it EMPTY. One discarded poke of the first
    // pokeable row settles the lane for everyone after it.
    for (int row = 1; row <= 60; row++) {
        if (row == 29 || row == 30 || row == 59 || row == 60) continue;
        if (nodeHasAnyBridgePM(row)) continue;
        legAdd(s, row, ADC0 + ch);
        legAdd(s, row, rovingNode(s));
        if (legsBuild(s)) {
            rovingOut(s, true);
            delay(2);
            rovingIn(s, 0);
            (void)readAdcVoltage(ch, 1);
            delay(6);
            (void)readAdcVoltage(ch, 4);
            legsClear(s);
        }
        break;
    }

    int found = 0;
    for (int row = 1; row <= 60; row++) {
        if (row == 29 || row == 30 || row == 59 || row == 60) {
            rowFlags[row] = 3;
            continue;
        }
        if (nodeHasAnyBridgePM(row)) {
            rowFlags[row] = 2;   // the user's wiring; their part, their net
            continue;
        }
        if (abortCheck != nullptr && abortCheck()) break;
        if (progress != nullptr) progress(row, 0);   // the cursor row

        legAdd(s, row, ADC0 + ch);
        legAdd(s, row, rovingNode(s));
        if (!legsBuild(s)) {
            rowFlags[row] = 4;
            if (progress != nullptr) progress(row, 2);
            continue;
        }
        rovingOut(s, true);      // charge the row hard
        delay(2);
        rovingIn(s, 0);          // release (buffer off - a pure open)
        float v0 = readAdcVoltage(ch, 1);   // FIRST ring sample, ~20-40us out
        delay(6);
        float v1 = readAdcVoltage(ch, 4);
        legsClear(s);
        if (v0dbg) v0dbg[row] = v0;
        if (v1dbg) v1dbg[row] = v1;
        // Bench truth (this board, 2026-08-26): EVERY row bleeds through its
        // own sense leg with tau ~200us, so nothing "holds charge" for long.
        // What separates a part is (a) the instant charge-share - a junction
        // or load eats the charge within nanoseconds, so the first sample
        // after release is already low, where an empty row still reads
        // ~85-90% of the drive - or (b) a pin that HOLDS the row somewhere
        // 6ms later (TTL inputs sat at 1.63V, driven outputs at 0.3-0.4V;
        // empties are at ~0.00 by then).
        if (v0 < 2.2f || v1 > 0.12f) {
            rowFlags[row] = 1;
            found++;
        }
        if (progress != nullptr) progress(row, rowFlags[row] == 1 ? 1 : 2);
    }

    restoreRovingGpio(s);
    infraReleaseAdc(INFRA_ADC_SCAN);
    refreshLocalConnections(1, 0, 0);
    waitCore2();
    return found;
}

int partScanPairSweep(uint8_t* rowFlags, bool (*abortCheck)(void),
                      void (*progress)(int row, int state),
                      int16_t* gapPairsOut, int* nGapPairsOut,
                      int gapPairsCap) {
    if (rowFlags == nullptr) return -2;
    if (rp2040.cpuid() != 0) return -2;
    static ScanSession s;
    if (s.active) return -2;
    // The sweep owns DAC0 and the shunt for ~100 measurements. The probe
    // power feed is PARKED for the duration (any source - a DAC0 feed
    // shares the stimulus source outright; the old refusal here read as
    // "the sweep never runs" under a DAC0 feed), and a user net touching
    // the measurement path gets the SAME brief-unwire treatment an
    // identify session gives it (Kevin's ruling; bench: the standing
    // UART_TX->ISENSE_MINUS wire would otherwise disable the sweep on
    // this board forever). Both go back at every exit below.
    bool ppRestore = infraProbePowerWanted();
    infraSetProbePowerEnabled(false);
    s.nLift = 0;
    {
        const int liftCap = (int)(sizeof(s.liftA) / sizeof(s.liftA[0]));
        for (int i = 0; i < globalState.connections.numBridges; i++) {
            int n1 = globalState.connections.bridges[i][0];
            int n2 = globalState.connections.bridges[i][1];
            bool touches = (n1 == ISENSE_PLUS || n2 == ISENSE_PLUS ||
                            n1 == ISENSE_MINUS || n2 == ISENSE_MINUS ||
                            n1 == DAC0 || n2 == DAC0);
            if (!touches) continue;
            if (globalState.isEphemeralConnection(n1, n2)) continue;
            if (infraIsBridge(n1, n2)) continue;
            if (s.nLift >= liftCap) {   // too wired to briefly unwire
                // no lift has landed and no refresh ran since the park -
                // the feed is physically untouched, the flag goes back
                infraSetProbePowerEnabled(ppRestore);
                return -2;
            }
            s.liftA[s.nLift] = (int16_t)n1;
            s.liftB[s.nLift] = (int16_t)n2;
            s.liftDup[s.nLift] = globalState.connections.bridges[i][2];
            s.nLift++;
        }
        if (s.nLift > 0) {
            for (int i = 0; i < s.nLift; i++)
                removeBridgeFromState(s.liftA[i], s.liftB[i], false);
            refreshQuiet();
        }
    }
    uint8_t claimMask = 0x0F;   // parity with census claims, minus wired ADCs
    for (int c = 0; c < 4; c++)
        if (nodeHasAnyBridgePM(ADC0 + c)) claimMask &= (uint8_t)~(1u << c);
    int ch = infraAcquireAdc(INFRA_ADC_SCAN, claimMask, false);
    if (ch < 0) {
        infraReleaseAdc(INFRA_ADC_SCAN);
        // the lift (and the feed teardown, if a lift refresh ran) already
        // landed - put both back before bailing, one refresh
        infraSetProbePowerEnabled(ppRestore);
        for (int i = 0; i < s.nLift; i++)
            addBridgeToState(s.liftA[i], s.liftB[i], s.liftDup[i], false);
        refreshQuiet();
        s.nLift = 0;
        return -2;
    }
    s.nEph = 0;
    s.ephAddFailed = false;
    s.gpioIdx = -1;

    // Bench truth: with the ISE legs routed the shunt reads a CONSTANT
    // standing current (2.39mA here - the GuideChecks ledger's board-
    // internal sink), and the stimulus shifts every EMPTY pair by an
    // equally constant amount (-0.77mA +-10uA at 1V on this board).
    // Absolute thresholds are hopeless against an artifact that big; the
    // pair population is its own calibration - sweep everything, take the
    // per-direction MEDIAN as the empty line, flag what deviates. The
    // 2N3906's junction pairs sat 2.2mA off the line.
    //
    // Drive is 3.0V (was 1.0V): an LED's 1.8-3.1V forward knee never
    // conducted at 1V - the exact cross-gap LED this sweep now looks for
    // was invisible to it. 3V through the ~130R loop caps a dead short at
    // ~23mA for 3ms, which identify's own servo already does deliberately.
    // The higher drive also drops the deviation floor to 0.15mA, so
    // resistors up to ~19k make the line (10k deviated only ~0.1mA at 1V,
    // under the old 0.35 threshold - silently unseen).
    setDac0voltage(0.0f, 0, 0, false);
    (void)inaSettledMa();   // settle the poll pipeline before the loop

    int newHits = 0;
    if (nGapPairsOut != nullptr) *nGapPairsOut = 0;
    {
        // Two arrangements cover the layouts seen on real boards (Kevin,
        // 12:53: "we're only scanning adjacent pairs still, so we miss the
        // LED on 21-51"): legs side by side (n,n+1) and legs straddling
        // the center channel (n,n+30 - same column, other half). The later
        // arrangement only sweeps pairs the first left unexplained, so a
        // mostly-found board pays almost nothing extra. A one-apart pass
        // (n,n+2) ran here for one bench round and was cut for time
        // (Kevin, 14:00: "insane long") - skip-one and wider spans wait
        // for the group-testing pooled-query refinement.
        static const int kGap[2] = {1, 30};
        static float di[2][58];
        static int16_t pairA[58];
        static int16_t pairB[58];
        bool aborted = false;
        // the gap-1 pass's per-direction empty line doubles as the edge
        // stage's reference (same fixture, same artifact - too few edge
        // pairs exist to self-calibrate)
        float edgeMedian[2] = {0.0f, 0.0f};
        bool haveEdgeMedian = false;
        for (int gi = 0; gi < 2 && !aborted; gi++) {
            int gap = kGap[gi];
            int nPairs = 0;
            for (int half = 0; half < 2 && nPairs < 58; half++) {
                int lo, hi;
                if (gap == 30) {
                    if (half == 1) break;   // one pass spans both halves
                    lo = 1; hi = 28;        // b = a+30 covers 31..58
                } else {
                    lo = half ? 31 : 1;
                    hi = (half ? 58 : 28) - gap;
                }
                for (int a = lo; a <= hi && nPairs < 58; a++) {
                    int b = a + gap;
                    // at least one row still unexplained, neither wired /
                    // refused: a part can straddle a census hit (a BJT's
                    // base HOLDS, its E and C don't - the base flag must
                    // not veto the junctions)
                    bool aFree = (rowFlags[a] == 0), bFree = (rowFlags[b] == 0);
                    bool aOk = aFree || rowFlags[a] == 1 || rowFlags[a] == 5;
                    bool bOk = bFree || rowFlags[b] == 1 || rowFlags[b] == 5;
                    if (!(aOk && bOk) || (!aFree && !bFree)) continue;
                    if (abortCheck != nullptr && abortCheck()) {
                        aborted = true;
                        break;
                    }
                    if (progress != nullptr) progress(a, 3);   // pair cursor
                    pairA[nPairs] = (int16_t)a;
                    pairB[nPairs] = (int16_t)b;
                    di[0][nPairs] = di[1][nPairs] = 1.0e9f;  // no-reading sentinel
                    for (int dir = 0; dir < 2; dir++) {
                        int src = dir ? b : a, snk = dir ? a : b;
                        setDac0voltage(0.0f, 0, 0, false);
                        legAdd(s, DAC0, src);
                        legAdd(s, snk, ISENSE_PLUS);
                        legAdd(s, ISENSE_MINUS, GND);
                        if (!legsBuild(s)) continue;
                        setDac0voltage(3.0f, 0, 0, false);
                        delay(3);
                        // settled: the first read re-syncs the conversion
                        // pipeline, the second is trustworthy - single reads
                        // both missed a real junction and invented one (stale)
                        (void)inaFreshMa();
                        di[dir][nPairs] = inaFreshMa();
                        setDac0voltage(0.0f, 0, 0, false);
                        legsClear(s);
                    }
                    if (progress != nullptr) progress(a, 4);   // painted from flags
                    nPairs++;
                }
            }
            if (nPairs > 0 && nPairs < 5) {
                // no silent caps: a nearly-full board can leave an
                // arrangement without enough pairs to self-calibrate
                Serial.print("PARTSCAN sweep gap-");
                Serial.print(gap);
                Serial.print(": only ");
                Serial.print(nPairs);
                Serial.println(" pairs - too few to judge, skipped");
                continue;
            }
            if (nPairs < 5) continue;
            for (int dir = 0; dir < 2; dir++) {
                float sorted[58];
                int n = 0;
                for (int i = 0; i < nPairs; i++)
                    if (di[dir][i] < 1.0e8f) sorted[n++] = di[dir][i];
                if (n < 5) continue;
                for (int x = 1; x < n; x++)
                    for (int y = x; y > 0 && sorted[y] < sorted[y - 1]; y--) {
                        float t = sorted[y]; sorted[y] = sorted[y - 1]; sorted[y - 1] = t;
                    }
                float median = sorted[n / 2];
                if (gap == 1) {
                    edgeMedian[dir] = median;
                    haveEdgeMedian = true;
                }
                for (int i = 0; i < nPairs; i++) {
                    if (di[dir][i] > 1.0e8f) continue;
                    if (fabsf(di[dir][i] - median) > 0.15f) {
                        int a = pairA[i], b = pairB[i];
                        if (rowFlags[a] == 0) { rowFlags[a] = 5; newHits++; }
                        if (rowFlags[b] == 0) { rowFlags[b] = 5; newHits++; }
                        if (progress != nullptr) {
                            progress(a, 1);
                            progress(b, 1);
                        }
                        // a cross-gap pair is its own finding: the two rows
                        // land in different halves and can never form one
                        // span - the launcher identifies them as a pair
                        if (gap == 30 && gapPairsOut != nullptr &&
                            nGapPairsOut != nullptr &&
                            *nGapPairsOut < gapPairsCap) {
                            bool dup = false;
                            for (int q = 0; q < *nGapPairsOut; q++)
                                if (gapPairsOut[2 * q] == a) dup = true;
                            if (!dup) {
                                gapPairsOut[2 * *nGapPairsOut] = (int16_t)a;
                                gapPairsOut[2 * *nGapPairsOut + 1] = (int16_t)b;
                                (*nGapPairsOut)++;
                            }
                        }
                    }
                }
            }
        }

        // Edge-row stage: rows 29/30/59/60 exist as chip K/L x-pins - the
        // census can't poke them, but parts END on those columns (bench,
        // 2026-08-27: Kevin's 7-seg's common anode sits on row 59, its dp
        // cathode on 29, and the whole display was invisible because
        // nothing ever measured against them). Pairs (n, edge) with n in
        // the 6 columns beside the edge sweep against the gap-1 pass's
        // empty line; an edge row that shows hits then earns its MIRROR
        // window too (the display's top segments bond to the BOTTOM
        // common - only mirror pairs can see them). Evidence-gated, so an
        // empty board pays one window, not two. Deviating pairs go to
        // gapPairsOut like cross-gap pairs do - the edge row can never
        // join a span (its census flag stays 3) and the launcher groups
        // pairs sharing one edge row into a single finding.
        if (!aborted && haveEdgeMedian) {
            static const int kEdge[4] = {29, 30, 59, 60};
            bool edgeHadHit[4] = {false, false, false, false};
            for (int stage = 0; stage < 2 && !aborted; stage++) {
                int nPairs = 0;
                for (int e = 0; e < 4 && !aborted && nPairs < 58; e++) {
                    if (stage == 1 && !edgeHadHit[e]) continue;
                    int E = kEdge[e];
                    int base = (stage == 0) ? E : ((E <= 30) ? E + 30 : E - 30);
                    for (int n = base - 6; n <= base - 1 && nPairs < 58; n++) {
                        if (n < 1 || n == 29 || n == 30 || n == 59 || n == 60)
                            continue;
                        if (rowFlags[n] != 0 && rowFlags[n] != 1 &&
                            rowFlags[n] != 5)
                            continue;
                        if (abortCheck != nullptr && abortCheck()) {
                            aborted = true;
                            break;
                        }
                        if (progress != nullptr) progress(n, 3);
                        pairA[nPairs] = (int16_t)n;
                        pairB[nPairs] = (int16_t)E;
                        di[0][nPairs] = di[1][nPairs] = 1.0e9f;
                        for (int dir = 0; dir < 2; dir++) {
                            int src = dir ? E : n, snk = dir ? n : E;
                            setDac0voltage(0.0f, 0, 0, false);
                            legAdd(s, DAC0, src);
                            legAdd(s, snk, ISENSE_PLUS);
                            legAdd(s, ISENSE_MINUS, GND);
                            if (!legsBuild(s)) continue;
                            setDac0voltage(3.0f, 0, 0, false);
                            delay(3);
                            (void)inaFreshMa();
                            di[dir][nPairs] = inaFreshMa();
                            setDac0voltage(0.0f, 0, 0, false);
                            legsClear(s);
                        }
                        if (progress != nullptr) progress(n, 4);
                        nPairs++;
                    }
                }
                for (int dir = 0; dir < 2; dir++) {
                    for (int i = 0; i < nPairs; i++) {
                        if (di[dir][i] > 1.0e8f) continue;
                        if (fabsf(di[dir][i] - edgeMedian[dir]) <= 0.15f)
                            continue;
                        int n = pairA[i], E = pairB[i];
                        for (int e = 0; e < 4; e++)
                            if (kEdge[e] == E) edgeHadHit[e] = true;
                        if (rowFlags[n] == 0) { rowFlags[n] = 5; newHits++; }
                        if (progress != nullptr) progress(n, 1);
                        if (gapPairsOut != nullptr && nGapPairsOut != nullptr &&
                            *nGapPairsOut < gapPairsCap) {
                            bool dup = false;
                            for (int q = 0; q < *nGapPairsOut; q++)
                                if (gapPairsOut[2 * q] == n &&
                                    gapPairsOut[2 * q + 1] == E)
                                    dup = true;
                            if (!dup) {
                                gapPairsOut[2 * *nGapPairsOut] = (int16_t)n;
                                gapPairsOut[2 * *nGapPairsOut + 1] = (int16_t)E;
                                (*nGapPairsOut)++;
                            }
                        }
                    }
                }
            }
        } else if (!aborted) {
            // no silent caps: without the gap-1 empty line the edge rows
            // can't be judged, and a part ending on 29/30/59/60 stays dark
            Serial.println("PARTSCAN edge rows unswept (no calibration"
                           " population) - parts ending on 29/30/59/60"
                           " won't be seen");
        }
    }
sweepDone:
    // hardware back to what the state says it is - the sweep borrowed DAC0
    // (a bare 0V here left a user's dac_set() supply silently dead while
    // the UI still showed their voltage)
    setDac0voltage(globalState.power.dac0, 0, 0, false);
    infraReleaseAdc(INFRA_ADC_SCAN);
    // the briefly-unwired measurement-path wiring and the parked probe
    // power feed go back, duplicate stacking and all, one refresh
    infraSetProbePowerEnabled(ppRestore);
    for (int i = 0; i < s.nLift; i++)
        addBridgeToState(s.liftA[i], s.liftB[i], s.liftDup[i], false);
    s.nLift = 0;
    refreshLocalConnections(1, 0, 0);
    waitCore2();
    return newHits;
}
