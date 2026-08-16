// FlashPark: park the other core for a flash erase/program - without the
// lost-doorbell race in arduino-pico's _MFIFO handshake.
//
// Every flash write on this board ends in the SDK's flash_range_erase() /
// flash_range_program() (SPIFTL under FatFS, EEPROM.commit(), everything).
// While the flash is being written XIP is off, so the OTHER core must be
// parked in RAM with interrupts masked. arduino-pico does that with a doorbell
// (rp2040.idleOtherCore()), but its core-1 handler clears the sticky doorbell
// AFTER it has left the park spin and re-enabled interrupts. Two back-to-back
// flash ops (every multi-page program, every erase+program pair) then race:
// core 0 rings again for the next page while core 1 is in that gap, core 1's
// clear swallows the new ring, and core 0 spins forever in idleOtherCore()
// with interrupts off - the USB stack dies and the board "drops off the bus".
// Caught over SWD (2026-08-15) after ~18 iterations of a flash-write soak with
// the mic disabled: core 0 in idleOtherCore() from SPIFTL program(), core 1
// back in loop1(), doorbell clear, __otherCoreIdled false. Upstream still has
// the same handler.
//
// This module wraps the two SDK functions (-Wl,--wrap in platformio.ini) with
// its own doorbell + two-flag protocol: the parked core clears the bell FIRST
// and only then acknowledges, and the resume waits until the parked core has
// actually left, so a ring can never be swallowed and at most one ring is ever
// outstanding. Once both cores have registered their handler (setup() /
// setup1()) it takes over and neuters the framework's park with
// rp2040.fifo.begin(1); until then the framework's path is used unchanged, so
// boot-time flash writes behave exactly as before.
//
// RP2350 only (RP2040 has no doorbells; the OG board keeps arduino-pico's
// FIFO-based park). On RP2040 the wrappers are pure pass-throughs.
// ─────────────────────────────────────────────────────────────────────────────
// DISABLED (2026-08-15). Compiled out, kept in-tree as the ready-to-finish fix.
//
// WHY: this needs one shared-IRQ handler chain slot, and there are none free.
// irq_handler_chain_slots[] is defined in irq_handler_chain.S inside the
// PREBUILT lib/rp2350/libpico.a, so PICO_MAX_SHARED_IRQ_HANDLERS cannot be
// raised with a -D (verified: the array is 0x48 bytes = 6 slots in the linked
// ELF; arduino-pico already ships 6 rather than the SDK's default 4).
// The 6 are spoken for: arduino-pico's doorbell park handler (registered on
// BOTH cores = 2), Adafruit TinyUSB USBCTRL_IRQ, CH446Q PIO0_IRQ_1, and then
// MicroPython's machine_pin_irq_init (IO_IRQ_BANK0) + rp2_dma_jl (DMA_IRQ_0)
// on the first REPL init. irq_add_shared_handler() hard_asserts when the pool
// is empty, so enabling this panics core 0 inside mp_embed_init() the moment
// anything touches MicroPython (SWD-confirmed stack: machine_pin_jl.c:125 ->
// irq_add_shared_handler -> hard_assertion_failure -> panic).
//
// TO ENABLE, first free a slot. Options, best first:
//   1. Stop arduino-pico registering its doorbell handler twice. It installs
//      the same handler from main() and main1() but PICO_VTABLE_PER_CORE is 0,
//      so the chain is shared and the second registration is pure waste; only
//      the NVIC enable is per-core. A one-line core patch (or a local core
//      override) frees a slot AND is correct on its own terms.
//   2. Take over the park completely and remove the framework's handler
//      (needs the handler's address - _MFIFO::_irq is private).
//   3. Ship a libpico.a built with a larger PICO_MAX_SHARED_IRQ_HANDLERS.
// Then set JL_FLASH_PARK_ENABLE=1 AND JL_FLASH_PARK_WRAPPED=1 together with the
// two -Wl,--wrap flags (all on the same platformio.ini line, so they cannot
// drift apart), and re-run test/hil/swd/stress_flash.py - the bug it fixes
// reproduces there in under ~20 iterations.
//
// NOTE the pool is at its limit WITHOUT this module too: with MicroPython
// initialised there are ZERO slots left, so the next feature to ask for one
// silently kills a core - MicroPython's UART (machine_uart_jl.c:297) is in
// that queue. That is pre-existing and worth fixing for its own sake.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef FLASHPARK_H
#define FLASHPARK_H

#ifndef JL_FLASH_PARK_ENABLE
#define JL_FLASH_PARK_ENABLE 0
#endif

// Set to 1 on the SAME platformio.ini line that adds the two --wrap flags.
// flashParkTakeover() static_asserts on it, so an enabled-but-unwrapped build
// fails to compile instead of silently shipping with no park at all.
#ifndef JL_FLASH_PARK_WRAPPED
#define JL_FLASH_PARK_WRAPPED 0
#endif

#include <stdint.h>

// Call once, early, on EACH core (top of setup() and setup1()).
void flashParkRegisterCore( void );

// Call once on core 0 after core 1 has finished setupCore2stuff(): switches
// every flash write onto this protocol. No-op until both cores registered.
void flashParkTakeover( void );

// Diagnostics: parks that timed out waiting for the other core (each one is a
// flash op that ran with the other core NOT parked), and whether we own it.
uint32_t flashParkTimeouts( void );
bool     flashParkActive( void );

#endif // FLASHPARK_H
