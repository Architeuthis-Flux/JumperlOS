// SPDX-License-Identifier: MIT
//
// Unattended factory hardware self test. See SelfTest.h for the overview.
//
// Design notes:
// - An MCU restart does NOT reset external hardware: the MCP4728 DAC holds
//   its outputs, the CH446Q crossbars keep crosspoints closed (no readback),
//   and the INA219s keep their ADC config. selfTestNormalizeHardware() runs
//   at both ends of every test session to force known silicon state without
//   trusting the RAM model.
// - Results paint into the "_SELFTEST_" graphic overlay, which renders on
//   top of every LED refresh and lives in RAM only, so it clears on reset.
//   serializeOverlaysToYAML() filters it out so it can never persist into a
//   slot file.
// - First start ends in rp2040.restart(), so results are also written to
//   /selftest.json plus a one-shot marker; boot repaints the overlay from
//   the marker and deletes it (overlay shows until the NEXT reset).

#include "SelfTest.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>

#include "CH446Q.h"
#include "Commands.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"
#include "Graphics.h"
#include "GraphicOverlays.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "InfraPaths.h"
#include "Probing.h"
#include "PsramArena.h"
#include "RotaryEncoder.h"
#include "States.h"
#include "config.h"
#include "configManager.h"
#include "oled.h"

#include "hardware/gpio.h"
#include "pico/unique_id.h"

// Defined in Apps.cpp / Probing.cpp (not exported in headers)
void leaveApp( void );
// Core0-safe SM pause/resume: parks core1's probeLEDhandler before touching
// the shared button/LED PIO state machine. NEVER call the raw
// probeButtonPausePolling/ResumePolling from this file - those are core1's
// (see the banner above their definitions in Probing.cpp; the raw calls
// hardware-wedged core1 in showBlocking during the crossbar phase).
extern void probeButtonPausePollingFromCore0( void );
extern void probeButtonResumePollingFromCore0( void );

static const char* selfTestNames[ SELFTEST_NUM_TESTS ] = {
    "probe_cable", "crossbar", "tip_voltage", "psram", "peripherals",
};

static const char* SELFTEST_JSON_PATH = "/selftest.json";
static const char* SELFTEST_MARKER_PATH = "/selftest_show.txt";
static const char* SELFTEST_OVERLAY_NAME = "_SELFTEST_";

// Pause between tests so each one is watchable on the LEDs/OLED
#define SELFTEST_PAUSE_MS 1500

#if defined(OG_JUMPERLESS)

// The OG has no crossbar-routable buffer, single-LED rows, and a different
// analog front end - none of this test suite applies.
void runFullSelfTest( bool ) { Serial.println( "Self test is not supported on Jumperless OG." ); }
void selfTestWaitForInput( const char* ) { }
void selfTestClearOverlay( void ) { }
void selfTestWaitForInputThenReset( void ) { rp2040.restart( ); }
void probeCableTestApp( void ) { runFullSelfTest( false ); }
void crossbarTestApp( void ) { runFullSelfTest( false ); }
void tipVoltageTestApp( void ) { runFullSelfTest( false ); }
void psramTestApp( void ) { runFullSelfTest( false ); }
void fullSelfTestApp( void ) { runFullSelfTest( false ); }
void selfTestPrintStoredReport( void ) { Serial.println( "::SELFTEST::none::END::" ); }
void selfTestShowSavedResultIfPending( void ) { }

#else // V5 implementation

// ============================================================================
// Framework
// ============================================================================

static void initReport( SelfTestReport& r ) {
    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        r.status[ i ] = SELFTEST_NOTRUN;
        r.detail[ i ][ 0 ] = '\0';
    }
}

static bool reportOverallPass( const SelfTestReport& r ) {
    bool anyRun = false;
    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        if ( r.status[ i ] == SELFTEST_FAIL )
            return false;
        if ( r.status[ i ] == SELFTEST_PASS )
            anyRun = true;
    }
    return anyRun;
}

// Force external hardware to a known state WITHOUT trusting the RAM model.
// Safe to call even after a restart that left the crossbars/DACs configured.
static void selfTestNormalizeHardware( void ) {
    Serial.println( "  normalizing hardware to a known state:" );
    // Blind clear-all: the CH446Qs have no readback, so sweep every
    // crosspoint on all 12 chips. ~1500 raw writes, tens of ms.
    Serial.println( "    blind-clearing all 1536 crosspoints on 12 CH446Q chips..." );
    for ( int chip = 0; chip < 12; chip++ ) {
        for ( int x = 0; x < 16; x++ ) {
            for ( int y = 0; y < 8; y++ ) {
                sendXYraw( chip, x, y, 0 );
            }
        }
    }
    globalState.clearAllConnections( );
    refreshConnections( -1, 0, 1 );
    waitCore2( );

    Serial.println( "    zeroing MCP4728: DAC0, DAC1, top rail, bottom rail -> 0.000V" );
    for ( int d = 0; d < 4; d++ ) {
        setDacByNumber( d, 0.0, 0 );
    }

    Serial.println( "    releasing routable GPIOs 1-8 (pins 20-27) to inputs" );
    for ( int g = 0; g < 8; g++ ) {
        pinMode( gpioDef[ g ][ 0 ], INPUT );
    }

    Serial.println( "    restoring INA219 ADC config (0x0b)" );
    INA0.setBusADC( 0x0b );
    INA1.setBusADC( 0x0b );
    delay( 10 );
}

static const char statusChars[ 4 ] = { 'N', 'P', 'F', 'S' }; // NOTRUN/PASS/FAIL/SKIP

// Short names that fit the 128x32 OLED next to a PASS/FAIL word
static const char* oledNames[ SELFTEST_NUM_TESTS ] = {
    "Probe", "Xbar", "Tip V", "PSRAM", "Chips",
};

static void oledStatus( const char* line ) {
    if ( oled.isConnected( ) ) {
        oled.clearPrintShow( line, 2, true, true, true );
    }
}

// Live LED progress: fill one breadboard row (1-60) with a status color.
static void paintRowColor( int row, uint32_t color ) {
    b.printRawRow( 0b00011111, row - 1, color, 0xfffffe );
    showLEDsCore2 = 2;
}

static void paintRowStatus( int row, bool ok ) {
    paintRowColor( row, ok ? 0x001200 : 0x160000 );
}

static const char* statusWord( SelfTestStatus s ) {
    switch ( s ) {
    case SELFTEST_PASS: return "pass";
    case SELFTEST_FAIL: return "fail";
    case SELFTEST_SKIP: return "skip";
    default: return "notrun";
    }
}

static const char* statusWordUpper( SelfTestStatus s ) {
    switch ( s ) {
    case SELFTEST_PASS: return "PASS";
    case SELFTEST_FAIL: return "FAIL";
    case SELFTEST_SKIP: return "SKIP";
    default: return "--";
    }
}

static uint32_t statusColor( SelfTestStatus s ) {
    switch ( s ) {
    case SELFTEST_PASS: return 0x001400; // green
    case SELFTEST_FAIL: return 0x160000; // red
    case SELFTEST_SKIP: return 0x000314; // dim blue
    default: return 0x000000;            // transparent
    }
}

// Paint the result overlay: overall status on columns 1-3, then one 4-column
// band per test (left to right in test order) with a blank column between.
static void paintSelfTestOverlay( const SelfTestReport& r ) {
    static uint32_t colors[ MAX_OVERLAY_PIXELS ]; // 300 * 4B; static to spare stack
    memset( colors, 0, sizeof( colors ) );

    bool anyRun = false;
    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        if ( r.status[ i ] != SELFTEST_NOTRUN )
            anyRun = true;
    }
    uint32_t overall = anyRun ? statusColor( reportOverallPass( r ) ? SELFTEST_PASS : SELFTEST_FAIL ) : 0;

    for ( int row = 0; row < 10; row++ ) {
        for ( int col = 0; col < 3; col++ ) {
            colors[ row * 30 + col ] = overall;
        }
        for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
            uint32_t c = statusColor( r.status[ i ] );
            if ( c == 0 )
                continue;
            int startCol = 4 + i * 5;
            for ( int col = startCol; col < startCol + 4; col++ ) {
                colors[ row * 30 + col ] = c;
            }
        }
    }

    graphicOverlayState.addOverlay( SELFTEST_OVERLAY_NAME, 1, 1, 30, 10, colors );
    showLEDsCore2 = -2;
}

