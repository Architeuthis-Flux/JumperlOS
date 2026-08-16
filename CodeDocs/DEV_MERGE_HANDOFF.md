# `dev` merge — what's done, what's open, what to do next

Sessions of 2026-08-15. Branch **`dev`** is a fast-forward of
`main` → `infra-paths` → `usb-audio-uac2` plus the commits below. Nothing has
been pushed. `firmware.uf2` in `.pio/build/jumperless_v5/` is the HEAD build.

Read this first; the two feature handoffs (`USB_AUDIO_HANDOFF.md`,
`reading-display-handoff.md`) have the deep detail per feature.

---

## TL;DR

The USB Audio microphone works and the crash that blocked it is root-caused
and fixed. The reading-display work is finished. A 161-agent adversarial
review's 38 verified findings are all fixed. Then, in the second session:

- **The logic analyzer and JulseView are gone** — 5,300 lines of code that
  could never run (see below), plus their MicroPython API, config key, debug
  menu, click-menu leaf and docs.
- **The flash-write park race is fixed for real: FlashPark is ON.** It has the
  shared-IRQ slot it needed, and a 40-iteration flash soak (146 flash-writing
  ops) ran clean with `timeouts 0` — the pre-fix code died at iteration 18 of
  the same soak.
- **A latent bug in the shared-IRQ dedupe** (from the first session's
  `IrqSlots.cpp`) is fixed: it never forgot a removed handler, so a
  remove/re-add cycle would have been dropped as a "duplicate".
- `X` (resource status) now prints the **shared-IRQ slot census** and which
  flash-write park is in charge, so nobody has to guess at this again.
- **The live reading line repaints in place again** (2026-08-16). It was
  scrolling one line + one blank per reading: `clickMenu()` - polled every
  main-loop pass - dropped the pin ~1000x/s. Fixed, and the pin now
  invalidates itself off a port-1 linefeed counter instead of trusting
  callers. `probe_tap()` became a real simulated tap to make this
  reproducible.

**Two things remain, both need Kevin's hands** (open item 1 below): the sensory
checks (listen, probe-while-recording, OLED layouts, Windows boot-restore),
and the A/B soak against `main` — which needs the debug probe on the bus, and
it wasn't this session.

---

## What landed (all committed on `dev`)

| # | Commit | What | Verified how |
|---|---|---|---|
| 1 | `caa413c` | JsonState: bound + JSON-escape every name reaching `get_state()` | builds; `get_state()` exercised via HIL |
| 2 | `ecd23e2` | JsonState: drop seed-only reserved nets from the netlist | as above |
| 3 | `c63d43e` | InfraPaths: a user DAC write claims the DAC from the probe feed | **HIL `test_routing` FAIL → PASS** |
| 4 | `3a1e1fb` | Config save: `MAX_CONFIG_SIZE` 3000→8192, truncation fallback, droop keys, table gaps | **HIL `test_config` PASS** |
| 5 | `0060b72` | Reading display: guard promotion, `handleEnter` hook, width guard, exit-erase ordering | builds; REPL handback exercised |
| 6 | `5ab7d16` | Crash log (HardFault → scratch → reboot → printed once) + stale-doorbell boot guard | **deliberate BusFault captured end-to-end** |
| 7 | `68b93e3` | **USB Audio Class microphone** + WRITE_ADDR runaway fix + probe-arbitration redesign | **15-min soak, 128 iterations, 0 failures** |
| 8 | `bd15e73` | FlashPark (then disabled) + the doorbell-race evidence + SWD tooling | SWD-verified, then disabled on purpose |
| 9 | `42f32af`, `d95b7d6` | The adversarial review's 6 blockers, 10 S-items, 5 W-items | builds; HIL 5/6 with mic streaming; `M?` non-destructive; BusFault path re-verified |
| 10 | `4673813` | Shared-IRQ chain: dedupe arduino-pico's double doorbell registration, decline instead of panic | `free_slot_head` −1 → 5 measured over SWD |
| 11 | `54c931d` | **Remove LogicAnalyzer + JulseView** (dead code) | both boards build; `dir(jumperless)` has no `la_*`; stale config key loads; HIL 5/6 |
| 12 | `66b0eb5` | IrqSlots: forget a handler on `irq_remove_handler`; slot census in `X` | census measured (below); UART(0)/UART(1) from REPL take no slot; HIL 5/6 |
| 13 | `b4fd719` | **Enable FlashPark**; probe-less `stress_flash.py`; ELF-resolved SWD addresses | **40-iteration soak clean, `timeouts 0`** |
| 14 | `46d9d0f` | IrqSlots: swallow the `irq_remove_handler` of a handler it declined (the SDK would assert on the miss) | both boards build; census unchanged; HIL 5/6 |
| 15 | `6046b30` | Reading line repaints in place: fix `clickMenu()`'s polled pin-drop; self-invalidating pin via a port-1 LF counter (`--wrap,tud_cdc_n_write`); `probe_tap()` simulation | raw port-1 capture: one pin, in-place repaints, one re-pin after foreign output; both boards build; HIL 5/6 |

