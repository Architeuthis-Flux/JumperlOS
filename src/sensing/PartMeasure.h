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
    uint8_t savedOwned = 0;         // gpioPythonOwned before the claim
    int savedFunc = 0;              // pin mux (gpio_get_function) before the claim
    // live legs, for the teardown funnel
    int16_t ephA[8]; int16_t ephB[8]; uint8_t nEph = 0;
    bool ephAddFailed = false;  // a leg never even STAGED (slots/table full)
                                // - legsBuild turns this into a clean false
                                // instead of measuring an incomplete fixture
    // user bridges briefly lifted for the session (Kevin's ruling: "if the
    // part is wired in, just briefly unwire it to test") - restored with
    // their duplicate stacking by partScanEnd
    int16_t liftA[12]; int16_t liftB[12]; int16_t liftDup[12];
    uint8_t nLift = 0;
    float dac0Restore = 0.0f;
    float iLimit_mA = 10.0f;
    // the probe power feed (DAC0/GPIO -> BUFFER_IN) is parked for the whole
    // session - Kevin's ruling supersedes the old DAC0-feed refusal: "just
    // disconnect the probe power entirely when doing part testing, connect
    // it back when it's finished"
    bool probePowerParked = false;
    bool probePowerRestore = false;
};

// The scan's liveness pulse (Kevin, 2026-08-30: "it should never seem like
// it's frozen for more than a quarter of a second"). Nullable; when set,
// every wait loop inside a session - INA conversion settles, decay
// watches, drains - calls it, and the owner animates whatever surface it
// likes (the Parts app shimmers the rows under the meter). The hook must
// be cheap and self-rate-limited; it runs on core 0 inside measurement
// loops. Cleared by whoever set it - never left dangling past an app.
extern void (*partScanActivityHook)(void);

// Begin/end. rows are breadboard rows (1-60, not 29/30/59/60). User wiring
// on the DUT rows or the measurement path (ISENSE pair / DAC0) is briefly
// LIFTED for the session and restored by partScanEnd - s.nLift says how
// many wires that took; the probe power feed is parked the same way (any
// source - it shares DAC0 and energizes the tip) and restored at end.
// Returns 0 ok, -1 bad args, -2 machinery busy (ADC
// pool / not core 0), -3 too wired to briefly
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

// Capacitance MEASUREMENT (Kevin, 2026-08-28: "we should also be testing
// part values like capacitors"). Two fixtures, picked by what the part
// does:
//   1) pull-down decay (~5nF-20uF): charge rows[idxA] from the roving
//      GPIO, release into the pin's own pulldown, and time the row's ADC
//      through two fixed crossings. tau/R_pull = C, with R_pull measured
//      in-session (partScanCalibratePull) - the internal pull is 30-70k
//      and per-chip, never a constant.
//   2) hard-loop charge integration (bigger, or no GPIO free): step DAC0
//      through the drive fixture and integrate the charge into the cap -
//      the first 30ms from the SINK row's voltage (the pair sweep's
//      Ohm's-law trick, ~25us samples, bench-constant path resistance),
//      the rest from the INA's true mA. C = Q / V(end).
// True = a capacitor is PRESENT. *farads > 0 when it could be measured
// (0 = detect-only: presence proven, value refused), *tauMs = the stage-1
// time constant when stage 1 ran. Claims the roving GPIO when the session
// doesn't already hold one; partScanEnd restores it either way.
bool partScanCapMeasure(ScanSession& s, int idxA, int idxB, float* farads,
                        float* tauMs);

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

