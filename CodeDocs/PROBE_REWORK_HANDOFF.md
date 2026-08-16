# Probe rework 5.7.2.0 — what landed, what's open, what the next session does

Session of **2026-08-16**, branch **`dev`**, nothing pushed. Seven commits on top of the
`5.7.2.0` checkpoint tag (five code, two docs/test). The board was attached the whole time; every claim below says
how it was verified.

The task was three things: (1) checkpoint the tree, (2) **make probe handling solid** —
taps, switch sensing, the tip feed in measure position, the DAC set once and *not* re-sent
over I2C, a config flag choosing GPIO-first vs DAC-first, one single crossbar path — and
(3) a **recommendations doc** on using RP2350 peripherals to smooth the concurrent work.
(1) and (2) are done. **(3) is the whole job left** — see "What the next session does".

Read `PROBE_INFRAPATHS_HANDOFF.md` alongside this: it is the living reference for the
probe/InfraPaths subsystem and was updated in place by this work. This file is the
session handoff.

---

## Commit trail (all on `dev`, none pushed)

| # | Commit | What | Verified how |
|---|---|---|---|
| 0 | `7ffdb03` | **Checkpoint 5.7.2.0** — `VERSION` + rebuilt uf2, tagged `5.7.2.0` | v5 + og build; `FIRMWARE_VERSION "5.7.2.0"` in the ELF |
| 1 | `6fa2744` | DAC **set-once at the driver**, `dacs.probe_power_source` order flag, single path made observable | builds x3; `test_infra_paths` 24/24; `run_all` 5/6; full `self_test` PASS; 10 connect/disconnects → DAC0 writes unchanged (20 skips); wavegen 2 s on DAC0 (62,940 sample writes) → exactly one park write at the next rebuild |
| 2 | `13e2313` | **Two switch detectors + agreement classifier** (shadow by default), droop off the hot path, `resetConfigToDefaults` keeps calibration | builds x3; `test_infra_paths` 24/24; `test_config` 30/30; detectors read correctly in both positions under both feeds (numbers below); agree mode classified from boot and survived the dark-LED trick |
| 3 | `b9bfac2` | Probe **LED frame counters**, event-driven show on a dedicated pin, **INA poll off `pauseCore2`**, row bounds | builds x3; `run_all` 5/6; `X` counters over 30 s idle + 20 connect/disconnects |
| 4 | `1036b18` | **Calibration reads both feeds and both detectors**; the self test asks the tip, not the shunt | builds x3; full `self_test` **OVERALL PASS** (`probe_cable PASS sw:meas(tip)`, `tip_voltage PASS droopR:183 sw:meas`) |

`run_all` is **5/6** throughout: the only failure is the pre-existing
`test_net_currents` phantom-current check (`[nvscan] path 1 net 2 20->101 2xp dup0
-7.00 mA` on a zero-load TOP_RAIL net). It fails identically on the checkpoint commit —
not caused by this work, and not in scope for it.

---

## The electrical model everything rests on

Verified against the schematic, the KiCad netlist memory note, and live measurement.

The probe's DPDT switch moves **two** things at once:

| | PROBE_PIN (GPIO10) | ROUTABLE_BUFFER_IN (K.X2) |
|---|---|---|
| **SELECT** | the needle (drives the pad ladder → read on ADC5) | probe WS2811 LEDs + their 100 µF cap C1 |
| **MEASURE** | probe LEDs + C1 | the needle = the tip (read on ADC7) |

The "probe power feed" InfraPaths routes onto K.X2 is therefore **both** the select-mode
LED supply **and** the measure-position tip drive. It is never mode-conditional. Feed
candidates: **DAC0** (K.X7, 2 crosspoints, INA219@0x41's shunt R57 is hardwired in DAC0's
output path so its current is directly sensed) or a **routable GPIO 1..8** (L.X4..11, 4
crosspoints, measured 182–183 Ω on this board, invisible to INA1).

Probe **button and LED data share GPIO 9** (`hardware.probe_led_on_button_pin`, default
on) — one PIO SM with two programs swapped at runtime. This is load-bearing for the LED
cadence question below.

---

## What landed, in detail

### The DAC is set once, in the driver (`src/MCP4728.h/.cpp`)

`MCP4728` now keeps a **class-static per-channel shadow** of the last register word it
delivered — one aligned 32-bit word per channel (bit 16 = known, bits 0..15 = the packed
VREF/PD/GAIN/D11..D0 word) so core-1 stores and core-0 loads can't tear. It is shared
across instances on purpose: Peripherals' `mcp` and WaveGen's `_dac` are the same physical
chip.

