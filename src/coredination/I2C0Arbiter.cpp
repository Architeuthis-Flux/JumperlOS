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
#include "I2C0Arbiter.h"
#include "hardware/gpio.h"
#include "pico/error.h"
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

// ---- alternate pin pair (I2C0Arbiter.h) -----------------------------------
static int s_altSda = -1, s_altScl = -1, s_priSda = -1, s_priScl = -1;
static int s_altAddr = -1;
static bool s_altLive = false;   // the block currently sits on the alternate pair

// The block's SDA/SCL inputs follow whichever pins carry its function. With
// no pin assigned they read low, so un-muxing a pair before muxing the other
// in drops both lines and raises them again in register-write order - which
// the master can take for a START it never saw a STOP for, after which it
// holds every later command in its FIFO waiting for a free bus (seen
// 2026-09-24: INA219 reads timing out at 1 s with the block idle and a
// command queued). So: the incoming pair is muxed in first, SCL before SDA,
// while the outgoing pair still holds the lines high; then the outgoing pair
// is parked. Both buses idle high, so the overlap is edge-free.
static void arbMuxIn(int sda, int scl) {
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_set_function(sda, GPIO_FUNC_I2C);
}
static void arbMuxOut(int sda, int scl) {
    // Parked as a plain input: its own pull-ups hold the lines idle-high for
    // the devices on it while the block talks on the other pair.
    gpio_set_function(scl, GPIO_FUNC_SIO);
    gpio_set_dir(scl, false);
    gpio_set_function(sda, GPIO_FUNC_SIO);
    gpio_set_dir(sda, false);
}

// Before a transaction: the pair this address lives on gets the block, and
// only that pair. Checked against the pins' real function select every time,
// not against s_altLive: TwoWire's timeout recovery (end + begin) and any
// Wire.begin() re-mux the primary pair on their own, which would leave both
// pairs assigned to the block - the one state that breaks the bus.
static inline void arbRoute(i2c_inst_t *i2c, uint8_t addr) {
    if (i2c != i2c0 || s_altSda < 0) return;
    bool wantAlt = (s_altAddr >= 0 && addr == (uint8_t)s_altAddr);
    int offSda = wantAlt ? s_priSda : s_altSda, offScl = wantAlt ? s_priScl : s_altScl;
    int onSda = wantAlt ? s_altSda : s_priSda, onScl = wantAlt ? s_altScl : s_priScl;
    if (gpio_get_function(onSda) != GPIO_FUNC_I2C || gpio_get_function(onScl) != GPIO_FUNC_I2C) {
        arbMuxIn(onSda, onScl);
    }
    if (gpio_get_function(offSda) != GPIO_FUNC_SIO || gpio_get_function(offScl) != GPIO_FUNC_SIO) {
        arbMuxOut(offSda, offScl);
    }
    s_altLive = wantAlt;
}

// A transaction that timed out with a pair registered: the block may be
// waiting on a bus it wrongly believes busy. Disable/enable resets its state
// machine and FIFOs (the SDK's own abort path leaves those alone).
static inline void arbRecover(i2c_inst_t *i2c, int r) {
    if (i2c != i2c0 || s_altSda < 0 || r != PICO_ERROR_TIMEOUT) return;
    i2c->hw->enable = 0;
    i2c->hw->enable = 1;
}

void i2c0ArbiterSetAltPins(int altSda, int altScl, int priSda, int priScl) {
    i2c0ArbiterClearAltPins();
    s_altSda = altSda; s_altScl = altScl; s_priSda = priSda; s_priScl = priScl;
    arbMuxOut(altSda, altScl);   // parked until its first transaction
}
void i2c0ArbiterSetAltAddr(int addr) { s_altAddr = addr; }
void i2c0ArbiterClearAltPins(void) {
    if (s_altSda >= 0) {
        arbMuxIn(s_priSda, s_priScl);    // overlap first, as arbRoute does
        arbMuxOut(s_altSda, s_altScl);
    }
    s_altLive = false;
    s_altSda = s_altScl = s_priSda = s_priScl = -1;
    s_altAddr = -1;
}
bool i2c0ArbiterAltPinsActive(void) { return s_altSda >= 0; }

// A failed transaction (NAK, timeout) is aborted by the SDK - the bus is
// free again, whatever nostop said - so it always releases.
extern "C" int __wrap_i2c_write_blocking_until(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop, absolute_time_t until) {
    arbRoute(i2c, addr);
    bool paused = arbEnter(i2c);
    int r = __real_i2c_write_blocking_until(i2c, addr, src, len, nostop, until);
    arbRecover(i2c, r);
    arbLeave(i2c, paused, nostop && (r >= 0));
    return r;
}

extern "C" int __wrap_i2c_read_blocking_until(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop, absolute_time_t until) {
    arbRoute(i2c, addr);
    bool paused = arbEnter(i2c);
    int r = __real_i2c_read_blocking_until(i2c, addr, dst, len, nostop, until);
    arbRecover(i2c, r);
    arbLeave(i2c, paused, nostop && (r >= 0));
    return r;
}

// WaveGen's monitor: drop a held pause (see above).
extern "C" void i2c0ArbiterForceRelease(void) { s_held = false; }
