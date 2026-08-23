#include "Apps.h"
#include "ReadingDisplay.h" // resetLastShown() - drop the live-reading pin
#include "InfraPaths.h"

#include "ArduinoStuff.h"
#include "CH446Q.h"
#include "Commands.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"
#include "GraphicOverlays.h"
#include "Graphics.h"
#include "JumperlOS.h" // For ContextManager
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "Menus.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "ProjectsApp.h"  // projectsAppLauncher - the "Guides" apps[] row
#include "Python_Proper.h"
#include "RotaryEncoder.h"
#include "SelfTest.h"
#include "States.h"
#include "USBAudio.h"
#include "Undo.h"
#include "config.h"
#include "configManager.h"
#include "oled.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <algorithm> // std::sort for medianInPlace

#include "Images.h"
#include "externVars.h"
#include <Display_cfg.h>
#include <DmxInput.h>
#include <DmxOutput.h>
#include <JDI_MIP_Display.h>

#define DISPLAY_WIDTH 72
#define DISPLAY_HEIGHT 144

#define COLOR_BLACK 0x00
#define COLOR_BLUE 0x02
#define COLOR_GREEN 0x04
#define COLOR_CYAN 0x06
#define COLOR_RED 0x08
#define COLOR_MAGENTA 0x0a
#define COLOR_YELLOW 0x0c
#define COLOR_WHITE 0x0e

#define CMD_NO_UPDATE 0x00
#define CMD_BLINKING_BLACK 0x10
#define CMD_BLINKING_INVERSION 0x14
#define CMD_BLINKING_WHITE 0x18
#define CMD_ALL_CLEAR 0x20
#define CMD_VCOM 0x40
#define CMD_UPDATE 0x90
#define OLED_CONNECTED 0

// JDI display now allocated dynamically in jdiMIPdisplay() to save 5.1KB
JDI_MIP_Display* jdi_display = nullptr;

// Wrapper for i2cScan to match void(*)(void)
static void i2cScanWrapper( void ) { i2cScan( ); }

static void pythonScriptsAppLauncher( void );



struct app apps[ NUM_APPS ] = {
    { "Bounce Startup", 0, 1, bounceStartup },
    { "Calib  DACs", 1, 1, calibrateDacs },
    { "I2C    Scan", 2, 1, i2cScanWrapper },
    { "Custom App", 3, 1, customApp },
    { "PNG Image", 4, 1, displayImage },
    { "Scan", 5, 1, scanBoard },
    { "XLSX   GUI", 6, 1, xlsxGui },
    { "Micropython", 7, 1, microPythonREPLapp },
    { "uPythonREPL", 8, 1, microPythonREPLapp },
    { "File   Manager", 9, 1, fileManagerApp },
    { "eKilo Editor", 10, 1, eKiloApp },
    { "Probe  Calib", 11, 1, probeCalibApp },
    { "JDI MIPdisplay", 12, 1, jdiMIPdisplay },
    { "DOOM", 16, 1, playDoom },
    { "Snake", 17, 1, runSnakeGame },
    { "DMX Serial", 13, 1, DMXSerialApp },
    { "OLED Images", 14, 1, imagesAppLauncher },
    { "Switch Calib", 15, 1, calibrateProbeSwitchThresholds },
    { "Python", 18, 1, pythonScriptsAppLauncher },
    { "Files", 19, 1, pythonScriptsAppLauncher },
    { "Full   Test", 20, 1, fullSelfTestApp },
    { "Probe  Cable", 21, 1, probeCableTestApp },
    { "Xbar   Test", 22, 1, crossbarTestApp },
    { "Tip    Voltage", 23, 1, tipVoltageTestApp },
    { "PSRAM  Test", 24, 1, psramTestApp },
    // Name-matched by runApp(-1, name) with the TOP-LEVEL menu line's own text
    // (menuTree.h "Guides" -> Menus.cpp getActionCategory -> APPSACTION), so
    // this string and that one move together or the row goes dead.
    { "Guides", 25, 1, projectsAppLauncher },
    // others can remain uninitialized (works=0)
};

// Helper: normalize spaces (trim + collapse)
String normalizeSpaces( const char* s ) {
    if ( !s )
        return String( );
    String in( s );
    in.trim( );
    String out;
    out.reserve( in.length( ) );
    bool lastSpace = false;
    for ( size_t i = 0; i < in.length( ); ++i ) {
        char c = in[ i ];
        if ( c == '\31' ) {
            // out += '\n';
            continue;
        }
        if ( c == ' ' ) {
            if ( !lastSpace ) {
                out += ' ';
                lastSpace = true;
            }
        } else {
            out += c;
            lastSpace = false;
        }
    }
    return out;
}

void runApp( int index, char* name ) {
    const int APP_COUNT = NUM_APPS;

    // Apps print scrolling output and own the terminal, which moves the rows a
    // live reading pinned for itself. Drop that anchor so the next reading
    // pins a fresh pair below the app's output rather than erasing one of its
    // lines. (See ReadingDisplay::resetLastShown.)
    ReadingDisplay::resetLastShown( );

    // Try to resolve by name
    if ( index < 0 || index >= APP_COUNT ) {
        if ( name ) {
            String want = normalizeSpaces( name );
            for ( int i = 0; i < APP_COUNT; ++i ) {
                if ( !apps[ i ].works || !apps[ i ].action )
                    continue;
                if ( normalizeSpaces( apps[ i ].name ).equalsIgnoreCase( want ) ) {
                    index = i;
                    break;
                }
            }
        }
    }

    // Validate
    if ( index < 0 || index >= APP_COUNT || !apps[ index ].works || !apps[ index ].action ) {
        Serial.println( F( "App not found" ) );
        return;
    }

    // Ensure we have a display name
    if ( !name )
        name = apps[ index ].name;

    Serial.println( "Running app: " + normalizeSpaces( name ) );
    // Serial.println( "Index: " + String( index ) );

    // Determine context type based on app name
    // Some apps (File Manager, MicroPython REPL, Python/Files) push their own specific context,
    // so we use APP_GENERIC for others to avoid double-pushing
    String appName = normalizeSpaces( name );
    bool appPushesOwnContext = appName.equalsIgnoreCase( "file manager" ) ||
                               appName.equalsIgnoreCase( "micropython repl" ) ||
                               appName.equalsIgnoreCase( "python" ) ||
                               appName.equalsIgnoreCase( "files" );

    if ( !appPushesOwnContext ) {
        // Push generic app context for cleanup tracking
        ContextEntry ctx( ContextType::APP_GENERIC );
        ctx.onExit = []( void* ) {
            // Generic cleanup - close any open files
            closeAllFiles( );
        };
        ContextManager::getInstance( ).pushContext( ctx );
    }

    // Direct dispatch (no giant switch)
    apps[ index ].action( );

    // Pop context if we pushed one
    if ( !appPushesOwnContext ) {
        ContextManager::getInstance( ).popContext( );
    }
}

// Run a Python script from path. Uses safeFileOpen/safeFileClose for thread safety.
static void runPythonScriptFromPath( const String& path ) {
    if ( path.length( ) == 0 )
        return;
    File f = safeFileOpen( path.c_str( ), "r" );
    if ( !f ) {
        Serial.println( "\r\nFailed to open " + path );
        return;
    }
    String content = f.readString( );
    safeFileClose( f, false );
    if ( content.length( ) == 0 ) {
        Serial.println( "\r\nScript is empty: " + path );
        return;
    }
    Serial.print( "\033[2J\033[H" );
    Serial.flush( );
    delay( 30 );
    setGlobalStreamWithInterrupt( &Serial );  // Ensure Python output goes to main serial

    // Reset encoder state so the selection click doesn't instantly trigger KeyboardInterrupt.
    // Add delay so user can release and encoder state machine can settle.
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    delay( 400 );

    Serial.println( "\r\nRunning " + path + " ...\r\n" );
    executePythonFileContent( content.c_str( ) );
    Serial.println( "\r\n--- script finished ---" );
}

// Pick a .py file from click menu, then run it.
static void pythonScriptsAppLauncher( void ) {
    String path = pickPythonScriptFromClickMenu( );
    if ( path.length( ) > 0 ) {
        runPythonScriptFromPath( path );
    }
}

void microPythonREPLapp( void ) {
    enterMicroPythonREPL( global_mp_stream );
}

void leaveApp( void ) {
    SlotManager::getInstance( ).exitTemporarySlot( );  // Restores original slot + refreshes hardware
}

void fileManagerApp( void ) { filesystemApp( true ); }

// Median of the calibration readings (sorts in place; callers don't reuse the
// array). The reading distribution is bimodal when the probe LED drops out
// mid-sample (its supply is the rail being measured): a mean gets dragged
// toward the artifact, the median ignores it as long as good samples are the
// majority. Shared (non-static): Probing.cpp and SelfTest.cpp extern it.
float medianInPlace( float* vals, int n ) {
    std::sort( vals, vals + n );
    return ( n & 1 ) ? vals[ n / 2 ] : 0.5f * ( vals[ n / 2 - 1 ] + vals[ n / 2 ] );
}

// Drive the probe LED the same way runtime does: hand the requested mode to
// core1's probeLEDhandler via showProbeLEDs and wait for it to latch. Calling
// probeLEDs.showBlocking() directly from core0 pushes pixel data at the shared
// PIO state machine WITHOUT pausing the button-polling program that owns it,
// so the LED ends up dark/garbage - and since the LED is powered from the
// rail the calibration measures, that corrupts the readings themselves.
// Shared (non-static): SelfTest.cpp externs it to latch a known LED load
// before its current measurements.
// Returns true when core 1 consumed the request (a frame went out), false
// when it timed out - core2stuff only calls probeLEDhandler while
// checkingButton == 0 AND the probe button state machine is idle, so a
// button the cable test's own line-driving left "pressed" silently starves
// every frame and the calibration measures a stale colour.
bool setProbeLEDModeAndSettle( int mode ) {
    showProbeLEDs = mode;
    unsigned long start = millis( );
    while ( showProbeLEDs != 0 && millis( ) - start < 200 ) {
        delay( 1 );
    }
    bool latched = ( showProbeLEDs == 0 );
    if ( !latched ) {
        Serial.printf( "  (probe LED mode %d NOT latched by core 1 in 200 ms: checkingButton=%d buttonState=%d)\n\r",
                       mode, (int)checkingButton, ProbeButton::getInstance( ).getButtonState( ) );
    }
    delay( 200 ); // let the LED's supply current settle before sampling
    return latched;
}

// Hot-plug poll for the OLED while a calibration app is running, so a display
// plugged in mid-calibration comes up without restarting the app. Reuses the
// boot-time autodetect (internal I2C0 ping + config fixup), which is a cheap
// addressed ping when nothing is plugged. Crossbar-attached OLEDs can't be
// hot-plugged here: connecting one would add bridges that disturb the
// calibration in progress. Returns true the moment a new connection comes up
// so callers can redraw their instructions.
static bool oledCalibHotplugPoll( void ) {
    static unsigned long lastPoll = 0;
    if ( oled.isConnected( ) || millis( ) - lastPoll < 1000 ) {
        return false;
    }
    lastPoll = millis( );
    if ( !autoDetectAndConfigureOled( ) ) {
        return false;
    }
    oled.init( );
    return oled.isConnected( );
}

// One position step of the switch calibration: the user has just confirmed
// the switch is in `wantPos` (0 = MEASURE, 1 = SELECT). Reads, in order:
//   1. detector A (tip sense) - the cheap check that the switch really IS
//      where we asked; a contradiction means the readings below would be
//      garbage, so the caller refuses to save;
//   2. NUM_READINGS of the INA1 zero-corrected current with the feed pinned
//      to DAC0 (the legacy signature; median);
//   3. with the feed pinned to the GPIO candidate: the unloaded/loaded ADC7
//      tip voltage (median) and the feed-blink held % (detector B, GPIO).
// Leaves the feed pinned back on DAC0.
struct SwitchCalibStep {
    int   detA;        // 0/1/-1
    float inaMedian;   // mA, DAC0 feed
    float adc7Median;  // V, GPIO feed
    int   blinkPct;    // -1 = not measured
    bool  gpioOk;      // a GPIO candidate was available
};

static SwitchCalibStep switchCalibReadPosition( int wantPos, int ledMode, int rowStart ) {
    SwitchCalibStep r = { -1, 0.0f, 0.0f, -1, false };
    const int NUM_READINGS = 20;
    float readings[ NUM_READINGS ];

    setProbeLEDModeAndSettle( ledMode ); // latched by core1 like at runtime
    r.detA = probeSwitchTipSenseNow( );
    (void)checkProbeCurrent( );          // throwaway read: flush the conversion that
                                         // straddled the LED/switch change

    Serial.printf( "Taking %s readings (DAC0 feed, INA1)...\n\r", wantPos ? "SELECT" : "MEASURE" );
    b.clear( 0 );
    b.print( "Read ", 0x100010, 0x000000, 0, -1, -1 );
    int row = rowStart;
    for ( int i = 0; i < NUM_READINGS; i++ ) {
        delay( 200 );
        float current = checkProbeCurrent( );
        readings[ i ] = current;
        Serial.printf( "Reading %d: %.3f mA\n\r", i + 1, current );
        if ( i % 6 == 0 ) row++;
        b.printRawRow( 0xFF << ( ( 5 - ( i % 6 ) ) ), row, 0x001010, 0x000000 );
    }
    // Median, not mean: robust against the occasional sample taken while the
    // LED was mid-relatch or an I2C read hiccuped.
    r.inaMedian = medianInPlace( readings, NUM_READINGS );
    Serial.printf( "\n%s INA median: %.3f mA   tip sense: %s\n\r",
                   wantPos ? "SELECT" : "MEASURE", r.inaMedian,
                   r.detA == 0 ? "H (measure)" : ( r.detA == 1 ? "L (select)" : "-" ) );

    // GPIO feed pass: same position, the other feed. Force candidate 1, let
    // the rebuild land (same recipe as SelfTest's droop phase), then read the
    // tip and blink it.
    infraForceCandidate( "probe_power", 1 );
    refreshConnections( -1, 0, 0 );
    waitCore2( );
    delay( 30 );
    if ( infraProbePowerGpioIdx( ) >= 0 ) {
        r.gpioOk = true;
        for ( int i = 0; i < 9; i++ ) {
            delay( 20 );
            readings[ i ] = readAdcVoltage( 7, 16 );
        }
        r.adc7Median = medianInPlace( readings, 9 );
        int pct = -1;
        for ( int tries = 0; tries < 5 && pct < 0; tries++ ) {
            (void)probeSwitchFeedBlinkNow( &pct );
            if ( pct < 0 ) delay( 25 );
        }
        r.blinkPct = pct;
        Serial.printf( "%s GPIO feed: tip %.3f V   blink held %d%%\n\r",
                       wantPos ? "SELECT" : "MEASURE", r.adc7Median, r.blinkPct );
    } else {
        Serial.println( "(no free routable GPIO - skipping the GPIO-feed pass)" );
    }
    infraForceCandidate( "probe_power", 0 );
    refreshConnections( -1, 0, 0 );
    waitCore2( );
    delay( 30 );
    return r;
}

// Wait for a click (any button state) or a serial byte; false = aborted.
static bool switchCalibWaitClick( const char* oledText ) {
    encoderButtonState = IDLE;
    while ( encoderButtonState != HELD && encoderButtonState != PRESSED ) {
        if ( oledCalibHotplugPoll( ) ) {
            oled.showMultiLineSmallText( oledText, true, true );
        }
        if ( Serial.available( ) > 0 ) {
            while ( Serial.available( ) > 0 ) Serial.read( );
            return false;
        }
        delay( 10 );
    }
    delay( 50 );
    encoderButtonState = IDLE;
    return true;
}

