// SPDX-License-Identifier: MIT
#include "Peripherals.h"
#include "externVars.h" // core-1 frame hold (core1FramesHeld)
#include <Arduino.h>
#include "CH446Q.h"
#include "FileParsing.h"
#include "JumperlessDefines.h"
#include "LEDs.h"

#include "MatrixState.h"
#include "RotaryEncoder.h"
#include "States.h"
#include "NetManager.h"

#include "hardware/adc.h"
#include <math.h>
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/structs/io_bank0.h"
#include "pico/time.h" // For hardware timer support
#include "ReadingDisplay.h"
#include <oled.h>
#include <stdio.h>
//#include <Adafruit_MCP4728.h>  // Old blocking library

#include "PersistentStuff.h"
#include "USBAudio.h"
#include "FakeGpio.h"
#include <Wire.h>
#include "Commands.h"
#include "Graphics.h"
#include "InfraPaths.h"
#include "Probing.h"
#include "Highlighting.h"
#include "ArduinoStuff.h"
#include "Menus.h"
#include "LEDs.h"

#include "MCP4728.h"  // New library
#include "WaveGen.h"  // wavegen.isRunning() - shared I2C0 bus arbitration
#include "AdcRing.h"  // the always-on ADC ring (T2.1): readAdc() reads it when active
extern WaveGen wavegen; // defined in main.cpp

// ============================================================================
// Peripherals Class Implementation
// ============================================================================

// Static member initialization
static void resetCurrentSenseMeasurement();
static bool pollCurrentSenseMeasurement();
static unsigned long lastCurrentSensePollMs = 0;
static constexpr unsigned long CURRENT_SENSE_POLL_INTERVAL_MS = 50;
// Scan-scoped fast cadence - see Peripherals.h. Volatile: set on core 0 by
// the scan sessions, read by the same core's poll, but keep the intent
// explicit for anyone moving the poll later.
static volatile bool s_inaFastPoll = false;

void inaFastPollMode(bool on) {
    if (on == s_inaFastPoll) return;
    s_inaFastPoll = on;
    // 2^n samples: 0 = 1 sample (~532us), 4 = 16 samples (~8.5ms). Shunt
    // config NEVER changes here (the 5 uA/LSB contract above).
    INA0.setBusSamples(on ? 0 : 4);
}
static constexpr float CURRENT_SENSE_FILTER_ALPHA = 0.55f;
static constexpr float CURRENT_SENSE_DIRECTION_EPSILON_MA = 0.25f;
Peripherals& Peripherals::getInstance() {
    static Peripherals inst;
    return inst;
}

Peripherals::Peripherals() {
    // Initialize defaults
    gpioToggleFrequency = 250; // ms
}

/**
 * @brief Main service method for peripherals system
 * 
 * This is called each loop iteration and handles:
 * - Measurement display (if enabled)
 */
ServiceStatus Peripherals::service() {
    lastStatus = ServiceStatus::IDLE;
    
   // if (millis() > 3000) {
    if (pollCurrentSenseMeasurement()) {
        lastStatus = ServiceStatus::BUSY;
   // }
}

    // Show measurements if enabled
    if (showReadings >= 1) {
        showMeasurements(16, 0, 0);
        lastStatus = ServiceStatus::BUSY;
    }
   // showLEDmeasurements();
    
    return lastStatus;
}

static void resetCurrentSenseMeasurement() {
    currentSenseState.active = false;
    currentSenseState.current_mA = 0.0f;
    currentSenseState.filteredCurrent_mA = 0.0f;
    currentSenseState.busVoltage_V = 0.0f;
    currentSenseState.shuntVoltage_mV = 0.0f;
    currentSenseState.currentDirection = 0;
    lastCurrentSensePollMs = 0;
}

unsigned long lastCurrentSenseOffsetPollMs = 0;

static bool pollCurrentSenseMeasurement() {
    if ( !currentSenseState.plusConnected || !currentSenseState.minusConnected ) {
        if ( currentSenseState.active ) {
            resetCurrentSenseMeasurement();
        }
        return false;
    }

    unsigned long now = millis();
    // Fast mode (scan sessions): no wall-clock pacing - the CNVR flag,
    // cleared each read below, paces polls to the real ~9ms conversions.
    if ( !s_inaFastPoll &&
         now - lastCurrentSensePollMs < CURRENT_SENSE_POLL_INTERVAL_MS ) {
        return false;
    }
    // Attempt gate, stamped BEFORE any bus traffic: this runs from
    // serviceInner() too (probeMode's ~20 us loop), and the poll stamp
    // above only advances on a completed read - so a not-ready conversion
    // used to be re-asked over I2C on every single pass. >= 10 ms between
    // attempts bounds the CNVR read to 100 Hz whatever the loop rate
    // (fast mode: 2 ms / 500 Hz, one ~0.1ms bus read per attempt).
    static unsigned long lastAttemptMs = 0;
    if ( now - lastAttemptMs < ( s_inaFastPoll ? 2u : 10u ) ) {
        return false;
    }
    lastAttemptMs = now;

    // if (now - lastCurrentSenseOffsetPollMs > 20000 && false) {
    //     // Temporarily disconnect ISENSE_PLUS from whatever it is connected to,
    //     // measure the offset, then restore the original connections.

    //     // These are defined in FileParsing.cpp and track which nodes were
    //     // disconnected by removeBridgeFromState(node, -1, ...)
    //     extern int lastRemovedNodes[20];
    //     extern int lastRemovedNodesIndex;

    //     // Local copy so we can safely restore after measurement
    //     int savedNodes[20];
    //     int savedCount = 0;

    //     bool hadConnections = removeBridgeFromState(ISENSE_PLUS, -1, true);

    //     if (hadConnections && lastRemovedNodesIndex > 0) {
    //         for (int i = 0; i < lastRemovedNodesIndex && i < 20; i++) {
    //             savedNodes[i] = lastRemovedNodes[i];
    //         }
    //         savedCount = lastRemovedNodesIndex;
    //         // waitCore2();
    //         // delayMicroseconds(10000);
    //     }

    //     while ( INA0.getConversionFlag() == false ) {
    //         tight_loop_contents();
    //     }

    //     lastCurrentSenseOffsetPollMs = now;
    
    //     // delay(1000);
    //     currentReadingOffset0_mA = INA0.getCurrent_mA();
    //     Serial.println("currentReadingOffset0_mA: " + String(currentReadingOffset0_mA));
    //     Serial.flush();

    //     // Restore any original connections for ISENSE_PLUS
    //     if (hadConnections && savedCount > 0) {
    //         for (int i = 0; i < savedCount; i++) {
    //             int otherNode = savedNodes[i];
    //             if (otherNode > 0) {
    //                 // -1 duplicates parameter = allow duplicates as needed
    //                 addBridgeToState(ISENSE_PLUS, otherNode, -1, true);
    //             }
    //         }
    //         // waitCore2();
    //         // delayMicroseconds(10000);
    //     }
    // }

    // BUS gate: while the function generator runs, its stream owns I2C0
    // (T3.3: a DMA stream started from core 0; before that, core 1's
    // blocking loop) - the INA219s share that bus. Skip the poll entirely.
    //
    // (2026-08-16) No core-1 pause toggle around the read any more. I2C0 is
    // core-0-only - the INA219s, the MCP4728 (Peripherals' mcp) and the
    // internal-I2C0 OLED are all driven from core 0; core 1's only Wire
    // user is WaveGen, and it is excluded above by isRunning(). The old
    // "pause core 1; delay 50 us; read; restore" made core 1 abort
    // whatever LED frame it was in 20 times a second for nothing.
    if ( wavegen.isRunning( ) ) {
        return false;
    }

    // CNVR is real now: the POWER read below clears it every poll (the
    // Probing.cpp INA1 idiom), so this is true only when a genuinely NEW
    // conversion landed - which is what lets fast mode drop the wall-clock
    // gate without ever stamping the same conversion fresh twice (the
    // stale-read discipline inaSettledMa is built on).
    if ( !INA0.getConversionFlag() ) {
        return false;
    }

    lastCurrentSensePollMs = now;

    INA0.getLastError();  // flush stale error flag
    float current_mA = INA0.getCurrent_mA()- currentReadingOffset0_mA;
    if ( INA0.getLastError() != 0 ) {
        // Failed I2C read: the driver returns 0, which would show up as a
        // fake (-offset) current. Keep the previous state instead.
        return false;
    }
    float busVoltage = 0.0f;//min(8.0f, fabs(current_mA * 0.5f));
   // float busVoltage = 5.0;//INA0.getBusVoltage();

  //  if ( !currentSenseState.active ) {
        currentSenseState.filteredCurrent_mA = current_mA;
    // } else {
    //     currentSenseState.filteredCurrent_mA =
    //         ( CURRENT_SENSE_FILTER_ALPHA * current_mA ) +
    //         ( ( 1.0f - CURRENT_SENSE_FILTER_ALPHA ) * currentSenseState.filteredCurrent_mA );
    // }

    currentSenseState.current_mA = current_mA;
    currentSenseState.busVoltage_V = busVoltage;

    // SHUNT VOLTAGE: the fine current source (invest-measurement.md 0(a)/1.7).
    // The current register's LSB is fixed by setMaxCurrentShunt(1, 2.0) at
    // 30.5 uA/bit, so a 47k part at 3.3 V (~70 uA) is TWO counts - which is
    // exactly the bench's val=0.03mA. The shunt-voltage register's LSB is a
    // hardware constant 10 uV, i.e. 5 uA across the 2 ohm R1: six times finer,
    // and available without touching calibration. THE CONFIG STAYS UNTOUCHED -
    // this poll owns the chip's cadence and its CNVR flag; consumers only read
    // the field (see inaShuntCurrent_mA in Peripherals.h).
    //
    // Same failed-read discipline as the current read above: getShuntVoltage()
    // returns 0 on an I2C error, and a fake 0 mV reads downstream as a
    // confident "no current". Hold the previous value instead.
    float shunt_mV = INA0.getShuntVoltage_mV();
    bool shuntOk = ( INA0.getLastError() == 0 );
    if ( shuntOk ) {
        currentSenseState.shuntVoltage_mV = shunt_mV;
    }
    // Reading POWER clears CNVR (Probing.cpp's INA1 idiom), re-arming the
    // conversion flag so the next poll only fires on a genuinely new sample.
    (void)INA0.getPower_mW();

    int direction = 0;
    if ( current_mA > CURRENT_SENSE_DIRECTION_EPSILON_MA ) {
        direction = 1;
    } else if ( current_mA < -CURRENT_SENSE_DIRECTION_EPSILON_MA ) {
        direction = -1;
    }
    currentSenseState.currentDirection = direction;

    currentSenseState.active = true;
    // The stamp is what makes a sample FRESH to a consumer (the guide check's
    // inaAccumulateFresh averages one reading per new stamp). Holding the
    // previous shuntVoltage_mV on an I2C error is only half the fix: stamping
    // anyway would hand that held value out as a new sample and let it be
    // averaged twice. Advance the stamp only when the shunt read really
    // landed. current_mA already returned early on ITS error, so a stamped
    // tick means both halves are good.
    if ( shuntOk ) {
        currentSenseState.lastUpdatedMs = now;
    }

    return true;
}

