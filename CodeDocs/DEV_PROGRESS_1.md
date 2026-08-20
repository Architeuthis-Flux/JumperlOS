# DEV_PROGRESS_1 — rail-adjust shortcut + current-sense hardening (2026-08-20)

Everything in this doc is **uncommitted working-tree state**, flashed to the
bench board and HIL-verified, awaiting Kevin's hands (the standing
commit-only-when-verified rule). It implements both items from
`CodeDocs/TODO81926.md`: task #31 (the rail/DAC click-to-adjust shortcut) and
the current-sense "sparkling" hardening. On the way it also moved task #32's
symptom (see the numbers below).

---

## Part 1 — rail/DAC click-to-adjust (task #31)

The click path existed but was fully dormant (rails and DACs commented out of
`shouldPersistHighlight()` / `wantsToHandleButtonPress()`); everything
downstream — `handleEncoderButtonPress()` → `VoltageAdjuster::adjust()` →
rail/DAC setters + slot persistence — was already complete and is reused
unchanged.

**Config**: `[dacs] rail_click_adjust` (moved from `[display]` on Kevin's
call, round 2) — `0` = off, `1` = only while an OLED is connected
(**default** — the OLED's `adjust?` prompt is what makes the click
discoverable), `2` = always (LED-matrix adjuster UI only). Parsed with
`parseInt` (tri-state; `parseBool` would destroy `2`). Note: a config.txt
carrying the old `[display]` line is silently ignored — the bench board's
`2` was re-applied under `[dacs]` by hand.

**One predicate** — `clickAdjustEnabled()` in Highlighting.cpp
(`flag == 2 || (flag == 1 && oledConnected)`) — gates all three touch points:

1. `shouldPersistHighlight()`: rails (`TOP_RAIL`/`BOTTOM_RAIL` node) and DAC
   nets (4/5) persist again, which also selects the 15 s persistent
   highlight timeout — the window the prompt needs.
2. `wantsToHandleButtonPress()`: Menus defers the click to Highlighting.
3. The `adjust?` prompt: `ReadingDisplay::show()` grew an optional `hint`
   parameter — rendered as a right-aligned 5 pt tag on the bottom value row
   (no vertical room for a fourth row on the 32 px panel), folded into the
   dedupe key (gating changes repaint), and riding the pinned serial line.
   Passed by the four one-shot rail/DAC branches of `highlightNets()` and
   both live-updater branches.

Gated off (flag 1, no OLED ACK), boards behave exactly as before: 1.8 s
highlight, click opens the menu — verified by test_encoder_ui's click-open
check passing on the bench board (whose OLED does not ACK — see the finding
at the bottom).

