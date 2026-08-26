// SPDX-License-Identifier: MIT
// Display bus layer. Contract: DisplayBus.h.

#include "DisplayBus.h"

#include <Wire.h>
#include "hardware/gpio.h"

#include "Peripherals.h"   // gpio_function_map, gpioState
#include "States.h"        // globalState.config.gpioPythonOwned
#include "config.h"        // jumperlessConfig.top_oled.connection_type
#include "oled.h"          // oled.oledConnected (is Wire1 owned?)
#include "boards/board.h"  // caps.breadboardDisplays

// Soft-I2C pin choice: RP 24/25 (nodes 135/136) - deliberately the pair that
// can NEVER be hardware I2C1 ((24,25) maps to i2c0, the internal DAC/INA
// bus), so soft use here forecloses nothing. (22,23) stays free for SPI,
// (26,27) for the onboard OLED.
static const int SOFT_SDA_PIN = 24;
static const int SOFT_SCL_PIN = 25;

// Half-bit delay: ~2 us -> ~250 kHz, and with push-pull drive the edges are
// nanoseconds - the fabric's pull-up RC rise time (the old marginal-bit
// source) is out of the timing entirely, so this rate is CLEAN, not
// borderline (SSD1306 is specced to 400 kHz; headroom exists if wanted).
// HONEST BUDGET: ~37 us/byte -> a 16-byte chunk (23 on the wire) ~= 0.9 ms;
// the service BURSTS chunks under a time budget every 4 ms, so a delta
// frame lands in one visit and even a full 512-byte sweep takes ~50 ms.
static const int SOFT_HALF_BIT_US = 2;

// ---------------------------------------------------------------------------
// Soft-I2C primitives - PUSH-PULL where the protocol allows it.
//
// This bus has exactly one master (us) and a slave that never drives SDA
// except during ACK and never stretches SCL (SSD1306-family, write-only use).
// So SCL is driven BOTH ways and SDA is driven both ways for data bits -
// the pull-up + fabric RC rise time (the source of the marginal bits that
// wedged the panel mid-byte) is out of the timing entirely. SDA goes
// open-drain (input + pull-up) only for the ACK window, and it is driven
// HIGH first so the pad releases from a solid rail. Bit ops are raw SIO
// (pinMode/digitalWrite cost 1-2 us per bit through the HAL).
// ---------------------------------------------------------------------------

static inline void sdaLow(const DisplayInstance& d) {
    gpio_put(d.sdaPin, 0);
    gpio_set_dir(d.sdaPin, GPIO_OUT);
}
static inline void sdaHigh(const DisplayInstance& d) {
    gpio_put(d.sdaPin, 1);
    gpio_set_dir(d.sdaPin, GPIO_OUT);
}
static inline void sdaRelease(const DisplayInstance& d) {
    gpio_set_dir(d.sdaPin, GPIO_IN);   // ACK window only - internal pull-up
                                       // plus whatever the module carries
}
static inline void sclLow(const DisplayInstance& d) {
    gpio_put(d.sclPin, 0);
    gpio_set_dir(d.sclPin, GPIO_OUT);
}
static inline void sclHigh(const DisplayInstance& d) {
    gpio_put(d.sclPin, 1);
    gpio_set_dir(d.sclPin, GPIO_OUT);
}
static inline bool sdaRead(const DisplayInstance& d) {
    return gpio_get(d.sdaPin);
}