- `setChannelValueCached()` skips the I2C transaction when the word is already there.
- `setChannelValue()` stays **unconditional** — WaveGen uses its transaction time as the
  sample clock, so a dedupe there would break waveform pacing — but it updates the shadow.
- The streaming writers (`quickSetChannelValue`, `writeSampleRepeatedStart`,
  `fastWriteAsync`) and address programming **invalidate**.
- `begin()` deliberately does **not** invalidate: it only probes the address, and a second
  instance beginning must not make the first one's next write look new.

`Peripherals`' four setters use the cached variant and toggle LDAC only when a write
happened. **All bookkeeping below the write runs unconditionally** (state, `s_dacHwVolts`,
the user-claim latches and their nudges) — the bookkeeping is about what the caller meant,
not about the bus. That matters for the adjuster's cancel-restore, where the hardware
never moves but state and latches must still update.

The **park epoch is gone**. `parkDacAtMeasureTarget()` just calls the setter; every blind
write that used to hide behind an in-window state value (self-test normalize,
`calibrateDacs`, WaveGen, MicroPython `dac_set(save=False)`) passes through the driver and
moves the shadow. `infraDacParkEpochBump()` survives as `MCP4728::invalidateCache(0/1)` so
its four callers stay valid.

**Measured:** 10 connect/disconnects = 20 rebuilds → DAC0 write count **unchanged**, 20
skips. Wavegen 2 s on DAC0 = 62,940 sample writes → exactly **one** park write at the next
rebuild. `X` and `i@` print `mcp4728 writes A/B/C/D skips A/B/C/D`; the HIL test scrapes it.

*Behaviour note:* a rebuild while WaveGen streams DAC0 now injects one park sample
mid-stream (the old state check skipped it). Rare and benign — don't chase it as a wavegen
glitch.

### Feed order flag (`dacs.probe_power_source`)

`0` = DAC0 first (default, status quo), `1` = GPIO first. Implemented as an `order(walk)`
hook on the `InfraFunction` table, so **candidate indices never move** (0 = DAC0, 1 = GPIO)
and every `infraForceCandidate` caller is untouched. Both candidates always remain; the
flag only decides who is tried first, and it always falls through.

