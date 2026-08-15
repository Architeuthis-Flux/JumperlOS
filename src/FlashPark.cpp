// See include/FlashPark.h.
#include "FlashPark.h"

#include <Arduino.h>
#include <hardware/flash.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <pico/multicore.h>

#if !JL_FLASH_PARK_ENABLE

// Compiled out - see the banner in include/FlashPark.h for why (no free
// shared-IRQ handler chain slot) and what has to change to turn it on.
// The stubs keep the call sites in main.cpp valid, so enabling is a one-line
// flag flip plus the two -Wl,--wrap flags in platformio.ini.
void flashParkRegisterCore( void ) {}
void flashParkTakeover( void ) {}
uint32_t flashParkTimeouts( void ) { return 0; }
bool     flashParkActive( void )   { return false; }

#elif defined(PICO_RP2350)

namespace {
int           s_bell = -1;              // our doorbell (claimed for both cores)
bool          s_handlerInstalled = false;         // chain slot taken (once, not per core)
volatile bool s_ready[ 2 ]   = { false, false };  // handler registered on core n
volatile bool s_parked[ 2 ]  = { false, false };  // core n is inside the park spin
volatile bool s_release[ 2 ] = { false, false };  // core n may leave the park spin
volatile bool s_active = false;         // we own the flash park now
volatile uint32_t s_timeouts = 0;

// Generous: a healthy other core answers a doorbell in microseconds. If it
// hasn't answered in this long it is dead or wedged, and spinning forever with
// interrupts off (the failure this replaces) helps nobody - proceed, count it.
constexpr uint32_t kAckTimeoutUs = 1000000u;

// The parked core's side. Runs on whichever core got rung, in IRQ context.
// RAM-resident: XIP is about to go away, and this must not touch flash.
void __not_in_flash_func( flashParkIrq )( void ) {
    if ( s_bell < 0 || !multicore_doorbell_is_set_current_core( (uint) s_bell ) ) return;
    const uint core = get_core_num( );
    const uint32_t st = save_and_disable_interrupts( );
    // Clear FIRST. A ring that lands from here on stays pending in the sticky
    // bell and re-enters this handler after we return - never swallowed.
    multicore_doorbell_clear_current_core( (uint) s_bell );
    s_release[ core ] = false;
    __sync_synchronize( );
    s_parked[ core ] = true;
    while ( !s_release[ core ] ) { tight_loop_contents( ); }
    s_release[ core ] = false;
    __sync_synchronize( );
    s_parked[ core ] = false;
    restore_interrupts( st );
}

// The flash-writing core's side.
bool __not_in_flash_func( parkOther )( void ) {
    if ( !s_active ) return false;
    const uint other = 1u - get_core_num( );
    if ( !s_ready[ other ] ) return false;
    s_release[ other ] = false;
    __sync_synchronize( );
    multicore_doorbell_set_other_core( (uint) s_bell );
    const uint32_t t0 = time_us_32( );
    while ( !s_parked[ other ] ) {
        if ( (uint32_t) ( time_us_32( ) - t0 ) > kAckTimeoutUs ) {
            s_timeouts++;
            return false;   // proceed unparked rather than wedge the board
        }
        tight_loop_contents( );
    }
    return true;
}

void __not_in_flash_func( resumeOther )( bool parked ) {
    const uint other = 1u - get_core_num( );
    if ( !parked ) {
        // The other core may still ack late; if it does it will find the
        // release already set and leave immediately.
        s_release[ other ] = true;
        return;
    }
    s_release[ other ] = true;
    const uint32_t t0 = time_us_32( );
    // Wait for it to actually leave, so the next park cannot race this one.
    while ( s_parked[ other ] ) {
        if ( (uint32_t) ( time_us_32( ) - t0 ) > kAckTimeoutUs ) { s_timeouts++; return; }
        tight_loop_contents( );
    }
}
} // namespace

void flashParkRegisterCore( void ) {
    const uint core = get_core_num( );
    if ( s_bell < 0 ) {
        // Claim once (core 0 gets here first, at the top of setup(), before
        // core 1 has finished starting). Non-panicking: decline gracefully.
        int b = multicore_doorbell_claim_unused( 0b11, false );
        if ( b < 0 ) return;
        s_bell = b;
    }
    const uint irq = multicore_doorbell_irq_num( (uint) s_bell );
    // Install the handler exactly ONCE. PICO_VTABLE_PER_CORE is 0 here, so both
    // cores dispatch through the same RAM vector table and the same shared
    // handler chain - adding it per core would both run it twice per IRQ and
    // burn a second chain slot. Only the NVIC enable is genuinely per-core.
    // (The slot pool is PICO_MAX_SHARED_IRQ_HANDLERS deep and
    // irq_add_shared_handler hard_asserts when it runs out - see the build flag
    // in platformio.ini.)
    if ( !s_handlerInstalled ) {
        s_handlerInstalled = true;
        irq_add_shared_handler( irq, flashParkIrq, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY );
    }
    irq_set_enabled( irq, true );
    s_ready[ core ] = true;
}

void flashParkTakeover( void ) {
    if ( s_bell < 0 || !s_ready[ 0 ] || !s_ready[ 1 ] || s_active ) return;
    // From here every flash write parks the other core through flashParkIrq;
    // the framework's idleOtherCore()/resumeOtherCore() become no-ops
    // (_MFIFO::begin(1) clears its _multicore flag and nothing else).
    s_active = true;
    __sync_synchronize( );
    rp2040.fifo.begin( 1 );
}

uint32_t flashParkTimeouts( void ) { return s_timeouts; }
bool     flashParkActive( void )   { return s_active; }

extern "C" {
void __real_flash_range_erase( uint32_t flash_offs, size_t count );
void __real_flash_range_program( uint32_t flash_offs, const uint8_t* data, size_t count );

void __wrap_flash_range_erase( uint32_t flash_offs, size_t count ) {
    const bool parked = parkOther( );
    __real_flash_range_erase( flash_offs, count );
    if ( s_active ) resumeOther( parked );
}

void __wrap_flash_range_program( uint32_t flash_offs, const uint8_t* data, size_t count ) {
    const bool parked = parkOther( );
    __real_flash_range_program( flash_offs, data, count );
    if ( s_active ) resumeOther( parked );
}
} // extern "C"

#else // RP2040 / OG: pass-through, the framework's FIFO park stays in charge.

void flashParkRegisterCore( void ) {}
void flashParkTakeover( void ) {}
uint32_t flashParkTimeouts( void ) { return 0; }
bool     flashParkActive( void )   { return false; }

extern "C" {
void __real_flash_range_erase( uint32_t flash_offs, size_t count );
void __real_flash_range_program( uint32_t flash_offs, const uint8_t* data, size_t count );
void __wrap_flash_range_erase( uint32_t flash_offs, size_t count ) { __real_flash_range_erase( flash_offs, count ); }
void __wrap_flash_range_program( uint32_t flash_offs, const uint8_t* data, size_t count ) { __real_flash_range_program( flash_offs, data, count ); }
}

#endif
