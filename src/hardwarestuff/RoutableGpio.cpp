// SPDX-License-Identifier: MIT
// RoutableGpio - the V5's user-routable GPIO bank (8 breadboard GPIOs + UART
// Tx/Rx). Phase 1a of the RoutableGpio consolidation (CodeDocs/GPIO_plan.md):
// everything below was moved VERBATIM from Peripherals.cpp and
// remembering/PersistentStuff.cpp - zero behavior change, OG guards intact.
#include "RoutableGpio.h"

#include <Arduino.h>
#include "JumperlessDefines.h" // RP_GPIO_x / RP_UART_x for gpioDef
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/sync.h" // __dmb() in readGPIO
#include "pico/time.h"     // repeating timers for slow PWM

#include "ArduinoStuff.h" // flashingArduino (readGPIO skips a mid-flash Arduino)
#include "AsyncPassthrough.h" // uartTrafficSinceBoot (routableGpioAvailable)
#include "CH446Q.h"       // sendXYrawUnchecked (erattaClearGPIO)
#include "Commands.h"     // requestLedShow (BCD modals)
#include "Graphics.h"     // bread b - LED matrix text (BCD modals)
#include "Highlighting.h" // brightenedNet / highlightTimer (probeToggle, toggleGPIO)
#include "InfraPaths.h"   // infraOwnsNode (routableGpioAvailable)
#include "JumperlOS.h"    // jOS.serviceInner + probing (BCD modals)
#include "Menus.h"        // Menus::getInstance().inClickMenu (BCD modals)
#include "NetsToChipConnections.h" // numberOfNets (anyGpio* predicates)
#include "Peripherals.h" // showADCreadings, getDacVoltage, initI2C
#include "Probing.h"       // measuredState enum, ProbeButton, probeGpioPowerClaimIdx
#include "ReadingDisplay.h" // OLED/serial mirror of the BCD modals
#include "RotaryEncoder.h" // encoder globals + EncoderAccelerator (BCD modals)
#include "States.h"        // globalState
#include "configManager.h" // configChanged (updateGPIOConfigFromState)
#include <oled.h>

// gpioDef[i][0] is the pin number
// gpioDef[i][1] is the RP_GPIO_x define
// gpioDef[i][2] is the index of the gpioState array
// Defined ONCE here (was a per-TU internal-linkage copy in Peripherals.h); the
// explicit `extern` keeps external linkage on a namespace-scope const so the
// extern in RoutableGpio.h binds to this one definition.
extern const int gpioDef[10][3] = {
    {20, RP_GPIO_1, 0},
    {21, RP_GPIO_2, 1},
    {22, RP_GPIO_3, 2},
    {23, RP_GPIO_4, 3},
    {24, RP_GPIO_5, 4},
    {25, RP_GPIO_6, 5},
    {26, RP_GPIO_7, 6},
    {27, RP_GPIO_8, 7},
    {0, RP_UART_TX, 8},
    {1, RP_UART_RX, 9}
};

/// GPIO states: 0=output low, 1=output high, 2=input, 3=input pullup, 
///              4=input pulldown, 5=unknown, 6=I2C, 7=bus keeper
/// GPIO array layout (50 total):
/// Indices 0-9:   Real GPIO pins (RP2040 GPIO 20-27 + UART)
/// Indices 10-17: Fake GP Outputs (FAKE_GP_OUT_0 through FAKE_GP_OUT_7)
/// Indices 18-49: Fake GP Inputs (FAKE_GP_IN_0 through FAKE_GP_IN_31)
uint8_t gpioState[ 50 ] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // Real GPIO (0-9)
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,              // Fake GP Out (10-17)
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // Fake GP In 0-9 (18-27)
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // Fake GP In 10-19 (28-37)
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // Fake GP In 20-29 (38-47)
    0xff, 0xff                                                   // Fake GP In 30-31 (48-49)
};


uint8_t gpioReadFloating [10] = { 1,1,1,1,1,1,1,1, 0, 0 }; // For real GPIO pins - tracks if we should read floating (1 = yes, 0 = no)

uint8_t gpioReading[ 50 ] = {
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  // Real GPIO (3 = unknown)
    3, 3, 3, 3, 3, 3, 3, 3,        // Fake GP Out
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  // Fake GP In 0-9
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  // Fake GP In 10-19
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  // Fake GP In 20-29
    3, 3                           // Fake GP In 30-31
};

int gpioNet[ 50 ] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // Real GPIO
    -1, -1, -1, -1, -1, -1, -1, -1,          // Fake GP Out
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // Fake GP In 0-9
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // Fake GP In 10-19
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  // Fake GP In 20-29
    -1, -1                                   // Fake GP In 30-31
};

// PWM state tracking
float gpioPWMFrequency[ 10 ] = { 1000.0, 1000.0, 1000.0, 1000.0, 1000.0,
                                 1000.0, 1000.0, 1000.0, 1000.0, 1000.0 };
float gpioPWMDutyCycle[ 10 ] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
bool gpioPWMEnabled[ 10 ] = { false, false, false, false, false,
                              false, false, false, false, false };

// Slow PWM state tracking (for frequencies below 10Hz)
bool gpioSlowPWMEnabled[ 10 ] = { false, false, false, false, false,
                                  false, false, false, false, false };
