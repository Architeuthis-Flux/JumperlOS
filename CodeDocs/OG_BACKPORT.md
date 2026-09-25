# OG Jumperless (RP2040) Backport — Living Doc

**This is the source of truth for the OG backport across chats. Update the
status checklist at the bottom at the end of every working session.**

## Why this exists

The "OG" Jumperless runs an **RP2040** (264 KB SRAM, **no PSRAM**, 16 MB
W25Q128 flash). JumperlOS today targets only the Jumperless **V5** (RP2350B +
8 MB PSRAM). The goal of the backport is to let the cheaper OG hardware run the
JumperlOS **shared core** so it is controllable by **LLM tools and
MicroPython** — that is the primary deliverable. The fancy V5-only UI (menus,
rotary encoder, OLED, breadboard text, logic analyzer, editors) is intentionally
**not** ported.

This is really a **board-support architecture**: once it exists, adding the OG
(and the future **V6**) is mechanical. The shared core never branches on board
`#define`s; it asks a board descriptor what the hardware can do.

## Architecture

```
shared core (board-agnostic)
  NetManager · unified router · States/slots · MicroPython API · serial/LLM CDC
        │  calls only the contract, never a board macro
        ▼
src/boards/board.h   ← THE CONTRACT
  BoardTopology (pure data) + HAL function decls + capability queries
        ├── src/boards/v5/board_v5.cpp   (Y0 = BOUNCE_NODE, 445 LEDs, MCP4728 I2C, PSRAM)
        └── src/boards/og/board_og.cpp   (Y0 = CHIP_L,      111 LEDs, MCP4822 SPI,  no PSRAM)
```

Board selection is compile-time (different MCUs ⇒ one PlatformIO env per board).
`OG_JUMPERLESS` selects the OG package; the default V5 env is unchanged.
Same-MCU revision differences (e.g. V5 r4 vs r5, or V5 vs V6 later) can still be
resolved at runtime via `jumperlessConfig.hardware`.

### The contract (`src/boards/board.h`)

- `enum class Y0Rule { BounceNode, ChipL }` — the single biggest topology
  difference (see below).
- `struct BoardTopology` — name, `y0Rule`, `y0Node`, crossbar `xMap[12][16]` /
  `yMap[12][8]`, `bbNodesToChip[62]`, **explicit** GPIO / ADC / DAC tables, and
  a `BoardCaps` flag block.
- Routing primitives the unified router builds on:
  `boardY0Node(b)`, `boardRowToChipY(b,row,&chip,&y)`.
- Capability queries: `boardFindGpio/Adc/Dac`, `boardCanSetRailVoltage`,
  `boardHasNode`, and `boardCapabilitiesJson(b,buf,cap)` (compact JSON for the
  USBSer3 LLM backchannel; Arduino-free + bounds-checked).
- The header is deliberately **Arduino-free** so the descriptors are unit
  tested on the host. Keep it that way.

Both `v5BoardTopology` and `ogBoardTopology` are always linked; `currentBoard()`
picks via `OG_JUMPERLESS`. The host test compares them directly.

## OG vs V5 hardware differences

| Area | V5 (RP2350B) | OG (RP2040) | Where handled |
|------|--------------|-------------|----------------|
| Crossbar Y0 | `BOUNCE_NODE` (199), virtual hop bus; chip L Y selects BB chip | `CHIP_L` (11) directly; L is the literal hub | `BoardTopology.y0Rule` / `yMap`, unified router |
| Corner rows (1/30/31/60) | rows 30/31/60 → K/L; row 1 → chip A | rows 1/30/31/60 → chip L | `bbNodesToChip` |
| LEDs | 5 per breadboard row, 445 total, logo ring + pads | 1 per row, 111 total, 1 logo LED, no pads | LED HAL: sample center pixel (col 2) |
| DAC | MCP4728 quad, I2C | MCP4822 dual, SPI (faster waveforms) | HAL `initDac/setDac*`; `caps.spiDac` |
| Rails | firmware-controlled (DAC ch C/D) | hardware switch (+3.3/+5/±8V) — read-only | `caps.railsFirmwareControlled=false` |
| DACs ranges | DAC0/DAC1 ±8 V | DAC0 0–5 V, DAC1 ±8 V | `BoardTopology.dac[]` |
| ADCs | 8 ch (pins 40–47) | 4 ch: ADC0–2 buffered 0–5 V, ADC3 ±8 V | `BoardTopology.adc[]` |
| Routable GPIO | 8 (`RP_GPIO_1..8`) + UART TX/RX = 10 | **3**: `RP_GPIO_0`, `RP_UART_TX`, `RP_UART_RX` | `BoardTopology.gpio[]` (RP_GPIO_0 is its OWN node, never aliased to GPIO_1) |
| Nano reset | two hardwired GPIO reset lines | single routable `NANO_RESET` node | OG `xMap` chip I uses `NANO_RESET` |
| Probe | resistive ADC pads + buttons + INA switch | crossbar **scanning** (`scanRows`) | Phase 2; `caps.scanningProbe=true` |
| UI | encoder + OLED + breadboard text + menus | **none** — serial + 1-LED/row only | dropped via `build_src_filter` |
| Memory | 264 KB SRAM **+ 8 MB PSRAM** | 264 KB SRAM, **no PSRAM** | V5 MP heap 96 KB; **OG MP heap 20 KB** (SRAM, ~30 KB total pool); `caps.hasPsram=false` |

Node-id namespace (GND=100, DAC0=106, ADC0=110, RP_GPIO_0=114, …) is shared
between OG and V5, which is why one descriptor table type serves both.

## The Y0 difference (most important detail for the router)

- **V5 `BounceNode`:** `yMap[A..H][0] = BOUNCE_NODE`. It is *not* a hole; the
  pathfinder uses it as an internal bus to hop between chips. Chip L's Y-axis
  selects which breadboard chip a special-function line bridges to.
- **OG `ChipL`:** `yMap[A..H][0] = CHIP_L`. Chip L is the central hub itself;
  there is no bounce concept. To hop between BB chips you route through L.

`boardRowToChipY()` already skips Y0 for both boards (Y0 is never a routable
row). The unified router must consult `boardY0Node()` instead of hard-coding
`BOUNCE_NODE` at the chip-hop sites.

## How to add a new board (e.g. V6)

1. `src/boards/v6/board_v6.cpp` — define `const BoardTopology v6BoardTopology`
   (copy the closest existing descriptor, edit the maps/tables/caps).
2. `src/boards/board.h` — add `extern const BoardTopology v6BoardTopology;`
   and a branch in `currentBoard()` (e.g. `#elif defined(JUMPERLESS_V6)`).
3. `platformio.ini` — add `[env:jumperless_v6]` with the right `board`/platform
   (V6 is RP2350-family, so it can extend `env:jumperless_v5`) and
   `-DJUMPERLESS_V6`.
4. Add the board to the host test's parity assertions.
5. No shared-core file should need to change. If it does, that's a missing seam
   in the contract — add the seam, don't sprinkle `#ifdef`s.

## Verification (the runnable check)

Host-buildable, no hardware / PlatformIO needed:

```
cd JumperlOS
g++ -std=c++17 -I src \
    test/test_boards/test_boards.cpp \
    src/boards/board.cpp src/boards/v5/board_v5.cpp src/boards/og/board_og.cpp \
    -o /tmp/test_boards && /tmp/test_boards
```

It asserts the topology kernel returns the right chip/Y for both Y0 rules,
the corner-row differences, GPIO non-aliasing (OG `RP_GPIO_0` ≠ `GPIO_1`),
ADC3/DAC1 ±8 V ranges, capability flags, and the capability-JSON bounds safety.
**Run this after any edit to the board layer.**

## Per-phase status checklist

### Phase 1 — skeleton (boot + route + MicroPython + LLM serial)

Done (architecture + first hardware bring-up — VERIFIED ON A REAL OG BOARD):
- [x] `src/boards/board.h` contract (BoardTopology, HAL decls, capability queries).
- [x] `src/boards/board.cpp` selection + topology-driven routing primitives.
- [x] `src/boards/v5/board_v5.cpp` descriptor (mirrors current V5 data; not yet
      wired into V5 live routing → zero V5 behavior change).
- [x] `src/boards/og/board_og.cpp` descriptor (ported from OG reference firmware).
- [x] `src/boards/og/og_atomic.cpp` — `__atomic_test_and_set` shim (M0+ has no
      native atomics; backed by a fixed RP2040 hardware spinlock, NOT a global
      ctor).
- [x] `boards/jumperless_og.json` — repo-local board (RP2040, cortex-m0plus,
      16 MB flash, variant `jumperless_v1`, Jumperless USB VID/PID).
- [x] `[env:jumperless_og]` in `platformio.ini`: minimal-diff build (compile the
      full tree for RP2040, drop only `boards/v5/`), `-DOG_JUMPERLESS`,
      `-URP2350_PSRAM_CS`, 1 MB FatFS, own extra_scripts (no V5 fs-lock).
- [x] `USB_CDC_ENABLE_COUNT` / `USB_MSC_ENABLE` per-board overridable;
      `boardCapabilitiesJson()` for the USBSer3 LLM backchannel.
- [x] Host test `test/test_boards/test_boards.cpp` (green).
- [x] **Compiles + links** (`pio run -e jumperless_og`): Flash 10.5%, RAM 79.3%.
- [x] **Flashes** over SWD (`openocd ... program`) AND UF2/BOOTSEL.
- [x] **BOOTS STABLY on real RP2040 hardware AND is controllable over serial.**
      Enumerates as USB "Jumperless OG" / "JLOGport" with all 4 CDC ports
      (`JLOGport1/3/5/7` = CDC0 cmds / USBSer1 passthrough / USBSer2 mpremote /
      USBSer3 backchannel), stable indefinitely. The **USBSer3 LLM backchannel
      works**: an `A` query returns the full status JSON (version, ADC, INA
      current, GPIO, net list). RAM 69.9% static (183 KB). The PRIMARY GOAL
      (LLM/serial control of the OG) is met. MicroPython REPL on USBSer2 is the
      next thing to verify.

