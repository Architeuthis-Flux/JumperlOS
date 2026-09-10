# Handoff 2026-09-07 evening - Jumperless-docs sweep against 5.7.11.0

Kevin's ask (from GitHub issue #21 "Hello world type projects?" and a new
user's "getting started" comment): sweep every docs page against the current
firmware, rename Basic Controls to Getting Started with collapsible
follow-along steps, simplify the GPIO page, and add a page for the preloaded
examples. Work landed in `~/Documents/GitHub/Jumperless-docs` (one commit on
its main branch, not pushed). This note is the firmware-side fallout.

## What the sweep did (docs repo)

- Audited 19 pages (20 slices) against `JumperlOS-main` (5.7.11.0, f2f4137)
  with one auditor + one verifier per slice: 308 findings, 271 confirmed,
  27 partly, 9 refuted. Findings and verdicts:
  `scratchpad/sweep_result.json` of session e5bc5fb4 (also
  `findings/*.json` per page). Every confirmed/partly finding was applied.
- New `01-getting-started.md` (old URL redirects), new `08.5-examples.md`,
  `05.7-gpio.md` rewritten plain, `09.8-odds-and-ends.md` lost its two
  duplicate blocks (Writing Native apps -> `11-WritingApps.md`, LLM tool spec
  -> `07.5-automation.md`), `07.5-automation.md` node map -> API reference.
- mkdocs: `admonition`, `md_in_html`, `pymdownx.details` enabled (the
  `!!! warning` on Basic Controls had been rendering as literal text);
  JFS and Writing Apps are in the nav now; `jumperless-agent-skill.md` is
  `not_in_nav`.
- Local `mkdocs build` is clean (zero warnings).

## Firmware-side follow-ups (nothing here was changed in JumperlOS)

Guided projects cannot wire their parts in 5.7.11.0 (found while
verifying the Examples page, read in the release tree):
- Opening a project loads its parts with `placed: false` (no `placed:` key in
  any shipped wiring) and `expandPartsToBridges` skips unplaced parts. The
  only callers of `applyPartPlacement` are the Parts app (a NEW part from the
  DB, power bridges only), MicroPython `place_part` (needs the full pin JSON)
  and DisplayService. The blocking guide that used to commit a part on
  confirm went out with Guides-Simplification (2026-08-24); what shipped is
  the StepViewer (wheel browses, hold closes, `z steps ...`) plus the
  on-demand `z check <part>` / `z check step <k>`. No gesture places a
  project's own part, and all four projects ship without `bridges:`, so none
  of them can be built from the board alone. test_projects.py phase 6(e)
  asserts the unconnected load and its comment still assumes "the READMEs
  send people through the guided build".
- The docs now say opening a project does not make the connections and give
  the 555's `+` line (derived from wiring.yaml's `connect:` fields and the
  dip8 row math in jl_part_clamp_fingerprint / nodeForPin). Remove that box
  once a placement gesture exists.
- 555/wiring.yaml's first guide note says "Turn, click, hold=exit"; click
  does nothing in the viewer (it opens the menu).
- The four READMEs describe the old blocking guide throughout: confirm/skip
  (`s`), quit (`q`), compact vs expanded tap, "tap a free hole to move pin
  1", `m <row>` / `c`, the resume-or-start-over prompt, "Run main.py when it
  offers at the end". None of that exists in the release.

Shipped on every board (`/projects/*/README.md`, provisioned):
- All four project READMEs say "Clickwheel: `Guides` - it is a top-level menu
  row, before `Apps`". That row was retired 2026-08-24 (comment above "Parts"
  in `src/remembering/menuTree.h`); `apps[]` keeps `{ "Guides", 26, ... }`
  for name dispatch only. Real launch paths: `z <name>` on the terminal, or
  click the project's `wiring.yaml` in the Files browser. The docs describe
  the real paths; the READMEs on the board still describe the dead one.
- `scripts/ex/README.md` (repo only, not provisioned) still says
  `pythonStuff/ex/` and lists ten examples; the generator reads `scripts/ex/`
  and ships 24.