static String selfTestToJson( const SelfTestReport& r ) {
    char id[ 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1 ];
    pico_get_unique_board_id_string( id, sizeof( id ) );

    String j;
    j.reserve( 640 );
    j += "{\"id\":\"";
    j += id;
    j += "\",\"fw\":\"";
    j += firmwareVersion;
    j += "\",\"pass\":";
    j += reportOverallPass( r ) ? "true" : "false";
    j += ",\"results\":{";
    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        if ( i )
            j += ",";
        j += "\"";
        j += selfTestNames[ i ];
        j += "\":\"";
        j += statusWord( r.status[ i ] );
        j += "\"";
    }
    j += "},\"details\":{";
    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        if ( i )
            j += ",";
        j += "\"";
        j += selfTestNames[ i ];
        j += "\":\"";
        j += r.detail[ i ]; // snprintf'd below: never contains quotes/backslashes
        j += "\"";
    }
    j += "}}";
    return j;
}

// Sentinel framing keeps host-side parsing trivial even when the report is
// interleaved with other serial output.
static void printSentinelReport( const String& json ) {
    Serial.print( "::SELFTEST::" );
    Serial.print( json );
    Serial.println( "::END::" );
    Serial.flush( );
}

static void printHumanReport( const SelfTestReport& r ) {
    Serial.println( "\n\r---- Hardware Self Test Results ----" );
    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        Serial.printf( "  %-12s %-6s %s\n\r", selfTestNames[ i ],
                       statusWord( r.status[ i ] ), r.detail[ i ] );
    }
    if ( reportOverallPass( r ) ) {
        changeTerminalColor( 84, true ); // green
        Serial.println( "\n\r  OVERALL: PASS\n\r" );
    } else {
        changeTerminalColor( 196, true ); // red
        Serial.println( "\n\r  OVERALL: FAIL\n\r" );
    }
    changeTerminalColor( -1, true );
}

static void writeTextFile( const char* path, const String& contents ) {
    File f = safeFileOpen( path, "w" );
    if ( !f )
        return;
    safeFileWrite( f, (const uint8_t*)contents.c_str( ), contents.length( ) );
    safeFileClose( f, true );
}

// ============================================================================
// Test 0: probe cable
// ============================================================================
// A healthy TRRS cable shorts PROBE_LED_PIN (GPIO 2) to BUTTON_PIN (GPIO 9)
// at the jack - the most common cable failure is that short being open,
// almost always on the GPIO2-side contact. Verify it by driving each pin
// and reading the other, with the sense pin pulled toward the OPPOSITE
// level so an open cable reads back the pull, not the drive.
//
// The firmware's WS2812 data + button sampling live on ONE pin
// (probeLEDs.getPin(), GPIO9 by default via probe_led_on_button_pin), so a
// flaky contact on the OTHER (spare) pin no longer breaks anything - when
// the functional checks below prove the data pin reaches the probe, an
// open short is reported as a warning instead of a failure.

#define SHORT_SAMPLES 8

static int countShortAgreement( uint drivePin, uint sensePin, const char* driveName,
                                const char* senseName ) {
    int good = 0;
    for ( int lv = 0; lv <= 1; lv++ ) {
        gpio_set_function( sensePin, GPIO_FUNC_SIO );
        gpio_set_dir( sensePin, false );
        gpio_set_pulls( sensePin, lv == 0, lv == 1 ); // pull opposite the driven level
        gpio_set_input_enabled( sensePin, true );

        gpio_set_function( drivePin, GPIO_FUNC_SIO );
        gpio_disable_pulls( drivePin );
        gpio_set_dir( drivePin, true );

        // Sample repeatedly with a wiggle to the opposite level in between,
        // so every read is a fresh edge instead of a re-read of a settled
        // line - an intermittent contact that holds still for one sample
        // shows up here as a dropped edge.
        int agree = 0;
        for ( int s = 0; s < SHORT_SAMPLES; s++ ) {
            gpio_put( drivePin, lv );
            delayMicroseconds( 50 );
            if ( gpio_get( sensePin ) == lv )
                agree++;
            gpio_put( drivePin, !lv );
            delayMicroseconds( 20 );
        }
        bool ok = ( agree == SHORT_SAMPLES );
        Serial.printf( "    drive %s %s with %s pulled %s -> %s follows drive %d/%d %s\n\r",
                       driveName, lv ? "HIGH" : "LOW", senseName,
                       lv ? "down" : "up", senseName, agree, SHORT_SAMPLES,
                       ok ? "(shorted, ok)" : "(NO SHORT / intermittent)" );
        if ( ok )
            good++;

        gpio_set_dir( drivePin, false );
        gpio_disable_pulls( sensePin );
    }
    return good; // 2 = solidly shorted at both levels, every sample
}

// No-button float check (the "0 1" decode from the button reader): drive the
// shared line to a rail, release to HiZ, and sample - a floating line stays
// where parasitic capacitance left it, while a pressed button or a cable
// short to a rail snaps it back. Runs on the ACTIVE data pin (the one the
// firmware actually samples the button on) and repeats each direction so an
// intermittent rail short can't hide behind a single lucky read.
static bool probeLineFloatCheck( uint pin ) {
    gpio_set_function( pin, GPIO_FUNC_SIO );
    gpio_disable_pulls( pin );
    gpio_set_input_enabled( pin, true );

    int lowGood = 0, highGood = 0;
    for ( int s = 0; s < SHORT_SAMPLES; s++ ) {
        gpio_set_dir( pin, true );
        gpio_put( pin, false );
        delayMicroseconds( 50 );
        gpio_set_dir( pin, false );
        delayMicroseconds( 5 );
        if ( gpio_get( pin ) == 0 )
            lowGood++;

        gpio_set_dir( pin, true );
        gpio_put( pin, true );
        delayMicroseconds( 50 );
        gpio_set_dir( pin, false );
        delayMicroseconds( 5 );
        if ( gpio_get( pin ) == 1 )
            highGood++;
    }
    Serial.printf( "    drive line LOW, release to HiZ -> holds %d/%d %s\n\r", lowGood,
                   SHORT_SAMPLES, lowGood == SHORT_SAMPLES ? "(held by capacitance, ok)"
                                                           : "(SNAPPED HIGH: button/rail short)" );
    Serial.printf( "    drive line HIGH, release to HiZ -> holds %d/%d %s\n\r", highGood,
                   SHORT_SAMPLES, highGood == SHORT_SAMPLES ? "(held by capacitance, ok)"
                                                            : "(SNAPPED LOW: button/rail short)" );

    return lowGood == SHORT_SAMPLES && highGood == SHORT_SAMPLES;
}

