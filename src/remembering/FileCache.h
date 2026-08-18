// SPDX-License-Identifier: MIT
//
// Write-back PSRAM file cache.
//
// Sits between callers like safeFileWriteAll/safeFileReadAll and FatFS.
// When PSRAM is available, file writes land in PSRAM-backed entries and a
// background flush worker drains them to flash on a debounce. When PSRAM
// is unavailable, every operation passes straight through to the existing
// safe wrappers.
//
// MicroPython JFS bypasses this cache by design - see plan Phase 2.2. JFS
// callers should call fileCacheInvalidate(path) to keep cache coherent
// when they mutate flash directly.

#ifndef FILE_CACHE_H
#define FILE_CACHE_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Master switch for the PSRAM write-back cache. DEFAULT ON.
//
// The SPIFTL delta-journal made the *metadata* persist cheap (~2 ms per
// f_close), but the underlying FatFS data write for a slot save is still
// ~15-80 ms (open/truncate + sector programs + dir/FAT update). Doing that
// synchronously on every connection edit stalls the UI core and drops button
// presses, so we keep the write-back cache: a save is an instant memcpy into
// PSRAM and the real flash write is coalesced + deferred to the background
// flush service. What the journal DID let us drop is the separate deferred
// "SPIFTL metadata sync" layer - metadata is now persisted inline (cheaply) at
// each flush's f_close, so there is no 750 ms debt to coalesce anymore.
//
// Define USE_FILE_CACHE=0 to compile the cache out entirely (the fileCache*
// API becomes a thin pass-through to the raw safeFile* FatFS calls - see the
// #else block in FileCache.cpp). That removes the PSRAM/SRAM body buffers but
// makes every save a synchronous ~70 ms flash write, so only do it on builds
// that don't care about UI latency.
//
// ! Currently DISABLED (0): the write-back cache made PSRAM boards diverge from
// ! SRAM boards in subtle, hard-to-debug ways (e.g. the undo-history file coming
// ! back with zeroed ops only on PSRAM). Routing every board straight to FatFS
// ! makes PSRAM and SRAM behave identically and keeps flash the single source of
// ! truth. Re-enable only after the cache's PSRAM path is proven coherent.
#ifndef USE_FILE_CACHE
#define USE_FILE_CACHE 0
#endif

#include "JumperlOS.h"  // Service base class

// Maximum number of cache entries (file slots). Each entry tracks a path
// and points to a PSRAM-allocated body buffer.
#ifndef FILE_CACHE_MAX_ENTRIES
#define FILE_CACHE_MAX_ENTRIES 32
#endif

// Maximum path length stored in the cache (after canonicalization).
#define FILE_CACHE_PATH_MAX 64

// Initialize the cache. Safe to call before psram_arena_init() - the cache
// will simply pass-through. Called from main.cpp setup() after the arena.
void fileCacheInit();

// Reconcile orphan <path>.~flc temp files left by an atomic-write commit
// that was interrupted by a power loss. Called from fileCacheInit() but
// also exposed so reset / format paths can re-run it.
void fileCacheRecoverPendingWrites();

// Per-area debug flags. fc_debug enables FileCache + Filesystem trace,
// fc_atomic_debug zooms in on just the atomic-write commit sequence.
// psram_debug (defined in PsramArena.h) acts as a master that also
// enables these.
extern "C" volatile int fc_debug;
extern "C" volatile int fc_atomic_debug;

// Look up a path in the cache, returning the cached body if present.
// Returns true on hit; *outData and *outSize are set to the cached body
// (do NOT free *outData - it's owned by the cache). Returns false on miss.
//
// CAUTION: the returned pointer is only valid until the cache mutex is
// dropped; another core can fileCacheWrite() the same path and realloc
// the body. Prefer fileCacheReadInto() unless you actually need a zero-
// copy view AND can hold off concurrent writers.
bool fileCacheRead(const char* path, const uint8_t** outData, size_t* outSize);