/**
 * @brief Public method to poll current sense measurements
 * 
 * This is exposed publicly so it can be called from serviceInner()
 * to keep current measurements updating even during blocking operations
 * like probe mode.
 */
void Peripherals::pollCurrentSense() {
    pollCurrentSenseMeasurement();
}

// Backward compatibility - create references to singleton members
unsigned long& gpioToggleFrequency = Peripherals::getInstance().gpioToggleFrequency;
int& showReadings = Peripherals::getInstance().showReadings;

// ============================================================================
// Existing Functions
// ============================================================================

int i2cSpeed = 400000;

// Compatibility for clangd - these are provided by Arduino.h at compile time
#ifndef abs
#define abs( x ) ( ( x ) < 0 ? -( x ) : ( x ) )
#endif
#ifndef sin
extern double sin( double );
#endif
#ifndef round
extern double round( double );
#endif

#define CSI Serial.write( "\x1B\x5B" );

#define DAC_RESOLUTION 9

float adcRange[ 8 ][ 2 ] = { { -8, 8 }, { -8, 8 }, { -8, 8 }, { -8, 8 }, { 0, 5 } };

float dacSpread[ 4 ] = { 20.2, 20.2, 20.2, 20.2 };
int dacZero[ 4 ] = { 1650, 1650, 1650, 1650 };

float adcSpread[ 8 ] = { 18.28, 18.28, 18.28, 18.28, 5.0, 17.28, 21.6, 17.28 };
float adcZero[ 8 ] = { 8.0, 8.0, 8.0, 8.0, 0.0, 8.0, 8.0, 8.0 };


int revisionNumber = 0;
int probeRevision = 0;

float adcReadings[ 8 ] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// int adcNet[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
int showADCreadings[ 8 ] = { 0, 0, 0, 0, 0, 0, 0, 0 };
uint32_t adcReadingColors[ 8 ] = { 0x050505, 0x050505, 0x050505,
                                   0x050505, 0x050505, 0x050505 };
float adcReadingRanges[ 8 ][ 2 ] = {
    { -8.0, 8.0 },
    { -8.0, 8.0 },
    { -8.0, 8.0 },
    { -8.0, 8.0 },
    { 0.0, 5.0 },
};

int inaConnected = 0;
int showINA0[ 3 ] = { 1, 1, 1 }; // 0 = current, 1 = voltage, 2 = power
int showINA1[ 3 ] = { 0, 0, 0 }; // 0 = current, 1 = voltage, 2 = power

int showDAC0 = 0;
int showDAC1 = 0;

int adcCalibration[ 6 ][ 3 ] = { { 0, 0, 0 },
                                 { 0, 0, 0 },
                                 { 0, 0, 0 },
                                 { 0, 0, 0 } }; // 0 = min, 1 = middle, 2 = max,
int dacCalibration[ 4 ][ 3 ] = { { 0, 0, 0 },
                                 { 0, 0, 0 },
                                 { 0, 0, 0 },
                                 { 0, 0, 0 } }; // 0 = min, 1 = middle, 2 = max,

float freq[ 3 ] = { 1, 1, 0 };
uint32_t period[ 3 ] = { 0, 0, 0 };
uint32_t halvePeriod[ 3 ] = { 0, 0, 0 };

// q = square
// s = sinus
// w = sawtooth
// t = stair
// r = random
char mode[ 3 ] = { 'z', 'z', 'z' };

int dacOn[ 3 ] = { 0, 0, 0 };
int amplitude[ 3 ] = { 0, 0, 0 };
int offset[ 3 ] = { 2048, 1650, 0 };
int calib[ 3 ] = { 0, 0, 0 };

MCP4728 mcp;


// MCP4725_PICO dac0_5V(5.0);
// MCP4725_PICO dac1_8V(railSpread);

// MCP4822 dac_rev3; // A is dac0  B is dac1

INA219 INA0( 0x40 );
INA219 INA1( 0x41 );

uint16_t count;
uint32_t lastTime = 0;

// LOOKUP TABLE SINE
uint16_t sine0[ 360 ];
uint16_t sine1[ 360 ];

int reads2[ 30 ][ 2 ];
int readIndex2 = 0;

int foundRolloff = 0;

int lastReading = 0;
int dacSetting = 0;
int reads[ 30 ][ 2 ];
int readIndex = 0;

void initADC( void ) {
    // Initialize pico-sdk ADC for direct hardware access
    adc_init( );

    // Make sure GPIO pins are set up for ADC

    // Note: ADC4 is temperature sensor (internal)    adc_fifo_setup( false, false, 0, false, false );
    adc_fifo_drain( );

    // Set Arduino ADC resolution back to 12 bits for compatibility
    // analogReadResolution( 12 );
    adc_set_clkdiv( 1.0 );
    adc_select_input( 0 );
    adc_set_round_robin( 0 );
    adc_run( false );

    #if (OG_JUMPERLESS)
        // Re-enable ADC GPIO pins for normal operation
    for ( int i = 0; i < 4; i++ ) {
        adc_gpio_init( 26 + i ); // Jumperless ADCs on pins 26-29
    }
    #else


    // Re-enable ADC GPIO pins for normal operation
    for ( int i = 0; i < 8; i++ ) {
        adc_gpio_init( 40 + i ); // Jumperless ADCs on pins 40-47
    }
    #endif

    // Set Arduino ADC resolution to 12 bits for compatibility
    analogReadResolution( 12 );
}

void initDAC( void ) {

    
    initGPIO( );

#if defined(OG_JUMPERLESS)
    // OG Jumperless uses an MCP4822 dual DAC on SPI (pin 1 = CS, pin 2 = SCK,
    // pin 3 = MOSI), NOT the V5's MCP4728 quad DAC on I2C. The SPI DAC backend
    // is Phase 2 work; until then, skip the I2C probe entirely so we don't
    // waste boot time scanning addresses and printing "Failed to find MCP4728".
    return;
#endif

    Wire.setSDA( 4 );
    Wire.setSCL( 5 );
    // This is THE I2C0 clock owner (I2C0_BUS_CLOCK_HZ, JumperlessDefines.h).
    // 1MHz, not 1.7MHz: this bus is shared with both INA219s, and 1.7MHz is
    // far past their non-high-speed rating (Hs-mode needs a master-code
    // handshake the RP2350 I2C block never sends) - it produced intermittent
    // silently-failed reads. WaveGen's sample-rate math already assumes 1MHz,
    // so DAC streaming throughput is unchanged.
    // ponytail: 1MHz is still above the INA219's 400kHz F/S rating; if reads
    // are ever still flaky, 400000 is the fully-in-spec fallback (costs 2.5x
    // wavegen sample rate).
    // Nobody else may setClock() this bus: MCP4728::begin() used to force
    // 1.7MHz here (and again on every wavegen_start()), and the OLED driver on
    // connection_type 2 used to leave it at 400kHz - both fixed (T1.9). The
    // OLED still transfers at its own 400kHz but restores this rate after.
    Wire.setClock( I2C0_BUS_CLOCK_HZ );
    Wire.begin( );

    delayMicroseconds( 100 );
    pinMode( LDAC, OUTPUT );
   
    int mcpAddress = 0x60;
    mcp.setAddress(mcpAddress);
    // Try to initialize!
    while ( !mcp.begin(mcpAddress) ) {
        delay(  1 );
       // Serial.println( "Failed to find MCP4728 chip at 0x" + String(mcpAddress, HEX) );
        mcpAddress++;
        mcp.setAddress(mcpAddress);
        if ( mcpAddress > 0x67 ) {
            Serial.println( "Failed to find MCP4728 chip" );
            return;
        }
    }

    if (mcpAddress != 0x60) {
        mcp.setMCPAddressBits(0x0);
    } 

   // Serial.println( "Found MCP4728 chip at 0x" + String(mcpAddress, HEX) );


    

    digitalWrite( LDAC, HIGH );

    // // Vref = MCP_VREF_VDD, value = 0, 0V
    mcp.setChannelValue( MCP4728_CHANNEL_A, 1650 );
    mcp.setChannelValue( MCP4728_CHANNEL_B, 1650 );
    mcp.setChannelValue( MCP4728_CHANNEL_C, 1660 );
    mcp.setChannelValue( MCP4728_CHANNEL_D, 1641 ); // 1650 is roughly 0V
    digitalWrite( LDAC, LOW );

    delayMicroseconds( 1000 );

    setRailsAndDACs( 0 );
    delayMicroseconds( 1000 );
}

int findI2CAddress( int sdaPin, int sclPin, int i2cNumber, int print ) {
    int address = -1;
    for ( int i = 0; i < 128; i++ ) {
        if ( i2cNumber == 0 ) {
            Wire.beginTransmission( i );
        } else {
            Wire1.beginTransmission( i );
        }
        int error = 0;
        if ( i2cNumber == 0 ) {
            error = Wire.endTransmission( );
        } else {
            error = Wire1.endTransmission( );
        }

        if ( error == 0 ) {
            address = i;
            if ( print == 1 ) {
                Serial.print( "Found I2C address: " );
                Serial.println( address );
            }
            break;
        }
    }
    return address;
}

