# Reading display + Python handoff — where this stands

Working notes for picking this up. Originally on branch `infra-paths`; finished on
`usb-audio-uac2` and merged into `dev` (2026-08-15). Everything below **builds
clean** on both boards. The "still open" list at the bottom is now closed - see
"Closed 2026-08-15"; what remains is the hands-on OLED/probe pass, which needs a
human at the board.

---

## The bug that started it

Running a MicroPython script (the 555 astable placement demo) left the OLED
stuck showing one row's reading — `19` plus a voltage — until reboot. Scrolling
the clickwheel or tapping other rows wouldn't retarget it.

Two independent latches, both confirmed by reading the code:

1. **MeasureMode.** `measurementActive` could only ever be cleared by the
   `switchPosition == 1` branch of `MeasureMode::service()`. Position `-1`
   ("unknown" — what `checkSwitchPosition()` holds when it can't sense, and what
   `jl_set_switch_position()` may legally write) latched it on forever. While
   latched it repainted a row + voltage at 10 Hz and returned `BUSY` with
   "don't process other services", starving everything else.
2. **`Highlighting::showReadingNet`.** Assigned in exactly one place
   (`checkForReadingChanges`) and reset *nowhere* in the entire tree.
   `clearHighlighting()` deliberately skips it, because keeping the last reading
   visible after the highlight fades is a feature. Once positive, the 40 ms live
   updater re-rendered that net forever.

Why a *script* triggered it: `jOS.serviceAll()` runs inside MicroPython's delay
hook (`JumperlessMicroPythonAPI.cpp`), so highlighting and measure mode keep
ticking *during* `time.sleep()` in the script's probe-tap loops. The script's own
taps latched them. The exit hooks restored only `switchPosition` and two service
intervals — nothing display-related — and only on the raw-REPL path.

---

## What has landed

### 1. Python-exit state reset — the actual fix

- **`Highlighting::resetReadingState()`** (new public method) drops
  `showReadingNet`, `showReadingRow`, `lastPrintedNet`, and calls
  `ReadingDisplay::resetLastShown()`. Deliberately **not** folded into
  `clearHighlighting()` — that runs on the highlight timeout, where keeping the
  last reading on screen is the intended behavior.