Example scripts (`scripts/ex/`, all shipped), found by the extractors and
checked against the code:
- `led_brightness_control.py`: `voltage` is only assigned inside
  `if pad != NO_PAD`, but `oled_print` uses it every loop, so it raises
  `NameError` on the first iteration unless the probe is already on a pad.
  Effectively broken as shipped.
- `voltage_monitor.py`: docstring says row 20, code connects ADC0 to row 12.
- `uart_loopback.py`: its printed instructions say to open the second serial
  port; the script reads the loopback itself and prints it in the REPL.
- `oled_layout_editor.py`: docstring/on-screen help say + rail pads nudge up
  and GND pads nudge down; the code has `TOP_NUDGE_PADS = (TOP_RAIL_PAD,
  BOTTOM_RAIL_PAD)` and `BOT_NUDGE_PADS = (TOP_GND_PAD, BOTTOM_GND_PAD)`, so
  either + pad is up and either GND pad is down. (Docs follow the code.)
- `oled_stats_page.py`: the `{status}` Text element is commented out, so the
  `set_var("status", ...)` loop shows nothing.
- `pin_irq_freq_counter.py`: `freq += (last_pos - pos) * STEP` inverts the
  wheel (turning "up" lowers the frequency).
- `async_read.py`: banner says Ctrl+C; on the Jumperless terminal it's Ctrl+Q.
  Its EOF branch prints "Exiting" but only breaks the inner loop.
- `dac_basics.py`: leaves TOP_RAIL and BOTTOM_RAIL at 0 V when done (docs
  say so).
- `gpio_basics.py`, `node_connections.py`, `test_neopixel.py`,
  `excel_listener.py` call `nodes_clear()` on start (docs say so).

Autocomplete stub `pythonStuff/jumperless.pyi` vs `modjumperless.c`:
- Four stub names are not registered: `disconnect_all`, `net_color`,
  `quick_connect`, `voltage_divider`.
- 88 registered names are absent from the stub (most are object methods:
  jfs file methods, usb_audio status fields, Node methods; the top-level ones
  include `undo`/`redo`, `history_*`, `part_identify`, `place_part`,
  `list_parts`, `remove_part`, `load_project`, `bg_start/stop/active`,
  `usb_audio_*`, `get_node_voltage`, `get_net_current`, `get_path_current`,
  `probe_autoconnect`, `gpio_claim_pin/release_pin`, `get_all_nets`,
  `net_name/net_info`, `oled_screen_reset`, `probe_tap`, `fs_*`).
- OLED signatures in the stub are stale (`oled_get_framebuffer`,
  `oled_set_framebuffer`, `oled_display_bitmap`) per `test_oled_features.py`.
- Full diff: session scratchpad `api_name_diff.txt`.

Other repos (report only, untouched):
- `Jumperless-App/README_PYPI.md` and `JumperlessV5/README.md` still say
  "and a logic analyzer" in the What-is-it paragraph; the LA/JulseView code
  was removed 2026-08-15 (54c931d). The docs index no longer says it.
- `JumperlessV5/README.md` links `.../01-basic-controls/`; the docs keep a
  redirect stub at that URL so it still lands.
- Docs still hot-link several screenshots from github user-attachments
  (03-app, 06-config); fine while GitHub keeps them, but they're not in the
  repo.

## Open docs items for Kevin (voice check)

Per his standing rule, every page below was touched and needs his read for
voice: index, 01-getting-started (new), 03-app, 04-oled, 05-arduino,
05.5-parts, 05.7-gpio (rewritten), 06-config, 07-debugging,
07.5-automation, 08-file-manager, 08-micropython, 08.5-examples (new),
09.5-micropythonAPIreference, 09.6-jfs, 09.8-odds-and-ends, 11-WritingApps,
99-glossary, jumperless-agent-skill.

### 10.5-og-jumperless (2026-09-09, untracked in the docs repo)