int initI2C( int sdaPin, int sclPin, int speed ) {

    // Serial.println("initI2C");
    static int i2c1Pins[ 3 ] = { 26, 27, 100000 };
    static int i2c0Pins[ 3 ] = { 4, 5, 100000 };

    int gpioI2Cmap[ 15 ][ 3 ] = {
        { 0, 0, 0 },
        { 1, 1, 0 },
        { 4, 0, 0 },
        { 5, 1, 0 },
        { 6, 0, 1 },
        { 7, 1, 1 },
        { 20, 0, 0 },
        { 21, 1, 0 },
        { 22, 0, 1 },
        { 23, 1, 1 },
        { 24, 0, 0 },
        { 25, 1, 0 },
        { 26, 0, 1 },
        { 27, 1, 1 },
    };
    // I2C0 is taken by current sensors and dac
    // maybe bitbang I2C0?
    int sdaFound = 0;
    int sclFound = 0;
    int portFound = -1;  // Will be determined from mapping

    for ( int i = 0; i < 15; i++ ) {
        if ( gpioI2Cmap[ i ][ 0 ] == sdaPin ) {
            sdaFound = i;
            portFound = gpioI2Cmap[ i ][ 2 ];  // Get I2C port from mapping (0 or 1)
        }
        if ( gpioI2Cmap[ i ][ 0 ] == sclPin ) {
            sclFound = i;
        }
        if ( sdaFound != 0 && sclFound != 0 ) {
            break;
        }
    }

    if ( sdaFound == 0 || sclFound == 0 || portFound == -1 ) {
        // Serial.println("Failed to find I2C pins");
        return -1;
    } else if ( portFound == 0 ) {
        // I2C0 (Wire) - used for GPIO 4/5, 0/1, 20/21, 24/25
        gpio_set_pulls( sdaPin, true, false ); // Enable pull-up on SDA
        gpio_set_pulls( sclPin, true, false ); // Enable pull-up on SCL

        Wire.setSDA( sdaPin );
        Wire.setSCL( sclPin );
        Wire.setClock( speed );
        Wire.begin( );
        // Bound any single I2C0 transaction to 15ms so a hot-unplugged
        // device (OLED on rev 7, etc.) can't wedge the main loop while
        // checkConnection() catches up and clears oledConnected.
        // reset_with_timeout=true: on timeout, arduino-pico reinits the I2C
        // block and clocks SCL to release a stuck-low SDA, so one wedged
        // transaction doesn't corrupt every INA/DAC read after it. Bounded
        // (~9 SCL toggles + reinit), no meaningful blocking.
        Wire.setTimeout( 15, true );

        if ( i2c0Pins[ 0 ] == sdaPin && i2c0Pins[ 1 ] == sclPin &&
             i2c0Pins[ 2 ] == speed ) {
            return gpioI2Cmap[ sdaFound ][ 2 ] +
                   10; // returns 10 if the pins are already set
        }
        i2c0Pins[ 0 ] = sdaPin;
        i2c0Pins[ 1 ] = sclPin;
        i2c0Pins[ 2 ] = speed;
        i2cSpeed = speed;

        return gpioI2Cmap[ sdaFound ][ 2 ];
    } else if ( portFound == 1 ) {
        // I2C1 (Wire1) - used for GPIO 6/7, 26/27, 22/23
        // Only update gpioState for GPIO pins 20-29 (valid gpioDef indices)
        if ( sdaPin >= 20 && sdaPin <= 29 ) {
            gpioState[ gpioDef[ sdaPin - 20 ][ 2 ] ] = 6;
        }
        if ( sclPin >= 20 && sclPin <= 29 ) {
            gpioState[ gpioDef[ sclPin - 20 ][ 2 ] ] = 6;
        }

        gpio_set_pulls( sdaPin, true, false ); // Enable pull-up on SDA
        gpio_set_pulls( sclPin, true, false ); // Enable pull-up on SCL

        Wire1.setSDA( sdaPin );
        Wire1.setSCL( sclPin );
        Wire1.setClock( speed );
        Wire1.begin( );
        // Bound any single I2C1 transaction to 15ms so a hot-unplugged
        // OLED can't wedge show()/display() for the Earle pico Wire
        // default (hundreds of ms) before checkConnection() flips
        // oledConnected to false. reset_with_timeout=true recovers a
        // stuck bus (see Wire.setTimeout above).
        Wire1.setTimeout( 15, true );

        i2cSpeed = speed;

        if ( i2c1Pins[ 0 ] == sdaPin && i2c1Pins[ 1 ] == sclPin &&
             i2c1Pins[ 2 ] == speed ) {
            return gpioI2Cmap[ sdaFound ][ 2 ] +
                   10; // returns 11 if the pins are already set
        }
        i2c1Pins[ 0 ] = sdaPin;
        i2c1Pins[ 1 ] = sclPin;
        i2c1Pins[ 2 ] = speed;

        return gpioI2Cmap[ sdaFound ][ 2 ];
    }

    // Serial.println("Failed to find I2C pins");
    return -1;

    
}


// Debug flag for fake GPIO visual integration
bool debugFakeGpio = false;  // Set to true to see detailed fake GPIO debug output

volatile bool readingADC = false;
volatile bool usingI2C = false;
CurrentSenseState currentSenseState;


// ============================================================================
// Fake GPIO Background Reading
// ============================================================================
// Moved to FakeGpio.cpp -- readFakeGPIO() is now defined there.
// This section was legacy dead code (immediately returned without doing anything).

void setRailsAndDACs( int saveEEPROM ) {

    // Serial.println("setRailsAndDACs");
    // Serial.flush();
    setTopRail( globalState.power.topRail, 1, 0 );
    // delay(10);
    setBotRail( globalState.power.bottomRail, 1, 0 );
    // delay(10);
    setDac0voltage( globalState.power.dac0, 1, 0 );
    // delay(10);
    setDac1voltage( globalState.power.dac1, 1, saveEEPROM );
    // delay(10);
}
// The voltage most recently WRITTEN to each rail, whether or not the write was
// persisted - the exact twin of s_dacHwVolts below, and for the same reason.
// Until now "the rails only ever move through the state" was true, so every
// rail readout could read globalState.power. The guide's exit restore broke
// that: it puts the user's pre-guide rails back with save=0 on purpose (the
// project's run file must keep the safe 0 V it was left at), which left every
// readout on the board reporting 0 V over rails that were physically live -
// the "rails aren't setting" false-bug class.
//
// PERSISTENCE NEVER READS THIS. What gets saved is globalState.power,
// untouched; this array feeds READOUTS only (dac_get 2/3, the rail net
// reading, the rail LED dots).  -100 = never written.
float railHwVolts[ 2 ] = { -100.0f, -100.0f };   // [0] = top, [1] = bottom

void setTopRail( float value, int save, int saveEEPROM ) {

    int dacValue = ( value * 4095 / dacSpread[ 2 ] ) + dacZero[ 2 ];

    if ( dacValue > 4095 ) {
        dacValue = 4095;
    } else if ( dacValue < 0 ) {
        dacValue = 0;
    }

    // Cached write: the driver skips the I2C transaction when the chip
    // already holds this exact word (setRailsAndDACs re-sends every rail on
    // several paths). LDAC only needs to fall when something was written.
    bool wrote = false;
    digitalWrite( LDAC, HIGH );
    mcp.setChannelValueCached( MCP4728_CHANNEL_C, dacValue, MCP4728_VREF_VDD,
                               MCP4728_GAIN_1X, MCP4728_PD_MODE_NORMAL, &wrote );
    digitalWrite( LDAC, LOW );
    (void)wrote;

    railHwVolts[ 0 ] = value;   // hardware truth, persisted or not

    // Update globalState for YAML persistence (single source of truth)
    // ONLY update when save == 1 to avoid spurious dirty marks
    if ( save ) {
        globalState.setRailVoltage(true, value);  // true = top rail
        //configChanged = true;
    }
    if ( saveEEPROM && false) {
        saveVoltages( globalState.power.topRail,
                      globalState.power.bottomRail, globalState.power.dac0,
                      globalState.power.dac1 );
    }
}

void setBotRail( float value, int save, int saveEEPROM ) {

    int dacValue = ( value * 4095 / dacSpread[ 3 ] ) + dacZero[ 3 ];

    if ( dacValue > 4095 ) {
        dacValue = 4095;
    } else if ( dacValue < 0 ) {
        dacValue = 0;
    }

    bool wrote = false;
    digitalWrite( LDAC, HIGH );
    mcp.setChannelValueCached( MCP4728_CHANNEL_D, dacValue, MCP4728_VREF_VDD,
                               MCP4728_GAIN_1X, MCP4728_PD_MODE_NORMAL, &wrote );
    digitalWrite( LDAC, LOW );
    (void)wrote;

    railHwVolts[ 1 ] = value;   // hardware truth, persisted or not

    if ( save ) {
        // Update globalState for YAML persistence (single source of truth)
        globalState.setRailVoltage(false, value);  // false = bottom rail
        //configChanged = true;
    }
    if ( saveEEPROM && false) {
        saveVoltages( globalState.power.topRail,
                      globalState.power.bottomRail, globalState.power.dac0,
                      globalState.power.dac1 );
    }
}


// The voltage most recently WRITTEN to each DAC, whether or not the write was
// persisted (save=0 writes - the MicroPython dac_set() default - move only the
// hardware and leave globalState alone). This is the hardware truth that the
// probe-power feed must respect: a user script that sets DAC0 to 2.5 V without
// "save" has still taken DAC0, and the feed re-parking it at 3.33 V on the
// next rebuild (which is what happened when viability was judged from
// globalState) reads as "my DAC voltage silently reverted".
static float s_dacHwVolts[ 2 ] = { -100.0f, -100.0f };   // -100 = never written

// Set when a USER write (terminal / MicroPython - the checkProbePower paths)
// parks DAC 0/1 outside the probe-power window; cleared when a user write puts
// it back inside. Blind writes from calibration and the self test
// (checkProbePower=false) don't touch it - those WANT the feed to re-park the
// DAC afterwards (they bump the park epoch for that).
static bool s_dacUserClaimed[ 2 ] = { false, false };

bool dacUserClaimed( int dac ) {
    return ( dac == 0 || dac == 1 ) ? s_dacUserClaimed[ dac ] : false;
}

float getDacHardwareVoltage( int dac ) {
    if ( dac == 0 || dac == 1 ) {
        return ( s_dacHwVolts[ dac ] > -99.0f ) ? s_dacHwVolts[ dac ] : getDacVoltage( dac );
    }
    // Channels 2/3 are the rails, and they now answer the same way (see
    // railHwVolts). Before the guide's save=0 restore existed, rails only ever
    // moved through the state, so this fell through to globalState.power - and
    // still does until the first rail write of the session.
    if ( dac == 2 || dac == 3 ) {
        int r = dac - 2;
        return ( railHwVolts[ r ] > -99.0f ) ? railHwVolts[ r ] : getDacVoltage( dac );
    }
    return getDacVoltage( dac );
}

float getDacVoltage( int dac ) {
    if ( dac == 0 ) {
        return globalState.power.dac0;
    } else if ( dac == 1 ) {
        return globalState.power.dac1;
    } else if ( dac == 2 ) {
        return globalState.power.topRail;
    } else if ( dac == 3 ) {
        return globalState.power.bottomRail;
    }
    return 0;
}



