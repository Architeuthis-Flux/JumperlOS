# PoC spec: wireless capacitive position sensing on the Jumperless V5 probe-pad ladder

## 1. Goal

Demonstrate that a battery-powered, electrically floating "probe" (a needle driven with an AC carrier) can be located along the Jumperless V5's existing probe-sensing pad strip to roughly 1 mm accuracy, with no cable between probe and board, using the 92-resistor ladder that already sits under the pads as the position transducer.

This is a physics/firmware proof of concept. Bodge wires and a helper Raspberry Pi Pico 2 are acceptable. Do not aim for integration into the main Jumperless firmware yet; aim for a standalone measurement harness that produces believable numbers and plots.

## 2. Principle

Today the ladder is used in contact mode: the ladder is driven, and the wired probe needle reads the DC voltage of whichever pad it touches. This PoC reverses the roles:

- The probe drives its tip with a 3.3 V square wave at carrier frequency `f0` (~50 kHz). The probe is floating; its AC return path is capacitive, through the user's hand and body to earth and back to the board through its USB/supply. No ground wire.
- The tip couples capacitively (or galvanically, on contact) into the pad strip at position `x`. The injected carrier current splits toward the two ends of the resistive ladder in inverse proportion to the resistance each way (a current divider).
- The board measures the carrier amplitude arriving at each end, `A_L` and `A_R`, with a software lock-in (Goertzel). Position is the ratio:

```
x_norm = A_R / (A_L + A_R)        # 0.0 at left end, 1.0 at right end
x_mm   = x_norm * L_strip_mm
```

Everything unknown or unstable — coupling capacitance, hover height, grip, skin, battery voltage — scales `A_L` and `A_R` identically and cancels in the ratio. Hand-only proximity (no carrier) produces nothing coherent at `f0` and is rejected by the lock-in.

Two receive schemes are specified in §5. Scheme A (alternate-ground voltage mode) needs zero analog components. Scheme B (shunt current sense) is the fallback if Scheme A's ADC loading proves ugly.

## 3. Recon phase (do this first)

Fill in these unknowns from the JumperlessV5 firmware repo and schematic before wiring anything. Record answers in `NOTES.md` in the PoC repo.

1. Ladder topology: confirmed 92 series resistors with a sensing pad at each tap. Record the per-step resistance `R_step`, total `R_total`, and the physical pitch of the pads (expected 2.54 mm) and total strip length `L_strip_mm`.
2. End nets: one end is confirmed hard-grounded, and cutting that trace to bodge it out is pre-approved (see §4.4) — but do it only after Scheme C bring-up (§5) validates the signal chain. Identify what drives the other end (DAC output? fixed rail? mux?) and how to make the V5 firmware release it (tristate/disable, build flag, or a startup mode where the ladder is undriven).
3. Probe-tip net: how the needle's ADC path works today (which ADC, any series R, any mux). Not needed for the PoC itself but useful for Phase 1 characterization and for a later integrated version.
4. Estimate per-pad parasitic capacitance to ground (from layout or by measurement in Phase 1). With `C_pad ≈ 2–5 pF` per tap, the ladder is a distributed RC line; compute its corner frequency and confirm `f0` sits comfortably below it. If `R_total` is large (>100 kΩ), lower `f0` accordingly.
5. Confirm the LED subsystem and user circuits can be left running during tests (they are noise sources we explicitly want present in the final experiments).

## 4. Hardware setup

### 4.1 Transmitter (the floating probe)

- Raspberry Pi Pico 2 (RP2350) powered from a LiPo or USB power bank. It must not share USB or ground with the Jumperless during wireless tests.
- One GPIO configured as PWM, 50% duty, frequency `f0`, driving the tip through a series 100 pF capacitor and a series 1 kΩ resistor (protection).
- Tip electrode: start with a sewing needle or stiff wire with ~5×5 mm of copper tape wrapped near the point (bigger electrode = more signal, slightly blurrier injection; fine for PoC). A bare needle is the stretch configuration.
- Shield: wrap the "barrel" (the part the hand holds) in copper tape or foil connected to the Pico's GND. The hand must touch this shield. This anchors the probe ground to the hand and keeps the hand coherently quiet; only the tip radiates.
- Firmware: MicroPython is fine. Deliver `tx.py`: sets up the PWM, prints the actual achieved frequency, and provides a REPL helper to change `f0` and to toggle a slow on/off gating (e.g. 1 s on / 1 s off) used in the rejection experiments.

### 4.2 Receiver (helper Pico on the board side)

- Second Pico 2, USB-connected to the host computer (shares earth with the Jumperless via the host — this is the intended return path).
- Bodge one wire to each end of the ladder (call them END_L and END_R). Each bodge goes through a series 1 kΩ protection resistor to the helper Pico.
- Each end connects to two helper-Pico pins:
  - an ADC-capable pin (always connected, high-Z), and
  - a plain GPIO used as a switchable ground: output-low = end grounded (~30 Ω), input = end floating.
