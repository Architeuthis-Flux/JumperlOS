# `dev` merge — what's done, what's open, what to do next

Sessions of 2026-08-15 through 08-18. Branch **`dev`** is a fast-forward of
`main` → `infra-paths` → `usb-audio-uac2` plus the commits below. **Kevin pushes
`dev` as work lands**: at row 59 `origin/dev` = `e795630` (row 58), the `5.7.3.0`
tag is on `origin`, and only the T2.2c commit `1c7249f` (row 59) + the `5.7.3.1`
tag are local; `origin/main` is still `e45af3b`. `firmware.uf2` in
`.pio/build/jumperless_v5/` is the HEAD build.

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
- **Boot names the switch position instead of assuming it** (2026-08-16):
  the Probing constructor hard-coded switchPosition = 1 (select) and the
  classifier had only transition rules, so a wrong guess stood until the
  user flipped the switch. Boot now starts at "unknown" and an absolute
  branch classifies from the signature itself (~0 mA = measure, ~1-1.5 mA =
  select), with the dark-LED guard on the low side. Verified live: first
  check after a reboot with the switch at select -> SELECT (CHANGED).
- **Switch sensing survives a dark probe LED** (2026-08-16): a DAC claim
  relocating the buffer feed could reset the probe LED chip, whose idle draw
  IS the position signature - the sensed position flipped and latched until
  reboot. Fixed three ways (release-nudge, confirmed flips, LED keep-alive);
  one hands-on confirm remains (open item 1).
- **The live reading line repaints in place again** (2026-08-16). It was
  scrolling one line + one blank per reading: `clickMenu()` - polled every
  main-loop pass - dropped the pin ~1000x/s. Fixed, and the pin now
  invalidates itself off a port-1 linefeed counter instead of trusting
  callers. `probe_tap()` became a real simulated tap to make this
  reproducible.

**Probe rework (2026-08-16, rows 18-22): see `PROBE_REWORK_HANDOFF.md`** — the probe
feed, switch sensing and their calibration were reworked end to end. The agreement
classifier is built, logging and hardware-checked in both positions but **decides nothing
until a hands-on touch matrix promotes it** (`debug.probe_switch_agree`), and
`probe_current_zero` was found to swing wider (0.5 → 2.3 mA across boots) than the ~1.4 mA
signal the legacy current thresholds ride on.

