# Component Identification ("Part ID") — Design Blueprint (2026-08-20)

**FOLLOW-UP BRANCH.** Only 4 hooks land in the projects/guided-placement branch (§11).
Siblings: DESIGN_PROJECTS_SUBSYSTEM.md, DESIGN_GUIDED_PLACEMENT.md. V5 only.

> **2026-08-26 status: Layers 0+1 are LANDED and bench-verified** —
> `src/sensing/PartMeasure.*` (session/fixtures/primitives),
> `src/sensing/PartClassify.*` (two-lead + three-lead trees), MP binding
> `part_identify(r1, r2 [, r3])`. The 2N3906 on rows 17/18/19 identifies as
> BJT_PNP roles E/B/C in any tap order, conf 0.90, Vbe 0.62V, hFE ~350,
> ~1.5s total. See §15 below for the bench facts that amended this design;
> the deltas from the blueprint as written:
> - **Per-measurement legs, not a persistent fixture.** A sip3 part has all
>   three rows on ONE CH446Q; a parked 6-leg fixture exhausts that chip's
>   lanes (the fabric refused the 6th leg on the bench). Every primitive
>   builds its 3-5 legs, measures, removes them (~10ms per bypass pass), and
>   validates the build against `unconnectablePaths[]` — a state-side add
>   the hardware refused must fail loudly, not read 0V.
> - **Hard-lows and discharge are row→GND legs, not GPIO pads** (no E9
>   exposure, no claims); ONE roving GPIO covers every pull/gate duty.
> - The junction map runs at ~50uA pull-up sensing (0.6V fwd / 3.2V blocked,
>   both outside the E9 band); hFE drives from DAC0 at 1.2V so the base
>   never enters the pad-leak band; E/C by two-orientation Ic asymmetry;
>   Ib = Vbase / the in-session-calibrated pull (±20-25%).
> - `identifyTwoLead` needs no GPIO at all (DAC + ISENSE + ADC lanes only,
>   fully E9-free) — diode/LED/Zener/R/cap-detect per §5's recipes with the
>   ratio law (Kübbeler doc): junction Vf barely moves over a 20x current
>   ratio, a resistor's V scales, a cap keeps climbing on a re-read.

## 0. Verified fabric facts this design builds on

From `src/routing/MatrixState.cpp:83-156` (`chipStatusInit`, V5r1):

- Each breadboard row lands on exactly one A–H chip y-pin (7 rows/chip; y0 = a
  dedicated `BOUNCE_NODE` line per chip). **Exceptions: rows 29/30/59/60 exist only as
  x-pins** (K x0=29, K x1=59, L x0=30, L x1=60).
- Each A–H chip has one lane each to I, J, K, L plus 2 lanes to each other A–H chip.
- Chip K x-map: x4=TOP_RAIL, x5=BOTTOM_RAIL, x6=DAC1, x7=DAC0, x8-x11=ADC0-3, x15=GND.
- Chip L x-map: x3=ADC4_5V, x4-x11=RP_GPIO_20-27, x15=GND.
- **ISENSE_PLUS is only on chip I x11; ISENSE_MINUS only on chip J x11.** The
  topological gift: an INA-in-series drive into a DUT row is forced through the DUT
  chip's J lane, leaving its K lane (the only 1-hop path to ADC0-3) free for sense
  taps → true Kelvin measurement for the dominant case.
- Bridges added dup=0 never get stacked paths (infra bridges guarantee this,
  InfraPaths.h:31-33).
- INA0: INA219, `setMaxCurrentShunt(1, 2.0)`, 16-sample averaging (~8.5-17 ms/conv),
  LSB ~30 µA, ~2 Ω shunt (Peripherals.cpp:1760-1777). CM 0..+26 V — **never drive
  negative through the shunt**.