int failedToSetDac0 = 0;
void setDac0voltage( float voltage, int save, int saveEEPROM,
                     bool checkProbePower ) {
    // int dacValue = (voltage * 4095 / 19.8) + 1641;
    int dacValue = ( voltage * 4095 / dacSpread[ 0 ] ) + dacZero[ 0 ];

    if ( dacValue > 4095 ) {
        dacValue = 4095;
    }
    if ( dacValue < 0 ) {
        dacValue = 0;
    }

    // Set-once: the driver's per-channel shadow skips the I2C write when
    // DAC0 already holds this word - the probe feed's park calls this on
    // EVERY rebuild, and used to cost one MCP4728 transaction each time.
    // Everything below the write (state, hardware-truth volts, the claim
    // latches and their nudges) runs whether or not a byte went out: the
    // bookkeeping is about what the caller MEANT, not about the bus.
    bool wrote = false;
    digitalWrite( LDAC, HIGH );
    // delay(10);
    if ( mcp.setChannelValueCached( MCP4728_CHANNEL_A, dacValue, MCP4728_VREF_VDD,
                                    MCP4728_GAIN_1X, MCP4728_PD_MODE_NORMAL,
                                    &wrote ) == false ) {
        // delay(3000);
        //Serial.println( "Failed to set DAC0 value" );
#if !defined(OG_JUMPERLESS)
        // OG has no MCP4728 at all (initDAC returns before begin()), so every
        // write "fails" there — counting them would just print forever.
        failedToSetDac0++;
        if ( failedToSetDac0 > 10 ) {
            Serial.println( "Failed to set DAC0 value" );
            failedToSetDac0 = 0;
        }
#endif
    } else {
        failedToSetDac0 = 0;
    }
    // delay(10);
    digitalWrite( LDAC, LOW );
    (void)wrote;
    
    // Update globalState for YAML persistence (single source of truth)
    // ONLY update state if save == 1 to avoid marking state dirty unnecessarily
    if ( save ) {
        globalState.setDacVoltage(0, voltage);
    }
    s_dacHwVolts[ 0 ] = voltage;   // hardware truth, persisted or not

    // A user parking this DAC outside the probe-power window claims it: the
    // nudged rebuild's infra evaluation sees the candidate non-viable (the
    // claim latch below is what it checks, so this holds for save=0 writes
    // too - the MicroPython dac_set() default) and relocates the buffer feed.
    // Replaces the old inline probePowerDAC swap.
    // The claim tracks USER writes only (terminal / MicroPython). A blind write
    // - self test, calibration, infra's own park - must CLEAR it: leaving it set
    // meant probe power stayed on the GPIO fallback (ADC7 droop model instead of
    // INA1 sensing) until reboot, because dacVoltageInProbeWindow() kept
    // returning false for a DAC nobody was claiming any more.
    bool wasClaimed0 = s_dacUserClaimed[ 0 ];
    s_dacUserClaimed[ 0 ] = checkProbePower && ( voltage > 3.9 || voltage < 2.80 );
    if ( checkProbePower && infraProbePowerSource( ) == DAC0 &&
         ( voltage > 3.9 || voltage < 2.80 ) ) {
        Serial.println(
            "DAC 0 was powering the probe - relocating the buffer feed" );
        infraNudge( );
    }
    // The RELEASE direction needs the same nudge as the claim. Without it,
    // a dac_set() back inside the probe window cleared the latch but nothing
    // triggered a rebuild, so the buffer feed sat on the GPIO fallback (ADC7
    // droop sensing instead of INA1) until the next unrelated refresh -
    // observed on hardware as switch sensing staying degraded after the user
    // was done with the DAC. Gated on checkProbePower like the claim side:
    // BLIND writes (self test, calibration sweeps, infra's own park) also
    // clear the latch, and a nudge-triggered refresh mid-sweep would churn
    // the crossbar under an in-flight measurement - those keep the old
    // behavior (feed returns at the next rebuild).
    if ( checkProbePower && wasClaimed0 && !s_dacUserClaimed[ 0 ] &&
         infraProbePowerSource( ) != DAC0 ) {
        infraNudge( );
    }

    if ( saveEEPROM && false) {
        saveVoltages( globalState.power.topRail, globalState.power.bottomRail,
                      globalState.power.dac0, globalState.power.dac1 );
    }
}

void setDac1voltage( float voltage, int save, int saveEEPROM,
                     bool checkProbePower ) {

    int dacValue = ( voltage * 4095 / dacSpread[ 1 ] ) + dacZero[ 1 ];

    if ( dacValue > 4095 ) {
        dacValue = 4095;
    }
    if ( dacValue < 0 ) {
        dacValue = 0;
    }
    bool wrote = false;
    digitalWrite( LDAC, HIGH );
    mcp.setChannelValueCached( MCP4728_CHANNEL_B, dacValue, MCP4728_VREF_VDD,
                               MCP4728_GAIN_1X, MCP4728_PD_MODE_NORMAL, &wrote );
    digitalWrite( LDAC, LOW );
    (void)wrote;

    // Update globalState for YAML persistence (single source of truth)
    // ONLY update state if save == 1 to avoid marking state dirty unnecessarily
    if ( save ) {
        globalState.setDacVoltage(1, voltage);
    }
    s_dacHwVolts[ 1 ] = voltage;   // hardware truth, persisted or not
    bool wasClaimed1 = s_dacUserClaimed[ 1 ];
    s_dacUserClaimed[ 1 ] = checkProbePower && ( voltage > 3.9 || voltage < 2.80 );

    // See setDac0voltage: out-of-window saved voltage = user claim; the
    // nudged rebuild reads globalState.power.dac1 (written just above) and
    // relocates the buffer feed. Replaces the old inline probePowerDAC swap.
    // (There is no DAC1 feed candidate today, so this fires only if one is
    // ever added back - kept symmetric with DAC0 on purpose.)
    if ( checkProbePower && infraProbePowerSource( ) == DAC1 &&
         ( voltage > 3.9 || voltage < 2.80 ) ) {
        Serial.println(
            "DAC 1 was powering the probe - relocating the buffer feed" );
        infraNudge( );
    }
    // Release direction, same as DAC0: a user write back inside the window
    // clears the latch and re-evaluates so a candidate that was waiting on
    // DAC1 can take over now rather than at the next unrelated rebuild.
    if ( checkProbePower && wasClaimed1 && !s_dacUserClaimed[ 1 ] &&
         infraProbePowerSource( ) != DAC1 ) {
        infraNudge( );
    }

    if ( saveEEPROM && false) {
        saveVoltages( globalState.power.topRail, globalState.power.bottomRail,
                      globalState.power.dac0, globalState.power.dac1 );
    }
}

uint8_t csToPin[ 16 ] = { 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7 };
// uint8_t csToPin[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
// 15};
uint16_t chipMask[ 16 ] = {
    0b0000000000000001, 0b0000000000000010, 0b0000000000000100,
    0b0000000000001000, 0b0000000000010000, 0b0000000000100000,
    0b0000000001000000, 0b0000000010000000, 0b0000000100000000,
    0b0000001000000000, 0b0000010000000000, 0b0000100000000000,
    0b0001000000000000, 0b0010000000000000, 0b0100000000000000,
    0b1000000000000000 };

// RAM-resident: called from the CH446Q per-crosspoint ISR (isrFromPio) - see CH446Q.cpp.
void __not_in_flash_func( setCSex )( int chip, int value ) {

    if ( chip > 11 ) {
        return;
    }
    #if !defined(OG_JUMPERLESS)

    if ( value > 0 ) {
        gpio_put( chip + 28, 1 );
        // digitalWrite(chip + 28, HIGH);
        //  Serial.println(chip+28);
    } else {
        gpio_put( chip + 28, 0 );
        // digitalWrite(chip + 28, LOW);
        //  Serial.println(chip+28);
    }
    #else
    // OG chip-select GPIO map: crosspoint chips A..H (0-7) -> GPIO 6-13,
    // chips I..L (8-11) -> GPIO 20-23. chip 8 must land on GPIO 20, so the
    // offset for the second bank is +12 (chip - 8 + 20), NOT +20.
    if ( chip >= 0 && chip <= 7 ) {
        if ( value > 0 ) {
            gpio_put( chip + 6, 1 );
        } else {
            gpio_put( chip + 6, 0 );
        }
    } else if ( chip >= 8 && chip <= 11 ) {
        if ( value > 0 ) {
            gpio_put( chip + 12, 1 );
        } else {
            gpio_put( chip + 12, 0 );
        }
    }
    #endif
}


void writeGPIOex( int value, uint8_t pin ) {}

float currentReadingOffset0_mA = 0.0f;
float currentReadingOffset1_mA = 0.0f;

void initINA219( void ) {

#if defined(OG_JUMPERLESS)
    // Initialize standard I2C0 bus on pins 4/5 at 400kHz for the single INA219 chip
    Wire.setSDA( 4 );
    Wire.setSCL( 5 );
    Wire.setClock( 400000 );
    Wire.begin( );

    if ( !INA0.begin( ) ) {
        Serial.println( "Failed to find INA219 chip" );
    }

    INA0.setShuntSamples(4);  // 16 samples averaged
    INA0.setBusSamples(4);    // 16 samples averaged
    INA0.setMaxCurrentShunt( 1, 2.0 );
    INA0.setBusVoltageRange( 16 );

    uint32_t start = millis();
    while ( INA0.getConversionFlag() == false && (millis() - start < 100) ) {
        tight_loop_contents();
    }

    currentReadingOffset0_mA = INA0.getCurrent_mA();
    currentReadingOffset1_mA = 0.0f;
#else
    if ( !INA0.begin( ) || !INA1.begin( ) ) {
        // Remove blocking delay - just log the error
        Serial.println( "Failed to find INA219 chip" );
    }


    // Set shunt samples to 4 (16 averages) for more stable readings
    // This gives ~8.5ms conversion time which is acceptable for probe readings
    INA0.setShuntSamples(4);  // 16 samples averaged
    INA1.setShuntSamples(4);  // 16 samples averaged
    
    // Also set bus samples for consistency
    INA0.setBusSamples(4);    // 16 samples averaged
    INA1.setBusSamples(4);    // 16 samples averaged

    INA0.setMaxCurrentShunt( 1, 2.0 );
    INA1.setMaxCurrentShunt( 1, 2.0 );

    INA0.setBusVoltageRange( 16 );
    INA1.setBusVoltageRange( 16 );


    uint32_t start0 = millis();
    while ( INA0.getConversionFlag() == false && (millis() - start0 < 100) ) {
        tight_loop_contents();
    }


    // delay(1000);
    currentReadingOffset0_mA = INA0.getCurrent_mA();

    uint32_t start1 = millis();
    while ( INA1.getConversionFlag() == false && (millis() - start1 < 100) ) {
        tight_loop_contents();
    }
    // delay(1000);
    currentReadingOffset1_mA = INA1.getCurrent_mA();
    // Serial.println("currentReadingOffset0_mA: " + String(currentReadingOffset0_mA));
    // Serial.println("currentReadingOffset1_mA: " + String(currentReadingOffset1_mA));
    // Serial.flush();
#endif
}

void setDacByNumber( int dac, float voltage, int save, int saveEEPROM,
                     bool checkProbePower ) {
    switch ( dac ) {
    case 0:
        setDac0voltage( voltage, save, saveEEPROM, checkProbePower );
        break;
    case 1:
        setDac1voltage( voltage, save, saveEEPROM, checkProbePower );
        break;
    case 2:
        setTopRail( voltage, save, saveEEPROM );
        break;
    case 3:
        setBotRail( voltage, save, saveEEPROM );
        break;
    }
}

