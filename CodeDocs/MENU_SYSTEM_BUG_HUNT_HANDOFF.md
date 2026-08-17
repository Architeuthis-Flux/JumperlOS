# Menu System Bug Hunt — Handoff (2026-06-10)

Status doc for continuing the menu/transition/encoder debugging session. Everything
below was found while building the menu frame-transition engine
(`src/MenuTransitions.h/.cpp`) and its Menu FX tuner (`D` → Menu FX in the status
& diagnostics menu). Fixed items are listed for context so they don't get
re-investigated; open items are the actual work.

---

## OPEN ISSUE 1 (primary): torn menu frames ("misaligned pixels")

**Symptom:** scrolling menus occasionally shows one frame of garbled / shifted /
partially-drawn text.

**Mechanism (verified in code, refined twice):**

- The strip driver does NOT stream the live pixel buffer over DMA. In
  `lib/Jadafruit_NeoPixel/Jeopixel_RP2.cpp` → `rp2040Show()`, the frame is
  **copied synchronously from `pixels[]` into a dedicated `dma_buffer`**
  (interrupts disabled, ~300µs for 900 bytes) before the DMA transfer starts.
  There is also a `dma_buffer_backup` double-buffer for shows issued while DMA
  is busy. So the snapshot moment is the copy loop, on Core 1.
- The tear happens when Core 0 is mid-repaint (`b.clear()` + `b.print()` in
  `renderMenuLine()` / `selectSubmenuOption()`) at the instant Core 1 runs that
  copy loop → the snapshot mixes the old and half-painted new frame.