`infraServiceTick` gained a general **switch-back safety net**: if the walk's first
candidate isn't the active one and it resolves + is viable right now, nudge a rebuild.
Two ways this is needed, both hardware-observed today:
- GPIO-first: MicroPython `gpio_release_pin()` frees a GPIO without touching any bridge.
- DAC0-first: a `dac_set()` bringing DAC0 back inside the window when it had been outside
  by persisted **state** rather than by the user-claim latch (booted with `dac0 = 0.8 V`
  saved → no latch → `setDac0voltage`'s release-nudge never fires).

`i@` prints `order:DAC0>GPIO|GPIO>DAC0` and, for the live feed, `paths:1 dup:0 xp:2|4` —
the single-path invariant is now *observable*, not just asserted. `test_infra_paths` grew
a 10-check GPIO-first section (feed on `GP_x`, one path, `xp:4`, DAC0 writes flat across a
rebuild, all-8-claimed → falls to DAC0 `xp:2`, free one → hops back, restore).

### Switch sensing: two detectors, agreement classifier (shadow by default)

| Detector | Reads | MEASURE | SELECT |
|---|---|---|---|
| **A** tip sense (`probeSwitchTipSense`) | PROBE_PIN: 2 mA drive, LOW 2 µs, release to input (no pulls — E9), read after 3 µs, restore OUTPUT-HIGH 8 mA. ~6 µs, no ADC, no I2C. | **H** — the pin sits on the LED supply + C1, which holds it | **L** — the pin is the floating needle, or a pad ladder that drains it |
| **B** per feed | DAC0: INA1 zero-corrected mA vs the hysteresis pair + the new `probe_switch_select_max_ma` ceiling (above it the buffer is LOADED by a touch, never SELECT). GPIO: `probeSwitchFeedBlink` — ADC7 **in volts** before/during a 20 µs feed-pin LOW, held % vs `probe_switch_blink_hold_pct`. | INA ~0 mA / blink **~0 %** | INA ~1.4 mA / blink **~98 %** |

Working in volts for the blink is not cosmetic: ADC7 sits behind the ±8 V scale/offset
network, so 0 V reads mid-scale. The first build compared raw counts and a fully collapsed
tip looked "73 % held" — caught on hardware, fixed.

The predicted `≈170/(170+47) ≈ 78 %` divider for SELECT was **wrong**: measured 97–99 %.
The cable adds essentially nothing. The 50 % default therefore sits in the middle of a very
wide gap.

`debug.probe_switch_agree` selects who **decides**:
- `0` (default): the legacy classifier decides, verbatim; the agreement verdict is computed
  and printed as `shadow:M/S/hold`.
- `1`: agreement decides — A == B sets (fires straight from −1 at boot, no LED needed),
  disagreement **holds**, four disagreements in a row adopt **A**. A is right in every
  persistent case (dark LED, no probe, weak GPIO droop, measure-position load); B is only
  right in the transient select + hot-net touch. The pad-sense veto and the dark-LED
  re-send are kept.

`checkSwitchPositionFast()` (A only, 250 ms, two agreeing reads) runs inside `probeMode()`'s
blocking loop under agree mode — the position used to freeze for up to 80 s there because
`ProbeSwitch` is NORMAL and that loop services CRITICAL only.

Stats line:
`[switch] ina 1.40 mA thr lo/hi 0.90/1.20 A:L B:S agree*:S -> SELECT`
(`droop … ADC7 … V0 …` under a GPIO feed, `B:S(98%)` carries the blink %, `shadow:` when
legacy decides).

**Measured today** (probe attached, tip free, both feeds, both positions, switch physically
flipped during the run): A read H in MEASURE and L in SELECT under both feeds; B read
correctly under both (DAC0 0.00 / 1.40–1.46 mA; GPIO blink 0 % / 97–99 %, droop 0.00 /
1.49–1.56 mA at the calibrated 183 Ω); **every** observed flip had `shadow:` agreeing with
the legacy verdict on the same check. Under agree mode: SELECT on the first check after
`machine.reset()`, and the dark-LED trick (`dac_set(0,0.5)` relocates the feed → LED goes
dark → `dac_set(0,3.33)`) never moved the position.

### Droop model and config-save storms

`gpioDroopCurrentEstimate` no longer writes `probe_droop_v0` back to config from the hot
path — that was the config-save-storm source (every tap is a droop step). V0 is RAM-only,
seeded at claim; only the calibration app and the self test write the persisted value.

`infraProbeDroopOhms()` returns **two different answers on purpose**: the empirical 30 Ω
while the *legacy* classifier decides (it is a reverse fit that branch is tuned to — ~40 mV
of droop reading as ~1.4 mA), and the physical `probe_pad_ohms + xp × crosspoint_resistance`
under agree mode, where nobody divides by R for a verdict any more. Do **not** cherry-pick
the physical fallback onto the legacy path: it is a ×5.5 change to every droop current.

### `resetConfigToDefaults` keeps the whole struct

It restored calibration from a hand-maintained field list that omitted `probe_droop_ohms`,
`probe_droop_v0`, `probe_pad_ohms`, `crosspoint_resistance`, the hysteresis pair,
`probe_max/min_measure`, `use_pio_probe_button` and `probe_led_on_button_pin`. **That was
the "probe_droop_ohms reverts to 0.0" open bug** — `` `reset `` (and the menu reset) zeroed
the self test's droop calibration. It copies the `calibration` and `hardware` structs now,
keeping only the `probe_min/max == 0` fixup. A struct copy can't forget a field.

`test_config` grew 16 checks: plant a sentinel `probe_droop_ohms = 123.4`, run `` `reset ``,
assert it and eight other keys survive, then put every non-calibration key the reset changed
back and assert the file matches its pre-reset contents. (That last assertion caught a real
inconsistency: `flash_reset_type` was serialized as a **number** by the full-save writer and
as a **name** by the in-place updater, so a full save looked like a config change. Now both
write the name.)

### Timing hygiene

- `X` prints `probe led frames N (requests M)  button samples N  led-frame aborts(pause) N`.
  **Measured:** ~2,560 frames/s for ~7 real colour requests per 30 s, ~2,600 button
  samples/s, **0** pause-aborts idle (2 across 20 connect/disconnects, from autosave flash
  writes).
- The current-sense poll **no longer toggles `pauseCore2`** around its I2C read. I2C0 is
  core-0-only — the INA219s, the MCP4728 and the internal-I2C0 OLED are all core-0 driven;
  core 1's only Wire user is WaveGen, already excluded by `isRunning()`. Verified by grep
  across every core-1 path. It also stamps an attempt time **before** any bus traffic with
  a ≥ 10 ms gate (it runs from `serviceCritical()`, i.e. probeMode's ~20 µs loop, and the
  poll stamp only advanced on a *completed* read, so a not-ready conversion was re-asked
  over I2C on every single pass), and `serviceCritical()` no longer polls it a second time.
- `sizeof(probeRowMap)` (= 432 bytes) used as a row bound is now the element count (108).

### Calibration

The Switch Calib app reads **both feeds and both detectors** per position: detector A, then
20 INA medians with the feed pinned to DAC0, then — with the feed pinned to the GPIO
candidate — the ADC7 tip median and the feed-blink held %. It **refuses to save** when the
tip sense contradicts the position the step asked the user to set, saves
`probe_switch_select_max_ma` and (when the two blink percentages straddle with margin)
`probe_switch_blink_hold_pct` at their midpoint, seeds `probe_droop_v0` from the MEASURE tip
only, and **unforces the feed on every exit path** including the new serial-key abort
(which previously did not exist at all — the old code unforced in exactly one place).

The self test's `probe_cable` step now infers the switch position from the **tip sense**
when it has an opinion. This fixed a live FAIL: the old rule was "corrected current above
the SELECT threshold, or an LED delta", and the current half is **not a position** — with
the feed on DAC0 the buffer + tip draw ~1.8 mA through INA1's shunt in MEASURE too. The
detail string now names its evidence: `sw:meas(tip)`.

---

## Open items

### 1. Promote the agreement classifier — needs Kevin's hands (the main one)

Everything is in place and logging; nothing decides yet. The promotion gate is a matrix
that needs a human hand on the probe. With `` `[debug] probe_switch_stats = 1 `` and both
values of `dacs.probe_power_source`, in **each** switch position, touch: nothing / a low pad
/ a top pad / a GND row / a 3.3 V row / a 5 V row / connect held / remove held / no probe
plugged in at all.

Expected: **A** — MEASURE → `H` always, SELECT → `L` except while touching a driven-high
row; **B** — `L`(oaded) or `-` on the heavy touches, never a wrong `S`; **`shadow:`** never
wrong. Then `` `[debug] probe_switch_agree = 1 `` and re-run a flip soak (20 flips each way
should follow within ≤ 2 checks).

Release-note when promoted: a board with **no probe attached settles at SELECT** (the A
tiebreak), where today it settles at MEASURE.

### 2. `probe_current_zero` moves more than the signal it corrects

Found while chasing the cable-test failure and **not yet fixed**. `checkProbeCurrentZero()`
re-measures the INA1 offset at every boot; observed values today: **0.519 mA** at one boot
and **2.289 mA** at the next, against a compile-time default of 2.0. The measure/select
separation the legacy classifier lives on is only ~1.4 mA — so the zero can swing wider
than the signal, and every current threshold rides on it.

This is exactly why the cable test failed on a good cable, and it is a strong argument for
promoting the agreement classifier (detector A does not care about the zero at all). Worth
a session of its own: find out what the offset is actually measuring at each boot (LED
state, feed routed or not, conversion timing) and either stabilise it or stop persisting a
value that isn't reproducible.

### 3. Encoder DAC adjuster — one hands-on confirm

The live path is now a user write (`checkProbePower=true`), so scrubbing the encoder across
2.80 V fires one claim/release nudge + rebuild per crossing. It should be fine (MicroPython
`dac_set` has always done this), but it wants a soak with eyes on it — including the
`Probing.cpp:2040/2048` adjuster lambdas, which write hardware on **every** callback
(preview included) and so rebuild more often than Highlighting's.

Judging feed viability from hardware truth instead was **considered and rejected** — the
reasoning is worth keeping: the self test's normalize step blind-writes DAC0 to 0 V and
*then* forces the DAC0 candidate for the cable phase. With hardware-truth viability that
candidate would never be viable again until a user write, because the park that fixes the
voltage runs only *after* viability passes. The adjuster fix closes the window without
touching the predicate.

### 4. Probe LED cadence — deliberately not changed

The plan's 3.2 (event-driven show) and 3.3 (move the show to core 0) were **not done**, and
the reason is physical. With `probe_led_on_button_pin = 1` (default) the LED data and the
button sampler share GPIO 9, and every sampler pulse (~2,600/s) is a valid WS2811 data bit
that shifts the LED's registers. The ~2,560 frames/s re-send is what overwrites the shifted
frame before it can latch as a visibly wrong colour — it looks like waste and is actually
the mitigation. Making it event-driven would expose the corruption: a bit shifted in per
millisecond, dark within ~24 ms of the last frame.

What landed instead: a colour request always gets a frame; with the LED on its **own** pin
(`probe_led_on_button_pin = 0`) frames are event-driven (nothing else drives that line, and
the 5 s keep-alive re-requests the idle pattern for a chip that reset); and
`hardware.probe_led_refresh_us` (default 0 = every pass) exists to *measure* the corruption
on a scope with the shared line — not as a setting to leave on.

**The real fix is one PIO program that owns GPIO 9** and emits both the WS2811 frame and
the sample pulses itself, from a `pull noblock` / X-held colour word. Zero cross-core, a
constant refresh, and no short-frame corruption possible. That belongs in the
recommendations doc as a Tier-2 item, and it is the thing that would let the show move off
the hot loop.

---

## What the next session does

**Phase 5 of the plan: write `CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md`.** It is
recommendations-only — no behaviour changes — and the plan in the original task carries the
full section outline (Executive summary, A–F). Everything below is *verified this session*
so the next chat doesn't have to re-derive it; cite `file:line` throughout as the plan asks.

**Verified facts to build on:**

- **The scheduler has no notion of time.** `jOSmanager`'s only pacing is a per-band loop
  divisor — `criticalDivisor 1, highDivisor 1, normalDivisor 3, lowDivisor 20`
  (`JumperlOS.cpp:59-62`). No periods, no deadlines, no overrun accounting.
- **Service priorities today** (grep `getPriority` across `src/*.h`): CRITICAL =
  MpRemote, Peripherals, Probing, TermSerial, InjectedCmd; HIGH = Highlighting, Menus,
  MeasureMode, TinyUSB, ProbeButton→`Probing.h:267`; NORMAL = SingleCharCommands,
  ProbeSwitch (`Probing.h:217`), usbPeriodic, oledGui; LOW = configSave, FileCache,
  ProbePads (`Probing.h:240`), oled, liveCrossbar. Note `main.cpp`'s registration comments
  contradict the actual priorities in several places (`peripherals` was commented NORMAL
  while `Peripherals.h:31` says CRITICAL — fixed this session; others remain).
- **`tud_task()` is called raw from 54 places in `src/`** — Python_Proper 13,
  AsyncPassthrough 12, JumperlOS 6, USBfs 4, Commands 4, USBAudio 3, MpRemoteService 3,
  Jerial 3, rest scattered. The Adafruit port already pumps TinyUSB from its IRQ under
  `__usb_mutex`, so these bypass the mutex — re-entrancy hazard. Fix is mechanical:
  `TinyUSB_Device_Task()` / `yield()`. (The plan said 73; the verified count today is 54.)
- **The ADC free-run + FIFO + DMA ring already exists**, in USBAudio:
  `adc_set_round_robin(g_rrMask)` + `dma_channel_configure(..., &adc_hw->fifo, ...)`
  (`USBAudio.cpp:395-423`), and `readAdc()` already snapshots it when streaming
  (`usbAudioSnapshotRaw`, `USBAudio.cpp:814`). Promoting it to always-on is a *reuse*
  recommendation, not new work: `readAdc` becomes a ring read, the pad-ladder burst goes
  from 1–3 ms to ~10 µs, and the ADC lock disappears. This session already split
  `readAdc()` into the lock + `readAdcHeld()` and added `adcTryAcquire()/adcRelease()`,
  which is the seam that work would use.
- **CH446Q single-SM offload is impossible on RP2350B.** Data/clock are GPIO **14/15**
  (`CH446Q.cpp:89-96`), the 12 chip-selects are GPIO **28..39** (`CH446Q.cpp:110-113`), and
  a PIO block's GPIOBASE is 0 or 16 — one SM cannot reach both halves. So the design is
  either two SMs in different blocks handshaking (`irq set 0 next` / `wait 1 irq 0 prev`)
  or a DMA→FIFO half-offload. Note the honest payoff: latency is dominated by the
  `waitCore2()` / `core_sync` handshake, so the win is stability, not speed.
- **`pauseCore2`'s cost is now measurable** — `X` prints `led-frame aborts(pause)`, and the
  INA poll's 20 Hz contribution is already gone. The remaining sources are flash writes.
- **Cross-core protocol** — the flags to tabulate (flag → hazard → replacement):
  `sendAllPathsCore2`, `showLEDsCore2`, `showProbeLEDs`, `showingProbeLEDs`,
  `checkingButton`, `pauseCore2`, `core1busy`/`core2busy`, and the 25 ms `waitCore2()`
  guess. Replacement shape: a core-1 request `queue_t` (`{SEND_PATHS, SHOW_LEDS,
  PROBE_LED, DUMP_LEDS}`) + a generation counter. Ownership rules to state: I2C0 = core 0
  (WaveGen the sanctioned exception, guarded by `isRunning()`) — **this session made that
  rule real**; ADC = the ring engine; USB = core 0; flash = FlashPark; PIO0 CH446Q SM =
  core 1; button PIO IRQ = core 0.
- **Don't-do list for the doc:** no `tud_task` from an IRQ or core 1; count shared-IRQ
  slots before adding one (the pool is **6/6 used** today — `X` prints the census, and
  `IrqSlots.cpp` declines rather than panicking); no I2C/flash/Serial from alarm callbacks;
  don't vendor `async_context_poll` (not linked in this arduino-pico — borrow its
  `at_time` / `when_pending` shape instead).

