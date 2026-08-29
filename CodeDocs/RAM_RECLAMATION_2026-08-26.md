# RAM Reclamation Pass — 2026-08-26 (env: jumperless_v5, no-PSRAM bench unit)

Plan: `.cursor/plans/ram_waste_reclamation_6322c6b2.plan.md`. Three audit
subagents produced per-symbol verdicts; this ledger records what landed, what
was measured, and what remains.

## Measured result (bench, no-PSRAM V5, Memory Map diagnostic)

| | baseline | final | delta |
|---|---|---|---|
| static (map) | 352.5 KB | 305.7 KB | **−46.8 KB** |
| heap total | 159.5 KB | 206.3 KB | +46.8 KB |
| free heap at idle* | 19.2 KB | 46.5 KB | +27.3 KB |
| MP GC heap | 48 KB (stepped down) | 64 KB (configured) | +16 KB |
| undoInit boot cost | 23,008 B | 14,080 B | −8.9 KB |

*with the 24 KB SharedBuffer AND the full 64 KB MP heap both live. The
SharedBuffer `ensureBuffer()` malloc **succeeds on no-PSRAM units for the
first time** — eKilo can open files again; the memory map borrows instead of
falling back.

MP heap decision (plan's open question): **keep the configured 64 KB.** The
rung-down logic in `Python_Proper.cpp` remains the guard; with ~46 KB idle
free there is no need to pin 48 KB.

## Changes (all build on jumperless_v5 AND jumperless_og)

- `src/SingleCharCommands.cpp` — `z check` bare-part `GuideScript` scratch:
  function-local static (7.8 KB .bss) → per-call `calloc`/`free`.
- `src/selfreflection/Debugs.cpp` — trace ring `kTraceN` 1024→256 on V5
  (~7 KB); pre/post dump window now derived from ring size + static_assert
  (fixed 300/300 was larger than the new ring). `uartReceived` extern 4096.
- `src/Graphics.h/.cpp` — `ROW_ANIMATION_COUNT` 50→40 (~1.2 KB; init writes
  slots 0–37); hardcoded `>= 50` guard now uses the constant.
  `currentSenseOverlayState` moved from header-`static` (one 3.8 KB copy per
  including TU; NTCC's debug path wrote a *different instance* than the
  renderer read) to a single extern instance in Graphics.cpp.
- `src/LEDs.cpp/.h` — logo palette dedup (~2.7 KB): 7 never-read palettes
  deleted (master rows only), rainbow + V5 cold/hot/pink are now aliases into
  `logoColorsAll` rows; OG keeps real cold/hot/pink arrays (its master is
  rainbow-only). `logoColors8vSelect` kept (stored reversed vs master).
- `src/eyecandy/StepViewer.cpp` — `viewerScript`+`viewerScreen` (11 KB .bss)
  → one heap `ViewerAlloc` alive only arm()…disarm(). disarm() aborts an
  in-flight z-check via new `guideCheckUsesScript()`
  (`src/guiding/GuideChecks.cpp/GuideScript.h`) before freeing.
- `src/JumperlessMicroPythonAPI.cpp` — `allPathsBuffer`/`fileBuffer`/
  `overlayBuffer` (12 KB .bss) → lazily-allocated per-VM scratch, freed at
  `deinitMicroPythonProper()` via `jl_bridge_free_scratches()`
  (`src/snakes/Python_Proper.cpp`). Ownership stays C++-side: a wrapper-freed
  malloc would leak on any `mp_obj_new_*` MemoryError (nlr_jump).
- `src/CH446Q.cpp` — `dmaWords`/`dmaCs` 1024→512 words (~2.5 KB; max seen 73
  per send, cap-hit flushes mid-list and continues).
- `src/tubes/AsyncPassthrough.cpp` — `uartReceived` DMA RX ring 8→4 KB
  (~350 ms slack at 115200). `uartReceivedOverflowCount` (printed in both
  UART stats dumps, never incremented) now counts real ring laps via the
  RX-DMA transfer_count delta.
- `src/remembering/Undo.cpp` — no-PSRAM rung halved: 256 ops / 64 txns /
  2 KB blobs (clear-all blob <2 KB still fits; oversize aborts cleanly).

### Attempted and reverted

- `include/custom_tusb_config.h` CDC FIFOs 1024→512 (−4 KB): the bench unit
  dropped off USB entirely minutes after boot on that build (cores running,
  host sees nothing, no crashlog). Reproduced once on the reverted build too,
  so the FIFO size is NOT convicted — but it's a confound and 4 KB isn't
  worth it. Re-attempt only on a known-good board with a soak test.

## Verification status

- Serial-verified on the final build: Memory Map/Usage diagnostics, boot
  ledger, `fs_read`/`get_all_paths`/`overlay_serialize` (values + scratch
  reuse), slot/project context loads, `z steps` error paths (no-source arm,
  unarmed next/off).
- NOT yet verified: StepViewer happy-path arm (needs a guide-carrying
  project open — overwrites the user's run file, deferred), avrdude
  passthrough over the 4 KB ring (no Arduino confirmed on bench), full
  `run_all.py projects` (kept dying from bench contention, below).

## Bench findings (independent of this pass)

- **Bench contention**: a second agent shares this board+probe. Repeated
  spontaneous USB drops with no crashlog and cores running normally match
  external SWD halts/resets, not firmware faults. The HIL suite cannot run
  to a verdict while both agents share the bench.
- **SWD flash recipe** (this bench, RP2350 + Debug Probe, pico-sdk openocd):
  `init; reset halt; mww 0x400a0000 0; mww 0x50000464 0xffff; sleep 20;
  program <elf> verify; reset run` at 4 MHz. Bare `program … verify reset
  exit` fails verify; the plain `pio -t upload` USB path bricked the board
  once (partial image, dark USB; rescue = the recipe above).
- **rowAnimations off-by-one (pre-existing bug, NOT fixed)**: the four tail
  blocks in `initRowAnimations()` stamp `.index` into slot N and write the
  animation data into slot N+1 (the keeper loop correctly uses
  `currentIndex-1`). Actual layout: warning=34, hl-net=35, hl-row=36,
  probe-connect=37 (orphaned above the count), slot 33 zeroed. Consumers
  hardcode 33 (warningNet → renders the zeroed dummy) and 34 (brightenedNet
  → renders the warning animation). Behavior change to fix; work-listed.

## Deferred work-list (from the plan)

- `globalState` deep shrink: `paths[128]` ~17.9 KB and `nets[60]` ~16.8 KB of
  all-`int` fields holding 0–15 values; ~15–20 KB, touches routing core.
- `graphicOverlayState` sparse pixel storage (up to ~8 KB).
- CDC FIFO 512 re-attempt (above).
