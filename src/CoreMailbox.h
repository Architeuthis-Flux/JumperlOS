// SPDX-License-Identifier: MIT
#ifndef COREMAILBOX_H
#define COREMAILBOX_H

// ---------------------------------------------------------------------------
// Core 0 -> core 1 request mailbox (SCHEDULER_AND_HARDWARE_OFFLOAD.md, section
// D; T2.2b = migration step 2: REQ_SEND_PATHS only).
//
// Replaces the `sendAllPathsCore2` int (0 / 1 / -1 / 3) whose value encoding
// lost requests: a second request overwrote the first (a -1 clean lost under
// a 1), and sendPaths() zeroed the flag at its end, erasing any request that
// landed during the send. Here a request is a bit set (requests COALESCE and
// a clean is sticky - never downgraded), each slot carries a request
// generation and a done generation, and completion is "doneGen has reached the
// generation my post returned" instead of "the flag reads 0". A request that
// lands while one is in flight stays set and is served on the next pass.
//
// Two slots, because the "3" bypass has different service points (it is also
// served inside loop1()'s pauseCore2 spin and at the top of core2stuff() with
// a 1 ms core_sync try) and never has a waiter, while a SEND is what
// refreshConnections() and waitCore2() wait on:
//   REQ_SEND    bits SEND_PATHS (+ SEND_CLEAN): send the paths, cleaning first
//               if the clean bit is set. Served in core2stuff()'s main branch.
//   REQ_BYPASS  the old "3": send without cleaning, right now, even while
//               pauseCore2 is held. Served in loop1()'s pause spin and at the
//               top of core2stuff().
//
// All updates are made under a FIXED SIO hardware spinlock (PICO_SPINLOCK_ID_
// OS2 - reserved for OS/user use, nobody else in the tree uses it; the OG's
// atomic helper uses OS1). Fixed rather than claimed: core 1 is launched
// BEFORE setup() by arduino-pico, so there is no safe place to claim one, and
// a fixed instance needs no init at all (SIO spinlocks are reset by the SDK
// runtime init). Whether RP2350's exclusive monitor is cross-core is a
// datasheet question the doc leaves open, so no bare atomics here. The
// locked regions are a handful of loads/stores - never around the work.
//
// Rules: core 1 takes (bits cleared) only when it is about to do the work - a
// peek is not a take. complete() IS called from an IRQ (the CH446Q PIO ISR
// ends a DMA-fed list send, T2.3) and that is safe: spin_lock_blocking()
// disables IRQs on the holding core, so a same-core holder cannot be
// preempted by that ISR, and a cross-core holder releases within a few
// instructions - a bounded ~ns spin. post()/take() are never called from
// IRQ context.
// ---------------------------------------------------------------------------

#include <stdint.h>

namespace core1req {

enum Slot : uint8_t {
    REQ_SEND = 0,
    REQ_BYPASS = 1,
    SLOT_COUNT = 2
};

// REQ_SEND bits
enum : uint32_t {
    SEND_PATHS = 1u << 0,
    SEND_CLEAN = 1u << 1, // reset the crossbar first; sticky until served
};

// Core 0 (or 1): OR `bits` into the slot, bump its request generation.
// Returns the generation to wait for with isDone(). Cheap; never blocks
// beyond the other core's few-instruction critical section.
uint32_t post( Slot s, uint32_t bits );

// Any bit pending in the slot? (does not clear anything)
bool pending( Slot s );

// Core 1 (or the core that will do the work): atomically read AND clear the
// slot's bits; *gen receives the request generation covered. Returns the
// bits taken (0 = nothing pending; *gen untouched). Call ONLY when the work
// will be done right after.
uint32_t take( Slot s, uint32_t* gen );

// The work for generation `gen` is done (monotonic - a lower gen never moves
// doneGen back).
void complete( Slot s, uint32_t gen );

// Has the work for `gen` completed?
bool isDone( Slot s, uint32_t gen );

// Nothing pending and nothing in flight in this slot.
bool idle( Slot s );

// Nothing pending or in flight in any slot (what waitCore2() waits for).
bool allIdle( void );

// Diagnostics (X): bits pending, request gen, done gen.
void snapshot( Slot s, uint32_t* bits, uint32_t* reqGen, uint32_t* doneGen );

} // namespace core1req

#endif
