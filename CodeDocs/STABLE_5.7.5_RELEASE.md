# Stable 5.7.5.0 — what this branch is and how to release it

**Branch:** `stable-5.7.5` · **Base:** `c747406` (two commits past the pushed
`5.7.4.1` tag — those two are a CodeDocs note and the examples-generator drift
fix) · **Worktree:** `/Users/kevinsanto/Documents/GitHub/JumperlOS-stable-5.7.5`
· **Dev continues on** `projects-guided-placement`.

## Why a cut, not a strip

Every public tag from 5.7.2.0 through 5.7.4.1 was cut from the dev branch, and
5.7.4.1 sits exactly at the boundary where the guides/projects era begins.
Everything before that boundary — InfraPaths, the scheduler pass, IrqSlots +
FlashPark, CrashLog, the C7 encoder rewrite, the PIO registry, the folder
reorg, USB audio (runtime-off by default), the logic-analyzer removal — is
already in the lineage users run. So the stability release is: **the tree
users already have, plus the fixes made since that were never about guides.**
Nothing user-facing is new except bug fixes. Stripping guides out of dev HEAD
was the alternative; it needed edits in 17 shared files with `States.cpp`
serialization surgery, and would have shipped slots-become-files (a behavior
change) inside a "stability" release. Rejected.

## What's on this branch (13 commits + doc/UF2 past the base)