**Scheduler / hardware offload (2026-08-16/17, rows 24–29 and 32–49, plus the paste fix in row 30 and
the switch-classifier-in-probe-mode note in row 31): see
`SCHEDULER_AND_HARDWARE_OFFLOAD.md`** — the sweep's proposals were reviewed by Kevin in plan
mode; its section 0 records what was approved, what was declined, and the status of each
approved item as it lands (one commit each). One finding was corrected on the way in:
**I2C0 runs at 400 kHz on this board, not 1.7 MHz** — the OLED on I2C0 (rev 7) owns the
clock after `oled.init()`; T1.9 gives the bus one owner at 1 MHz. **Where it is:** T1.1,
T1.2/1.3 and T1.8 landed (rows 25–28); **T1.9 landed (row 29)** — the I2C0 clock turned out to be *dynamic* (every `wavegen_start()`
flipped it to 1.7 MHz, the next OLED frame dropped it to 400 kHz); now it reads 1.00 MHz on
every boot and around every wavegen/OLED event, the INA219s are clean at sustained 1 MHz, and
the wavegen stream costs −33 % (30.4 k → ~20 k writes/s); HIL 5/6, `test_infra_paths` 24/24.
**T1.4 landed (row 32)** — the scheduler has periods, `requestRun()` and a per-service µs
table in `X`; its first reading: **~85 % of core 0's loop is probe-pad polling while nothing
is touched** (ProbePads 36.8 ms per call at 20 Hz), the doc had estimated ~⅓. **T1.5 landed
(row 33)** — the modal set is named (`serviceInner()`), and the Arduino UART bridge now keeps
running inside probe mode and the menus. **T1.7 landed (row 34)** — the `loop()` help spins are
gone: in line mode (the app's mode) `help` works, `x?` no longer clears the board, and every
CR/LF-terminated command answers ~100 ms sooner. **T1.6 measure-only landed (row 35)** — `X`
shows the longest gap between would-be watchdog kicks per core; the numbers (compute-bound
MicroPython 5.2 s, a wavegen stream = the whole stream on core 1, slot save 1.8 s) say the
enable needs VM-hook and WaveGen kicks. **T1.10 landed (row 36)** — the terminal LED picture is
drawn from core 0 now (an inner-set `LedDumpService`), core 1 no longer writes USB CDC.
**Tier 1 is complete.** T2.2a (row 37) added the tap→crossbar→LEDs latency probe to `X` with
the before numbers (the flag handshake is ~0.1–0.3 ms; the LED show is the ~7 ms); **T2.2b
(row 38) replaced the `sendAllPathsCore2` flag with a generation-counted mailbox** — no request
can be lost any more, and the probe reads the same; **T2.3 (row 39) made the crossbar list
send DMA-fed and ISR-completed** — core 1 is no longer blocked for a rebuild, the crossbar
self-test passes, 0 stalls in a 500-rebuild soak. **Stopped before T2.1 (row 40 explains why:
the ADC ring cannot be staged dark — it is the probe-reader rewrite, and its gate is Kevin's
hands).** The afternoon (rows 41–47) was the probe switch: the in-session classifier decides
on agreement, the idle classifier cannot flip against the tip sense, `probe_current_zero` is
the 2nd-lowest sample (the 2.4 mA boot that oscillated is explained), the probe LED keeps the
session's pattern through a flip; plus T1.7b (no argument wait in line mode) and T1.6b (VM /
WaveGen kick sites: nothing in the suite leaves a gap above 1.8 s). The doc's "▶ CONTINUE
HERE" block starts with a **state-at-a-glance** for the next chat, then the T2.1 analysis, the
queue (T2.1 / T2.2c / the watchdog enable — all three need Kevin), and **the consolidated
hands-on checklist**.

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
| 16 | `f441fdb`+ | Switch sensing: release-side `infraNudge()`, SELECT→MEASURE flip needs two lows + an LED re-send, 5s LED keep-alive | claim/relocate/release replayed on hardware with stats on; release-nudge verified (feed back to DAC0/INA in ~1.5s); SELECT-side discard needs the physical switch (open item 1) |
| 17 | `0437832` | Boot classifies the switch from absolute signatures instead of assuming SELECT; `-1` renders as UNKNOWN in stats | reboot with switch at select: first check UNKNOWN→SELECT (CHANGED), no flip; both boards build; HIL 5/6 |
| 18 | `7ffdb03` | **Checkpoint 5.7.2.0** (`VERSION` + rebuilt uf2, tagged `5.7.2.0`, not pushed) | v5 + og build; `FIRMWARE_VERSION "5.7.2.0"` in the ELF |
| 19 | `6fa2744` | Probe feed: **DAC set-once in the MCP4728 driver** (per-channel shadow, shared across instances), `dacs.probe_power_source` order flag (DAC0-first default / GPIO-first), single path made observable in `i@`, encoder DAC adjuster is a user write | builds ×3; `test_infra_paths` **24/24** (10 new GPIO-first checks); HIL 5/6; full `self_test` PASS; 20 rebuilds → DAC0 writes **unchanged**; wavegen 2 s on DAC0 (62,940 writes) → exactly one park write at the next rebuild |
| 20 | `13e2313` | **Two switch detectors** (tip-side digital sense; per-feed INA ceiling / feed-side blink) + agreement classifier behind `debug.probe_switch_agree` (shadow by default); droop V0 off the hot path; `resetConfigToDefaults` copies the calibration + hardware **structs** | builds ×3; both detectors correct in both positions under both feeds, every observed flip agreed with legacy; agree mode classified from boot and survived the dark-LED trick; `test_config` **30/30** (sentinel survives `` `reset ``) |
| 21 | `b9bfac2` | Probe LED frame/request/button counters in `X`; event-driven show when the LED has its own pin; **INA poll drops the `pauseCore2` toggle** (I2C0 is core-0-only) + ≥10 ms attempt gate + no double poll; `probeRowMap` bounds | builds ×3; HIL 5/6; measured ~2,560 frames/s vs ~7 requests/30 s, ~2,600 button samples/s, 0 pause-aborts idle |
| 22 | `1036b18` | Switch Calib app measures **both feeds and both detectors**, refuses to save on a contradicting tip sense, unforces on every exit; self test's `probe_cable` infers position from the **tip sense** (fixed a live FAIL on a good cable), `tip_voltage` gates `probe_droop_v0` on an unloaded tip | builds ×3; full `self_test` **OVERALL PASS** (`probe_cable PASS sw:meas(tip)`, `tip_voltage PASS droopR:183 sw:meas`) |
| 23 | `a6ad4ba` | **A measure calibration per feed, converged against each other**; SELECT pins DAC0 so its switch current is measurable; the app's position comes from the tip sense (`PROBE_REWORK_HANDOFF.md` row 5) | **Kevin on hardware**: SELECT engages on a flip, convergence lands after a SELECT round-trip, taps decode under both feeds (feeds settled 74 counts apart); builds ×3; `run_all` 5/6; `test_infra_paths` 24/24; `test_config` 30/30 |
| 24 | (docs) | **`SCHEDULER_AND_HARDWARE_OFFLOAD.md` rewritten as the reviewed proposals + decisions** — Kevin's approved scope (Tier 1 incl. T1.9, T2.1–T2.3; T2.4 next session; the rest design-only), the commit-gate disposition for the autonomous pass, and the corrected I2C0 finding (400 kHz measured, three clock owners) | register readout over the REPL (`IC_FS_SCL_HCNT/LCNT` 150/225 @150 MHz); baseline `X` + `run_all` 5/6 captured into the doc's section F |
| 25 | `a3e58f4` | **No raw `tud_task()` anywhere in `src/`** (59 sites → `TinyUSB_Device_Task()` / `yield()`, the Adafruit port's mutex-guarded entry points; the port also pumps from its USB soft-IRQ, so a raw call could re-enter the stack); `TinyUSBService` is CRITICAL (every pass, and in the modal loops' set) instead of NORMAL (every 3rd pass) | builds ×3; HIL 5/6 ×5 incl. a 4-run soak with port 7 held open by a second process (0 errors, no port drops, uptime continuous); `X` census unchanged |
| 26 | `3fc5c57` | Priority/comment truth (`main.cpp` registration comments, `JumperlOS.cpp` stale ID map, Highlighting 40 ms, MpRemote 8192, ProbeSwitch NORMAL); the five verified no-op services are no longer registered (TermSerial, RelayedCmd, SingleCharCommands, USBPeriodic, FileCacheFlush behind `#if USE_FILE_CACHE`); `core1request` (written, never read) deleted; `inClickMenu` is `volatile` like every other cross-core mode flag | builds ×3; HIL 5/6; `X` census unchanged |
| 27 | `b0fd157` | **Vocabulary rename, nothing behavioural**: the "injected command / injection buffer" family is now "relayed command / relay buffer" (`RelayedCommandService`, `RelayBufferStream`, `Jerial.relayInput()`, `hasRelayedCommand`, `relay_buffer`, `DEBUG_RELAYED_COMMANDS`, `withANSI`), and comment words that read wrong out of context were reworded (steal→take over, poison→corrupt, kill→stop/break, sniff→watch, forged→simulated, attack→rise, hostile→untrusted). Datasheet excerpts left as quoted. No Python-facing API or command changed | builds ×3 |
| 28 | `f3e4f6f` | **CH446Q per-crosspoint path and its ISR run from RAM**: `sendPaths`/`sendAllPaths` were `__not_in_flash_func` but `sendPath` / `sendXYrawUnchecked` / `sendXYraw` / `isrFromPio` / `setCSex` ran from flash (`X` showed the irq 16 handler at `0x10055931`) | builds ×3; HIL 5/6; `test_infra_paths` 24/24 (after resetting a stray DAC0=2.0 V that made the feed non-viable — board state, reproduced on the pre-T1.1 code); `X`: irq 16 handler `0x20000835`; `nm` addresses in RAM in both ELFs |
| 29 | `9bca7b5` | **I2C0 has one clock owner, `initDAC()` at 1 MHz (`I2C0_BUS_CLOCK_HZ`)**: `MCP4728::begin()` no longer forces 1.7 MHz (it ran on every `wavegen_start()`, so the shared bus flipped to 1.7 MHz mid-session until the next OLED frame dropped it to 400 kHz — the INA219s and WaveGen ran at whichever was last); the OLED-on-I2C0 driver keeps its 400 kHz per transfer but restores 1 MHz after (`clkAfter`), and `oled::connect()` passes the bus rate to `initI2C()` for `connection_type 2` (T1.9) | builds ×3; I2C0 register readout 60/90 = 1.00 MHz on 11/11 boots, before/during/after wavegen and after forced OLED frames; DAC1 stream 30.4 k → ~20 k writes/s (the −33 % is the honest cost); 80 000 INA0/INA1 reads at sustained 1 MHz, 0 failures; 10 reboots: INA1 median identical, OLED Connected; `probe_current_zero` on 10 further boots 1.01–1.51 mA (mean 1.28; history 0.5–2.3). HIL 5/6; `test_infra_paths` 24/24; `i@` `probe_power on -> DAC0`. (A first HIL pass showed `net_currents`+`stress` failing only their `i?`-audit checks — the board was in char mode, `terminal_line_buffering = 0`, where `i?` returns the help page; `B1` fixes it — see `SCHEDULER_AND_HARDWARE_OFFLOAD.md` §0 working rules) |

| 30 | `f5a6cd0` | **Pasting a state back works again (`S` YAML, `L` JSON)**: both readers stopped at the *first* empty line, and `Y`'s output has blank lines between sections, so a pasted Y round-trip ended at `sourceOfTruth:` and every later line ran as a menu command (in both terminal modes). It only ever "worked" when the terminal sent CR-only line ends and `readStringUntil('\n')` swallowed the whole paste as one 1 s-timeout blob; the jumperless app now sends `\n` per Enter. New shared `readPastedBlock()`: `\n`/`\r`/`\r\n` line ends, inner blank lines kept, ends on an empty line + 500 ms of quiet (30 s idle fallback as before). + `test/hil/test_paste_state.py` (Y→S, CR paste, J→L; in `run_all.py`) | builds ×3; Y→S and J→L round-trips in line and char mode, LF/CR/CRLF, one burst and byte-by-byte, and through the real `jumperless` app (interactive mode): bridges back, no stray commands; HIL **6/7** (the suite gained a file — the one FAIL is still the phantom-current check) |
| 31 | `2761825` | **The switch classifier runs inside probe mode too** (Kevin, 2026-08-17: "if we hit the switch during probing, the pads are way off"): the pad frame follows `switchPosition`, but `probeMode()`'s blocking loop only ran the CRITICAL set, so a mid-session flip left every pad reading scaled for the wrong position until the session ended (up to 80 s). `checkSwitchPosition()` is now `infraServiceTick()` + `classifySwitchPosition()`; the loop calls the classifier (500 ms self-gated, button/LED-settle/touch vetoes intact) and NOT the infra tick (a nudge is a crossbar rebuild — not mid-session). `checkSwitchPositionFast()` (A-only, agree mode) stays alongside | builds ×3; `[switch]` stats lines still flow every 500 ms outside probe mode (`A:H B:M -> MEASURE`); HIL **7/7** this run (the phantom-current check happened to pass — board state, nothing here touches it) ; `test_infra_paths` 24/24. **Pending Kevin hands-on:** flip the switch mid-session, pads should re-scale within ~1 s (no debug probe attached, so probe mode could not be entered hands-free) |

| 32 | `e3d4d36` | **T1.4 (B3): the scheduler has time.** `Service` gets `periodUs()` (0 = every pass), `requestRun()` (ISR/other-core safe latch) and per-service stats; `serviceAll()` drops the per-band loop divisors (1/1/3/20 — never a cadence) for a due-or-pending gate on `time_us_32()` (C12) through one `runService()` helper that `serviceCritical()` and `forceServiceBy*` share, so the modal loops' calls are counted too. Periods only encode gates the services already apply (ProbeSwitch 10 ms, OLED 20 ms, LiveXbar 100 ms + `requestRun` from `requestUpdate()`, Peripherals 10 ms, ProbePads 50 ms, OledGui 15 ms, States 50 ms, ConfigSave 100 ms + `requestRun` from `requestConfigSave()`; all else 0); nothing internal deleted. C6: the button PIO IRQ `requestRun()`s ProbeButton + Probing on a state change. `X` prints the table: name / prio / period_us / runs / last_us / max_us / avg_us / overruns / share | builds ×3; `X`: all 16 services runs>0 with the expected periods, modal-loop calls visible; wait-loop timing debug 12 s: 0 SLOW SERVICE lines; no `refresh:` line over 20 routing ops; HIL **7/7**; `test_infra_paths` 24/24. **First measured number:** ProbePads (`checkPads` = 12 × `readProbeRaw`) = **36.8 ms per call at 20 Hz = 64–70 % of core 0**, + Probing::service ~16 % → ~85 % of the loop is pad polling while nothing is touched (the doc estimated ~⅓); the loop makes ~1 000 passes/s. **Pending Kevin hands-on:** probe feel + menu feel unchanged |

| 33 | `545b7e6` | **T1.5 (B4): the modal set is named.** `Service::inInnerSet()` (default CRITICAL; `AsyncPassthrough` opts in) + `jOS.serviceInner()` = one due-gated pass over it; `serviceCritical()` deleted, its 23 call sites (Menus ×10, Probing ×8, BitmapEditor, GraphicOverlays, ImagesApp, Peripherals, Python_Proper `mp_hal_delay_ms`) call `serviceInner()`; `serviceAll()` runs the same set while a service is BLOCKING; `servicePython()` = `serviceInner()`. **Intended Kevin-visible delta:** the Arduino UART bridge and the USB pump keep running inside probe mode / click and pad menus / apps / MicroPython delays (the bridge used to stop there). Also: `nextDueUs` is 64-bit (`time_us_64()`) — a 32-bit deadline left a service parked behind a modal loop >35.8 min "not due" for another ~35 min. `X` marks inner-set rows with `*` | builds ×3; `X` after a 3 s REPL `time.sleep()`: TinyUSB / ProbeButton / MpRemote / **AsyncPassthrough (`HIGH*`)** runs = passes + 42, non-inner services = passes exactly; HIL 6/7 (run 1: `test_net_currents` FAIL, check line not captured) then **7/7** (run 2; the file standalone 8/8 ×4 — the known marginal phantom check); `test_infra_paths` 24/24. **Pending Kevin hands-on:** probe mode / click menu / REPL feel; passthrough while probing with an Arduino on the header |

| 34 | `b8d00d9` | **T1.7 (B6): `loop()` cleanup.** The 10 ms `secondSerialHandler()`/`replyWithSerialInfo()`/`serviceNetVoltageScanDebug()` block is `PortHousekeepingService` (NORMAL, 10 ms); the two 100 ms help-wait spins are gone — line mode decides `help` / `help <cat>` / `[cmd]?` from the completed line (**before: in line mode "help" printed the menu, "m?" the menu, and "x?" CLEARED THE BOARD — and every CR-/LF-terminated command paid the 100 ms**), char mode parks the char (`helpArmed`) and keeps servicing until the next char or the deadline; one list `helpQuestionApplies()` names the commands that own their `?` in both modes (`A?`/`a?`/`i?`/`M?` — char mode used to show the help page for `i?`/`M?`); `cmd_usbAudio` reads its sub-command via `getCommandArgs()` (in char mode "M?" used to toggle the device = USB re-enumeration); the post-command raw-Serial drain (ate multi-line pastes) is gone | builds ×3; both modes scripted over port 1: `help`, `help probe`, `x?` (bridge survives), `m?`, `h`, `?`, `i?`, `A?`, `M?` all right; `n` → netlist p50: line mode CR/LF **146 → 46 ms**, CRLF 5 → 5, char mode 4–5 unchanged (old build re-flashed for the before numbers); `X`: PortHousekeeping row at ~24 Hz; HIL **7/7**; `test_infra_paths` 24/24. **Pending Kevin hands-on:** help/`x?` feel in his terminal, latency in the app |

| 35 | `574e749` | **T1.6 (C11) watchdog, measure-only stage.** `src/KickGap.h/.cpp`: stamps where a watchdog kick would go (top of `loop()`'s busy pass, `serviceInner()`, top of `loop1()`), the longest gap between stamps per core with the bracketing sites and time; `X` prints it, `X!` resets the maxima. **No `watchdog_enable()`.** | builds ×3; numbers (after `X!`, one blocker, `X`): idle **42 ms** core 0 (= the ProbePads block) / 4 ms core 1; config write 44 / 4 ms; slot auto-save **1.8 s / 1.1 s**; `wavegen_start(1)` 10 s → core 1 **10.0 s** (captured for the stream); compute-bound MicroPython 5 s → core 0 **5.2 s**; `time.sleep(5)` 0.2 s; `~` 1.06 s; whole `run_all.py` **11.5 s** core 0 / 1.2 s core 1; boot 353 / 60 ms. → the enable needs a kick inside the MicroPython VM hook and inside (or an exclusion for) WaveGen's core-1 stream; everything else fits an 8 s timeout ×4 margin. HIL 7/7; `test_infra_paths` 24/24 |

| 36 | `dff24b3` | **T1.10: LED-dump mode off core 1.** The `dumpLEDs()` block in `loop1()` (a USB CDC writer on the non-USB core — the documented wedge family) is gone; core 1 raises `ledDumpFrameReady` after each shown frame and a new inner-set `LedDumpService` (NORMAL, 10 ms) on core 0 draws the terminal picture: every `dumpLEDrate` (250 ms) when a fresh frame is there, at least every 1 s, skipped while `core2busy`. Inner so the picture keeps updating through probe mode / menus / MicroPython scripts as it did | builds ×3; `R!` on port 1: dumps every ~340 ms idle and through a 2.4 s modal MicroPython script (48 KB); `serial_1.function = leds` + port 3 held open: 60 KB in 4 s on USBSer1, port 5 alive, config restored; `X`: `LedDump NORM* 10000` avg 2.6 ms/dump, max 52 ms; HIL 7/7; `test_infra_paths` 24/24 |

| 37 | `de297c5` | **T2.2a: tap→crossbar→LEDs latency probe** (`src/XbarLatency.h/.cpp`, section F). Stamps: tap (probeMode commit), req (`sendAllPathsCore2` written, 3 sites), pickup (top of `sendPaths`), sendDone (end of `sendPaths`), show (first `leds.show()` after a send); last/max/n per segment in `X`; `X!` resets. Instrumentation only | builds ×3; before numbers over 40 REPL routing ops ×2: req→pickup ~100–200 µs (max 0.3 / 2.2 ms), pickup→send ~40–50 µs, send→show ~7.1 ms (max ~10), req→show ~7.3 ms (max 10–12); tap→req n=0 (needs a real tap); HIL 7/7; `test_infra_paths` 24/24 |

| 38 | `3f02a14` | **T2.2b: the core-1 request mailbox, `REQ_SEND_PATHS`** (`src/CoreMailbox.h/.cpp`, `core1req::`): two slots (`REQ_SEND` with sticky `SEND_CLEAN`; `REQ_BYPASS` = the old "3"), pending bits + request/done generations under a fixed SIO spinlock (OS2 — core 1 launches before `setup()`, so nothing can be claimed safely); `sendAllPathsCore2` deleted everywhere; `refreshConnections` waits on its generation, `sendPaths()` no longer zeroes a flag at its end (that erased a request landing mid-send), `waitCore2()` = `core2busy || !allIdle()` with the same 25 ms bound; call sites untouched; `X` shows the mailbox | builds ×3; latency probe same-or-better (req→pickup ~0.1–0.2 ms, send→show ~6–8 ms); 200-op REPL routing soak: 0 WARNINGs, mailbox idle after; wavegen-pending check: the send stays posted while streaming (`bypass bits 0x1 … busy`) and lands on `wavegen_stop()` (`req→pickup 7.68 s`); HIL 7/7; `test_infra_paths` 24/24 |

| 39 | `9db4675` | **T2.3: the DMA-fed CH446Q list send.** On core 1 (V5) `sendAllPaths()` collects the crosspoint words + chips into a snapshot (`sendXYrawUnchecked()` appends while collecting — bookkeeping and chip-K safety unchanged), one DMA channel (DREQ = the SM's TX FIFO) feeds the words, the PIO ISR strobes `dmaCs[dmaIdx++]` and completes the mailbox request on the last one; the existing PIO handshake throttles the DMA (no PIO edit, no DMA IRQ); `sendPaths(clean, reqSlot, reqGen)` returns once kicked; CPU single-crosspoint sends and DMA sends arbitrate under a fixed spinlock; a 200 ms no-progress stall (park-tolerant accounting) aborts, marks suspect, restarts the SM, completes anyway; core-0 callers and the OG keep the CPU path; `X` shows the DMA counters | builds ×3; latency probe same (pickup→send ~50 µs); 500-rebuild soak: 738 DMA sends / 15 408 words, 0 stalls, 0 pio timeouts, 0 WARNINGs, `i?` self-check PASS + `suspect=0x000`; crossbar self-test app (`run_app("Xbar   Test")`) **PASS rows 60/60 gpio 8/8 rails 2/2**; `test_net_currents` 8/8; HIL 7/7; `test_infra_paths` 24/24 |

| 40 | `38a4806` | **Docs: where the scheduler pass stopped and why T2.1 was not started autonomously** — the always-on ADC ring has no "dark" stage (free-running vs START_ONCE cannot share the ADC; an always-on engine puts every reader on the mic-open snapshot path, where channels 5/7 are a sentinel for the row decoder), so it is the probe-reader rewrite in one step, with a gate that is Kevin's hands (taps under both feeds/positions, the mic). Recommended as a session with Kevin at the board; the 20-line touch pre-check in `checkPads()` is the cheap alternative for the CPU load alone | docs only |

| 41 | `b03d25b` | **In-session switch classifier decides on agreement only** (Kevin: "assuming measure mode when it's not"). Row 31's in-session classifier used the legacy B-only rule, calibrated for the idle LED signature; the session's connect/remove/net patterns draw in or below the dead band → B said MEASURE in SELECT, the classifier flipped, lit the green measure pattern (0.2–0.9 mA in SELECT) and the legacy MEASURE→SELECT rule (> 1.2 mA) never fired again — a latched false MEASURE, seen on the board right after the report (`A:L B:M shadow:S -> MEASURE`). `classifySwitchPosition(inSession)`: in a session the position changes only when A == B (no B-only verdict, no adopt-A tiebreak — a needle on a hot row fools A, no dark-LED heal); idle unchanged | builds ×3; idle classifier `A:H B:M -> MEASURE` then `A:L B:S -> SELECT (CHANGED)` as Kevin flipped; HIL 7/7 (second run; the first had two transient raw-REPL failures while the board was being handled, both files pass standalone); `test_infra_paths` 24/24. **Kevin:** flip mid-session again |

| 42 | `bd1e7bc` | **Legacy switch classifier: tip-sense veto** (Kevin: "erroneous flipping sometimes with the switch in select"). His board at idle: 15 flips in 40 s, `A:L` on every line, corrected current 0.1 ↔ 1.6 mA — `probe_current_zero` on that boot was ≈ 2.4 mA (raw INA1 a flat 2.5; three reboots later 0.4–1.05) = open item 2 of the probe handoff blinding B. The legacy rule still decides on its own but a flip A contradicts is held (SELECT→MEASURE vetoed while A:L, MEASURE→SELECT while A:H, boot verdicts likewise; A abstains while a button is held) | builds ×3; reproduced with `` `[calibration] probe_current_zero = 2.4 ``: 22 vetoes, **0 changes**, SELECT held over 12 s (was oscillating); zero restored to 1.05; HIL 6/7 (known phantom check); `test_infra_paths` 24/24. **Kevin:** flip at idle + mid-session again |

| 43 | `4dd3eee` | **T1.7b: no argument wait for complete-line commands.** `getCommandArgs()` returned after its 20–100 ms stream timeout for every bare command in line mode (the app's mode), the relayed path and the port-7 backchannel (which waited on port 1's stream); `g_commandInputIsLine` (set by `loop()` / CommandBuffer / Ser3Backchannel, cleared only on the char-mode read path) skips the wait. All 23 callers audited: same-line suffixes only, no prompt-then-read | builds ×3; help/mode matrix unchanged in both modes; `c` line mode 4 ms p50 (was ≥ 100 ms by construction); char mode 2 ms; port-7 `X` fine; HIL 6/7 (known phantom check); `test_infra_paths` 24/24 |
| 44 | `ba64238` | Docs: rows 42–43 hashes, T1.7b block, why T2.2c also waits for Kevin's eyes (~200 `showLEDsCore2` sites, all LED behaviour) | docs only |

| 45 | `c785d0d` | **probe_current_zero: 2nd-lowest sample instead of the median** (+ `X` prints the calibration's diagnostics: zero, sample spread, LED-off ack, xbar idle, run count). 17 reboots showed the LED-off request acked in 0–1 ms and the DAC0 disconnect landed before sampling every time — but the 8 samples spread 0.79..2.38 mA on most boots (floor 0.79 = INA1 with DAC0 open; 1–7 of 8 samples land at 1.3–2.4 — upward blips that occur even with nothing connected). A median catches them; the boot that gave Kevin's 2.4 mA zero had ≥ 4. Also: `` `[dacs] auto_connect_probe = 1 `` after a 0 now re-enables the feed (the applier only handled the off side) | builds ×3; 9 reboots with the 2nd-lowest pick: 0.79–1.04 mA (median gave 0.9–1.3 the same hour, 2.4 on the bad boot); `[switch]` idle SELECT 1.8 mA corrected; HIL 6/7 (known phantom check); `test_infra_paths` 24/24 |

| 46 | `e1fc7f8` | **T1.6b: the VM-hook (`mp_hal_check_interrupt`, 1 ms throttle) and WaveGen stream-loop kick sites, measure-only** (`KICK_VM`, `KICK_WAVEGEN`; still no `watchdog_enable()`) | builds ×3; compute-bound MicroPython 5 s: core 0 max gap **5.2 s → 173 ms**; wavegen 10 s: core 1 **10.0 s → 3.9 ms**; whole `run_all.py`: core 0 1.82 s (slot save), core 1 1.16 s (down from 11.5 / 9.4 s) — an 8 s watchdog would have ×4 margin over everything measured; HIL 6/7 (known phantom check); `test_infra_paths` 24/24 |

| 47 | `e148384` | **In-session probe LED stays the session's** (Kevin: "if I flip the switch inside the probing loop it lights up measure; it should continue to say connect/remove"): the classifier's `showProbeLEDs = 3/4` on a detected flip is idle-only now; the agree-mode A-only tracker no longer writes the LED either; the flip still re-scales the pads; `probeMode()`'s exit lights the right idle pattern | builds ×3; idle unchanged; `test_infra_paths` 24/24. **Kevin:** flip mid-session |

| 48 | `eafb218` | Docs: the "START HERE" state-at-a-glance at the top of the doc's CONTINUE HERE block (what landed, what needs Kevin and in what order, nothing input-free left in the approved queue), row 47's hash | docs only |

| 49 | `7e6f069` | **Diagnostics speed test: two labelled passes** — raw (`sendXYrawUnchecked`) and checked (`sendXYraw` + RouteSafety short-check). Kevin's "300 kHz → 10 kHz": the test only ran the checked path and called it raw; bisected via BOOTSEL/picotool to the 5.7.2.0 checkpoint — 93–96 µs per cycle on every build since the short-check landed; raw path 3.9 µs per cycle (256 kHz) | builds ×3; on HEAD: raw 256 kHz, checked 10.8 kHz; check toggled back on |
| 50 | `6ee9abf` | **Release 5.7.3.0** — `VERSION` 5.7.2.0 → 5.7.3.0 (Kevin: "let's tag this as a release where we are now"; the next hand-bumped third component after his `5.7.2.0` checkpoint — the fourth is CI's auto-bump, the second is the public named line, his call), rebuilt uf2, annotated tag `5.7.3.0` on this commit (local only, nothing pushed) | builds ×3 (v5 5.7.3.0, v5_debug 5.7.3.0, og 1.7.3.0 — the version string grep'd in each uf2); flashed the fresh v5 uf2 via BOOTSEL/picotool; `?` on port 1 → `Jumperless firmware version: 5.7.3.0`; `i@` probe_power on → DAC0; `run_all.py` ×3 = 6/7 each (the tolerated phantom-current check only) |
| 51 | `7b5c412` | **T3.2 (C2b): the CH446Q chip-select strobe done by a second state machine on PIO2 (GPIOBASE 16)** (Kevin: "start with the second SM CH446Q strobe, put it on PIO2"). `ch446_pio2cs.pio(.h)`: the shifter's per-crosspoint CPU handshake (`irq nowait 1` / `wait 0 irq 1 rel`) becomes a cross-block one (`irq prev set 4` / `wait 1 irq 5` — PIO0's *prev* wraps to PIO2), and an 8-instruction strobe program on PIO2 pulses the selected chip's STB (~160 ns) and acks; ONE word carries the address byte (top) and the chip-select mask + LAST (bottom), so a list send is two DMA channels over the same `dmaWords[]`, a single send is two FIFO puts + a flag poll (no ISR), and one exclusive PIO2_IRQ_1 per list completes the mailbox request (shared-IRQ chain 6/6 → 5/6: the PIO0_IRQ_1 handler is not registered in strobe mode). Legacy ISR path kept as the fallback (and the OG), its blanket PIO0 flag clear narrowed to flag 1; `chipSelect` is a real single-send token (init −1, `sendPath` no longer clobbers it). PIO layout: PIO0 = shifter + probe LED/button SM (`probeLEDs.setPreferredPIO(pio0)` — new `JeoPixel::setPreferredPIO`), PIO1 = top strip + encoder, PIO2@16 = strobe + breadboard strip; the encoder / debug-button block searches skip a base-16 block. `X`: `cs strobe: PIO2 SM0 (base 16), one IRQ per list … | probe LED SM: PIOn button: PIO/CPU` (FALLBACK + reason otherwise), and the PIO map shows each block's base | builds ×3; `X` census PIO0@0 SM0/1, PIO1@0 SM0/1, PIO2@16 SM0/1, CS pins = PIO2, `button: PIO`, shared-IRQ 5/6; `run_all.py` ×3 = 6/7 (phantom check only; the real current loop 4.76 mA); connect/clear/connect/disconnect INA0 4.76 → 0.58 → 4.76 → 0.52 mA; crossbar self-test app `rows:60/60 gpio:8/8 rails:2/2 OVERALL: PASS`; 500-op soak 662 lists = 662 IRQs, 0 stalls, 0 timeouts, 0 WARNINGs, mailbox idle, audit suspect=0; ~860 k singles, 0 timeouts; A/B vs `6ee9abf`: latency probe pickup→send 61/217 vs 57/236 µs, speed test raw 3.5 vs 3.9 µs per cycle, kick gaps under `run_all` 2.34/1.29 s vs 2.30/1.19 s (pre-existing) |
| 52 | `42bf038` | **T3.3 (C14): WaveGen streams over I2C0 DMA with a pacing timer; the I2C0 arbiter** ("then continue with the rest"). `WaveGen.cpp/.h` rewritten: an image of the exact IC_DATA_CMD entries per sample (heap, ≤ 1024 samples), an aligned static address ring, channel A (image → I2C0 TX FIFO, 3 entries per trigger), B2 (ring → A's READ_ADDR_TRIG), B1 (a D-transfer divider paced by a DMA timer, chained to B2 and back); f = clk·X/(Y·D·N), exact — N a power of two the bus can carry (85 % of the capacity computed from I2C0's SCL counts), D and X/Y by continued fractions; control synchronous on core 0 (all callers are core 0), core 1's `service()` is a monitor (laps, wedge latch, TX_ABRT clear); stop by the datasheet's abort sequence (EN+CHAIN off, one CHAN_ABORT, bounded — the SDK's `dma_channel_abort()` hung once on a timer-parked channel), drain, DMA_CR off; `isRunning()` held across setter restarts; legacy loop = fallback and the OG. `I2C0Arbiter.cpp`: `--wrap` on `i2c_write_blocking_until` / `i2c_read_blocking_until` — every Wire master transaction pauses the stream at a sample boundary and resumes it (pause held across a repeated-START pair; monitor releases a stale hold; failed transactions release). `Commands.cpp` refresh wait keys on `isCoreLoopStreaming()` (legacy only) — the crossbar follows the netlist during a stream. `X`: `wavegen:` line (plan + health) | builds ×3; lap rate 4.982/99.963/999.663 per s at 5/100/1000 Hz over 15 s; ADC0 via crossbar 5.0011 Hz for 5.0, 20 Hz edges 50.0 ms apart through a flash-write stall; start/stop at 5/100/1000 Hz stop wait ≤ 57 µs, 0 abort timeouts; mid-stream (2 kHz) routing: INA0 4.76 → 0.55 → 4.79 mA with `dac_set`/`ina_get_current` through the arbiter (6 yields, max 48 µs; 16/201/118 mA garbage without it), req→pickup max 349 µs (was 7 684 589 µs), mailbox idle; kick gaps in a 10 s 2 kHz stream core 1 0.53 ms (was 10 007 ms); stream + soak500 + LEDs together: 1530 lists = 1530 IRQs, 0 stalls/timeouts/WARNINGs, audit ok; `run_all.py` ×3 = 6/7 (phantom check; one post-flash 4/7 transient); `i@` probe_power on → DAC0, DAC0 3.33 V |
| 53 | `060d52e` | **T3.3 follow-up: the four MicroPython API INA reads (`ina_get_current/voltage/bus_voltage/power`) no longer pause core 2** (they paused it 50 µs + an aborted LED frame per call "to prevent Core 2 I2C conflicts" — obsolete: I2C0 has no core-1 user, and the wavegen's DMA stream is handled by the I2C0 arbiter around that very read). C3's rule, now for the API. The rest of `pauseCore2` (T3.4 / C16, ~230 sites) waits for Kevin per section D's order (after T2.2c / T2.4) | builds ×3; `ina_get_current(0)` on the 4.76 mA loop reads 4.79 mA idle and 4.79 mA during a 2 kHz stream (arbiter), voltage/bus/power sane; `run_all.py` 6/7 then 5/7 — the extra failure is `test_paste_state`'s 1.0 s prompt window vs the deferred flash flush (flaky on `7b5c412` too: bisected by re-flashing; 4/5 spaced runs pass on HEAD) — not this change; `i@` probe_power on → DAC0 |
| 54 | `15db8c0` | Docs: the paste wait blocks loop0 (terminal + REPL unresponsive mid-paste), the 32 s `X` artefact from the harness explained, checklist item 10 (INA-read LED stutter gone, the paste flake), row 53's hash | docs only |
| 55 | `557203e` | **T2.1 (C1): the always-on ADC ring** (Kevin picked it, at the board). `AdcRing.h/.cpp`: the ADC free-runs round-robin over all 8 inputs at 48 kHz/ch (384 ksps) into an aligned 8 KB SRAM ring by ONE DMA channel in RP2350 TRIGGER_SELF mode (1 ms blocks, hardware write-ring wrap, block IRQ on DMA_IRQ_1, core 0) — no core in the data path; halfword i = channel i&7 of sweep i>>3; sample counter under a claimed spinlock from the live write pointer; overrun → resync (generation++). `readAdc()`/`readAdcHeld()` keep the 'fresh burst now' meaning via `adcRingMeanAfter` (waits ~21 µs/sample) — drive-then-read sites correct unaudited; `adcTryAcquire()` = true on the ring. Hot paths: `readProbeRaw` = the same N-burst/variance/median decode over ordered windows of the newest N×B sweeps (one memory read), `checkPads` waits ≥16 fresh sweeps between its 12 decodes, the OLED cache = 16-sample means; NetVoltageScan taps read fresh sweeps, no lock. USB audio = a consumer (`usbAudioOnRingBlock` from the block IRQ, decimating; rates must divide 48 k; ownership/probe-pause/sentinels gone). D menu 'ADC Ring A/B' flips ring↔legacy live; `X` `adc ring:` line. OG keeps START_ONCE | builds ×3; X: 48.5 k sweeps/s, 1 kHz block IRQs, 0 overruns/resyncs/stalls after 7.5 M reads; A/B `adc_get()` ring vs legacy identical on all 8 channels; ProbePads 36.8 → 4.6–5.7 ms/poll (share 65–70 → 4.6–8.9 %), core-0 loop 1.3 k → 3.6 k passes/s; `run_all.py` 7/7; `test_infra_paths` 24/24; 500-op soak clean; **Kevin at the board: taps SELECT/MEASURE, connect/clear/remove, pad menus, drag highlight, reading display → 'same or better — commit it'** |
| 56 | `8428397` | **`ch446.pio` is the source of truth again + an SRAM diet, after two finds while tagging 5.7.3.1:** (1) the platform's `_build_pioasm.py` re-runs pioasm on any `src/**/*.pio` newer than its `.pio.h` — a stash/checkout that touched the `.pio` regenerated the hand-edited `ch446.pio.h` and silently put the legacy shifter back on IRQ flag 0 while `isrFromPio` clears flag 1 (V5 unaffected while the PIO2 strobe is active; the OG and the V5 fallback would have been dead) — the `.pio` now carries the flag-1 program and the real init (`pio_set_irq1_source_enabled`, drive strength), regenerated header byte-identical in its instruction words; `quadrature.pio.h` regenerated (comments only). (2) `test_config` rebooted the board 1 in ~2 runs on the T2.1 build: crashlog `HardFault core 0, PC=__breakpoint LR=abort` — the only `abort()` callers in the ELF are `operator new` / `__throw_bad_alloc` / `__assert_func`; heap free had gone 37.5 KB (5.7.3.0) → 33.3 KB (T3.3, its 4 KB static ring) → 25.0 KB (T2.1, +8 KB ADC ring, +2 KB audio ring), and the deferred incremental config save aborted on a `new`. SRAM given back: `CH446Q_DMA_MAX_WORDS` 2048 → 1024 (−5 KB; max seen 73 words per send, a full collect flushes anyway), the audio SPSC ring 2048 → 1024 samples (−2 KB), WaveGen `MAX_WAVEFORM_TABLE_SIZE` 1024 → 512 (−2 KB static, −4 KB heap when used) → free 34 KB again. **Open for Kevin:** the config-save path has a plain `new` that aborts the board on OOM — 33 KB of headroom is thin for it; the 8 MB PSRAM is 0 % used | builds ×3; `test_config` ×8 = 8/8, 0 reboots (was 1 in ~2); `run_all.py` ×2 = 6/7 (phantom check); heap free 33–34 KB; adc ring 0 overruns after 8.7 M reads (1 stall = a wait spanning a flash-write park); uptime continuous 941 s |
| 57 | `331dd6b` | **Release 5.7.3.1** (Kevin: "tag this with a bugfix version") — `VERSION` 5.7.3.0 → 5.7.3.1, rebuilt uf2, annotated tag `5.7.3.1` on this commit (local only) | builds ×3 (5.7.3.1 / 5.7.3.1 / og 1.7.3.1); flashed; `?` → 5.7.3.1; the row-56 verification is on this exact build; `i@` on → DAC0 |
| 58 | `e795630` | **`src/` reorganised into feature folders (Kevin's move, replayed with `git mv` on the verified tree: 97 renames — coredination, eyecandy, hardwarestuff, remembering, routing, selfreflection, sensing, snakes, tubes) + `scripts/src_subdirs_include.py`** (pre-build: every folder under `src/` with headers goes on CPPPATH, so includes stay bare names; `-Isrc/routing/` dropped from `platformio.ini`; the one relative include `../CoreMailbox.h` in RouteSafety.cpp fixed; comments naming moved files updated) | builds ×3 (the script lists the folders it added); flashed; `?` 5.7.3.1; X census / heap / adc ring identical; `run_all.py` 6/7 (phantom check) |
| 59 | `1c7249f` | **T2.2c (D): `showLEDsCore2` -> `core1req::REQ_SHOW_LEDS`** (Kevin picked it from the queue). The packed magic-int (0/±1/2/3/+10, ~270 writers) became a typed mailbox request: `requestLedShow(1/2/3/-1/12/0)` keeps the vocabulary at every site, `ledShowIdle()`/`ledGraphicsOwned()` the readers; mode bits NETS/MENU/GFX + flags CLEAR/BLOCKING; a MENU flush cannot downgrade a pending NETS/GFX (keep-rule inside `postMode()` under the slot spinlock); core 1 takes it in core2stuff and completes the generation after the show; staged-graphics ownership is a state bit. `X`: `leds bits` mailbox column, `led frames shown`, `led takes` 32-entry log. Three lessons (in-code + doc): waitCore2 waits on the SEND slots only (an LED-slot wait added ~25 ms to every refresh - Kevin felt it); the exit-blank/frozen-ants bug (press-anim mode-2 flush downgraded the menu/probe exit's nets+clear - found with the SWD encoder harness + the take-log, fixed by the keep-rule); the SWD harness is the repro path for LED-ordering reports | builds ×3; **Kevin on hardware: menu exit (hold-to-back + click-out), probe exit, current ants - "all seems to work fine"**; run_all 7/7 (pre-hardening) then 6/7 (phantom check) on the flashed build; SWD take-log shows the exit's nets+clear surviving; idle nets renders 20.4/s; leds req==done at rest; heap 33 KB. Separate PRE-EXISTING finding (checklist 1, A/B'd on 557203e): the probe LED's faint measure-green flicker in connect mode - the switch classifier's, not T2.2c (no showProbeLEDs write changed) |
| 60 | `18e9f67` | Docs: corrected the remote state in both handoff docs (dev is pushed through the reorg `e795630`; the `5.7.3.0` tag is on origin; only T2.2c `1c7249f` + the `5.7.3.1` tag are local) - the earlier "nothing pushed" was stale | docs only |
| 61 | `ab97c7f` | **No-PSRAM V5 hard boot loop fixed** (Kevin's report: flashing 5.7.3.1 onto a board without PSRAM boot-looped). Root cause, SWD-proven end to end: `initMicroPythonProper()`'s blind `malloc(96 KB)` fails on the no-PSRAM SRAM heap (observed `mp_heap == NULL` over SWD; ~105 KB nominally free but no 96 KB contiguous block - fragmentation), `MpRemoteService::service()` IGNORED the failed init and ran `pyexec_event_repl_init()` on a heapless VM, every allocation raised MemoryError through the emergency exception buffer (`mp_state_ctx+0x28` was the exception val on the dead stack) with no nlr catcher on that path, and `nlr_jump_fail`'s mp_deinit/mp_init "recovery" re-raised: 28 nested rounds, ~0x100 B of stack per round, one round per ~2 s (its two `delay(1000)`s - hence `sleep_us` in every SWD halt sample) -> STKOF at t+28.5 s every single boot -> watchdog reboot, forever. The <10 s crashlog bootloop guard never trips at 28.5 s, and the recorded PC/LR looked like printf guts because an STKOF exception frame is VETOED (SP pinned at MSPLIM, writes suppressed) - the record is residue on this fault class. Three fixes: (1) MpRemoteService honors the init result - single choke point, one port-1 message, raw REPL cleanly disabled; (2) `mpAllocHeap()` ladder (configured/64/48/32/24/16 KB, each rung really malloc'd - free total != largest block - keeping a 24 KB C-heap reserve, the config-save `new`-abort floor measured in T2.1; failures print on port 1, not just port 3, which is why the original failure was invisible); (3) `nlr_jump_fail` reentry guard in BOTH identical port copies (`lib/micropython/port/` AND `lib/micropython/micropython_embed/port/` - both compile), flag set at entry so a raising recovery parks instead of recursing. Plus: CrashLog gained the **STKOF postmortem** - when the consumed record is a core-0 STKOF, `crashlogLatchAtBoot()` walks the dead stack above the pinned SP (SRAM survives the watchdog reboot; the deep frames are intact at early setup) and the report prints raw words + flash-text LR candidates, repeats = recursion fingerprint. That walk plus SWD halt-sampling is the standard STKOF postmortem now | builds ×3; flashed the no-PSRAM board (id B30E80A140169BAB): before = HardFault at uptime 28.45-28.49 s on 3/3 observed cycles; after = 1 port open / 0 disconnects in 100 s, port 1 interactive, `X` census clean (SRAM 206 KB total / 41 KB free after MP, adc ring + cs strobe healthy, uptime 174 s+), SWD: `mp_heap=0x20065950` size 64 KB (ladder stepped 96->64); run_all on the no-PSRAM board: 5/7 — test_config PASS (the F2 gate: config save with the 24 KB reserve holds) and test_micropython_fs PASS (MicroPython real work on the 64 KB rung); the two fails are the standing tolerated ones: test_net_currents (phantom-current check) and test_paste_state (the documented S/L-prompt flake — "the paste still applied"; board was unplugged for shipping prep before a solo re-run could land) |
| 62 | `0399aab` | **Release 5.7.3.2** — the no-PSRAM boot-loop fix (row 61) is the ship blocker it clears: Kevin is flashing no-PSRAM units for shipment. VERSION 5.7.3.1 -> 5.7.3.2 (Kevin's bump), uf2 rebuilt with 5.7.3.2 baked in (og 1.7.3.2), annotated tag `5.7.3.2` local — Kevin pushes `dev` + the tag (CI publishes from the tag per release.yml) | builds ×3, version strings verified in the elf (5.7.3.2 / 1.7.3.2); the fix itself was hardware-verified at row 61 on the no-PSRAM board; the board was unplugged for shipping prep before a banner check of this exact build — logic-identical to the row-61 build except the version string |
| 63 | `_______` | **MicroPython GC heap default 96 -> 64 KB on V5 (Kevin's working-tree edit, committed as found)** — `MICROPY_HEAP_SIZE`/`_PSRAM` (`JumperlessDefines.h:41-42`). The row-61 ladder proved 64 KB is the rung a no-PSRAM board actually gets (96 KB never fits the SRAM heap), so the configured size now matches reality and the ladder's step-down message stops firing on every no-PSRAM boot; PSRAM boards drop 96 -> 64 KB too (Kevin's call). OG stays 28 KB | verified together with row 64's build: ×3 builds, flashed, `test_micropython_fs` PASS at the 64 KB default (the whole HIL run below ran on this heap) |

"HIL 5/6" everywhere (6/7 from row 30 on, when `test_paste_state.py` joined the suite) means: the one failure is `test_net_currents` "zero-load
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

- **Switch sensing after a DAC claim** (the "losing the switch state" report,
  2026-08-16 - root-caused and fixed, needs one hands-on confirm). Chain:
  `dac_set(0, <outside 2.80-3.90V>)` relocates the buffer feed off DAC0; the
  relocation can reset the probe's LED chip dark; the select-idle LED's draw
  IS the current signature the classifier reads, so a dark LED reads exactly
  like MEASURE and the position flipped with the switch untouched - and
  releasing the DAC never moved the feed back (no nudge), so it stayed wrong
  until reboot. Three fixes in `checkSwitchPosition()`/`setDac0voltage()`:
  release-side `infraNudge()` (verified on hardware: feed returns to DAC0/INA
  within ~1.5s), SELECT->MEASURE now needs two consecutive below-low readings
  with an LED re-send between them (a reset chip re-lights and the flip is
  discarded), and a 5s LED keep-alive re-sends the static idle pattern so a
  dark chip always heals. **To confirm:** switch at SELECT, probe attached,
  run `dac_set(0, 0.5)` then `dac_set(0, 3.3)` from the REPL - position must
  stay SELECT (worst case one discarded-flip message with
  `probe_switch_stats = 1`), and the probe LED must visibly re-light within
  ~5s if it blinks out.

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
- Droop and INA report very different magnitudes for the same probe LED load
  (~9 mA droop vs ~1.4 mA INA) against SHARED thresholds (0.90/1.20 mA). It
  works today because both sit on the correct side with margin, but it is the
  next robustness hole in this classifier if anything recalibrates.
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
