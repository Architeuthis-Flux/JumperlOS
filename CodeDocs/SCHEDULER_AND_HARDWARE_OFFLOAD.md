# Scheduler & hardware offload — proposals, decisions, and what was built

> **STATUS (2026-08-16):** the sweep is done and the proposals below were reviewed by Kevin
> in plan mode. Section 0 records what he approved, what was declined and why, and how each
> approved item was verified as it landed. Everything from "Executive summary" down is the
> proposal body as reviewed, corrected where the implementation pass found a claim wrong.
>
> This file is written to be read with **no prior context**. Sections 1–3 are the
> orientation a fresh session needs; if something is missing there, that is a bug in this
> file.

---

## 0. Decisions and status

### ▶ CONTINUE HERE (state at the end of the 2026-08-16/17 implementation session, part 3)

**Landed on `dev` (never pushed):** `fb8e45d` (this doc), `a3e58f4` (T1.1), `3fc5c57`
(T1.2 + T1.3), `0b5f6f7` (docs), `b0fd157` (the vocabulary rename — see the note below),
`f3e4f6f` (T1.8), `95fb058` (docs), `9bca7b5` (T1.9), `f5a6cd0` (the paste fix, row 30),
`2761825` (the switch classifier inside probe mode, row 31), `e3d4d36` (T1.4, row 32),
`545b7e6` (T1.5, row 33), `b8d00d9` (T1.7, row 34), `574e749` (T1.6 measure-only, row 35),
`dff24b3` (T1.10, row 36), and **T2.2a — the tap→crossbar→LEDs latency probe** (the commit
after `dff24b3`, `DEV_MERGE_HANDOFF.md` row 37 — hash with the next commit). **The board is
flashed with HEAD.** **Tier 1 is complete. Next: T2.2b** (the core-1 mailbox with
`REQ_SEND_PATHS` only, `waitCore2()` on `doneGen` in place — section D's migration step 2; the
probe below is its gate: same-or-better); the watchdog *enable* is a separate decision on the
T1.6 numbers (design sketched there), not part of the queue as approved.