void calibrateProbeSwitchThresholds( void ) {
    b.clear( );

   // showProbeLEDs = 4;
    cycleTerminalColor( true, 5.0, true );
    Serial.println( "\n\rProbe Switch Threshold Calibration" );
    cycleTerminalColor( );
    Serial.println( "This will automatically set the switch position detection thresholds\n\r" );
    Serial.println( "(any serial key aborts without saving)\n\r" );

    // (No manual `netSlot = 8` here: enterTemporarySlot sets netSlot,
    // activeSlotNumber AND activeSlotPath together. Assigning netSlot first
    // used to be harmless; with the path companion it would desync the pair
    // for the instant before enterTemporarySlot ran.)
    SlotManager::getInstance( ).enterTemporarySlot( 8 );  // Save current slot, switch to temp slot 8
    refreshConnections( -1, 0 );
    // routableBufferPower( 1, 1, 1 );
    //delay( 50 );
    checkProbeCurrentZero( );

    // The current thresholds ARE signatures through INA1 (its shunt R57 is
    // hardwired in DAC_0's output path), so the INA readings are taken with
    // the feed pinned to the DAC0 candidate; each position step then pins
    // the GPIO candidate for the tip / feed-blink numbers and comes back.
    // Unforced on EVERY exit (the done: label) - a forced feed left behind
    // would sit there until reboot.
    infraForceCandidate( "probe_power", 0 );

    routableBufferPower( 1, 1, 1 );

    // NOTE: thresholds are calibrated against the zero-corrected current
    // (checkProbeCurrent(), i.e. raw - probe_current_zero) - the SAME quantity
    // the live detector (checkSwitchPosition) compares. Using the calibrated
    // reading everywhere means a firmware update never invalidates a user's
    // saved thresholds, so they don't have to re-calibrate. (The zero offset
    // is measured at boot in checkProbeCurrentZero(); as long as calibration is
    // run after boot, both sides share the same probe_current_zero.)

    SwitchCalibStep meas = { -1, 0.0f, 0.0f, -1, false };
    SwitchCalibStep sel  = { -1, 0.0f, 0.0f, -1, false };
    bool aborted = false;

    // ===== MEASURE MODE CALIBRATION =====
    b.clear( );
    for (int i = 0; i < 10; i++) {
        showSwitchPosition(i, "Measure", 0x000000, 0x000000);
        delay(50);
    }

    if (jumperlessConfig.top_oled.connection_type == 2 && oled.isConnected() == false) {
        oled.connect();
    }

    oled.showMultiLineSmallText( "Switch to MEASURE\n\r(AWAY from TIP)\n\n\rClick wheel = start", true, true );

    Serial.println( "\n\rStep 1: MEASURE mode (switch away from tip)" );
    Serial.println( "Move the probe switch AWAY from the TIP" );
    Serial.println( "Press the clickwheel when ready\n\r" );
    Serial.flush( );

    if ( !switchCalibWaitClick( "Switch to MEASURE\n\r(AWAY from TIP)\n\n\rClick wheel = start" ) ) {
        aborted = true;
        goto done;
    }
    meas = switchCalibReadPosition( 0, 3, 22 );
    delay( 1000 );

    // ===== SELECT MODE CALIBRATION =====
    b.clear( );
    for (int i = 10; i >= 0; i--) {
        showSwitchPosition(i, "Select", 0x000000, 0x000510);
        delay(50);
    }
    oled.showMultiLineSmallText( "Switch to SELECT\n\r(TOWARDS TIP)\n\n\rClick wheel = start", true, true );

    Serial.println( "\n\rStep 2: SELECT mode (switch towards tip)" );
    Serial.println( "Move the probe switch TOWARDS the tip" );
    Serial.println( "Press the clickwheel when ready\n\r" );
    Serial.flush( );

    if ( !switchCalibWaitClick( "Switch to SELECT\n\r(TOWARDS TIP)\n\n\rClick wheel = start" ) ) {
        aborted = true;
        goto done;
    }
    delay( 450 );
    sel = switchCalibReadPosition( 1, 4, 22 );

    {
        float measureAvg = meas.inaMedian;
        float selectAvg  = sel.inaMedian;

        // Tip sense must agree with the position we asked for. If it doesn't
        // the user (or the switch) put the probe somewhere else and every
        // number above describes the wrong position - refuse to save rather
        // than calibrate a lie. (-1 = the detector skipped, e.g. a button held:
        // not a contradiction.)
        bool tipContradicts = ( meas.detA == 1 ) || ( sel.detA == 0 );
        if ( tipContradicts ) {
            changeTerminalColor( 196, true );
            Serial.printf( "\n\rWARNING: the tip sense says the switch was %s during the %s step.\n\r",
                           meas.detA == 1 ? "in SELECT" : "in MEASURE",
                           meas.detA == 1 ? "MEASURE" : "SELECT" );
            Serial.println( "Nothing saved - re-run with the switch where each step asks.\n\r" );
            changeTerminalColor( -1, true );
            oled.showMultiLineSmallText( "Switch was in the\n\rwrong position -\n\rnothing saved", true, true );
            delay( 2500 );
            goto done;
        }

        // Sanity window: MEASURE historically read ~0 mA, but the probe LED's
        // idle draw rides on INA1 in the corrected frame and shifts with the LED
        // wiring (probe_led_on_button_pin boards idle near ~1 mA), so allow up
        // to 1.4 mA there. What actually matters for detection is SEPARATION -
        // no usable gap (or values wildly out of range) means a bad/unplugged
        // probe cable, so fall back to defaults instead of saving garbage.
        bool badReadings = measureAvg < -0.4f || measureAvg > 1.4f ||
                           selectAvg < 1.0f || selectAvg > 3.6f ||
                           ( selectAvg - measureAvg ) < 0.5f;
        if ( badReadings ) {
            changeTerminalColor( 196, true ); // Red
            Serial.printf( "\n\rWARNING: readings out of expected range "
                           "(measured %.3f / %.3f mA, expected ~0.0 / ~1.8 mA)\n\r",
                           measureAvg, selectAvg );
            Serial.println( "Using default thresholds instead - this may mean a bad probe cable!\n\r" );
            changeTerminalColor( -1, true );
            oled.showMultiLineSmallText( "Bad readings!\n\rUsing defaults\n\r(check probe cable)", true, true );
            delay( 2000 );
            measureAvg = 0.0f;
            selectAvg = 1.8f;
        }

        // Clamp the zero-corrected averages to >= 0. In the calibrated frame the
        // MEASURE-mode current sits at ~0 and INA noise can push the average
        // slightly negative; clamping keeps the midpoint/threshold math well-behaved.
        if (measureAvg < 0.0){
            measureAvg = 0.0;
        }
        if (selectAvg < 0.0){
            selectAvg = 0.0;
        }
        // ===== CALCULATE THRESHOLDS =====
        // Set thresholds with hysteresis in the middle of the two averages
        float midpoint = ( measureAvg + selectAvg ) / 2.0;
        float range = fabs( selectAvg - measureAvg );
        float buffer = range * 0.05; // 5% buffer

        // Ensure MEASURE mode is always the lower value
        float lowerAvg = min( measureAvg, selectAvg );
        float higherAvg = max( measureAvg, selectAvg );

        // Set thresholds with hysteresis
        jumperlessConfig.calibration.probe_switch_threshold_low = midpoint - buffer;
        jumperlessConfig.calibration.probe_switch_threshold_high = midpoint + buffer;
        // SELECT ceiling for the agreement classifier: anything above the
        // select signature by more than the whole measure/select gap (at
        // least 1 mA) is a touch loading the buffer, not the switch.
        jumperlessConfig.calibration.probe_switch_select_max_ma =
            higherAvg + ( range > 1.0f ? range : 1.0f );

        // GPIO-feed numbers. Unloaded tip in MEASURE seeds the droop model's
        // V0 (only when the tip really was unloaded: sanity band); the two
        // blink percentages must straddle the hold threshold - if they do
        // but the margin is thin, or they don't, put the threshold at their
        // midpoint so the detector is calibrated to THIS probe.
        if ( meas.gpioOk && meas.adc7Median >= 3.0f && meas.adc7Median <= 3.6f ) {
            jumperlessConfig.calibration.probe_droop_v0 = meas.adc7Median;
        }
        if ( meas.blinkPct >= 0 && sel.blinkPct >= 0 && sel.blinkPct > meas.blinkPct + 20 ) {
            jumperlessConfig.calibration.probe_switch_blink_hold_pct =
                ( meas.blinkPct + sel.blinkPct ) / 2;
        }

        Serial.println( "\n\r=== Calibration Results ===" );
        Serial.printf( "MEASURE mode median: %.3f mA   (tip %s)\n\r", lowerAvg,
                       meas.detA == 0 ? "H ok" : "-" );
        Serial.printf( "SELECT mode median:  %.3f mA   (tip %s)\n\r", higherAvg,
                       sel.detA == 1 ? "L ok" : "-" );
        Serial.printf( "Midpoint: %.3f mA\n\r", midpoint );
        Serial.printf( "Range: %.3f mA\n\r", range );
        Serial.printf( "\nLow threshold:  %.3f mA (switch to MEASURE)\n\r",
                       jumperlessConfig.calibration.probe_switch_threshold_low );
        Serial.printf( "High threshold: %.3f mA (switch to SELECT)\n\r",
                       jumperlessConfig.calibration.probe_switch_threshold_high );
        Serial.printf( "Select ceiling: %.3f mA (above = loaded by a touch)\n\r",
                       jumperlessConfig.calibration.probe_switch_select_max_ma );
        if ( meas.gpioOk || sel.gpioOk ) {
            Serial.printf( "GPIO feed: tip %.3f V unloaded / %.3f V select, blink held %d%% / %d%% -> hold threshold %d%%\n\r",
                           meas.adc7Median, sel.adc7Median, meas.blinkPct, sel.blinkPct,
                           jumperlessConfig.calibration.probe_switch_blink_hold_pct );
        }
        Serial.println( "\n\rSaving configuration..." );

        char oledBuffer[ 64 ] = "Calibration Done!\n\n\r";
        snprintf( oledBuffer + strlen( oledBuffer ), sizeof( oledBuffer ) - strlen( oledBuffer ), "Low:  %.2f mA\n", jumperlessConfig.calibration.probe_switch_threshold_low );
        snprintf( oledBuffer + strlen( oledBuffer ), sizeof( oledBuffer ) - strlen( oledBuffer ), "High: %.2f mA", jumperlessConfig.calibration.probe_switch_threshold_high );
        oled.showMultiLineSmallText( oledBuffer, true, true );

        // Show success on breadboard
        b.clear( );
        b.print( "Done!", 0x002000, 0x000000, 1, -1, -1 );

        // Back to automatic candidate selection (GPIO reclaims if DAC0 is
        // claimed) before the config save.
        infraForceCandidate( "probe_power", -1 );
        saveConfig( );
        delay( 2000 );
    }

done:
    // Every exit passes here: never leave the feed pinned.
    infraForceCandidate( "probe_power", -1 );
    if ( aborted ) {
        Serial.println( "\n\rSwitch calibration aborted - nothing saved." );
        oled.showMultiLineSmallText( "Calibration\n\raborted", true, true );
        delay( 800 );
    }
    leaveApp( );
}


void testSwitchThresholds( void ) {
    float thresholdLow = jumperlessConfig.calibration.probe_switch_threshold_low;
    float thresholdHigh = jumperlessConfig.calibration.probe_switch_threshold_high;



    Serial.printf( "Threshold Low: %.3f mA\n\r", thresholdLow );
    Serial.printf( "Threshold High: %.3f mA\n\r", thresholdHigh );
    Serial.flush( );
}

// Pin the probe feed to one candidate and let the rebuild land, so a
// calibration phase measures the source it says it is measuring. Same recipe
// the switch-threshold app and the self test's droop phase use. candIdx: 0 =
// DAC0, 1 = GPIO scan, -1 = back to automatic.
static void probeCalibForceFeed( int candIdx ) {
    infraForceCandidate( "probe_power", candIdx );
    refreshConnections( -1, 0, 0 );
    waitCore2( );
    delay( 30 );
}

// Switch position for the calibration app, from the TIP SENSE rather than
// the runtime classifier. The classifier is gated to 500ms and reads either
// INA1 (needs ~17ms conversion windows) or the ADC7 droop model - and this
// app deliberately thrashes the feed during convergence, which disturbs
// both and wanders the tip ~70mV per rebuild. The tip sense is ~6us, needs
// no ADC or I2C, and does not care which source is feeding the buffer, so
// it is the only position source that stays honest in here. Two agreeing
// reads to debounce; falls back to the runtime position when it skips
// (a probe button is held).
static int probeCalibSwitchPos( int lastPos ) {
    int a = probeSwitchTipSenseNow( );
    if ( a < 0 ) return lastPos >= 0 ? lastPos : Probing::getInstance( ).switchPosition;
    static int pending = -1;
    static int pendingCount = 0;
    if ( a == lastPos ) {
        pending = -1;
        pendingCount = 0;
        return lastPos;
    }
    if ( pending == a ) {
        pendingCount++;
    } else {
        pending = a;
        pendingCount = 1;
    }
    if ( pendingCount >= 2 ) {
        pending = -1;
        pendingCount = 0;
        return a;
    }
    return lastPos >= 0 ? lastPos : a;
}