Checked line by line against the dev tree (`OG_BACKPORT.md` sessions of
09-07/09-08, `board_og.cpp`, `Probing.cpp`, `LEDs.cpp`, the rev 3.1 PCB
netlist) and the GitHub releases API. Photos in: `JumperlessCropLights.jpg`
as the hero, `BackBoard.jpg` under the flashing steps (both untracked, 1.3 MB
and 2.1 MB - in range of the existing assets). Corrected:

- `USB BOOT` is on the **back**, next to the USB port (the photo shows it).
- The rail switch is **three** positions: 3.3V / 5V / ±8V (`SW2` pads on the
  PCB: +3V3, +5V, +8V and -8V onto TOP_RAIL / BOTTOM_RAIL). The page said two.
  The probe warning now names the ±8V position as the dangerous one.
- MicroPython row: the V5 has a **64 KB** SRAM heap plus PSRAM, not 96 KB
  (`MICROPY_HEAP_SIZE` 64*1024). The OG's 24 KB is what the ladder lands on.
- `ina_get_current(0)` is the crossbar `I_P` / `I_N` sensor, `(1)` the DAC
  output; the page's `I+` / `I-` were not names the terminal accepts.
- A held row's LED can lose its highlight to the next net repaint if the row
  is already in a net (ledger 09-08 night, "NOT fixed"); the page now says so
  and points at the logo's bright pink instead of promising the LED stays on.
- A note that the probe section needs a `dev` build: the released
  `1.7.11.0` registers no probe stack for the OG at all (`main.cpp` on main
  gates on `hasProbePads`; `PROBE_PIN` there is still 10). Drop the note at
  the next release.

Held as written (verified true on dev): 750 ms long press, 80 s timeout,
45 ms settle window, DAC 0-4.096 V / DAC1 -6.9..+7 V, ADC ranges, `GPIO 0` +
UART, the 26 s idle lap, pink/orange/blue logo code, the chooser prompts
(`[5] 12   short press = next, long press = select` and
`3.3V   short press = 5V, long press = select`), remembered rail answer,
the "row tied to a rail through a part reads as the rail" caveat.

Not bench-verified by me (needs the needle): everything in "The probe"
onward is from code and the 09-08 ledgers, not from a live session.

## Night run (2026-09-07 23:00 onward): hardware verification + voice guide

Kevin asked for a 9-hour pass: naive agents follow the docs on the plugged-in V5
(5.7.11.0, confirmed by the missing dev-only `s` command), with OLED and
breadboard screenshots, plus a guide to writing in his voice built from his
public writing. Bench snapshot: `test/hil/kevin_state_2026-09-07_2303.yaml`
(host) and `.hilbak` copies of slot0/slot3/last_active/config/undo_history/
selftest/555_run on the board. Work runs in slot 3.

Firmware-side facts that limited the hands-free surface (follow-ups):
- The undo file is `/undo_history.txt` (`UNDO_FILE_PATH` in Undo.cpp;
  `/undo.hist` is an old path the boot deletes). The docs sweep had it wrong
  the other way; reverted.
- `clickwheel_up/down/press()` from MicroPython never reach a menu or the
  highlighter: `onPythonSessionEnd()` (Python_Proper.cpp) zeroes
  encoderDirectionState / encoderButtonState / lastButtonEncoderState /
  encoderOverride after EVERY raw-REPL exec and after every `bg_start`
  callback tick, and the menu loop only samples those between execs. Driving
  ENC_PUSH (GPIO 11) as an output from `machine.Pin` didn't register either.
  So on the release build there is NO hands-free way to open or navigate the
  click menu, turn the wheel, hold it, or press a probe button. Suggest a
  backchannel/HIL verb that injects an encoder click/turn/hold and a probe
  button press *from the service loop* (not from a Python exec), so docs
  verification and test_encoder_ui can run without a Debug Probe.
- The Debug Probe on the bus does not reach the V5 right now (rp2350.cfg:
  DPIDR reads, CPUID 0x0; rp2040.cfg: SWD WAIT/DAPABORT) - SWD injection is
  off the table until the cable is on the V5's SWD header.
