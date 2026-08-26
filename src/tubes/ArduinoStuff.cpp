// SPDX-License-Identifier: MIT

#include "ArduinoStuff.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "SerialUART.h"
#include "config.h"

#include "AsyncPassthrough.h"
#include "CH446Q.h"
#include "Commands.h"
#include "FileParsing.h"
#include "Graphics.h"
#include "NetManager.h"
#include "config.h"
#include "configManager.h"
#include "hardware/uart.h"
#include "usb_interface_config.h"
#include "States.h"
#include "Jerial.h"

#ifdef USE_TINYUSB
#include "tusb.h" // tud_cdc_n_* for the 1200-baud bootloader touch below
#endif
#include "pico/bootrom.h" // reset_usb_boot()

#include "hardware/structs/io_bank0.h"
#include "hardware/structs/xip.h"

// USB is pumped through TinyUSB_Device_Task() (mutex-guarded), never raw tud_task().

// #include "SerialWrapper.h"

// #include <SoftwareSerial.h>

// #if USB_CDC_ENABLE_COUNT >= 2
Adafruit_USBD_CDC USBSer1;
// #endif

// #if USB_CDC_ENABLE_COUNT >= 3
Adafruit_USBD_CDC USBSer2;
// #endif

// #if USB_CDC_ENABLE_COUNT >= 4
Adafruit_USBD_CDC USBSer3;
// #endif

// #if USB_CDC_ENABLE_COUNT >= 5
// Adafruit_USBD_CDC Serial4;
// #endif

// general debug printing
#define ARDUINO_DEBUG_PRINTLN( x )                 \
    if ( debugArduino > 0 ) {                      \
        changeTerminalColor( 38, true, &Serial );  \
        Serial.println( x );                       \
        changeTerminalColor( -1, false, &Serial ); \
    }
#define ARDUINO_DEBUG_PRINT( x )                   \
    if ( debugArduino > 0 ) {                      \
        changeTerminalColor( 38, true, &Serial );  \
        Serial.print( x );                         \
        changeTerminalColor( -1, false, &Serial ); \
    }
#define ARDUINO_DEBUG_PRINTF( x, ... )             \
    if ( debugArduino > 0 ) {                      \
        changeTerminalColor( 38, true, &Serial );  \
        Serial.printf( x, __VA_ARGS__ );           \
        changeTerminalColor( -1, false, &Serial ); \
    }
// we'll use this for sending data to the USBSer1
#define ARDUINO_DEBUG2_PRINTLN( x )                \
    if ( debugArduino > 1 ) {                      \
        changeTerminalColor( 43, true, &Serial );  \
        Serial.println( x );                       \
        changeTerminalColor( -1, false, &Serial ); \
    }
#define ARDUINO_DEBUG2_PRINT( x )                  \
    if ( debugArduino > 1 ) {                      \
        changeTerminalColor( 43, true, &Serial );  \
        Serial.print( x );                         \
        changeTerminalColor( -1, false, &Serial ); \
    }
#define ARDUINO_DEBUG2_PRINTF( x, ... )            \
    if ( debugArduino > 1 ) {                      \
        changeTerminalColor( 43, true, &Serial );  \
        Serial.printf( x, __VA_ARGS__ );           \
        changeTerminalColor( -1, false, &Serial ); \
    }

// we'll use this for sending data to the Arduino
#define ARDUINO_DEBUG3_PRINTLN( x )                \
    if ( debugArduino > 1 ) {                      \
        changeTerminalColor( 117, true, &Serial ); \
        Serial.println( x );                       \
        changeTerminalColor( -1, false, &Serial ); \
    }
#define ARDUINO_DEBUG3_PRINT( x )                  \
    if ( debugArduino > 1 ) {                      \
        changeTerminalColor( 117, true, &Serial ); \
        Serial.print( x );                         \
        changeTerminalColor( -1, false, &Serial ); \
    }
#define ARDUINO_DEBUG3_PRINTF( x, ... )            \
    if ( debugArduino > 1 ) {                      \
        changeTerminalColor( 117, true, &Serial ); \
        Serial.printf( x, __VA_ARGS__ );           \
        changeTerminalColor( -1, false, &Serial ); \
    }

#define ARDUINO_DEBUG_FLUSH( ) \
    if ( debugArduino > 0 ) {  \
        Serial.flush( );       \
    }

bool arduinoDTR[ 3 ] = { false, false, false };

bool arduinoDTRpulse = false;

int debugArduino = 0;
int connectOnBoot1 = 0;
int connectOnBoot2 = 0;
int lockConnection1 = 0;
int lockConnection2 = 0;

int baudRateUSBSer1 = 115200; // for Arduino-Serial
int baudRateUSBSer2 = 115200; // for Routable Serial

volatile int backpowered = 1;

volatile bool flashingArduino = false;



void initArduino( void ) // if the UART is set up, the Arduino won't flash from
                         // it's own USB port
{

    // Serial1.setRX(17);

    // Serial1.setTX(16);

    // Serial1.begin(115200);

    // delay(1);

    pinMode( 18, INPUT );
    SetArduinoResetLine(LOW, 1);


    // USBSer3.begin(115200);
   // pinMode( 19, INPUT );
}

bool ManualArduinoReset = false;
bool LastArduinoDTR = true;
bool LastRoutableDTR = true;
uint8_t numbitsUSBSer1 = 8;
uint8_t paritytypeUSBSer1 = 0;
uint8_t stopbitsUSBSer1 = 0;
uint8_t numbitsUSBSer2 = 8;
uint8_t paritytypeUSBSer2 = 0;
uint8_t stopbitsUSBSer2 = 0;

unsigned long FirstDTRTime = 5000;
bool FirstDTR = true;
bool ESPBoot = false;
unsigned long ESPBootTime = 5000;

unsigned long microsPerByteSerial1 = ( 1000000 / 115200 + 1 ) * ( 8 + 0 + 1 );
unsigned long microsPerByteSerial2 = ( 1000000 / 115200 + 1 ) * ( 8 + 0 + 1 );

// volatile char serialCommandBuffer[512];
volatile int serialCommandBufferIndex = 0;

