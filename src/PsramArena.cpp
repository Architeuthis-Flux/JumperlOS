// SPDX-License-Identifier: MIT
//
// PsramArena implementation.
//
// Uses a simple first-fit free-list allocator over a fixed PSRAM region.
// We don't need TLSF complexity - the typical allocation pattern is a
// handful of long-lived blocks (file cache entries, undo rings, scratch
// buffers) plus occasional editor scratch. Fragmentation is bounded by
// keeping the number of active blocks small.
//
// Thread-safety: uses a dedicated arena mutex (NOT core_sync_mutex).
// This is critical because file-cache code holds its own lock and then
// calls psram_alloc - sharing a single mutex would deadlock since Pico
// SDK mutex_t is non-recursive.

#include "PsramArena.h"

#include <Arduino.h>
#include <pico/mutex.h>
#include <string.h>

extern "C" size_t mp_embed_get_psram_size(void);

// Debug trace flag - definition lives here; declarations in PsramArena.h.
// Default OFF; toggle at runtime with the `%` single-char command when
// diagnosing crashes or hangs in the cache/undo system.
extern "C" volatile int psram_debug = 0;

// Per-core ID printed in trace lines. Pico SDK has get_core_num() but we
// avoid the extra include and just compute it from sio_hw->cpuid bit 0.
#include "hardware/structs/sio.h"
static inline int dbg_core() { return (int)(sio_hw->cpuid & 1); }
// No Serial.flush() - psram_debug is the master that also turns on the FS / FC
// / undo traces, so it produces the heaviest flood. A per-line blocking flush
// (some of these fire while the arena lock is held) can stall USB CDC servicing
// and wedge an SRAM-only board. Background drain is safe; we only risk losing
// the last few trace lines on a hard crash.
#define PSDBG(fmt, ...) do { if (psram_debug) { Serial.printf("[%lu c%d] " fmt "\n", (unsigned long)millis(), dbg_core(), ##__VA_ARGS__); } } while(0)

// =============================================================================
// Internal state
// =============================================================================

