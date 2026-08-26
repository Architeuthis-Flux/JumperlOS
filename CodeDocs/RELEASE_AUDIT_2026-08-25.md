# Release audit — 2026-08-25

Two adversarial sweeps ahead of the first user-facing release carrying the
Parts system. Shape: N dimension finders in parallel → dedup → **two
refute-biased verifiers per finding** (default position: the finding is
wrong), verdict CONFIRMED only when both verifiers failed to refute it.

| Sweep | Scope | Finders | Agents | Confirmed |
|---|---|---|---|---|
| arc | the 2026-08-25 bench work (parts, probe taps, display stack, readouts) | 8 | 57 | 21 |
| codebase | everything else, including code the arc never touched | 12 | 70 | 27 |

**48 confirmed, 48 fixed** (commits `57cf63d`, `47325d3`, `dec1b45`, and the
`/.bak` gate). Both targets build green at every commit. 50 further findings
reached "plausible but unverified" when the first run hit a session limit —
they are listed at the bottom as the next pass's work-list, NOT as known-good.

## What the sweeps caught that mattered most

**Memory corruption reachable in normal use.** The per-net `bridges[]` table is
`[MAX_NODES][2]`, but the index scan ran to `MAX_BRIDGES` and its `0x7f`
fallback wrote further still — a 25-bridge GND bus on OG corrupted the net's
own do-not-intersect safety list and then marched into the neighbouring net's
name pointer, **on every rebuild**. Alongside it: `sprintf("%s cleared")` into
a 12-byte stack buffer on every named-node clear ("NANO_3V3 cleared" is 17),
the GPIO pad menu writing `gpioState[-1]`/`gpioDirection[-1]` (the latter is
`railPriority`, which persists), `help()`'s unbounded `strcpy`, and
`couldntFindPath` writing past `unconnectablePaths[10]`.

**Silent data loss.** Cancelling the Rails slider drove both rails to 0 V and
persisted that over the saved voltage. The Snake app called `clearAll()` every
frame, so user overlays were wiped *and* auto-saved. Host-initiated USB eject
threw away dirty breadboard state (the device-side eject path saves it). Raw
delete/rename/mkdir had no `usbMountedByHost` guard. A corrupt
`/undo_history.txt` length wrapped a pointer comparison into a ~4 GB memcpy at
every boot.

**Contract drift the bench could never show.** Three of the display service's
modal-load gates keyed on `getBlockingService()`, which is always `nullptr`
inside the synchronous modal loops that pump `serviceInner()` — so every one
of them was dead. That is now `jOS.inModalContext()` (a real inner-dispatch
depth). Same family: `MeasureMode` was the last consumer still waiting for 5
stable probe readings against a cache that emits a held row once per 500 ms —
it only worked because that service happens to run every pass.

**An OG landmine.** `DisplayService` registered unconditionally, and its bus
hardcodes GPIO 24/25 — which on OG are the CH446Q **RESET** line and the
WS2812 **LED data** line. A shared project file with a display part would have
held the crossbar in reset, wiping every connection while the firmware
believed the routes were intact. Now gated on `caps.breadboardDisplays`, at
both the service and the acquire.

## Ledger

Full titles, files, and the verifiers' traces live in the two workflow
journals under
`.claude/projects/.../subagents/workflows/wf_44c66552-684/` and
`wf_76944873-c95/` (`journal.jsonl`, one line per agent).

### Fixed in this pass — arc sweep (21)

setGPIO/readGPIO honoring the `gpioState==6` bus mark · OG board gate ·
`inModalContext()` for the three dead modal gates · power auto-routing gated
on role VCC (regulator VOUT and 405x VEE were being wired to a rail) ·
400 ms tap deadtime (a resting probe was accepted the instant the prompt
opened) · `UndoIngestGuard` around `routeDataPins` (one undo press after
placing a display disconnected it) · YIELDED release no longer stomping the
user's pins · PWM counted as a pin claim · `pollParts` comparing part **name**
(table compaction could slide a same-driver part into our index) ·
pre-placement netlist / bridge-adoption guards · serial typed-row honoring the
used-row refusal · MeasureMode contract · `activeDataNodes` covering YIELDED ·
`netSemanticName` truncation preserving the pin suffix and skipping special
nets · parts placement/clear made non-undoable (the bridges were in the undo
stream but the parts table was not) · detach resetting every strike counter ·
framebuffer alloc before routing, with a warning · routing-refusal backoff ·
ghost-scan unsticking the bus instead of blaming pull-ups.

### Fixed in this pass — codebase sweep (27)

NetManager bridge/node table bounds (+ the `combineNets`/`shiftNets` sibling
loops and the 6-of-8 DNI clear) · `node1Name` stack smash and its
uninitialized read · GPIO pad-menu index base · Rails slider cancel ·
`help()` strcpy · USB→UART backpressure · Snake overlay wipe (and
`_DIRECT_PIXELS_` becoming session-only) · OLED pin-change panic · OLED
runtime geometry desync · `unconnectablePaths` bounds · double-tap confirm
made cadence-independent (it was unreachable on the CPU button path) ·
LED-show request completed and re-posted on frame-hold abort ·
`printPowerSupplySense` no longer writing to USB CDC from core 1 ·
`menuLines[-1]` · the two duplicate single-char triggers (`^`, `_`) plus a
collision warning · `display_type` dangling pointer · USB-eject dirty save ·
`/.bak` stale-mirror restore gated off · raw-FS USB guards · undo blob-length
wrap · OLED rotation persistence · background ticks raising
`jl_vm_exec_depth` · quiet MicroPython init mounting the VFS.

## Not fixed — the next pass's work-list

50 findings were verified-pending when the run hit its limit. They were
produced by the same finders but have NOT had adversarial verification, so
expect a meaningful refutation rate. Highlights worth looking at first:

- `new (std::nothrow)` reportedly **aborts** on OOM in both shipped images —
  if true, every allocation fallback in the firmware is dead code.
- `initINA219`'s V5 path may boot-wedge forever if either INA219 is silent
  (the OG path is bounded).
- PWM is not OG-gated: `jl_pwm_setup` on OG would drive the CH446Q chip
  select, WS2812 data, and ADC pins (same family as the display-bus landmine
  fixed above).
- GPIO pin claims are never released after file-run scripts or a soft reboot.
- A stale duplicate `micropython_embed.c` is compiled in; only link order
  keeps the GC-root fix alive.
- `usb_cdc.ignore_dtr=1` turning an unread CDC port into a core-0 spin.
- USB audio mic reportedly streams silence (the PCM ring consumer was deleted
  in T2.1 and never reinstated).

The full list with scenarios is in the workflow journals named above.

## Bench gates for this release

1. Place a part per signal (tap VCC/GND/SCL/SDA) → labels bloom → power
   auto-routes to the rails.
2. `i!` → `[disp]` line: `frames` climbing, `retries`/`lost` near zero.
3. Probe a part pin → the OLED says `<part> <pin>`; probe GND → still says GND.
4. Double-tap REMOVE to undo — this should now work on both button paths.
5. Rails → Both → drag → **cancel**: the rails must stay where the drag left
   them, not drop to 0 V.
6. Mount over USB, edit a file on the host, eject: host edits land and your
   unsaved wiring is not lost.