- `:leds` on port 7 dumps only LED_COUNT=300 (the rows); rails, logo, pad
  and header LEDs (300-444) are not in any read-only dump. The terminal's
  `R!` dump is the only place they appear (256-color ANSI). Suggest `:leds`
  emit LED_COUNT+LED_COUNT_TOP.
- `set_switch_position(0|1)` works for the switch; `probe_tap(node)` works
  for taps (idle highlight); there is no probe-button simulation.

### Wedge found 09:20 (needs a firmware look)
- Running `/projects/555/main.py` (via `z 555` or the path) on 5.7.11.0 and then
  typing anything on port 1 before Ctrl+Q leaves the board unstoppable from
  the host: `check_stream_for_interrupt()` only peeks the HEAD byte of Serial
  and consumes nothing but control bytes, and no one else reads port 1 while a
  script runs, so an Enter (CR) in front of Ctrl+Q hides it forever. Port 5
  has the CR/LF skip (5.7.6 fix) but any non-CR byte parked there (the raw
  REPL opener `\x01`, a queued command) poisons it the same way. Port 7 is
  main-loop-serviced, so it goes silent too, the 1200-baud touch never fires,
  and picotool sees no reset interface. Only a wheel hold or a power cycle
  recovers it. Suggest: skip leading CR/LF on Serial too, and drain (or at
  least skip past) any parked bytes ahead of an interrupt char while
  `jl_vm_exec_depth > 0`; or give the backchannel a `:stop` verb serviced
  from the IRQ pump.
- Two agents lost their run to this; the bench harness now sends interrupt
  bytes bare (`bench.py stop`) and refuses a bare `z <project>`.

### Why projects reopen with no parts (root cause, 11:30)
Not a serializer bug. `x` (and Parts > Clear) runs partsClearAllRecords():
parts table, guideSource, guideStep all go (Kevin's rule: "x clear all
connections should remove all parts too"). If the active context is a
project run file at that moment, the idle autosave writes the emptied table
into `<proj>_run.yaml`, and the next `z <proj>` reopens it: `PARTS n=0`,
"guide step 2: unknown part 'U1' (skipped)", VIEWER steps=3 or 4 (only the
note/power steps parse), no edge labels, `z check <part>` = "unknown part".
Kevin's own 555_run.yaml (103 bytes) is in that state, which is why the
docs' example project fails on his board. Reproduced twice by the agents
(nand00, eeprom) after the harness reset's `x` ran inside a project
context; the harness now leaves the project (`<3`) before clearing.
Suggestions: when a run file opens with zero parts and its wiring has a
`parts:` section, re-adopt the wiring's parts (or say so on the terminal and
OLED); or make `x` inside a project context ask, or clear connections only.
- (an earlier note here about node_connections.py going quiet was wrong; see the recheck ruling below)

### Example scripts on hardware (5.7.11.0, no-PSRAM V5, 9 naive agents, 09-08)
Verified good: dac_basics, adc_basics (2 Hz, Ctrl+Q stops it when nothing
is parked in the input), gpio_basics (GP_1->3, GP_2->13 light green),
machine_pin_basics, file_io_basics, uart_basics (banner only, as documented),
pin_irq_basics, pin_irq_reaction_game (ends after 5 misses with no
best/average line), oled_stats_page (layout overflows on a negative reading:
"-0.00" wraps its V under the uptime row), usb_audio_mic (macOS lists
"JL Audio In", 2 ch, 16 kHz current rate, stays listed after the script
stops), async_read (prints "ack" before each echo; banner says Ctrl+C),
viperide_reinit, file manager keys, `z steps` family, the 555 `+` line.
Broken or off-spec as shipped:
- node_connections.py WORKS (recheck): its four pairs (1,30), (15,45),
  (DAC0,20), (GPIO_1,25) connect, is_connected and disconnect within a
  fraction of a second each, so the naive agents' 5 s LED reads never saw a
  row lit and read "goes quiet" as a hang. The docs Try-it uses gpio_basics
  instead because its rows stay lit.
- oscilloscope.py (25 KB) and excel_listener.py (15 KB): "Failed to read
  ... (out of memory?)" with ~41-45 KB heap free. They cannot load on a
  stock (no PSRAM) board at all.