int serConfigChangedUSBSer1 = 0;
int serConfigChangedUSBSer2 = 0;

// SerialPIO SerialPIO1(0, 1, 1024);

void initSecondSerial( void ) {
    // Initialize USB CDC interfaces
    // Note: AsyncPassthrough uses pico-sdk UART directly, NOT Arduino Serial1
    // So we don't start Serial1 at all - AsyncPassthrough::begin() handles UART init

#if USB_CDC_ENABLE_COUNT >= 2
    // USBSer1 maps to CDC interface 1 (Arduino Serial passthrough)
    if ( jumperlessConfig.serial_1.function != 0 ) {
        USBSer1.begin( baudRateUSBSer1 );
        // Don't start Arduino Serial1 - AsyncPassthrough uses pico-sdk UART directly
        // This avoids conflicts between Arduino's UART handling and pico-sdk IRQ-based handling
    }
#endif

#if USB_CDC_ENABLE_COUNT >= 3
    // USBSer2 maps to CDC interface 2 (Routable Serial) - conditionally
    // if ( jumperlessConfig.serial_2.function != 0 ) {
    USBSer2.begin( baudRateUSBSer2, makeSerialConfig( 8, 0, 1 ) );
    // Serial.println("  USBSer2 (Routable) initialized");
    // Serial2.begin( baudRateUSBSer2, makeSerialConfig( 8, 0, 0 ) );
    // } else {
    // Serial.println("  USBSer2 disabled by config");
    // }
#endif

#if USB_CDC_ENABLE_COUNT >= 4
    // USBSer3 maps to CDC interface 3 (Debug Serial)

    USBSer3.begin( 115200 );
    // delay(3000);//!son of a bitch
    //  Ser3.println("Serial3 initialized");

    // Serial.println("  USBSer3 (Debug) initialized");
#endif

// Give time for USB enumeration
// delay(200);

// Serial.println("Enabled USB interfaces with dynamic naming:");
// Serial.println("  Interface 0: Jumperless Main (this Serial)");
#define PRINT_SERIAL_INFO_AT_STARTUP 0
#if PRINT_SERIAL_INFO_AT_STARTUP
#if USB_CDC_ENABLE_COUNT >= 2
    Serial.print( "  Interface 1: " );
    const char* func1_name =
        getStringFromTable( jumperlessConfig.serial_1.function, uartFunctionTable );
    if ( func1_name && strcmp( func1_name, "off" ) != 0 &&
         strcmp( func1_name, "disable" ) != 0 ) {
        Serial.print( "JL " );
        // Print with first letter capitalized and underscores as spaces
        char c = func1_name[ 0 ];
        if ( c >= 'a' && c <= 'z' )
            c = c - 'a' + 'A';
        Serial.print( c );
        for ( int i = 1; func1_name[ i ]; i++ ) {
            Serial.print( func1_name[ i ] == '_' ? ' ' : func1_name[ i ] );
        }
        // Serial.println(" (USBSer1)");
    } else {
        // Serial.println("Jumperless Serial 1 (USBSer1)");
    }
#endif
#if USB_CDC_ENABLE_COUNT >= 3
    if ( jumperlessConfig.serial_2.function != 0 ) {
        Serial.print( "  Interface 2: " );
        const char* func2_name = getStringFromTable(
            jumperlessConfig.serial_2.function, uartFunctionTable );
        if ( func2_name && strcmp( func2_name, "off" ) != 0 &&
             strcmp( func2_name, "disable" ) != 0 ) {
            Serial.print( "JL " );
            // Print with first letter capitalized and underscores as spaces
            char c = func2_name[ 0 ];
            if ( c >= 'a' && c <= 'z' )
                c = c - 'a' + 'A';
            Serial.print( c );
            for ( int i = 1; func2_name[ i ]; i++ ) {
                Serial.print( func2_name[ i ] == '_' ? ' ' : func2_name[ i ] );
            }
            // Serial.println(" (USBSer2)");
        } else {
            // Serial.println("Jumperless Serial 2 (USBSer2)");
        }
    }
#endif
#if USB_CDC_ENABLE_COUNT >= 4
    // Serial.println("  Interface 3: Jumperless Debug (USBSer3)");
#endif

#if USB_MSC_ENABLE
    Serial.println( "  MSC: Mass Storage" );
#endif
#if USB_HID_ENABLE_COUNT > 0
    Serial.print( "  HID: " );
    Serial.print( USB_HID_ENABLE_COUNT );
    Serial.println( " interface(s)" );
#endif
#if USB_MIDI_ENABLE
    Serial.println( "  MIDI: MIDI Device" );
#endif
#if USB_VENDOR_ENABLE
    Serial.println( "  Vendor: Custom Interface" );
#endif

// Serial.println("\nNote: Interface names are generated dynamically based on
// your serial function configuration."); Serial.println("serial_1.function = "
// + String(jumperlessConfig.serial_1.function) +
//                ", serial_2.function = " +
//                String(jumperlessConfig.serial_2.function));
#endif
}

