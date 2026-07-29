# Filesystem Audit - Status After the 2026-07-28 Fix Session

The full-stack filesystem audit (2026-07-28 morning) produced a handoff list of
verified-but-unfixed bugs. The fix session (same day, evening) resolved
**everything in that list** except the items in "Still open" below. This
document is now the post-fix state: what was fixed (briefly, for archaeology),
what validation exists, and the NEW backlog of smaller bugs discovered while
fixing (each agent was told to report anything suspicious it saw).

Build: `~/.platformio/penv/bin/platformio run -e jumperless_v5`
(the plain `pio` on PATH uses Python 3.14, which the platform rejects).
Firmware builds clean with all fixes.

---

## Fixed this session

### P0 - SPIFTL power-loss data loss (lib/FatFS/lib/SPIFTL, submodule commit 07bfd26)
- **Torn metadata snapshot**: `doLoadHighestEpochMetadata()` now requires a
  complete, gap-free block set (`ceil(payload/4080)` indices) before
  deserializing; `readMetadata8b()` is fail-safe on an empty list (was UB -
  observed as SIGBUS); rejected epochs fall back to the previous intact epoch.
- **Rejected-epoch remnants erased** (found during the fix, beyond the audit):
  CRC-valid blocks of a torn epoch could collide with a reused epoch number
  after fallback; a later boot would gather a MIX of stale+fresh blocks for
  one epoch (per-block CRCs all pass) and load silent garbage.
