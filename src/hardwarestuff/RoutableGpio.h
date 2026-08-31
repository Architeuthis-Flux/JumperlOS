// SPDX-License-Identifier: MIT
#ifndef ROUTABLE_GPIO_H
#define ROUTABLE_GPIO_H

// RoutableGpio - the V5's user-routable GPIO bank (8 breadboard GPIOs + UART
// Tx/Rx). Phase 1a of the RoutableGpio consolidation (CodeDocs/GPIO_plan.md):
// a pure mechanical move out of Peripherals.cpp/.h and PersistentStuff.cpp -
// zero behavior change. Peripherals.h includes this header so every existing
// consumer compiles unchanged.

#include <Arduino.h>
#include "hardware/gpio.h" // gpio_function_t
#include "pico/time.h"     // repeating_timer_t

// GPIO arrays: 10 real RP2040 GPIOs + 8 fake outputs + 32 fake inputs = 50 total
// Index layout:
//   0-9:   Real RP2040 GPIOs (RP_GPIO_20 through RP_GPIO_27, etc.)
//   10-17: Fake GP Outputs (FAKE_GP_OUT_0 through FAKE_GP_OUT_7)
//   18-49: Fake GP Inputs (FAKE_GP_IN_0 through FAKE_GP_IN_31)
extern uint8_t gpioState[50];
extern uint8_t gpioReading[50];
extern uint8_t gpioReadFloating[10];
extern int gpioNet[50];

extern uint32_t gpioReadingColors[50];  // 10 real + 8 fake out + 32 fake in

// PWM state tracking
extern float gpioPWMFrequency[10];
extern float gpioPWMDutyCycle[10];
extern bool gpioPWMEnabled[10];

// Slow PWM state tracking (for frequencies below 10Hz). These had no header
// declaration before the move (DisplayBus.cpp carried its own extern for
// gpioSlowPWMEnabled); declared here as a set for uniformity.
extern bool gpioSlowPWMEnabled[10];
extern repeating_timer_t gpioSlowPWMTimers[10];
extern volatile uint32_t gpioSlowPWMCounter[10];
extern volatile uint32_t gpioSlowPWMPeriod[10];
extern volatile uint32_t gpioSlowPWMDutyTicks[10];

extern volatile bool readingGPIO;

// Pin-aware function name lookup (handles F9 ambiguity per RP2350 GPIO mux).
// The name table itself is file-local to RoutableGpio.cpp - nothing else
// reads it.
const char* gpio_function_name_for_pin( uint gpio, gpio_function_t function );

// Register truth for the routable bank: the pin's current mux function, read
// live from the pads (gpio_get_function) - there is no shadow copy anymore.
// idx is a gpioDef index 0..9; out of range returns GPIO_FUNC_NULL.
gpio_function_t routableGpioFunction( int idx );

// gpioDef[i][0] is the pin number
// gpioDef[i][1] is the RP_GPIO_x define
// gpioDef[i][2] is the index of the gpioState array
// (defined once in RoutableGpio.cpp - was a per-TU copy in Peripherals.h)
extern const int gpioDef[10][3];

// Apply the persisted direction/pull config to the whole bank (called from
// initDAC() at boot). Had no declaration anywhere before the move - it linked
// only because its sole caller sat below it in the same TU.
void initGPIO(void);

int getGPIOIndexFromPin(int pin);

int anythingInteractiveConnected(int net = -1);
int anyGpioOutputConnected(int net = -1);
int anyGpioInputConnected(int net = -1);
int anyAdcConnected(int net = -1); // returns adc number

void setGPIO(void);
void readGPIO(void);
void printGPIOState(Stream* target = &Serial);

// gpio = -1 means toggle the brightened net
// lowHigh = 2 means toggle
int toggleGPIO(int lowHigh = 2, int gpio = -1, int onlyCheck = 0);
int probeToggle(int buttonState = -1);
void erattaClearGPIO(int gpio = -1);

int gpioReadWithFloating(int pin, unsigned long usDelay = 10);

// PWM functions
int setupPWM(int gpio_pin, float frequency = 1000.0, float duty_cycle = 0.5);
int setPWMDutyCycle(int gpio_pin, float duty_cycle);
int setPWMFrequency(int gpio_pin, float frequency);
int stopPWM(int gpio_pin);

// Slow PWM functions (for frequencies below 10Hz)
int setupSlowPWM(int gpio_pin, float frequency, float duty_cycle);
int setSlowPWMDutyCycle(int gpio_pin, float duty_cycle);
int setSlowPWMFrequency(int gpio_pin, float frequency);
int stopSlowPWM(int gpio_pin);