// Function to print current USB interface naming (can be called anytime)
void printUSBInterfaceNames( void ) {
    // Serial.println("\n=== Current USB Interface Names ===");
    Serial.println( "Interface 0: Jumperless Main" );

#if USB_CDC_ENABLE_COUNT >= 2
    Serial.print( "Interface 1: " );
    const char* func1_name =
        getStringFromTable( jumperlessConfig.serial_1.function, uartFunctionTable );
    if ( func1_name && strcmp( func1_name, "off" ) != 0 &&
         strcmp( func1_name, "disable" ) != 0 ) {
        Serial.print( "JL " );
        // Print with first letter capitalized and underscores as spaces
        char c = func1_name[ 0 ];
        if ( c >= 'a' && c <= 'z' )
            c = c - 'a' + 'A';
        Serial.print( c );
        for ( int i = 1; func1_name[ i ]; i++ ) {
            Serial.print( func1_name[ i ] == '_' ? ' ' : func1_name[ i ] );
        }
        Serial.println( );
    } else {
        Serial.println( "Jumperless Serial 1" );
    }
#endif

#if USB_CDC_ENABLE_COUNT >= 3
    if ( jumperlessConfig.serial_2.function != 0 ) {
        Serial.print( "Interface 2: " );
        const char* func2_name = getStringFromTable(
            jumperlessConfig.serial_2.function, uartFunctionTable );
        if ( func2_name && strcmp( func2_name, "off" ) != 0 &&
             strcmp( func2_name, "disable" ) != 0 ) {
            Serial.print( "JL " );
            // Print with first letter capitalized and underscores as spaces
            char c = func2_name[ 0 ];
            if ( c >= 'a' && c <= 'z' )
                c = c - 'a' + 'A';
            Serial.print( c );
            for ( int i = 1; func2_name[ i ]; i++ ) {
                Serial.print( func2_name[ i ] == '_' ? ' ' : func2_name[ i ] );
            }
            Serial.println( );
        } else {
            Serial.println( "Jumperless Serial 2" );
        }
    } else {
        Serial.println( "Interface 2: Disabled by config" );
    }
#endif

#if USB_CDC_ENABLE_COUNT >= 4
    Serial.println( "Interface 3: Jumperless TUI" );
#endif

#if USB_MSC_ENABLE
    Serial.println( "MSC: JL Mass Storage" );
#endif

    // Serial.println("=====================================\n");
}

unsigned long serial1LEDTimer = 0;
unsigned long lastSerial1TxRead = 0;
unsigned long lastSerial1RxRead = 0;
unsigned long lastSerial2TxRead = 0;
unsigned long lastSerial2RxRead = 0;
volatile int arduinoInReset = 0;
unsigned long lastTimeResetArduino = 0;

int printSerial1Passthrough = 0;
int printSerial2Passthrough = 0;

int flashArduinoNextLoop = 0;

int arduinoConnected = 0;

unsigned long lastSerial1Check = 5000;
unsigned long lastSerial2Check = 5000;

unsigned long lastSerialPassthrough = millis( );
int serialPassthroughStatus = 0;
int serialPassthroughStatusTimeout = 50;

int secondSerialHandler( void ) {
    // All serial bridging + the cap-emulation Arduino reset now happen inside
    // AsyncPassthrough (DMA bridge + checkDTRState()). This handler only does the
    // two things that must run on Core 0's main loop:
    //   1. one-time UART crossbar auto-connect when a flash starts, and
    //   2. polling for flash completion to release the Arduino to normal mode.

    #ifdef USE_TINYUSB
    // ── 1200-baud bootloader touch on the MicroPython REPL port (CDC 2) ──
    // Adafruit TinyUSB only honors the classic "open at 1200 baud, then
    // close" reset convention on CDC instance 0 (Jumperless Main), but
    // JumperIDE talks to CDC 2 — honor the same convention there so the IDE
    // can reboot us into BOOTSEL for an in-browser PICOBOOT flash. The MSD
    // interface is disabled (mask bit 0) so the host never mounts a drive;
    // a manual BOOTSEL (button held at power-up) still exposes the drive
    // for drag-and-drop. CDC 1 is deliberately excluded: avrdude does its
    // own 1200-baud games through the UART passthrough when flashing an
    // attached Arduino.
    if ( !tud_cdc_n_connected( 2 ) ) {
        cdc_line_coding_t coding;
        tud_cdc_n_get_line_coding( 2, &coding );
        if ( coding.bit_rate == 1200 ) {
            reset_usb_boot( 0, 1 ); // bit 0 = disable BOOTSEL MSD, PICOBOOT only
        }
    }
    #endif

    #if ASYNC_PASSTHROUGH_ENABLED == 1
    // A DTR rising edge already reset the Arduino and entered flash mode (in
    // checkDTRState). Here we just ensure the UART is routed to the Arduino.
    if ( AsyncPassthrough::wasDTRPulseDetected() ) {
        AsyncPassthrough::clearDTRPulse();
        flashArduino( 0 );  // non-blocking: one-time auto-connect (+ ESP32 reset dance)
    }

    // Poll for flash completion (STK500 protocol watch / inactivity / no-data abort).
    // Replaces the old blocking servicing loop - the DMA bridge self-pumps.
    if ( flashingArduino && AsyncPassthrough::checkFlashDone() ) {
        // Release ESP32 boot-strap lines if this was an ESP32 flash.
        if ( jumperlessConfig.serial_1.flash_reset_type == 2 ) {
            SetArduinoResetLine( HIGH, 2 );
        }
        AsyncPassthrough::exitFlashMode();
        AsyncPassthrough::setDTRLockout( 2000 );  // ignore the port-close DTR toggle
        flashingArduino = false;
        // Tag parsing re-enables automatically via the inactivity logic in
        // AsyncPassthrough::task() once the Arduino has had its boot delay.
    }
    #endif

    return 0;
}

char arduinoCommandStrings[ 10 ][ 50 ] = {
    // commands to watch for from the Arduino
    "jumperlessConfig.serial_1.function",
    "jumperlessConfig.serial_1.connect_on_boot",
    "jumperlessConfig.serial_1.lock_connection",

};



void flashArduino( unsigned long timeoutTime ) {
    // =========================================================================
    // ARDUINO FLASH START (non-blocking)
    // =========================================================================
    // The cap-emulation reset and flash-mode entry already happened instantly
    // in AsyncPassthrough::checkDTRState() the moment avrdude asserted DTR. The
    // DMA bridge self-pumps the bytes both ways, so there is NO blocking
    // servicing loop here anymore (that loop was the "hang" and the missed
    // bootloader window). All this does now is the one-time heavy work that
    // must run on Core 0: route the UART crossbar to the Arduino, and - for
    // ESP32 targets - perform the B1+RST download-mode dance.
    // Completion is polled in secondSerialHandler() via checkFlashDone().
    // =========================================================================
    (void)timeoutTime;

    ARDUINO_DEBUG_PRINTLN( "Arduino DTR pulse - flash start (DMA bridge, non-blocking)" );

    // Make sure the UART is actually routed to the Arduino so bytes can flow.
    arduinoConnected = checkIfArduinoIsConnected( );
    if ( arduinoConnected == 0 && jumperlessConfig.serial_1.autoconnect_flashing == 1 ) {
        connectArduino( 1, 1 );
        arduinoConnected = checkIfArduinoIsConnected( );
    }

    // ESP32 targets (flash_reset_type == 2) need the blocking B1+RST sequence,
    // which can't be done as a simple GPIO pulse in checkDTRState(). AVR
    // (type 1) was already reset there; type 0 does no reset.
    if ( jumperlessConfig.serial_1.flash_reset_type == 2 ) {
        ESPReset();
        AsyncPassthrough::drainUARTRxBuffers();
    }

    flashingArduino = true;  // (already set in checkDTRState; keep consistent)
}