### BOOT ROOT CAUSES — RESOLVED
Two deterministic boot crashes (found via SWD+GDB `vector_catch`/`break abort`)
plus heap-exhaustion crashes, all fixed:
1. **RP2040 flash unique-ID read** (`src/boards/og/og_unique_id.c`): the RP2040
   has NO on-chip unique-ID register (unlike RP2350), so the pico-sdk reads it
   from QSPI flash via a `0x4B` command (`flash_get_unique_id` -> `__flash_do_cmd`)
   in a PRE-MAIN constructor. That early flash command (XIP disabled, flash->RAM
   veneer) hard-faulted and reset-looped the board. Override `flash_get_unique_id`
   to return a fixed ID with no flash access (we don't need it - USB serial is the
   fixed "JLOGport"). Build links src/ first + `--allow-multiple-definition`, so
   our def wins. (This ALSO made `openocd ... reset` boot cleanly, since the
   firmware no longer issues that flash command the debugger-reset couldn't survive.)
2. **FatFS FIL too big** (`lib/FatFS/src/ffconf.h`): `FF_MAX_SS=4096` + `FF_FS_TINY=0`
   gave each open file a 4 KB sector buffer -> `make_shared<FIL>` = 4152 bytes,
   which `bad_alloc`-aborted on every file open (config/slot/provisioning) on the
   tight/fragmented ~71 KB heap. Set `FF_FS_TINY=1` on OG: FIL shrinks to ~56
   bytes (shares the volume window), no on-disk format change, slower I/O.
3. **Heap exhaustion in loop()**: `undoInit` grabbed ~21 KB (no-PSRAM ring +
   persist scratch) and fragmented the heap so even a 107-byte `printf` aborted.
   Disabled undo on OG (`Undo.cpp` undoInit early-returns; record hooks no-op when
   uninitialized). Also skipped `oled.init()` on OG (no OLED; it kicked
   refreshConnections + debug printfs).
Plus: `JumperlessState` made non-copyable (States.h) with all 5 copy sites
rewritten in place (the ~50 KB copies couldn't fit the RP2040 stack/heap), and
`provisionFirmwareFiles` skipped on OG (V5 LED/OLED image assets).

### UPDATE: JumperlessState is now NON-COPYABLE (state-copy fix applied)
Per "never copy the state": `JumperlessState`'s copy ctor + copy assignment are
now `= delete` (States.h), so any copy is a COMPILE ERROR. The 5 copy sites the
compiler flagged (all in States.cpp) were refactored to work in place:
- `migrateOldSlotFile`: parses the legacy file directly into `activeState`
  (was a ~50KB `JumperlessState newState` stack local + copy — the prime
  boot-crash suspect when a persisted legacy slot is loaded at boot).
- `printSlotInfo`: persists-if-dirty + reload-from-disk instead of save/restore
  via a 50KB copy (also removed a dead `tempState`).
- `pushHistory`/`undo`/`redo`: the legacy full-state snapshot history
  (STATE_HISTORY_SIZE==0, superseded by Undo.cpp deltas) is now a no-op instead
  of copying the whole state.
Builds clean (OG 72.5% RAM). **NOT yet verified on hardware** — needs a BOOTSEL
reflash (user was away). If it boots reliably now, this was the root cause. If
not, get the backtrace (probe + vector_catch) and also erase the persisted FS.

### CRITICAL ROOT CAUSE (original finding that led to the fix above)
`JumperlessState` (the full board state: nets[MAX_NETS] each with
nodes/bridges[MAX_NODES], paths[MAX_BRIDGES], chipStates, power, ...) is ~50 KB
on V5, ~33 KB on OG after the MAX_BRIDGES/MAX_NODES cut. **The RP2040 has only
~50 KB of TOTAL free RAM** (8 KB core0 stack at 0x20040000-0x20042000 + the heap
from `&end`≈0x2002e000 to 0x20040000). `States.cpp` COPIES the whole state in
several places — stack locals (`JumperlessState newState` in the legacy-slot
migration ~line 3216; `tempState`/`savedState` in preview ~line 3323) and
assignments. A 33-50 KB copy **overflows the 8 KB stack → hard fault**, or
exhausts the heap. States.h:264 even warns "DANGER: copy uses 50KB stack!".
- This is why it booted ONCE with a fresh FS (boot path avoided a copy) and now
  deterministically crashes: the persisted 1 MB FatFS has a slot file whose
  load/migration path makes a copy. Adding heap does NOT help a stack copy.
- **FIX (next session), in order:**
  1. Reattach the SWD probe; `openocd ... -c "flash erase_address <FS_START> 0x100000"`
     to wipe the persisted FatFS (FS is at the top of the 16 MB flash just under
     the 4 KB EEPROM; confirm FS_START from the build/linker), OR generate a
     blank-FS UF2. A fresh FS should let it boot again immediately (confirms the
     diagnosis). picotool v2.0.0 here has NO `erase`, so use openocd or a UF2.
  2. Make the OG-relevant state-copy paths in States.cpp NOT copy 50 KB: operate
     in place on `globalState`, or use a single shared scratch buffer, or gate
     legacy-slot migration / preview off on OG (capability flag). Grep
     `JumperlessState ` for stack locals + `= activeState` / `= globalState`.
  3. Consider shrinking `JumperlessState` further on OG (it's the only ~big
     object) so any unavoidable copy fits.
- Get the boot backtrace to confirm: SWD probe + `openocd` gdb server, then
  `gdb -batch`: `monitor reset halt; monitor cortex_m vector_catch hard_err;
  break abort; continue; bt`. (NOTE: halting through `__flash_do_cmd` /
  `flash_get_unique_id` at boot gives a debugger-induced fault — let it run, then
  halt ~3 s later, or catch the real fault with vector_catch + a DTR/connect trigger.)

RP2040-vs-RP2350 source fixes applied (all guarded by `#if defined(PICO_RP2350)`
so V5 is byte-identical):
- `ArduinoStuff.cpp` PSRAM XIP CS1 registers; `Debugs.cpp` / `Peripherals.cpp` /
  `SingleCharCommands.cpp` / `RotaryEncoder.cpp` third PIO block (`pio2`);
  `Peripherals.cpp` GPIO-function-name table (RP2350-only mux entries);
  `Probing.cpp` `gpio_coproc.h` + `gpioc_bit_oe_set/clr` → `gpio_set_dir`.
- `usb_descriptors.cpp`: product/serial strings board-gated to "Jumperless OG"/"JLOGport".

RAM reduction done (RP2040 has ~34 KB heap after static; static-init + FatFS were
aborting boot via `operator new` → `bad_alloc` → `abort`. Found each via SWD+GDB
`break abort; bt`). Cuts (all `#if defined(OG_JUMPERLESS)`), 98% → 79% static:
- GraphicOverlays `MAX_GRAPHIC_OVERLAYS` 8→1 + render no-op (~9 KB).
- MicroPython API scratch buffers (`JumperlessMicroPythonAPI.cpp`) shrunk (~11 KB).
- `MpRemoteService` buffers (8K+4K+4K+512 → 1536+1K+1K+512); these `new[]` at
  static-init and were the FIRST abort.
- Current-sense overlay `kCurrentSenseMaxPathLength` 320→8; FatFS SafeStrings
  (`FileParsing.cpp`); CDC FIFOs 1024→256 (`custom_tusb_config.h`).
- FS partition 4 MB→1 MB (SPIFTL FTL map ~16 KB→~4 KB heap).
- `kTraceN` 1024→64 (Debugs, ~9 KB); `uartReceived` ring 8 K→2 K (AsyncPassthrough).
- OG MicroPython heap sized to 16 KB (lazy-init, doesn't affect boot).

### Session 2026-06-20 — OG BOOTS, MicroPython + native module CONFIRMED WORKING
Three fixes this session took the OG from a boot/connect crash-loop to a stable
board that runs MicroPython end to end (verified over SWD + serial):
1. **LED buffer overflow → heap corruption (the boot crash).** `setup()` calls
   `drawAnimatedImage(0)` → `drawImage(44)` which emits V5 pixel indices (up to
   ~445) into the breadboard NeoPixel buffer. On OG that buffer is only
   `LED_COUNT`=111 px (333 B), so the RAM-resident fast path
   (`setPixelColorRamHelper`, a raw `pixels[n*3]` write with NO bounds check) ran
   ~1 KB past the buffer and zeroed the heap singleton right after it — caught via
   a hardware watchpoint: the `MeasureMode` singleton's vtable went `0x10152454`
   → `0` between main.cpp:330 and :415, then `registerService(&measureModeService)`
   virtual-called through the null vtable → hard fault. FIX (`LEDs.cpp`): added
   `ledMaxPixels()` and bounds-check all three setters
   (`setPixelColor` x2 + `setPixelColorDirect`); also fixed `clear()` to size from
   `bbleds.numPixels()` (was `LED_COUNT+LED_COUNT_TOP`, a 3-B overrun on OG). This
   is the trust-boundary guard that makes ANY shared V5 graphics path safe on the
   smaller OG strip — no need to special-case every drawing routine.
2. **Provisioning OOM.** `jumperless.py` (36 KB) + `jumperless.pyi` (45 KB) can't
   be written on the tiny OG heap (`writeStringToFile` needs content+2 KB C-heap).
   Skipped both on OG (`micropythonExamples.h`: `INCLUDE_JUMPERLESS_MODULE/STUB`
   gated off) — the native C `jumperless` module already satisfies
   `import jumperless`; the .py is only an autocomplete re-export, the .pyi is
   IDE-only stubs. Also skipped `rp2.py` provisioning on OG (`Python_Proper.cpp`,
   ~10 KB C-heap spike; PIO @asm_pio not needed for the minimal goal).
3. **MicroPython MemoryError (`allocating 4168 bytes`, repeated).** Measured at
   runtime: ~30 KB total for (MP GC heap + C runtime heap). Registering the native
   module eats ~12 KB of the GC heap, so the old 16 KB heap left <4 KB and every
   init/exec script OOM'd. 24 KB fixed the OOM but starved the C heap (~6.5 KB) →
   main-loop reboot-loop. **20 KB is the sweet spot** (`JumperlessDefines.h`):
   ~8 KB free in the GC heap, ~10.5 KB C heap. No OOM, no reboot.

VERIFIED on hardware (SWD flash + pyserial REPL drive): boots stably (0 USB drops
over 70 s, free-running), no provisioning errors, no MemoryError, and a held REPL
session runs `import jumperless` → `print('IMPORT_OK')` →
`jumperless.adc_get(0)` returning `0.86` (a real voltage) → `dir(jumperless)`
listing the native DAC fns. **The primary deliverable (LLM/MicroPython control of
the OG) works.** The periodic reset cycle that initially masked this was the
core1 encoder-PIO poll — see "RESOLVED" below. Also quieted the OG MeasureMode
flood (3) and gated ADC channels 5-7 off on OG (4).
3. **MeasureMode flood** (`MeasureMode.cpp`): service() no-ops on OG (no probe-pad
   ADC / connect-measure switch wired in; the scanning probe is Phase 2). Was
   spamming the "row A1" voltage line every loop and poking the uninitialized OG
   OLED I2C each update.
4. **ADC channels 5-7** (`Peripherals.cpp updateLazyAdcReadings`): the slow loop
   read ADC ch 5-7 (V5 has 8 ADC inputs), but the RP2040 ADC only has inputs 0-4.
   Gated the slow loop off on OG (its 4 ADCs are covered by the fast 0-4 loop).

### ✅ RESOLVED: periodic ~6 s reset cycle was core1 polling a dead encoder PIO
SYMPTOM: after boot the board ran a few seconds then reset (USB dropped ~every
5-6 s; sometimes recovered, sometimes core1 hit a Cortex-M LOCKUP with halted
PC = `0xFFFFFFFE` and died until reflash). It was NOT caught by
`break isr_hardfault`/`abort`/`panic` (a lockup escalates past the handler), and
`monitor reset halt` always landed core0 in boot code (`data_cpy_loop`/`setup`),
confirming a reset cycle. Bisecting core1 work (`core2stuff`, main.cpp) found it:
ROOT CAUSE = **`rotaryEncoderStuff()` on core1** reading
`quadrature_encoder_get_count(pioEnc, smEnc)` on a PIO state machine that was
never loaded on the OG — OG has no encoder AND the RP2040's 2 PIO blocks are
oversubscribed (boot logs "probe button PIO: no instruction memory"), so the
quadrature program failed to load. Polling that dead/invalid SM stalled/faulted
core1 → core0 stalled → reset. FIX: `rotaryEncoderStuff()` early-returns on OG
(`RotaryEncoder.cpp`). After the fix: **0 drops over 70 s**, and a full MP REPL
session held without interruption.
- Bisection steps applied along the way (all correct OG changes, kept): gate
  MeasureMode (`MeasureMode.cpp`), `updateLazyAdcReadings` (`Peripherals.cpp`,
  OLED-only cache + no ADC ch 5-7 on RP2040), and the V5 boot animation
  (`drawAnimatedImage`, main.cpp) off on OG. None were the cause, but they
  removed flooding / invalid reads and sped boot.
- FOLLOW-UP (not blocking): the OG still oversubscribes PIO (encoder + probe
  button + WS2812 strips on 2 PIO blocks). Now that the encoder is off, audit PIO
  allocation so the WS2812 LED program is guaranteed a slot. `core1_stack` is only
  2 KB; fine now but watch it if core1 work grows.

### ✅ RESOLVED: OG LEDs completely unlit was initGPIO() clobbering the LED pin
SYMPTOM: the OG's WS2812 strip was completely dark; the GPIO status dump showed
pin 25 (the OG LED data line, `LED_PIN 25`) in SIO mode with free PIO SMs
available. ROOT CAUSE: `initGPIO()`/`setGPIO()` (Peripherals.cpp) iterate the
**V5** routable-GPIO bank (pins 20-27 = RP_GPIO_1..8). On the OG that map is wrong
- pin 25 is the LED strip and 26-29 are the ADC inputs. `initLEDs()` (core1)
claims pin 25 for the WS2812 PIO, but `initDAC()->initGPIO()` (core0, after
`configLoaded=1` releases core1) calls `gpio_init(25)` which re-muxes the pad back
to SIO, winning the cross-core race and killing the strip. FIX: both `initGPIO()`
and `setGPIO()` early-return on `#if defined(OG_JUMPERLESS)` (the OG's only
routable GPIO - RP_GPIO_0 + UART - are owned by their own subsystems). V5
byte-identical (the guard compiles out). Also fixed a `platformio.ini` parse error
(tab-indented `upload_port`/`monitor_port` were folded into `extra_scripts`).
ponytail: Phase 2 should make initGPIO/setGPIO iterate `board::currentBoard().gpio`
instead of the hard-coded V5 pin bank. **NOT yet verified on hardware** - flash
and confirm the strip lights.
- Workflow gotchas learned: do NOT `monitor halt` a *running* OG for register
  reads — halting mid-flash-write/lockup double-faults a core and USB never comes
  back (recover with `program ... verify reset exit`, or `program ... reset exit`
  if verify times out on an unstable target). A USB re-enum drops the SWD
  multidrop link mid-session. And `grep`-no-match in a piped shell command here
  can swallow the whole line's output — redirect to a file and read it instead.

NOTE on SWD while running: `monitor halt`/register reads on a *running* OG can
double-fault a core and break USB (TinyUSB starves) — recover with a clean
`program ... verify reset exit`. For crash autopsy use breakpoints
(`break abort` / `break isr_hardfault`) + `continue`, not halt-polling; and note
a USB re-enum (e.g. triggered by opening a port) can drop the SWD multidrop link.

REMAINING for Phase 1:
- [x] **DTR-on-connect crash** — was NOT a true blocker (see session note above).
      A single connect is survivable; the prior "hard-fault on DTR" was the LED
      heap corruption (now fixed) plus rapid reopen thrashing in the test harness.
- [x] **More heap for MicroPython** — MP heap 16 KB → 20 KB; OOM gone, MP usable.
      Further headroom still requires reclaiming static `.bss`; best remaining
      targets (from the SRAM map): `rowAnimations` (4.8 K), inflate `window` (4 K)
      + `frame_buf` (2.6 K) [gate the boot animation/`drawAnimatedImage` off on OG
      first], `logoColorsAll` (3.4 K), Undo `toastScreen` (3.2 K), and ultimately
      `globalState` (~30 K). Each byte cut lowers `&end` and grows the heap 1:1,
      so the MP heap could then be pushed back toward 24–32 KB.
- [x] **Unified router wiring:** replace the hard-coded `BOUNCE_NODE` chip-hop
      sites in `NetsToChipConnections.cpp` with `boardY0Node(currentBoard())` +
      the OG descriptor maps. Currently the OG runs the V5 bounce-node router on
      OG topology data — routing correctness on OG is UNVERIFIED. Keep V5 on its
      `ch[]` path until Phase 3.
- [x] **LED HAL (display correctness):** the buffer-overflow/heap-corruption bug
      is FIXED (bounds-checked setters, see session note) so the V5 framebuffer no
      longer crashes the OG. Net rendering uses `nodesToPixelMap[node]` directly
      (1 px/row). Whether nets/animations *look right* on the 111-px strip is a
      VISUAL check the user must make. Still TODO: OG renderer should sample the
      center pixel of each V5 row (`Graphics.cpp rowColumnToPixelIndex(row, 2)`;
      rows 31–60 mirror columns) for animations/highlights. `caps.ledsPerRow==1`.
- [x] **MicroPython smoke test** — DONE. `import jumperless` (native module) +
      `jumperless.gpio_get(1)` → `FLOATING` over the JLOGport1 REPL.

How to flash + debug the OG (workflow established this session):
- Build: `~/.platformio/penv/bin/pio run -e jumperless_og` (NOT the homebrew
  `pio` — its Python 3.14 is rejected by the platform; the penv has 3.11).
- Flash over SWD (no BOOTSEL): `openocd -s <scripts> -c "adapter driver cmsis-dap;
  adapter speed 4000" -f target/rp2040.cfg -c "program .pio/build/jumperless_og/firmware.elf verify reset exit"`.
- Crash backtrace: run openocd as a gdb server, then `arm-none-eabi-gdb -batch`
  with `target extended-remote 127.0.0.1:3333; monitor reset halt; break abort;
  continue; bt`. (A Raspberry Pi Debug Probe `2e8a:000c` is wired to the OG SWD pads.)
- Gotchas: don't `pkill -f openocd` (matches & kills your own shell); zsh aborts
  the line on a no-match glob (use `ls /dev/ | grep usbmodem`). After any
  `reset run` give the board ~12 s before the next SWD attach: attaching while
  it is still booting / re-enumerating left both cores halted with a garbage
  SP once (2026-09-08, "Failed to read memory at 0xffffffe0", USB gone) -
  recovery is `program firmware.elf verify reset exit`.

### Session 2026-06-24 — static-RAM reclaim + grouped feature flags + caps-gated scheduler
Reclaimed ~16.6 KB of OG static RAM (69.9% -> **63.6%**, 166,660 B) so the 20 KB
MicroPython heap malloc has a contiguous block again. Introduced a clean
**two-tier gating model** (documented in `JumperlessDefines.h`):
- **Compile-time feature-group flags** (the only thing that frees `.bss`): one
  flag per subsystem, header provides no-op stubs when off so call sites are
  untouched. First group: `UNDO_ENABLED` (0 on OG). `Undo.cpp`'s whole body is
  now `#if UNDO_ENABLED` with an `#else` stub block for the entire `Undo.h` API +
  `undo_debug`/`g_undoApplying`. This drops the ~3.2 KB static `OledScreen
  toastScreen` and the dead code, replacing the old runtime `undoInit`
  early-return. Undo + `undoToast` gate as ONE unit (not piecemeal).
- **Runtime `BoardCaps`** (correctness / contract, does NOT free `.bss`): added
  `hasStartupAnimation`; `main.cpp` now gates the probe stack on `hasProbePads`,
  `fileCacheFlushService` on `hasPsram`, `initRotaryEncoder()` on
  `hasRotaryEncoder` (real win: stops OG claiming the quadrature PIO slot), and
  the boot `drawAnimatedImage()` on `hasStartupAnimation` - all replacing OG
  `#ifdef`s. V5 caps are all `true` so V5 is byte-identical (verified: V5 builds,
  RAM 49.1%).
Individual cuts: `-DJL_USE_COMPRESSED_STARTUP_FRAMES=0` on OG (drops the 4 KB
inflate window + 2.6 KB frame_buf, ~6.6 KB; uncompressed frames -> flash);
deleted dead `newBridges` (~3.8 KB, both boards, also removed its Debugs.cpp
RAM-map entry); `rowAnimations[50]->[40]` on OG via shared `ROW_ANIMATION_COUNT`
(~1 KB); OG CDC FIFOs 256->128 (~1 KB). `s_uart_response_queue` left unchanged
(per request). **OLED preserved on OG** (a user can wire an SSD1306 to
GP16=GND/17=Vcc/18=SCL/19=SDA): OLED services stay registered (inert without a
panel); actually bringing up an OG panel on GP18/19 (config defaults + the
firstLoop `#if !OG`) is a deferred follow-up. Host board test green (OG + V5).
**NOT yet verified on hardware** - needs a BOOTSEL/SWD reflash + REPL smoke test
(`import jumperless`, confirm no "FATAL: failed to malloc 20 KB heap").

### Session 2026-09-07 — the scanning probe lands (uncommitted on dev until Kevin's hands-on pass)

> **The session-open reboot is fixed (2026-09-08, next section):** it was the
> flash bootloader, not the probe. The bisect and the SWD recipes stay in
> **`CodeDocs/HANDOFF_2026-09-08_OG_PROBE_REBOOT.md`**.

**What the OG probe is.** A needle in the GPIO 19 hole and a button that shorts
that line to GPIO 18 (`Probe_Guide.md`); no pads, no switch, no addressable LED
(the kit LED sits between the two lines). Until now the OG build had
`PROBE_PIN 10` / `BUTTON_PIN 9` (the V5 values = the OG's chip selects E and D)
and `ADC0_PIN 40` (RP2350 numbering); both are OG-conditional now (19/18 and
26..29 in `JumperlessDefines.h`).

**How it finds the row** (the reference firmware's algorithm, topology-driven):
the needle carries a 25 kHz tone; every breadboard row, corner row and header
pin is routed in turn through the crossbar to the RP2040's ADC0/ADC1 pin (read
digitally, behind the OG's LM324 buffers) and the one that follows the tone -
four samples a quarter period after the falling edge read 0,1,0,1 - is the
touch. A non-touched node reads 1111 (`s r <node>` shows the raw patterns).
The crossbar has to be empty for this, so a sweep resets the chips - the
original OG probe-mode behaviour; between sweeps the board stays reset, and
`Probing::probeExitTail()` restores the circuit with a forced clean
`refreshLocalConnections(1,1,1)` (the OG's RouteSafety is stubbed, so the
suspect-shadow upgrade a V5 would get does not exist here). A sweep is chunked
into 13 groups (one chip's 7 rows, the 4 corners, half a header chip) so one
probe tick stays ~1 ms; a full sweep measured **9.1-9.4 ms** on the bench.
`sweepBegin()` waits for core 1 (`waitCore2`) and a sweep restarts itself if
`routingGeneration` moves underneath it (a connection landed mid-sweep).

**The button** is read by coupling (10 kHz tone on the needle shows up on GPIO
18 under BOTH pulls - the kit LED couples one way only) and decoded by hold
length in `ProbeButton::scanProbeButtonService()`: short press = the current
mode's own button (idle: connect, in a session: exit / cancel a half-made
pick), long press (>= 750 ms) = the other button (idle: clear, in a session:
toggle connect <-> clear). That is the original OG gesture set ("long press =
connect / clear, short press = commit") laid over the V5 session, which
already treats the same button as exit and the other as a mode switch. One
event per physical press; 12 ms sampling (~360 us per sample), 2-sample
debounce. `processSample()` (two buttons, PIO sampler, double-tap undo) is
bypassed on this board.

**Wiring into the stack** (all runtime-gated on `caps.scanningProbe` /
`caps.hasProbePads`, no new board macro): `main.cpp` registers `probeButton` +
`probing` for either probe kind, the pad-only services (highlighting, measure
mode, switch classifier, pad reader) stay V5; `Probing::service()` skips the
pad read at idle (a sweep would empty the crossbar); `Probing::readProbe()`
dispatches to `Probing::scanProbeRead()`; `probeLEDhandler()` and
`classifySwitchPosition()` return early without pads (the JeoPixel was never
begun on the OG, and the classifier read a stub INA). The tip pre-read (needle
as input, pull-up vs pull-down) reports a hard level as GND / SUPPLY_3V3 and
never drives the tone into a rail (the reference did).

**Diagnostic:** `s` (OG-only, debug menu) = tip level, button, one full sweep
with timing, then a clean refresh; `s b` watches the decoder for 5 s;
`s r <node>` prints the raw tone patterns for one node.

**Bench (serial-verified, nothing touching the board):** tip floating, button
released, 5 sweeps 9101-9365 us with 0 nodes; `c` (crossbar dump) identical
before and after a sweep with a live `+ 1-2` connection = restore works.
**Not yet verified (needs hands):** a real touch -> row on the terminal, two
touches -> a connection, the button decode (short/long) entering and toggling
probe mode, the kit LED lighting in a session, header-pin and corner-row
touches, a touch on a rail reading GND / 3.3 V.

**Two things found on the way, both pre-existing:**
1. **Two USB loads ended with stale flash.** Twice, `picotool load -x`
   (once via the 1200-baud touch, once from a button BOOTSEL) reported success
   and the board then hard-faulted before `setup()`: SWD showed sectors
   0x10003000..0x10056fff (the first 84 sectors of `.text`) holding an
   older build while everything after matched the uf2, so crt0's literal pool
   sent `runtime_init` into `__retarget_lock_acquire(NULL)`. What was NOT
   isolated: whether the write was incomplete or something rewrote those
   sectors on the first boot. The discriminator is cheap - `picotool load`
   without `-x`, dump over SWD while still in BOOTSEL, diff. The OTA stub is
   not the culprit (it mounts LittleFS; the OG's FS is FatFS behind the SPIFTL
   translation layer - the first blocks of the region read 0xFF over SWD, but
   `/` lists projects, python_scripts and slots, so the FS is fine). Programming the ELF
   over the debug probe (`openocd ... program firmware.elf verify; reset run`,
   `target/rp2040.cfg`) verified and booted first time. Until this is
   understood, flash the OG over SWD or verify after a USB load
   (`picotool verify`). Note openocd probes this flash as 32 MB.
2. **MicroPython is disabled on the OG build** at boot: `mpAllocHeap` finds
   40248 bytes of C heap (`X`: "Free: 39 KB") against a 40960-byte need (16 KB
   rung + 24 KB reserve) - 712 bytes short, on the build that was on the board
   before this session too (this session adds 376 B of static RAM). Also
   "Not enough memory to write file (54KB needed)" when `/` tries to create
   `jumperless.pyi`. MpRemoteService retried the heap every pass, printing the
   FATAL line ~200 times a second on port 1 - the terminal was unusable. The
   line now prints once (`Python_Proper.cpp`); the heap shortfall itself is
   untouched and is the OG's primary-deliverable regression to chase next (the
   static-RAM reclaim list in this doc is where the kilobyte comes from).
3. Smaller, seen in `X`: the GPIO table still labels 9/10 as PROBE_BUTTON /
   PROBE_PROBE and knows nothing about 18/19 (a V5 label table), and the OLED
   block reports its default "crossbar" pins as GPIO 26/27 - on the OG those
   are the ADC0/ADC1 pins the sweep (and every ADC read) uses. The OLED is
   inert here today; if an OG panel is ever brought up it must not land on
   26/27 or 18/19.

**Follow-ups (design calls for Kevin):** a positive rail reads as 3.3 V with no
way to pick 5 V (the reference asked with short/long press); the needle on a
row that is pulled to a rail through a resistor classifies as that rail (as
before); when the needle's net spans several holes the lowest is reported
(`debug.probing` prints the rest); a ±8 V rail on the needle over-drives GPIO
19 (hardware, unchanged); a terminal way into probe mode for OG users with no
button; the OLED-on-GP18/19 idea in this doc conflicts with the probe pins;
the HIL harness knows only `JLV5port*` (the OG enumerates as `JLOGport1/3/5/7`
= terminal / UART passthrough / MicroPython REPL / TUI).

### Session 2026-09-08 — the probe-session reboot was the flash bootloader

**Symptom:** opening a probe session (button press, or the press written
into `ProbeButton` over SWD) HardFaulted core 1 at a random PC 0–11.5 s in;
eight of eight runs on the previous build. Bare sweeps from the terminal
never faulted.

**What the debugger showed** (vector catch on both cores, Tcl scripts in the
handoff): three faults at plain instructions that touch no memory (`lsrs`,
`blx r3`, `beq.n`) and one whose stacked frame held PC `0x41000200` with the
Thumb bit clear - a function pointer loaded from a flash literal pool that
read back as garbage. Hardware breakpoints on `__wrap_flash_range_erase` /
`__wrap_flash_range_program` never fired before a fault, the SSI/XIP
registers read normal at the fault, `ch446q_timeout_count` stayed 0, and
core 1's SP sat 0x48–0x120 below the top of its 8 KB block (no overflow).
So: not a flash write, not the crosspoint ISR, not the stack - the flash
itself was returning bad data.

**Root cause:** `boards/jumperless_og.json` named no second-stage
bootloader, so the platform linked its fallback `boot2_generic_03h_2`:
single-bit SPI, the `03h` Read Data command, CLKDIV 2 = **66.5 MHz** at the
133 MHz sys clock. The W25Q128's `03h` read is rated to 50 MHz. Reads were
marginal, and they failed exactly when core 1 fetched hard (LED rendering in
a session) with ~200 crosspoint switches per sweep adding noise - core 0
runs the sweep mostly from RAM, so core 1 took every fault. Live registers:
`SSI_CTRLR0=0x001f0300` (standard frame format), `SSI_BAUDR=2`. The
reference OG firmware (`board = pico`, its `pico.json`) and the framework's
own `jumperless_v1` variant both use a quad Winbond boot2.

**Fix:** `build.arduino.earlephilhower.boot2_source =
boot2_w25q080_2_padded_checksum.S` in `boards/jumperless_og.json` (quad
I/O `EBh` at CLKDIV 2, the reference's choice; the framework's variant uses
`w25q128jvxq_4`, 33 MHz, if this ever needs to be more conservative). The
`.boot2` section of the ELF changes, nothing else does. XIP is also faster
now (4 bits per clock instead of 1).

**Verified (serial + SWD, no hands):** 3 x 20 s and 2 x 60 s injected
sessions, `probeActive=1`, sweeps completing at 11.5–12.6 ms (core 1 busy),
zero faults; `?` and `s` normal after the reflash. The `g_debugMask` bisect
scaffolding is removed. V5 builds unchanged (its board json was not touched).
**Still needs Kevin's hands:** everything in the 2026-09-07 list above (a real
touch, two touches, the short/long press gestures, the kit LED).

**Commit these together (the boot2 fix, separable from the probe work):**
`boards/jumperless_og.json`, this section of `CodeDocs/OG_BACKPORT.md`, and
the RESOLVED banner in `CodeDocs/HANDOFF_2026-09-08_OG_PROBE_REBOOT.md`.
The probe files (`src/sensing/ScanProbe.*`, `src/Probing.*`, `src/main.cpp`,
`src/JumperlessDefines.h`, `src/SingleCharCommands.*`,
`src/snakes/Python_Proper.cpp`) are the 2026-09-07 session's commit. Never
stage `.pio/build/jumperless_v5/firmware.uf2` outside a release.

**Worth re-testing now:** the two `picotool load -x` USB flashes that "left
stale sectors" (2026-09-07 finding 1) were diagnosed by reading flash back
over SWD - through the same marginal XIP path. That verdict may have been a
read error, not a write error.

**OG flash notes:** openocd's probe reports the part as 32 MB
(`RP2040 Flash Probe: 33554432 bytes`); the board json still says 16 MB.
Whichever it is, the quad boot2 works on both W25Q128 and W25Q256 (3-byte
addressing covers the first 16 MB).

### Session 2026-09-08 (afternoon) — parity batch: MicroPython back, DAC, UART, INA, ADC scaling

All runtime-gated on `board::currentBoard()` (caps / descriptor tables), V5
builds unchanged in behaviour (its `pinNames` table moved into the descriptor
as `kV5GpioNames`, 192 B of .data to rodata). Host board test green. Bench:
serial + MicroPython REPL + SWD, no hands.

**Contract additions (`board.h`):** `BoardCaps::uartTxPin/uartRxPin` (V5 0/1,
OG 16/17), `BoardCaps::mpCHeapReserveKb` (V5 24, OG 12),
`BoardTopology::gpioNames/gpioNameCount` + `boardGpioName()`; all in
`boardCapabilitiesJson` (`uart_tx_pin`, `uart_rx_pin`,
`mp_c_heap_reserve_kb`) with host-test assertions.

**MicroPython is back on the OG.** `mpAllocHeap` takes its C-heap reserve
from the board: with 12 KB the ladder lands on a **24 KB GC heap** (the 28 KB
configured rung needs 40960 B), leaving ~15.6 KB of C heap. Verified on the
REPL (port 5): `import jumperless` -> `adc_get`, `gpio_get`, `os.listdir('/')`,
`gc.mem_free()` = 15632 after import; config saves (`:`) and slot autosaves
(`+`/`-`) ran with the heap allocated, `X` free heap 15.3 KB, no abort over a
140 s session. It is a small Python: a 4 KB bytearray plus a 300-string join
raises MemoryError. If a C-heap abort ever shows up in a file path, raise the
OG reserve to 14 (the ladder then still gives 24 KB) before shrinking anything.

**UART passthrough pins.** `AsyncPassthrough` muxed GPIO 0/1 to UART0 on
every board; on the OG those are the routable `RP_GPIO_0` node (0, via R7 to
chip L) and the **MCP4822's SPI chip-select (1)**. The Nano-header UART0 is on
GPIO 16 (TX) / 17 (RX) - the PCB netlist and the reference's
`Serial1.setTX(16)/setRX(17)` agree. The pins now come from the descriptor,
and the MicroPython port's `machine.UART(0)` defaults are set to 16/17 for the
OG env (`-DMICROPY_HW_UART0_TX/RX/CTS/RTS`), so Python cannot re-mux the DAC
CS either.

**Both INA219s.** The rev 3.1 PCB carries two (bus scan: 0x40, 0x41): 0x40
across the crossbar's CURR_SENSE lanes, 0x41 across the DAC output path. The
OG init brought up only INA0; both are initialised now and the OG-only zero
stubs in `vi1` and `jumperless.ina_get_*(1)` are gone.

**MCP4822 DAC backend** (`caps.spiDac`, Peripherals.cpp): pico-sdk
`spi_init(spi0, 8 MHz)`, 16-bit frames, only SCK (2) and MOSI (3) muxed to
SPI - GPIO 0 (SPI0 RX) stays the routable node - CS (1) as SIO. 2x gain,
LDAC is tied to GND on the PCB. Rails are a hardware switch: `setTopRail` /
`setBotRail` keep only the bookkeeping when `!railsFirmwareControlled`.
Scaling measured through the crossbar into the ADCs (`+ DAC0-ADC0`,
`+ DAC1-ADC3`), with the +5 V supply as the reference (it reads 4.64 V on
ADC0 and 4.77 V on ADC3 with the formulas below - a USB rail behind a diode):

| requested | DAC0 out (unity from 4.096 V FS) | DAC1 out (16 V / 4096 codes) |
|---|---|---|
| 0 V | 0.05 V | +1.08 V at code 2048 -> zero moved to code 1772 |
| 2.5 V | 2.10 V with the reference's V*4095/5 -> now code = V*4095/4.096 | |
| +4 / -4 V | | +5.1 / -3.1 V around the old zero; symmetric around 1772 |
| +8 / -8 V | 4.17 V (full scale) | saturates at +7.0 V; code 0 = -6.9 V |

Re-verified after the fix (`jumperless.dac_set` -> `adc_get` through the
crossbar): DAC0 0 / 1 / 2.5 / 4 / 4.096 V read 0.05 / 1.03 / 2.56 / 4.06 /
4.17 V; DAC1 -6 / -4 / 0 / 4 / 6.5 V read -6.17 / -4.15 / -0.10 / 4.01 /
6.47 V. Idle (floating) inputs now read 4.99 V on ADC0-2 and 7.0 V on ADC3 -
the buffers sit high with nothing connected.

So the OG's DAC0 is **0-4.096 V** and DAC1 **-6.9..+7.0 V**; the descriptor
ranges say so, and `initDAC()` sets `dacSpread/dacZero` = {4.096, 0} and
{16, 1772} so the shared `V*4095/spread + zero` formula produces the codes
(a future `$` calibration lands in the same arrays). The reference firmware's
nominal V*4095/5 and +2048 were 18 % low on DAC0 and +1.1 V off on DAC1.

**ADC scaling from the descriptor.** `readAdcVoltage` used the V5's static
`adcSpread/adcZero` (18.28 / 8.0) on the OG, reading a floating buffered
input as 9.2 V. `initADC()`'s OG branch now copies the descriptor's ranges
into those arrays: ADC0-2 0-5 V, ADC3 -8.1..+8.24 (the reference's 16/4010
and -8.1). `jumperless.adc_get()` follows.

**X panel:** the pin table is the board's (`kOgGpioNames`, 30 rows: 0 GPIO_0,
1-3 DAC_CS/SCK/MOSI, 16/17 UART_TX/RX, 18/19 PROBE_BUTTON/PROBE_PROBE, 24
CH_RESET, 25 LED_BB, 26-29 ADC_0-3); the V5 output is unchanged.

**First hands-on finding (Kevin, 09:57): "nothing shows until the second
connection."** `printGraphicsRow()` - the primitive under every
`b.printRawRow()` / `b.lightUpNode()` - returns immediately on a
one-LED-per-row board, so the session's first-node latch, its three flash
frames and the delete fades painted nothing on the OG; only a finished
connection showed (through `showNets()`, which maps rows itself).
`bread::printRawRow` / `bread::lightUpNode` now paint the row's single pixel
from `nodesToPixelMap` on `ledsPerRow == 1` (any lit column lights it, the
`0xFFFFFE` bg keeps it transparent), while `printGraphicsRow` stays a no-op
there - glyph text has no meaning on one pixel. Add `src/Graphics.cpp` to the
parity commit. **Bench-verified by Kevin: pending.**

**Seen on the way, not fixed (Kevin's calls / follow-ups):**
- `RP_UART_TX` / `RP_UART_RX` node perspective: the OG's nets are named from
  the Nano's side, so `kOgGpio` maps `RP_UART_TX` to GPIO 17 = the RP2040's
  RX; the V5 names the same node from the RP's side (GPIO 0 = TX). Changing
  it is a routing-semantics change - left alone, flagged.
- `TOP_RAIL` / `BOTTOM_RAIL` are "Invalid node" on the OG (its rail nodes are
  `TOP_1/TOP_30/BOTTOM_1/BOTTOM_30`); an LLM tool using the V5 names fails.
- `+ GND-ADC3` reports no path (GND cannot reach chip L's ADC3 lane on the
  OG router), and `+ GND-ADC2` read full scale on ADC2 - unverified whether
  the crosspoint or the ADC2 buffer; ADC0/ADC1/ADC3 behave.
- `$` (calibrate DACs) on the OG would overwrite the spiDac
  `dacSpread/dacZero` with V5-style values - gate it on `!spiDac` or make it
  OG-aware before anyone runs it there.
- `MICROPY_HW_UART0_CTS/RTS` default to 18/19, the probe pins; only muxed if
  a script asks for flow control, and there is no harmless choice on this
  pinout (the other option, 2/3, is the DAC bus).
- `loop1` still reads `readAdcVoltage(6, 4)` for `supplySense` on a part with
  four ADC inputs (pre-existing; gate on `adcCount`).
- `MICROPY_HEAP_SIZE` for the OG is 28 KB, which no longer fits with the
  12 KB reserve, so every boot prints "configured 28 KB doesn't fit"; set it
  to 24 KB so the first rung lands.
- Validation caveats: the slot-autosave write path ran with the GC heap
  allocated (two netlist changes); the config.txt write was only inferred
  (`:` marks config dirty but `saveConfig()` skips an unchanged file and the
  DAC voltages live in the slot). A real config change through `` ` `` with
  MicroPython up is the airtight test of the 12 KB reserve. The V5 `X` output
  identity is by inspection of the split printf, not a bench run (V5 was busy).
- The positive-rail 3.3/5 V ask, the OLED default on 26/27, the HIL
  harness's `JLOGport` discovery, and the flash part identity (openocd
  reports 32 MB without a JEDEC read; the json says 16 MB, which is where the
  FatFS partition and the 4 KB EEPROM emulation are placed - if the part is
  really 32 MB they sit mid-part, harmless but worth one `picotool info`).

**Commit these together (the parity batch):** `src/boards/board.{h,cpp}`,
`src/boards/v5/board_v5.cpp`, `src/boards/og/board_og.cpp`,
`test/test_boards/test_boards.cpp`, `src/Peripherals.cpp`,
`src/tubes/AsyncPassthrough.cpp`, `src/snakes/Python_Proper.cpp`,
`src/SingleCharCommands.cpp`, `src/JumperlessMicroPythonAPI.cpp`,
`platformio.ini` (the OG env's MicroPython UART defines).

### Session 2026-09-08 (evening) — rail ask, multi-row pick, exit clear (build 12)

Kevin, hands-on with the needle after the first-node LED fix landed: "we need
to get rail sensing working now ... The old Jumperless had a system where
tapping a rail would ask the user to select 5 or 3 V and use short probe
clicks to cycle and long clicks to confirm. ... make sure when we exit probing
with a single node lit, we clear it. ... disambiguation mode ... light up all
the sensed rows and then use short and long clicks to cycle through and
select them."

**The reference (Jumperless repo, tag 1.3.9, `JumperlessNano/src/Probing.cpp`):**
`voltageSelect()` asked ONCE per boot (`voltageChosen` never reset; the forum
how-to says "persists until power cycling"), lit rows 1-3 / 31-35 in the
voltage's color, short press cycled, long press selected.
`selectFromLastFound()` lit every found row pink with one brighter, short =
next, long = select, and dropped GND/3V3/5V from the list. Both were blocking
loops.

**What landed (all gated on `scanprobe::available()`, V5 paths untouched):**

- `scanProbeRead()` returns `kScanRailTouch` (-21) for a positive hard level
  instead of guessing `SUPPLY_3V3`; a low level is still `GND`. A sweep that
  finds several rows now hands the whole list back sorted in `connectedRows`
  (`connectedRowsIndex = n`), lowest as the read value. An empty sweep sets
  `Probing::scanLifted` - the "needle came off" signal both sub-states need,
  so the touch just answered cannot reopen them on the next tick (advisor).
- `Probing::scanSessionFilter(s, read)` runs right after `readProbe()` in the
  tick. Two sub-states in `ProbeSession`:
  - **ask** (`askOpen`): terminal `      3.3V   short press = 5V, long press
    = select` (one line, rewritten in place - the banner rewind counts lines),
    both positive rail strips painted amber `0x502800` (3.3 V) or red-orange
    `0x500a00` (5 V) via the new `ogRailsPaint()`. Short = flip, long =
    select: the supply node then goes through the normal latch/commit path
    as if the needle had read it, and the rails KEEP the color while the
    supply node is held (`railHeld`) - a supply node has no row LED, so
    without that "holding 5V" would show nothing (advisor). Asks on every
    positive-rail tap, preselecting the last answer (`s_scanRailChoice`, RAM,
    default 3.3 V) - the switch can be flipped any time and the firmware can't
    see it, so per-tap is the honest ask (the reference's once-per-boot is
    one line away if Kevin prefers it). A lifted needle landing on a row
    abandons the ask.
  - **pick** (`pickCount > 0`): rows painted `0x4000e8` (current) /
    `0x0a0020` (others) through `printRawRow`, previous pixel colors saved
    and put back on close; terminal `  [5] 12   short press = next, long
    press = select`. Short = next (wraps), long = select and latch. A lifted
    needle on a different net abandons the pick.
- Button kind: `ProbeButton::scanLastPressWasLong()` (set in the decoder's
  `post()`), read next to the -16/-18 code. The decoder maps short/long onto
  the mode's own/other button using `connectOrClearProbe`, which only the
  wrapper set - so a clear session entered by toggling read the opposite of
  one entered from idle. The session's two toggle handlers now keep it in
  sync (scan boards only; `LEDs.cpp:3194` reads it for the V5 logo, hence
  the gate). The resulting one-button gesture table (code-derived, bench
  check pending):

  | mode | short press | long press (750 ms) |
  |---|---|---|
  | idle | `connect` | `clear` |
  | `connect` | drop the held row / exit when nothing is held | `clear` |
  | `clear` | exit | `connect` |

- **Exit clear:** `probeExitTail()` on scan boards tears down an open ask /
  pick, restores the rails, and posts `requestLedShow( -1 )` after the
  `refreshLocalConnections( 1, 1, 1 )` (a plain 1 renders without clearing,
  and nothing repaints a raw-lit row on a 1-LED strip). The -18 toggle, the
  -16 drop, the -16 switch-to-connect, "can't connect" and the bridge-refused
  paths do the same on scan boards.
- `clearLEDsExceptRails()` on the OG now clears rows 0-59 and the header
  80-109 only (rails 60-79 and the logo 110 keep their color, as the name
  says); before, the whole strip went dark on every clearing render.
- `LEDs.cpp`: `kOgRailPixels` / `ogRailOwnColor()` hoisted out of
  `showNets()` into `ogRailsPaint(positiveColor, onlyUnlit)`; showNets calls
  it with `(0, true)` (its only-unlit rule, unchanged, is what lets a
  session's rail paint survive the swirl-pass renders).

**Verified:** both targets build (OG RAM 67.6% / 177168 B; V5 319852 B),
flashed over SWD (`Verified OK`), ports back, `? -> 1.7.11.0`; a session
opened by SWD injection printed `connect nodes`, a terminal key ended it
(banner rewind), `n` answered after. That is the whole serial-side
verification: every needle path (rail, pick, exit with a lit row) is
**bench-pending, Kevin's hands**. The advisor's completion review caught one
bug before the reflash: the exit tail zeroed `askOpen` before calling
`scanRailsRelease()`, which early-returns on the flag, so an exit during an
open ask would have left the rails in the ask color until reboot (fixed,
build 13 flashed).

Watch for on the bench (pre-existing, from the 09-07 port, not this batch):
the 700 ms `doubleSelectTimeout` reset sets `s.row[1] = -2`, so a needle
HELD on a row (or the GND rail) past 700 ms can re-latch the same node as
node 2 and drop the pair. Tap, don't hold.

**Bench script for Kevin (build 12):**
1. Rail: `connect`, tap a `+` rail -> both `+` strips amber, terminal
   `3.3V ...`; short press -> red-orange `5V`; long press -> the rails stay
   red-orange (holding); tap row 10 -> `5V - 10 connected`, rails back to
   normal, `n` shows 10 on `5V`. Then the reverse order (row first, rail
   second). Then a `-` rail -> straight to `GND`.
2. Pick: a wire between rows 5 and 12, tap 5 -> both lit, one brighter,
   terminal `[5] 12 ...`; short press moves it; long press picks. Repeat with
   a resistor, and find out whether a 100 nF cap reads as two rows (at the
   25 kHz tone it is ~64 ohm, so it probably does). The dim color of the
   other rows (`0x0a0020`, then `paintSingleLedRow`'s -40 brightness scale)
   may be too dim to see: Kevin's eyes decide.
3. Exit: tap a row, then leave three ways (short press with nothing else
   held, a key in the terminal, the 80 s timeout) -> the row goes dark each
   time. Also toggle to `clear` with a row held.
4. Gestures: confirm the table above with `probe_button_trace` on (the
   clear-mode row is the one that changed).

**Known limits (documented on the docs page, not fixed):** a row tied to a
rail through a part (a pull-up, an LED to GND) reads as the rail, not the
row (`tipLevel()` sees a DC path). The pick shows at most 8 rows
(`kMaxFound`).

**Docs:** `Jumperless-docs/docs/10.5-og-jumperless.md` + a nav entry under 3D
Printable Stand (uncommitted, like the firmware): firmware download + BOOTSEL,
a V5/OG table, the one-button gestures, rails (switch not sensed, `3V3`/`5V`
are nodes, DACs as the adjustable alternative with the measured ranges), the
pick, clearing, measuring. Written against `WRITING_LIKE_KEVIN.md`'s
checklist; Kevin's pass wanted before it ships.

**Commit-together (parity batch + this):** `src/Probing.cpp`, `src/Probing.h`,
`src/LEDs.cpp`, `src/LEDs.h`, `src/Graphics.cpp`, this doc.

### Session 2026-09-08 (evening, 2) — why a press during the pick exited the session

Kevin, on the bench: "when I press a row shorted to another and press the
button, it just exits probing instead of letting me cycle through them and
select."

**The exit is by design, and the filter was the only thing in front of it.**
The `-18` / `-16` handlers in `probeTick()` both have their old `break`
commented out, so a press that neither mode-branch returns from falls through
to `s.done = true` (`Probing.cpp`, the "Committing paths!" block). On the V5
that is the documented "click Connect with nothing held to leave probe mode".
The OG inherits it through the one-button decoder, so ANY press the
scanning-probe filter does not intercept ends the session. Three ways a press
got past the filter, all found by measurement, not reading:

1. **The sweep flickers.** A single sweep does not report the same set twice
   running - one row of a shorted pair this pass, both the next, none the one
   after (the button decoder drives a tone on the needle every 12 ms and the
   session aborts sweeps around it). The old filter treated ONE empty sweep as
   "the needle lifted" and any non-idle read while lifted as "the needle moved",
   so the pick was torn down and rebuilt several times a second. Measured on
   the board with a deliberately flickering fake touch: `pick OPEN` /
   `pick ABANDON` alternating, the highlight resetting to index 0 every time,
   and a press landing in a closed window falling through to the exit.
2. **The deferred press bypassed the filter.** Every in-session press was
   stashed for `kWindowMs` (~420 ms) so a double-tap could cancel it, then
   re-queued straight into `s.row[0]` - the one path to the button handler
   that never calls `scanSessionFilter`.
3. **A press during a settling touch could never open the pick.** While the
   button is down `scanProbeRead()` aborts the sweep every tick, so a user who
   touches and presses without pausing never gets a completed multi-row sweep:
   the pick does not exist yet and the press exits.

**Fixed (all scanning-probe gated; the V5 paths, debug output included, are
untouched):**

- `scanLifted` (one empty sweep) became `scanEmptySweeps`, and "the needle is
  off" is now `kScanLiftSweeps` (4) consecutive empty sweeps. A level read
  (GND / a rail) resets the counter - it is something on the needle, not a lift.
- **The touch is settled before it is answered**, unioning every row seen over
  `kScanSettleSweeps` (5) sweeps and at least `kScanSettleMs` (45 ms). This is
  what the OG reference firmware did (`scanRows` three more times, keep the
  largest set) and it is what makes a shorted pair read as a pair rather than
  as whichever row a single sweep happened to catch. Raise it if a real pair
  still answers single.
- **The pick only closes on a real change**: a read that shares no node with
  the open pick AND a settled lift. Sweep flicker no longer touches it.
- **One node per touch** (`touchAnswered`): the needle reports its node once
  and says nothing more until it lifts or lands somewhere else. This also
  retires the pre-existing "held past 700 ms re-latches as node 2" trap.
- **A press while a touch is still settling belongs to that touch**: it opens
  the pick (or answers the single row) and is eaten. A press with nothing on
  the needle still exits - `touchSweeps` is 0 then, verified on the board.
- **No press deferral on scanning-probe boards.** The deferral exists so a
  double-tap can cancel click 1, and `scanProbeButtonService()` never runs
  `processSample()`, so `g_probeDoubleTapBail` cannot fire on the OG. It was
  pure latency (~420 ms per press) and the bypass in item 2.
- **A pressed button no longer reads as GND.** `tipLevel()` sees the needle
  shorted to the low-driven button line for the ~24-36 ms before the decoder
  debounces the press. That returned GND, which could latch - and in clear
  mode latching GND cleared GND's whole net. The low path now asks
  `scanprobe::buttonPressed()` first (~0.5 ms, rare path).
- A settled lift also drops an UNANSWERED touch, so two separate taps can no
  longer union into one spurious pick.

**How it was verified.** A temporary SWD-drivable fake-touch hook stood in for
the needle (removed before this landed; the traces behind `debugProbing`
stayed - the OG has no display and this is the only way to watch a probe
session). On the board, with `debugProbing = 1`:

| check | result |
|---|---|
| two-row touch, 2 short presses, 1 long | `pick OPEN n=2`, cycle 0-1-0, `pick select 5` |
| the same under a flickering sweep | pick opens once, no ABANDON churn, cycling sticks |
| plain tap, lift, second tap | `5 - 20   connected` |
| rail touch, long press | `ask OPEN`, `5V`, selected and held (`n12=1`) |
| press with nothing on the needle | `[EXIT] button fallthrough` - the exit gesture still works |
| long press in connect mode | `clear nodes` - mode switch, no exit |

Both targets build (OG RAM 67.6% / 177168 B; V5 319852 B), flashed over SWD,
`? -> 1.7.11.0`.

**What that table does NOT cover, stated plainly.** The last three fixes - the
press-during-settle guard, the lift-drops-an-unanswered-touch reset, and the
button-as-GND check - were written AFTER the fake-touch hook came out, so only
the first four rows above ran against the code that is on the board now. Of
the last three, only the negative case was re-run on the shipped build (a
press with nothing on the needle still exits, and a long press still switches
mode). Their positive paths are Kevin's to confirm. The rail ask's SHORT press
was never observed either - the injection raced the firmware's own consume and
only the long press landed; it is the same code shape as the pick's cycle,
which was observed six times.

**Known limit, not fixed.** While the button is physically down the needle is
shorted to the low-driven button line, so no sweep can see the row AT ALL
during a press. The press-during-settle guard only helps when at least one
sweep completed between the needle landing and the press debouncing (~24 ms).
A touch and a squeeze in one motion, with no daylight between them, still
reaches the exit gesture. Kevin's normal gesture almost certainly clears that
window; if it does not, the fix is to remember the last settled touch across
the press rather than requiring one in flight.

**Still needs Kevin's hands** beyond the above: every check used an injected
touch, so the real sweep's behaviour on a real shorted pair - whether 45 ms is
long enough to see both rows - is unverified. If a pair still answers with one
row, raise `kScanSettleMs` / `kScanSettleSweeps`.

**Bench hygiene.** The two-tap and rail tests made real bridges in Kevin's
active slot (`5-20`, then `5V-20`). Removed with `- 5V-20`; `b` afterwards
shows an empty bridge array and `numberOfPaths: 0`. Capture-before-you-touch
was skipped for these injected runs, which is why the cleanup had to be
reconstructed from `b` rather than from a snapshot - do the capture next time.

**Diagnostics left in.** `debugProbing` (nonzero) now prints every non-idle
filter read plus the pick / ask / exit events on a scanning-probe board. It is
off by default and V5 output is unchanged. It is how a repro gets handed over
without an SWD round trip - the OG has no display, so there is no other way to
watch a probe session.

**A multi-agent pass (14 agents) over the press path** produced the exit-site
confirmation and, adversarially verified, the three gaps above that the first
fix missed. Its ledger: the workflow journal under
`subagents/workflows/wf_b8255a92-7c7/`.

### Session 2026-09-08 (evening, 3) — the OG logo LED

Kevin: "let's make the logo LED on the OG jumperless cycle much slower with
less saturation. then in probe mode states, have it a fixed color instead of
the cycle."

The OG logo is ONE pixel (110). It used to be a sample of `LOGO_LED_START + 0`,
one of the eight LEDs the V5 swirl paints across a palette - so it inherited
the ring's ~3 s fully saturated rainbow, which on a single LED reads as a
blinking light rather than a swirl. In probe mode it sampled the cold / pink /
hot palettes the same way, so it CYCLED through shades of the mode colour
instead of holding one.

- `logoSwirlState` (new, `LEDs.cpp`): `logoSwirl()` now records which of its
  branches painted the logo - OTHER (menu ring, press animation, the undo /
  filesystem / measure indicators, an explicit override), IDLE, or one of the
  three probe states. Write-only on the V5; nothing there reads it.
- The OG overlay in `showNets()` paints pixel 110 itself from that state
  instead of sampling the ring. Idle is an HSV drift, hue `(millis()/100) &
  0xFF` at saturation 105 and value 130 - about 26 s a lap against the ring's
  3 s, and pastel rather than full rainbow. The constants sit together at the
  top of the block. OTHER still samples the ring, which is the only place the
  indicators exist.
- Probe states are fixed literals, in the OG reference firmware's own colour
  code (the forum how-to: pink connect, orange clear, blue disambiguate):
  `0x50002A` connect with nothing held, `0xB00060` holding a node, `0xA02800`
  clear, `0x0020B0` while a chooser is up.
- `probeChooserActive` (new, `Probing.cpp`): true while the multi-row pick or
  the rail ask is open, so "it is asking you something" gets its own colour.
  Set at pick open / close, ask open / select / release, session begin and the
  exit tail.

**Verified on the board, at the pixel.** Two ways to read the OG strip without
a camera, both worth keeping:

- `:leds` on the port-7 backchannel dumps every pixel as RGB hex
  (`Ser3Backchannel.cpp`). Read-only, does not disturb port 1 - but it does
  NOT answer during a probe session, because `probeMode()` pumps only CRITICAL
  services and the backchannel is not one.
- Over SWD, the JeoPixel buffer is the heap pointer at `bbleds + 0x40`
  (0x20034c18 in this build), 3 bytes per pixel in **GRB** order, so the logo
  is at `+ 110*3`. That works mid-session. Identify the pointer by matching a
  pixel that does not drift (109 = VIN, `c00010`) against a `:leds` dump.

| state | logoSwirlState | pixel 110 (RGB) | |
|---|---|---|---|
| idle | 1 | 0x784C82 -> 0x826E4C, drifting | pastel, max channel 130 / min 76 = 41% saturation |
| connect, nothing held | 2 | 0x50002A | exactly the literal |
| clear | 4 | 0xA02800 | exactly the literal |
| after exit | 1 | drifting again | |

Sampled every 2.5 s while idle, the hue advances ~70 degrees per 5 s: about
26 s a lap against the ring's 3.06 s (60 steps x 51 ms), so ~8x slower.

Both targets build (OG RAM 67.7% / 177364 B; V5 319900 B) and the OG is
flashed. **Not verified**: the holding state (3) and the chooser blue both
need a needle, and nothing was checked on a V5 beyond the build. Kevin's eyes
decide whether 26 s and saturation 105 are the right numbers - they are two
named constants at the top of the block.

**A five-dimension adversarial review (9 agents) found four real defects,
all now fixed and verified at the pixel.** (An earlier partial read of its
journal showed nothing standing; that was the verify stage still running.)

1. **The OG rails were painted once and then frozen.** The morning's
   `clearLEDsExceptRails` change stopped zeroing pixels 60-79, and showNets
   mirrors the rails with `ogRailsPaint(0, onlyUnlit=true)` - so after the
   first frame every rail pixel was non-zero and `ogRailOwnColor`'s sign test
   was unreachable. A rail set NEGATIVE kept its positive colour forever.
   Now `ogRailsPaint(0, probeActive != 0)`: only-unlit protects the probe
   session's rail paint, and outside a session the rails re-evaluate every
   frame. Nothing else writes 60-79 on this board (lightUpNet's node loop
   stops at NANO_A7), so the unconditional repaint is safe.
2. **The pick and the rail ask could never close on a lift.** `lifted` can
   only be true on a tick whose read is -1, because every other return path
   zeroes `scanEmptySweeps` first - so `if ( read == -1 || ... || !lifted )
   return -1;` always returned before the close below it, making that code
   dead. A chooser stayed up until a press or the end of the session, and the
   logo stayed blue with it. `!lifted` now gates the "leave it up" tests
   instead of sitting behind them.
3. **Every flash write repainted the logo with the old fast saturated
   rainbow.** On a one-LED board `LOGO_PALETTE_COUNT` is 1, so the undo /
   filesystem / measure indicator palettes all fold to the rainbow, and the
   overlay sampled the ring for them. Since a save follows every probe-session
   exit and holds `filesystemActiveUntil` for 4 s, the swirl Kevin asked to
   remove came back for four seconds at a time. `LOGO_SWIRL_UNDO`, `_FS` and
   `_OVERRIDE` were APPENDED to the enum (so the already-verified 1/2/3/4 keep
   their numbers) and given their own fixed OG colours.
4. **The probe colours were far dimmer than idle.** Connect sat at Rec.709
   Y=20 against idle's 81-125: opening a session read as the logo going out.
   All five colours now sit in one band (Y roughly 40-90) with the idle value
   dropped from 130 to 95. Still meant to be tuned by eye.
   A fifth, folded in: the OVERRIDE arm writes the requested colour straight
   through instead of through `scaleUpBrightness`, whose x12 multiply fires
   only when all three channels are under 0x90 - so 0x8F8F8F came out white
   and 0x909090 came out 1.8x dimmer.

**Verified on the board after the fixes**, reading the strip buffer over SWD:

| check | result |
|---|---|
| rail pixel 70 overwritten with white | back to `011b0b` within 400 ms |
| idle | max channel 95, min 55 - the v=95 s=105 pastel |
| connect, nothing held | `0xA00050` exactly |
| chooser (pick open) | `0x0030C8` exactly, state 2 |
| needle lifted with the pick open | back to connect - **the pick closes on a lift now** |
| long press selects from the pick | `0xF00080`, state 3 (holding) |
| `filesystemActiveUntil` driven forward | `0x502000` amber, state 6 - not the rainbow |
| `undoActivityUntil` driven forward | `0x504000` yellow, state 5 |

The indicator checks drove the two flags directly over SWD rather than
performing a real flash write, so nothing was saved to Kevin's slots. The
temporary fake-touch hook went back in for this pass and came out again.

What the review established beyond the bench:

- V5 renders byte-identically, checked at the object level rather than by
  reading: in the V5 ELF `logoSwirlState` is referenced from exactly ONE
  literal pool, inside `logoSwirl` itself, so no reader is linked; the
  `clearLEDsExceptRails` `#else` arm is byte-identical to the old body; and
  `ogRailsPaint` compiles to a 2-byte `bx lr`. Cost on V5 is 4 bytes of BSS
  plus 1 byte plus one literal-pool word.
- No stuck logo state. The `logoLedAccess` bail is the only exit that leaves
  `logoSwirlState` unassigned, every holder of that latch releases it, and the
  assignment sits after the take and before every early return - so a torn
  cross-core read resolves to OTHER (the sample path), never to a stale probe
  colour.
- No `probeChooserActive` leak: every open has a matching clear, and
  `pickCount` is only zeroed inside `scanPickClose`, so the pick cannot close
  behind the flag's back.

**Work-list item it surfaced (no wrong colour on any board that exists, so not
fixed here).** `ogRailsPaint()`'s CALLERS are gated on the runtime capability
`caps.scanningProbe`, but its BODY is gated on the compile-time
`OG_JUMPERLESS` macro. Those two gates are different in kind. A V6 - or an
OG-capability board built without the macro - would call a no-op stub and put
the 3.3 V / 5 V ask on screen with no rail colour behind it. The honest fix is
a rail-pixel map in the board descriptor rather than the hardcoded
`kOgRailPixels`, which is a design change, not a patch.

### Session 2026-09-08 (evening, 4) — two things Kevin hit with the needle

"we shouldn't need to hold the row poked to disambiguate, and also the logo
led stays yellow"

**1. The chooser now survives a lift.** Yesterday's review flagged the
pick's and the ask's close-on-lift as dead code; the fix made a lift close
them, which is backwards. You poke the row, the choices light up, and you take
the probe OFF the board to cycle and select - holding a needle steady on a
shorted row while clicking a button on the same needle is not a thing anyone
wants to do, and the OG reference firmware's `selectFromLastFound()` /
`voltageSelect()` were blocking loops that did not look at the needle at all.
So the `lifted` test is gone from both blocks: nothing on the needle, or the
needle back on the same net, leaves the chooser up; only a node that is NOT
part of it closes it, and that read is then handled as a fresh touch. (Which
also absorbs the sweep's flicker, the reason the test was there.)

**2. "The logo led stays yellow" was the autosave indicator.** Two causes,
both fixed OG-side:

- FileCache holds `filesystemActiveUntil` for **4 s** per flush so the cue is
  unmissable on the V5's 8-LED ring. On one LED that meant amber for four
  seconds after every connection the autosave picked up - effectively always,
  while probing. The OG overlay now shows it only while flash is ACTUALLY
  being written (`filesystemActive`, plus a 250 ms tail so it is perceptible).
  The 4 s window is shared V5 code and was not touched.
- Inside `logoSwirl` the undo and filesystem indicators OUTRANK the probe
  branch. That is right for a ring of 8 and wrong for the only status LED on
  the board: while a session is open, what the logo has to say is which mode
  you are in. The OG overlay now derives the probe colour from `probeActive` /
  `connectOrClearProbe` / `node1or2` / `probeChooserActive` directly, ahead of
  every indicator.

Note this was a pre-existing OG condition that the fixed logo colours merely
made visible: before, every indicator palette folded to the rainbow
(`LOGO_PALETTE_COUNT` is 1 on the OG), so a permanently-armed indicator looked
exactly like a normal swirl.

**Verified on the board** (pixel buffer over SWD, plus the port-1 trace):

| check | result |
|---|---|
| pick open, needle ON | blue `0x0030C8` |
| pick open, needle taken OFF | still blue, still open |
| short press with the needle off | `pick cycle -> 1` |
| long press with the needle off | `pick select 12`, logo to hold pink `0xF00080` |
| `filesystemActiveUntil` far ahead, `filesystemActive` false | idle drift, NOT amber |
| `filesystemActive` true | amber `0x502000` |
| session open while the fs flag is still set | connect pink - probe outranks it |

Both targets build (OG RAM 67.8% / 177608 B; V5 319932 B). The temporary
fake-touch hook went in and came out again; Kevin's three bridges (42-50,
6-28, 21-12) were on the board throughout and are untouched.

### Session 2026-09-08 (night) — idle saturation, and what the PCB does to the logo

Kevin: "add more saturation to the idle animation, the pcb adds a lot of
yellowish filter."

The OG logo LED shines UP THROUGH the board, and the PCB filters it yellowish
and eats a lot of the colour, so a value that looks right in the pixel buffer
reads washed out on the bench. `kOgLogoIdleSat` 105 -> 180: most of the way
back to the ring's full 255, still visibly softer. Nothing else changed - the
26 s lap, the value (95) and the fixed probe colours are untouched, so if it
now reads dimmer than before that is the saturation eating the white floor
(min channel drops from 55 to 27 while max stays 95) and the value is the knob
for it.

Worth carrying into any future OG colour work: **judging OG logo colours from
the buffer is unreliable.** Everything else on this board is a top-firing LED
under a diffuser; the logo is not.

**The pick now overrides a row's net colour.** Kevin: "disambiguation mode
should override the lit color of a node if it's already connected to another
net." It painted the rows with `printRawRow` and asked for a menu flush
(`requestLedShow( 2 )`), which does not run `showNets` - so the next swirl pass
DID run it, `lightUpNet` repainted every net row, and the highlight vanished
from exactly the rows worth disambiguating. The pick is now published to the
renderer (`probePickCount` / `probePickIndex` / `probePickNodes`) and painted
at the END of the OG overlay, after the nets, and `scanPickShow` asks for a
nets render instead of a flush (`scanPickClose` asks for a clearing one, since
a pick row that is in no net has nothing to repaint over it). Colours are the
OG reference's: all found rows pink, the current one much brighter - dim
`0x300010`, bright `0xF00068`, written straight to the pixel with no
brightness scaling.

Verified with a fake pick published over rows 6 and 28 (both in one of Kevin's
nets, colour `0x5A004B`): row 6 went `0xF00068` and row 28 `0x300010` while a
second net's rows were untouched; moving the index swapped which was bright;
withdrawing the pick put both rows back to `0x5A004B`.

**The same defect exists for the held-node highlight** (the first tap's latch
paints with `printRawRow` too), and it is NOT fixed - Kevin asked about
disambiguation. Tapping a row that is already in a net in connect mode will
show the latch flash and then lose the held indication to the next `showNets`.
Same remedy if he wants it.

#### Bench: the debug probe died, and MicroPython replaced it

Mid-session SWD stopped connecting entirely - `Failed to connect multidrop
rp2040.dap0` on every attempt, at 5000, 2000 and 1000 kHz, with and without
`reset halt`, while the CMSIS-DAP probe still enumerated. A flash attempt had
already half-run when it went: `picotool info` then reported **"Program
Information: none"**, i.e. the image was damaged. Recovered over USB with the
1200-baud touch on port 1 plus `picotool load` (NO `-x`) + `picotool verify` +
`picotool reboot` - the sequence this doc already recommends over `load -x`.
Check `picotool info` reports an RP2040 before writing: the V5 on the same
host is an RP2350, so the CPU type is the discriminator.

**`uctypes.bytearray_at(addr, size)` in the OG's MicroPython is a full
read/write window onto RAM, over USB, with no debug probe.** (`machine.mem32`
is not built in; `uctypes` is.) That is what verified the pick work with SWD
down, and it replaces SWD for anything that only needs to poke a global -
publishing a fake pick, setting `debugProbing`, driving an indicator flag.
Injecting a button press still works the same way: write 2 to
`ProbeButton::getInstance()::inst + 0x5c`.

### Phase 2 — analog + probe
- [x] SPI `MCP4822` DAC backend (2026-09-08; measured DAC0 0–4.096 V, DAC1
      −6.9..+7.0 V - see the session above; `caps.spiDac`).
- [x] 4 ADCs scaled from the descriptor (ADC3 ±8 V), both INA219s (2026-09-08).
- [ ] 3 routable GPIO + single routable `NANO_RESET` (UART pins are right now;
      `RP_GPIO_0` routing itself untested; the UART node naming is a design call).
- [x] Scanning probe ported from the OG reference firmware (2026-09-07,
      `src/sensing/ScanProbe.cpp`; serial-verified on the bench, hands-on
      pending - see the 2026-09-07 session below).
- [ ] Capability-aware structured errors for unsupported ops (rail voltage set,
      GPIO 4–10, out-of-range DAC, probe pads) on serial + MicroPython.

### Phase 3 — parity + V5 cutover
- [ ] Migrate V5 onto the unified router; prove parity on real V5 hardware
      (host test + HIL) before removing the old `ch[]` path.
- [ ] Optional extras: wavegen via RP2040 PIO, undo, etc.

### Deferred — docs website
- [ ] Add an **OG Jumperless** page to the `Jumperless-docs` site (for humans
      AND agents): what features the OG supports vs V5, how to flash the
      `jumperless_og` firmware, the capability JSON an LLM tool should read, and
      the MicroPython/serial control surface. Not started; intentionally
      deferred until the OG firmware boots.

### Session 2026-09-22 — a user's OG fried itself: the router shorted supplies, and nothing was checking

A field report of an OG burning a crossbar chip on the backport build. Found
by compiling the OG router on the host and fuzzing random netlists through it
against a wire model of the OG fabric (`test/test_routing_og/run.sh`, the
runnable check; `regress` mode holds the eight netlists below, all of which
shorted or mis-routed on the shipped build). Root causes, all in
`NetsToChipConnections_OG.cpp` unless noted:

1. **`Lchip` was never cleared.** `clearAllNTCC()` memsets the path table to
   -1, which reads back as `true` for that bool, and the flag was only ever set.
   Every alt path took the chip-L branches, including a same-SF-chip hop with a
   loop that has no `break`: it claimed all eight hub lines and parked the net
   on chip H's Y0. Two nets there = a hard short. **`GND-D6` + `3V3-D1` shorted
   GND to 3.3 V** through I.y7 / J.y7 / H.x14 / H.x15. Now decided (and cleared)
   in `assignPathType`.
2. **SF<->SF hops through a breadboard chip** (`D6-A0`, `5V-D9`) never set
   `chip[3]` (half the route unsent) and never checked or claimed chip L's side
   of the hub line they rode - `5V-D9` sat on B.Y0 while `DAC0-ADC0` sat on
   L.Y[B] = the same copper: 5 V into the DAC0 op-amp. One checked loop replaces
   the reference's two.
3. **The BB->L alt path's lane-1 branch wrote a hop-chip lane index into chip
   L's X slot** (`x[1] = xMapL1c1`): chip D rows reached UART_TX through L.x7 =
   DAC0, chip H rows through L.x15 = GPIO_0, chips E/F through L.x9/x11 = rows
   30/60.
4. **NANO->L paths went through the breadboard->L code** (`commitPaths`, case
   labels include NANOtoSF): `yMapChipL = chip[0] = 8` put y = 8 in the path -
   `sendPath` masks that to y0 - and wrote `yStatus[8]` past the array. Case
   now requires a breadboard `chip[0]`; `sendPath` and the validator refuse
   out-of-range coordinates.
5. `Lchip` was decided before `resolveChipCandidates()` picked the chips, so a
   5V/DAC0/ADC2 that resolved to L went down the I/J/K hop loop and came out
   unrouted. Recomputed from the resolved chips in `resolveAltPaths`.
6. The paired `-2` bounce slots of a row->L route (same chip, Y0 and the row)
   resolved to two different lanes under the virgin-only lane test; same for
   the hop chip's `-2` Y slots in the BB->SF and SF->L hops. They are one lane
   / Y0 now.
7. Nano pins used the shared V5 `nano` table (AREF on chip K): every AREF
   bridge was unrouted. `findStartAndEndChips` finds Nano pins on the board's
   own xMap like every other SF node, which also opens chip K for primaries.

**RouteSafety now builds for the OG** (`RouteSafety.cpp`): the wire graph,
`componentHasShort()`, `validateAllPaths()` and the audit; only the V5 fast
path and the V5-fabric self-check stay stubbed. Fixes needed for the OG
fabric: a breadboard chip's Y0 is the hub wire shared with L.Y[c] (it fell
through to `wireForNode(CHIP_L=11)` = row 11); a chip reference on an SF
chip's X pin must be >= CHIP_I (L.x8 is row 1 = CHIP_B, and read as a lane it
made row 1 chip B's hub); `RP_GPIO_0` is node 114 = V5's ADC4, so the driven /
high-Z classifier is per board. Two rules added for both boards: a driven
source or a breadboard row that is in NO net must not ride a net's copper
(the two-net rule cannot see an unused 5V), and a path with an out-of-range
coordinate is corrupt. And `validateAllPaths()` now WIPES the coordinates of
every skipped path - `sendPath()` and `updateChipStateArray()` never looked at
`skip`, so on V5 too a path the validator refused still went to the crossbar.
Cost on the RP2040: 4.6 KB static (`kMaxWires` 200, two lanes, no per-root
node lists). OG build 73.6 % RAM.

Fuzz numbers, 20 000 random netlists of up to 16 bridges: shipped build 120
netlists with a short or a stray source per 5 000, 13.6 % of bridges open;
now 0 shorts, 0 validator drops (router and validator agree), 0.6 % open
(0.27 % on row-heavy netlists).

### Session 2026-09-23 — on the bench: a build that could not boot, then 599 bridges clean

**The local OG build had been unbootable since Sep 17, for a reason unrelated
to routing.** `lib/micropython/library.json` pulled `extmod/{vfs,vfs_reader,
modos}.c` from `../../../micropython_repo/` - outside the library, so
PlatformIO compiled them into `.pio/build/micropython_repo/` with NO env in the
path, shared by the V5 and OG envs and never rebuilt. The V5 build left
**ARMv8-M objects** there; linked into the OG, they promoted the ELF to
`Tag_CPU_arch: v8-M.mainline`, and GNU ld then emitted Thumb-2 long-branch
veneers (`ldr.w pc, [pc]`) for every flash->RAM call. The first one executed
is libgcc's `__gnu_thumb1_case_uhi` from TinyUSB's `tud_task_ext()` (the
arduino-pico linker script puts libgcc in RAM): undefined instruction on the
M0+, HardFault before `setup()`, dark board, no USB. Caught over SWD with
`vector_catch hard_err`. Fix: the three lines are gone from `library.json`
(the embed tree's `+<extmod/*.c>` already compiles byte-identical copies
per env; the duplicates only linked because of `--allow-multiple-definition`).
`rm -rf .pio/build/micropython_repo` once. Verify a build with
`arm-none-eabi-readelf -A firmware.elf | grep Tag_CPU_arch` = `v6S-M`.
This is very likely the doc's earlier "USB load left stale flash" mystery -
the flash was fine, the image was not.

**Do not bench with a debug-probe session attached.** A core left halted
pauses the RP2040 timer (`DBGPAUSE`), `main()`'s `delay(1)` before the core-1
launch never returns, and the board sits enumerated-but-silent (or drops off
USB). It cost an hour looking for a routing hang that was not there.

**Routing, on copper** (`test/test_routing_og/bench_og.py`: random netlists
over port 1, `:crossbar` over port 7, the wire model on lastChipXY):
`GND-D6` + `3V3-D1` = `J.x15/x6 on J.y0`, `I.x14/x1 on I.y0`, nothing on A-H
or L. Then 25 + 59 + 28 netlists (599 bridges, up to 14 per netlist, half
the endpoints SF nodes): **0 shorts, 0 strays**. Two opens in the first
dense batch, both `Couldn't find a path`, both the same cause: the Lchip
alt loop ended in a `break` (reference too), so a row->L route that could
not use CHIP A's hub failed outright - every hop in the OG's history went
through chip A. Now `continue`: host fuzz opens 0.6 % -> **0.03 %**, and
the rerun on the board was 0 open. Also: the row's own chip is skipped as a
hop candidate (its lane index to itself is -1 -> `xStatus[-1]`), the hub is
checked from both ends, and a hop releases the direct-route reservation
when nothing else of the net is on it.

**Connection-string parser** (`FileParsing.cpp`, both boards): a token the
alias tables did not turn into a number failed `toInt()` and **left node1/
node2 holding the previous bridge's node** - `+ 5V-3, GPIO_0-8` put 5 V on
row 8. Now "Unknown node name" and the command stops. Why tokens failed:
short aliases matched inside longer names (`T_R` in `UART_RX`, `I_P`/`I_N`
in the very `I_POS`/`I_NEG` that `b` prints); long names now go first, and
`GPIO_0`/`GP_0`/`RST` exist as aliases (`GPIO_0` only on a board whose GPIO
table has RP_GPIO_0). Node 84 (NANO_RESET, routable on the OG) printed as
**"3V3"** - the positional nano name table had the wrong label; now `RST`.
Node 114 prints `GPIO_0` on the OG (`ADC_4` is the V5's).

### Session 2026-09-23 (afternoon) — MicroPython back on the OG: 26.5 KB of static RAM reclaimed

**Symptom:** JumperIDE "Device not responding" on the OG. Port 5 opened but
never answered anything (not even Ctrl-C); port 7 was fine. `:fs` on port 7
said `{"error":"mp_init_failed"}`: `mpAllocHeap` needs 28 KB of free C heap
(16 KB rung + the OG's 12 KB reserve) and found 17.4 KB. Static RAM had
grown 177.6 → 193.8 KB since the 09-08 session that verified the REPL (4.6 KB
of it RouteSafety on 09-22, the rest not itemized), and this morning's build -
the first bootable OG since Sep 17 - was the first to show it. Side effect:
MpRemoteService retried the failed init every pass, 48 us / 28 % of core 0.
The REPL service path itself is identical to V5; the heap floor is the only
OG-specific difference that kills port 5.

**Cuts (OG only unless noted):** AdcRing `s_ring` 8 KB sat outside its own
`ADC_RING_BUILD` gate (the flag now lives in AdcRing.h; `adcRingData()` is
NULL on the OG); the `:padraw` 1.4 KB dump buffer gates on the same flag;
MenuTransitions frames 3 x 1.2 KB → 1 pixel (no click wheel, no menus);
PartLabels `lblScratch` 1.2 KB → placeholder, with a `ledsPerRow` guard in
`compose()`; `-DMICROPY_PY_MATH_SPECIAL_FUNCTIONS=0` in the OG env
(erff/erfcf/lgammaf/tgammaf are 7.5 KB of RAM-resident libm under the
arduino-pico linker script; the library carries three mpconfigport.h copies,
so a build flag); Debugs `sepAccuracyPct` uses an A&S 7.1.26 erf (both
boards, |err| < 1.5e-7) so libm's double erf/erfc (3.6 KB) leave the image.

**Result:** static 193,800 → 167,320 B (73.9 → 63.8 %); arena 60,984 →
93,816 B. On the board (1.7.11.2, flashed by the port-5 1200-baud touch +
`picotool load`/`verify`/`reboot`, filtered to 2e8a:0003): the GC heap
allocates at the configured 28 KB rung with 21.7 KB of C heap left, `:fs`
walks, the port-5 raw REPL answers, `gc.mem_free()` 19 KB after
`import jumperless`; JumperIDE's whole post-connect script set (device info,
walkFs in 0.34 s, version read) replayed OK over pyserial. MpRemote 14 us /
10 %. V5 builds; its RAM map is unchanged apart from the erf swap.

**LED buffers, second pass (same afternoon):** the OG's strip buffer was
`updateLength(445)` since the first tentative backport ("so V5-only paths
can't write past it") - but every setPixelColor variant has guarded on
`ledMaxPixels()` since that same commit, so it was 1 KB of heap and 13 ms of
DMA per frame (445 x 30 us) for a 111-LED chain. Now `OG_LED_COUNT` (111:
rows 0-59, rails 60-79, header 80-109, logo 110) in LEDs.h sizes the buffer,
`topleds` is a 1-pixel stub on the OG (never begun there; was 435 B of dead
heap), `getPixelColor()` got the same bound as the writers, and the `:leds`
dump is 111 pixels on the OG (1.1 KB static back; V5 output unchanged). On
the board `:leds` before/after match on all 110 non-logo pixels; free C heap
at boot 28.4 KB with the GC heap allocated. Static 167,320 → 166,200 B; V5
+24 B (the read guard is RAM-resident code). Bench note: after this
`picotool load`, ports 5 and 7 came back enumerated but silent while port 1
answered `?` - a 1200-baud touch on port 5 + `picotool reboot` (no load)
re-enumerated them clean. Left alone: `wireStatus[64][5]` 1.3 KB (V5 wire
mode, dead on the OG but indexed [i][0..4] in shared code) and the menu
tables (`menuLines[150]` Strings 1.8 KB static + their heap copies,
`menuLevels`/`stayOnTop`/`optionSlpitLocations` 1.8 KB) - the next ~6 KB if
it's needed, but they're menu code, not LED code.

**globalState audit + lazy menu (third pass, same afternoon).** From the V5
image's DWARF (`arm-none-eabi-gdb -batch -ex 'ptype /o globalState'`; the OG
env builds without debug info) with the OG constants: 32.5 KB = nets 11.8 KB
(196 B each: nodes 48 + bridges 96 + ...), paths 9.2 KB (128 B each),
DisplayState 5.3 KB, parts 2.4 KB, chipXY 1.5 KB, chipStates 1 KB.
Cut: `chipXY[12]` (1,536 B, both boards - only ever memset, the live
crossbar image is `lastChipXY`); `MAX_CUSTOM_NET_ENTRIES` 16 on the OG for
the custom color/name tables (3,872 B; 60 hand-named nets on a 60-row board
is not a thing). NOT cut, and it cost two boot loops to learn: the per-net
`bridges[MAX_NODES][2]` table (5.5 KB). A `grep | head -12` hid the hits in
`NetsToChipConnections_OG.cpp` (1455, 1980): the OG router reads it, and a
1-slot table let those loops read past the array into the net's name pointer
and colour fields - HardFault ~3 s into boot, watchdog reboot, loop. Rescue
that works from the desk: 1200-baud touch on PORT 1 (the core's CDC0 handler
runs from the USB task even when setup() never finishes; the port-5 touch
needs the main loop), `picotool load` the last good uf2 with a retry, verify,
reboot. Candidates left: pathStruct int→int8/int16 packing (~6 KB OG, ~11 KB
V5, both routers), `wireStatus[64][5]` 1.3 KB.
Menu tree: `menuLines`/`menuLevels`/`stayOnTop`/`numberOfChoices`/`actions`/
`optionSlpitLocations` are heap tables allocated by the first `initMenu()`
(the tree literals live in flash as `kMenuTreeDefault[]`); boot loads them
only where `caps.hasRotaryEncoder` (V5 timing unchanged, .bss → heap), the OG
loads on its first open - `clickMenu()`'s activation branch, `getMenuSelection`,
`requestReopenAtTopLevel` all call the latched `::initMenu()` (mind the
`Menus::initMenu` declaration with no body: inside a member function the
unqualified name picks it and the link fails). `menuChars[1000]` was never
read: deleted. Result: OG static 166,200 → 156,916 B (59.9 %), globalState
27,088 B; V5 −5.4 KB static. Board: 40.8 KB C heap free at boot with the
28 KB GC heap up, `connect(1,2)`/`(2,3)`/`(10,11)` → two nets, crossbar
populated, `nodes_clear()` back to empty.

**Regression + fix (same evening):** on the V5 the menu went "out of bounds"
after the lazy-menu commit. Cause, read from the V5's RAM over port 5
(`uctypes.bytes_at` at the ELF's symbol addresses): `menuRead 1`,
`menuParsed 0`, every `menuLevels[]` 0, `categoryIndex 16`. `menuParsed = 1`
had always sat INSIDE `parseMenuFile()`'s `if (printMenuLinesAtStartup == 1)`
debug block, so it never latched on a normal boot - harmless while boot was
`initMenu()`'s only caller, but the lazy load calls `initMenu()` on every
menu open, and a second parse of already-stripped lines finds no dashes,
zeroes the levels and appends categories until `categoryRanges[16]` is full.
The latch now sits at the end of the function. Verified on both boards by
opening the real click menu twice through the Debugs "Menu FX" tuner
(port 1: SI, `D`, Enter, 15 x Down, Enter; a byte ends a session and the
tuner reopens it) and re-reading the tables: `menuParsed 1`, `categoryIndex
14`, levels `0,1,2,0,1,...`, mirror rows `>Rails`. The check lives in
`test/hil/menu_check.py`-shaped form in the session scratchpad; worth
promoting. Build note: the OG image came out labeled 5.7.11.2 twice today.
`include/FirmwareVersion.generated.h` is one shared file rewritten by each
env's pre-script, and an object SCons considers up to date keeps the version
it was compiled with (the OG's `main.cpp.o` from a two-env `pio run` embedded
the V5's string and survived an OG-only rebuild). `version_from_file.py` now
passes `FIRMWARE_VERSION` as a per-env `-D` (`env.StringifyMacro`) and writes
the header as a guarded fallback for editors and the host QSTR build only.

**Banner (evening).** The OG's MicroPython banner said `jumperless-v5
v1.7.11.2 with rp2350b` and `sys.platform` said `jumperless-rp2350`: the
three `mpconfigport.h` copies hard-coded the V5's names. The OG env already
passes `-DOG_JUMPERLESS`, so the copies now pick `jumperless-og` / `rp2040` /
`jumperless-rp2040` under it (all three edited identically; keep them
byte-identical). None of these are QSTRs (`MP_DEFINE_STR_OBJ`), so the
host-side QSTR build, which never sees the define, is unaffected. Verified on
the OG over port 5: `sys.implementation._machine` = `jumperless-og v1.7.11.2
with rp2040`, `os.uname().machine` = `jumperless-og with rp2040`. JumperIDE
and the desktop app were taught the OG the same evening (JumperIDE
`src/firmware_feed.mjs`, app `classify_firmware()`): both key on the version
major 1 as a fallback, because every OG build shipped before this change
still claims to be a V5.

### Session 2026-09-24 — the self test runs on the OG; two OG bugs underneath it

**Why a probe calibration kept starting.** Kevin: "it wants to calibrate the
probe which doesn't apply". Not the self test (stubbed out on the OG until
today) and not `calibrateDacs`'s tail (it returns at the top on the OG): the
firmware-version migration in `updateConfigFromFile` sets
`probeCalibrationNeeded` whenever `probe.droop_ohms == 0`, and boot then runs
`calibrateProbeSwitchThresholds()`. The pad probe's droop calibration is the
only thing that ever writes `droop_ohms`, so on an OG it is 0 forever and
every version change (each of yesterday's reflashes) re-armed it. The OG's
config confirmed it over port 5: `droop_ohms = 0.0000` under
`firmware_version = 1.7.11.2`. The sentinel is now gated on
`caps.hasProbePads`, and `calibrateProbeSwitchThresholds()` /
`probeCalibApp()` bail with one line on a board without pads, so no other
caller (menu, app table) can start them either.

**Self test on the OG.** `SelfTest.cpp` compiled unchanged for the OG once
the stub block was removed (the compiler was the cheapest way to find that
out), so the port is runtime gating on the descriptor: probe_cable and
tip_voltage SKIP without pads, psram SKIPs without `hasPsram`, the
peripherals test takes `spiDac` as "no I2C DAC to ACK", the crossbar test
loops the descriptor's routable GPIOs (V5: GPIO_1-8, OG: GPIO_0) instead of
`gpioDef[0..7]` and skips the rail phase unless `railsFirmwareControlled`,
`selfTestNormalizeHardware` zeroes only the DACs the board has and releases
only those same GPIOs, encoder input is ignored where there is no encoder
(the OG's `BUTTON_ENC` pin reads something else), and the LED result is
painted straight into rows 1-3 / 5-8 / 10-13 / ... on a 1-LED-per-row
board, since `renderGraphicOverlays` returns on the OG. The overlay's
300-pixel buffer was a static that the OG now paid 1.2 KB for; it is a
heap allocation for the moment `addOverlay` needs it (which copies).
First start: the tail of `calibrateDacs` (examples, self test, pad
calibration, undo wipe, restart) is now `firstStartFinish()`, and the OG
runs it too - minus the pad calibration - instead of skipping everything
along with the DAC sweep. On a pad-less board that hold is bounded (20 s,
any input cuts it short): an OG's first start is a user dropping a UF2 with
no terminal and maybe no probe, not an operator at a bench. That path is
read-verified only (no factory reset of the bench OG). The sentinel fix was
checked by flashing a build labeled 1.7.11.3 over the 1.7.11.2 config: boot
came up clean and `?` answered with the version line, no Switch Calib.

**The reading that was not a crossbar fault.** The first OG run failed all
60 rows at -5.8 V on 0-5 V channels and GPIO_0 LOW at -8.85 V. MicroPython
read the same route at 2.0 V. The arrays in RAM at boot were the descriptor's
(ADC0-2 spread 5 zero 0, ADC3 16.34/8.1) - but `readSettingsFromConfig()`
copies `[calibration]` from config.txt over `adcSpread/adcZero/dacSpread/
dacZero` on every config sync (seven call sites), and the OG's config holds
the V5 defaults (zero 9.0, spread 18.28, dac zero 1650): the readings decode
exactly to those constants. New capability `analogCalInConfig` (V5 true, OG
false; in `boardCapabilitiesJson` and `test_boards.cpp`); the sync applies
the calibration block only when it is set. Independently, the crossbar sweep
now measures DAC1 straight into each ADC first and judges the rows against
that reference (+/-0.35 V) instead of the 2.5 V nominal: the OG's bipolar
DAC1 stage reads ~0.5 V low through the reference firmware's constants
(2.5 -> 1.97 V, 3.3 -> 2.78, 1.0 -> 0.49 over MicroPython), which is a
calibration observation for later, not sixty dead crosspoints.

**Bench.** OG, `self_test` from port 1 (line-buffered mode - SO then the
backtick line; the raw path never dispatched): crossbar PASS 60/60 with
every row within 0.1 V of its reference, GPIO_0 3.333/0.042 V, peripherals
PASS, three SKIPs, 13 s end to end, the hold released by a serial byte, reset
as designed. Not verified: the row paint's persistence during the hold (port
7 is serviced by the main loop, which the hold blocks, so no LED dump). Host
`test_boards` OK. V5: builds; RAM -1.2 KB from the overlay buffer.

**DAC calibration on the OG (afternoon).** Kevin: "the rows reading ~2V is
weird, we should calibrate the dacs". Measured with the descriptor constants
in RAM (`dacZero` 1772 confirmed as int32): DAC0 within 25 mV at every
point, DAC1 0.5 V low at every point with the slope right - a per-board zero
code, not a scaling error. `calibrateDacs()` on the OG now runs
`calibrateAnalogOg()` (Apps.cpp): ADC0 is the reference (descriptor unity
0-5 V; its zero is read off GPIO_0 driven low, its gain checked against
GPIO_0 driven high), DAC0 and DAC1 are fitted to it through the crossbar
(least squares of ADC0 volts on the DAC code; clipped points dropped), then
ADC1-3 to the fitted DAC1 (ADC3 over -4..+4 V). Each fit is range-checked
and verified at a mid point before it is kept; the set is saved only if
every fit passed. Bench, this board: DAC0 spread 4.086 zero 13, DAC1 spread
15.922 zero 1913 (vs 1772 nominal), ADC0-2 zero ~0.04 V, ADC3 16.44/8.29.
After: DAC1 2.5 -> 2.496 V, 3.3 -> 3.295, -3.0 -> -2.988 on ADC3; DAC0 2.5
-> 2.501. 

**Where the constants live, second pass.** Yesterday's `analogCalInConfig`
capability (config calibration never applied on the OG) could not survive
the OG calibrating itself. Replaced by a stamp: `BoardTopology.generation`
(V5 5, OG 1) and `applyAnalogCalibration()` (Peripherals.cpp) - descriptor
defaults first, then config.txt's [calibration] only when
`hardware.generation` in the config equals the board's. `calibrateDacs`
writes the stamp with the constants; `resetConfigToDefaults(clearCal)` puts
it back to the config.h default (5). So an OG config that still carries the
V5 numbers (stamped 5) is ignored, as before, until the board calibrates;
the V5 (stamped 5 by default) is unchanged. `initDAC`/`initADC` and
`readSettingsFromConfig` all go through the one function, so the boot-order
trap (config sync before initADC re-seeded the descriptor over it) is gone.
The config zero rows (`dac_*_zero`, `adc_0..3_zero`) now allow a little
below 0: the loader clamps to the X-table range and DAC0's fitted zero is a
few codes negative on some boards.

**Bench notes.** `$` runs the calibration from port 1 in line mode. My
port-1 capture dropped mid-calibration because Kevin's app (a Python
process) held that port from ~12:50 and macOS let both readers in: the
board finished, the config proves it, and the PASS banner went to his
terminal. Disclosed in the session report; the one-process-per-port rule
stands.

### Session 2026-09-24 (afternoon) — an OLED on the OG, through the UART pins

Kevin: "an og jumperless should be able to connect to an oled", then "it can
use uart", then "we can use I2C0 and just share the bus", then "oh wait
nevermind, it's different pins". All four were right in turn. The V5's
connection_type 0 routes the panel's SDA/SCL from Nano D2/D3 through the
crossbar to its routable GPIO 26/27 (I2C1); the OG's only I2C-capable
routable pins are the UART pair, 16/17, which are I2C0 SDA/SCL - and I2C0 is
the INA219 bus, hardwired on 4/5. The wires can't be shared, the block can:
`I2C0Arbiter.cpp` (which already wraps every I2C0 transfer for WaveGen)
now takes an alternate pin pair and, per transaction, muxes in the pair the
target address lives on (0x3C -> 16/17, everything else -> 4/5). Two pairs
assigned to one peripheral input are combined by the GPIO mux, so never
both. I2C0 is core-0-only (readCurrent's own note), so no lock.

What it took, in the order the bench found it:
- `BoardTopology` gained the crossbar I2C pair (`xbarI2c*`: V5 26/27 on
  RP_GPIO_7/8, OG 16/17 on RP_UART_TX/RX, plus the UI name), `caps.hasOled`
  is true on the OG and a new `caps.internalOledHeader` (V5 rev 7 only)
  guards the boot probe of the internal I2C0 header. Type 0's pins come from
  the descriptor at every `oled::init`, never from config.txt: the OG's file
  still said 26/27, which are its ADC0/ADC1 pins. The bus is chosen by the
  SDA pin's block (bit 1), not by connection type.
- `initI2C` knows 16/17 and, when I2C0 already runs on another pair,
  registers the alternate pair instead of `Wire.setSDA` - which on this core
  is a panic on a running bus, not a move.
- `AsyncPassthrough::releaseUartPins/reclaimUartPins`: the passthrough's
  receiver and DMA stop while the OLED holds 16/17, and come back the way
  boot brings them up. **While an OLED is connected on the OG, the UART
  passthrough (port 3, Arduino flashing) is off.**
- The pair switch wedged the block: un-muxing the old pair before muxing
  the new one in leaves the block's SDA/SCL inputs unassigned (they read
  low) for a few writes, and the master took the edges for a START it never
  saw a STOP for - after which it held every command in its FIFO waiting for
  a free bus (INA reads timing out at exactly TwoWire's 1 s, block "idle",
  TXFLR=1). Now the incoming pair is muxed in first (SCL, then SDA) while
  the outgoing one still holds both buses high, then the outgoing pair is
  parked; a transaction that still times out with a pair registered toggles
  the block's enable. The mux is verified against the pins' real function
  select before every transaction, because TwoWire's timeout recovery and
  any `Wire.begin()` re-mux the primary pair on their own.
- `checkConnection()` answers from a once-a-second cache, and `connect()`
  primes that cache with "present" without asking the panel; on the UART
  pair `init` now pings for real (up to three tries, 20 ms apart) and, if
  nothing answers, `disconnect()`s - routes dropped, passthrough back -
  rather than holding a bridge and a parked UART for a display that is not
  there (every OG boot comes through here, connect_on_boot defaults to 1).
- The detection then still failed on the bench with the panel powered (D2
  and D3 read 3.27 V through its pull-ups). Driving GPIO 16 and 17 and
  reading each UART lane through ADC0: the UART_TX lane is GPIO 16 and
  UART_RX is 17. `kOgGpio` had them the other way round, so SDA was being
  routed to the SCL lane. Table fixed; `test_boards` now pins the lane-to-
  pin mapping.

Two more from the review pass and the boot tests:
- The passthrough begins ~3 s after `startupCompleteRequestTime`, i.e.
  after the boot OLED init, and two other paths re-enable its receiver.
  `s_pinsReleased` (set by releaseUartPins, cleared by reclaim) is honoured
  by `begin()` (no pin mux, receiver left off) and by every
  `enableUARTReceiver()`, whichever order boot runs them in. Port 3 read 0
  bytes with the OLED live, before and after the passthrough's start.
- Detection at boot was a coin toss across resets while manual connects
  were 6/6, and the run that recovered it had toggled the SDA lane by hand
  first: a reset mid-transaction leaves the SSD1306 holding SDA low, which
  is a STOP away from answering again. `i2cBusUnstick()` (nine SCL clocks
  and a STOP, bit-banged while the pair is still SIO) now runs before the
  alternate pair's first ping; the init pings five times over 200 ms.

- The last way the panel went dark: the passthrough's own routing.
  `connectArduino()` bridges the UART lanes to Nano D0/D1 as plain bridges
  (at boot, on a DTR on port 3, on flash), and a plain bridge on the OLED's
  GPIO node is exactly what makes its infra pairs yield ("a user claiming
  the pins wins"). Every dead-panel episode in the log followed a port-3
  open. Now `oledOwnsUartPins` (set at the top of connect(), cleared in
  disconnect()) makes `connectArduino()` a no-op and `enSerial1()` false
  while the OLED holds the lanes; connect() parks any D0/D1 bridges it
  finds and disconnect() puts them back. Reproduced and fixed on the bench:
  OLED live at boot after the passthrough's boot routing, still live with
  port 3 open (0 bytes on it), D0/D1 routes back after disconnect, lanes
  back to the OLED on reconnect.

Bench (OG, over port 5 - Kevin's app holds port 1): OLED live from boot on
5/5 consecutive boots, 6/6 manual reconnects, every `oled_show` 16.7 ms (a
full 512-byte frame at 400 kHz), INA0/INA1 reads correct and 0.4 ms across
every pair switch, pin-to-row path proven by driving GPIO 16/17 and reading
D2/D3 through ADC0 with the panel's pull-ups as the load,
`:nets` shows D2<->UART_Tx and D3<->UART_Rx while connected and nothing
after. V5: builds and `test_boards` passes; one V5 behaviour change:
connection_type 3 (custom) picks the bus by its SDA pin now, where before
it always took I2C1, and `connect()` waits 10 ms after the crossbar send.
`I2C0_BUS_CLOCK_HZ` is 400 kHz on the OG (was a literal in initINA219).

Follow-ups: `updateLazyAdcReadings` still returns early on the OG (the OLED
GUI's {adc:N} tokens read a stale cache there); `provisionFirmwareFiles`
still skips the OLED image assets on the OG; the config TUI's type-0 label
comes from the descriptor but the config-file token is still `gpio_7_8`;
`gpioDef[8..9]` (pins 0/1 for the UART nodes) is V5 wiring on both boards.

**Still open:** `os.statvfs` is missing (the IDE tolerates it); MpRemoteService
still retries a failed heap alloc every pass (latch it); the OG router prints
a burst of blank lines per refresh on port 1 (not from routing itself -
MicroPython connects print nothing - something per self-test row);
the config TUI shows `hardware.generation 5` on an uncalibrated OG (the stamp's default; cosmetic). Build note: with the IDE
open, a venv `pio run` and the IDE's own PlatformIO take turns cleaning
`.pio/build` (project.checksum mismatch) - builds die mid-way with "can't
create ...o" and the tracked V5 `firmware.uf2` gets deleted. Build with
`PLATFORMIO_BUILD_DIR` pointed at a scratch dir and `git checkout` the uf2
before committing.

### Session 2026-09-25 — a user's circuit stayed dead after probing: the exit's clean resend was never clean

**Report (an OG user, via Kevin):** a 1k + LED circuit on the board. On the
original firmware the LED goes out on entering probing mode and comes back on
leaving it, as the connections are re-made. On every JumperlOS OG release they
tried (1.7.11.1 through 1.7.11.4) the LED goes out and stays out.

**Mechanism, traced hop by hop (the OG was not on the bus, so this is a code
trace, not a bench repro):**

1. `scanprobe::sweepBegin()` empties the crossbar with a RESETPIN pulse
   (`crossbarReset()`), which is the reference firmware's behaviour and why the
   LED goes out. It marks every chip suspect, but that mark is only read by
   `refreshConnections()`.
2. `Probing::probeExitTail()` restores with `refreshLocalConnections(1, 1, 1)`,
   asking for a clean resend.
3. `refreshLocalConnections()` never used its `clean` argument: it posted
   `REQ_BYPASS` unconditionally, and before the mailbox (3f02a14) it was
   `sendAllPathsCore2 = 3` unconditionally. Neither cleans.
4. A bypass is `sendPaths(0)` → `sendAllPaths(0)`, a diff against
   `lastChipXY`. The reset pulse never touched that shadow, so every
   pre-existing crosspoint still reads "set" and nothing is sent. Only a
   crosspoint that changes later (a new connection) goes out — which is what
   the user saw: the old circuit is dead until the next real edit.

The `s r` diagnostic (SingleCharCommands) restores through the same call after
its own reset and prints "crossbar restored (clean refresh)", so it had the
same bug.

**Fix (both targets build, host `test_boards` OK, bench-verified below). What
it routes into was already bench-proven:** yesterday's OG self test drove
`refreshConnections(-1, 0, 1)` — `sendPaths(1)`, the same RESETPIN pulse and
full chip-ordered resend — repeatedly, and its crossbar sweep passed 60/60.
The only untested piece today is the one-line routing of the local refresh's
`clean` into that slot.
`refreshLocalConnections()` honours `clean`: with `clean == 1` it posts
`REQ_SEND` with `SEND_PATHS | SEND_CLEAN` (sticky, served on core 1's next
8 ms tick; the LED branch's `allIdle()` gate means the exit's
`requestLedShow(-1)` renders after the crossbar is back). Still no wait, as
before. The suspect mark is deliberately NOT mirrored in the local refresh —
that would put a clean on the V5's tap hot path.

`FileParsing.cpp`'s paste refresh had passed `clean = 1` for years without
getting one; it now passes 0 so the paste path keeps running exactly as it
always did (no reset pulse + full resend per paste on either board). That is
behaviour preservation, not a fix.

**Bench, 09:00–09:20 (Kevin plugged the OG back in; it was running the
released 1.7.11.1 with an empty slot — the user's exact firmware):**

- Baseline on 1.7.11.1, ports 1 + 7 (port 5 is dead on that release, the
  heap-floor bug): `f DAC1-10, ADC0-10`, DAC1 at 0 V, ADC0 0.05 V; `s r 10`;
  ADC0 4.99 V ×3 (the OG's floating buffer input) — the circuit is gone. The
  `:crossbar` shadow is byte-identical before and after: it still claims the
  crosspoints. That release's ADC calibration is garbage with today's config
  (2.5 V read 9.25 V), so the 0 V case is the clean one.
- Fixed build (1.7.11.3 label, flashed through the bootloader): DAC1 2.5 V →
  ADC0 2.29 V; `s r 10`; ADC0 2.26 / 2.28 / 2.26 V; DAC1 stepped to 1.0 V →
  ADC0 0.76 V (a live path follows; a held float would not); back to 2.5 V →
  2.27 V. `s r` restores through the identical `refreshLocalConnections(1, 1, 1)`
  the probe exit uses. The probe-button path itself was not pressed (no hands).
- Side effect of the 1.7.11.1 flash: the config came back with V5 default
  calibration stamped generation 5, so yesterday's OG calibration was gone
  (hence 2.29 V for 2.5 V: the fixed build ignored the foreign stamp, as
  designed). Re-ran `$`: generation 1, DAC1 2.5 V → ADC0 2.502 V. Bench left
  with an empty slot and DAC1 at 0 V, as found.

**Follow-ups (not in this commit):**
- `board_og.cpp:75`'s comment still says UART TX/RX are GPIO 17/16; the table
  under it (measured and fixed 2026-09-24) says 16/17. Stale comment.
- OLED + a probe session on the OG is untested: every sweep resets the crossbar
  and cuts the panel's SDA/SCL mid-transaction; the stuck-slave unstick runs
  only at connect, not at probe exit. No user has both yet.

**Bench checks** (both need port 1 free of the desktop app):

- The user's scenario: an LED + 1k on a net, press the probe button to enter
  connect mode, press again / send a byte to leave. Fixed: the LED comes back
  on together with the exit's LED render (the LED branch's `allIdle()` gate
  runs it after the clean send completes). Broken: it stays off until the
  next edit.
- Button-free, scriptable: from port 5 `jumperless.dac_set(1, 2.5)`,
  `jumperless.connect("DAC1", 10)`, `jumperless.connect("ADC0", 10)`,
  `jumperless.adc_get(0)` → 2.5 V. Then on port 1 `s r 10` (resets the
  crossbar, restores through the same `refreshLocalConnections(1, 1, 1)`),
  and read ADC0 again. Broken build: ~0 V. Fixed: 2.5 V.

## Agent conventions

- **Never** branch the shared core on `OG_JUMPERLESS`/board macros — extend the
  `board.h` contract instead.
- **Never** change V5 runtime behavior while doing OG work. The V5 descriptor is
  not yet wired into V5's live routing; V5 keeps `MatrixState.cpp ch[]` until
  Phase 3. If you must touch a shared file, gate OG-only paths behind board
  capabilities and leave the V5 path byte-identical.
- Peripherals/features are **enumerated, never counted** (the OG `RP_GPIO_0`
  must never be treated as `GPIO_1`). Binary features → `BoardCaps` flags.
- Run the host test after any board-layer edit; add an assertion when you add a
  capability.
- Update this doc's checklist before ending a session.

## Key files

- Contract: `src/boards/board.h`, `src/boards/board.cpp`
- Descriptors: `src/boards/v5/board_v5.cpp`, `src/boards/og/board_og.cpp`
- Build: `platformio.ini` (`[env:jumperless_og]`)
- Test: `test/test_boards/test_boards.cpp`
- OG reference firmware (for porting data/logic):
  `../Jumperless/JumperlessBackport/src/` (`MatrixStateRP2040.cpp`,
  `JumperlessDefinesRP2040.h`, `Probing.cpp`, `Peripherals.cpp`, `LEDs.h`).
  Note: that tree is a draft snapshot ("probably don't use this") — use it as a
  reference for OG topology/probe/DAC, not as production code.

## Change inventory vs `main` (for V5-safety review) — 2026-06-27

This is the audit map for a fresh review of everything the `OGbackport` branch
touches relative to `main`. Generate the live list with
`git diff --stat main OGbackport`.

**Is V5 byte-identical to `main`? NO** — and that's expected. Two reasons:
1. The board-descriptor layer (`src/boards/board.cpp`, `src/boards/v5/board_v5.cpp`)
   is now compiled and LINKED into V5 (adds `board::currentBoard()` +
   `v5BoardTopology`/`kV5XMap`/`kV5YMap`/`kV5BbNodesToChip`/`kV5Gpio/Adc/Dac`
   rodata). This is mostly inert data on V5, but it grows the binary.
2. A few SHARED files were refactored to be board-data-driven instead of
   hardcoded. Those are behavior-preserving ONLY IF the V5 descriptor reproduces
   the old hardcoded V5 values. The host test `test/test_boards/test_boards.cpp`
   is meant to assert that parity — run it as part of review.

To reproduce the V5 delta: build `-e jumperless_v5` on both branches and compare
`firmware.bin` (sha256), or `arm-none-eabi-nm --print-size --size-sort firmware.elf`
and diff. (`firmware.bin` is build-path independent: no `__FILE__/__DATE__/__TIME__`.)

### Verify FIRST (shared, NOT `#ifdef`-gated — real V5 code-path changes)
- `src/NetsToChipConnections.cpp` `findStartAndEndChips()`: corner-row chip
  assignment was unified from hardcoded cases (29/59 -> `CHIP_K`, 30/60 ->
  `CHIP_L`, 1..28/31..58 -> `bbNodesToChip[]`) to a single
  `case 1..60 -> board::currentBoard().bbNodesToChip[node]`. V5 correctness now
  depends on `v5BoardTopology.bbNodesToChip` matching the old logic. Also deleted
  dead `newBridges[MAX_NETS][MAX_DUPLICATE][2]`.
- `src/States.cpp` / `src/States.h`: `JumperlessState` made NON-copyable
  (`= delete` copy ctor/assign) and the 5 copy sites rewritten in place; legacy
  full-state history (`pushHistory`/`undo`/`redo`) no-op. Affects V5 too.
- `src/FileParsing.cpp`: `isNodeValid()` grew and a new `isNodeOnBoard()` was
  added (node validation now consults the board descriptor). Confirm V5 accepts
  exactly the same node set as before.
- `src/boards/v5/board_v5.cpp`: the V5 descriptor data is the new source of
  truth for the above — review it against the old hardcoded V5 maps.

### New files (no V5 source impact, except the board layer linkage noted above)
- `src/boards/board.{h,cpp}`, `src/boards/v5/board_v5.cpp`,
  `src/boards/og/board_og.cpp`, `src/boards/og/og_atomic.cpp`,
  `src/boards/og/og_unique_id.c` — board contract + descriptors + RP2040 shims.
- `src/NetsToChipConnections_OG.cpp` — entire body is `#ifdef OG_JUMPERLESS`, so
  it compiles to ZERO symbols on V5 (the OG router; V5 still uses
  `NetsToChipConnections.cpp`).
- `boards/jumperless_og.json` (board package), `platformio.ini`
  `[env:jumperless_og]`/`[env:jumperless_og_debug]` (the `[env:jumperless_v5]`
  block is unchanged — verified), `.github/workflows/og-prerelease.yml`,
  `test/test_boards/test_boards.cpp`, `CodeDocs/*.md`, `.vscode/*`.

### Shared edits intended to be V5 no-ops (gated by `#if defined(OG_JUMPERLESS)`
or `board::currentBoard().caps.*`) — verify the gating actually wraps every hunk
- `src/main.cpp` — boot path: OG `ogStartupAnimation()` vs V5
  `drawAnimatedImage()`; caps gating of probe stack / `fileCacheFlushService` /
  `initRotaryEncoder` / startup animation.
- `src/LEDs.cpp` / `src/LEDs.h` — bounds-checked pixel setters (`ledMaxPixels`,
  both boards), OG overlay in `showNets()` (header dim-purple, hardwired pins,
  rails at px 60-79, single logo LED), `ogStartupAnimation()`, OG branches in
  `lightUpNet`/`showSkippedNodes`.
- `src/Graphics.cpp` / `src/Graphics.h` — `dumpLEDs()` OG geometry
  (`dumpGridRow`, header reads), `rowColumnToPixelIndex`/`wireStatusToPixelIndex`
  gated by `caps.ledsPerRow`.
- `src/Peripherals.cpp` — `setCSex` OG CS bank (chips 8-11 -> GPIO 20-23),
  `initGPIO`/`setGPIO` OG early-return, `initDAC`/`initADC`/`initINA219` OG,
  `updateLazyAdcReadings`, GPIO-function name table RP2350 guards.
- `src/PersistentStuff.cpp` — `readSettingsFromConfig` GPIO-bank loop gated +
  OG brightness floor; `updateStateFromGPIOConfig` OG early-return.
- `src/CH446Q.cpp` — `initCH446Q` OG CS pin banks + LOW idle init; chip-K
  voltage-source guard `#if !defined(OG_JUMPERLESS)`.
- `src/RotaryEncoder.cpp`, `src/Debugs.cpp`, `src/Probing.cpp`,
  `src/ArduinoStuff.cpp` — RP2350-only PIO/PSRAM/coproc paths guarded so RP2040
  compiles; `rotaryEncoderStuff` OG early-return.
- `src/SingleCharCommands.cpp` / `.h` — OG-only `cmd_testChipSelect` ('I')
  wrapped in `#if defined(OG_JUMPERLESS)`.
- `src/Undo.cpp` — `UNDO_ENABLED` feature group (0 on OG, 1 on V5 -> V5 keeps
  undo + `toastScreen`).
- `src/GraphicOverlays.{cpp,h}`, `src/MatrixState.{cpp,h}`, `src/MeasureMode.cpp`,
  `src/MpRemoteService.h`, `src/JumperlessMicroPythonAPI.cpp`,
  `src/Python_Proper.cpp`, `src/micropythonExamples.h`, `src/Ser3Backchannel.cpp`,
  `src/AsyncPassthrough.cpp`, `src/Apps.cpp`, `src/usb_descriptors.cpp`,
  `src/configManager.cpp`, `src/config.h` — OG memory/feature gating, USB CDC
  count + strings, config additions.
- `src/JumperlessDefines.h` — OG node ids (`NANO_VIN`, `NANO_3V3`, `NANO_5V`,
  `NANO_RESET_0/1`, `NANO_GND_0/1`, ...) and feature-group flags. Verify these
  are ADDITIONS only (no existing V5 node id / constant changed value).
- `include/custom_tusb_config.h`, `include/usb_interface_config.h` — per-board
  `USB_CDC_ENABLE_COUNT` / FIFO sizes; verify V5 path unchanged.
- `lib/FatFS/src/ffconf.h` — `FF_FS_TINY` is `#if defined(OG_JUMPERLESS)` (V5
  stays 0; confirmed OG-gated).

### This session's specific work (2026-06-26/27)
- Nano-routing root cause: chip selects for chips I-L (GPIO 20-23) were being
  reconfigured to inputs by `readSettingsFromConfig` (and `updateStateFromGPIOConfig`);
  both now OG-gated in `PersistentStuff.cpp`. (`setCSex` chip+12 mapping was
  already correct.)
- LEDs: `showNets()` OG overlay (dim-purple header, hardwired pins at px
  82/83/96/97/106/107/108/109, rails 60-79), single logo LED sampled from
  `LOGO_LED_START+0` and brightened, OG rainbow boot animation, `dumpLEDs` OG
  geometry, OG brightness floor.
- Routing: `routeDuplicateViaAltNanoChip()` in `NetsToChipConnections_OG.cpp`
  (parallel BB<->NANO path via the alternate nano chip for lower resistance);
  `showSkippedNodes` duplicate guard. (OG-only file -> no V5 impact.)
- CI/version: `scripts/version_from_file.py` remaps the OG major to 1 (V5
  5.x.x.x -> OG 1.x.x.x); `release.yml` builds + attaches both firmwares;
  `og-prerelease.yml` manual OG pre-release. (Applied on `OGbackport`; `main`
  left V5-only.)







## To fix
- diagram.json should be bidirectional, so editing the text will update the board
- we should preload at startup
  "version": 1,
  "author": "JumperIDE",
  "editor": "wokwi",
  "parts": [
    {
      "type": "wokwi-breadboard-half",
      "id": "bb1",
      "top": -41.4,
      "left": -54.8,
      "attrs": {
        "color": "#575756"
      }
    },
    {
      "type": "wokwi-arduino-nano",
      "id": "nano",
      "top": -129.6,
      "left": 28.3,
      "attrs": {}
    }
  ],
  "connections": [],
  "dependencies": {}
}

- we shouldn't need to select ports when we 

- If I drag something from the main editor tab into the bottom repl tab, the tab expands to the top and resizing is backwards
- when we do windows, dome content doesn't load
- in windowing, the tabs from the main ditor area should be able to be free floating, so wokwi tab can be next to a serial terminal tab
- when we flash, it should show the debug log while its happening then auto hide