"HIL 5/6" everywhere means: the one failure is `test_net_currents` "zero-load
TOP_RAIL net shows < 1 mA phantom current", which was **A/B-verified against
`main`'s firmware** (identical failure) — pre-existing, out of scope.

### The logic analyzer / JulseView removal, for the record

Neither could ever run: `LogicAnalyzer::handler()` was gated in `loop1()` on a
`last_command_time` that only the handler itself set, `SETUP_LOGIC_ANALYZER_ON_BOOT`
was 0, `cmd_logicAnalyzer` was never registered, JulseView had no callers. And
each would have hard_asserted a core the moment it asked for a shared-IRQ slot
on a board that had touched MicroPython. Removed with them:

- the `jumperless.la_*` MicroPython API (12 functions, their qstrs in
  `qstrdefs.generated.h`, the syntax-highlighter names, `scripts/jumperless_module.py`
  and the `pythonStuff/` copies; `src/micropythonExamples.h` regenerated);
- the `[debug] logic_analyzer` config key — unknown keys are ignored on load
  and preserved by the incremental writer, so an existing `config.txt` with the
  stale line still loads (verified on the board);
- the debug menu's `l` flag and `j` JulseView submenu, `debugLA`, the
  "Logic Analyzr" click-menu leaf, the LA clauses in `loop1()`, `readGPIO()`
  and the `core2stuff()` gate.

`WokwiParser`'s `logicAnalyzerPinToGPIO()` stays — that is Wokwi's logic
analyzer *part*, unrelated. `.github/copilot-instructions.md` still lists a
logic analyzer in the product blurb; that's marketing copy, Kevin's call.

### The shared-IRQ slot census (measured, `X` on the board)

Six slots, all spoken for now that FlashPark is on:

| irq | handler | who |
|---|---|---|
| 14 USBCTRL | `dcd_rp2040_irq` | TinyUSB core |
| 14 USBCTRL | `usb_task_trigger_irq` | Adafruit TinyUSB wrapper (this was the "unidentified 5th user") |
| 26 SIO_IRQ_BELL | `_MFIFO::_irq` | arduino-pico's park — dead weight after takeover, but `_irq` is private so it can't be removed (deduped from 2) |
| 26 SIO_IRQ_BELL | `flashParkIrq` | **FlashPark** |
| 16 PIO0_IRQ_1 | `isrFromPio` | CH446Q |
| 21 IO_IRQ_BANK0 | `jl_gpio_irq` | MicroPython pins (initialised at boot, not on first REPL) |

The first handoff's claim that MicroPython's `rp2_dma_jl` takes a slot was
wrong — `rp2_dma_init()` has no callers. `machine.UART(0)` from the REPL takes
**no** slot on V5: UART0 is already enabled by AsyncPassthrough, so the port
piggybacks and never registers its handler; `UART(1)` uses an exclusive
handler. Both were exercised from the REPL with the census re-read after:
still 6/6, declined 0. So nothing reachable in the shipped configuration is
displaced by FlashPark. The one way to change that: a config where UART0 is
*not* started at boot (`serial_1.function = 0` with async passthrough off —
otherwise either `AsyncPassthrough::begin()` or `Serial1.begin()` brings it
up); `machine.UART(0)` would then ask for a 7th slot and be declined — counted
and printed at boot and in `X`, and its later `deinit()` is swallowed rather
than handed to the SDK to assert on. Not a brick, and not the default. Any
future feature that asks gets the same treatment.

---

## Open items — ranked

### 1. Hands-on checks only Kevin can do