char commandStartString[] = "`[";

char commandString[ 256 ];

int USBSer1Available = 0;
int Serial1Available = 0;
int countCheck = 0;

// REMOVED: handleSerialPassthrough() - deprecated, async passthrough is now always used
// Data bridging is handled by AsyncPassthrough::task() via asyncPassthroughService
void resetArduino( int topBottomBoth, unsigned long holdMicroseconds ) {
    SetArduinoResetLine( LOW, topBottomBoth );
    delayMicroseconds( holdMicroseconds );
    SetArduinoResetLine( HIGH, topBottomBoth );
}

void printMicrosPerByte( void ) {
    Serial.println( );
    checkForConfigChangesUSBSer1( 2 );
    Serial.println( "uS per byte    = (1000000 /  baud  + 1)     * (numbits + "
                    "stopbits + paritybits)" );

    Serial.print( "uS per byte    = (1000000 / " );
    Serial.print( baudRateUSBSer1 );
    Serial.print( " + 1 = " );
    Serial.print( 1000000 / baudRateUSBSer1 + 1 );
    Serial.print( ") * (   " );
    Serial.print( numbitsUSBSer1 );
    Serial.print( "     +    " );
    Serial.print( stopbitsUSBSer1 );
    Serial.print( "    +     " );
    Serial.print( paritytypeUSBSer1 == 0 ? 0 : 1 );
    Serial.print( "      = " );
    Serial.print( numbitsUSBSer1 + stopbitsUSBSer1 +
                  ( paritytypeUSBSer1 == 0 ? 0 : 1 ) );
    Serial.print( ")  =  " );
    Serial.println( microsPerByteSerial1 );

    Serial.println( );
    Serial.print( "microsPerByteSerial1: " );
    Serial.println( microsPerByteSerial1 );
    Serial.println( );

    // Serial.print("microsPerByteSerial2: ");
    // Serial.println(microsPerByteSerial2);
}

// void toggleArduinoResetLine(void){
//   pinMode(ARDUINO_RESET_0_PIN, OUTPUT_12MA);
//   pinMode(ARDUINO_RESET_1_PIN, OUTPUT_12MA);
//   digitalWrite(ARDUINO_RESET_0_PIN, HIGH);
//   digitalWrite(ARDUINO_RESET_1_PIN, HIGH);
//   delay(1);
//   digitalWrite(ARDUINO_RESET_0_PIN, LOW);
//   digitalWrite(ARDUINO_RESET_1_PIN, LOW);
// }

void connectArduino( int flashOrLocal, int refreshConnections ) {

    // Use RAM-based state system
    // CRITICAL: Pass autoRefresh=false to avoid triggering refreshLocalConnections()
    // on EACH addBridgeToState call. Without this, 3 sequential refreshes happen
    // (2 auto + 1 explicit), each with zero tud_task() during compute = 30-150ms
    // of USB starvation that causes macOS to disconnect the CDC device.
    addBridgeToState( RP_UART_RX, NANO_D1 , 0, false);
    addBridgeToState( RP_UART_TX, NANO_D0, 0, false);
    // refresh( flashOrLocal, -1, 1, 0 );  // Single combined refresh for both bridges
    refreshLocalConnections(-1, 1);

    // CRITICAL FIX: Reduced timeout and service AsyncPassthrough during wait
    // The original 2-second timeout was blocking AsyncPassthrough::task()
    // which prevented data bridging during Arduino flashing
    unsigned long connectTimeout = millis();
    const unsigned long CONNECT_TIMEOUT_MS = 500;  // Reduced to 500ms
    
    while ( checkIfArduinoIsConnected( ) == 0 ) {
        // CRITICAL: Service AsyncPassthrough to keep data bridging active
        // Without this, avrdude's sync bytes don't reach Arduino during flashing
        #if ASYNC_PASSTHROUGH_ENABLED == 1
        AsyncPassthrough::task();
        #endif
        
        // Also service USB directly (mutex-guarded pump)
        TinyUSB_Device_Task();
        
        // Timeout check to prevent infinite loop
        if (millis() - connectTimeout > CONNECT_TIMEOUT_MS) {
            // Timeout - Arduino connection state not detected, but bridges are added
            // Continue anyway - the hardware connection should still work
            break;
        }
        
        delayMicroseconds(50);  // Small yield
    }
}

void disconnectArduino( int flashOrLocal ) {

    // Use RAM-based state system
    removeBridgeFromState( NANO_D1, RP_UART_RX );
    removeBridgeFromState( NANO_D0, RP_UART_TX );
    // State functions already refresh, but do explicit refresh for safety
    refreshLocalConnections(-1, 1);
    
    // if (flashOrLocal == 1) {
    //   refreshLocalConnections(1, 0);
    //   } else {
    //   refreshConnections(1, 0);
    //   }
    
    // refreshBlind(1, 0);
    // sendPaths();
    // waitCore2();
    // sendPaths();
}

int checkIfArduinoIsConnected( void ) {
    

        int connected = globalState.hasConnection( NANO_D1, RP_UART_RX );
    connected += globalState.hasConnection( NANO_D0, RP_UART_TX );
    // Serial.println("connected: " + String(connected));
    if ( connected == 2 ) {
        return 1;
    }
    return 0;
}


int arduinoPresence = 0;

// Settle time after releasing a reset line to INPUT before sampling it. The line
// is frequently driven LOW just before this (boot hold, flash auto-reset, manual
// reset) and only rises again through the Arduino's weak (~20-50k) reset pull-up
// fighting the breadboard/wiring capacitance — an RC of tens to hundreds of µs.
// The old 10µs settle sampled before the line had charged, so a present Arduino
// read back LOW (false "not present"). A couple ms is ample and is cheap: this
// only runs on a presence query (DC4 / A?), not in any hot path.
#define ARDUINO_RESET_SENSE_SETTLE_US 2000