- **Rollover ring preservation**: `doFullSnapshot()` picks the new ring while
  the old is still marked `ebJournal`, then releases-but-PINS the old blocks
  across `doPersist()`. The pin also covers the metadata-stream allocation
  inside `doPersist()` (an erase path the audit's proposed fix missed).
- Related: `eb >= 0` guards in `FlashInterfaceRP2040` (eb=-1 would erase
  firmware), assert at the metadata alloc site, `setJournal(true)` re-anchors.
- **Validation**: `journaltest.cpp` scenarios J and K sweep a simulated power
  cut across every flash op (~170 points each) of a snapshot persist and a
  ring rollover, verify acknowledged persists survive + in-flight write is
  all-or-nothing + a post-recovery persist/reboot works. Pre-fix: J crashes
  SIGBUS; with only the rollover reorder reverted, K loses 64 persists.
  Run: `cd lib/FatFS/lib/SPIFTL && make journaltest`.

### P0 - USB MSC two-writer corruption cluster (USBfs.cpp, States.cpp)
- (a) Mount detection fires on first host READ10/WRITE10 or PREVENT-ALLOW
  (prevent=1), gated on `mscModeEnabled` so a stray READ10 can't latch it.
- (b) Write refusal while host-mounted, enforced at the chokepoints:
  `safeFileOpen` (write modes), `safeFileWriteAll`, `writeStringToFile`
  (bypasses safeFileOpen - would have been missed), `jl_fs_open_file`
  (raises OSError(EROFS) in Python). Read paths untouched.
- (c) Host READ10/WRITE10 defer with TinyUSB retry-later (`return 0`,
  verified lossless in both vendored driver copies) while firmware holds
  `fs_mutex`. Key subtlety: a plain try-acquire would recursively SUCCEED
  mid-save on the same core (tud_task is pumped from inside FS critical
  sections), so the guard is `fs_mutex_held_by_this_core()` first.
- (d) Both unmount paths do `FatFS.end()/begin()` + `fileCacheDropAll()`
  before the slot reload (skipped with a ponytail note if re-entered from
  inside a firmware FS op; the disable path redoes it quiescently).
- (e) Dirty-slot save moved after `tud_disconnect()` + settle + remount.

### P1 - user-data durability
- `openNodeFile` holds `fs_mutex` (per-core recursive, verified) across the
  whole nodeFile open->read->close; `core2busy` force-clears deleted at all 7
  sites (Commands.cpp x2, FileParsing.cpp x3, USBfs.cpp x2) along with the
  `sendAllPathsCore2` cancellation; `core1busy` pairing restored.
- `ScriptHistory::flushToDisk`: write results checked, temp deleted + `dirty`
  kept on failure, replace only after verified flush.
- config.txt full save: temp-write -> verify (~sticky-f_write argument +
  1KB floor) -> delete+rename. UART-RX IRQ resume leaks fixed (2 sites).
- jfs: module-level `read()` leak fixed via vstr; listdir joins on `'\n'`
  (comma filenames; ALL FOUR parsers - the audit knew of two - including
  `lib/micropython/port/modos_jl.c`); stream-seek ioctl contract fixed.

### P2 / P3 - everything listed in the old doc, highlights
- FileCache: SRAM-budget leak, truncation-detectable `fileCacheReadInto`
  (false + required size), `USE_FILE_CACHE` build flag honored (default OFF),
  temp suffix `.new` -> `.~flc`, canonicalize resolves `./..` + rejects
  overflow (slot aliasing served file A's bytes for file B), flushNow/All use
  the snapshot-clone pattern (no core_sync across ~700ms writes).
- FilesystemStuff: 64KB caps on script/viewer reads, bounded viewer lines,
  `isValidFilename` rejects `/ \ . ..`, directory-not-empty message on `x`,
  ESC-parse wait 2ms -> 25ms polling, listing-truncation indicator,
  `.yaml/.yml` FileType, examples[]/verifier single source (drift of 6 fixed),
  `safeFileReadAll` optional `truncated` out-param (Raw signature unchanged
  for cross-file extern compat).
- FatFS wrapper: MKFS_PARM unscrambled preserving shipped geometry (n_root
  512, au_size auto - verified against f_mkfs), openDir root-file fix (the
  double-f_opendir would have leaked FF_FS_LOCK slots), OM_TRUNCATE honored,
  GET_BLOCK_SIZE=8 sectors, begin() FTL re-toggle latch fixed,
  **FF_FS_LOCK=16** (double-open now fails FR_LOCKED instead of corrupting;
  sized for MAX_JFS_OPEN_FILES=8 + firmware handles).
- jfs layer: ABA-proof opaque handles (slot + 28-bit generation encoded in
  the void*, zero jl_fs_* signature changes), EMFILE via `jl_fs_open_errno()`,
  close blocks 250ms / EBUSY instead of silently leaking, `read(0)`->`''`,
  `read(-1)`->read-all, listdir ENOENT on missing dir, `fs_write` takes a
  length (embedded NULs OK), MAX_JFS_OPEN_FILES 4 -> 8, `jl_fs_name` deleted.
  Bonus fixes: `jl_fs_stat_size` dir-detection, `jfs.stat()` S_IFDIR + no
  slot burn, `write('')` no-ops, debug-print NULL deref.
- PsramArena: calloc overflow check; realloc header read under the arena lock.
- OledGui: raw FatFS calls -> safeFile* helpers (mutex + core pause).
- Python_Proper: `appendEmergencyLog` deleted; history load capped at 64KB.
- States.cpp: 323-line `&& false` file-monitor block deleted (+ its orphaned
  `lastFileModTime` bookkeeping and the misleading "monitoring ACTIVE" log).

---

## Still open (deliberate)

- **FTL write-hole between syncs** (SPIFTL `write()` + `selectBestEB`): a
  block still referenced by the durable L2P can be freed, re-picked, erased,
  and rewritten between two syncs; power loss maps live sectors onto foreign
  flash. Inherent upstream design; the journal shrinks the window. The new
  sweep tests deliberately use workloads that avoid it (noted in comments).
  A fix needs a design change (deferred-erase list keyed on last-persist).
- **TEST UNIT READY blips** (`SlotManager::service()` pulses
  `usbFilesystemBusy` every pass while mounted; hosts read NOT-READY as
  media removal). Dead-block deletion shrank the window to microseconds but
  the design flaw remains. Recommended: once `usbMountedByHost`, report READY
  unconditionally (firmware writes are refused anyway now). Needs hardware
  confirmation (dmesg / USB analyzer).
- **Cable yank while mounted** (new behavior consequence): with mount
  detection now reliable, a yank (no eject) leaves `usbMountedByHost` latched
  and firmware writes refused until USB mode is exited. Arguably correct
  while the drive stays exported; nothing handles `tud_umount_cb`/
  `tud_suspend_cb`. Decide deliberately.
- **Lock-order inversion** (write helpers pause->mutex vs open/close
  mutex->pause): investigated - cannot hard-deadlock (pause is a
  timeout-bounded flag, XIP safety comes from `idleOtherCore` inside the
  flash op), but the current order freezes Core 1 rendering for up to ~2s
  under contention. Recommended: unify on mutex-first, pause-second.
- **refreshListing "journal appends on any f_close" comments overstate**:
  proven that read-mode closes never reach the journal (`f_sync` is fully
  inside `FA_MODIFIED`). Soften comments; the Core-1 parking around directory
  walks is unnecessary-but-harmless.

## New backlog (found while fixing; reported, not fixed)

Durability / correctness:
- `Python_Proper.cpp` deleteScript("history...") prints "Deleting", returns
  true, never deletes the file (only clearHistory() + dirty).
- `Python_Proper.cpp` 4 more unbounded `readString()` slurps (~1317, ~2102,
  ~3067, ~3220).
- `configManager.cpp` MAX_CONFIG_SIZE=3000 too tight (real save ~3KB; one
  long font/startup_message silently degrades every save to full rewrite);
  incremental save's tail-clear writes unchecked but counted as success;
  `provisionEmbeddedFile` uses pauseCore2ForFlash(100) vs savers' 1000.
- `FileCache.cpp` (compiled out): deleted/renamed files resurrect from stale
  `/.bak` mirrors on boot; `writeFullToPathHeld` "r+" in-place overwrite
  contradicts the ABA recovery contract (hybrid content unrecoverable);
  `fileCacheRename` of a dirty never-flushed entry reports failure but
  serves/flushes under the new name; boot scans put ~3.5KB on the 8KB stack.
- `FatFS.h` `~FatFSFileImpl()` on a leaked WRITABLE File does f_sync + full
  CTRL_SYNC journal program with no mutex and no Core-1 pause.
- `JumperlessMicroPythonAPI.cpp` `jl_fs_mkdir` TOCTOU (drops fs_mutex between
  exists-check and mkdir -> EIO instead of EEXIST).
- `modjumperless.c` file-object `seek()` flushes, module-level `jfs.seek()`
  doesn't (read-after-write staleness inconsistency).
- `mphalport.c:53` stale duplicate `mp_obj_jfs_file_t` struct (layout
  landmine if ever used); two independent CWDs (`modos_jl.c` vs `jl_vfs_cwd`).

USB / UX / minor:
- `USBfs.cpp`: unconditional "SCSI command handled" serial spam; START STOP
  "temporary stop" branch unreachable + genuine power-condition stop takes no
  action (its flush can never run); eject does duplicate remount+reload work
  (~1s); `validateAllSlots`/`promptRefreshConnections` raw `FatFS.exists`
  without fs_mutex (the latter from SCSI context); `printf("%u")` on LBA_t
  (UB if FF_LBA64); `onUnplug()` clears `mscModeEnabled` as a registration
  side effect.
- `FilesystemStuff.cpp`: `formatDateTime` uses non-reentrant `localtime`;
  FM operation feedback erased instantly by `drawInterface()`; `[NO_FS]`
  fake entries are selectable and passed to real file ops; `moveSelection`
  pages with a different line count than the renderer; `promptInOutputArea`
  unbounded paste; dead `renameFile`/`copyFile` declarations and
  `writeStringToFileSimple`.
- `Commands.cpp`/`FileParsing.cpp`: unbounded `paths[]` write in
  `parseStringToBridges` (dead path); `removeBridgeFromState` asymmetric
  fake-GPIO update; `refreshLocalConnections` modulo-based tud_task pump.
- `FatFS.h`: `FatFSConfig` setters return by value (chaining silently drops
  settings); `exists("/")` false; `name()` nullptr after close (printf crash
  hazard); `info()` ignores f_getfree result.
- `configManager.cpp:588` unconditional 200ms boot delay annotated
  `//!son of a bitch` - masks an unresolved race worth root-causing.

## Hardware validation checklist (needs a connected V5)

1. Flash, then run `/python_scripts/examples/file_io_basics.py` and
   `pin_irq_basics.py` (assert + print PASS). Section 5 now expects the
   8-handle limit and exercises FF_FS_LOCK read-sharing (4 concurrent reads).
2. `jfs.listdir('/python_scripts')` (reworked openDir + '\n' listdir).
3. USB MSC on macOS + Windows: mount, host-copy a file, firmware-side write
   attempt must refuse; eject; verify slot reload sees host changes; watch
   for TEST UNIT READY remount blips (dmesg).
4. Editor save + config save while MSC mounted -> clean refusal, dirty kept.
5. Dual-core FS stress (Python file I/O loop + FM browsing) - watch for
   FR_LOCKED surfacing anywhere legitimate (FF_FS_LOCK=16 headroom).
