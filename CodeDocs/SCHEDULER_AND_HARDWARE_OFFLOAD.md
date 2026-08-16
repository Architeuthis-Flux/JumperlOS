# Scheduler & hardware offload — brief for the next session

> **STATUS: NOT WRITTEN YET.** This file is the *brief*: the agreed section outline plus
> every fact verified on hardware so far, so the work can start without re-deriving
> anything. The deliverable is this same file, rewritten as the actual recommendations
> document. Nothing here is a recommendation yet.

## The job

Study Zephyr / FreeRTOS / the pico-sdk, sweep **every** `service()` and core 1's loop, and
deliver **recommendations** for using RP2350 peripherals to make the concurrent work smooth.
It is **recommendations-only — no behaviour changes in this pass.** Cite `file:line`
throughout.

This came out of a three-part task (2026-08-16): checkpoint the tree, make probe handling
and its calibration solid, and write this doc. The first two are done and committed — see
`PROBE_REWORK_HANDOFF.md` for what changed, because several of those changes are the seams
the recommendations below would build on.

**Out of scope for this pass** (they belong *in* the doc as recommendations, not as edits):
the `tud_task` sweep, the scheduler deadline API, ADC ring promotion, CH446Q PIO/DMA, the
`probeMode` state machine, encoder/core-1 queues, and the watchdog.

---

## Verified facts to build on

Measured or read out of the tree during the probe session — not assumptions. Where a number
disagrees with the original plan, the number here is the one that was checked.

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

---

## The agreed section outline

Trim or reorder as the evidence warrants, but this is the shape that was agreed with Kevin.

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

---

## Also on this pass

- Strike/update `PERFORMANCE_OPTIMIZATIONS_ROUND2` §2d "Only Check routableBufferPower for
  Power Nets" — **already marked stale** (2026-08-16); it describes the pre-InfraPaths feed
  and a `probePowerDAC` that is now a vestigial view pinned to 0.
- Add a `DEV_MERGE_HANDOFF.md` row per landed commit (rows 18–22 are already in).
- The electrical-model memory note is already written
  (`memory/probe-electrical-model.md`).

---

## How to measure anything you claim

Existing hooks: `X` (resource status — now also MCP4728 write/skip counters, probe LED
frames vs requests, button samples, core-1 LED-frame aborts caused by `pauseCore2`), `i@`
(InfraPaths status, feed order, `paths/dup/xp`), `debugWaitLoopTiming`, the `PROFILE_*`
macros, the HIL suite (`python3 test/hil/run_all.py`, expect **5/6** — the only failure is
the pre-existing `test_net_currents` phantom-current check), and the SWD scripts under
`test/hil/swd/`.

Builds: `pio run -e jumperless_v5 -e jumperless_og -e jumperless_v5_debug` — all three,
every step. Flash: `pio run -e jumperless_v5 -t upload`.

Config keys are set over port 1 in **bracket** form — `` `[dacs] probe_power_source = 1 `` —
or dot form `config.section.key = value`. A bare `` `dacs.probe_power_source = 1 `` is *not*
parsed ("No ] found and not dot notation format").