### The section outline the doc should follow

Reproduced here in full because it lived only in the task prompt —
`CodeDocs/current_task_plan.md` is an unrelated older plan, so there is no other copy in
the repo. Trim or reorder as the evidence warrants, but this is the shape that was agreed.

**Executive summary** — the scheduler has no notion of time (per-band divisor 1/1/3/20,
`JumperlOS.cpp:59-62`); `probeMode()` is a nested blocking loop; TinyUSB is already
IRQ-pumped by the Adafruit port under `__usb_mutex`, so the raw `tud_task()` calls in
`src/` bypass it (re-entrancy hazard; fix = `TinyUSB_Device_Task()` / `yield()`); the ADC
free-run + FIFO + DMA ring already exists in USBAudio and `readAdc` already snapshots it
when streaming — promote it; the INA poll's `pauseCore2` toggle was 20 Hz of LED-frame
aborts (**fixed this session**); single-SM CH446Q strobe offload is impossible on RP2350B
(PIO GPIOBASE 0|16 per block; data/clk 14/15 vs CS 28..39) → a two-block `irq next/prev`
design or a DMA→FIFO half-offload.

**A. What RTOSes do vs jOS** — a table with rows for time-driven wake / ISR→task notify /
software timers / cross-core messaging / mutual exclusion / poll-many / watchdog / SMP,
and columns for FreeRTOS, Zephyr, **the pico-sdk primitive actually available**
(`alarm_pool`, `queue_t`, doorbells, `critical_section_t`, `hardware_claim`,
`watchdog_hw->scratch`), jOS today, and a verdict. Then a "what NOT to do" list: no
`tud_task` from an IRQ or core 1; count shared-IRQ slots first (the pool is **6/6 used**,
`IrqSlots.cpp` declines rather than panicking); no I2C / flash / Serial from alarm
callbacks; don't vendor `async_context_poll` (not linked in this arduino-pico — borrow its
`at_time` / `when_pending` shape).