// Cache lookup + copy in one atomic step. The cached body is memcpy'd
// into `dst` (NUL-terminated, so the body must fit in `dstSize - 1`
// bytes) under the cache mutex so concurrent writers can't realloc out
// from under us. Returns true on hit-and-fit. Returns false on miss OR
// when the cached body would not fit: nothing is copied and *outSize is
// set to the required body size (0 on a plain miss) so truncation is
// detectable. Callers fall back to the raw flash read on false - note
// that for a DIRTY oversized entry that fallback serves the older flash
// copy, so size buffers from safeFileSize (which consults the cache).
bool fileCacheReadInto(const char* path, uint8_t* dst, size_t dstSize, size_t* outSize);

// Write content into the cache (write-back). Returns true on success.
// On success the caller's buffer can be freed - we always copy. Marks the
// entry dirty for later flush. Returns false if PSRAM is unavailable or
// the cache is full and we couldn't make room.
bool fileCacheWrite(const char* path, const uint8_t* data, size_t size);

// Invalidate any cached entry for path. Used by JFS / direct-FatFS writers
// to keep the cache coherent. Drops dirty data. Returns true if an entry
// was actually invalidated.
bool fileCacheInvalidate(const char* path);

// Force-flush a single entry to flash (synchronous). Returns true on success
// (or if there was nothing to flush). Used for calibration saves and other
// "must be durable now" paths.
bool fileCacheFlushNow(const char* path);

// Flush every dirty entry. Called on USB MSC mount, shutdown, etc.
bool fileCacheFlushAll();

// "Big-event" flush: drains every dirty entry under one combined Core-1
// pause window. Bypasses the idle gate (callers know the user is at a
// natural pause-point: clear-all, slot switch, USB mount, probe-mode exit,
// graceful shutdown, explicit 's' save). `reason` is logged for debug.
void fileCacheFlushNowAll(const char* reason);

// Force the SPIFTL metadata to be persisted to flash NOW. SPIFTL runs
// in lazy-persist mode (see lib/FatFS/src/FatFS_LazyPersist.h) so each
// successful flushEntryChunked leaves the L2P / peCount / ebState
// metadata dirty in RAM. The flush service drains this debt during
// idle windows; this call drives it explicitly at coarse-grained safe
// points where a longer freeze is acceptable - slot switch, USB MSC
// mount, shutdown, etc. No-op if the metadata is already coherent.
// `reason` is logged for debug.
//
// `force` bypasses the internal dirty-burst gate. That gate only counts writes
// that went through the PSRAM cache flush path, so it is blind to direct
// safeFile*/FatFS writes (every write on a no-PSRAM unit, and saveConfig's r+
// overwrite). Durability-critical callers should pass force=true; it stays
// cheap because SPIFTL's forceSync() itself no-ops when nothing was written.
bool fileCacheSpiftlSync(const char* reason, bool force = false);

// Drop all cached state (does NOT flush). Used by recovery / manual reset.
void fileCacheDropAll();

// Cache-aware versions of the FilesystemStuff safe wrappers. The originals
// in FilesystemStuff.cpp delegate to these so all callers benefit
// transparently.
bool fileCacheExists(const char* path);
int32_t fileCacheSize(const char* path);
bool fileCacheDelete(const char* path);
bool fileCacheRename(const char* pathFrom, const char* pathTo);

// Diagnostic dump (called by `psram_status` debug command).
void fileCacheDumpStatus();

// Stats accessors used by the journal / telemetry.
size_t fileCacheEntryCount();
size_t fileCacheDirtyCount();
size_t fileCacheBytesInUse();

// Service registered with jOSmanager for background flushing.
class FileCacheFlushService : public Service {
public:
    static FileCacheFlushService& getInstance();
    ServiceStatus service() override;
    const char* getName() const override { return "FileCache"; }
    ServicePriority getPriority() const override { return ServicePriority::LOW; }

    FileCacheFlushService(const FileCacheFlushService&) = delete;
    FileCacheFlushService& operator=(const FileCacheFlushService&) = delete;
private:
    FileCacheFlushService() = default;
    uint32_t m_lastTickMs = 0;
};

extern FileCacheFlushService& fileCacheFlushService;

#endif  // FILE_CACHE_H