- Common ground wire between helper Pico GND and Jumperless GND.
- The Jumperless runs its normal firmware but with the ladder driver released (per §3.2). LEDs and any user circuit may run.

### 4.3 Ground-truth ruler

The pads themselves are the ruler: touching pad `k` in contact mode is a known position `k × pitch`. All accuracy numbers are reported against this. Collect the contact-mode reference *before* any surgery (§4.4), since lifting the ground end breaks the stock DC divider.

### 4.4 Board surgery (pre-approved, do last)

Cutting the ladder's hard-grounded end and bodging it to the helper Pico is approved by the owner. Rules:

- Do it only after Scheme C (§5) has validated TX, ADC bursts, Goertzel, and slot sequencing with zero cuts.
- Cut between the last ladder resistor and the ground via/pour; bodge from the ladder side through the standard 1 kΩ protection resistor to an ADC pin + grounding GPIO, exactly as END_L in §4.2.
- Bodge hygiene matters: when floated, this node's source impedance is up to `R_total` and the wire is an antenna. Keep the bodge as short as possible, run it alongside a ground wire (twisted if practical), and route it away from the LED data lines.
- Record which physical end (pad 1 or pad 92) was cut and verify sign conventions empirically.
- Expected side effect: stock contact-mode probe sensing in the V5 firmware is broken while the end is lifted. Restoration is one solder blob. (Design note for V6: make this a 0 Ω jumper or an analog switch so DC contact mode and wireless mode can coexist.)

## 5. Measurement schemes

### Scheme A — alternate-ground voltage mode (primary, zero analog parts)

Slot 1: ground END_R (GPIO low), float END_L, sample END_L's ADC. All injected current flows toward R, so the injection node's voltage is `I × R_toR(x)`, and the floating L segment carries no current, so `V_L = I × R_toR(x)`.
Slot 2: swap ends. `V_R = I × R_toL(x)`.

Compute Goertzel amplitude at `f0` in each slot: `A_L`, `A_R`. Then `x_norm = A_L / (A_L + A_R)` (note: `A_L` measured in slot 1 is proportional to distance from the R end; get the sign convention right and verify empirically with pads 1 and 92).

Caveat to verify: the RP2350 ADC input is not ideally high-Z (sampling cap + mux). If loading visibly distorts the division, either add one cheap op-amp buffer per end (MCP6002 on a scrap of perfboard) or fall back to Scheme B.

### Scheme B — shunt current sense (fallback)

Ground both ends permanently through known shunts (10 kΩ bodged resistors to GND) and measure the AC voltage across each shunt with the two ADC pins simultaneously (mux alternation is fine at these frequencies). `A_L/A_R` is now a true current ratio. Signals are smaller (~1–20 mV with the copper-tape tip); rely on the lock-in gain and longer windows.

### Scheme C — single-end, no-cut bring-up variant

Works before any surgery, using only the accessible (non-grounded) end, END_L, with the far end still hard-grounded. Alternate END_L between two states:

- Slot V: END_L floating into the ADC. All injected current exits through the grounded far end, so `V_float = I × R_toGND(x)` — proportional to distance from the grounded end.
- Slot I: END_L grounded through a bodged 10 kΩ shunt to GND, ADC measuring across the shunt. Now the ladder is a current divider and `V_shunt = I × (x_fraction_toward_L) × 10 kΩ`.

The ratio `V_shunt / V_float` cancels `I` (coupling, height, grip) just like Scheme A, and maps monotonically to position given `R_total`. Conditioning is poor near the grounded end (both numerator behaviors get small), so treat Scheme C as bring-up and qualitative validation: prove the TX, the lock-in, slot sequencing, hand rejection, and rough position tracking. Then perform §4.4 and switch to Scheme A for the accuracy experiments. Note the 10 kΩ shunt slightly perturbs the divider; keep it or remove it consistently and calibrate.

### DSP spec (all schemes)

- Sample bursts of `N = 512` ADC samples per slot at the max stable ADC rate (target ≥ 250 kS/s; 500 kS/s if achievable). Implementation may be MicroPython with `@micropython.viper`/`native` (Femtonyl demonstrated ~250 kS/s pipelines) or C/pico-sdk if Python can't keep up. Agent's choice; correctness first.
- Per burst compute the Goertzel magnitude at `f0`. Also compute magnitude at a guard frequency (e.g. `f0 × 1.37`) as a noise reference; report SNR.
- Slot pair every ~4–10 ms → 100–250 raw position fixes/s. Apply a short EMA (alpha ≈ 0.3) for display.
- Contact detection: total amplitude `A_L + A_R` above a threshold with hysteresis; report `no_target` below it.
- Calibration: a table mapping measured `x_norm` at each of the 92 pads (collected in a guided sweep) to true position; apply by linear interpolation. Report accuracy both raw and calibrated.