**Mitigations already in place** (shrink, don't close, the window):

- `menuTransitionBeginDraw()`/`menuTransitionArm()` bracket all menu-line and
  option-row repaints; Core 2's menu branch skips painting AND showing while
  the bracket is open (`mtDrawing`), with a 300ms stuck-flag timeout.
- `menuTransitionCanShow()` is re-checked **immediately before** `leds.show()`
  in `core2stuff()` (`src/main.cpp`, search "menuTransitionCanShow") because
  `menuTransitionRender()`'s verdict can go stale before the show executes.
- Remaining window: Core 0 starting a repaint in the sub-ms gap between the
  `menuTransitionCanShow()` check and `rp2040Show()`'s copy-loop completion.

**Proposed real fix (recommended): steady-stamp menu frames from snapshots.**
The transition engine already owns clean per-frame snapshots (`targetFrame`,
captured by `arm()` after the paint completes). During transitions + the 50ms
linger, Core 2 already paints the breadboard exclusively from those snapshots.
Extend that to be **permanent while `inClickMenu == 1`**: steady state = stamp
`targetFrame` into the live buffer every menu frame, so whatever Core 0 is
doing to the live buffer between brackets is never visible. Painters that
legitimately bypass the bracket (value editors, node pickers, `doMenuAction()`)
already call `menuTransitionCancel()` — make cancel also disable the steady
stamp so they regain direct buffer ownership. This makes menu frames fully
atomic with no driver changes.

Alternative: a driver-level frame lock in `ledClass` shared by painters and
`show()` — more invasive, also covers non-menu contexts.

---

## OPEN ISSUE 2: transient USB CDC drops during File Manager bursts

Rapid serial-driven FM navigation (enter/up cycles) can drop the CDC port; the
firmware stays healthy (verified via SWD: both cores running, no fault) and
usually re-enumerates within seconds. Adding `tud_task()` to the FM main loop
(`FileManager::run`, `src/FilesystemStuff.cpp`) improved survival 3 → 21 cycles
in stress (`scripts/repro_menufx.py` has the test pattern; the FM stress was a
/tmp script: open `/`+Enter, then arrow-down/Enter/`.` cycles).

Contributors: per-keystroke multi-KB ANSI redraws + OLED I2C probe stalls when
the OLED is disconnected (`oled::checkConnection` → `_probe` bit-bang, 1s
throttle) + directory walks. Each nav also parks Core 1 briefly now (see fixed
item F2). Needs its own session; consider rate-limiting full TUI redraws and
skipping OLED updates entirely when `oledConnected == false`.

## ~~OPEN ISSUE 3~~ RESOLVED (2026-06-11): it was never an XIP/lockout hole

The "Core 1 executed XIP garbage during a flash program" faults (F2, PC=0x16,
and the click-menu Files crash, PC=0x10) were **Core 0 stack overflow into
Core 1's stack**. Core 0's stack is scratch bank Y (0x20081000–0x20082000,
4KB physical); Core 1's stack topped out at exactly 0x20081000. The click-menu
→ File Manager path (`loop → Menus::service → clickMenu → getMenuSelection →
doMenuAction → runApp → pythonScriptsAppLauncher → pickPythonScriptFromClickMenu
→ FileManager::run → … → refreshListing` + String/malloc internals) blows past
4KB; Core 0's frames land on Core 1's saved registers, and Core 1 pops a
Core-0 stack remnant into PC → INVSTATE. Caught red-handed via SWD autopsy:
Core 0 malloc-lock frames written below 0x20081000, Core 1's faulting popped
PC (0x00000010) literally present as Core 0 frame data at 0x20080ff4.

The flash correlation was a red herring — FM flash writes simply happen at
maximum stack depth. `idleOtherCore()` was working fine all along.

**Fix (verified by 30-cycle FM stress over the click-menu path):**
- `bool core1_separate_stack = true;` in `src/main.cpp` — Core 1 gets an 8KB
  heap stack; Core 0 owns both scratch banks (8KB) before reaching heap.
- MSPLIM hardware stack-limit guards armed on both cores (`armStackLimit()` in
  `src/main.cpp`) — future overflow = immediate STKOF UsageFault, not silent
  cross-core corruption.
- `FileManager` instances heap-allocated in `src/FilesystemStuff.cpp` (three
  entry points) to keep the deepest sessions off the boot stack.

The `pauseCore2ForFlash()` brackets from F2 are harmless but were treating the
symptom; they can stay for LED-stutter reasons.

## RESOLVED (2026-06-11): encoder skipping / phantom reverse steps

Full encoder overhaul in `RotaryEncoder.cpp`. Root causes, in order of impact:

- **`pio_sm_restart()` corrupted the quadrature count.** The PIO program
  keeps its previous-pin-state in the OSR; restart clears it, so any restart
  with pins at 01/10 relays a signed phantom ±1 count. Restarts fired on
  EVERY button press and EVERY `rotaryDivider` change (menus=4, probe=3,
  carousel=8, highlighting=4 → constant thrash) — raw count drifted
  thousands of counts per session. Both restart sites removed; the SM is
  initialized once and never restarted. The divider is software bookkeeping
  only.
- **Step collapsing.** Emission snapped `lastPositionEncoder` to
  `encoderRaw`, so multi-detent twists / busy-consumer backlogs collapsed
  into one step. Now paced: one event per consumer ack, backlog preserved,
  250ms abandon timeout, backlog dropped on click and on direction change.
- **Confirm gate ate the `encoderPosition` stream.** The 35ms click/turn
  gate rebased `encoderPositionOffset` and left it standing on commit, so
  each deliberate detent's travel was permanently swallowed (probe cursor /
  editor consumers — menus were unaffected). Offset is now restored on
  commit so held motion flushes through.
- **Detent recoil.** Fast flicks bounce the shaft 1-2 REAL detents backward
  off the detent spring. Velocity-aware recoil guard: while steps commit
  <100ms apart (+200ms), reverse travel slides the baseline instead of
  committing. Plus a static half-divider backlash margin for slow-speed
  settle.
- **Divider calibration.** One physical detent = 8 raw counts. Menu /
  yesNo / probe-voltage dividers were 4 (= 2 steps per detent, masked for
  years by the step collapsing above) — all set to 8.