static void runProbeCableTest( SelfTestReport& r ) {
    Serial.println( "\n\r[selftest] probe cable..." );
    b.clear( );
    b.print( "Probe", (uint32_t)0x000a12 );
    showLEDsCore2 = 2;

    // The firmware's WS2812-data/button pin follows probeLEDs; the other pin
    // of the shared TRRS ring is the parked spare (GPIO2 when
    // probe_led_on_button_pin is set, the default).
    const uint dataPin = (uint)probeLEDs.getPin( );
    const uint sparePin = ( dataPin == (uint)BUTTON_PIN ) ? (uint)PROBE_LED_PIN
                                                          : (uint)BUTTON_PIN;
    char dataName[ 8 ], spareName[ 8 ];
    snprintf( dataName, sizeof( dataName ), "GPIO%u", dataPin );
    snprintf( spareName, sizeof( spareName ), "GPIO%u", sparePin );

    // The PIO button poller owns the shared line; park it while we drive.
    probeButtonPausePollingFromCore0( );
    gpio_function_t savedLedFunc = gpio_get_function( dataPin );

    // Release the 3.3V feed (PROBE_PIN) so the switch network can't bias the line
    gpio_set_function( PROBE_PIN, GPIO_FUNC_SIO );
    gpio_disable_pulls( PROBE_PIN );
    gpio_set_dir( PROBE_PIN, false );

    Serial.printf( "  step 1/5: cable short, driving from %s (data/button pin):\n\r", dataName );
    int fwd = countShortAgreement( dataPin, sparePin, dataName, spareName );
    Serial.printf( "  %s -> %s short: %d/2 %s\n\r", dataName, spareName, fwd,
                   fwd == 2 ? "ok" : "OPEN" );
    paintRowStatus( 1, fwd == 2 );

    Serial.printf( "  step 2/5: cable short, driving from %s (spare pin):\n\r", spareName );
    int rev = countShortAgreement( sparePin, dataPin, spareName, dataName );
    Serial.printf( "  %s -> %s short: %d/2 %s\n\r", spareName, dataName, rev,
                   rev == 2 ? "ok" : "OPEN" );
    paintRowStatus( 2, rev == 2 );

    Serial.printf( "  step 3/5: no-button float check on %s (line must not be tied to a rail):\n\r",
                   dataName );
    gpio_set_dir( sparePin, false );
    bool floats = probeLineFloatCheck( dataPin );
    Serial.printf( "  line float (no button): %s\n\r", floats ? "ok" : "STUCK" );
    paintRowStatus( 3, floats );

    // Restore: spare pin parked as input-pulldown (its boot state), data pin
    // back to its PIO function with the pad input the button program needs,
    // and PROBE_PIN re-driven HIGH - the old version left the 3.3V feed HiZ
    // after the test, which degraded PIO button decode until the next
    // CPU-path read happened to re-drive it.
    gpio_set_pulls( sparePin, false, true );
    gpio_set_input_enabled( sparePin, true );
    gpio_set_function( dataPin, savedLedFunc );
    gpio_set_input_enabled( dataPin, true );
    gpio_set_dir( PROBE_PIN, true );
    gpio_put( PROBE_PIN, true );
    probeButtonResumePollingFromCore0( );

    // Power the probe through the routable buffer and measure the cable's
    // current signature with the LED latched in KNOWN states. (The old
    // version took ONE reading with the LED in whatever state the last
    // animation left it - judged against thresholds calibrated with a
    // latched LED. That mismatch was the main source of flaky verdicts.)
    extern void setProbeLEDModeAndSettle( int mode ); // Apps.cpp
    Serial.println( "  step 4/5: probe power path (routable buffer -> cable -> LED):" );
    // This step measures the DAC -> INA1 -> buffer -> cable -> LED current
    // signature, so pin the probe-power function to its DAC1 candidate
    // (INA1 sits on the DAC path and would see nothing on a GPIO feed - no
    // LED delta, no end-to-end proof). Unforced at the end of the test.
    infraForceCandidate( "probe_power", 0 ); // candidate 0 = DAC1
    routableBufferPower( 1, 0, 1 );
    delay( 150 );

    // Baseline: LED off, median of 9 independent conversions. The button
    // poller's drive edges ride the same net INA1 measures behind, so park
    // it across each sampling window.
    setProbeLEDModeAndSettle( 10 ); // off
    probeButtonPausePollingFromCore0( );
    float curOff = probeCurrentMedian( 9 );
    probeButtonResumePollingFromCore0( );

    // LED full white: in SELECT position the WS2812's supply current flows
    // from DAC0 through INA1, so a healthy DATA path shows up as a current
    // step - an end-to-end proof that pixels reach the probe through the
    // cable. In MEASURE position the LED is fed from PROBE_PIN instead and
    // the step is invisible to INA1, so the delta only gates the verdict
    // when the baseline says SELECT.
    setProbeLEDModeAndSettle( 8 ); // full white
    probeButtonPausePollingFromCore0( );
    float curOn = probeCurrentMedian( 9 );
    probeButtonResumePollingFromCore0( );
    float ledDelta = curOn - curOff;

    // Fresh classification from the medians - NOT checkSwitchPosition(),
    // whose internal 500ms gate usually hands back a stale cached position.
    bool selectPos = ( curOff > jumperlessConfig.calibration.probe_switch_threshold_high ) ||
                     ( ledDelta >= 0.8f );
    bool dataPathProven = ( ledDelta >= 0.8f );

    Serial.printf( "    LED off: %.3f mA, LED white: %.3f mA, delta: %.3f mA (zero offset %.3f)\n\r",
                   (double)curOff, (double)curOn, (double)ledDelta,
                   (double)jumperlessConfig.calibration.probe_current_zero );
    Serial.printf( "    inferred switch position: %s%s\n\r",
                   selectPos ? "select" : "measure",
                   dataPathProven ? " (LED data path proven end-to-end)"
                   : ( selectPos ? " (NO LED current step: data conductor broken?)"
                                 : " (delta not measurable in measure position)" ) );

    // Hand the LED back to the runtime color for the inferred position.
    showProbeLEDs = selectPos ? 4 : 3;

    Serial.println( "  step 5/5: probe tip sense ADC (channel 5, pin 45):" );
    extern float medianInPlace( float* vals, int n ); // Apps.cpp
    float tipSamples[ 9 ];
    for ( int i = 0; i < 9; i++ ) {
        tipSamples[ i ] = (float)readAdc( 5, 8 );
        delay( 3 );
    }
    int tipRaw = (int)medianInPlace( tipSamples, 9 );
    Serial.printf( "    idle raw reading (median of 9x8): %d/4095 (probe_min:%d probe_max:%d)\n\r",
                   tipRaw, jumperlessConfig.calibration.probe_min,
                   jumperlessConfig.calibration.probe_max );

    bool shortOk = ( fwd == 2 && rev == 2 );
    // ponytail: the absolute window stays wide (-1.5..8 mA) because the
    // expected baseline depends on the unattended switch position; the
    // position-specific signal is the LED delta gate below.
    bool curOk = ( curOff > -1.5f && curOff < 8.0f );
    bool tipOk = ( tipRaw < 4090 ); // pegged high = tip divider shorted to supply
    // In SELECT position a healthy cable MUST show the LED current step.
    bool dataOk = !selectPos || dataPathProven;
    paintRowStatus( 4, curOk && dataOk );
    paintRowStatus( 5, tipOk );

    // Short-open policy: with data+button on BUTTON_PIN, the GPIO2-side
    // contact is unused by firmware - an open short with a PROVEN data path
    // is downgraded to a warning in the detail string. When the data pin is
    // still GPIO2 (flag off), an open short means the CPU-fallback sense pin
    // is flaky, so it stays a failure.
    bool shortWaived = !shortOk && dataPathProven && ( dataPin == (uint)BUTTON_PIN );
    if ( shortWaived ) {
        changeTerminalColor( 214, true ); // orange
        Serial.printf( "  WARNING: %s<->%s short open, but the %s data path works "
                       "end-to-end - firmware is unaffected (spare contact flaky)\n\r",
                       dataName, spareName, dataName );
        changeTerminalColor( -1, true );
    }

    snprintf( r.detail[ SELFTEST_PROBE_CABLE ], sizeof( r.detail[ 0 ] ),
              "short:%d/2+%d/2%s float:%s off:%.2fmA dLED:%.2fmA sw:%s tip:%d",
              fwd, rev, shortWaived ? "(waived)" : "", floats ? "ok" : "stuck",
              (double)curOff, (double)ledDelta, selectPos ? "sel" : "meas", tipRaw );
    r.status[ SELFTEST_PROBE_CABLE ] =
        ( ( shortOk || shortWaived ) && floats && curOk && tipOk && dataOk )
            ? SELFTEST_PASS
            : SELFTEST_FAIL;

    // Direct struct write doesn't set configChanged, so this can't trigger a
    // spurious config save; the runtime re-claims its GPIO on the next
    // routableBufferPower(1) pass.
    infraForceCandidate( "probe_power", -1 );
}