int checkArduinoResetPin0( void ) {
    // Arduino reset pin 0 should be pulled high when Arduino is present
    pinMode( ARDUINO_RESET_0_PIN, INPUT );
    delayMicroseconds( ARDUINO_RESET_SENSE_SETTLE_US ); // Let the pull-up charge the line
    return digitalRead( ARDUINO_RESET_0_PIN );
}

int checkArduinoResetPin1( void ) {
    // Skip if PSRAM mod is installed - GPIO 19 is used for PSRAM CS
    if ( jumperlessConfig.hardware.psram_installed ) {
        return -1; // Return -1 to indicate pin not available
    }
    // Arduino reset pin 1 should be pulled high when Arduino is present
    pinMode( ARDUINO_RESET_1_PIN, INPUT );
    delayMicroseconds( ARDUINO_RESET_SENSE_SETTLE_US ); // Let the pull-up charge the line
    return digitalRead( ARDUINO_RESET_1_PIN );
}

int checkArduinoPresence( void ) {
    // Check if either Arduino (top or bottom slot) is present
    // Returns: 1 if Arduino detected, 0 if not
    int pin0 = checkArduinoResetPin0( );
    int pin1 = checkArduinoResetPin1( ); // Returns -1 if PSRAM mod installed
    
    // Arduino pulls reset line high via internal pullup
    // If both are low, no Arduino is present
    // pin1 == -1 means PSRAM mod installed, only check pin0
    if ( pin0 == HIGH || ( pin1 != -1 && pin1 == HIGH ) ) {
        
        return 1;
    }
    return 0;
}

void autoConnectArduino( void ) {
    if ( checkArduinoPresence( ) == 1 && arduinoPresence == 0 ) {
        arduinoPresence = 1;
        connectArduino( 1, 0 );
    }
}

void SetArduinoResetLine( bool state, int topBottomBoth ) {
    if ( state == LOW ) {
        // Serial.println("Setting Arduino Reset Line to LOW");
        if ( topBottomBoth == 1 || topBottomBoth == 2 ) {
            pinMode( ARDUINO_RESET_0_PIN, OUTPUT_12MA );
            digitalWrite( ARDUINO_RESET_0_PIN, LOW );
            rstColors[ 1 ] = 0x002a10;
        }
        // Skip RESET_1 if PSRAM mod installed - GPIO 19 is used for PSRAM CS
        if ( !jumperlessConfig.hardware.psram_installed && ( topBottomBoth == 0 || topBottomBoth == 2 ) ) {
            pinMode( ARDUINO_RESET_1_PIN, OUTPUT_12MA );
            digitalWrite( ARDUINO_RESET_1_PIN, LOW );
            rstColors[ 0 ] = 0x002a10;
        }

        requestLedShow( 2 );

        delayMicroseconds( 1000 );

    } else if ( state == HIGH ) {
        // Serial.println("Setting Arduino Reset Line to HIGH");
        //  digitalWrite(ARDUINO_RESET_0_PIN, HIGH);
        //  digitalWrite(ARDUINO_RESET_1_PIN, HIGH);

        if ( topBottomBoth == 1 || topBottomBoth == 2 ) {
            pinMode( ARDUINO_RESET_0_PIN, INPUT );
        }
        // Skip RESET_1 if PSRAM mod installed - GPIO 19 is used for PSRAM CS
        if ( !jumperlessConfig.hardware.psram_installed && ( topBottomBoth == 0 || topBottomBoth == 2 ) ) {
            pinMode( ARDUINO_RESET_1_PIN, INPUT );
        }
        // headerColors[0] = 0x2000b9;
        // headerColors[1] = 0x0020f9;
        // requestLedShow( 2 );
    }
}

// Arduino Nano ESP32 bootloader sequence: B1 (boot strap) to GND first, then
// pulse RST. RESET_1 = B1, RESET_0 = RST. B1 must stay low so chip enters
// download mode on reset; release RST after ~50 ms.
void ESPReset( ) {
    ARDUINO_DEBUG_PRINTLN( "ESP32 bootloader: B1 low, pulse RST" );
    // Skip RESET_1 (B1) if PSRAM mod installed - GPIO 19 is used for PSRAM CS
    if ( !jumperlessConfig.hardware.psram_installed ) {
        pinMode( ARDUINO_RESET_1_PIN, OUTPUT_12MA );
        digitalWrite( ARDUINO_RESET_1_PIN, LOW );  // B1 = GND (boot strap, hold)
        rstColors[ 0 ] = 0x002a10;
    }
    pinMode( ARDUINO_RESET_0_PIN, OUTPUT_12MA );
    digitalWrite( ARDUINO_RESET_0_PIN, LOW );     // RST low
    rstColors[ 1 ] = 0x002a10;
    requestLedShow( 2 );
    delay( 50 );   // Hold RST low ~50 ms
    pinMode( ARDUINO_RESET_0_PIN, INPUT );        // Release RST; B1 stays low → download mode
    // RESET_1 (B1) left LOW until flash is done (released in flashArduino cleanup)
}

void applyPsramModeChange( int psramEnabled ) {
    // Called when psram_installed config changes at runtime
    // GPIO 19 is shared between NANO_RESET_1 and PSRAM_CS
    
    if ( psramEnabled ) {
#if defined(PICO_RP2350)
        // XIP CS1 (second QSPI chip select for PSRAM) only exists on RP2350.
        // The OG Jumperless is an RP2040 with no PSRAM, so this path is dead
        // there; psram_installed is always 0 on that board.
        gpio_set_function( ARDUINO_RESET_1_PIN, GPIO_FUNC_XIP_CS1 );
        xip_ctrl_hw->ctrl|=XIP_CTRL_WRITABLE_M1_BITS;

        Serial.println( "PSRAM mode enabled - GPIO 19 released for PSRAM CS" );
        Serial.println( "Note: Top Arduino slot reset (NANO_RESET_1) is now disabled" );
#else
        Serial.println( "PSRAM mode unavailable on this board (no XIP CS1)" );
#endif
    } else {
        pinMode( ARDUINO_RESET_1_PIN, INPUT );
        Serial.println( "PSRAM mode disabled - GPIO 19 restored as NANO_RESET_1" );
        Serial.println( "Note: Top Arduino slot reset is now available" );
    }

    applyHeaderColorsForPsram();
    requestLedShow( 2 );
}