repeating_timer_t gpioSlowPWMTimers[ 10 ];
volatile uint32_t gpioSlowPWMCounter[ 10 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
volatile uint32_t gpioSlowPWMPeriod[ 10 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
volatile uint32_t gpioSlowPWMDutyTicks[ 10 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// ============================================================================
// THE central OG guard (Phase 1b-ii). Every mutating entry point in this
// module opens with a one-line check on this helper instead of carrying its
// own #if block - this is the ONLY OG_JUMPERLESS conditional in the file.
//
// Why it must fail closed: gpioDef describes the V5 routable-GPIO bank, but on
// the OG (RP2040) those same physical pins are real control lines:
//   20-23 = CH446Q crossbar chip selects for chips I/J/K/L (setCSex) - re-mux
//           one to a GPIO input and every breadboard<->SF (nano/DAC/ADC)
//           connection silently fails to program while A-H keep working
//   24    = CH446Q RESET - driving it can wedge the whole crossbar
//   25    = WS2812 breadboard-LED data (PIO-claimed by initLEDs on core1) -
//           gpio_init() here races that claim, wins, and leaves the strip
//           dark (observed: pin 25 stuck in SIO)
//   26-27 = RP2040 ADC inputs - driving them corrupts every reading
//   0/1   = the OG's real GPIO 0 and the MCP4822 DAC SPI chip select
// A lost guard doesn't no-op - it drives those lines. The OG's only routable
// GPIO (RP_GPIO_0 + UART TX/RX) are owned by their own subsystems.
// ponytail: when the OG routable-GPIO map is finalized (Phase 2) this should
// consult board::currentBoard().gpio instead of a compile-time macro.
static inline bool routableGpioAbsent( void ) {
#if defined(OG_JUMPERLESS)
    return true;
#else
    return false;
#endif
}

void initGPIO( void ) {
    if ( routableGpioAbsent( ) ) {
        return;
    }
    for ( int i = 0; i < 8; i++ ) {
        int gpio_pin = 0;
        if ( i < 8 ) {         // Regular GPIO pins 0-7 are on pins 20-27
            gpio_pin = i + 20; // Map GPIO 0-7 to pins 20-27

        } else if ( i == 8 ) { // UART TX (pin 0)
            gpio_pin = 0;
        } else if ( i == 9 ) { // UART RX (pin 1)
            gpio_pin = 1;
        }
        gpio_init( gpio_pin );

        switch ( globalState.config.gpioDirection[ i ] ) {
        case 0:                             // output
            gpio_set_dir( gpio_pin, true ); // Set as output
            break;
        default:                             // input
            gpio_set_dir( gpio_pin, false ); // Set as input
            break;
        }

        switch ( globalState.config.gpioPulls[ i ] ) {
        case 2:                                       // no pull
            gpio_set_pulls( gpio_pin, false, false ); // No pulls
            break;
        case 1:                                      // pullup
            gpio_set_pulls( gpio_pin, true, false ); // Pull up
            break;
        case 0:                                      // pulldown
            gpio_set_pulls( gpio_pin, false, true ); // Pull down
            break;
        case 3:                                     // bus keeper - weakly retains current logical state when not driven
            gpio_set_pulls( gpio_pin, true, true ); // Both pulls enabled = bus keeper
            break;
        default:
            gpio_set_pulls( gpio_pin, false, true ); // No pulls
            break;
        }
    }
}

int getGPIOIndexFromPin(int pin) {
    for (int i = 0; i < 10; i++) {
        if (gpioDef[i][0] == pin) {
            return i;
        }
    }
    Serial.print("getGPIOIndexFromPin: ");
    Serial.println(pin);
    Serial.println("Failed to find GPIO index");
    Serial.flush();
    return -1;
}

void setGPIO( void ) {
    ///return;
    // Runs on every refreshConnections() - on OG that would re-assert
    // gpio_set_dir()/gpio_put() on the LED/CS bank each refresh.
    if ( routableGpioAbsent( ) ) {
        return;
    }
    // Restore GPIO configurations from jumperlessConfig after
    // refreshConnections()
    for ( int i = 0; i < 10; i++ ) {
        uint8_t gpio_pin = gpioDef[ i ][ 0 ];

        // Skip PWM-enabled pins — setGPIO() must not override the PWM function
        // or drive the pin with gpio_put() while PWM is active.
        if ( globalState.config.gpioPwmEnabled[ i ] ) {
            continue;
        }

        // Skip MicroPython-claimed pins (same contract as readGPIO()).
        // machine.Pin owns dir/pulls until jl_gpio_release_all_pins(); without
        // this, every refreshConnections() stomps e.g. a Pin.PULL_UP back to
        // the persisted config's pulldown and Pin.irq() edges get swallowed.
        if ( globalState.config.gpioPythonOwned[ i ] ) {
            continue;
        }

        // Skip bus-role pins (gpioState 6: the display service's soft-I2C, the
        // oled.cpp precedent). Re-asserting config dir/pulls here put a
        // PULLDOWN on a live bus's ACK window and re-stamped state 4, which
        // then sent core 1's readGPIO down the pull-twiddling float path mid-
        // transaction (sweep finding, high - the mark existed exactly to
        // prevent this and nothing honored it).
        if ( gpioState[ i ] == 6 ) {
            continue;
        }

        // Set direction
        if ( globalState.config.gpioDirection[ i ] == 0 ) {
            gpio_set_dir( gpio_pin, true ); // Set as output
        } else {
            gpio_set_dir( gpio_pin, false ); // Set as input
        }

        // Set pull resistors and update gpioState for animation system
        switch ( globalState.config.gpioPulls[ i ] ) {
        case 0: // pulldown
            gpio_set_pulls( gpio_pin, false, true );
            if ( globalState.config.gpioDirection[ i ] == 1 ) { // input
                gpioState[ i ] = 4;                            // input with pulldown
            }
            break;
        case 1: // pullup
            gpio_set_pulls( gpio_pin, true, false );
            if ( globalState.config.gpioDirection[ i ] == 1 ) { // input
                gpioState[ i ] = 3;                            // input with pullup
            }
            break;
        case 2: // no pull
            gpio_set_pulls( gpio_pin, false, false );
            if ( globalState.config.gpioDirection[ i ] == 1 ) { // input
                gpioState[ i ] = 2;                            // input with no pull
            }
            break;
        case 3: // bus keeper - weakly retains current logical state when not driven
            gpio_set_pulls( gpio_pin, true, true );
            if ( globalState.config.gpioDirection[ i ] == 1 ) { // input
                gpioState[ i ] = 7;                            // bus keeper mode
            }
            break;
        default:
            gpio_set_pulls( gpio_pin, false, false );
            if ( globalState.config.gpioDirection[ i ] == 1 ) { // input
                gpioState[ i ] = 2;                            // input with no pull
            }
            break;
        }

        // Set initial output state for output pins
        if ( globalState.config.gpioDirection[ i ] == 0 ) {
            gpio_put( gpio_pin, gpioState[ i ] );
        }
    }
}

int gpioReadWithFloating(
    int pin, unsigned long usDelay ) { // 2 = floating, 1 = high, 0 = low
    // On OG never twiddle pulls/input-enable on this bank (CH446Q CS/RESET,
    // WS2812 data, ADC inputs - see routableGpioAbsent()): just report the
    // plain level. Safe before the lock: on OG every pull/IE-twiddling path
    // is behind the central guard, so no concurrent twiddler exists and a
    // bare gpio_get() needs no serialization.
    if ( routableGpioAbsent( ) ) {
        return gpio_get( pin );
    }
    enum measuredState state = unknownState;
    int settleDelay = 18;
    int reading = -1;
    int readingPulldown = -1;
    int readingPullup = -1;
    // readingGPIO is a real lock, not just a status flag: this function is
    // called concurrently from core 1 (jl_gpio_get / MicroPython) and core 2
    // (readGPIO background scan). Both twiddle the SAME pad's input-enable and
    // pulls; unserialized overlap makes a driven-HIGH pin read low ~40% of the
    // time (one core disables IE right as the other core reads). Spin here
    // until the other core's read finishes (worst case ~200us).
    //
    // 100ms takeover timeout (same pattern as readingADC): a legit hold is
    // ~200us, so a wait this long means the holder crashed or was hard-parked
    // (doorbell idleOtherCore during a flash write) while holding the lock.
    // Taking it over risks one corrupted reading; spinning forever wedges this core
    // (and, from core 0, stops USB).
    unsigned long gpioWaitStart = micros( );
    while ( __atomic_test_and_set( (volatile char *)&readingGPIO, __ATOMIC_ACQUIRE ) ) {
        if ( micros( ) - gpioWaitStart > 100000 ) {
            break; // take it over
        }
        delayMicroseconds( 1 );
    }

    if (gpio_get_function(pin) == GPIO_FUNC_I2C || gpio_get_function(pin) == GPIO_FUNC_UART || pin < 2){
        __atomic_clear( (volatile char *)&readingGPIO, __ATOMIC_RELEASE );  // unlock before early return
        return 0;
        return gpio_get( pin );
    }

    int dir = gpio_get_dir( pin );
    
    if ( dir == 1 ) { // we'll just quickly set the pin to input and read it and
                      // then set it back to whatever it was
    
        if (pin > 1 && gpio_get_function(pin) != GPIO_FUNC_I2C && gpio_get_function(pin) != GPIO_FUNC_UART){
        gpio_set_input_enabled(pin, false);
        gpio_set_input_enabled(pin, true);
        }
        int result = gpio_get( pin );
        __atomic_clear( (volatile char *)&readingGPIO, __ATOMIC_RELEASE );  // unlock before early return
        return result;
    }

    
    int pullupState = 0;
    int pulldownState = 0;
    //if ( gpio_is_pulled_up( pin ) == 0 && gpio_is_pulled_down( pin ) == 0 ) {
        // pullupState = -1;
        // pulldownState = -1;
    //} else 
    if ( gpio_is_pulled_up( pin ) == 1 ) {
        pullupState = 1;
        // The input buffer is left disabled between reads, and a disabled
        // buffer always reads 0 — enable it or this check always returns low.
        gpio_set_input_enabled( pin, true );
        // Allow time for pullup to settle before checking
        delayMicroseconds( settleDelay );
        if ( gpio_get( pin ) == 0 ) { /// don't mess with the pullups if the pin is
                                      /// already being pulled down by external source
            // state = high;
            __atomic_clear( (volatile char *)&readingGPIO, __ATOMIC_RELEASE );  // unlock before early return
            return low;
        }
        gpio_disable_pulls( pin );
    } else if ( gpio_is_pulled_down( pin ) == 1 ) {
        pulldownState = 1;
        
        // Apply RP2350 errata fix FIRST to ensure proper input buffer state
        if (pin >1 && gpio_get_function(pin) != GPIO_FUNC_I2C && gpio_get_function(pin) != GPIO_FUNC_UART){
            gpio_set_input_enabled(pin, false);
            delayMicroseconds(1);
            gpio_set_input_enabled(pin, true);
        }
        
        // Allow time for pulldown to settle before checking
        delayMicroseconds( settleDelay * 3 );  // Extra time for pulldown to take effect
        
        if ( gpio_get( pin ) == 1 ) { /// pin is still HIGH despite pulldown - external pull-up detected
            //gpio_set_pulls(pin, pullupState, pulldownState);
            __atomic_clear( (volatile char *)&readingGPIO, __ATOMIC_RELEASE );  // unlock before early return
            return high;
        }

        gpio_disable_pulls( pin );
    }

    gpio_set_input_enabled( pin, true );
    delayMicroseconds( settleDelay );

    reading = gpio_get( pin );
    gpio_set_input_enabled( pin, false );

    if ( reading != 0 ) { // if the pin is high, check with pulldowns to make sure
                          // it's not floating high

        gpio_set_pulls( pin, false, true );

        // RP2350-E9 errata: enabling the input buffer while the pad is still
        // above ~1V makes the pad leak enough current to overpower the
        // internal pulldown and latch at ~2.2V, which still reads high — so a
        // floating pin randomly reads high instead of floating. Discharge the
        // node with the input buffer disabled (no leakage path), then enable
        // it only to take the reading.
        delayMicroseconds( settleDelay * 3 );

        gpio_set_input_enabled( pin, true );
        delayMicroseconds( 2 );
        readingPulldown = gpio_get( pin );
        gpio_set_input_enabled( pin, false );

        if ( readingPulldown == 0 ) {
            state = floating;
        } else {
            state = high;
        }

    } else { // if the pin is low, check with pullups to make sure it's not
             // floating low
        gpio_set_pulls( pin, true, false );

        // Same ordering as the pulldown check above: charge the node with the
        // input buffer disabled, then enable it only to take the reading.
        delayMicroseconds( settleDelay * 3 );

        gpio_set_input_enabled( pin, true );
        delayMicroseconds( 2 );
        readingPullup = gpio_get( pin );
        gpio_set_input_enabled( pin, false );

        if ( readingPullup == 1 ) {
            state = floating;
        } else {
            state = low;
        }
    }

    gpio_set_pulls( pin, pullupState,
                    pulldownState ); // set the pullups and pulldowns back to
                                     // whatever they were

    // The float checks above leave the input buffer disabled (that's the E9
    // discharge dance, not a resting state) and a disabled buffer always reads
    // 0 — every later plain gpio_get() on this pad (bus keeper, state-6 bus
    // reads, MicroPython) would be stuck low until something re-enabled it.
    gpio_set_input_enabled( pin, true );

    if ( dir == 1 ) {
        /// gpio_set_dir(pin, true); //set the pin back to whatever it was
    }

    __atomic_clear( (volatile char *)&readingGPIO, __ATOMIC_RELEASE );

    return state;
}

// The GPIO function mux differs by MCU: RP2350 adds HSTX, PIO2, XIP_CS1,
// CORESIGHT_TRACE and UART_AUX functions that don't exist on the RP2040.
// File-local: gpio_function_name_for_pin() below is the only reader.
struct gpio_function_name_struct {
    gpio_function_t function;
    char name[10];
    };

static gpio_function_name_struct gpio_function_names[] = {
#if defined(PICO_RP2350)
    { GPIO_FUNC_HSTX, "HSTX" },
#endif
    { GPIO_FUNC_SPI, "SPI" },
    { GPIO_FUNC_UART, "UART" },
    { GPIO_FUNC_I2C, "I2C" },
    { GPIO_FUNC_PWM, "PWM" },
    { GPIO_FUNC_SIO, "SIO" },
    { GPIO_FUNC_PIO0, "PIO0" },
    { GPIO_FUNC_PIO1, "PIO1" },
#if defined(PICO_RP2350)
    { GPIO_FUNC_PIO2, "PIO2" },
#endif
    { GPIO_FUNC_GPCK, "GPCK" },
#if defined(PICO_RP2350)
    { GPIO_FUNC_XIP_CS1, "XIP_CS1" },
    { GPIO_FUNC_CORESIGHT_TRACE, "CORESIGHT" },
#endif
    { GPIO_FUNC_USB, "USB" },
#if defined(PICO_RP2350)
    { GPIO_FUNC_UART_AUX, "UART_AUX" },
#endif
    { GPIO_FUNC_NULL, "NULL" } };

// sizeof-based so the per-MCU entry count above stays correct on both targets.
#define GPIO_FUNCTION_NAMES_COUNT                                              \
    ( (int)( sizeof( gpio_function_names ) / sizeof( gpio_function_names[0] ) ) )

// Register truth for the routable bank (Phase 1b-i): the pin's current mux
// function straight from the pads. There is no shadow function map anymore -
// anything that wants to know what a routable pin is doing asks the
// hardware. idx is a gpioDef index 0..9.
gpio_function_t routableGpioFunction( int idx ) {
    if ( idx < 0 || idx > 9 ) {
        return GPIO_FUNC_NULL;
    }
    return gpio_get_function( (uint)gpioDef[ idx ][ 0 ] );
}

// Pin-aware function name lookup for RP2350
// Function code 9 maps to different peripherals depending on the GPIO pin:
// - XIP_CS1n:    GPIO 0, 8, 19, 47
// - TRACECLK:    GPIO 1
// - TRACEDATA0:  GPIO 2
// - TRACEDATA1:  GPIO 3
// - TRACEDATA2:  GPIO 4
// - TRACEDATA3:  GPIO 5
// - GPIN0:       GPIO 12, 20
// - GPIN1:       GPIO 22
// - GPOUT0:      GPIO 13, 21
// - GPOUT1:      GPIO 14, 23
// - GPOUT2:      GPIO 24
// - GPOUT3:      GPIO 25
const char* gpio_function_name_for_pin( uint gpio, gpio_function_t function ) {
    // Handle function code 9 which has multiple meanings per pin
    if ( function == 9 ) {
        // XIP_CS1 pins
        if ( gpio == 0 || gpio == 8 || gpio == 19 || gpio == 47 ) {
            return "XIP_CS1";
        }
        // Trace pins
        if ( gpio == 1 ) {
            return "TRACECLK";
        }
        if ( gpio == 2 ) {
            return "TRACEDAT0";
        }
        if ( gpio == 3 ) {
            return "TRACEDAT1";
        }
        if ( gpio == 4 ) {
            return "TRACEDAT2";
        }
        if ( gpio == 5 ) {
            return "TRACEDAT3";
        }
        // Clock input pins
        if ( gpio == 12 || gpio == 20 ) {
            return "GPIN0";
        }
        if ( gpio == 22 ) {
            return "GPIN1";
        }
        // Clock output pins
        if ( gpio == 13 || gpio == 21 ) {
            return "GPOUT0";
        }
        if ( gpio == 14 || gpio == 23 ) {
            return "GPOUT1";
        }
        if ( gpio == 24 ) {
            return "GPOUT2";
        }
        if ( gpio == 25 ) {
            return "GPOUT3";
        }
        // Default for F9 on other pins (shouldn't happen, but fallback)
        return "F9_UNK";
    }

    // For all other function codes, use the simple lookup
    // Function codes 0-8 and 10+ have unique meanings.
    // Bound by the actual table size: it's 15 on RP2350 (unchanged) but smaller
    // on RP2040 where the RP2350-only mux functions are compiled out, so a raw
    // `< 15` would index past the end. The F9 special-casing above is RP2350
    // pin mapping; on RP2040 this lookup is best-effort for the debug display.
    if ( (int)function < GPIO_FUNCTION_NAMES_COUNT ) {
        return gpio_function_names[ function ].name;
    }

    // GPIO_FUNC_NULL (0x1f) or other special values
    if ( function == GPIO_FUNC_NULL ) {
        return "NULL";
    }

    return "UNKNOWN";
}

void printGPIOState( Stream* target ) {

    
    target->println( );
    target->println(
        "   number:\t\b1\t\b2\t\b3\t\b4\t\b5\t\b6\t\b7\t\b8\t\bTx\t\bRx" );
    
    target->print( "      net:\t" );
    for ( int i = 0; i < 10; i++ ) {
        if ( gpioNet[ i ] == -1 ) {
            target->print( "." );
        } else {
            target->print( gpioNet[ i ] );
        }
        target->print( "\t" );
    }

    target->println( );

    target->print( " function:\t" );
    for ( int i = 0; i < 10; i++ ) {
        target->print( gpio_function_name_for_pin( gpioDef[ i ][ 0 ], routableGpioFunction( i ) ) );

        target->print( "\t" );
    }
    target->println( );
    target->print( "set direction:\t" );
    for ( int i = 0; i < 8; i++ ) {
        switch ( globalState.config.gpioDirection[ i ] ) {
        case 0:
            target->print( "out" );
            break;
        case 1:
            target->print( "in" );
            break;
        }
        target->print( "\t" );
    }
    target->println( );

    target->print( "direction:\t" );
    for ( int i = 0; i < 8; i++ ) {
        switch ( gpio_get_dir( i + 20 ) ) {
        case 1:
            target->print( "out" );
            break;
        case 0:
            target->print( "in" );
            break;
        }
        target->print( "\t" );
    }
    target->println( );
    target->print( "    pulls:\t" );
    for ( int i = 0; i < 10; i++ ) {
        uint8_t pin = i + 20;
        if ( i == 8 ) {
            pin = 0;
        } else if ( i == 9 ) {
            pin = 1;
        }
        uint8_t pulls;
        bool pullup_enabled = gpio_is_pulled_up( pin );
        bool pulldown_enabled = gpio_is_pulled_down( pin );

        if ( pullup_enabled && pulldown_enabled ) {
            pulls = 3; // bus keeper
        } else if ( pullup_enabled ) {
            pulls = 1; // pullup
        } else if ( pulldown_enabled ) {
            pulls = 0; // pulldown
        } else {
            pulls = 2; // no pull
        }

        switch ( pulls ) {
        case 0:
            target->print( "down" );
            break;
        case 1:
            target->print( "up" );
            break;
        case 2:
            target->print( "none" );
            break;
        case 3:
            target->print( "keeper" );
            break;
        }
        target->print( "\t" );
    }
    target->println( );
    target->print( "  reading:\t" );
    for ( int i = 0; i < 10; i++ ) {
        switch ( gpioReading[ i ] ) {
        case 0:
            target->print( "low" );
            break;
        case 1:
            target->print( "high" );
            break;
        case 2:
            target->print( "float" );
            break;
        case 3:
            target->print( "?" );
            break;
        }
        target->print( "\t" );
    }
    target->println( );
}

uint32_t gpioReadingColors[ 50 ] = { 
    0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507,  // Real GPIO
    0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507,                      // Fake GP Out
    0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507,  // Fake GP In 0-9
    0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507,  // Fake GP In 10-19
    0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507, 0x050507,  // Fake GP In 20-29
    0x050507, 0x050507                                                                                   // Fake GP In 30-31
};

volatile bool readingGPIO = false;
unsigned long lastReadGPIOStartTime = 0;
unsigned long readGPIOInterval = 1000;

void __not_in_flash_func(readGPIO)( ) {

    // Symmetric guard for the core1/core2 race: if the other core is mid-read
    // (jl_gpio_get / MicroPython), skip this scan pass instead of contending.
    // Atomic acquire-load pairs with the __atomic_test_and_set/__atomic_clear
    // in gpioReadWithFloating(); a microsecond overlap window remains but the
    // real serialization is that lock — this check only avoids wasting a pass.
    if ( __atomic_load_n( (volatile char *)&readingGPIO, __ATOMIC_ACQUIRE ) ) {
        return;
    }

    if ( micros( ) - lastReadGPIOStartTime < readGPIOInterval ) {
        return;
    }

    lastReadGPIOStartTime = micros( );
    
    // CRITICAL: Memory barrier to ensure Core 2 sees latest ownership flags from Core 1
    // Without this, Core 2 might use cached values and not skip claimed pins
    __dmb();  // Data Memory Barrier
    
    for ( int i = 0; i < 8; i++ ) { // if you want to read the UART pins, set this to 10

        // Skip pins owned by MicroPython for timing-critical operations (e.g., NeoPixels)
        // The volatile keyword + memory barrier ensures we see the latest value from Core 1
        if ( globalState.config.gpioPythonOwned[ i ] ) {
            // Pin is owned by MicroPython - skip to avoid interference with timing-critical operations
            continue;
        }

        // Leave the GPIO alone while an Arduino is being flashed - keep the
        // previous state rather than perturb the programming pins.
        if ( flashingArduino == true ) {
            return;
        }

        uint8_t pin = gpioDef[ i ][ 0 ];
        if ( i == 8 ) {
            pin = gpioDef[ i ][ 0 ];

        } else if ( i == 9 ) {
            pin = gpioDef[ i ][ 0 ];
        }
        if (gpio_get_function(gpioDef[i][0]) == GPIO_FUNC_UART) {
            // int read = gpio_get(pin);
            // gpioReadingColors[i] = read == 0 ? 0x002004 : 0x200400;
            continue;
        }

        if ( gpioNet[ i ] == -1 ) {
            if ( gpioState[ i ] != 6 ) {   // bus-role marks survive the
                gpioState[ i ] = 4;        // acquire-to-route window (sweep)
            }
            // continue;
        } else if ( gpioNet[ i ] == -2 ) {
            // gpioState[i] = 6;
            continue;
        } else if ( gpioNet[ i ] == -3 ) {
            // gpioState[i] = 5;
            continue;
        }

        if ( gpioNet[ i ] >= 0 ) {
            int reading = 0;

            if ( globalState.config.gpioDirection[ i ] == 0 ) {
                reading = gpio_get_out_level( gpioDef[ i ][ 0 ] );

                switch ( reading ) {
                case 0:
                    gpioReading[ i ] = 0;
                    gpioReadingColors[ i ] = 0x052302;
                    break;
                case 1:
                    gpioReading[ i ] = 1;
                    gpioReadingColors[ i ] = 0x230205;
                    break;
                }
                continue;
            } else if ( gpioState[ i ] == 6 ) {
                reading = gpio_get( gpioDef[ i ][ 0 ] );
            } else if ( gpioState[ i ] == 7 ) {
                // Bus keeper mode - just read current state, no floating detection
                // needed
                reading = gpio_get( gpioDef[ i ][ 0 ] );
            } else if (gpio_get_function(gpioDef[i][0]) == GPIO_FUNC_UART) {
                // For UART pins, just read the current state without floating detection
                reading = gpio_get( gpioDef[ i ][ 0 ] );
            } else if (gpioReadFloating[i] == 1) {
                reading = gpioReadWithFloating(gpioDef[ i ][ 0 ] ); // check if the pin is floating or has a state
            } else {
                reading = gpio_get( gpioDef[ i ][ 0 ] );
            }

            delayMicroseconds( 2 );
            
            switch ( reading ) {

            case 0:
                gpioReading[ i ] = 0;
                gpioReadingColors[ i ] = 0x002004;
                break;
            case 1:

                gpioReading[ i ] = 1;
                gpioReadingColors[ i ] = 0x200400;
                break;
            case 2:

                gpioReading[ i ] = 2;
            
                gpioReadingColors[ i ] = 0x040408; // just in case it isn't

                break;
            }
        } else {
            gpioReading[ i ] = 3;
        }
        
    }
}

void erattaClearGPIO( int gpio ) {
    // NEW coverage (was unguarded, reachable via cmd_erattaClear on OG): the
    // chip-L crosspoint layout below is the V5 map - on OG these
    // sendXYrawUnchecked() calls would program bogus crosspoints.
    if ( routableGpioAbsent( ) ) {
        return;
    }

    int freeYL = -1;
    if ( gpio == -1 ) {

        // check free y connections on chip L
        for ( int i = 0; i < 8; i++ ) {

            if ( globalState.connections.chipStates[ 11 ].yStatus[ i ] == -1 ) {
                freeYL = i;
                break;
            }
        }

        if ( freeYL == -1 ) {
            // idk do something else if all y connections are taken
            return;
        }

        // we should check if thse connections are already made

        // Unchecked on purpose: this momentary GND<->GPIO bridge IS the
        // discharge - RouteSafety would (correctly) refuse it as a short.
        sendXYrawUnchecked( 11, 15, freeYL, 1 );

        for ( int i = 0; i < 8; i++ ) {
            if ( gpio_get( gpioDef[ i ][ 0 ] ) == 0 ) {
                continue;
            }
            sendXYrawUnchecked( 11, 4 + i, freeYL, 1 );


            sendXYrawUnchecked( 11, 4 + i, freeYL, 0 );
        }

        sendXYrawUnchecked( 11, 15, freeYL, 0 );
    }
}

int probeToggle( int buttonState ) {
    // OG coverage note: this function's ONLY pin mutations flow through
    // toggleGPIO(), which carries the central routableGpioAbsent() guard and
    // returns -2 ("no gpio found") on OG - so the connect path falls through
    // to -6/-3 and the disconnect path to -5/-2 exactly as an empty bank
    // reads today. An entry guard here would be WRONG: the -5 return drives
    // the two-press regular-net disconnect warning, which is live OG
    // functionality. (The DAC block below is dead - `&& false`.)

    // If buttonState is -1, read from hardware (legacy behavior)
    if ( buttonState == -1 ) {
        buttonState = ProbeButton::getInstance().getButtonState();
    }

    if ( buttonState == 0 ) {
        return -1; // no button pressed
    }

    // Handle DAC voltage control for nets 4 and 5
    if ( (brightenedNet == 4 || brightenedNet == 5 )&& false) {
        float currentVoltage = getDacVoltage( brightenedNet == 4 ? 0 : 1 );
        float dacStep = 0.25; // Default step size
        float newVoltage = currentVoltage;

        // if ( buttonState == 2 ) { // Connect button - increase voltage
        //     newVoltage = currentVoltage + dacStep;
        //     if ( newVoltage > 8.0 )
        //         newVoltage = 8.0;        // Clamp to max
        // } else if ( buttonState == 1 ) { // Disconnect button - decrease voltage
        //     newVoltage = currentVoltage - dacStep;
        //     if ( newVoltage < -8.0 )
        //         newVoltage = -8.0; // Clamp to min
        // }

        // // Set the new voltage (setDac functions update globalState.power)
        // if ( brightenedNet == 4 ) {
        //     setDac0voltage( newVoltage, 1, 0, true );
        // } else {
        //     setDac1voltage( newVoltage, 1, 0, true );
        // }

        // Reset the highlight timer to keep DAC highlighted during adjustment
        highlightTimer = millis( );

        // Update display - show only current voltage
        Serial.print( "\r                                 \r" );
        Serial.printf( "DAC %d   %0.2f V", brightenedNet == 4 ? 0 : 1, newVoltage );
        Serial.flush( );

        char oledString[ 30 ];
        sprintf( oledString, "DAC %d\n%0.2f V", brightenedNet == 4 ? 0 : 1,
                 newVoltage );
        oled.clearPrintShow( oledString, 1, true, true, true );

        return brightenedNet; // Return the DAC net number
    }

    if ( buttonState == 2 ) { // connect button
        int toggleResult = toggleGPIO( 2, -1 );

        if ( toggleResult >= 0 ) {
            return toggleResult;
        } else {
            if ( brightenedNet > 0 ) {
                return -6; // no net highlighted - connect button pressed
            }
            return -3; // no gpio connected - connect button pressed
        }
    }

    if ( buttonState == 1 ) { // disconnect button

        if ( brightenedNet > 0 ) {
            int toggleResult = toggleGPIO( 2, -1, 1 );
            if ( toggleResult != -2 ) { //-2 means gpio is not found/connected
                // GPIO output is connected - return -4 to unhighlight it
                // Note: Don't clear brightenedNet here, let caller handle it
                return -4; // gpio output is connected - disconnect button pressed
            } else {
                // No GPIO output on this net - this is a regular net that might have connections
                return -5; // no gpio output - disconnect button pressed on regular net
            }
        } else {

            return -2; // no net highlighted - disconnect button pressed
        }
    }

    return -1;
}

int toggleGPIO( int lowHigh, int gpio, int onlyCheck ) {
    // NEW coverage (was unguarded): gpio_put()/dir mutations on the gpioDef
    // bank. -2 reads as "no gpio found/connected" to both probeToggle paths -
    // the true answer on OG, where this bank doesn't exist.
    if ( routableGpioAbsent( ) ) {
        return -2;
    }

    int gpioOutputFound = -2;
    if ( gpio >= 0 && gpio <= 9 ) {
        // An explicit in-range arg is the GPIO INDEX (0..9), resolved to its
        // physical pin through gpioDef. (Pre-fix, an explicit arg skipped the
        // search entirely, leaving gpioOutputFound = -2 - negative-index reads
        // of gpioDirection/gpioDef, a stray gpioState[] write, and gpio_put()
        // on the raw 0..9 value as a physical pin. Latent: no live caller
        // passes an explicit arg today, only probeToggle with -1.)
        gpioOutputFound = gpio;
        gpio = gpioDef[ gpioOutputFound ][ 0 ];
    } else {
        for ( int i = 0; i < 10; i++ ) {
            // Keep scanning past input pins on the net: only an OUTPUT
            // (direction 0) match ends the search. (The old loop broke on the
            // first gpioNet match even when that pin was an input, shadowing
            // an output later in the bank on the same net -> spurious -2.)
            if ( gpioNet[ i ] == brightenedNet &&
                 globalState.config.gpioDirection[ i ] == 0 ) {
                gpio = gpioDef[ i ][ 0 ];
                gpioOutputFound = i;
                break;
            }
        }
        if ( gpioOutputFound == -2 ) {
            return -2;
        }
        // return -2;
    }

    if ( onlyCheck == 1 ) {
        return gpioOutputFound;
    }

    if ( globalState.config.gpioDirection[ gpioOutputFound ] == 0 ) {

        if ( lowHigh == 0 ) {
            gpio_put( gpio, 0 );
            gpioState[ gpioDef[ gpioOutputFound ][ 2 ] ] = 0;
            Serial.print( "\r                                  \r" );
            Serial.print( " gpio " );
            Serial.print( gpioDef[ gpioOutputFound ][ 2 ] + 1 );
            // printNodeOrName(gpioDef[gpioOutputFound][1], 1);
            Serial.print( "\t" );
            Serial.print( gpio_get_out_level( gpio ) ? "high" : "low" );
            Serial.print( " > " );
            Serial.print( "low" );
            Serial.flush( );
            return 0;
        } else if ( lowHigh == 1 ) {
            gpio_put( gpio, 1 );
            gpioState[ gpioDef[ gpioOutputFound ][ 2 ] ] = 1;
            Serial.print( "\r                                  \r" );
            Serial.print( " gpio " );
            // Serial.print(gpioDef[gpioOutputFound][0]);
            Serial.print( gpioDef[ gpioOutputFound ][ 2 ] + 1 );
            Serial.print( "\t" );
            Serial.print( gpio_get_out_level( gpio ) ? "high" : "low" );
            Serial.print( " > " );
            Serial.print( "high" );
            // Serial.print();
            Serial.flush( );
            return 1;
        } else {
            bool currentState = gpio_get_out_level( gpio );
            gpioState[ gpioDef[ gpioOutputFound ][ 2 ] ] = !currentState;
            gpio_put( gpio, !currentState );
            Serial.print( "\r                                  \r" );
            Serial.print( " gpio " );
            Serial.print( gpioDef[ gpioOutputFound ][ 2 ] + 1 );
            Serial.print( "\t " );
            Serial.print( currentState ? "high" : "low" );
            Serial.print( " > " );
            Serial.print( !currentState ? "high" : "low" );
           // Serial.println( );
            Serial.flush( );
            return !currentState;
        }
    }
    return -1;
}

/// @brief check if any measurements or gpio outputs are connected
/// @return  0 = gpio output, 1 = gpio input, 2 = adc, -1 if no measurements or
/// outputs are connected
int anythingInteractiveConnected( int net ) {

    if ( anyAdcConnected( net ) != -1 ) {
        // Serial.print("adc connected");
        return 2;
    }
    if ( anyGpioOutputConnected( net ) != -1 ) {
        // Serial.print("gpio output connected");
        return 0;
    }
    if ( anyGpioInputConnected( net ) != -1 ) {
        // Serial.print("gpio input connected");
        return 1;
    }
    // Serial.print("no interactive connected");
    return -1;
}

/// @brief check if any gpio outputs are connected
/// @return  gpio number if a gpio output is connected, -1 if no outputs are
/// connected (remember user facing gpio numbers are 1-8, this will return 0-10)
int anyGpioOutputConnected( int net ) {
    if ( net == -1 ) {
        for ( int i = 0; i < 10; i++ ) {
            if ( gpioNet[ i ] > 0 && gpioNet[ i ] <= numberOfNets ) {
                if ( globalState.config.gpioDirection[ i ] == 0 ) {
                    // Only treat as GPIO output if pin function is SIO
                    if ( routableGpioFunction( i ) == GPIO_FUNC_SIO ) {
                        return i;
                    }
                }
            }
        }
    } else {
        for ( int i = 0; i < 10; i++ ) {
            if ( gpioNet[ i ] == net ) {
                if ( globalState.config.gpioDirection[ i ] == 0 ) {
                    // Only treat as GPIO output if pin function is SIO
                    if ( routableGpioFunction( i ) == GPIO_FUNC_SIO ) {
                        return i;
                    }
                }
            }
        }
    }
    return -1;
}




/// @brief check if any gpio inputs are connected
/// @return  gpio number if a gpio input is connected, -1 if no inputs are
/// connected (remember user facing gpio numbers are 1-8, this will return 0-10)
int anyGpioInputConnected( int net ) {
    if ( net == -1 ) {
        for ( int i = 0; i < 10; i++ ) {
            if ( gpioNet[ i ] > 0 && gpioNet[ i ] <= numberOfNets ) {
                if ( globalState.config.gpioDirection[ i ] == 1 ) {
                    if ( routableGpioFunction( i ) == GPIO_FUNC_SIO ) {
                        return i;
                    }
                }
            }
        }
        return -1;
    } else {
        for ( int i = 0; i < 10; i++ ) {
            if ( gpioNet[ i ] == net ) {
                if ( globalState.config.gpioDirection[ i ] == 1 ) {
                    if ( routableGpioFunction( i ) == GPIO_FUNC_SIO ) {
                        return i;
                    }
                }
            }
        }
    }
    return -1;
}

/// @brief check if any measurements are connected
/// @return -1 if no measurements are connected, return adc number if a
/// measurement is connected
int anyAdcConnected( int net ) {
    if ( net == -1 ) {
        for ( int i = 0; i < 8; i++ ) {
            // Serial.print("showADCreadings[i]: ");
            // Serial.println(showADCreadings[i]);

            if ( showADCreadings[ i ] > 0 && showADCreadings[ i ] <= numberOfNets &&
                 i != 7 ) {
                return i;
            }
        }
    } else {
        for ( int i = 0; i < 8; i++ ) {
            if ( showADCreadings[ i ] == net ) {
                return i;
            }
        }
    }
    return -1;
}

// The PWM family (8 entry points below) resolves gpio_pin 1-8 through gpioDef
// to physical pins 20-27 - on OG those are real control lines (see
// routableGpioAbsent()), so each opens with the central guard returning a
// SILENT -1: the MicroPython jl_pwm_* wrappers and DisplayBus call these with
// no OG gate of their own and depend on the bare -1. (The old OG-only
// "PWM isn't available" Serial-printing helper and its 4 call blocks were
// statically unreachable - each sat behind a primary guard's unconditional
// return -1 - and are deleted with this change.)

// Slow PWM Functions (for frequencies below 10Hz)
// Hardware timer callback for slow PWM
bool __not_in_flash_func( slowPWMTimerCallback )( repeating_timer_t* rt ) {
    // Get the GPIO index from the user data
    int gpio_index = (int)(uintptr_t)rt->user_data;

    // Increment counter
    gpioSlowPWMCounter[ gpio_index ]++;

    // Check if we've reached the period
    if ( gpioSlowPWMCounter[ gpio_index ] >= gpioSlowPWMPeriod[ gpio_index ] ) {
        gpioSlowPWMCounter[ gpio_index ] = 0; // Reset counter
    }

    // Set pin state based on duty cycle
    int physical_pin = gpioDef[ gpio_index ][ 0 ];
    if ( gpioSlowPWMCounter[ gpio_index ] < gpioSlowPWMDutyTicks[ gpio_index ] ) {
        gpio_put( physical_pin, 1 ); // HIGH
    } else {
        gpio_put( physical_pin, 0 ); // LOW
    }

    return true; // Continue timer
}

// Setup slow PWM using hardware timer
int setupSlowPWM( int gpio_pin, float frequency, float duty_cycle ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    // Validate frequency (0.001Hz to 10Hz for slow PWM)
    if ( frequency < 0.001 || frequency > 10.0 ) {
        return -2; // Invalid frequency
    }

    // Validate duty cycle (0.0 to 1.0)
    if ( duty_cycle < 0.0 || duty_cycle > 1.0 ) {
        return -3; // Invalid duty cycle
    }

    int gpio_index = gpio_pin - 1;                 // Convert to 0-based index
    int physical_pin = gpioDef[ gpio_index ][ 0 ]; // Get physical pin number

    // Stop any existing slow PWM on this pin
    if ( gpioSlowPWMEnabled[ gpio_index ] ) {
        cancel_repeating_timer( &gpioSlowPWMTimers[ gpio_index ] );
        gpioSlowPWMEnabled[ gpio_index ] = false;
    }

    // Configure pin as output
    gpio_set_function( physical_pin, GPIO_FUNC_SIO );
    gpio_set_dir( physical_pin, true );
    gpio_put( physical_pin, 0 ); // Start low

    // Calculate timer parameters
    // Use 1ms timer resolution for good precision
    uint32_t timer_interval_us = 1000; // 1ms intervals
    uint32_t period_ms = (uint32_t)( 1000.0f / frequency );
    uint32_t duty_ms = (uint32_t)( period_ms * duty_cycle );

    // Convert to timer ticks
    gpioSlowPWMPeriod[ gpio_index ] = period_ms;  // Period in ms
    gpioSlowPWMDutyTicks[ gpio_index ] = duty_ms; // Duty cycle in ms
    gpioSlowPWMCounter[ gpio_index ] = 0;

    // Start the repeating timer
    bool success = add_repeating_timer_ms( -1, slowPWMTimerCallback,
                                           (void*)(uintptr_t)gpio_index,
                                           &gpioSlowPWMTimers[ gpio_index ] );
    if ( !success ) {
        return -4; // Timer setup failed
    }

    // Update state tracking
    gpioPWMFrequency[ gpio_index ] = frequency;
    gpioPWMDutyCycle[ gpio_index ] = duty_cycle;
    gpioSlowPWMEnabled[ gpio_index ] = true;
    gpioPWMEnabled[ gpio_index ] = false; // Not using hardware PWM

    // Update config
    globalState.config.gpioPwmFrequency[ gpio_index ] = frequency;
    globalState.config.gpioPwmDutyCycle[ gpio_index ] = duty_cycle;
    globalState.config.gpioPwmEnabled[ gpio_index ] = true;
    globalState.markDirty();

    return 0; // Success
}

// Set slow PWM duty cycle
int setSlowPWMDutyCycle( int gpio_pin, float duty_cycle ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    // Validate duty cycle (0.0 to 1.0)
    if ( duty_cycle < 0.0 || duty_cycle > 1.0 ) {
        return -2; // Invalid duty cycle
    }

    int gpio_index = gpio_pin - 1; // Convert to 0-based index

    // Check if slow PWM is enabled
    if ( !gpioSlowPWMEnabled[ gpio_index ] ) {
        // Set up slow PWM with default frequency if not already enabled
        float default_freq = ( gpioPWMFrequency[ gpio_index ] < 0.01 )
                                 ? 1.0
                                 : gpioPWMFrequency[ gpio_index ];
        return setupSlowPWM( gpio_pin, default_freq, duty_cycle );
    }

    // Update duty cycle
    uint32_t period_ms = gpioSlowPWMPeriod[ gpio_index ];
    uint32_t duty_ms = (uint32_t)( period_ms * duty_cycle );
    gpioSlowPWMDutyTicks[ gpio_index ] = duty_ms;
    gpioPWMDutyCycle[ gpio_index ] = duty_cycle;
    globalState.config.gpioPwmDutyCycle[ gpio_index ] = duty_cycle;
    globalState.markDirty();

    return 0; // Success
}

// Set slow PWM frequency
int setSlowPWMFrequency( int gpio_pin, float frequency ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    // Validate frequency (0.001Hz to 10Hz for slow PWM)
    if ( frequency < 0.001 || frequency > 10.0 ) {
        return -2; // Invalid frequency
    }

    int gpio_index = gpio_pin - 1; // Convert to 0-based index

    // Check if slow PWM is enabled
    if ( !gpioSlowPWMEnabled[ gpio_index ] ) {
        // Set up slow PWM with default duty cycle if not already enabled
        float default_duty = ( gpioPWMDutyCycle[ gpio_index ] < 0.0 ||
                               gpioPWMDutyCycle[ gpio_index ] > 1.0 )
                                 ? 0.5
                                 : gpioPWMDutyCycle[ gpio_index ];
        return setupSlowPWM( gpio_pin, frequency, default_duty );
    }

    // Re-setup slow PWM with new frequency
    return setupSlowPWM( gpio_pin, frequency, gpioPWMDutyCycle[ gpio_index ] );
}

// Stop slow PWM
int stopSlowPWM( int gpio_pin ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    int gpio_index = gpio_pin - 1;                 // Convert to 0-based index
    int physical_pin = gpioDef[ gpio_index ][ 0 ]; // Get physical pin number

    // Stop the timer
    if ( gpioSlowPWMEnabled[ gpio_index ] ) {
        cancel_repeating_timer( &gpioSlowPWMTimers[ gpio_index ] );
        gpioSlowPWMEnabled[ gpio_index ] = false;
    }

    // Set pin low and back to input
    gpio_put( physical_pin, 0 );
    gpio_set_function( physical_pin, GPIO_FUNC_SIO );
    gpio_set_dir( physical_pin, false );

    // Update state tracking
    gpioPWMEnabled[ gpio_index ] = false;
    globalState.config.gpioPwmEnabled[ gpio_index ] = false;
    globalState.markDirty();

    // Restore gpioState to LOW so setGPIO() drives the pin LOW on the next refresh.
    gpioState[ gpio_index ] = 0;

    return 0; // Success
}

// PWM Functions
int setupPWM( int gpio_pin, float frequency, float duty_cycle ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    // Validate frequency (0.01Hz to 62.5MHz)
    if ( frequency < 0.01 || frequency > 62500000.0 ) {
        return -2; // Invalid frequency
    }

    // For frequencies below 10Hz, use slow PWM with hardware timer
    if ( frequency < 10.0 ) {
        return setupSlowPWM( gpio_pin, frequency, duty_cycle );
    }

    int gpio_index = gpio_pin - 1;                 // Convert to 0-based index
    int physical_pin = gpioDef[ gpio_index ][ 0 ]; // Get physical pin number

    // Set up PWM
    gpio_set_function( physical_pin, GPIO_FUNC_PWM );

    // Find out which PWM slice is connected to this GPIO
    uint slice_num = pwm_gpio_to_slice_num( physical_pin );

    // Calculate PWM parameters
    // System clock is 150MHz by default
    float clock_freq = 150000000.0f;
    uint32_t divider = (uint32_t)( clock_freq / ( frequency * 65536 ) ) + 1;
    if ( divider > 255 )
        divider = 255;

    uint32_t wrap = (uint32_t)( clock_freq / ( frequency * divider ) ) - 1;
    if ( wrap > 65535 )
        wrap = 65535;

    // Set the PWM parameters
    pwm_set_clkdiv( slice_num, divider );
    pwm_set_wrap( slice_num, wrap );

    // Set duty cycle
    uint32_t level = (uint32_t)( duty_cycle * ( wrap + 1 ) );
    pwm_set_gpio_level( physical_pin, level );

    // Enable PWM
    pwm_set_enabled( slice_num, true );

    // Update state tracking
    gpioPWMFrequency[ gpio_index ] = frequency;
    gpioPWMDutyCycle[ gpio_index ] = duty_cycle;
    gpioPWMEnabled[ gpio_index ] = true;

    // Update config
    globalState.config.gpioPwmFrequency[ gpio_index ] = frequency;
    globalState.config.gpioPwmDutyCycle[ gpio_index ] = duty_cycle;
    globalState.config.gpioPwmEnabled[ gpio_index ] = true;
    globalState.markDirty();

    return 0; // Success
}

int setPWMDutyCycle( int gpio_pin, float duty_cycle ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    // Validate duty cycle (0.0 to 1.0)
    if ( duty_cycle < 0.0 || duty_cycle > 1.0 ) {
        return -2; // Invalid duty cycle
    }

    int gpio_index = gpio_pin - 1;                 // Convert to 0-based index
    int physical_pin = gpioDef[ gpio_index ][ 0 ]; // Get physical pin number

    // Check if slow PWM is enabled
    if ( gpioSlowPWMEnabled[ gpio_index ] ) {
        return setSlowPWMDutyCycle( gpio_pin, duty_cycle );
    }

    // Check if regular PWM is enabled
    if ( !gpioPWMEnabled[ gpio_index ] ) {
        // Set up PWM with default frequency if not already enabled
        float default_freq = ( gpioPWMFrequency[ gpio_index ] < 0.01 )
                                 ? 1000.0
                                 : gpioPWMFrequency[ gpio_index ];
        return setupPWM( gpio_pin, default_freq, duty_cycle );
    }

    // Re-setup PWM with the new duty cycle (simpler approach)
    return setupPWM( gpio_pin, gpioPWMFrequency[ gpio_index ], duty_cycle );
}

int setPWMFrequency( int gpio_pin, float frequency ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    // Validate frequency (0.01Hz to 62.5MHz)
    if ( frequency < 0.01 || frequency > 62500000.0 ) {
        return -2; // Invalid frequency
    }

    int gpio_index = gpio_pin - 1; // Convert to 0-based index

    // Check if slow PWM is enabled
    if ( gpioSlowPWMEnabled[ gpio_index ] ) {
        return setSlowPWMFrequency( gpio_pin, frequency );
    }

    // Check if regular PWM is enabled
    if ( !gpioPWMEnabled[ gpio_index ] ) {
        // Set up PWM with default duty cycle if not already enabled
        float default_duty = ( gpioPWMDutyCycle[ gpio_index ] < 0.0 ||
                               gpioPWMDutyCycle[ gpio_index ] > 1.0 )
                                 ? 0.5
                                 : gpioPWMDutyCycle[ gpio_index ];
        return setupPWM( gpio_pin, frequency, default_duty );
    }

    // Re-setup PWM with new frequency
    return setupPWM( gpio_pin, frequency, gpioPWMDutyCycle[ gpio_index ] );
}

int stopPWM( int gpio_pin ) {
    if ( routableGpioAbsent( ) ) {
        return -1; // silent by contract: jl_pwm_* / DisplayBus depend on bare -1
    }
    // Validate GPIO pin number (1-8 for regular GPIO pins)
    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        return -1; // Invalid pin
    }

    int gpio_index = gpio_pin - 1;                 // Convert to 0-based index
    int physical_pin = gpioDef[ gpio_index ][ 0 ]; // Get physical pin number

    // Check if slow PWM is enabled
    if ( gpioSlowPWMEnabled[ gpio_index ] ) {
        return stopSlowPWM( gpio_pin );
    }

    // Regular hardware PWM
    // Find out which PWM slice is connected to this GPIO
    uint slice_num = pwm_gpio_to_slice_num( physical_pin );

    // Disable PWM
    pwm_set_enabled( slice_num, false );

    // Set pin back to SIO function
    gpio_set_function( physical_pin, GPIO_FUNC_SIO );

    // Update state tracking
    gpioPWMEnabled[ gpio_index ] = false;
    globalState.config.gpioPwmEnabled[ gpio_index ] = false;
    globalState.markDirty();

    // Restore gpioState to LOW so setGPIO() drives the pin LOW on the next refresh.
    // (The pin is now in SIO/output mode; drive it LOW immediately to match.)
    gpioState[ gpio_index ] = 0;
    gpio_put( physical_pin, 0 );

    return 0; // Success
}

void updateStateFromGPIOConfig(int onlyIdx) {
  if ( routableGpioAbsent( ) ) {
    return;
  }
  // NOTE: the probe's GPIO buffer-power claim (debug.probe_power_gpio) holds
  // one of these pins output-HIGH; forcing it through the "output starts
  // low" default here grounded ROUTABLE_BUFFER_IN and broke measure-mode
  // probing with the claim bridge still in place. Skip the claimed pin.
  // (An old stray `break` also made this function only ever touch the FIRST
  // SIO pin - i.e. GPIO_1, the usual claim - regardless of which pin the
  // caller had changed. Callers now pass the index they modified.)
  int claimIdx = probeGpioPowerClaimIdx();

  for (int i = 0; i < 10; i++) {  // Changed from 8 to 10 to include UART pins
    // Map gpioState to direction and pull settings

    int gpio_pin = gpioDef[i][0];  // Map GPIO 0-7 to pins 20-27
    if (onlyIdx >= 0 && i != onlyIdx) {
      continue;
    }
    if (i == claimIdx) {
      continue;
    }
    if (routableGpioFunction(i) == GPIO_FUNC_SIO) {

      switch (globalState.config.gpioDirection[i]) {
        case 0: // output (starts low)
          gpioState[i] = 0;
          gpio_set_dir(gpio_pin, true);  // Set as output
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          // gpio_put(gpio_pin, 0);
          break;
        case 1: // input (pulls per config)
          switch (globalState.config.gpioPulls[i]) {
            case 0: // pulldown
              gpioState[i] = 4;
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, false, true);  // Pull down
              break;
            case 1: // pullup
              gpioState[i] = 3;
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, true, false);  // Pull up
              break;
            case 2: // no pull
              gpioState[i] = 2;
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, false, false);  // No pulls
              break;
            case 3: // bus keeper
              gpioState[i] = 7;  // New state for bus keeper
              gpio_set_dir(gpio_pin, false);  // Set as input
              gpio_set_pulls(gpio_pin, true, true);  // Both pulls enabled = bus keeper
              break;

            }
          break;

        }

      }
    }
  }

