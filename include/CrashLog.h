// Crash log: turn a silent HardFault into evidence.
//
// The pico-sdk default HardFault handler is a bare `bkpt`. With no debugger
// attached that halts the faulting core silently: if it was core 0 the USB
// stack dies with it and the board just vanishes; if it was core 1 the board
// looks alive until the next flash write parks a core that can never answer.
// Either way there is nothing to read afterwards.
//
// This handler records the fault (which core, stacked PC/LR/xPSR, the SCB
// fault registers) into reset-retained scratch registers, then reboots the
// board through the watchdog. The record is printed once, to the first
// terminal that gets the menu after the reboot, as
//
//   [crashlog] The last reset was a HardFault on core 1 ...
//   [crashlog]   PC=0x1000ABCD LR=0x1000AB01 CFSR=0x00010000 ...
//
// instead of a bricked-looking board.
//
// Why scratch registers and not RAM: arduino-pico's OTA stub at the start of
// flash copies its own .data into 0x20000110.. on EVERY boot, and that is
// exactly where the linker puts the main app's .uninitialized_data - so
// __uninitialized_ram() is not reset-safe on this framework (SWD-verified,
// 2026-08-15). RP2350: POWMAN scratch 0-7 + watchdog scratch 0-2 (nothing in
// the SDK, the core or this tree uses them). RP2040/OG: watchdog scratch 0-3
// only (its Cortex-M0+ has no fault status registers anyway); scratch 4-7 are
// the SDK's own reboot vector and stay untouched.
//
// For live SWD debugging, break on crashlog_hardfault_c() - the handler never
// stops for a debugger.
#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <Arduino.h>
#include <stdint.h>

struct CrashRecord {
    uint32_t seq;         // fault count since power-on, 1-based
    uint32_t core;
    uint32_t pc;
    uint32_t lr, xpsr, sp;              // RP2350 only (no scratch room on RP2040)
    uint32_t cfsr, hfsr, bfar, mmfar;   // 0 on RP2040 (no fault status registers)
    uint32_t exc_return;
    uint32_t uptime_ms;
};

// True if a logged fault is waiting to be shown.
bool crashlogPending( void );

// Call ONCE, early in setup(): latches any pending record into RAM and clears
// the pending flag. Without this a record that never found a terminal stays
// armed across a reflash and is later reported against a different binary.
void crashlogLatchAtBoot( void );

// Print the pending record (once, and only once a terminal is connected).
void crashlogReportOnce( Stream& out );

// Fetch the last record (whether or not shown); false if none is valid.
bool crashlogLast( CrashRecord* out );

#endif // CRASHLOG_H
