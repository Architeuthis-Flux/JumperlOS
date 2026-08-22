// See include/CrashLog.h. Everything on the fault path is RAM-resident and
// self-contained: a fault can happen while XIP is disabled for a flash write,
// and the handler must not touch flash-resident code (no watchdog_reboot(),
// no printf) or it double-faults into a lockup.
#include "CrashLog.h"

#include <hardware/structs/psm.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/watchdog.h>
#include <hardware/structs/timer.h>
#include <hardware/regs/psm.h>
#include <hardware/regs/watchdog.h>
#include <pico/platform.h>
#include <stdio.h>
#ifdef PICO_RP2350
#include <hardware/structs/powman.h>
#endif

// Slot layout. Word 0 is the header: 0xC0DE in the top half, then
// seq(8) | core(4) | pending(1) | valid(1)... packed so a single word write
// commits the record.
#define CRASHLOG_TAG        0xC0DE0000u
#define CRASHLOG_TAG_MASK   0xFFFF0000u
#define CRASHLOG_SEQ_SHIFT  8u
#define CRASHLOG_CORE_SHIFT 4u
#define CRASHLOG_PENDING    0x2u
#define CRASHLOG_VALID      0x1u

#ifdef PICO_RP2350
// 0-7: POWMAN scratch, 8-10: watchdog scratch 0-2.
#define CRASHLOG_SLOTS 11u
static inline volatile uint32_t* __not_in_flash_func( crashSlot )( unsigned i ) {
    return ( i < 8u ) ? &powman_hw->scratch[ i ] : &watchdog_hw->scratch[ i - 8u ];
}
enum { SLOT_HDR = 0, SLOT_PC, SLOT_LR, SLOT_XPSR, SLOT_SP, SLOT_CFSR, SLOT_HFSR, SLOT_BFAR,
       SLOT_MMFAR, SLOT_EXCRET, SLOT_UPTIME };
#else
// RP2040: only watchdog scratch 2 and 3 are actually free. scratch[0] and [1]
// are the bootrom's reset_usb_boot() parameters (usb_activity_gpio_pin_mask and
// disable_interface_mask) - and src/ArduinoStuff.cpp calls reset_usb_boot(0,1)
// for the JumperIDE PICOBOOT path, which would zero the 0xC0DE tag and destroy
// the record with the very reflash that follows a crash. scratch[4..7] are the
// SDK's own reboot vector.
#define CRASHLOG_SLOTS 2u
static inline volatile uint32_t* __not_in_flash_func( crashSlot )( unsigned i ) {
    return &watchdog_hw->scratch[ i + 2u ];
}
enum { SLOT_HDR = 0, SLOT_PC };
#endif

// Note: POWMAN's 0x5AFE password scheme applies only to its 16-bit control
// registers (STATE, timer setup, ...); SCRATCH0-7 are plain 32-bit RW.
static inline void __not_in_flash_func( slotWrite )( unsigned i, uint32_t v ) {
    *crashSlot( i ) = v;
}

