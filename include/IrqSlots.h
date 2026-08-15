// See src/IrqSlots.cpp: shared-IRQ chain slot accounting. The wrapper drops
// duplicate registrations (arduino-pico registers its doorbell handler from
// both cores into a chain that is already shared) and declines rather than
// panicking when the pool is exhausted.
#ifndef IRQSLOTS_H
#define IRQSLOTS_H

#include <Arduino.h>

unsigned jlIrqSlotsUsed( void );
unsigned jlIrqSlotsFree( void );
unsigned jlIrqSlotsDeclined( void );   // registrations refused because the chain was full
unsigned jlIrqSlotsDeduped( void );    // duplicate registrations dropped

// Prints only if something was declined. Safe to call once a terminal exists.
void jlIrqSlotsReport( Stream& out );

#endif // IRQSLOTS_H