// ============================================================================
// Test 1: crossbar routing
// ============================================================================
// The CH446Qs have no readback, so a measured voltage through each route is
// the only ground truth that a crosspoint actually closed. Route DAC1 (2.5V)
// to every breadboard row with a rotating ADC0-3 return path, then loop the
// routable GPIOs and both rails through the matrix.

static void runCrossbarTest( SelfTestReport& r ) {
    Serial.println( "\n\r[selftest] crossbar routing..." );
    b.clear( );
    b.print( "Xbar", (uint32_t)0x120400 );
    showLEDsCore2 = 2;

    // Phase 2 drives ALL eight routable GPIOs as outputs. A GPIO buffer-power
    // claim would be fought mid-test (and the claim's HIGH drive would
    // corrupt its loopback reading) - pin the function to DAC1 so any claim
    // releases first. Same guard the cable test uses; unforced at the end.
    infraForceCandidate( "probe_power", 0 ); // candidate 0 = DAC1
    routableBufferPower( 1, 0, 1 );

    const float target = 2.5f;
    const float tol = 0.35f;
    setDacByNumber( 1, target, 0 );
    Serial.printf( "  phase 1/3: DAC1 at %.3fV routed to each of the 60 rows,\n\r"
                   "             read back through a rotating ADC0-3 return path\n\r"
                   "             (pass window %.2f..%.2fV; measured voltage is the\n\r"
                   "             only ground truth a crosspoint actually closed)\n\r",
                   (double)target, (double)( target - tol ), (double)( target + tol ) );

    int rowFails = 0;
    char rowList[ 32 ] = "";
    for ( int row = 1; row <= 60; row++ ) {
        globalState.clearAllConnections( );
        int adcCh = row % 4;
        addBridgeToState( DAC1, row );
        addBridgeToState( ADC0 + adcCh, row );
        refreshConnections( -1, 0, 1 );
        waitCore2( );
        delay( 8 );
        float v = readAdcVoltage( adcCh, 16 );
        bool ok = fabsf( v - target ) <= tol;
        paintRowStatus( row, ok ); // progressive green/red fill across the board
        // 4 rows per line keeps the live log readable across 60 rows
        Serial.printf( "  row %2d ADC%d %5.3fV %-4s%s", row, adcCh, (double)v,
                       ok ? "ok" : "FAIL", ( row % 4 == 0 ) ? "\n\r" : "  " );
        if ( !ok ) {
            rowFails++;
            size_t len = strlen( rowList );
            if ( len < sizeof( rowList ) - 5 ) {
                snprintf( rowList + len, sizeof( rowList ) - len, "%s%d",
                          len ? "," : "", row );
            }
        }
    }
    setDacByNumber( 1, 0.0, 0 );

    // GPIO loopback: drive each routable GPIO high/low through the matrix
    Serial.println( "  phase 2/3: GPIO 1-8 driven high then low, measured through routed ADC" );
    int gpioFails = 0;
    for ( int g = 0; g < 8; g++ ) {
        globalState.clearAllConnections( );
        int row = 15 + g; // spread across rows; exact row doesn't matter
        int adcCh = g % 4;
        addBridgeToState( RP_GPIO_1 + g, row );
        addBridgeToState( ADC0 + adcCh, row );
        refreshConnections( -1, 0, 1 );
        waitCore2( );
        delay( 5 );
        int pin = gpioDef[ g ][ 0 ];
        pinMode( pin, OUTPUT );
        digitalWrite( pin, HIGH );
        delay( 3 );
        float vHigh = readAdcVoltage( adcCh, 16 );
        digitalWrite( pin, LOW );
        delay( 3 );
        float vLow = readAdcVoltage( adcCh, 16 );
        pinMode( pin, INPUT );
        bool ok = !( vHigh < 2.9f || vHigh > 3.6f || fabsf( vLow ) > 0.4f );
        paintRowColor( row, ok ? 0x000814 : 0x160000 ); // blue = gpio phase
        Serial.printf( "  gpio %d loopback: high=%.3fV low=%.3fV %s\n\r", g + 1,
                       (double)vHigh, (double)vLow, ok ? "ok" : "FAIL" );
        if ( !ok ) {
            gpioFails++;
        }
    }

    // Rails through the matrix to their calibration ADCs
    Serial.println( "  phase 3/3: rails set to 3.300V, routed to ADC2/ADC3" );
    int railFails = 0;
    for ( int rail = 0; rail < 2; rail++ ) {
        globalState.clearAllConnections( );
        setDacByNumber( 2 + rail, 3.3f, 0 );
        addBridgeToState( rail == 0 ? TOP_RAIL : BOTTOM_RAIL, ADC2 + rail );
        refreshConnections( -1, 0, 1 );
        waitCore2( );
        delay( 8 );
        float v = readAdcVoltage( 2 + rail, 16 );
        setDacByNumber( 2 + rail, 0.0f, 0 );
        bool ok = fabsf( v - 3.3f ) <= tol;
        Serial.printf( "  %s rail set 3.300V, measured %.3fV %s\n\r",
                       rail == 0 ? "top" : "bottom", (double)v, ok ? "ok" : "FAIL" );
        if ( !ok ) {
            railFails++;
        }
    }

    globalState.clearAllConnections( );
    refreshConnections( -1, 0, 1 );

    // Restore the GPIO buffer-power preference; the next
    // routableBufferPower(1) pass re-claims a pin if enabled.
    infraForceCandidate( "probe_power", -1 );

    if ( rowFails || gpioFails || railFails ) {
        printChipStateArray( ); // routing model dump to help diagnose
        snprintf( r.detail[ SELFTEST_CROSSBAR ], sizeof( r.detail[ 0 ] ),
                  "rowFail:%d[%s] gpioFail:%d railFail:%d", rowFails, rowList,
                  gpioFails, railFails );
        r.status[ SELFTEST_CROSSBAR ] = SELFTEST_FAIL;
    } else {
        snprintf( r.detail[ SELFTEST_CROSSBAR ], sizeof( r.detail[ 0 ] ),
                  "rows:60/60 gpio:8/8 rails:2/2" );
        r.status[ SELFTEST_CROSSBAR ] = SELFTEST_PASS;
    }
}

// ============================================================================
// Test 2: probe tip voltage auto-cal
// ============================================================================
// Servo the measure-mode buffer voltage (config name:
// calibration.measure_mode_output_voltage) until the buffer output - which
// ADC7 is hardwired to - exactly matches what DAC1 at 3.300V reads
// through the same ADC calibration. Replaces the "watch the probe accuracy"
// step of probeCalibApp for factory purposes.

// Multi-burst averaged read of ADC7 (buffer output / probe tip). More cycles
// beat single-shot reads here: the tip node hangs off the TRRS cable, so
// anything wiggling the line (button poller, a hand on the probe, external
// wiring) shows up as burst-to-burst spread the caller can detect and reject
// instead of silently servoing to a moving target.
static float readTipAveraged( int bursts, int samplesPerBurst, float* spreadOut ) {
    float sum = 0.0f, mn = 99.0f, mx = -99.0f;
    for ( int i = 0; i < bursts; i++ ) {
        float v = readAdcVoltage( 7, samplesPerBurst );
        sum += v;
        if ( v < mn )
            mn = v;
        if ( v > mx )
            mx = v;
        delay( 2 );
    }
    if ( spreadOut )
        *spreadOut = mx - mn;
    return sum / bursts;
}

// Tip readings must sit centered in the acceptance window AND hold still.
#define TIP_SERVO_TOL_V 0.005f      // per-iteration convergence (averaged)
#define TIP_FINAL_TOL_V 0.008f      // final verification mean error
#define TIP_SPREAD_MAX_V 0.020f     // burst spread = something moving the tip

