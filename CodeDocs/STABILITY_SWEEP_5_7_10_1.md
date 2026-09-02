# 5.7.10.1 stability sweep — handoff (2026-09-02)

## Where things stand

dev = `75758b6`, five commits past the 5.7.10.0 release (`5cbe06e`):
sweep part 1, part 2, the isolated OG-router fix, this handoff, and a
follow-up (auto_connect persists as "on"; the menu DAC preview is flagged
as a user write so the probe feed does not re-park DAC 0). Both targets build (V5 and
OG). The V5 board on the bench runs the part-1+2 build (fingerprinted:
XIP `0x10000000 + 0x14dd58` reads the new ekilo status string). Kevin's
slot0 was captured over port5 before the bench checks and restored after;
the semantic fingerprint matched.

- `020f59a` part 1: release blockers + one-line fixes (44 files)
- `1a24172` part 2: terminal parsers, undo arena, measurement restores (7 files + uf2)
- `ec92a1a` OG router hop-pin fix — **untested on OG hardware, drop if in doubt**
- `75758b6` follow-up (above); the board was re-flashed with it and the
  flash bytes match the image at four offsets.

Known gaps stated plainly: bcdEnsureRange only drives the ROUTED-pins
default range immediately (the fallback-run site still persists without
driving); a highlighted negative rail is plain blue while the unhighlighted
rows are the "hot" doubled blue (cosmetic); MicroPython connect()/
fast_connect() now return 0 for a refused bridge (was always 1); the
`f { ... }` paste keeps rails/parts/names (was: reset to defaults) - the
jumperless-v5 skill and the LBM repo do not send it.

VERSION is still 5.7.10.0. The bump → merge to main → release workflow is
deliberately left for after Kevin has turned the wheel on the encoder paths
below. Never push.

## How the sweep ran (so the next one is cheap)

- Fleet: 81 chunk finders + 32 change-unit reviews found 244 raw candidates
  (209 after dedup). One Opus verifier per candidate, 7-tool-call budget.
- Per-agent cost after the budget rewrite: verifier ~0.17M weighted tokens,
  finder ~0.46M (was 2.37M median). The whole verified run was 312 agents,
  16M subagent tokens, 2.2 h, no agent died.
- Fable read the code for every one of the 209 confirmed items (all 40
  crit/high with surrounding code, mediums/lows against the verifier's
  cited lines) before anything was edited. 35 verifier-rejected items were
  reviewed too and all 35 rejections stood.
- Build: system `pio` is Python-3.14-gated. Use
  `<scratch>/pio313/bin/pio` (python3.13 venv, platformio==6.1.19); see the
  memory note `pio-broken-python314`.
- Flash: `nodes_save()` over port5 → 1200-baud touch on port5 →
  `picotool load -x` (first attempt succeeded). Fingerprint with
  `uctypes.bytes_at(0x10000000 + <bin offset>, n)` against a string that
  exists only in the new build.

## What was fixed

The commit messages carry the full lists. Headlines:

Release blockers (critical):
1. Output > Voltage passed the picked option INDEX to getActionFloat as a
   RAIL code — dialing DAC 0 live-drove BOTH rails. Option text now maps
   to codes (Top R=1, Bot R=2, DAC 0=3, DAC 1=4); getActionFloat previews
   DACs in the same 0..5 V band; doMenuAction never writes a NaN power
   field after a cancelled slider.
2. ekilo entered "new file" mode under the same name for ANY failed open
   (too large, fs timeout); the first save truncated the real file.
3. Wokwi parseVoltageString spun ~4.3e9 iterations on an empty string.

GPIO (the big cluster): unrouted outputs came up HIGH because readGPIO
stamped the input marker on them and the appliers used gpioState as the
level. Plus: PWM re-arms from the slot on load (the docs already promised
this), shared PWM slices are sibling-aware, slow→fast PWM tears down the
timer, config apply keeps a live output level, counter reads skip
PWM-owned bits under the pad lock, queued probe press events are consumed
on carousel exits, and the GPIO click window was gated on a global that
nothing ever wrote (`oled.oledConnected` now — the 1 s window never opened
before this).

Everything else is in `git show 020f59a` / `git show 1a24172`.

## Deferred (Kevin decides) — real per the read, not in this release

