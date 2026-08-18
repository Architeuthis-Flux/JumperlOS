// SPDX-License-Identifier: MIT
//
// PsramArena - dedicated app-side allocator over a configurable region
// of the optional 8MB PSRAM mod kit.
//
// MicroPython's gc_add() claims whatever PSRAM we don't reserve here.
// All callers must handle a nullptr return by falling back to malloc()
// or their own SRAM-only path.
//
// Layout (when PSRAM available):
//   0x11000000 .. 0x11000000 + appSize        -> App arena (this allocator)
//   0x11000000 + appSize .. 0x11000000 + chip -> MicroPython GC heap
//
// The first PSRAM_ARENA_HEADER_BYTES of the app region are reserved
// for the file-cache journal / undo header (see FileCache.cpp).

#ifndef PSRAM_ARENA_H
#define PSRAM_ARENA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Magic value at offset 0 of the arena - used to detect a warm boot
// where PSRAM contents survived (e.g. watchdog reset, software reboot).
#define PSRAM_ARENA_MAGIC 0x4A4F535053524100ULL  // "JOSPSRA\0"

// Bytes reserved at the start of the arena for headers (journal/undo log
// metadata that wants to be at a known location).
#define PSRAM_ARENA_HEADER_BYTES 4096

// Initialize the arena. Called once from setup() after loadConfig().
// detectedPsramBytes is the chip size (0 if no PSRAM); appSizeKb is the
// configured app region size in KiB. Returns true if the app arena was
// successfully reserved.
bool psram_arena_init(size_t detectedPsramBytes, int appSizeKb);

// Returns true if PSRAM is present and the arena was initialized.
bool psram_available(void);

// Whether the arena retained valid contents from a previous boot.
// Set by psram_arena_init() based on the magic value.
bool psram_warm_boot(void);

// Allocate from the app arena. Returns nullptr if PSRAM unavailable or
// the arena is full. Allocations are 8-byte aligned.
void* psram_alloc(size_t bytes);

// Same as psram_alloc but zero-initialized.
void* psram_calloc(size_t n, size_t bytes);

// Resize. May return a new pointer (data is copied). Returns nullptr on
// failure (caller's old pointer is still valid in that case).
void* psram_realloc(void* p, size_t bytes);

// Free a previously psram_alloc'd block. No-op if p is nullptr.
void psram_free(void* p);

// Free bytes available in the arena (excludes the reserved header).
size_t psram_app_free(void);

// Total size of the app arena (including header bytes).
size_t psram_app_total(void);

// Base address of the MicroPython sub-region (0 if no PSRAM).
// Used by jl_get_psram_mp_base() so micropython_embed.c can call
// gc_add() with the correct partition.
uintptr_t psram_mp_base(void);

// Size in bytes of the MicroPython sub-region.
size_t psram_mp_size(void);

// Pointer to the reserved header area at the start of the arena.
// Returns nullptr if PSRAM unavailable.
void* psram_header_ptr(void);

// Print arena status to Serial - used by the `psram_status` debug command.
void psram_arena_dump_status(void);

// Run the pattern write/read self test against the raw PSRAM window
// (save/restore, safe on a live arena). Used by the hardware self test.
bool psram_pattern_test(size_t chipBytes);

// ---------------------------------------------------------------------------
// Introspection for the granular memory-map viewer (Debugs.cpp).
// ---------------------------------------------------------------------------

// Absolute base address of the allocator pool (after the ArenaState + reserved
// header). 0 if PSRAM unavailable.
uintptr_t psram_pool_base(void);

// Size in bytes of the allocator pool (excludes ArenaState + reserved header).
size_t psram_pool_size(void);

// Per-block visitor callback. `addr` is the absolute address of the block
// header, `size` includes the header, `used` is 1 for an allocated block and
// 0 for a free block.
typedef void (*psram_block_visitor)(void* ctx, uintptr_t addr, size_t size, int used);

// Walk every block in the pool in address order, invoking `cb` for each.
// Returns the number of blocks visited. Takes the arena lock, so the callback
// MUST NOT call psram_alloc / psram_free / psram_realloc (it would deadlock).
// No-op (returns 0) if PSRAM is unavailable.
size_t psram_arena_walk(psram_block_visitor cb, void* ctx);

// Debug-trace flag. When non-zero, PsramArena / FileCache / Undo emit
// per-step Serial prints so deadlocks/hangs can be diagnosed. Defaults
// on while the new code is bedding in; flip to 0 once stable.
extern volatile int psram_debug;

#ifdef __cplusplus
}
#endif

#endif // PSRAM_ARENA_H