**B. jOS upgrade** — the corrected priority table + fixing `main.cpp`'s contradicting
registration comments; an API of `periodUs()` / `nextDueUs` / `pending` + an ISR-safe
`requestRun()`, plus per-service last/max/avg µs and overrun counts in `X`; `serviceAll`
runs due-or-pending only, with no catch-up bursts; BLOCKING becomes an explicit modal set;
`serviceInner()` = {ProbeButton, MpRemote, AsyncPassthrough, TinyUSB (mutex-guarded),
Peripherals} exactly once; drop the no-op services (TermSerial, InjectedCmd,
SingleCharCommands, OLED-null, FileCache); and `probeMode` → a state machine
(OFF→ENTER→ARMED→TAP_SEEN→NODE1→ACTION→FEEDBACK→…→EXIT) with milestones: extract
`probeTick()`, replace the `delay()`s with deadlines, move it into `service()`, delete the
`serviceCritical()` calls. Plus `loop()` cleanup (the 10 ms block → a service, the help
waits, the serial drain).

**C. Per-service hardware-offload table** — columns: now → peripheral → SDK calls → gain →
risk → effort. Rows: the always-on ADC ring engine (`readAdc` = a ring read; the pad-ladder
burst 1–3 ms → ~10 µs; the ADC lock disappears; USB audio becomes one consumer; freshness
generations after route changes); CH446Q step 1 = DMA→PIO FIFO + an IRQ STB queue
(non-blocking `sendPaths`), step 2 = a second SM at GPIOBASE 16 with `irq set 0 next` /
`wait 1 irq 0 prev` (payoff is stability — latency is dominated by the `waitCore2` /
`core_sync` handshake); INA continuous + read-latest with no `pauseCore2` (**done**);
MCP4728 dedupe (**done**) + LDAC batching; the probe LED on core 0 and its prerequisite
**one PIO program owning GPIO 9** (button sample + WS2811 frame from a `pull noblock` /
X-held colour word) — zero cross-core, constant refresh, no short-frame corruption; button
IRQ → `requestRun()`; encoder events via `queue_t`; the LED frame tick via a core-1 alarm
pool flag; the USB entry points; OLED (Wire1 for most types) chunked / DMA; watchdog +
scratch post-mortem; `time_us_32` deadlines; and a core-1 request queue
`{SEND_PATHS, SHOW_LEDS, PROBE_LED, DUMP_LEDS}` + a generation counter replacing
`sendAllPathsCore2` / `showLEDsCore2` / `showProbeLEDs` and the 25 ms `waitCore2` guess.

