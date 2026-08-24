# Heap census — 2026-08-24

Where the ~181 KB heap actually goes on a no-PSRAM shipping board, from the
full-tree allocation sweep that followed the boot-ledger work (dc45e0a). This
is the reference for the next round of heap recovery; the two bugs it found
are already fixed on both branches (f23d07b / 124f30f here, 08dd34c / 228a33a
on stable-5.7.5).

## The headline arithmetic

First Python REPL entry used to permanently claim **~88 KB**: the 64 KB MP GC
heap (load-bearing) plus 24 KB of SharedBuffer that the ctor allocated even
when the touch was only `clear()` on the REPL's exit path. With the ctor fix,
the 24 KB is claimed only when content actually moves (eKilo, file transfer,
the memory-map screen). Resident from `setup()` regardless: undo's 22,752 B
(ops 512×16 + txns 128×40 + 4096 blob + 5344 persist reserve — **all four
pieces verified live**, no dead half; shrinking it is a functionality
tradeoff, Kevin's call).

## Resident allocations (never freed)

| bytes | where | verdict |
|---|---|---|
| 65,536 | Python_Proper.cpp `mpAllocHeap` (64 KB V5 / 28 KB OG) | load-bearing; the file's "96KB/128KB" comment is stale |
| 24,576 | SharedBuffer (lazy, SRAM fallback) | load-bearing once actually used; ctor no longer allocates |
| 22,752 | Undo.cpp `undoInit` | load-bearing, exact breakdown above |
| ~2,700 | CommandBuffer singleton (3×512 cmds + 1024 uart) | load-bearing |
| 2,048 | oled.cpp hold/panel shadow buffers (2×1024) | load-bearing; bad SharedBuffer fit (lives across the hold window) |
| ~1–2K | the 11 jOS service singletons + managers, global-ctor time | load-bearing |
| ~900×2 | ScriptHistory: one heap copy per TermControl AND a function-static twin in Python_Proper.cpp:1341 | duplicate-instance smell — consolidate |
| 8–16 | States.cpp `new JumperlessState[STATE_HISTORY_SIZE]` with SIZE **= 0**; `cleanupHistory()` has zero callers | dead, trivial size — delete when touching the file |

## Transient spikes (freed, but big against a fragmented heap)

- **65,536** States.cpp:3361 slot-YAML **parse-failure recovery** malloc — a
  64 KB single block mid-session will usually FAIL, silently disabling the
  recovery exactly when it's needed. Best SharedBuffer-borrow candidate, or
  chunk it.
- **32,768** Ser3Backchannel GPIO capture (cap n at 6144 → fits SharedBuffer).
- **32,768** ImagesApp bitmap load (comment says 1 KB cap, code says 32 KB).
- **16,384** configManager incremental save (2×8 KB, EVERY background save) —
  the top recurring fragmentation source; SharedBuffer-borrow needs an
  ownership guard against eKilo/REPL.
- toYAML String reserve ~4–24 KB per auto-save; 8–16 KB paste buffers — fine.

## Bugs still open (dev follow-ups, none in the stable release path)

1. **BitmapEditor.loadFile**: `dataSize = fileSize` with NO cap in the raw
   path — a 100 KB file attempts a 100 KB alloc; plus re-entry leaks the
   previous `bitmapData` block.
2. **modjumperless.c overlay_set** leaks its colors buffer if
   `mp_obj_get_int` raises mid-loop (no nlr guard).
3. **Apps.cpp DMX**: `dmxTx.begin()` failure path returns without freeing the
   513 B universe buffer.
4. **oledGui**: 8 undestroyed screens from a script ≈ 25.6 KB; needs a
   ceiling or auto-destroy on interpreter reset.
5. `LEDs.cpp chillinColors[500]` is a non-const global — 2 KB of .data RAM
   that `const` moves to flash.

## Verified non-problems (don't re-chase)

- AsyncPassthrough: zero heap, all BSS.
- USB audio: 2 KB BSS ring, no heap; compiled in on V5, runtime-off default.
- FileCache: entirely compiled out (`USE_FILE_CACHE` 0, no -D flag).
- CrashLog, LEDs, mphalport: no heap.
- MpRemoteService: clean after dc45e0a.
- `psram_alloc` returns nullptr without PSRAM (no internal SRAM fallback);
  every current call site carries its own malloc fallback.
- Jerial mux_stream/term_control/history: properly deleted, no leak.
- The static `_defaultDisplay` never runs `begin()` — no orphaned 1 KB
  framebuffer.
