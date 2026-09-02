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