- AdcRing: 48 kHz/ch × 8, 10.7 ms history, post-hoc window slicing.
- `fastConnectPath`/`buildEphemeralRoute` (RouteSafety.cpp:565+) currently route
  node↔ADC0-3 only; 1-hop = 2 crosspoints; generation-guarded; short-checked.
- Unsolved 3-hop read anomaly (~10% pull toward 3.4 V, FAST_PATH_SEND_HANDOFF.md §2) —
  1-hop reads true.
- `pairSenseTap` (NetVoltageScan.cpp:330) — zero-skew dual-node ring read;
  `senseNodeVoltage` (:230) drift-based floating rejection (`nodeDrift[]`).
- ADC arbitration: `infraAcquireAdc(InfraAdcUser, mask, allowShared)`
  (InfraPaths.h:159-180).
- Crosspoint R: `calibration.crosspoint_resistance` = 40 Ω small-signal, ~30 Ω @ 12 mA
  — regime-dependent (config.h:274-289). DEV_PLANS_82026.md Plan 2 (crosspoint-R
  calibration) is a prerequisite/sibling — cite, don't duplicate.
- `measureCurrent(int,int)` stub at Commands.cpp:~970. `MeasurementType` reserves
  CURRENT/LOGIC_PROBE/DATA_SCAN (MeasureMode.h:28). Apps table has free slots.

## 1. Architecture

**Layered: C++ primitives + C++ shipped classifier; every primitive exposed to
MicroPython; dev iteration via host-side HIL Python.**

- **Layer 0 — primitives** (`src/sensing/PartMeasure.*`): fixture setup/teardown,
  safety envelope, Kelvin R, junction Vf servo, cap step-response, I-V sweep,
  discharge.
- **Layer 1 — classifier** (`src/sensing/PartClassify.*`): AVR-tester-style decision
  tree. C++ so the probe→OLED flow is self-contained and HIL can regression-test
  shipped behavior.
- **Layer 2 — MicroPython bindings**: every primitive + `identify()`. Users can write
  their own classifiers; prototype in Python, freeze into the C++ tree when stable.

## 2. Routing tiers

**Sense: `fastConnectPath` ADC taps, 1-hop ENFORCED, sequential.** The session owns
the fabric (≤4 fixture bridges live), so 1-hop routes are available by construction; a
3-hop route from the builder is a bug signal → abort. Sequential taps through the
*same* ADC cancel `adcZero` exactly — for settled DC this beats `pairSenseTap` (defer
its export). Optional stage-2: row→L x3 = ADC4 (2 crosspoints, 0-5 V) for simultaneous
two-row sensing.

**Stimulus, two stages:**
- **Stage 1 (bring-up): state-tier ephemeral bridges** — `addBridgeToState(a, b, 0,
  false)` + `refreshLocalConnections` + `waitCore2()`, the SelfTest droop-phase
  pattern (SelfTest.cpp:973-1030). Always dup=0. After refresh, **read back**
  `globalState.connections.paths` to (a) count crosspoints per segment, (b) verify the
  DUT chip's K lane wasn't consumed; if consumed → sense through the K-side stub and
  subtract one crosspoint (I·R_xp), flag `degraded`.
- **Stage 2: raw fixture builder** — extend RouteSafety (or `MeasureFixture.cpp`
  beside it) with stimulus shapes: K-resource→row via neighbor-chip bounce
  (K x_src → y_N → N's K lane → N y0 BOUNCE → N's DUT-chip lane → DUT row, ~4
  crosspoints); ISENSE±→row direct via the DUT chip's I/J lane (2 crosspoints);
  L GPIO→row. **Policy: never consume the DUT chip's K lane; bounce chips ≠ DUT chip;
  distinct bounce chips per segment.** Makes pin-role permutation ~µs-scale.

Teardown = disconnect + one `refreshConnections` (restores user state). A
`routingGeneration` bump mid-session aborts cleanly. v1 refuses DUT legs on rows
29/30/59/60.

## 3. Layout