// Tier-1 unpowered clamp fingerprint (DESIGN_IC_IDENTIFICATION.md 5.1). An
// unpowered chip is a diode mesh: every real pin conducts to the supply
// pins through its ESD/substrate structures, and WHICH directions conduct
// at WHAT drop is a family fingerprint. Bench truth (7447, 2026-08-28):
// standard-TTL paths are junction CHAINS, not single diodes - inputs read
// ~1.5V to GND (substrate->collector->base->emitter), open-collector
// outputs ~1.7V to GND, and NOTHING conducts pin-above-VDD (that absence
// is the open-collector/TTL signature; a 74HC clamps both ways at ~0.7V).
// One servo at 1mA per direction - the 50uA two-current ratio law was
// tried and dropped: it lives inside the fabric's transient floor and
// painted phantom paths (see clampProbeDir in the .cpp).
enum : uint8_t {
    PART_CLAMP_OPEN = 0,        // no conduction inside 3.3V at 1mA
    PART_CLAMP_JUNCTION = 1,    // junction(s): >= ~0.4V drop at 1mA
    PART_CLAMP_RESISTIVE = 2,   // hard tie: < 0.25V drop (strap to a rail)
};
struct ClampPin {
    int row = 0;
    bool probed = false;   // false: rail row, x-pin row, or session refused
    uint8_t toGnd = PART_CLAMP_OPEN;   // gnd row driven ABOVE the pin
    uint8_t toVdd = PART_CLAMP_OPEN;   // pin driven ABOVE the vdd row
    float vfGnd = 0.0f;    // drop at 1mA when conducting
    float vfVdd = 0.0f;
};
// pinRows in physical pin order (the caller owns the geometry - see
// PartDefinition::nodeForPin); pins that ARE the rails come back unprobed
// with row filled in. Two 2-row sessions per pin (one per direction) -
// never a 3-row session, whose roving-GPIO claim would refuse every pin on
// a board with all GPIOs wired (bench: the 7-seg record owns all eight).
// Wiring on the rail rows themselves (a rail feed keeping the chip
// powered) is lifted ONCE for the whole run and restored after. Returns
// pins probed, or -1 bad args. abortCheck (nullable) is polled between
// pins. progress (nullable) narrates onto whatever surface the caller
// owns, per pin: state 0 = probing this row now, then the verdict -
// 1 = open, 2 = junction to GND, 3 = junction to VDD, 4 = both ways,
// 5 = hard tie (strapped), 6 = skipped (a rail row, or the session
// refused).
int partScanClampFingerprint(const int* pinRows, int nPins, int gndRow,
                             int vddRow, ClampPin* out,
                             bool (*abortCheck)(void) = nullptr,
                             void (*progress)(int row, int state) = nullptr);

// Ground every session row briefly (GND legs, no pads), then remove.
void partScanDischarge(ScanSession& s);

// Whole-board row census (the Auto scan's first pass): every free row gets
// a charge-share poke - drive it hard for a moment, release, and watch
// whether the charge stays (empty row, ~tens of pF, holds for many ms) or
// slumps into something (a part leg shares it with the rest of the part
// instantly). rowFlags[1..60]: 0 = empty, 1 = something there, 2 = has
// user wiring (skipped - its part is the user's business), 3 = not
// pokeable (rows 29/30/59/60), 4 = fabric refused the poke legs.
// abortCheck (may be nullptr) is polled between rows; poked rows are left
// charged (harmless - they leak back to the lane bias).
// v0/v1 (may be nullptr, [61]) get the raw release/settled voltages for
// tuning. Returns the number of rows flagged 1, -2 = machinery busy, or
// -6 = no clean measurement lane (every candidate ADC is user-wired or
// reads driven after a discharge - a fed lane censuses the whole board
// as one giant phantom part; bench, 2026-08-27).
// progress (optional) narrates onto whatever UI the caller owns:
//   state 0 = probing this row, 1 = row hit, 2 = row empty,
//   3 = sweeping pair (row, row+1), 4 = pair done (repaint from flags).
int partScanCensus(uint8_t* rowFlags, float* v0dbg, float* v1dbg,
                   bool (*abortCheck)(void),
                   void (*progress)(int row, int state) = nullptr);

// The census's second pass: an isolated junction part (a lone transistor,
// a diode) is INVISIBLE to the single-row poke - the drive pre-charges the
// whole part through its own junctions, so nothing shares at release
// (bench, 2N3906). This sweeps free pairs in two arrangements - adjacent
// (n,n+1) and across the center channel (n,n+30 - the same column on the
// other half) - at 3.0V through the shunt, both directions (~10ms per
// check; current is read as the sink row's voltage on the scan ADC - the
// ants' Ohm's-law idea - not the 50ms-gated INA): a junction, an LED, or
// a resistor (up to ~19k) conducts, and both rows get flagged 5. A third, evidence-gated stage sweeps the six
// columns beside each x-pin row (29/30/59/60, unpokeable by the census)
// against that row, widening to the mirror half when an edge row shows
// hits - a 7-seg's common anode on row 59 fans out to segments in both
// halves (bench, 2026-08-27) and is invisible any other way. User wiring on the
// measurement path (DAC0 / the ISENSE pair) is briefly LIFTED like an
// identify session does it, and restored on every exit (bench: the standing
// UART_TX->ISENSE_MINUS wire would otherwise sideline the sweep forever).
// Conducting CROSS-GAP pairs also land in gapPairsOut ([2i]=top row,
// [2i+1]=its +30 partner, up to gapPairsCap pairs): their rows live in
// different halves and can never form one span, so the launcher must treat
// each pair as its own finding.
// Returns rows newly flagged, or -2 = busy / probe fed from DAC0 (infra,
// can't be lifted) / more path wiring than the lift list holds - then the
// scan stays census-only and the launcher says so.
int partScanPairSweep(uint8_t* rowFlags, bool (*abortCheck)(void),
                      void (*progress)(int row, int state) = nullptr,
                      int16_t* gapPairsOut = nullptr,
                      int* nGapPairsOut = nullptr, int gapPairsCap = 0);

#endif // PART_MEASURE_H
