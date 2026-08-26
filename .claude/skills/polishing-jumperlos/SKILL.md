---
name: polishing-jumperlos
description: Use when running a stability, latency, or coherence pass over JumperlOS — pre-release audits, verifying a previous session's fixes, chasing input-to-feedback response time, hunting cross-surface (clickwheel/probe/OLED/LEDs/terminal) incoherence, or extending the OS to new hardware like Jumperless V6. Also use before flashing or driving the bench board over serial.
---

# Polishing JumperlOS

## Overview

A polish pass makes the Jumperless feel like an extension of the user's mind:
instant, predictable, hints not instructions. It adds NO functionality. Old
code is solidified without behavior changes; new "app-like" code may change
behavior to get smoother. Response time from any user action to visible
feedback (LEDs, OLED, terminal) is a first-class goal.

**Old vs new is by feature area, not file age:** `PartsApp.cpp`,
`ProjectsApp.cpp`, `src/guiding/`, `src/partdb/`, `src/displays/` (the
ambient-parts stack) are app-like. Everything else — and any shared helper an
old surface calls — counts as old: crash/bounds/stall removal only.

**Verify before fixing. Measure before optimizing. Restore the bench after
touching it.**

## The pass, in order

1. **Start the baseline build in the background, then read while it runs.**
   Both targets (see Build below). A fix you can't compile at 4am is a fix
   you don't have.
2. **Recover prior state.** Read the newest `CodeDocs/*AUDIT*`, `*VERDICTS*`,
   `*FIXCHECK*`, `*LATENCY*` ledgers. Prior multi-agent runs leave
   `journal.jsonl` files under
   `~/.claude/projects/-Users-kevinsanto-Documents-GitHub-JumperlOS/<session>/subagents/workflows/wf_*/`
   — one JSON line per agent result; the matching `agent-<id>.jsonl`
   transcripts in the same dir hold the prompts, which is how you map a
   verdict back to its finding. Findings with no verdict are the
   highest-yield work-list.
3. **Verify claims against the CURRENT tree.** A "checker" here is a
   subagent given the original bug description whose job is to attack, not
   confirm: search by symbol (line numbers rot), then try (a) sibling call
   paths, (b) the other board target, (c) early-exit/error paths that skip
   the guard, (d) callers the fix forgot. Inherited unverified findings get
   refute-biased verification (default: the claim is stale). Calibration
   from real passes, not a gate: ~20% of inherited claims die, ~half of
   "complete" fixes have an open sibling.
4. **Measure latency with arithmetic, not vibes.** Trace action→feedback hop
   by hop: poll cadences, debounce windows, I2C byte math, render budgets —
   the constants live in the code (`RotaryEncoder.cpp`, `oled.cpp` init,
   `main.cpp` core-1 loop) and in `CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md`
   (measured mailbox timings). A finding without a computed dominant term is
   not actionable.
5. **Fix in file-exclusive batches.** One agent per file group, never two in
   one file — investigation agents count as owners of files they might edit.
   Defer anything needs-bench or design-shaped to the work-list.
6. **Build both targets, commit per batch, never push.** Stay on `dev`.
   Stage files explicitly — never `git add -A`: `.pio/**/firmware.uf2` is
   tracked and every build dirties it; it belongs only in a release commit.
7. **HIL-verify on the bench** (see below), then write this pass's ledgers
   as `CodeDocs/<TYPE>_<YYYY-MM-DD>.md` (types: FIXCHECK, PENDING_VERDICTS,
   LATENCY_AUDIT, or a pass overview) — written incrementally as each stage
   lands, so a dead session still leaves findings on disk.

## Bench discipline (the failure modes that actually happened)

- **Ports:** `/dev/cu.usbmodemJLV5port1` = terminal (single-char commands),
  `/dev/cu.usbmodemJLV5port5` = MicroPython raw REPL. `test/hil/jl.py`
  wraps both and discovers them itself. One host process per port — the
  Jumperless desktop app must be closed.
- **The suite is `python3 test/hil/run_all.py`** (optional substring arg
  runs matching files, e.g. `run_all.py projects`). Full run ≈ 15–40 min.
  PASS = the final `HIL suite: PASS` line; a SKIP is not a pass. Run it
  detached with a full log — `nohup python3 test/hil/run_all.py > log 2>&1 &`
  — never through a piped `tail` or a `timeout`: the runner restores the
  user's bench state only if it survives to the end (`jl_exec` sys.exits on
  REPL failure).
- **If a run dies:** the board is likely stranded in a fixture context or an
  interactive script. Recover: write `b'\x03'` (Ctrl-C) to port 1; then in
  `test/hil`, `jl.active_context()` to see where it is, read the on-device
  file `/slots/last_active.txt` (via `jl.device_text`) for the user's real
  context, `jl.restore_context(slot, path)` back (slot number, or `-1` +
  path for file contexts), `jl.reboot_board()` for a clean heap, and
  re-check `jl.active_context()` matches. Report all of it in the summary.
- **Capture before you touch:** for manual driving outside the runner, take
  `jl.board_state_capture()` first — you can only restore what you captured.
- **Confirm the board's firmware first** (`?` on port 1 prints
  `Jumperless firmware version: …`) so a suite/firmware mismatch isn't
  chased as a bug.
- Distinguish **serial-verified** from **bench-verified**: label blooms,
  OLED rendering, and LED flicker need human eyes; say which you did.

## Build

`pio` on this machine is broken under system Python; use:
```
python3.13 -m venv "$SCRATCH/pio313"
"$SCRATCH/pio313/bin/pip" -q install platformio==6.1.19
"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og
```
(`$SCRATCH` = the session scratchpad dir; the venv is disposable.) Both
targets compile nearly all of `src/` — every fix must be right for V5
(RP2350) *and* OG (RP2040). `NetsToChipConnections.cpp` has separate V5/OG
copies: fix both. Host-side board tests: `pio test` env in `test/test_boards/`.

## Extending to new hardware (the V6 paradigm)

Gate features on **capabilities, not board names**: add a `BoardCaps` field
(`src/boards/board.h`) named for what the hardware does (never `isV6`), set
it in every board descriptor (they're positional aggregate initializers —
set all of them), and check it at the service entry *and* the hardware
acquire (the `caps.breadboardDisplays` double gate in
`src/displays/DisplayService.cpp` + `src/displays/DisplayBus.cpp` is the
model — ungated on OG those pins are the CH446Q RESET and WS2812 data
lines: an ungated feature doesn't no-op, it drives real lines). Add the cap
to `boardCapabilitiesJson` (`src/boards/board.cpp`) and a
`test/test_boards/test_boards.cpp` assertion so initializer drift can't
silently flip it. Unification targets for a coherent V6 story live in
`CodeDocs/LATENCY_AUDIT_*.md` under "Unification proposals".

## Red flags

| Thought | Reality |
|---|---|
| "The audit doc says it was fixed" | Check the current tree; fixes get half-landed and later commits move code. |
| "This checker says MISSING — apply the fix" | Checkers misread too; read the code before re-fixing. |
| "It feels slow" | Compute the path. No arithmetic, no fix. |
| "Old code, obvious improvement" | Behavior change on an old surface is out of scope; work-list it. |
| "The suite will restore the bench" | Only if it survives. Detach it, log it fully, verify restore. |
| "One more batch before building" | Build first; 16 s of compile beats an hour of bisecting. |
| "git add -A and commit the batch" | The tracked firmware.uf2 just went in. Stage files explicitly. |