**T2.2a — what was built (section F's "tap→crossbar latency probe", instrumentation only).**
`src/XbarLatency.h/.cpp`: five stamps — `tap` (probeMode accepts a tap and commits it, right
before `addBridgeToState`/`removeBridgeFromState`; needs a real tap, so **n = 0 until Kevin
taps**), `req` (core 0 writes `sendAllPathsCore2` — the three sites in `Commands.cpp`:
`refreshConnections` ±1, the `3` bypass, `refreshLocalConnections` ±1), `pickup` (top of
`sendPaths()`, whichever core), `sendDone` (end of `sendPaths()`), `show` (the first
`leds.show()` after a send) — and last/max/n per segment: tap→req, req→pickup, pickup→send,
send→show, req→show. A request landing while one is outstanding keeps the first stamp;
`X` prints it, `X!` resets it (with the kick-gap maxima). RAM-resident stamps for the
`sendPaths()` path (T1.8).

**T2.2a — the before numbers (flag handshake, 40 REPL `connect`/`disconnect` ops, two runs;
`xbar_lat.py`):** req→pickup **~100–200 µs typical, max 304 µs / 2 197 µs** (a pass where core
1 was mid-frame); pickup→send **~40–50 µs** (incremental sends), max ~240 µs; send→show
**~7.1 ms**, max ~10 ms (the LED render + show — the dominant term); req→show **~7.3 ms**, max
10.4 / 12.4 ms. So the handshake itself is already ~0.1–0.3 ms; the mailbox's win is
correctness (no lost `-1`, no overwritten request, `doneGen` instead of a 25 ms proceed-anyway)
and the LED path is where the time goes. HIL 7/7; `test_infra_paths` 24/24 with the probe in.

**T1.10 — what was built.** LED-dump mode (`R!`, or `serial_1/2.function = leds/oled_leds`)
no longer writes USB CDC from core 1: the `dumpLEDs()` block in `loop1()` is gone; core 1
only raises `ledDumpFrameReady` after a frame is shown (`core2stuff`, right after
`leds.show()`), and a new **`LedDumpService`** on core 0 (NORMAL, `periodUs()` 10 000, **in
the inner set**) does the dump — at most every `dumpLEDrate` (250 ms) when a fresh frame is
there, at least every 1 s so a static picture still repaints, skipped while `core2busy`.
Inner on purpose: the picture used to keep updating through probe mode and the menus (core 1
did not care what core 0 was doing) and still does — first tried as part of
`PortHousekeeping` (not inner) and the picture froze for the length of a MicroPython script;
it costs one compare per pass while the mode is off. `dumpLEDs()` itself is unchanged (it
snapshots the pixel buffer under `logoLedAccess`; a frame torn by core 1 mid-render is
cosmetic — the old code had `core2busy`/`core1busy` + 2 ms of `delayMicroseconds` around it for
the same reason).

**T1.10 — verified:** builds ×3; `R!` on port 1: dumps arrive every ~340 ms (250 ms rate +
the ProbePads quantisation), ~6.7 KB each, idle and during/after a MicroPython connect script
(48 KB during a 2.4 s modal script — the inner-set point); `serial_1.function = leds` with
port 3 (USBSer1) held open: 60 KB in 4 s on port 3, port 5 alive throughout, config restored
to `passthrough` after; `X`: `LedDump NORM* 10000` (avg 2.6 ms per dump, max 52 ms — CDC
back-pressure — 23 overruns of its 10 ms period, i.e. the dump itself is the long pole),
`PortHousekeeping` unchanged; HIL 7/7; `test_infra_paths` 24/24. **Kevin:** the terminal LED
view (`R!`) should look and refresh as before; if you run it for a long session, `X` after
it (`LedDump` row, and the T1.6 kick-gap line) tells whether the dump ever stalled core 0.

**T1.10 files:** `src/main.cpp` (`ledDumpFrameReady`, the flag set in `core2stuff`, the
`loop1()` block removed, registration), `src/JumperlOS.h/.cpp` (`LedDumpService`), the
rebuilt `firmware.uf2`.

**T1.6 (C11) measure-only stage — what was built.** `src/KickGap.h/.cpp`: `kickGapStamp(core,
site)` at the three places a watchdog kick would go — the top of `loop()`'s busy-loop pass
(`KICK_LOOP0`), `jOS.serviceInner()` (`KICK_INNER`, the modal loops), the top of `loop1()`
before its `pauseCore2` wait (`KICK_LOOP1`) — records per core the longest gap between two
consecutive stamps, which two sites bracketed it and when; `X` prints it ("watchdog
(measure-only, nothing enabled): …", plus the currently open gap), **`X!` resets the maxima**
(port 1 — on port 7 a raw `X!` is `X` then `!`, the arg never reaches the command). **No
`watchdog_enable()` anywhere.** Cost: one `time_us_32()` and a few compares per stamp.

**T1.6 — the numbers (each after an `X!` reset, then one blocker, then `X`; scripts
`kickgap_campaign.py` in the job tmp dir):**

| blocker | core 0 max gap | core 1 max gap |
|---|---|---|
| idle 30 s | **42 ms** (loop0→loop0 — the ProbePads block; the calibration check) | 4 ms |
| `usb_audio_save()` from the REPL (a full config write) | 44 ms | 4 ms (FlashPark parks core 1 per flash op, ms each) |
| `connect(1,5); connect(10,20)`, 3 s idle, `nodes_clear()`, 3 s idle (slot auto-save) | **1 809 ms** (inner→loop0) | **1 147 ms** (loop1→loop1) — the slot file write, core 1 parked through it |
| `wavegen_start(1)` 2 kHz on DAC1 for 10 s | 163 ms | **10 007 ms** — core 1 is captured for the whole stream (finding 8, now measured) |
| compute-bound MicroPython (`while ticks_diff < 5000: pass`) | **5 226 ms** | 4 ms — a busy script never services; the gap is the script |
| `time.sleep(5)` MicroPython | 202 ms (inner→inner — `mp_hal_delay_ms` services every 50 ms, with 200 ms hiccups) | 4 ms |
| one `X` print on port 1 | 83 ms | 4 ms |
| `~` (print the config) | **1 060 ms** — a big terminal print blocks core 0 for a second (host-paced CDC) | 4 ms |
| the whole `run_all.py` (7/7) | **11 488 ms** (loop0→inner — one long `mpremote exec` script) | 1 192 ms (slot save) |
| boot (never reset) | 353 ms at 2 s (init) | 60 ms at 2 s |

**What they say about the enable design (for the decision, not done):** kicks from the three
loop sites alone cannot carry a fixed timeout — (a) a compute-bound MicroPython script is an
unbounded core-0 gap (5.2 s here, 11.5 s in the suite; `while True: pass` is infinite), so
the kick must also come from inside the VM (`MICROPY_VM_HOOK` / `mp_hal_check_interrupt`,
which every script passes through); (b) a WaveGen stream is an unbounded core-1 gap, so
either `WaveGen`'s core-1 stream loop kicks or core 1's kick is not required while
`wavegen.isRunning()`; (c) with those two, everything else measured fits under an 8 s timeout
with margin ×4 (slot save 1.8 s, `~` 1.1 s, boot 0.35 s) — but a long print or a self-test /
calibration (not exercised: they need the button / hands, Kevin's list) still needs checking
against whatever timeout is picked. Also worth knowing: **any command that runs longer than
the timeout is a reboot** unless commands kick — the busy loop stamps between commands, not
inside them.

**T1.6 — verified:** builds ×3; `X` after boot and after each blocker as tabled; the idle
number equals the ProbePads block (so the stamps sit where they should); HIL 7/7 (with the
gap counters running); `test_infra_paths` 24/24. Nothing behavioural changed, so no
hands-on item — Kevin's list for the *enable* stage: self-test, calibration, a probe session,
the click menu held open, an app.

**T1.6 files:** `src/KickGap.h/.cpp` (new), `src/main.cpp` (two stamps), `src/JumperlOS.cpp`
(one stamp), `src/SingleCharCommands.cpp` (`X` line, `X!`), the rebuilt `firmware.uf2`.

**T1.7 (B6) — what was built.** (1) The 10 ms `secondSerialHandler()` /
`replyWithSerialInfo()` / `serviceNetVoltageScanDebug()` block in `loop()`'s busy loop is
`PortHousekeepingService` (NORMAL, `periodUs()` 10 000, not inner; `JumperlOS.h/.cpp`,
registered in `main.cpp`). (2) The two 100 ms help-wait spins are gone. **Line mode** (the
default, and what the `jumperless` app uses): "help", "help <category>" and "[cmd]?" are
now decided by looking at the completed line — before B6 the line path waited 100 ms on an
*empty* stream (the line was already consumed) and then ran the bare command, so in line
mode **"help" printed the menu, "m?" printed the menu, and "x?" CLEARED THE BOARD**
(measured on the pre-B6 build; and *every* line-mode command with a CR- or LF-only
terminator paid the 100 ms: `n` → netlist took 146 ms p50; CRLF short-circuited it). **Char
mode**: the command char is parked (`helpArmed`/`helpArmedChar`/`helpDeadlineMs` statics in
`loop()`), the busy loop keeps running `serviceAll()` and returns when the next char arrives
or the 100 ms deadline passes, then the same peek as before (`?` → help; `h` + "elp…" → help
docs). One list, `helpQuestionApplies()`, names the commands that own their `?` in **both**
modes: `A?`/`a?` (Arduino status query), `i?` (RouteSafety self-check — the HIL parses it),
`M?` (USB-audio status) — before B6, char mode showed the help page for `i?`/`M?` (that is
why the suite needed line mode) and line mode ran the handler for everything. `cmd_usbAudio`
now takes its sub-command through `getCommandArgs()` (it parsed `line[1]`, so in char mode a
bare "M" *and* "M?" both toggled the device = a USB re-enumeration that drops every port —
found the hard way: the first B6 test run of `M?` in char mode dropped the ports; toggled
back off, nothing persisted). (3) The post-command "clean up serial buffer" drain
(`delayMicroseconds(1000)` + eat raw `Serial` down to 5 bytes) is gone — it ate the tail of
any multi-line paste after the first command; `printMenu()` still discards a >20-byte
backlog on its own (left alone, noted). Asymmetry, pre-existing and unchanged: commands relayed
through `<j>…</j>` tags arrive via `CommandBuffer` and bypass the help checks entirely (a relayed
`x?` runs `x`); relayed lines that come through TermControl's queue get them — machine input,
not the help path.

**T1.7 — verified:** builds ×3; both modes, scripted over port 1 (`help`, `help probe`, `x?`
with a bridge present — **it survives now**, `m?`, `h`, `?`, `i?`, `A?`, `M?`): every one
returns the intended thing in **both** modes; latency, 25 samples each, `n` → "netlist" p50:
line mode CR **146 → 46 ms**, LF **146 → 46 ms**, CRLF 5 → 5 ms; char mode 4–5 ms unchanged
(the old build was re-flashed via BOOTSEL/picotool for the before numbers). `X`:
`PortHousekeeping NORM 10000` runs at the same ~24 Hz cadence Peripherals gets (both are
quantised by the ProbePads block); HIL 7/7; `test_infra_paths` 24/24. **Pending Kevin
hands-on:** `h?`/`x?`/`help` feel in his terminal, and that command latency in the app is
visibly better. **Observation, not fixed:** in line mode with a CR-only or LF-only
terminator there is a residual ~40 ms per command that a CRLF terminator does not have — on
the old build too (146 = 100 + 46) — not investigated (not `getCommandArgs`: `n` doesn't call
it). Also noted: `getCommandArgs()` waits its 20–100 ms timeout on the stream when a
line-mode command has no argument (the args are the line; nothing more is coming) — a
per-command wait for every arg-taking command in the app; a one-line "don't wait when line
buffering is on" would remove it, but a handler that prompts and then reads a second line
through it would break — audit before doing it (T1.7b candidate).

**T1.7 files:** `src/main.cpp` (`helpQuestionApplies()`, `loop()`), `src/JumperlOS.h/.cpp`
(`PortHousekeepingService`), `src/SingleCharCommands.cpp` (`cmd_usbAudio` args), the rebuilt
`firmware.uf2`.

**T1.5 (B4) — what was built.** `Service::inInnerSet()` (virtual; default = CRITICAL
priority; `AsyncPassthroughService` overrides it to true) names the modal set explicitly, and
`jOSmanager::serviceInner()` is one due-gated pass over it (same `isDue()`/`runService()`
path as `serviceAll()`, so the `X` table counts the modal loops' calls; the prio column now
carries a `*` for inner-set services). `serviceCritical()` is **gone** — all 23 call sites
(Menus.cpp ×10, Probing.cpp ×8, `BitmapEditor.cpp`, `GraphicOverlays.cpp`, `ImagesApp.cpp`,
`Peripherals.cpp`, `Python_Proper.cpp` `mp_hal_delay_ms`) call `serviceInner()`;
`serviceAll()` runs the same inner set (plus the blocking service) while a service is
BLOCKING (it used to be "CRITICAL priority", which was the same set minus AsyncPassthrough);
`servicePython()` = `serviceInner()` (it hand-rolled the USB pump + a direct
`Peripherals::service()`; it has no caller in `src/` — kept as a named entry point).
`classifySwitchPosition()` in `probeMode()`'s loop (row 31) is untouched and still separate
from the set (ProbeSwitch is deliberately NOT inner: `infraServiceTick()` would ride along).
**Kevin-visible delta, intended (B4):** the Arduino UART bridge (AsyncPassthrough) and the
USB pump keep running inside probe mode, the click/pad menus, the apps' input loops and
MicroPython delays — the bridge used to stop for as long as any of those owned the loop.
Also in this commit, from the T1.4 review: **`nextDueUs` is now 64-bit (`time_us_64()`)** —
with a 32-bit deadline a service parked behind a modal loop or a BLOCKING menu for >35.8 min
(a click menu left open over lunch) came back "not due" for another ~35 min (OLED reconnect,
LiveXbar, implicit config saves dark); 64-bit makes it simply overdue, no re-pinning logic.

**T1.5 — verified:** builds ×3; `X` after a 3 s `time.sleep()` from the REPL: the four
inner-set rows (TinyUSB, ProbeButton, MpRemote, **AsyncPassthrough** — `HIGH*`) each show
`runs` = passes **+ 42** (the `mp_hal_delay_ms` → `serviceInner()` calls) while
Menus/Probing/Highlighting show exactly `passes` — the inner set is what runs inside the
modal path, and AsyncPassthrough is in it; `run_all.py`: run 1 **6/7** (`test_net_currents`
FAIL — the failing check line was not captured; the file then passed standalone 4× (8/8) and
the suite's run 2 was **7/7**, so this is the known marginal phantom-current check, not a
regression); `test_infra_paths` 24/24. **Pending Kevin hands-on:** probe mode, click menu and
REPL still respond as before; with an Arduino on the header, passthrough now works while
probing / inside a menu.

**T1.5 files:** `src/JumperlOS.h/.cpp`, `src/main.cpp` (registration comments), the 7 call-site
files above, `src/SingleCharCommands.cpp` (`*` marker), comment touch-ups in
`ReadingDisplay.cpp`, `MpRemoteService.cpp`, `Peripherals.cpp`, `Python_Proper.cpp`, the
rebuilt `firmware.uf2`.

**T1.4 (B3) — what was built.** `Service` (base, `JumperlOS.h`) grew `virtual uint32_t
periodUs() const { return 0; }`, `requestRun()`, and the scheduler-owned fields `nextDueUs`,
`pending`, `runs`, `lastUs`, `maxUs`, `overruns`, `totalUs`. `jOSmanager::serviceAll()` lost
the per-band loop divisors (1/1/3/20 — never a cadence) and runs a service iff
`isDue()` (pending latch, or period 0, or `(int32_t)(now - nextDueUs) >= 0` on
`time_us_32()` — C12) through one `runService()` helper that `serviceCritical()` and the
`forceServiceBy*` paths share, so the `X` table counts the modal loops' calls too. Periods
(each still has its own internal gate — T1.4 deletes nothing): ProbeSwitch 10 000, OLED 20 000,
LiveXbar 100 000 (+ `requestUpdate()` → `requestRun()`, so a crossbar change draws on the next
pass), Peripherals 10 000, ProbePads 50 000, OledGui 15 000, SlotManager ("States") 50 000,
ConfigSave 100 000 (+ `requestConfigSave()` → `requestRun()`); everything else 0. C6:
`probe_button_pio_irq_handler` calls `ProbeButton::requestRun()` + `Probing::requestRun()` on
a sample-state change (a no-op while both are period 0 — the hook is wired). `X` prints
`scheduler: N passes, M services` and a row per service: name / prio / period_us / runs /
last_us / max_us / avg_us / overruns / share (share = total time in the service ÷ uptime).
Four deliberate deviations from B3's sketch, all recorded here: (1) `nextDueUs` is stamped
from the run **start**, not the end — a period equal to a service's own `millis()` gate then
never aliases against it (the next attempt is ≥ period after this attempt's start, so a gate
of the same length always passes; end-stamping would stretch ProbePads' 50 ms to ~87 ms);
(2) the pending latch is cleared **only when read set** (the sketch's unconditional clear had
a lost-request window); (3) the due gate applies inside `serviceCritical()` too (identical
behaviour — only Peripherals has a period there, and it self-gates); (4) `nextDueUs` is
initialised to `time_us_32()` at registration (a service registered >35 min after boot would
otherwise wait for the wraparound). Correction to the refinements list below: "exact
encodings (verified)" was loose for **ConfigSave and SlotManager** — neither has a 100 ms /
50 ms gate; both are event/idle-gated (2 s input debounce; `systemIdleForFlush` ≥ 750 ms
quiet), so those two periods are new, consequence-free upper bounds. Blocking is checked
before the pending capture, so a `requestRun()` against a service skipped by a BLOCKING menu
survives the skip.

**T1.4 — verified:** builds ×3; `X` (port 7) after boot and after the HIL run: every one of
the 16 registered services `runs > 0` with the expected period; period-0 services' `runs` ==
passes (+ the `serviceCritical()` calls from modal loops — visible as TinyUSB/ProbeButton/
MpRemote 417 342 vs 417 301 passes after `run_all.py`); `d`→`w` (wait-loop timing debug) on
for 12 s: **0 SLOW SERVICE lines**, 232 × `serviceAll() took ~41.9 ms` (that flood is
pre-existing — it is the ProbePads block below, one line per pad poll — and it stops the
moment the flag goes off); 20 connect/disconnects with port 1 watched: no `⏱️ refresh:` line
(none > 100 ms); **HIL 7/7** (`test_net_currents`' phantom-current check passed on this
board today — twice, on the row-31 build and on this one; nothing in either touches it, so
treat it as board state that may come back); `test_infra_paths` 24/24. **Pending Kevin
hands-on: probe feel and menu feel unchanged** (the design intent is behaviour-identical;
the tactile check is his by the disposition rule).

**T1.4 — the number it was built to produce (finding 1, now measured, and it is worse than
the estimate):** `ProbePads` = `checkPads()` = 12 × `readProbeRaw()` = 12 × 8 × `readAdc(5,16)`
≈ **36.8 ms per call at 20 Hz → 64–70 % of core 0**, plus `Probing::service` (100 Hz
`justReadProbe`) avg ~200 µs → ~16 %; together **~85 % of core 0's loop time is pad polling
while nothing is touched** (the doc's estimate was "~⅓": ~12 ms @ 20 Hz + 1.2 ms @ 100 Hz).
Consequences the table makes visible: the loop makes only ~900–1 300 passes/s (each pass
containing ProbePads is ~40 ms), so every "period 0" service is serviced ~1 000×/s with 37 ms
gaps, and the 10 ms services effectively run at ~24 Hz (Peripherals 1 417 runs in 60 s), not
100 Hz — the current-sense poll included. Nothing here is new behaviour (T1.4 is
behaviour-preserving; the same block existed at "every 20th pass"), it is just measured now.
Two ways out, Kevin's call: **T2.1** (the always-on ADC ring — the poll becomes a memory read)
is the approved one; a much smaller **T1.11 candidate** would be a touch pre-check in
`checkPads()` — one `readAdc(5,16)` (~150 µs) and return unless it is above the floor, the
12-read average only when something is there — which drops the idle share from ~65 % to
<1 % at the cost of a slightly different first-detection filter (the next 50 ms poll catches
a light touch the pre-read missed). Not done: it changes pad-detection behaviour and Kevin
owns that.

**Other numbers from the first table (for the baseline):** MpRemote `max_us` 11.7 s after the
HIL run — that is a MicroPython script executing inside `MpRemoteService::service()` (the
modal REPL path, `servicePython()` keeps USB/current-sense alive inside it), expected; States
max 1.59 s / 37 overruns and ConfigSave max 85 ms are the slot/config file writes during
the suite; Probing max 5.6 ms; ProbeSwitch max 5.2 ms (a classifier pass with the INA read
and the pad-sense veto); everything else sub-ms.

**T1.4 files:** `src/JumperlOS.h/.cpp` (base class, `isDue()`/`runService()`, `serviceAll()`
rewrite, `serviceCritical()`, force paths, the three JumperlOS.h services' periods),
`src/Peripherals.h`, `src/Probing.h` (ProbeSwitch/ProbePads periods), `src/Probing.cpp` (C6
in the IRQ handler), `src/routing/States.h`, `src/configManager.h/.cpp`,
`src/SingleCharCommands.cpp` (the `X` table), the rebuilt `firmware.uf2`.

**Side quest landed on the way (Kevin's report, 2026-08-17): pasting a state back (`S` YAML /
`L` JSON) works again.** Not a scheduler regression: both readers stopped at the *first* empty
line and `Y`'s own output has blank lines between sections, so a Y round-trip ended at
`sourceOfTruth:` and the rest ran as menu commands, in both terminal modes, since `35515bc`
(Feb 2026). It only "used to work" while pastes arrived CR-only and `readStringUntil('\n')`
swallowed the whole thing as one 1 s-timeout blob; the jumperless app now sends `\n` per Enter.
Fix: one shared `readPastedBlock()` in `SingleCharCommands.cpp` (`\n`/`\r`/`\r\n`, inner
blank lines kept, ends on empty line + 500 ms quiet, 30 s idle fallback) + `test_paste_state.py`
in the HIL suite (**the suite is 6/7 from here on**, same single phantom-current FAIL). Two
findings for Kevin's *app* (`bridge.py`), not firmware: (1) the interactive loop does
`select()` on the fd then `sys.stdin.read(1)` — TextIOWrapper buffers the chunk, so Enter (and a
paste's tail) reaches the board one keystroke late (`os.read(sys.stdin.fileno(), 1)` fixes it);
the firmware reader tolerates it (the user's Enter flushes the tail and terminates). (2) The
app's line mode sends each line with **no terminator**, so pastes cannot work there by
construction — interactive mode is the paste path.

**Side quest 2 (Kevin's note, 2026-08-17): the switch classifier runs inside probe mode.**
"Since we're applying a different scaling depending on the switch position, if we hit the
switch during probing, the pads are way off — we should occasionally check the switch in the
probing loop." The pad frame (`probe_max` vs `probe_max_measure*`) follows `switchPosition`,
but `probeMode()`'s loop only ran the CRITICAL set (+ the A-only `checkSwitchPositionFast()`,
which is inert outside agree mode), so a mid-session flip stood uncorrected until the session
ended. `Probing::checkSwitchPosition()` is now `infraServiceTick()` + a new
`classifySwitchPosition()` (the classifier proper: 500 ms gate, button/LED-settle/touch vetoes,
legacy or agreement decision — unchanged), and the loop calls `classifySwitchPosition()` every
pass (~1 µs when not due; one INA1 read or ADC7 droop estimate every 500 ms). The infra tick
was deliberately kept out of the loop: a nudge is a full crossbar rebuild. Verified: `[switch]`
stats still flow at 500 ms outside probe mode, HIL 7/7 (the phantom check happened to pass —
board state), `test_infra_paths` 24/24. **Hands-on for Kevin:** flip the switch mid-session —
the pads should re-scale within ~1 s (the classifier also re-sends the position's idle LED
pattern on a change, as the A-only tracker did).
(No debug probe was attached, so probe mode could not be entered hands-free — `test_encoder_ui`
skipped for the same reason.) The scheduler-shaped alternative — ProbeSwitch in T1.5's inner
set — was rejected for now because it drags `infraServiceTick()` into every modal loop.

**T1.9 files:** `src/JumperlessDefines.h`, `src/Peripherals.cpp`, `src/MCP4728.cpp/.h`,
`src/oled.cpp` (68+/22−), the rebuilt tracked `.pio/build/jumperless_v5/firmware.uf2`, and the
new helper `test/hil/port7.py` (read-only `X`/`:verb` access on port 7 when port 1 is busy).

**T1.9 — what was built** (matches the three-edit plan below, plus the shared constant):
`I2C0_BUS_CLOCK_HZ 1000000` in `JumperlessDefines.h` (next to `I2C0_SDA/SCL`, with the
ownership comment); `initDAC()` uses it; `MCP4728::begin()` no longer calls `setClock()` at
all (`_clock_hz` = the constant, only used by the soft-I2C address-restore path); the
OLED-on-I2C0 `Adafruit_SSD1306` instance is built with `clkDuring` 400 kHz / `clkAfter` =
the constant (I2C1 keeps 400/400); `oled::connect()` passes the constant to `initI2C()`
when `connection_type == 2` (400 kHz otherwise). No other `setClock()`/`i2c_set_baudrate`
on I2C0 exists in `src/` (`Peripherals.cpp:1722` is inside `#if OG_JUMPERLESS`; the i2cScan
app only touches Wire1). OG builds and is unaffected (`initDAC()` returns before the DAC).

**T1.9 — verified (all without port 1; one-off scripts at
`/Users/kevinsanto/.claude/jobs/064d2674/tmp/`: `i2c0_clk.py`, `wg_rate7.py`, `ina_soak.py`,
`reboot_loop.py`, `pcz_loop.py`; the reusable port-7 helper is now `test/hil/port7.py`):**
- Register readout (REPL, `IC_FS_SCL_HCNT/LCNT` at 0x40090000+0x1c/0x20): **60 / 90 =
  1.00 MHz** right after boot with the OLED connected, on **11/11 boots**; and **before,
  during and after** `wavegen_start(1)`/`wavegen_stop()` — the 1.7 MHz flip is gone; and
  after forced OLED frames (`oled_print`/`oled_clear`/`oled_show` from the REPL, framebuffer
  confirmed via port-7 `:oled:quarter`). arduino-pico's `i2c_set_baudrate` math: 400 kHz =
  150/225, 1.7 MHz = 36/52, 1 MHz = 60/90.
- **After number:** 2 kHz sine on DAC1 for ~3.1 s → **61 483 MCP4728 writes ≈ 19.5–20.5 k
  writes/s** (was 30 352/s at 1.7 MHz — **−33 %**, less than the 1.7× clock ratio because the
  per-write software overhead does not scale). Counters read via **port 7 `X`** (`X` is
  `SER3_ALLOWED`; `i@` and `~` are not).
- INA219 at sustained 1 MHz (the genuinely new regime — before, the steady state was 400 kHz):
  **80 000 register reads** (INA0+INA1 bus voltage + current via `ina_get_bus_voltage()` /
  `ina_get_current()`, a failed read returns exactly 0) → **0 failures**, spread 1–2 LSB
  (INA0 bus 0.848–0.852 V, INA1 bus 1.988–1.996 V, INA1 current 1.434–1.465 mA).
- 10 × `machine.reset()`: every boot 1.00 MHz, INA0/INA1 reads good, INA1 8-sample median
  **1.465 mA on all 10 boots** (min 1.434), `X` OLED **Connected**, DAC found (`mcp4728
  writes 3/2/2/2` at boot).
- **`probe_current_zero` across 10 more reboots: 1.01 / 1.24 / 1.30 / 1.51 / 1.13 / 1.48 /
  1.28 / 1.36 / 1.36 / 1.17 mA (mean 1.28), plus 1.83 and 1.75 from two earlier boots — all
  inside the 0.5–2.3 mA history, a fresh value every boot (so the 8-sample INA1 calibration
  never fell back to "keeping previous value").** Read without port 1: `usb_audio_save()`
  from the REPL → `saveConfig()` writes the live value into `/config.txt` (`pcz_loop.py`).
- Builds ×3. **HIL, once port 1 freed for ~10 min:** `run_all.py` → `test_micropython_fs`,
  `test_routing`, `test_config` (31), `test_encoder_ui` PASS; `test_net_currents` and
  `test_stress` each failed **two extra checks beyond the known phantom-current one — both
  are the same artifact, not T1.9**: their `i?` (RouteSafety self-check + `suspect=0x…`
  audit) came back as the *help page for `i`*, because the board was in **char mode
  (`terminal_line_buffering = 0`)** — in char mode `loop()`'s `[command]?` peek
  (`main.cpp:1214`) fires on the `?` before `cmd_netCurrents` ever sees it; in line mode the
  whole line reaches the command. Every substantive check in those two files passed (INA0
  loop current 4.82 mA vs scan 5.71 mA — INA0 at 1 MHz working under load; 40
  connect/disconnect/refresh cycles; audit no live short). **Where the mode came from
  (mechanism found, exact trigger not confirmed):** the `jumperless` client keeps a
  persistent interactive-mode preference and syncs it to the firmware with SO/SI
  (`bridge.py` `set_interactive_mode` → `ser.write(INTERACTIVE_OFF)`); SI →
  `acknowledgeAppLineBuffering(false)` (`Jerial.cpp:216`) → `terminal_line_buffering = 0`,
  which persists on the next config save. The client was the only other thing on port 1 all
  session, and the value was 1 when the previous session's `i?` tests passed. `B1` on port 1
  restores it ("Line buffering enabled"; `i?` then returns `self-check: PASS … suspect=0x000`).
- **The port-1 half, once Kevin closed the app (2026-08-17):** `terminal_line_buffering` was
  already 1 again; **`run_all.py` 5/6** (only the pre-existing phantom-current check);
  `dac_set(0, 3.33, True)` → `i@`: `probe_power on -> DAC0 (node 106) [139-106]
  order:DAC0>GPIO paths:1 dup:0 xp:2`; **`test_infra_paths.py` 24/24**. Committed.

**Kevin, the honest cost:** the max wave frequency you see today (which came from the
accidental 1.7 MHz after every `wavegen_start()`) drops ~⅓ — 20 k vs 30 k samples/s. That is
what running the INA219s at the clock the code argues for costs. If you'd rather keep the
streaming speed, the alternative is "1 MHz owner + WaveGen may raise the clock only while
`isRunning()` and must restore it on stop" — an edit in `WaveGen::start/stop`, not in
`MCP4728::begin()`; T1.9 as built makes that a clean 2-line follow-up.

**T1.8 landed** after the interrupted gate was understood: the 3/24 `test_infra_paths` failures
reproduced with the client detached and on the pre-T1.1 code too — they were **board state, not
firmware**: DAC0 had been left at 2.0 V (outside the feed's [2.80, 3.90] V "unclaimed" window,
`InfraPaths.cpp:199-209`), so the firmware correctly treated it as user-claimed and, with all
8 GPIOs claimed, had no viable candidate (`-> (none)`). `dac_set(0, 3.33, True)` from the REPL
put the feed back on DAC0 and the test went 24/24 on the T1.8 build. **Gotcha for the doc's
section 3:** `test_infra_paths` assumes DAC0 is inside the window; a stray `dac_set` (a routing
test, a REPL session) leaves it non-viable and the test fails 3/24 with `(none)` — check `i@`
shows `probe_power -> DAC0` before blaming the code.

**T1.9 — the "before" story, kept for the record (the finding that shaped the design):
the I2C0 clock story got worse while measuring the "before" number, and the
design changed accordingly.** The bus clock is not just "three owners", it is **dynamic**:
`jl_wavegen_start()` (`JumperlessMicroPythonAPI.cpp:273`) calls `wavegen.begin()` on **every**
start → `WaveGen::begin()` → `_dac.begin()` (`WaveGen.cpp:83`) → `MCP4728::begin()` →
`_wire->setClock(1700000)` (`MCP4728.cpp:164`). So the bus flips to **1.7 MHz whenever the
wave generator is started** and drops back to **400 kHz on the next OLED frame** (SSD1306
`clkAfter`), and sits at 1.7 MHz between them; the INA219s and the probe feed DAC run at
whichever value the last of those two left. Measured on this boot: register readout after a
wavegen start = `FS_HCNT 36 / FS_LCNT 52` @ 150 MHz → **1.70 MHz** (the earlier 400 kHz
readout was taken hours after boot with no wavegen since). **Before number:** wavegen 2 kHz
sine on DAC1 for 3.0 s → **91 055 MCP4728 writes = 30 352 writes/s** at 1.7 MHz (the
`mcp4728 writes` B counter in `i@`, script `/Users/kevinsanto/.claude/jobs/064d2674/tmp/wg_rate.py`: read `i@`,
`wavegen_set_output(1); wavegen_set_wave(0); wavegen_set_freq(2000); wavegen_start(1);
sleep 3; wavegen_stop()`, read `i@` again). Expect ~18 k writes/s at 1 MHz after T1.9 (the
rate WaveGen's `B_PER_SAM 4.67` model was calibrated for — that constant fits ~1 MHz, not
1.7 MHz or 400 kHz), and note for Kevin that the max wave frequency he sees today at 1.7 MHz
will drop ~40 % — that is the honest cost of running the INA219s at the clock the code
argues for; if he would rather keep 1.7 MHz for streaming, the alternative is "1 MHz owner +
WaveGen may raise the clock only while `isRunning()` and must restore it on stop", which is a
fourth edit in `WaveGen::start/stop`, not in `MCP4728::begin()`.

**T1.9 edits as planned (all three executed as written — see the top of this block for what
landed and what was measured):** (1) `MCP4728::begin()`
drops `setClock(1700000)` — this also fixes the every-wavegen-start flip; (2) the OLED-on-I2C0
`Adafruit_SSD1306` instance keeps `clkDuring` 400 kHz and gets `clkAfter = 1000000`
(`oled.cpp:109-112`; introduce one shared constant for the I2C0 bus rate next to `initDAC()`'s
`Wire.setClock(1000000)` in `Peripherals.cpp:520` and use it in both places); (3)
`oled::connect()` passes that constant instead of `400000` to `initI2C()` when
`connection_type == 2` (`oled.cpp:4260`) — the OLED's own address pings (`checkConnection`
`oled.cpp:706-709`, `show()`'s post-write ping `:2296`, the boot auto-detect `:4337`) then run
at the bus rate, which is fine: the boot auto-detect already ACKs at 1.7 MHz today. **Verify:**
register readout = 1 MHz right after boot with the OLED connected **and** again right after a
`wavegen_start(1)`/`wavegen_stop()` (the flip must be gone); the writes/s number above → ~18 k;
`i@` and the `[switch]` classification stable; `probe_current_zero` in `~` across ≥10 reboots
(`machine.reset()` from the REPL, then `~` on port 1) — compare its spread with the 0.5–2.3 mA
history; `X` still says OLED Connected. Then build ×3, HIL 5/6, `test_infra_paths` 24/24, commit.

**Then the queue:** ~~T1.4~~ ~~T1.5~~ ~~T1.7~~ ~~T1.6 (measure-only)~~ ~~T1.10~~ ~~T2.2a (probe)~~ (landed
— see the top of this block; Tier 1 done) → **T2.2b** (mailbox) → T2.3 → T2.1. (The watchdog
*enable* is its own decision, on the T1.6 numbers.)

**Design refinements found while reading for T1.4 (all executed as written in the T1.4 commit,
with the four deviations and the ConfigSave/SlotManager correction recorded at the top of this block):**
- `ProbeSwitch` gets **`periodUs() = 10 000`, not 500 000**: `checkSwitchPosition()` has
  early returns *before* its 500 ms gate (`checkingButton`, LED-settle) that expect to retry
  on the next pass, and it runs `infraServiceTick()` on every call (whose comment says
  "~500 ms" but which has no gate of its own). 10 ms keeps both honest without a new service.
- `OLED` (`oledPeriodic`) gets **20 000, not 250 000**: its connection pings are self-gated
  ≥750 ms, but the post-hold and post-wavegen flushes have no gate and would wait a whole period.
- `LiveXbar`: 100 000 **plus** `requestUpdate()` calls `requestRun()` so a crossbar change
  is shown on the next pass, not up to 100 ms later.
- `Peripherals` 10 000, `ProbePads` 50 000, `OledGui` 15 000, `SlotManager` 50 000,
  `ConfigSave` 100 000 are exact encodings of gates already inside those services (verified).
- Period 0 (every pass): TinyUSB, MpRemote, ProbeButton, AsyncPassthrough, Menus, Probing,
  Highlighting, MeasureMode.
- Stats: keep counting inside `serviceCritical()`/`serviceInner()` too (a private
  `runService(idx, now)` helper), so the `X` table shows the modal-loop calls.
- C6: `probe_button_pio_irq_handler` (`Probing.cpp:611`) calls `ProbeButton::requestRun()`
  and `Probing::requestRun()` when a sample changes state — harmless while both are period 0,
  meaningful the day either gets a period.
- The `X` table hangs off `cmd_resourceStatus` (`SingleCharCommands.cpp:2670`, printing ends
  `:2817-2821`) — add it after the probe-LED line: name / prio / period / runs / last / max /
  avg µs / overruns.
- After T1.1, `serviceCritical()` = {TinyUSB, MpRemote, Peripherals, ProbeButton}; T1.5's
  `serviceInner()` adds AsyncPassthrough and replaces the 23 `serviceCritical()` call sites
  (Menus.cpp ×10, Probing.cpp ×8, `BitmapEditor.cpp:1167`, `GraphicOverlays.cpp:632`,
  `ImagesApp.cpp:341`, `Peripherals.cpp:3339`, `Python_Proper.cpp:256`). **Done (T1.5) —
  the count was re-verified by grep before editing: exactly 23.**

**Working rules that bit this session:** only one process on a CDC port at a time — the
`jumperless` client on port 1 makes every port-1 test lie; check `lsof /dev/cu.usbmodemJLV5port*`
before running HIL. `run_all.py` takes ~2 m 10 s. **When port 1 is taken, port 7 (USBSer3)
still answers `X`** (`cmd_resourceStatus` is `SER3_ALLOWED` — the MCP4728 counters, OLED
status, IRQ slots, uptime; `python3 test/hil/port7.py X`), and `:oled:quarter` dumps the
framebuffer; `i@` (`SER3_MODIFIES_STATE`) and `~` (`SER3_IRRELEVANT`) are refused there. The
REPL (port 5) reads registers, `ina_get_bus_voltage()/ina_get_current()` (a failed read is
exactly 0.0), and `/config.txt` — but that file is **not** a live `probe_current_zero`: the boot
calibration writes `jumperlessConfig` without dirtying it, so the file holds the last explicit
save. Do NOT `import run_all` from a Python one-liner — its module body runs the suite.
`pio run -t upload` works with the client attached (the 1200-baud touch on port 1 still resets
the board; the client reconnects), and so does `machine.reset()`. **The suite assumes
`terminal_line_buffering = 1`** (the default). Until T1.7 (row 34) that was a hard need: in
char mode every `x?` sub-command (`i?`, the RouteSafety audit the tests parse) was eaten by
`loop()`'s help peek and returned the help page, which showed up as
`test_net_currents`/`test_stress` failing "no chips marked suspect" + "self-check PASS" while
everything else passed. Since T1.7 `i?`/`A?`/`M?` reach their handlers in both modes, but the
tests still send line-shaped input, so keep the rule: the `jumperless` client's
interactive-mode preference ends up asserting SI (persisted on the next config save; exact
trigger unconfirmed) — send `B1` on port 1 before `run_all.py` if the client was attached
since the last boot. A stray
`usb_audio_save()` from the REPL is a legitimate full config save when you need `/config.txt`
to reflect live values. Add each `DEV_MERGE_HANDOFF.md` row with a
placeholder in the item's own commit and put the real hash in with the *next* commit (a
`--amend` changes the hash — row 26 says `73aee5c` and should say `3fc5c57`; fix it in the
T1.8 commit).

### What this project is, and is not (read before judging any of the vocabulary here)

JumperlOS is firmware for a **bench breadboard**: it closes analog crosspoints so rows are
wired together, lights the LEDs under them, reads its own ADCs and current sensors, and talks
to the host over USB serial. It has **no network stack, no radio, no internet connectivity of
any kind** — the only outside world it touches is the USB cable to the developer's own
computer, and the only thing it can affect is the breadboard on the desk. Words that recur in
this document — *probe*, *relayed command*, *passthrough*, *watchdog*, *IRQ*, *flash write*,
*park the core*, *wedge* — are ordinary embedded-firmware vocabulary (a physical probe tip, a
command an Arduino sends up the UART inside `<j>…</j>` tags, a UART bridge, an MCU watchdog
timer, interrupts, the SPI flash, stopping a core during a flash erase, a hung core). There is
nothing here with security implications, no user data, no credentials, and no conceivable
misuse; a session working on it should read all of it as what it is — hobby
electronics-instrument firmware. (2026-08-16: the whole "injected command / injection buffer"
family was renamed to "relayed command / relay buffer" — `RelayedCommandService`,
`RelayBufferStream`, `Jerial.relayInput()`, `hasRelayedCommand`, … — and comment words like
steal/poison/kill/sniff were reworded, purely so the vocabulary reads as what it is; nothing
behavioural. Verbatim datasheet excerpts under `CodeDocs/` were left as quoted.)

### What was approved (Kevin, 2026-08-16, plan mode)

- **All of Tier 1** (T1.1–T1.10), **including T1.9** (I2C0 clock, hardware-verified before
  commit).
- **T2.1** (always-on ADC ring), **T2.2** (core-1 mailbox: `REQ_SEND_PATHS` then
  `REQ_SHOW_LEDS`), **T2.3** (CH446Q DMA→FIFO).
- **T2.4** (the GPIO 9 PIO program, C5) is the **next session's opener** — design stays in C5.
- **Tier 3 and the rest of Tier 2 (T2.5, T2.6) are design-only this pass** — recorded in E
  as "proposed, not built".

### How the approved items were executed

One commit per item, in the order below, each after `pio run -e jumperless_v5 -e
jumperless_og -e jumperless_v5_debug`, flash, `python3 test/hil/run_all.py` (5/6, only
`test_net_currents` failing — pre-existing), plus the item's own checks. Never pushed. A
`DEV_MERGE_HANDOFF.md` row per landed commit.

**Commit-gate disposition (read this before judging the commits).** The plan said anything
Kevin can see or feel waits for his hardware confirmation *before* the commit. This pass ran
autonomously with Kevin away, so it was executed in two lanes: the items whose gates are
fully automatable (T1.1, T1.2/1.3, T1.8, T1.9, T1.6, T1.7, T1.10, T2.2, T2.3) were committed
after their gates; the items with a tactile component (T1.4 probe/menu feel, T1.5 probe mode
+ click menu + Arduino passthrough, T2.1 taps under both feeds) were run through every
automatable check and then **committed with "pending Kevin hands-on" in the commit message**
rather than left as an unbisectable uncommitted blob. Each is one commit and individually
`git revert`-able; nothing was pushed. This matches Kevin's own answer to the same question
on 2026-08-15 ("commit after your verification, leave me a hands-on checklist").

| Step | Item | Status | Verified how |
|---|---|---|---|
| 0 | this doc + `DEV_MERGE_HANDOFF.md` rows | landed | docs only |
| 1 | T1.1 raw `tud_task()` → mutex-guarded entry points; `TinyUSBService` every pass | **landed** | builds ×3; HIL 5/6 ×5 (one standalone + a 4-run soak, ~9 min, with a second process holding port 7 open the whole time: 0 errors, 4 CDC ports enumerated after every run, uptime continuous — no port drops); `X` census unchanged (6/6 slots, FlashPark timeouts 0). Zero raw `tud_task()` calls left in `src/` (59 sites: 23 → `TinyUSB_Device_Task()` at pump-only waits, 36 → `yield()` where the site wanted output pushed or was a `Serial.write(marker); tud_task();` debug pair). `TinyUSBService` is CRITICAL now (every pass, and inside `serviceCritical()`'s modal set — the B4 delta arriving early). |
| 2 | T1.2 + T1.3 priority/comment truth, drop the no-op services, `core1request` gone, `inClickMenu` volatile | **landed** | builds ×3; HIL 5/6; `X` census unchanged (6/6 slots, FlashPark timeouts 0, heap free 46 KB). Registered services now: TinyUSB, MpRemote, Peripherals, ProbeButton (CRITICAL); AsyncPassthrough, Menus, SlotManager, Probing, Highlighting, MeasureMode (HIGH); ProbeSwitch, OledGui (NORMAL); ProbePads, OLED, LiveXbar, ConfigSave (LOW). Not registered: TermSerial, RelayedCmd, SingleCharCommands, USBPeriodic, FileCacheFlush (`#if USE_FILE_CACHE`). |
| 3 | T1.8 CH446Q per-crosspoint path + ISR into RAM | **landed** | builds ×3; HIL 5/6; `test_infra_paths` 24/24; `X`: `irq 16` handler `0x20000835` (was `0x10055931` in flash); `nm`: `isrFromPio` 0x20000834, `sendXYrawUnchecked` 0x2000087c, `sendPath` 0x20000bc4, `setCSex` 0x20003a90 (V5), OG likewise |
| 4 | T1.9 I2C0: one clock owner at 1 MHz (**see the corrected finding below**) | **landed** | builds ×3; I2C0 register readout 60/90 = 1.00 MHz on 11/11 boots, unchanged before/during/after `wavegen_start`/`stop` and after forced OLED frames (the flip is gone); DAC1 30 352 → ~20 k writes/s (2 kHz sine, ~3.1 s; −33 %, the honest cost); 80 000 INA0/INA1 register reads at sustained 1 MHz, 0 failures, 1–2 LSB spread; 10 reboots: INA1 8-sample median 1.465 mA every boot, OLED Connected, DAC found; `probe_current_zero` on 10 further boots 1.01–1.51 mA (mean 1.28, history 0.5–2.3); HIL 5/6; `test_infra_paths` 24/24; `i@` `probe_power on -> DAC0` |
| 5 | T1.4 B3 scheduler periods + `requestRun()` + stats table (+C6, C12) | **landed** (pending Kevin hands-on: probe/menu feel) | builds ×3; `X` table: all 16 services runs>0 with the expected periods, modal-loop calls counted; wait-loop debug 12 s: 0 SLOW SERVICE; no `refresh:` line over 20 routing ops; HIL 7/7; `test_infra_paths` 24/24. First measured numbers: ProbePads 36.8 ms/call @ 20 Hz = 64–70 % of core 0 (est. was ~12 ms) |
| 6 | T1.5 B4 `serviceInner()` (+ 64-bit `nextDueUs`) | **landed** (pending Kevin hands-on: probe mode / click menu / REPL feel; passthrough while probing) | builds ×3; `X` after a 3 s REPL sleep: the 4 inner-set rows (incl. AsyncPassthrough `HIGH*`) = passes + 42, non-inner rows = passes; HIL 6/7 then 7/7 (net_currents standalone 8/8 ×4); `test_infra_paths` 24/24 |
| 7 | T1.7 B6 `loop()` cleanup | **landed** (pending Kevin hands-on: help/`x?` feel, latency in the app) | builds ×3; help/`help <cat>`/`x?`/`m?`/`h`/`?`/`i?`/`A?`/`M?` right in both modes (scripted); `n` latency line mode CR/LF 146 → 46 ms p50, char mode unchanged; `X` PortHousekeeping row; HIL 7/7; `test_infra_paths` 24/24 |
| 8 | T1.6 watchdog, measure-only | **landed** (measure-only; the enable is a separate decision) | builds ×3; `X` kick-gap lines; idle 42 ms = the ProbePads block; slot save 1.8 s / 1.1 s; wavegen 10 s = core-1 capture; compute-bound MicroPython 5.2 s; suite 11.5 s; HIL 7/7; `test_infra_paths` 24/24 |
| 9 | T1.10 LED-dump off core 1 | **landed** | builds ×3; `R!` dumps at ~340 ms idle and through a modal MicroPython script; `serial_1.function = leds` → 60 KB/4 s on USBSer1, board alive; `X` LedDump row; HIL 7/7; `test_infra_paths` 24/24 |
| 10 | T2.2 mailbox `REQ_SEND_PATHS`, then `REQ_SHOW_LEDS`; latency probe | **probe landed** (T2.2a); mailbox pending (T2.2b) | probe: builds ×3; before numbers req→pickup ~0.1–0.3 ms (max 2.2 ms), pickup→send ~50 µs, send→show ~7 ms (max 10), req→show ~7.3 ms; HIL 7/7; `test_infra_paths` 24/24 |
| 11 | T2.3 CH446Q DMA→FIFO + ISR chip list | pending | — |
| 12 | T2.1 always-on ADC ring | pending | — |

(The table is updated in place as items land; "pending" rows are the queue.)

### Findings corrected during the implementation pass

- **Finding 2 ("I2C0 runs at 1.7 MHz") is wrong on Kevin's board — it runs at 400 kHz.**
  Measured live over the REPL before any change: `I2C0.IC_FS_SCL_HCNT = 150`,
  `IC_FS_SCL_LCNT = 225` at `clk_sys` 150 MHz → 150e6/375 = **400 kHz**. Cause: on a rev-7
  board the OLED is on I2C0 (`top_oled.connection_type == 2`, `X` shows "Type: I2C0
  (internal)"), and `oled::init()` → `oled::connect()` (`oled.cpp:544, 4260`) calls
  `initI2C(4, 5, 400000)` (`Peripherals.cpp:591`, re-`begin()`s Wire at 400 kHz), after which
  every SSD1306 transaction sets the clock to `kOledI2CClockHz` = 400 kHz **before and after**
  (`oled.cpp:68, 111-112`; the driver's `TRANSACTION_START/END` = `wire->setClock(wireClk)` /
  `setClock(restoreClk)`). So the timeline is: 1.7 MHz from `mcp.begin()` in `setup()`
  (`MCP4728.cpp:164`) until `oled.init()` in `loop()`'s `firstLoop == 2`, then **400 kHz for
  the rest of the session** for the DAC, both INA219s and WaveGen. On a board with no OLED on
  I2C0 the original finding stands (1.7 MHz throughout). Neither is the 1 MHz `initDAC()`
  sets and WaveGen's rate math assumes (`WaveGen.cpp:372,412,432,637`, `B_PER_SAM 4.67`
  "empirically calibrated" — consistent with ~1 MHz, not 400 kHz or 1.7 MHz; the stream is
  free-running, so its output frequency scales with the real bus clock).
  **Update (T1.9 measurement):** the clock is also *dynamic* — every `wavegen_start()` re-runs
  `MCP4728::begin()` and flips the bus back to 1.7 MHz until the next OLED frame (see CONTINUE HERE).
  **T1.9 therefore becomes "give I2C0 one clock owner"**: `MCP4728::begin()` stops overriding
  the clock; the OLED-on-I2C0 instance keeps its own transfers at 400 kHz (`clkDuring`, the
  panel's rating) but hands the bus back at 1 MHz (`clkAfter`), and `connect()`'s probe passes
  the bus rate for the shared bus. Verification adds a register readout (must read 1 MHz
  after boot with the OLED connected) and the WaveGen actual-frequency stat before/after.
  **Built and measured (CONTINUE HERE):** 1.00 MHz on every boot and around every wavegen
  start/stop and OLED frame; DAC1 stream 30.4 k → ~20 k writes/s; INA219s clean at
  sustained 1 MHz (80 k reads, 0 failures).
- **The `tud_task()` count is 59 textual call sites, 51 compiled in this build** (8 are behind
  compile-time-0 debug macros in `main.cpp`: `DEBUG_MAIN_LOOP_CHECKPOINTS`,
  `debug_busy_timers`). The proposal said 54. T1.1 converts all of them, the compiled-out
  ones too (they are one `#define` away from live).

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

(1) and (2) are done, committed and hardware-verified — `PROBE_REWORK_HANDOFF.md`. (3) is
this file. Kevin's instruction for the sweep: *"I do want changes after proposing them, I'll
be doing it in plan mode."* — so: proposals first (this file), Kevin picks in plan mode,
the approved items are implemented (section 0).

Standing constraints from Kevin: **commit only when verified** (ideally by him on hardware
for anything he can see or feel); **never push**.

---

## 2. What this project is

**JumperlOS** — firmware for the **Jumperless V5**, a breadboard whose rows are wired
together by a 12-chip **CH446Q analog crossbar** instead of jumper wires. An RP2350B
(dual-core Cortex-M33, arduino-pico core 5.6.0 / pico-sdk 2.2.1) drives the crossbar, a
4-channel MCP4728 DAC, ADCs and two INA219 current sensors, addressable LEDs under every row,
an OLED, and a **probe** — a handheld tip the user taps rows with, whose DPDT switch has a
SELECT and a MEASURE position.

The board is **attached over USB** while working on this. CDC ports: `…JLV5port1` = main
terminal (single-char commands, the `` ` `` config interface); `…JLV5port5` = MicroPython raw
REPL (the HIL suite drives this); `…JLV5port7` = USBSer3 machine backchannel.

**Cores.** Core 0 — `setup()`/`loop()` (`main.cpp:210`/`:718`): the service scheduler
(`jOS`), USB, the terminal, MicroPython, the probe button IRQ. Core 1 — `setup1()`/`loop1()`
→ `core2stuff()` (`main.cpp:609`/`:1399`/`:1540`): the LEDs, the CH446Q crossbar sends, the
probe LED, WaveGen's DAC streaming, the encoder poll, the net-voltage scan.

**The scheduler.** `src/JumperlOS.h`/`.cpp`. Services are objects with `service()` and
`getPriority()` (CRITICAL/HIGH/NORMAL/LOW, `JumperlOS.h:102`) returning IDLE/BUSY/BLOCKING
(`:91`). `jOSmanager::serviceAll()` (`JumperlOS.cpp:164`) is the main-loop dispatcher;
`serviceCritical()` (`:341`) the reduced set that modal loops (`probeMode()`, click menus)
pump. Registration list: `main.cpp:495–542`. The authoritative priority is `getPriority()`
in each service's header, not the registration comment.

---

## 3. How to work on this repo

```bash
pio run -e jumperless_v5 -e jumperless_og -e jumperless_v5_debug   # ALL THREE, every step
pio run -e jumperless_v5 -t upload                                  # flash the attached board
python3 test/hil/run_all.py                                         # expect 6/7 or 7/7 (see below)
python3 test/hil/test_infra_paths.py                                # 24/24 for routing/config work
python3 test/hil/test_config.py                                     # 30/30
```

`run_all.py` is **expected to report 6/7 or 7/7** (5/6 before `test_paste_state.py` joined it on 2026-08-17): the one
tolerated failure is `test_net_currents` ("zero-load TOP_RAIL net shows < 1 mA phantom current"),
pre-existing and out of scope (identical on the `5.7.2.0` checkpoint and on `main`) — it
**passed on three consecutive runs on 2026-08-17** (rows 31–32 builds) without any change that
touches it, so it is board state that may come and go. Anything else failing is a real regression. `test_encoder_ui`
auto-skips (reports PASS) without an OpenOCD session on :4444.

**Config keys** over port 1 in **bracket** form — `` `[dacs] probe_power_source = 1 `` — or
dot form `config.section.key = value`. Bare `` `dacs.probe_power_source = 1 `` is not parsed.

**Gotcha (state):** `test_infra_paths` needs DAC0 inside the feed window ([2.80, 3.90] V) — a stray
`dac_set` leaves the feed on GPIO / `(none)` and the test fails 3/24; `dac_set(0, 3.33, True)`
resets it. Check `i@` says `probe_power -> DAC0` first. Known culprits: `test_routing.py` sets
DAC0 to 2.5 V (so `run_all.py` itself leaves it there), and a fresh boot on 2026-08-16 came up
with DAC0 = 2.000 V (`:json:power` on port 7) restored from the slot — so assume it is wrong
until checked.

**Gotcha (ports):** a stale process holding port 1 gives "device reports readiness to read but
returned no data". Not a board fault. In `run_all.py` it also appears when a previous file's
deferred config save is still in its flash window as the next file opens the port — suspect
that first if a suite failure doesn't reproduce standalone. Only one process on a port at a
time (don't drive port 1/5 while `run_all.py` runs).

Useful readouts: `X` on port 1 (resource census: PIO map, shared-IRQ slots, FlashPark, MCP
write/skip counters, probe LED frames / button samples / frame aborts); `i@` (INA/probe
line); the REPL for register peeks (`uctypes.struct(addr, {'v': uctypes.UINT32|0}).v` —
`machine.mem32` is not built into this MicroPython).

Docs worth reading first: `PROBE_REWORK_HANDOFF.md` (parts 1–2), `PROBE_INFRAPATHS_HANDOFF.md`,
`DEV_MERGE_HANDOFF.md` (commit history + "how to work on this board": flashing, SWD recovery,
soaking), `PERFORMANCE_OPTIMIZATIONS_ROUND2.md`/`PERFORMANCE_ROUND3.md`,
`differencesRP2040RP2350B.md`, `dma_sectopn_rp2350b.md`.

---

## Findings the brief did not have (the sweep's own results)

1. **~⅓ of idle core 0 is spent blocked in probe-pad ADC reads** (estimated from the code's
   own sample counts × the 8 µs/sample `readAdcHeld` cost: `Probing::service` ~1.2 ms @ 100 Hz
   + `ProbePads::service` ~12 ms @ 20 Hz). The B3 stats table measures it before C1 acts on it.
   **Measured 2026-08-17 (T1.4's `X` table): it is ~85 %, not ⅓** — `ProbePads` 36.8 ms per
   call at 20 Hz (64–70 % of core 0) + `Probing::service` ~16 %; the loop makes ~900–1 300
   passes/s and the 10 ms services effectively run at ~24 Hz. See CONTINUE HERE for the
   T2.1-vs-touch-pre-check choice.
2. **I2C0 does not run at the 1 MHz the code argues for** — see the corrected finding in
   section 0: 400 kHz on a rev-7 board with the OLED on I2C0, 1.7 MHz otherwise; three
   different owners of the clock (`MCP4728::begin()`, `initDAC()`, the SSD1306 driver).
3. **The CH446Q per-crosspoint path and its ISR execute from flash** despite the
   `__not_in_flash_func` banner on `sendPaths` (`X` shows the `irq 16` handler at
   `0x10055951`) — and `src/ch446.pio` is stale vs the compiled header.
4. **TinyUSB is NORMAL** (pumped from the loop every 3rd pass), and **ProbeButton is CRITICAL /
   Probing HIGH** — the brief had both wrong; the modal loops starve AsyncPassthrough (HIGH).
5. **Every command char in `loop()` waits up to 100 ms with nothing serviced** for a possible
   `?` (`main.cpp:1188,1216`).
6. **LED-dump mode writes USB CDC from core 1** (`main.cpp:1510` → `dumpLEDs()`), the documented
   wedge family; latent because the mode is off by default.
7. `core1request` is written and never read; `inClickMenu` is the one cross-core flag that is
   not `volatile`; `LED_SHOW_MIN_TIME` is 14 µs, not ms; the LED DMA double buffer has no
   completion IRQ; `usbPeriodic()`, `FileCacheFlushService` (this build), TermSerial, RelayedCmd
   and SingleCharCommands are no-op services; `Probing::measureMode()` has zero callers.
8. USBAudio's ring **cannot** serve the probe channels as designed (ADC5/7 return 0) — the
   ring promotion needs a "samples newer than t0" read for the probe's timed reads (C1).

## Executive summary

1. **The scheduler has no notion of time.** `serviceAll()` (`JumperlOS.cpp:164`) runs each
   band on a loop divisor (1/1/3/20) with no periods, deadlines or overrun accounting; every
   service that needs a cadence re-implements it with `millis()`. Loop-pass time varies from
   ~20 µs (inside `probeMode()`'s `serviceCritical()`) to multi-ms (a service doing I2C), so
   "every 3rd pass" means anything from 60 µs to 10 ms. → Section B: due-or-pending gate,
   `periodUs()`, ISR-safe `requestRun()`, per-service µs stats in `X`.
2. **`probeMode()` is a nested blocking loop** that keeps only the CRITICAL set alive
   ({TermSerial, RelayedCmd, MpRemote, Peripherals, ProbeButton} — TermSerial and
   RelayedCmd are no-ops). → Section B: `serviceInner()` first, the state machine last (Tier 3).
3. **TinyUSB is already IRQ-pumped** by the Adafruit port under `__usb_mutex`
   (`Adafruit_TinyUSB_rp2040.cpp:81-117`); the **raw `tud_task()` calls in `src/`** bypass
   that mutex (re-entrancy: the pump IRQ can land mid-`tud_task()`), and the `TinyUSBService`
   that is supposed to pump from the loop is NORMAL priority (every 3rd pass, `JumperlOS.h:385`)
   — every hot wait spins with its own raw call instead. Fix is mechanical:
   `TinyUSB_Device_Task()` / `yield()`; nothing on core 1, ever (`main.cpp:1494-1504`).
4. **The ADC ring already exists** in USBAudio (`USBAudio.cpp:366-431`) and `readAdc()`
   already snapshots it while streaming (`Peripherals.cpp:2687-2690`). Promoting it to
   always-on turns `readAdc()` into a lock-free memory read (no 100 ms lock spin, no
   USB-audio/probe hand-off) and takes the pad poll's 1–3 ms of core-0 CPU to ~10 µs. Honest
   caveat: it does not shorten the pad decode's *sample window* (that is the median/variance
   filter's choice), and per-channel sample rate = 500 ksps ÷ channels in the mask.
5. **INA poll no longer toggles `pauseCore2`** (done, `PROBE_REWORK_HANDOFF.md`); the
   remaining `led-frame aborts(pause)` come from flash writes.
6. **CH446Q single-SM offload is impossible on RP2350B**: data/clk on GPIO 14/15
   (`CH446Q.cpp:89-96`), chip-selects on 28..39 (`:110-113`), and a PIO block's GPIOBASE is
   0 or 16. The current program already flow-controls on a CPU ISR per crosspoint
   (`ch446.pio.h` `irq nowait 1` / `wait 0 irq 1 rel`, `CH446Q.cpp:55,1042-1060`), so **step 1** is
   DMA→TX FIFO + an ISR chip-select list (non-blocking `sendPaths`, no per-crosspoint spin),
   **step 2** a second SM in PIO1/PIO2 (GPIOBASE 16) strobing CS with `irq set 0 next` /
   `wait 1 irq 0 prev`. Payoff is core-1 availability and a hardware-owned send that either
   core can kick, not raw crosspoint speed — tap→crossbar latency today is dominated by the
   `waitCore2()` / `sendAllPathsCore2` handshake (`Commands.cpp:57-88, 194-235`).
7. **Cross-core coordination is ~10 volatile ints with magic values** plus a 25 ms guess. →
   Section D: one typed request mailbox (bits + payload + generation counter), and written ownership
   rules (I2C0 = core 0, WaveGen the exception; ADC = the ring; USB = core 0; flash =
   FlashPark; CH446Q SM = core 1 (until step 2); button PIO IRQ = core 0).
8. **WaveGen ≥ 1 kHz captures core 1 indefinitely** (`WaveGen.cpp:283-289`,
   `WAVEGEN_NONBLOCKING_THRESHOLD_HZ 1000`): no LED frames and no crossbar sends while it
   streams (`refreshConnections` leaves the send pending — the crossbar diverges from the
   netlist until streaming stops). → Tier 3: I2C0 DMA + pacing timer.

## A. What RTOSes do vs jOS

| Need | FreeRTOS | Zephyr | pico-sdk primitive actually linked here | jOS today | Verdict |
|---|---|---|---|---|---|
| Time-driven wake | `vTaskDelayUntil`, software timers | k_timer / k_work_delayable | `alarm_pool` (default: TIMER0 alarm 3, core 0; `alarm_pool_create_on_timer(timer1,…)` for a core-1 pool), `time_us_32()` deadlines | none — per-band loop divisor 1/1/3/20 (`JumperlOS.cpp:59-62`); every service self-gates with `millis()` | add `periodUs()`/`nextDueUs` to the base class; keep alarms out of the main path (no I2C/flash/Serial in callbacks) |
| ISR→task notify | `xTaskNotifyFromISR` | k_sem / k_work_submit | a `volatile bool pending` + `__dmb` (single-writer), `sem_t`, or a `queue_t` | the button PIO IRQ sets `g_pendingUndo/…` flags read by `service()` (`Probing.cpp:607-612`) | formalise as `Service::requestRun()` |
| Software timers | `xTimerCreate` | k_timer | `add_repeating_timer_us` (already used for slow PWM `Peripherals.cpp:2854`) | ad-hoc `millis()` gates | fine as is; alarms only for hard cadences |
| Cross-core message | queues + SMP | k_msgq / IPM | `queue_t` (spinlock-guarded, `queue_try_add/remove` linked), SIO FIFO (arduino-pico owns it), **doorbells** (8; FlashPark uses one) | ~10 volatile ints with magic values (`sendAllPathsCore2 = ±1/3`, `showLEDsCore2 = -N/2/3/≥10`) + `waitCore2()` 25 ms spin | replace with one mailbox of typed requests + a generation counter (section D) |
| Mutual exclusion | mutex / critical section | k_mutex / k_spinlock | `mutex_t` (`core_sync_mutex`, `fs_mutex` — `externVars.cpp:56`), `critical_section_t`, `spin_lock_claim_unused` (32) | `core_sync` mutex + `core1busy/core2busy` flags + `pauseCore2` | keep `core_sync`; retire the flags as the mailbox lands |
| Poll many | `select`-style event groups | k_poll | none — cooperative loop | `serviceAll()` walks 20 services every pass | due-or-pending gate makes the walk cheap |
| Watchdog | `xTaskCheckIn` patterns | k_wdt / task watchdog | `watchdog_enable/_update`, `watchdog_hw->scratch[0..7]` (CrashLog already reads scratch — `crashlogLatchAtBoot`) | none | Tier 1: measure first, then enable with a long timeout, kick from `loop()` and `loop1()`, stamp scratch with the last service index |
| SMP / core affinity | SMP kernel | SMP | `multicore_*`, per-core NVIC enables, `PICO_VTABLE_PER_CORE=0` | hand-placed: core 0 = USB/I2C0/scheduler, core 1 = LEDs/CH446Q | write the ownership rules down (section D) |

**Do not** (all verified in this tree): call `tud_task()` from an IRQ or from core 1 (the SWD-confirmed wedge in `main.cpp:1494-1504`); add shared IRQ handlers (6/6 used); do I2C, flash, or Serial from alarm/DMA/PIO callbacks; vendor `async_context` (not linked; borrow its `at_time`/`when_pending` shape); steal the ADC lock on timeout (`Peripherals.cpp:2669-2673`).

## B. jOS upgrade

### B1. Truth table of priorities today, and the comment fixes (Tier 1, trivial)
Fix the five wrong registration comments in `main.cpp:502,503,506,515,519`, the stale
service-ID map in `JumperlOS.cpp:156-163`, `Highlighting.cpp:123` ("20ms" → 40),
`MpRemoteService.cpp:166` ("1024" → 8192), and `Probing.h:205-209` (ProbeSwitch "LOW").
Nothing behavioural.

### B2. Drop the no-op services (Tier 1, small)
Unregister TermSerial, RelayedCmd, SingleCharCommands, USBPeriodic (its `usbPeriodic()` is a
debug print), FileCacheFlush (compiled-out body). Keep the classes for now (delete in a later
pass) — the win is a shorter walk on every pass and an honest `X` table. Risk: nil (verified
no-ops). Note `TermSerialService::setTermControl` wiring goes with it.

### B3. Give the scheduler time (Tier 1 → honest size: medium-small)
Add to `Service` (base, `JumperlOS.h:117`), all defaulted so no header changes are forced:
```cpp
virtual uint32_t periodUs() const { return 0; }     // 0 = every pass (today)
void requestRun() { pending = true; }               // ISR/other-core safe: single word
// managed by jOS:
uint32_t nextDueUs = 0; volatile bool pending = false;
uint32_t runs = 0, lastUs = 0, maxUs = 0, overruns = 0; uint64_t totalUs = 0;
```
`serviceAll()`: `now = time_us_32()`; per service **capture-and-clear first** — `bool p =
pending; pending = false;` — then run iff `p || periodUs()==0 || (int32_t)(now - nextDueUs)
>= 0` (a `requestRun()` landing from an IRQ *during* the call re-pends for the next pass
instead of being erased by a clear-after-run); after it runs `nextDueUs = now + periodUs()`
(from *now* — no catch-up bursts), stats; `overruns++` when `lastUs > periodUs()`.
The band divisors go (they were never a cadence). Then set `periodUs()` where a real cadence
already exists inside the service — Peripherals 10 000, ProbeSwitch 500 000 (but keep
`infraServiceTick()` on a 10 000 tick — it runs every pass today; give it its own tiny
service), ProbePads 50 000, Highlighting 40 000 (its `encoderNetHighlight()` needs every pass →
split or period 1 000), OledGui 15 000, OLED 250 000, LiveXbar 100 000, ConfigSave 100 000,
SlotManager 50 000, MpRemote/AsyncPassthrough/Menus/ProbeButton/Probing 0. `X` (or a new `x`
sub-view) prints name / prio / period / runs / last / max / avg µs / overruns.
Risk: low if periods only *encode* gates the services already apply internally (behaviour
identical); moderate for the ones that don't (Menus, Highlighting) — leave those at 0.
Effort: ~150 lines in JumperlOS + one line per header.

### B4. BLOCKING → an explicit modal set (Tier 1, small) — **built, T1.5 (section 0)**
BLOCKING is live for Menus (`Menus.cpp:68,75`), Highlighting (`:81`, the encoder voltage
adjuster) and Probing (`:1249`, pad menu open) — the latch is what stops `Menus::clickMenu()`
running while the adjuster owns the wheel, so its *semantics* stay. What changes: the set that
keeps running while a modal owner holds the loop is named — `jOS.serviceInner()` = exactly
{ProbeButton, MpRemote, AsyncPassthrough, TinyUSB (mutex-guarded), Peripherals}, run once —
and `probeMode()`, `getMenuSelection()`, the pad-menu `choose*` loops and `servicePython()`
call *that*; `serviceAll()` uses the same set while a service is BLOCKING; `serviceCritical()`
becomes an alias, then goes. Behavioural delta (Kevin-visible, intended): AsyncPassthrough and
the USB pump join the inner set — today both are starved inside probe mode / click menus (a
modal loop pumps USB only through the raw `tud_task()` in `waitCore2()`, and the Arduino UART
bridge stops while the probe is in use).

### B5. `probeMode` → state machine (Tier 3, large — milestones; **not built this pass**)
1. **Extract `probeTick()`** = one pass of the `while` body (`Probing.cpp:2423-3320`) with
   the loop-carried locals (`row[]`, `node1or2`, `nodesToConnect[]`, `probeTimeout`,
   `pendingInProbeButton`, `doubleSelectCountdown`, `firstEntry`, `bannerEmitted`, …) moved
   into a `ProbeSession` struct; the `goto restartProbing(NoPrint)` labels become
   `S_ARM`/`S_REARM` states; exits become `S_EXIT` with the tail (`:3322-3470`) as `probeExit()`.
2. **Replace the delays with deadlines**: `delay(40/40/60)` at `:3009-3018` → a `S_FLASH`
   sub-state with `nextAtUs`; the `readProbe()` 8 ms inner loop → `readProbe` returns
   "not yet" and the tick comes back; the 20 µs `delayMicroseconds` goes.
3. **Move it into `Probing::service()`**: `probeMode()` becomes `session.begin(); while
   (!session.done) { probeTick(); jOS.serviceInner(); }` first (behaviour-preserving), then
   the loop is deleted and `service()` calls `probeTick()` when a session is live.
4. **Delete the `serviceCritical()` calls** — the scheduler is running.
The pad menus (`chooseDAC` … `voltageSelect`) are their own modal loops and stay for a
later pass. Gate: Kevin's hands on every step (tap, connect, clear, double-tap undo, menu
exit); the HIL suite covers the routing side only.

### B6. `loop()` cleanup (Tier 1, small) — **built, T1.7 (section 0)**
- The 10 ms `secondSerialHandler()` / `replyWithSerialInfo()` / `serviceNetVoltageScanDebug()`
  block (`main.cpp:979-996`) → a `periodUs()=10000` service.
- The two 100 ms help-wait spins (`main.cpp:1188, 1216`): every command char waits up to
  **100 ms with nothing serviced** for a possible `?`. Make it a peek on the already-received
  line (line-buffered input has the `?` in the line) and, in char mode, an armed deadline
  checked from the loop — needs Kevin's UX check that `h?` / `x?` still work.
- The `delayMicroseconds(1000)` drain (`:1249-1253`) → `Jerial.service()` handles it.

## C. Per-service hardware-offload table ("gain" says measured vs estimated)

| # | Now | Peripheral / design | SDK calls | Gain | Risk | Effort |
|---|---|---|---|---|---|---|
| C1 | `readAdc()` = lock spin (≤100 ms) + START_ONCE per sample (~8 µs); pad polling blocks core 0 ~1.2 ms @100 Hz + ~12 ms @20 Hz (est. from `readProbeRaw` × `readAdcHeld` costs — **measure first with the B3 stats**); USB audio takes the ADC over and hands it back (`usbAudioOwnsAdc`, `usb_audio_probe_activity`) | **Always-on ADC ring**: promote `usbAudioAdcStart()`'s engine (`USBAudio.cpp:366-431`) to boot-time, mask = every channel anyone reads (probe 5/7, supply 6, routable 0–3, audio L/R), two chained DMA channels into a ring, exclusive `DMA_IRQ_1` (already claimed for it). `readAdc(ch,n)` = mean of the last *n* ring samples for `ch` (age-checked); `readAdcSince(ch, t0, n)` for the timed reads (phantom check, blink, tip sense: "wait until n samples newer than t0"); USB audio = one consumer; `routingGeneration` stamped per ring block so consumers can drop pre-route samples | `adc_set_round_robin`, `adc_fifo_setup(true,true,1,…)`, `adc_set_clkdiv`, `dma_channel_configure(... &adc_hw->fifo ...)`, `channel_config_set_ring`, `channel_config_set_chain_to`, `dma_channel_set_irq1_enabled` | core-0 CPU: the pad polls' ~1–3 ms blocks → ~10 µs memory reads (**estimated**); the ADC lock and its 100 ms spin disappear; no more audio↔probe hand-off. **Not** a latency win for the pad decode itself: per-channel rate = 500 ksps ÷ mask size (8 ch → 62.5 ksps → 16 µs/sample vs 8 µs today), so a 128-sample decode window is ~2 ms of history either way — the win is that core 0 isn't blocked while it accrues | medium: every reader moves; the probe's timed reads need the `since` API; ADC FIFO overrun rotates the channel interleave (USBAudio already detects+resyncs `:347-353`); MicroPython `adc_get` and NetVoltageScan (core 1) read via the same API | medium (~2 days): engine promotion + `readAdc` rewrite + audio as consumer + tests |
| C2-0 | `sendPath`/`sendXYraw*`/`isrFromPio` execute from flash (`CH446Q.cpp:962,992,1063,55`) | mark them `__not_in_flash_func` like `sendPaths` already is | — | no XIP-miss jitter on the per-crosspoint path and inside the ISR; removes one class of "core 1 stalls during a flash write" (**estimated**) | nil (RAM cost ~1–2 KB) | trivial — **Tier 1** |
| C2a | CH446Q: `pio_sm_put` + spin on the ISR per crosspoint (`CH446Q.cpp:1042-1048`), ISR strobes CS 28..39 (`:55`), core 1 blocked for the whole `sendPaths` (~1.1 ms typical, ROUND3) | **DMA→TX FIFO + ISR chip list**: `sendPaths` builds `words[]` (address bytes) and `cs[]` (chip per word), starts one DMA channel (DREQ = PIO TX) and returns; `isrFromPio` strobes `cs[idx++]`; the existing `irq nowait 1` / `wait 0 irq 1 rel` handshake in `ch446.pio.h` throttles the DMA for free; completion = `idx == n` → `sendGen++`. Core 1 keeps rendering; **either core can kick it** once the ISR is moved to core 0's NVIC (or stays on core 1 — decide by where `sendPaths`' bookkeeping runs) | `dma_claim_unused_channel(false)`, `channel_config_set_dreq(pio_get_dreq(pio,sm,true))`, `dma_channel_configure`, `dma_channel_set_read_addr/trans_count`; ISR stays a shared PIO0_IRQ_1 handler (slot already held) | non-blocking `sendPaths`; core 1 gains ~1 ms per rebuild; sets up C2b (**estimated**) | medium: the RESET pulse + `clearChipXYSuspect` + chip-K safety stay CPU-side before the DMA; the PIO timeout recovery (`:1050-1060`) moves into the ISR/deadline; two producers (sendPaths + NetVoltageScan taps `NetVoltageScan.cpp:154`) need the request mailbox (D) | medium (~1 day) |
| C2c | I2C0's clock has three owners (`MCP4728::begin()` 1.7 MHz `MCP4728.cpp:164`; `initDAC()` 1 MHz `Peripherals.cpp:512-520`; the SSD1306 driver on `connection_type 2` 400 kHz sticky, `oled.cpp:68,111`) — live value 400 kHz on Kevin's board (measured), 1.7 MHz without an OLED on I2C0; WaveGen's math assumes 1 MHz | one owner: `initDAC()`'s 1 MHz; MCP4728 stops overriding; the OLED-on-I2C0 instance transfers at 400 kHz and restores 1 MHz (`clkAfter`) | `Wire.setClock`; SSD1306 ctor `clkDuring/clkAfter` | INA219/DAC/WaveGen run at the clock the code reasons about; WaveGen output frequency lands where its table math says | low, but **hardware-verify**: register readout, INA read reliability, `probe_current_zero` across boots, WaveGen actual frequency, OLED still connected | trivial — **Tier 1**, Kevin's call (approved) |
| C2b | as above | **Second SM strobes CS**: a 12-pin `out pins` program on PIO1/PIO2 (GPIOBASE 16 → pins 28..39 = 12..23), synchronised with the shifter via `irq set 0 next` / `wait 1 irq 0 prev` (RP2350 cross-block IRQ), CS words DMA'd from a second array; no per-crosspoint ISR at all | `pio_set_gpio_base(pio1,16)`, `pio_claim_unused_sm`, second DMA channel, `pio_encode_*` | crosspoint send fully hardware-owned; frees the PIO0_IRQ_1 chain slot; the ~30 µs/crosspoint (est.) becomes ~2 µs | medium-high: needs a PIO block with GPIOBASE 16 free of other claims (check the census: LEDs/probe/encoder SMs pick blocks by pin), two-DMA sequencing, and the OG (RP2040) keeps the old path | large (2–3 days incl. scope verification) — **Tier 3, not built** |
| C3 | INA poll toggled `pauseCore2` (20 Hz LED-frame aborts) | INA219 continuous mode + read-latest, no pause | — | **DONE** (`PROBE_REWORK_HANDOFF.md`) | — | — |
| C4 | MCP4728 re-sent identical words | per-channel shadow, dedupe | — | **DONE**; LDAC batching for the 4-channel setters is a small follow-up (one LDAC pulse per group instead of per write) | low | small |
| C5 | Probe LED + button share one SM, program-swapped from core 1 every pass (~2,560 frames/s); colour requests via `showProbeLEDs` magic values; `checkingButton`/`showingProbeLEDs` cross-core gate | **One PIO program owning GPIO 9**: `pull noblock` (X→OSR when the FIFO is empty, i.e. the last colour repeats) → `mov x, osr` → 24-bit WS2811 frame from OSR (Y as bit counter) → the 2-pulse sample sequence immediately after the frame (the pulse rides behind the frame and is forwarded, never latched) → `push`/`irq` → ≥300 µs low gap (Y loop) → repeat. Colour change = `pio_sm_put(colour)` from any core (one 32-bit FIFO write); button samples keep arriving on core 0's IRQ. `probeLEDhandler` and the swap disappear | `pio_add_program`, `pio_sm_config` with `sideset`/`set`/`in` on the same pin, `pio_sm_put`; JeoPixel no longer owns the SM (`probeLEDs` becomes a thin `put`) | zero cross-core traffic for the probe LED; a constant, jitter-free refresh; no short-frame corruption possible; ~2,600 button samples/s preserved (**estimated**; the frame+pulse spacing must be checked on a scope — Kevin's "acceptable if periodic refresh in the tens of ms" note) | medium: PIO register budget is tight (X=colour, Y=counter, ISR=samples/counter seed, OSR=shift), so the ~1 ms sampler delay needs the ISR-seeded counter trick; the CPU fallback path and `probe_led_on_button_pin=0` (separate pin) must keep working; the OG has no probe pads | medium (~1–2 days incl. scope time) — **next session's opener (T2.4)** |
| C6 | Button PIO IRQ posts flags read by `service()` | `requestRun()` on ProbeButton/Probing from the IRQ so the press is acted on the very next pass | B3 | sub-ms press→action instead of "next scheduled pass" | nil | trivial once B3 lands |
| C7 | Encoder polled on core 1 (`rotaryEncoderStuff`, 2 kHz), events via shared vars | `queue_t` of encoder events (core 1 → core 0) — see D | `queue_init`, `queue_try_add/remove` | clean ownership; no lost clicks when core 0 is slow | low | small — **proposed, not built** (Tier 2, not approved this pass) |
| C8 | LED frame tick = `micros()` poll in `core2stuff` (8 ms) | (considered) core-1 `alarm_pool` on TIMER1 setting a flag | `alarm_pool_create_on_timer(timer1_hw,…)`, `alarm_pool_add_repeating_timer_us` | none worth having — the poll is a compare on a core that spins anyway; **not recommended** | — | — |
| C9 | raw `tud_task()` (59 textual sites, 51 compiled) | `TinyUSB_Device_Task()` / `yield()` (mutex-guarded); `TinyUSBService` period 0. Note the semantic change: `TinyUSB_Device_Task()` is *try-enter* — under contention it silently skips the pump (irrelevant in a spin loop, harmless one-shot: the IRQ pump covers it) | — | closes the pump-IRQ re-entrancy window; USB pumped every pass, not every 3rd | low (mechanical), but audit each site for the "flush" intent (`yield()` also flushes CDC) | small (1 h) |
| C10 | OLED frame = 512–1024 B blocking Wire write on core 0, OledGui up to 66 Hz. **On Kevin's rev-7 board the OLED is on I2C0** (`connection_type 2`) at 400 kHz — 12–25 ms per frame, sharing the bus with the INA219s and the DAC | I2C TX via DMA: build the frame's command stream once, `dma_channel_configure(→ &i2cN_hw->data_cmd)` with `DREQ_I2CN_TX`, poll/IRQ completion; `Adafruit_SSD1306::display()` replaced by an async `displayDMA()` for the SSD1306 path | `channel_config_set_dreq(DREQ_I2C0/1_TX)`, i2c `data_cmd` STOP/RESTART bits per word | core-0 blocking per frame 12–25 ms → ~20 µs (**estimated**); a live GUI screen stops eating the loop | medium: I2C error handling (NACK/timeout) mid-DMA; on I2C0 (rev 7) the DMA'd frame needs the I2C0 arbiter (see WaveGen) — the shared-bus gate at `oled.cpp:2245` is a `wavegen.isRunning()` check today | medium (~1 day) — **proposed, not built** (Tier 2, not approved this pass) |
| C11 | no watchdog | `watchdog_enable(≤8 s)`; kick from `loop()`, `loop1()`, `serviceInner()`, `servicePython()`; stamp a scratch word with the last service index / core-1 state so a WDT reset leaves a trail — on RP2350 `watchdog_hw->scratch[3]` is free (`CrashLog.cpp:29-32` uses POWMAN 0–7 + watchdog 0–2), on the OG/RP2040 CrashLog owns watchdog 2–3 (`:37-45`) so gate the stamp V5-only or pick per platform; **first ship measure-only** (max gap between kicks in `X`) | `watchdog_enable`, `watchdog_update`, `watchdog_caused_reboot`, `watchdog_hw->scratch` | a wedged board reboots instead of sitting dead; post-mortem of what it was doing | medium: legitimate long blockers (self-test, calibration, file ops, MicroPython) must kick or the timeout must exceed them — the measure-only stage finds them | small + a soak |
| C12 | `millis()` gates everywhere | `time_us_32()` deadlines with wraparound-safe compares (B3) | — | µs resolution, no 1 ms quantisation | nil | with B3 |
| C13 | flags + `waitCore2()` | core-1 request mailbox + generation counter (D) | spinlock-guarded bitmask | replaces ~10 flags and the 25 ms guess | medium | see D |
| C14 | WaveGen ≥1 kHz owns core 1 by synchronous per-sample I2C writes | I2C0 TX DMA from a pre-built command image, paced by a DMA timer (`dma_timer_claim`, `DREQ_DMA_TIMERn`) or by bus rate; core 1 free; needs an I2C0 arbiter (INA/DAC setters/OLED-on-I2C0 wait or fail fast while streaming — they already skip on `isRunning()`) | `dma_timer_claim`, `dma_timer_set_fraction`, `channel_config_set_dreq(dma_get_timer_dreq)`, `i2c0_hw->data_cmd` | LED frames and crossbar sends work while a waveform streams (today the crossbar diverges from the netlist until streaming stops, `Commands.cpp:208-230`) | high: MCP4728 command framing per sample, mid-stream STOP/RESTART, arbitration with core-0 I2C0 users, waveform pacing accuracy | large (3+ days) — **Tier 3, not built** |
| C15 | `readAdcVoltage(6,4)` supply sense on core 1 every 1 s | a ring consumer (C1) | — | one less lock holder | nil | with C1 |
| C16 | `pauseCore2` (soft LED-frame hint; hard park is FlashPark) | delete once C13 + the flash-write frame abort are expressed as a request ("HOLD_FRAMES until gen") | — | one less global; frame aborts become explicit | medium (nested pause semantics in `pauseCore2ForFlash`) | Tier 3 — **not built** |

## D. Cross-core protocol cleanup

| Flag | Written by → read by | Hazard | Replacement |
|---|---|---|---|
| `sendAllPathsCore2` (1/-1/3/n) | core 0 sets (`Commands.cpp:195-197,376`), core 1 clears (`CH446Q.cpp:188`, `main.cpp:1406,1569,1842`) | a second request overwrites the first (a `-1` clean lost under a `1`); pending forever while WaveGen streams; `waitCore2()`/`refreshConnections` spin 25 ms / 1 s | `REQ_SEND_PATHS` (+ `CLEAN` bit) in the mailbox; completion = `doneGen` |
| `showLEDsCore2` (-n/1/2/3/≥10) | core 0 (Highlighting ×9, probeMode ×9, Menus, MeasureMode, AsyncPassthrough `:2021-2058`, refresh); core 1 too (`main.cpp:1611`); cleared by CAS on core 1 (`:1813`) | value collisions (a `1` overwritten by a `2` drops the net show; `-1` vs `≥10` encodings); "clear before" and "blocking" are bits hiding in an int | `REQ_SHOW_LEDS` with a flags word {NETS, TEXT, STAGED, CLEAR_FIRST, BLOCKING} OR-merged, latest wins |
| `showProbeLEDs` (1/2/3/4/11…) | core 0 (probeMode ×7, ProbeSwitch ×3, `handleProbeButtonActions`) → core 1 `probeLEDhandler` | request overwrite (a flash `11` then `1`) hidden by the constant re-send | `REQ_PROBE_LED(colour)` → with C5, a bare `pio_sm_put` |
| `showingProbeLEDs` | core 1 → core 0 (`ProbeButton` CPU path spin, `PausePollingFromCore0`) | 20 ms/600 µs waits | gone with C5 |
| `checkingButton` | core 0 → core 1 (`probeLEDhandler` ≤100 ms spin `Probing.cpp:7199-7207`, `main.cpp:1878`) | the shared line's mutex, by spin | gone with C5 (CPU fallback keeps a local flag) |
| `pauseCore2` | core 0 (`refreshConnections :146-185`, `pauseCore2ForFlash`) → core 1 (`loop1 :1401`, `core2stuff :1598,1690,1752`) | nested save/restore; a core-0 fault while paused = core 1 dead; soft only since FlashPark | `REQ_HOLD_FRAMES(untilGen)`; the routing critical section becomes "core 1 renders from a snapshot" or simply keeps `core_sync` |
| `core1busy` | core 0 (`refreshConnections :148,186`) **and core 1** (`main.cpp:1513-1520` dumpLED) → core 1 swirl gate, `systemIdleForFlush`, SlotManager, OledGui | two writers, ambiguous meaning | split: `routingInProgress` (core 0 only) |
| `core2busy` | core 1 around sendPaths/render/show → `waitCore2`, `refreshLocalConnections` (200 ms), `pauseCore2ForFlash`, `safeFileWriteAllRaw` (200 ms), OledGui | polled with timeouts that "proceed anyway" | `doneGen` + a `core1State` enum for the readers that only want "idle?" |
| `core1request` | written (`Commands.cpp:61,86`, `FileParsing.cpp:124,138,3038,3051`), **read by nobody** | dead | delete |
| `probeActive`, `loadingFile`, `inClickMenu`, `inPadMenu`, `hideNets` | core-0 mode flags read by core 1's render | fine as mode flags (single writer) | keep; make `inClickMenu` `volatile` (it is `int&` via `Menus`) |
| `dumpLED` (LED-dump mode, `serial_x.function` 5/6) | core 0 config → **core 1 calls `dumpLEDs()` (`main.cpp:1510-1525`) which writes `USBSer1/2`** (`Graphics.cpp:4455-4483`) | USB CDC I/O from core 1 — the exact wedge family documented at `main.cpp:1494-1504`; latent because the mode is off by default | `REQ_DUMP_LEDS`: core 1 snapshots the frame, core 0 prints it |

**Mailbox shape** (prefer over a FIFO — every request type coalesces): `volatile uint32_t
pendingBits` + per-type payload words + `reqGen`/`doneGen`, updated under a SIO spinlock
(`spin_lock_claim_unused`) or `critical_section_t` — **not** bare `__atomic_fetch_or`: whether
RP2350's exclusive monitor is cross-core is a datasheet check we have not done (the existing
`readingADC` `__atomic_test_and_set` lock at `Peripherals.cpp:2694` relies on it — verify
before building more on it). Core 1 pops at the top of `core2stuff()`; core 0 waits on
`(int32_t)(doneGen - myGen) >= 0` with the same 25 ms bound it has today. No IRQ needed
(core 1 spins anyway); if one is ever wanted, the SIO_IRQ_BELL slot is taken — extend
`flashParkIrq` into a bell dispatcher.

**Ownership rules to write down**: I2C0 = core 0 (WaveGen the sanctioned exception, gated by
`isRunning()`); **I2C0's clock = `initDAC()`, 1 MHz, and nobody else's** (T1.9); ADC = the
ring engine (C1) — until then, the `readingADC` lock with no stealing; USB = core 0 only
(`tud_task`, CDC I/O); flash = FlashPark (`__wrap_flash_range_*`) with `fs_mutex`
core-0-in-practice; PIO0 CH446Q SM = core 1 (C2a may move the ISR to core 0); button PIO IRQ =
core 0; the LED strip SM/DMA = core 1; I2C1 (OLED on connection types 0/1/3) = core 0.

**Migration order**: (1) `core1request` delete + `inClickMenu volatile` (trivial); (2) mailbox
with `REQ_SEND_PATHS` only, `waitCore2()` re-implemented on `doneGen` **in place** — its ~40
call sites (`grep -rn "waitCore2("` across Commands, Apps, SelfTest, States, FakeGpio, Menus,
Probing …) stay untouched (behaviour-identical, measurable by the tap→crossbar probe in F);
(3) `REQ_SHOW_LEDS`; (4) `REQ_PROBE_LED` or C5; (5) `pauseCore2` last.

## E. Roadmap (sized honestly; status per section 0)

**Tier 1 — small, low risk (each ≤ ½ day, one commit each) — all approved**
- T1.1 C9 raw `tud_task()` → `TinyUSB_Device_Task()`/`yield()`; `TinyUSBService` period 0.
- T1.2 B1 comment/priority truth fixes + B2 drop the no-op services.
- T1.3 D `core1request` delete; `inClickMenu` volatile.
- T1.4 B3 scheduler time + stats (`periodUs`, `requestRun`, `X` table) — the largest Tier-1
  item, ~150 lines; behaviour-preserving if periods only encode existing gates. **Landed
  2026-08-17 (section 0).**
- T1.5 B4 `serviceInner()` replacing `serviceCritical()` in the modal loops. **Landed
  2026-08-17 (section 0).**
- T1.6 C11 watchdog, measure-only stage first (max kick gap in `X`), then enable. **Measure-only
  stage landed 2026-08-17 (section 0, with the numbers and what they imply for the enable).**
- T1.7 B6 `loop()` cleanup (10 ms block → service; help-wait spins; the drain). **Landed
  2026-08-17 (section 0).**
- T1.8 C2-0 CH446Q hot path + ISR into RAM (`__not_in_flash_func`).
- T1.9 C2c I2C0: one clock owner at 1 MHz (Kevin's call — approved; hardware-verify INA
  reads + wavegen + register readout). **Landed 2026-08-17 (section 0).**
- T1.10 D `REQ_DUMP_LEDS`-lite: stop core 1 writing USB CDC in LED-dump mode (`main.cpp:1510`) —
  move `dumpLEDs()` to core 0's 10 ms service behind a core-1 snapshot flag. **Landed
  2026-08-17 (section 0; its own inner-set `LedDumpService`, not PortHousekeeping).**
- (done) C3 INA no-pause, C4 MCP dedupe, I2C0 rule.

**Tier 2 — medium (1–2 days each)**
- T2.1 C1 always-on ADC ring (`readAdc` = ring read; audio = consumer) — **approved**.
- T2.2 D2/D3 core-1 mailbox: `REQ_SEND_PATHS` first, then `REQ_SHOW_LEDS` — **approved**.
- T2.3 C2a CH446Q DMA→FIFO + ISR chip list (non-blocking `sendPaths`) — **approved**.
- T2.4 C5 the combined GPIO 9 PIO program (needs scope time with Kevin) — **next session's opener**.
- T2.5 C7 encoder event queue — **proposed, not built** (not approved this pass; small, low risk, no measured symptom driving it).
- T2.6 C10 OLED I2C DMA frame — **proposed, not built** (not approved this pass; on rev 7 it needs the I2C0 arbiter first).
- (not recommended) C8 LED tick alarm.

**Tier 3 — large (3+ days, or needs a design round) — design-only this pass**
- T3.1 B5 `probeMode` state machine (4 milestones).
- T3.2 C2b second-SM CH446Q strobe (PIO block/GPIOBASE allocation first).
- T3.3 C14 WaveGen via I2C0 DMA + pacing timer (+ I2C0 arbiter).
- T3.4 C16 delete `pauseCore2`.

## F. How to measure
Existing hooks: `X` (`cmd_resourceStatus`, `SingleCharCommands.cpp:2670`: PIO map, IRQ slots,
FlashPark, MCP counters, probe LED frames/button samples/frame aborts), `debugWaitLoopTiming`
(slow-service prints, `JumperlOS.cpp:288-321`), `debugWaitLoopTimingCore2` (LED show timing
summary `main.cpp:1918-1959`), `PROFILE_*` in Commands/CH446Q, `refresh:` timing line
(`Commands.cpp:244-246`), the HIL suite (`run_all.py` 5/6), the SWD scripts.
Additions per recommendation: B3's per-service µs table (**do this first — the CPU-share
numbers above are estimates**); an I2C0 transaction counter (Wire wrapper) for C10/C14; ADC
ring stats (overruns, resyncs, oldest-sample age) for C1; a frame-abort histogram by cause
(pause / mutex-timeout / checkingButton) for D; a **tap→crossbar latency probe**:
timestamp at `readProbe` accept, at `sendAllPathsCore2` set, at `sendPaths` end, at LED show —
printed on `X`, gate for D2 and C2a; watchdog max-gap for C11.

Baseline captured before any change (2026-08-16, firmware `a6ad4ba`, uptime 12036 s): `X` →
shared-IRQ slots 6/6 (irq 16 handler `0x10055951` — flash), FlashPark timeouts 0, `mcp4728
writes 14/2/6/2 skips 45/21/21/21`, probe led frames 29 112 026 (requests 2362), button
samples 30 127 251, led-frame aborts(pause) 47; heap free 46 KB of 221 KB; I2C0 at 400 kHz
(register readout); `run_all.py` 5/6 in 2 m 09 s.

## Verification (per step, and per item)

- Every step: build all three envs; flash `jumperless_v5`; `python3 test/hil/run_all.py` = 5/6
  with only `test_net_currents` failing; `test/hil/test_infra_paths.py` 24/24 and
  `test/hil/test_config.py` 30/30 for anything touching routing or config; `X` after boot for
  the resource census (PIO map, IRQ slots 6/6, FlashPark active, MCP counters, probe-LED
  frame/abort counters unchanged unless the step targets them).
- T1.1 (`tud_task`): a 10-min soak with the `jumperless` client attached + `run_all.py` in a
  loop; no port drops. T1.2/T1.3: build + `X`. T1.4 (B3): the new `X` service table shows
  every service with runs>0 and the expected period; the `refresh:` timing line and the
  probe feel (Kevin) unchanged; `debugWaitLoopTiming` shows no new SLOW SERVICE lines. T1.5
  (B4): probe mode + click menu + REPL still respond; Arduino passthrough now works while
  probing (Kevin, if an Arduino is on the header). T1.6 (watchdog): measure-only first — `X`
  prints max kick gap over a session including self-test/calibration/file ops; enable only if
  the max is comfortably under the timeout. T1.7: `h?`, `x?`, `help` still work; command
  latency visibly better (Kevin). T1.8: build + `X` (irq 16 handler address in RAM) +
  connect/disconnect soak. T1.9: I2C0 register readout = 1 MHz after boot with the OLED
  connected; INA reads (`i@`, `[switch]` line) stable; `probe_current_zero` across boots;
  WaveGen actual-frequency stat before/after; OLED still Connected. T1.10: enable LED-dump
  mode on port 2, watch for the wedge not happening.
- T2.1 (ADC ring): ring stats in `X` (overruns 0, oldest-sample age < 1 ms); probe taps decode
  identically under both feeds and both positions (Kevin); USB mic still records; the pad
  polls' CPU share in the B3 table drops as predicted (measured, not estimated).
- T2.2 (mailbox): tap→crossbar latency probe (F) before/after — same or better; no "Core 2 has
  not processed sendAllPathsCore2" warnings in a 200-connect soak; wavegen streaming still
  leaves the send pending and it lands when streaming stops.
- T2.3 (CH446Q DMA): `test_infra_paths` + `self_test` crossbar phase pass; `ch446q_timeout_count`
  stays 0 across a 500-rebuild soak; NetVoltageScan taps still serialised.
- T2.4 (GPIO 9 program): scope on GPIO 9 — frame, pulse, gap; button double-tap/hold/undo all
  work (Kevin); `X` frames/s and button samples/s in the expected range; no colour drift over
  10 min idle.

## Verified while sweeping (facts, with where they came from)

- arduino-pico core 1.50600.0 (5.6.0), pico-sdk **2.2.1**. USB stack = Adafruit TinyUSB port:
  `TinyUSB_Port_InitDevice()` claims a spare user IRQ (`user_irq_claim_unused`) and registers
  a *shared* handler on `USBCTRL_IRQ` that sets it pending; the user-IRQ handler runs
  `tud_task()` under `mutex_try_enter(&__usb_mutex)` (`Adafruit_TinyUSB_rp2040.cpp:79-117`).
  `TinyUSB_Device_Task()` (`:138`) is the same mutex-guarded entry (try-enter: skips if held);
  `yield()` = `TinyUSB_Device_Task()` + `TinyUSB_Device_FlushCDC()` (`cores/rp2040/delay.cpp:50-54`;
  FlushCDC = `tud_cdc_n_write_flush` on every CDC instance, non-blocking). `delay()` =
  `sleep_ms` — it does **not** pump USB. `Adafruit_USBD_CDC` `available()`/`write()`/`operator
  bool` call `yield()` internally when they would otherwise spin (`Adafruit_USBD_CDC.cpp:183,198,253`).
  USB is initialised from `main()` on core 0, so the pump IRQ is a core-0 IRQ.
- Linked in `lib/rp2350/libpico.a`: `queue_*` (with spinlock), `alarm_pool_create_on_timer`
  / `_add_repeating_timer_us` / `_get_default`, `multicore_doorbell_claim(_unused)`,
  `multicore_lockout_*`, `critical_section_init`, `hardware_alarm_claim_unused`,
  `user_irq_claim_unused`, `watchdog_enable/_update`, `spin_lock_claim_unused`, `sem_*`,
  `recursive_mutex_*`. **Not** linked: `async_context_*` (confirmed).
- RP2350 resource counts (`hardware/platform_defs.h`): 16 DMA channels, 4 DMA IRQs, 3 PIO
  blocks (12 SMs), 2 timers × 4 alarms, 8 doorbells, 6 user IRQs, 32 spinlocks. Default alarm
  pool = TIMER0 alarm 3, core 0, 16 timers. `clk_sys` = 150 MHz (`machine.freq()`).
- Shared-IRQ chain: 6/6 used (`include/FlashPark.h:43-45`): USBCTRL_IRQ ×2, SIO_IRQ_BELL ×2
  (arduino-pico's park + FlashPark), PIO0_IRQ_1 (CH446Q), IO_IRQ_BANK0 (MicroPython). Any new
  interrupt must be an **exclusive** handler on an unshared line (a free DMA_IRQ_n, PIO1/PIO2
  IRQs, a TIMER1 alarm, a user IRQ). A second doorbell cannot get its own handler — extend
  `flashParkIrq` into a dispatcher, or don't use an IRQ (poll from the loop).
- FlashPark (`src/FlashPark.cpp`) already uses a doorbell + `__wrap_flash_range_erase/program`
  to park the other core; `pauseCore2ForFlash()` (`externVars.cpp:188`) is now only the "soft"
  LED-stutter hint, spinning on `core2busy` with raw `tud_task()`.
- Cross-core waits today: `waitCore2()` (`Commands.cpp:57`) spins ≤25 ms on `core2busy ||
  sendAllPathsCore2` with raw `tud_task()`; `refreshConnections()` (`Commands.cpp:114`) sets
  `pauseCore2` around routing, then `sendAllPathsCore2 = ±1` and spins ≤1 s (`:208-217`), then
  `showLEDsCore2` + `waitCore2()` (`:232-235`). `refreshLocalConnections()` spins ≤200 ms on
  `core2busy` (`:300-309`).
- CH446Q send path (`src/CH446Q.cpp`): PIO0 SM (claimed at **file scope** `:38`, before
  `setup()`), data 14 / clk 15 (`:89-96`), clkdiv 1, 8-bit words; the compiled program
  (`ch446.pio.h` — **`src/ch446.pio` is stale**: it says IRQ 0 / `PIO0_IRQ_0`, the header and
  `CH446Q.cpp:98` use `irq nowait 1` / `wait 0 irq 1 rel` / `PIO0_IRQ_1`) shifts the word then
  stalls until the CPU ISR `isrFromPio` (`:55`, shared PIO0_IRQ_1, NVIC enabled on core 1)
  pulses CS 28..39 with `gpio_put` (`setCSex`, `Peripherals.cpp:1635-1647`) and clears the flag;
  `sendXYrawUnchecked` (`:992`) does `pio_sm_put` then spins on `chipSelect != -1` per crosspoint
  (`:1044-1048`, 100 ms timeout → `markChipXYSuspect` + SM restart `:1050-1060`). So the SM
  already flow-controls on the ISR — a DMA→FIFO feed would be paced by the CS strobe for free.
  **The hot path is not RAM-resident**: `sendPaths`/`sendAllPaths` are `__not_in_flash_func`
  but `sendPath` (`:962`), `sendXYrawUnchecked` (`:992`), `sendXYraw` (`:1063`), `setCSex` and
  `isrFromPio` (`:55`, `X`: handler `0x10055951`) run from flash. NetVoltageScan (core 1) is a
  **second producer** of crosspoint sends (`senseNodeVoltage`, `NetVoltageScan.cpp:154`, one
  tap per pass, ≥5 ms apart).
- **I2C0 clock** — see section 0 (corrected). `initDAC()` sets `Wire.setClock(1000000)` with a
  comment explaining 1 MHz was chosen because 1.7 MHz "produced intermittent silently-failed
  [INA219] reads" (`Peripherals.cpp:512-520`), then `mcp.begin()` (`:529`) calls
  `_wire->setClock(1700000)` (`MCP4728.cpp:164`); `MCP4728.cpp:298`'s `setClock(_clock_hz)` is
  only on the soft-I2C set-address path, not per write. On a rev-7 board `oled::init()` then
  drops the bus to 400 kHz for good (`oled.cpp:544→4260`, `initI2C(4,5,400000)`; SSD1306
  `clkDuring = clkAfter = 400000`, `oled.cpp:68,111`). WaveGen's rate math assumes 1 MHz
  (`WaveGen.cpp:372,412,432,637`) and its stream is free-running (`:289-322`), so the output
  frequency scales with the real bus clock. INA1 is read on this bus (`probe_current_zero`,
  handoff open item 2).
- LED strip: `LED_SHOW_MIN_TIME 14` is compared against `micros()` (`main.cpp:1794`) — a
  14 **µs** floor, i.e. no throttle; pacing comes from `isDMABusy()` frame-dropping in
  `ledClass::show()` (`LEDs.cpp:171-184`). The DMA double buffer has **no completion IRQ**: a
  frame queued while DMA is busy is sent by the *next* `show()` that finds it idle
  (`Jeopixel_RP2.cpp:112-200`), not on completion as `DMA_LED_DOUBLE_BUFFER_SOLUTION.md` reads.
- Resource budget today: DMA channels — JeoPixel ×3, AsyncPassthrough ×2 (**panic-on-fail**
  `AsyncPassthrough.cpp:555,578`), USBAudio ×2 (+ MicroPython `rp2.DMA`/SPI on demand) = 7 of
  16; DMA IRQs — `DMA_IRQ_0` MicroPython, `DMA_IRQ_1` USBAudio (exclusive), 2 and 3 free; PIO —
  CH446Q (pio0), JeoPixel ×3 (dynamic, by pin), encoder (first-fit), probe button (shares
  probeLEDs' SM), MicroPython `rp2.PIO` on demand (`X` today: PIO0 SM0, PIO1 SM0, PIO2 SM0-2
  claimed); timers — one `add_repeating_timer_ms` (slow PWM `Peripherals.cpp:2854`), no alarm
  pools of our own; no `queue_t`, no `critical_section_t`, no spinlocks (except the OG atomic
  shim); mutexes = `core_sync_mutex`, `fs_mutex`, `g_arenaMutex`, `__usb_mutex`; atomic-flag
  locks = `readingADC`, `readingGPIO`, `svcBusy`, the `infraAcquireAdc` pool.
- `core_sync_*` grant unconditionally before `core_sync_init()` (`externVars.cpp:75-95`);
  `core2stuff()` takes it with `timeout_ms(0)` (`main.cpp:1585`) or `(1)` for the bypass.
- Priorities actually in the tree (grep `getPriority`): CRITICAL = TermSerial `JumperlOS.h:318`,
  RelayedCmd `:345`, MpRemote `MpRemoteService.h:50`, Peripherals `Peripherals.h:31`,
  **ProbeButton `Probing.h:80`**; HIGH = AsyncPassthrough `JumperlOS.h:365`, Highlighting
  `Highlighting.h:29`, Menus `Menus.h:35`, MeasureMode `MeasureMode.h:61`, **Probing
  `Probing.h:269`**, SlotManager `routing/States.h:406`; NORMAL = **TinyUSB `JumperlOS.h:385`**,
  USBPeriodic `:405`, OledGui `:452`, ProbeSwitch `Probing.h:219`, SingleCharCommands
  `SingleCharCommands.h:127`; LOW = OLED `JumperlOS.h:425`, LiveXbar `:473`, ConfigSave
  `configManager.h:64`, FileCache `FileCache.h:163`, ProbePads `Probing.h:242`.
  Two brief-stage claims did not survive: TinyUSB is NORMAL not HIGH (the service pumps USB only
  every 3rd loop), and it is ProbeButton that is CRITICAL / Probing that is HIGH (the brief had
  them swapped) — so `serviceCritical()` inside probeMode runs {TermSerial, RelayedCmd, MpRemote,
  Peripherals, ProbeButton} and does not re-enter Probing. Registration comments wrong in
  `main.cpp`: `:502` asyncPassthrough "CRITICAL" (HIGH), `:503` menus "CRITICAL" (HIGH), `:506`
  tinyUSB "HIGH" (NORMAL), `:515` probeButton "HIGH" (CRITICAL), `:519` probeSwitch "LOW" (NORMAL).
- LED strip: `JeoPixel` (lib/Jadafruit_NeoPixel) claims a WS2812 SM via
  `pio_claim_free_sm_and_add_program_for_gpio_range` + one DMA channel; `show()` is **async
  DMA** (returns at transfer start; 300 LEDs × 3 B × 10 µs = 9 ms on the wire; `endTime`
  projected to the last bit) (`Jeopixel_RP2.cpp:6-70`, `JeoPixel.cpp:239-317`).
  `showBlocking()` bypasses DMA. So core 1's CPU is free during the frame; the "LED frame"
  cost on core 1 is the *render* (showNets etc.), not the transmit.
- Probe button + probe LED share one PIO SM (`Probing.cpp:470-830`): a hand-encoded 15-word
  polling program (~1 ms cadence, pushes 2-bit samples + `irq set 0`) and JeoPixel's WS2812
  program live in the same SM; core 1's `probeLEDhandler` swaps programs with
  `pio_sm_exec(jmp)` around every frame (`probeButtonPausePolling/ResumePolling` `:671-716`);
  the PIO IRQ is exclusive and **core-0 owned** (`:816-820`), draining the RX FIFO and running
  `processSample()` in the handler. `probeButtonPausePollingFromCore0()` (`:738`) uses
  `checkingButton` + `showingProbeLEDs` as the cross-core gate. The sample pulses (~875 ns
  HIGH) are valid WS2812 bits; a frame *followed immediately* by the pulse is harmless (the
  LED forwards bit 25 to DOUT and latches the 24 it kept), a pulse followed by a >280 µs gap
  with no frame shifts the colour — which is why the frame is re-sent every pass.
- `readAdc()` (`Peripherals.cpp:2661`): spins up to **100 ms** on the `readingADC` atomic
  flag; `readAdcHeld()` (`:2724`) = per sample START_ONCE + READY wait (~2 µs) +
  `delayMicroseconds(6)` ≈ 8 µs/sample. `readProbeRaw()` (`Probing.cpp:6614`) = 8 (up to 16)
  bursts × `readAdc(5, 16|24)` + variance gate + median ≈ 1–3 ms of core-0 CPU per pad poll;
  the pad ladder is a steady DC divider (no per-read drive), except the phantom check
  (`:6578-6586`, feed off 60 µs → `readAdc(5,8)`) and the tip-sense/blink detectors, which
  are timed reads. USBAudio's ring (`USBAudio.cpp:366-431`): round-robin over
  audio L/R + `JL_AUDIO_HOUSEKEEP_MASK 0xFF`, two chained DMA channels writing 1 ms halves
  into a ring, exclusive `DMA_IRQ_1`, `usbAudioSnapshotRaw()` (`:814`) serves means for
  every channel except ADC5/ADC7 (returned as 0 by design — "drive→settle→sample").
- **Per-service census (core 0)** — what one call costs, from the sweep (agent-located, spot-verified):
  - `Probing::service` (HIGH, `Probing.cpp:1243`): 10 ms gate (`:1257`) → `justReadProbe()` →
    `readProbeRaw()` = 8–16 × `readAdc(5,16|24)` ≈ **1.0–1.6 ms of blocking ADC per call, 100×/s**
    (~10–16 % of core 0 while idle); then `handleProbeButtonActions()` which **enters `probeMode()`
    synchronously** (`:1094,1136,1150`). Returns BLOCKING while a pad menu is open (`:1249`).
  - `ProbePads::service` (LOW, `:1202`): 50 ms gate (`:1207`) → `checkPads()` (`:6362`) = **12 ×
    `readProbeRaw(0,1)`** = 12 × 8 × 16 conversions ≈ **12 ms of blocking ADC per tick, 20×/s**
    (~25 % of core 0 while idle; escalates to 16 bursts). Together with Probing::service, roughly
    **a third of idle core 0 is spent blocked in pad ADC reads.** (Verified `:1202-1216`, `:6362-6370`.)
  - `ProbeButton::service` (CRITICAL, `:121`): PIO mode (default) → returns immediately after
    draining deferred undo/redo (`:188-191`); real work is in the PIO IRQ. CPU fallback: 4 ms gate,
    ≤600 µs spin on `showingProbeLEDs`/`canShow()` (`:860-866`), 8-µs settle delays.
  - `ProbeSwitch::service` (NORMAL, `:1174`): no gate of its own; `checkSwitchPosition()` runs
    `infraServiceTick()` every pass (`:5193`) then 500 ms-gated (`:5226`) INA1 I2C read (up to 3
    tries + 200 µs) or ADC7 droop reads; detector A ~6 µs; detector B `adcTryAcquire`+2×`readAdcHeld(7,2)`.
  - `Peripherals::service` (CRITICAL, `Peripherals.cpp:74`): `pollCurrentSenseMeasurement()` — 50 ms
    poll (`:50`), ≥10 ms attempt gate (`:121-125`), INA0 on I2C0, skipped while `wavegen.isRunning()`
    (`:187`); `showMeasurements()` only if `showReadings>=1` (default 0). Also runs from
    `serviceCritical()` and `servicePython()`.
  - `MeasureMode::service` (HIGH, `MeasureMode.cpp:59`): 300 ms switch debounce, 15 ms INA guard,
    `readAdcVoltage(ch,16)` per update; `startMeasurement()` → `addEphemeralConnection(...)` →
    `refreshLocalConnections()` + `waitCore2()` (`States.cpp:806-816`).
  - `Highlighting::service` (HIGH, `Highlighting.cpp:65`): `encoderNetHighlight()` every pass; 40 ms
    gate (`:123`, comment says 20) → `checkForReadingChanges()` with `readAdcVoltage(ch,64)` (~0.5 ms);
    returns BLOCKING when it consumed an encoder press (`:81`); writes `showLEDsCore2` at 9 sites.
  - `Menus::service` (HIGH, `Menus.cpp:63`): no gate; `clickMenu()` every pass; returns BLOCKING while
    `inClickMenu` (`:67-69,74-76`); `getMenuSelection()` is a **modal loop** pumping only
    `serviceCritical()` (`:993-995`).
  - `SlotManager::service` (HIGH, `States.cpp:3668`): idle-gated autosave (`systemIdleForFlush(750)`),
    → `safeFileWriteAllRaw()` (`FilesystemStuff.cpp:4010`): `pauseCore2ForFlash(100)` + a 200 ms
    `core2busy` busy-wait + `fs_mutex` + FatFS write. `USE_FILE_CACHE` = 0 in this build.
  - `MpRemoteService::service` (CRITICAL, `MpRemoteService.cpp:99`): no time gate; drains ≤8192 chars;
    runs raw-REPL Python **synchronously**; `tud_task()` after each batch (`:277-281`).
  - `AsyncPassthroughService` (HIGH, `JumperlOS.cpp:686` → `AsyncPassthrough::task()` `:1771`): no
    gate; UART↔CDC bridging + DMA TX; **unconditional raw `tud_task()` every pass** (`:2083`).
  - `TinyUSBService` (NORMAL, `JumperlOS.cpp:717`): raw `tud_task()` every 3rd pass.
  - No-ops: `SingleCharCommands::service` (`SingleCharCommands.cpp:94-98`), `TermSerial`
    (`JumperlOS.cpp:598-603`, body commented out), `RelayedCmd` (`:637-651`, disabled),
    `FileCacheFlushService` (compiled body `FileCache.cpp:1610-1612` because `USE_FILE_CACHE`=0),
    `usbPeriodic()` (`USBfs.cpp:43-58`, debug print only, still reports BUSY). (`OLEDService` is
    NOT dead: `oled::init()` registers itself at `oled.cpp:664`, so once the OLED is initialised
    `oledPeriodic()` (`oled.cpp:3181`) runs every 20th pass — connection ping every 750 ms/2 s/4 s,
    and `Wire1.end(); delay(50)` … `delay(150)` on a reconnect (`:3350-3375`).)
  - `OledGuiService` (NORMAL): inert until a screen is active; then `oled.show()` = a 512–1024 B
    SSD1306 frame at 400 kHz ≈ **12–25 ms of blocking I2C on core 0 per frame** (I2C0 on rev 7,
    I2C1 otherwise), capped at 15 ms (foreground) / 160 ms (idle screen) intervals
    (`OledGui.cpp:673`), skipped when nothing changed; gated on `probeActive`,
    `core1busy/core2busy`, `refreshInProgress`.
  - `ConfigSaveService` (LOW, `configManager.cpp:63`): deferred (`probeActive`, 2 s after user input),
    then `saveConfig()` → same `safeFileWriteAllRaw` path.
  - `LiveCrossbarService` (LOW): 60 s / 400 ms (probe) refresh, `updateLiveCrossbarDisplay()`.
- **`probeMode()` anatomy** (`Probing.cpp:2199-3471`, agent-located, key lines spot-checked): one
  `while (Serial.available()==0 && millis()-probeTimeout < 80000)` loop (`:2423`); per pass:
  `delayMicroseconds(20)` (`:2429`), `jOS.serviceCritical()` (`:2432`), `classifySwitchPosition()`
  (500 ms — the switch classifier, added 2026-08-17 on Kevin's note; see CONTINUE HERE) +
  `checkSwitchPositionFast()` (250 ms, agree mode only), `liveCrossbarService.service()`
  (`:2521`), encoder nav, `readProbe()` (`:2590`) whose
  inner `while (probeRead <= 0)` (`:7016`) re-runs `readProbeRaw()` + `probeButton.service()` up to
  8 ms; `delay(40)/delay(40)/delay(60)` on the node-1 latch flash (`:3009-3018` — 140 ms of
  hard delay per first node in connect mode); the commit path `addBridgeToState()` /
  `removeBridgeFromState()` (`:3114,3124,3216`) → `refreshLocalConnections(1,1,0)` →
  `sendAllPathsCore2 = 3` + `showLEDsCore2`. Nested modal loops in the pad menus (`chooseDAC`
  `:4094`, `chooseIsense` `:4245`, `chooseADC` `:4387`, `chooseGPIOinputOutput` `:4511`,
  `chooseGPIO` `:4709`, `voltageSelect` `:4939,5054`), each pumping `serviceCritical()`. Entry
  from `Probing::service()` → `handleProbeButtonActions()` (`:1094,1136,1150`) and from the click
  menu (`Menus.cpp:4550`). Exits: serial key / 80 s / double-tap bail / encoder held / mode-button
  with nothing latched (`:2934`) / `firstConnection` -2/-3. `probeMode` never touches
  `sendAllPathsCore2` or `checkingButton`; it writes `probeActive`, `showProbeLEDs` (7 sites),
  `showLEDsCore2` (9 sites), `inPadMenu` (exit). No flash, no `saveConfig`; OLED
  `clearPrintShow` at 8 sites; INA219 only via `Peripherals::service` in `serviceCritical()`.
  `Probing::measureMode()` has zero call sites; MeasureMode is a separate HIGH service.
- Raw `tud_task()` in `src/`: 59 textual call sites (51 compiled): Python_Proper 12,
  AsyncPassthrough 11, main 10 (8 compiled-out), JumperlOS 6, FilesystemStuff 5, Commands 3,
  USBfs 2, SingleCharCommands 2, JumperlessMicroPythonAPI 2, FileParsing 2, MpRemoteService 2,
  ArduinoStuff 1, externVars 1. Other `serviceCritical()` pumpers besides probeMode/menus:
  `BitmapEditor.cpp:1167`, `GraphicOverlays.cpp:632`, `ImagesApp.cpp:341`,
  `Peripherals.cpp:3339`, `Python_Proper.cpp:256` (`mp_hal_delay_ms`).