void updateGPIOConfigFromState(void) {
  if ( routableGpioAbsent( ) ) {
    return;
  }
  // Serial.println("updateGPIOConfigFromState");
  // Serial.flush();
  // return;
  int changed = 0;
  for (int i = 0; i < 10; i++) {  // Changed from 8 to 10 to include UART pins
    // Map gpioState to direction and pull settings

    int gpio_pin = gpioDef[i][0];  // Map GPIO 0-7 to pins 20-27

    // Skip MicroPython-claimed pins (same contract as readGPIO()/setGPIO()):
    // machine.Pin owns dir/pulls until jl_gpio_release_all_pins(), and claimed
    // pins report GPIO_FUNC_SIO so they'd otherwise be stomped right here.
    if (globalState.config.gpioPythonOwned[i]) {
      continue;
    }

    if (routableGpioFunction(i) == GPIO_FUNC_SIO) {

      switch (gpioState[i]) {
        case 0: // output low
          if (globalState.config.gpioDirection[i] != 0 || globalState.config.gpioPulls[i] != 2) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 0; // output
          globalState.config.gpioPulls[i] = 2; // no pull
          gpio_set_dir(gpio_pin, true);  // Set as output
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          break;
        case 1: // output high
          if (globalState.config.gpioDirection[i] != 0 || globalState.config.gpioPulls[i] != 2) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 0; // output
          globalState.config.gpioPulls[i] = 2;
          gpio_set_dir(gpio_pin, true);  // Set as output
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          break;
        case 2: // input
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 2) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 2; // no pull
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, false, false);  // No pulls
          break;
        case 3: // input pullup
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 1) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 1; // pullup
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, true, false);  // Pull up
          break;
        case 4: // input pulldown
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 0) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 0; // pulldown
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, false, true);  // Pull down
          break;
        case 5: // unknown
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 0) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // default to input
          globalState.config.gpioPulls[i] = 0; // default to pulldown
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, false, true);  // Pull down
          break;
        case 6: // do nothing
          break;
        case 7: // bus keeper
          if (globalState.config.gpioDirection[i] != 1 || globalState.config.gpioPulls[i] != 3) {
            changed = 1;
            }
          globalState.config.gpioDirection[i] = 1; // input
          globalState.config.gpioPulls[i] = 3; // bus keeper
          gpio_set_dir(gpio_pin, false);  // Set as input
          gpio_set_pulls(gpio_pin, true, true);  // Both pulls enabled = bus keeper
          break;
        }
      }
    }


  if (changed == 1) {
    configChanged = true; // Mark config as changed so it will be saved
    }

  }