namespace {

constexpr uintptr_t PSRAM_BASE = 0x11000000;
constexpr size_t MIN_BLOCK_BYTES = 32;       // smaller wastes more on header
constexpr size_t ALIGN = 8;
constexpr uint32_t BLOCK_MAGIC_USED = 0xA5A5A500;
constexpr uint32_t BLOCK_MAGIC_FREE = 0xA5A5A5F0;

// Header for each allocated/free block. Sized to maintain 8-byte alignment.
struct BlockHeader {
    uint32_t magic;     // BLOCK_MAGIC_USED or BLOCK_MAGIC_FREE
    uint32_t size;      // size of this block including header
    BlockHeader* next;  // free list pointer (used only when free)
    uint32_t _pad;      // keep 16-byte alignment
};

// 64-byte arena state struct stored at the very start of the PSRAM region.
// The leading 4096-byte header area sits AFTER this state, so:
//   [ArenaState 64B][header 4096B][allocator pool ...]
struct ArenaState {
    uint64_t magic;          // PSRAM_ARENA_MAGIC
    uint32_t version;
    uint32_t totalSize;      // size of the app arena (excluding any MP region)
    uint32_t headerOffset;   // 64
    uint32_t headerSize;     // PSRAM_ARENA_HEADER_BYTES
    uint32_t poolOffset;     // 64 + headerSize
    uint32_t poolSize;       // totalSize - poolOffset
    uint32_t bytesInUse;     // running count for diagnostics
    uint32_t allocCount;
    uint32_t freeCount;
    uint32_t bootCount;      // increments every boot - tells us cold vs warm
    uint32_t _reserved[3];
};

static_assert(sizeof(ArenaState) == 64, "ArenaState must be 64 bytes");

ArenaState* g_state = nullptr;
BlockHeader* g_freeList = nullptr;
uint8_t* g_pool = nullptr;
size_t g_poolSize = 0;
bool g_available = false;
bool g_warmBoot = false;

uintptr_t g_mpBase = 0;
size_t g_mpSize = 0;

// Dedicated arena mutex - NOT shared with core_sync_mutex. The file cache
// and other consumers acquire their own locks and then call psram_alloc;
// sharing one mutex would deadlock on same-core re-entry (Pico mutex_t is
// non-recursive).
mutex_t g_arenaMutex;
bool g_arenaMutexInited = false;

inline void arenaLock() {
    if (!g_arenaMutexInited) return;
    if (psram_debug) { Serial.printf("[%lu c%d] PSRAM> lock acquire\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1)); }
    mutex_enter_blocking(&g_arenaMutex);
    if (psram_debug) { Serial.printf("[%lu c%d] PSRAM> lock acquired\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1)); }
}
inline void arenaUnlock() {
    if (!g_arenaMutexInited) return;
    mutex_exit(&g_arenaMutex);
    if (psram_debug) { Serial.printf("[%lu c%d] PSRAM> lock release\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1)); }
}

inline size_t alignUp(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

void initEmptyPool() {
    BlockHeader* root = reinterpret_cast<BlockHeader*>(g_pool);
    root->magic = BLOCK_MAGIC_FREE;
    root->size = static_cast<uint32_t>(g_poolSize);
    root->next = nullptr;
    g_freeList = root;
    g_state->bytesInUse = sizeof(BlockHeader);  // header always present
}

// Walk the free list and merge adjacent free blocks. O(n) but n is small.
void coalesce() {
    BlockHeader* prev = nullptr;
    BlockHeader* cur = g_freeList;
    while (cur) {
        BlockHeader* next = cur->next;
        if (!next) break;
        // sort by address as we go - bubble pass per call
        if (reinterpret_cast<uintptr_t>(next) < reinterpret_cast<uintptr_t>(cur)) {
            // swap adjacent nodes: prev -> next -> cur -> (next's old tail).
            // The predecessor (or list head) MUST be re-pointed at `next`,
            // otherwise `next` becomes unreachable and its bytes silently
            // leak out of the free list on every swap.
            cur->next = next->next;
            next->next = cur;
            if (prev) {
                prev->next = next;
            } else {
                g_freeList = next;
            }
            // advance from the head again - cost is bounded
            prev = nullptr;
            cur = g_freeList;
            continue;
        }
        uintptr_t curEnd = reinterpret_cast<uintptr_t>(cur) + cur->size;
        if (curEnd == reinterpret_cast<uintptr_t>(next)) {
            cur->size += next->size;
            cur->next = next->next;
            continue;
        }
        prev = cur;
        cur = next;
    }
}

// Find the first free block large enough. Splits if there's enough leftover
// for another header + MIN_BLOCK_BYTES.
BlockHeader* allocFromPool(size_t bytes) {
    size_t needed = alignUp(bytes + sizeof(BlockHeader), ALIGN);
    if (needed < MIN_BLOCK_BYTES) needed = MIN_BLOCK_BYTES;

    BlockHeader* prev = nullptr;
    BlockHeader* cur = g_freeList;
    while (cur) {
        if (cur->magic != BLOCK_MAGIC_FREE) {
            // free list corruption - bail out
            return nullptr;
        }
        if (cur->size >= needed) {
            // Split if leftover is large enough to be useful
            if (cur->size >= needed + MIN_BLOCK_BYTES) {
                BlockHeader* split = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<uint8_t*>(cur) + needed);
                split->magic = BLOCK_MAGIC_FREE;
                split->size = cur->size - needed;
                split->next = cur->next;

                cur->size = static_cast<uint32_t>(needed);

                if (prev) prev->next = split;
                else g_freeList = split;
            } else {
                // take the whole block
                if (prev) prev->next = cur->next;
                else g_freeList = cur->next;
            }
            cur->magic = BLOCK_MAGIC_USED;
            cur->next = nullptr;
            g_state->bytesInUse += cur->size;
            g_state->allocCount++;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    return nullptr;
}

// Verify the PSRAM window is backed by real, coherent memory before we
// trust any of it. Some boards report a PSRAM size (rp2040.getPSRAMSize())
// even when the chip is absent, unpopulated, or dead - in that case writes
// to the 0x11xxxxxx window are silently dropped and reads return flash/ROM
// alias garbage. Anything we place there (eKilo's file buffer, the file
// cache, the undo log) gets silently corrupted: files open as garbled text
// and "buffer overflow" appears on save.
//
// We probe several addresses spread across the chip - including the first
// and last byte - writing a distinct value to each. Distinct values mean an
// aliasing window (the same physical byte mapped repeatedly) also fails the
// check, not just a fully dead bus. Original bytes are saved and restored so
// a genuine warm-boot's arena state survives the test.
bool psramSelfTest(size_t chipBytes) {
    if (chipBytes < 64) return false;

    volatile uint8_t* mem = reinterpret_cast<volatile uint8_t*>(PSRAM_BASE);
    constexpr int N = 5;
    const size_t off[N] = {
        0,
        chipBytes / 4,
        chipBytes / 2,
        (chipBytes / 4) * 3,
        chipBytes - 1,
    };

    uint8_t saved[N];
    for (int i = 0; i < N; i++) saved[i] = mem[off[i]];

    for (int i = 0; i < N; i++) mem[off[i]] = static_cast<uint8_t>(0xA5 ^ (i * 37 + 1));
    __asm volatile("" ::: "memory");  // prevent the write/read pair being elided

    bool ok = true;
    for (int i = 0; i < N; i++) {
        if (mem[off[i]] != static_cast<uint8_t>(0xA5 ^ (i * 37 + 1))) {
            ok = false;
            break;
        }
    }

    for (int i = 0; i < N; i++) mem[off[i]] = saved[i];
    return ok;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

extern "C" {

bool psram_pattern_test(size_t chipBytes) {
    return psramSelfTest(chipBytes);
}

bool psram_arena_init(size_t detectedPsramBytes, int appSizeKb) {
    if (!g_arenaMutexInited) {
        mutex_init(&g_arenaMutex);
        g_arenaMutexInited = true;
    }
    g_available = false;
    g_warmBoot = false;
    g_state = nullptr;
    g_freeList = nullptr;
    g_pool = nullptr;
    g_poolSize = 0;
    g_mpBase = 0;
    g_mpSize = 0;

    if (detectedPsramBytes == 0 || appSizeKb <= 0) {
        return false;
    }

    // Confirm the reported PSRAM is actually present and working. If the
    // window isn't backed by real RAM, refuse the arena entirely so callers
    // (psram_alloc) return null and fall back to the SRAM heap instead of
    // handing out dead pointers.
    if (!psramSelfTest(detectedPsramBytes)) {
        Serial.println("[PSRAM] self-test failed - window not backed by working RAM; disabling app arena");
        return false;  // g_available stays false
    }

    // Clamp app region to a reasonable fraction of total PSRAM.
    // We must always leave at least 1MB for MicroPython.
    size_t requested = static_cast<size_t>(appSizeKb) * 1024;
    size_t maxApp = detectedPsramBytes - (1024 * 1024);
    if (requested > maxApp) requested = maxApp;

    // Round down to 4KB boundary
    requested &= ~size_t(0xFFF);

    // Minimum useful arena: ArenaState + header + at least 16KB pool
    constexpr size_t MIN_ARENA = 64 + PSRAM_ARENA_HEADER_BYTES + 16 * 1024;
    if (requested < MIN_ARENA) {
        return false;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(PSRAM_BASE);
    g_state = reinterpret_cast<ArenaState*>(base);

    bool warmBoot = (g_state->magic == PSRAM_ARENA_MAGIC) &&
                    (g_state->totalSize == requested) &&
                    (g_state->headerSize == PSRAM_ARENA_HEADER_BYTES);

    if (warmBoot) {
        // Trust the existing state. Walk free list to validate.
        g_pool = base + g_state->poolOffset;
        g_poolSize = g_state->poolSize;
        // Reconstruct free list pointer from first free block scan.
        // For simplicity we just rebuild the pool fresh on warm boot too -
        // anything important must already be in the reserved header area or
        // in known-offset structures. This trades warm-boot allocation
        // recovery for simplicity - the file-cache journal lives in the
        // header area which is stable.
        g_warmBoot = true;
        g_state->bootCount++;
        initEmptyPool();
    } else {
        // Cold init
        memset(g_state, 0, sizeof(ArenaState));
        g_state->magic = PSRAM_ARENA_MAGIC;
        g_state->version = 1;
        g_state->totalSize = static_cast<uint32_t>(requested);
        g_state->headerOffset = sizeof(ArenaState);
        g_state->headerSize = PSRAM_ARENA_HEADER_BYTES;
        g_state->poolOffset = sizeof(ArenaState) + PSRAM_ARENA_HEADER_BYTES;
        g_state->poolSize = static_cast<uint32_t>(requested - g_state->poolOffset);
        g_state->bootCount = 1;
        g_pool = base + g_state->poolOffset;
        g_poolSize = g_state->poolSize;
        // Zero the header so journal code sees a clean slate
        memset(base + g_state->headerOffset, 0, PSRAM_ARENA_HEADER_BYTES);
        initEmptyPool();
    }

    // Compute MP sub-region: everything past the app arena
    g_mpBase = PSRAM_BASE + requested;
    g_mpSize = detectedPsramBytes - requested;

    g_available = true;
    return true;
}

bool psram_available(void) { return g_available; }
bool psram_warm_boot(void) { return g_warmBoot; }

void* psram_alloc(size_t bytes) {
    if (!g_available || bytes == 0) return nullptr;

    PSDBG("PSRAM> alloc(%u) entry", (unsigned)bytes);
    arenaLock();
    BlockHeader* h = allocFromPool(bytes);
    arenaUnlock();
    PSDBG("PSRAM> alloc(%u) -> %p", (unsigned)bytes, h ? (void*)((uint8_t*)h + sizeof(BlockHeader)) : nullptr);

    if (!h) return nullptr;
    return reinterpret_cast<uint8_t*>(h) + sizeof(BlockHeader);
}

void* psram_calloc(size_t n, size_t bytes) {
    size_t total = n * bytes;
    void* p = psram_alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void psram_free(void* p) {
    if (!p || !g_available) return;
    BlockHeader* h = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(p) - sizeof(BlockHeader));

    PSDBG("PSRAM> free(%p)", p);
    arenaLock();
    if (h->magic != BLOCK_MAGIC_USED) {
        arenaUnlock();
        return;
    }
    h->magic = BLOCK_MAGIC_FREE;
    if (g_state->bytesInUse >= h->size) g_state->bytesInUse -= h->size;
    h->next = g_freeList;
    g_freeList = h;
    g_state->freeCount++;
    coalesce();
    arenaUnlock();
}

void* psram_realloc(void* p, size_t bytes) {
    if (!p) return psram_alloc(bytes);
    if (bytes == 0) { psram_free(p); return nullptr; }

    BlockHeader* h = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(p) - sizeof(BlockHeader));
    if (h->magic != BLOCK_MAGIC_USED) return nullptr;

    size_t oldUserSize = h->size - sizeof(BlockHeader);
    if (bytes <= oldUserSize) return p;  // shrink in place

    void* np = psram_alloc(bytes);
    if (!np) return nullptr;
    memcpy(np, p, oldUserSize);
    psram_free(p);
    return np;
}

size_t psram_app_free(void) {
    if (!g_available) return 0;
    size_t total = g_state->poolSize;
    size_t used = g_state->bytesInUse;
    if (used > total) return 0;
    return total - used;
}

size_t psram_app_total(void) {
    if (!g_available) return 0;
    return g_state->totalSize;
}

uintptr_t psram_mp_base(void) { return g_mpBase; }
size_t psram_mp_size(void) { return g_mpSize; }

void* psram_header_ptr(void) {
    if (!g_available) return nullptr;
    return reinterpret_cast<uint8_t*>(PSRAM_BASE) + g_state->headerOffset;
}

uintptr_t psram_pool_base(void) {
    return g_available ? reinterpret_cast<uintptr_t>(g_pool) : 0;
}

size_t psram_pool_size(void) {
    return g_available ? g_poolSize : 0;
}

size_t psram_arena_walk(psram_block_visitor cb, void* ctx) {
    if (!g_available || !g_pool) return 0;

    arenaLock();
    size_t count = 0;
    uint8_t* p = g_pool;
    uint8_t* endp = g_pool + g_poolSize;
    // Linear walk: every byte of the pool is covered by exactly one block
    // header chain (alloc/free keep blocks contiguous), so advancing by
    // h->size visits both used and free blocks in address order.
    while (p + sizeof(BlockHeader) <= endp) {
        BlockHeader* h = reinterpret_cast<BlockHeader*>(p);
        // Bail on corruption rather than spin forever.
        if (h->magic != BLOCK_MAGIC_USED && h->magic != BLOCK_MAGIC_FREE) break;
        if (h->size < sizeof(BlockHeader)) break;
        if (p + h->size > endp) break;
        int used = (h->magic == BLOCK_MAGIC_USED) ? 1 : 0;
        if (cb) cb(ctx, reinterpret_cast<uintptr_t>(p), h->size, used);
        count++;
        p += h->size;
    }
    arenaUnlock();
    return count;
}

void psram_arena_dump_status(void) {
    if (!g_available) {
        Serial.println("[PSRAM] arena not available (no PSRAM detected)");
        return;
    }
    Serial.printf("[PSRAM] arena %u KB at 0x%08X\n",
        (unsigned)(g_state->totalSize / 1024), (unsigned)PSRAM_BASE);
    Serial.printf("        pool   : %u KB free of %u KB\n",
        (unsigned)(psram_app_free() / 1024),
        (unsigned)(g_state->poolSize / 1024));
    Serial.printf("        in-use : %u bytes (%u allocs, %u frees)\n",
        (unsigned)g_state->bytesInUse,
        (unsigned)g_state->allocCount,
        (unsigned)g_state->freeCount);
    Serial.printf("        boots  : %u  warm-boot=%d\n",
        (unsigned)g_state->bootCount, g_warmBoot ? 1 : 0);
    Serial.printf("        MP region: %u KB at 0x%08lX\n",
        (unsigned)(g_mpSize / 1024), (unsigned long)g_mpBase);
    Serial.printf("        MP GC actually using: %u KB\n",
        (unsigned)(mp_embed_get_psram_size() / 1024));
}

}  // extern "C"