| commit | what | why it qualifies |
|---|---|---|
| `6f9a746`+`a760408` | clickwheel-hold interrupt double-fire fix | pre-existing encoder/REPL bug |
| `07ce825` | categoryRanges out-of-bounds writes in parseMenuFile | memory corruption, shipped code |
| `82fa65a` | chip D lane names contradicted its own xMap | crossbar correctness |
| `42d7f04` | **heap: 16,896 B of dead MpRemoteService buffers removed** + the boot heap ledger (`heapMark`, printed from the Memory Usage screen) | measured on dev hardware: boot free heap 78,464 → 95,432; MP GC heap reaches its configured 64 KB; largest contiguous block +54% |
| `796c56c` | **encoder has no double-click** — the driver substituted DOUBLECLICKED for the second fast press and every consumer idiom missed it, silently swallowing the second click at ~18 call sites | deterministic input bug; Kevin's standing rule |
| `09cd9dd` | **I2C bit-bang open-drain helpers fixed** (polarity inverted AND never claimed the pad off `GPIO_FUNC_I2C`) — every `machine.I2C.scan()`, SoftI2C, and onewire was broken | measured: 5.6 s of timeouts → 16 ms scan; negative control confirms |
| `5c65251` | CrashLog abort latch: deliberate `abort()`/`assert()` now names its caller in the next boot's crash report, with a bounded live announce | any user OOM/assert crash becomes attributable |
| `08dd34c` | **PSRAM Test menu action lost four `return`s** — on a no-PSRAM board (the shipping config) it null-derefs on the first branch; later branches double-free | user-reachable crash from a diagnostics menu |
| `228a33a` | **SharedBuffer stops allocating in its constructor** — REPL exit `clear()` was permanently claiming 24 KB whether or not any content ever moved | 24 KB of heap back for every session that enters the REPL |
| `cb59367` | VERSION → 5.7.5.0 | the release workflow tags from this file |
| `8abe273` | Probing.h legacy wrapper layer removed — 24 inline shims gone, 71 call lines repointed at the `probing` reference (Kevin's call) | dead API duplication; 10 wrappers had zero callers |
| `8778d1e` | all 23 lazy-`new` singletons become Meyer's local statics | no malloc at static init; **Used Heap AND Total Heap both read ~9.5 KB lower on the memory screen — that's heap→BSS, not a regression** |
| `49c6c0c` | census bug sweep: BitmapEditor (re-entry leak, stale hasHeader, unbounded raw-path alloc capped at 32 KB, `new (std::nothrow)` behind the null checks), DMX begin-failure leak, overlay_set raise-path leak (nlr frame), SlotManager's dead `new[0]`, `chillinColors` → const (2 KB .data to flash) | real leaks and a crash-shaped alloc, all user-reachable |

The census fixes (PSRAM returns, SharedBuffer ctor, the bug sweep, the
refactors) came out of the 2026-08-24 heap hunt; the rest are ports of
dev-branch fixes (`(cherry picked from ...)` trailers name the originals).

## Deliberately NOT here (all continue in dev)

- **Guides/projects entirely**: `src/guiding/`, ProjectsApp, PartPlacement,
  projectFiles provisioning, the four shipped projects, the `z` command, the
  Guides menu row, `parts:`/`guide:`/`guideProgress:` YAML, the Python
  bindings (`load_project`/`place_part`/...). The UX is being rethought
  (CodeDocs/guidesSimplification.md) — nothing half-final ships.
- **Slots-become-files + run files** (`activeSlotPath`, `last_active.txt`,
  `slots.boot_mode`): a real improvement, but a behavior change users would
  notice, and it's entangled with the guides launcher. It is NOT a data
  migration (slot filenames are unchanged), so it merges cleanly later.
- **`4ab0e16` net-scan hardening stages 0–7 + `display.rail_click_adjust`**
  — **KEVIN'S CALL**: it fixed the task-#32 scan-vs-INA disagreement and has
  been in every dev build since, but it was committed pre-bench, changes
  measure-mode behavior, and adds a small user-facing feature. One
  cherry-pick away if wanted: `git cherry-pick 4ab0e16`.
- The vf-tap / INA fine-register / four-wire continuity work, the H1–H3
  GuideChecks batches, the MemoryError harness work — all guides-era.

## Verify before releasing (the gate is Kevin's bench)

Machine-verifiable (done/doable from the worktree — suites are the cut-era
set: micropython_fs, routing, net_currents, config, stress, paste_state,
encoder_ui, infra_paths):

1. `pio run -e jumperless_v5 -e jumperless_og` both green.
2. Flash V5, `python3 test/hil/run_all.py` from THIS worktree (era-matched
   harness; close the Jumperless app first — one host process per CDC port).
3. Memory Usage screen: boot ledger prints; free heap should beat dev's
   95,432 (no projects provisioning here).

Hands-on (nobody but Kevin can):
- **A normal boot at all** — the wrapper removal and the singleton conversion
  touch boot-path code and are compile-verified only; reaching the banner
  and a working probe proves them.
- Two fast clicks anywhere in the menus = two actions (the double-click fix).
- Diagnostics → PSRAM Test on a no-PSRAM board: prints an error, returns to
  the menu, no crash.
- REPL: `import machine; machine.I2C(0, scl=..., sda=...).scan()` with the
  OLED attached finds 0x3C fast (the mphalport fix) — or any I2C module on
  GPIO 7/8 via the crossbar.
- Enter and exit the REPL without transferring a file, then Memory Usage:
  the SharedBuffer 24 KB should NOT be claimed.
- Normal bench pass: probing, measure mode, rails, config save, slot
  load/save, OLED — nothing should feel different from 5.7.4.1.

## Release steps (after the bench pass)

```
git checkout main
git merge stable-5.7.5          # fast-forward-ish: main == merge-base
git push origin main            # CI: bump_version_if_needed sees no 5.7.5.0
                                # tag and releases it as-is
```

Dev keeps moving on `projects-guided-placement`; the two new fixes authored
here (PSRAM returns, SharedBuffer ctor) are mirrored onto dev so the branches
don't fight at the eventual guides merge. When dev finally merges, git sees
the cherry-picked changes already in main and resolves them as no-ops.

## Flash state note for the bench board

The bench board currently runs dev firmware with `slots.boot_mode 1` and an
active 555 run file. This release doesn't read `last_active.txt`, so after
flashing it boots plain slot 0; the run file and all slots stay on disk
untouched. Reflashing the dev UF2 afterward puts the 555 run back as it was.

**Expected on THIS board only:** first RC boot will park `_original` copies
next to some `/examples` files. Dev 5.7.4.3 provisioned them, and its content
hashes aren't in this branch's `hash_history.json`, so provisioning reads them
as user-edited and preserves them. Bench clutter, not a bug — a real user
upgrading from 5.7.4.1 has matching hashes and sees nothing. (Same story as
the i2cscrn `_original` cleanup on 2026-08-23.)