- **`onPythonSessionEnd()`** (`Python_Proper.cpp`, declared in `Python_Proper.h`)
  is the shared handback, called from **both** script paths:
  - `executePythonFileContent()` — file manager / Apps runs
  - `MpRemoteService::onScriptExecutionComplete()` — mpremote / ViperIDE raw REPL
  It stops measure mode, clears highlighting + reading state *only if something
  is actually latched* (so it's silent after every ViperIDE console line), resets
  script-injected encoder state, and clears `pauseCore2`.
  Both call sites are exception-safe: the raw-REPL hook fires outside
  MicroPython's success/exception branch, and `mp_embed_exec_str()` catches
  Python exceptions internally, so KeyboardInterrupt and uncaught exceptions both
  still run the handback.
- **MeasureMode exit latch**: `else if (switchPosition == 1)` → `else if
  (switchPosition != 0)`. Scoped to `!= 0` on purpose so the 300 ms debounce
  window at position 0 (where `switchStable` is false) doesn't stop an active
  measurement on a switch glitch. `stopMeasurement()` also resets its own probe
  stability tracking now, so an external stop can't instantly re-latch.
- **`switchPosition` restore is dirty-flag gated**. `jl_set_switch_position()`
  sets `switchPositionScriptDirty`; the raw-REPL hook restores its snapshot only
  if a script actually wrote it. Restoring unconditionally clobbered a genuine
  mid-script physical flip, and `checkSwitchPosition()` holds its last value when
  it can't sense, so that bad write did *not* self-heal.
- **`jl_reset_python_display_prefs()`** resets `default_oled_text_size` and
  `oled_copy_print_enabled` (the latter previously had **no** clearer anywhere —
  a script that enabled print-to-OLED kept it until reboot). Called only on the
  file-run path; raw-REPL console lines legitimately persist prefs between execs.

**Left alone on purpose** — this stops the zombie repainters, it does not blank
the screen: connections, DAC voltages, LED overlays, and whatever the script last
drew on the OLED are its *output* and survive.

### 2. `src/ReadingDisplay.{h,cpp}` — one renderer for every reading

`ReadingDisplay::show(name, rowNode, value, value2)` is now the single place a
measurement becomes pixels + serial text. `showName()` is the name-only form.

- The OLED rich layout (5 pt header with name left / row label right, best-fit
  centered value rows, `fixedH = 7` header pin) moved here verbatim from
  `Highlighting.cpp`'s old file-static `showNetReading`.
- The row label is now a **parameter** (`rowNode`) instead of reading the global
  `brightenedNode`, which is what let non-highlight callers in.
- **Dedupe**: identical composed content is dropped rather than repainted, so
  callers that fire every loop pass don't flicker the panel or flood serial.
  `resetLastShown()` forces the next paint.
- `Highlighting.cpp` keeps a two-line `showNetReading` shim
  (`ReadingDisplay::show(name, brightenedNode, ...)`) so all ~20 existing
  highlight call sites are untouched.

Migrated onto it: `VoltageAdjuster::updateDisplay` (Peripherals), the probe
cursor name display (Probing), the UART **static config** display, and
`MeasureMode::updateVoltageDisplay`. MeasureMode's OLED now shows the node name
as header + big voltage, with the second value slot free for a current reading
the way I Sense nets show V over mA.

Also cleaned up in passing: six copy-pasted UART framing blocks collapsed into
`uartConfigText()` / `uartDirectionName()` / `uartRefreshLiveView()`; the one-shot
DAC path unified onto `getDacVoltage(n)`; dead members `lastNetPrinted` and
`currentHighlightedNet` removed; both vestigial `\r`-and-spaces pre-clears in
`highlightNets` deleted.

**Not migrated, on purpose:** the UART *live* Tx/Rx view still uses the `OLEDOut`
scrolling small-text renderer — the fixed rich layout can't express a streaming
byte view. `Peripherals::showMeasurements()` (the `showReadings` continuous ADC/INA
dump) is a different feature and was left alone.

### 3. The pinned live serial line

`ReadingDisplay::emitLiveSerialLine()` / `clearLiveSerialLine()` own the terminal
mechanics. A reading now renders on **two reserved rows above the input line**
(reading row + blank spacer), with the cursor always restored mid-word:

```
3.29 V  row 25          <- pinned reading, rewritten in place
                        <- blank spacer row
> user input + cursor   <- cursor restored here
```

Sequences: `ESC 7`/`ESC 8` (DECSC/DECRC), `CSI 2 A` (CUU), `CSI 2 K` (EL — full
row erase; the default `Ps=0` only erases rightward, which is why the old
spaces-then-`\r` wipe left tails of longer readings behind). Note the literal
split in `"\x1b" "7"` — written `"\x1b7"` the compiler parses one overlong hex
escape.

**Anchor policy.** The anchor is cursor-relative, so anything that scrolls the
terminal moves it. Two things keep it honest:
- Blocking contexts that take over the terminal run their loops on
  `jOS.serviceCritical()`, which dispatches only CRITICAL services — Highlighting
  and MeasureMode are HIGH, so no reading can repaint underneath them.
- Those contexts announce themselves by calling `resetLastShown()`, which drops
  the anchor **without erasing** (a blind CUU+EL would wipe one of *their* rows).
  Wired at: `Apps.cpp:138`, `Menus.cpp:503`, `Probing.cpp:2203`, and
  `Highlighting::clearHighlighting()` — which also covers
  `SingleCharCommands::printMenu()`, since printMenu calls `clearHighlighting()`
  first, and therefore covers the main menu reprint after every command.

---

## Closed 2026-08-15

1. **Guard promotion — done.** The ~17 function-local `static` change-detection
   latches in `checkForReadingChanges()` and `highlightNets()` now live in one
   file-scope `struct LiveReadingGuards { void reset(); }` (`Highlighting.cpp`,
   just above `clearHighlighting()`), bound into the functions with `auto&`
   references so the call sites read as before. `resetReadingState()` calls
   `g_readingGuards.reset()`; `clearHighlighting()` deliberately does not (it runs
   twice per command cycle - `main.cpp` loop top and `printMenu()` - and would force
   a repaint each time). Float sentinels are `kNoReading = -1000.0f`: not 0.0 (a
   live 0.00 V equalled the old sentinel and skipped the first paint) and not NaN
   (`fabs(x - NaN) > t` is always false, which would skip forever).
2. **Jerial hooks — decided: `handleEnter`, not `writeToOutputs`.** The Enter
   echo that scrolls the pinned rows is `stream->print(JERIAL_NEWLINE_OUT)` inside
   `TermControl::handleEnter()`, written straight to the stream - a hook at the top
   of `writeToOutputs` would never see it. `handleEnter` now calls
   `ReadingDisplay::resetLastShown()` right before that echo (once per line, no
   per-byte cost, drops the anchor without erasing). `writeToOutputs` is left
   alone: it is per-byte on the `Print` paths, and the announce-yourself policy
   already covers menus/apps/probe mode.
3. **Width guard — done.** `TermControl` now tracks `prompt_visible_cols` (set by
   `setPrompt`/`setColoredPrompt` from the *plain* prompt - the SGR wrapper takes no
   columns) and exposes `getLineLength()`; `JerialClass::getInputLineColumns()`
   returns prompt + typed. `emitLiveSerialLine()` releases the pin (no output at
   all) once that reaches 70 columns and re-pins after the line is submitted or
   shortened. No `CSI 6 n`.
4. **Two bugs found by the audit, fixed:**
   - `MeasureMode::service()`'s exit path called `stopMeasurement()` *before*
     `ReadingDisplay::clearLiveSerialLine()`, but `stopMeasurement()` →
     `clearHighlighting()` → `resetLastShown()` had already dropped `pinReserved`,
     so the erase was a no-op exactly when a measurement had been live and the last
     voltage stayed frozen above the prompt looking current. Erase now runs first.
     `onPythonSessionEnd()` got the same ordering (erase → stop → clear), gated on
     "something was actually latched" so a plain ViperIDE console line doesn't wipe
     a reading parked on the main terminal.
   - `ReadingDisplay::show()` called `definesToChar(rowNode)` - which returns a
     pointer into one shared static buffer for values it can't map - before
     composing `name`; a caller passing a `definesToChar()` result as the name would
     have seen it replaced by the row label. The name is copied to a local first.
   Also: `lastVoltage`/`lastMeasuredNode` in `MeasureMode.cpp` are `static` now
   (they were exported globals).

**Verified on hardware (2026-08-15):** the final build boots, runs the HIL suite,
and a raw-REPL script that flips the switch position hands it back cleanly on
exit (no stray output on port 1 after exit). **Not** verifiable without hands -
`probe_tap()` is a stub and the clickwheel injection doesn't reach the highlighter
from a raw-REPL exec - so the zombie-repaint check with a *latched* measurement /
highlight, and the pinned line's ESC7/CUU2/EL2/ESC8 framing during a real reading,
are on the checklist below. The ordering fixes themselves are straightforward and
were reviewed against the call chain (`stopMeasurement()` → `clearHighlighting()`
→ `resetLastShown()`).

## Hardware verification checklist

**The bug itself.** Reproduce first, and identify which painter is stuck — the
two layouts are visually distinct: a bare two-line size-2 `19` over a voltage is
MeasureMode; a 5 pt header with name left / row right over big centered values is
the shared renderer. Then, after the fix, on **both** script paths (ViperIDE raw
REPL and Apps → run a `.py`): scrolling and probe taps must retarget normally
with no zombie repaint. Do the first check with the probe **off** the board — a
re-latch with the switch physically at measure and the tip on a row is correct
behavior, not a regression.

- Abnormal termination: Ctrl-C mid-`probe_read_blocking` (raw REPL) and
  clickwheel-hold KeyboardInterrupt (file run) both still hand back.
- Script output survives: nets, `TOP_RAIL` voltage, overlays all still there.
- Feature preserved: with no Python involved, tap a net and let the highlight
  time out — the OLED must keep showing the last reading.
- Expected, not a bug: typing ViperIDE console lines while actively measuring
  (switch at 0) briefly stops and re-latches the measurement per line —
  `onPythonSessionEnd()` fires on every console exec.

**Display regression pass** — every node type, checking OLED and the pinned
serial line agree: plain wired net, GND, top/bottom rail, DAC 0/1, ADC, GPIO
in (including `FLOATING`, which is what forced best-fit sizing), GPIO out, I2C,
PWM, an I Sense +/- pair, UART (static config *and* live Tx/Rx traffic).
Then: rail/DAC adjuster via encoder click (should no longer flicker — dedupe
killed a per-loop repaint flood); measure mode's new header+voltage layout, and
that the oscilloscope screen is untouched; probe cursor on rails/logo pads
(name-only, and note the serial `>>>> ` prefix is gone).

**Terminal.** Type a command while readings refresh and confirm nothing eats the
input line; press Enter (pinned rows scroll — self-heal on menu reprint); trigger
a menu reprint and confirm no stale reading is left mid-output; run a clickwheel
session (screen clear + reprint) and confirm no artifacts; scroll back through
history and confirm command echoes sit directly above their own output.

---

## Files touched

| File | What |
|---|---|
| `src/ReadingDisplay.{h,cpp}` | **new** — the sink, dedupe, pinned serial line |
| `src/Highlighting.{h,cpp}` | `resetReadingState()`, shim, UART helpers, dead members, `\r` idioms |
| `src/MeasureMode.{h,cpp}` | exit-latch fix, self-contained `stopMeasurement()`, display migration |
| `src/Python_Proper.{h,cpp}` | `onPythonSessionEnd()` + file-run call site |
| `src/MpRemoteService.cpp` | raw-REPL hook: gated restore + handback |
| `src/JumperlessMicroPythonAPI.cpp` | dirty flag, `jl_reset_python_display_prefs()` |
| `src/Peripherals.cpp` | `VoltageAdjuster::updateDisplay` migration |
| `src/Probing.cpp` | cursor name display migration, `resetLastShown()` |
| `src/Apps.cpp`, `src/Menus.cpp` | `resetLastShown()` on blocking-context entry |

Committed on `dev` after the automated verification above; the OLED/probe-tap pass
below is Kevin's.