static void runTipVoltageTest( SelfTestReport& r ) {
    Serial.println( "\n\r[selftest] probe tip voltage..." );
    b.clear( );
    b.print( "Tip V", (uint32_t)0x0a0a00 );
    showLEDsCore2 = 2;

    // In SELECT position the probe LED is powered FROM the buffer output this
    // test measures: any lit color is a steady load that drags the node down
    // (hardware: dim select-idle color read 2.70V where LED-off reads 3.10V).
    // Latch the LED off BEFORE parking the handler so the reference and servo
    // see the same unloaded buffer that measure mode has at runtime (there the
    // LED is fed from PROBE_PIN instead).
    extern void setProbeLEDModeAndSettle( int mode ); // Apps.cpp
    setProbeLEDModeAndSettle( 10 ); // off

    // The probe button PIO poller periodically drives the shared cable line,
    // which couples onto the tip node at the millivolt scale this servo
    // works at. Park it for the whole test (mirrors the cable test).
    probeButtonPausePollingFromCore0( );

    // 1) Reference: DAC1 at 3.300V, fed through the SAME buffer + ADC7 path
    //    the servo uses. Using one sensor for both readings makes this a
    //    comparison, so ADC calibration error cancels - measuring the
    //    reference on a different ADC put the tip ~1% off (one probe row).
    //
    //    Why DAC1 and not a GPIO driven high: BUFFER_IN is not an unloaded
    //    node - the buffer's quiescent draw plus (in SELECT position) the
    //    probe LED's supply hang on it, and the load is nonlinear. On
    //    hardware a GPIO-high reference settled to a deterministic 2.70V
    //    (2.73V even at 12mA pad drive) where the low-impedance DAC path
    //    reads ~3.2V. There is no routable fixed 3.3V rail (NANO_3V3 is a
    //    probe pad, not a crossbar node), so the calibrated DAC1 is the
    //    stiffest honest 3.3V source available - and since reference and
    //    servo see the SAME load, matching them still equalizes voltages.
    Serial.println( "  step 1/2: measuring the 3.3V reference\n\r"
                    "    routing DAC1 (3.300V) -> ROUTABLE_BUFFER_IN,\n\r"
                    "    reading via ADC7 (same buffer + ADC the servo uses, so ADC\n\r"
                    "    calibration error cancels out of the comparison)" );
    globalState.clearAllConnections( );
    addBridgeToState( DAC1, ROUTABLE_BUFFER_IN );
    refreshConnections( -1, 0, 1 );
    waitCore2( );
    delay( 8 );
    setDacByNumber( 1, 3.3f, 0 );
    delay( 25 );

    // step 0 (piggybacked): two-point ADC7 self-calibration against the
    // calibrated DAC1. calibrateDacs() covers ADC0-3 only, so boards carry
    // ADC7's compile-time zero (8.0) forever - which reads ~+1V high
    // (hardware-observed: this very 3.3V reference reading 4.44V, and
    // measure mode's tip scaling off at the top of the pad ladder). The
    // DAC1 -> buffer -> ADC7 route is already up; sample two known levels
    // and solve spread/zero.
    {
        setDacByNumber( 1, 1.2f, 0 );
        delay( 25 );
        float raw1 = (float)readAdc( 7, 64 );
        setDacByNumber( 1, 3.3f, 0 );
        delay( 25 );
        float raw2 = (float)readAdc( 7, 64 );
        if ( raw2 - raw1 > 100.0f ) {
            float spread7 = ( 3.3f - 1.2f ) * 4095.0f / ( raw2 - raw1 );
            float zero7 = raw1 * spread7 / 4095.0f - 1.2f;
            // Sanity: same bands the other bipolar channels calibrate into.
            if ( spread7 > 15.0f && spread7 < 22.0f && zero7 > 7.0f && zero7 < 11.0f ) {
                adcSpread[ 7 ] = spread7;
                adcZero[ 7 ] = zero7;
                jumperlessConfig.calibration.adc_7_spread = spread7;
                jumperlessConfig.calibration.adc_7_zero = zero7;
                Serial.printf( "  ADC7 calibrated: spread=%.2f zero=%.2f (raw %.0f@1.2V, %.0f@3.3V)\n\r",
                               (double)spread7, (double)zero7, (double)raw1, (double)raw2 );
            } else {
                Serial.printf( "  ADC7 cal out of band (spread=%.2f zero=%.2f) - keeping current\n\r",
                               (double)spread7, (double)zero7 );
            }
        }
    }

    // Average 6 bursts and require them to agree: a wandering reference
    // poisons the servo target. Retry a couple of times - transients die -
    // then fail loudly so the operator knows the tip is being disturbed.
    float refSpread = 0.0f;
    float vTarget = 0.0f;
    for ( int attempt = 0; attempt < 3; attempt++ ) {
        vTarget = readTipAveraged( 6, 32, &refSpread );
        if ( refSpread <= TIP_SPREAD_MAX_V )
            break;
        Serial.printf( "    reference unstable (spread %.1fmV) - something is moving the tip, retrying...\n\r",
                       (double)( refSpread * 1000.0f ) );
        delay( 250 );
    }
    setDacByNumber( 1, 0.0f, 0 );
    Serial.printf( "  DAC1 3.3V reference (via buffer/ADC7): %.3f V (spread %.1f mV over 6 bursts)\n\r",
                   (double)vTarget, (double)( refSpread * 1000.0f ) );

    if ( refSpread > TIP_SPREAD_MAX_V ) {
        snprintf( r.detail[ SELFTEST_TIP_VOLTAGE ], sizeof( r.detail[ 0 ] ),
                  "tip unstable: ref spread %.0fmV (external disturbance?)",
                  (double)( refSpread * 1000.0f ) );
        r.status[ SELFTEST_TIP_VOLTAGE ] = SELFTEST_FAIL;
        probeButtonResumePollingFromCore0( );
        showProbeLEDs = ( switchPosition == 0 ) ? 3 : 4; // back to runtime color
        return;
    }
    if ( vTarget < 2.8f || vTarget > 3.6f ) {
        snprintf( r.detail[ SELFTEST_TIP_VOLTAGE ], sizeof( r.detail[ 0 ] ),
                  "bad 3.3V reference: %.3fV", (double)vTarget );
        r.status[ SELFTEST_TIP_VOLTAGE ] = SELFTEST_FAIL;
        probeButtonResumePollingFromCore0( );
        showProbeLEDs = ( switchPosition == 0 ) ? 3 : 4; // back to runtime color
        return;
    }

    // 2) Servo DAC0 -> ROUTABLE_BUFFER_IN until ADC7 (buffer output / probe
    //    tip) matches the DAC1 3.3V reference.
    Serial.println( "  step 2/2: servoing the probe tip voltage to match\n\r"
                    "    routing DAC0 -> ROUTABLE_BUFFER_IN; ADC7 is hardwired to the\n\r"
                    "    buffer output (= probe tip), adjusting DAC0 until they match" );
    globalState.clearAllConnections( );
    addBridgeToState( DAC0, ROUTABLE_BUFFER_IN );
    refreshConnections( -1, 0, 1 );
    waitCore2( );
    delay( 8 );

    // 2a) DAC sanity sweep before servoing: three points across the working
    //     range must track through the buffer within a coarse tolerance and
    //     be monotonic. Catches a dead DAC0, a wrong-slope calibration, or a
    //     mis-routed buffer in one readable line instead of 40 confused
    //     servo iterations walking into a rail.
    {
        const float sweepSet[ 3 ] = { 3.0f, 3.5f, 4.0f };
        float sweepGot[ 3 ];
        bool sweepOk = true;
        for ( int i = 0; i < 3; i++ ) {
            setDac0voltage( sweepSet[ i ], 0 );
            delay( 25 );
            sweepGot[ i ] = readTipAveraged( 3, 32, nullptr );
            if ( fabsf( sweepGot[ i ] - sweepSet[ i ] ) > 0.15f )
                sweepOk = false;
        }
        if ( !( sweepGot[ 0 ] < sweepGot[ 1 ] && sweepGot[ 1 ] < sweepGot[ 2 ] ) )
            sweepOk = false;
        Serial.printf( "  DAC sanity sweep: 3.0->%.3fV 3.5->%.3fV 4.0->%.3fV %s\n\r",
                       (double)sweepGot[ 0 ], (double)sweepGot[ 1 ], (double)sweepGot[ 2 ],
                       sweepOk ? "(tracks, ok)" : "(NOT TRACKING)" );
        if ( !sweepOk ) {
            setDac0voltage( 0.0f, 0 );
            globalState.clearAllConnections( );
            refreshConnections( -1, 0, 1 );
            probeButtonResumePollingFromCore0( );
            showProbeLEDs = ( switchPosition == 0 ) ? 3 : 4; // back to runtime color
            snprintf( r.detail[ SELFTEST_TIP_VOLTAGE ], sizeof( r.detail[ 0 ] ),
                      "dac sweep not tracking: 3.0->%.2f 3.5->%.2f 4.0->%.2f",
                      (double)sweepGot[ 0 ], (double)sweepGot[ 1 ], (double)sweepGot[ 2 ] );
            r.status[ SELFTEST_TIP_VOLTAGE ] = SELFTEST_FAIL;
            return;
        }
    }

    float set = jumperlessConfig.calibration.measure_mode_output_voltage;
    if ( set < 2.8f || set > 5.0f )
        set = 3.33f;

    // Each iteration reads a 3-burst average; convergence needs the error
    // inside the CENTER of the window (+/-5mV of a +/-8mV acceptance) three
    // times in a row, so a lucky pair of noisy reads can't end the servo
    // near the window edge - "sometimes a little bit off".
    float v = 0.0f;
    int converged = 0;
    bool done = false;
    for ( int iter = 0; iter < 40 && !done; iter++ ) {
        setDac0voltage( set, 0 );
        delay( 25 );
        v = readTipAveraged( 3, 32, nullptr );
        float err = vTarget - v;
        if ( fabsf( err ) < TIP_SERVO_TOL_V ) {
            if ( ++converged >= 3 )
                done = true;
        } else {
            converged = 0;
            set += err * 0.8f;
            set = constrain( set, 2.5f, 5.2f );
        }
        Serial.printf( "  set=%.4f measured=%.4f target=%.4f (avg of 3x32)\n\r",
                       (double)set, (double)v, (double)vTarget );
    }

    // Final verification at the converged setpoint: a long averaged burst
    // must sit centered on the target AND hold still. Catches a servo that
    // exited on flattering reads and anything still disturbing the tip.
    float finalErr = 0.0f, finalSpread = 0.0f;
    if ( done ) {
        v = readTipAveraged( 8, 32, &finalSpread );
        finalErr = vTarget - v;
        bool centered = fabsf( finalErr ) <= TIP_FINAL_TOL_V;
        bool still = finalSpread <= TIP_SPREAD_MAX_V;
        Serial.printf( "  verification (8x32): tip=%.4fV err=%+.1fmV spread=%.1fmV -> %s\n\r",
                       (double)v, (double)( finalErr * 1000.0f ),
                       (double)( finalSpread * 1000.0f ),
                       ( centered && still ) ? "centered + stable"
                       : ( centered ? "UNSTABLE" : "OFF-CENTER" ) );
        done = centered && still;
    }

    setDac0voltage( 0.0f, 0 );
    globalState.clearAllConnections( );
    refreshConnections( -1, 0, 1 );

    // ------------------------------------------------------------------
    // GPIO-variant phase: measure the droop model's constants for real.
    // Force the probe-power GPIO candidate, read the unloaded tip (-> V0
    // seed), then pull a known load through the ISENSE shunt so INA0 -
    // the most accurate measurement on the board - reads the true feed
    // current while ADC7 reads the drooped tip:
    //     probe_droop_ohms = (V0 - V_loaded) / I
    // V5-only, and non-fatal: a failed phase leaves probe_droop_ohms = 0,
    // which keeps the computed pad+crosspoints fallback in play.
    // ------------------------------------------------------------------
    float droopOhmsMeasured = 0.0f;
    if ( done ) {
        Serial.println( "  GPIO droop phase: forcing GPIO probe-power candidate..." );
        infraSetProbePowerEnabled( true );
        infraForceCandidate( "probe_power", 1 ); // candidate 1 = GPIO scan
        refreshConnections( -1, 0, 0 );
        waitCore2( );
        delay( 30 );
        if ( infraProbePowerGpioIdx( ) >= 0 ) {
            float spread0 = 0.0f;
            float v0 = readTipAveraged( 4, 32, &spread0 );
            // Load: BUFFER_IN -> INA0 shunt (ISENSE) -> GND. Reserved-node
            // allowance is already open for the session.
            addBridgeToState( ROUTABLE_BUFFER_IN, ISENSE_PLUS, 0, false );
            addBridgeToState( ISENSE_MINUS, GND, 0, false );
            refreshConnections( -1, 0, 0 );
            waitCore2( );
            delay( 50 ); // INA conversion + settle
            float vL = readTipAveraged( 4, 32, nullptr );
            float i1 = INA0.getCurrent_mA( );
            delay( 20 );
            float i2 = INA0.getCurrent_mA( );
            float i_mA = ( i1 + i2 ) * 0.5f;
            removeBridgeFromState( ROUTABLE_BUFFER_IN, ISENSE_PLUS, false );
            removeBridgeFromState( ISENSE_MINUS, GND, false );
            refreshConnections( -1, 0, 1 );
            if ( i_mA > 1.0f && v0 > vL ) {
                droopOhmsMeasured = ( v0 - vL ) * 1000.0f / i_mA;
            }
            Serial.printf( "  droop: V0=%.4fV loaded=%.4fV I=%.2fmA -> R=%.1f ohm\n\r",
                          (double)v0, (double)vL, (double)i_mA,
                          (double)droopOhmsMeasured );
            if ( droopOhmsMeasured >= 10.0f && droopOhmsMeasured <= 400.0f ) {
                jumperlessConfig.calibration.probe_droop_ohms = droopOhmsMeasured;
                if ( v0 >= 3.0f && v0 <= 3.6f ) {
                    jumperlessConfig.calibration.probe_droop_v0 = v0;
                }
            } else {
                Serial.println( "  droop R out of sanity band - keeping computed fallback" );
                droopOhmsMeasured = 0.0f;
            }
        } else {
            Serial.println( "  no free GPIO for the droop phase - skipping (computed fallback stays)" );
        }
        infraForceCandidate( "probe_power", -1 );
        infraSetProbePowerEnabled( false ); // session teardown restores the real state
        globalState.clearAllConnections( );
        refreshConnections( -1, 0, 1 );
    }

    probeButtonResumePollingFromCore0( );
    showProbeLEDs = ( switchPosition == 0 ) ? 3 : 4; // back to runtime color

    snprintf( r.detail[ SELFTEST_TIP_VOLTAGE ], sizeof( r.detail[ 0 ] ),
              "ref:%.3fV tip:%.3fV err:%+.1fmV spr:%.1fmV dac0set:%.3fV droopR:%.0f%s",
              (double)vTarget, (double)v, (double)( finalErr * 1000.0f ),
              (double)( finalSpread * 1000.0f ), (double)set,
              (double)droopOhmsMeasured,
              done ? "" : " no-converge" );
    if ( done ) {
        // Do NOT touch probe_max here. An earlier version rescaled it by the
        // tip-voltage ratio, assuming pad readings scale with the measure-mode
        // drive - hardware says otherwise: the select-mode full-scale reading
        // stays ~4040 raw regardless of the servo result (a 3.33->3.18V servo
        // dragged probe_max 4055->3867 and shifted the pad->row map). probe_max
        // belongs to the Probe Pads calibration alone.
        jumperlessConfig.calibration.measure_mode_output_voltage = set;
        saveConfig( ); // synchronous: first-start restarts right after this
        r.status[ SELFTEST_TIP_VOLTAGE ] = SELFTEST_PASS;
    } else {
        r.status[ SELFTEST_TIP_VOLTAGE ] = SELFTEST_FAIL;
    }
}