```
src/sensing/PartMeasure.h/.cpp     — session, fixtures, primitives (Layer 0)
src/sensing/PartClassify.h/.cpp    — result structs, decision tree (Layer 1)
src/sensing/PartVerify.cpp         — verifyPart() bridge to project parts: entries
src/routing/MeasureFixture.h/.cpp  — stage-2 raw fixture builder
src/Apps.cpp                       — "Part ID" + "Curve Tracer" apps
src/Commands.cpp                   — implement measureCurrent(n1,n2) for real
JumperlessMicroPythonAPI.cpp + modjumperless.c — bindings
src/selfreflection/SelfTest.cpp    — scan-calibration phase (per-regime loop R)
test/hil/test_part_id.py + test/hil/partid_bench.md
Phase 2: src/remembering/PartDb.h/.cpp + data/partdb/
```

## 4. API sketch

```cpp
struct ScanSession;            // acquire ADCs, pause NetVoltageScan, audit, snapshot
int  scanSessionBegin(ScanSession& s, uint32_t iLimit_uA = 10000); // <0 = refused
void scanSessionEnd(ScanSession& s);   // discharge, park DACs, full refresh

struct Meas { float value; float sigma; uint8_t xpHops; bool kelvin; bool ok; };
Meas measureResistance(ScanSession&, int rowA, int rowB);
Meas measureJunction  (ScanSession&, int anode, int cathode, float i_uA);
Meas measureCapacitance(ScanSession&, int rowA, int rowB);
Meas measureLeakage   (ScanSession&, int rowA, int rowB, float v);
struct IvPoint { float v, i; };
int  sweepIV(ScanSession&, int rowA, int rowB, float v0, float v1, int n,
             IvPoint* out, bool inaAccurate);
void dischargeRows(ScanSession&, const int* rows, int n);
float rowsPoweredCheck(const int* rows, int n);

enum class PartType : uint8_t { EMPTY, UNKNOWN, SHORT_CIRCUIT, RESISTOR, CAPACITOR,
       DIODE, LED, ZENER, BJT_NPN, BJT_PNP, NFET, PFET, INDUCTOR_OR_LOW_R };
enum class PinRole : uint8_t { NONE, A, K, B, C, E, G, D, S, LEAD };
struct PartResult {
    PartType type; float confidence;
    float value;        // R / C / Vf / Vz
    float value2;       // hFE / Vth / leakage
    uint8_t rows[3]; PinRole roles[3]; uint8_t nRows;
    bool degraded;      // non-Kelvin fallback used
};
PartResult identifyTwoLead(int rowA, int rowB);
PartResult identifyThreeLead(int a, int b, int c);
int verifyPart(const PartDefinition& decl, PartResult* out);   // guided projects
```

MicroPython: `part_identify([rows]) -> dict`, `measure_res`, `measure_cap`,
`measure_vf`, `iv_sweep(..., accurate=False) -> list`, `discharge(rows)`.

**The v1 front door (Kevin, 2026-08-21, from the bench-notes session):** one
call that does the whole job with everything defaulted —

```python
identify_part(leftmost_row=scan, pins=autodetect, types=all, timeout=1s)
```

Pass only what you know (here `leftmost_row`); the defaults cycle through the
known tester types, autodetect which adjacent/straddled rows hold pins, figure
the part out, and return a value. `identifyTwoLead`/`identifyThreeLead` above
are the layers under this; `identify_part` is the shape users (and the guided
flow's place-step display) actually call. The guided-placement checks will
show whatever they already measure in the meantime — full identification
lands here, in its own session.

## 5. Recipes (all: audit → powered-row refusal → discharge; positive-only through the
shunt; DAC servo ramps ≤0.1 V/step with INA current-limit checks)