**Round 2 (Kevin's first hands-on, 2026-08-20):**
1. *Hint overlapped the current reading* — with a hint present, the bottom
   value row is now flush-left (the hint stays right-anchored in 5 pt);
   centered, "10.0 mA" ran into the tag. No hint → centered as before.
2. *Click registered only ~half the time on rails* — the rail conditions
   were keyed on `brightenedNode == TOP_RAIL/BOTTOM_RAIL`, but
   `brightenedNode` is whatever node the probe last touched and clears to
   −1 on lift; the DACs never had the bug because their condition was
   net-based. All three touch points now key rails on the NET
   (`highlightedNet` 2 = Top Rail, 3 = Bottom Rail — MatrixState's fixed
   specials; GND is net 1 and never matches), matching the DAC idiom. Any
   node of the rail net now accepts the click.
3. *Config key moved to `[dacs]`* (above).
If clicks still drop occasionally after this build, the next suspect is a
live probe session consuming the wheel press (probeMode's own click
handling racing Highlighting's) — timing-dependent, needs a bench repro.

---

## Part 2 — current-sense hardening (stages 0–7, all landed)

Diagnosis (verified in code before touching anything): the ants are fed by
`NetVoltageScan` (5 ms round-robin taps → per-node EMA → 20 Hz
`computePathCurrents()` → per-path EMA → `renderNetCurrentAnts()`), and the
sparkle/phantom mechanisms were, strongest first: setpoint-vs-measured
offset on rail/DAC-fed paths, the pre-EMA 35 mV hard deadband, single-sample
ant on/off flips, EMA reseed after scan pauses, v1/v2 temporal skew across
the scan cycle, and silent stale-data fallbacks in the ring reader.

- **Stage 0 — metric**: ant on/off flip counter (Graphics.cpp), printed and
  reset by `i!` as `[ants] flips:N in Ss` (+ per-path) — two captures a
  minute apart read directly as flips/min.
- **Stage 1 — fresh-or-fail ring reads**: `adcRingMeanAfterStrict(ch, after,
  n, timeoutUs, bool* fresh)`; `*fresh` false on timeout / engine stop /
  generation bump / DMA lap during the sum. `adcRingMeanAfter()` is now a
  wrapper. Scan taps fail instead of eating history; `ringstale:` counts it
  on the `i!` taps line.
- **Stage 2 — TDM settle**: the 80 µs `delayMicroseconds` in
  `switchAndRead()` actually runs (it was committed already-commented-out in
  the file's first commit; under the ring, `readActive()`'s window starts at
  the call, so pre-settle sweeps were being averaged in).
- **Stage 3 — filtering**: no deadband before the EMA; EMA seeded per
  routing epoch (`pathEmaSeeded[]`, cleared only at fingerprint reset and
  scan-disable) so menu pauses can't inject a full-amplitude reseed;
  deadband AFTER smoothing, per path (`0.035 V × g`, i.e. the same 35 mV
  scaled by the duplicate-folded conductance), with 1.25×/0.75× hysteresis.
  `pathShown_mA[]` (solid 0 below the band) is what `pathCurrentSigned_mA()`,
  `netCurrentInfo`, and the `i!` path lines serve; the raw EMA prints as
  `(ema …)` — unsuffixed, so mA-regex parsers can't match it.
- **Stage 4 — ant vote**: a state flip needs 3 consecutive scanner compute
  ticks agreeing (`netScanComputeGeneration()`, ~20 Hz — NOT LED frames,
  which resample the same tick many times), on top of the existing level
  hysteresis.
- **Stage 5 — longer windows, same dwell**: one ~580 µs hold sliced from
  ring history — early = sweeps s0+1..s0+8 after the settle, late = the
  newest 8 (≥17 sweeps apart, generation-stable, else the tap fails). Twice
  the samples per window of the old read-wait-read.
- **Stage 6 — measured known sources**: in-use rails/DACs are tapped through
  the same sense path ~1/s (same zero offset, same smoothing);
  `fillKnownSources()` prefers the fresh measurement, falls back to the
  setpoint (and immediately on a setpoint change). `i!` gains a
  `sources: 101=set5.00V/meas4.79V …` line. This deliberately reverses
  `isKnownSourceNode`'s exclusion — a momentary high-Z tap is what every
  user node already gets; GND stays untouched (zero reference; the auto-zero
  handles it).
- **Stage 7 — pairwise differential taps** (`[debug] net_scan_pair_taps`,
  default **1**; set 0 for A/B): both ends of a path closed at once on two
  pool ADCs (`infraAcquireAdc` twice), both read from the SAME ring sweeps
  in one dwell, delta stored per path (`pathPairDv[]`) and preferred by
  `computePathCurrents`. Channel assignment alternates per pass so
  per-channel gain mismatch averages out of the EMAs. Falls back to a
  single-ended tap when a second channel or the pair route isn't available;
  taps stay serial, one path per 5 ms slot. `i!`: `(pair:N)` in the taps
  line, ` pair` marker on pair-fed path lines.

Pico-sdk answer to the TODO's question: RP2350B has ONE SAR ADC muxed over 8
inputs; round-robin + FIFO + DMA are already fully exploited by the AdcRing.
"More ADCs" = using more of the 4 routable channels *simultaneously* — which
is exactly Stage 7.

---

## Bench results (same board, baseline firmware = Stage 0 only)

| Check | Baseline | After |
|---|---|---|
| Zero-load TOP_RAIL→row 20 | **−5.44 mA solid phantom** (row 20 measures 4.78 V vs the 5.00 V *setpoint* — a 220 mV systematic; too big to sparkle, it was solid-on) | `+0.00 mA (ema −0.26) pair` — solid off |
| INA0 agreement (DAC0→ISENSE→row 5→GND) | **FAIL**: scan 6.71 vs INA0 4.27 mA (task #32's symptom) | **PASS**: 4.4 vs 4.27 mA |
| Ant flips (60 s no-load watch) | 0 (phantom was solid, not sparkling, on this bench) | 0 |
| Menu-exit ant-flash (mechanism #4) | eyeball item | **instrumented PASS**: SWD click → menu, 8 s hold past the freshness window, long-hold quit → `flips:0` |
| `i!` taps | ok:28425 drift:671 | ok:64916 **(pair:62098)** noroute:12 adcbusy:0 ringstale:0 |
| `X` ring / holds | — | overruns 0, resyncs 0, stalls 0 (max wait 422 µs / 7.9M reads); frame hold 0/0; ch446q pio timeouts 0 |
| `run_all.py` | 6/7 (standing net_currents phantom) | **7/7 — the suite's first full pass on record** |

test_net_currents standalone: 3×/3× PASS 8/8. Both `jumperless_v5` and
`jumperless_og` build. The one run_all encoder_ui FAIL along the way was the
documented stale per-build `jl_input.py` ADDR table (handoff rows 71–72);
after `refresh_jl_input_addrs.sh` it passes, hence the 7/7.

**Disclosure — one HIL check edited.** `test_net_currents.py`'s zero-load
check now filters to the TOP_RAIL net's paths (node 101), matching its own
docstring. The full report also contains the probe buffer feed
(GPIO8→ROUTABLE_BUFFER_IN, nodes 138/139), which legitimately carries
~1.4 mA at all times; the scan measuring it is correct, not phantom, and it
was among the baseline offenders for the wrong reason.

Task #32: the INA-agreement number moved from 57 % high to 3 % without the
planned bisect — the plan deliberately did NOT promise this; leave #32 open
until Kevin agrees the number holds across setups.

---

## RESOLVED (round 3) — the OLED "ping" was a broken bit-bang, not a real I2C transaction

Kevin's hunch ("look up how the oled and I2C works, I don't think it
necessarily acks") was exactly right. Every OLED presence check —
`checkConnection()`, the 1/s health check, the post-frame write-verify, and
the boot autodetect (`probeOledOnInternalI2C0`) — used a **zero-length**
`beginTransmission/endTransmission`. On arduino-pico that is NOT a hardware
transaction: Wire.cpp special-cases 0-length writes into `_probe()`, a
bit-banged GPIO probe that seizes SDA/SCL into SIO and hand-wiggles the
address with an inter-edge delay of `(1e6/clock)/2` µs, INTEGER. At I2C0's
1 MHz bus clock that is **zero** — the wiggle runs at raw GPIO toggle speed
and no SSD1306 (a 400 kHz part) can ever ACK it. So on the internal bus the
detector said "absent" forever while real frames through the I2C block
displayed fine: the panel worked, the detector lied, and everything gated on
`oledConnected` (flag-1 rail-adjust included) stayed off. The repo already
knew about this class: `I2C0Arbiter.cpp`'s header explicitly lists "TwoWire's
zero-length probe ... bit-bangs the pins as GPIO and never enters the SDK"
as NOT covered by the arbiter wrap.

**Fix**: one helper, `oledI2cPing()` (oled.cpp) — a real ONE-byte write
through the hardware block (an SSD1306 control byte 0x00 with no command
after it; the panel executes nothing), at the panel's rated 400 kHz with the
shared bus's 1 MHz restored after (the driver's own clkDuring/clkAfter
dance). Return codes now mean what they say: 0 ACKed, 4 NACK, 5 timeout —
and the write goes through `i2c_write_blocking_until`, so the I2C0
arbiter covers it (the bit-bang was documented as uncovered). All four OLED
ping sites converted. The generic scanners (`findI2CAddress`, SelfTest's
`i2cAck`, Debugs/Apps) are deliberately NOT converted: a lone 0x00 data byte
is a Fast-Write fragment to the MCP4728 at 0x60 — a 1-byte ping is only safe
when you know the target device. They remain debug tools with the bit-bang
caveat at high bus clocks.

Post-fix bench state: `oledConnected` still reads 0 over SWD — with the ping
now genuinely correct, that means no panel is ACKing at 0x3C on the bench
board at this moment (it displayed for Kevin earlier around an external
reset; likely unplugged/reseated since, or on another board). Once a panel
ACKs, the default flag-1 gate arms by itself — `2` is no longer needed.

## Superseded round-1 notes — the bench board's OLED does not ACK (why "no adjust? prompt")

Kevin reported no `adjust?` prompt with "an oled on the internal bus".
Debugged live on the board:

- Config is correct and was already loaded: `rail_click_adjust = 1`,
  `top_oled.enabled = true`, `connection_type = i2c0` (GPIO 4/5), address
  0x3C — matches the r7 netlist (OLED behind the U6 source mux on the
  MCP4728/INA219 bus).
- **`oledConnected` (0x2004d7a8 this build) reads 0 over SWD** — so
  `clickAdjustEnabled()` at flag 1 is correctly false: no persist, no claim,
  no prompt. The reading Kevin sees is the pinned serial line / LED
  highlight, not the OLED.
- Ruled out with the firmware's own driver (`oled_connect()` after config
  flips, all restored afterwards): 0x3C and 0x3D on i2c0/GPIO4-5, and 0x3C
  on type 1/Wire1/GPIO6-7 — **no ACK anywhere**, while INA219 reads on the
  same i2c0 bus work throughout (the bus itself is healthy).
- Open hardware questions for Kevin: does the panel currently show
  *anything* (a frozen startup image would mean it worked at boot and a
  shared-bus error dropped it later — the health check pings 1/s and clears
  `oledConnected` on one error)? Which J9/J10 connector, and which way is
  the U6 mux set? `debug.oled = true` + a reboot captures the init attempt.
- Workaround to use the shortcut today: `` `[display] rail_click_adjust = 2``
  (always) — click while a rail/DAC is highlighted enters the adjuster with
  no OLED needed.

---

## Needs Kevin before commit

1. **Part 1 end-to-end** (once the OLED actually ACKs): highlight rail →
   reading + `adjust?` on the panel → click → adjust → confirm → measure →
   survives reboot; DACs too; GPIO-net click still opens the menu; flag 0/2
   behaviors; long-hold cancel; no double-entry ghosts (the adjuster pumps
   `serviceInner`, an unexercised nesting).
2. **Clickwheel feel** — Stages 2/5/7 lengthened core-1/2 dwells.
3. **Stage 2's own check** (zero functional verification on record): two
   FakeGpio inputs at 0 V / 3.3 V, no cross-channel leakage.
4. The 5-minute eyeball no-flicker watch and the ISENSE loop's steady march.
   If anything regresses: `` `[debug] net_scan_pair_taps = 0`` isolates
   Stage 7.

## Next

Plans for the follow-on work — Kevin's "do we even need the OLED I2C speed
switch?" (answer: only until a panel proves out at 1 MHz; ping and frames
must move together), the crosspoint-resistance measurement/calibration, and
the preloaded-projects folder — are drafted in `DEV_PLANS_82026.md`.

## Files touched

`src/config.h`, `src/configManager.cpp` (2 new keys, all sites),
`src/eyecandy/Highlighting.cpp`, `src/eyecandy/ReadingDisplay.{h,cpp}`,
`src/Graphics.cpp`, `src/sensing/NetVoltageScan.{h,cpp}`,
`src/hardwarestuff/AdcRing.{h,cpp}`, `src/sensing/TimeDomainMultiplexer.cpp`,
`test/hil/test_net_currents.py`, plus `DEV_MERGE_HANDOFF.md` ("In flight")
and `TODO81926.md` annotations.