- **A/B the flash soak against `main`, with the debug probe.** FlashPark's
  positive result is in; what's missing is the empirical proof that the wedge
  is pre-existing. The `main` firmware was built at
  `/private/tmp/claude-501/-Users-kevinsanto-Documents-GitHub-JumperlOS/8de8c89e-ef11-4d86-9611-765cedd133d5/scratchpad/wt-main/.pio/build/jumperless_v5/firmware.elf`
  (commit `01c3f7a`) — a temp path macOS may have purged; if it is gone,
  `git worktree add <dir> main && (cd <dir> && pio run -e jumperless_v5)`
  rebuilds it in a minute. Procedure:
  1. Plug in the Debug Probe, start OpenOCD (command in `test/hil/swd/README.md`).
  2. `JL_ELF=<that main ELF> python3 test/hil/swd/stress_flash.py 40` after
     flashing it (`pio run -e jumperless_v5 -t upload` from that worktree, or
     `flash_swd.sh <elf>`). Expect it to die around iteration 18 with core 0 in
     `_MFIFO::idleOtherCore()`; `flash_swd.sh` recovers the board.
  3. Re-flash `dev` and confirm 40 clean again.
  It was deliberately **not** run this session: without the probe a wedged
  board needs a hand on the power, and the board would have been dead for
  whoever came back to it.
- **Listen test.** DAC tone through the crossbar onto ADC0's row → GarageBand
  or REAPER. Note the earlier CoreAudio wedge on the Mac (A/B'd against the
  previous firmware: identical hang, host-side) — reboot or
  `sudo killall coreaudiod` first.
- **Probe while recording.** Tap rows, select/connect mode, measure mode;
  `probe_pauses` ticks once per use, capture resumes ~300 ms after the tip lifts.
- **The reading display, visually.** OLED layouts per node type; the pinned
  serial line during a live reading; typing while readings refresh; the
  zombie-repaint check needs a real probe tap.
- **Windows** boot-restore (`bcdDevice` 0x0101 composite re-enumeration).

### 2. Pre-existing, noticed, not acted on

- `test_net_currents` phantom-current failure (A/B'd on `main`, identical).
- `machine.UART` in `lib/micropython/port/machine_uart_jl.c`: `deinit()`'s
  `if (!was_enabled)` guard reads inverted relative to `init()`'s, so a live
  `uart.deinit()` never runs `uart_deinit()`/`irq_remove_handler` and leaves
  the pins in UART function. Harmless on the slot budget (see census) but it
  bit the HIL routing test once this session: `UART(1)` from the REPL left
  GPIO 20/21 as UART until the routing test reconfigured them.
- `usb_audio_status()` no longer reports bring-up counters; `scripts/jumperless.pyi`
  not regenerated for the new field set (nothing broken).
- `probe_tap(node)` is now a real (simulated) tap: it holds the cached probe
  reading on `node` for ~1.2 s, so MeasureMode and the probe highlighter latch
  like they would for a held tip. It cannot fake button presses/connect mode,
  and a genuine tap wins over it. Used to reproduce and verify the pinned
  reading-line fix.
- `test/hil/swd/tap_session.py` still carries a hardcoded address table
  (`sample_state.py`/`stress_flash.py` now resolve from the ELF).
- The board is left with the mic **saved disabled** (opt-in via `M`/`Ms`).

---

## How to work on this board (learned the hard way)

**Flashing.** Normal: `pio run -e jumperless_v5 -t upload` (picotool, 1200-baud
touch on port 1). When USB is gone: **`test/hil/swd/flash_swd.sh`** — do not
hand-roll the OpenOCD command, because:

1. **OpenOCD's reset does not stop peripherals.** If the mic was streaming, the
   ADC DMA keeps writing into RAM — including OpenOCD's flash work area — and
   you get `** Verify Failed **`. The helper aborts all DMA and stops the ADC
   after `reset halt`.
2. **Never `pkill` OpenOCD.** An un-shutdown session can leave the reset
   vector-catch armed, after which even a BOOTSEL/picotool boot halts silently
   in ROM. The helper always ends with `reset run; shutdown`.
3. **Stale SIO doorbells survive a debugger reset** — `main.cpp` clears both
   cores' bells from a static constructor, so this is fixed; if you see a boot
   hang at the top of `setup()`, that's the shape of it.
4. Halting core 0 over SWD for more than a moment drops the USB device on the
   host. That is the debugger, not a firmware bug; `reset run` brings it back.

**Soaking.** `test/hil/swd/stress_flash.py N` — with OpenOCD on :4444 it
samples the cross-core flags and PCs; without (`--no-swd`, or automatically
when :4444 is closed) it uses USB liveness and brackets the run with the `X`
census. Addresses come from the ELF (`JL_ELF` overrides; default is the
release `jumperless_v5` build — make sure that's what is flashed).

**Slot census / park status.** `X` on the main terminal, last two lines.

**Crash evidence is automatic.** Any HardFault records core, PC/LR/xPSR/SP and
the fault status registers into reset-retained scratch, reboots, and prints
once to the first terminal that gets the menu, with the `addr2line` command.

---

## Suggested order for the next session

1. Kevin: the sensory checks and the probe-assisted `main` A/B (item 1).
2. Then `dev` is releasable.