// The readSettingsFromConfig() GPIO block (remembering/PersistentStuff.cpp),
// extracted verbatim - readSettingsFromConfig() now calls this instead.
void applyGpioSettingsFromConfig(void) {
  // Runs at boot and on every config change, AFTER initCH446Q() has set the
  // OG's CS pins 20-23 to OUTPUT on core1 - the central guard replaces the
  // whole-body #if wrapper this function carried through Phase 1a.
  if ( routableGpioAbsent( ) ) {
    return;
  }
  for (int i = 0; i < 10; i++) {  // Changed from 8 to 10 to include UART pins

    // Combine direction and pull settings into a single value
    // 0 = output low, 1 = output high, 2 = input, 3 = input pullup, 4 = input pulldown

    int gpio_pin = gpioDef[i][0];

    // Skip bus-role pins (gpioState 6), same contract as setGPIO().
    if (gpioState[i] == 6) {
      continue;
      }

    // if (i == 8) {
    //   gpio_pin = 0; // UART TX
    //   } else if (i == 9) {
    //     gpio_pin = 1; // UART RX
    //     }

   
   // gpio_init(gpio_pin);

   // if (gpio_get_function(gpio_pin) == GPIO_FUNC_SIO) {
    if (globalState.config.gpioDirection[i] == 0) { // output
      //gpioState[i] = globalState.config.gpioPulls[i] ? 1 : 0; // 1 for high, 0 for low
      gpio_set_dir(gpio_pin, true);
      gpioState[i] = 0;
      // Serial.print("gpio_pin: ");
      // Serial.print(gpio_pin);
      // Serial.print(" gpioState[i]: ");
      // Serial.print(gpioState[i]);
      // Serial.print(" actual: ");
      // Serial.println(gpio_get_dir(gpio_pin));
      //Serial.flush();
      } else if (globalState.config.gpioDirection[i] == 1) { // input
        gpio_set_dir(gpio_pin, false);
        gpioState[i] = 2;
        //         Serial.print("gpio_pin: ");
        // Serial.print(gpio_pin);
        // Serial.print(" gpioState[i]: ");
        // Serial.print(gpioState[i]);
        // Serial.print(" actual: ");
        // Serial.println(gpio_get_dir(gpio_pin));
        // Serial.flush();
        if (globalState.config.gpioPulls[i] == 2) { // no pull
          //  gpioState[i] = 2;
          gpio_set_pulls(gpio_pin, false, false);
          } else if (globalState.config.gpioPulls[i] == 1) { // pullup
            gpioState[i] = 3;
            gpio_set_pulls(gpio_pin, true, false);
            } else if (globalState.config.gpioPulls[i] == 0) { // pulldown
              gpioState[i] = 4;
              gpio_set_pulls(gpio_pin, false, true);
              } else if (globalState.config.gpioPulls[i] == 3) { // bus keeper
              gpioState[i] = 7;
              gpio_set_pulls(gpio_pin, true, true);
              } else {
              gpioState[i] = 5; // unknown
              gpio_set_pulls(gpio_pin, false, false);
              }
        } else {
        gpioState[i] = 5; // unknown
        // gpio_set_dir(gpio_pin, false);
        // gpio_set_pulls(gpio_pin, false, false);
        }
     // }
    }
}

// The applyStateToHardware() GPIO loop (routing/States.cpp), moved here
// verbatim so the central guard covers it - it was the 19th scattered
// OG_JUMPERLESS block, wrapping a fourth copy of the config->hardware apply
// loop that runs on every slot load. Its semantics deliberately differ from
// updateStateFromGPIOConfig() and are preserved EXACTLY:
//   - only skip: bus-role pins (gpioState 6) - no FUNC_SIO gate, no probe
//     power-claim skip, no Python-owned skip, no PWM skip
//   - pulls are applied per config even to OUTPUT pins
//   - outputs keep their loaded level: gpio_put(pin, gpioState[i]) restores
//     the slot's saved high/low instead of forcing "starts low"
// (Closest relative is setGPIO(), minus its PWM/Python skips.)
void applyStateGpioToHardware(void) {
    if ( routableGpioAbsent( ) ) {
        return;
    }
    for (int i = 0; i < 10; i++) {
        uint8_t gpio_pin = gpioDef[i][0];

        // Skip bus-role pins (gpioState 6: the display service's soft-I2C),
        // same guard as setGPIO(). A slot load otherwise re-asserts the config
        // direction/pulls on the live bus - a PULLDOWN across SDA's ACK window
        // - and re-stamps state 4, which sends readGPIO down the pull-twiddling
        // float path mid-transaction.
        if (gpioState[i] == 6) {
            continue;
        }

        // Apply direction to hardware
        if (globalState.config.gpioDirection[i] == 0) {
            gpio_set_dir(gpio_pin, true);  // output
        } else {
            gpio_set_dir(gpio_pin, false);  // input
        }

        // Apply pull resistors to hardware and update gpioState for animations
        switch (globalState.config.gpioPulls[i]) {
            case 0: // pulldown
                gpio_set_pulls(gpio_pin, false, true);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 4;  // input with pulldown
                }
                break;
            case 1: // pullup
                gpio_set_pulls(gpio_pin, true, false);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 3;  // input with pullup
                }
                break;
            case 2: // no pull
                gpio_set_pulls(gpio_pin, false, false);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 2;  // input with no pull
                }
                break;
            case 3: // bus keeper
                gpio_set_pulls(gpio_pin, true, true);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 7;  // bus keeper mode
                }
                break;
            default:
                gpio_set_pulls(gpio_pin, false, false);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 2;  // input with no pull
                }
                break;
        }

        // Set initial output state for output pins
        if (globalState.config.gpioDirection[i] == 0) {
            gpio_put(gpio_pin, gpioState[i]);
        }
    }

    // Re-drive a loaded slot's counter (Phase 3): the loop above restored
    // per-pin config; the counter overlays its encoded value on the range.
    if (globalState.config.bcdStart >= 0) {
        bcdApply();
    }
}