// OPTIMIZATION: Cache readings and only rebuild when nets change
static bool readingsValid = false;
static int lastNumberOfNets = 0;
unsigned long lastRebuildShownReadingsTime = 0;
void __not_in_flash_func(rebuildShownReadings)() {

    // lastRebuildShownReadingsTime = micros( );
    // Clear all readings
    showADCreadings[ 0 ] = 0;
    showADCreadings[ 1 ] = 0;
    showADCreadings[ 2 ] = 0;
    showADCreadings[ 3 ] = 0;
    showADCreadings[ 4 ] = 0;
    showADCreadings[ 5 ] = 0;
    showADCreadings[ 6 ] = 0;
    showADCreadings[ 7 ] = 0;

    showINA0[ 0 ] = 0;
    showINA0[ 1 ] = 0;
    showINA0[ 2 ] = 0;

    inaConnected = 0;

    int detectedPlusNet = -1;
    int detectedMinusNet = -1;

    // Scan paths to find which net each ADC/ISENSE node is connected to
    // We can't use nodeToNetIndex because ADC nodes might not be in nets[].nodes[] arrays
    //
    // Skip paths that belong to FakeGPIO inputs -- they share an ADC via TDM
    // and should not be treated as user-facing ADC measurements. Path nodes are
    // already expanded from FAKE_GP_IN_x to ADCn, so we detect them by checking
    // if the path's net matches any active fakeGpioInput's netIndex.
    // fakeGpioInputAdcChannel and fakeGpioInputs are declared in FakeGpio.h
    int tdmAdcNode = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : -1;

    for ( int i = 0; i < numberOfPaths && i < MAX_BRIDGES; i++ ) {
        int n1 = globalState.connections.paths[ i ].node1;
        int n2 = globalState.connections.paths[ i ].node2;
        int pathNet = globalState.connections.paths[ i ].net;

        // If this path uses the TDM's ADC, check if it belongs to a fake GPIO input
        if ( tdmAdcNode >= 0 && ( n1 == tdmAdcNode || n2 == tdmAdcNode ) ) {
            bool isFakeGpioNet = false;
            for ( int s = 0; s < MAX_FAKE_GP_IN; s++ ) {
                if ( fakeGpioInputs[ s ].active && fakeGpioInputs[ s ].netIndex == pathNet ) {
                    isFakeGpioNet = true;
                    break;
                }
            }
            if ( isFakeGpioNet ) continue;  // Skip -- TDM handles this, not regular ADC display
        }

        // Check ADC channels
        if ( n1 == ADC0 || n2 == ADC0 ) {
            showADCreadings[ 0 ] = pathNet;
        }
        if ( n1 == ADC1 || n2 == ADC1 ) {
            showADCreadings[ 1 ] = pathNet;
        }
        if ( n1 == ADC2 || n2 == ADC2 ) {
            showADCreadings[ 2 ] = pathNet;
        }
        if ( n1 == ADC3 || n2 == ADC3 ) {
            showADCreadings[ 3 ] = pathNet;
        }
        if ( n1 == ADC4 || n2 == ADC4 ) {
            showADCreadings[ 4 ] = pathNet;
        }
        
        // Check current sense channels
        if ( globalState.connections.paths[ i ].node1 == ISENSE_PLUS || globalState.connections.paths[ i ].node2 == ISENSE_PLUS ) {
            detectedPlusNet = globalState.connections.paths[ i ].net;
        }
        if ( globalState.connections.paths[ i ].node1 == ISENSE_MINUS || globalState.connections.paths[ i ].node2 == ISENSE_MINUS ) {
            detectedMinusNet = globalState.connections.paths[ i ].net;
        }
    }

    currentSenseState.plusNet = detectedPlusNet;
    currentSenseState.minusNet = detectedMinusNet;
    currentSenseState.plusConnected = ( detectedPlusNet > 0 );
    currentSenseState.minusConnected = ( detectedMinusNet > 0 );

    if ( currentSenseState.plusConnected && currentSenseState.minusConnected ) {
        inaConnected = 1;
        showINA0[ 0 ] = 1;
    } else {
        inaConnected = 0;
        showINA0[ 0 ] = 0;
        resetCurrentSenseMeasurement();
    }

    // Serial.println(micros( ) - lastRebuildShownReadingsTime);
    
    // Memory barrier to ensure all writes to showADCreadings[] are visible to other cores
    __dmb();
}

void chooseShownReadings( void ) {
    // OPTIMIZATION: Only rebuild when nets change or paths change
    // This saves ~30-40us per refresh when nothing has changed
    extern int numberOfNets;
    
    // Always rebuild on Core 2 since we call this every frame now (see main.cpp Core 2 loop)
    // This ensures ADC mappings are always fresh from current paths
    // The ~20us cost is acceptable for correctness
    rebuildShownReadings();
    readingsValid = true;
    lastNumberOfNets = numberOfNets;
    
    // Note: If we wanted to optimize further, we could cache based on:
    // - numberOfPaths hash (to detect path changes)
    // - lastNetCount (to detect net structure changes)
    // But for now, always rebuilding ensures no race conditions
}

float railSpread = 17.88;

unsigned long lastShowLEDmeasurementsStartTime = 0;
unsigned long showLEDmeasurementsInterval = 5000;

void __not_in_flash_func(showLEDmeasurements)( void ) {
    //return;

  

    if ( micros( ) - lastShowLEDmeasurementsStartTime < showLEDmeasurementsInterval ) {
        return;
    }

    lastShowLEDmeasurementsStartTime = micros( );

    for ( int i = 0; i < 8; i++ ) {
        int samples = 8;

        float adcReading;

        int bs = 0;

        int numReadings = 0;

        uint32_t color = 0x000000;

        if ( showADCreadings[ i ] > 0 && showADCreadings[ i ] <= numberOfNets ) {
            numReadings++;
            // This runs on core 1 INSIDE core2stuff's core_sync hold - never
            // wait on a contested ADC lock here (every stalled microsecond
            // extends the hold that core 0's blocking core_sync_acquire()
            // callers sit behind). If the lock is busy (probe read on core 0,
            // scan tap), adcReadings[] was refreshed within the last few tens
            // of ms by updateLazyAdcReadings - show that instead of stalling.
            if ( !readingADC ) {
                adcReading = readAdcVoltage( i, samples );
                adcReadings[ i ] = adcReading;
            } else {
                adcReading = adcReadings[ i ];
            }
            color = measurementToColor( adcReading, adcRange[ i ][ 0 ], adcRange[ i ][ 1 ] );

            int brightness =
                LEDbrightnessSpecial +
                (int)abs( adcReading * 2.0 ); // map(abs((adcReading*10)), 0, 80,
                                              // -LEDbrightnessSpecial, 150);
            if ( brightness <= 4 ) {
                brightness = 4;
            } else if ( brightness > 100 ) {
                brightness = 100;
            }

            if ( jumperlessConfig.display.lines_wires == 0 ||
                 numberOfShownNets > MAX_NETS_FOR_WIRES ) {
                lightUpNet( showADCreadings[ i ], -1, 1, brightness, 0, 0, color );
            }
            // Serial.println(brightness);
            int scaleVoltage = map( (int)abs( adcReading ), 0, 8, -30, 70 );
            // Serial.println(scaleVoltage);
            int scaledBrightness = map( brightness, LEDbrightnessSpecial,
                                        LEDbrightnessSpecial + 45, -50, 50 );

            color = scaleBrightness( color, scaleVoltage );

            // Serial.println(scaledBrightness);
            //  Serial.println((color, map(brightness, LEDbrightnessSpecial,
            //  LEDbrightnessSpecial+45, -90, 100)));
            
            // Only update the color cache - assignNetColors() will write to netColors[] and globalState
            // This prevents race condition where both functions write to the same variables
            adcReadingColors[ i ] = color;
            
            // drawWires(showADCreadings[0]);
            // requestLedShow( 2 );
        }
    }
    
    // Memory barrier to ensure all writes to adcReadingColors[] are visible to rendering code
    __dmb();
}

// Lazily refresh the whole adcReadings[] cache from core1 so consumers like the
// OLED GUI ({adc:N} tokens) get live-ish values without doing their own
// blocking hardware read. Cycles one channel per tick to stay light: channels
// 0-4 on the fast interval, 5-7 on the slow one. Self-throttled, so it's safe
// to call every core1 loop iteration. Marked __not_in_flash_func + gated on
// the frame hold/core2busy to match the rest of the core1 hardware paths so it
// can't run during a flash write.
void __not_in_flash_func(updateLazyAdcReadings)( void ) {
#if defined(OG_JUMPERLESS)
    // OG's only consumer of the adcReadings[] lazy cache is the OLED GUI
    // ({adc:N} tokens), and OG has no OLED. This runs on core1 every loop;
    // disabled on OG (MP adc_get() does its own fresh readAdcVoltage). Also a
    // bisection step for the intermittent core1 lockup (the core1 ADC path).
    return;
#endif
#if LAZY_ADC_READINGS
    extern volatile bool core2busy;
    if ( core1FramesHeld( ) ) return; // core0 wants core1 quiesced (e.g. flash write)

#if USB_AUDIO_ENABLE
    // This is the single most important audio guard. It runs on core1 every
    // loop; without it every tick would call readAdcVoltage() on a channel the
    // audio stream owns and the whole cache would decay to 0. The two streamed
    // channels refresh for free from the DMA half-buffer mean; every other
    // entry keeps its last good value instead of being zeroed.
    // (Legacy path only: with the ring engine live usbAudioOwnsAdc is never set.)
    if ( usbAudioOwnsAdc ) {
        usbAudioRefreshLazy( adcReadings );
        return;
    }
#endif

    if ( adcRingActive( ) ) {
        // T2.1: the whole cache is a memory read off the ring - every channel
        // every pass, from the mean of its newest LAZY_ADC_SAMPLES samples; no
        // converter time, no core2busy hold. The resync service rides here
        // (an ADC FIFO overrun asks for a clean restart).
        adcRingService( );
        static unsigned long lastRingUs = 0;
        unsigned long nowUs = micros( );
        if ( (unsigned long)( nowUs - lastRingUs ) < 1000u ) return;   // 1 kHz is plenty for a display cache
        lastRingUs = nowUs;
        for ( int ch = 0; ch < 8; ch++ ) {
            int raw = adcRingMeanNewest( ch, 16 );   // 16 x 21 us: a steadier display than the 4 the burst could afford
            float v = ( raw ) * ( adcSpread[ ch ] / 4095 );
            if ( ch != 4 && ch != 5 ) v -= adcZero[ ch ];
            adcReadings[ ch ] = v;
        }
        return;
    }

    unsigned long nowUs = micros();
    static unsigned long lastFastUs = 0;
    static unsigned long lastSlowUs = 0;
    static uint8_t fastIdx = 0;   // cycles channels 0..4
    static uint8_t slowIdx = 0;   // cycles channels 5..7

    bool didRead = false;
    bool wasBusy = core2busy;

    if ( (unsigned long)( nowUs - lastFastUs ) >= (unsigned long)LAZY_ADC_FAST_INTERVAL_US ) {
        lastFastUs = nowUs;
        core2busy = true;
        adcReadings[ fastIdx ] = readAdcVoltage( fastIdx, LAZY_ADC_SAMPLES );
        fastIdx = ( fastIdx + 1 ) % 5;
        didRead = true;
    }

#if !defined(OG_JUMPERLESS)
    // V5 (RP2350B) has ADC channels 5-7; the RP2040 ADC only has inputs 0-4, so
    // OG must not select channels 5-7 (they read garbage and would also index
    // adcSpread/adcZero out of range). OG's 4 ADCs are covered by the fast loop.
    if ( (unsigned long)( nowUs - lastSlowUs ) >= (unsigned long)LAZY_ADC_SLOW_INTERVAL_US ) {
        lastSlowUs = nowUs;
        core2busy = true;
        uint8_t ch = 5 + slowIdx;
        adcReadings[ ch ] = readAdcVoltage( ch, LAZY_ADC_SAMPLES );
        slowIdx = ( slowIdx + 1 ) % 3;
        didRead = true;
    }
#else
    (void)lastSlowUs; (void)slowIdx;
#endif

    if ( didRead ) {
        core2busy = wasBusy;
        __dmb();   // publish the fresh reading to the other core
    }
#endif
}