- **Single-core ownership.** All encoder state (PIO FIFO, gates, button SM)
  is mutated by Core 1 only; Core 0 call sites are no-ops. Unconditional
  throttled poll at the top of `core2stuff()` (the old in-branch sites
  starved in probe mode). No mutex needed.

## OPEN ISSUE 4 (minor / by design, revisit if annoying)

- Holding to back out of a deep menu keeps `encoderButtonState` in
  HELD/MEDIUM/LONG (needed so the hold sweep shows) — a very long hold after
  reaching top level proceeds toward the LONG_HELD reboot countdown, same as
  the stock main-screen behavior.
- `RotaryEncoder.cpp` still carries the FOLLOW-UP comment about collapsing the
  two encoder consumer paths onto a single `committedRaw` owner — still valid.
- Menu FX tuner: each serial keypress closes/reopens the real menu, so the
  cursor returns to the top level (menu position isn't preserved on
  serial-exit; only the timeout path saves `returnToMenuPosition`).

---

## FIXED THIS SESSION (context — do not re-fix)

- **F1. `menuLines[]` out-of-bounds class** (hard fault, `free(0x23)`):
  `menuTree.h` array was sized by its initializer (117) while everything
  assumes 150; `menuParsed`/`menuPosition`/`menuTreeFile` live right after it.
  `readMenuFile(1)` never latched `menuRead` and destroyed its own `"end"`
  sentinel, so any second `initMenu()` set `menuLineIndex = 150` and poisoned
  every menu walk. Fixed: explicit `String menuLines[150]`, `menuRead` latch,
  bounds clamps. Caught live via SWD (`menuRead=0, menuParsed=0,
  menuLineIndex=150` at the fault).
- **F2. FileManager flash-vs-XIP hard fault**: FM directory walks
  (`refreshListing`, `changeDirectory`) used raw FatFS calls; the SPIFTL
  journal appends a flash page on f_close; Core 1 fetched XIP mid-program →
  INVSTATE crash (caught at the `micros` veneer). Fixed with
  `pauseCore2ForFlash()` brackets around the FS sections.
- **F3. `clickMenu()` polled-cleanup stomp**: `Menus::service()` polls
  `clickMenu()` every main-loop tick; the exit cleanup
  (`clearColorOverrides(false, true, false)`) ran on every fall-through,
  wiping connector-pad overrides thousands of times/sec on the main screen —
  erasing the encoder hold sweep's pad writes (caught with a debugger
  watchpoint on `GPIOcolorOverride0`). Cleanup now runs only if a menu session
  actually opened.
- **F4. Back-pop landed on the next item**: the back handler set
  `firstTime = 1`, and the firstTime nav branch does `menuPosition += 1` (how
  a fresh menu walks from -1 onto item 0). Removed; the no-remembered-pick
  fallback now scans to the first real sibling instead of relying on that.
- **F5. Hold-to-back rework** (`getMenuSelection`): first back-step registers
  and redraws the moment HELD fires; further levels gate at 800ms
  (`HOLD_BACK_REPEAT_MS`); physical release re-arms instantly; the old
  blocking 1s `while (encoderButtonState == HELD)` spin is gone. The state is
  deliberately NOT consumed so `holdAnimationStuff()` + `renderLogoRing()`
  keep the hold/reboot sweep visible in menus; the menu depth pads are
  suppressed during hold states and restored by a one-shot repaint on release.
- **F6. OOB index reads**: `selectSubmenuOption` read
  `menuLines[previousMenuSelection[1]]` (can be -1) and
  `previousMenuSelection[menuLevel - 2]` (negative at shallow levels) — now
  guarded via `pickedLineContains()`. `currentAction.fromAscii[subSelection]`
  with `subSelection == -1` guarded at three sites.
