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
// ENABLED (2026-08-15) - JL_FLASH_PARK_ENABLE=1, JL_FLASH_PARK_WRAPPED=1 and the
// two -Wl,--wrap flags on one platformio.ini line (flashParkTakeover()
// static_asserts on WRAPPED, so an enabled build without the wraps fails to
// link rather than shipping with no park at all).
//
// It needs one shared-IRQ handler chain slot, and for a while there was none:
// irq_handler_chain_slots[] is defined in irq_handler_chain.S inside the
// PREBUILT lib/rp2350/libpico.a, so PICO_MAX_SHARED_IRQ_HANDLERS cannot be
// raised with a -D (verified: 0x48 bytes = 6 slots in the linked ELF). Two
// things freed one: src/IrqSlots.cpp stopped arduino-pico registering its own
// doorbell handler from both cores into the one shared chain, and the logic
// analyzer / JulseView - the other queued consumers - were removed as dead
// code. Measured census with this on ('X' resource status): 6/6 used -
// USBCTRL_IRQ x2 (TinyUSB), SIO_IRQ_BELL x2 (arduino-pico's park + ours),
// PIO0_IRQ_1 (CH446Q), IO_IRQ_BANK0 (MicroPython pins). Nothing else in the
// tree asks for a slot: MicroPython's machine.UART(0) piggybacks on the UART0
// AsyncPassthrough already enabled, UART(1) uses an exclusive handler. If a
// future feature does ask, IrqSlots declines it (counted, reported) instead
// of hard_asserting a core.
//
// The framework's own park handler still holds its slot after takeover; it is
// dead weight (rp2040.fifo.begin(1) neuters it) but _MFIFO::_irq is private,
// so it cannot be removed from here without a core patch.
//
// TO DISABLE: change the platformio.ini line to JL_FLASH_PARK_ENABLE=0,
// JL_FLASH_PARK_WRAPPED=0 and drop the two --wrap flags. The stubs below keep
// main.cpp's call sites valid either way.
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
