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

struct gpio_function_name_struct {
    gpio_function_t function;
    char name[10];
    };

// The definition's entry count differs by MCU (the RP2350-only mux functions
// are compiled out on RP2040): 15 entries on RP2350, 10 on RP2040. Size the
// extern per target so the sizeof-based count below stays correct in every TU
// (the old Peripherals.h extern said [15] unconditionally - wrong on OG).
#if defined(PICO_RP2350)
extern gpio_function_name_struct gpio_function_names[15];
#else
extern gpio_function_name_struct gpio_function_names[10];
#endif

#define GPIO_FUNCTION_NAMES_COUNT                                              \
    ( (int)( sizeof( gpio_function_names ) / sizeof( gpio_function_names[0] ) ) )

// Pin-aware function name lookup (handles F9 ambiguity per RP2350 GPIO mux)
const char* gpio_function_name_for_pin( uint gpio, gpio_function_t function );

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
// and on every config change. Compiled out on OG (guard inside).
void applyGpioSettingsFromConfig(void);

#endif