**D. Cross-core protocol cleanup** — a flag → hazard → replacement table, the ownership
rules (I2C0 core 0 with a WaveGen token; ADC = the ring engine; USB core 0; flash =
FlashPark; PIO0 CH446Q SM core 1; button PIO IRQ core 0), and a migration order.

**E. Roadmap in three tiers** — Tier 1 (small, low risk): raw `tud_task` → guarded entry
points; priority/comment fixes + dropping dead services; the INA no-pause + I2C0 rule
(**done**); `inClickMenu` volatile; scheduler deadlines + stats; watchdog; the help-wait
and drain spins. Tier 2 (medium): the ADC ring; the LED tick alarm; the CH446Q
half-offload; a core-1 `queue_t` (SEND_PATHS first); the encoder queue; the combined GPIO 9
PIO program; OLED. Tier 3 (large): the `probeMode` state machine; full CH446Q PIO offload;
WaveGen via I2C DMA + a pacing timer; deleting `pauseCore2`.

**F. How to measure** — the hooks that already exist (`X`, `debugWaitLoopTiming`,
`PROFILE_*`, the HIL suite, the SWD scripts) plus the additions each recommendation needs
(a per-service µs table, an I2C0 transaction counter, ADC ring stats, a frame-abort
histogram — `X` already has the `pauseCore2` half of this — and a tap→crossbar latency
probe).