// ---------------------------------------------------------------------------
// ABORT LATCH. A HardFault whose CFSR is 0 and whose HFSR is DEBUGEVT is not a
// wild pointer - it is a BKPT with no debugger, i.e. a DELIBERATE abort(). The
// stacked PC/LR then say `_exit` / `abort` and nothing about WHO called abort,
// which is the only interesting question. (2026-08-22: exactly that record came
// off Kevin's bench and could not be attributed.)
//
// The firmware has five ways to reach abort() and none of them announce
// themselves on the USB console:
//   * operator new on a failed malloc (-fno-exceptions -> __throw_bad_alloc)
//   * std::vector length/alloc failures
//   * newlib assert() - __assert_func fiprintf()s to stderr, which on this
//     target goes nowhere a bench user can see. SPIFTL's flash GC/metadata
//     asserts and the TLSF PSRAM allocator's ~50 asserts are all live.
// So: latch the CALLER of abort()/assert() into a reset-retained scratch word
// and print it with the crash report on the next boot. Zero cost on every path
// that never aborts.
//
// WHAT THIS DOES AND DOES NOT NAME - read this before trusting a site:
//   * assert() family: the latched address IS the asserting function, and the
//     live [abort] line carries file:line: expr too. Fully named.
//   * ALLOCATION family (operator new, __throw_bad_alloc,
//     __throw_length_error, __throw_bad_array_new_length): these are SHARED
//     CHOKE POINTS. __throw_bad_alloc `bl`s abort, so the latched address is
//     inside __throw_bad_alloc - not the allocation that failed. A latched site
//     that symbolizes to a throw helper or to operator new means only "an
//     allocation failed somewhere". crashlogAbortFrameScan() below exists to
//     recover the frames above it; treat those as candidates, not as an answer.
//   * panic() - 13 SDK call sites (hard_assert, panic_unsupported, the flash
//     and multicore guards) - is `naked` and ends in its OWN bkpt without ever
//     entering abort(). Identical CFSR=0 + HFSR.DEBUGEVT fingerprint, NOTHING
//     latched. That is why the no-site report line names panic() too.
//   * An assert inside a __not_in_flash_func latches NOTHING: it writes a RAM
//     address, abortSitePlausible() rejects it at boot, and first-writer-wins
//     then blocks abort()'s own write. No such assert exists in the image today
//     (checked); see the note on abortSitePlausible if one ever appears.
//   * A future libstdc++ that TAIL-calls abort() would latch the grandparent
//     frame instead of the caller. Not the case in this image (verified: all
//     four allocation helpers use `bl`), but it would fail silently.
//
// RP2350 only: watchdog scratch 3 is the one word this file's slot map (POWMAN
// 0-7 + watchdog 0-2) leaves unclaimed, and nothing in the SDK or this tree
// touches it. RP2040 has no spare - the OG build gets the live print only.
#ifdef PICO_RP2350
#define CRASHLOG_HAS_ABORT_SLOT 1
static inline volatile uint32_t* __not_in_flash_func( abortSiteSlot )( void ) {
    return &watchdog_hw->scratch[ 3 ];
}
#endif

// Bounds come from the LINK, not from a literal: a hardcoded 4 MB ceiling would
// start silently dropping sites the day the image outgrows it, with no
// diagnostic. __flash_binary_start/_end are linker-defined in both envs.
extern "C" char __flash_binary_start[];
extern "C" char __flash_binary_end[];

// A Thumb return address has bit 0 set and points inside the image. Anything
// else in the slot is residue, not a site.
//
// FLASH ONLY is deliberate, not an oversight. Every caller of abort() in this
// image - operator new, the three std::vector throw helpers, and all 789
// __assert_func sites (SPIFTL, TLSF, ArduinoJson, MicroPython) - is
// flash-resident; the one and only RAM-address call to abort is
// __assert_func's own, and rejecting that is exactly right, because
// __assert_func has already stashed the asserting function.
//
// The cost is stated in the ABORT LATCH note above: an assert inside a
// __not_in_flash_func latches nothing at all. Widening this to admit
// .time_critical RAM text would fix that AND re-admit __assert_func's internal
// `bl abort`, so it is not a one-liner - it needs a different discriminator.
static inline bool abortSitePlausible( uint32_t v ) {
    return ( v & 1u ) &&
           v >= (uint32_t) __flash_binary_start + 0x100u &&
           v < (uint32_t) __flash_binary_end;
}

// FIRST writer wins: __assert_func stashes the asserting function, and the
// abort() it then calls must not overwrite it with its own caller (which would
// just be __assert_func).
//
// RAM-resident, and called BEFORE anything else in abort(): SPIFTL's asserts
// can fire with XIP disabled mid-flash-program, where the console print that
// follows would itself fault. Stash first and even that fault arrives carrying
// the abort site.
static void __not_in_flash_func( crashlogStashAbortSite )( uint32_t pc ) {
#ifdef CRASHLOG_HAS_ABORT_SLOT
    if ( !abortSitePlausible( *abortSiteSlot( ) ) ) *abortSiteSlot( ) = pc;
#else
    (void) pc;
#endif
}

