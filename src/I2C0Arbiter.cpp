// SPDX-License-Identifier: MIT
//
// The I2C0 arbiter (T3.3, CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md C14):
// while WaveGen's DMA stream owns I2C0, every foreign Wire master
// transaction on that bus pauses the stream at a sample boundary and
// resumes it afterwards. arduino-pico's TwoWire issues every master write
// through i2c_write_blocking_until() and every read through
// i2c_read_blocking_until() (Wire.cpp endTransmission / requestFrom), so
// wrapping those two SDK symbols catches every driver on the bus - the
// INA219s (a registry library, not ours to edit), the MCP4728 (dac_set, the
// probe feed park, calibration), the SSD1306, MicroPython's machine.I2C -
// without touching any of them. Same linker-wrap pattern as FlashPark's
// flash_range_* and IrqSlots' irq_add_shared_handler; the flags live on one
// line in platformio.ini with JL_I2C0_ARBITER_WRAPPED so a build without the
// wraps fails to link here instead of shipping without the arbiter.
//
// Not covered: TwoWire's zero-length probe (beginTransmission +
// endTransmission with no data) bit-bangs the pins as GPIO and never enters
// the SDK; it happens in begin()/isConnected() paths (MCP4728::begin,
// INA219::begin, I2C scanners) - WaveGen::begin() stops the stream before
// its own, and the scanners are debug commands.
//
// The other I2C0 users that already skip on wavegen.isRunning() (the INA
// poll, the OLED, the net-voltage scan, probing's INA1 reads) keep doing so:
// that is the cheaper gate - it costs the wave nothing - and this arbiter is
// for the calls that must return the real thing (a REPL dac_set() or
// ina_get_current() during a stream: a ~50-100 us dip in the wave, not a
// collision).
#include <Arduino.h>
#include "hardware/i2c.h"

#ifndef JL_I2C0_ARBITER_WRAPPED
#error "I2C0Arbiter.cpp needs -Wl,--wrap,i2c_write_blocking_until -Wl,--wrap,i2c_read_blocking_until (see platformio.ini)"
#endif

extern "C" bool wavegenBusPause(void);    // WaveGen.cpp: true = the stream was live and is now paused
extern "C" void wavegenBusResume(void);

extern "C" int __real_i2c_write_blocking_until(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop, absolute_time_t until);
extern "C" int __real_i2c_read_blocking_until(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop, absolute_time_t until);

// A transaction that ends WITHOUT a STOP (nostop: Wire's endTransmission(false),
// the register-pointer write before a requestFrom) leaves the bus mid-
// transaction, addressed to the foreign device; the stream must stay paused
// until the transaction that follows it ends with a STOP, or the DAC's bytes
// would be appended to the foreign device's transfer. `s_held` carries the
// pause across that pair (same core, sequential - TwoWire has no cross-core
// use). WaveGen's monitor releases a pause held for > 100 ms (a nostop write
// whose partner never came) so a stream can never stay paused forever.
static volatile bool s_held = false;

static inline bool arbEnter(i2c_inst_t *i2c) {
    if (i2c != i2c0) return false;
    if (s_held) return true;
    return wavegenBusPause();
}
static inline void arbLeave(i2c_inst_t *i2c, bool paused, bool nostop) {
    if (i2c != i2c0 || !paused) return;
    if (nostop) { s_held = true; return; }
    s_held = false;
    wavegenBusResume();
}

// A failed transaction (NAK, timeout) is aborted by the SDK - the bus is
// free again, whatever nostop said - so it always releases.
extern "C" int __wrap_i2c_write_blocking_until(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop, absolute_time_t until) {
    bool paused = arbEnter(i2c);
    int r = __real_i2c_write_blocking_until(i2c, addr, src, len, nostop, until);
    arbLeave(i2c, paused, nostop && (r >= 0));
    return r;
}

extern "C" int __wrap_i2c_read_blocking_until(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop, absolute_time_t until) {
    bool paused = arbEnter(i2c);
    int r = __real_i2c_read_blocking_until(i2c, addr, dst, len, nostop, until);
    arbLeave(i2c, paused, nostop && (r >= 0));
    return r;
}

// WaveGen's monitor: drop a held pause (see above).
extern "C" void i2c0ArbiterForceRelease(void) { s_held = false; }