void updateGPIOConfigFromState(void);
// Apply the persisted direction/pull config to hardware + gpioState.
// onlyIdx >= 0 applies exactly that gpioDef index (what the probe GPIO menu
// wants after changing one pin); -1 applies the whole SIO bank. Outputs get
// gpioState = 0 ("starts low"), so bank-wide calls must not run while the
// probe's GPIO buffer-power claim is active (the claim pin is skipped).
void updateStateFromGPIOConfig(int onlyIdx = -1);

// The readSettingsFromConfig() GPIO block, extracted verbatim (Phase 1a):
// applies the persisted direction/pull config to the routable bank at boot
// and on every config change. No-op on OG (central guard inside).
void applyGpioSettingsFromConfig(void);

// The applyStateToHardware() GPIO loop (routing/States.cpp), moved here so
// the module's central OG guard covers it. Runs on every slot load. Its
// semantics differ from updateStateFromGPIOConfig() on purpose - only skip is
// bus-role pins (gpioState 6), pulls apply even to outputs, and outputs are
// restored to their loaded level via gpio_put(pin, gpioState[i]).
void applyStateGpioToHardware(void);

// THE mutation funnel: apply globalState.config for one gpioDef index (0..9)
// to hardware + gpioState[], then markDirty() for the slot autosave. Every
// future mutation surface (options carousel, GPIO settings app, BCD counter)
// goes through this. No-op on OG (central guard) and on out-of-range idx.
void applyPinConfig(int idx);

// May the user assign this pin? False when something owns it; then *ownerOut
// (if given) points at a short static literal naming the owner - one of
// "index", "board", "python", "probe power", "serial lock", "routing",
// "OLED", "PWM", "UART". Checked most specific owner first. Deliberately no
// user-bridge check: a user bridge on the node is the expected state for
// assignment, not a conflict.
bool routableGpioAvailable(int idx, const char** ownerOut = nullptr);

// ============================================================================
// BCD/binary counter (Phase 3, CodeDocs/GPIO_plan.md). The range lives in
// globalState.config (bcdStart/bcdWidth/bcdMode/bcdValue, persisted per-slot);
// these drive it onto the pins.
// ============================================================================

// Counter bit k (LSB-first) -> gpioDef index: bcdStart + k walks GPIO 1-8
// (indices 0-7) and continues into UART Tx (8) and Rx (9) as the top bits.
// Returns -1 when no range is set, k is outside the width, or the bit falls
// past the end of the bank.
int bcdBitIndex(int bit);

// Encode bcdValue onto the range pins: forces each claimable range pin to an
// SIO output through applyPinConfig() (displacing leftover PWM) and drives
// its bit level, mirroring gpioState[] so LEDs/readouts agree. Pins something
// else owns are skipped. No-op when bcdStart < 0 or on OG.
void bcdApply(void);

// Largest value the configured range can show: binary = 2^width - 1; BCD =
// all-9s over the full nibbles, with a partial top nibble capped at
// min(9, 2^bits - 1) as the top digit.
int bcdMaxValue(void);

// bcdValue = wrap(bcdValue + delta) into [0, bcdMaxValue()] (wraps BOTH
// directions), applied live to the pins and marked dirty for the slot
// autosave. Returns the new value, or -1 (untouched, not dirtied) when no
// range is configured (bcdStart < 0) - callers toast instead of counting.
int bcdIncrement(int delta);

// Pure-logic assert run over the encode/wrap math (no pin writes, safe on
// OG): prints pass/fail per case to `out`, returns overall pass. Reachable
// from the serial test path: 'D' status menu -> "BCD SelfCheck".
bool bcdSelfCheck(Stream* out = &Serial);

// Blocking counter modal (VoltageAdjuster::adjust idiom): encoder counts the
// value live on the pins, click-release/probe-connect confirms (returns the
// value, >= 0), hold/probe-remove/serial byte cancels and restores the entry
// value (-1). Returns -2 immediately when no range is configured - the
// caller routes to bcdRangeSetup().
int bcdAdjust(void);

// Two-step blocking range pick (same modal idiom): step 1 start pin (skips
// pins routableGpioAvailable() refuses, PWM excepted - bcdApply displaces
// it), step 2 width (1..contiguous claimable pins from that start). Applies
// via setBcdRange() + bcdApply() and returns 0; cancel = -1, nothing changed.
int bcdRangeSetup(void);

#endif