static uint32_t g_bootAbortSite = 0;

static bool g_reportedThisBoot = false;
// Latched once at boot. The scratch registers survive far more than our own
// reset - reset_usb_boot(), a picotool reboot and a UF2 reflash all leave them
// intact - and PENDING was only cleared by a DELIVERED report, which needs a
// terminal. A crash with nobody attached therefore stayed armed, and the next
// session (different firmware) printed "the last reset was a HardFault" with a
// PC from a binary that no longer exists. Consuming PENDING at boot means the
// record is reported at most once per boot, by the build that recorded it.
static CrashRecord g_bootRecord;
static bool        g_bootRecordValid = false;

#ifdef PICO_RP2350
// STKOF postmortem (diagnostic). When the recorded fault is a core-0 stack
// overflow (CFSR bit 20, UFSR.STKOF), the stacked frame was VETOED - the M33
// pins SP at MSPLIM and suppresses the writes - so the recorded PC/LR/xPSR are
// residue, not the faulting context. The real evidence is the dead stack
// itself: SRAM survives the watchdog reboot, and crashlogLatchAtBoot() runs
// early enough in setup() that the DEEP frames (low addresses, just above the
// pinned SP) have not been reused yet. Capture them here, print them with the
// report. Raw words catch EXC_RETURN magics (nested-IRQ pileup) and
// RAM-resident code; the filtered list catches flash-text return addresses.
// Repeated identical values are the fingerprint of runaway recursion - do NOT
// dedupe.
#define STKOF_RAW_WORDS 96u
#define STKOF_MAX_CAND  128u
static uint32_t g_stkofRaw[ STKOF_RAW_WORDS ];
static uint32_t g_stkofRawBase  = 0;
static uint32_t g_stkofRawCount = 0;
static uint32_t g_stkofCandOff[ STKOF_MAX_CAND ];
static uint32_t g_stkofCandVal[ STKOF_MAX_CAND ];
static uint32_t g_stkofCandCount = 0;

static void crashlogCaptureStkofStack( void ) {
    const CrashRecord& r = g_bootRecord;
    if ( r.core != 0 || !( r.cfsr & ( 1u << 20 ) ) ) return;
    if ( r.sp < 0x20080000u || r.sp >= 0x20082000u ) return;
    uint32_t msp;
    __asm volatile( "mrs %0, msp" : "=r"( msp ) );
    const uint32_t* lo = (const uint32_t*) ( r.sp & ~3u );
    const uint32_t* hi = (const uint32_t*) ( ( msp - 256u ) & ~3u );
    if ( hi > (const uint32_t*) 0x20082000u ) hi = (const uint32_t*) 0x20082000u;
    g_stkofRawBase = (uint32_t) lo;
    for ( const uint32_t* p = lo; p < hi && g_stkofRawCount < STKOF_RAW_WORDS; p++ )
        g_stkofRaw[ g_stkofRawCount++ ] = *p;
    for ( const uint32_t* p = lo; p < hi && g_stkofCandCount < STKOF_MAX_CAND; p++ ) {
        uint32_t w = *p;
        if ( ( w & 1u ) && w >= 0x10000100u && w < 0x10400000u ) {
            g_stkofCandOff[ g_stkofCandCount ] = (uint32_t) p - (uint32_t) lo;
            g_stkofCandVal[ g_stkofCandCount ] = w;
            g_stkofCandCount++;
        }
    }
}