// The single mutation funnel (Phase 1b-ii). Applies globalState.config for
// one gpioDef index to hardware + gpioState[] and marks the state dirty for
// the slot autosave. Assigning a pin RE-MUXES it to SIO: a live PWM claim is
// displaced through its own stop path (stopPWM/stopSlowPWM force SIO and
// clear both flag sets), anything else (UART passthrough, stale I2C) is
// re-muxed directly - otherwise updateStateFromGPIOConfig's SIO gate would
// silently drop the change on exactly the pins a user is reassigning.
// Pins a service OWNS are refused outright (fail closed even if the caller
// skipped the routableGpioAvailable() check).
void applyPinConfig( int idx ) {
    if ( idx < 0 || idx > 9 ) {
        return;
    }
    if ( routableGpioAbsent( ) ) {
        return;
    }
    if ( gpioState[ idx ] == 6 ) {
        return; // bus role: OLED / soft-I2C holds this pin
    }
    if ( globalState.config.gpioPythonOwned[ idx ] ) {
        return; // MicroPython machine.Pin claim
    }
    if ( probeGpioPowerClaimIdx( ) == idx ) {
        return; // probe buffer-power claim holds it output-HIGH
    }
    if ( idx <= 7 ) { // PWM only exists on GPIO 1-8
        if ( gpioSlowPWMEnabled[ idx ] ) {
            stopSlowPWM( idx + 1 );
        }
        if ( gpioPWMEnabled[ idx ] ) {
            stopPWM( idx + 1 );
        }
    }
    if ( routableGpioFunction( idx ) != GPIO_FUNC_SIO ) {
        gpio_set_function( (uint)gpioDef[ idx ][ 0 ], GPIO_FUNC_SIO );
    }
    updateStateFromGPIOConfig( idx );
    globalState.markDirty( );
}

// May the USER assign this pin? (Phase 1b-ii availability gate.) Ordered most
// specific owner first so ownerOut names the real holder. Deliberately NO
// user-bridge check: a user bridge on the pin's node is the EXPECTED state
// for assignment (they wired GPIO 3 to a row precisely to configure it) -
// the four firmware skip-list scans (rpGpio/partsFreeGpios/claimRovingGpio/
// oscTryGpioRoute) answer the different question "which pin can the firmware
// silently borrow" and must not be copied here.
bool routableGpioAvailable( int idx, const char** ownerOut ) {
    const char* owner = nullptr;

    if ( idx < 0 || idx > 9 ) {
        owner = "index"; // out of range - not a routable pin at all
    } else if ( routableGpioAbsent( ) ) {
        owner = "board"; // OG: the bank is CH446Q CS/RESET, WS2812, ADC lines
    } else if ( globalState.config.gpioPythonOwned[ idx ] ) {
        owner = "python"; // machine.Pin claim (jl_gpio_claim_pin)
    } else if ( probeGpioPowerClaimIdx( ) == idx ) {
        owner = "probe power"; // buffer-power claim holds it output-HIGH
    } else if ( infraOwnsNode( gpioDef[ idx ][ 1 ] ) ) {
        // The infra registry holds the UART nodes (RP_UART_TX/RX) only via the
        // serial_1 lock bridges (rpSerial1), so on idx 8/9 name the real
        // owner; everything else in the registry is a routing/feed claim.
        owner = ( idx >= 8 ) ? "serial lock" : "routing";
    } else if ( gpioState[ idx ] == 6 ) {
        owner = "OLED"; // bus-role mark: display soft-I2C / oled::connect
    } else if ( globalState.config.gpioPwmEnabled[ idx ] ||
                gpioPWMEnabled[ idx ] || gpioSlowPWMEnabled[ idx ] ) {
        // The config flag can be a slot-load ghost (dropGhostPwmClaim); the
        // RAM flags are live truth. Check all three so neither a ghost claim
        // nor a live-but-unsaved PWM slips through.
        owner = "PWM";
    } else if ( idx >= 8 && AsyncPassthrough::uartTrafficSinceBoot( ) ) {
        owner = "UART"; // real bytes moved through the passthrough this boot
    }

    if ( owner != nullptr ) {
        if ( ownerOut != nullptr ) {
            *ownerOut = owner;
        }
        return false;
    }
    return true;
}

// ============================================================================
// BCD/binary counter (Phase 3, CodeDocs/GPIO_plan.md). Range + value live in
// globalState.config (persisted per-slot); the pure encode/wrap helpers below
// are file-static and touch NO globals so bcdSelfCheck() can assert them
// without pin writes.
// ============================================================================

static int bcdPow10( int n ) {
    int result = 1;
    while ( n-- > 0 ) {
        result *= 10;
    }
    return result;
}

static int bcdClampWidth( int width ) {
    if ( width < 1 ) {
        return 1;
    }
    if ( width > 10 ) {
        return 10;
    }
    return width;
}

// Largest value a range of `width` bits can show. Binary: 2^width - 1. BCD:
// all-9s over the full nibbles; a partial top nibble (width % 4 bits) carries
// a top digit capped at min(9, 2^bits - 1) - so width 6 counts 0-39 (2 top
// bits cap the tens digit at 3) and width 10 counts 0-399.
static int bcdMaxValueFor( int width, int mode ) {
    width = bcdClampWidth( width );
    if ( mode == 0 ) {
        return ( 1 << width ) - 1;
    }
    int fullDigits = width / 4;
    int topBits = width % 4;
    int scale = bcdPow10( fullDigits );
    if ( topBits == 0 ) {
        return scale - 1;
    }
    int topDigitMax = ( 1 << topBits ) - 1;
    if ( topDigitMax > 9 ) {
        topDigitMax = 9;
    }
    return topDigitMax * scale + ( scale - 1 );
}

// Wrap (not clamp) into [0, maxValue] - both directions, so max+1 -> 0 and
// 0-1 -> max.
static int bcdWrapValue( int value, int maxValue ) {
    int span = maxValue + 1;
    if ( span <= 0 ) {
        return 0;
    }
    int wrapped = value % span;
    if ( wrapped < 0 ) {
        wrapped += span;
    }
    return wrapped;
}

// Level (0/1) of counter bit `bit` (LSB-first) for `value`. Binary: plain
// bits. BCD: decimal digits LSD-first, one nibble (4 bits, LSB-first) per
// digit - digit d = (value / 10^d) % 10 occupies bits 4d..4d+3.
static int bcdEncodeBit( int value, int bit, int mode ) {
    if ( bit < 0 ) {
        return 0;
    }
    if ( mode == 0 ) {
        return ( value >> bit ) & 1;
    }
    int digit = ( value / bcdPow10( bit / 4 ) ) % 10;
    return ( digit >> ( bit % 4 ) ) & 1;
}

// Inverse of bcdEncodeBit over a whole level array - the self-check's
// round-trip decoder.
static int bcdDecodeLevels( const int* levels, int width, int mode ) {
    width = bcdClampWidth( width );
    if ( mode == 0 ) {
        int value = 0;
        for ( int bit = 0; bit < width; bit++ ) {
            value |= ( levels[ bit ] & 1 ) << bit;
        }
        return value;
    }
    int value = 0;
    for ( int digit = 0; digit * 4 < width; digit++ ) {
        int nibble = 0;
        for ( int b = 0; b < 4 && digit * 4 + b < width; b++ ) {
            nibble |= ( levels[ digit * 4 + b ] & 1 ) << b;
        }
        value += nibble * bcdPow10( digit );
    }
    return value;
}

// May the COUNTER claim this pin? routableGpioAvailable() with one carve-out:
// a pin refused only for leftover PWM is claimable, because the apply path
// funnels through applyPinConfig() which displaces the PWM. Every other
// owner (python / probe power / routing / serial lock / OLED / UART) wins.
static bool bcdPinClaimable( int idx ) {
    const char* owner = nullptr;
    if ( routableGpioAvailable( idx, &owner ) ) {
        return true;
    }
    return owner != nullptr && strcmp( owner, "PWM" ) == 0;
}

int bcdBitIndex( int bit ) {
    int start = globalState.config.bcdStart;
    if ( start < 0 || bit < 0 || bit >= globalState.config.bcdWidth ) {
        return -1;
    }
    int idx = start + bit;
    if ( idx > 9 ) {
        return -1; // past UART Rx - the bank ends
    }
    return idx; // start+k rolls through GPIO 1-8 into Tx (8) then Rx (9)
}

void bcdApply( void ) {
    if ( globalState.config.bcdStart < 0 || routableGpioAbsent( ) ) {
        return;
    }
    int width = bcdClampWidth( globalState.config.bcdWidth );
    int mode = globalState.config.bcdMode;
    // Encode from a locally wrapped copy: a hand-edited or stale slot file
    // can load bcdValue out of range (deserialize deliberately doesn't
    // validate), and wrapping here keeps BCD digits 0-9 without writing the
    // correction back on a pure load path.
    int value = bcdWrapValue( globalState.config.bcdValue,
                              bcdMaxValueFor( width, mode ) );
    for ( int bit = 0; bit < width; bit++ ) {
        int idx = bcdBitIndex( bit );
        if ( idx < 0 ) {
            break; // range ran past the end of the bank
        }
        if ( !bcdPinClaimable( idx ) ) {
            continue; // something owns this bit's pin - the bit stays skipped
        }
        // Claimable: force the pin to an SIO output through the funnel
        // (displaces leftover PWM, re-muxes to SIO, marks dirty), then drive
        // the bit's level and mirror gpioState so LEDs/readouts agree
        // (0 = output low, 1 = output high).
        globalState.config.gpioDirection[ idx ] = 0;
        applyPinConfig( idx );
        int level = bcdEncodeBit( value, bit, mode );
        gpio_put( gpioDef[ idx ][ 0 ], level );
        gpioState[ idx ] = (uint8_t)level;
    }
}

int bcdMaxValue( void ) {
    return bcdMaxValueFor( globalState.config.bcdWidth, globalState.config.bcdMode );
}

int bcdIncrement( int delta ) {
    if ( globalState.config.bcdStart < 0 ) {
        return -1; // no range configured - nothing moved, nothing dirtied
    }
    int value = bcdWrapValue( globalState.config.bcdValue + delta, bcdMaxValue( ) );
    globalState.setBcdValue( value ); // markDirty via the setter
    bcdApply( );
    return value;
}

bool bcdSelfCheck( Stream* out ) {
    if ( out == nullptr ) {
        out = &Serial;
    }
    bool allPass = true;
    auto report = [ & ]( const char* label, bool pass ) {
        out->print( pass ? "  pass  " : "  FAIL  " );
        out->println( label );
        if ( !pass ) {
            allPass = false;
        }
    };
    // Encode every value the range can show, decode the levels back, expect
    // the same value.
    auto roundTrip = [ & ]( int width, int mode, const char* label ) {
        int maxV = bcdMaxValueFor( width, mode );
        bool pass = true;
        for ( int v = 0; v <= maxV; v++ ) {
            int levels[ 10 ] = { 0 };
            for ( int bit = 0; bit < width; bit++ ) {
                levels[ bit ] = bcdEncodeBit( v, bit, mode );
            }
            if ( bcdDecodeLevels( levels, width, mode ) != v ) {
                pass = false;
                break;
            }
        }
        report( label, pass );
    };

    out->println( "\r\nbcdSelfCheck: encode/wrap logic (no pin writes)" );

    roundTrip( 1, 0, "binary width 1 round-trip (0-1)" );
    roundTrip( 4, 0, "binary width 4 round-trip (0-15)" );
    roundTrip( 10, 0, "binary width 10 round-trip (0-1023)" );
    roundTrip( 4, 1, "BCD width 4 round-trip (0-9)" );
    roundTrip( 8, 1, "BCD width 8 round-trip (0-99)" );
    roundTrip( 6, 1, "BCD width 6 round-trip (0-39)" );

    report( "binary width 10 max = 1023", bcdMaxValueFor( 10, 0 ) == 1023 );
    report( "BCD width 4 max = 9", bcdMaxValueFor( 4, 1 ) == 9 );
    report( "BCD width 8 max = 99", bcdMaxValueFor( 8, 1 ) == 99 );
    report( "BCD width 6 max = 39 (2 top bits cap tens at 3)",
            bcdMaxValueFor( 6, 1 ) == 39 );
    report( "BCD width 10 max = 399 (2 top bits cap hundreds at 3)",
            bcdMaxValueFor( 10, 1 ) == 399 );

    {
        static const int expected5[ 4 ] = { 1, 0, 1, 0 }; // 5 = 0b0101, LSB-first
        bool pass = true;
        for ( int bit = 0; bit < 4; bit++ ) {
            if ( bcdEncodeBit( 5, bit, 0 ) != expected5[ bit ] ) {
                pass = false;
            }
        }
        report( "binary 5 @ width 4 = 1010 (LSB-first)", pass );
    }
    {
        // 42: digit0 = 2 -> nibble 0100 LSB-first, digit1 = 4 -> 0010
        static const int expected42[ 8 ] = { 0, 1, 0, 0, 0, 0, 1, 0 };
        bool pass = true;
        for ( int bit = 0; bit < 8; bit++ ) {
            if ( bcdEncodeBit( 42, bit, 1 ) != expected42[ bit ] ) {
                pass = false;
            }
        }
        report( "BCD 42 @ width 8 = 0100 0010 (LSD-first)", pass );
    }
    {
        // 39: digit0 = 9 -> 1001 LSB-first, partial top nibble = 3 -> 11
        static const int expected39[ 6 ] = { 1, 0, 0, 1, 1, 1 };
        bool pass = true;
        for ( int bit = 0; bit < 6; bit++ ) {
            if ( bcdEncodeBit( 39, bit, 1 ) != expected39[ bit ] ) {
                pass = false;
            }
        }
        report( "BCD 39 @ width 6 = 1001 11 (partial top nibble)", pass );
    }

    report( "binary width 4 wrap 15+1 -> 0",
            bcdWrapValue( 15 + 1, bcdMaxValueFor( 4, 0 ) ) == 0 );
    report( "binary width 4 wrap 0-1 -> 15",
            bcdWrapValue( 0 - 1, bcdMaxValueFor( 4, 0 ) ) == 15 );
    report( "binary width 10 wrap 1023+1 -> 0",
            bcdWrapValue( 1023 + 1, bcdMaxValueFor( 10, 0 ) ) == 0 );
    report( "binary width 10 wrap 0-1 -> 1023",
            bcdWrapValue( 0 - 1, bcdMaxValueFor( 10, 0 ) ) == 1023 );
    report( "BCD width 8 wrap 99+1 -> 0",
            bcdWrapValue( 99 + 1, bcdMaxValueFor( 8, 1 ) ) == 0 );
    report( "BCD width 8 wrap 0-1 -> 99",
            bcdWrapValue( 0 - 1, bcdMaxValueFor( 8, 1 ) ) == 99 );
    report( "BCD width 6 wrap 39+1 -> 0",
            bcdWrapValue( 39 + 1, bcdMaxValueFor( 6, 1 ) ) == 0 );
    report( "BCD width 6 wrap 0-1 -> 39",
            bcdWrapValue( 0 - 1, bcdMaxValueFor( 6, 1 ) ) == 39 );

    out->println( allPass ? "bcdSelfCheck: ALL PASS" : "bcdSelfCheck: FAILURES" );
    return allPass;
}

// ============================================================================
// BCD modals (VoltageAdjuster::adjust idiom - Peripherals.cpp)
// ============================================================================

// One frame of the counter/range modals: label on the top half, value on the
// bottom (the VoltageAdjuster::updateDisplay layout); OLED + serial go
// through the shared reading display (repeat calls dedupe); atomic LED show.
static void bcdDrawFrame( const char* label, const char* valueText ) {
    b.clear( );
    b.print( label, 0x0a0a12, 0xffffff, 1, 0, 2 );
    b.print( valueText, 0x0a1206, 0xffffff, 0, 1, 1 );
    ReadingDisplay::show( label, -1, valueText );
    requestLedShow( 12 ); // 12 = blocking mode (atomic menu display)
}

// User-facing pin name for a gpioDef index: "GPIO 1".."GPIO 8", "Tx", "Rx".
static void bcdPinLabel( int idx, char* buf, size_t bufLen ) {
    if ( idx == 8 ) {
        snprintf( buf, bufLen, "Tx" );
    } else if ( idx == 9 ) {
        snprintf( buf, bufLen, "Rx" );
    } else {
        snprintf( buf, bufLen, "GPIO %d", idx + 1 );
    }
}