void setBaudRate( int baudRate ) {}

void arduinoPrint( void ) {}

void uploadArduino( void ) {}

uint16_t makeSerialConfig( uint8_t numbits, uint8_t paritytype,
                           uint8_t stopbits ) {
    uint16_t config = 0;

    //   #define SERIAL_PARITY_EVEN   (0x1ul)
    // #define SERIAL_PARITY_ODD    (0x2ul)
    // #define SERIAL_PARITY_NONE   (0x3ul)
    // #define SERIAL_PARITY_MARK   (0x4ul)
    // #define SERIAL_PARITY_SPACE  (0x5ul)
    // #define SERIAL_PARITY_MASK   (0xFul)

    // #define SERIAL_STOP_BIT_1    (0x10ul)
    // #define SERIAL_STOP_BIT_1_5  (0x20ul)
    // #define SERIAL_STOP_BIT_2    (0x30ul)
    // #define SERIAL_STOP_BIT_MASK (0xF0ul)

    // #define SERIAL_DATA_5        (0x100ul)
    // #define SERIAL_DATA_6        (0x200ul)
    // #define SERIAL_DATA_7        (0x300ul)
    // #define SERIAL_DATA_8        (0x400ul)
    // #define SERIAL_DATA_MASK     (0xF00ul)
    // #define SERIAL_5N1           (SERIAL_STOP_BIT_1 | SERIAL_PARITY_NONE  |
    // SERIAL_DATA_5)

    unsigned long parity = 0x3ul;
    unsigned long stop = 0x10ul;
    unsigned long data = 0x400ul;

    switch ( numbits ) {
    case 5:
        data = 0x100ul;
        break;
    case 6:
        data = 0x200ul;
        break;
    case 7:
        data = 0x300ul;
        break;
    case 8:
        data = 0x400ul;
        break;
    default:
        data = 0x400ul;
        break;
    }

    switch ( paritytype ) {
    case 0:
        parity = 0x3ul;
        break;
    case 2:
        parity = 0x1ul;
        break;
    case 1:
        parity = 0x2ul;
        break;
    case 3:
        parity = 0x3ul;
        break;
    case 4:
        parity = 0x4ul;
        break;
    case 5:
        parity = 0x5ul;
        break;
    default:
        parity = 0x3ul;
        break;
    }

    switch ( stopbits ) {
    case 1:
        stop = 0x10ul;
        break;
    case 2:
        stop = 0x30ul;
        break;
    default:
        stop = 0x10ul;
        // stopbits = 1; // default to 1 stop bit
        break;
    }

    config = data | parity | stop;

    return config;
}

uint16_t getSerial1Config( void ) {

    uint8_t numbits = USBSer1.numbits( );
    uint8_t paritytype = USBSer1.paritytype( );
    uint8_t stopbits = USBSer1.stopbits( );

    return makeSerialConfig( numbits, paritytype, stopbits );
}

uint16_t getSerial2Config( void ) {

    uint8_t numbits = USBSer2.numbits( );
    uint8_t paritytype = USBSer2.paritytype( );
    uint8_t stopbits = USBSer2.stopbits( );

    return makeSerialConfig( numbits, paritytype, stopbits );
}

void checkForConfigChangesUSBSer1( int print ) {

    if ( USBSer1.numbits( ) != numbitsUSBSer1 ) {
        numbitsUSBSer1 = USBSer1.numbits( );
        serConfigChangedUSBSer1 = 1;
    }

    if ( USBSer1.paritytype( ) != paritytypeUSBSer1 ) {
        paritytypeUSBSer1 = USBSer1.paritytype( );
        serConfigChangedUSBSer1 = 1;
    }

    if ( USBSer1.stopbits( ) + 1 != stopbitsUSBSer1 ) {
        stopbitsUSBSer1 = USBSer1.stopbits( ) + 1;
        serConfigChangedUSBSer1 = 1;
    }

    if ( USBSer1.baud( ) != baudRateUSBSer1 ) {
        baudRateUSBSer1 = USBSer1.baud( );
        // microsPerByteSerial1 = (1000000 / baudRateUSBSer1 + 1) * (numbitsUSBSer1
        // + stopbitsUSBSer1 + paritytypeUSBSer1==0?0:1);
        //  USBSer1.begin(baudRate);
        serConfigChangedUSBSer1 = 1;
    }

    if ( serConfigChangedUSBSer1 == 1 && jumperlessConfig.serial_1.function != 0 ) {
        USBSer1.begin(
            baudRateUSBSer1,
            makeSerialConfig( numbitsUSBSer1, paritytypeUSBSer1, stopbitsUSBSer1 ) );
        // No Serial1.begin() here: uart0 belongs to AsyncPassthrough's DMA bridge
        // (see initSecondSerial). Starting Arduino's Serial1 re-inits uart0 and
        // installs an exclusive UART0_IRQ that steals passthrough bytes until
        // reboot. AsyncPassthrough tracks the host's line coding itself.
        microsPerByteSerial1 =
            ( 1000000 / baudRateUSBSer1 + 1 ) *
            ( numbitsUSBSer1 + stopbitsUSBSer1 + ( paritytypeUSBSer1 == 0 ? 0 : 1 ) );
        serConfigChangedUSBSer1 = 0;

        if ( print > 0 && millis( ) > 4000 ) {
            if ( print == 1 ) {
                ARDUINO_DEBUG_PRINT( "Serial1 config changed " );
                if ( debugArduino > 1 ) {
                    ARDUINO_DEBUG_PRINT( "to " );
                }
            } else if ( print == 2 ) {
                ARDUINO_DEBUG_PRINT( "Serial1 config = " );
            }

            ARDUINO_DEBUG2_PRINTF( "%d ", baudRateUSBSer1 );
            // ARDUINO_DEBUG_PRINT( " " );

            ARDUINO_DEBUG2_PRINTF( "%d", numbitsUSBSer1 );
            switch ( paritytypeUSBSer1 ) {
            case 0:
                ARDUINO_DEBUG2_PRINT( "N" );
                break;
            case 1:
                ARDUINO_DEBUG2_PRINT( "O" );
                break;
            case 2:
                ARDUINO_DEBUG2_PRINT( "E" );
                break;
            case 3:
                ARDUINO_DEBUG2_PRINT( "M" );
                break;
            case 4:
                ARDUINO_DEBUG2_PRINT( "S" );
                break;
            default:
                ARDUINO_DEBUG2_PRINT( "N" );
                break;
            }

            ARDUINO_DEBUG2_PRINTF( "%d\n\r", stopbitsUSBSer1 );
            ARDUINO_DEBUG_FLUSH( );
        }

        // delay(1);
        // } else if (serConfigChangedUSBSer1 == 1) {
        //   serConfigChangedUSBSer1 = 2;
        //   delay(1);
        //   } else if (serConfigChangedUSBSer1 == 2) {
        //     serConfigChangedUSBSer1 = 3;
        //     delay(1);
    } else if ( print == 2 ) {
        ARDUINO_DEBUG_PRINT( "Serial1 config = " );

        ARDUINO_DEBUG3_PRINTF( "%d ", baudRateUSBSer1 );
        // ARDUINO_DEBUG_PRINT( " " );

        ARDUINO_DEBUG3_PRINTF( "%d", numbitsUSBSer1 );
        switch ( paritytypeUSBSer1 ) {
        case 0:
            ARDUINO_DEBUG3_PRINT( "N" );
            break;
        case 1:
            ARDUINO_DEBUG3_PRINT( "O" );
            break;
        case 2:
            ARDUINO_DEBUG3_PRINT( "E" );
            break;
        case 3:
            ARDUINO_DEBUG3_PRINT( "M" );
            break;
        case 4:
            ARDUINO_DEBUG3_PRINT( "S" );
            break;
        default:
            ARDUINO_DEBUG3_PRINT( "N" );
            break;
        }

        ARDUINO_DEBUG3_PRINTF( "%d\n\r", stopbitsUSBSer1 );
        ARDUINO_DEBUG_FLUSH( );
    }
}

