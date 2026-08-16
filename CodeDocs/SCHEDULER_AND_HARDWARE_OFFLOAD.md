# Scheduler & hardware offload — recommendations

> **STATUS: NOT WRITTEN YET.** Everything below is the *brief* — the original ask, the
> orientation a fresh session needs, the facts already verified on hardware, and the agreed
> section outline. **The deliverable is this same file, rewritten as the actual
> recommendations document.** Nothing below the outline is a recommendation yet.
>
> This file is written to be read with **no prior context**. It should contain everything
> needed to start; if something is missing, that is a bug in this file.

---

## 1. The original ask, verbatim

The task was given as three parts (2026-08-16, branch `dev`):

> (1) tag the current tree `5.7.2.0` as a checkpoint; (2) make probe handling solid — taps,
> switch sensing, the tip feed in measure position, the DAC set once and *not* re-sent over
> I2C, a config flag choosing GPIO-first vs DAC-first as the feed (always falling through),
> and one single crossbar path (~160 Ω) for a GPIO feed; (3) **study Zephyr/FreeRTOS +
> pico-sdk and do a broad sweep of every `service()` and core 1's loop, delivering
> *recommendations* for using RP2350 peripherals to make the concurrent work smooth.**
> Priority now = probing/calibration; the sweep is a doc.

**(1) and (2) are done, committed and hardware-verified.** See `PROBE_REWORK_HANDOFF.md`.
**(3) is this file, and it is the only part left.**

Two standing constraints from Kevin, which apply to this work too:

- **Recommendations-only this pass. No behaviour changes.** The point is a document.
- **Never push.** Commit locally when work is verified; pushing is his call.

---

## 2. What this project is

**JumperlOS** — firmware for the **Jumperless V5**, a breadboard whose rows are wired
together by a 12-chip **CH446Q analog crossbar** instead of jumper wires. An RP2350B
(dual-core Cortex-M33, arduino-pico core) drives the crossbar, a 4-channel DAC, several
ADCs/INA219 current sensors, addressable LEDs under every row, an OLED, and a **probe** —
a handheld tip the user taps rows with, whose DPDT switch has a SELECT and a MEASURE
position.

The board is **attached over USB** while working on this. It enumerates several CDC ports:

| Port | Use |
|---|---|
| `…JLV5port1` | main terminal — single-char commands, the `` ` `` config interface |
| `…JLV5port5` | MicroPython raw REPL (the HIL suite drives this) |
| `…JLV5port7` | USBSer3 machine backchannel (`:` verbs, JSON/YAML) |

### Cores, roughly

- **Core 0** — `setup()` / `loop()` (`main.cpp:210` / `:718`). Runs the **service
  scheduler** (`jOS`), USB, the terminal, MicroPython, the probe button IRQ.
- **Core 1** — `setup1()` / `loop1()` → `core2stuff()` (`main.cpp:609` / `:1399` / `:1540`).
  Owns the **LEDs** and the **CH446Q** crossbar sends, plus the probe LED and the wave
  generator's DAC streaming.

Cross-core coordination today is a set of **shared volatile flags** (`sendAllPathsCore2`,
`showLEDsCore2`, `showProbeLEDs`, `pauseCore2`, `core1busy`/`core2busy`, …) plus a
`core_sync` mutex and a 25 ms `waitCore2()` guess. Replacing that is section D.

### The scheduler you are reviewing

`src/JumperlOS.h` / `src/JumperlOS.cpp`. Services are objects with a `service()` method and
a `getPriority()` (`ServicePriority`, `JumperlOS.h:102`) returning CRITICAL / HIGH / NORMAL
/ LOW, returning a `ServiceStatus` (`JumperlOS.h:91`: IDLE / BUSY / BLOCKING).

- `jOSmanager::serviceAll()` — `JumperlOS.cpp:164`, the main-loop dispatcher.
- `jOSmanager::serviceCritical()` — `JumperlOS.cpp:341`, the reduced set that blocking
  modal loops (notably `probeMode()`) call to stay alive.
- Registration list — `main.cpp:495–538`, one `jOS.registerService(...)` per service with a
  trailing comment naming its priority. **Several of those comments are wrong**; the
  authoritative answer is `getPriority()` in each service's header.

**Sweep every `service()`** means: for each registered service, what it does, how often it
actually runs, what it blocks on, and what RP2350 peripheral could do that job instead.

---

## 3. How to work on this repo

```bash
# build ALL THREE environments after every step - og and debug catch different things
pio run -e jumperless_v5 -e jumperless_og -e jumperless_v5_debug

pio run -e jumperless_v5 -t upload        # flash the attached board

python3 test/hil/run_all.py               # hardware-in-the-loop suite; expect 5/6
```

**`run_all.py` is expected to report 5/6.** The one failure — `test_net_currents`, "zero-load
TOP_RAIL net shows < 1 mA phantom current" — is **pre-existing and out of scope**; it fails
identically on the `5.7.2.0` checkpoint commit. Anything *else* failing is a real regression.

Individual suites worth running: `test/hil/test_infra_paths.py` (24 checks),
`test/hil/test_config.py` (30 checks).

**Config keys** are set over port 1 in **bracket** form:

```
`[dacs] probe_power_source = 1
```

or dot form `config.section.key = value`. A bare `` `dacs.probe_power_source = 1 `` is **not**
parsed ("No ] found and not dot notation format") — this cost a debugging round.

**Gotcha:** a stale process holding port 1 produces "device reports readiness to read but
returned no data". Not a board fault. It also appears in `run_all.py` when a previous file's
deferred config save is still in its flash window as the next file opens the port — if a
suite run fails in a way a standalone re-run doesn't reproduce, suspect that first.

**Docs worth reading before starting:**

| File | Why |
|---|---|
| `PROBE_REWORK_HANDOFF.md` | what changed in parts (1) and (2); several changes are seams this work would build on |
| `PROBE_INFRAPATHS_HANDOFF.md` | the probe/InfraPaths subsystem reference |
| `DEV_MERGE_HANDOFF.md` | the branch's whole commit history + "how to work on this board" (flashing, SWD recovery, soaking) |
| `PERFORMANCE_OPTIMIZATIONS_ROUND2.md`, `PERFORMANCE_ROUND3.md` | prior optimisation passes; §2d of ROUND2 is marked stale |
| `differencesRP2040RP2350B.md`, `dma_sectopn_rp2350b.md` | RP2350B specifics |

---

## 4. Verified facts to build on

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



---

## 5. The agreed section outline

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

---

## 6. Also on this pass

- Add a `DEV_MERGE_HANDOFF.md` row per landed commit (rows 18–22 are already in).
- `PERFORMANCE_OPTIMIZATIONS_ROUND2` §2d "Only Check routableBufferPower for Power Nets" is
  **already marked stale** — no action needed unless the sweep finds more like it.
- The electrical-model memory note is already written (`memory/probe-electrical-model.md`).

---

## 7. What "done" looks like

A single document — this file — that a firmware engineer can act on:

- Every claim carries a `file:line`.
- Every recommendation names the **peripheral**, the **SDK calls**, the **gain**, the
  **risk**, and the **effort**, and is honest about payoff (the CH446Q entry, for instance,
  should say the win is stability rather than latency, because latency is dominated by the
  `waitCore2()` / `core_sync` handshake).
- Anything bounded or sampled says so out loud rather than reading as full coverage.
- The roadmap is tiered so Tier 1 could be done in an afternoon.
- **No code changes are made in this pass.**