- led_brightness_control.py: NameError 'voltage' on the first loop
  (docstring order bug, listed above), reproduced.
- uart_loopback.py: reads back None (occasionally b'\xff') instead of the
  message, on a clean board.
- voltage_monitor.py: disconnects whatever was on row 12 when it starts
  (docstring says row 20; code row 12).
- test_oled_features.py: terminal output stops at "=== Testing Small Text
  Scrolling ===" ~24 s in; "ALL TESTS PASSED!" never prints.
- test_neopixel.py: ImportError no module named neopixel, before its
  nodes_clear(); no hint about the package manager.
- oled_layout_editor.py: wheel = size, CONNECT = font, REMOVE = save (docs
  said wheel = font and size); stopping leaves a raw KeyboardInterrupt
  traceback from oledgui.free().
- Ctrl+P in the eKilo editor (from `/`): pastes the file into the REPL and
  stops at a `...` continuation prompt (needs an extra Enter); twice it
  re-enumerated USB ("[Errno 6] Device not configured" on the host).
- Every Ctrl+Q stop prints a raw KeyboardInterrupt traceback.
- REPL banner still says "Go to: https://viper-ide.org/"; opening a file
  in eKilo prints "=== ekilo_run() ENTRY ===" / "[ekilo_run] Free heap"
  debug lines to the user's terminal; `> adc_get(0)` at the menu prints
  nothing (needs print()); the File Manager's `h` help lists `..` for
  up-dir while the docs (and the keys) use `.`; `delete` and `exit` are
  not REPL commands (docs fixed).

### Third wedge (09-08 23:35, by a recheck agent) and a host-side hang
adc_basics.py left running again; this time the host's own reads of
/dev/cu.usbmodemJLV5port1 blocked in the kernel and the Python processes
went uninterruptible (U state, unkillable), so even a fresh `open()` of port
1 hung. Recovery needs the board to come back (wheel hold) and, if the
stuck processes do not clear, a USB unplug/replug so the device node is
re-created. Same firmware root cause as the first two wedges.
- Rechecks also ruled: the 555's main.py DOES stop on a bare Ctrl+Q sent
  first (twice reproduced) - the earlier failures were the harness's Enter
  in front of it; uart_loopback's "missed" Ctrl+Q was the rig reopening the
  port between run and keystroke.

Status at 03:25 on 09-09: still wedged (ports enumerate, nothing answers on
1, 5 or 7; no host process holds them any more). The docs commit is in, the
bench restore is NOT. When the board answers again (wheel hold ~3 s, or
replug), run, with ports free:

    python3 test/hil/docs_bench/restore.py