uint32_t measurementToColor( float measurement, float min, float max ) {
    uint32_t color = 0;
    hsvColor hsv;
    int minInt = -80;
    int maxInt = 80;
    int measurementInt = measurement * 10;

    // if ( measurement < jumperlessConfig.dacs.limit_min ) {
    //     measurement = jumperlessConfig.dacs.limit_min;
    // } else if ( measurement > jumperlessConfig.dacs.limit_max ) {
    //     measurement = jumperlessConfig.dacs.limit_max;
    // }

    int shift = 228;
    hsv.h = map( measurementInt, minInt, maxInt, 210, 10 );
    hsv.h += shift;
    hsv.s = 255;

    if ( measurement < -0.7 ) {
        hsv.h += 10;

        if ( measurement < -5.4 ) {
            hsv.s -= abs( measurement + 5.4 ) * 48;
            if ( hsv.s < 0 ) {
                hsv.s = 0;
            }
            // Serial.println(measurement + 5.4);
            // Serial.println(hsv.s);
        } else {
            hsv.s = 230;
        }
    } else if ( measurement > 0.5 ) {
        hsv.h += 239;

        if ( measurement > 5.4 ) {
            hsv.s -= ( measurement - 5.4 ) * 48;

            if ( hsv.s < 0 ) {
                hsv.s = 0;
            }
        }
    } else {
        // hsv.h += 254 - (measurement - 0.0) * 64;
    }
    // hsv.s = 255;

    hsv.h = hsv.h % 256;
    hsv.v = jumperlessConfig.display.special_net_brightness;
    // Serial.println(hsv.h);
    //  int measurementInt = measurement * 10;
    //  int distance = abs(0 - measurementInt);
    //  hsv.v = map(distance, min*11, max*11, 0, 100);
    rgbColor rgb = HsvToRgb( hsv );
    color = packRgb( rgb );

    return color;
}

void Peripherals::showMeasurements( int samples, int printOrBB, int oneShot ) {
    unsigned long startMillis = millis( );
    int printInterval = 150;
    static unsigned long lastPrintTime = 0;
    // while (Serial.available() == 0 && Serial1.available() == 0 &&
    //        probing.checkProbeButton() == 0)

    //   {

    if ( millis( ) - lastPrintTime < printInterval ) {
        return;
    }
    Serial.print( "\r                                                             "
                  "         \r" );
    lastPrintTime = millis( );
    // Serial.flush();
    int adc0ReadingUnscaled;
    float adc0Reading;

    int adc1ReadingUnscaled;
    float adc1Reading;

    int adc2ReadingUnscaled;
    float adc2Reading;

    int adc3ReadingUnscaled;
    float adc3Reading;

    int adc4ReadingUnscaled;
    float adc4Reading;

    int adc7ReadingUnscaled;
    float adc7Reading;

    int bs = 0;

    if ( showADCreadings[ 0 ] != 0 ) {
        //       if (readFloatingOrState(ADC0_PIN, -1) == floating)//this doesn't
        //       work because it's buffered
        // {

        //  bs += Serial.print("Floating  ");
        // }
        // adc0ReadingUnscaled = readAdc(0, samples);

        // adc0Reading = (adc0ReadingUnscaled) * (railSpread / 4095);
        // adc0Reading -= railSpread / 2; // offset
        adc0Reading = readAdcVoltage( 0, samples );
        bs += Serial.print( "ADC 0: " );
        bs += Serial.print( adc0Reading );
        bs += Serial.print( "V\t" );
        // int mappedAdc0Reading = map(adc0ReadingUnscaled, 0, 4095, -40, 40);
        // int hueShift = 0;

        // if (mappedAdc0Reading < 0) {
        //   hueShift = map(mappedAdc0Reading, -40, 0, 0, 200);
        //   mappedAdc0Reading = abs(mappedAdc0Reading);
        // }
    }
    if ( showADCreadings[ 1 ] != 0 ) {
        //       if (readFloatingOrState(ADC0_PIN, -1) == floating)//this doesn't
        //       work because it's buffered
        // {

        //  bs += Serial.print("Floating  ");
        // }
        // adc1ReadingUnscaled = readAdc(1, samples);

        // adc1Reading = (adc1ReadingUnscaled) * (railSpread / 4095);
        // adc1Reading -= railSpread / 2; // offset

        adc1Reading = readAdcVoltage( 1, samples );
        bs += Serial.print( "ADC 1: " );
        bs += Serial.print( adc1Reading );
        bs += Serial.print( "V\t" );
        // int mappedAdc1Reading = map(adc1ReadingUnscaled, 0, 4095, -40, 40);
        // int hueShift = 0;

        // if (mappedAdc1Reading < 0) {
        //   hueShift = map(mappedAdc1Reading, -40, 0, 0, 200);
        //   mappedAdc1Reading = abs(mappedAdc1Reading);
        // }
    }

    if ( showADCreadings[ 2 ] != 0 ) {

        adc2ReadingUnscaled = readAdc( 2, samples );
        adc2Reading = ( adc2ReadingUnscaled ) * ( railSpread / 4095 );
        adc2Reading -= railSpread / 2; // offset
        bs += Serial.print( "ADC 2: " );
        bs += Serial.print( adc2Reading );
        bs += Serial.print( "V\t" );
        int mappedAdc2Reading = map( adc2ReadingUnscaled, 0, 4095, -40, 40 );
        int hueShift = 0;

        if ( mappedAdc2Reading < 0 ) {
            hueShift = map( mappedAdc2Reading, -40, 0, 0, 200 );
            mappedAdc2Reading = abs( mappedAdc2Reading );
        }
    }

    if ( showADCreadings[ 3 ] != 0 ) {

        adc3ReadingUnscaled = readAdc( 3, samples );
        adc3Reading = ( adc3ReadingUnscaled ) * ( railSpread / 4095 );
        adc3Reading -= railSpread / 2; // offset
        bs += Serial.print( "ADC 3: " );
        bs += Serial.print( adc3Reading );
        bs += Serial.print( "V\t" );
        int mappedAdc3Reading = map( adc3ReadingUnscaled, 0, 4095, -40, 40 );
        int hueShift = 0;

        if ( mappedAdc3Reading < 0 ) {
            hueShift = map( mappedAdc3Reading, -40, 0, 0, 200 );
            mappedAdc3Reading = abs( mappedAdc3Reading );
        }
    }

    if ( showADCreadings[ 7 ] != 0 ) {

        adc7ReadingUnscaled = readAdc( 7, samples );
        adc7Reading = ( adc7ReadingUnscaled ) * ( railSpread / 4095 );
        adc7Reading -= railSpread / 2; // offset
        bs += Serial.print( "ADC 7: " );
        bs += Serial.print( adc7Reading );
        bs += Serial.print( "V\t" );
        int mappedAdc7Reading = map( adc7ReadingUnscaled, 0, 4095, -40, 40 );
        int hueShift = 0;

        if ( mappedAdc7Reading < 0 ) {
            hueShift = map( mappedAdc7Reading, -40, 0, 0, 200 );
            mappedAdc7Reading = abs( mappedAdc7Reading );
        }
    }

    if ( showADCreadings[ 4 ] != 0 ) {

        adc4ReadingUnscaled = readAdc( 4, samples );
        adc4Reading = ( adc4ReadingUnscaled ) * ( 5.0 / 4095 );
        // adc1Reading -= 0.1; // offset
        bs += Serial.print( "ADC 4: " );
        bs += Serial.print( adc4Reading );
        bs += Serial.print( "V\t" );
    }

    // if (showINA0[0] == 1 || showINA0[1] == 1 || showINA0[2] == 1) {
    //   bs += Serial.print("   INA219: ");
    // }

    if ( showINA0[ 0 ] == 1 ) {
        bs += Serial.print( "INA 0: " );
        bs += Serial.print( INA0.getCurrent_mA( ) );
        bs += Serial.print( "mA\t" );
        // bs += Serial.print("\tINA 1: ");
        // bs += Serial.print(INA1.getCurrent_mA());
        // bs += Serial.print("mA\t");
    }

    if ( showINA0[ 1 ] == 1 ) {
        bs += Serial.print( " V: " );
        bs += Serial.print( INA0.getBusVoltage( ) );
        bs += Serial.print( "V\t" );
    }
    if ( showINA0[ 2 ] == 1 ) {
        bs += Serial.print( "P: " );
        bs += Serial.print( INA0.getPower_mW( ) );
        bs += Serial.print( "mW\t" );
    }
    // Serial.print(digitalRead(buttonPin));
    bs += Serial.print( "      \r" );
    // rotaryEncoderStuff();
    // if (encoderButtonState != IDLE) {
    //   // showReadings = 0;
    //   return;
    //   }
    // while (millis() - startMillis < printInterval &&
    //        (Serial.available() == 0 && probing.checkProbeButton() == 0)) {

    //   showLEDmeasurements();
    //   delayMicroseconds(5000);
    //   }
    startMillis = millis( );
    Serial.flush( );
}

void printPIOStateMachines( ) {
    Serial.println( "=== PIO STATE MACHINE STATUS ===" );

    // Check all PIO instances. RP2350 has three blocks; RP2040 has two.
#if defined(PICO_RP2350)
    PIO pio_instances[] = { pio0, pio1, pio2 };
    const char* pio_names[] = { "PIO0", "PIO1", "PIO2" };
#else
    PIO pio_instances[] = { pio0, pio1 };
    const char* pio_names[] = { "PIO0", "PIO1" };
#endif
    const int numPio = (int)( sizeof( pio_instances ) / sizeof( pio_instances[0] ) );

    for ( int pio_idx = 0; pio_idx < numPio; pio_idx++ ) {
        PIO current_pio = pio_instances[ pio_idx ];
        Serial.printf( "%s:\n\r", pio_names[ pio_idx ] );

        // Check all 4 state machines per PIO
        for ( int sm = 0; sm < 4; sm++ ) {
            bool is_claimed = pio_sm_is_claimed( current_pio, sm );

            if ( is_claimed ) {
                Serial.printf( "  SM%d: CLAIMED\n\r", sm );
            } else {
                Serial.printf( "  SM%d: FREE\n\r", sm );
            }
        }
    }

    // The placement registry: who owns which instruction words on each
    // block, and each block's GPIO base (task #30's instrument).
    {
        extern void pioRegistryPrint( Stream& target );
        pioRegistryPrint( Serial );
    }
}

