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
#include "CH446Q.h"       // sendXYrawUnchecked (erattaClearGPIO)
#include "Highlighting.h" // brightenedNet / highlightTimer (probeToggle, toggleGPIO)
#include "NetsToChipConnections.h" // numberOfNets (anyGpio* predicates)
#include "Peripherals.h" // showADCreadings, getDacVoltage, initI2C
#include "Probing.h"       // measuredState enum, ProbeButton
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

void initGPIO( void ) {
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; it does NOT apply to the
    // OG. On the OG pin 25 is the WS2812 breadboard-LED data line (claimed by
    // PIO in initLEDs on core1) and pins 26-29 are the RP2040 ADC inputs. Running
    // gpio_init() over 20-27 here re-muxes pin 25 from PIO back to SIO - and
    // because initDAC()->initGPIO() on core0 races initLEDs() on core1, it wins
    // and leaves the whole strip dark (observed: pin 25 stuck in SIO). It would
    // also stomp the ADC pins. The OG's only routable GPIO (RP_GPIO_0 + UART
    // TX/RX) are owned by their own subsystems, so skip this bank entirely.
    // ponytail: when the OG routable-GPIO map is finalized (Phase 2) this should
    // iterate board::currentBoard().gpio instead of the hard-coded V5 pins.
    return;
#endif
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
#if defined(OG_JUMPERLESS)
    // See initGPIO(): the V5 routable-GPIO bank (pins 20-27) isn't present on the
    // OG, and pins 25 (WS2812 LED) / 26-29 (ADC) must never be driven as SIO here.
    // setGPIO() runs on every refreshConnections(), so leaving it active would
    // re-assert gpio_set_dir()/gpio_put() on the LED pin each refresh. Skip.
    return;
#endif
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

    int gpioOutputFound = -2;
    if ( gpio < 0 || gpio > 9 ) {
        for ( int i = 0; i < 10; i++ ) {
            if ( gpioNet[ i ] == brightenedNet ) {
                if ( globalState.config.gpioDirection[ i ] == 0 ) {
                    gpio = gpioDef[ i ][ 0 ];
                    gpioOutputFound = i;
                }
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

#if defined(OG_JUMPERLESS)
// The PWM family resolves gpio_pin 1-8 through gpioDef to physical pins 20-27 -
// the V5 routable-GPIO bank, which does NOT exist on the OG. There those pins
// are the crosspoint chip selects for chips I-L (20-23), RESETPIN (24), the
// WS2812 breadboard-LED data line (25) and the RP2040 ADC inputs (26-27), so
// muxing one to GPIO_FUNC_PWM (or gpio_put()ing it from the slow-PWM timer
// callback) corrupts routing, the LED strip or the ADCs. Same bank initGPIO()
// and setGPIO() already refuse to touch.
// ponytail: drop this once the OG routable-GPIO map (Phase 2) lands.
static int pwmUnavailableOnOG( void ) {
    Serial.println( "PWM isn't available on the Jumperless OG" );
    return -1;
}
#endif

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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

#if defined(OG_JUMPERLESS)
    return pwmUnavailableOnOG( );
#endif
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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

#if defined(OG_JUMPERLESS)
    return pwmUnavailableOnOG( );
#endif
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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

#if defined(OG_JUMPERLESS)
    return pwmUnavailableOnOG( );
#endif
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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

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
#if defined(OG_JUMPERLESS)
    // The pins-20-27 bank is the V5 routable-GPIO map; on OG these are the
    // CH446Q chip selects (20-23), RESETPIN (24), WS2812 data (25) and the
    // ADC inputs (26-27). PWM on them drives real control lines - refuse
    // until the Phase 2 OG map lands (audit #21, 2026-08-26).
    return -1;
#endif

#if defined(OG_JUMPERLESS)
    return pwmUnavailableOnOG( );
#endif
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
#if defined(OG_JUMPERLESS)
  // SKIPPED on OG: same hazard as readSettingsFromConfig()/setGPIO() etc - the
  // gpioDef bank (pins 20-27) is the V5 routable-GPIO map, but on the OG pins
  // 20-23 are the CH446Q chip selects for chips I/J/K/L. Driving them here
  // (reachable via the probe GPIO-direction selection, a Phase-2 path on OG)
  // would re-mux those CS lines to inputs and break all SF routing.
  return;
#endif
  // NOTE: the probe's GPIO buffer-power claim (debug.probe_power_gpio) holds
  // one of these pins output-HIGH; forcing it through the "output starts
  // low" default here grounded ROUTABLE_BUFFER_IN and broke measure-mode
  // probing with the claim bridge still in place. Skip the claimed pin.
  // (An old stray `break` also made this function only ever touch the FIRST
  // SIO pin - i.e. GPIO_1, the usual claim - regardless of which pin the
  // caller had changed. Callers now pass the index they modified.)
  extern int probeGpioPowerClaimIdx(void);
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
#if defined(OG_JUMPERLESS)
  // The V5 routable-GPIO bank described by gpioDef (pins 20-27) does NOT exist
  // on the OG. There, pins 20-23 are the CH446Q chip selects for chips I/J/K/L,
  // pin 24 is RESET, pin 25 is the WS2812 LED data line and 26-27 are ADC
  // inputs. Running gpio_set_dir()/gpio_set_pulls() over that bank here re-muxes
  // the SF chip-select pins into GPIO inputs, after which setCSex() can no
  // longer assert them -- so every breadboard<->SF (nano/DAC/ADC) connection
  // silently fails to program while A-H (CS 6-13, untouched) keep working. The
  // OG has no user-routable GPIO bank here, so skip entirely (matches
  // initGPIO()/setGPIO()).
  return;
#endif
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
#if !defined(OG_JUMPERLESS)
  // SKIPPED on OG: the gpioDef bank (pins 20-27) is the V5 routable-GPIO map.
  // On the OG pins 20-23 are the CH446Q chip selects for chips I/J/K/L, 24 is
  // RESET, 25 is the WS2812 LED data line and 26-27 are ADC inputs.
  // readSettingsFromConfig() runs at boot and on every config change, AFTER
  // initCH446Q() has set 20-23 to OUTPUT on core1. The default routable-GPIO
  // config is input+pulldown, so this loop drives pins 20-23 back to inputs
  // (gpio_set_dir false) -- setCSex() can then no longer assert them and every
  // breadboard<->SF (nano/DAC/ADC) connection silently fails to program while
  // A-H (CS 6-13, untouched) keep working. Matches the OG guards in
  // initGPIO()/setGPIO()/updateGPIOConfigFromState()/applyStateToHardware().
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
#endif // !OG_JUMPERLESS
}
