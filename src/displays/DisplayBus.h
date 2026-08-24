// SPDX-License-Identifier: MIT
#ifndef DISPLAY_BUS_H
#define DISPLAY_BUS_H

#include <Arduino.h>
#include <stdint.h>

#include "DisplayTypes.h"

// The display service's bus layer: soft-I2C (open-drain bit-bang on routed
// GPIOs) or hardware Wire1, chosen per Kevin's ruling - software I2C whenever
// the onboard OLED owns hardware I2C1 (its shipping default), Wire1 on
// (26,27) only when the OLED sits on internal I2C0. Separate buses mean no
// 0x3C collision and no OLED parking, ever.
//
// Soft-I2C electrical model: open-drain emulation (drive LOW as output,
// release HIGH as input) at ~50 kHz - inside the demonstrated fabric
// envelope. External pull-ups (~4.7k) are REQUIRED on a bare panel; the #1
// failure mode is a missing pull-up and it LOOKS like success (every address
// ACKs because SDA never rises) - the beacon's >8-ACK heuristic names it.

// Pick pins + peripheral for this instance and mark them
// (gpio_function_map/gpioState - the oled.cpp:4316 precedent, which survives
// Python exits; jl_gpio_claim_pin does not). Returns false with a reason
// when the pins are user-claimed.
bool displayBusAcquire(DisplayInstance& d, const char** reasonOut);
void displayBusRelease(DisplayInstance& d);

// True when a user script has claimed one of this instance's pins
// (gpioPythonOwned) - the YIELDED trigger.
bool displayBusUserClaimed(const DisplayInstance& d);

// One I2C write: true on ACK. reg = 0x00 (command) / 0x40 (data stream).
bool displayI2cWrite(DisplayInstance& d, uint8_t control,
                     const uint8_t* bytes, uint16_t n);

// One transaction with the payload verbatim (no control byte prepended) -
// for merged command+data chunks built with 0x80 continuation controls,
// which is what keeps a soft-bus chunk to ONE start/addr/stop overhead.
bool displayI2cWriteRaw(DisplayInstance& d, const uint8_t* bytes, uint16_t n);

// Address ping (start + address + stop). Used by the beacon.
bool displayI2cPing(DisplayInstance& d, uint8_t addr);

// The stuck-SDA honesty scan: how many of the display-class addresses ACK.
// >8 on a full scan means SDA can't rise (no pull-up) - the beacon prints
// the diagnosis instead of "detecting" a phantom panel.
int displayI2cCountGhosts(DisplayInstance& d);

#endif // DISPLAY_BUS_H