float __not_in_flash_func(readAdcVoltage)( int channel, int samples ) {

    if ( channel < 0 || channel > 7 ) {
        return 0;
    }
#if USB_AUDIO_ENABLE
    // While USB audio owns the converter, readAdc() deliberately returns a 0
    // SENTINEL for the two probe channels (5 = pad sense, 7 = tip) rather than a
    // rolling mean, because a smeared value fed to the probe's row decoder picks
    // the WRONG row. That sentinel must not be scaled: 0 * spread - adcZero[7]
    // is about -9 V, a confident, completely wrong voltage that fed
    // gpioDroopCurrentEstimate() (~400 mA against a ~1 mA threshold, enough to
    // latch switch position) and printed as the tip voltage in three places.
    // The sweep DOES cover these channels, so serve the cached real value that
    // usbAudioRefreshLazy() maintains. readAdc() itself is untouched - the row
    // decoder still gets its sentinel.
    if ( usbAudioOwnsAdc && ( channel == 5 || channel == 7 ) ) {
        return adcReadings[ channel ];
    }
#endif
    int adcReadingUnscaled = readAdc( channel, samples );

    float adcReading = ( adcReadingUnscaled ) * ( adcSpread[ channel ] / 4095 );
    if ( channel != 4 && channel != 5 ) {
        adcReading -= adcZero[ channel ]; // offset - use calibrated zero value
    }

    return adcReading;
}

int __not_in_flash_func(readAdc)( int channel, int samples ) {
    // T2.1: with the ring engine live, a read is "n fresh sweeps starting
    // now" off the ring - the same meaning as the START_ONCE burst below (a
    // drive-then-read caller gets samples taken after its drive), no lock, no
    // converter access from any core, ~21 us per sample instead of ~9.
    if ( adcRingActive( ) ) {
        if ( channel < 0 || channel > 7 ) return 0;
        if ( samples < 1 ) samples = 1;
        return adcRingMeanAfter( channel, adcRingSweeps( ), samples, (uint32_t)samples * 25u + 400u );
    }
    // Claim the single ADC peripheral for this core. CRITICAL: the acquire must
    // be ATOMIC - a plain "while(flag) flag=true" is a check-then-set race that
    // lets both cores pass simultaneously, then both drive the ADC mux/state
    // machine concurrently. That corrupts it and (with the SDK's adc_read())
    // wedges a core - and if that core was holding core_sync, the other core
    // deadlocks. __atomic_test_and_set serializes the two cores.
    //
    // NEVER take the lock over on timeout: the old 100ms takeover fired against any
    // legitimate long hold (USB audio capture keeps readingADC for as long as
    // the host has the mic open) and produced exactly the two-driver corruption
    // above. A failed acquire now degrades to a 0 reading - the ADC goes
    // briefly blind instead of a core going permanently dead.
#if USB_AUDIO_ENABLE
    // USB audio streaming reconfigures the ADC for free-running round-robin
    // capture into a DMA FIFO, which makes everything below structurally
    // invalid: adc_select_input() writes an AINSEL that round-robin immediately
    // overwrites, START_ONCE means nothing while START_MANY is set, results go
    // to adc_hw->fifo rather than adc_hw->result, and ADC_CS_READY toggles
    // continuously so the wait loop exits at a random phase.
    //
    // Bail out BEFORE the lock, not on its timeout: the audio path holds
    // readingADC for the whole stream, so waiting would burn the full 100 ms on
    // every call - and updateLazyAdcReadings() calls this from core1 on every
    // loop, which would make the board feel hung. The two streamed channels are
    // still served, from the DMA half-buffer mean.
    if ( usbAudioOwnsAdc ) {
        int raw = 0;
        return usbAudioSnapshotRaw( channel, &raw ) ? raw : 0;
    }
#endif

    unsigned long adcWaitStart = micros();
    while ( __atomic_test_and_set( &readingADC, __ATOMIC_ACQUIRE ) ) {
        if (micros() - adcWaitStart > 100000) {
            return 0; // holder is busy/stuck - do NOT touch the ADC or the lock
        }
        tight_loop_contents( );
    }
    int adcReading = readAdcHeld( channel, samples );
    // Release the ADC for other cores
    __atomic_clear( &readingADC, __ATOMIC_RELEASE );
    return adcReading;
}

// Non-waiting acquire of the ADC lock. For callers that must not sit on a
// hardware side effect while the other core finishes a read (the probe
// feed-side blink holds the tip's supply LOW while it samples ADC7 - a
// 100ms wait there would be 100ms of dark tip). Returns false when the ADC
// is busy or USB audio owns it; on true the caller MUST call adcRelease().
bool adcTryAcquire( void ) {
    if ( adcRingActive( ) ) return true;   // T2.1: nothing to hold - the ring is everyone's
#if USB_AUDIO_ENABLE
    if ( usbAudioOwnsAdc ) return false;
#endif
    return !__atomic_test_and_set( &readingADC, __ATOMIC_ACQUIRE );
}

void adcRelease( void ) {
    if ( adcRingActive( ) ) return;
    __atomic_clear( &readingADC, __ATOMIC_RELEASE );
}

// The conversion loop with the lock ALREADY HELD by the caller (readAdc()
// wraps it; adcTryAcquire()/adcRelease() callers use it directly).
int __not_in_flash_func(readAdcHeld)( int channel, int samples ) {
    if ( adcRingActive( ) ) {              // T2.1: same fresh-burst meaning, off the ring
        if ( channel < 0 || channel > 7 ) return 0;
        if ( samples < 1 ) samples = 1;
        return adcRingMeanAfter( channel, adcRingSweeps( ), samples, (uint32_t)samples * 25u + 400u );
    }
    unsigned long adcReadingAverage = 0;
    // if (channel == 0) { // I have no fucking idea why this works //future me:
    // the op amps were untamed

    //   pinMode(ADC1_PIN, OUTPUT);
    //   digitalWrite(ADC1_PIN, LOW);
    // }

    if ( channel > 8 ) {
        return 0;
    }
    unsigned long timeoutTimer = micros( );

    int actualSamples = 0;
    adc_select_input( channel );
    for ( int i = 0; i < samples; i++ ) {
        if ( micros( ) - timeoutTimer > 5000 ) {
            break;
        }

        // Manually triggered conversion with a per-conversion deadline (same
        // pattern as NetVoltageScan's readScanAdcVoltage). The SDK's adc_read()
        // busy-waits on READY with no timeout, so a corrupted state machine
        // hangs the calling core forever - and this function runs on core 1
        // inside the core_sync hold, where that hang deadlocks the whole board.
        // A conversion is ~2us; 100us of no READY means the ADC is sick and the
        // sample is abandoned, not waited for.
        hw_set_bits( &adc_hw->cs, ADC_CS_START_ONCE_BITS );
        unsigned long convStart = micros( );
        while ( !( adc_hw->cs & ADC_CS_READY_BITS ) ) {
            if ( micros( ) - convStart > 100 ) {
                break;
            }
        }
        if ( !( adc_hw->cs & ADC_CS_READY_BITS ) ) {
            continue;
        }
        adcReadingAverage += ( adc_hw->result & 0xFFF );
        actualSamples++;
        delayMicroseconds( 6 );
    }

    int adcReading =
        ( actualSamples > 0 ) ? ( adcReadingAverage / actualSamples ) : 0;

    // float adc3Voltage = (adc3Reading - 2528) / 220.0; // painstakingly measured

    // if (channel == 0) {
    //   pinMode(ADC1_PIN, INPUT);
    // }
    // Serial.println(adcReading);
    // Serial.print("adcReading:               ");
    // Serial.println(adcReading, DEC);
    // Serial.flush();
    // Serial.print("adcReading & 0xFFF0 >> 4: ");
    // Serial.println((adcReading & 0xFFF0) >> 4, DEC);
    // Serial.flush();
    
    return adcReading;
}


// ============================================================================
// VoltageAdjuster Class Implementation
// ============================================================================

float VoltageAdjuster::snapValues[3] = {0.0, 3.3, 5.0};

uint32_t VoltageAdjuster::determineColor(float value, const VoltageAdjustConfig& config) {
    // Start with base color
    uint32_t numberColor = config.posColor;
    
    if (value > 0.05) {
        numberColor = config.posColor;
    }
    
    // Blended regions (approaching special values)
    if (value < 5.3 && value > 4.7) {
        numberColor = config.fiveBlended;
    } else if (value < 3.45 && value > 3.05) {
        numberColor = config.threeBlended;
    } else if (value < 0.35 && value > -0.35) {
        numberColor = config.zeroBlended;
    }
    
    // Exact special values (tight ranges for snap points)
    if (value > -0.05 && value < 0.05) {
        numberColor = config.zeroColor;
    } else if (value > 3.25 && value < 3.35) {
        numberColor = config.threeColor;
    } else if (value > 4.95 && value < 5.05) {
        numberColor = config.fiveColor;
    } else if (value > 7.95 && value < 8.55) {
        numberColor = config.maxColor;
    } else if (value < -0.05) {
        numberColor = config.negColor;
    }
    
    return numberColor;
}

void VoltageAdjuster::updateDisplay(float value, uint32_t color, const VoltageAdjustConfig& config) {
    // Format the string
    char floatString[16];
    if (value < 0.00) {
        snprintf(floatString, sizeof(floatString), "%0.1f V", value);
    } else {
        snprintf(floatString, sizeof(floatString), " %0.1f V", value);
    }
    
    // Update LED display
    b.clear();
    
    // Show label on top row if provided
    if (config.label != nullptr) {
        if (strcmp(config.label, "Top Rail") == 0) {
            b.print("Top", color, 0xffffff, 0, 0, -2);
            b.print("Rail", color, 0xffffff, 3, 0, 1);
        } else if (strcmp(config.label, "Bot Rail") == 0) {
            b.print("Bot", color, 0xffffff, 0, 0, -2);
            b.print("Rail", color, 0xffffff, 3, 0, 1);
        } else if (strcmp(config.label, "Rails") == 0) {
            b.print("Rails", color, 0xffffff, 1, 0, 2);
        } else if (strcmp(config.label, "DAC 1") == 0) {
            b.print("DAC 1", color, 0xffffff, 1, 0, 0);
        } else if (strcmp(config.label, "DAC 0") == 0) {
            b.print("DAC 0", color, 0xffffff, 1, 0, 0);
        } else {
            b.print(config.label, color, 0xffffff, 1, 0, 2);
        }
    }
    
    // Show voltage value in middle
    b.print(floatString, color, 0xffffff, 0, 1, 1);
    
    // Calculate position indicator: show where current value is in range
    // Position indicator moves horizontally across rows 0-29 (30 positions)
    float valueRange = config.maxVoltage - config.minVoltage;
    float normalizedPosition = (value - config.minVoltage) / valueRange;
    // Clamp to 0-1
    if (normalizedPosition < 0.0) normalizedPosition = 0.0;
    if (normalizedPosition > 1.0) normalizedPosition = 1.0;
    // Map to row positions 0-29 (30 positions for finer resolution)
    int rowPosition = (int)(normalizedPosition * 29.0 + 0.5);
    if (rowPosition < 0) rowPosition = 0;
    if (rowPosition > 29) rowPosition = 29;
    
    // Show position indicator as a single LED at the bottom of the calculated row
    uint8_t indicator = 0b00000001; // Bottom-most LED in the row
    b.printRawRow(indicator, rowPosition, 0x101010, 0xfffffe);
    
    // OLED + serial go through the shared reading display, so adjusting a rail
    // looks like every other reading (and repeat calls dedupe - this runs on
    // every loop pass while the probe rides the selection pads).
    // floatString keeps its leading pad for the LED matrix; the panel doesn't
    // want it.
    char valueText[16];
    snprintf(valueText, sizeof(valueText), "%0.1f V", value);
    ReadingDisplay::show(config.label, -1, valueText);

    requestLedShow( 2 );
}