It calms the board, restores every snapshotted file byte for byte while
slot 3 is active, deletes the run's slot files, goes back to slot 0,
restores slot3.yaml, reboots, and then verifies: fingerprint diff of a fresh
Y capture vs the 23:03 snapshot, `:json:power` (top 4.4 V, bottom 7.4 V),
post-reboot re-hash of every restored file, absence of the files that were
absent, directory listings of /, /slots, /python_scripts, /python_scripts/lib,
/projects/* against the snapshot-time listings (removes anything the agents
created: `bench_repl_test.py`, a REPL history.txt, i2cscrn/nand00/eeprom run
files, /screens, /slots/slotPython.yaml, any .hilbak), and that the V5 no
longer enumerates `JL Audio In` (the mic example turns the device on live
and never calls usb_audio_save(), so a reboot should drop it; the check
confirms that, and the config.txt restore covers the case where it did not).
Kevin's 555_run.yaml goes back as found: 103 bytes, partless - it was already
in that state before the run (that is the `x`-clears-parts finding above).
DONE 16:03 on 09-09, after Kevin's wheel hold: live state VERIFIED against the
snapshot, slot 0 active, top 4.4 V / bottom 7.4 V, DACs 2.5 / 0.6, all seven
restored files re-hash to the snapshot values after the reboot, the absent
files are absent, no `JL Audio In` interface, and the run's leftovers removed
(slotPython.yaml, bench_repl_test.py, bench_repl_test2.py, _temp_repl_edit.py,
history.txt under /python_scripts). The bench is as found on 09-07 23:03.
Later the same afternoon Kevin's docs rulings landed: the renders' bottom
strip order (rail above GND), current sense through the click wheel (the
building pads don't always register a tap - probeRowMap puts them in the two
lowest decode buckets, under probe.min_valid_reading), and examples run from
JumperIDE rather than by path.

Afternoon of 09-09, the JumperIDE screenshots and what they cost:
- JumperIDE (ide.jumperless.org) was driven with Playwright + the Homebrew
  Chromium (scratch venv, test/hil is untouched). Web Serial needs a human
  click in the port chooser; after that a page-side override of
  navigator.serial.requestPort() picks the granted port that answers a CR
  with a REPL prompt. The grant does NOT survive a board reboot on this
  V5 (Chromium drops it when the device disconnects), so: grant, shoot
  everything, and only then reboot. Screenshots: scratchpad ide/shots/;
  the docs embed gpio_basics running (docs/assets/verify/jumperide-run.png).
  Verified on the board: connect on the 3rd port, File Manager tree,
  Run (F5) prints in the REPL Terminal, Run again stops (KeyboardInterrupt
  at line 22 of adc_basics.py, prompt back).
- gpio_basics.py calls oled_copy_print(True): every print() also goes to
  the OLED, which makes a chatty script (and the REPL port) crawl - Kevin
  pointed this out when port 5 went slow after the run.
- Restore lesson, expensive: jl.run_file_restore() restores from the
  device-side .hilbak the capture made and DELETES it on success. The
  16:03 restore consumed them; when gpio_basics ran unparked (the venv had
  no pyserial, wrap.py pre died silently) and cleared slot 0, the 16:10
  restore had nothing to restore from and every slot file came back as
  the empty autosave. Recovery: the capture's device_files/ copies (one
  trailing newline appended by the capture, strip it; then the FNV-1a
  hashes match), written over port 5 in base64 chunks, in this order:
  park in slot 3, wait 3 s for the deferred save of the old slot, write
  slot0.yaml, `<0`, wait 3 s, write slot3.yaml, small files, reboot,
  verify. restore.py now falls back to the host copies itself.
- undo_history.txt could not be put back: its 454 original bytes existed
  only on the device and the file is a state log the firmware rewrites
  (JLUNDO v3), not append-only. It was deleted so the next boot starts
  with an empty history (Undo.cpp) rather than the run's transactions.
- /projects/bench555 (a recheck agent's byte-copy of 555 for a safe
  `z new`) was still on the board; removed. Final state 16:22: live
  fingerprint VERIFIED, slot 0 active, 4.4 / 7.4 V, slot0/slot3/
  last_active/config hashes match, /python_scripts and /slots clean,
  /projects back to the four shipped directories.
- 16:40-16:47: a JumperIDE feature tour on the live slot-0 circuit (nothing
  run or saved): landing, dirty file, API reference with Go To Clicked
  Function, JumperNet Registry (list, a script opened, the OLED image
  browser), the pinned Serial Terminal tab on port 1 showing the menu, the
  OLED bitmap editor pushing a smiley live (captured on the real OLED over
  port 7), settings and packages. Shots in scratchpad ide/shots/feat_*.png,
  nine of them embedded in docs/08-micropython.md. The deployed
  ide.jumperless.org has no schematic-export tool yet (only the local repo).
- The tour's port probe opened every granted CDC port for a moment, and
  after it two bridges appeared in slot 0: RP_UART_Tx-NANO_D0 and
  RP_UART_Rx-NANO_D1 (the same pair showed up in the undo log after the
  morning's IDE run). Opening port 3, the CDC1<->UART0 AsyncPassthrough
  port, is the likely trigger (not traced to a line). The probe now never
  opens index 1 and remembers which index answered. Slot 0 was rewritten
  from the host bytes and re-verified (16:47), then the board rebooted to
  clear the smiley off the OLED; fingerprint VERIFIED after.

Evening of 09-09 (Kevin: docs panel closed in every shot, explain the
JumperNet registry, screenshot and write up the VS Code extension):
- Web IDE: every shot retaken with the API panel closed; two new ones for
  the registry (upload dialog, history). Three lessons that cost retakes:
  (1) the board's four CDC ports come back in a different order after every
  reboot, so the page-side port picker probes by ANSWER (Ctrl-B, CR ->
  ">>>" for the REPL; "m" -> "Menu" for port 1) with a 2 s window and a
  500 ms release before the IDE opens the port; (2) the injected picker was
  invalid JavaScript for an hour because a Python non-raw triple-quoted
  string turned a backslash-r into a real carriage return inside a JS
  literal - the drivers now use raw strings; (3) the UART-to-Nano bridges
  (RP_UART_TX-NANO_D0, RP_UART_RX-NANO_D1) appear after IDE sessions even
  when only the REPL port is opened, so it is not the passthrough port; the
  pair is the firmware's serial_1 infra path (InfraPaths.cpp rpSerial1,
  gated on serial_1.lock_connection). Trigger still not traced; every
  session now ends with rewrite_files.py (host bytes) and the check.
- VS Code extension 1.0.2 installed from the Marketplace into an isolated
  profile (~/.cache/jumperide-docs-shots/data, extensions in the scratchpad)
  and driven over CDP: launch Contents/MacOS/Code directly with
  --remote-debugging-port and a SHORT user-data-dir (the scratchpad path
  overflowed the IPC socket name), and unset ELECTRON_RUN_AS_NODE, which
  this shell carries and which makes the binary run as plain Node ("bad
  option" for every flag). Playwright's Python package has no Electron
  launcher; connect_over_cdp works, but once the API reference webview
  opens, the page object latches onto that OOPIF and the workbench is gone
  until VS Code restarts. Webview canvases are unreachable, so the OLED
  editor was drawn by page.mouse strokes inside the canvas rectangle read
  off a screenshot. 15 shots in docs/assets/verify/vscode-*.png. The
  extension's first activation also rewrote ~/.jumperless/typings
  (jumperless.pyi, oledgui.pyi, time.pyi), which is what it does on every
  activation for any install; and during the pass the firmware rewrote
  config.txt's pad_min and current_zero (its own calibration), restored to
  the snapshot values.
- Docs: 08-micropython.md now carries a registry explanation (no account,
  name + your name + description, live at once, wiki-style edits, history
  with Load, delete by renaming to "delete", OLED images, GitHub sync) and
  a VS Code guide (install, connect with the not-pre-selected starred port,
  files/run/stop, serial terminal, OLED bitmaps, API reference, JumperNet,
  settings). The old VS Code assets (JumperIDEconnect.png, TerminalConnect.png,
  REPLandMainSerial.png, EditingOLED.gif, APIreferencePanel.png) are no
  longer referenced but were left in place.
- Fact sheets from the two reader agents are in scratchpad vscode/facts.json;
  the extension one flags real bugs worth a look: the picker computes a
  preselection it never applies, connectOnStartup opens the picker rather
  than connecting, a script named "delete" deletes on its next edit, and
  image deletion skips the author check.

## 2026-09-09 evening: a user's VS Code + Jupyter question, and the native-code crash

Kevin forwarded a user question (Windows, VS Code, Jupyter notebooks with micropython-magic,
JumperIDE extension installed): which port, why the stubs only half work, and why a
`@micropython.native` function reboots the board when called.

**Native / viper crash: root cause found, fix built, NOT flashed.** Reproduced on the bench
(5.7.11.0, no PSRAM): a pure-int `@micropython.native` loop with no board calls drops the V5 off
USB on its first call. The firmware's own record (`m` on port 1 after the reboot prints it):
`[crashlog] HardFault core 0 PC=0x20064294 LR=0x1002DD87 CFSR=0x00020000 HFSR=0x40000000`.
CFSR bit 17 = UFSR.INVSTATE: a branch to an even address in the GC heap, i.e. the emitted
Thumb code called without the Thumb bit. `lib/micropython/port/mpconfigport.h` enables the
emitters (section 6, since cd8a147 2026-06-01) but never defines `MICROPY_MAKE_POINTER_CALLABLE`,
which py/objfun.c wraps every native/viper/asm_thumb call in; upstream rp2 and stm32 define it as
`((void *)((mp_uint_t)(p) | 1))`. Every V5 release since June crashes on the first native call.
Fix: that one macro, added to all THREE config copies (`lib/micropython/port/`,
`lib/micropython/micropython_embed/port/`, and `lib/micropython/micropython_embed/mpconfigport.h`,
which is the one `-Ilib/micropython/micropython_embed/` actually resolves). Build: PlatformIO
did not notice the header change; `rm -rf .pio/build/jumperless_v5/libb36/micropython` then
`pio run` recompiled the 179 MicroPython objects, and `objfun.c.o` now reads
`orr.w r4, r4, #1` before `blx r4` (uf2 md5 0d92ccfe, 22:08). The PICOBOOT flash was refused by
the session's permission classifier, so the fix is compiled but not hardware-tested and is left
UNCOMMITTED in the dev tree (only the three mpconfigport.h files changed; the tracked
`.pio/build/jumperless_v5/firmware.uf2` now holds this build). To finish: flash with the touch
script (`scratchpad/bench/flash_touch.py <uf2>` does nodes_save, Ctrl-C on port1, 1200-baud
touch, filtered picotool, then fingerprints XIP against the uf2), then in ONE raw-REPL exec define
and call a native int loop, a native loop calling adc_get(0), and a viper function; `m` on port 1
must print no `[crashlog]`; then reflash JumperlOS-main HEAD's uf2 (md5 b33f9026) and verify the
bench. The OG shares this config: RP2040 has no Thumb-2, and `MICROPY_EMIT_THUMB_ARMV7M` defaults
to 1 (upstream rp2 sets it and `MICROPY_EMIT_INLINE_THUMB_FLOAT` to 0 for RP2040), so native code
on the OG will still fault after this fix, differently. Not touched: untestable here.

The debug probe on this Mac (E6633861A378632C) cannot see the V5 right now ("Error connecting
DP: cannot read IDR" on every attempt), so vector-catch capture was not possible; the crashlog
record was enough. The on-disk `JumperlOS-main/.pio/build/jumperless_v5/firmware.elf` (Sep 2) is
NOT the 5.7.11.0 build (first byte mismatch at +0x300C against the committed uf2), so addr2line
against it lies.

**Port:** micropython-magic works on the MicroPython REPL port (port5 here, the 3rd CDC); mpremote
`exec` works there with and without `resume`. The magic's `%mpy --select X --verify` failed both
times ("could not enter raw repl") while the identical mpremote command works by hand; plain
`--select` works; not chased.

**Stubs:** PyPI `jumperless` is the Wokwi bridge (module `jumperless_pkg`), unrelated. Recipe
verified with pyright 1.1.400 and in a scratch VS Code (Pylance 2026.3.1) on a live notebook:
`typings/jumperless.pyi` (JumperlOS-main `scripts/jumperless.pyi`, sha256 43ed18…, byte-identical
to the board's `/python_scripts/lib/jumperless.pyi`; the extension's bundled copy is 8 functions
behind) + `typings/__builtins__.pyi` containing `from jumperless import *` + `pyrightconfig.json`
`{"stubPath": "typings", "reportMissingModuleSource": false}`. Pylance ignores cells that start
with an unknown `%%` magic (pylance-release #3846/#4629); micropython-magic's input transformer
turns `# %%micropython` back into the magic, verified live, so that spelling gets Pylance in board
cells. Extension global setup: with Pylance installed at activation it does write
`python.analysis.stubPath`/`extraPaths`/severity overrides (verified on reload); the earlier
scratch profile without Pylance never got a settings.json. Side effect on this Mac: the scratch
VS Code's extension rewrote `~/.jumperless/typings/` (builtins.pyi now vendored from Pylance
2026.3.1, 103 KB) at 22:17.