### Host tooling

`viz.py` on the host: reads the serial stream, plots live `A_L`, `A_R`, SNR, `x_mm`, and logs CSV. Serial line protocol from the helper Pico, one line per fix:

```
t_ms, A_L, A_R, snr_db, x_norm, x_mm, contact_flag
```

## 6. Experiment plan and success criteria

Run in order; each produces a CSV + short note in `RESULTS.md`.

1. Bench sanity (wired, Scheme C, pre-surgery): clip the TX output directly to a mid-strip pad through 100 pF. Verify a strong Goertzel line at `f0`, SNR > 40 dB, and a monotonic position estimate across a sweep of pads. Pass: correct ordering, no dead zones. Then perform §4.4 and repeat under Scheme A, expecting `x_norm ≈ 0.5` at mid-strip.
2. Static accuracy (contact, floating TX, hand-held): touch every 5th pad, 2 s each. Pass: after calibration, std dev ≤ 0.5 mm per point and absolute error ≤ 1 mm at every tested pad; before calibration, monotonic and within ±1 pad.
3. Hover: hold the tip 1, 2, 5 mm above a known pad (use a plastic shim). Record amplitude vs height and position error vs height. Pass: usable fix (≤ 2 mm error) at 2 mm height.
4. Ratio invariance: at one pad, vary grip (fingertips vs full fist), swap operator if possible, run TX battery from full to sagging. Pass: position shifts < 1 mm across all conditions while amplitude varies freely.
5. Rejection: (a) hand waved over the strip with TX gated off — no contact_flag, no track; (b) LEDs running a bright animation during test 2 — accuracy unchanged; (c) a jumper-wire circuit toggling on nearby breadboard rows — accuracy unchanged. Pass/fail with numbers.
6. Dynamics: drag the tip along the strip at ~50 mm/s; plot x vs t. Pass: smooth, monotonic, latency subjectively < 50 ms.
7. (Scheme A only) ADC-loading check: repeat test 2 with a 1 MΩ resistor temporarily in series with the ADC pin; if calibration shifts materially, document it and evaluate Scheme B or buffers.

## 7. Known risks and fallbacks

- Driven end not releasable in firmware: worst case, desolder its end resistor and bodge from the lifted pad, same rules as §4.4. Document any board surgery in `NOTES.md`.
- RC dispersion: if `f0` is near the ladder's RC corner, amplitude ratio becomes position-dependent in a nonlinear way. First fallback: lower `f0` to 10–20 kHz. Note: dispersion is calibratable and phase is a bonus observable, but keep the PoC simple.
- Mains hum / LED harmonics at `f0`: `f0` is a parameter; make it trivially changeable and pick a quiet bin using the guard-frequency scan.
- Signals too small in Scheme B: enlarge the tip electrode, raise `f0` (injected current scales with f), or add the MCP6002 buffers/TIAs.
- Re-radiation: a wire or component lead touched by the tip re-injects the carrier elsewhere on the strip. Out of scope for pass/fail, but capture one example trace for the record.

## 8. Default parameters

| Parameter | Default | Notes |
|---|---|---|
| f0 | 50 kHz | must be << ladder RC corner; adjustable at runtime |
| TX drive | 3.3 V square, 50% | through 100 pF + 1 kΩ |
| Tip electrode | 5×5 mm copper tape | bare needle = stretch |
| ADC burst | 512 samples | per slot |
| ADC rate | ≥ 250 kS/s | 500 kS/s stretch |
| Slot pair period | 4–10 ms | → 100–250 fixes/s |
| EMA alpha | 0.3 | display only; log raw |
| Contact threshold | set empirically | with hysteresis, log both thresholds |
| Shunts (Scheme B) | 10 kΩ | bodged |
| Protection R | 1 kΩ | every bodge and the TX output |

## 9. Deliverables

1. `tx.py` — transmitter firmware (MicroPython, Pico 2).
2. `rx/` — receiver firmware (MicroPython or C, helper Pico 2): slot sequencing, ADC bursts, Goertzel, serial protocol.
3. `viz.py` — host live plot + CSV logger.
4. `calibrate.py` — guided pad-sweep calibration, writes `cal.json`.
5. `NOTES.md` — recon answers (§3), wiring photos/description, any board surgery.
6. `RESULTS.md` — experiment results vs the criteria in §6, with plots.

## 10. Stretch goals (only after §6 passes)

- Second transmitter at `f1` (e.g. 71 kHz); track two probes simultaneously with two Goertzel bins.
- Both flanks: repeat on the second pad strip (if V5 has one on the far side) and estimate lateral position from the left/right total-amplitude ratio.
- Replace the helper Pico with the V5's own RP2350: route/bodge the end nets to a spare ADC input and port the RX code into a Jumperless firmware app.
- PIO-only receive (Femtonyl-style discharge timing with the wireless probe acting as the kick electrode) to prove a zero-analog-parts variant.
