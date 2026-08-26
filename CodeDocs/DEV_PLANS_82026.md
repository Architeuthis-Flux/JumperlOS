# DEV_PLANS_82026 — OLED @ 1 MHz, crosspoint-R calibration, projects folder

Plans drafted 2026-08-20, continuing from `DEV_PROGRESS_1.md`. Nothing here
is implemented unless marked; the current-sense batch in DEV_PROGRESS_1 is
still awaiting Kevin's hands before commit.

---

## Plan 1 — Kevin's question: do we even need the OLED I2C speed switch?

**Short answer: the switch exists only for the panel's 400 kHz spec rating.
If the panel proves out at 1 MHz, everything gets simpler — but the "it'll
work at 1 MHz" hunch has NO on-board evidence yet, and nothing on the bench
ACKs today, so this is a plan, not an edit.**

Facts established in code:

- **This firmware has never driven a panel at 1 MHz.** Every frame ever
  shipped went at clkDuring = 400 kHz — the static placeholder uses the
  Adafruit default (400 kHz), and the dynamic instance passes
  `kOledI2CClockHz` = 400 kHz explicitly (oled.cpp:149–152). The thing that
  failed at 1 MHz was the bit-bang probe's integer delay math, not hardware
  I2C. "Frames displayed fine" ≠ "panel runs at 1 MHz".
- **The ping speed must equal clkDuring, whatever they are.** A 1 MHz ping
  against a panel that only ACKs at 400 kHz makes the detector lie in the
  opposite direction from the bug just fixed: it declares absent a panel
  that frames would drive fine. So "drop the switch" only works as "run
  ping AND frames at 1 MHz together". clkAfter stays `I2C0_BUS_CLOCK_HZ`
  (1 MHz) no matter what — that's the DAC/INA bus's clock, owned by initDAC.
- **The dance costs almost nothing per se** (two `i2c_set_baudrate` register
  reprograms, ~µs each). The real payoffs of 1 MHz are elsewhere:
  1. `display()` blocks its core for the whole frame transfer: 512 payload
     bytes + control/addr overhead ≈ **~12 ms at 400 kHz vs ~5 ms at
     1 MHz** per 128×32 frame — UI latency and, on I2C0, bus occupancy the
     INA219s/DAC wait out.
  2. **A hazard class disappears** (finding, this session): `setClock` →
     `i2c_set_baudrate` **disables and re-enables the I2C block** and is
     NOT wrapped by the I2C0 arbiter (the arbiter wraps only
     `i2c_write/read_blocking_until`). Every `oledI2cPing` on the shared
     bus brackets its arbiter-covered write with two unwrapped baudrate
     changes — and not every ping site is wavegen-gated: `show()` and
     `oledPeriodic` gate on `wavegen.isRunning()`, but **Menus.cpp:4841,
     Apps.cpp:1245, and `init()`'s post-connect check (oled.cpp:589) do
     not**. A ping from one of those during a WaveGen DMA stream yanks the
     block out from under the stream. At equal speeds the ping helper can
     skip `setClock` entirely and those sites become hazard-free without
     hunting down every caller.

