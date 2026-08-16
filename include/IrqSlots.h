// See src/IrqSlots.cpp: shared-IRQ chain slot accounting. The wrappers drop
// duplicate registrations (arduino-pico registers its doorbell handler from
// both cores into a chain that is already shared), forget a handler again when
// irq_remove_handler releases it, and decline rather than panic when the pool
// is exhausted.
#ifndef IRQSLOTS_H
#define IRQSLOTS_H

#include <Arduino.h>

unsigned jlIrqSlotsUsed( void );
unsigned jlIrqSlotsFree( void );
unsigned jlIrqSlotsDeclined( void );   // registrations refused because the chain was full
unsigned jlIrqSlotsDeduped( void );    // duplicate registrations dropped
unsigned jlIrqSlotsRemoved( void );    // registrations released again via irq_remove_handler

// Prints only if something was declined. Safe to call once a terminal exists.
void jlIrqSlotsReport( Stream& out );

// Always prints: the counters plus every (irq, handler) currently registered.
// Part of the 'X' resource status.
void jlIrqSlotsDump( Stream& out );

#endif // IRQSLOTS_H