// WHY THERE IS NO ABORT-FRAME SCAN HERE - a tried-and-failed experiment, kept
// so nobody spends the afternoon again.
//
// The one-word latch names abort's IMMEDIATE caller, which for the allocation
// family is a shared throw helper (see the ABORT LATCH note). The obvious
// recovery is to do for abort what crashlogCaptureStkofStack does for STKOF:
// walk the dead stack from the recorded SP and print the flash-text words as
// caller candidates. It was implemented and run on hardware. It recovers
// NOTHING, and the reason is structural rather than a tuning problem:
//
//   STKOF's dead frames sit DEEP - just above the pinned SP, near the stack
//   FLOOR - which is the one region boot never reaches, so they survive.
//   An abort happens at ordinary call depth (Kevin's was SP=0x20081840, the
//   bench repro SP=0x20081C88 - roughly 1-2 KB below the 0x20082000 top), and
//   its caller frames are ABOVE it, i.e. exactly the few hundred bytes that
//   setup() itself is standing on by the time crashlogLatchAtBoot() runs.
//
// Measured, not assumed: the scan window came out valid and non-empty
// (lo=0x20081C88 hi=0x20081EB8, 140 words) and contained ZERO plausible
// flash-text addresses - all of it already churned by the boot path.
//
// Recovering the allocation site therefore needs the capture to happen ON the
// crash path, which needs somewhere to put it, which is the thing the one-word
// scratch budget does not have. Until then the ceiling in the ABORT LATCH note
// is the honest statement of what this instrument can and cannot name.
#endif

