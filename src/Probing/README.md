# Probe Engine v2 — architecture, calibration model, and open issues

Handoff notes for the calibration-free probe decode work (Aug 2026). Read
this before touching anything in `src/Probing/` or debugging pad decode.

## Background: how pad sensing works

The V5 probe detects which pad the tip touches through a resistor ladder:

```
tip drive ──(Rsrc)── probe tip ── pad ── ladder resistance R_i ── ADC_PROBE_SENSE ──(10K)── GND
                                                                        │
                                                                      ADC5
```

- Every pad (rows 1–60, 4 rail pads, 30 nano header pins, logo/SF/building
  pads = 101 positions) has a unique series resistance `R_i` to the sense
  node. `ADC5 counts = FS * (Vtip/Vrail) * 10K / (10K + R_i + Rsrc)`.
- **SELECT position**: tip driven by PROBE_PIN (GPIO high = the 3.3V rail).
  ADC reference is the same rail, so raw counts are rail-ratiometric — USB
  sag cancels by construction. `Rsrc` ≈ GPIO Rout + 2×47R ≈ 125Ω.
- **MEASURE position**: tip driven by the routable buffer (GPIO-powered
  claim by default, `debug.probe_power_gpio`; DAC0 fallback). ADC7 is
  hardwired to the buffer output = the live tip voltage.
- The ladder resistors were chosen so counts step *uniformly* (~40 counts
  per pad, "bands"). Consequence used everywhere below: `R_i / ΔR_i` is
  nearly constant (~22) through the chain, so a **proportional** error in
  resistance shifts *every* mid-chain pad by the same fraction of a band.

**Why legacy drifted:** the legacy engine decodes with
`map(reading, probe_min, probe_max, 101, 0)` — two user-calibrated count
endpoints stretched linearly. 1% scale error = one row at the top; the
MEASURE frame mixed absolute DAC volts with `vTip/3.3f` and endpoints
seeded from the SELECT frame. Users had to recalibrate constantly.

## What exists now

### Two engines, runtime flag

- `debug.probe_engine_v2` (default **on**, forced off on OG). Legacy
  `src/Probing.cpp`/`Probing.h` are untouched as the fallback engine.
- Dispatch lives in the inline wrappers in `Probing.h`
  (`probeMode/justReadProbe/readProbeRaw/checkPads/readProbe/
  getNothingTouched/probeLastReading`) and in dispatcher services
  registered in `main.cpp` (`ProbeEngineService` name "Probing",
  `ProbeSwitchGate`, `ProbePadsGate` — names preserved for
  `forceServiceByName`).
- Hardware drivers are **shared, not duplicated**: ProbeButton (PIO+IRQ),
  switch-position sensing, probeLEDhandler, buffer power claim all stay in
  Probing.cpp; `ProbeManager` is the organized facade over them.
- Shared globals (`switchPosition`, `connectOrClearProbe`, `showProbeLEDs`,
  `probeRowMap`…) remain owned by the legacy singletons; v2 reads/writes
  through the same aliases so the engines can't diverge.

### Non-blocking probe sessions

`Pads::probeMode` is a session (MeasureMode service pattern): `beginSession`
→ one pipeline pass per `Pads::service()` call → `endSession(reason)`. The
legacy blocking loop implicitly suspended most services; v2 reproduces that
with `ProbeActiveGate` wrappers (menus, highlighting, measureMode,
slotManager, oledService, peripherals, fileCacheFlush, **configSave**) gated
on `probeActive`. SF-menu choosers are still blocking while open (copied
into Pads.cpp so their reads use v2 decode; converting them to session
states is the known upgrade path).

### PadDecode: the calibration model

`expected_i = floor + (scale − floor) * 10K / (10K + R_i·ladder_trim + Rsrc)`

Decode = nearest expected value, boundaries at midpoints, with a boundary
deadzone (see below). All in raw ADC counts of the one shared ADC.

| Parameter | Config key | Default | What it absorbs | How it self-adjusts |
|---|---|---|---|---|
| Ladder table `R_i` | `ProbeLadderTable.h` (generated) | schematic BOM | design truth | regenerate via script |
| `Rsrc` | `calibration.probe_rsrc_ohms` | 125Ω | tip feed source R | manual only |
| `ladder_trim` | `calibration.probe_ladder_trim` | 1.023 | resistor batch vs schematic nominals, relative to the 10K divider | deep decodes (idx ≥ 30, gain 2%); **deep-nano anchor** idx 78–94 in SELECT (gain 10%, unambiguous since R steps are 10–12% there) |
| `scale_trim` (select) | `calibration.probe_scale_trim` | 1.0 | full-scale / Rsrc-model residue | shallow decodes idx ≤ 20, gain 2% |
| `scale_trim_measure` | `calibration.probe_scale_trim_measure` | 1.0 | measure-frame residue (only used by the volts fallback) | shallow decodes in measure |
| floor | RAM only | ~10–15 counts | ADC offset + leakage | continuous EMA of no-touch readings; seeded by `getNothingTouched()` at boot |
| measure anchor `s_c7Unloaded` | RAM only | — | ADC7 front-end divider tolerance | sampled while untouched in MEASURE with GPIO-powered buffer: unloaded tip = rail = ADC5 full scale, so `S = 4095·(c7'/c7_unloaded')` — fully ratiometric |