int bcdAdjust( void ) {
    if ( globalState.config.bcdStart < 0 ) {
        return -2; // no range configured - caller routes to bcdRangeSetup()
    }
    if ( routableGpioAbsent( ) ) {
        return -1;
    }

    // CRITICAL: reset button state so the press that launched this UI can't
    // instantly confirm (VoltageAdjuster::adjust idiom).
    Menus::getInstance( ).inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    int entryValue = globalState.config.bcdValue;
    int value = entryValue;

    int lastDivider = rotaryDivider;
    rotaryDivider = 3;

    // Slow preset + divider 3 is the bench-proven "one step per careful
    // detent, ~5 on a fast spin" pairing (Probing.cpp node selection).
    EncoderAccelerator accelerator = EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    char valueText[ 16 ];

    // Drive the entry value onto the pins so the display and the range agree
    // from the first frame.
    bcdApply( );
    snprintf( valueText, sizeof( valueText ), "%d", value );
    b.clear( 1 );
    bcdDrawFrame( "BCD", valueText );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        // Discard the pad reading but keep the call: it services the probe
        // state machine the raw getButtonState() comparisons below need.
        probing.justReadProbe( true );

        // Cancel (hold / raw probe 1 - raw pre-swap codes, copied verbatim
        // from VoltageAdjuster::adjust): restore the entry value to the pins.
        if ( encoderButtonState == HELD || probeButton.getButtonState( ) == 1 ) {
            if ( value != entryValue ) {
                globalState.setBcdValue( entryValue );
                bcdApply( );
            }
            rotaryDivider = lastDivider;
            encoderButtonState = IDLE;
            requestLedShow( -1 );
            b.clear( );
            Menus::getInstance( ).inClickMenu = 0;
            return -1;
        }

        // Confirm (click-release / raw probe 2): keep the value (already
        // live on the pins and marked dirty by bcdIncrement).
        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             probeButton.getButtonState( ) == 2 ) {
            encoderButtonState = IDLE;
            rotaryDivider = lastDivider;
            Menus::getInstance( ).inClickMenu = 0;
            b.clear( );
            requestLedShow( -1 );
            return value;
        }

        // Serial byte cancels (third exit, same restore as hold-cancel)
        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            if ( value != entryValue ) {
                globalState.setBcdValue( entryValue );
                bcdApply( );
            }
            rotaryDivider = lastDivider;
            Menus::getInstance( ).inClickMenu = 0;
            b.clear( );
            requestLedShow( -1 );
            return -1;
        }

        // Encoder: negated like the probe node-selection idiom so clockwise
        // counts up (VoltageAdjuster's value -= delta, same convention).
        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps; // keep the fractional part
                value = bcdIncrement( steps ); // live on the pins, wraps, marks dirty
                snprintf( valueText, sizeof( valueText ), "%d", value );
                bcdDrawFrame( "BCD", valueText );
            }
        }
    }

    // Should never reach here (VoltageAdjuster tail)
    rotaryDivider = lastDivider;
    Menus::getInstance( ).inClickMenu = 0;
    return -1;
}

int bcdRangeSetup( void ) {
    if ( routableGpioAbsent( ) ) {
        return -1;
    }

    // Step-1 candidates: every pin the counter may claim (PWM counts - the
    // apply path displaces it).
    int candidates[ 10 ];
    int candidateCount = 0;
    for ( int idx = 0; idx < 10; idx++ ) {
        if ( bcdPinClaimable( idx ) ) {
            candidates[ candidateCount++ ] = idx;
        }
    }
    if ( candidateCount == 0 ) {
        Serial.println( "\r\nBCD: no free pins" );
        oled.clearPrintShow( "BCD\nno free pins", 2, 1200 );
        return -1;
    }

    Menus::getInstance( ).inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    int lastDivider = rotaryDivider;
    rotaryDivider = 3;

    EncoderAccelerator accelerator = EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    int step = 0; // 0 = pick start pin, 1 = pick width
    int cursor = 0;
    for ( int i = 0; i < candidateCount; i++ ) {
        if ( candidates[ i ] == globalState.config.bcdStart ) {
            cursor = i; // open on the current start when it's still free
            break;
        }
    }
    int start = -1;
    int width = 1;
    int maxWidth = 1;

    // getButtonState() is a LEVEL (currentButtonState), not an edge - one
    // probe press must not confirm both steps, so the probe confirm disarms
    // on the step transition and re-arms once the button reads released.
    bool probeConfirmArmed = true;

    char valueText[ 16 ];
    bcdPinLabel( candidates[ cursor ], valueText, sizeof( valueText ) );
    b.clear( 1 );
    bcdDrawFrame( "Start", valueText );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        probing.justReadProbe( true );

        if ( !probeConfirmArmed && probeButton.getButtonState( ) != 2 ) {
            probeConfirmArmed = true;
        }

        // Cancel (hold / raw probe 1 / serial byte): nothing changed
        bool serialCancel = ( Serial.available( ) > 0 );
        if ( serialCancel ) {
            Serial.read( );
        }
        if ( encoderButtonState == HELD || probeButton.getButtonState( ) == 1 ||
             serialCancel ) {
            rotaryDivider = lastDivider;
            encoderButtonState = IDLE;
            requestLedShow( -1 );
            b.clear( );
            Menus::getInstance( ).inClickMenu = 0;
            return -1;
        }

        // Confirm (click-release / raw probe 2)
        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ( probeConfirmArmed && probeButton.getButtonState( ) == 2 ) ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE; // step 2 needs a NEW press
            probeConfirmArmed = false;     // ...and a NEW probe press
            if ( step == 0 ) {
                // Start picked - the width pick spans the contiguous
                // claimable run from it.
                start = candidates[ cursor ];
                maxWidth = 0;
                for ( int idx = start; idx <= 9; idx++ ) {
                    if ( !bcdPinClaimable( idx ) ) {
                        break;
                    }
                    maxWidth++;
                }
                if ( maxWidth < 1 ) {
                    maxWidth = 1; // start itself was claimable
                }
                width = globalState.config.bcdWidth;
                if ( width < 1 ) {
                    width = 1;
                }
                if ( width > maxWidth ) {
                    width = maxWidth;
                }
                step = 1;
                encoderAccumulator = 0.0f;
                accelerator.reset( );
                lastEncoderPosition = encoderPosition;
                snprintf( valueText, sizeof( valueText ), "%d bit%s", width,
                          width == 1 ? "" : "s" );
                bcdDrawFrame( "Width", valueText );
                continue;
            }
            // Width picked - commit the range (mode kept), drive the value.
            globalState.setBcdRange( start, width, globalState.config.bcdMode );
            bcdApply( );
            rotaryDivider = lastDivider;
            Menus::getInstance( ).inClickMenu = 0;
            b.clear( );
            requestLedShow( -1 );
            return 0;
        }

        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps;
                if ( step == 0 ) {
                    cursor = ( ( cursor + steps ) % candidateCount + candidateCount ) %
                             candidateCount;
                    bcdPinLabel( candidates[ cursor ], valueText, sizeof( valueText ) );
                    bcdDrawFrame( "Start", valueText );
                } else {
                    width = ( ( width - 1 + steps ) % maxWidth + maxWidth ) % maxWidth + 1;
                    snprintf( valueText, sizeof( valueText ), "%d bit%s", width,
                              width == 1 ? "" : "s" );
                    bcdDrawFrame( "Width", valueText );
                }
            }
        }
    }

    // Should never reach here
    rotaryDivider = lastDivider;
    Menus::getInstance( ).inClickMenu = 0;
    return -1;
}

// ============================================================================
// GPIO options carousel (Phase 2, CodeDocs/GPIO_plan.md). Launched from
// encoderNetHighlight() mode-1 when a turn lands on a highlighted routable-
// GPIO net. Two-level modal: the carousel scrolls Direction - PWM - Pulls -
// BCD; click/probe-connect enters an item's sub-editor; the sub-editor
// returns to the carousel on confirm OR cancel (the carousel is the
// stay-in-place level); hold/probe-remove/serial byte exits the whole thing.
//
// This is the codebase's first NESTED modal, and the raw probe codes and
// encoder HELD are LEVELS, not edges (RotaryEncoder.cpp re-stamps HELD every
// pass while the button is down past the threshold) - so every trigger that
// crossed a level boundary is disarmed on the way back up and re-arms only
// once its level reads clear. Without that, the probe-remove that cancelled
// a sub-editor would cascade-cancel the carousel on the very next pass.
// ============================================================================

// Sub-editor return codes: 0 = confirmed, -1 = cancelled (nothing beyond
// what the editor already applied live), -2 = serial byte (the carousel
// propagates this as a full exit - a typing user wants the terminal back).

enum GpioCarouselItem {
    GPIO_ITEM_DIRECTION = 0,
    GPIO_ITEM_PWM,
    GPIO_ITEM_PULLS,
    GPIO_ITEM_BCD,
};

// Order matches config.gpioPulls / updateStateFromGPIOConfig's switch:
// 0 = pulldown, 1 = pullup, 2 = none, 3 = bus keeper.
static const char* gpioPullNames[ 4 ] = { "down", "up", "none", "keep" };

// Short frequency text that fits both the LED matrix and the option row.
static void gpioFormatFrequency( float freq, char* buf, size_t bufLen ) {
    if ( freq >= 1000000.0f ) {
        snprintf( buf, bufLen, "%.1fM", (double)( freq / 1000000.0f ) );
    } else if ( freq >= 1000.0f ) {
        snprintf( buf, bufLen, "%.1fk", (double)( freq / 1000.0f ) );
    } else if ( freq >= 100.0f ) {
        snprintf( buf, bufLen, "%.0f", (double)freq );
    } else if ( freq >= 10.0f ) {
        snprintf( buf, bufLen, "%.1f", (double)freq );
    } else {
        snprintf( buf, bufLen, "%.2f", (double)freq );
    }
}

// Current-state text for a carousel row, read from config / the live PWM
// flags - cheap lookups only.
static void gpioCarouselItemState( int gpioIdx, int item, char* buf,
                                   size_t bufLen ) {
    switch ( item ) {
        case GPIO_ITEM_DIRECTION:
            snprintf( buf, bufLen, "%s",
                      globalState.config.gpioDirection[ gpioIdx ] == 0 ? "out"
                                                                       : "in" );
            break;
        case GPIO_ITEM_PWM:
            snprintf( buf, bufLen, "%s",
                      ( gpioPWMEnabled[ gpioIdx ] || gpioSlowPWMEnabled[ gpioIdx ] )
                          ? "on"
                          : "off" );
            break;
        case GPIO_ITEM_PULLS: {
            int pull = globalState.config.gpioPulls[ gpioIdx ];
            if ( pull < 0 || pull > 3 ) {
                pull = 2;
            }
            snprintf( buf, bufLen, "%s", gpioPullNames[ pull ] );
            break;
        }
        case GPIO_ITEM_BCD:
            if ( globalState.config.bcdStart < 0 ) {
                snprintf( buf, bufLen, "off" );
            } else {
                snprintf( buf, bufLen, "%d", globalState.config.bcdValue );
            }
            break;
        default:
            buf[ 0 ] = '\0';
            break;
    }
}

// One carousel frame: OLED gets a header row (the pin's name) plus the
// focused option as "< Dir  out >" via clearPrintShowRich; the LED matrix
// keeps to the item name (the pin identity is already lit - the highlighted
// net IS this pin's net); atomic LED show.
static void gpioCarouselDrawFrame( int gpioIdx, int item ) {
    static const char* itemNames[ 4 ] = { "Dir", "PWM", "Pulls", "BCD" };
    const char* itemName = itemNames[ item ];
    char stateText[ 16 ];
    gpioCarouselItemState( gpioIdx, item, stateText, sizeof( stateText ) );

    b.clear( );
    b.print( itemName, 0x0a0a12, 0xffffff, 1, 0, 2 );

    if ( oled.oledConnected ) {
        char pinLabel[ 16 ];
        bcdPinLabel( gpioIdx, pinLabel, sizeof( pinLabel ) );
        char optionText[ 24 ];
        if ( stateText[ 0 ] != '\0' ) {
            snprintf( optionText, sizeof( optionText ), "< %s  %s >", itemName,
                      stateText );
        } else {
            snprintf( optionText, sizeof( optionText ), "< %s >", itemName );
        }
        // Header in Andale Mono 5pt (index 12, the ReadingDisplay header
        // idiom, pinned to its 7px cap height); option row in Pragmatism 8pt
        // (index 19) - "< Pulls  keep >" still fits 128px there.
        OledTextRow rows[ 2 ] = { };
        rows[ 0 ].segs[ 0 ] = { pinLabel, 12, OLED_ALIGN_INHERIT };
        rows[ 0 ].segCount = 1;
        rows[ 0 ].align = OLED_ALIGN_CENTER;
        rows[ 0 ].fixedH = 7;
        rows[ 1 ].segs[ 0 ] = { optionText, 19, OLED_ALIGN_INHERIT };
        rows[ 1 ].segCount = 1;
        rows[ 1 ].align = OLED_ALIGN_CENTER;
        oled.clearPrintShowRich( rows, 2, 1, true, true, true );
    }

    requestLedShow( 12 ); // 12 = blocking mode (atomic menu display)
}

// Frequency changes cross the 10 Hz slow/fast boundary through a full
// stop+setup: setPWMFrequency() REFUSES the slow->fast crossing (it routes
// to setSlowPWMFrequency, whose <=10 Hz validation returns -2 without ever
// reaching the hardware-PWM path), and the fast->slow crossing via
// setupPWM()->setupSlowPWM() re-muxes the pin to SIO but leaves the PWM
// slice enabled behind it with gpioSlowPWMEnabled/gpioPWMEnabled then
// disagreeing about who owns the pin. stopPWM() first (it routes to
// stopSlowPWM() itself when the slow flag is set) makes either direction
// clean; non-crossing changes stay on setPWMFrequency(), which re-setups in
// place. The cancel-restore path goes through here too, so restoring an
// entry value that sat on the other side of the boundary works.
static void gpioPwmApplyFrequency( int gpioIdx, float freq ) {
    if ( freq < 0.01f ) {
        freq = 0.01f;
    }
    if ( freq > 62500000.0f ) {
        freq = 62500000.0f;
    }
    bool isSlow = gpioSlowPWMEnabled[ gpioIdx ];
    bool wantSlow = ( freq < 10.0f );
    if ( isSlow != wantSlow ) {
        float duty = gpioPWMDutyCycle[ gpioIdx ];
        if ( duty < 0.0f || duty > 1.0f ) {
            duty = 0.5f;
        }
        stopPWM( gpioIdx + 1 );
        setupPWM( gpioIdx + 1, freq, duty ); // < 10 Hz delegates to slow PWM
    } else {
        setPWMFrequency( gpioIdx + 1, freq );
    }
}

// Direction sub-editor: encoder toggles Input/Output, click/probe-connect
// confirms (config write + applyPinConfig), hold/probe-remove cancels with
// nothing written. Engine semantic (updateStateFromGPIOConfig's switch):
// direction 0 = output, 1 = input.
static int gpioDirectionEditor( int gpioIdx ) {
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    EncoderAccelerator accelerator = EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    int dir = ( globalState.config.gpioDirection[ gpioIdx ] == 1 ) ? 1 : 0;

    // The probe raw-2 level that ENTERED this editor must not instantly
    // confirm; the raw-1 arm mirrors it so nothing stale cancels either.
    bool probeConfirmArmed = false;
    bool probeCancelArmed = false;

    bcdDrawFrame( "Dir", dir == 0 ? "out" : "in" );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        probing.justReadProbe( true );

        int probeState = probeButton.getButtonState( );
        if ( !probeConfirmArmed && probeState != 2 ) {
            probeConfirmArmed = true;
        }
        if ( !probeCancelArmed && probeState != 1 ) {
            probeCancelArmed = true;
        }

        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            return -2; // carousel exits fully
        }

        if ( encoderButtonState == HELD ||
             ( probeCancelArmed && probeState == 1 ) ) {
            encoderButtonState = IDLE;
            return -1; // nothing written
        }

        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ( probeConfirmArmed && probeState == 2 ) ) {
            encoderButtonState = IDLE;
            // Only a CHANGED direction goes through the funnel: an unchanged
            // confirm must not re-run "output starts low" over a pin the
            // user set HIGH (or displace its live PWM for nothing).
            if ( dir != globalState.config.gpioDirection[ gpioIdx ] ) {
                globalState.config.gpioDirection[ gpioIdx ] = dir;
                applyPinConfig( gpioIdx ); // re-mux + dir/pulls + markDirty
            }
            return 0;
        }

        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps;
                dir = 1 - dir; // two options: any detent batch flips
                bcdDrawFrame( "Dir", dir == 0 ? "out" : "in" );
            }
        }
    }

    return -1; // unreachable
}