bool VoltageAdjuster::isInLiveRange(float value, const VoltageAdjustConfig& config) {
    return config.liveUpdateInRange && 
           value >= config.liveUpdateMin && 
           value <= config.liveUpdateMax;
}

AdjustResult VoltageAdjuster::adjust(VoltageAdjustConfig& config) {
    // CRITICAL: Reset button state so we wait for a NEW button press to confirm/cancel
    // Otherwise the button press that launched this UI will immediately confirm
    Menus::getInstance().inClickMenu = 1;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    
    // Store original value for cancellation
    float originalValue = config.initialValue;
    float currentValue = config.initialValue;
    
    // Snap detection
    int snapToValue = 0;
    
    // Position-based tracking for direct encoder reading
    long lastEncoderPosition = encoderPosition;
    
    // Acceleration tracking with direction
    unsigned long lastChangeTime = millis();
    float accelerationMultiplier = 1.0;
    int lastDirection = 0;  // -1=down, 0=none, 1=up
    int consecutiveFastCount = 0;  // Track consecutive fast movements
    
    // Set rotary divider for good responsiveness
    int lastDivider = rotaryDivider;
    rotaryDivider = 3;
    
    // Clear display and show initial value
    b.clear(1);
    uint32_t color = determineColor(currentValue, config);
    updateDisplay(currentValue, color, config);
    
    // CRITICAL: Use blocking LED update for atomic display (flag + 10 = blocking)
    // This prevents flickering from partial updates (clear + text)
    requestLedShow( 12 );  // 12 = blocking mode (menu display)
    
    // If we have a callback and we're in live range, call it immediately
    if (config.callback && isInLiveRange(currentValue, config)) {
        config.callback(currentValue, true, config.context);
    }
    
    bool firstUpdate = true;
    int accelCount = 0;
    // Main adjustment loop
    while (true) {
        //delayMicroseconds(380);
        rotaryEncoderStuff();
        jOS.serviceInner();
        // Read probe pads for direct voltage selection
        int probeReading = probing.justReadProbe(true);
        
        // Check if probe is touching the voltage selection area (rows 31-60)
        if (probeReading >= 31 && probeReading <= 60) {
            // Map probe position to voltage value based on config range
            // Probe rows 31-60 (30 positions) map to minVoltage-maxVoltage
            float voltageRange = config.maxVoltage - config.minVoltage;
            float normalizedPosition = (probeReading - 31) / 29.0;  // 0.0 to 1.0
            currentValue = config.minVoltage + (normalizedPosition * voltageRange);
            
            // Round to 0.1V increments
            currentValue = roundf(currentValue * 10.0f) / 10.0f;
            
            // Clamp to limits
            if (currentValue > config.maxVoltage) currentValue = config.maxVoltage;
            if (currentValue < config.minVoltage) currentValue = config.minVoltage;
            
            // Update display and LEDs
            color = determineColor(currentValue, config);
            updateDisplay(currentValue, color, config);
            
            // CRITICAL: Use blocking LED update for atomic display (flag + 10 = blocking)
            // Prevents flickering from showing partial text/graphics
            requestLedShow( 12 );  // 12 = blocking mode (menu display)
            
            // Call callback for preview
            if (config.callback) {
                bool isLive = isInLiveRange(currentValue, config);
                config.callback(currentValue, isLive, config.context);
            }
            
            // Reset encoder-based tracking since we're using probe now
            lastEncoderPosition = encoderPosition;
            accelerationMultiplier = 1.0;
            consecutiveFastCount = 0;
            lastDirection = 0;
            firstUpdate = true;
            continue;
        }
        
        // Check for cancellation (long press)
        if (encoderButtonState == HELD || probeButton.getButtonState() == 1) {
            // Restore original value if we have a callback
            // if (config.callback) {
            //     config.callback(originalValue, true, config.context);
            // }
            
            rotaryDivider = lastDivider;
            encoderButtonState = IDLE;
            requestLedShow( -1 );
            b.clear();
            Menus::getInstance().inClickMenu = 0;
            return AdjustResult::CANCELLED;
        }
        
        // Check for confirmation (short press)
        if ((encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) || probeButton.getButtonState() == 2) {
            encoderButtonState = IDLE;
            //requestLedShow( -1 );
            // Final update with callback if not already in live range
            // if (config.callback && !isInLiveRange(currentValue, config)) {
            //     config.callback(currentValue, false, config.context);
            // }
            
            rotaryDivider = lastDivider;
            config.initialValue = currentValue; // Update config with new value
            Menus::getInstance().inClickMenu = 0;
            b.clear();
            requestLedShow( -1 );
            
            return AdjustResult::CONFIRMED;
        }
        
        // Handle serial input for cancellation
        if (Serial.available() > 0 ) {
            Serial.read(); // Consume character
            if (config.callback) {
                config.callback(originalValue, true, config.context);
            }
            rotaryDivider = lastDivider;
            Menus::getInstance().inClickMenu = 0;
            

            b.clear();
            requestLedShow( -1 );
            return AdjustResult::CANCELLED;
        }
        
        // Read encoder position directly for immediate response
        long currentEncoderPosition = encoderPosition;
        long encoderDelta = currentEncoderPosition - lastEncoderPosition;
        
        bool valueChanged = false;
        
        // Process encoder movement
        if (encoderDelta != 0 || firstUpdate) {
            // Handle snap delay
            if (snapToValue > 0 && !firstUpdate) {
                snapToValue--;
                // Update last position even when snapping so we don't accumulate
                lastEncoderPosition = currentEncoderPosition;
                continue;
            }
            
            if (!firstUpdate) {
                // Determine current direction
                int currentDirection = (encoderDelta > 0) ? 1 : ((encoderDelta < 0) ? -1 : 0);
                
                // Reset acceleration if direction changed
                if (currentDirection != 0 && currentDirection != lastDirection) {
                    accelerationMultiplier = 1.0;
                    consecutiveFastCount = 0;
                    lastDirection = currentDirection;
                }
                
                // Calculate acceleration based on delta magnitude and timing
                unsigned long currentTime = millis();
                unsigned long timeSinceLastChange = currentTime - lastChangeTime;
                int deltaMagnitude = abs(encoderDelta);
                
                // Fast rotation = large delta between polls OR short time between changes
                bool isFastRotation = (deltaMagnitude >= 4);
                
                if (isFastRotation) {
                    // Fast movement detected - increment consecutive count
                    consecutiveFastCount++;
                    
                    // Only accelerate after 3 consecutive fast movements in same direction
                    if (consecutiveFastCount >= 0) {
                        accelerationMultiplier += 3.5;
                        if (accelerationMultiplier > 5.0) {
                            accelerationMultiplier = 5.0;
                        }
                    }
                } else if (timeSinceLastChange > 120) {
                    // Slow/stopped - reset everything
                    accelerationMultiplier = 1.0;
                    consecutiveFastCount = 0;
                    lastDirection = 0;
                } else {
                    // Medium/slow speed - reset fast count but maintain acceleration
                    consecutiveFastCount = 0;
                }
                
                lastChangeTime = currentTime;
                
                // Calculate voltage change based on encoder delta
                // Base change per encoder click, scaled by acceleration
                float deltaMultiplier = 0.01 * accelerationMultiplier;
                float addToValue = encoderDelta * deltaMultiplier;
                
                // Apply the change
                currentValue -= addToValue;
  
                // Debug output
                // Serial.print("val=");
                // Serial.print(currentValue, 1);
                // Serial.print(" delta=");
                // Serial.print(encoderDelta);
                // Serial.print(" mult=");
                // Serial.print(accelerationMultiplier, 1);
                // Serial.print(" time=");
                // Serial.print(timeSinceLastChange);
                // Serial.print(" dir=");
                // Serial.println(currentDirection);
                // Serial.flush();
            }
            
            lastEncoderPosition = currentEncoderPosition;
            valueChanged = true;
            firstUpdate = false;
        }
        
        if (valueChanged) {
            // Clamp to limits
            if (currentValue > config.maxVoltage) {
                currentValue = config.maxVoltage;
            }
            if (currentValue < config.minVoltage) {
                currentValue = config.minVoltage;
            }
            
            // // Snap to zero (in full precision)
            // if (currentValue > -0.05 && currentValue < 0.05) {
            //     currentValue = 0.0;
            // }
            
            // Check for snap values (using full precision)
            if (snapToValue == 0 && accelerationMultiplier == 1.0) {
                for (int i = 0; i < 3; i++) {
                    if (abs(currentValue) > snapValues[i] - 0.03 && 
                        abs(currentValue) < snapValues[i] + 0.03) {
                        snapToValue = 1;
                        break;
                    }
                }
            }
            
            // Round for display and hardware setting
            float roundedValue = roundf(currentValue * 10.0f) / 10.0f;
            if (roundedValue == -0.0) {
                roundedValue = 0.0;
            }
            
            // Update display with rounded value
            color = determineColor(roundedValue, config);
            updateDisplay(roundedValue, color, config);
            
            // CRITICAL: Use blocking LED update for atomic display (flag + 10 = blocking)
            // Prevents flickering from showing partial voltage bar/text
            requestLedShow( 12 );  // 12 = blocking mode (menu display)
            
            // ALWAYS call callback for preview (both live and non-live)
            // Pass ROUNDED value to hardware/state
            // This ensures LEDs show the value even outside 0-5V range
            if (config.callback) {
                bool isLive = isInLiveRange(roundedValue, config);
                config.callback(roundedValue, isLive, config.context);
            }
        }
    }
    
    // Should never reach here
    rotaryDivider = lastDivider;
    Menus::getInstance().inClickMenu = 0;
    return AdjustResult::ERROR;
}