void probeCalibApp( void ) {
    b.clear( );

    cycleTerminalColor( true, 5.0, true );
    Serial.println( "Probe Calibration App" );
    cycleTerminalColor( );
    Serial.println( "\n\rTap rows with the probe and rotate the clickweel until they're lighting up the correct row" );
    cycleTerminalColor( );
    Serial.println( "be sure to check nano header rows too" );
    cycleTerminalColor( );
    Serial.println( "SELECT position calibrates probe_max. The feed is pinned to DAC0 there so" );
    Serial.println( "INA1 can measure the switch (probe LED supply) current - shown as sw:." );
    cycleTerminalColor( );
    Serial.println( "MEASURE position: hold the tip on ONE pad and the app flips the feed" );
    Serial.println( "between DAC0 and a GPIO, auto-trimming probe_max_measure_gpio until both" );
    Serial.println( "feeds decode the SAME row (the drive voltage cancels out of the decode;" );
    Serial.println( "the two feeds' source impedance does not, which is what this removes)." );
    Serial.println( "Once converged the wheel moves BOTH endpoints together to align the row." );
    Serial.println( "Short-click re-runs the convergence." );
    cycleTerminalColor( );
    Serial.println( "Hold the clickwheel when you're done\n\n\r" );
    cycleTerminalColor( );

    refreshConnections( -1, 0 );
    routableBufferPower( 1, 1, 1 );

    // if (jumperlessConfig.top_oled.connection_type == 2 && oled.isConnected() == false) {
        oled.connect();

    // }

    oled.showMultiLineSmallText( "Tap pads + rotate wheel to align both switch positions\n\rmeasure: hold one pad to converge feeds\n\rhold click = save", true, true );
    // oled.showMultiLineSmallText("be sure to check nano header rows too\n\r", false, true);
    // oled.showMultiLineSmallText("Hold the clickwheel when you're done\n\r", false, true);
    oled.flushFramebuffer( );
    // calibrateProbeSwitchThresholds();

    // OLEDOut.println("Test 1: PASS");
    // OLEDOut.println("Test 2: PASS");
    // OLEDOut.println("All tests OK!");

    int finishCountdown = -1;
    unsigned long finishCountdownTimer = 0;
    bool done = false;
    
    SlotManager::getInstance( ).enterTemporarySlot( 8 );  // Save current slot, switch to temp slot 8


    resetEncoderPosition = true;
    int lastEncoderPosition = encoderPosition;
    int reading = -1;
    int lastReading = -1;

    int probeMax = jumperlessConfig.calibration.probe_max;
    // Measure-position endpoint (3.3V frame; the decode scales it by the
    // live ADC7 tip voltage). The old app nudged measure_mode_output_voltage
    // to align measure-mode rows - the ratiometric decode now cancels a
    // drive-voltage change (and under debug.probe_power_gpio the DAC isn't
    // even driving the tip), so the knob adjusts probe_max_measure directly.
    int probeMaxMeasure = jumperlessConfig.calibration.probe_max_measure;

    // MEASURE has two calibrations because the tip is driven by two very
    // different sources (dacs.probe_power_source picks which one the runtime
    // prefers, and either can end up live):
    //   phase 0 - feed = DAC0: a stiff ~2-crosspoint source. The knob here is
    //             the DAC drive itself, calibration.measure_mode_output_voltage.
    //   phase 1 - feed = GPIO: ~183 ohm through 4 crosspoints, so the drive is
    //             fixed 3.3V logic minus droop and the only knob is the map
    //             endpoint, calibration.probe_max_measure.
    // The feed is FORCED per phase so each calibration measures its own source
    // instead of whatever the arbitration happened to pick. Unforced on exit.
    // Convergence state. The tip drive VOLTAGE cancels out of the decode
    // (ratiometric: endpoints x live ADC7 / 3.3 - hardware-confirmed), so it
    // is left fixed at the servo'd measure_mode_output_voltage and is NOT a
    // knob here. What does not cancel is the two feeds' source impedance
    // (DAC0 ~2 crosspoints and stiff; a routable GPIO ~170-185 ohm through 4,
    // sagging under the pad ladder). So each feed has its own top endpoint
    // and this app trims the GPIO one until, on the SAME held pad, both feeds
    // decode the same row. Then the wheel moves both together.
    int convFeed = 0;          // which feed the next sample takes (0 = DAC0, 1 = GPIO)
    int appliedFeed = -1;      // feed currently forced (-1 = automatic)
    float convNormDac = -1.0f; // normalized pad position measured under DAC0
    int convRowDac = -1;
    int convRowGpio = -1;
    int convStable = 0;        // consecutive alternations agreeing on the row
    bool converged = false;
    bool gpioFeedAvailable = true;
    unsigned long convNextMs = 0;
    int probeMaxMeasureGpio = jumperlessConfig.calibration.probe_max_measure_gpio;
    // Switch (probe LED supply) current, zero-corrected. Only meaningful
    // while the feed is DAC0 - INA1's shunt R57 sits in DAC0's output path
    // and is blind to a GPIO feed - so it is sampled in SELECT, where this
    // app pins DAC0 for exactly that reason, and left at NAN otherwise.
    float switchCurrent_mA = NAN;
    unsigned long switchCurrentNextMs = 0;

    int nodeSelected = -1;
    int lastNodeSelected = -1;

    int nodeSelectedWithOldMapping = -1;
    int lastNodeSelectedWithOldMapping = -1;

    int lastValidProbeRead = -1;
    int lastSwitchPosition = -1;

    int measureOrSelect = 0;

    b.printRawRow( 0b00011110, 0, 0x100005, 0x000000 );
    b.printRawRow( 0b00011000, 1, 0x100005, 0x000000 );
    b.printRawRow( 0b00010100, 2, 0x100005, 0x000000 );
    b.printRawRow( 0b00010010, 3, 0x100005, 0x000000 );
    b.printRawRow( 0b00000010, 4, 0x100005, 0x000000 );
    b.printRawRow( 0b00000100, 5, 0x100005, 0x000000 );
    b.printRawRow( 0b00000000, 6, 0x100005, 0x000000 );

    b.print( "Adjust", 0x100005, 0x000000, 1, 0, 1 );

    b.printRawRow( 0b00010000, 50, 0x000510, 0x000000 );
    b.printRawRow( 0b00010000, 51, 0x000510, 0x000000 );
    b.printRawRow( 0b00010100, 52, 0x000510, 0x000000 );
    b.printRawRow( 0b00010010, 53, 0x000510, 0x000000 );
    b.printRawRow( 0b00001111, 54, 0x000510, 0x000000 );
    b.printRawRow( 0b00000010, 55, 0x000510, 0x000000 );
    b.printRawRow( 0b00000100, 56, 0x000510, 0x000000 );

    b.print( "Tap", 0x000510, 0x000000, 0, 1, 3 );
    requestLedShow( 2 );
    delay( 200 );
    // b.clear( );
    bool touched = false;

    int probeRead = -1;
    bool firstRead = true;
    Probing::getInstance( ).smoothProbeReading( -1, true );
    // while (probeRead == -1) {
    //     probeRead = readProbeRaw( 0, true );

    // }

    while ( done == false ) {
        if ( oledCalibHotplugPoll( ) ) {
            oled.showMultiLineSmallText( "Tap pads + rotate wheel to align both switch positions\n\rmeasure: hold one pad to converge feeds\n\rhold click = save", true, true );
        }
        probeRead = readProbeRaw( 0, true );

        if ( probeRead != -1 ) {
            lastValidProbeRead = Probing::getInstance( ).smoothProbeReading( probeRead );
            if ( !touched ) {
                touched = true;
                finishCountdown = 0;
                finishCountdownTimer = millis( );
                // b.clear( );
            }
        }

        // Update the switch position FIRST: probeMapRange() keys off it, and
        // measure/select decode with different endpoint pairs - mapping with
        // last loop's position would mis-map rows for a frame after a flip.
        int checkSwitch = probeCalibSwitchPos( lastSwitchPosition );
        // probeMapRange() - the decode this app is calibrating - picks its
        // endpoint pair from the GLOBAL switchPosition, and the runtime
        // ProbeSwitch service keeps writing that from the current/droop
        // classifier this app's feed thrashing disturbs. Re-assert the tip
        // sense's answer every pass so what the app displays and what the
        // decode uses can never disagree.
        if ( checkSwitch >= 0 ) {
            Probing::getInstance( ).switchPosition = checkSwitch;
        }
        if ( checkSwitch != lastSwitchPosition ) {
            if ( checkSwitch == 0 ) {
                lastSwitchPosition = 0;
                measureOrSelect = 0;
                probeMaxMeasure = jumperlessConfig.calibration.probe_max_measure;
            } else {
                lastSwitchPosition = 1;
                measureOrSelect = 1;
                probeMax = jumperlessConfig.calibration.probe_max;
            }
            resetEncoderPosition = true;
        }

        // Short click (RELEASED off a PRESSED, the same edge every menu uses)
        // re-runs the convergence. A HOLD still saves+exits; a click in SELECT
        // does nothing - there is only one endpoint there.
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) {
            encoderButtonState = IDLE;
            if ( measureOrSelect == 0 ) {
                converged = false;
                convStable = 0;
                convNormDac = -1.0f;
                convFeed = 0;
                gpioFeedAvailable = true;
                Serial.println( "\n\rre-running feed convergence - hold the tip on one pad" );
            }
        }

        // In SELECT the tip is driven straight from PROBE_PIN, so the feed
        // does not affect the pad reading at all - which frees us to pin it
        // to DAC0 and get the switch current for free: in this position the
        // feed IS the probe LED supply, and INA1's shunt is in DAC0's output
        // path, so that current is the switch signature the thresholds are
        // calibrated against. Pinned on entry to SELECT, not every pass.
        if ( measureOrSelect == 1 && appliedFeed != 0 ) {
            probeCalibForceFeed( 0 );
            appliedFeed = 0;
            switchCurrent_mA = NAN;
            switchCurrentNextMs = millis( ) + 120; // let the LED load settle
            lastReading = -1;
        }
        if ( measureOrSelect == 1 && millis( ) >= switchCurrentNextMs ) {
            switchCurrentNextMs = millis( ) + 100; // bound the I2C traffic
            switchCurrent_mA = checkProbeCurrent( );
        }

        // ---- feed convergence -------------------------------------------
        // One alternation step per loop pass while a pad is held: force a
        // feed, let the rebuild + tip settle, sample, and when both feeds
        // have been sampled solve the GPIO endpoint that puts them on the
        // same row. Each step costs a crossbar rebuild, so it is gated on
        // actually touching something and on a settle interval.
        if ( measureOrSelect == 0 && !converged && gpioFeedAvailable &&
             probeRead != -1 && millis( ) >= convNextMs ) {
            int want = ( convFeed == 0 ) ? 0 : 1;
            if ( appliedFeed != want ) {
                probeCalibForceFeed( want );
                appliedFeed = want;
                if ( want == 1 && infraProbePowerGpioIdx( ) < 0 ) {
                    // Nothing to converge against - every routable GPIO is
                    // claimed. Say so once, park on DAC0 and let the wheel
                    // work on the endpoints we can still set.
                    Serial.println( "\n\rno free routable GPIO - convergence skipped, DAC endpoint only" );
                    if ( oled.isConnected( ) ) {
                        oled.showMultiLineSmallText( "No free GPIO -\n\rconvergence\n\rskipped", true, true );
                    }
                    gpioFeedAvailable = false;
                    probeCalibForceFeed( 0 );
                    appliedFeed = 0;
                }
            }
            if ( gpioFeedAvailable ) {
                // Median of 3 raw reads: a rebuild wanders the tip for a
                // while afterwards (~70mV, measured), so a single sample
                // here would chase that instead of the feed difference.
                int samples[ 3 ];
                int got = 0;
                for ( int i = 0; i < 3; i++ ) {
                    int r = readProbeRaw( 0, true );
                    if ( r != -1 ) samples[ got++ ] = r;
                    delay( 2 );
                }
                if ( got > 0 ) {
                    if ( got == 3 ) {
                        // tiny 3-element sort
                        if ( samples[ 0 ] > samples[ 1 ] ) std::swap( samples[ 0 ], samples[ 1 ] );
                        if ( samples[ 1 ] > samples[ 2 ] ) std::swap( samples[ 1 ], samples[ 2 ] );
                        if ( samples[ 0 ] > samples[ 1 ] ) std::swap( samples[ 0 ], samples[ 1 ] );
                    }
                    int rawNow = samples[ got / 2 ];
                    int cMin, cMax;
                    probeMapRange( &cMin, &cMax );
                    int rowNow = map( rawNow, cMin, cMax, 101, 0 );
                    float span = (float)( cMax - cMin );
                    float norm = ( span != 0.0f ) ? ( (float)( rawNow - cMin ) / span ) : -1.0f;

                    if ( convFeed == 0 ) {
                        convNormDac = norm;
                        convRowDac = rowNow;
                        convFeed = 1;
                    } else {
                        convRowGpio = rowNow;
                        // Solve the GPIO endpoint that reproduces the DAC
                        // feed's normalized position at this same pad:
                        //   norm = (raw - min) / (max - min)  ->
                        //   max = min + (raw - min) / norm
                        // then undo the live ratiometric scale to store it.
                        if ( convNormDac > 0.02f && norm > 0.0f ) {
                            // Recover the live ratiometric scale from the TOP
                            // endpoint, not the bottom: probeMapRange returns
                            // ints, and probe_min_measure is ~10 counts, so
                            // cMin/mMin quantises to 0.9 or 1.0 - a 10% error
                            // straight into the solved endpoint. cMax is ~4000,
                            // where the same rounding is negligible.
                            int usedMax = jumperlessConfig.calibration.probe_max_measure_gpio;
                            float scale = ( usedMax > 0 ) ? ( (float)cMax / (float)usedMax ) : 1.0f;
                            if ( scale < 0.5f || scale > 1.5f ) scale = 1.0f;
                            float wantMaxScaled =
                                (float)cMin + ( (float)( rawNow - cMin ) / convNormDac );
                            int wantMax = (int)( wantMaxScaled / scale + 0.5f );
                            // Damp: move a third of the way each alternation so
                            // one noisy pair cannot throw the endpoint.
                            int cur = jumperlessConfig.calibration.probe_max_measure_gpio;
                            int next = cur + ( wantMax - cur ) / 3;
                            if ( next == cur && wantMax != cur ) next = cur + ( wantMax > cur ? 1 : -1 );
                            if ( next < 15 ) next = 15;
                            if ( next > 4095 ) next = 4095;
                            jumperlessConfig.calibration.probe_max_measure_gpio = next;
                            probeMaxMeasureGpio = next;
                        }
                        if ( convRowDac == convRowGpio ) {
                            convStable++;
                            if ( convStable >= 3 ) {
                                converged = true;
                                Serial.printf( "\n\rfeeds converged on row %d: DAC max %d / GPIO max %d\n\r"
                                               "now turn the wheel to align the row (moves both)\n\r",
                                               convRowDac,
                                               jumperlessConfig.calibration.probe_max_measure,
                                               jumperlessConfig.calibration.probe_max_measure_gpio );
                                // Leave the runtime's own preference live now
                                // that the endpoints agree.
                                probeCalibForceFeed( -1 );
                                appliedFeed = -1;
                            }
                        } else {
                            convStable = 0;
                        }
                        convFeed = 0;
                    }
                    lastReading = -1; // repaint with the new endpoints
                }
                convNextMs = millis( ) + 40;
            }
        }

        // Decode with the SAME mode-aware endpoints the runtime uses (base
        // pair in select, ADC7-scaled measure pair in measure), so what
        // lights up here is exactly what normal probing will decode.
        int mapMin, mapMax;
        probeMapRange( &mapMin, &mapMax );
        int rowProbed = map( lastValidProbeRead, mapMin, mapMax, 101, 0 );
        int rowProbedWithOldMapping = map( lastValidProbeRead, jumperlessConfig.calibration.probe_min, probeMax, 101, 0 );

        nodeSelected = probeRowMap[ rowProbed ];
        nodeSelectedWithOldMapping = probeRowMapByPad[ rowProbedWithOldMapping ];
        if ( probeRead != -1 ) {

            reading = rowProbed;
            if (firstRead) {
                firstRead = false;
                
            }
        }

        if ( encoderPosition != lastEncoderPosition || reading != lastReading ) {
            lastEncoderPosition = encoderPosition;
            if ( measureOrSelect == 0 ) {
                // Both measure endpoints move TOGETHER: convergence has already
                // removed the difference between the feeds, so aligning the row
                // is one shared adjustment. Moving only one would re-open the
                // gap the convergence just closed.
                int deltaMax = encoderPosition;
                int newDac = probeMaxMeasure - deltaMax;
                int newGpio = probeMaxMeasureGpio - deltaMax;
                if ( newDac < 15 ) newDac = 15;
                if ( newGpio < 15 ) newGpio = 15;
                jumperlessConfig.calibration.probe_max_measure = newDac;
                jumperlessConfig.calibration.probe_max_measure_gpio = newGpio;
            } else {
                jumperlessConfig.calibration.probe_max = probeMax - encoderPosition;
                if ( jumperlessConfig.calibration.probe_max < 15 ) {
                    jumperlessConfig.calibration.probe_max = 15;
                }
            }
            char debugOutput[100];
            if (measureOrSelect == 0) {
                snprintf(debugOutput, sizeof(debugOutput), "MEASURE %s\n\rread: %d\n\rnode: %s\n\rD%d G%d",
                           converged ? "ok" : ( gpioFeedAvailable ? "conv.." : "1feed" ),
                           rowProbed,
                           definesToChar( nodeSelected ),
                           jumperlessConfig.calibration.probe_max_measure,
                           jumperlessConfig.calibration.probe_max_measure_gpio );
            } else {
                snprintf(debugOutput, sizeof(debugOutput), "SELECT   raw: %d\n\rread: %d\n\rnode: %s\n\rmax: %d sw:%.2fmA",
                           lastValidProbeRead, rowProbed,
                           definesToChar( nodeSelected ),  
                           jumperlessConfig.calibration.probe_max,
                           isnan( switchCurrent_mA ) ? 0.0 : (double)switchCurrent_mA );
            }
            if (firstRead == false) {
                if (oled.isConnected()) {
                    oled.showMultiLineSmallText(debugOutput, true, true);
                }
            }



            // \r first (return to column 0), then content, then EL (\033[K) to
            // wipe leftover chars from a longer previous line. Spaces-before-\r
            // only extends the line rightward and never clears it.
            if ( measureOrSelect == 0 ) {
                Serial.printf( "\rraw: %d enc: %d reading: %d maxD: %d maxG: %d node: %s measure %s\033[K",
                               lastValidProbeRead, encoderPosition, rowProbed,
                               jumperlessConfig.calibration.probe_max_measure,
                               jumperlessConfig.calibration.probe_max_measure_gpio,
                               definesToChar( nodeSelected ),
                               converged ? "(converged - wheel moves both)"
                                         : ( gpioFeedAvailable
                                                 ? "(converging: hold one pad)"
                                                 : "(no free GPIO)" ) );
            } else {
                if ( isnan( switchCurrent_mA ) ) {
                    Serial.printf( "\rraw: %d enc: %d reading: %d max: %d node: %s select sw: --  \033[K",
                                   lastValidProbeRead, encoderPosition, rowProbed,
                                   jumperlessConfig.calibration.probe_max,
                                   definesToChar( nodeSelected ) );
                } else {
                    Serial.printf( "\rraw: %d enc: %d reading: %d max: %d node: %s select sw: %.2f mA (thr %.2f/%.2f)\033[K",
                                   lastValidProbeRead, encoderPosition, rowProbed,
                                   jumperlessConfig.calibration.probe_max,
                                   definesToChar( nodeSelected ),
                                   (double)switchCurrent_mA,
                                   (double)jumperlessConfig.calibration.probe_switch_threshold_low,
                                   (double)jumperlessConfig.calibration.probe_switch_threshold_high );
                }
            }
            Serial.flush( );
        }

        // if ( reading == -1 )
        //     continue;

        if ( reading != lastReading && reading != -1 ) {

            // Serial.println( "reading: " + String( reading ) );
            // Serial.flush( );
            uint32_t modeColor = measureOrSelect == 0 ? 0x200010 : 0x001030;
            uint32_t modeLogoColor = measureOrSelect == 0 ? 0xa00060 : 0x3080f0;
            clearLEDsExceptRails( );
            clearColorOverrides( true, true, true );
            if ( nodeSelected >= LOGO_PAD_TOP ) {
                // Serial.print( "Node selected: " );
                // Serial.println( nodeSelected );
                // Serial.flush( );

                switch ( nodeSelected ) {
                case ADC_PAD:

                    setLogoOverride( ADC_0, modeLogoColor );
                    setLogoOverride( ADC_1, modeLogoColor );
                    break;
                case DAC_PAD:
                    setLogoOverride( DAC_0, modeLogoColor );
                    setLogoOverride( DAC_1, modeLogoColor );
                    break;
                case GPIO_PAD:
                    setLogoOverride( GPIO_0, modeLogoColor );
                    setLogoOverride( GPIO_1, modeLogoColor );
                    break;
                case LOGO_PAD_TOP:
                    setLogoOverride( LOGO_TOP, modeLogoColor );
                    break;
                case LOGO_PAD_BOTTOM:
                    setLogoOverride( LOGO_BOTTOM, modeLogoColor );
                    break;
                case BUILDING_PAD_TOP:
                    modeLogoColor = measureOrSelect == 0 ? 0x905000 : 0x008090;
                    setLogoOverride( LOGO_TOP, modeLogoColor );
                    break;
                case BUILDING_PAD_BOTTOM:
                    modeLogoColor = measureOrSelect == 0 ? 0x905000 : 0x008090;
                    setLogoOverride( LOGO_BOTTOM, modeLogoColor );
                    break;
                }
                requestLedShow( 2 );
            }

            if ( nodeSelected != nodeSelectedWithOldMapping && measureOrSelect == 1 ) {
                b.lightUpNode( nodeSelectedWithOldMapping, 0x050205 );
            }
            if ( measureOrSelect == 0 ) {
                b.lightUpNode( nodeSelected, modeColor );
            } else {
                b.lightUpNode( nodeSelected, modeColor );
            }
        }
        lastReading = reading;
        lastNodeSelected = nodeSelected;
        lastNodeSelectedWithOldMapping = nodeSelectedWithOldMapping;

        // Serial.println( "lastButtonEncoderState: " + String( lastButtonEncoderState ) );

        // Serial.println( "encoderButtonState: " + String( encoderButtonState ) );

  

       
            if ( millis( ) - finishCountdownTimer > 30 ) {
                int countup = 0;
                if ( ( encoderButtonState == 4 && lastButtonEncoderState == 4 ) || ( encoderButtonState == 2 && lastButtonEncoderState == 2 ) || encoderButtonState >= 4 ) {
                    
                finishCountdown++;
                countup = 1;
                } else {
                    finishCountdown--;
                    countup = 0;
                    if (finishCountdown < 0) {
                        finishCountdown = -1;
                    }
                }
                finishCountdownTimer = millis( );
               
                switch ( finishCountdown ) {
                // case 0:
                //     if (countup == 0) {
                //         //setLogoOverride( GPIO_1, -3);
                //     }

                //  //   setLogoOverride( GPIO_0, 0xa0a0f0 );

                //     requestLedShow( 2 );
                //     break;
                // case 1:
                //     if (countup == 0) {
                //        // setLogoOverride( DAC_0, -3);
                //     }
                //    // setLogoOverride( GPIO_1, 0xa0a0f0 );

                //     requestLedShow( 2 );
                //     break;
                // case 2:
                //     if (countup == 0) {
                //        // setLogoOverride( DAC_1, -3);
                //     }

                //    // setLogoOverride( DAC_0, 0xa0a0f0 );

                //     requestLedShow( 2 );
                //     break;
                // case 3:
                //     if (countup == 0) {
                //         //setLogoOverride( ADC_0, -3);
                //     }
                //     //setLogoOverride( DAC_1, 0xa0a0f0 );

                //     requestLedShow( 2 );
                //     break;
                // case 4:
                //     if (countup == 0) {
                //         //setLogoOverride( ADC_1, -3);
                //     }
                //     //setLogoOverride( ADC_0, 0xa0a0f0 );

                //     requestLedShow( 2 );
                //     break;
                // case 5:
                //     //setLogoOverride( ADC_1, 0xa0a0f0 );

                //     requestLedShow( 2 );
                //     break;
                case 0 ... 6:
                    clearColorOverrides( true, true, true );
                    done = true;
                    break;
                default:
                    if (countup == 0) {
                       
                        //setLogoOverride( GPIO_0, -3);
       
                    }
                    
                    break;
                }
            
        } 

            
        

        if ( done ) {
            // int timeout = 1200;
            // uint32_t startTime = millis();

            oled.checkConnection(true);
            if (oled.isConnected()) {
                oled.showMultiLineSmallText( "Probe calibration\n\rsaved!\n\rReturning to menu", true, true );
                Serial.println( "\n\rProbe calibration saved!\n\rReturning to menu" );
            }
            

            // Never leave the feed pinned: a forced candidate survives until
            // something else unforces it, and the runtime would keep using the
            // calibration phase's source forever.
            probeCalibForceFeed( -1 );

            Serial.println( "\n\n\r" );
            Serial.printf( "measure (DAC0 feed): probe_max_measure      = %d\n\r",
                           jumperlessConfig.calibration.probe_max_measure );
            Serial.printf( "measure (GPIO feed): probe_max_measure_gpio = %d%s\n\r",
                           jumperlessConfig.calibration.probe_max_measure_gpio,
                           gpioFeedAvailable ? ( converged ? "  (converged)" : "  (NOT converged)" )
                                             : "  (no free GPIO - unchanged)" );
            Serial.printf( "select:              probe_max              = %d\n\r",
                           jumperlessConfig.calibration.probe_max );
            if ( !isnan( switchCurrent_mA ) ) {
                Serial.printf( "select switch current (DAC0 feed, INA1): %.2f mA   thresholds %.2f / %.2f\n\r",
                               (double)switchCurrent_mA,
                               (double)jumperlessConfig.calibration.probe_switch_threshold_low,
                               (double)jumperlessConfig.calibration.probe_switch_threshold_high );
                if ( switchCurrent_mA <= jumperlessConfig.calibration.probe_switch_threshold_high ) {
                    Serial.println( "  NOTE: that is not above the SELECT threshold - run Switch Calib." );
                }
            }
            Serial.printf( "tip drive (fixed):   measure_mode_output_voltage = %.3f V\n\r",
                           (double)jumperlessConfig.calibration.measure_mode_output_voltage );
            Serial.println( "Saving config..." );

            saveConfig( );
            delay( 1200 );
            if (oled.isConnected()) {
                oled.showJogo32h();
            }

            leaveApp( );
        }
    }
}
void customApp( void ) {
    leds.clear( );
    b.clear( );
    delay( 1000 );
    b.print( "This isa demo", (uint32_t)0x002008 );
    requestLedShow( 2 );
    delay( 2000 );
    b.clear( );
    b.print( "It will show", (uint32_t)0x002008 );
    delay( 1000 );
    b.clear( );
    b.print( "random stuff", (uint32_t)0x002008 );
    delay( 1000 );
    b.clear( );
    b.print( "look atthe FW", (uint32_t)0x002008 );
    delay( 1000 );

    SlotManager::getInstance( ).enterTemporarySlot( 8 );  // Save current slot, switch to temp slot 8
    int leave = 0;

    addBridgeToState( 12, 25 );
    addBridgeToState( TOP_RAIL, 52 );
    refreshConnections( -1, 1 );

    setTopRail( (float)3.3 );

    addBridgeToState( RP_GPIO_1, 42 );
    addBridgeToState( RP_GPIO_2, 42 );
    refreshConnections( -1, 0 );

    pinMode( GPIO_1_PIN, OUTPUT );
    pinMode( GPIO_2_PIN, INPUT );

    digitalWrite( GPIO_1_PIN, HIGH );
    int reading = digitalRead( GPIO_2_PIN );
    Serial.print( "GPIO 2 Reading: " );
    Serial.println( reading );
    delay( 10 );

    digitalWrite( GPIO_1_PIN, LOW );
    reading = digitalRead( GPIO_2_PIN );
    Serial.print( "GPIO_2_PIN: " );
    Serial.println( reading );

    removeBridgeFromState( RP_GPIO_1, 42 );
    removeBridgeFromState( RP_GPIO_2, 42 );
    saveStateToSlot( );
    refreshConnections( -1, 0 );

    setDacByNumber( 1, 5.35 );
    addBridgeToState( DAC1, 20 );
    saveStateToSlot( );
    refreshConnections( -1, 0 );
    delay( 10 );

    addBridgeToState( ADC0, 20 );
    saveStateToSlot( );
    refreshConnections( -1, 0 );
    waitCore2( );

    float voltage = readAdcVoltage( 0, 8 );
    Serial.print( "\n\rADC0: " );
    Serial.println( voltage );

    if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
        leaveApp( );
        return;
    }

    listNets( );
    listSpecialNets( );
    Serial.println( "\n\n\r" );
    printPathsCompact( );
    Serial.println( "\n\n\n\n\r" );

    removeBridgeFromState( ADC0, 20 );
    removeBridgeFromState( DAC1, 20 );
    saveStateToSlot( );
    refreshConnections( -1, 0 );

    addBridgeToState( ISENSE_PLUS, 9 );
    addBridgeToState( ISENSE_MINUS, 28 );
    saveStateToSlot( );
    refreshConnections( );

    float current_ma = INA0.getCurrent_mA( );

    Serial.print( "current between row 19 and 28: " );
    Serial.print( current_ma );
    Serial.println( " mA\n\r" );

    removeBridgeFromState( ISENSE_PLUS, 9 );
    removeBridgeFromState( ISENSE_MINUS, 28 );
    saveStateToSlot( );
    refreshConnections( -1, 0 );
    delay( 1 );

    if ( ( encoderButtonState == PRESSED && lastButtonEncoderState == RELEASED ) || Serial.available( ) > 0 ) {
        leaveApp( );
        return;
    }

    setTopRail( 4.20F );
    addBridgeToState( TOP_RAIL, ISENSE_PLUS );
    saveStateToSlot( );
    refreshConnections( -1, 0 );
    waitCore2( );
    delay( 10 );
    float voltage2 = INA0.getBusVoltage( );
    Serial.print( "top rail voltage (from INA219): " );
    Serial.print( voltage2, 4 );
    Serial.println( " V\n\r" );

    removeBridgeFromState( TOP_RAIL, ISENSE_PLUS );
    saveStateToSlot( );
    refreshConnections( -1, 0 );

    float probeVoltage = readAdcVoltage( 7, 8 );
    Serial.print( "Probe voltage: " );
    Serial.print( probeVoltage, 4 );
    Serial.println( " V\n\r" );

    float probeCurrent = checkProbeCurrent( );
    Serial.print( "Probe current: " );
    Serial.print( probeCurrent, 2 );
    Serial.println( " mA\n\r" );

    if ( ( encoderButtonState == PRESSED && lastButtonEncoderState == RELEASED ) || Serial.available( ) > 0 ) {
        leaveApp( );
        return;
    }

    requestLedShow( -3 );
    b.clear( );
    clearLEDsExceptRails( );

    b.printRawRow( 0b00001, 55, 0x170010, 0xffffff );
    b.printRawRow( 0b00011, 56, 0x150012, 0xffffff );
    b.printRawRow( 0b00111, 57, 0x130013, 0xffffff );
    b.printRawRow( 0b01111, 58, 0x100015, 0xffffff );
    b.printRawRow( 0b11111, 59, 0x080018, 0xffffff );

    // Friendlier text
    for ( uint8_t i = 0; i < 254; i++ ) {
        hsvColor hsvTextColor = { i, 255, 30 };
        rgbColor rgbTextColor = HsvToRgb( hsvTextColor );
        uint32_t textColor = (uint32_t)rgbTextColor.r << 16 |
                             (uint32_t)rgbTextColor.g << 8 |
                             (uint32_t)rgbTextColor.b;
        b.print( "Fuck    you!", (uint32_t)textColor );
        requestLedShow( -3 );
        delayMicroseconds( 200 );
    }

    if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
        leaveApp( );
        return;
    }

    int switchPosition = checkSwitchPosition( );
    Serial.print( "Switch position: " );
    Serial.println( switchPosition == 0 ? "Measure" : "Select" );

    b.clear( );
    b.print( " Tap    Rows!", (uint32_t)0x00140a );
    requestLedShow( -3 );
    delay( 100 );
    requestLedShow( -1 );

    int probeRow = -1;
    int lastProbedRow = 0;

    Serial.println( "Click the probe button to exit\n\n\n\r" );
    // Use state-based button check in loop (doesn't consume events)
    while ( checkProbeButtonState( ) == 0 ) {
        if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
            leaveApp( );
            return;
        }
        probeRow = justReadProbe( );
        if ( probeRow != -1 ) {
            Serial.print( "\r                 \rProbed row: " );
            printNodeOrName( probeRow );
            b.printRawRow( 0b11111, lastProbedRow - 1, 0x000000, 0xffffff );
            b.printRawRow( 0b11111, probeRow - 1, 0x172000, 0xffffff );
            b.print( probeRow, (uint32_t)0x002008 );
            lastProbedRow = probeRow;
            delayMicroseconds( 100 );
        }
    }

    removeBridgeFromState( 12, -1 );
    removeBridgeFromState( 52, -1 );
    refreshConnections( -1, 0 );

    addBridgeToState( 12, 25 );
    refreshLocalConnections( -1, 0 );
    delay( 100 );

    removeBridgeFromState( 12, -1 );
    refreshLocalConnections( -1, 0 );
    delay( 100 );

    if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
        leaveApp( );
        return;
    }

    for ( int i = 1; i <= 31; i++ ) {
        removeBridgeFromState( 1, i - 1 );
        addBridgeToState( 1, i );
        refreshLocalConnections( -1, 0 );
        requestLedShow( -1 );
        waitCore2( );
    }

    removeBridgeFromState( 30, -1 );
    refreshLocalConnections( -1, 0 );

    if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
        leaveApp( );
        return;
    }

    for ( int i = 31; i <= 60; i++ ) {
        removeBridgeFromState( 31, i - 1 );
        addBridgeToState( 31, i );
        refreshConnections( -1, 0 );
        requestLedShow( -1 );
        if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
            leaveApp( );
            return;
        }
    }
    removeBridgeFromState( 60, -1 );
    refreshConnections( -1, 0 );
    delay( 100 );

    sendXYraw( CHIP_K, 4, 0, 1 );
    sendXYraw( CHIP_A, 9, 1, 1 );

    unsigned long startTime = millis( );
    unsigned long timeout = 5000;
    while ( millis( ) - startTime < timeout ) {
        if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
            leaveApp( );
            return;
        }
        sendXYraw( CHIP_K, 4, 0, 1 );
        delayMicroseconds( 100 );
        sendXYraw( CHIP_K, 4, 0, 0 );
        sendXYraw( CHIP_K, 15, 0, 1 );
        delayMicroseconds( 100 );
        sendXYraw( CHIP_K, 15, 0, 0 );
    }

    sendXYraw( CHIP_K, 4, 0, 0 );
    sendXYraw( CHIP_K, 15, 0, 0 );
    sendXYraw( CHIP_A, 9, 1, 0 );
}

