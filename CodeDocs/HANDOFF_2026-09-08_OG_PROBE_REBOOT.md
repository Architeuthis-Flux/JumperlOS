# Handoff — OG scanning probe works, but opening a probe session reboots the board

> **RESOLVED 2026-09-08 (same day, next chat).** The reboot was not a flash
> write and not the crosspoint ISR: the OG build was linking the platform's
> fallback second-stage bootloader, `boot2_generic_03h_2` (single-bit SPI,
> `03h` read command, CLKDIV 2 = 66.5 MHz at 133 MHz sys), which is above the
> W25Q128's 50 MHz rating for `03h` reads. Flash fetches went marginal
> exactly when core 1 was fetching hard (LED rendering in a session) with
> the crosspoint switching noise on top - hence random-PC HardFaults on core
> 1 only, never outside a session. Live SSI registers at the fault:
> `CTRLR0=0x001f0300` (standard SPI frame), `BAUDR=2`. The reference OG
> firmware and the framework's own `jumperless_v1` board both use a quad
> Winbond boot2. Fix: `boards/jumperless_og.json` now names
> `boot2_w25q080_2_padded_checksum.S` (what the reference `pico.json` used).
> Verified over SWD: 3 x 20 s and 2 x 60 s injected sessions with
> `probeActive=1` and sweeps completing, zero faults (the old build faulted
> in 8 of 8 runs within 11.5 s). The `g_debugMask` scaffolding is removed.
> Full story in `CodeDocs/OG_BACKPORT.md`, "Session 2026-09-08". The rest of
> this file is the hunt as it stood, kept for the SWD recipes.

Written 2026-09-08 for a fresh chat. Everything below is UNCOMMITTED on `dev`.
Companion: `CodeDocs/OG_BACKPORT.md` → "Session 2026-09-07" has the design
rationale for the probe itself; this file is the bug hunt.

## Bench safety — read before touching anything

- **A Jumperless V5 is on USB and another chat is actively using it.** Do not
  flash it, do not open `/dev/cu.usbmodemJLV5port*`, do not run the HIL suite
  (`test/hil/run_all.py` targets the V5 only). Your board is the **OG**
  (RP2040), which enumerates as `/dev/cu.usbmodemJLOGport1/3/5/7`.
- A Raspberry Pi Debug Probe (CMSIS-DAP, `2e8a:000c`) is wired to the OG's SWD
  pads. **Flash the OG over SWD, not USB** — two `picotool load -x` runs this
  session reported success and left the first 84 sectors of `.text` holding an
  older build, which hard-faults in `crt0` before `setup()`. See "Flashing".
- OG serial: port 1 = terminal, port 3 = UART passthrough, port 5 = MicroPython
  REPL (currently dead, see "Known unrelated"), port 7 = TUI.

## The symptom

Kevin, on hardware: *"when I click the probe button, the light goes on and then
the board reboots."*

Reproduced with no hands on the board, by writing the probe button's press
event straight into RAM over SWD (details in "Recipes"). The reboot is a
**HardFault on core 1**, every time. Timing is not deterministic: it lands
anywhere from 0.0 s to 11.5 s after the session opens.

## What is confirmed

1. **The fault is always on core 1**, at `isr_hardfault` (`pc=0x200037ec`,
   xPSR mode 3). Core 0 is doing something different every time it is caught
   (`micros`, `rotaryEncoderStuff`, `scanProbeRead`) — nothing suspicious.
2. **Core 1's stacked exception frame points at ordinary, unrelated code**, and
   at a *different* place each run: once `core2stuff()`, once
   `HsvToRgb`/`scaleBrightness` (LED rendering). Core 1 was minding its own
   business and something external broke it.
3. Core 1's SP at fault (0x2003_1740–0x2003_17b8) sits in the 8 KB stack
   `malloc`'d by the Arduino core (`core1_separate_stack`), not the linker's
   `core1_stack` at 0x2004_0000. Depth looked healthy, but the exact bounds of
   that malloc'd block were never established — cheap check, still open.