- **F7. Encoder PIO FIFO race**: `quadrature_encoder_get_count()` drains the
  PIO RX FIFO with blocking reads and was polled from both cores;
  `rotaryEncoderStuff()` is now wrapped in `mutex_try_enter`
  (`encoderPollMutex`) — loser skips the poll. (An earlier suspicion that this
  mutex caused crashes was disproven; it was F1's memory layout shifting.)
- **F8. Stuck final transition frame**: `leds.show()` drops frames while the
  strip DMA is busy (~9ms/frame), so the transition's landing frame could
  vanish, stranding a mid-blend frame on the LEDs. Fixed with a 50ms linger
  (`MT_LINGER_MS`) that keeps repainting/showing the landed target.
- **F9. Stale-blend stomp on unbracketed painters**: entering value editors /
  actions mid-transition let the old blend stamp over their fresh paint.
  `menuTransitionCancel()` added and called at entry of `selectNodeAction`,
  `getActionFloat/Int/String/Bitmap`, `doMenuAction`.
- **F10. Unbounded waits**: button-release spin in the click handler got
  `jOS.serviceCritical()` + 2s timeout.
- Misc: transitions bracket `yesNoMenu` and `selectSubmenuOption` repaints
  (slot-preview rows excluded — they render nets, not menu text); accent color
  (selected item's logo-ring hue) feeds the colorful types; **Glow is the
  default type** (`MenuTransitionConfig`); 10 types total, `t` cycles them in
  the tuner.

---

## DEBUG INFRASTRUCTURE (working setup, reuse it)

- **OpenOCD server** (Raspberry Pi Debug Probe on the V5 SWD pads):
  `~/.platformio/packages/tool-openocd-rp2040-earlephilhower/bin/openocd -s
  ~/.platformio/packages/tool-openocd-rp2040-earlephilhower/share/openocd/scripts
  -f interface/cmsis-dap.cfg -c "adapter speed 2000" -f target/rp2350.cfg`
  (telnet :4444, gdb :3333, both cores as threads).
- **Fault trapping**: per-core `cortex_m vector_catch hard_err bus_err mm_err
  state_err chk_err nocp_err int_err` via telnet; faults then freeze the core
  for post-mortem `bt` instead of rebooting.
- **Flash over SWD** (no bootloader dance):
  `reset halt` → `program .pio/build/jumperless_v5/firmware.elf verify` →
  `reset run` via telnet. For picotool instead: open the port at 1200 baud
  **with DTR deasserted before open** to enter BOOTSEL.
- **Memory access while running**: `read_memory <addr> <width> 1` /
  `write_memory <addr> <width> <val>` work on a RUNNING target — prefer them
  over `mdw` (which needs halt → resume and starves TinyUSB → CDC drops).
- **Watchpoints find scribblers**: `watch GPIOcolorOverride0` + breakpoint
  commands (`bt`, `continue`) caught F3's culprit in one run.
- **Simulated encoder input** (no hands needed): do NOT bit-bang GPIO/INOVER —
  write the RotaryEncoder.cpp state variables directly over SWD. Full recipe,
  per-build address regeneration, and working scripts live in the
  `jumperless-swd-input` skill (`~/.cursor/skills/jumperless-swd-input/`).
  Short version: forge the click edge {lastButtonEncoderState=PRESSED,
  encoderButtonState=RELEASED, buttonEventTimestamp=fresh TIMERAWL} in ONE
  OpenOCD TCL line (10ms expiry window); pulse encoderDirectionState
  (UP=1 moves the cursor forward) with encoderDirectionConsumed=0; use the
  serial echo of the highlighted line as navigation ground truth.
- **Serial regression script**: `scripts/repro_menufx.py` (modes: `scroll`,
  `idle`, `menufx`) drives the status menu / Menu FX tuner over
  `/dev/cu.usbmodemJLV5port1` and detects port drops — kill any serial monitor
  first.