void xlsxGui( void ) {
    unsigned long startTime = millis( );
    unsigned long messageTime = startTime;
    unsigned long timeout = 1000000;

    String string_from_UART = "";
    String command = "", args = "";
    String queuedCommand = "  ";
    bool isEchoEnabled = true;
    bool isJumperlessReply = false;

    int DT = 10;

    USBSer1.println( F( "Message,Jumperless GUI started" ) );
    USBSer1.flush( );

    while ( millis( ) - startTime < timeout ) {
        uint32_t currentMillis = millis( );

        if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 ) {
            Serial.println( F( "Exiting XLSX GUI early" ) );
            break;
        }

        if ( currentMillis > messageTime ) {
            messageTime = currentMillis + 1000;
            USBSer1.dtr( );
            USBSer1.print( F( "Message,Jumperless GUI is running..." ) );
            USBSer1.print( F( "\n" ) );
            USBSer1.flush( );
            USBSer1.println( F( "|Message,,Jumperless GUI is running..." ) );
            USBSer1.flush( );
        }

        if ( USBSer1.available( ) ) {
            char c = USBSer1.read( );
            if ( isEchoEnabled ) {
                Serial.print( c );
                Serial.flush( );
            }
            if ( ( c != '\n' ) && ( c != '\r' ) && ( c != '|' ) ) {
                string_from_UART += c;
            } else {
                string_from_UART.trim( );
                if ( string_from_UART.length( ) > 0 ) {
                    delay( DT );
                    int index_of_first_comma = string_from_UART.indexOf( ',' );
                    bool isExcelCommand = false;
                    if ( index_of_first_comma == 0 ) {
                        isExcelCommand = true;
                        string_from_UART = string_from_UART.substring( 1, string_from_UART.length( ) );
                        index_of_first_comma = string_from_UART.indexOf( ',' );
                    }
                    if ( index_of_first_comma != -1 ) {
                        command = string_from_UART.substring( 0, index_of_first_comma );
                        args = string_from_UART.substring( index_of_first_comma + 1 );
                    } else {
                        command = string_from_UART;
                        args = "";
                    }

                    USBSer1.print( F( "Command: " ) );
                    USBSer1.println( command );
                    USBSer1.print( F( "Arguments: " ) );
                    USBSer1.println( args );
                    USBSer1.flush( );

                    if ( command == "h" || command == "help" ) {
                        delay( DT );
                        USBSer1.println( F( "" ) );
                        delay( DT );
                        USBSer1.println( F( "|'h' or 'help',, Prints this list" ) );
                        delay( DT );
                        USBSer1.println( F( "|'echo,<val>' ,, Prints the echo setting; <val> (optional)" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, is 'on' or 'off' to set the echo setting accordingly" ) );
                        delay( DT );
                        USBSer1.println( F( "|'TakeOnMe'   ,, Plays Take On Me from pin 11" ) );
                        delay( DT );
                        USBSer1.println( F( "|'dance'      ,, progresses the 'dance' LED" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, pattern by one step" ) );
                        delay( DT );
                        USBSer1.println( F( "|'send,<val>' ,, Sends a single-character command " ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, to Jumperless (<val> = n, q, or Q)" ) );
                        delay( DT );
                        USBSer1.println( F( "|'P0_?,<val>' ,, Sends <val> as 'f' command to Jumperless; prints" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, an abridged result; then sends 'n' to" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, Jumperless and prints an Excel-compatible result;" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, <val> = '' will clear all connections" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, P0_H/P0_L sets GPIO 0 HIGH/LOW respectively" ) );
                        delay( DT );
                        USBSer1.println( F( "|             ,, TBD..." ) );
                    } else if ( command == "echo" ) {
                        if ( args == "" ) {
                            USBSer1.print( F( "Serial echo is currently " ) );
                            USBSer1.println( isEchoEnabled ? F( "enabled" ) : F( "disabled" ) );
                        } else if ( args == "on" ) {
                            isEchoEnabled = true;
                        } else if ( args == "off" ) {
                            isEchoEnabled = false;
                        } else {
                            USBSer1.println( F( "The echo command only accepts the arguments 'on' and 'off'." ) );
                        }
                        delay( DT );
                    } else {
                        USBSer1.print( F( "Unknown command: " ) );
                        if ( isExcelCommand )
                            USBSer1.print( F( "," ) );
                        USBSer1.println( string_from_UART );
                        delay( DT );
                    }
                    USBSer1.flush( );
                    string_from_UART = "";
                }
            }
        }
    }
    Serial.println( F( "XLSX GUI Done!" ) );
    Serial.flush( );
    USBSer1.println( F( "XLSX GUI Done!" ) );
    USBSer1.flush( );
    return;
}

void bounceStartup( void ) {
    Serial.print( "\n\rPress any key to exit\n\n\r" );
    leds.clear( );
    holdCore1Frames( ); // park core 1 while we own the LED strip

    int bounceDelay = 300;
    resetEncoderPosition = true;
    while ( 1 ) {
        drawAnimatedImage( 0, bounceDelay );
        if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 )
            break;
        delayMicroseconds( bounceDelay );
        drawAnimatedImage( 1, bounceDelay );
        if ( digitalRead( BUTTON_ENC ) == 0 || Serial.available( ) > 0 )
            break;
    }
    releaseCore1Frames( );
    requestLedShow( -1 );
    waitCore2( );
}

void scanBoard( void ) {
    int countLoop = 0;
    int countMult = 18;
    refreshConnections( -1, 0 );

    Serial.println( "\n\n\r" );

    int lastRow = 0;
    int lastFloat = 0;
    int lastNode = 0;
    int leave = 0;

    while ( Serial.available( ) == 0 && leave == 0 ) {
        for ( int i = 1; i < 96; i++ ) {
            if ( i == 84 || i == NANO_RESET_0 || i == NANO_RESET_1 )
                continue;
            if ( ( i > 60 && i < 70 ) || i == 17 )
                continue;

            struct rowLEDs currentRow = getRowLEDdata( i );
            float measuredVoltage = measureVoltage( 2, i, true );

            if ( measuredVoltage == 0xFFFFFFFF ) {
                if ( lastFloat == -1 || i == 1 ) {
                    lastFloat = i;
                    printNodeOrName( i );
                    Serial.print( " - " );
                }
            } else {
                printNodeOrName( lastRow );
                Serial.println( "\tfloating" );
                lastFloat = -1;
                Serial.print( "\t\t\t" );
                int len = printNodeOrName( i );
                for ( int j = 0; j < 3 - len; j++ )
                    Serial.print( " " );
                Serial.print( " = " );
                Serial.print( measuredVoltage );
                Serial.println( " V" );
                Serial.flush( );
            }

            setRowLEDdata( i, currentRow );

            if ( Serial.available( ) > 0 ) {
                leave = 1;
                break;
            }
            if ( encoderButtonState == PRESSED ) {
                leave = 1;
                break;
            }

            requestLedShow( 2 );
            delay( 3 );
            lastRow = i;
        }

        Serial.println( "\r                   \r\n\n\r" );
        Serial.flush( );
        countLoop++;
        if ( countLoop * countMult > 95 ) {
            countLoop = 0;
            countMult -= 2;
        }
        if ( leave == 1 )
            break;
    }
}

int i2cScan( int sdaRow, int sclRow, int sdaPin, int sclPin, int leaveConnections, int internalScan ) {
    // BRIDGE OWNERSHIP (fixed in the H1 wave - it was inverted, and its
    // teardown was unconditional).
    //
    // The routing branch was gated on `internalScan == 0` skipping the add,
    // i.e. bridges were only ever added for the INTERNAL scan - the one that
    // talks to the on-board Wire and needs no breadboard route at all - while
    // the EXTERNAL scan, which is exactly the one that has to get sdaRow/sclRow
    // onto RP 26/27, added nothing. Every caller that passes real rows means
    // the external scan, so the gate is `internalScan != 0` now.
    //
    // The removal tail then ran unconditionally on leaveConnections == 0 with
    // real rows, so it deleted bridges this call had never added - including a
    // project's own committed SDA/SCL wiring, with markDirty behind it. It now
    // removes ONLY what it actually added: addConnection() returns true for a
    // pair that already exists (it just bumps the duplicate count), so "the add
    // succeeded" is not the same question as "the bridge is mine".
    bool addedSda = false, addedScl = false;
    if ( sdaRow < 0 || sclRow < 0 || internalScan != 0 ) {
        Serial.println( "defaulting to \n\n\rGPIO 26 = SDA\n\rGPIO 27 = SCL" );
    } else {
        addedSda = !globalState.hasConnection( RP_GPIO_26, sdaRow ) &&
                   addBridgeToState( RP_GPIO_26, sdaRow ); // SDA
        addedScl = !globalState.hasConnection( RP_GPIO_27, sclRow ) &&
                   addBridgeToState( RP_GPIO_27, sclRow ); // SCL
        if ( addedSda || addedScl ) {
            refreshConnections( -1, 1 );
            waitCore2( );
        }
    }

    TwoWire* WireScan = &Wire1; // default to Wire1
    if ( internalScan == 1 ) {
        WireScan = &Wire; // use Wire for internal scan
    }

    oled.clear( );
    oled.print( "GPIO 26 = SDA\n\r" );
    oled.print( "GPIO 27 = SCL" );
    oled.show( );
    delay( 20 );

    if ( internalScan == 0 ) {
        WireScan->end( );
        WireScan->setSDA( sdaPin );
        WireScan->setSCL( sclPin );
        WireScan->begin( );
        WireScan->setClock( 100000 );
    }
    if ( internalScan == 1 ) {
        Serial.println( "\nScanning internal I2C bus..." );
        Serial.println( "    _0  _1  _2  _3  _4  _5  _6  _7  _8  _9  _A  _B  _C  _D  _E  _F " );
    } else {
        Serial.println( "\nScanning I2C bus..." );
        Serial.println( "    _0  _1  _2  _3  _4  _5  _6  _7  _8  _9  _A  _B  _C  _D  _E  _F " );
    }

    int nDevices = 0;
    uint8_t addressesFound[ 128 ];
    int addressesFoundCount = 0;
    int addressesFoundIndices[ 128 ]; // fixed spelling

    for ( int baseAddr = 0; baseAddr < 128; baseAddr += 16 ) {
        Serial.printf( "\n\r%02X:", baseAddr / 16 );
        for ( int addr = 0; addr < 16; addr++ ) {
            int deviceAddr = baseAddr + addr;
            if ( deviceAddr > 0 && deviceAddr < 127 ) {
                WireScan->beginTransmission( deviceAddr );
                byte error = WireScan->endTransmission( );
                if ( error == 0 ) {
                    Serial.printf( " %02X ", deviceAddr ); // zero-padded
                    nDevices++;
                    addressesFound[ addressesFoundCount ] = deviceAddr;
                    addressesFoundIndices[ addressesFoundCount ] = baseAddr + addr;
                    addressesFoundCount++;
                } else {
                    Serial.print( " -- " );
                }
            } else {
                Serial.print( " -- " );
            }
        }
    }

    Serial.println( "\n\nI2C Scan Results:" );
    if ( nDevices == 0 ) {
        Serial.println( "No I2C devices found" );
        requestLedShow( -3 );
        b.clear( );
        b.print( "No I2C  Found", (uint32_t)0x070003 );
        delayWithButton( 2000 );
        b.clear( );
        requestLedShow( -1 );
    } else {
        Serial.printf( "Found %d I2C device(s)\n", nDevices );
        requestLedShow( -3 );
        b.clear( );
        requestLedShow( -3 );
        if ( addressesFoundCount == 1 ) {
            b.print( "Found", (uint32_t)0x000b01, (uint32_t)0x000000, 0, 0, 3 );
            b.print( addressToHexString( addressesFound[ 0 ] ), (uint32_t)0x000a05, (uint32_t)0x000000, 1, 1 );
        } else if ( addressesFoundCount > 1 ) {
            b.print( addressToHexString( addressesFound[ 0 ] ), (uint32_t)0x000a05, (uint32_t)0x000000, 1, 1 );
            b.print( addressToHexString( addressesFound[ 1 ] ), (uint32_t)0x000808, (uint32_t)0x000000, 1, 1 );
        }
        delayWithButton( 2000 );
        requestLedShow( -1 );
    }

    if ( leaveConnections == 0 && ( addedSda || addedScl ) ) {
        if ( addedSda )
            removeBridgeFromState( RP_GPIO_26, sdaRow );
        if ( addedScl )
            removeBridgeFromState( RP_GPIO_27, sclRow );
        refreshConnections( -1, 1 );
    }

    if ( internalScan == 0 ) {
        WireScan->end( );
        // Wire1 is SHARED with the top OLED (oled.cpp: connection types 0/1/3
        // all sit on I2C1). setSDA/setSCL above moved it onto sdaPin/sclPin and
        // a bare begin() would leave it there, so an OLED on I2C1 goes silent
        // for everything after this scan. Put its pins back before re-begin.
        // Connection type 2 is I2C0 (a different peripheral entirely) and is
        // not ours to touch.
        if ( jumperlessConfig.top_oled.enabled &&
             jumperlessConfig.top_oled.connection_type != 2 ) {
            WireScan->setSDA( jumperlessConfig.top_oled.sda_pin );
            WireScan->setSCL( jumperlessConfig.top_oled.scl_pin );
        }
        WireScan->begin( );
    }
    if ( oled.oledConnected == true ) {
        delay( 500 );
        oled.clear( );
        oled.print( "Found = " );
        oled.setTextSize( 1 );
        if ( addressesFoundCount > 0 )
            oled.print( addressToHexString( addressesFound[ 0 ] ) );
        oled.show( );
    }

    return nDevices;
}

void calibrateDacs( ) {
#if defined(OG_JUMPERLESS)
    Serial.println( "DAC calibration is not supported on Jumperless OG." );
    return;
#endif
    // Calibration solves for ADC constants with set/measure pairs; it needs the
    // real converter, not the USB audio stream's rolling sweep. Yield for the
    // whole run; the stream resumes on exit if the host still has the mic open.
    //
    // If the stream will NOT let go, bail before touching a single DAC. The
    // pump that releases the ADC only runs from core2stuff(), which loop1()
    // skips while a core-1 frame hold is set, so
    // this is reachable - and calibrating anyway would solve every constant
    // from sweep means (a hard 0 on the probe channels) and then PERSIST them.
    UsbAudioAdcYield audioYield( "DAC calibration" );
    if ( !audioYield.ok( ) ) {
        Serial.println( "DAC calibration aborted: the USB audio stream still holds the ADC." );
        Serial.println( "Close the microphone on the host (or run M to disable it) and try again." );
        return;
    }

    // Enter temporary slot FIRST to preserve user's active slot
    SlotManager::getInstance( ).enterTemporarySlot( 8 );  // Save current slot, switch to temp slot 8

    // Park probe power for the whole calibration: the infra evaluation
    // otherwise re-parks DAC1 at measure_mode_output_voltage on every one of
    // this app's many refreshes - IN BETWEEN this code's own set/measure
    // pairs - which corrupts the zero/spread constants being solved for
    // (hardware-observed as DAC1 outputting ~4.4V when told 3.3V).
    bool wasProbePowerOn = infraProbePowerWanted( );
    infraSetProbePowerEnabled( false );
    refreshConnections( -1, 0, 0 ); // flush the feed teardown before measuring
    
    if ( firstStart == 1 ) {
        Serial.println( "\n\rFirst startup calibration\n\n\r" );

        // Ensure routing system is properly initialized after filesystem wipe
        Serial.println( "Initializing routing system for first startup..." );
        Serial.flush( );
        initChipStatus( );                  // Initialize chip mappings based on hardware revision
        globalState.clearAllConnections( ); // Clear temp slot connections
        delay( 100 );                       // Give system time to stabilize
        Serial.println( "Routing system initialized." );
    } else {
        // Serial.println("Calibration");
        globalState.clearAllConnections( ); // Clear temp slot connections
    }
    // delay(3000);
    float setVoltage = 0.0;

    uint32_t dacColors[ 4 ] = { 0x150003, 0x101000, 0x001204, 0x000512 };
    // sendAllPathsCore2 = 1;
    INA0.setBusADC( 0x0e );
    INA1.setBusADC( 0x0e );
    if (jumperlessConfig.top_oled.connection_type == 2 && oled.isConnected() == false) {
        oled.init();

    }
    int failedToConverge = 0;
    // for (int i = 0; i < 4; i++) {

    // Serial.print("netSlot: ");
    // Serial.println(netSlot);
    // printPathsCompact();
    // printChipStatus();
    b.print( "Calib", 0x001010, 0x000000, 0, -1, -2 );
    Serial.println( "\n\r\t\tCalibrating\n\r" );

    Serial.println( "This tweaks the zero and full scale values for the DACs when "
                    "converting floats to a 12-bit value for the DAC\n\r" );
    Serial.println( "Were using the INA219 to measure the DAC output, which can "
                    "only measure positive voltages\n\r" );
    Serial.println(
        "The DAC outputs 0 to 4.096 V, which is scaled and shifted to +-8V\n\r" );
    Serial.println(
        "This is the float voltage to 12-bit DAC formula:\n\n\r\tint dacValue = "
        "(voltage * 4095 / dacSpread[n]) + dacZero[n];\n\n\r" );

    Serial.println( "**UNPLUG ANYTHING CONNECTED TO THE RAILS**\n\n\r" );

    Serial.flush( );
    int skipZeroing = 0;
    if ( skipZeroing == 0 ) {
        for ( int d = 0; d < 4; d++ ) {

            //     removeBridgeFromNodeFile(ISENSE_PLUS, -1, netSlot);
            // removeBridgeFromNodeFile(ADC0+d, -1, netSlot);
            b.clear( );

            b.print( "Zero  ", 0x001010, 0x000000, 0, -1, -1 );

            for ( int j = d; j >= 0; j-- ) {
                // b.print(".", dacColors[j], 0xfffffe, 5, 0, (j*2)-2);
                if ( j != d ) {
                    b.printRawRow( 0b00010001, 22 + ( j * 2 ), dacColors[ j ], 0xfffffe );
                }

                b.printRawRow( 0b00000001, 22 + ( j * 2 ), dacColors[ j ], 0xfffffe );
            }

            b.print( "DAC ", dacColors[ d ], 0x000000, 0, 1, -1 );

            b.print( d, dacColors[ d ], 5, 1, -1 );
            refreshPaths( );
            globalState.clearAllConnections( );
            refreshConnections( 0, 0, 1 );

            if ( firstStart == 1 ) {
                delay( 1 );
            } else {
                delay( 8 );
            }
            // delay(10);

            switch ( d ) {
            case 0:

                addBridgeToState( DAC0, ISENSE_PLUS );
                // addBridgeToState(DAC0, ROUTABLE_BUFFER_IN);
                addBridgeToState( DAC0, ADC0 );
                Serial.println( "\n\n\r\tDAC 0" );
                break;
            case 1:

                addBridgeToState( DAC1, ISENSE_PLUS );
                addBridgeToState( DAC1, ADC1 );
                Serial.println( "\n\n\r\tDAC 1" );
                break;
            case 2:

                addBridgeToState( TOP_RAIL, ISENSE_PLUS );
                addBridgeToState( TOP_RAIL, ADC2 );
                Serial.println( "\n\n\r\tTop Rail" );
                break;
            case 3:

                addBridgeToState( BOTTOM_RAIL, ISENSE_PLUS );
                addBridgeToState( BOTTOM_RAIL, ADC3 );
                Serial.println( "\n\n\r\tBottom Rail" );
                break;
            }

            refreshConnections( 0, 0, 1 );
            if ( firstStart == 1 ) {
                delay( 20 );
            } else {
                delay( 18 );
            }
            printPathsCompact( );

            // Serial.print("\n\n\r\tDAC ");
            // Serial.println(d);

            Serial.println( "\n\r\t\tzeroing DAC" );

            int zeroFound = 0;
            float zeroTolerance = 2.1;

            int counter = 0;
            dacZero[ d ] = dacZero[ d ] + 5;

            while ( zeroFound < 2 && counter < 80 ) {
                setVoltage = 0.0;
                setDacByNumber( d, setVoltage, 0 );
                if ( firstStart == 1 ) {
                    delay( 32 );
                } else {
                    delay( 38 );
                }
                float reading = INA0.getBusVoltage_mV( );

                while ( INA0.getConversionFlag( ) == 0 ) {

                    // Serial.print(".");
                    delayMicroseconds( 400 );
                }

                reading = INA0.getBusVoltage_mV( );

                // delay(20);
                //  for (int i = 0; i < 1; i++) {
                //    delay(10);
                //    //INA0.getConversionFlag();
                //    float sample = INA0.getBusVoltage_mV();
                //    reading += sample;
                //    Serial.println(sample);

                // }
                //  reading = reading / 1;

                if ( reading < zeroTolerance && reading > -zeroTolerance ) {
                    zeroFound++;
                } else if ( reading < 2.0 ) {
                    dacZero[ d ] = dacZero[ d ] + 1;
                } else if ( reading > 2.0 ) {
                    dacZero[ d ] = dacZero[ d ] - 1;
                }

                // if (reading < 20.0 && reading > -20.0) // prevent the loop from
                // running
                //                                        // forever if it never finds
                //                                        zero
                // {
                counter++;
                //}

                if ( counter > 80 ) {
                    zeroFound++;
                    failedToConverge++;
                }

                Serial.print( "dacZero: " );
                Serial.print( dacZero[ d ] );

                Serial.print( "\t\tmeasured: " );
                Serial.print( reading );
                Serial.println( " mV" );
                // zeroFound = 1;
            }

            int spreadFound = 0;
            float tolerance = 4.1; // mV
            int giveUp = 0;
            Serial.println( "\n\n\rfinding spread\n\r" );
            b.print( "Spred", 0x080010, 0x000000, 0, -1, -1 );

            for ( int j = d; j >= 0; j-- ) {
                // b.print(".", dacColors[j], 0xfffffe, 5, 0, (j*2)-2);

                b.printRawRow( 0b00010001, 22 + ( j * 2 ), dacColors[ j ], 0xfffffe );
            }

            while ( spreadFound < 2 && giveUp < 40 ) {

                setVoltage = 5.0;
                giveUp++;
                float setMillivoltage = setVoltage * 1000;

                if ( dacSpread[ d ] < 18.0 || dacSpread[ d ] > 25.0 ||
                     dacSpread[ d ] != dacSpread[ d ] ) {
                    dacSpread[ d ] = 20.1;
                }
                setDacByNumber( d, setVoltage, 0 );
                if ( firstStart == 1 ) {
                    delay( 1 );
                } else {
                    delay( 8 );
                }
                // delay(20 * (spreadFound + 1));

                float reading = INA0.getBusVoltage_mV( );
                while ( INA0.getConversionFlag( ) == 0 ) {
                    delayMicroseconds( 100 );
                }

                reading = INA0.getBusVoltage_mV( );
                Serial.print( "Set: " );
                Serial.print( setVoltage );
                Serial.print( " V\t" );
                Serial.print( "dacSpread: " );
                Serial.print( dacSpread[ d ], 3 );
                Serial.print( " V\tmeasured: " );
                Serial.print( reading, 2 );
                Serial.println( " mV" );

                if ( reading <= ( setMillivoltage + tolerance ) &&
                     reading >= ( setMillivoltage - tolerance ) ) {
                    spreadFound++;
                } else if ( reading <= setMillivoltage - 14.5 ) {
                    dacSpread[ d ] = dacSpread[ d ] - 0.1;
                    // dacSpread[d] = dacSpread[d] - (abs((reading / 1000) - setVoltage));
                } else if ( reading >= setMillivoltage + 14.5 ) {
                    dacSpread[ d ] = dacSpread[ d ] + 0.1;
                    // dacSpread[d] = dacSpread[d] + (abs((reading / 1000) - setVoltage));
                } else if ( reading <= setMillivoltage - 4.5 ) {
                    dacSpread[ d ] = dacSpread[ d ] - 0.03;
                    // dacSpread[d] = dacSpread[d] - (abs((reading / 1000) - setVoltage));
                } else if ( reading >= setMillivoltage + 4.5 ) {
                    dacSpread[ d ] = dacSpread[ d ] + 0.03;
                    // dacSpread[d] = dacSpread[d] + (abs((reading / 1000) - setVoltage));
                }
            }

            if ( giveUp >= 40 )
                failedToConverge++;
        }

        // ADC calibration - use DAC 1 to calibrate all ADCs
        Serial.println( "\n\n\rCalibrating ADCs against INA readings using DAC 1\n\r" );

        int adcChannels[] = { 0, 1, 2, 3, 4, 7 }; // ADC channels to calibrate
        int numAdcChannels = 6;

        for ( int adcIdx = 0; adcIdx < numAdcChannels; adcIdx++ ) {
            int d = adcChannels[ adcIdx ];
            b.clear( );
            b.print( "calib", 0x100010, 0x000000, 0, 0, -1 );
            // b.print("ADC", dacColors[d], 0x000000, 3, -1, -2);
            char adcName[ 10 ] = "";

            switch ( d ) {
            case 0:
                strcpy( adcName, "ADC 0" );
                break;
            case 1:
                strcpy( adcName, "ADC 1" );

                break;
            case 2:
                strcpy( adcName, "ADC 2" );
                break;
            case 3:
                strcpy( adcName, "ADC 3" );
                break;
            case 4:
                strcpy( adcName, "ADC 4" );
                break;
            case 7:
                strcpy( adcName, "Probe" );
                break;
            }

            b.print( adcName, dacColors[ d % 4 ], 0x000000, 0, 1, 3 );

            // Use DAC 1 to calibrate all ADCs (it's working properly)
            globalState.clearAllConnections( );
            // refreshConnections(0, 0, 1);

            // Always use DAC 1 as the voltage source
            addBridgeToState( DAC1, ISENSE_PLUS );

            // Connect to the appropriate ADC
            switch ( d ) {
            case 0:
                addBridgeToState( DAC1, ADC0 );
                Serial.println( "\n\n\r\tADC 0 calibration (using DAC 1)" );
                break;
            case 1:
                addBridgeToState( DAC1, ADC1 );
                Serial.println( "\n\n\r\tADC 1 calibration (using DAC 1)" );
                break;
            case 2:
                addBridgeToState( DAC1, ADC2 );
                Serial.println( "\n\n\r\tADC 2 calibration (using DAC 1)" );
                break;
            case 3:
                addBridgeToState( DAC1, ADC3 );
                Serial.println( "\n\n\r\tADC 3 calibration (using DAC 1)" );
                break;
            case 4:
                addBridgeToState( DAC1, ADC4 );
                Serial.println( "\n\n\r\tADC 4 calibration (0-5V range, using DAC 1)" );
                break;
            case 7:
                addBridgeToState( DAC1, ROUTABLE_BUFFER_IN );
                Serial.println( "\n\n\r\tADC 7 calibration (Probe tip, using DAC 1)" );
                break;
            }

            refreshConnections( 0, 1, 1 );
            delay( 250 );
            printPathsCompact( );

            // Calibrate ADC using multiple voltage points
            float voltagePoints[ 8 ];
            int numPoints;

            if ( d == 4 ) {
                // ADC 4 has 0-5V range, use points within this range
                float adc4Points[] = { 0.0, 1.0, 2.0, 3.0, 4.0, 4.5 };
                numPoints = 6;
                for ( int i = 0; i < numPoints; i++ ) {
                    voltagePoints[ i ] = adc4Points[ i ];
                }
            } else {
                // ADCs 0-3 and 7 have ±8V range, but use 0-6V for calibration (INA can't measure negative)
                float standardPoints[] = { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
                numPoints = 7;
                for ( int i = 0; i < numPoints; i++ ) {
                    voltagePoints[ i ] = standardPoints[ i ];
                }
            }
            float totalError = 0.0;
            float sumINA = 0.0;
            float sumADC = 0.0;
            float sumADCSquared = 0.0;
            float sumADCxINA = 0.0;
            int validPoints = 0;

            for ( int p = 0; p < numPoints; p++ ) {
                float testVoltage = voltagePoints[ p ];
                setDacByNumber( 1, testVoltage, 0 ); // Always use DAC 1
                delay( 100 );

                // Get INA reading
                float inaReading = INA0.getBusVoltage( );

                while ( INA0.getConversionFlag( ) == 0 ) {
                    delayMicroseconds( 100 );
                }
                inaReading = INA0.getBusVoltage( );

                // Get raw ADC reading for the specific channel
                int rawADC = readAdc( d, 64 );

                // Only use positive voltages for calibration (INA can't measure negative)
                if ( inaReading > 0.1 ) {
                    sumINA += inaReading;
                    sumADC += rawADC;
                    sumADCSquared += rawADC * rawADC;
                    sumADCxINA += rawADC * inaReading;
                    validPoints++;

                    Serial.print( "Set: " );
                    Serial.print( testVoltage );
                    Serial.print( "V, INA: " );
                    Serial.print( inaReading );
                    Serial.print( "V, raw ADC" );
                    Serial.print( d );
                    Serial.print( ": " );
                    Serial.println( rawADC );
                }
            }

            // Calculate calibration using linear regression
            if ( validPoints >= 2 ) {
                // Use linear regression to find: INA_voltage = slope * rawADC + intercept
                float n = validPoints;
                float slope = ( n * sumADCxINA - sumADC * sumINA ) / ( n * sumADCSquared - sumADC * sumADC );
                float intercept = ( sumINA - slope * sumADC ) / n;

                // Handle different ADC formulas
                if ( d == 4 ) {
                    // ADC 4 formula: voltage = (rawADC * adcSpread / 4095) (no offset)
                    // We want: INA_voltage = slope * rawADC + intercept
                    // So: adcSpread/4095 = slope, and intercept should be 0
                    adcSpread[ d ] = slope * 4095.0;
                    adcZero[ d ] = 0.0; // No offset for ADC 4
                } else {
                    // Other ADCs formula: voltage = (rawADC * adcSpread / 4095) - adcZero
                    // We want: INA_voltage = slope * rawADC + intercept
                    // Matching coefficients: adcSpread/4095 = slope, so adcSpread = slope * 4095
                    // And: -adcZero = intercept, so adcZero = -intercept
                    adcSpread[ d ] = slope * 4095.0;
                    adcZero[ d ] = -intercept; // This is what gets subtracted in the offset
                }

                if ( abs( slope ) > 0.0001 ) {

                    // Clamp to reasonable values based on ADC type
                    if ( d == 4 ) {
                        // ADC 4 is 0-5V range
                        if ( adcSpread[ d ] < 3.0 || adcSpread[ d ] > 8.0 ) {
                            Serial.print( "ADC4 spread out of range: " );
                            Serial.print( adcSpread[ d ] );
                            Serial.println( ", using default" );
                            adcSpread[ d ] = 5.0; // Default for 0-5V
                            adcZero[ d ] = 0.0;
                        }
                    } else {
                        // Other ADCs are ±8V range
                        if ( adcSpread[ d ] < 10.0 || adcSpread[ d ] > 30.0 ) {
                            Serial.print( "adcSpread out of range: " );
                            Serial.print( adcSpread[ d ] );
                            Serial.println( ", using default" );
                            adcSpread[ d ] = 18.28; // Default value
                            adcZero[ d ] = 8.0;
                        } else if ( abs( adcZero[ d ] ) > 50.0 ) {
                            Serial.print( "adcZero out of range: " );
                            Serial.print( adcZero[ d ] );
                            Serial.println( ", using default" );
                            adcZero[ d ] = 8.0; // Default value
                        }
                    }

                    // Print calibration results if values are reasonable
                    Serial.print( "ADC " );
                    Serial.print( d );
                    Serial.print( " calibration: spread=" );
                    Serial.print( adcSpread[ d ], 2 );
                    Serial.print( ", zero=" );
                    Serial.print( adcZero[ d ], 2 );
                    Serial.print( " (slope=" );
                    Serial.print( slope, 6 );
                    Serial.print( ", intercept=" );
                    Serial.print( intercept, 2 );
                    Serial.println( ")" );
                } else {
                    Serial.print( "ADC calibration failed - slope too small: " );
                    Serial.println( slope, 6 );
                    // Keep default values
                }
            } else {
                Serial.println( "ADC calibration failed - insufficient valid points" );
            }

            setDacByNumber( 1, 0.0, 0 ); // Reset DAC 1 to 0V
        }

        Serial.println( "\n\n\tCalibration Values\n\n\r" );
        Serial.print( "            DAC Zero\tDAC Spread\tADC Zero\tADC Spread\n\r" );
        for ( int i = 0; i < 4; i++ ) {

            switch ( i ) {
            case 0:
                Serial.print( "DAC 0       " );
                break;
            case 1:
                Serial.print( "DAC 1       " );
                break;
            case 2:
                Serial.print( "Top Rail    " );
                break;
            case 3:
                Serial.print( "Bottom Rail " );
                break;
            }
            Serial.print( dacZero[ i ] );
            // Serial.print("\tdacSpread[");
            Serial.print( "\t" );

            Serial.print( dacSpread[ i ] );

            Serial.print( "\t\t" );

            Serial.print( adcZero[ i ] );

            Serial.print( "\t\t" );

            Serial.println( adcSpread[ i ] );
        }

        Serial.println( "\n\r            ADC Zero\tADC Spread" );
        Serial.print( "ADC 4 (0-5V)   " );
        Serial.print( adcZero[ 4 ] );
        Serial.print( "\t" );
        Serial.println( adcSpread[ 4 ] );
        Serial.print( "ADC 7 (Probe)  " );
        Serial.print( adcZero[ 7 ] );
        Serial.print( "\t" );
        Serial.println( adcSpread[ 7 ] );
        delay( 10 );
        // checkProbeCurrentZero( );
        saveDacCalibration( );
    }
    setRailsAndDACs( );

    if ( failedToConverge == 0 ) {
        changeTerminalColor( 84, true ); // Green
    } else {
        changeTerminalColor( 196, true ); // Red
    }

    // Print big block text for PASS or FAIL
    if ( failedToConverge == 0 ) {
        Serial.println( "\r\n" );
        Serial.println( "███████   █████   ███████ ███████" );
        Serial.println( "██    ██ ██   ██  ██      ██     " );
        Serial.println( "███████  ███████  ███████ ███████" );
        Serial.println( "██       ██   ██       ██      ██" );
        Serial.println( "██       ██   ██  ███████ ███████" );
        Serial.println( "\r\n" );
    } else {
        Serial.println( "\r\n" );
        Serial.println( "███████  █████  ██ ██     " );
        Serial.println( "██      ██   ██ ██ ██     " );
        Serial.println( "█████   ███████ ██ ██     " );
        Serial.println( "██      ██   ██ ██ ██     " );
        Serial.println( "██      ██   ██ ██ ███████" );
        Serial.println( "\r\n" );
    }
    Serial.println( );
    Serial.print( "\n\n\rFailedToConverge = " );
    Serial.println( failedToConverge );
    changeTerminalColor( -1 );

    Serial.println( "\n\n\rrun test? (y/n)\n\n\rmake damn sure nothing is "
                    "physically connected to the rails\n\r" );

    b.clear( );
    b.print( "Test?", 0x0a0a00, 0x000000, 1, -1, -1 );
    requestLedShow( -2 );
    int yesNo;
    if ( firstStart == 1 ) {
        yesNo = 1; // yesNoMenu(800);
    } else {
        yesNo = yesNoMenu( 8000 );
    }

    failedToConverge = 0;

    float tolerance = 0.6;
    if ( yesNo == 1 ) {

        b.clear( );
        setDacByNumber( 0, 0.0, 0 );
        setDacByNumber( 1, 0.0, 0 );
        setDacByNumber( 2, 0.0, 0 );
        setDacByNumber( 3, 0.0, 0 );
        for ( int d = 0; d < 4; d++ ) {
            b.clear( 0 );

            globalState.clearAllConnections( );
            // refreshConnections(0, 0, 1);
            if ( firstStart == 1 ) {
                delay( 8 );
            } else {
                delay( 8 );
            }
            switch ( d ) {
            case 0:

                // addBridgeToState(DAC0, ISENSE_PLUS);

                addBridgeToState( DAC0, ROUTABLE_BUFFER_IN );
                // addBridgeToState(DAC0, ADC0);
                Serial.println( "\n\n\r\tDAC 0 test" );
                b.print( "DAC 0", dacColors[ d ], 0x000000, 1, -1, -1 );
                break;
            case 1:
                /// removeBridgeFromState(ADC0+d, -1);
                addBridgeToState( DAC1, ISENSE_PLUS );
                addBridgeToState( DAC1, ADC1 );
                Serial.println( "\n\n\r\tDAC 1 test" );
                b.print( "DAC 1", dacColors[ d ], 0x000000, 1, -1, -1 );
                break;
            case 2:
                // removeBridgeFromState(ADC0+d, -1);
                addBridgeToState( TOP_RAIL, ISENSE_PLUS );
                addBridgeToState( TOP_RAIL, ADC2 );
                Serial.println( "\n\n\r\tTop Rail test" );
                // b.print("Top Ral", dacColors[d], 0x000000, 0, -1, -1);
                b.print( "Top", dacColors[ d ], 0x000000, 0, -1, -1 );
                b.print( "Ra", dacColors[ d ], 0xfffffe, 4, -1, -2 );
                b.print( "i", dacColors[ d ], 0xfffffe, 6, 0, -3 );
                b.print( "l", dacColors[ d ], 0xfffffe, 6, 0, -1 );
                break;
            case 3:
                // removeBridgeFromState(ADC0+d, -1);
                addBridgeToState( BOTTOM_RAIL, ISENSE_PLUS );
                addBridgeToState( BOTTOM_RAIL, ADC3 );
                Serial.println( "\n\n\r\tBottom Rail test" );
                b.print( "Bot", dacColors[ d ], 0x000000, 0, -1, -1 );
                b.print( "Ra", dacColors[ d ], 0xfffffe, 4, -1, -2 );
                b.print( "i", dacColors[ d ], 0xfffffe, 6, 0, -3 );
                b.print( "l", dacColors[ d ], 0xfffffe, 6, 0, -1 );
                break;
            }

            refreshConnections( 0, 1, 1 );
            // refreshBlind(1, 0);
            if ( firstStart == 1 ) {
                delay( 1 );
            } else {
                delay( 8 );
            }
            printPathsCompact( );
            Serial.println( " " );

            int nextRow = 0;

            for ( int i = -1; i <= 8; i++ ) {
                setVoltage = i * 1.0;
                setDacByNumber( d, setVoltage, 0 );
                Serial.print( "set : " );
                Serial.printf( "%*.3f", 6, setVoltage );
                Serial.print( " V\t" );
                if ( firstStart == 1 ) {
                    delay( 25 );
                } else {
                    delay( 150 );
                }
                float reading = 0.0;

                int voltage = map( i, -3, 8, 0, 4 );

                b.printRawRow( 0b00000001 << voltage, nextRow + 30 + ( d * 6 ),
                               dacColors[ d ], 0xfffffe );
                nextRow++;

                if ( firstStart == 1 ) {
                    delay( 5 );
                } else {
                    delay( 8 );
                }
                if ( d == 0 ) {
                    reading = readAdcVoltage( 7, 64 );
                } else {
                    reading = readAdcVoltage( d, 64 );
                }
                Serial.print( "\tADC measured: " );

                if ( abs( reading - setVoltage ) > tolerance && i <= 7 ) {
                    failedToConverge++;
                }
                // if (i < 0) {
                //  Serial.print(setVoltage); // + random(-4, 4) / 100.0);

                // } else if (i > 8) {
                //   Serial.print(setVoltage); // + random(-4, 4) / 100.0);
                // } else {
                Serial.printf( "%*.3f", 6, reading );
                //}
                Serial.print( " V" );
                if ( firstStart == 1 ) {
                    delay( 3 );
                } else {
                    delay( 8 );
                }

                if ( d == 0 ) {
                    reading = INA1.getBusVoltage( );

                    while ( INA1.getConversionFlag( ) == 0 ) {
                        // Serial.print(".");
                        // delay(1);
                        delayMicroseconds( 100 );
                    }

                    reading = INA1.getBusVoltage( );

                    if ( abs( reading - setVoltage ) > tolerance && i >= 0 ) {
                        failedToConverge++;
                    }

                } else {

                    reading = INA0.getBusVoltage( );

                    while ( INA0.getConversionFlag( ) == 0 ) {
                        // Serial.print(".");
                        // delay(1);
                        delayMicroseconds( 100 );
                    }

                    reading = INA0.getBusVoltage( );

                    if ( abs( reading - setVoltage ) > tolerance && i >= 0 ) {
                        failedToConverge++;
                    }
                }

                Serial.print( "\t     INA measured: " );
                Serial.printf( "%*.3f", 6, reading );
                Serial.println( " V" );

                // dacCalibration[0][i] = reading;
            }
            setDacByNumber( d, 0.0, 0 );
            // Old code (replaced with globalState.power): setDacByNumber(d, d < 2 ? dacOutput[d] : railVoltage[d - 2], 0);
        }
    }
    unsigned long timeout = millis( );
    Serial.println( "\n\r" );
    while ( 1 ) {
        if ( millis( ) - timeout > 1000 ) {
            break;
        }
        if ( Serial.available( ) > 0 ) {
            break;
        }
        if ( encoderButtonState == PRESSED ) {
            encoderButtonState = IDLE;
            break;
        }
    }
    // delay(5000);
    INA0.setBusADC( 0x0b );
    INA1.setBusADC( 0x0b );
    // removeBridgeFromState(ISENSE_PLUS, -1);
    
    leaveApp( );  // Restore original slot
if ( yesNo == 1 ) {
    // Print big block text for PASS or FAIL
    if ( failedToConverge < 7 ) {
        changeTerminalColor( 84, true ); // Green
        Serial.println( "\r\n" );
        Serial.println( "███████   █████   ███████ ███████" );
        Serial.println( "██    ██ ██   ██  ██      ██     " );
        Serial.println( "███████  ███████  ███████ ███████" );
        Serial.println( "██       ██   ██       ██      ██" );
        Serial.println( "██       ██   ██  ███████ ███████" );
        Serial.println( "\r\n" );
    } else {
        changeTerminalColor( 196, true ); // Red
        Serial.println( "\r\n" );
        Serial.println( "███████  █████  ██ ██     " );
        Serial.println( "██      ██   ██ ██ ██     " );
        Serial.println( "█████   ███████ ██ ██     " );
        Serial.println( "██      ██   ██ ██ ██     " );
        Serial.println( "██      ██   ██ ██ ███████" );
        Serial.println( "\r\n" );
    }
    // Serial.println();
    Serial.print( "\n\n\rFailedToConverge = " );
    Serial.println( failedToConverge );
    Serial.println( );
    Serial.println( );
}
    changeTerminalColor( -1, true );

    // The calibration sweeps wrote the DAC hardware with save=0 (state
    // untouched) - force the feed's next park to re-write the MCP so the
    // probe doesn't come back at whatever the last sweep step left.
    infraDacParkEpochBump( );
    infraSetProbePowerEnabled( wasProbePowerOn );
    refreshConnections( -1 );
    if ( wasProbePowerOn ) {
        routableBufferPower( 1, 0, 1 ); // forced nudge; unconditional 1 here used to discard the restore above
    }
    // showProbeLEDs = 1;
    refreshConnections( -1 );
    configChanged = true;

    // Run probe switch threshold calibration
    // Serial.println( "\n\n\r" );
    // Serial.println( "========================================" );
    // Serial.println( "Starting Probe Switch Calibration..." );
    // Serial.println( "========================================\n\r" );
    // delay( 1000 );
    // calibrateProbeSwitchThresholds( );

    if ( firstStart == 1 ) {
        initializeMicroPythonExamples( false );

        // Unattended hardware self test replaces the interactive probe apps
        // (calibrateProbeSwitchThresholds / probeCalibApp) so mass flashing
        // needs no operator input. Both remain available from the clickwheel
        // calibration menu for manual fine-tuning. This writes /selftest.json
        // plus the one-shot marker that repaints the result overlay after the
        // restart below.
        runFullSelfTest( true );

        // Hold the result overlay on the breadboard LEDs; the operator's
        // touch (probe button / encoder / serial byte) chains into the
        // interactive probe pad calibration instead of resetting - they are
        // already at the board, so use the moment to align the pad map.
        selfTestWaitForInput( "start probe pad calibration" );
        selfTestClearOverlay( );
        probeCalibApp( ); // saves config (incl. pad endpoints) on finish

        // Start the device with a clean undo/redo history: the calibration
        // connects/disconnects above are internal setup, not user actions, and
        // any history carried over from a factory/test image shouldn't ship to
        // the user. Wipe it before the reboot that ends first-start.
        undoWipeHistory( );

        Serial.println( "Resetting..." );
        Serial.flush( );
        delay( 150 );
        rp2040.restart( );
    }
}

// ---- printSerial1stuff() ----
void printSerial1stuff( void ) {
    Serial.println( "Printing Serial1 stuff" );
    Serial.flush( );

    oled.clear( );

    while ( 1 ) {
        if ( Serial.available( ) > 0 ) {
            char c = Serial.read( );
            USBSer1.write( c );
            USBSer1.flush( );
            oled.print( c );
            oled.show( );
            Serial.print( "sent: " );
            Serial.println( c );
            Serial.flush( );
            if ( c == 'x' )
                break;
        }
        if ( USBSer1.available( ) > 0 ) {
            char c = USBSer1.read( );
            Serial.print( "received: " );
            Serial.println( c );
            Serial.flush( );
        }
    }
}

void displayImage( void ) {
    Serial.println( F( "PNG Image app not implemented yet." ) );
}

const char* addressToHexString( uint8_t address ) {
    static char hexStr[ 6 ];
    sprintf( hexStr, "0x%02X", address );
    return hexStr;
}

// Unified RGB->JDI palette mapping
static inline uint16_t rgbToJdi( uint8_t r, uint8_t g, uint8_t b ) {
    uint16_t col = 0;
    if ( r >= 0x50 )
        col |= COLOR_RED;
    if ( g >= 0x80 )
        col |= COLOR_GREEN;
    if ( b >= 0x60 )
        col |= COLOR_BLUE; // unified threshold
    return col;
}

void jdiMIPdisplay( void ) {
    // Allocate JDI display object (5.1KB) - freed on exit
    jdi_display = new ( std::nothrow ) JDI_MIP_Display( );
    if ( jdi_display == nullptr ) {
        Serial.println( "Failed to allocate JDI display (5.1KB)" );
        return;
    }

    jdi_display->begin( );
    jdi_display->displayOn( );
    jdi_display->clearScreen( );
    jdi_display->refresh( );

    cycleTerminalColor( true, 5.3, true );
    Serial.println( "\n\rThis is meant to run with a JDI LPM009M360A 72x144 display" );
    Serial.println( "It will bounce the startup animation and then exit\n\r" );
    cycleTerminalColor( );
    Serial.println( "Connections: \n\r" );

    Serial.println( "  ┃  8  7  6  5  4  3  2  1  ┃  " );
    Serial.println( "  ┃  ▌     ▌  ▌  ▌  ▌  ▌  ▌  ┃  " );
    Serial.println( "  ┃  ▌     ▌  ▌  ▌  ▌  ▌  ▌  ┃  " );
    Serial.println( "  ┃  ▌     ▌  ▌  ▌  ▌  ▌  ▌  ┃  " );
    Serial.println( "  ┃  ▌  ╳  ▌  ▌  ▌  ▌  ▌  ▌  ┃  " );
    Serial.println( "  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━┛  " );
    cycleTerminalColor( );
    Serial.println( "     G  N  3  D  E  C  M  C     " );
    Serial.println( "     N  C  V  i  x  S  O  L     " );
    Serial.println( "     D     3  s  t  .  S  K     " );
    Serial.println( "     ⏚     +  p     .  I  .     " );
    Serial.println( "              .     .  .  .     " );
    cycleTerminalColor( );
    Serial.println( "    GPIO      6     2  4  3     " );

    Serial.println( );
    cycleTerminalColor( );
    Serial.println( "GPIO 2 - CS" );
    Serial.println( "GPIO 3 - SCK" );
    Serial.println( "GPIO 4 - MOSI" );
    Serial.println( "GPIO 6 - DISP" );
    cycleTerminalColor( );
    Serial.println( "\n\rPress the encoder button or any key to exit\n\n\r" );
    cycleTerminalColor( );
    Serial.println( "Paste this in if you have the display soldered to the adapter board's SMD footptint" );
    Serial.println( "f { 139-106,136-79,101-80,82-100,132-77,134-76,133-75, } " );
    cycleTerminalColor( );
    Serial.flush( );

    const int srcW = 32;
    const int srcH = 21;
    const int rotW = srcH; // 90° rotation width
    const int rotH = srcW; // 90° rotation height

    for ( int outer = 0; outer < 100; outer++ ) {
        for ( int fi = 0; fi < startupFrameLEN; fi++ ) {
            const uint32_t* src = jl_get_startup_frame( fi );
            for ( int dy = 0; dy < DISPLAY_HEIGHT; dy++ ) {
                int uy = ( dy * rotH ) / DISPLAY_HEIGHT;
                for ( int dx = 0; dx < DISPLAY_WIDTH; dx++ ) {
                    int ux = ( dx * rotW ) / DISPLAY_WIDTH;
                    int sx = srcW - 1 - uy;
                    int sy = ux;
                    uint32_t c = src[ sy * srcW + sx ];
                    uint8_t r = ( c >> 16 ) & 0xFF;
                    uint8_t g = ( c >> 8 ) & 0xFF;
                    uint8_t b = ( c ) & 0xFF;
                    uint16_t col = rgbToJdi( r, g, b );
                    jdi_display->drawBufferedPixel( DISPLAY_WIDTH - 1 - dx, DISPLAY_HEIGHT - 1 - dy, col );
                }
            }
            jdi_display->refresh( );
        }
        for ( int fi = startupFrameLEN - 1; fi >= 0; fi-- ) {
            const uint32_t* src = jl_get_startup_frame( fi );
            for ( int dy = 0; dy < DISPLAY_HEIGHT; dy++ ) {
                int uy = ( dy * rotH ) / DISPLAY_HEIGHT;
                for ( int dx = 0; dx < DISPLAY_WIDTH; dx++ ) {
                    int ux = ( dx * rotW ) / DISPLAY_WIDTH;
                    int sx = srcW - 1 - uy;
                    int sy = ux;
                    uint32_t c = src[ sy * srcW + sx ];
                    uint8_t r = ( c >> 16 ) & 0xFF;
                    uint8_t g = ( c >> 8 ) & 0xFF;
                    uint8_t b = ( c ) & 0xFF;
                    uint16_t col = rgbToJdi( r, g, b ); // same thresholds both passes
                    jdi_display->drawBufferedPixel( DISPLAY_WIDTH - 1 - dx, DISPLAY_HEIGHT - 1 - dy, col );
                }
            }
            jdi_display->refresh( );
        }

        if ( encoderButtonState == PRESSED || Serial.available( ) > 0 )
            break;
        delay( 10 );
    }

    // Free JDI display object
    if ( jdi_display != nullptr ) {
        delete jdi_display;
        jdi_display = nullptr;
    }
    return;
}

// Global counter for DMX packet callbacks
static volatile uint32_t g_dmx_packet_count = 0;

// Callback function that gets called when a DMX packet is received
void dmx_packet_callback( DmxInput* dmxInput ) {
    g_dmx_packet_count++;
}

// DMX Configuration Structure
struct DMXLightConfig {
    // Hardware configuration
    uint8_t tx_pin;      // GPIO pin for DMX TX (connects to RS-485 transceiver)
    uint8_t dmx_address; // Starting DMX address (1-512)

    // Channel mode configuration
    enum ChannelMode {
        MODE_6CH = 6,  // 6 channel: R, G, B, W, A, UV
        MODE_10CH = 10 // 10 channel: Intensity, R, G, B, W, A, UV, Strobe, Function, Speed
    } channel_mode;

    // Color values (0-255)
    struct {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        uint8_t white;
        uint8_t amber;
        uint8_t uv;
    } color;

    // Extended controls (for 10-channel mode)
    struct {
        uint8_t intensity; // Master intensity (Ch1 in 10ch mode)
        uint8_t strobe;    // Strobe speed (Ch8 in 10ch mode, 0-50: off, 51-255: slow to fast)
        uint8_t function;  // Function select (Ch9 in 10ch mode)
        uint8_t speed;     // Function speed (Ch10 in 10ch mode)
    } extended;

    // Transmission settings
    uint16_t refresh_rate_ms; // DMX refresh rate in milliseconds (typical: 25-50ms for 20-40Hz)
    bool wireless_mode;       // True = wireless (512 bytes, no start code), False = wired (513 bytes with start code)
};

// Helper function to update DMX universe from config
void updateDMXUniverse( uint8_t* universe, const DMXLightConfig& config ) {
    // Universe[0] is ALWAYS the start code (0x00 for DMX512)
    // The Pico-DMX library requires this format
    universe[ 0 ] = 0x00;

    // DMX addresses are 1-based: address 1 = universe[1], address 2 = universe[2], etc.
    // Fixture at DMX address 1 reads its Channel 1 from universe[1]
    uint16_t addr = config.dmx_address; // Starting address (1-based, maps directly to array index)

    if ( config.channel_mode == DMXLightConfig::MODE_6CH ) {
        // 6 Channel mode: R, G, B, W, A, UV
        // For fixture at address 1: Red->universe[1], Green->universe[2], etc.
        universe[ addr ] = config.color.red;       // Channel 1 at address
        universe[ addr + 1 ] = config.color.green; // Channel 2
        universe[ addr + 2 ] = config.color.blue;  // Channel 3
        universe[ addr + 3 ] = config.color.white; // Channel 4
        universe[ addr + 4 ] = config.color.amber; // Channel 5
        universe[ addr + 5 ] = config.color.uv;    // Channel 6
    } else if ( config.channel_mode == DMXLightConfig::MODE_10CH ) {
        // 10 Channel mode: Intensity, R, G, B, W, A, UV, Strobe, Function, Speed
        universe[ addr ] = config.extended.intensity;
        universe[ addr + 1 ] = config.color.red;
        universe[ addr + 2 ] = config.color.green;
        universe[ addr + 3 ] = config.color.blue;
        universe[ addr + 4 ] = config.color.white;
        universe[ addr + 5 ] = config.color.amber;
        universe[ addr + 6 ] = config.color.uv;
        universe[ addr + 7 ] = config.extended.strobe;
        universe[ addr + 8 ] = config.extended.function;
        universe[ addr + 9 ] = config.extended.speed;
    }
}

void DMXSerialApp( void ) {
    Serial.println( "\n=== DMX Light Controller ===" );
    Serial.println( "Controlling RGBA[UV] DMX Light" );
    Serial.println( "Press encoder button or 'q' to exit\n" );

    // Initialize DMX configuration
    DMXLightConfig config = {
        .tx_pin = GPIO_1_PIN,                     // TX on GPIO 1 (hardware GPIO 20)
        .dmx_address = 1,                         // Start at DMX address 1
        .channel_mode = DMXLightConfig::MODE_6CH, // Use 6-channel mode
        .color = {
            .red = 100, // Test with maximum value
            .green = 0,
            .blue = 0,
            .white = 0,
            .amber = 0,
            .uv = 100 },
        .extended = {
            .intensity = 50, // Full intensity in 10ch mode
            .strobe = 0,     // No strobe
            .function = 1,   // Function off (0-50 = Ch1-7 valid)
            .speed = 0       // Speed off
        },
        .refresh_rate_ms = 40, // 400ms refresh rate
        .wireless_mode = false // Start in wired mode (direct RS-485 connection)
    };

    // DMX universe buffer (513 bytes: 1 start code + 512 channels)
    // Allocate on heap to reduce stack pressure
    constexpr uint16_t UNIVERSE_SIZE = 513;
    uint8_t* universe = new ( std::nothrow ) uint8_t[ UNIVERSE_SIZE ];
    if ( universe == nullptr ) {
        Serial.println( "ERROR: Failed to allocate DMX universe buffer (513 bytes)" );
        return;
    }
    memset( universe, 0, UNIVERSE_SIZE );
    universe[ 0 ] = 0x00; // DMX512 start code

    // Initialize DMX transmitter
    DmxOutput dmxTx;
    Serial.print( "Initializing DMX transmitter on GPIO " );
    Serial.print( config.tx_pin );
    Serial.println( " using PIO0..." );

    if ( dmxTx.begin( config.tx_pin, pio0 ) != DmxOutput::SUCCESS ) {
        Serial.println( "ERROR: Failed to initialize DMX transmitter!" );
        return;
    }
    Serial.println( "✓ DMX transmitter initialized" );

    // Setup GPIO 2 (hw 21) as inverted output of GPIO 1 for pseudo-differential signaling
    // This creates DMX+ and DMX- without needing an RS-485 transceiver chip
    Serial.print( "Setting up GPIO 2 (hw 21) as inverted output..." );

    // Create a PIO program to invert GPIO 1 onto GPIO 2
    // We'll use PIO1 to avoid conflicts with the DMX transmitter on PIO0
    // PIO Assembly:
    //   .wrap_target
    //   in pins, 1      ; Read 1 bit from GPIO 1 into ISR
    //   mov osr, !isr   ; Invert ISR and move to OSR
    //   out pins, 1     ; Output 1 bit from OSR to GPIO 2
    //   .wrap
    static const uint16_t inverter_program[] = {
        0x4001, // in pins, 1     - Read 1 bit from input pins
        0xa027, // mov osr, !isr  - Invert ISR and move to OSR
        0x6001, // out pins, 1    - Output 1 bit to output pins
    };

    struct pio_program inverter = {
        .instructions = inverter_program,
        .length = 3,
        .origin = -1 };

    // // Try to add program to PIO1
    // if ( !pio_can_add_program( pio1, &inverter ) ) {
    //     Serial.println( " FAILED (no PIO space)" );
    //     Serial.println( "⚠️  Running without inverted output" );
    // } else {
    //     uint offset = pio_add_program( pio1, &inverter );
    //     int sm = pio_claim_unused_sm( pio1, false );

    //     if ( sm == -1 ) {
    //         Serial.println( " FAILED (no state machine)" );
    //         Serial.println( "⚠️  Running without inverted output" );
    //     } else {
    //         // Configure GPIO 2 as output for the inverter PIO
    //         pio_gpio_init( pio1, GPIO_2_PIN );
    //         pio_sm_set_consecutive_pindirs( pio1, sm, GPIO_2_PIN, 1, true );   // Output

    //         // NOTE: Don't initialize GPIO 1 for PIO1 - it's already owned by PIO0 for DMX output
    //         // We can still READ its state by mapping it as the input pin in the state machine config

    //         // Configure state machine
    //         pio_sm_config c = pio_get_default_sm_config( );
    //         sm_config_set_in_pins( &c, GPIO_1_PIN );      // Read GPIO 1 state (without claiming it)
    //         sm_config_set_out_pins( &c, GPIO_2_PIN, 1 );  // Write to GPIO 2
    //         sm_config_set_in_shift( &c, false, false, 1 ); // Shift left, no autopush, threshold 1
    //         sm_config_set_out_shift( &c, true, false, 1 ); // Shift right, no autopull, threshold 1
    //         sm_config_set_wrap( &c, offset, offset + 2 ); // Wrap from instruction 0 to 2
    //         sm_config_set_clkdiv( &c, 1.0f );             // Run as fast as possible (~125MHz)

    //         // Initialize and start the state machine
    //         pio_sm_init( pio1, sm, offset, &c );
    //         pio_sm_set_enabled( pio1, sm, true );

    //         Serial.println( " ✓ OK" );
    //         Serial.println( "✓ Pseudo-differential output configured" );
    //         Serial.println( "  GPIO 1 (hw 20) = DMX+" );
    //         Serial.println( "  GPIO 2 (hw 21) = DMX- (inverted)\n" );
    //     }
    // }

    // Display configuration
    Serial.println( "Configuration:" );
    Serial.print( "  DMX Address: " );
    Serial.println( config.dmx_address );
    Serial.print( "  Channel Mode: " );
    Serial.print( config.channel_mode );
    Serial.println( " channels" );
    Serial.print( "  Refresh Rate: " );
    Serial.print( config.refresh_rate_ms );
    Serial.println( "ms" );
    Serial.print( "  Output Mode: " );
    Serial.println( config.wireless_mode ? "Wireless (testing)" : "Wired (standard)" );
    Serial.println( "  Note: Currently sending 513 bytes in both modes\n" );

    // Test pattern selection
    enum TestPattern {
        PATTERN_COLOR_FADE,
        PATTERN_RGB_CYCLE,
        PATTERN_WHITE_STROBE,
        PATTERN_RAINBOW,
        PATTERN_SPECTRUM, // New: smooth RGBA[UV] spectrum cycle
        PATTERN_COUNT
    };
    TestPattern currentPattern = PATTERN_COLOR_FADE;

    Serial.println( "Commands: m = toggle mode (6/10ch), p = toggle manual/pattern, w = wired/wireless, a/z = address -/+, q = quit\n" );

    // Channel names for encoder control
    const char* channelNames6ch[] = { "Red", "Green", "Blue", "White", "Amber", "UV" };
    const char* channelNames10ch[] = { "Intensity", "Red", "Green", "Blue", "White", "Amber", "UV", "Strobe", "Function", "Speed" };

    // Control mode: manual encoder control vs test patterns
    bool manualMode = true;
    uint8_t currentChannel = 0;
    int lastEncoderPos = encoderPosition;
    encoderButtonStates lastButtonState = IDLE;

    // Get pointer to current channel
    auto getChannelPtr = [ & ]( ) -> uint8_t* {
        if ( config.channel_mode == DMXLightConfig::MODE_6CH ) {
            switch ( currentChannel ) {
            case 0:
                return &config.color.red;
            case 1:
                return &config.color.green;
            case 2:
                return &config.color.blue;
            case 3:
                return &config.color.white;
            case 4:
                return &config.color.amber;
            case 5:
                return &config.color.uv;
            }
        } else {
            switch ( currentChannel ) {
            case 0:
                return &config.extended.intensity;
            case 1:
                return &config.color.red;
            case 2:
                return &config.color.green;
            case 3:
                return &config.color.blue;
            case 4:
                return &config.color.white;
            case 5:
                return &config.color.amber;
            case 6:
                return &config.color.uv;
            case 7:
                return &config.extended.strobe;
            case 8:
                return &config.extended.function;
            case 9:
                return &config.extended.speed;
            }
        }
        return &config.color.red;
    };

    auto getChannelName = [ & ]( ) -> const char* {
        return ( config.channel_mode == DMXLightConfig::MODE_6CH ) ? channelNames6ch[ currentChannel ] : channelNames10ch[ currentChannel ];
    };

    Serial.println( "MANUAL CONTROL MODE" );
    Serial.println( "Live Dashboard: [Selected] channel shown in brackets\n" );

    unsigned long lastTransmitTime = 0;
    unsigned long lastDisplayTime = 0;
    unsigned long patternTime = 0;
    uint16_t patternStep = 0; // Use uint16_t for values > 255

    while ( true ) {
        unsigned long now = millis( );

        // MANUAL MODE: Encoder controls individual channels
        if ( manualMode ) {
            // Handle encoder button (cycle channels)
            if ( encoderButtonState != lastButtonState ) {
                if ( encoderButtonState == PRESSED ) {
                    uint8_t maxCh = ( config.channel_mode == DMXLightConfig::MODE_6CH ) ? 5 : 9;
                    currentChannel = ( currentChannel + 1 ) % ( maxCh + 1 );
                    lastEncoderPos = encoderPosition;
                    lastDisplayTime = 0; // Force immediate display update
                } else if ( encoderButtonState == HELD ) {
                    goto exit_loop;
                }
                lastButtonState = encoderButtonState;
            }

            // Handle encoder rotation (adjust value) - REVERSED for intuitive control
            int delta = lastEncoderPos - encoderPosition; // Reversed!
            if ( delta != 0 ) {
                lastEncoderPos = encoderPosition;

                uint8_t* val = getChannelPtr( );
                int newVal = *val + delta;
                if ( newVal < 0 )
                    newVal = 0;
                if ( newVal > 255 )
                    newVal = 255;
                *val = newVal;
                lastDisplayTime = 0; // Force immediate update on change
            }

            // Live dashboard update (throttled) - updates continuously
            if ( now - lastDisplayTime >= 50 ) { // Faster refresh for smoother feel
                lastDisplayTime = now;
                uint8_t* val = getChannelPtr( );

                // Clear line and print live dashboard
                Serial.print( "\r                                                                                      \r" );

                // Compact bar graphs for all 6 channels (4 chars per bar)
                auto printBar = [ & ]( uint8_t value, bool selected ) {
                    int bars = value / 32; // 0-3 bars for compact display
                    if ( selected )
                        Serial.print( "[" );
                    for ( int i = 0; i < 8; i++ ) {
                        Serial.print( i < bars ? "█" : "░" );
                    }
                    if ( selected )
                        Serial.print( "]" );
                    else
                        Serial.print( " " );
                };

                Serial.print( "R" );
                printBar( config.color.red, currentChannel == 0 );
                Serial.print( "G" );
                printBar( config.color.green, currentChannel == 1 );
                Serial.print( "B" );
                printBar( config.color.blue, currentChannel == 2 );
                Serial.print( "W" );
                printBar( config.color.white, currentChannel == 3 );
                Serial.print( "A" );
                printBar( config.color.amber, currentChannel == 4 );
                Serial.print( "U" );
                printBar( config.color.uv, currentChannel == 5 );

                Serial.print( " | " );
                Serial.print( getChannelName( ) );
                Serial.print( ":" );
                Serial.print( *val );
                if ( *val < 10 ) {
                    Serial.print( "  " );
                } else if ( *val < 100 ) {
                    Serial.print( " " );
                }
                Serial.flush( );
            }
        }
        // PATTERN MODE: Run test patterns
        else {
            if ( encoderButtonState == HELD ) {
                goto exit_loop;
            }
            lastButtonState = encoderButtonState;

            // Update test pattern
            switch ( currentPattern ) {
            case PATTERN_COLOR_FADE:
                // Fade through red, green, blue, white
                if ( now - patternTime > 120 ) {
                    patternTime = now;
                    if ( patternStep < 255 ) {
                        config.color.red = patternStep;
                        config.color.green = 0;
                        config.color.blue = 0;
                        config.color.white = 0;
                    } else if ( patternStep < 510 ) {
                        config.color.red = 510 - patternStep;
                        config.color.green = patternStep - 255;
                        config.color.blue = 0;
                        config.color.white = 0;
                    } else if ( patternStep < 765 ) {
                        config.color.red = 0;
                        config.color.green = 765 - patternStep;
                        config.color.blue = patternStep - 510;
                        config.color.white = 0;
                    } else if ( patternStep < 1020 ) {
                        config.color.red = 0;
                        config.color.green = 0;
                        config.color.blue = 1020 - patternStep;
                        config.color.white = patternStep - 765;
                    } else {
                        config.color.white = 0;
                        patternStep = 0;
                    }
                    patternStep++;
                }
                break;

            case PATTERN_RGB_CYCLE:
                // Cycle through pure R, G, B
                if ( now - patternTime > 1000 ) {
                    patternTime = now;
                    config.color = { 0, 0, 0, 0, 0, 0 };
                    if ( patternStep == 0 )
                        config.color.red = 255;
                    else if ( patternStep == 1 )
                        config.color.green = 255;
                    else if ( patternStep == 2 )
                        config.color.blue = 255;
                    patternStep = ( patternStep + 1 ) % 3;
                }
                break;

            case PATTERN_WHITE_STROBE:
                // Strobe white light
                if ( now - patternTime > 100 ) {
                    patternTime = now;
                    config.color.white = ( patternStep % 2 ) ? 255 : 0;
                    patternStep++;
                }
                break;

            case PATTERN_RAINBOW:
                // Rainbow using all colors
                if ( now - patternTime > 15 ) {
                    patternTime = now;
                    config.color.red = (uint8_t)( ( sin( patternStep * 0.02 ) + 1.0 ) * 127 );
                    config.color.green = (uint8_t)( ( sin( patternStep * 0.02 + 2.09 ) + 1.0 ) * 127 );
                    config.color.blue = (uint8_t)( ( sin( patternStep * 0.02 + 4.18 ) + 1.0 ) * 127 );
                    patternStep++;
                }
                break;

            case PATTERN_SPECTRUM:
                // Smooth spectrum cycle through Red→Amber→Green→Cyan→Blue→UV→Magenta→Red
                // Using all 6 channels: R, G, B, W, A, UV
                if ( now - patternTime > 15 ) {
                    patternTime = now;

                    // Cycle through 9 phases (0-1530 steps, 170 steps per phase)
                    uint16_t phase = patternStep % 1530;

                    // Reset all channels
                    config.color.red = 0;
                    config.color.green = 0;
                    config.color.blue = 0;
                    config.color.white = 0;
                    config.color.amber = 0;
                    config.color.uv = 0;

                    if ( phase < 170 ) {
                        // Phase 0: Pure Red
                        config.color.red = 255;
                    } else if ( phase < 340 ) {
                        // Phase 1: Red→Amber (Orange)
                        uint8_t t = phase - 170;
                        config.color.red = 255 - ( t * 255 / 170 );
                        config.color.amber = t * 255 / 170;
                    } else if ( phase < 510 ) {
                        // Phase 2: Pure Amber (Warm yellow)
                        config.color.amber = 255;
                    } else if ( phase < 680 ) {
                        // Phase 3: Amber→Green
                        uint8_t t = phase - 510;
                        config.color.amber = 255 - ( t * 255 / 170 );
                        config.color.green = t * 255 / 170;
                    } else if ( phase < 850 ) {
                        // Phase 4: Green→Cyan (Green+Blue)
                        uint8_t t = phase - 680;
                        config.color.green = 255;
                        config.color.blue = t * 255 / 170;
                    } else if ( phase < 1020 ) {
                        // Phase 5: Cyan→Blue
                        uint8_t t = phase - 850;
                        config.color.green = 255 - ( t * 255 / 170 );
                        config.color.blue = 255;
                    } else if ( phase < 1190 ) {
                        // Phase 6: Blue→UV (Deep violet)
                        uint8_t t = phase - 1020;
                        config.color.blue = 255 - ( t * 255 / 170 );
                        config.color.uv = t * 255 / 170;
                    } else if ( phase < 1360 ) {
                        // Phase 7: Pure UV (Purple)
                        config.color.uv = 255;
                    } else {
                        // Phase 8: UV→Red (Magenta)
                        uint8_t t = phase - 1360;
                        config.color.uv = 255 - ( t * 255 / 170 );
                        config.color.red = t * 255 / 170;
                    }

                    patternStep++;
                    if ( patternStep >= 1530 )
                        patternStep = 0;
                }
                break;

            case PATTERN_COUNT:
            default:
                // Should not reach here
                break;
            }
        } // End of pattern mode block

        // Transmit DMX packets at configured refresh rate
        if ( now - lastTransmitTime >= config.refresh_rate_ms ) {
            lastTransmitTime = now;

            // Update universe from config
            updateDMXUniverse( universe, config );

            // Transmit the universe
            // CRITICAL: Wireless modules require the FULL 512 channels + start code (513 bytes total)
            // Sending partial universes causes the wireless module to misinterpret data
            // The Wtlx512 manual states it "Regenerates" the full DMX signal

            dmxTx.write( universe, 513 ); // Send 513 bytes with start code
            // Serial.print("Sending: ");
            // Serial.print(universe[0]);
            // Serial.print(" ");
            // Serial.print(universe[1]);
            // Serial.print(" ");
            // Serial.print(universe[2]);
            // Serial.print(" ");
            // Serial.print(universe[3]);
            // Serial.print(" ");
            // Serial.print(universe[4]);
            // Serial.print(" ");
            // Serial.print(universe[5]);
            // Serial.print(" ");
            // Serial.print(universe[6]);
            // Serial.print(" ");
            // Serial.println();
            // Wait for transmission to complete
            while ( dmxTx.busy( ) ) {
                // Busy wait - transmission takes ~23ms for full 512 channel universe
            }
        }

        // Handle user commands
        if ( Serial.available( ) > 0 ) {
            char c = Serial.read( );
            switch ( c ) {
            case 'p':
            case 'P':
                // Toggle between manual and pattern mode
                manualMode = !manualMode;
                if ( manualMode ) {
                    Serial.println( "\n\n✓ MANUAL CONTROL MODE" );
                    Serial.println( "Live Dashboard below:\n" );
                    lastEncoderPos = encoderPosition;
                    lastDisplayTime = 0; // Force immediate update
                } else {
                    Serial.println( "\n\n✓ TEST PATTERN MODE" );
                    Serial.println( "Use 1-5 to select pattern:" );
                    Serial.println( "  1=Fade  2=Cycle  3=Strobe  4=Rainbow  5=Spectrum\n" );
                    patternStep = 0;
                }
                break;

            case '1':
                if ( !manualMode ) {
                    currentPattern = PATTERN_COLOR_FADE;
                    patternStep = 0;
                    Serial.println( "Pattern: Color Fade" );
                }
                break;

            case '2':
                if ( !manualMode ) {
                    currentPattern = PATTERN_RGB_CYCLE;
                    patternStep = 0;
                    Serial.println( "Pattern: RGB Cycle" );
                }
                break;

            case '3':
                if ( !manualMode ) {
                    currentPattern = PATTERN_WHITE_STROBE;
                    patternStep = 0;
                    Serial.println( "Pattern: White Strobe" );
                }
                break;

            case '4':
                if ( !manualMode ) {
                    currentPattern = PATTERN_RAINBOW;
                    patternStep = 0;
                    Serial.println( "Pattern: Rainbow" );
                }
                break;

            case '5':
                if ( !manualMode ) {
                    currentPattern = PATTERN_SPECTRUM;
                    patternStep = 0;
                    Serial.println( "Pattern: Spectrum Cycle (RGBAUV)" );
                }
                break;

            case 'm':
            case 'M':
                // Toggle between 6 and 10 channel mode
                if ( config.channel_mode == DMXLightConfig::MODE_6CH ) {
                    config.channel_mode = DMXLightConfig::MODE_10CH;
                    Serial.println( "\n\n✓ Switched to 10-channel mode\n" );
                } else {
                    config.channel_mode = DMXLightConfig::MODE_6CH;
                    Serial.println( "\n\n✓ Switched to 6-channel mode\n" );
                }
                if ( manualMode ) {
                    currentChannel = 0;  // Reset to first channel
                    lastDisplayTime = 0; // Force immediate dashboard update
                }
                break;

            case 'a':
            case 'A':
                // Adjust DMX address (decrement)
                if ( config.dmx_address >= 1 ) {
                    config.dmx_address--;
                    Serial.print( "\n\n✓ DMX Address: " );
                    Serial.print( config.dmx_address );
                    Serial.println( "\n" );
                } else {
                    Serial.println( "\n\n⚠️  Already at minimum address (0)\n" );
                }
                if ( manualMode ) {
                    lastDisplayTime = 0; // Force immediate dashboard update
                }
                break;

            case 'z':
            case 'Z':
                // Adjust DMX address (increment)
                if ( config.dmx_address < 255 ) { // uint8_t max is 255
                    config.dmx_address++;
                    Serial.print( "\n\n✓ DMX Address: " );
                    Serial.print( config.dmx_address );
                    Serial.println( "\n" );
                } else {
                    Serial.println( "\n\n⚠️  Already at maximum address (255)\n" );
                }
                if ( manualMode ) {
                    lastDisplayTime = 0; // Force immediate dashboard update
                }
                break;

            case 'w':
            case 'W':
                // Toggle wireless mode flag (currently both modes send 513 bytes)
                // This is a placeholder for future wireless-specific handling
                config.wireless_mode = !config.wireless_mode;
                Serial.print( "\n\n✓ Mode: " );
                Serial.println( config.wireless_mode ? "WIRELESS (testing)" : "WIRED (standard)" );
                Serial.println( "  Currently both modes send 513 bytes - testing in progress.\n" );
                if ( manualMode ) {
                    lastDisplayTime = 0; // Force immediate dashboard update
                }
                break;

            case 'q':
            case 'Q':
                goto exit_loop;
            }
        }

        delay( 1 ); // Small delay to prevent tight loop
    }

exit_loop:
    // Clean shutdown
    dmxTx.end( );

    // Free DMX universe buffer
    if ( universe != nullptr ) {
        delete[] universe;
    }

    Serial.flush( );
    Serial.println( "\n✓ DMX transmitter stopped" );
    Serial.println( "Exiting DMX Light Controller" );
}

/*
Channel mapping:

6 Channel mode:
Ch 1: Red

Ch 2: Green

Ch 3: Blue

Ch 4: White

Ch 5: Amber

Ch 6: UV





12 Channel mode:
DMX Footprint (10 ch)
Ch 1: Intensity

Ch 2: Red


Ch 3: Green


Ch 4: Blue


Ch 5: White


Ch 6: Amber


Ch 7: UV


Ch 8: Shutter Strobe, slow to fast


Ch 9: Function Select 000-050: Ch1-ch7 valid


Ch 10: Function Speed 51-100: Color output (8kinds), selected by CH10






Function and Description
1: A001: 001-512: DMX512 Address Code Setting-10 Channel Mode
2: d002: 001-512: DMX512 Address Code Setting-6 Channel Mode
3: R003: 000-255: Red dimming, dim to bright.
4: G004: 000-255: Green dimming, dim to bright.
5: B005: 000-255: Blue dimming, dim to bright.
6: u006: 000-255: White dimming, dim to bright.
7: Y007: 000-255: Amber dimming, dim to bright.
8: P008: 000-255: Purple dimming, dim to bright.
9: FH99: 01-99: Strobe, slow to fast.
10: CL01: 01-08: 8 colors output.
11: CC01: 01-99: Jump change, slow to fast.
12: DE99: 01-99: Fade, slow to fast.
13: CP99: 01-99: Pulse change, slow to fast.
14: SU01: 01-09: Mode: 1-8 for color selection strobe, 9 for multi-color

*/