void checkForConfigChangesUSBSer2( int print ) {

    if ( USBSer2.numbits( ) != numbitsUSBSer2 ) {
        numbitsUSBSer2 = USBSer2.numbits( );
        serConfigChangedUSBSer2 = 1;
    }

    if ( USBSer2.paritytype( ) != paritytypeUSBSer2 ) {
        paritytypeUSBSer2 = USBSer2.paritytype( );
        serConfigChangedUSBSer2 = 1;
    }

    if ( USBSer2.stopbits( ) != stopbitsUSBSer2 ) {
        stopbitsUSBSer2 = USBSer2.stopbits( );
        serConfigChangedUSBSer2 = 1;
    }

    if ( USBSer2.baud( ) != baudRateUSBSer2 ) {
        baudRateUSBSer2 = USBSer2.baud( );
        // microsPerByteSerial2 = 1000000 / baudRateUSBSer2 + 1;
        //  USBSer1.begin(baudRate);
        serConfigChangedUSBSer2 = 1;
    }

    if ( serConfigChangedUSBSer2 == 1 && jumperlessConfig.serial_2.function != 0 ) {
        USBSer2.begin(
            baudRateUSBSer2,
            makeSerialConfig( numbitsUSBSer2, paritytypeUSBSer2, stopbitsUSBSer2 ) );
        Serial2.begin(
            baudRateUSBSer2,
            makeSerialConfig( numbitsUSBSer2, paritytypeUSBSer2, stopbitsUSBSer2 ) );
        microsPerByteSerial2 =
            ( 1000000 / baudRateUSBSer2 + 1 ) *
            ( numbitsUSBSer2 + stopbitsUSBSer2 + ( paritytypeUSBSer2 == 0 ? 0 : 1 ) );
        serConfigChangedUSBSer2 = 0;

        if ( print > 0 && millis( ) > 2000 ) {
            ARDUINO_DEBUG_PRINT( "Serial2 config changed " );
            if ( debugArduino > 1 ) {
                ARDUINO_DEBUG_PRINT( "to " );
            }
            ARDUINO_DEBUG2_PRINTF( "%d ", baudRateUSBSer2 );
            // ARDUINO_DEBUG_PRINT( " " );

            ARDUINO_DEBUG2_PRINTF( "%d ", numbitsUSBSer2 );
            switch ( paritytypeUSBSer2 ) {
            case 0:
                ARDUINO_DEBUG2_PRINT( "N" );
                break;
            case 1:
                ARDUINO_DEBUG2_PRINT( "O" );
                break;
            case 2:
                ARDUINO_DEBUG2_PRINT( "E" );
                break;
            case 3:
                ARDUINO_DEBUG2_PRINT( "M" );
                break;
            case 4:
                ARDUINO_DEBUG2_PRINT( "S" );
                break;
            default:
                ARDUINO_DEBUG2_PRINT( "N" );
                break;
            }
            ARDUINO_DEBUG2_PRINTF( "%d\n\r", stopbitsUSBSer2 );
            ARDUINO_DEBUG_FLUSH( );
        }
        /// delay(10);
    } else if ( serConfigChangedUSBSer2 == 1 ) {
        serConfigChangedUSBSer2 = 2;
        /// delay(10);
    } else if ( serConfigChangedUSBSer2 == 2 ) {
        serConfigChangedUSBSer2 = 3;
        /// delay(10);
    }
}