// ============================================================================
// Test 3: PSRAM
// ============================================================================

static void runPsramTest( SelfTestReport& r ) {
    Serial.println( "\n\r[selftest] psram..." );
    b.clear( );
    b.print( "PSRAM", (uint32_t)0x001008 );
    showLEDsCore2 = 2;

    size_t sz = rp2040.getPSRAMSize( );
    if ( sz == 0 ) {
        Serial.println( "  no PSRAM detected (optional mod kit) - skipping" );
        snprintf( r.detail[ SELFTEST_PSRAM ], sizeof( r.detail[ 0 ] ), "not installed" );
        r.status[ SELFTEST_PSRAM ] = SELFTEST_SKIP; // optional mod kit, not a failure
        return;
    }
    Serial.printf( "  detected %u bytes (%u MB) at 0x11000000\n\r", (unsigned)sz,
                   (unsigned)( sz >> 20 ) );
    Serial.println( "  writing distinct patterns at 5 offsets spread across the chip\n\r"
                    "  (first byte, 1/4, 1/2, 3/4, last byte - catches dead bus AND\n\r"
                    "  aliasing windows), reading back, restoring original contents..." );
    bool ok = psram_pattern_test( sz );
    Serial.printf( "  pattern test: %s\n\r", ok ? "ok" : "FAIL" );
    snprintf( r.detail[ SELFTEST_PSRAM ], sizeof( r.detail[ 0 ] ), "%u bytes %s",
              (unsigned)sz, ok ? "ok" : "read/write FAILED" );
    r.status[ SELFTEST_PSRAM ] = ok ? SELFTEST_PASS : SELFTEST_FAIL;
}