- **Resistor (Kelvin, the centerpiece)**: `DAC0→ISENSE_PLUS`, `ISENSE_MINUS→rowA`
  (forced through the DUT chip's J lane), `GND→rowB`. Servo DAC0 to mid-range
  current. I from INA0 (median ~6), V_A/V_B by sequential 1-hop taps (same ADC).
  `R = (V_A − V_B) / I`. High-R correction: ~1 MΩ ADC divider loads the node — take
  INA reading with taps open or subtract V/1MΩ. Auto-range 100 µA → 1 mA → 10 mA.
- **Diode/LED/Zener**: same fixture; Vf at 100 µA/1 mA/~5 mA (log-linearity separates
  junction from resistor). Reverse: swap row roles, ramp to +7 V watching INA — knee
  <7 V ⇒ Zener; leakage-only ⇒ diode/LED. Vf ≥ ~1.5 V ⇒ LED (visibly lights at 5 mA).
  Both directions symmetric+linear ⇒ resistor; both junction-like ⇒ two legs of a
  transistor.
- **Capacitor**: detect via decaying INA current. (a) 0.3-50 µF: pre-connect 1-hop
  sense tap, make the final stimulus crosspoint the step edge, slice the RC ramp from
  the ring (20.8 µs/sample), τ from log-fit, `C = τ/R_loop` with live-tared R. (b)
  >50 µF: charge integration (∫I dt / ΔV). Floor: τ ≈ 50 µs at ~150 Ω ⇒ **~0.3 µF
  minimum**; sub-0.3 µF is a phase-2 experiment. Promise nothing below.
- **BJT**: junction map over 6 ordered pairs at ~500 µA → base = pin with two forward
  junctions; polarity ⇒ NPN/PNP; C/E by hFE asymmetry (AVR-tester method). hFE by the
  crossbar-native two-configuration trick: same operating point, INA in the collector
  loop (read I_C) then the base loop (read I_B); `hFE = I_C/I_B`, one reference
  ammeter. INA floor ⇒ I_B ≥ ~100 µA (near-saturation hFE, like cheap DMMs).
- **MOSFET**: gate = no-DC-conduction pin; body diode ⇒ D/S + polarity; confirm by
  gate 0↔3.3 V toggling D-S conduction; Vth = gate ramp (0→5 V, 0.1 V steps) crossing
  I_D ~250 µA-1 mA (report the threshold current). Depletion/JFET best-effort, low
  confidence. **Always discharge the gate after.**
- **Curve tracer**: accurate ~10 pts/s (INA ≤100 Hz) / fast ~100+ pts/s (ΔV over tared
  loop R, ±10%). Output: terminal CSV stream, coarse ASCII plot, OLED reuse of
  MeasureMode's 128-sample mini-scope, MP generator.

### Expected accuracy (be honest in UI and docs)

| Measurement | Band | Expected |
|---|---|---|
| R | 100 Ω–100 kΩ | ±2–5% (headline) |
| R | 10–100 Ω | ±5–10% |
| R | 100 kΩ–1 MΩ | ±10–30% |
| R | >1 MΩ | detect-only |
| Diode Vf | 0.15–4 V | ±20–30 mV |
| Zener Vz | ≤ ~6.5 V | ±5% |
| C | 0.3–50 µF | ±10–20% |
| C | 50 µF–10 mF | ±10% |
| C | <0.3 µF | not v1 |
| hFE | @ I_B ≥ 100 µA | ±25% |
| Vth | 0.5–4.5 V | ±100 mV |
| L | — | out of scope |

## 6. User flow

"Part ID" app: OLED prompts "tap the part's legs with the probe"; taps accumulate 2-3
rows; connect-button confirms → scan. Results: OLED type glyph + value + pin roles;
terminal full detail (confidence, degraded); LED painting of DUT rows per type,
pin-role colors for 3-lead. Optional later: occupancy pre-pass (charge-poke +
drift-decay signatures) suggesting candidate rows. Guided-project "verify placement"
iterates `parts:` entries with `type:`/`value:` → `verifyPart()` → green/red painting.

## 7. Safety

Rails-off precondition + `rowsPoweredCheck` (>0.1 V ⇒ refuse, no override in v1);
current limits from servo ramps + INA watchdog (10 mA default; 30 mA opt-in LED test);
GPIO stimulus pad-limited; rails never stimulus. `auditLastChipXY` at session start;
suspect ⇒ full refresh. Session end: discharge, park DACs, full refresh. Suspend
NetVoltageScan for the session; `infraAcquireAdc(INFRA_ADC_SCAN)` exclusive. Core 0
only.

## 8. Calibration

Prereq: DEV_PLANS Plan 2. **Per-session live tare**: measure the actual drive-loop R
against a directly-grounded spare row in the actual regime — kills both the 40 Ω/hop
quantization and the 30-vs-40 Ω regime problem. One-time SelfTest phase stores
`calibration.scan_loop_ohms_smallsig`/`_12ma` (normal config migration; no
current-branch hook). Per-route-shape offsets for the 3-hop anomaly: not needed
(forbidden by construction).

## 9. IC / pinout database (phase 2)

`/partdb/` on FatFS, one file per family:

```yaml
part: "7400"
aka: [74HC00, 74LS00]
package: DIP14
power: { vcc: 14, gnd: 7 }
class: logic
vectors:
  - { in: {1:0, 2:0}, out: {3:1} }
  - { in: {1:1, 2:1}, out: {3:0} }
```

Plus `/partdb/i2c_addr.txt` (address → candidates → optional WHO_AM_I probes) used
with the existing `i2cScan` app. Runner: data-driven C++ — inputs via routable GPIOs
(3.3 V) or FakeGpio outputs (nodes 150-157) for 5 V logic; outputs via GPIOs/TDM
virtual inputs. Auto-ID = run all vector sets matching pin count. Power pins connected
last, current-limited first power-up, INA watchdog. `parts:` entries reference records
via `part_id: "74xx/7400"`. Seed content authored from datasheets (facts aren't
copyrightable); check licenses before importing any existing database.

## 10. Build order

0. Prereq: Plan 2 calibration; HIL-verify chip-K x-map assumptions.
1. Session + safety + Kelvin R (+ real `measureCurrent`). HIL known resistors;
   characterize true INA floor + ADC noise (corrects the accuracy table).
2. Two-lead classifier (EMPTY/SHORT/R/diode/LED/Zener/cap-detect), terminal UI.
3. Capacitance.
4. Fixture builder (stage 2) → un-degraded Kelvin + fast permutation; ADC4-via-L.
5. Three-lead: junction map, BJT/FET, hFE/Vth; probe-select + OLED + LED UI.
6. Curve tracer + MP bindings + CSV.
7. verifyPart + guided-project integration.
8. Phase 2: /partdb/, vector runner, I2C table.

## 11. Hooks required in the CURRENT branch (only these)

1. `parts:` schema: optional `type:`, `value:`, `part_id:` — and the parser **must
   ignore unknown keys** (the tolerance is the real hook).
2. Project manifest parsing tolerant of unknown top-level sections (future `verify:`).
3. Reserve `INFRA_ADC_SCAN` in `InfraAdcUser` (InfraPaths.h:159) — one enum line.
4. Leave `measureCurrent(int,int)` signature untouched (Commands.cpp:~970).

## 12. HIL

Fixture-free: empty rows → EMPTY; powered-board refusal; tare sanity; session restore;
1-hop-only assertion; audit-clean after 100 sessions. Bench kit
(test/hil/partid_bench.md): 100 Ω/1 k/10 k/100 k 1%, 1 µF film, 100 µF electro,
1N4148, red LED, 3V3-5V1 Zener, 2N3904, 2N3906, 2N7000 — type + value in band +
pinout across all 6 row-permutations.

## 13. Licensing

JumperlOS is MIT; the AVR TransistorTester (k/m firmware) is GPL. Use the
documentation (ttester_eng104k.pdf) as the algorithm reference — clean-room C++, never
read/port GPL source. Header note: "measurement method informed by the documented
approach of the AVR TransistorTester; original implementation, MIT."

## 14. Risks

3-hop anomaly (mitigated by construction; `degraded` flag keeps stage-1 honest); INA
floor unknown in practice (phase-1 characterization gates hFE/high-R claims);
write-only CH446Q divergence (audit + generation guard; worst case a wrong reading,
never a short — `wouldShort` still gates every closure); I2C0 congestion (pace INA
reads, pause OLED during tight sequences); parts-in-circuit misreads (v1 messaging:
isolated parts on selected rows); MAX_EPHEMERAL_CONNECTIONS must fit 3-4 fixture
bridges alongside MeasureMode's (check at phase 1).

## 15. Bench-measured hardware facts (2026-08-26, V5 r7, MP prototype + C++ verify)

Raw JSONs in the session scratchpad (charz_results, junction_map_2n3906,
hfe*_2n3906); the numbers that survived into code:

- **Hard-drive loop** DAC0→row→GPIO-low total ≈ **130Ω** (2Ω shunt + ~40Ω
  DAC-side path + ~90Ω GPIO pad+path). DAC0 setpoint offset **−77mV**, gain
  ~0.98 (read voltages with the ADC, don't trust the setpoint).
- **ADC0-3 sense legs are near-high-Z**: input rests at **~2.31V bias**;
  loading on a 50k-pulled node ~0.1V (Thevenin ~440k-class as loaded, but
  float-recovery after a discharge takes tens of ms — treat the bias as a
  float *signature*, give it settle time, never rely on it as a bias source).
- **E9 pad leak, live**: a 50k pulldown cannot drag a node from above ~2.2V
  (latched at 2.115V); a high-Z input node anywhere in ~1.5-2.6V gets pulled
  toward ~2.1V with >50uA available — it CANCELLED the base pulldown at
  VE=2.5V (zero collector current, both orientations identical) and works
  fine at VE=1.2V (base 0.55V). Session pins run with the input buffer OFF
  (`gpio_set_input_enabled(pin,false)`) and biased nodes stay <1.5V.
- **INA1 (DAC0 shunt) carries a permanent V-proportional ~1.5k load** (probe
  feed / output stage) — baseline-subtract or don't use it; **INA0 in the
  DUT's ground-side return leg reads 0.000mA on an empty chain** and 5uA/LSB
  from the shunt register. A user bridge on ISENSE± left 2.4mA standing
  through INA0 — the session refuses (-6) when ISENSE±/DAC0 carry wiring.
- **Junction map bands (through the real fabric, ADC legs attached)**:
  Si forward 0.55-0.75V, blocked ~3.2V, BJT E-C floating-base leak ~2.7-2.9V
  (varies with base loading — decide on it, never report it).
- **2N3906 ground truth**: junction map finds B+PNP in one pass; fwd/rev Ic
  asymmetry **21x** (3.59mA vs 0.17mA) — E/C unambiguous; hFE ~340-380 at
  the 1.2V operating point (quasi-sat, honest ±25%).
- **infraAcquireAdc keep-what-you-own**: repeated acquisition under one user
  returns the SAME channel unless its bit is masked out of the next call —
  three "distinct" legs all landed on ADC0 and shorted the DUT rows together
  through the lane until masked (the GuideChecks two-channel pattern).
- **SWD bench hygiene**: a week-old zombie openocd (platformio earlephilhower)
  held the CMSIS-DAP probe and corrupted flash *readback* (phantom verify
  failures, sticky AP faults reporting stale addresses). Recovery that
  worked: kill the zombie, pyusb-reset the probe, and when `reset halt`
  breaks while plain attach works, a **watchdog TRIGGER (0x400d8000 =
  0x80000000) via the working core's AP** full-chip-resets back to a
  bootable state. `target/rp2350-rescue.cfg` parks the chip in a state the
  standard cfg cannot examine (core0 AP faults on the flash driver's DMA
  read) — avoid it unless truly bricked.