Trim persistence: `persistTrimsIfQuiet()` marks `configChanged` only after
3s of no touch (≤1/min), and `configSaveService` is probeActive-gated —
flash writes were stalling probing (~100–300ms "hangs") before this.

Other decode machinery (mostly carried from legacy): burst reads + variance
gate, `probeReadingIsPhantom` tip-feed blink, `smoothProbeReading`,
2-consecutive-read row confirmation, 500ms duplicate limiter.

### Ladder table generation

`scripts/generate_probe_ladder.py` netlists
`JumperlessV5/Jumperless23V50/EverythingOnOneSchematic/EverythingOnOneSchematic.kicad_sch`
(the only file with the complete connected network — the per-board project
files are partial) and walks the resistor tree from `ADC_PROBE_SENSE`.
Self-check: 101 pads, strictly monotonic, chain ≈ linear. Output:
`src/Probing/ProbeLadderTable.h` (generated, checked in).

## Hardware findings so far (2 boards)

1. **Board 1, first test:** rows 1–27 exact, everything from ~28 on decoded
   +1 position (rails, rows 31–60, row 60 → NANO_D1), identical in both
   switch positions. Diagnosis: real ladder ~2.3% more resistive than
   schematic nominals relative to the 10K divider; because `R/ΔR` is
   constant, that's a uniform ~1-band shift. Fix: `probe_ladder_trim`
   (fitted 1.023). After fix: full sweep exact in SELECT **and** MEASURE
   (28/31/35/40/45/50/55/60, rails, D1, A0, VIN).
2. **Board 2:** SELECT fine, MEASURE shifted; also "pads sometimes don't
   register for a while". Diagnosis: per-board ADC7 front-end tolerance
   through the volts-frame conversion; near-boundary readings then hit the
   deadzone. Fix: the unloaded-tip measure anchor (above). Board 2's
   status after: ladder trim self-adjusted to 1.0220, anchor live at ~722.
3. **Early pads reject light touches**: tip contact resistance (10–50Ω,
   pressure-dependent) moves row 1 by ~0.4 counts/Ω against a ~100Ω ladder
   pitch — a third of a band — while row 60 barely moves. Deadzone now
   tapers: idx ≤ 34 accepts to 46.5% of band, deeper 42%.
4. **Nano header LEDs stuck in measure**: `MeasureMode::stopMeasurement`
   only unpainted netless nodes on rows 1–60; nano pixels are 400–429,
   **above `LED_COUNT` (300 = breadboard section only)** — a
   `< LED_COUNT` bounds check silently skipped them. Fixed. Beware this
   trap anywhere header/logo pixels (400–445) are touched.
5. **MicroPython blocks the service loop**: python probe-read loops stall
   `serviceAll`, so `switchPosition` goes stale if the switch is flipped
   mid-script → phantom gate rejects everything (looks like dead probing).
   Test protocol: flip the switch, wait ~2s, *then* start capture scripts.

## Verifying decode on hardware

Tap-capture (device decodes while the user taps a known sequence), via the
jumperless-v5 skill:

```python
# /tmp/probe_capture.py — exec via: python3 scripts/jumperless.py exec --file ... --timeout 100
import time
last = None; lastt = 0
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 88000:
    p = probe_read_nonblocking()
    try: val = int(p) if p is not None else -1
    except (TypeError, ValueError): val = -1
    if val > 0 and (val != last or time.ticks_diff(time.ticks_ms(), lastt) > 800):
        print("decoded:", p, "(", val, ")"); last = val; lastt = time.ticks_ms()
    time.sleep_ms(15)
```

Decoder state: the "Probe Calib" clickwheel app under v2 prints
`PadDecode::printStatus()` (floor, Rsrc, ladder trim, select scale, measure
tip + anchor) and exits — the interactive calibration app only exists on
the legacy engine now. First-start also skips probe calibration under v2.
`printStatus(true)` dumps the full expected-counts table (only wired into
SelfTest currently). Note the "measure tip" line is only meaningful while
the switch is actually in measure.

A/B against legacy: `[debug] probe_engine_v2 = 0` in config.

## Known remaining issues / not yet verified

- **Calibration is still not fully hands-off across boards.** The anchors
  recover per-board error *if* the user happens to touch deep nano pads
  (ladder trim) or idle in measure (measure anchor). A fresh board that's
  off by >~half band mid-chain decodes wrong-but-confident there, and only
  the anchors can pull it back.
