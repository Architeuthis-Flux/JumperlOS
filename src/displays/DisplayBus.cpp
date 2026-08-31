// SPDX-License-Identifier: MIT
// Display bus layer. Contract: DisplayBus.h.

#include "DisplayBus.h"

#include <Wire.h>
#include "hardware/gpio.h"

#include "Peripherals.h"   // gpioState, gpioPWMEnabled, stopPWM
#include "States.h"        // globalState.config.gpioPythonOwned
#include "config.h"        // jumperlessConfig.top_oled.connection_type
#include "oled.h"          // oled.oledConnected (is Wire1 owned?)
#include "boards/board.h"  // caps.breadboardDisplays

// PWM's other RAM-side flag: Peripherals.h exports gpioPWMEnabled but not the
// slow-PWM twin, and telling a LIVE claim from a slot-restored ghost needs
// both (see dropGhostPwmClaim).
extern bool gpioSlowPWMEnabled[10];

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

// A persisted gpioPwmEnabled with no PWM behind it is a GHOST claim: the flag
// rides the slot YAML (States.cpp:2247/2394) but setupPWM is never re-run on
// load, so a slot that remembers pwm(5) paused a perfectly live panel forever
// with "claimed by your script" and no script anywhere (sweep finding). The
// RAM-side flags are what an actual setupPWM/setupSlowPWM sets, and they are
// set BEFORE the config flag, so config-set + both-RAM-clear is exactly the
// ghost. Newer owner wins: the flag yields and the YAML heals on the next save.
static void dropGhostPwmClaim(int idx) {
    if (!globalState.config.gpioPwmEnabled[idx]) return;
    if (gpioPWMEnabled[idx] || gpioSlowPWMEnabled[idx]) return;   // really running
    globalState.config.gpioPwmEnabled[idx] = false;
    globalState.markDirty();
}

// Does PWM hold this pin RIGHT NOW? The RAM flags are the honest answer once
// the ghost is dropped (config-set implies RAM-set after that), and they also
// catch the reverse skew - a slot load that clears the config flag out from
// under a PWM that is still running.
static bool pwmHoldsPin(int idx) {
    dropGhostPwmClaim(idx);
    return gpioPWMEnabled[idx] || gpioSlowPWMEnabled[idx];
}

// One pin, one answer. Claims are PER PIN everywhere they are made
// (jl_gpio_claim_pin takes a pin, gpioPwmEnabled is indexed) - the old
// all-or-nothing instance test is what left an unclaimed sibling half-owned.
static bool pinClaimed(int pin) {
    if (pin < 20 || pin > 27) return false;
    int idx = pin - 20;
    return globalState.config.gpioPythonOwned[idx] || pwmHoldsPin(idx);
}

// machine.PWM(24) - the RAW-INT form. mp_hal_get_pin_obj() takes a bare int,
// so no machine.Pin is constructed and nothing registered a claim: the bus
// kept bit-banging SIO into a PWM-muxed pad ("8 bus errors - re-beaconing",
// forever). Called from lib/micropython/port/machine_pwm_jl.c right after
// gpio_set_function(PWM), so machine.PWM lands in the SAME bookkeeping
// jumperless.pwm() uses and the service yields on its next tick. It lives
// here because this file is the pin-arbitration point and the MicroPython C
// side cannot see globalState.
extern "C" void jl_display_bus_pwm_taken(int pin) {
    if (!board::currentBoard().caps.breadboardDisplays) return;
    if (pin != SOFT_SDA_PIN && pin != SOFT_SCL_PIN) return;
    int idx = pin - 20;
    gpioPWMEnabled[idx] = true;                     // RAM side first, like
    globalState.config.gpioPwmEnabled[idx] = true;  // setupPWM - a LIVE claim,
    globalState.markDirty();                        // never a ghost
    // Newer owner wins: drop OUR bus-role mark on this pin so setGPIO() and
    // readGPIO() stop treating it as display plumbing. The sibling pin keeps
    // its mark until the (per-pin) release.
    if (gpioState[idx] == 6) gpioState[idx] = 0;
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

    // OWNERSHIP OF GP24/GP25, in one place (Kevin's rule: last one wins, and
    // no refusals between the display and PWM - "allow the user to mess it up
    // if they assign pwm on a display bus"):
    //   * A script's machine.Pin claim wins outright: we refuse here, the
    //     service yields with one line, and the claim clears itself on script
    //     exit (jl_gpio_release_all_pins) so the poll resumes us.
    //   * Display vs PWM is LAST WINS in BOTH directions. Acquiring STOPS
    //     whatever PWM holds the pin (stopPWM kills the slice, the config
    //     flag and the pad function - regular and slow alike); a PWM set up
    //     afterwards takes the pin straight back, and both entry points
    //     (setupPWM's config flag, machine.PWM's jl_display_bus_pwm_taken)
    //     make the service yield before it touches the wire.
    //   * A persisted PWM flag with NO live PWM behind it is a ghost from a
    //     slot load and yields to any live owner (dropGhostPwmClaim).
    //   * Release is per pin, and the gpioState 6 marks never outlive the
    //     attachment - they are our bookkeeping, not the claimant's.
    for (int k = 0; k < 2; k++) {
        int pin = (k == 0) ? d.sdaPin : d.sclPin;
        int idx = pin - 20;
        // A Pin claim outranks us, so don't kill a PWM we're about to refuse
        // over anyway.
        if (pwmHoldsPin(idx) && !globalState.config.gpioPythonOwned[idx]) {
            stopPWM(pin - 19);   // stopPWM speaks GPIO 1-8, not RP pin numbers
            Serial.print("\r\nDISPLAY took GP");
            Serial.print(pin);
            Serial.println(" back from PWM (newer owner wins)");
        }
    }
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
    // gpioState 6 = "bus role" for the UI/scan, the oled.cpp:4316 precedent.
    // These marks survive Python exits (jl_gpio_claim_pin's do not) and stop
    // refreshConnections re-asserting config pulls under the live bus.
    gpioState[d.sdaPin - 20] = 6;
    gpioState[d.sclPin - 20] = 6;
    return true;
}

void displayBusRelease(DisplayInstance& d) {
    if (d.sdaPin < 0) return;
    // PER PIN (sweep finding): the skip used to be all-or-nothing across both
    // pins while claims are per-pin, so a PWM or Pin claim on ONE pin made us
    // forget the OTHER while it was still a driven output - nothing ever
    // released it. And gpioState 6 is OUR bus-role mark, never the claimant's:
    // always drop it, or setGPIO() and readGPIO() stay diverted on that pin
    // for the rest of the session. Dropping it is safe under a live claim
    // because gpioPythonOwned/gpioPwmEnabled gate those paths already.
    bool hardwareBus = (d.sdaPin == 26);
    for (int k = 0; k < 2; k++) {
        int pin = (k == 0) ? d.sdaPin : d.sclPin;
        if (pin < 20 || pin > 27) continue;
        gpioState[pin - 20] = 0;
        if (pinClaimed(pin)) continue;   // the new owner's pad is its own now
        if (!hardwareBus) pinMode(pin, INPUT);   // soft pins: leave them released
    }
    d.sdaPin = d.sclPin = -1;
}

bool displayBusUserClaimed(const DisplayInstance& d) {
    // PWM on our pins is a claim too (sweep finding), and it is checked per
    // pin - a claim on either one pauses the bus, but only the claimed pin is
    // treated as the claimant's on release.
    return pinClaimed(d.sdaPin) || pinClaimed(d.sclPin);
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