// Pulls sub-editor: encoder cycles Down/Up/None/Keeper, click confirms.
// An INPUT applies immediately through applyPinConfig(); an OUTPUT just
// stores + markDirty (pulls take effect when it becomes an input - the
// config funnel would stomp the output level for nothing).
static int gpioPullsEditor( int gpioIdx ) {
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    EncoderAccelerator accelerator = EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    int sel = globalState.config.gpioPulls[ gpioIdx ];
    if ( sel < 0 || sel > 3 ) {
        sel = 2; // none
    }

    bool probeConfirmArmed = false;
    bool probeCancelArmed = false;

    bcdDrawFrame( "Pull", gpioPullNames[ sel ] );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        probing.justReadProbe( true );

        int probeState = probeButton.getButtonState( );
        if ( !probeConfirmArmed && probeState != 2 ) {
            probeConfirmArmed = true;
        }
        if ( !probeCancelArmed && probeState != 1 ) {
            probeCancelArmed = true;
        }

        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            return -2;
        }

        if ( encoderButtonState == HELD ||
             ( probeCancelArmed && probeState == 1 ) ) {
            encoderButtonState = IDLE;
            return -1;
        }

        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ( probeConfirmArmed && probeState == 2 ) ) {
            encoderButtonState = IDLE;
            globalState.config.gpioPulls[ gpioIdx ] = sel;
            if ( globalState.config.gpioDirection[ gpioIdx ] == 1 ) {
                applyPinConfig( gpioIdx ); // input: pulls take effect now
            } else {
                globalState.markDirty( ); // output: stored for later
            }
            return 0;
        }

        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps;
                sel = ( ( sel + steps ) % 4 + 4 ) % 4;
                bcdDrawFrame( "Pull", gpioPullNames[ sel ] );
            }
        }
    }

    return -1; // unreachable
}

// Live Freq/Duty adjust (the PWM sub-editor's leaf). Encoder-accelerated;
// every detent batch lands on the pin immediately. Frequency steps are
// MULTIPLICATIVE (~2% per accelerated step) - the range spans 0.01 Hz to
// 62.5 MHz, so linear Hz steps would be useless at both ends; Medium
// acceleration makes a hard spin sweep decades. Duty is linear 1% steps on
// Slow. Click keeps the value (the setup/set functions already persisted
// it + markDirty); hold restores the entry value through the same
// crossing-aware apply path.
static int gpioPwmParamAdjust( int gpioIdx, bool isFreq ) {
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    EncoderAccelerator accelerator =
        isFreq ? EncoderAccelerator::Medium( ) : EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    float entryValue =
        isFreq ? gpioPWMFrequency[ gpioIdx ] : gpioPWMDutyCycle[ gpioIdx ];
    float value = entryValue;

    bool probeConfirmArmed = false;
    bool probeCancelArmed = false;

    char valueText[ 16 ];
    if ( isFreq ) {
        gpioFormatFrequency( value, valueText, sizeof( valueText ) );
    } else {
        snprintf( valueText, sizeof( valueText ), "%d%%",
                  (int)( value * 100.0f + 0.5f ) );
    }
    bcdDrawFrame( isFreq ? "Freq" : "Duty", valueText );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        probing.justReadProbe( true );

        int probeState = probeButton.getButtonState( );
        if ( !probeConfirmArmed && probeState != 2 ) {
            probeConfirmArmed = true;
        }
        if ( !probeCancelArmed && probeState != 1 ) {
            probeCancelArmed = true;
        }

        // Serial byte: restore the entry value (bcdAdjust's serial-cancel
        // contract), then propagate the full exit.
        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            if ( value != entryValue ) {
                if ( isFreq ) {
                    gpioPwmApplyFrequency( gpioIdx, entryValue );
                } else {
                    setPWMDutyCycle( gpioIdx + 1, entryValue );
                }
            }
            return -2;
        }

        // Cancel: restore the entry value on the pin (same routed path the
        // live steps used - it may sit across the 10 Hz boundary).
        if ( encoderButtonState == HELD ||
             ( probeCancelArmed && probeState == 1 ) ) {
            encoderButtonState = IDLE;
            if ( value != entryValue ) {
                if ( isFreq ) {
                    gpioPwmApplyFrequency( gpioIdx, entryValue );
                } else {
                    setPWMDutyCycle( gpioIdx + 1, entryValue );
                }
            }
            return -1;
        }

        // Confirm: the value is already live and persisted.
        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ( probeConfirmArmed && probeState == 2 ) ) {
            encoderButtonState = IDLE;
            return 0;
        }

        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps;
                if ( isFreq ) {
                    float next = value * powf( 1.02f, (float)steps );
                    if ( next < 0.01f ) {
                        next = 0.01f;
                    }
                    if ( next > 62500000.0f ) {
                        next = 62500000.0f;
                    }
                    gpioPwmApplyFrequency( gpioIdx, next );
                    value = gpioPWMFrequency[ gpioIdx ]; // read back what landed
                    gpioFormatFrequency( value, valueText, sizeof( valueText ) );
                } else {
                    value += 0.01f * (float)steps;
                    if ( value < 0.0f ) {
                        value = 0.0f;
                    }
                    if ( value > 1.0f ) {
                        value = 1.0f;
                    }
                    setPWMDutyCycle( gpioIdx + 1, value ); // routes slow/fast itself
                    snprintf( valueText, sizeof( valueText ), "%d%%",
                              (int)( value * 100.0f + 0.5f ) );
                }
                bcdDrawFrame( isFreq ? "Freq" : "Duty", valueText );
            }
        }
    }

    return -1; // unreachable
}

// PWM sub-editor (gpioDef idx 0-7 only - the PWM functions validate
// gpio_pin 1-8). Stage 0: encoder toggles the pending on/off, click
// confirms - off->on runs setupPWM() with the stored config values (it
// delegates to slow PWM itself below 10 Hz), on->off runs stopPWM() (which
// routes to stopSlowPWM() when gpioSlowPWMEnabled is set) and returns.
// Confirming ON opens stage 1: a Freq/Duty picker whose entries drop into
// the live adjust above. Hold backs out one stage.
static int gpioPwmEditor( int gpioIdx ) {
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    EncoderAccelerator accelerator = EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    bool liveOn = gpioPWMEnabled[ gpioIdx ] || gpioSlowPWMEnabled[ gpioIdx ];
    bool pending = liveOn;

    int stage = 0;  // 0 = on/off toggle, 1 = Freq/Duty picker
    int cursor = 0; // stage 1: 0 = Freq, 1 = Duty

    bool probeConfirmArmed = false;
    bool probeCancelArmed = false;
    // HELD is a re-stamped LEVEL: the hold that cancels the Freq/Duty leaf
    // is still down on the next pass here and would back the picker out
    // too. Disarmed after the leaf returns, re-armed once the button reads
    // IDLE (released).
    bool holdArmed = true;

    char valueText[ 16 ];

    auto drawStage = [ & ]( ) {
        if ( stage == 0 ) {
            bcdDrawFrame( "PWM", pending ? "on" : "off" );
        } else if ( cursor == 0 ) {
            gpioFormatFrequency( gpioPWMFrequency[ gpioIdx ], valueText,
                                 sizeof( valueText ) );
            bcdDrawFrame( "Freq", valueText );
        } else {
            snprintf( valueText, sizeof( valueText ), "%d%%",
                      (int)( gpioPWMDutyCycle[ gpioIdx ] * 100.0f + 0.5f ) );
            bcdDrawFrame( "Duty", valueText );
        }
    };
    drawStage( );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        probing.justReadProbe( true );

        int probeState = probeButton.getButtonState( );
        if ( !probeConfirmArmed && probeState != 2 ) {
            probeConfirmArmed = true;
        }
        if ( !probeCancelArmed && probeState != 1 ) {
            probeCancelArmed = true;
        }
        if ( !holdArmed && encoderButtonState == IDLE ) {
            holdArmed = true;
        }

        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            return -2;
        }

        // Hold backs out one stage: the picker returns to the toggle's
        // parent (the carousel); the toggle cancels with nothing changed.
        if ( ( holdArmed && encoderButtonState == HELD ) ||
             ( probeCancelArmed && probeState == 1 ) ) {
            encoderButtonState = IDLE;
            return ( stage == 1 ) ? 0 : -1;
        }

        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ( probeConfirmArmed && probeState == 2 ) ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            probeConfirmArmed = false; // a NEW press for the next level
            probeCancelArmed = false;
            if ( stage == 0 ) {
                if ( pending != liveOn ) {
                    if ( pending ) {
                        // Stored config values; the same fallbacks the set
                        // functions use when the slot carries garbage.
                        float freq = globalState.config.gpioPwmFrequency[ gpioIdx ];
                        float duty = globalState.config.gpioPwmDutyCycle[ gpioIdx ];
                        if ( freq < 0.01f ) {
                            freq = 1000.0f;
                        }
                        if ( duty < 0.0f || duty > 1.0f ) {
                            duty = 0.5f;
                        }
                        setupPWM( gpioIdx + 1, freq, duty );
                    } else {
                        stopPWM( gpioIdx + 1 ); // routes to stopSlowPWM if slow
                    }
                    // Read the truth back rather than assuming the apply
                    // stuck (setupPWM can refuse).
                    liveOn = gpioPWMEnabled[ gpioIdx ] ||
                             gpioSlowPWMEnabled[ gpioIdx ];
                    pending = liveOn;
                }
                if ( !liveOn ) {
                    return 0; // off: nothing more to edit
                }
                stage = 1; // on: offer Freq / Duty
            } else {
                int r = gpioPwmParamAdjust( gpioIdx, cursor == 0 );
                if ( r == -2 ) {
                    return -2; // serial byte propagates all the way out
                }
                // Back in the picker: the leaf consumed presses and detents;
                // the hold that may have cancelled it is still a live level.
                encoderButtonState = IDLE;
                lastButtonEncoderState = IDLE;
                holdArmed = false;
            }
            accelerator.reset( );
            encoderAccumulator = 0.0f;
            lastEncoderPosition = encoderPosition;
            drawStage( );
            continue;
        }

        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps;
                if ( stage == 0 ) {
                    pending = !pending; // two options: any detent batch flips
                } else {
                    cursor = 1 - cursor; // Freq <-> Duty
                }
                drawStage( );
            }
        }
    }

    return -1; // unreachable
}

int gpioOptionsCarousel( int gpioIdx ) {
    if ( gpioIdx < 0 || gpioIdx > 9 ) {
        return -1;
    }
    if ( routableGpioAbsent( ) ) {
        return -1;
    }

    // CRITICAL: reset button state so nothing in flight can instantly
    // confirm (VoltageAdjuster::adjust idiom).
    Menus::getInstance( ).inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;

    int lastDivider = rotaryDivider;
    rotaryDivider = 3;

    EncoderAccelerator accelerator = EncoderAccelerator::Slow( );
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f;

    // Items, built up front. PWM is skipped for the UART pins (idx 8/9) -
    // setupPWM/stopPWM validate gpio_pin 1-8 only.
    int items[ 4 ];
    int itemCount = 0;
    items[ itemCount++ ] = GPIO_ITEM_DIRECTION;
    if ( gpioIdx <= 7 ) {
        items[ itemCount++ ] = GPIO_ITEM_PWM;
    }
    items[ itemCount++ ] = GPIO_ITEM_PULLS;
    items[ itemCount++ ] = GPIO_ITEM_BCD;
    int cursor = 0;

    // Level-triggered exits start ARMED (entry is a consumed encoder turn,
    // nothing is in flight) and DISARM whenever a sub-editor returns - the
    // hold/remove that cancelled the child is still a live level up here.
    bool holdExitArmed = true;
    bool probeCancelArmed = true;
    bool probeConfirmArmed = true;

    b.clear( 1 );
    gpioCarouselDrawFrame( gpioIdx, items[ cursor ] );

    while ( true ) {
        rotaryEncoderStuff( );
        jOS.serviceInner( );
        // Discard the pad reading but keep the call: it services the probe
        // state machine the raw getButtonState() comparisons below need.
        probing.justReadProbe( true );

        int probeState = probeButton.getButtonState( );
        if ( !probeConfirmArmed && probeState != 2 ) {
            probeConfirmArmed = true;
        }
        if ( !probeCancelArmed && probeState != 1 ) {
            probeCancelArmed = true;
        }
        if ( !holdExitArmed && encoderButtonState == IDLE ) {
            holdExitArmed = true;
        }

        // Exit: hold / raw probe 1 / serial byte (raw pre-swap codes, copied
        // verbatim from VoltageAdjuster::adjust - never invent a swap).
        bool serialExit = ( Serial.available( ) > 0 );
        if ( serialExit ) {
            Serial.read( );
        }
        if ( ( holdExitArmed && encoderButtonState == HELD ) ||
             ( probeCancelArmed && probeState == 1 ) || serialExit ) {
            rotaryDivider = lastDivider;
            encoderButtonState = IDLE;
            requestLedShow( -1 );
            b.clear( );
            Menus::getInstance( ).inClickMenu = 0;
            return 0;
        }

        // Enter the focused item's sub-editor (click-release / raw probe 2).
        if ( ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) ||
             ( probeConfirmArmed && probeState == 2 ) ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;

            int r = 0;
            switch ( items[ cursor ] ) {
                case GPIO_ITEM_DIRECTION:
                    r = gpioDirectionEditor( gpioIdx );
                    break;
                case GPIO_ITEM_PWM:
                    r = gpioPwmEditor( gpioIdx );
                    break;
                case GPIO_ITEM_PULLS:
                    r = gpioPullsEditor( gpioIdx );
                    break;
                case GPIO_ITEM_BCD:
                    // Phase 3's counter modal; no range yet routes through
                    // the range picker first, then counts.
                    r = bcdAdjust( );
                    if ( r == -2 ) {
                        r = ( bcdRangeSetup( ) == 0 ) ? bcdAdjust( ) : -1;
                    }
                    if ( r >= 0 ) {
                        r = 0; // bcdAdjust returns the counted value
                    }
                    break;
                default:
                    break;
            }

            if ( r == -2 ) {
                // Serial byte inside a sub-editor: full exit, same teardown
                // as the carousel's own exit paths.
                rotaryDivider = lastDivider;
                encoderButtonState = IDLE;
                requestLedShow( -1 );
                b.clear( );
                Menus::getInstance( ).inClickMenu = 0;
                return 0;
            }

            // Back at the stay-in-place level: re-assert EVERY modal
            // invariant (the BCD modals restored their caller's divider and
            // cleared inClickMenu on the way out), resync the encoder (a
            // stale delta would jump items), and disarm the level-triggered
            // exits until their levels read clear - the hold or probe-remove
            // that cancelled the child must not cascade up.
            Menus::getInstance( ).inClickMenu = 1;
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            rotaryDivider = 3;
            holdExitArmed = false;
            probeCancelArmed = false;
            probeConfirmArmed = false;
            accelerator.reset( );
            encoderAccumulator = 0.0f;
            lastEncoderPosition = encoderPosition;
            b.clear( 1 );
            gpioCarouselDrawFrame( gpioIdx, items[ cursor ] );
            continue;
        }

        long currentEncoderPosition = encoderPosition;
        long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );
        if ( encoderDelta != 0 ) {
            lastEncoderPosition = currentEncoderPosition;
            encoderAccumulator += accelerator.getAcceleratedDelta( encoderDelta );
            int steps = (int)encoderAccumulator;
            if ( steps != 0 ) {
                encoderAccumulator -= steps;
                cursor = ( ( cursor + steps ) % itemCount + itemCount ) % itemCount;
                gpioCarouselDrawFrame( gpioIdx, items[ cursor ] );
            }
        }
    }

    // Should never reach here (VoltageAdjuster tail)
    rotaryDivider = lastDivider;
    Menus::getInstance( ).inClickMenu = 0;
    return 0;
}

// ============================================================================
// Click-menu apps (Phase 4, CodeDocs/GPIO_plan.md). Two parts-style
// stay-in-menu launchers, name-dispatched by runApp() from the menuTree.h
// "GPIO" children. The picker below is a clone of PartsApp.cpp's
// partsPicker - that one reads PartsApp's file-static s_led/s_title/s_desc
// arrays, so it can't take a caller-supplied list; this one does, plus a
// selectable[] flag for pins something owns (the cursor lands on them so the
// OWNER is named on the line - the plan's "greyed with the owner named" -
// but a click refuses and stays).
// ============================================================================

// bcdDrawFrame's palette, one dim shade added for greyed rows.
static const uint32_t GPIO_APP_HEADER_COLOR = 0x0a0a12;
static const uint32_t GPIO_APP_ITEM_COLOR = 0x0a1206;
static const uint32_t GPIO_APP_GREYED_COLOR = 0x020202;