extern "C" {

// The C half of the handler. `frame` is the exception stack frame the core
// pushed (r0-r3, r12, lr, pc, xpsr); `excReturn` is the EXC_RETURN in lr.
void __attribute__((used)) __not_in_flash_func( crashlog_hardfault_c )( uint32_t* frame, uint32_t excReturn ) {
    // No `bkpt` here even when DHCSR.C_DEBUGEN is set: that bit stays set after
    // a debugger disconnects (until power cycle), so a "stop for the debugger"
    // branch would halt a board that has no debugger on it.
    const uint32_t oldHdr = *crashSlot( SLOT_HDR );
    uint32_t seq = 1u;
    if ( ( oldHdr & CRASHLOG_TAG_MASK ) == CRASHLOG_TAG && ( oldHdr & CRASHLOG_VALID ) ) {
        seq = ( ( oldHdr >> CRASHLOG_SEQ_SHIFT ) & 0xFFu ) + 1u;
    }
    const uint32_t core = sio_hw->cpuid & 0xFu;
    // Read BEFORE the slots below are overwritten.
    const uint32_t uptimeMs = (uint32_t) ( timer_hw->timerawl / 1000u );

    slotWrite( SLOT_PC, frame[ 6 ] );
#ifdef PICO_RP2350
    slotWrite( SLOT_LR,   frame[ 5 ] );
    slotWrite( SLOT_XPSR, frame[ 7 ] );
    slotWrite( SLOT_SP,     (uint32_t) frame );
    slotWrite( SLOT_CFSR,   *(volatile uint32_t*) 0xE000ED28u );
    slotWrite( SLOT_HFSR,   *(volatile uint32_t*) 0xE000ED2Cu );
    slotWrite( SLOT_BFAR,   *(volatile uint32_t*) 0xE000ED38u );
    slotWrite( SLOT_MMFAR,  *(volatile uint32_t*) 0xE000ED34u );
    slotWrite( SLOT_EXCRET, excReturn );
    slotWrite( SLOT_UPTIME, uptimeMs );
#else
    (void) excReturn;
#endif
    // Header last: this is the commit.
    slotWrite( SLOT_HDR, CRASHLOG_TAG | ( ( seq & 0xFFu ) << CRASHLOG_SEQ_SHIFT ) |
                         ( core << CRASHLOG_CORE_SHIFT ) | CRASHLOG_PENDING | CRASHLOG_VALID );

    // BOOTLOOP GUARD. A fault that reproduces every boot would otherwise reset
    // forever, and a board whose USB never stays up long enough for the
    // 1200-baud touch to land can then only be recovered with physical BOOTSEL
    // - while the record explaining why cannot be printed either, because
    // crashlogReportOnce() waits for a terminal that never attaches. After
    // three consecutive faults that each landed within 10 s of boot, stop
    // rebooting and just halt THIS core. That is exactly what the SDK's weak
    // handler did, and for a core-1 fault it leaves core 0, USB and the REPL
    // alive - which is the console you need to fix the offending slot or
    // config. crashlogReportOnce() clears the counter once the board proves it
    // can reach a terminal, so unrelated faults spread over days never trip it.
    if ( seq >= 3u && uptimeMs < 10000u ) {
        for ( ;; ) { __asm volatile( "wfi" ); }
    }

    // Reboot the whole chip through the watchdog - the RAM-only equivalent of
    // watchdog_reboot(0, 0, 0). Reset everything except the oscillators (the
    // SDK's own selection), no debug pause, fire immediately. Note SIO
    // doorbells survive this; main.cpp's ClearStaleDoorbells handles that.
    hw_clear_bits( &watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS );
    hw_set_bits( &psm_hw->wdsel, PSM_WDSEL_BITS & ~( PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS ) );
    hw_clear_bits( &watchdog_hw->ctrl, WATCHDOG_CTRL_PAUSE_DBG0_BITS |
                                       WATCHDOG_CTRL_PAUSE_DBG1_BITS |
                                       WATCHDOG_CTRL_PAUSE_JTAG_BITS );
    watchdog_hw->scratch[ 4 ] = 0;   // regular boot, not a scratch-vector jump
    hw_set_bits( &watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS );
    for ( ;; ) { __asm volatile( "wfi" ); }
}

// Strong definition overrides the SDK's weak `bkpt` handler in crt0.S. Naked:
// pick the active stack, hand the frame and EXC_RETURN to the C half.
// Thumb-1 only (Cortex-M0+ on the OG board has no IT blocks or tst-immediate),
// which also runs unchanged on the V5's M33.
void __attribute__((naked, section(".time_critical.crashlog"))) isr_hardfault( void ) {
    __asm volatile(
        "movs r0, #4           \n"
        "mov  r1, lr           \n"
        "tst  r0, r1           \n"   // EXC_RETURN bit 2: which stack was in use
        "beq  1f               \n"
        "mrs  r0, psp          \n"
        "b    2f               \n"
        "1:                    \n"
        "mrs  r0, msp          \n"
        "2:                    \n"
        "mov  r1, lr           \n"
        "ldr  r2, =crashlog_hardfault_c \n"
        "bx   r2               \n"
        ".ltorg                \n" );
}

// --- abort()/assert() interception (see the ABORT LATCH note above) ---------
// Strong definitions. The linker resolves `abort` / `__assert_func` here and
// never pulls newlib's abort.o or assert.o, so there is no duplicate symbol.
//
// Both end in the SAME `bkpt` newlib's abort would have reached (via _exit), so
// isr_hardfault still records the fault, still reboots, and the existing
// [crashlog] report still prints - it just gains a line naming the caller.

// Best-effort console notice. The scratch stash is the reliable half; this is
// the half you get to read immediately, and it is the ONLY half on OG (no spare
// scratch word there).
//
// No String, no new, no Arduino concatenation - abort's commonest cause IS a
// failed allocation. The one non-trivial call left is snprintf, which for
// %s/%d/%lX does not allocate in newlib; that is a read of newlib's behaviour,
// not a guarantee, and it matters because 53 of the candidate aborts are
// heap-corruption asserts inside TLSF. The stash is committed before we get
// here precisely so this call cannot cost us the evidence.
//
// GUARD 1 - core 0, thread mode, interrupts enabled. Serial/TinyUSB is core
// 0's, and - see GUARD 2 - the bytes only move because an IRQ moves them.
// PRIMASK is load-bearing TWICE: with interrupts masked the wait loop below
// would sleep its whole budget and ship nothing.
//
// GUARD 2 - NEVER issue a write that cannot fit. Adafruit_USBD_CDC::write()
// loops `while (remain && tud_cdc_n_connected())` with NO timeout, so a host
// that holds DTR without draining (or, with usb_cdc.ignore_dtr=1, a port with
// no reader at all) hangs us here FOREVER - before the bkpt. That costs the
// HardFault, the crashlog record, the reboot AND the bootloop guard: strictly
// worse than the silent reboot this whole latch replaces. availableForWrite()
// is non-blocking and returns 0 on an invalid CDC, so gating on it makes the
// write unable to block. A backlogged-but-live host therefore loses the live
// line by design - the scratch stash still carries the site.
static void __attribute__((noinline)) crashlogAnnounceAbort( const char* what,
                                                             uint32_t site,
                                                             const char* file,
                                                             int line,
                                                             const char* expr ) {
    uint32_t ipsr, primask;
    __asm volatile( "mrs %0, ipsr" : "=r"( ipsr ) );
    __asm volatile( "mrs %0, primask" : "=r"( primask ) );
    if ( ipsr != 0 || primask != 0 || ( sio_hw->cpuid & 0xFu ) != 0 ) return;

    char buf[ 192 ];
    char shortBuf[ 48 ];
    snprintf( shortBuf, sizeof( shortBuf ), "\n\r[abort] %s from 0x%08lX\n\r",
              what, (unsigned long) site );
    const char* msg = shortBuf;
    if ( expr ) {
        snprintf( buf, sizeof( buf ), "\n\r[abort] %s: %s:%d: %s (site 0x%08lX)\n\r",
                  what, file ? file : "?", line, expr, (unsigned long) site );
        msg = buf;
    }

    // Longest form first, then the ~40-byte fallback: OG's TX FIFO is 128 bytes
    // against an assert message that routinely exceeds it, and OG has no scratch
    // slot - skipping outright would leave that board with no instrument at all.
    size_t n = strlen( msg );
    if ( (size_t) Serial.availableForWrite( ) < n && msg != shortBuf ) {
        msg = shortBuf;
        n = strlen( msg );
    }
    if ( (size_t) Serial.availableForWrite( ) < n ) return;   // nothing of ours to ship
    Serial.print( msg );

    // Now let it actually leave. A bkpt one instruction later strands anything
    // still in the CDC FIFO. delay() alone does NOT pump the device task - it is
    // sleep_ms() and nothing else; what ships the bytes is the USBCTRL IRQ
    // pending a soft IRQ whose handler runs tud_task(). yield() calls that pump
    // directly, so the mechanism lives in this loop instead of in an interrupt
    // nobody documented. flush() is bounded here because the write above already
    // fit in the FIFO.
    for ( int i = 0; i < 20; i++ ) {
        yield( );
        Serial.flush( );
        delay( 10 );
    }
}

// Both live in RAM, like the fault handler and for the same reason: SPIFTL's
// asserts are the ones most likely to fire with XIP disabled mid-program, and
// a flash-resident abort() could not even reach the stash. The console notice
// after it still touches flash (the format strings are .rodata) and may fault -
// by then the site is already committed, so that fault arrives named too.
void __attribute__((noreturn)) __not_in_flash_func( abort )( void ) {
    crashlogStashAbortSite( (uint32_t) __builtin_return_address( 0 ) );
    crashlogAnnounceAbort( "abort()", (uint32_t) __builtin_return_address( 0 ),
                           NULL, 0, NULL );
    __breakpoint( );
    for ( ;; ) { __asm volatile( "wfi" ); }
}

void __attribute__((noreturn)) __not_in_flash_func( __assert_func )( const char* file, int line,
                                                                     const char* func, const char* expr ) {
    (void) func;
    crashlogStashAbortSite( (uint32_t) __builtin_return_address( 0 ) );
    crashlogAnnounceAbort( "assert", (uint32_t) __builtin_return_address( 0 ),
                           file, line, expr );
    abort( );
}

} // extern "C"