| # | Where | Why deferred |
|---|---|---|
| 13 | Probing.cpp DAC adjust lambdas ignore `isLive` | Honouring it changes live-scroll UX for bipolar DACs; #11's cancel-restore fixed the actual harm |
| 124 | CrashLog bootloop guard can never trip | Fixing it makes a 3-crash board HALT (its intended behaviour) — semantics call |
| 74 | RotaryEncoder reset overridden by the click AFTER window | Encoder core; subtle, needs bench with the wheel |
| 82 | DisplayService routeDataPins partial rollback | Bridge-table-full only; moderate rework |
| 95 | applyPinConfig writes pulls/dir unlocked vs core 1's float sweep | Lock nesting with bcdApply; needs a design pass |
| 104 | WaveGen legacy (OG) stop() vs core 1 Wire | OG/legacy path, complex wait logic |
| 109 | streamFrameToSer3 FIFO gate | Not implementable as suggested (FIFO < one frame) |
| 110/111 | ekilo low-memory insert/load failures | Only under low heap; needs bool-returning row API |
| 114/115 | USBfs mutex re-entrancy on mount/eject | USB MSC edge cases, needs bench |
| 135 | USB audio PCM ring has no consumer (mic streams silence) | Feature work, USB audio is low priority |
| 77 | HOOK_OLED_PIN live re-pin leaves gpio_sda/scl stale | Needs the pin→RP_GPIO_n node table |
| 165 | config.txt.tmp never recovered after a failed rename | Moderate; add recovery in the not-found branch |
| 139, 183, 189, 202, 44, 84/173, 57, 66-alt, 125, 132, 174, 197, 203 | assorted | Low value / needs more reading than the release warranted |

`#57` (Menus.cpp header highlight `node - 70`) — the verifier said the
suggested fix is wrong and I did not find the right one from the digest;
read `bbPixelToNodesMapV5` before touching it.

## Bench-required (needs hands on the wheel or a part on the board)

Not drivable from port5/port7 — please check on the real thing:

- **Output > Voltage > DAC 0 → turn the wheel**: rails must stay put, DAC 0
  moves in the 0..5 V band, confirm applies the final value. (The critical.)
- **Rails > Both → enter and immediately long-press**: rails and the saved
  state must be unchanged (was: both stamped with their average).
- **Brightness > Wires**: preview is the wire scaling and
  `special_net_brightness` no longer changes. Note: existing users have a
  corrupted `special_net_brightness` in config; the fix does not repair it.
- **Click menu → GPIO/UART/DAC → pick a nano-header pin**: the right pin.
- **Highlight a rail, click, turn past 5 V, click**: the rail carries the
  confirmed value (was preview-only). Long-press cancel puts it back.
- **Highlighted net + remove button twice**: one probe session, not two.
- **GPIO carousel exits via the probe button**: no phantom probe session.
- **Highlight a GPIO net on a board with an OLED**: the 1 s `options?`
  click window now actually opens.
