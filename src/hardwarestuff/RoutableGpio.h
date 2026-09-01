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
// True while a GPIO/BCD on-board UI is open. Those UIs render to the OLED
// and never paint the LED matrix, so core 1 must keep showing the circuit
// even though inClickMenu is set (see the gate in main.cpp).
extern volatile bool g_gpioUiShowsCircuit;

bool routableGpioAvailable(int idx, const char** ownerOut = nullptr);

// ============================================================================
// Binary counter (Phase 3, CodeDocs/GPIO_plan.md). The range lives in
// globalState.config (bcdPins/bcdValue, persisted per-slot); these drive it
// onto the pins. The range is a pin MASK - bit i = gpioDef index i (GPIO
// 1-8 = bits 0-7, UART Tx = 8, Rx = 9), so the counter spans routed-but-
// non-contiguous pins without phantom bits. The encoding is always plain
// binary; the READOUT is hexadecimal (one uppercase digit per 4 bits of the
// pin count) - there is no decimal/BCD-nibble mode.
// ============================================================================

// Counter bit k (LSB-first) -> gpioDef index: the k-th SET bit of
// config.bcdPins, LSB-first. Returns -1 when no mask is set or k is past
// the mask's pin count.
int bcdBitIndex(int bit);

// Encode bcdValue onto the mask's pins: forces each claimable mask pin to an
// SIO output through applyPinConfig() (displacing leftover PWM) and drives
// its bit level, mirroring gpioState[] so LEDs/readouts agree. Pins something
// else owns are skipped. No-op when bcdPins == 0 or on OG.
//
// fromLoad = true is the LOAD-PATH re-drive (applyStateGpioToHardware /
// applyGpioSettingsFromConfig): it sets mux/dir/level inline instead of
// going through applyPinConfig(), so it never markDirty()s a state that was
// just read off disk (that dirty made the idle autosave rewrite the slot on
// every boot and left read-only templates nagging "unsaved edits"). It also
// never displaces PWM - stopPWM() marks dirty - so a range pin PWM owns is
// left undriven until the next user-initiated count.
void bcdApply(bool fromLoad = false);

// Compose the counter's value from the LIVE pin levels rather than the
// stored field, so a probe toggle / pad action / MicroPython write shows up
// in the readouts and counts from what is actually on the pins. An output's
// level is gpio_get_out_level() (the driven truth readGPIO/toggleGPIO use);
// an input reads gpio_get(). A bit whose pin is not SIO, or that something
// else owns (bcdApply skipped it), reads 0. Returns 0 when bcdPins == 0.
int bcdReadValue(void);

// True when idx is a live OUTPUT bit of the configured counter. readGPIO's
// sweep checks this so it never stamps a driven counter bit back to an input.
bool bcdOwnsPin(int idx);

// The counter readout: uppercase hex, zero-padded to one digit per 4 bits
// of the configured pin count ("0".."F" at 4 pins, "00".."FF" at 8,
// "000".."3FF" at 10). Every count display goes through this.
void bcdFormatValue(int value, char* buf, size_t bufLen);

// Largest value the configured mask can show: 2^popcount(bcdPins) - 1.
int bcdMaxValue(void);

// value = wrap(bcdReadValue() + delta) into [0, bcdMaxValue()] (wraps BOTH
// directions), applied live to the pins and marked dirty for the slot
// autosave. The arithmetic starts from the LIVE pin levels, not the stored
// field, so a manual toggle followed by a pad tap counts from what is
// actually there. Returns the new value, or -1 (untouched, not dirtied) when
// no range is configured (bcdPins == 0) - callers toast instead of counting.
int bcdIncrement(int delta);

// Pure-logic assert run over the encode/wrap math (no pin writes, safe on
// OG): prints pass/fail per case to `out`, returns overall pass. Reachable
// from the serial test path: 'D' status menu -> "BCD SelfCheck".
bool bcdSelfCheck(Stream* out = &Serial);

// Blocking counter modal (VoltageAdjuster::adjust idiom). Entry READS the
// current value off the pins; the encoder then counts it live. The counter
// IS the pins, so every exit keeps what is on them - click/probe-connect
// returns the value (>= 0), hold/probe-remove/serial byte just leaves (-1).
// Nothing reverts.
//
// No range configured? It DEFINES one instead of asking (Kevin: "just click
// BCD and then scrolling the wheel updates them live"). The default is
// every ROUTED claimable pin (Kevin: "make the BCD counter default to all
// the gpio pins that are currently routed") - preferredStart only matters
// when nothing is routed, which falls back to the old shape: a contiguous
// claimable run from preferredStart (or the lowest claimable pin), capped
// at 4 bits (one hex digit). Returns -1 without a range only when no pin is
// claimable at all, or on OG.
int bcdAdjust(int preferredStart = -1);

// Two-step blocking range pick (same modal idiom): step 1 start pin (skips
// pins routableGpioAvailable() refuses, PWM excepted - bcdApply displaces
// it), step 2 width (1..contiguous claimable pins from that start). This
// picker builds CONTIGUOUS masks only; the sparse routed-pin default comes
// from bcdAdjust's auto-define. Applies via setBcdPins() + bcdApply() and
// returns 0; cancel = -1, nothing changed. This is the EXPLICIT "Range"
// item only - no path forces a user through it.
int bcdRangeSetup(void);

// GPIO options carousel (Phase 2, CodeDocs/GPIO_plan.md): the highlight
// gate's turn-to-configure modal, same VoltageAdjuster::adjust idiom as the
// BCD modals above. Items Direction - PWM - Pulls - BCD (PWM skipped on idx
// 8/9 - the PWM functions validate gpio_pin 1-8 only); encoder scrolls,
// click/probe-connect enters an item's sub-editor, hold/probe-remove/serial
// byte exits. Every mutation funnels through applyPinConfig() or the PWM
// setup/stop functions (which persist their own config). Returns 0 on a
// user exit, -1 when it refused to open (bad idx / OG). The CALLER owns the
// re-highlight and its own rotaryDivider; this restores the divider it found.
int gpioOptionsCarousel(int gpioIdx);

// ============================================================================
// Click-menu apps (Phase 4, CodeDocs/GPIO_plan.md) - the parts-style
// stay-in-menu launchers. Name-dispatched by runApp() from the menuTree.h
// "GPIO" children ("Set Pins" / "BCD Counter" apps[] rows in Apps.cpp).
// ============================================================================

// Pin settings app: level 1 picks a pin (GPIO 1-8, Tx, Rx - each line shows
// live state; pins something owns show the owner and refuse selection),
// level 2 picks Direction / Pulls / PWM / BCD (PWM absent for Tx/Rx) and
// drops into the Phase 2 sub-editors. After a value applies it returns to
// level 2 (stay in menu); a HOLD anywhere unwinds the WHOLE app and reopens
// the top-level click menu on its "GPIO" row (Menus::
// requestReopenAtTopLevel) - there is no back-one-level in this launcher. A
// serial byte exits the whole app with no reopen (the terminal wants the
// board). No-op toast on OG.
void gpioSettingsLauncher(void);

// BCD counter app: one level - Count (straight into the bcdAdjust modal,
// which defaults the range to every routed claimable pin when none is set)
// and Range (bcdRangeSetup, for an explicit contiguous start/width). A HOLD
// anywhere unwinds the whole app and reopens the top-level click menu on
// its "GPIO" row; a serial byte exits with no reopen. No-op toast on OG.
void bcdMenuLauncher(void);

#endif
