// SPDX-License-Identifier: MIT
//
// FileCache implementation.

#include "FileCache.h"
#include "PsramArena.h"
#include "FilesystemStuff.h"  // safeFileReadAll/WriteAll backing store
#include "externVars.h"        // core_sync_acquire / fs_mutex_try_acquire, systemIdleForFlush
#include "AsyncPassthrough.h"  // suspendUARTRxIRQ / resumeUARTRxIRQ around flush wrap

#include <Arduino.h>
#include <FatFS.h>
#include <FatFS_LazyPersist.h>  // fatFsForceSync() - SPIFTL metadata sync hook
#include <string.h>
#include "hardware/structs/sio.h"

// Per-area debug flags. fc_debug controls the file-cache + filesystem layer
// trace (FCDBG / FSDBG). psram_debug stays as a master that also enables
// these so a single toggle still turns everything on.
//
// Wrapped in extern "C" {} block (rather than `extern "C" int x = 0`) so
// the linter doesn't see "initialized + extern" on the same line.
extern "C" {
    volatile int fc_debug = 0;
    volatile int fc_atomic_debug = 0;   // atomic-write commit traces
}

// Trace macros - psram_debug works as a master, fc_debug for cache-only.
// NOTE: no Serial.flush() in any of these. fc_lock_got() in particular prints
// while core_sync is HELD; a blocking flush there waits for the USB CDC TX FIFO
// to drain while the lock is held, which on an SRAM-only board (little USB/heap
// headroom) can stall USB servicing long enough to wedge the device under a
// trace flood. Letting the lines drain in the background avoids that; the only
// cost is possibly losing the last few trace lines on a hard crash.
#define FC_TRACE_ON()    (fc_debug || psram_debug)
#define FCDBG(fmt, ...) do { if (FC_TRACE_ON()) { Serial.printf("[%lu c%d] FC> " fmt "\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), ##__VA_ARGS__); } } while(0)
#define FCADBG(fmt, ...) do { if (fc_atomic_debug || fc_debug || psram_debug) { Serial.printf("[%lu c%d] FC.atomic> " fmt "\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), ##__VA_ARGS__); } } while(0)
static inline void fc_lock_trace(const char* where) {
    if (FC_TRACE_ON()) { Serial.printf("[%lu c%d] FC> core_sync acquire @%s\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), where); }
}
static inline void fc_lock_got(const char* where) {
    if (FC_TRACE_ON()) { Serial.printf("[%lu c%d] FC> core_sync acquired @%s\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), where); }
}
static inline void fc_lock_rel(const char* where) {
    if (FC_TRACE_ON()) { Serial.printf("[%lu c%d] FC> core_sync release @%s\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), where); }
}
#define FC_LOCK(where)   do { fc_lock_trace(where); core_sync_acquire(); fc_lock_got(where); } while(0)
#define FC_UNLOCK(where) do { core_sync_release(); fc_lock_rel(where); } while(0)

// Forward decls so we don't have to include configManager.h here
extern struct config jumperlessConfig;
extern bool usbMountedByHost;

// EEPROM deferred-commit hooks (PersistentStuff.cpp). EEPROM.commit() is a
// flash erase+program that shares the single inter-core idleOtherCore() lockout
// with SPIFTL, so committing it synchronously while Core 1 ran was the hardfault
// path. Instead we coalesce a pending EEPROM commit into our Core-1 pause +
// fs_mutex window (eepromCommitHeld), or commit it in its own safe envelope
// from the idle flush tick (eepromCommitSafe) when there's no file work.
extern bool eepromCommitPending(void);
extern bool eepromCommitHeld(void);
extern bool eepromCommitSafe(void);

// Backing-store calls in FilesystemStuff.cpp that bypass the cache. We use
// these to actually touch flash without recursing into the cache.
extern bool safeFileWriteAllRaw(const char* path, const char* content, size_t content_len, uint32_t timeout_ms);
extern bool safeFileReadAllRaw(const char* path, char* buffer, size_t buffer_size, size_t* bytes_read, uint32_t timeout_ms);
extern bool safeFileExistsRaw(const char* path, uint32_t timeout_ms);
extern int32_t safeFileSizeRaw(const char* path, uint32_t timeout_ms);
extern bool safeFileDeleteRaw(const char* path, uint32_t timeout_ms);
extern bool safeFileRenameRaw(const char* pathFrom, const char* pathTo, uint32_t timeout_ms);

// USE_FILE_CACHE comes from FileCache.h (build flag -DUSE_FILE_CACHE=1
// overrides; the header defaults it to 0).

#if USE_FILE_CACHE
// ===========================================================================
// Full write-back PSRAM file cache. Compiled in only when USE_FILE_CACHE != 0.
// (The thin pass-through used when the cache is disabled is in the #else block
// at the very bottom of this file.)
// ===========================================================================

namespace {

// Debounce: a dirty entry waits this long before being flushed unless it
// gets force-flushed. ~1s gives time for follow-up writes to coalesce.
constexpr uint32_t FLUSH_DEBOUNCE_MS = 600;

// Service tick rate - we run roughly every 250ms even when nothing is dirty.
constexpr uint32_t SERVICE_TICK_MS = 250;

// bodyHeap discriminator for free()/psram_free() pairing. Stored per-entry
// because on a non-PSRAM build (or when PSRAM is exhausted) we fall back to
// SRAM malloc and must remember which allocator owns each body.
enum : uint8_t {
    BODY_HEAP_NONE = 0,
    BODY_HEAP_PSRAM = 1,
    BODY_HEAP_SRAM = 2,
};

struct Entry {
    bool used;
    bool dirty;
    bool pinned;            // never evict (e.g. /undo.log, active slot)
    uint8_t bodyHeap;       // BODY_HEAP_* tag for freeEntryBody
    char path[FILE_CACHE_PATH_MAX];
    uint8_t* data;          // body (PSRAM or SRAM allocated)
    size_t size;
    size_t capacity;
    uint32_t lastModifiedMs;
    uint32_t lastAccessMs;
    uint16_t version;
    uint16_t flushedVersion;   // version most recently written to canonical
    uint16_t mirroredVersion;  // version most recently written to /.bak mirror
};

Entry g_entries[FILE_CACHE_MAX_ENTRIES];
bool g_initialized = false;

// Deferred-SPIFTL-metadata-sync accounting. DORMANT under the current
// configuration: SPIFTL runs in delta-journal mode with lazy-persist OFF, so
// each flushEntryChunked's f_close already persists the L2P/peCount metadata
// inline (a cheap ~2 ms journal append) - there is no deferred debt to track,
// and flushEntryChunked no longer increments this counter. The counter and the
// flush-service drain branch are retained because they're the mechanism that
// would be needed again if lazy-persist were re-enabled (then f_close would
// NOT persist metadata and a coalesced fatFsForceSync() during idle would be
// required). With journaling on it simply stays 0 and the drain branch never
// fires.
//
// Volatile so reads from any core see the latest value without the
// compiler caching it across the cooperative service ticks.
volatile uint32_t g_metaDirtyBursts = 0;
// Wall clock at which g_metaDirtyBursts last became non-zero (unused while the
// counter is dormant; kept for the lazy-persist path described above).
volatile uint32_t g_metaDirtySinceMs = 0;
constexpr uint32_t META_BACKSTOP_MS = 60000;

// SRAM fallback cap. We don't want a runaway cache to drain the 60-70 KB
// SRAM free heap, so when PSRAM isn't backing us we hard-limit the total
// bytes lived in SRAM bodies. Two active slot YAMLs (~5 KB each) easily
// fit inside this budget.
constexpr size_t SRAM_FALLBACK_MAX_BYTES = 16 * 1024;
size_t g_sramFallbackBytes = 0;

// Canonicalize `in` into `out`: leading slash ensured, duplicate/trailing
// slashes stripped, '.' segments dropped, '..' segments resolved against
// the parent (clamped at root) - so "/a/../b" and "/b" share one cache
// slot. Returns false when `in` doesn't fit in outCap: callers must treat
// such paths as UNCACHEABLE, because a silently truncated path would
// alias a different file's slot and serve that file's contents.
inline bool canonicalize(const char* in, char* out, size_t outCap) {
    out[0] = '\0';
    if (!in || outCap < 2) return false;
    size_t o = 0;
    out[o++] = '/';
    while (*in) {
        while (*in == '/') in++;
        const char* seg = in;
        while (*in && *in != '/') in++;
        size_t len = (size_t)(in - seg);
        if (len == 0 || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;  // drop last segment
            if (o > 1) o--;                          // and its separating '/'
            continue;
        }
        if (out[o - 1] != '/') {
            if (o >= outCap - 1) { out[o] = '\0'; return false; }
            out[o++] = '/';
        }
        if (o + len > outCap - 1) { out[o] = '\0'; return false; }
        memcpy(out + o, seg, len);
        o += len;
    }
    out[o] = '\0';
    return true;
}

inline bool pathMatch(const Entry& e, const char* canonPath) {
    return e.used && strncmp(e.path, canonPath, FILE_CACHE_PATH_MAX) == 0;
}

Entry* findEntry(const char* canonPath) {
    for (auto& e : g_entries) if (pathMatch(e, canonPath)) return &e;
    return nullptr;
}

Entry* findFreeOrEvictable() {
    // First pass: an unused slot
    for (auto& e : g_entries) if (!e.used) return &e;
    // Second pass: evict the oldest non-pinned, non-dirty entry
    Entry* victim = nullptr;
    uint32_t oldest = UINT32_MAX;
    for (auto& e : g_entries) {
        if (e.pinned || e.dirty) continue;
        if (e.lastAccessMs < oldest) { oldest = e.lastAccessMs; victim = &e; }
    }
    return victim;
}

void freeEntryBody(Entry& e) {
    if (e.data) {
        if (e.bodyHeap == BODY_HEAP_SRAM) {
            free(e.data);
            if (g_sramFallbackBytes >= e.capacity) g_sramFallbackBytes -= e.capacity;
            else g_sramFallbackBytes = 0;
        } else if (e.bodyHeap == BODY_HEAP_PSRAM) {
            psram_free(e.data);
        }
        e.data = nullptr;
    }
    e.bodyHeap = BODY_HEAP_NONE;
    e.size = 0;
    e.capacity = 0;
}

void resetEntry(Entry& e) {
    freeEntryBody(e);
    e.used = false;
    e.dirty = false;
    e.pinned = false;
    e.bodyHeap = BODY_HEAP_NONE;
    e.path[0] = '\0';
    e.lastModifiedMs = 0;
    e.lastAccessMs = 0;
    e.version = 0;
    e.flushedVersion = 0;
    e.mirroredVersion = 0;
}

// Allocate / grow the body buffer. Round capacity up to 1KB granularity.
// Prefers PSRAM when available; falls back to SRAM malloc subject to the
// SRAM_FALLBACK_MAX_BYTES budget so non-PSRAM units don't exhaust the heap.
bool ensureCapacity(Entry& e, size_t needed) {
    if (e.capacity >= needed) return true;
    size_t newCap = (needed + 1023) & ~size_t(1023);
    uint8_t* p = nullptr;
    uint8_t newHeap = BODY_HEAP_NONE;

    // Try PSRAM first - faster path when the mod kit is present and avoids
    // contention with the SRAM heap (used by the editor / Python REPL).
    if (psram_available()) {
        p = (uint8_t*)psram_alloc(newCap);
        if (p) newHeap = BODY_HEAP_PSRAM;
    }

    // Fall back to SRAM, but only if we'd stay under the cap. The existing
    // body's bytes (if SRAM) count toward the cap currently and will be
    // released after the swap, so subtract them from the projected total.
    if (!p) {
        size_t freeingBytes = (e.bodyHeap == BODY_HEAP_SRAM) ? e.capacity : 0;
        if (freeingBytes > g_sramFallbackBytes) freeingBytes = g_sramFallbackBytes;  // drift clamp (matches freeEntryBody); prevents underflow wrap
        size_t projected = (g_sramFallbackBytes - freeingBytes) + newCap;
        if (projected <= SRAM_FALLBACK_MAX_BYTES) {
            p = (uint8_t*)malloc(newCap);
            if (p) {
                newHeap = BODY_HEAP_SRAM;
                g_sramFallbackBytes = projected;
            }
        }
    }

    if (!p) return false;
    if (e.data && e.size > 0) memcpy(p, e.data, e.size);

    // Release the old body (handles both heaps + SRAM accounting).
    if (e.data) {
        if (e.bodyHeap == BODY_HEAP_SRAM) {
            free(e.data);
            if (newHeap == BODY_HEAP_SRAM) {
                // SRAM -> SRAM: accounting already folded in via `projected`.
            } else if (g_sramFallbackBytes >= e.capacity) {
                // SRAM -> PSRAM: the SRAM-fallback branch above never ran,
                // so return the freed body's bytes to the budget here
                // (previously leaked - the 16KB budget shrank permanently).
                g_sramFallbackBytes -= e.capacity;
            } else {
                g_sramFallbackBytes = 0;  // drift clamp (matches freeEntryBody)
            }
        } else if (e.bodyHeap == BODY_HEAP_PSRAM) {
            psram_free(e.data);
        }
    }

    e.data = p;
    e.capacity = newCap;
    e.bodyHeap = newHeap;
    return true;
}

// Streaming write from PSRAM through a small SRAM bounce buffer.
//
// Why: flash erase/program on the RP2350 monopolizes the QMI controller,
// so reads from PSRAM (CS1) during the flash write window stall. We must
// hand FatFS data that's in SRAM. But staging the whole file in SRAM
// risks OOM when the file is large (slot YAMLs can be up to 64KB, the
// SRAM heap typically has 50-70KB free). Bounce-buffering through a
// fixed chunk keeps SRAM usage bounded regardless of file size.
//
// CRITICAL: the bounce buffer MUST be static (BSS), not a local variable
// - the Arduino-Pico Core 0 stack is only 8KB and a 4KB local would blow
// it once IRQs fire during the flash write. flushEntryChunked is only
// ever called from Core 0 (FileCacheFlushService::service or
// fileCacheFlushNow), so single-threaded reuse of one static buffer is
// safe.
//
// All writes go through one safeFileOpen/safeFileClose session so the
// fs_mutex is held continuously and FatFS can do its FAT updates as
// part of the same atomic write.
static constexpr size_t FC_CHUNK = 4096;
static uint8_t g_chunkBounce[FC_CHUNK];

// Suffix for the cache's atomic-write temp files. Deliberately weird
// (".~flc") so the boot-time orphan sweep in scanDirForOrphans can never
// eat a user's hand-made file - the previous ".new" suffix would have
// deleted e.g. a user's "notes.new" at boot. Kept short so suffixed
// max-length paths still fit the FILE_CACHE_PATH_MAX + 8 buffers.
// (Only the orphan scanner uses it today - the current ABA-pair flusher
// writes canonical + /.bak mirror in place and never stages temp files.
// ponytail: orphan ".new" files left by pre-ABA firmware are no longer
// swept; they're harmless stale files the user can delete by hand.)
static constexpr const char* FC_TMP_SUFFIX = ".~flc";

// Hidden mirror directory used by the ABA-pair flusher. Each canonical
// file `/foo/bar.yaml` gets a backup at `/.bak/foo/bar.yaml`. The leading
// dot keeps the mirror hidden in macOS Finder, Linux `ls`, and most
// editors. The mirror is the boot-recovery fallback when the canonical
// file is missing, empty, or unparseable.
static constexpr const char* FC_BAK_ROOT = "/.bak";

// Map `/foo/bar.yaml` -> `/.bak/foo/bar.yaml`. The canonical path must
// start with '/'. Returns false on buffer overflow. Pre-canonicalized
// paths feed in here so we don't have to re-strip dup slashes.
static bool pathToBakMirror(const char* canon, char* out, size_t outSize) {
    if (!canon || canon[0] != '/' || outSize < 2) return false;
    // Reject paths that ALREADY live under the bak root (no double-nesting).
    static constexpr size_t bakRootLen = 5;  // strlen("/.bak")
    if (strncmp(canon, FC_BAK_ROOT, bakRootLen) == 0 &&
        (canon[bakRootLen] == '/' || canon[bakRootLen] == '\0')) {
        return false;
    }
    int n = snprintf(out, outSize, "%s%s", FC_BAK_ROOT, canon);
    return (n > 0 && (size_t)n < outSize);
}

// Build the parent-directory path of `path` into `out`. Returns false on
// overflow or if path has no parent (just "/"). E.g. "/.bak/slots/a.yaml"
// -> "/.bak/slots".
static bool parentDirOf(const char* path, char* out, size_t outSize) {
    if (!path || !out || outSize == 0) return false;
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash || lastSlash == path) return false;
    size_t len = (size_t)(lastSlash - path);
    if (len + 1 > outSize) return false;
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

// Ensure every directory along `path` exists. We assume `path` is a file
// path; the parent (and any ancestor) is created if missing. Called
// rarely - only when we're about to write a `.bak/` mirror for the first
// time after boot. Caller MUST hold fs_mutex - we use FatFS directly so
// the LED indicator doesn't flicker for each mkdir.
static bool ensureParentDirsHeld(const char* path) {
    char parent[FILE_CACHE_PATH_MAX + 16];
    if (!parentDirOf(path, parent, sizeof(parent))) return true;  // root has no parent
    // Walk from root, creating each segment that doesn't exist.
    char buf[FILE_CACHE_PATH_MAX + 16];
    size_t plen = strlen(parent);
    size_t i = 1;  // skip leading '/'
    while (i <= plen) {
        if (i == plen || parent[i] == '/') {
            memcpy(buf, parent, i);
            buf[i] = '\0';
            if (!FatFS.exists(buf)) {
                if (!FatFS.mkdir(buf)) {
                    FCDBG("ensureParentDirsHeld mkdir FAIL %s", buf);
                    return false;
                }
            }
        }
        i++;
    }
    return true;
}

// Helper: write `totalSize` bytes from `src` to `path`. Caller MUST
// already hold fs_mutex and the core-1 frame hold - hence "Held". Uses FatFS
// directly to avoid re-acquiring the mutex.
//
// PERFORMANCE: prefers "r+" mode (open existing for read/write) over
// "w" (truncate-on-open). "w" mode releases ALL clusters back to the
// FAT free pool then allocates new ones - lots of FAT churn. "r+" mode
// keeps the existing clusters allocated and overwrites their contents
// in place; we only touch the FAT if the new size differs from the
// old. On flash with SPIFTL this saves ~1-2 sector-erase cycles per
// save.
//
// Power-loss note: "r+" + seek-0 + write doesn't truncate-then-fill
// the way "w" does. If the new content is SHORTER than the old, the
// tail bytes from the previous save remain until truncate() runs. We
// truncate as the last step (when written_size < old_size); a power
// loss between body write and truncate leaves a file that's
// new-prefix + old-suffix. YAML parsers tolerate trailing garbage
// because the `bridges:` section terminates explicitly.
static bool writeFullToPathHeld(const char* path, const uint8_t* src, size_t totalSize) {
    File file = FatFS.open(path, "r+");
    bool isNewFile = false;
    if (!file) {
        // File doesn't exist - fall back to "w" mode to create it.
        // First-time saves and the recovery-from-mirror copy hit this.
        file = FatFS.open(path, "w");
        isNewFile = true;
        if (!file) {
            FCDBG("writeFullToPathHeld open FAIL %s (tried r+ then w)", path);
            return false;
        }
    } else {
        // Existing file - seek to beginning and overwrite in place.
        if (!file.seek(0)) {
            FCDBG("writeFullToPathHeld seek(0) FAIL %s", path);
            file.close();
            return false;
        }
    }

    size_t oldSize = isNewFile ? 0 : (size_t)file.size();
    size_t remaining = totalSize;
    const uint8_t* p = src;
    bool ok = true;
    while (remaining > 0) {
        size_t cn = remaining < FC_CHUNK ? remaining : FC_CHUNK;
        memcpy(g_chunkBounce, p, cn);
        int written = file.write(g_chunkBounce, cn);
        if (written < 0 || (size_t)written != cn) {
            FCDBG("writeFullToPathHeld write FAIL %s wrote=%d/%u",
                  path, written, (unsigned)cn);
            ok = false;
            break;
        }
        p += cn;
        remaining -= cn;
    }

    // If we shrank the file, truncate the tail. File class exposes
    // truncate() which chops at the current position (which is end-of-
    // -write after the loop above). Only do this if we actually shrank;
    // a same-size or growing rewrite doesn't need it.
    if (ok && !isNewFile && totalSize < oldSize) {
        if (!file.truncate(totalSize)) {
            FCDBG("writeFullToPathHeld truncate FAIL %s newSize=%u oldSize=%u",
                  path, (unsigned)totalSize, (unsigned)oldSize);
            // Not fatal - file is just longer than expected. YAML parser
            // will stop at the bridges: terminator. Log and continue.
        }
    }
    file.close();  // fsyncs in r+ or w mode
    return ok;
}

// Which target(s) a single flushEntryChunked call writes to. The cost
// per call is approximately:
//   WT_CANONICAL_ONLY : ~700 ms freeze (one slot YAML to canonical)
//   WT_MIRROR_ONLY    : ~700 ms freeze (one slot YAML to /.bak mirror)
//   WT_BOTH           : ~1.4 s freeze (both, single pause window)
//
// Routine probe-exit / idle-tick saves use WT_CANONICAL_ONLY so the
// user-visible freeze is minimal. The mirror catches up on the next
// idle tick via the separate "mirror sync" pass in
// FileCacheFlushService::service(). True big events (USB mount,
// shutdown, clear-all) can request WT_BOTH if the caller wants
// belt-and-suspenders durability in one go.
enum WriteTarget : uint8_t {
    WT_CANONICAL_ONLY = 1,
    WT_MIRROR_ONLY    = 2,
    WT_BOTH           = 3,
};

// ABA-pair flusher.
//
// Power-loss safety (when WT_BOTH or canonical+mirror happen in
// reasonably close succession):
//   Step 1: Write the new content directly to canonical <path> in mode "w".
//           Truncates the existing file BEFORE writing the new bytes,
//           so a power loss mid-step leaves <path> empty/partial.
//   Step 2: Write the same content to /.bak/<path>.
//
// Power-loss windows and recovery:
//   - During canonical write : canonical empty/partial. Mirror is the
//                              PREVIOUS-save valid copy. Boot loader
//                              falls back to mirror.
//   - Between writes         : canonical valid. Mirror one save behind.
//                              Boot loader uses canonical.
//   - During mirror write    : canonical valid. Mirror partial. Boot
//                              loader uses canonical; next save retries
//                              mirror.
//   - After both             : both valid. Done.
//
// Worst-case loss with split passes (canonical written, mirror not yet
// caught up, then power dies mid the next canonical write): two saves
// of changes (latest + the one whose mirror hadn't propagated yet).
// In practice the mirror sync runs within ~1s of the canonical so the
// gap is small.
//
// LED-stutter optimization:
//   Each call runs inside ONE pauseCore2ForFlash window so LEDs freeze
//   once per call. The filesystem-activity logo color is set BEFORE
//   the pause so Core 1 paints it and the LEDs hold that color through
//   the freeze.
static bool flushEntryChunked(const char* path, const uint8_t* psramData, size_t totalSize, WriteTarget wt) {
    // Build the mirror path. We tolerate failures here (we'd still
    // write the canonical, just without a backup) but log loudly.
    char bakPath[FILE_CACHE_PATH_MAX + 8];
    bool haveBakPath = pathToBakMirror(path, bakPath, sizeof(bakPath));
    if (!haveBakPath) {
        FCDBG("flushEntryChunked could not build .bak mirror for %s - canonical-only save", path);
    }

    uint32_t flushStartMs = millis();

    // Light up the filesystem-activity LED indicator BEFORE we pause
    // Core 1. Core 1 is the one that actually paints the logo LEDs;
    // once we pauseCore2ForFlash it can't update them. Give Core 1 a
    // brief moment (~2 ms = one render tick) to see the flag and paint
    // the indicator color - after that the LEDs stay frozen on that
    // color for the duration of the pause, so the user can SEE that a
    // save is happening.
    filesystemActive = true;
    filesystemActiveUntil = millis() + 4000;
    __dmb();
    delay(2);

    AsyncPassthrough::suspendUARTRxIRQ();
    uint32_t pauseStartMs = millis();
    bool was_paused = pauseCore2ForFlash(100);
    uint32_t pauseEnteredMs = millis();
    FCDBG("flushEntryChunked pause acquired in %ums",
          (unsigned)(pauseEnteredMs - pauseStartMs));

    bool canonOk = false;
    bool bakOk = false;
    // Track which step "succeeded" for the purposes of the caller's
    // record-keeping. If the caller asked for canonical-only and we
    // didn't touch the mirror, canonOk reflects the canonical write
    // alone. If the caller asked for mirror-only, treat bakOk's status
    // as the overall save outcome.
    bool gotMutex = fs_mutex_acquire_timeout_ms(5000);
    if (!gotMutex) {
        FCDBG("flushEntryChunked: fs_mutex acquire FAILED for %s", path);
    } else {
        // ---- Step 1: write canonical (the read-path source of truth) ----
        if (wt & WT_CANONICAL_ONLY) {
            FCADBG("ABA step 1: write canonical %s (%u bytes)",
                   path, (unsigned)totalSize);
            canonOk = writeFullToPathHeld(path, psramData, totalSize);
        }

        // ---- Step 2: write the .bak mirror ----
        // Always skipped when path is in /.bak itself, or paths aren't
        // long enough for the mirror prefix - both rare/impossible for
        // slot YAMLs. Also skipped if the caller asked WT_CANONICAL_ONLY.
        // If both are requested AND canonical failed, we skip mirror -
        // the canonical write failing means mirror would obscure that.
        bool wantMirror = (wt & WT_MIRROR_ONLY);
        bool canonGate = (wt == WT_MIRROR_ONLY) || canonOk;
        if (wantMirror && haveBakPath && canonGate) {
            if (!ensureParentDirsHeld(bakPath)) {
                FCDBG("flushEntryChunked: cannot create parent for %s", bakPath);
            } else {
                FCADBG("ABA step 2: mirror to %s (%u bytes)",
                       bakPath, (unsigned)totalSize);
                bakOk = writeFullToPathHeld(bakPath, psramData, totalSize);
                if (!bakOk) {
                    FCDBG("flushEntryChunked: mirror write FAILED for %s", bakPath);
                }
            }
        }

        // Coalesce a pending EEPROM commit into this SAME Core-1 pause +
        // fs_mutex window ("save EEPROM when we're also saving files"). Core 1
        // is parked and fs_mutex is held, so EEPROM.commit()'s internal
        // idleOtherCore() can't collide with a SPIFTL flash op.
        if (eepromCommitPending()) {
            FCADBG("coalescing pending EEPROM commit into this flush window");
            eepromCommitHeld();
        }

        fs_mutex_release();
    }

    uint32_t pauseExitMs = millis();
    unpauseCore2ForFlash(was_paused);
    AsyncPassthrough::resumeUARTRxIRQ();
    uint32_t flushElapsed = millis() - flushStartMs;
    uint32_t pauseHeldMs = pauseExitMs - pauseEnteredMs;
    // "Success" depends on which targets were requested.
    bool overallOk;
    switch (wt) {
        case WT_CANONICAL_ONLY: overallOk = canonOk; break;
        case WT_MIRROR_ONLY:    overallOk = bakOk; break;
        case WT_BOTH:           overallOk = canonOk; break;  // canonical is required; bak best-effort
        default:                overallOk = false; break;
    }
    FCDBG("flushEntryChunked %s wt=%d ok=%d (canon=%d bak=%d) elapsed=%ums (Core1 paused for %ums)",
          path, (int)wt, (int)overallOk, (int)canonOk, (int)bakOk,
          (unsigned)flushElapsed, (unsigned)pauseHeldMs);

    // Re-arm the idle gate: treat the just-completed flash op as
    // "activity" so the next flush (e.g. mirror sync after canonical)
    // waits a fresh IDLE_QUIET_MS before firing. Without this, the next
    // FlushService tick fires within ~25ms because lastUserInputMs
    // hasn't moved (Core 1 was frozen). User sees two ~700ms freezes
    // back-to-back with a 25ms gap; with this they're spaced ~750ms+
    // apart and the second only fires if the user is still idle.
    noteUserInput();

    // NOTE: We deliberately do NOT bump g_metaDirtyBursts here anymore.
    // SPIFTL now runs in delta-journal mode with lazy-persist OFF, so the
    // f_close inside the write above already persisted the L2P/peCount
    // metadata to flash (a ~2 ms journal append, logged as
    // "[SPIFTL] persist ... journal-append"). There is no deferred metadata
    // debt to coalesce, so the old "schedule a fatFsForceSync() during the
    // next idle window" path is obsolete - keeping it just produced redundant
    // background metaSync ticks. The explicit fileCacheSpiftlSync(force=true)
    // path (e.g. saveConfig) still works: it calls fatFsForceSync(), which is
    // a cheap no-op when (as now) the metadata is already coherent.
    return overallOk;
}

// Synchronously flush one entry's body to its canonical path WITHOUT
// holding core_sync across the ~700 ms flash write (which starves
// Core 1). Same snapshot-clone pattern as FileCacheFlushService::
// service() - see the use-after-free war story in the comment there:
// clone the body under the lock, release, write the clone, then re-take
// the lock and clear dirty only if the entry still holds the same
// path + version. If a concurrent write/evict/rename changed the entry
// during the unlocked window it stays dirty and a later flush picks up
// the NEW content - a stale snapshot never masks a newer version.
//
// Does NOT update the /.bak mirror - that happens in a separate pass
// via flushEntryMirror() so the user-visible freeze stays at ~700ms
// instead of ~1.4s.
//
// On success, version book-keeping:
//   - flushedVersion <- snapshot version
//   - dirty <- false (only when the path/version re-check passes)
//   - mirroredVersion stays at whatever it was (mirror is now stale by
//     `flushedVersion - mirroredVersion` saves)
bool flushEntrySnapshot(Entry& e) {
    char path[FILE_CACHE_PATH_MAX];
    uint8_t* dataClone = nullptr;
    size_t size = 0;
    uint16_t version = 0;

    FC_LOCK("flushSnap");
    if (!e.used || !e.dirty) { FC_UNLOCK("flushSnap/clean"); return true; }
    memcpy(path, e.path, sizeof(path));
    size = e.size;
    version = e.version;
    if (size > 0 && e.data) {
        // Prefer PSRAM for the clone if available (keeps SRAM heap free).
        if (psram_available()) dataClone = (uint8_t*)psram_alloc(size);
        if (!dataClone) dataClone = (uint8_t*)malloc(size);
        if (dataClone) memcpy(dataClone, e.data, size);
    }
    FC_UNLOCK("flushSnap");

    if (size > 0 && !dataClone) {
        FCDBG("flushSnap clone alloc FAILED for %s (size=%u)", path, (unsigned)size);
        return false;  // entry stays dirty; caller / next tick retries
    }

    FCDBG("flushSnap ENTER path=%s size=%u (canonical-only)", path, (unsigned)size);
    bool ok = flushEntryChunked(path, dataClone, size, WT_CANONICAL_ONLY);
    FCDBG("flushSnap EXIT path=%s ok=%d", path, (int)ok);
    if (dataClone) {
        // Free with the matching allocator. PSRAM allocations live in the
        // 0x11000000-0x12000000 address window; everything else is SRAM.
        uintptr_t addr = reinterpret_cast<uintptr_t>(dataClone);
        if (addr >= 0x11000000u && addr < 0x12000000u) psram_free(dataClone);
        else free(dataClone);
    }

    FC_LOCK("flushSnap/mark");
    // Re-validate the slot didn't get recycled / rewritten during the
    // unlocked window before clearing its dirty flag.
    if (ok && e.used && e.version == version &&
        strncmp(e.path, path, FILE_CACHE_PATH_MAX) == 0) {
        e.dirty = false;
        e.flushedVersion = version;
    }
    FC_UNLOCK("flushSnap/mark");
    return ok;
}

// Drain the SPIFTL lazy-persist debt. Calls fatFsForceSync() inside
// the standard Core1-pause + UART-IRQ-suspend envelope so it doesn't
// race the LED render loop or USB interrupt handlers. Returns true
// on success or no-op (g_metaDirtyBursts was already 0).
//
// Caller MUST verify it's safe to freeze Core 1 (idle gate) before
// calling - this routine intentionally doesn't second-guess the
// caller because the explicit-sync paths (slot switch, USB mount,
// shutdown) want to fire it regardless of idle state.
bool flushSpiftlMetaSync(const char* reason, bool force = false) {
    // The g_metaDirtyBursts counter only tracks writes that went through the
    // PSRAM cache flush path (flushEntryChunked). Direct safeFile*/FatFS writes
    // (e.g. every write on a no-PSRAM unit, or saveConfig's r+ overwrite) leave
    // SPIFTL metadata debt that this counter never sees. `force` callers (the
    // explicit "must be durable now" paths) therefore skip the gate and let
    // SPIFTL's own metadataAge check (inside forceSync()) decide whether there
    // is actually anything to persist - so a forced sync is a cheap no-op when
    // nothing was written, and a real persist when there was.
    if (!force && g_metaDirtyBursts == 0) {
        return true;  // already coherent
    }
    uint32_t bursts = g_metaDirtyBursts;
    uint32_t age = millis() - g_metaDirtySinceMs;
    FCDBG("metaSync ENTER reason=%s bursts=%u age=%ums",
          reason ? reason : "?", (unsigned)bursts, (unsigned)age);

    filesystemActive = true;
    filesystemActiveUntil = millis() + 4000;
    __dmb();
    delay(2);

    AsyncPassthrough::suspendUARTRxIRQ();
    uint32_t pauseStartMs = millis();
    bool was_paused = pauseCore2ForFlash(100);
    uint32_t pauseEnteredMs = millis();

    bool gotMutex = fs_mutex_acquire_timeout_ms(5000);
    bool ok = false;
    if (!gotMutex) {
        FCDBG("metaSync fs_mutex acquire FAILED");
    } else {
        ok = fatFsForceSync();
        fs_mutex_release();
    }

    uint32_t pauseExitMs = millis();
    unpauseCore2ForFlash(was_paused);
    AsyncPassthrough::resumeUARTRxIRQ();

    if (ok) {
        // Clear the debt. If a new write came in between fatFsForceSync()
        // and now, it would have re-armed g_metaDirtySinceMs in the
        // accountant inside flushEntryChunked, but the bursts counter
        // we read at function entry is what we successfully drained.
        // Subtract atomically (within the cooperative service tick)
        // rather than zeroing so concurrent writes aren't lost.
        if (g_metaDirtyBursts >= bursts) {
            g_metaDirtyBursts -= bursts;
        } else {
            g_metaDirtyBursts = 0;
        }
        if (g_metaDirtyBursts == 0) {
            g_metaDirtySinceMs = 0;
        }
    }

    FCDBG("metaSync EXIT ok=%d drained=%u remaining=%u (Core1 paused for %ums, total %ums)",
          (int)ok, (unsigned)bursts, (unsigned)g_metaDirtyBursts,
          (unsigned)(pauseExitMs - pauseEnteredMs),
          (unsigned)(millis() - pauseStartMs));

    // Same idle-gate re-arm trick as flushEntryChunked - prevents the
    // next mirror-sync / canonical-flush from firing back-to-back.
    noteUserInput();
    return ok;
}

// Update the /.bak mirror for an entry if it's stale (flushedVersion >
// mirroredVersion). Called by the deferred mirror-sync pass in the
// flush service when the system is idle. Costs ~700 ms of Core 1
// pause but happens during a real idle window so the user shouldn't
// notice. Returns true iff the mirror is up to date after the call.
bool flushEntryMirror(Entry& e) {
    if (!e.used) return true;
    if (e.mirroredVersion == e.flushedVersion) return true;  // already in sync
    // Need flushed canonical (data may have been written to flash but the
    // in-RAM body is still valid - that's what we mirror). If for some
    // reason data is null, give up gracefully.
    if (!e.data) return true;
    FCDBG("flushEntryMirror ENTER path=%s flushed=%u mirrored=%u",
          e.path, (unsigned)e.flushedVersion, (unsigned)e.mirroredVersion);
    bool ok = flushEntryChunked(e.path, e.data, e.size, WT_MIRROR_ONLY);
    FCDBG("flushEntryMirror EXIT path=%s ok=%d", e.path, (int)ok);
    if (ok) {
        e.mirroredVersion = e.flushedVersion;
    }
    return ok;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

// (Previously: a PSRAM "journal" header sat at the start of the arena
// header region to flag pending writes for warm-boot recovery. Removed
// because PSRAM doesn't survive cold power-off, which is the common
// shutdown path on this hardware - the journal could only ever cover
// watchdog/soft reboots, but those are rare relative to cold yanks. The
// orphan-temp-file recovery scan below is the actual safety net and works
// across cold boots since it reads from flash.)

// Recovery scan: on boot, walk directories that the cache touches and:
//   1. Sweep up any orphan <path>.~flc temp files left by an atomic
//      write that was interrupted by a power loss.
//   2. For each canonical file that's missing or empty, restore it from
//      the /.bak mirror written by the current ABA flusher.
//
// Best-effort: any errors are logged and we continue.

// Copy file `from` -> `to` byte-for-byte using the static SRAM bounce
// buffer. Caller does NOT need to hold any locks (we use the safe
// wrappers). Returns true on success.
static bool copyFile(const char* from, const char* to) {
    File src = safeFileOpen(from, "r", 5000);
    if (!src) {
        FCDBG("copyFile open-read FAIL %s", from);
        return false;
    }
    size_t srcSize = src.size();
    // safeFileOpen holds fs_mutex; we can't open the dest with another
    // safeFileOpen (would self-deadlock). Use FatFS directly under the
    // already-held mutex.
    File dst = FatFS.open(to, "w");
    if (!dst) {
        FCDBG("copyFile open-write FAIL %s", to);
        src.close();
        fs_mutex_release();
        return false;
    }
    bool ok = true;
    size_t remaining = srcSize;
    while (remaining > 0) {
        size_t want = remaining < FC_CHUNK ? remaining : FC_CHUNK;
        int got = src.read(g_chunkBounce, want);
        if (got <= 0) { ok = false; break; }
        int wrote = dst.write(g_chunkBounce, (size_t)got);
        if (wrote != got) { ok = false; break; }
        remaining -= (size_t)got;
    }
    dst.close();
    src.close();
    fs_mutex_release();
    return ok;
}

static void scanDirForOrphans(const char* dirPath) {
    // Phase 1: enumerate candidates under fs_mutex. Keep this read-only -
    // the file mutations happen in Phase 2 with each operation taking its
    // own lock (via the safe* wrappers).
    struct Found { char real[FILE_CACHE_PATH_MAX + 8]; char tmp[FILE_CACHE_PATH_MAX + 8]; };
    constexpr int MAX_CANDIDATES = 16;
    Found cand[MAX_CANDIDATES];
    int ncand = 0;

    if (!fs_mutex_acquire_timeout_ms(2000)) {
        Serial.printf("[FileCache] recover: fs_mutex timeout scanning %s\n", dirPath);
        return;
    }
    {
        Dir dir = FatFS.openDir(dirPath);
        while (dir.next() && ncand < MAX_CANDIDATES) {
            if (dir.isDirectory()) continue;
            String name = dir.fileName();
            if (!name.endsWith(FC_TMP_SUFFIX)) continue;
            String tmpFull;
            if (strcmp(dirPath, "/") == 0) {
                tmpFull = "/" + name;
            } else {
                tmpFull = String(dirPath) + "/" + name;
            }
            String realFull = tmpFull.substring(0, tmpFull.length() - strlen(FC_TMP_SUFFIX));
            if (tmpFull.length() >= sizeof(cand[ncand].tmp)) continue;
            if (realFull.length() >= sizeof(cand[ncand].real)) continue;
            strncpy(cand[ncand].tmp, tmpFull.c_str(), sizeof(cand[ncand].tmp) - 1);
            cand[ncand].tmp[sizeof(cand[ncand].tmp) - 1] = '\0';
            strncpy(cand[ncand].real, realFull.c_str(), sizeof(cand[ncand].real) - 1);
            cand[ncand].real[sizeof(cand[ncand].real) - 1] = '\0';
            ncand++;
        }
    }
    fs_mutex_release();

    // Phase 2: drop orphan temp files. The ABA flusher doesn't create them
    // today, but a future atomic-write flusher (or an interrupted one from
    // another firmware) may leave some behind.
    for (int i = 0; i < ncand; i++) {
        Serial.printf("[FileCache] recover: removing orphan temp %s\n", cand[i].tmp);
        safeFileDeleteRaw(cand[i].tmp, 2000);
    }
}

// Sweep canonical slot files; if any is missing or empty, restore from
// the /.bak mirror. Logs every action.
static void recoverFromBakMirror(const char* dirPath) {
    // Phase 1: enumerate candidate canonical files we should check.
    struct CanonFile { char canon[FILE_CACHE_PATH_MAX + 8]; bool needsRestore; };
    constexpr int MAX_FILES = 16;
    CanonFile files[MAX_FILES];
    int nfiles = 0;

    char bakDir[FILE_CACHE_PATH_MAX + 8];
    if (!pathToBakMirror(dirPath, bakDir, sizeof(bakDir))) return;

    if (!fs_mutex_acquire_timeout_ms(2000)) {
        Serial.printf("[FileCache] recoverFromBak: fs_mutex timeout for %s\n", dirPath);
        return;
    }
    {
        // Look at canonical dir first - record files that are missing or empty.
        Dir dir = FatFS.openDir(dirPath);
        while (dir.next() && nfiles < MAX_FILES) {
            if (dir.isDirectory()) continue;
            String name = dir.fileName();
            // Skip cache temp artifacts (already handled by scanDirForOrphans).
            if (name.endsWith(FC_TMP_SUFFIX)) continue;
            String full;
            if (strcmp(dirPath, "/") == 0) full = "/" + name;
            else full = String(dirPath) + "/" + name;
            if (full.length() >= sizeof(files[nfiles].canon)) continue;
            strncpy(files[nfiles].canon, full.c_str(), sizeof(files[nfiles].canon) - 1);
            files[nfiles].canon[sizeof(files[nfiles].canon) - 1] = '\0';
            files[nfiles].needsRestore = (dir.fileSize() == 0);
            nfiles++;
        }

        // Also pull in files that EXIST in /.bak/<dirPath> but NOT in
        // /<dirPath> - those are saves whose canonical write was lost
        // before any bytes hit disk.
        if (FatFS.exists(bakDir)) {
            Dir bdir = FatFS.openDir(bakDir);
            while (bdir.next() && nfiles < MAX_FILES) {
                if (bdir.isDirectory()) continue;
                String name = bdir.fileName();
                String full;
                if (strcmp(dirPath, "/") == 0) full = "/" + name;
                else full = String(dirPath) + "/" + name;
                // Skip if we already have this canonical path queued.
                bool already = false;
                for (int j = 0; j < nfiles; j++) {
                    if (strcmp(files[j].canon, full.c_str()) == 0) { already = true; break; }
                }
                if (already) continue;
                if (!FatFS.exists(full.c_str())) {
                    if (full.length() >= sizeof(files[nfiles].canon)) continue;
                    strncpy(files[nfiles].canon, full.c_str(), sizeof(files[nfiles].canon) - 1);
                    files[nfiles].canon[sizeof(files[nfiles].canon) - 1] = '\0';
                    files[nfiles].needsRestore = true;
                    nfiles++;
                }
            }
        }
    }
    fs_mutex_release();

    // Phase 2: for each file that needs restoring, copy from the mirror
    // (if the mirror exists). Per-file copy takes its own fs_mutex.
    for (int i = 0; i < nfiles; i++) {
        if (!files[i].needsRestore) continue;
        char bakPath[FILE_CACHE_PATH_MAX + 16];
        if (!pathToBakMirror(files[i].canon, bakPath, sizeof(bakPath))) continue;
        if (!safeFileExistsRaw(bakPath, 1000)) {
            Serial.printf("[FileCache] recoverFromBak: %s is empty/missing and no mirror at %s - leaving alone\n",
                          files[i].canon, bakPath);
            continue;
        }
        int32_t bakSize = safeFileSizeRaw(bakPath, 1000);
        if (bakSize <= 0) {
            Serial.printf("[FileCache] recoverFromBak: %s mirror is empty too - skipping\n",
                          files[i].canon);
            continue;
        }
        Serial.printf("[FileCache] recoverFromBak: restoring %s from %s (%d bytes)\n",
                      files[i].canon, bakPath, (int)bakSize);
        if (!copyFile(bakPath, files[i].canon)) {
            Serial.printf("[FileCache]   copy FAILED - leaving %s as-is\n", files[i].canon);
        }
    }
}

void fileCacheRecoverPendingWrites() {
    // Clean up orphan <path>.~flc temp files from an interrupted
    // atomic write. Once these are gone, future boots see nothing to do.
    scanDirForOrphans("/slots");
    scanDirForOrphans("/");

    // Restore empty/missing canonical files from the /.bak mirror.
    recoverFromBakMirror("/slots");
}

void fileCacheInit() {
    if (g_initialized) return;
    for (auto& e : g_entries) resetEntry(e);
    g_initialized = true;

    // Reconcile any half-finished atomic writes from a previous power
    // cycle. Safe to call here: fs_mutex is up by now (Filesystem init
    // runs before FileCache in main.cpp setup).
    fileCacheRecoverPendingWrites();
}

bool fileCacheRead(const char* path, const uint8_t** outData, size_t* outSize) {
    if (!g_initialized || !path) return false;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return false;  // uncacheable path
    FCDBG("read entry path=%s", cp);
    FC_LOCK("read");
    Entry* e = findEntry(cp);
    bool hit = false;
    if (e && e->data && e->size > 0) {
        e->lastAccessMs = millis();
        if (outData) *outData = e->data;
        if (outSize) *outSize = e->size;
        hit = true;
    }
    FC_UNLOCK("read");
    FCDBG("read exit hit=%d", (int)hit);
    return hit;
}

bool fileCacheReadInto(const char* path, uint8_t* dst, size_t dstSize, size_t* outSize) {
    if (!g_initialized || !path || !dst || dstSize == 0) return false;
    if (outSize) *outSize = 0;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return false;  // uncacheable path
    FCDBG("readInto entry path=%s dstSize=%u", cp, (unsigned)dstSize);
    FC_LOCK("readInto");
    Entry* e = findEntry(cp);
    bool hit = false;
    size_t copied = 0;
    size_t body = 0;
    if (e && e->data && e->size > 0) {
        e->lastAccessMs = millis();
        body = e->size;
        if (body <= dstSize - 1) {
            memcpy(dst, e->data, body);
            copied = body;
            hit = true;
        }
        // else: body doesn't fit - copy NOTHING and miss (see below).
    }
    FC_UNLOCK("readInto");
    if (hit) {
        dst[copied] = '\0';
        if (outSize) *outSize = copied;
    } else if (body > 0) {
        // Truncation is now detectable instead of silent: report the
        // required body size through *outSize and return false so the
        // caller falls back to the raw read (or resizes and retries).
        if (outSize) *outSize = body;
        FCDBG("readInto REFUSED (would truncate): body=%u dst=%u",
              (unsigned)body, (unsigned)(dstSize - 1));
    }
    FCDBG("readInto exit hit=%d copied=%u", (int)hit, (unsigned)copied);
    return hit;
}

bool fileCacheWrite(const char* path, const uint8_t* data, size_t size) {
    if (!g_initialized || !path) return false;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return false;  // uncacheable -> caller raw-writes
    FCDBG("write entry path=%s size=%u", cp, (unsigned)size);
    // ponytail: the cache can't represent an EMPTY body (every read hook
    // treats size==0 as a miss and would fall through to STALE flash), so
    // route empty writes straight to flash via the caller's raw fallback
    // and drop any cached entry so reads follow. Upgrade path: an explicit
    // `empty` flag on Entry + hit-path support.
    if (size == 0) {
        fileCacheInvalidate(cp);
        return false;
    }
    FC_LOCK("write");
    Entry* e = findEntry(cp);
    if (!e) {
        FCDBG("write findFreeOrEvictable");
        e = findFreeOrEvictable();
        if (!e) { FC_UNLOCK("write/full"); FCDBG("write FAIL no slot"); return false; }
        FCDBG("write resetEntry slot=%p", (void*)e);
        resetEntry(*e);
        e->used = true;
        strncpy(e->path, cp, FILE_CACHE_PATH_MAX - 1);
        e->path[FILE_CACHE_PATH_MAX - 1] = '\0';
    }
    // Pin the undo history file so eviction never silently drops it during
    // bursts of slot/script writes - losing it would forfeit cross-reboot
    // undo. Bounded to UNDO_PERSIST_MAX_BYTES (~64 KB) by the serializer.
    // The live name is /undo_history.txt (Undo.cpp UNDO_FILE_PATH); /undo.hist,
    // /undo.snap and /undo.log are legacy names from earlier persistent-history
    // implementations, kept here so any stragglers still pin cleanly.
    if (strcmp(cp, "/undo_history.txt") == 0 ||
        strcmp(cp, "/undo.hist") == 0 ||
        strcmp(cp, "/undo.snap") == 0 || strcmp(cp, "/undo.log") == 0) {
        e->pinned = true;
    }

    // CONTENT DEDUP: if the new bytes are byte-for-byte identical to the
    // cached body, this is a no-op write. SlotManager::service can land
    // here even when no actual user-visible change occurred (state was
    // re-serialized due to internal markDirty() from refresh/route
    // operations). Without dedup, every such call would trigger a
    // canonical flash write later. With dedup, only real content changes
    // bump the version + dirty flag + lastModifiedMs.
    if (e->data && e->size == size && size > 0 &&
        memcmp(e->data, data, size) == 0) {
        e->lastAccessMs = millis();
        FC_UNLOCK("write/dedup");
        FCDBG("write DEDUP no-op (size=%u, ver=%u)", (unsigned)size, (unsigned)e->version);
        return true;
    }

    FCDBG("write ensureCapacity needed=%u currentCap=%u", (unsigned)size, (unsigned)e->capacity);
    if (!ensureCapacity(*e, size)) {
        // Drop the entry rather than keep it. The caller falls back to a
        // raw flash write of the NEW content; a kept entry would either be
        // a phantom (fresh slot with path set but no body - fileCacheExists
        // would report a file that may not exist) or hold the OLD body,
        // whose later flush would clobber that newer flash content.
        resetEntry(*e);
        FC_UNLOCK("write/oom");
        FCDBG("write FAIL ensureCapacity (entry dropped)");
        return false;
    }
    FCDBG("write memcpy");
    memcpy(e->data, data, size);
    e->size = size;
    e->dirty = true;
    e->version++;
    e->lastModifiedMs = millis();
    e->lastAccessMs = e->lastModifiedMs;
    FC_UNLOCK("write");
    FCDBG("write exit OK ver=%u", (unsigned)e->version);
    return true;
}

bool fileCacheInvalidate(const char* path) {
    if (!g_initialized || !path) return false;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return false;  // uncacheable -> no entry possible
    FCDBG("invalidate %s", cp);
    FC_LOCK("invalidate");
    Entry* e = findEntry(cp);
    bool found = false;
    if (e) { resetEntry(*e); found = true; }
    FC_UNLOCK("invalidate");
    return found;
}

bool fileCacheFlushNow(const char* path) {
    if (!g_initialized) return true;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return true;  // uncacheable -> nothing to flush
    FCDBG("flushNow %s", cp);
    FC_LOCK("flushNow");
    Entry* e = findEntry(cp);
    FC_UNLOCK("flushNow");
    if (!e) return true;
    // ponytail: e is re-validated under flushEntrySnapshot's own lock, but in
    // the gap between these two lock windows the slot could be recycled to a
    // DIFFERENT path; we'd then flush that (self-consistent) entry instead,
    // and the requested path - just invalidated - has nothing left to flush.
    // Upgrade path: fold findEntry into flushEntrySnapshot's snapshot lock.
    bool ok = flushEntrySnapshot(*e);
    FCDBG("flushNow %s result=%d", cp, (int)ok);
    return ok;
}

bool fileCacheFlushAll() {
    if (!g_initialized) return true;
    bool allOk = true;
    // One snapshot-clone flush per entry: core_sync is held only for the
    // clone + dirty-mark steps, never across the ~700 ms flash writes
    // (which used to freeze Core 1's LED/input loop for the whole drain).
    for (auto& e : g_entries) {
        if (!flushEntrySnapshot(e)) allOk = false;
    }
    return allOk;
}

// Big-event flush. Caller has decided the user is at a natural pause point
// and a brief LED freeze is acceptable. We don't gate on systemIdleForFlush
// here - the whole reason this exists is to override that gate.
//
// Implementation note: this is currently a thin wrapper around the same
// per-entry path. Future work (see plan) wraps the whole thing under a
// single hoisted pauseCore2ForFlash so the LED freeze is one continuous
// block instead of several short ones.
void fileCacheFlushNowAll(const char* reason) {
    if (!g_initialized) return;
    size_t dirty = fileCacheDirtyCount();
    if (dirty == 0) {
        // Even with nothing dirty in the cache, SPIFTL may still owe a
        // metadata persist from previous flushes that were in lazy mode.
        // Drain it now so the caller's "be durable" guarantee actually
        // holds (e.g. before USB MSC hands the volume to the host).
        fileCacheSpiftlSync(reason ? reason : "flushNowAll-no-dirty");
        // A pending EEPROM commit must also be durable before a USB-MSC /
        // shutdown handoff.
        if (eepromCommitPending()) eepromCommitSafe();
        return;
    }
    FCDBG("flushNowAll reason=%s dirty=%u",
          reason ? reason : "(null)", (unsigned)dirty);
    fileCacheFlushAll();
    // After draining the per-file flushes, also drain SPIFTL metadata
    // so the entire write chain is durable on flash before we return.
    fileCacheSpiftlSync(reason ? reason : "flushNowAll");
    // And commit any still-pending EEPROM (e.g. it wasn't coalesced because no
    // entry happened to flush). No-op if a flushEntryChunked already took it.
    if (eepromCommitPending()) eepromCommitSafe();
    FCDBG("flushNowAll reason=%s done", reason ? reason : "(null)");
}

bool fileCacheSpiftlSync(const char* reason, bool force) {
    if (!g_initialized) return true;
    return flushSpiftlMetaSync(reason, force);
}

void fileCacheDropAll() {
    if (!g_initialized) return;
    core_sync_acquire();
    for (auto& e : g_entries) resetEntry(e);
    core_sync_release();
}

bool fileCacheExists(const char* path) {
    if (!g_initialized || !path) return false;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return false;  // uncacheable -> consult flash
    core_sync_acquire();
    Entry* e = findEntry(cp);
    bool present = (e != nullptr);
    core_sync_release();
    return present;
}

int32_t fileCacheSize(const char* path) {
    if (!g_initialized || !path) return -1;
    char cp[FILE_CACHE_PATH_MAX];
    if (!canonicalize(path, cp, sizeof(cp))) return -1;  // uncacheable -> consult flash
    core_sync_acquire();
    Entry* e = findEntry(cp);
    int32_t sz = (e && e->data) ? (int32_t)e->size : -1;
    core_sync_release();
    return sz;
}

bool fileCacheDelete(const char* path) {
    return fileCacheInvalidate(path);
}

bool fileCacheRename(const char* pathFrom, const char* pathTo) {
    if (!g_initialized || !pathFrom || !pathTo) return false;
    char from[FILE_CACHE_PATH_MAX];
    char to[FILE_CACHE_PATH_MAX];
    if (!canonicalize(pathFrom, from, sizeof(from))) return false;  // no entry possible
    bool toFits = canonicalize(pathTo, to, sizeof(to));
    core_sync_acquire();
    Entry* eFrom = findEntry(from);
    Entry* eTo = toFits ? findEntry(to) : nullptr;
    if (eTo) resetEntry(*eTo);
    if (eFrom) {
        if (toFits) {
            strncpy(eFrom->path, to, FILE_CACHE_PATH_MAX - 1);
            eFrom->path[FILE_CACHE_PATH_MAX - 1] = '\0';
            // dirty stays as-is so a flush will write under the new name
        } else {
            // Destination too long to track: drop the entry rather than
            // leave it flushing to the OLD name, which the caller's raw
            // rename is about to remove from flash.
            resetEntry(*eFrom);
        }
    }
    core_sync_release();
    return true;
}

void fileCacheDumpStatus() {
    if (!g_initialized) { Serial.println("[FileCache] not initialized"); return; }
    core_sync_acquire();
    int used = 0, dirty = 0; size_t bytes = 0;
    for (auto& e : g_entries) {
        if (!e.used) continue;
        used++;
        bytes += e.size;
        if (e.dirty) dirty++;
        const char* heapTag = (e.bodyHeap == BODY_HEAP_PSRAM) ? "P" :
                              (e.bodyHeap == BODY_HEAP_SRAM)  ? "S" : "?";
        Serial.printf("  %c%c %s %5u  %s\n",
            e.dirty ? 'D' : '.',
            e.pinned ? '*' : '.',
            heapTag,
            (unsigned)e.size, e.path);
    }
    Serial.printf("[FileCache] %d/%d used, %d dirty, %u bytes (psram=%s, sram-fallback=%u/%u)\n",
        used, FILE_CACHE_MAX_ENTRIES, dirty, (unsigned)bytes,
        psram_available() ? "on" : "off",
        (unsigned)g_sramFallbackBytes, (unsigned)SRAM_FALLBACK_MAX_BYTES);
    core_sync_release();
}

size_t fileCacheEntryCount() {
    size_t n = 0;
    for (auto& e : g_entries) if (e.used) n++;
    return n;
}

size_t fileCacheDirtyCount() {
    size_t n = 0;
    for (auto& e : g_entries) if (e.used && e.dirty) n++;
    return n;
}

size_t fileCacheBytesInUse() {
    size_t total = 0;
    for (auto& e : g_entries) if (e.used) total += e.size;
    return total;
}

// =============================================================================
// FileCacheFlushService
// =============================================================================

FileCacheFlushService& FileCacheFlushService::getInstance() {
    static FileCacheFlushService inst;
    return inst;
}

FileCacheFlushService& fileCacheFlushService = FileCacheFlushService::getInstance();

ServiceStatus FileCacheFlushService::service() {
    if (!g_initialized) {
        lastStatus = ServiceStatus::IDLE;
        return lastStatus;
    }

    uint32_t now = millis();
    if (now - m_lastTickMs < SERVICE_TICK_MS) {
        lastStatus = ServiceStatus::IDLE;
        return lastStatus;
    }
    m_lastTickMs = now;

    // Three kinds of work this service can do, in priority order:
    //   1. CANONICAL FLUSH: an entry has dirty=true (in-RAM body changed
    //      since last canonical write). Required for durability of the
    //      latest save.
    //   2. MIRROR SYNC: an entry has flushedVersion > mirroredVersion
    //      (canonical is on flash but the /.bak mirror is one or more
    //      saves behind). Reduces worst-case loss window if a future
    //      canonical-write power-loss corrupts the canonical.
    //   3. SPIFTL META SYNC: g_metaDirtyBursts > 0 (one or more flushes
    //      have been done in lazy mode and the SPIFTL L2P/peCount
    //      metadata is dirty in RAM). Without this drain, a power loss
    //      after a save would lose the FTL pointer to the new sectors
    //      even though the data was physically programmed. Cheaper
    //      than canonical flush only in the sense that we coalesce many
    //      saves into one persist - the persist itself is still
    //      ~750 ms on a 4 MB partition.
    //
    // We do at most ONE flash op per tick so the user only sees one
    // ~700 ms Core-1 freeze per tick, never two stacked.
    bool anyDirty = false;
    bool anyMirrorStale = false;
    for (auto& e : g_entries) {
        if (!e.used) continue;
        if (e.dirty) anyDirty = true;
        if (!e.dirty && e.flushedVersion != e.mirroredVersion) anyMirrorStale = true;
    }
    bool spiftlDebt = (g_metaDirtyBursts > 0);
    bool eepromDebt = eepromCommitPending();
    if (!anyDirty && !anyMirrorStale && !spiftlDebt && !eepromDebt) {
        lastStatus = ServiceStatus::IDLE;
        return lastStatus;
    }

    // USB-MSC mounted: the host owns writes. The mount edge already
    // called fileCacheFlushNowAll("usb_mount") to drain whatever was
    // pending. Don't keep writing while the host is plugged in.
    if (usbMountedByHost) {
        lastStatus = ServiceStatus::IDLE;
        return lastStatus;
    }

    // Idle gate: don't flush during probe mode, menus, refresh,
    // editor/REPL/etc, or within IDLE_QUIET_MS of the last user input.
    // Emergency backstop bounds worst-case cold-power-off loss for
    // canonical writes only (mirror staleness isn't urgent enough to
    // override the idle gate - mirror is best-effort). SPIFTL debt also
    // gets a backstop because a power loss with debt outstanding loses
    // every save in the burst.
    constexpr uint32_t IDLE_BACKSTOP_MS = 60000;
    uint32_t oldestDirtyAge = 0;
    for (auto& e : g_entries) {
        if (!e.used || !e.dirty) continue;
        uint32_t age = now - e.lastModifiedMs;
        if (age > oldestDirtyAge) oldestDirtyAge = age;
    }
    uint32_t metaDebtAge = (spiftlDebt && g_metaDirtySinceMs)
        ? (now - g_metaDirtySinceMs) : 0;
    bool idle = systemIdleForFlush();
    bool emergency = (anyDirty && oldestDirtyAge > IDLE_BACKSTOP_MS) ||
                     (spiftlDebt && metaDebtAge > META_BACKSTOP_MS);
    if (!idle && !emergency) {
        lastStatus = ServiceStatus::IDLE;
        return lastStatus;
    }
    if (emergency && !idle) {
        FCDBG("flushService EMERGENCY backstop firing (dirty for %ums, metaDebt %ums)",
              (unsigned)oldestDirtyAge, (unsigned)metaDebtAge);
    }

    // If everything else is clean and only SPIFTL debt remains, drain it
    // this tick. This is the steady-state "user finished a save burst,
    // is now idle, persist the FTL metadata" path.
    if (!anyDirty && !anyMirrorStale && spiftlDebt) {
        FCDBG("flushService spiftl-meta-sync bursts=%u age=%ums",
              (unsigned)g_metaDirtyBursts, (unsigned)metaDebtAge);
        flushSpiftlMetaSync("idle-tick");
        lastStatus = ServiceStatus::BUSY;
        return lastStatus;
    }

    // Lone pending EEPROM commit (no file/mirror/metadata work). Commit it in
    // its own safe envelope (frame hold + fs_mutex) so identity / calibration /
    // debug-flag persistence isn't starved when nothing else is dirty.
    if (!anyDirty && !anyMirrorStale && !spiftlDebt && eepromDebt) {
        FCDBG("flushService lone EEPROM commit");
        eepromCommitSafe();
        lastStatus = ServiceStatus::BUSY;
        return lastStatus;
    }

    // If only mirror is stale (no dirty canonical work), do a mirror
    // sync this tick and return. Cheap path - one ~700 ms freeze with
    // no other work involved.
    if (!anyDirty && anyMirrorStale) {
        Entry* mirrorVictim = nullptr;
        for (auto& e : g_entries) {
            if (e.used && !e.dirty && e.flushedVersion != e.mirroredVersion) {
                mirrorVictim = &e; break;
            }
        }
        if (mirrorVictim) {
            FCDBG("flushService mirror-sync %s flushed=%u mirrored=%u",
                  mirrorVictim->path,
                  (unsigned)mirrorVictim->flushedVersion,
                  (unsigned)mirrorVictim->mirroredVersion);
            flushEntryMirror(*mirrorVictim);
        }
        lastStatus = ServiceStatus::BUSY;
        return lastStatus;
    }

    // IMPORTANT: do NOT pre-acquire fs_mutex here. flushEntryChunked()
    // acquires fs_mutex itself - holding it here would self-deadlock
    // until its 5s timeout.
    //
    // We snapshot path/size/version while holding core_sync, then release it
    // for the actual flash write so Core 2 can keep using core_sync for LED
    // updates.
    //
    // We CLONE the entry body into a private PSRAM buffer under the lock
    // and flush from the clone. Earlier versions just captured a raw
    // pointer to e.data and dropped the lock - if a concurrent
    // fileCacheWrite landed during the unlocked flash-write window and
    // hit ensureCapacity()'s realloc path, it would psram_free() the old
    // body out from under us. The result was a use-after-free that wrote
    // garbage to the slot file. The version-check at the end correctly
    // left the entry dirty for a retry, but the corrupted bytes were
    // already on flash; a reboot in that window meant the next boot saw
    // a corrupted slot file. Cloning isolates us from that race entirely.
    char path[FILE_CACHE_PATH_MAX];
    uint8_t* dataClone = nullptr;
    size_t size = 0;
    uint16_t version = 0;
    Entry* victim = nullptr;

    FC_LOCK("flushService/snap");
    for (auto& e : g_entries) {
        if (!e.used || !e.dirty || (now - e.lastModifiedMs < FLUSH_DEBOUNCE_MS)) continue;
        memcpy(path, e.path, sizeof(path));
        size = e.size;
        version = e.version;
        victim = &e;
        if (size > 0 && e.data) {
            // Prefer PSRAM for the clone if available (keeps SRAM heap free).
            // Fall back to SRAM malloc on non-PSRAM units.
            if (psram_available()) dataClone = (uint8_t*)psram_alloc(size);
            if (!dataClone) dataClone = (uint8_t*)malloc(size);
            if (dataClone) memcpy(dataClone, e.data, size);
        }
        break;
    }
    FC_UNLOCK("flushService/snap");

    if (!victim) {
        lastStatus = ServiceStatus::IDLE;
        return lastStatus;
    }
    if (size > 0 && !dataClone) {
        // PSRAM clone alloc failed (arena full). Bail out this tick and
        // try again next time. Entry stays dirty.
        FCDBG("flushService PSRAM clone alloc failed for %s (size=%u)", path, (unsigned)size);
        lastStatus = ServiceStatus::ERROR;
        return lastStatus;
    }

    FCDBG("flushService -> canonical write %s (%u bytes)", path, (unsigned)size);
    bool ok = flushEntryChunked(path, dataClone, size, WT_CANONICAL_ONLY);
    FCDBG("flushService <- canonical write ok=%d (mirror will sync next tick)", (int)ok);
    if (dataClone) {
        // Free with the matching allocator. PSRAM allocations live in the
        // 0x11000000-0x12000000 address window; everything else is SRAM.
        uintptr_t addr = reinterpret_cast<uintptr_t>(dataClone);
        if (addr >= 0x11000000u && addr < 0x12000000u) psram_free(dataClone);
        else free(dataClone);
    }

    FC_LOCK("flushService/mark");
    // Re-validate the entry slot didn't get recycled to a different path
    // during the unlocked window before we clear its dirty flag.
    if (ok && victim->used && victim->version == version &&
        strncmp(victim->path, path, FILE_CACHE_PATH_MAX) == 0) {
        victim->dirty = false;
        victim->flushedVersion = version;
    }
    FC_UNLOCK("flushService/mark");
    FCDBG("flushService DONE wrote %s", path);

    lastStatus = ServiceStatus::BUSY;
    return lastStatus;
}

#else  // !USE_FILE_CACHE
// ===========================================================================
// Pass-through mode (cache compiled out).
//
// The cache-aware safe* wrappers in FilesystemStuff.cpp already fall back to
// the *Raw FatFS calls when the cache "misses" (read/readInto/write return
// false, exists returns false, size returns -1) or run a Raw op right after a
// coherence call (delete/rename). So the pass-through simply reports "miss"
// for the read/write/exists/size hooks - letting FilesystemStuff do the real
// Raw op with the caller's own timeout - and no-ops the coherence/flush hooks.
// Durability is handled by SPIFTL itself: every f_close -> CTRL_SYNC ->
// persist() appends a journal page, so there is nothing to "flush" here.
// ===========================================================================

#include <FatFS_LazyPersist.h>  // fatFsForceSync()

void fileCacheInit() {
    Serial.println("FileCache: disabled (compile-time pass-through to FatFS)");
}

void fileCacheRecoverPendingWrites() { /* no atomic temp-file commits in pass-through */ }

// Read/write hooks always "miss" so FilesystemStuff falls back to the Raw call
// (which carries the caller's timeout and does the real flash I/O).
bool fileCacheRead(const char*, const uint8_t**, size_t*) { return false; }
bool fileCacheReadInto(const char*, uint8_t*, size_t, size_t* outSize) {
    if (outSize) *outSize = 0;  // plain miss per the header contract
    return false;
}
bool fileCacheWrite(const char*, const uint8_t*, size_t) { return false; }

// Coherence hooks: nothing is cached, so nothing to invalidate/drop.
bool fileCacheInvalidate(const char*) { return false; }
void fileCacheDropAll() {}

// "Already on flash" - flush hooks are no-ops. The big-event/ explicit-sync
// hooks still drive a (cheap, usually no-op) SPIFTL forceSync so metadata is
// coherent at coarse safe points even after direct writes.
bool fileCacheFlushNow(const char*) { return true; }
bool fileCacheFlushAll() { return true; }
void fileCacheFlushNowAll(const char* reason) {
    (void)reason;
    fatFsForceSync();
    if (eepromCommitPending()) eepromCommitSafe();
}
bool fileCacheSpiftlSync(const char* reason, bool force) { (void)reason; (void)force; return fatFsForceSync(); }

// Existence/size "miss" so the wrapper consults flash directly; delete/rename
// are coherence no-ops (the wrapper runs the Raw op right after).
bool fileCacheExists(const char*) { return false; }
int32_t fileCacheSize(const char*) { return -1; }
bool fileCacheDelete(const char*) { return false; }
bool fileCacheRename(const char*, const char*) { return false; }

void fileCacheDumpStatus() {
    Serial.println("FileCache: disabled (compile-time pass-through)");
}

size_t fileCacheEntryCount() { return 0; }
size_t fileCacheDirtyCount() { return 0; }
size_t fileCacheBytesInUse() { return 0; }

// The flush service still exists (jOSmanager registers it) but has no work.
FileCacheFlushService& FileCacheFlushService::getInstance() {
    static FileCacheFlushService instance;
    return instance;
}

ServiceStatus FileCacheFlushService::service() {
    return ServiceStatus::IDLE;
}

FileCacheFlushService& fileCacheFlushService = FileCacheFlushService::getInstance();

#endif  // USE_FILE_CACHE
