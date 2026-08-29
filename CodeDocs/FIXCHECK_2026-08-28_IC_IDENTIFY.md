# FIXCHECK 2026-08-28 — IC identification (Tier 1 + Tier 3) landed

Session executed `DESIGN_IC_IDENTIFICATION.md` §5.3 in full. Everything
below is bench-verified on Kevin's V5 r7 (7447 + 2N3906 + 7-seg + copper
demo wiring) unless marked otherwise. Both firmware targets
(`jumperless_v5`, `jumperless_og`) build at every step; final HIL:
`test/hil/test_ic_identify.py` **PASS (23 checks)**.

## What landed

| Piece | Where | Bench evidence |
|---|---|---|
| Tier-1 clamp collector `partScanClampFingerprint` | `src/sensing/PartMeasure.h/.cpp` | 3 identical runs on the 7447: `fp=GGGGGGG-GGGBGGG-`, Vf repeatable ±0.05V |
| MP surface `part_fingerprint(base,w,gnd,vdd)` | `JumperlessMicroPythonAPI.cpp`, `modjumperless.c`, qstr table | `match=7447:1,74595:13` |
| partdb `fingerprint:` field + matcher | `PartDb.h/.cpp`, `generate_partdb.py`, `logic_7400.yaml` (9 seeds, 4 deduped) | matcher ranks 7447 over 74595 by 12 |
| Orientation-aware matching (`partdbFingerprintMismatchOriented`) | `PartDb.cpp` | 180° = U-order shifted by HALF (cyclic, not reversal) — proven by the pin-1-on-row-10 7447 |
| Rails carried into the scan confirm pass (`chipGnd/chipVdd`) | `PartsApp.cpp` | — (compile-verified; scan UI needs eyes) |
| Tier-3 vector schema (`PartDbVectorSet`, wired `vectorSetIdx`) + `vec:` YAML | `PartDb.h`, `generate_partdb.py` (+ self-tests) | 8 sets emitted |
| Seed vectors: 7400/02/04/08/32/47/86 + 74595 sequence | `logic_7400.yaml` | 7447 set passes on the real chip; 74595 set correctly fails on it |
| Vector runner `partsVectorIdentify` | `PartsApp.cpp` (+ `PartsApp.h`), MP `part_vectors()` | `7447(r):pass, 74595(r):fail@1` in 4.2s, board-powered mode |
| Scan "identify?" escalation (survivor-filtered picker, tapless commit) | `PartsApp.cpp` confirm pass | machinery bench-proven via MP; UI gestures not yet eyeballed |
| Parts > Test re-assign for generic `IC*` (closes §2.6's gap) | `partsTestLauncher` | same machinery; UI flow not yet eyeballed |
| `PartDbI2cIdent` widening: `whoami2`, `probe_order`, sorted candidates | `PartDb.h/.cpp`, generator, `modules.yaml` (+ BMP388/390 records) | compile-verified; MPU-before-DS3231 ordering encoded |
| Address-hint table widened from REF §6.3 | `partsI2CAddressName` | rodata only |
| §5.2 power decision recorded | `DESIGN_IC_IDENTIFICATION.md` | see below |

## Bench-measured facts that amended the design

- **The 7447's TRUE unpowered map** (measured with correct rails): every
  pin ONE substrate diode to GND at 0.66–0.68 V; nothing conducts
  pin-above-VCC. The earlier 1.5–1.7 V "junction chain" readings were an
  artifact of swapped rails — the placed record was 180° wrong until Kevin's
  pin-1-on-row-10 correction. Die quirk: BI/RBO shows a repeatable 0.89 V
  junction to VCC (1 fingerprint mismatch, tolerated).
- **The 50 µA two-current ratio law was tried and dropped**: fixture-build
  charge draining through the shunt false-trips a 50 µA target at the first
  DAC step, sometimes repeatably in-session. One 1 mA servo per direction
  never false-fired; junction-vs-strap comes from the drop itself
  (≥0.4 V junction, <0.25 V tie).
- **Power delivery (§5.2 DECIDED)**: TOP_RAIL through the crossbar holds a
  TTL 7447 at 4.05–4.43 V at the pin and it operates; DAC0@3.3 V stays the
  CMOS path; family-wide records run a 5 V pass then a 3.3 V retry (74HC@5V
  can't trust 3.3 V GPIO drive).
- **The copper lesson (twice)**: physical jumpers fed the chip AROUND a
  freshly built shunt fixture (INA read 0 on a live part), so the runner
  pre-reads BOTH rails: hot VDD ⇒ board-powered mode (use the user's
  supply); hot "GND" ⇒ refuse — swapped rails are geometrically identical
  to the flipped orientation, and grounding a live node is never right.
- **Core 2's `readGPIO()` twiddles unowned pads**: the runner's drive pins
  are claimed the way MicroPython claims them (owned flag + GPIO_FUNC_SIO +
  `__dmb()`), or driven levels get unmade mid-vector.
- **Bridge ownership**: only remove bridges the run itself created — a
  blind GND add-then-remove stole the placed record's own GND route
  (`partsBridgeExists` guard).
- **`gpio_set_dir` wants booleans**: int 1 lands as INPUT (the wrapper's
  OUTPUT=0 convention); `True` = OUTPUT.
- **OLED pin exclusion is connection-type-gated**: on type 2 the panel
  rides I2C0, so its configured gpio_sda/scl are stale numbers — a blanket
  exclusion cost the 7-input 7447 set its 7th GPIO (`partsFreeGpios`).

## Bench state

The 7447 record was re-placed to match physical reality (pin 1 on row 10,
rotated; VCC row 40 ↔ TOP_RAIL, GND row 3 ↔ GND) — the old record had the
power routes BACKWARDS and reverse-fed the chip whenever the rail was on.
All wiring verified restored at session end (GND↔3, TOP_RAIL↔40, all eight
GPIO↔segment bridges, rail DACs untouched at 5.0/3.37 V). The Jumperless
desktop app was closed for the session (held port 1).

## Honest ceilings / work-list

- **UI gestures unverified by eyes**: the scan "identify?" prompt and the
  Parts > Test re-assign flow run the bench-proven machinery, but the
  encoder/probe gesture paths and OLED copy need a human (or the SWD
  encoder-input harness).
- `claimRovingGpio` (PartMeasure) still blanket-excludes the OLED pins
  regardless of connection type — same latent bug fixed in
  `partsFreeGpios`; on a board with 2+ GPIO consumers, 3-row sessions
  refuse early. Fix both or fold the filters.
- Vector reads are ~2 refreshes per output move (~4s for two dip16
  candidates board-powered; self-powered adds passes). Fine for opt-in;
  batching legs would halve it.
- The 7447 record's fingerprint is the TTL map; an HCT47 mismatches all 14
  and falls back to the picker — honest, but a `C`-map alias record would
  serve HCT users better once one is on a bench.
- Records without vector sets can never auto-name; the fingerprint-only
  match currently informs (match= field, picker filter) but doesn't place.
- `part_fingerprint`/`part_vectors` are V5-only (OG refuses with status=-2,
  same as `part_identify`).
- MAX30102-style multi-read idents (`whoami2`) are schema+data only — the
  I2C probe doesn't yet read WHO_AM_I registers at scan time.