4. **The bare sweep is not the problem.** Outside a probe session, 40
   back-to-back full sweeps via the `s` command ran with zero reboots, and the
   crossbar dump (`c`) is byte-identical before and after a sweep taken while a
   `+ 1-2` connection was live. Sweep cost is 9.1–9.4 ms over 13 groups.

## The bisect

`scanprobe::g_debugMask` (in `src/sensing/ScanProbe.cpp`) is a temporary bench
knob whose bits switch off one piece of machinery at a time. Poke it over SWD
before triggering the session.

| mask | what it disables | result |
|---|---|---|
| 0 | nothing (baseline) | **FAULT** at 0.0 / 1.5 / 7.0 / 11.5 s |
| 1 | driving the button line low in a session (no kit LED) | **FAULT** 0.5 s |
| 2 | the sweep entirely (`scanProbeRead` returns nothing) | no fault in 12 s |
| 4 | the tone (PWM never enabled) | **FAULT** 1.5 s |
| 8 | the crossbar reset at sweep start | no fault in 12 s |
| 16 | 600 µs reset pulse → 20 µs (the reference firmware's value) | **FAULT** 1.0 / 10.0 s |
| 32 | the RESET pin toggle, keeping the 650 µs of delay | **FAULT** 0.5 / 3.5 s |
| 64 | every crosspoint switch in the sweep (reset + tone still run) | no fault in 15 s (×2) |

**What that rules in and out:**

- Necessary: the sweep must actually run (2), it must switch crosspoints (64),
  and `crossbarReset()`'s body must execute (8).
- **Not** necessary: the RESET *pin* (32 faults with the pin untouched — only
  the ~650 µs of `busy_wait` and a no-op loop remain), the kit-LED drive (1),
  the tone (4).
- Mask 8 vs mask 32 is the sharpest pair: the only difference is ~650 µs of
  core-0 stall inside `sweepBegin()` and a call to `markChipXYSuspect()`, which
  **is a no-op stub on the OG**. A pure timing change flipping the outcome says
  this is a **race**, not a logic error.

## Leading hypotheses, ranked, each with its decisive test

### H1 — a flash write with core 1 unparked (XIP hole). Strongest.

On the OG, `__wrap_flash_range_erase` / `__wrap_flash_range_program`
(`src/remembering/FlashPark.cpp`, the `#else // RP2040 / OG` branch) are **bare
pass-throughs**: JumperlOS's flash park is RP2350-only, so nothing in our code
parks core 1 around a flash erase. If core 0 erases flash while core 1 is
executing from XIP, core 1 takes a HardFault **at a random PC** — which is
exactly signature (2) above.

A probe session is a plausible flash-writer: the V5 had precisely this problem
(`test/hil/swd/README.md`: taps → `configChanged` → several full `config.txt`
writes per second while probing), plus `SlotManager` autosave. It also explains
why masks 2/8/64 are quiet: with no crosspoints closed nothing is ever found,
so no state changes and nothing wants saving.

**Test:** breakpoint `__real_flash_range_erase`, open a session, and when it
hits, read `__otherCoreIdled` and core 1's state. If a flash erase runs while
core 1 is not parked, that is the bug. Alternatively watch `filesystemActive`
and `configChanged` through a session and see whether they ever move.

### H2 — unarbitrated crosspoint sends across the two cores.

In `sendXYrawUnchecked()` (`src/CH446Q.cpp`) the whole spin-lock/token
arbitration block sits inside `#if CH446Q_DMA_SEND`, and `CH446Q_DMA_SEND` is
**0 on the OG**. So on this board a single crosspoint send writes the global
`chipSelect` unconditionally, pushes a word to the PIO, and spins until an ISR
clears it — with no protection against the other core doing the same thing.

Worse, `isrFromPio` is registered (`irq_add_shared_handler` + `irq_set_enabled`)
inside `initCH446Q()`, which runs in `setupCore2stuff()` — i.e. **on core 1**.
NVIC enables are per-core on RP2040, so **every core-0 crosspoint send depends
on core 1 servicing that IRQ.** A sweep is ~200 sends from core 0, all of them
leaning on core 1's ISR. That is a large new load on a path that used to be
driven almost entirely from core 1, and it fits mask 64 going quiet.

Note H1 and H2 compose: H2 keeps core 1 busy in ISR context, which is exactly
when it cannot answer the framework's FIFO park doorbell for H1.

**Test:** count `ch446q_timeout_count` and `sendxy_blocked_count` across a
session; and instrument (or breakpoint) the collision — core 0 in the
`while (chipSelect != -1)` wait while core 1 enters `sendXYrawUnchecked`.

### H3 — core 1 stack overflow in the malloc'd 8 KB block.

Weakest of the three; the observed depth looked fine, but the block's bounds
were never pinned down, and a stack that runs off the end of a heap allocation
would also produce random-PC faults.

**Test:** find the malloc'd address (`core1_separate_stack_address`), compare
against the observed SPs, and paint the block to measure the high-water mark.

Ruled out already: the OG's `flash_get_unique_id` override
(`src/boards/og/og_unique_id.c`) is a boot-time concern only and does not run
here.

## Recipes

Scratchpad from the last session (helpers live here):
`/private/tmp/claude-501/-Users-kevinsanto-Documents-GitHub-JumperlOS/414fe49b-6d42-46b4-b3f7-af3ccdaf59bb/scratchpad`
It holds a working PlatformIO venv (`pio313/`), `og_term.py`, `og_listen.py`,
the experiment configs (`exp.cfg`, `exp2.cfg`, `exp3.cfg`) and the ELFs that
were actually on the board (`og_build3..6.elf`). Rebuild any of it freely.

**Build** (system `pio` is broken on this machine):
```
S=<scratchpad>            # or: python3.13 -m venv $S/pio313 && $S/pio313/bin/pip install platformio==6.1.19
$S/pio313/bin/pio run -e jumperless_og     # and -e jumperless_v5 to prove V5 is untouched
g++ -std=c++17 -I src test/test_boards/test_boards.cpp src/boards/board.cpp \
    src/boards/v5/board_v5.cpp src/boards/og/board_og.cpp -o $S/test_boards && $S/test_boards
```

**Flash over SWD** (~2 min, verifies, leaves the board running):
```
OOCD=~/.platformio/packages/tool-openocd-rp2040-earlephilhower
$OOCD/bin/openocd -s $OOCD/share/openocd/scripts -f interface/cmsis-dap.cfg \
  -c "adapter speed 5000" -f target/rp2040.cfg \
  -c "init" -c "reset halt" -c "sleep 100" \
  -c "program .pio/build/jumperless_og/firmware.elf verify" -c "reset run" -c "shutdown"
```
Note this is `target/rp2040.cfg` — `test/hil/swd/flash_swd.sh` is the V5 script
and uses `rp2350.cfg`; don't point it at this board.

**Open a probe session with no hands** — write the button press event into the
`ProbeButton` singleton. `getButtonPress()` picks it up on the next service
pass and `handleProbeButtonActions()` opens the session:
```
mww <addr of ProbeButton::getInstance()::inst + 0x5c> 2      # 2 = connect
```
**Re-derive every address after every build** — they move:
```
T=~/.platformio/packages/toolchain-rp2040-earlephilhower/bin
$T/arm-none-eabi-nm -C .pio/build/jumperless_og/firmware.elf | \
  grep -E " (scanprobe::g_debugMask|ProbeButton::getInstance\(\)::inst|probeActive|__otherCoreIdled|filesystemActive)$"
```
For reference, in `og_build6.elf` these were: `ProbeButton` inst 0x2000dbf8
(so `buttonPress` = 0x2000dc54), `g_debugMask` 0x2001b03c, `probeActive`
0x20023c1c, `__otherCoreIdled` 0x2002a426, `filesystemActive` 0x2002abb3.

**Catch the fault instead of the reboot** — arm the vector catch on both cores,
then poll `curstate` until one halts (`$S/exp3.cfg` does exactly this):
```
rp2040.core0 cortex_m vector_catch hard_err
rp2040.core1 cortex_m vector_catch hard_err
```
Symbolize the stacked frame (offset 24 from SP is the stacked PC, 20 is LR):
```
$T/arm-none-eabi-addr2line -C -f -e $S/og_buildN.elf <addr>
```

**Talk to the terminal:** `$S/pio313/bin/python $S/og_term.py '<cmd>' [maxwait]`
sends one command to port 1, strips ANSI, and reads until the board goes quiet.
Careful: `/` and `%` open the interactive File Manager — Ctrl-Q to leave it.

The OG-only `s` command is the probe diagnostic: `s` = tip level, button state,
one full sweep with timing, then a clean refresh; `s b` watches the button
decoder for 5 s; `s r <node>` prints the raw tone patterns for one node (a
touched node reads `0101 / 0101`, an untouched one `1111`).

## State of the tree

Uncommitted on `dev`, per Kevin's rule that a fix is committed once it works on
his hardware:

- **New:** `src/sensing/ScanProbe.{h,cpp}` — tone sweep, tip pre-read, button
  read by coupling.
- **Edited:** `src/JumperlessDefines.h` (OG `PROBE_PIN` 19 / `BUTTON_PIN` 18 /
  ADC pins 26–29 — they were the V5's values, which on the OG are chip selects
  and RP2350 pin numbers), `src/Probing.{h,cpp}` (one-button hold decoder,
  `scanProbeRead()`, capability gates), `src/main.cpp` (probe stack registered
  when `caps.scanningProbe`), `src/SingleCharCommands.{h,cpp}` (the `s`
  diagnostic), `src/snakes/Python_Proper.cpp` (the MicroPython heap FATAL line
  prints once instead of ~200×/s).
- Both targets build; the host board test passes. OG RAM 67.6 %.

**Before committing anything**, take the bench scaffolding out: the
`g_debugMask` knob and its six `if (g_debugMask & N)` sites in `ScanProbe.cpp`,
the `xy()` wrapper's bit-64 branch, and the mask check at the top of
`Probing::scanProbeRead()`. They exist only to run the table above.

Two unrelated things in the working tree: the tracked
`.pio/build/jumperless_v5/firmware.uf2` is dirty from a V5 build (it belongs
only in a release commit), and `CodeDocs/ROUTING_SWEEP_2026-09-03.md` shows a
240-line deletion made at 11:44 on 09-07 that did **not** come from this
session — check with Kevin before staging it.

## Already verified — don't spend time re-testing

- Sweep timing and correctness outside a session (9.1–9.4 ms, 0 false
  positives over 45+ sweeps, crossbar restored exactly).
- Button decoder sampling (~363 µs per read, stable `state 0` at rest).
- V5 build unaffected; host board-support test green.

## Still unverified by hand (needs Kevin and a needle)

A real touch resolving to a row; two touches making a connection; the
short/long press gestures entering and toggling a session; whether the kit LED
actually lights; corner rows, header pins, and a needle on a rail (a positive
rail is reported as 3.3 V — the reference firmware asked 3.3 vs 5 V with a
short/long press, and that choice is Kevin's to make).

## Known unrelated, still open

- **MicroPython is disabled on the OG**: `mpAllocHeap` finds 40248 B of C heap
  against a 40960 B need — 712 bytes short — so the REPL on port 5 is dead and
  the 54 KB `jumperless.pyi` write fails. It was short on the previous build
  too, so this predates the probe work. It is the OG's primary-deliverable
  regression; `OG_BACKPORT.md` lists where the static RAM can come from.
- The `X` panel still labels GPIO 9/10 as the probe pins (a V5 table) and
  offers GPIO 26/27 as OLED pins — on the OG those are the ADC inputs the
  sweep uses. Harmless today; would matter if an OG panel is ever wired up.