// ============================================================================
// Test 4: peripherals (I2C ACK + EEPROM readback; OLED reported, not failed)
// ============================================================================

static bool i2cAck( uint8_t addr ) {
    Wire.beginTransmission( addr );
    return Wire.endTransmission( ) == 0;
}

static void runPeripheralsTest( SelfTestReport& r ) {
    Serial.println( "\n\r[selftest] peripherals..." );
    b.clear( );
    b.print( "Chips", (uint32_t)0x0c0010 );
    showLEDsCore2 = 2;

    bool ina0 = i2cAck( 0x40 );
    Serial.printf( "  INA219 #0 (0x40): %s\n\r", ina0 ? "ack" : "NO ACK" );
    bool ina1 = i2cAck( 0x41 );
    Serial.printf( "  INA219 #1 (0x41): %s\n\r", ina1 ? "ack" : "NO ACK" );
    bool dac = i2cAck( 0x60 );
    Serial.printf( "  MCP4728 DAC (0x60): %s\n\r", dac ? "ack" : "NO ACK" );
    // The boot path writes 0xAA here before we can run, so a readback of
    // anything else means the emulated-EEPROM flash sector is broken.
    uint8_t eepromByte = EEPROM.read( FIRSTSTARTUPADDRESS );
    bool eeprom = ( eepromByte == 0xAA );
    Serial.printf( "  EEPROM first-start byte @%d: 0x%02X (expect 0xAA) %s\n\r",
                   FIRSTSTARTUPADDRESS, eepromByte, eeprom ? "ok" : "FAIL" );
    bool oledConn = oled.isConnected( ); // optional accessory: reported only
    Serial.printf( "  OLED: %s (optional, never fails the board)\n\r",
                   oledConn ? "connected" : "not connected" );

    snprintf( r.detail[ SELFTEST_PERIPHERALS ], sizeof( r.detail[ 0 ] ),
              "ina0:%d ina1:%d dac:%d eeprom:%d oled:%d", ina0, ina1, dac,
              eeprom, oledConn );
    r.status[ SELFTEST_PERIPHERALS ] =
        ( ina0 && ina1 && dac && eeprom ) ? SELFTEST_PASS : SELFTEST_FAIL;
}

// ============================================================================
// Runners
// ============================================================================

typedef void ( *SelfTestFn )( SelfTestReport& );
static const SelfTestFn selfTestFns[ SELFTEST_NUM_TESTS ] = {
    runProbeCableTest, runCrossbarTest, runTipVoltageTest, runPsramTest,
    runPeripheralsTest,
};

// Shared session wrapper: temp slot + hardware normalize around the given
// tests, then teardown, overlay, and reports.
static void runSelfTestSession( SelfTestReport& r, const bool runMask[ SELFTEST_NUM_TESTS ],
                                bool fromFirstStart ) {
    if ( fromFirstStart ) {
        // First start runs before any host has opened the CDC port, and
        // TinyUSB drops writes when no DTR is asserted - so the whole live
        // log would vanish. Give the operator's terminal / watch script up
        // to 10s to attach; proceed anyway if nobody is listening (the
        // LEDs/OLED still show everything, and the JSON is stored).
        oledStatus( "Self Test" );
        unsigned long waitStart = millis( );
        while ( !Serial && millis( ) - waitStart < 10000 ) {
            delay( 50 );
        }
        delay( 200 ); // let the freshly-opened terminal settle
    }

    char id[ 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1 ];
    pico_get_unique_board_id_string( id, sizeof( id ) );
    Serial.println( "\n\r======== Hardware Self Test ========" );
    Serial.printf( "board: %s  fw: %s\n\r", id, firmwareVersion );
    oledStatus( "Self Test" );

    SlotManager::getInstance( ).enterTemporarySlot( 8 );
    selfTestNormalizeHardware( );

    // The tests own the crossbar and the buffer feed for the whole session:
    // park the probe-power infra function (or infraEvaluate would keep
    // re-adding the DAC1->BUFFER_IN feed into every test's refresh) and open
    // the reserved-node allowance for the tip test's direct BUFFER_IN
    // bridges. Both restored in the teardown below.
    bool wasProbePowerOn = infraProbePowerWanted( );
    infraSetProbePowerEnabled( false );
    infraSetSystemBridgeAllowance( true );

    for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
        if ( !runMask[ i ] )
            continue;
        oledStatus( oledNames[ i ] );
        selfTestFns[ i ]( r );

        // Re-park probe power after EVERY test: the cable test switches it
        // on for its INA signature (via routableBufferPower), and a feed
        // left enabled poisons the next test - the tip test's DAC1->BUF_IN
        // reference bridge matches the still-registered infra pair, so
        // evaluation takes it over and re-parks DAC1 at
        // measure_mode_output_voltage mid-measurement (hardware-observed as
        // a 'bad 3.3V reference' that reads whatever the parked level is).
        // The refresh flushes the teardown so the pair is unregistered
        // BEFORE the next test adds its own bridges on those nodes.
        infraForceCandidate( "probe_power", -1 );
        infraSetProbePowerEnabled( false );
        refreshConnections( -1, 0, 0 );

        // Live result: colored serial line + OLED verdict for this test
        bool fail = r.status[ i ] == SELFTEST_FAIL;
        changeTerminalColor( fail ? 196 : 84, true );
        Serial.printf( "  => %s: %s  %s\n\r", selfTestNames[ i ],
                       statusWordUpper( r.status[ i ] ), r.detail[ i ] );
        changeTerminalColor( -1, true );
        char oledLine[ 24 ];
        snprintf( oledLine, sizeof( oledLine ), "%s %s", oledNames[ i ],
                  statusWordUpper( r.status[ i ] ) );
        oledStatus( oledLine );
        delay( SELFTEST_PAUSE_MS ); // let the verdict be readable before the next test
    }

    // Teardown: never let a test voltage / crosspoint / driven GPIO leak
    // into the user's session or survive the first-start restart.
    selfTestNormalizeHardware( );
    infraSetSystemBridgeAllowance( false );
    infraSetProbePowerEnabled( wasProbePowerOn );
    leaveApp( ); // restore original slot
    setRailsAndDACs( 0 );
    refreshConnections( -1 );
    routableBufferPower( 1, 0, 1 );
    b.clear( );

    paintSelfTestOverlay( r );
    printHumanReport( r );
    oledStatus( reportOverallPass( r ) ? "Test PASS" : "Test FAIL" );
    printSentinelReport( selfTestToJson( r ) );
    // NOTE: report persistence lives in runFullSelfTest, not here - retry
    // rounds call this with partial masks and must not clobber the stored
    // result with a partial view.
}

