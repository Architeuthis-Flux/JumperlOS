#pragma once
// #include "TuiGlue.h"

// Pico SDK multicore synchronization primitives
#include "pico/mutex.h"

extern volatile bool core1busy;
extern volatile bool core2busy;

// =============================================================================
// CORE-1 FRAME HOLD (T3.4 / C16 - replaces the old `volatile bool pauseCore2`)
// =============================================================================
// "Please stop rendering": while the hold depth is nonzero, core 1 parks in
// loop1()'s spin (still serving REQ_BYPASS) and core2stuff() aborts its LED
// frame at the next checkpoint. This is the SOFT hint that keeps core 1 off
// the LED strip / crossbar / XIP-resident render code around flash writes and
// exclusive LED work; the HARD guarantee for flash safety is still
// rp2040.idleOtherCore() inside the flash op itself (FlashPark).
//
// Why a per-core depth instead of the old bool: every hold/release pair lives
// on one core's own call stack, so each core only ever writes its own word -
// no cross-core atomics needed (the RP2350 exclusive-monitor question stays
// moot). Nesting is counted exactly, which retires the fragile
// `was_paused` save/restore idiom the bool required. Saturates at 0 on a
// stray release. NOT IRQ-safe by design: no holder runs in interrupt context.
extern volatile uint32_t core1FrameHoldDepth[ 2 ];

// The reader - loop1() spins on this, so it must stay a bare inline load.
static inline bool core1FramesHeld( void ) {
    return ( core1FrameHoldDepth[ 0 ] | core1FrameHoldDepth[ 1 ] ) != 0;
}

// Scoped hold/release. Call in pairs on the same core (any core). Nests.
void holdCore1Frames( void );
void releaseCore1Frames( void );

// Filesystem activity indicator - set during flash/filesystem operations
// Used by LEDs.cpp to show colored logo during saves
extern volatile bool filesystemActive;
extern volatile unsigned long filesystemActiveUntil;  // Minimum display time tracking
extern const unsigned long FILESYSTEM_INDICATOR_MIN_MS;  // 1/4 second minimum display

// Configurable palette for filesystem indicator (use LogoPalette enum values from LEDs.h)
// Default: PALETTE_YELLOW (4). Set to any LogoPalette value to change the indicator color.
extern int filesystemIndicatorPalette;

// Measure mode indicator - set by MeasureMode service when actively measuring
// Used by LEDs.cpp to show pink logo during voltage measurement
extern volatile bool measureModeActive;
extern int measureModeIndicatorPalette;

// Undo/history activity indicator - logo turns yellow when:
//   * an undo or redo fires (brief flash)
//   * the disconnect probe button has been held long enough that the
//     hold-scroll gesture is armed (continuous while held)
//   * the History scrub menu is active (continuous while open)
// The "until" timestamp lets us light the logo for a minimum window
// after a single event so the user reliably sees the flash.
extern volatile unsigned long undoActivityUntil;
extern int undoIndicatorPalette;

// =============================================================================
// USER INPUT TIMESTAMP + IDLE GATE
// =============================================================================
// `lastUserInputMs` is bumped on every user-originated event (probe button,
// rotary encoder turn, Serial/Jerial byte received, OLED button). The
// FileCache flush service uses this plus several other signals to decide
// whether the system is genuinely idle and a flash write is safe to do
// without the user noticing the LED stutter.
//
// All writes go through `noteUserInput()` which is intentionally trivial
// so it's safe to call from ISR context.
extern volatile uint32_t lastUserInputMs;
void noteUserInput(void);

// Returns true iff the system is genuinely idle enough that hitting flash
// won't stall any interactive work. Checks:
//   - Current UI context is NONE or MAIN_MENU (i.e. not in probe / menu /
//     editor / REPL / file browser / help / debug / app)
//   - No refresh, file load, or core-coordination request in progress
//   - USB MSC host has not mounted the drive
//   - At least `quietMs` since the last user input
bool systemIdleForFlush(uint32_t quietMs = 750);
// extern TuiGlue tuiGlue;

// =============================================================================
// MULTICORE SYNCHRONIZATION SYSTEM
// =============================================================================
// These primitives ensure thread-safe access to shared resources between cores.
// The Pico SDK mutex provides proper hardware-backed mutual exclusion.
//
// Usage:
//   - Call core_sync_init() once in setup() before Core 2 starts
//   - Use core_sync_acquire() / core_sync_release() around shared resource access
//   - Use core_sync_try_acquire() for non-blocking access attempts
//   - Use fs_mutex_acquire() / fs_mutex_release() for filesystem operations
// =============================================================================

// Core synchronization mutex - protects core1busy, core2busy, shared data structures
extern mutex_t core_sync_mutex;

// Filesystem mutex - protects all FatFS operations
extern mutex_t fs_mutex;



// Initialize synchronization primitives (call once in setup())
void core_sync_init(void);

// Core synchronization functions
// These provide mutual exclusion between cores for accessing shared resources
void core_sync_acquire(void);     // Blocking acquire
void core_sync_release(void);     // Release
bool core_sync_try_acquire(void); // Non-blocking, returns true if acquired


// Filesystem mutex functions
// Use these around ALL filesystem operations to prevent concurrent access.
// fs_mutex is per-core RECURSIVE: the owning core may re-acquire without
// deadlocking (depth-counted); only the outermost release drops the lock.
// Cross-core exclusion is unchanged.
void fs_mutex_acquire(void);      // Blocking acquire
void fs_mutex_release(void);      // Release (depth-counted)
bool fs_mutex_try_acquire(void);  // Non-blocking, returns true if acquired

// True iff the calling core currently holds fs_mutex. Lets a path that may or
// may not already own the lock decide whether to acquire (avoids the
// self-deadlock footgun documented in FileCache.cpp).
bool fs_mutex_held_by_this_core(void);

// Timeout versions (returns true if acquired within timeout)
bool core_sync_acquire_timeout_ms(uint32_t timeout_ms);
bool fs_mutex_acquire_timeout_ms(uint32_t timeout_ms);

// =============================================================================
// FLASH OPERATION HELPERS
// =============================================================================
// Use these before/after flash write operations to prevent Core2 crashes.
// Flash writes disable XIP cache, so Core2 must not execute code from flash.

/**
 * Take a core-1 frame hold for a flash operation and wait for core 1 to
 * actually go quiet (core2busy clear). Since T3.4 this is a thin envelope over
 * holdCore1Frames() - the hold depth counts nesting exactly, so the returned
 * bool is vestigial (kept so ~50 call sites' `was_paused` idiom compiles
 * unchanged; it reports whether an outer hold already existed).
 * @param timeout_ms Maximum time to wait for core 1 to go quiet (default 100ms)
 * @return true if a hold was already active before this call (informational)
 */
bool pauseCore2ForFlash(uint32_t timeout_ms = 100);

/**
 * Release the frame hold taken by pauseCore2ForFlash.
 * @param was_paused Ignored since T3.4 (nesting is depth-counted now);
 *                   kept so existing call sites compile unchanged.
 */
void unpauseCore2ForFlash(bool was_paused);

// Note: Safe file operations (safeFileOpen, safeFileClose, etc.) are now in FilesystemStuff.h
