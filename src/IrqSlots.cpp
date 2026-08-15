// Shared-IRQ handler chain slots: deduplicate registrations, and decline
// instead of killing a core when the pool runs dry.
//
// THE PROBLEM. pico-sdk keeps shared IRQ handlers in a fixed array,
// irq_handler_chain_slots[], and irq_add_shared_handler() hard_asserts - i.e.
// panics, halting the calling core with no log - when it is full. The array is
// defined in irq_handler_chain.S inside the PREBUILT lib/rp2350/libpico.a, so
// PICO_MAX_SHARED_IRQ_HANDLERS cannot be raised with a -D; it is 6 here
// (arduino-pico already raised it from the SDK default of 4).
//
// Measured on a live board: irq_handler_chain_free_slot_head = -1. ZERO free.
// The six went to arduino-pico's doorbell park handler (registered from BOTH
// cores), Adafruit TinyUSB's USBCTRL_IRQ, CH446Q's PIO0_IRQ_1, and MicroPython's
// machine_pin_irq_init + rp2_dma_jl on the first REPL init. Everything after
// that silently killed a core: the logic analyzer (LogicAnalyzer.cpp), JulseView
// and MicroPython's UART are all in that queue.
//
// TWO FIXES, both here so they cannot drift:
//
// 1. DEDUPLICATE. arduino-pico's _MFIFO::registerCore() installs the SAME
//    handler on the same IRQ from each core. PICO_VTABLE_PER_CORE is 0, so both
//    cores dispatch through one shared vector table and one shared chain - the
//    second registration is pure waste, and it also makes that handler run
//    TWICE per interrupt. Dropping the duplicate is correct on its own terms
//    and gives a slot back.
//
// 2. DECLINE, DON'T PANIC. Once the pool is genuinely exhausted, refusing the
//    registration leaves that one feature without its interrupt; panicking
//    takes the whole board down with no diagnostic. A feature that does not
//    work is debuggable, a dead core is not.
#include <Arduino.h>
#include <hardware/irq.h>

// Matches the array actually linked from libpico.a. Verified by measuring the
// symbol: irq_handler_chain_slots is 0x48 bytes, and each slot is 12 bytes.
// If a future core ships a different size this only mis-predicts the LAST
// registration, which then declines instead of panicking - still the safe way
// to be wrong.
#define JL_IRQ_CHAIN_SLOTS 6u

namespace {
struct Reg { uint8_t num; irq_handler_t handler; };
Reg      s_regs[ JL_IRQ_CHAIN_SLOTS ];
unsigned s_count   = 0;
unsigned s_declined = 0;
unsigned s_deduped  = 0;
}  // namespace

extern "C" {

void __real_irq_add_shared_handler( uint num, irq_handler_t handler, uint8_t order_priority );

void __wrap_irq_add_shared_handler( uint num, irq_handler_t handler, uint8_t order_priority ) {
    for ( unsigned i = 0; i < s_count; i++ ) {
        if ( s_regs[ i ].num == (uint8_t) num && s_regs[ i ].handler == handler ) {
            // Already in the chain - see note 1 above.
            s_deduped++;
            return;
        }
    }
    if ( s_count >= JL_IRQ_CHAIN_SLOTS ) {
        // See note 2. Deferred report: this can run before Serial exists.
        s_declined++;
        return;
    }
    s_regs[ s_count ].num     = (uint8_t) num;
    s_regs[ s_count ].handler = handler;
    s_count++;
    __real_irq_add_shared_handler( num, handler, order_priority );
}

}  // extern "C"

unsigned jlIrqSlotsUsed( void )     { return s_count; }
unsigned jlIrqSlotsFree( void )     { return JL_IRQ_CHAIN_SLOTS - s_count; }
unsigned jlIrqSlotsDeclined( void ) { return s_declined; }
unsigned jlIrqSlotsDeduped( void )  { return s_deduped; }

void jlIrqSlotsReport( Stream& out ) {
    if ( s_declined == 0 ) return;
    out.printf( "\n\r[irq] %u shared-IRQ handler registration(s) DECLINED - the chain is full "
                "(%u/%u used).\n\r", s_declined, s_count, JL_IRQ_CHAIN_SLOTS );
    out.println( "[irq] Whatever asked last is running without its interrupt. This is a "
                 "pico-sdk limit, not a board fault - see src/IrqSlots.cpp." );
}
