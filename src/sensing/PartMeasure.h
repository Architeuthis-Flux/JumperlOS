// SPDX-License-Identifier: MIT
#ifndef PART_MEASURE_H
#define PART_MEASURE_H

// Part-identification measurement primitives (Layer 0 of
// CodeDocs/DESIGN_PART_ID_FOLLOWUP.md). One session at a time, core 0 only.
//
// Fixture discipline: every measurement builds exactly the legs it needs as
// ephemeral bridges, measures, and removes them again (one bypass refresh
// each way, ~10ms). Nothing stays parked: a sip3 part has all three rows on
// ONE CH446Q, and a persistent 6-leg fixture exhausts that chip's lanes -
// the fabric refused the sixth leg on the bench. Per-measurement legs keep
// the worst case at 5 and make every build verifiable (ephRefused).
//
// Hard-low and discharge duties are row->GND legs, not GPIO pads (no E9
// exposure, no pad claims). One roving GPIO covers every pull/gate duty.
// Sensing is always an ADC leg (ADC0-3 via INFRA_ADC_SCAN); the ISENSE
// shunt (INA0, 5uA/LSB) sits in the DUT's own return leg for currents.
//
// Bench-measured facts this layer is built on (2026-08-26, V5 r7):
//   - hard drive loop through the fabric ~110-140R total; DAC->row ~40R
//   - internal pulls ~30-70k; RP2350 E9 pad leak: any GPIO input holding a
//     node in ~1.5-2.6V drifts to ~2.1V - GPIO-biased nodes stay <1.5V and
//     session pins run with the input buffer off
//   - a floating routed row reads the ADC lane bias (~2.3V) but recovers
//     from a discharge only slowly - float verdicts need settle time
//   - INA1 (DAC0 shunt) carries a permanent ~1.5k V-proportional load;
//     INA0 in the DUT leg reads 0 on an empty chain (ground-side shunt)
// All state is one small static session; constants are rodata. No heap.

#include <stdint.h>

struct ScanSession {
    bool active = false;
    int rows[3] = { 0, 0, 0 };
    int nRows = 0;
    int adcCh[3] = { -1, -1, -1 };  // INFRA_ADC_SCAN channels (nRows of them)
    int gpioIdx = -1;               // the one roving GPIO (gpioDef index)
    int savedDir = 0, savedPull = 0;
    uint8_t savedFloat = 0, savedState = 0;
    // live legs, for the teardown funnel
    int16_t ephA[8]; int16_t ephB[8]; uint8_t nEph = 0;
    // user bridges briefly lifted for the session (Kevin's ruling: "if the
    // part is wired in, just briefly unwire it to test") - restored with
    // their duplicate stacking by partScanEnd
    int16_t liftA[12]; int16_t liftB[12]; int16_t liftDup[12];
    uint8_t nLift = 0;
    float dac0Restore = 0.0f;
    float iLimit_mA = 10.0f;
};

// Begin/end. rows are breadboard rows (1-60, not 29/30/59/60). User wiring
// on the DUT rows or the measurement path (ISENSE pair / DAC0) is briefly
// LIFTED for the session and restored by partScanEnd - s.nLift says how
// many wires that took. Returns 0 ok, -1 bad args, -2 machinery busy (ADC
// pool / DAC0 feeding the probe / not core 0), -3 too wired to briefly
// unwire (more than the lift list holds), -4 a row reads powered, -5 no
// free routable GPIO, -7 the fabric refused a leg.
int  partScanBegin(ScanSession& s, const int* rows, int nRows,
                   float iLimit_mA = 10.0f);
void partScanEnd(ScanSession& s);

// 3-row sessions: pairwise junction map. v[a][b] = volts at row a with a
// pulled up (50k) and b grounded, the third row untouched; the diagonal
// v[i][i] = row i's floating reading (~2.3V lane bias = nothing attached).
// Si junction forward ~0.55-0.75V, blocked ~3.2V. Each pair is its own
// 3-leg fixture (ADC+pull on a, GND on b) - ~6 fabric passes total.
void partScanJunctionMap(ScanSession& s, float v[3][3]);

// Kelvin servo between two session rows (by index into s.rows):
// DAC0 -> rows[idxA], rows[idxB] -> ISENSE_PLUS -> shunt -> GND, ADC legs
// on both rows. Raises DAC0 until INA0 reads >= iTarget_mA or vMax.
// True = target reached; *vPart = V(A)-V(B) Kelvin, *iPart = mA. On false
// the outputs hold the vMax operating point (reverse-knee scans read this).
bool partScanServo(ScanSession& s, int idxA, int idxB, float iTarget_mA,
                   float vMax, float* vPart, float* iPart);

// Two-point Kelvin resistance (~0.5mA and ~5mA, auto-ranged down to 50uA).
// *linearity = ratio of the two readings (~1.0 = ohmic).
bool partScanResistance(ScanSession& s, int idxA, int idxB, float* ohms,
                        float* linearity);

// Capacitor detect: step to vStep and watch the shunt current decay.
bool partScanCapDetect(ScanSession& s, int idxA, int idxB, float vStep,
                       float* decayMs);

// 3-row: calibrate the roving GPIO's pull against rows[idx] through the
// shunt-in-feed fixture (DAC0 -> ISENSE+ -> shunt -> ISENSE- -> row).
// up=false: pulldown at 1.2V row bias; up=true: pull-up at 0.3V.
// Returns ohms or <= 0.
float partScanCalibratePull(ScanSession& s, int idx, bool up);

// 3-row, BJT: one hFE orientation. PNP: DAC0 -> rows[eIdx] at 1.2V, base on
// the roving GPIO's pulldown, rows[cIdx] -> shunt -> GND. NPN mirrored
// (DAC0 -> collector, base pull-up, emitter -> shunt; the shunt reads Ie).
// *i_mA = shunt current, *vb = base row volts, *vbe = |V(driven row)-vb|.
bool partScanHfe(ScanSession& s, int eIdx, int bIdx, int cIdx, bool pnp,
                 float* i_mA, float* vb, float* vbe);

// 3-row, FET: gate driven by the roving GPIO, source -> shunt -> GND,
// drain from DAC0 at ~0.6V; *id_mA = channel current at that gate level.
bool partScanFetProbe(ScanSession& s, int gIdx, int dIdx, int sIdx,
                      bool gateHigh, float* id_mA);

// Ground every session row briefly (GND legs, no pads), then remove.
void partScanDischarge(ScanSession& s);

#endif // PART_MEASURE_H