**Also on the Phase 5 list:** strike/update `PERFORMANCE_OPTIMIZATIONS_ROUND2`
§routableBufferPower (stale — it describes the pre-InfraPaths feed), add a
`DEV_MERGE_HANDOFF.md` row per landed commit, and add a memory note for the electrical
model (GPIO9 shared LED/button, C1 on the LED supply, feed = LED supply **and** tip drive).

**Explicitly out of scope for the doc pass** (they belong *in* the doc as recommendations,
not as edits): the `tud_task` sweep, the scheduler deadline API, ADC ring promotion, CH446Q
PIO/DMA, the `probeMode` state machine, encoder/core-1 queues, and the watchdog.

---

## UNCOMMITTED in the working tree: two sequential measure calibrations

`src/Apps.cpp` has a **~126-line uncommitted change** to the Probe Calib app, written at
Kevin's request at the end of the session and **left uncommitted on purpose** — it is an
interactive app (tap pads, turn the wheel, hold to save) and cannot be verified without a
hand on the probe. If a new session starts with a dirty `Apps.cpp`, this is what it is:
either finish verifying it with Kevin and commit, or `git checkout src/Apps.cpp` to drop it.

**What it does.** Kevin's note: *"our calibration is running on the map max and min values
rather than the DAC output voltage when we're using the DAC. there will be 2 different
calibrations for measure mode and the app should let us set each one separately"*, and when
asked, he chose: the DAC-voltage-vs-GPIO-endpoints split, **with the app doing one, then
the other**. So MEASURE now has two phases, advanced with a **short click** (hold still
saves and exits, as before):

| Phase | Feed (forced) | The wheel adjusts |
|---|---|---|
| measure/DAC | DAC0 (2 crosspoints, stiff) | `calibration.measure_mode_output_voltage`, 5 mV/detent, clamped 3.0–3.6 V, applied to the DAC live (`save=0`, `checkProbePower=false`) |
| measure/GPIO | a routable GPIO (~183 Ω) | `calibration.probe_max_measure` (as before) |

SELECT still adjusts `probe_max` and leaves feed arbitration alone (there the tip is driven
from PROBE_PIN and the feed is only the LED supply). The feed is **forced** per phase so
each calibration measures its own source, re-forced only when the phase changes, and
unforced on exit. If every routable GPIO is claimed the GPIO phase says so and falls back
to the DAC phase rather than silently calibrating the wrong source.

**The open question this needs to answer on hardware.** On the current decode, moving the
DAC drive should have **no effect on the decoded row**: `probeMapRange()` scales the measure
endpoints by `live ADC7 / 3.3`, and the pad reading scales with tip voltage too, so the
drive cancels — which is precisely why the app had been switched off that knob. The
measure/DAC phase therefore prints **tip voltage and decoded row together**, so:

- rows stay put while the voltage moves → the ratio is cancelling, and the DAC knob is a
  physical-level setting (what the self test servos), not a row-alignment control;
- rows move → **the ratio is not cancelling and that is a real decode bug** — most likely
  the 2 ms ADC7 cache decoupling the two reads, or a non-proportional offset term that
  should not be scaled along with the endpoints.

Either answer is worth having, and this is the cheapest way to get it.

**How to verify** (needs hands): run `Probe Calib` from the apps menu; in SELECT tap rows
and confirm `probe_max` still aligns them; flip to MEASURE and confirm `i@` shows
`FORCED:0` with the feed on DAC0; turn the wheel and watch `dac:` and `tip:` move together
while `reading:` holds; short-click and confirm `FORCED:1`, the feed on `GP_x` `xp:4`, and
that the wheel now moves `max:`; hold to save; then confirm `i@` shows **no** `FORCED` and
the feed back on its configured preference.

---

## What the board is left with

Restored at the end of the session, verified by reading `/config.txt` back:
`probe_switch_stats = 0`, `show_probe_current = 1` (as found), `probe_switch_agree = 0`
(shadow mode — the classifier still decides the old way), `probe_power_source = 0`
(DAC0-first), `probe_led_refresh_us = 0` (legacy every-pass cadence). DAC0 was left at
2.0 V by the HIL suite — *outside* the feed's `[2.80, 3.90]` window, which parks the feed
on a GPIO — so it was set back to 3.33 V and the feed confirmed on DAC0 (`i@`:
`-> DAC0 … paths:1 dup:0 xp:2`).

The physical switch is in **SELECT** as of the last check, and the classifier agrees:
INA1 raw 3.69 mA − `probe_current_zero` 2.29 = **1.40 mA**, the measured SELECT signature,
with ADC7 at the loaded 3.17 V. `probe_current_zero` currently reads 2.29 and
`probe_droop_ohms` 182.9 (measured by the last self test).

The flashed firmware is the `1036b18` build (the two later commits are docs and a test
`.py` only), so what is on the board matches HEAD's sources.

---

## Assumptions to confirm with Kevin

- All V5 probe revisions have **C1** (the bulk cap) on the LED supply node. Detector A and
  the feed-side blink both lean on it. If some revision doesn't, both detectors degrade to
  "no opinion" and the classifier holds — safe, but it would mean agree mode can never be
  promoted on that hardware.
- **No-probe-attached settling at SELECT** (the A tiebreak) is acceptable.
- A periodic probe-LED refresh in the tens of ms would be acceptable *if* the combined PIO
  program lands and makes it possible.
- The scheduler / hardware-offload items stay recommendations-only this pass. Only the INA
  poll's `pauseCore2` change and the LED cadence knob touched core-1 behaviour.

---

## How to work on this

```bash
pio run -e jumperless_v5 -e jumperless_og -e jumperless_v5_debug   # all three, every step
pio run -e jumperless_v5 -t upload                                  # flash
python3 test/hil/run_all.py                                         # expect 5/6
python3 test/hil/test_infra_paths.py                                # 24 checks
python3 test/hil/test_config.py                                     # 30 checks
```

- **Config keys are set over port 1 in bracket form**: `` `[dacs] probe_power_source = 1 ``.
  The dot form is `config.section.key = value`; a bare `` `dacs.probe_power_source = 1 ``
  is **not** parsed ("No ] found and not dot notation format") — that cost a debugging
  round today.
- `` `self_test `` waits for input at the end and then resets the board; the session's
  driver script is `$CLAUDE_JOB_DIR/tmp/selftest.py` (sends `\r` at the "reset the board"
  prompt).
- A stale Python process holding `/dev/cu.usbmodemJLV5port1` gives "device reports
  readiness to read but returned no data". It is not a board fault — retry, or `lsof -t`
  the port. Kevin's `jumperless` client normally holds that port; the HIL helpers coexist
  with it fine but occasionally collide right after a flash. The same message appears in
  `run_all.py` when a *previous* file's deferred config save is still in its flash window
  as the next file opens the port — seen once as a spurious 3/6, both files passing
  standalone immediately after. `test_config` now settles 5 s before finishing for exactly
  this reason; if a suite run ever fails in a way a standalone re-run doesn't reproduce,
  suspect this before suspecting the firmware.
- New diagnostics this session: `X` → MCP4728 write/skip counters, probe LED frames vs
  requests, button samples, LED-frame aborts. `i@` → feed order, `paths/dup/xp`, the same
  MCP counters. `` `[debug] probe_switch_stats = 1 `` → one `[switch]` line per check with
  both detectors and the shadow verdict.