Risk: Jumperless users attach arbitrary SSD1306 modules (clones vary;
official spec is 400 kHz, but 1 MHz overclock is commonly fine — and this
bus's edges are already proven at 1 MHz by the INA219s). A fixed 1 MHz
would strand slow panels, so adopt via negotiation, not a constant flip.

### Stage 1 — bench experiment (blocked on a panel that ACKs)

One-line trial: set `kOledI2CClockHz` (oled.cpp:72) to `1000000`, flash,
then with the panel attached: boot autodetect finds it, `oled_connect()`
inits, frames render clean, 10-minute soak with the post-frame write-verify
staying at 0 consecutive failures, and `i!`/INA reads on the shared bus stay
healthy throughout. If the panel misbehaves, we keep the dance and this plan
ends here (record the panel model).

### Stage 2 — speed negotiation at detect time (only if Stage 1 passes)

**Files: `src/oled.cpp` only.**

- `static uint32_t s_oledClockHz = 1000000;` — the negotiated panel speed.
- Negotiate wherever a not-connected→connected transition happens (boot
  autodetect `probeOledOnInternalI2C0`, `oled_connect`): ping at 1 MHz →
  ACK ⇒ negotiated 1 MHz; NACK ⇒ ping at 400 kHz → ACK ⇒ negotiated
  400 kHz (today's dance, for slow panels); both NACK ⇒ absent. Hot-swap
  re-negotiates because detection transitions do.
- `oledI2cPing(wire, addr)` pings at `s_oledClockHz` and **skips both
  `setClock` calls when the target speed equals the bus's standing rate**
  (1 MHz on I2C0) — steady-state pings on a fast panel touch the baudrate
  registers zero times, which retires the un-arbitred-setClock finding for
  the ungated ping sites.
- `initDisplayForConnectionType()` passes
  `clkDuring = s_oledClockHz`, `clkAfter = (I2C0) ? I2C0_BUS_CLOCK_HZ :
  s_oledClockHz` — at 1 MHz on I2C0 the driver's per-frame SETWIRECLOCK /
  RESWIRECLOCK become same-value reprograms (harmless, and the frame path
  is already wavegen-gated in `show()`). Recreate the instance when the
  negotiated speed changes (same recreate path the Wire switch uses).
- No config key unless a real panel needs a manual override (YAGNI; the
  negotiation IS the override).

Verify: Stage 1's soak repeated through the negotiation path; then a
deliberate 400k-only simulation (temporarily make the 1 MHz ping return
NACK) to confirm the fallback lands in today's exact behavior.

---

## Plan 2 — measure the actual crossbar resistance, then calibrate the scan

The scan's whole current model is `I = ΔV / (xp × crosspoint_resistance)`
(NetVoltageScan.cpp:560–650) and `crosspoint_resistance = 40.0` is a
hand-measured guess ("a 2-crosspoint path measures ~80 Ω, so ~40 each",
config.h:287). The INA agreement now sits at +3 % (scan 4.4 vs INA0
4.27 mA) — and since both numbers describe the same ΔV over the same path,
**R_true = R_assumed × (I_scan / I_INA)**: the bench already implies
~41.2 Ω on that loop. We have everything needed to measure this properly:
INA0 is routable in series (`ISENSE_PLUS`/`ISENSE_MINUS`, nodes 108/109),
the HIL client already builds DAC0→ISENSE→row→GND loops
(test_net_currents.py:19–34), Stage 7 pair taps give clean per-path ΔV,
and the config plumbing for `calibration.crosspoint_resistance` exists end
to end.

Caveat to carry through: INA0 is "truth" only to its shunt tolerance +
calibration register (~1–2 %). Fine for calibrating a 40 Ω figure; not a
metrology standard.

### Stage A — measurement survey, ZERO firmware changes — **done this session**

**File: `test/hil/measure_crosspoint_r.py` (created and run 2026-08-20,
~13 min for the full default sweep, bench state restored after)** — a
tool, not a test:
no `check()` gates, prints a table. Uses the same `jl`/`port1_command`
harness and the bench snapshot/restore the suite already has.

Per row R in the #32 bisect range (34–79), at DAC0 = 0.8 V (the suite's
known-good drive):

```python
jl_exec(f"""
nodes_clear(); time.sleep(0.1)
dac_set(DAC0, 0.8)
connect(DAC0, ISENSE_PLUS)
connect(ISENSE_MINUS, {row})
connect(GND, {row})
""")
# settle ≥ 1.5 s (scan EMA α=0.25 @ 20 Hz converges ~200 ms; pair taps
# rotate one path per 5 ms slot; give the whole loop a few rotations)
# I_INA = median of 6 ina_get_current(0) reads (the suite's idiom)
# scan  = 'i!' per-path lines: parse node pair, xp count, and the raw
#         '(ema …)' value — NOT the shown mA (deadband-gated) and NOT the
#         mA regex (the ema prints unsuffixed on purpose)
# each segment of the loop (DAC0→ISENSE+, ISENSE−→row, row→GND) carries
# the same I_INA:  R_per_xp(segment) = 40.0 × ema_mA / ina_mA
```

Output: one line per row × segment — row, node pair, xp, I_INA, ema,
implied R/xp, ` pair` marker present or not — then mean/σ/min/max overall
and grouped breadboard vs Nano header. Mid-range current keeps the
deadband and INA resolution both irrelevant.

**RAN 2026-08-20 (this bench board, the DEV_PROGRESS_1 firmware), 108
segment measurements across rows 5 + 34–60 and NANO_D0–D9:**

```
all segments                 n=108  mean 41.78  sd 2.59  min 35.13  max 45.88 ohm
DAC0->ISENSE+ (control)      n= 37  mean 42.05  sd 2.30   (same node pair every pass)
ISENSE- -> row               n= 33  mean 40.04  sd 2.24
row -> GND                   n= 38  mean 43.02  sd 2.36
breadboard rows (<=60)       n= 78  mean 42.67  sd 1.84
nano header (70-79)          n= 30  mean 39.46  sd 2.83   (down to ~36 on odd pins)
pair-tapped only             n= 56  mean 41.39  sd 2.65
single-ended only            n= 52  mean 42.19  sd 2.48
```

Read of the data:
- **The 40 Ω constant is ~4.5 % low on this board** — measured mean
  41.8 Ω — which is the scan's remaining +3 % INA residual, sign and
  size. Setting ~42 Ω centers it.
- **The linear-in-xp model holds**: the 4xp control segment implies the
  same R/xp as the 2xp row segments (42.0 vs 40.0/43.0).
- **Real structure, not noise**: breadboard-row loops sit tight
  (σ 1.8 Ω); Nano-header loops run ~3 Ω lower with more spread. On the
  low-current passes (~3 mA, the odd Nano pins) ALL three segments
  including the fixed-node-pair control implied ~5–10 % lower R. Likely
  route variance — the router re-picks lanes after every `nodes_clear` —
  but UNVERIFIED (no route dump was captured), and the dips correlating
  with the lower loop current is also exactly what a Ron-vs-V/I
  dependence would produce. **Stage C discriminates between those two
  and is therefore motivated by this data, not optional garnish.**
- Sweeping node numbers 61–69 correctly SKIPs at INA ≈ 0.49 mA (its
  zero-ish reading with no loop routed): that range is an unmapped hole
  between breadboard row 60 and NANO_D0 = 70. The #32 "rows 34–79"
  bisect range is really rows 34–60 + Nano D0–D9.

**Decision for Stage B**: one global calibrated value (~42 Ω, measured per
board) is worth ~3–5 % immediately; the residual ±6 % tracks either the
chosen route or the loop current (Stage C settles which), so the finer
step, if wanted, is per-chip Ron or an R(V) term — only worth it if Kevin
wants better than ~±5 %.

### Stage B — calibrate `crosspoint_resistance` per board (firmware)

**Files: `src/selfreflection/SelfTest.cpp` (new phase), nothing else** —
the config key already parses/saves/diffs everywhere.

Model directly on the INA-referenced `probe_droop_ohms` phase
(SelfTest.cpp:973–1009): route the DAC0→ISENSE→row→GND loop internally on
2–3 rows, read INA0 + the scan's ema (or the node voltages directly),
compute R/xp, write the median to
`jumperlessConfig.calibration.crosspoint_resistance`, save. Non-fatal like
its sibling: a failed phase leaves the existing value untouched. Only add a
per-chip table if Stage A's data demands it — that's new config plumbing
and the accuracy win must be shown first.

### Stage C — Ron vs signal voltage (MOTIVATED by Stage A's data)

Stage A's low-R outliers correlate with the lower-current loops — which is
either route variance or exactly this effect; this stage discriminates.
Analog-switch Ron varies with signal voltage. Repeat Stage A at DAC0 ∈
{0.5, 1.5, 2.5, 3.3, 4.5 V} on ~6 rows. If R moves > ~10 % across the
span, consider a two-point linear R(V) in `computePathCurrents` (it already
has the per-path mean voltage in hand as `0.5×(v1+v2)`); if not, close the
question and keep the scalar.

### Stage D — feed it back into verification (this is task #32's tool)

Tighten test_net_currents' INA-agreement window from 50 % to what the
calibrated data supports (target 10–15 %). Stage A run across rows 34–79
is exactly the bisect #32 called for — it converts "leave #32 open until
the number holds across setups" into a measured close condition.

---

## Plan 3 — preloaded projects folder (outline + open questions for Kevin)

TODO #3 (TODO81926.md): a `projects/` tree of premade, documented,
loadable projects — 555 LED flasher, Nano-in-header driving a display
(I2C/SPI/parallel variants + Jumperless-drives-it-directly variants, both
writable over USB), 7400/4000 logic demos, EEPROM dumper, effect-pedal
audio circuits, transistor tester / curve tracer.

This is product/content-shaped; decisions needed before a build plan is
worth writing:

1. **What IS a project on disk?** Proposal: `/projects/<name>/` on
   littlefs with `manifest.txt` (name, blurb, BOM/placement note, node
   file, optional app hook), `nodes.txt` in the existing slot format, and
   optional `main.py` (MicroPython) for projects that need behavior —
   which reuses the existing slot loader and eKilo/REPL instead of a new
   format.
2. **Where do they live?** Shipped in the firmware image and unpacked to
   littlefs on first boot (survives `nodes_clear`, user-editable,
   restorable), vs. read-only from flash. Unpack-on-first-boot is the
   Proposal.
3. **UI entry point** — menu section ("Projects") listing manifests;
   probe/OLED flow for the display-type choice on the display projects.
4. **"Write stuff to the screen over USB"** — is this the existing
   passthrough/JSerial path pointed at the project's display, or a new CDC
   command? Needs a decision before the display projects are more than
   nodes files.
5. **Scope of v1** — Proposal: 3 projects (555 flasher, Nano+I2C-OLED,
   EEPROM dumper) to prove the manifest/loader/menu plumbing, then content
   grows without code changes.

No implementation plan until these are settled — the plumbing is a day,
the content is the product, and the content choices drive the manifest.
