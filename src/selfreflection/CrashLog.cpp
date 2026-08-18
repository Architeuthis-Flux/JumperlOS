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
    out.println( "[crashlog]   Symbolize with: arm-none-eabi-addr2line -C -f -e .pio/build/jumperless_v5/firmware.elf <PC> <LR>" );
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