void replyWithSerialInfo( int force ) {

    // if (flashingArduino == true) {
    //   return;
    // }

    // Check main Serial (CDC 0) for ENQ character - responds for ALL ports
    if ( Serial.available( ) > 0 || force == 1 ) {
        char c = Serial.peek( ); // Look at the character without removing it
        
        // Handle DC4 (0x14) for Arduino presence check - fast response
        if ( c == 0x14 ) {       // DC4 (Device Control 4) character
            Serial.read( );      // Remove the DC4 character from buffer
            
            // Quick response for Arduino presence check
            int isConnected = checkIfArduinoIsConnected();
            int isPresent = checkArduinoPresence();
            
            Serial.print(isConnected ? "Y," : "n,");
            Serial.println(isPresent ? "Y" : "n");
            Serial.flush();
            return;
        }
        
        if ( c == 0x05 ) {       // ENQ character
            Serial.read( );      // Remove the ENQ character from buffer

            // Report all enabled serial ports
            Serial.println( "CDC0: Jumperless Main" );

#if USB_CDC_ENABLE_COUNT >= 2
            if ( jumperlessConfig.serial_1.function != 0 ) {
                const char* func1_name = getStringFromTable(
                    jumperlessConfig.serial_1.function, uartFunctionTable );
                if ( func1_name && strcmp( func1_name, "off" ) != 0 &&
                     strcmp( func1_name, "disable" ) != 0 ) {
                    Serial.print( "CDC1: JL " );
                    // Print with first letter capitalized and underscores as spaces
                    char c = func1_name[ 0 ];
                    if ( c >= 'a' && c <= 'z' )
                        c = c - 'a' + 'A';
                    Serial.print( c );
                    for ( int i = 1; func1_name[ i ]; i++ ) {
                        Serial.print( func1_name[ i ] == '_' ? ' ' : func1_name[ i ] );
                    }
                    Serial.println( );
                } else {
                    Serial.println( "CDC1: Jumperless Serial 1" );
                }
            } else {
                Serial.println( "CDC1: Disabled" );
            }
#endif

#if USB_CDC_ENABLE_COUNT >= 3
            if ( jumperlessConfig.serial_2.function != 0 ) {
                const char* func2_name = getStringFromTable(
                    jumperlessConfig.serial_2.function, uartFunctionTable );
                if ( func2_name && strcmp( func2_name, "off" ) != 0 &&
                     strcmp( func2_name, "disable" ) != 0 ) {
                    Serial.print( "CDC2: JL " );
                    // Print with first letter capitalized and underscores as spaces
                    char c = func2_name[ 0 ];
                    if ( c >= 'a' && c <= 'z' )
                        c = c - 'a' + 'A';
                    Serial.print( c );
                    for ( int i = 1; func2_name[ i ]; i++ ) {
                        Serial.print( func2_name[ i ] == '_' ? ' ' : func2_name[ i ] );
                    }
                    Serial.println( );
                } else {
                    Serial.println( "CDC2: Jumperless Serial 2" );
                }
            } else {
                Serial.println( "CDC2: Disabled" );
            }

#endif

#if USB_CDC_ENABLE_COUNT >= 4
            Serial.println( "CDC3: Jumperless TUI" );
#endif
            Serial.flush( );
        }
    }
#if USB_CDC_ENABLE_COUNT >= 2
    // delay(100);
    // Check USBSer1 (CDC 1) for ENQ character - responds only for itself
    if ( USBSer1.available( ) > 0 && flashingArduino == false && millis( ) < 2000 ) {
        char c = USBSer1.peek( ); // Look at the character without removing it
        if ( c == 0x05 ) {        // ENQ character
            USBSer1.read( );      // Remove the ENQ character from buffer

            // Generate dynamic name based on config
            const char* func1_name = getStringFromTable(
                jumperlessConfig.serial_1.function, uartFunctionTable );
            if ( func1_name && strcmp( func1_name, "off" ) != 0 &&
                 strcmp( func1_name, "disable" ) != 0 ) {
                USBSer1.print( "CDC1: JL " );
                // Print with first letter capitalized and underscores as spaces
                char c = func1_name[ 0 ];
                if ( c >= 'a' && c <= 'z' )
                    c = c - 'a' + 'A';
                USBSer1.print( c );
                for ( int i = 1; func1_name[ i ]; i++ ) {
                    USBSer1.print( func1_name[ i ] == '_' ? ' ' : func1_name[ i ] );
                }
                USBSer1.println( );
            } else {
                USBSer1.println( "CDC1: Jumperless Serial 1" );
            }
            USBSer1.flush( );
        }
    }
#endif

#if USB_CDC_ENABLE_COUNT >= 3
    // delay(100);
    // Check USBSer2 (CDC 2) for ENQ character - responds only for itself
    if ( jumperlessConfig.serial_2.function != 0 && USBSer2.available( ) > 0 ) {
        char c = USBSer2.peek( ); // Look at the character without removing it
        if ( c == 0x05 ) {        // ENQ character
            USBSer2.read( );      // Remove the ENQ character from buffer

            // Generate dynamic name based on config
            const char* func2_name = getStringFromTable(
                jumperlessConfig.serial_2.function, uartFunctionTable );
            if ( func2_name && strcmp( func2_name, "off" ) != 0 &&
                 strcmp( func2_name, "disable" ) != 0 ) {
                USBSer2.print( "CDC2: JL " );
                // Print with first letter capitalized and underscores as spaces
                char c = func2_name[ 0 ];
                if ( c >= 'a' && c <= 'z' )
                    c = c - 'a' + 'A';
                USBSer2.print( c );
                for ( int i = 1; func2_name[ i ]; i++ ) {
                    USBSer2.print( func2_name[ i ] == '_' ? ' ' : func2_name[ i ] );
                }
                USBSer2.println( );
            } else {
                USBSer2.println( "CDC2: Jumperless Serial 2" );
            }
            USBSer2.flush( );
        }
    }

#endif
}

int checkForArduinoCommands( uint8_t serialBuffer[], int serialBufferIndex ) {
    for ( int i = 0; i < 10; i++ ) {
        if ( strcasecmp( arduinoCommandStrings[ i ], (const char*)serialBuffer ) == 0 ) {
            Serial.println( "Arduino command received" );
            Serial.println( arduinoCommandStrings[ i ] );
            Serial.println( );
            return i;
        }
    }
    Serial.println( "Arduino command not found" );
    Serial.println( (const char*)serialBuffer );
    Serial.println( );
    Serial.flush( );
    return -1;
}