// Countdown before a retry round. Any human input (probe button, encoder
// click/turn, or a serial byte) aborts the retry loop and accepts the
// failing report as final.
static bool selfTestRetryCountdownAborted( int seconds ) {
    encoderButtonState = IDLE;
    encoderDirectionState = NONE;
    while ( Serial.available( ) > 0 ) {
        Serial.read( );
    }
    for ( int s = seconds; s > 0; s-- ) {
        Serial.printf( "  retrying failed tests in %d... (any input keeps the failing report)  \r", s );
        Serial.flush( );
        unsigned long t0 = millis( );
        while ( millis( ) - t0 < 1000 ) {
            if ( checkProbeButtonState( ) != 0 )
                return true;
            if ( encoderButtonState != IDLE || isEncoderButtonPhysicallyPressed( ) )
                return true;
            if ( encoderDirectionState != NONE )
                return true;
            if ( Serial.available( ) > 0 )
                return true;
            delay( 10 );
        }
    }
    Serial.println( );
    return false;
}

void runFullSelfTest( bool fromFirstStart ) {
    SelfTestReport r;
    initReport( r );
    const bool all[ SELFTEST_NUM_TESTS ] = { true, true, true, true, true };
    runSelfTestSession( r, all, fromFirstStart );

    // Loop until EVERYTHING passes, re-running only the failed tests each
    // round (the operator reseats the cable / fixes the fixture between
    // rounds; passed results are kept). Any input during the countdown
    // accepts the failing report instead of looping forever.
    int round = 2;
    while ( !reportOverallPass( r ) ) {
        bool mask[ SELFTEST_NUM_TESTS ];
        char failed[ 64 ] = "";
        for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
            mask[ i ] = ( r.status[ i ] == SELFTEST_FAIL );
            if ( mask[ i ] ) {
                size_t len = strlen( failed );
                snprintf( failed + len, sizeof( failed ) - len, "%s%s",
                          len ? "," : "", selfTestNames[ i ] );
            }
        }
        changeTerminalColor( 196, true );
        Serial.printf( "\n\rStill failing: %s\n\r", failed );
        changeTerminalColor( -1, true );
        char oledLine[ 24 ];
        snprintf( oledLine, sizeof( oledLine ), "Retry %d", round );
        oledStatus( oledLine );
        if ( selfTestRetryCountdownAborted( 5 ) ) {
            Serial.println( "\n\rRetry aborted - keeping the failing report." );
            break;
        }
        Serial.printf( "\n\r======== retry round %d: %s ========\n\r", round, failed );
        runSelfTestSession( r, mask, false );
        round++;
    }

    // Persist the final accumulated report (pass or operator-aborted fail).
    String json = selfTestToJson( r );
    writeTextFile( SELFTEST_JSON_PATH, json );
    if ( fromFirstStart ) {
        char marker[ SELFTEST_NUM_TESTS + 2 ];
        for ( int i = 0; i < SELFTEST_NUM_TESTS; i++ ) {
            marker[ i ] = statusChars[ r.status[ i ] ];
        }
        marker[ SELFTEST_NUM_TESTS ] = '\n';
        marker[ SELFTEST_NUM_TESTS + 1 ] = '\0';
        writeTextFile( SELFTEST_MARKER_PATH, String( marker ) );
    }

    // Manual full runs hold + reset here; the first-start path (calibrateDacs
    // tail) instead chains the touch into the interactive probe pad
    // calibration, wipes the undo history, and resets when that finishes.
    if ( !fromFirstStart ) {
        selfTestWaitForInputThenReset( );
    }
}

// Block until any human input: probe button, encoder click/turn, or a serial byte.
static void waitForAnyInput( void ) {
    // Swallow whatever input state got us here
    encoderButtonState = IDLE;
    encoderDirectionState = NONE;
    while ( Serial.available( ) > 0 ) {
        Serial.read( );
    }
    delay( 300 ); // let a button held from earlier be released

    while ( true ) {
        if ( checkProbeButtonState( ) != 0 )
            break;
        if ( encoderButtonState != IDLE || isEncoderButtonPhysicallyPressed( ) )
            break;
        if ( encoderDirectionState != NONE )
            break;
        if ( Serial.available( ) > 0 )
            break;
        delay( 10 );
    }
}

void selfTestWaitForInput( const char* nextWhat ) {
    Serial.println( "\n\rResults are showing on the breadboard LEDs." );
    Serial.printf( "Touch a probe button, click or turn the encoder, or send any\n\r"
                   "serial byte to %s.\n\r", nextWhat );
    Serial.flush( );

    waitForAnyInput( );
}

void selfTestClearOverlay( void ) {
    graphicOverlayState.removeOverlay( SELFTEST_OVERLAY_NAME );
    showLEDsCore2 = -2;
}

void selfTestWaitForInputThenReset( void ) {
    selfTestWaitForInput( "reset the board" );

    Serial.println( "Resetting..." );
    Serial.flush( );
    delay( 150 );
    rp2040.restart( );
}

static void runSingleTest( int idx ) {
    SelfTestReport r;
    initReport( r );
    bool mask[ SELFTEST_NUM_TESTS ] = { false, false, false, false, false };
    mask[ idx ] = true;
    runSelfTestSession( r, mask, false );

    // Single tests don't reset: hold the result on the LEDs until any input,
    // then clear the overlay and hand the board back as-is.
    Serial.println( "\n\rResult is showing on the breadboard LEDs." );
    Serial.println( "Touch a probe button, click or turn the encoder, or send any" );
    Serial.println( "serial byte to clear it." );
    Serial.flush( );
    waitForAnyInput( );
    selfTestClearOverlay( );
}

void probeCableTestApp( void ) { runSingleTest( SELFTEST_PROBE_CABLE ); }
void crossbarTestApp( void ) { runSingleTest( SELFTEST_CROSSBAR ); }
void tipVoltageTestApp( void ) { runSingleTest( SELFTEST_TIP_VOLTAGE ); }
void psramTestApp( void ) { runSingleTest( SELFTEST_PSRAM ); }
void fullSelfTestApp( void ) { runFullSelfTest( false ); }

// ============================================================================
// Stored report / boot repaint
// ============================================================================

void selfTestPrintStoredReport( void ) {
    if ( !safeFileExists( SELFTEST_JSON_PATH ) ) {
        Serial.println( "::SELFTEST::none::END::" );
        return;
    }
    File f = safeFileOpen( SELFTEST_JSON_PATH, "r" );
    if ( !f ) {
        Serial.println( "::SELFTEST::none::END::" );
        return;
    }
    String json = f.readString( );
    safeFileClose( f, false );
    json.trim( );
    printSentinelReport( json );
}

void selfTestShowSavedResultIfPending( void ) {
    if ( !safeFileExists( SELFTEST_MARKER_PATH ) )
        return;
    safeFileDelete( SELFTEST_MARKER_PATH ); // one-shot
    // The operator already saw (and dismissed) the LED overlay during the
    // post-test hold, so don't repaint it - just re-print the JSON for hosts
    // that missed the pre-restart report across the USB re-enumeration.
    selfTestPrintStoredReport( );
}

#endif // OG_JUMPERLESS