- **Part scan with a 10 k resistor AND a large electrolytic**: the
  classifier gate change (#40) must call the 10 k a resistor and must not
  call a still-charging cap one.
- **4051 with VEE and VSS both on GND**: no `self_short` pulse.
- **OG board (if one is around)**: anything routed BB → nano pin via an
  alt path, then a second net that needs the same bounce lane.

## Round 2 (2026-09-02, after Kevin's first look)

Kevin's board was NOT running the dev build when he tested: raw flash bytes
differed from the 75758b6 image at three of four XIP offsets (an IDE upload
from JumperlOS-main replaces the dev build silently). So "setting the DACs
changes the rails" was the 5.7.10.0 bug, not a failed fix. The dev build
still has to be flashed and wheeled.

What changed in this round (build-verified on both targets, not benched -
the board is Kevin's right now):

- **DAC slider readout on the rail strips.** Kevin's design: the Output >
  Voltage slider borrows the TOP strip for DAC 0 and the BOTTOM strip for
  DAC 1, rendered exactly as that rail's own highlighted bar, while the rail
  itself never moves. `railLedPreviewVolts[2]` (LEDs.h, -100 = none) is read
  by lightUpRail ahead of railHwVolts; getActionFloat sets it on entry and
  every tick and clears it on all four exits. The probe-touch branch of the
  slider now applies DAC 0/1 too (it only applied rails).
- **OLED "FLOATING" -> "FLOAT"**: the long word overlapped the `options?`
  tag now that the GPIO click window actually opens (Highlighting.cpp, both
  sites). MicroPython's FLOATING constant is untouched.
- **Stacking per class.** New config keys `[routing] stack_gpio` and
  `stack_adcs` (both default 0), Stack menu rows GPIO / ADCs, and the `b`
  print shows them. The real bug was upstream: addConnection stamped the
  slot's stackPaths into every bridge added without a count, so the router
  never saw "-1 = default" and never classified - GPIO/ADC bridges stacked,
  rails got stack_paths (2) instead of stack_rails (3), DACs got 2 instead
  of 0. Now: -1 is stored as -1; the router resolves per class (rails,
  DACs, GPIO+UART, ADCs, else stack_paths; fake/virtual never); the YAML
  writer omits `dup` for defaults (older firmware parses that as -1 and
  classifies too); fromYAML turns a stored count equal to the slot's
  stackPaths back into default (that is the exact reversal of the old
  stamping - the one edge: an explicit per-connection count equal to
  stackPaths on a GPIO/ADC/rail/DAC bridge becomes default). Re-adding an
  existing pair without a count no longer increments its duplicate count
  (a probe tap on an existing wire used to spend a lane silently).
- **ADC flicker (Kevin: "ADCs rapidly flipping between 0 V and the real
  value")**: not reproducible now (GPIO 1-3 read OUTPUT / no pull over
  port5). In his screenshot all three 4051 select lines were INPUTS and
  4051_A read "input - floating"; core 1's floating test wiggles the pulls
  of a no-pull input every sweep (RoutableGpio.cpp readGPIO), which
  toggles the 4051's channel and makes both ADC readings alternate between
  the selected and the unselected channel - exactly the symptom. Likely
  mechanism, pre-existing, not confirmed. The HIL run touched only GPIO 1
  (output, then input, then the slot was reloaded); B and C were inputs too,
  so that state was not the HIL's. If it recurs: look for "input -
  floating" on a select line in the netlist column.

- **Clickwheel part walk goes round the whole part (Kevin, 14:20).** The
  encoder scroll used to walk only the pins on the half the part was
  entered on, then leave; now, from the entry pin, the detents go round the
  part in pin-number order - off the edge of one side, onto the next pin
  number on the other side - until every on-board pin has been seen, then
  the row scan resumes just past the part's span on the half it was entered
  from (the rows the part still owns are not stops for that one scan). The
  first step goes the way the wheel points on that side; UP keeps stepping
  the ring, DOWN steps it back, so reversing retraces. Host model of the
  rules (DIP-16 / SIP / 2-pin / 1-pin, all entry points): see the commit.

Bench-required additions:

- **Output > Voltage > DAC 0, turn the wheel**: the TOP strip shows the bar
  at the dialed value, the rails do not move, DAC 0 follows in 0..5 V,
  confirm applies, long-press restores the strip. Same for DAC 1 / bottom.
- **Highlight a GPIO input that floats**: the OLED says FLOAT and the
  `options?` tag no longer collides.
- **Scroll onto a DIP with the wheel**: from the landing, every detent is
  the next pin number round the chip (across the edge to the other side),
  then the scan continues past the chip on the side you came in on.
  Reverse the wheel mid-walk: it retraces.
- **Kevin's 4051 slot**: after loading it on this build, `b` should show no
  GPIO duplicate (path 21 in his paste) and ADC_1/ADC_2 should route (chip
  K had all eight lanes taken). Routing > Stack > GPIO / ADCs pick and
  the TUI [routing] rows show the two new keys.

Docs to add to the list below: 06-config `[routing]` table gets stack_gpio
and stack_adcs; any OLED screenshot showing FLOATING.

## Docs to update in Jumperless-docs once the behaviour is approved

- 05.7-gpio: "PWM settings go with the slot" is now true on reload (it
  was written before the re-arm existed).
- 06-config: in char mode the `reset`-family commands act on Enter (they
  fired mid-word before).
- MicroPython/ekilo page: ESC no longer discards unsaved edits (Ctrl-Q ×3
  does); unknown key sequences are ignored.

## Release steps (when Kevin says go)

1. Bump VERSION to 5.7.10.1 on dev (no local tag — the workflow tags).
2. Merge dev → main, push main; release.yml builds and creates the release.
3. Touch-flash the released uf2 so the board's on-board version matches.
