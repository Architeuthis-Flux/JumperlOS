// SPDX-License-Identifier: MIT
//
// I2C0 arbiter (I2C0Arbiter.cpp): every I2C0 transfer goes through the SDK's
// wrapped i2c_*_blocking_until, which pauses WaveGen's stream for the
// transaction - and, when an alternate pin pair is registered, hands the
// I2C0 block to that pair for transactions addressed to its device. That is
// how the OG's OLED (routed through the crossbar onto GPIO 16/17) shares the
// block with the INA219s hardwired on 4/5: two pin pairs assigned to one
// peripheral input are combined by the GPIO mux, so only one pair is ever
// muxed in, switched per transaction by address. I2C0 is core-0-only
// (Peripherals.cpp readCurrent), so no lock is needed around the switch.
#ifndef I2C0_ARBITER_H
#define I2C0_ARBITER_H
#include <stdint.h>

// Register the alternate pair (altSda/altScl) next to the pair Wire runs on
// (priSda/priScl). Transactions to `addr` use the alternate pair; everything
// else the primary. The alternate pins must already carry their pull-ups.
void i2c0ArbiterSetAltPins(int altSda, int altScl, int priSda, int priScl);
void i2c0ArbiterSetAltAddr(int addr);   // -1 = nothing routes to the alternate pair
// Put the block back on the primary pair and forget the alternate one. The
// alternate pins are left as plain inputs for their next owner.
void i2c0ArbiterClearAltPins(void);
bool i2c0ArbiterAltPinsActive(void);

#endif // I2C0_ARBITER_H