static bool headerValid( uint32_t hdr ) {
    return ( hdr & CRASHLOG_TAG_MASK ) == CRASHLOG_TAG && ( hdr & CRASHLOG_VALID );
}

bool crashlogPending( void ) {
    const uint32_t hdr = *crashSlot( SLOT_HDR );
    return headerValid( hdr ) && ( hdr & CRASHLOG_PENDING );
}

bool crashlogLast( CrashRecord* out ) {
    const uint32_t hdr = *crashSlot( SLOT_HDR );
    if ( !headerValid( hdr ) || out == NULL ) return false;
    *out = CrashRecord{ };
    out->seq  = ( hdr >> CRASHLOG_SEQ_SHIFT ) & 0xFFu;
    out->core = ( hdr >> CRASHLOG_CORE_SHIFT ) & 0xFu;
    out->pc   = *crashSlot( SLOT_PC );
#ifdef PICO_RP2350
    out->lr         = *crashSlot( SLOT_LR );
    out->xpsr       = *crashSlot( SLOT_XPSR );
    out->sp         = *crashSlot( SLOT_SP );
    out->cfsr       = *crashSlot( SLOT_CFSR );
    out->hfsr       = *crashSlot( SLOT_HFSR );
    out->bfar       = *crashSlot( SLOT_BFAR );
    out->mmfar      = *crashSlot( SLOT_MMFAR );
    out->exc_return = *crashSlot( SLOT_EXCRET );
    out->uptime_ms  = *crashSlot( SLOT_UPTIME );
#endif
    return true;
}