- **No raw-count telemetry during debugging.** Every hardware diagnosis so
  far was inferred from decoded-vs-tapped patterns; we never had raw ADC5
  counts live. This made root-causing slow.
- **DAC-powered measure fallback** (`probe_power_gpio = false`) still uses
  the volts-frame conversion + `scale_trim_measure` — no anchor.
- **Rail pad identity** (which of the +/− pair is RAIL vs RAIL_GND) is
  assumed from probeRowMap order; one capture had the first rail tap
  unregistered, so the pair order is plausible but unconfirmed.
- **Logo/SF/building pads** decode untested on hardware; they're excluded
  from trim learning (their star resistors are independent of the chain).
  BUILDING_PAD_BOTTOM (~40 counts) is decodable for the first time (legacy
  `minimum_probe_reading = 85` made it unreachable).
- **Early-pad light touches** may still be marginal (contact resistance is
  physics, not a bug); the taper helps but wasn't stress-tested.
- **Switch flip mid-session** leaves the decode frame stale until session
  exit (legacy parity).
- SF choosers still block the loop while open (legacy parity).

## Ideas to try next (roughly in order)

1. **Raw telemetry first.** Add a debug view before more tuning: e.g.
   `debug.probing` printing `raw / expected / index / residual / scale`
   per accepted burst, or expose raw ADC5 + `PadDecodeResult` to
   MicroPython. Every remaining issue becomes measurable instead of
   inferred.
2. **Residual ring buffer.** Keep the last N `(index, residual/band)`
   pairs in PadDecode and dump them in printStatus — one glance shows
   whether errors are proportional (ladder), uniform (scale), offset
   (floor/Rsrc) or contact noise.
3. **Boundary bias for contact resistance.** Contact R always shifts
   readings *down* (toward higher index). Instead of widening the early-pad
   deadzone further, bias the decision boundary downward on early pads by
   an expected-contact term (e.g. 15Ω equivalent), keeping the deadzone
   symmetric around the *biased* boundary.
4. **Two-tap guided anchor.** On first start (or in the status app): ask
   for one tap on row 1 and one on a deep nano pad; solve scale + ladder
   trim exactly from the two raw readings. Kills the "fresh board off by a
   band" case in five seconds, still zero-maintenance afterwards.
5. **Rsrc auto-fit.** Row 1–5 residuals are dominated by Rsrc (100Ω ladder
   vs 125Ω source); the scale trim currently absorbs the error unevenly.
   Fit Rsrc from shallow residuals instead of folding them into scale.
6. **Joint estimator.** Replace the three independent EMAs with one small
   least-squares update over (scale, ladder_trim) from all confident
   decodes — the current split (shallow→scale, deep→ladder) is a crude
   diagonal approximation of this.
7. **Retry-instead-of-reject in the deadzone**: on a deadzone hit, take one
   extra burst immediately and accept if both agree — recovers slow
   contact-settle touches without reintroducing flicker.
8. **Anchor coverage for the DAC-powered buffer**: sample the unloaded tip
   through ADC7 the same way; the anchor identity (unloaded = S in ADC5
   frame) doesn't actually require knowing the voltage — it requires the
   *unloaded ADC5-frame level*, which for the DAC case is
   `Vdac/Vrail·4095`; could be anchored against a simultaneous rail-ish
   reference instead.
9. **Verify rails + logo pads** with a capture session; confirm +/− pad
   order and building-pad decode.

## File map

- `src/Probing/Pads.{h,cpp}` — v2 engine: session pipeline, readers,
  encoder nav + SF choosers (copied from legacy, class renamed), dispatch
  functions, jOS dispatcher services.
- `src/Probing/PadDecode.{h,cpp}` — decode model, floor tracker, scale +
  anchors, trims, deadzone, printStatus.
- `src/Probing/ProbeLadderTable.h` — generated; do not hand-edit.
- `src/Probing/ProbeManager.{h,cpp}` — hardware facade (delegates to the
  shared drivers in legacy Probing.cpp).
- `scripts/generate_probe_ladder.py` — table generator + self-check.
- `src/Probing.cpp` / `src/Probing.h` — legacy engine (untouched) + shared
  drivers + dispatch wrappers (`Probing.h` only).
- Gates/registration: `src/main.cpp` (~line 450); `ProbeActiveGate` in
  `Probing.h`.
- v2 app/selftest touchpoints: `Apps.cpp` (`probeCalibApp` v2 branch,
  first-start skip in `calibrateDacs` tail), `SelfTest.cpp` (status print),
  `MeasureMode.cpp` (nano pixel unpaint).
- Config: `config.h` calibration + debug sections; parsing in
  `configManager.cpp` (6 touch points per key + clamp + requiredKeys).

Build: `pio run -e jumperless_v5` (also keep `-e jumperless_og` compiling).
Flash: `pio run -e jumperless_v5 -t upload`.
