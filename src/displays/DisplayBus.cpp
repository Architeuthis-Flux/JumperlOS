// SPDX-License-Identifier: MIT
// Display bus layer. Contract: DisplayBus.h.

#include "DisplayBus.h"

#include <Wire.h>
#include "hardware/gpio.h"

#include "Peripherals.h"   // gpio_function_map, gpioState
#include "States.h"        // globalState.config.gpioPythonOwned
#include "config.h"        // jumperlessConfig.top_oled.connection_type
#include "oled.h"          // oled.oledConnected (is Wire1 owned?)

// Soft-I2C pin choice: RP 24/25 (nodes 135/136) - deliberately the pair that
// can NEVER be hardware I2C1 ((24,25) maps to i2c0, the internal DAC/INA
// bus), so soft use here forecloses nothing. (22,23) stays free for SPI,
// (26,27) for the onboard OLED.
static const int SOFT_SDA_PIN = 24;
static const int SOFT_SCL_PIN = 25;

// Half-bit delay: ~10 us -> ~50 kHz, the fabric-reliable rate the i2cscrn
// README landed on for long crossbar paths.
static const int SOFT_HALF_BIT_US = 10;

// ---------------------------------------------------------------------------
// Soft-I2C primitives (open-drain emulation: LOW = drive, HIGH = release)
// ---------------------------------------------------------------------------

static inline void sdaLow(const DisplayInstance& d) {
    pinMode(d.sdaPin, OUTPUT);
    digitalWrite(d.sdaPin, LOW);
}
static inline void sdaRelease(const DisplayInstance& d) {
    pinMode(d.sdaPin, INPUT_PULLUP);   // internal pull is a courtesy only -
                                       // a real bus needs external ~4.7k
}
static inline void sclLow(const DisplayInstance& d) {
    pinMode(d.sclPin, OUTPUT);
    digitalWrite(d.sclPin, LOW);
}
static inline void sclRelease(const DisplayInstance& d) {
    pinMode(d.sclPin, INPUT_PULLUP);
}
static inline bool sdaRead(const DisplayInstance& d) {
    return digitalRead(d.sdaPin) != 0;
}

static void softStart(const DisplayInstance& d) {
    sdaRelease(d);
    sclRelease(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sdaLow(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sclLow(d);
}

static void softStop(const DisplayInstance& d) {
    sdaLow(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sclRelease(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sdaRelease(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
}

// One byte out, ACK back. Clock stretching honored with a bounded wait.
static bool softWriteByte(const DisplayInstance& d, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1) sdaRelease(d); else sdaLow(d);
        delayMicroseconds(SOFT_HALF_BIT_US);
        sclRelease(d);
        delayMicroseconds(SOFT_HALF_BIT_US);
        sclLow(d);
    }
    // ACK bit
    sdaRelease(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    sclRelease(d);
    delayMicroseconds(SOFT_HALF_BIT_US);
    bool ack = !sdaRead(d);
    sclLow(d);
    return ack;
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
    if (wire1IsFree()) {
        d.sdaPin = 26;
        d.sclPin = 27;
    } else {
        d.sdaPin = SOFT_SDA_PIN;
        d.sclPin = SOFT_SCL_PIN;
    }
    if (displayBusUserClaimed(d)) {
        if (reasonOut) *reasonOut = claimedReason;
        d.sdaPin = d.sclPin = -1;
        return false;
    }

    bool hw = wire1IsFree();
    if (hw) {
        Wire1.setSDA(d.sdaPin);
        Wire1.setSCL(d.sclPin);
        Wire1.begin();
        Wire1.setClock(100000);   // fabric-safe starting rate
        gpio_function_map[d.sdaPin - 20] = GPIO_FUNC_I2C;
        gpio_function_map[d.sclPin - 20] = GPIO_FUNC_I2C;
    } else {
        sdaRelease(d);
        sclRelease(d);
        gpio_function_map[d.sdaPin - 20] = GPIO_FUNC_SIO;
        gpio_function_map[d.sclPin - 20] = GPIO_FUNC_SIO;
    }
    // gpioState 6 = "bus role" for the UI/scan, the oled.cpp:4316 precedent.
    // These marks survive Python exits (jl_gpio_claim_pin's do not) and stop
    // refreshConnections re-asserting config pulls under the live bus.
    gpioState[d.sdaPin - 20] = 6;
    gpioState[d.sclPin - 20] = 6;
    return true;
}

void displayBusRelease(DisplayInstance& d) {
    if (d.sdaPin < 0) return;
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
           globalState.config.gpioPythonOwned[d.sclPin - 20];
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