void crashlogLatchAtBoot( void ) {
    if ( g_bootRecordValid ) return;
#ifdef CRASHLOG_HAS_ABORT_SLOT
    // Consume the abort site UNCONDITIONALLY, before the no-crash early return
    // below: leaving a stale address armed would let it be attributed to some
    // later, unrelated HardFault.
    //
    // "Unconditionally" means "from here on". This runs as the second statement
    // in setup() (main.cpp:236), so the slot is still armed during C++ static
    // constructors, armStackLimit and flashParkRegisterCore - an abort in THAT
    // window is suppressed by first-writer-wins and the PREVIOUS boot's site is
    // reported for it. Small window, and the bootloop guard still fires; noted
    // rather than closed because clearing earlier would need a second hook.
    {
        const uint32_t site = *abortSiteSlot( );
        *abortSiteSlot( ) = 0;
        if ( abortSitePlausible( site ) ) g_bootAbortSite = site;
    }
#endif
    if ( !crashlogPending( ) ) { g_bootRecordValid = false; return; }
    if ( !crashlogLast( &g_bootRecord ) ) return;
    g_bootRecordValid = true;
#ifdef PICO_RP2350
    crashlogCaptureStkofStack( );
#endif
    // Consume it now, while we still know it belongs to THIS firmware.
    // VALID stays set so the record remains readable; only PENDING is cleared,
    // along with the consecutive-fault counter (reaching this point means the
    // board booted far enough to run setup()).
    uint32_t hdr = *crashSlot( SLOT_HDR );
    hdr &= ~CRASHLOG_PENDING;
    hdr &= ~( 0xFFu << CRASHLOG_SEQ_SHIFT );
    *crashSlot( SLOT_HDR ) = hdr;
}