// One picker frame (the partsDrawItem layout): header + short label on the
// LED matrix, title + desc on the OLED, title overwriting one serial line.
static void gpioAppDrawItem( const char* header, const char* ledLabel,
                             const char* title, const char* desc, bool avail ) {
    b.clear( );
    b.print( header, GPIO_APP_HEADER_COLOR, 0xFFFFFF, 0, 0, 1 );
    b.print( ledLabel, avail ? GPIO_APP_ITEM_COLOR : GPIO_APP_GREYED_COLOR,
             0xFFFFFF, 0, 1, 1 );
    requestLedShow( 2 );

    if ( oled.oledConnected ) {
        char text[ 96 ];
        snprintf( text, sizeof( text ), "%s\n%s", title, desc ? desc : "" );
        oled.resetMultiLineSmallText( );
        oled.showMultiLineSmallText( text );
    }

    Serial.print( "\r  " );
    Serial.print( header );
    Serial.print( ": " );
    Serial.print( title );
    Serial.print( "                    \r" );
    Serial.flush( );
}

// The Phase 2 sub-editors return -1 on HELD immediately (their caller, the
// carousel, disarms instead of waiting), and the BCD modals' cancel paths do
// the same - so the hold that cancelled a child is still a live, re-stamped
// level when control lands back in a picker loop. Wait it out (the
// partsPicker/partsTapForRow discipline) so it can't cascade another level.
static void gpioAppWaitOutHold( void ) {
    while ( encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
            encoderButtonState == LONG_HELD ) {
        jOS.serviceInner( );
        rotaryEncoderButtonStuff( );
        delayMicroseconds( 1000 );
    }
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;
}

// Encoder picker over caller-supplied lines (the partsPicker contract):
// returns the chosen index, -1 on HELD (back one level), -2 on a serial byte
// (exit the app - a byte left unconsumed would land on the single-char
// handler). selectable == nullptr means everything is; a click on a
// non-selectable row is refused and the picker stays.
static int gpioAppPicker( const char* levelTag, const char* header, int count,
                          int startIdx, const char* const* leds,
                          const char* const* titles, const char* const* descs,
                          const uint8_t* selectable ) {
    if ( count <= 0 ) {
        return -1;
    }
    int idx = ( startIdx >= 0 && startIdx < count ) ? startIdx : 0;
    bool needsDraw = true;
    unsigned long lastShowRequest = 0;

    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderDirectionState = NONE;

    Serial.print( "\r\nGPIOPICK level=" );
    Serial.print( levelTag );
    Serial.print( " n=" );
    Serial.println( count );
    Serial.flush( );

    while ( true ) {
        if ( needsDraw ) {
            gpioAppDrawItem( header, leds[ idx ], titles[ idx ],
                             descs ? descs[ idx ] : nullptr,
                             selectable == nullptr || selectable[ idx ] );
            lastShowRequest = millis( );
            needsDraw = false;
        }

        jOS.serviceInner( );
        rotaryEncoderButtonStuff( );

        // Core 2's end-of-frame compare-and-swap can swallow a show request
        // issued mid-frame (Menus.cpp's menuShowKeepalive) - re-assert.
        if ( millis( ) - lastShowRequest >= 250 ) {
            requestLedShow( 2 );
            lastShowRequest = millis( );
        }

        if ( encoderButtonState == HELD ) {
            // Wait out the hold so the release can't echo into the next level.
            gpioAppWaitOutHold( );
            return -1;
        }
        if ( Serial.available( ) > 0 ) {
            Serial.read( );
            return -2;
        }
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) {
            encoderButtonState = IDLE;
            if ( selectable != nullptr && !selectable[ idx ] ) {
                // The owner is already named on the line - refuse and stay.
                Serial.print( "\r\nGPIOPICK refused " );
                Serial.println( titles[ idx ] );
                needsDraw = true;
                continue;
            }
            return idx;
        }
        if ( encoderDirectionState == UP ) {
            encoderDirectionState = NONE;
            idx = ( idx + 1 ) % count;
            needsDraw = true;
        } else if ( encoderDirectionState == DOWN ) {
            encoderDirectionState = NONE;
            idx = ( idx - 1 + count ) % count;
            needsDraw = true;
        }
        delayMicroseconds( 1000 );
    }
}

// Level-1 line for one gpioDef index: short LED label + live-state title.
// "3  out HIGH", "5  in  pull-up", "Tx  UART", "2  PWM 1.0k" - or, for a
// pin something owns (routableGpioAvailable false, PWM excepted - the
// editors displace PWM through applyPinConfig), the owner: "7  OLED".
static bool gpioSettingsPinEntry( int idx, char* led, size_t ledLen,
                                  char* title, size_t titleLen ) {
    if ( idx == 8 ) {
        snprintf( led, ledLen, "Tx" );
    } else if ( idx == 9 ) {
        snprintf( led, ledLen, "Rx" );
    } else {
        snprintf( led, ledLen, "%d", idx + 1 );
    }

    const char* owner = nullptr;
    if ( !routableGpioAvailable( idx, &owner ) &&
         !( owner != nullptr && strcmp( owner, "PWM" ) == 0 ) ) {
        snprintf( title, titleLen, "%s  %s", led,
                  owner != nullptr ? owner : "in use" );
        return false; // greyed: shown with the owner named, not selectable
    }

    if ( idx <= 7 && ( gpioPWMEnabled[ idx ] || gpioSlowPWMEnabled[ idx ] ) ) {
        char freqText[ 12 ];
        gpioFormatFrequency( gpioPWMFrequency[ idx ], freqText,
                             sizeof( freqText ) );
        snprintf( title, titleLen, "%s  PWM %s", led, freqText );
    } else if ( idx >= 8 && routableGpioFunction( idx ) == GPIO_FUNC_UART ) {
        // Untouched UART mux (no traffic since boot, or it would be owned
        // above) - assigning it re-muxes to SIO through applyPinConfig().
        snprintf( title, titleLen, "%s  UART", led );
    } else if ( globalState.config.gpioDirection[ idx ] == 0 ) {
        const char* level = ( gpioState[ idx ] == 1 ) ? "HIGH"
                            : ( gpioState[ idx ] == 0 ) ? "LOW"
                                                        : "";
        snprintf( title, titleLen, "%s  out %s", led, level );
    } else {
        static const char* pullLong[ 4 ] = { "pull-down", "pull-up", "float",
                                             "keeper" };
        int pull = globalState.config.gpioPulls[ idx ];
        if ( pull < 0 || pull > 3 ) {
            pull = 2;
        }
        snprintf( title, titleLen, "%s  in  %s", led, pullLong[ pull ] );
    }
    return true;
}

void gpioSettingsLauncher( void ) {
    if ( routableGpioAbsent( ) ) {
        Serial.println( "\r\nGPIO app: no routable GPIO on this board" );
        if ( oled.oledConnected ) {
            oled.clearPrintShow( "no routable\nGPIO", 2, true, true, true );
        }
        delay( 600 );
        return;
    }

    // Own the render mode for the whole session (menus render one item at a
    // time; core 1 suppresses net paint while inClickMenu). RE-assertion:
    // the APPSACTION arm zeroed it via exitMenuModeForAction() before
    // runApp. runPicker's save/restore discipline for the divider.
    Menus::getInstance( ).inClickMenu = 1;
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    {
        // Entry line buffers - one modal at a time, so statics keep them off
        // the stack (the partsApp s_* precedent).
        static char pinLed[ 10 ][ 4 ];
        static char pinTitle[ 10 ][ 24 ];
        const char* leds[ 10 ];
        const char* titles[ 10 ];
        const char* descs[ 10 ];
        uint8_t selectable[ 10 ];

        int pinCursor = 0;
        while ( true ) {
            // Rebuilt every pass - the lines show LIVE state and an edit
            // just changed it.
            for ( int i = 0; i < 10; i++ ) {
                selectable[ i ] = gpioSettingsPinEntry(
                                      i, pinLed[ i ], sizeof( pinLed[ i ] ),
                                      pinTitle[ i ], sizeof( pinTitle[ i ] ) )
                                      ? 1
                                      : 0;
                leds[ i ] = pinLed[ i ];
                titles[ i ] = pinTitle[ i ];
                descs[ i ] = selectable[ i ] ? "click = options" : "in use";
            }

            int pick = gpioAppPicker( "pin", "GPIO", 10, pinCursor, leds,
                                      titles, descs, selectable );
            if ( pick == -1 ) {
                break; // hold at the top level = exit
            }
            if ( pick == -2 ) {
                goto done; // serial byte = exit the app
            }
            pinCursor = pick;
            int gpioIdx = pick; // the list is all 10 pins in gpioDef order

            char pinHeader[ 16 ];
            bcdPinLabel( gpioIdx, pinHeader, sizeof( pinHeader ) );

            // Level 2 - per-pin options, the carousel's item set (PWM absent
            // for Tx/Rx - setupPWM/stopPWM validate gpio_pin 1-8 only).
            int optItems[ 4 ];
            int nOpts = 0;
            optItems[ nOpts++ ] = GPIO_ITEM_DIRECTION;
            optItems[ nOpts++ ] = GPIO_ITEM_PULLS;
            if ( gpioIdx <= 7 ) {
                optItems[ nOpts++ ] = GPIO_ITEM_PWM;
            }
            optItems[ nOpts++ ] = GPIO_ITEM_BCD;

            static const char* optLed[ 4 ] = { "Dir", "PWM", "Pull", "BCD" };
            static const char* optDesc[ 4 ] = { "input / output",
                                                "freq / duty",
                                                "up down none keep",
                                                "counter" };
            static char optTitle[ 4 ][ 24 ];
            const char* oLeds[ 4 ];
            const char* oTitles[ 4 ];
            const char* oDescs[ 4 ];

            int optCursor = 0;
            while ( true ) {
                // Live state on every line, rebuilt after each edit.
                for ( int i = 0; i < nOpts; i++ ) {
                    int item = optItems[ i ];
                    char stateText[ 16 ];
                    gpioCarouselItemState( gpioIdx, item, stateText,
                                           sizeof( stateText ) );
                    snprintf( optTitle[ i ], sizeof( optTitle[ i ] ),
                              "%s  %s", optLed[ item ], stateText );
                    oLeds[ i ] = optLed[ item ];
                    oTitles[ i ] = optTitle[ i ];
                    oDescs[ i ] = optDesc[ item ];
                }

                int o = gpioAppPicker( "opt", pinHeader, nOpts, optCursor,
                                       oLeds, oTitles, oDescs, nullptr );
                if ( o == -1 ) {
                    break; // hold = back to the pin list
                }
                if ( o == -2 ) {
                    goto done;
                }
                optCursor = o;

                // The Phase 2 sub-editors were tuned under the carousel's
                // divider 3 (raw encoderPosition deltas); the BCD modals
                // set their own and restore what they find.
                int r = 0;
                switch ( optItems[ o ] ) {
                    case GPIO_ITEM_DIRECTION:
                        rotaryDivider = 3;
                        r = gpioDirectionEditor( gpioIdx );
                        break;
                    case GPIO_ITEM_PULLS:
                        rotaryDivider = 3;
                        r = gpioPullsEditor( gpioIdx );
                        break;
                    case GPIO_ITEM_PWM:
                        rotaryDivider = 3;
                        r = gpioPwmEditor( gpioIdx );
                        break;
                    case GPIO_ITEM_BCD:
                        // Phase 3's counter modal; no range yet routes
                        // through the range picker first, then counts.
                        r = bcdAdjust( );
                        if ( r == -2 ) {
                            r = ( bcdRangeSetup( ) == 0 ) ? bcdAdjust( ) : -1;
                        }
                        if ( r >= 0 ) {
                            r = 0; // bcdAdjust returns the counted value
                        }
                        break;
                    default:
                        break;
                }
                if ( r == -2 ) {
                    goto done; // serial byte inside an editor = exit the app
                }
                // A value applied (or the editor cancelled): return to
                // LEVEL 2 - the stay-in-menu behavior. Re-assert what the
                // children may have cleared (the BCD modals zero inClickMenu
                // and restore the divider on their way out) and wait out a
                // live hold so it can't cascade this level too.
                Menus::getInstance( ).inClickMenu = 1;
                rotaryDivider = 8;
                gpioAppWaitOutHold( );
            }

            // Back at level 1: same re-assert (a BCD modal may have run).
            Menus::getInstance( ).inClickMenu = 1;
            rotaryDivider = 8;
            gpioAppWaitOutHold( );
        }
    }

done:
    // The parts teardown minus partLabels.clearTransients() (parts-specific).
    // The menu path runs refreshConnections(-1, 0) after runApp returns.
    Menus::getInstance( ).inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear( );
    requestLedShow( -1 );
    Serial.println( );
    oled.showJogo32h( );
}

void bcdMenuLauncher( void ) {
    if ( routableGpioAbsent( ) ) {
        Serial.println( "\r\nBCD app: no routable GPIO on this board" );
        if ( oled.oledConnected ) {
            oled.clearPrintShow( "no routable\nGPIO", 2, true, true, true );
        }
        delay( 600 );
        return;
    }

    Menus::getInstance( ).inClickMenu = 1; // re-assert (see gpioSettingsLauncher)
    int lastDivider = rotaryDivider;
    rotaryDivider = 8;

    {
        static char itemTitle[ 3 ][ 24 ];
        static const char* itemLed[ 3 ] = { "Cnt", "Rng", "Mode" };
        static const char* itemDesc[ 3 ] = { "turn to count",
                                             "start + width",
                                             "binary / BCD" };
        const char* leds[ 3 ];
        const char* titles[ 3 ];
        const char* descs[ 3 ];

        int cursor = 0;
        while ( true ) {
            // Live state per line, rebuilt every pass.
            if ( globalState.config.bcdStart < 0 ) {
                snprintf( itemTitle[ 0 ], sizeof( itemTitle[ 0 ] ),
                          "Count  no range" );
                snprintf( itemTitle[ 1 ], sizeof( itemTitle[ 1 ] ),
                          "Range  off" );
            } else {
                snprintf( itemTitle[ 0 ], sizeof( itemTitle[ 0 ] ),
                          "Count  %d", globalState.config.bcdValue );
                char startLabel[ 16 ];
                bcdPinLabel( globalState.config.bcdStart, startLabel,
                             sizeof( startLabel ) );
                snprintf( itemTitle[ 1 ], sizeof( itemTitle[ 1 ] ),
                          "Range  %s +%d", startLabel,
                          globalState.config.bcdWidth );
            }
            snprintf( itemTitle[ 2 ], sizeof( itemTitle[ 2 ] ), "Mode  %s",
                      globalState.config.bcdMode ? "BCD" : "binary" );
            for ( int i = 0; i < 3; i++ ) {
                leds[ i ] = itemLed[ i ];
                titles[ i ] = itemTitle[ i ];
                descs[ i ] = itemDesc[ i ];
            }

            int pick = gpioAppPicker( "bcd", "BCD", 3, cursor, leds, titles,
                                      descs, nullptr );
            if ( pick == -1 ) {
                break; // hold = exit (single level)
            }
            if ( pick == -2 ) {
                break; // serial byte = exit
            }
            cursor = pick;

            if ( pick == 0 ) {
                // Count: the bcdAdjust modal; no range yet routes through
                // the range picker first, then counts (the carousel's shape).
                int r = bcdAdjust( );
                if ( r == -2 && bcdRangeSetup( ) == 0 ) {
                    bcdAdjust( );
                }
            } else if ( pick == 1 ) {
                bcdRangeSetup( );
            } else {
                // Mode toggle - stored through the accessor, which permits
                // start -1 (validates -1..9), so no range is ever invented:
                // with the counter off the mode just persists for later.
                // setBcdRange only STORES - with a live range, re-encode the
                // value onto the pins in the new mode.
                int newMode = ( globalState.config.bcdMode != 0 ) ? 0 : 1;
                globalState.setBcdRange( globalState.config.bcdStart,
                                         globalState.config.bcdWidth,
                                         newMode );
                if ( globalState.config.bcdStart >= 0 ) {
                    bcdApply( );
                }
                Serial.print( "\r\nBCD mode -> " );
                Serial.println( newMode ? "BCD" : "binary" );
            }

            // Back at the Count/Range/Mode level (stay in menu): re-assert
            // what the modals cleared, wait out a live hold.
            Menus::getInstance( ).inClickMenu = 1;
            rotaryDivider = 8;
            gpioAppWaitOutHold( );
        }
    }

    // Same teardown contract as gpioSettingsLauncher.
    Menus::getInstance( ).inClickMenu = 0;
    rotaryDivider = lastDivider;
    b.clear( );
    requestLedShow( -1 );
    Serial.println( );
    oled.showJogo32h( );
}