static void softStart(const DisplayInstance& d) {
    sdaHigh(d);
    sclHigh(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sdaLow(d);           // SDA falls while SCL high = START
    delayMicroseconds(SOFT_HALF_BIT_US);
    sclLow(d);
}

static void softStop(const DisplayInstance& d) {
    sdaLow(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sclHigh(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sdaHigh(d);          // SDA rises while SCL high = STOP
    delayMicroseconds(SOFT_HALF_BIT_US);
}

// One byte out, ACK back. Data bits are push-pull both ways; SDA opens up
// (input + pull-up) only for the ACK window, released FROM the high rail.
static bool softWriteByte(const DisplayInstance& d, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1) sdaHigh(d); else sdaLow(d);
        delayMicroseconds(SOFT_HALF_BIT_US);
        sclHigh(d);
        delayMicroseconds(SOFT_HALF_BIT_US);
        sclLow(d);
    }
    // ACK bit: hand SDA to the slave from a solid high.
    sdaHigh(d);
    sdaRelease(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sclHigh(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    bool ack = !sdaRead(d);
    sclLow(d);
    return ack;
}

// The textbook wedged-slave recovery: an interrupted transfer can leave the
// panel mid-byte DRIVING SDA low, where no retry can even form a START (the
// exact lost->alive cycling from Kevin's log - the beacon's pings were doing
// this unstick by accident, 300 ms and one black re-init later). Nine clocks
// with SDA released let the slave finish whatever byte it thinks it is in,
// then a STOP resets its protocol state.
void displayBusUnstick(const DisplayInstance& d) {
    if (d.sdaPin < 0) return;
    sdaHigh(d);
    sdaRelease(d);
    for (int i = 0; i < 9; i++) {
        sclLow(d);
        delayMicroseconds(SOFT_HALF_BIT_US);
        sclHigh(d);
        delayMicroseconds(SOFT_HALF_BIT_US);
    }
    softStop(d);
}

// ---------------------------------------------------------------------------
// Acquire / release
// ---------------------------------------------------------------------------

static bool wire1IsFree(void) {
    // The onboard OLED owns Wire1 in every connection type except 2
    // (internal I2C0). A disconnected OLED frees it too.
    return jumperlessConfig.top_oled.connection_type == 2 || !oled.oledConnected;
}

bool displayBusAcquire(DisplayInstance& d, const char** reasonOut) {
    static const char* claimedReason = "GPIO claimed by your script";
    // Belt for future non-service callers (detect-driver, MP surface): on
    // boards without breadboard displays these pins are OTHER hardware
    // (OG: GPIO 24 = CH446Q RESET, GPIO 25 = the LED strip's data).
    if (!board::currentBoard().caps.breadboardDisplays) {
        if (reasonOut) *reasonOut = "no breadboard displays on this board";
        return false;
    }
    // ALWAYS soft-I2C (sweep finding, high): the hardware branch called
    // Wire1.setSDA(26) on a bus the OLED may have BEGUN on other pins
    // (connect_on_boot starts Wire1 on 6/7 even with no panel detected, and
    // oledConnected flipping false never calls Wire1.end()) - arduino-pico
    // panics on setSDA-while-running with different pins, which manifests
    // as a USB disconnect (the exact crash oled.cpp:4506 documents). The
    // reverse direction (our Wire1.begin(26,27), then the user switches
    // OLED connection type in the menu) breaks oled.cpp's Wire1-exclusivity
    // assumption the same way. Until a real Wire1 arbitration layer exists,
    // the ~1s/frame soft-bus ambient rate is the price of never panicking.
    (void)wire1IsFree;
    d.sdaPin = SOFT_SDA_PIN;
    d.sclPin = SOFT_SCL_PIN;
    if (displayBusUserClaimed(d)) {
        if (reasonOut) *reasonOut = claimedReason;
        d.sdaPin = d.sclPin = -1;
        return false;
    }

    // One-time pin setup for the raw-SIO bit ops: SIO function + pull-up via
    // pinMode (the pull covers SDA's ACK window), then idle the bus at the
    // driven-high rail - push-pull from here on.
    pinMode(d.sdaPin, INPUT_PULLUP);
    pinMode(d.sclPin, INPUT_PULLUP);
    sdaHigh(d);
    sclHigh(d);
    gpio_function_map[d.sdaPin - 20] = GPIO_FUNC_SIO;
    gpio_function_map[d.sclPin - 20] = GPIO_FUNC_SIO;
    // gpioState 6 = "bus role" for the UI/scan, the oled.cpp:4316 precedent.
    // These marks survive Python exits (jl_gpio_claim_pin's do not) and stop
    // refreshConnections re-asserting config pulls under the live bus.
    gpioState[d.sdaPin - 20] = 6;
    gpioState[d.sclPin - 20] = 6;
    return true;
}

void displayBusRelease(DisplayInstance& d) {
    if (d.sdaPin < 0) return;
    if (displayBusUserClaimed(d)) {
        // The USER's script owns these pins now (YIELDED detach) - resetting
        // modes/marks would stomp its live configuration. Just forget them.
        d.sdaPin = d.sclPin = -1;
        return;
    }
    gpio_function_map[d.sdaPin - 20] = GPIO_FUNC_NULL;
    gpio_function_map[d.sclPin - 20] = GPIO_FUNC_NULL;
    gpioState[d.sdaPin - 20] = 0;
    gpioState[d.sclPin - 20] = 0;
    if (d.sdaPin != 26) {   // soft pins: leave them released (inputs)
        pinMode(d.sdaPin, INPUT);
        pinMode(d.sclPin, INPUT);
    }
    d.sdaPin = d.sclPin = -1;
}

bool displayBusUserClaimed(const DisplayInstance& d) {
    if (d.sdaPin < 20 || d.sclPin < 20) return false;
    return globalState.config.gpioPythonOwned[d.sdaPin - 20] ||
           globalState.config.gpioPythonOwned[d.sclPin - 20] ||
           globalState.config.gpioPwmEnabled[d.sdaPin - 20] ||   // PWM on our
           globalState.config.gpioPwmEnabled[d.sclPin - 20];      // pins is a
                                              // claim too (sweep finding)
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

static bool usingHardware(const DisplayInstance& d) {
    return d.sdaPin == 26;
}

bool displayI2cWrite(DisplayInstance& d, uint8_t control,
                     const uint8_t* bytes, uint16_t n) {
    if (usingHardware(d)) {
        Wire1.beginTransmission(d.i2cAddr);
        Wire1.write(control);
        Wire1.write(bytes, n);
        return Wire1.endTransmission() == 0;
    }
    softStart(d);
    bool ok = softWriteByte(d, (uint8_t)(d.i2cAddr << 1));
    if (ok) ok = softWriteByte(d, control);
    for (uint16_t i = 0; ok && i < n; i++) ok = softWriteByte(d, bytes[i]);
    softStop(d);
    return ok;
}

bool displayI2cWriteRaw(DisplayInstance& d, const uint8_t* bytes, uint16_t n) {
    if (usingHardware(d)) {
        Wire1.beginTransmission(d.i2cAddr);
        Wire1.write(bytes, n);
        return Wire1.endTransmission() == 0;
    }
    softStart(d);
    bool ok = softWriteByte(d, (uint8_t)(d.i2cAddr << 1));
    for (uint16_t i = 0; ok && i < n; i++) ok = softWriteByte(d, bytes[i]);
    softStop(d);
    return ok;
}

bool displayI2cPing(DisplayInstance& d, uint8_t addr) {
    if (usingHardware(d)) {
        Wire1.beginTransmission(addr);
        return Wire1.endTransmission() == 0;
    }
    softStart(d);
    bool ack = softWriteByte(d, (uint8_t)(addr << 1));
    softStop(d);
    return ack;
}

int displayI2cCountGhosts(DisplayInstance& d) {
    // A spread of addresses no single panel answers all of; >8 ACKs across
    // the sweep = SDA stuck low (GuideChecks' i2c heuristic, verbatim logic).
    int acks = 0;
    for (uint8_t a = 0x08; a <= 0x70; a += 0x08) {
        if (displayI2cPing(d, a)) acks++;
    }
    return acks;
}