void crashlogReportOnce( Stream& out ) {
    if ( g_reportedThisBoot || !g_bootRecordValid ) return;
    // Wait for a terminal: the first menu print happens at boot with nobody
    // listening, and burning the one report there would lose it. USB CDC's
    // bool operator is "host has DTR asserted".
    if ( !Serial ) return;
    const CrashRecord& r = g_bootRecord;
    g_reportedThisBoot = true;

    out.printf( "\n\r[crashlog] The last reset was a HardFault on core %lu (uptime %lu ms, fault #%lu since power-on):\n\r",
                (unsigned long) r.core, (unsigned long) r.uptime_ms, (unsigned long) r.seq );
#ifdef PICO_RP2350
    out.printf( "[crashlog]   PC=0x%08lX LR=0x%08lX xPSR=0x%08lX SP=0x%08lX EXC_RETURN=0x%08lX\n\r",
                (unsigned long) r.pc, (unsigned long) r.lr, (unsigned long) r.xpsr,
                (unsigned long) r.sp, (unsigned long) r.exc_return );
#else
    out.printf( "[crashlog]   PC=0x%08lX\n\r", (unsigned long) r.pc );
#endif
#ifdef PICO_RP2350
    out.printf( "[crashlog]   CFSR=0x%08lX HFSR=0x%08lX BFAR=0x%08lX MMFAR=0x%08lX\n\r",
                (unsigned long) r.cfsr, (unsigned long) r.hfsr,
                (unsigned long) r.bfar, (unsigned long) r.mmfar );
#endif
    // A CFSR of 0 with HFSR.DEBUGEVT (bit 31) is a BKPT, not a bad access -
    // i.e. abort()/assert()/panic(), whose caller the stacked frame never
    // shows. Name it here or the record is unattributable (see the ABORT LATCH
    // note, which also says what a latched site does and does not prove).
    if ( g_bootAbortSite ) {
        out.printf( "[crashlog]   DELIBERATE abort()/assert() - called from 0x%08lX (symbolize this, not PC/LR)\n\r",
                    (unsigned long) g_bootAbortSite );
        // The ceiling, stated where it will actually be read. An assert site is
        // the real function; an allocation site is a choke point every failed
        // allocation in the image funnels through, and this instrument cannot
        // see past it (see the tried-and-failed note above).
        out.println( "[crashlog]   If that resolves to operator new / __throw_bad_alloc / __throw_length_error," );
        out.println( "[crashlog]   it is a SHARED helper: it means 'an allocation failed', not WHICH one." );
    } else if ( r.cfsr == 0 && ( r.hfsr & 0x80000000u ) ) {
        // panic() is `naked` and hits its own bkpt without entering abort(), so
        // it produces this exact fingerprint with nothing latched. Naming only
        // abort/assert here would send the reader hunting for the wrong thing.
        out.println( "[crashlog]   CFSR=0 + HFSR.DEBUGEVT = a BKPT with no site latched: abort()/assert() from" );
        out.println( "[crashlog]   RAM-resident code, or panic()/hard_assert(), which never reaches the latch." );
    }
    // Hand back a command that resolves what the lines above told you to look
    // at - not just PC/LR, which for a latched abort are the two addresses the
    // report has just finished saying to ignore.
    out.print( "[crashlog]   Symbolize with: arm-none-eabi-addr2line -C -f -e .pio/build/jumperless_v5/firmware.elf <PC> <LR>" );
    if ( g_bootAbortSite ) out.printf( " %08lX", (unsigned long) g_bootAbortSite );
    out.print( "\n\r" );
#ifdef PICO_RP2350
    if ( g_stkofRawCount ) {
        out.printf( "[crashlog]   STKOF: recorded frame was vetoed (PC/LR above are residue). Dead-stack capture from 0x%08lX, bottom-up:\n\r",
                    (unsigned long) g_stkofRawBase );
        for ( uint32_t i = 0; i < g_stkofRawCount; i += 8 ) {
            out.printf( "[crashlog]   raw +0x%03lX:", (unsigned long) ( i * 4u ) );
            for ( uint32_t j = i; j < i + 8 && j < g_stkofRawCount; j++ )
                out.printf( " %08lX", (unsigned long) g_stkofRaw[ j ] );
            out.print( "\n\r" );
        }
        out.printf( "[crashlog]   flash-text candidates (%lu, deepest first, repeats = recursion):\n\r",
                    (unsigned long) g_stkofCandCount );
        for ( uint32_t i = 0; i < g_stkofCandCount; i += 6 ) {
            out.print( "[crashlog]  " );
            for ( uint32_t j = i; j < i + 6 && j < g_stkofCandCount; j++ )
                out.printf( " +%03lX:%08lX", (unsigned long) g_stkofCandOff[ j ],
                            (unsigned long) g_stkofCandVal[ j ] );
            out.print( "\n\r" );
        }
    }
#endif
}
