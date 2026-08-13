#include "oled.h"
#include "OledGui.h"   // persistent idle screen takes the logo's place
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1306.h"
#include "Apps.h"
#include "ArduinoStuff.h"
#include "CH446Q.h"
#include "Commands.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"  // For safe file operations
#include "Graphics.h"
#include "JumperlOS.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "RotaryEncoder.h"
#include "States.h"
#include "WaveGen.h" // shared-I2C0 gate in show() checks wavegen.isRunning()
#include "Wire.h"
#include "config.h"
#include "configManager.h"

// Debug flag for OLED reconnection diagnostics - set to 1 to enable debug output
#define OLED_DEBUG 0
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
// #include "font_bounds_example.h"
#include "Adafruit_GFX.h"
#include "Highlighting.h"
#include <FatFS.h>
bool oledConnected = false;
bool oledUsingHardwiredPins = false; // Global flag: true if using RP6/RP7 (GPIO 6/7), false if using crossbar

// USBSer3 framebuffer streaming state (see oled.h / SingleCharCommands.cpp)
volatile bool oledSer3Stream = false;
volatile uint8_t oledSer3Enc = OLED_ENC_QUARTER;
Stream* oledSer3Target = nullptr;
static uint32_t oledSer3StreamHash = 0;  // last streamed frame hash (change detection)

#define OLED_RESET -1       // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32

// Enable per-line font scaling to fit horizontally (prevents wrapping)
#define OLED_SCALE_LINES_INDEPENDENTLY 1

// Dynamic Wire selection - pointer allows switching between Wire (I2C0) and Wire1 (I2C1) at runtime
// Initialize with Wire1 immediately to prevent nullptr crashes when OLED is disabled.
// NOTE: This static instance uses the Adafruit_SSD1306 default clkAfter=100kHz,
// which drops the bus to 100kHz between transfers and makes everything sluggish.
// initDisplayForConnectionType() replaces this with a dynamic instance that pins
// clkAfter to 400kHz on the very first OLED init, so the static is only used as
// a nullptr-safety placeholder.
static Adafruit_SSD1306 _defaultDisplay(128, 32, &Wire1, -1);
static Adafruit_SSD1306* _displayPtr = &_defaultDisplay;
static int _currentDisplayWire = 1;  // Track which Wire is currently active (0 = Wire, 1 = Wire1)
static bool _displayIsDynamic = false;  // Track if _displayPtr was dynamically allocated

// Target I2C clock for OLED transfers. We pin BOTH clkDuring and clkAfter to
// the same value so the SSD1306 driver doesn't toggle the bus speed every
// transaction (its default is 400kHz during, 100kHz after). The 100kHz "after"
// speed makes subsequent transfers visibly sluggish because every call has to
// reconfigure the I2C peripheral back up to 400kHz before sending.
static constexpr uint32_t kOledI2CClockHz = 400000;

// Function to get display reference - avoids macro conflicts with display() method name
// Declared in oled.h for use by other files
Adafruit_SSD1306& getDisplay() { return *_displayPtr; }

// Initialize or reinitialize display with the correct Wire based on connection_type
// Returns true if display was (re)created, false if already using correct Wire.
//
// The new dynamic instance is constructed with explicit clkDuring/clkAfter so
// the bus stays at 400kHz between transactions instead of dropping to the
// Adafruit-default 100kHz. We also force a recreate on first call (when the
// static placeholder is still in use) so the slow-default static never runs
// real OLED traffic.
bool initDisplayForConnectionType(int connectionType) {
    // Determine which Wire to use based on connection_type
    // Type 0 = GPIO 26/27 -> I2C1 (Wire1)
    // Type 1 = GPIO 6/7 -> I2C1 (Wire1)
    // Type 2 = GPIO 4/5 -> I2C0 (Wire)
    // Type 3 = custom -> check pins
    int needWire;
    if (connectionType == 2) {
        needWire = 0;  // I2C0 (Wire)
    } else {
        needWire = 1;  // I2C1 (Wire1) for types 0, 1, 3
    }

    // Already using the correctly-clocked dynamic instance on the right Wire.
    if (_displayIsDynamic && _displayPtr != nullptr && _currentDisplayWire == needWire) {
        return false;
    }

    // Delete old display ONLY if it was dynamically allocated (not the static default).
    if (_displayIsDynamic && _displayPtr != nullptr) {
        delete _displayPtr;
        _displayPtr = nullptr;
    }

    int width = jumperlessConfig.top_oled.width;
    int height = jumperlessConfig.top_oled.height;

    TwoWire* wirePtr = (needWire == 0) ? &Wire : &Wire1;
    _displayPtr = new Adafruit_SSD1306(width, height, wirePtr, OLED_RESET,
                                       /*clkDuring=*/kOledI2CClockHz,
                                       /*clkAfter=*/kOledI2CClockHz);

    _displayIsDynamic = true;
    _currentDisplayWire = needWire;
    return true;
}


int oledAddress = -1;
int numFonts = 35; // Updated for granular font sizes

// Global instance
class oled oled;

// // Font family mapping for automatic size selection
// enum FontFamily {
//     FONT_EUROSTILE = 0,
//     //FONT_BERKELEY = 1,
//     FONT_JOKERMAN = 2,
//     FONT_COMIC_SANS = 3,
//     FONT_COURIER_NEW = 4,
//     FONT_NEW_SCIENCE_MEDIUM = 5,
//     FONT_NEW_SCIENCE_MEDIUM_EXTENDED = 6,
//     //FONT_JUMPERLESS = 3
// };

struct font fontList[] = {
    // Core fonts - Size 1 (small) and Size 2 (large) for auto-fallback
    { &Eurostile8pt7b, "Eurostl", "Eurostile", 0, FONT_EUROSTILE, 8 },                                            // Index 0
    { &Eurostile12pt7b, "Eurostl", "Eurostile", 0, FONT_EUROSTILE, 12 },                                          // Index 1
    { &Jokerman8pt7b, "Jokermn", "Jokerman", 0, FONT_JOKERMAN, 8 },                                               // Index 2
    { &Jokerman12pt7b, "Jokermn", "Jokerman", 0, FONT_JOKERMAN, 12 },                                             // Index 3
    { &Comic_Sans8pt7b, "ComicSns", "Comic Sans", 0, FONT_COMIC_SANS, 8 },                                        // Index 4
    { &Comic_Sans12pt7b, "ComicSns", "Comic Sans", 0, FONT_COMIC_SANS, 12 },                                      // Index 5
    { &Courier_New8pt7b, "Courier", "Courier New", 0, FONT_COURIER_NEW, 8 },                                      // Index 6
    { &Courier_New12pt7b, "Courier", "Courier New", 0, FONT_COURIER_NEW, 12 },                                    // Index 7
    { &new_science_medium8pt7b, "Science", "New Science", 0, FONT_NEW_SCIENCE_MEDIUM, 8 },                        // Index 8
    { &new_science_medium12pt7b, "Science", "New Science", 0, FONT_NEW_SCIENCE_MEDIUM, 12 },                      // Index 9
    { &new_science_medium_extended8pt7b, "SciExt", "New Science Ext", 0, FONT_NEW_SCIENCE_MEDIUM_EXTENDED, 8 },   // Index 10
    { &new_science_medium_extended12pt7b, "SciExt", "New Science Ext", 0, FONT_NEW_SCIENCE_MEDIUM_EXTENDED, 12 }, // Index 11
    { &ANDALEMO5pt7b, "AndlMno", "Andale Mono", 0, FONT_ANDALE_MONO, 5 },                                         // Index 12
    { &FreeMono5pt7b, "FreMno", "Free Mono", 0, FONT_FREE_MONO, 4 },                                              // Index 13

    // Berkeley Mono - 2 sizes
    { &BerkeleyMono8pt7b, "BerkMono", "Berkeley Mono", 0, FONT_BERKELEY_MONO, 8 },   // Index 14
    { &BerkeleyMono12pt7b, "BerkMono", "Berkeley Mono", 0, FONT_BERKELEY_MONO, 12 }, // Index 15

    // Pragmatism - 8 granular sizes (5pt to 12pt) for smooth scaling
    { &Pragmatism5pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 5 },   // Index 16
    { &Pragmatism6pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 6 },   // Index 17
    { &Pragmatism7pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 7 },   // Index 18
    { &Pragmatism8pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 8 },   // Index 19
    { &Pragmatism9pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 9 },   // Index 20
    { &Pragmatism10pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 10 }, // Index 21
    { &Pragmatism11pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 11 }, // Index 22
    { &Pragmatism12pt7b, "Pragmtsm", "Pragmatism", 0, FONT_PRAGMATISM, 12 }, // Index 23

    // Iosevka Regular - 7 granular sizes (9pt to 15pt) for smooth scaling
    // Note: topRowOffset=0 to match other fonts - Iosevka's tall ascent handled by bounds calculation
    { &IosevkaSS08_Regular9pt7b, "IosevkaR", "Iosevka Regular", 1, FONT_IOSEVKA_REGULAR, 9 },   // Index 24
    { &IosevkaSS08_Regular11pt7b, "IosevkaR", "Iosevka Regular", 1, FONT_IOSEVKA_REGULAR, 11 }, // Index 25
    { &IosevkaSS08_Regular12pt7b, "IosevkaR", "Iosevka Regular", 1, FONT_IOSEVKA_REGULAR, 12 }, // Index 26
    { &IosevkaSS08_Regular13pt7b, "IosevkaR", "Iosevka Regular", 1, FONT_IOSEVKA_REGULAR, 13 }, // Index 27
    { &IosevkaSS08_Regular14pt7b, "IosevkaR", "Iosevka Regular", 1, FONT_IOSEVKA_REGULAR, 14 }, // Index 28
    { &IosevkaSS08_Regular15pt7b, "IosevkaR", "Iosevka Regular", 1, FONT_IOSEVKA_REGULAR, 15 }, // Index 29

    // Small fonts for file manager (4-5pt)
    { &ubuntu5pt7b, "Ubuntu", "Ubuntu", 0, FONT_EUROSTILE, 5 },                                // Index 30
    { &DotGothic16_Regular4pt7b, "DotGoth", "DotGothic", 0, FONT_EUROSTILE, 4 },               // Index 31
    { &IosevkaSS08_Light5pt7b, "IosevkaS5", "Iosevka Light 5pt", 0, FONT_IOSEVKA_REGULAR, 5 }, // Index 32
    { &FreeMono5pt7b, "FreMonoS5", "Free Mono 5pt", 0, FONT_FREE_MONO, 5 },                    // Index 33
    { &EnvyCodeRNerdFont_Regular5pt7b, "EnvyS5", "EnvyCode 5pt", 0, FONT_EUROSTILE, 5 },       // Index 34
};

// Font family mappings: {size1_index, size2_index} - BACKWARDS COMPATIBILITY
// -1 means no variant available for that size
// NOTE: With granular font system, prefer using FontManager::getFontForPointSize() instead
struct FontSizeMapping {
    int size1Index;
    int size2Index;
};

FontSizeMapping fontFamilyMap[] = {
    { 0, 1 },   // FONT_EUROSTILE: 8pt for size 1, 12pt for size 2
    { 2, 3 },   // FONT_JOKERMAN: 8pt for size 1, 12pt for size 2
    { 4, 5 },   // FONT_COMIC_SANS: 8pt for size 1, 12pt for size 2
    { 6, 7 },   // FONT_COURIER_NEW: 8pt for size 1, 12pt for size 2
    { 8, 9 },   // FONT_NEW_SCIENCE_MEDIUM: 8pt for size 1, 12pt for size 2
    { 10, 11 }, // FONT_NEW_SCIENCE_MEDIUM_EXTENDED: 8pt for size 1, 12pt for size 2
    { 12, 12 }, // FONT_ANDALE_MONO: 5pt for both sizes
    { 13, 13 }, // FONT_FREE_MONO: 4pt for both sizes
    { 24, 25 }, // FONT_IOSEVKA_REGULAR: 9pt for size 1, 11pt for size 2 (NEW INDEX)
    { 14, 15 }, // FONT_BERKELEY_MONO: 8pt for size 1, 12pt for size 2
    { 19, 23 }, // FONT_PRAGMATISM: 8pt for size 1, 12pt for size 2 (NEW INDEX)
};

// Per-family font characteristics for intelligent multi-line scaling
// Allows fonts with tight metrics (like Iosevka) to avoid aggressive downscaling
FontFamilyCharacteristics fontCharacteristics[] = {
    { FONT_EUROSTILE, 0.75, 2, 2 },                   // Standard proportions
    { FONT_JOKERMAN, 1.0, 2, 2 },                     // Standard
    { FONT_COMIC_SANS, 0.8, 1, 2 },                   // Standard
    { FONT_COURIER_NEW, 0.75, 2, 2 },                 // Standard
    { FONT_NEW_SCIENCE_MEDIUM, 0.75, 2, 2 },          // Standard
    { FONT_NEW_SCIENCE_MEDIUM_EXTENDED, 0.75, 2, 2 }, // Standard
    { FONT_ANDALE_MONO, 0.9, 1, 1 },                  // Small font, tight tolerance
    { FONT_FREE_MONO, 0.7, 1, 1 },                    // Small font, tight tolerance
    { FONT_IOSEVKA_REGULAR, 0.7, 4, 2 },              // TIGHT METRICS: 15% tighter line spacing, allow 4px vertical clip
    { FONT_BERKELEY_MONO, 0.7, 2, 2 },                // Slightly tight metrics
    { FONT_PRAGMATISM, 0.7, 2, 2 },                  // Standard
};

// Helper to get font characteristics for a family
FontFamilyCharacteristics getFontCharacteristics( FontFamily family ) {
    for ( int i = 0; i < 11; i++ ) {
        if ( fontCharacteristics[ i ].family == family ) {
            return fontCharacteristics[ i ];
        }
    }
    // Default characteristics
    return { family, 1.0, 2, 2 };
}

// ============================
// FONTMANAGER IMPLEMENTATION
// ============================
// Lightweight font lookup system - all data stored in PROGMEM (flash)
// Provides granular font scaling with minimal RAM usage

// Helper function to get text bounds using a specific font
// Note: We can't save/restore font in Adafruit_SSD1306, so caller must restore if needed
static void getTextBoundsWithFont( Adafruit_SSD1306& disp, const GFXfont* font, const char* text, int16_t* w, int16_t* h ) {
    disp.setFont( font );
    int16_t x1, y1;
    uint16_t w16, h16;
    disp.getTextBounds( text, 0, 0, &x1, &y1, &w16, &h16 );
    *w = w16;
    *h = h16;
    // Note: Caller should restore font if needed
}

// Find best font for family and desired point size
// Returns font index, automatically selects closest available size
int FontManager::getFontForPointSize( FontFamily family, uint8_t desiredPointSize ) {
    int bestIndex = -1;
    int bestDiff = 999;

    // Search through fontList to find closest matching font
    for ( int i = 0; i < numFonts; i++ ) {
        if ( fontList[ i ].family == family ) {
            int diff = abs( (int)fontList[ i ].pointSize - (int)desiredPointSize );
            if ( diff < bestDiff ) {
                bestDiff = diff;
                bestIndex = i;
            }
        }
    }

    // Fallback: if no match found, use default for family
    if ( bestIndex == -1 ) {
        // Use size1Index from old mapping as fallback
        if ( family >= 0 && family <= FONT_PRAGMATISM ) {
            bestIndex = fontFamilyMap[ family ].size1Index;
        } else {
            bestIndex = 0; // Ultimate fallback
        }
    }

    return bestIndex;
}

// Convert old textSize (1/2) to point size for backwards compatibility
// Now display-size aware for better scaling on small displays
uint8_t FontManager::textSizeToPointSize( int textSize, int displayHeight ) {
    if ( textSize <= 0 )
        return 5; // Extra small
    if ( textSize == 1 )
        return 8; // Small (reduced from 9)

    // For textSize >= 2, scale based on display height
    if ( displayHeight <= 32 ) {
        // Small displays (32px): Use conservative sizes
        return 10; // Start at 10pt instead of 12pt
    } else if ( displayHeight <= 48 ) {
        // Medium displays (48px): Use moderate sizes
        return 11;
    } else {
        // Large displays (64px+): Use full sizes
        return 12;
    }
}

// Find largest font that fits given text width
// Returns optimal point size for the text to fit within maxWidth
uint8_t FontManager::findBestFitPointSize( FontFamily family, const char* text, int16_t maxWidth, uint8_t maxPointSize, uint8_t minPointSize ) {
    // Start from largest size and work down
    for ( uint8_t pt = maxPointSize; pt >= minPointSize; pt-- ) {
        int fontIndex = getFontForPointSize( family, pt );
        if ( fontIndex < 0 || fontIndex >= numFonts )
            continue;

        // Check if text fits with this font
        int16_t w, h;
        getTextBoundsWithFont( getDisplay(), fontList[ fontIndex ].font, text, &w, &h );

        if ( w <= maxWidth ) {
            return fontList[ fontIndex ].pointSize; // Found a font that fits!
        }
    }

    // If nothing fits, return minimum size
    return minPointSize;
}

// Get all available point sizes for a font family (stored in PROGMEM)
void FontManager::getAvailableSizes( FontFamily family, uint8_t* sizes, int* count ) {
    *count = 0;

    // Scan fontList for all fonts in this family
    for ( int i = 0; i < numFonts && *count < 16; i++ ) {
        if ( fontList[ i ].family == family ) {
            // Check if we already have this size
            bool duplicate = false;
            for ( int j = 0; j < *count; j++ ) {
                if ( sizes[ j ] == fontList[ i ].pointSize ) {
                    duplicate = true;
                    break;
                }
            }
            if ( !duplicate ) {
                sizes[ *count ] = fontList[ i ].pointSize;
                ( *count )++;
            }
        }
    }

    // Sort sizes (simple bubble sort - small array)
    for ( int i = 0; i < *count - 1; i++ ) {
        for ( int j = 0; j < *count - i - 1; j++ ) {
            if ( sizes[ j ] > sizes[ j + 1 ] ) {
                uint8_t temp = sizes[ j ];
                sizes[ j ] = sizes[ j + 1 ];
                sizes[ j + 1 ] = temp;
            }
        }
    }
}

// Helper: find closest font index for a specific point size
int FontManager::findClosestFont( FontFamily family, uint8_t pointSize ) {
    return getFontForPointSize( family, pointSize );
}

// SIMPLIFIED POSITIONING SYSTEM
// ============================

// Constructor
oled::oled( ) {}

// Helper function to load bitmap from filesystem
// Returns true if successfully loaded, false otherwise
// Bitmap data is stored in a static buffer
// Bitmap buffer - exposed for MicroPython access
uint8_t customBitmapBuffer[ 1024 ] = { 0 }; // Support up to 128x64 displays (1024 bytes)
int customBitmapWidth = 0;
int customBitmapHeight = 0;
bool customBitmapLoaded = false;

// Helper to check if string looks like a file path
bool looksLikeFilePath( const char* str ) {
    if ( !str || strlen( str ) == 0 )
        return false;

    // Check for file extensions commonly used for bitmaps
    // Don't check filesystem existence here - let loadBitmapFromFile() handle that
    // This prevents startup issues when filesystem isn't ready yet
    const char* ext = strrchr( str, '.' );
    if ( ext ) {
        if ( strcasecmp( ext, ".bin" ) == 0 ||
             strcasecmp( ext, ".bmp" ) == 0 ||
             strcasecmp( ext, ".xbm" ) == 0 ||
             strcasecmp( ext, ".raw" ) == 0 ) {
            return true;
        }
    }

    // No bitmap extension, not a file path
    return false;
}

bool loadBitmapFromFile( const char* filepath ) {
    customBitmapLoaded = false;
    customBitmapWidth = 0;
    customBitmapHeight = 0;

    // Ensure path starts with / for FatFS
    String fullPath = filepath;
    if ( filepath[ 0 ] != '/' ) {
        fullPath = String( "/" ) + filepath;
    }

    // First check if file exists before trying to open using safe function
    if ( !safeFileExists( fullPath.c_str( ), 500 ) ) {
        return false;
    }

    File file = safeFileOpen( fullPath.c_str( ), "r", 1000 );
    if ( !file ) {
        return false;
    }

    size_t fileSize = file.size( );

    // Try to detect format and load accordingly
    // Format 1: Our custom format with 4-byte header (width, height as 16-bit little-endian)
    // Format 2: Raw bitmap data (guess dimensions from file size)

    if ( fileSize >= 4 ) {
        // Try reading as custom format with header
        uint8_t wl = file.read( );
        uint8_t wh = file.read( );
        uint8_t hl = file.read( );
        uint8_t hh = file.read( );

        int testWidth = wl | ( wh << 8 );
        int testHeight = hl | ( hh << 8 );

        // Check if dimensions are valid and match file size
        if ( testWidth > 0 && testWidth <= 128 &&
             testHeight > 0 && testHeight <= 64 ) {

            size_t expectedSize = ( testWidth * testHeight + 7 ) / 8;
            size_t remainingSize = fileSize - 4;

            // If size matches, it's our custom format
            if ( expectedSize == remainingSize ) {
                customBitmapWidth = testWidth;
                customBitmapHeight = testHeight;

                if ( expectedSize <= sizeof( customBitmapBuffer ) ) {
                    size_t bytesRead = file.readBytes( (char*)customBitmapBuffer, expectedSize );
                    safeFileClose( file, false );

                    if ( bytesRead == expectedSize ) {
                        customBitmapLoaded = true;
                        return true;
                    }
                }
            }
        }

        // Not our format, try as raw bitmap data
        file.seek( 0 );
    }

    // Try as raw bitmap data - guess dimensions from common sizes
    // 128x32 = 512 bytes, 128x64 = 1024 bytes, 64x32 = 256 bytes, 128x31 = 496 bytes
    if ( fileSize == 512 ) {
        customBitmapWidth = 128;
        customBitmapHeight = 32;
    } else if ( fileSize == 1024 ) {
        customBitmapWidth = 128;
        customBitmapHeight = 64;
    } else if ( fileSize == 256 ) {
        customBitmapWidth = 64;
        customBitmapHeight = 32;
    } else if ( fileSize == 496 ) {
        customBitmapWidth = 128;
        customBitmapHeight = 31;
    } else {
        safeFileClose( file, false );
        return false;
    }

    size_t bytesRead = file.readBytes( (char*)customBitmapBuffer, fileSize );
    safeFileClose( file, false );

    if ( bytesRead == fileSize ) {
        customBitmapLoaded = true;
        return true;
    }

    return false;
}

void oled::displayBitmap( int x, int y, const unsigned char* bitmap, int width, int height ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;
    
    getDisplay().drawBitmap( x, y, bitmap, width, height, 1 );
}

// Initialization
int oled::init( ) {
    #if OLED_DEBUG
    Serial.printf("[OLED] init() called, connection_type=%d\n", jumperlessConfig.top_oled.connection_type);
    #endif
    
    if ( jumperlessConfig.top_oled.enabled == 0 ) {
        jumperlessConfig.top_oled.enabled = 1;
    }

    int success = 0;
    address = jumperlessConfig.top_oled.i2c_address;
    sda_pin = jumperlessConfig.top_oled.sda_pin;
    scl_pin = jumperlessConfig.top_oled.scl_pin;
    sda_row = jumperlessConfig.top_oled.sda_row;
    scl_row = jumperlessConfig.top_oled.scl_row;

    // Check if using hardwired pins based on connection_type:
    // Type 0 = GPIO 7/8 (via crossbar, NOT hardwired) - uses I2C1 (Wire1)
    // Type 1 = RP6/RP7 (hardwired, GPIO 6/7) - uses I2C1 (Wire1)
    // Type 2 = internal I2C0 (hardwired, GPIO 4/5) - uses I2C0 (Wire)
    // Type 3 = custom (via crossbar, NOT hardwired) - uses I2C1 (Wire1)
    int connType = jumperlessConfig.top_oled.connection_type;
    oledUsingHardwiredPins = ( connType == 1 || connType == 2 );
    
    #if OLED_DEBUG
    Serial.printf("[OLED] init(): addr=0x%02X, connType=%d, hardwired=%d\n", address, connType, oledUsingHardwiredPins);
    #endif

    // Read display dimensions from config
    displayWidth = jumperlessConfig.top_oled.width;
    displayHeight = jumperlessConfig.top_oled.height;

    // Initialize or reinitialize display with correct Wire based on connection_type
    // This allows runtime switching between Wire (I2C0) and Wire1 (I2C1)
    initDisplayForConnectionType(connType);

    #if OLED_DEBUG
    Serial.println("[OLED] init(): calling connect()...");
    #endif
    success = connect( );
    #if OLED_DEBUG
    Serial.printf("[OLED] init(): connect() returned %d\n", success);
    #endif
    
    if ( checkConnection( ) == false ) {
        #if OLED_DEBUG
        Serial.println("[OLED] init(): checkConnection() failed after connect()");
        #endif
        // oledConnected = false;
        // Serial.println("oled::init checkConnection failed");
        // Serial.println(millis());
        // Serial.flush();
        // delay(100);

        // Serial.println("oled width: " + String(displayWidth));
        // Serial.println("oled height: " + String(displayHeight));
        // Serial.flush();
        // delay(100);

        // Serial.println("oled rotation: " + String(jumperlessConfig.top_oled.rotation));
        // Serial.flush();
        // delay(100);

        // Serial.println("oled font: " + String(jumperlessConfig.top_oled.font));
        // disconnect();

        // success = connect();
        // delay(10);
        //   if (checkConnection() == false) {
        //  Serial.println("oled::init checkConnection failed 2");
        //  Serial.println(millis());
        //  Serial.flush();
        //  delay(100);
        // return 0;

        if ( stillWriteToFramebuffer == false ) {
            return 0;
        }
        // }
    }
    // Set font from config value (config value IS the FontFamily enum)
    if ( jumperlessConfig.top_oled.font >= 0 && jumperlessConfig.top_oled.font <= FONT_PRAGMATISM ) {
        // Serial.print(" Font: ");
        // Serial.print(jumperlessConfig.top_oled.font);
        // Serial.flush();
        // delay(100);
        FontFamily family = (FontFamily)jumperlessConfig.top_oled.font;
        setFontForSize( family, 2 ); // Use size 2 (large/12pt) as default
        currentFontFamily = family;
    } else {
        // Fallback to Eurostile
        setFontForSize( FONT_EUROSTILE, 2 );
        currentFontFamily = FONT_EUROSTILE;
    }

    getDisplay().begin( SSD1306_SWITCHCAPVCC, address, false, false );

    //can give the oled some neat scanning effects that look like a crt


    // Apply rotation from config (0 = 0°, 1 = 90°, 2 = 180°, 3 = 270°)
    getDisplay().setRotation( jumperlessConfig.top_oled.rotation );

    getDisplay().setTextColor( SSD1306_WHITE );
    getDisplay().invertDisplay( false );
    getDisplay().setFont( currentFont );
    getDisplay().clearDisplay( );

    // Check if startup_message is set
    if ( strlen( jumperlessConfig.top_oled.startup_message ) > 0 ) {
        // Check if it looks like a file path
        if ( looksLikeFilePath( jumperlessConfig.top_oled.startup_message ) ) {
            // // Try to load and display bitmap from file
            // if (loadBitmapFromFile(jumperlessConfig.top_oled.startup_message)) {
            //     int x = (displayWidth - customBitmapWidth) / 2;
            //     int y = (displayHeight - customBitmapHeight) / 2 + 1;
            //     getDisplay().drawBitmap(x, y, customBitmapBuffer, customBitmapWidth, customBitmapHeight, SSD1306_WHITE);
            // } else {
            //     // File not found or invalid, display error message
            //     clearPrintShow("Bitmap\nError", 2, true, true, true);
            // }
            showJogo32h( );
        } else {
            // Display as text
            clearPrintShow( jumperlessConfig.top_oled.startup_message, 2, true, true, true );
        }
    } else {
        // No startup message, display default jogo bitmap
        int x = ( displayWidth - jumperlessConfig.top_oled.width ) / 2;
        int y = ( displayHeight - jumperlessConfig.top_oled.height ) / 2 + 1;
        showJogo32h( );
        // getDisplay().drawBitmap(x, y, jogo32h, jumperlessConfig.top_oled.width, jumperlessConfig.top_oled.height, SSD1306_WHITE);
    }

    if ( oledConnected ) {
        show();
        //getDisplay().display( );
    }
    setCursor( 0, 0 );
    // Bound the active OLED bus's transaction timeout. Without this, an
    // OLED that vanishes between checkConnection() polls makes every
    // subsequent display() write stall for the platform default (~hundreds
    // of ms) until oledConnected catches up, which is the UI freeze the
    // user sees on hot-unplug. 15ms is generous for any normal OLED frame
    // chunk at 400kHz but still keeps a single failed transfer below one
    // frame of visible lag.
    // reset_with_timeout=true: a timed-out transaction (hot-unplug mid-write)
    // also reinits the I2C block and clocks a stuck-low SDA free, so the
    // shared bus (INA219s + DAC live on Wire) recovers instead of failing
    // every transaction until reboot.
    if ( jumperlessConfig.top_oled.connection_type == 2 ) {
        Wire.setTimeout( 3, true );
    } else {
        Wire1.setTimeout( 3, true );
    }
    charPos = 0;
    refreshConnections( -1 );

    // Register this oled instance with OLEDService so it can call oledPeriodic()
    OLEDService::getInstance( ).setOledDisplay( this );

    #if OLED_DEBUG
    Serial.printf("[OLED] init() complete, returning %d, oledConnected=%d\n", success, oledConnected);
    #endif
    return success;
}

void oled::setDisplayClockDivideRatio(int divideRatio) {

    if (divideRatio < 0) {
        divideRatio = 0;
    } else if (divideRatio > 0x0F) {
        divideRatio = 0x0F;
    }
    setDisplayClock((uint8_t)divideRatio, 0x8);
}

void oled::setDisplayClock(uint8_t divideRatio, uint8_t oscillatorFreq) {
    setDisplayClockRaw(((oscillatorFreq & 0x0F) << 4) | (divideRatio & 0x0F));
}

void oled::setDisplayClockRaw(uint8_t clockDivOscSetting) {
    // SSD1306_SETDISPLAYCLOCKDIV (0xD5) expects one parameter byte:
    // upper nibble = oscillator frequency, lower nibble = clock divide ratio.
    getDisplay().ssd1306_command(0xD5);
    getDisplay().ssd1306_command(clockDivOscSetting);
}

// Helper to check I2C communication
bool oled::checkConnection( bool force  ) {
    // if (jumperlessConfig.top_oled.enabled == 0) {
    //     oledConnected = false;
    //     return false;
    // }
    if ( millis( ) - lastConnectionCheck > 1000 || force == true ) {
        // Use correct Wire based on current display configuration
        // If display not initialized yet, use connection_type to determine Wire
        int wireNum = _currentDisplayWire;
        if (wireNum == -1) {
            wireNum = (jumperlessConfig.top_oled.connection_type == 2) ? 0 : 1;
        }
        TwoWire& wire = (wireNum == 0) ? Wire : Wire1;
        
        wire.beginTransmission( address );
        int error = wire.endTransmission( );
        if ( error != 0 ) {
            #if OLED_DEBUG
            Serial.printf("[OLED] checkConnection: I2C error=%d on Wire%d addr=0x%02X\n", error, wireNum, address);
            #endif
            lastConnectionCheck = millis( );
            oledConnected = false;
            return false;
        }
        lastConnectionCheck = millis( );
        oledConnected = true;
    }
    return oledConnected;  // Return actual state, not always true
}

// Font management

// REMOVED: configValueToFontIndex array (obsolete)
// Config values now ARE FontFamily enum values (0-10) directly
// Use setFontForSize(family, textSize) to set fonts from config

// Map config value (0-10) to FontFamily enum
FontFamily mapConfigValueToFontFamily( int configValue ) {
    switch ( configValue ) {
    case 0:
        return FONT_EUROSTILE;
    case 1:
        return FONT_JOKERMAN;
    case 2:
        return FONT_COMIC_SANS;
    case 3:
        return FONT_COURIER_NEW;
    case 4:
        return FONT_NEW_SCIENCE_MEDIUM;
    case 5:
        return FONT_NEW_SCIENCE_MEDIUM_EXTENDED;
    case 6:
        return FONT_ANDALE_MONO;
    case 7:
        return FONT_FREE_MONO;
    case 8:
        return FONT_BERKELEY_MONO;
    case 9:
        return FONT_PRAGMATISM;
    case 10:
        return FONT_IOSEVKA_REGULAR;
    default:
        return FONT_EUROSTILE;
    }
}

int oled::cycleFont( void ) {
    currentFontFamily = (FontFamily)( currentFontFamily + 1 );
    if ( currentFontFamily > FONT_PRAGMATISM ) {
        currentFontFamily = FONT_EUROSTILE;
    }
    setFontForSize( currentFontFamily, currentTextSize );
    clearPrintShow( (String)fontList[ fontFamilyMap[ currentFontFamily ].size2Index ].longName, 2 );

    // Map FontFamily back to config value for proper saving
    int configValue = (int)currentFontFamily; // Direct mapping works now!
    jumperlessConfig.top_oled.font = configValue;
    saveConfig( );
    return currentFontFamily;
}

// Allocation-free font-name normalization: lowercases and strips spaces, tabs,
// newlines, underscores and dashes so "Comic Sans" / "comic_sans" /
// "comic-sans" all match. Writes a NUL-terminated result into `out`.
//
// The old path built an Arduino String per font-table entry (and one for the
// input) on every call - ~60 heap alloc/frees per lookup. getFontFamily() is
// called from the OLED render path, so at the live-screen refresh rate that was
// thousands of allocations per second on core0, thrashing the heap (and racing
// core1's allocations). This version touches only stack buffers.
static void normalizeFontNameInto( const char* in, char* out, size_t cap ) {
    size_t o = 0;
    if ( cap == 0 ) return;
    if ( in ) {
        for ( const char* p = in; *p && o + 1 < cap; ++p ) {
            char c = *p;
            if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                 c == '_' || c == '-' ) {
                continue;
            }
            if ( c >= 'A' && c <= 'Z' ) {
                c = (char)( c + ( 'a' - 'A' ) );
            }
            out[ o++ ] = c;
        }
    }
    out[ o ] = '\0';
}

// Longest real font name ("Iosevka Regular" -> "iosevkaregular", 14 chars).
#define OLED_FONT_NORM_MAX 32

FontFamily oled::getFontFamily( const char* fontName ) {
    char normInput[ OLED_FONT_NORM_MAX ];
    normalizeFontNameInto( fontName, normInput, sizeof( normInput ) );

    char normCand[ OLED_FONT_NORM_MAX ];
    for ( int i = 0; i < numFonts; i++ ) {
        normalizeFontNameInto( fontList[ i ].longName, normCand, sizeof( normCand ) );
        if ( strcmp( normInput, normCand ) == 0 ) {
            return fontList[ i ].family;
        }
        normalizeFontNameInto( fontList[ i ].shortName, normCand, sizeof( normCand ) );
        if ( strcmp( normInput, normCand ) == 0 ) {
            return fontList[ i ].family;
        }
    }
    return FONT_EUROSTILE;
}

FontFamily oled::getFontFamily( String fontName ) {
    return getFontFamily( fontName.c_str( ) );
}

int oled::setFont( String fontName, int justGetIndex ) {
    char normInput[ OLED_FONT_NORM_MAX ];
    normalizeFontNameInto( fontName.c_str( ), normInput, sizeof( normInput ) );

    char normCand[ OLED_FONT_NORM_MAX ];
    for ( int i = 0; i < numFonts; i++ ) {
        normalizeFontNameInto( fontList[ i ].longName, normCand, sizeof( normCand ) );
        if ( strcmp( normInput, normCand ) == 0 ) {
            if ( justGetIndex == 0 ) {
                setFont( i );
            }
            return i;
        }
        normalizeFontNameInto( fontList[ i ].shortName, normCand, sizeof( normCand ) );
        if ( strcmp( normInput, normCand ) == 0 ) {
            if ( justGetIndex == 0 ) {
                setFont( i );
            }
            return i;
        }
    }
    return -1;
}
void oled::setFont( const GFXfont* font ) {
    currentFont = font;

    getDisplay().setFont( currentFont );

    for ( int i = 0; i < numFonts; i++ ) {
        if ( fontList[ i ].font == currentFont ) {
            currentFontFamily = fontList[ i ].family;
            break;
        }
    }
}
int oled::setFont( char* fontName, int justGetIndex ) {
    return setFont( (String)fontName, justGetIndex );
}
void oled::setFont( FontFamily fontFamily ) {
    currentFontFamily = fontFamily;
    currentFont = fontList[ fontFamily ].font;
    getDisplay().setFont( currentFont );
}
void oled::setFont( int fontIndex ) {
    int safeIndex = fontIndex;
    if ( safeIndex < 0 || safeIndex >= numFonts ) {
        safeIndex = 0;
    }
    currentFont = fontList[ safeIndex ].font;
    currentFontFamily = fontList[ safeIndex ].family;

    getDisplay().setFont( currentFont );
}

// Smart font selection based on family and text size (BACKWARDS COMPATIBLE)
void oled::setFontForSize( FontFamily family, int textSize ) {
    if ( family < 0 || family > FONT_PRAGMATISM ) {
        family = FONT_EUROSTILE; // Default to Eurostile
    }
    currentFontFamily = family;

    // Convert textSize to point size and use new system (display-aware)
    uint8_t pointSize = FontManager::textSizeToPointSize( textSize, displayHeight );
    setFontPointSize( family, pointSize );
}

// New granular font selection by point size
void oled::setFontPointSize( FontFamily family, uint8_t pointSize ) {
    if ( family < 0 || family > FONT_PRAGMATISM ) {
        family = FONT_EUROSTILE; // Default to Eurostile
    }
    currentFontFamily = family;
    currentPointSize = pointSize;

    // Use FontManager to find best matching font
    int fontIndex = FontManager::getFontForPointSize( family, pointSize );

    if ( fontIndex >= 0 && fontIndex < numFonts ) {
        setFont( fontIndex );
    } else {
        // Fallback to default
        setFont( 0 );
    }
}

String oled::getFontName( FontFamily fontFamily ) {

    for ( int i = 0; i < numFonts; i++ ) {
        if ( fontList[ i ].family == fontFamily ) {
            return fontList[ i ].longName;
        }
    }
    return "Unknown";
}

// CORE POSITIONING FUNCTIONS
// ==========================

// Get character width for current font (useful for monospaced fonts)
int oled::getCharacterWidth( ) {
    if ( !currentFont ) {
        return 6; // Default monospace width
    }

    // For GFX fonts, get the advance width of a typical character (space)
    if ( currentFont->first <= 0x20 && 0x20 <= currentFont->last ) {
        GFXglyph* glyph = (GFXglyph*)&currentFont->glyph[ 0x20 - currentFont->first ]; // space character
        return glyph->xAdvance * currentTextSize;
    }

    // Fallback: get advance of first available character
    if ( currentFont->glyph ) {
        GFXglyph* glyph = (GFXglyph*)&currentFont->glyph[ 0 ];
        return glyph->xAdvance * currentTextSize;
    }

    return 6; // Default fallback
}

// Get font metrics with proper text size scaling
FontMetrics oled::getFontMetrics( ) {
    FontMetrics metrics = { 0 };

    if ( !currentFont ) {
        // Default font metrics scaled by text size
        metrics.lineHeight = 8 * currentTextSize;
        metrics.ascent = 8 * currentTextSize;
        metrics.descent = 0;
        metrics.maxWidth = 6 * currentTextSize;
        return metrics;
    }

    // For GFX fonts, calculate proper metrics
    metrics.lineHeight = currentFont->yAdvance * currentTextSize;

    // Calculate real ascent by examining font glyphs
    int16_t maxAscent = 0;
    for ( uint8_t c = currentFont->first; c <= currentFont->last && c < currentFont->first + 10; c++ ) {
        GFXglyph* glyph = (GFXglyph*)&currentFont->glyph[ c - currentFont->first ];
        int16_t ascent = -glyph->yOffset;
        if ( ascent > maxAscent )
            maxAscent = ascent;
    }

    if ( maxAscent == 0 ) {
        maxAscent = currentFont->yAdvance * 0.75; // Conservative estimate
    }

    metrics.ascent = maxAscent * currentTextSize;
    metrics.descent = ( currentFont->yAdvance - maxAscent ) * currentTextSize;
    metrics.maxWidth = currentFont->yAdvance * currentTextSize;

    return metrics;
}

// Get text bounds using current font and text size
TextBounds oled::getTextBounds( const char* str ) {
    TextBounds bounds = { 0 };

    if ( !str || strlen( str ) == 0 ) {
        return bounds;
    }

    // Normalize digits for bounds calculations so labels like "channel 0"
    // and "channel 1" resolve to a consistent measured width.
    String measureText = String( str );
    for ( size_t i = 0; i < measureText.length( ); i++ ) {
        if ( measureText[ i ] >= '0' && measureText[ i ] <= '9' ) {
            measureText.setCharAt( i, '0' );
        }
    }

    // Use Adafruit GFX's built-in function for accuracy
    int16_t x1, y1;
    uint16_t w, h;
    getDisplay().getTextBounds( measureText.c_str( ), 0, 0, &x1, &y1, &w, &h );

    bounds.width = w;
    bounds.height = h;
    bounds.x1 = x1;
    bounds.y1 = y1;
    bounds.x2 = x1 + w;
    bounds.y2 = y1 + h;

    // Calculate ascent and descent for positioning
    if ( currentFont ) {
        bounds.ascent = -y1;
        bounds.descent = h + y1;
        bounds.baseline = bounds.ascent;
    } else {
        bounds.ascent = h;
        bounds.descent = 0;
        bounds.baseline = h;
    }

    // Serial.print("bounds.ascent: ");
    // Serial.println(bounds.ascent);
    // Serial.print("bounds.descent: ");
    // Serial.println(bounds.descent);
    // Serial.print("bounds.baseline: ");
    // Serial.println(bounds.baseline);

    return bounds;
}

// String versions
TextBounds oled::getTextBounds( const String& str ) {
    return getTextBounds( str.c_str( ) );
}

// Get position to center text (unified function)
void oled::getCenteredPosition( const char* str, int16_t* x, int16_t* y, PositionMode mode ) {
    if ( !str || !x || !y )
        return;

    // Use unified bounds path so centering follows the same normalization
    // rules as fitting logic (including digit normalization).
    TextBounds bounds = getTextBounds( str );

    // Center horizontally
    *x = ( displayWidth - bounds.width ) / 2;
    // Serial.print("w: ");
    // Serial.println(w);
    // Serial.print("h: ");
    // Serial.println(h);
    // Serial.print("y1: ");
    // Serial.println(y1);
    // Serial.print("y: ");
    // Serial.println(*y);
    // Serial.println("--------------------------------");

    // Vertical positioning - return the Y coordinate that setCursor should use
    if ( mode == POS_TIGHT || ( mode == POS_AUTO && currentTextSize > 1 ) ) {
        // For larger text, center only the ascent (visible part above baseline)
        // Put baseline at screen_center + (ascent/2) so visual center is at screen center
        *y = ( displayHeight / 2 ) + ( bounds.ascent / 2 );
    } else {
        // Standard baseline positioning - center only the ascent
        *y = ( displayHeight / 2 ) + ( bounds.ascent / 2 );
    }
}

// Default to auto mode
void oled::getCenteredPosition( const char* str, int16_t* x, int16_t* y ) {
    getCenteredPosition( str, x, y, POS_AUTO );
}

// Check if text fits on display
bool oled::textFits( const char* str ) {
    if ( !str )
        return true;
    TextBounds bounds = getTextBounds( str );
    return ( bounds.width <= displayWidth && bounds.height <= displayHeight );
}

// UNIFIED CURSOR POSITIONING
// =========================

// Main cursor setting function with automatic positioning logic
void oled::setCursor( int16_t x, int16_t y, PositionMode mode ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    if ( !currentFont ) {
        // Default font - direct positioning
        getDisplay().setCursor( x, y );
        return;
    }

    // For GFX fonts, apply intelligent positioning
    FontMetrics metrics = getFontMetrics( );
    int16_t finalY = y;

    // Determine positioning mode
    if ( mode == POS_AUTO ) {
        // Auto mode: use tight positioning for y=0 or large text
        if ( y == 0 || currentTextSize > 1 ) {
            mode = POS_TIGHT;
        } else {
            mode = POS_BASELINE;
        }
    }

    if ( mode == POS_TIGHT ) {
        // Tight positioning: for special case of y=0, position at top
        if ( y == 0 ) {
            // Special case for top of screen - position baseline so text starts at y=0
            finalY = metrics.ascent;

            // Apply font-specific offset for multi-line displays
            for ( int i = 0; i < numFonts; i++ ) {
                if ( fontList[ i ].font == currentFont && fontList[ i ].topRowOffset != 0 ) {
                    if ( displayHeight >= 24 ) {              // Multi-line capable
                        finalY += fontList[ i ].topRowOffset; // Can be positive or negative
                    }
                    break;
                }
            }
        } else {
            // For other positions, y is already the correct baseline coordinate
            finalY = y;
        }
    } else {
        // Baseline positioning - y is where the baseline should be
        finalY = y;
    }

    getDisplay().setCursor( x, finalY );
}

// Default cursor setting (auto mode)
void oled::setCursor( int16_t x, int16_t y ) {
    setCursor( x, y, POS_AUTO );
}

void OLEDprintFromTerminal( void ) {
    // if ((!oledConnected || !text) && stillWriteToFramebuffer == false) return;
    char textBuffer[ 120 ];
    int textBufferIndex = 0;

    int exit = 0;
    int textSize = 1;
    FontFamily textFamily = FONT_EUROSTILE;

    Jerial.println( "Terminal mode active. Type '!' to exit." );
    Jerial.println( "Type '+' to increase text size." );
    Jerial.println( "Type '-' to decrease text size." );
    Jerial.println( "Type '=' to cycle through font families." );
    Jerial.println( "Type '|' to cycle through font families backwards." );
    Jerial.println( "Type '*' to clear the screen." );
    Jerial.println( "Type '!' to exit." );

    Jerial.write(0x03);
    
    Jerial.flush();



    while ( exit == 0 ) {
        if ( Serial.available( ) > 0 ) {
            char c = Serial.read( );
            if ( c == '\n' ) {
                textBuffer[ textBufferIndex ] = '\n';
                textBufferIndex++;

            } else if ( c == '*' ) {
                for ( int i = 0; i < textBufferIndex; i++ ) {
                    textBuffer[ i ] = ' ';
                }
                textBufferIndex = 0;
             } else if ( c == '!' ) {
                    exit = 1;
                }
                else if ( c == '+' ) {
                   textSize++;
                   
                    
                } else if ( c == '-' ) {
                    textSize--;
                    if ( textSize < 1 ) {
                        textSize = 1;
                    }
                } else if ( c == '=' ) {
                    textFamily = (FontFamily)( textFamily + 1 );
                    if ( textFamily > FONT_PRAGMATISM ) {
                        textFamily = FONT_EUROSTILE;
                    }
                    //oled.setFont( textFamily );
                    Serial.println( "Font family: " + String( oled.getFontName( textFamily ) ) );
                    oled.clearPrintShow( "Font family: " + String( oled.getFontName( textFamily ) ), 1, textFamily, true, true, true );
                    Jerial.flush();
                } else if ( c == '|' ) {
                    textFamily = (FontFamily)( textFamily - 1 );
                    if ( textFamily < FONT_EUROSTILE ) {
                        textFamily = FONT_PRAGMATISM;
                    }
                   // oled.setFont( textFamily );
                    Serial.println( "Font family: " + String( oled.getFontName( textFamily ) ) );
                    oled.clearPrintShow( "Font family: " + String( oled.getFontName( textFamily ) ), 1, textFamily, true, true, true );
                    Jerial.flush();
                } 
                else {
                    textBuffer[ textBufferIndex++ ] = c;
                    if ( textBufferIndex >= 120 ) {
                        textBufferIndex = 0;
                    }
                }

                oled.clearPrintShow( textBuffer, textSize, textFamily, true, true, true );
            }
        }
    
}

// Forward declarations of hold-gate plumbing; definitions live near
// oledPeriodic. show()/flushFramebuffer() check the gate; if held, they
// set the dirty flag and skip the I2C transmit so the panel stays frozen.
//
// oledHoldStashAfterPriorityFlush() is the mate of oledHoldBegin(): the
// priority-flush path in clearPrintShowSmall calls it right after the
// held message has been transmitted to the panel. If oledHoldBegin
// armed a snapshot, we restore that snapshot back into the live
// framebuffer here so writes during the hold accumulate against the
// pre-toast background, not on top of the just-painted toast pixels.
bool oledHoldActive();
extern volatile bool oledNeedsFlushAfterHold;
// Set by show()'s shared-I2C0 gate; oledPeriodic flushes once wavegen stops.
static volatile bool oledNeedsFlushAfterWavegen = false;
static void oledHoldStashAfterPriorityFlush();

// SIMPLIFIED DISPLAY FUNCTIONS
// ============================

// Main display function with all options
void oled::clearPrintShow( const char* text, int textSize, bool clear, bool showOled, bool center, int x_pos, int y_pos, int waitToFinish ) {
    // Early exit if OLED is disabled in config - prevents null pointer crash
    if ( jumperlessConfig.top_oled.enabled == 0 ) {
        return;
    }
    if ( ( !oledConnected || !text ) && stillWriteToFramebuffer == false )
        return;

    // Drawing our own content: let a persistent idle GUI page step aside.
    OledGui::getInstance().notePanelTakenByOther();

    if ( clear ) {
        charPos = 0;
        getDisplay().clearDisplay( );
    }

    // Auto-detect and switch framebuffer mode based on text size
    autoDetectMode( textSize );

    // Fit calculations must be done with wrapping disabled; otherwise width
    // can be underestimated and later render calls may spill off-screen.
    getDisplay().setTextWrap( false );

    // Handle \31 character replacement FIRST - keep String alive for function scope
    String processedText;
    if ( strchr( text, '\31' ) != nullptr ) {
        processedText = String( text );       // Create a copy
        processedText.replace( '\31', '\n' ); // Modify in place (returns void)
        text = processedText.c_str( );
    }

    // Check if multi-line text BEFORE scaling logic
    bool isMultiLine = ( strchr( text, '\n' ) != nullptr );

    // NEW GRANULAR FONT SCALING: Use FontManager to find best-fit point size
    // Context-aware: multi-line needs conservative start, single-line can be aggressive
    uint8_t desiredPointSize;
    if ( isMultiLine ) {
        // Multi-line: Use display-aware sizing (more conservative for small displays)
        desiredPointSize = FontManager::textSizeToPointSize( textSize, displayHeight );
    } else {
        // Single-line: Use full requested size (height is not a constraint)
        if ( textSize <= 0 )
            desiredPointSize = 5;
        else if ( textSize == 1 )
            desiredPointSize = 9;
        else
            desiredPointSize = 12; // Full size for single-line
    }
    uint8_t currentPt = desiredPointSize;

    // Leave a little headroom so near-edge strings don't oscillate in size
    // (e.g. "channel 0" vs "channel 1") and don't unexpectedly wrap.
    int16_t safeWidth = displayWidth;
    if ( displayWidth > 32 ) {
        safeWidth -= 4;
    } else if ( displayWidth > 16 ) {
        safeWidth -= 2;
    }

    // For explicit multi-line text at small sizes, preflight each line at minimum
    // point size. If any line still won't fit, use compact scrolling text mode.
    if ( isMultiLine && textSize <= 1 ) {
        setFontPointSize( currentFontFamily, 5 );
        setTextSize( 1 );
        this->currentTextSize = 1;

        const char* lineStart = text;
        const char* p = text;
        bool fallbackToSmallText = false;
        while ( true ) {
            if ( *p == '\n' || *p == '\0' ) {
                size_t len = (size_t)( p - lineStart );
                if ( len > 0 ) {
                    char lineBuf[ 96 ];
                    size_t copyLen = ( len < sizeof( lineBuf ) - 1 ) ? len : ( sizeof( lineBuf ) - 1 );
                    strncpy( lineBuf, lineStart, copyLen );
                    lineBuf[ copyLen ] = '\0';
                    TextBounds b = getTextBounds( lineBuf );
                    if ( b.width > safeWidth ) {
                        fallbackToSmallText = true;
                        break;
                    }
                }
                if ( *p == '\0' ) {
                    break;
                }
                lineStart = p + 1;
            }
            p++;
        }

        if ( fallbackToSmallText ) {
            showMultiLineSmallText( text, clear, showOled );
            getDisplay().setTextWrap( true );
            return;
        }
    }

    // Try to find largest font that fits (checking BOTH width and height)
    setFontPointSize( currentFontFamily, currentPt );
    setTextSize( 1 );
    this->currentTextSize = 1; // Always use native font sizes

    // Intelligent scaling for both single and multi-line text
    int wrap = 0;
    bool textFitsDisplay = false;

    while ( !textFitsDisplay && currentPt > 5 ) {
        setFontPointSize( currentFontFamily, currentPt );

        if ( isMultiLine ) {
            // For multi-line: check each line width + total height with font-specific characteristics
            FontMetrics metrics = getFontMetrics( );
            FontFamilyCharacteristics chars = getFontCharacteristics( currentFontFamily );

            int lineCount = 1;
            int maxLineWidth = 0;
            const char* lineStart = text;
            const char* p = text;

            // Count lines and find widest line
            while ( *p ) {
                if ( *p == '\n' ) {
                    // Measure this line
                    char lineBuf[ 64 ];
                    int len = p - lineStart;
                    if ( len > 0 && len < 63 ) {
                        strncpy( lineBuf, lineStart, len );
                        lineBuf[ len ] = '\0';
                        TextBounds bounds = getTextBounds( lineBuf );
                        if ( bounds.width > maxLineWidth ) {
                            maxLineWidth = bounds.width;
                        }
                    }
                    lineCount++;
                    lineStart = p + 1;
                }
                p++;
            }
            // Measure last line
            if ( *lineStart ) {
                TextBounds bounds = getTextBounds( lineStart );
                if ( bounds.width > maxLineWidth ) {
                    maxLineWidth = bounds.width;
                }
            }

            // Calculate total height with font-specific line spacing multiplier
            int adjustedLineHeight = (int)( metrics.lineHeight * chars.lineSpacingMultiplier );
            int totalHeight = lineCount * adjustedLineHeight;

            // Check if it fits with clipping tolerance
            // Allow some pixels to clip (for descenders like 'p', 'g', 'q')
            textFitsDisplay = ( maxLineWidth <= ( safeWidth + chars.horizontalClipTolerance ) &&
                                totalHeight <= ( displayHeight + chars.verticalClipTolerance ) );
        } else {
            // Single line: require a little width headroom for consistent sizing.
            TextBounds bounds = getTextBounds( text );
            textFitsDisplay = ( bounds.width <= safeWidth && bounds.height <= displayHeight );
        }

        if ( !textFitsDisplay ) {
            currentPt--;
            if ( currentPt <= 5 ) {
                wrap = 1;
                break;
            }
        }
    }
    // Serial.print("Final point size: ");
    // Serial.println(currentPt);

    // If single-line text still can't fit at minimum size, switch to compact small-text layout.
    if ( !isMultiLine && !textFitsDisplay ) {
        showMultiLineSmallText( text, clear, showOled );
        getDisplay().setTextWrap( true );
        return;
    }

    // Handle multi-line text
    if ( isMultiLine ) {
#if OLED_SCALE_LINES_INDEPENDENTLY
        // When per-line scaling is enabled, skip the global scaling for multi-line
        // and let displayMultiLineText handle it per line
        setFontPointSize( currentFontFamily, desiredPointSize );
        setTextSize( 1 );
        this->currentTextSize = 1;
#endif
        displayMultiLineText( text, center );
    } else {
        // Single line text
        int16_t x, y;

        if ( center || x_pos == -1 || y_pos == -1 ) {
            getCenteredPosition( text, &x, &y );
        } else {
            x = x_pos;
            y = y_pos;
        }

        setCursor( x, y );
        getDisplay().print( text );
        charPos += strlen( text );
    }

    getDisplay().setTextWrap( true );

    if ( showOled ) {
        show( waitToFinish );
        // getDisplay().display();
        // dumpFrameBufferQuarterSize(1);
    }
}

#define LEFT_JUSTIFY_TOP 8

// Render `text` using two fixed fonts (one per line) with explicit
// 2-line vertical centering on the 128x32 panel. Bypasses the auto-fit
// ladder in the regular clearPrintShow so a fixed UI element doesn't
// flip sizes when the label width straddles a tier boundary.
//
// PRIORITY: also bypasses the OLED hold gate, so a rapid sequence of
// toasts (e.g. multiple undos in <1.5s) always displays the most recent
// label immediately instead of being deferred until the prior hold
// expires. Each call still extends the hold window via the caller's
// oledHold() so non-priority writes stay frozen out.
//
// Each newline-separated line is horizontally centered with its own
// font. Up to 2 lines are rendered; if the caller passes more, only
// the first 2 are used. Defaults are tuned for the undo toast (larger
// verb + smaller detail); pass equal top/bot indices for matched sizes.
void oled::clearPrintShowRich( const OledTextRow* rows, int rowCount,
                               int rowGap, bool clear, bool show,
                               bool topAnchor ) {
    if ( jumperlessConfig.top_oled.enabled == 0 ) return;
    if ( ( !oledConnected || !rows ) && stillWriteToFramebuffer == false ) return;
    if ( rowCount < 1 ) return;

    // Drawing our own content: let a persistent idle GUI page step aside.
    OledGui::getInstance().notePanelTakenByOther();

    const int kFontCount = (int)( sizeof( fontList ) / sizeof( fontList[0] ) );

    if ( clear ) {
        clearFramebuffer();
    }

    // Save current font state - other code paths depend on it not changing.
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;
    uint8_t savedTextSize = currentTextSize;

    getDisplay().setTextWrap( false );
    getDisplay().setTextSize( 1 );
    currentTextSize = 1;

    // Per-segment measured geometry, plus per-row aggregate width/height.
    // Each row's height is the tallest segment so the shared baseline sits
    // at rowTop + rowHeight; smaller segments on the same row hang their
    // baseline from there (baseline alignment) rather than top alignment.
    // `flowW` is the packed width of only the INHERIT (flowing) segments -
    // segments with their own align anchor independently and don't take up
    // space in the flowing group.
    int16_t segW[8][OLED_RICH_MAX_SEGS] = {{0}};
    int16_t segH[8][OLED_RICH_MAX_SEGS] = {{0}};
    int flowW[8] = {0};
    int rowH[8] = {0};
    if ( rowCount > 8 ) rowCount = 8;

    int blockHeight = 0;
    for ( int r = 0; r < rowCount; r++ ) {
        const OledTextRow& row = rows[r];
        int n = row.segCount;
        if ( n > OLED_RICH_MAX_SEGS ) n = OLED_RICH_MAX_SEGS;
        int gap = row.segGap;
        int flowTotal = 0;
        int maxH = 0;
        int flowDrawn = 0;
        for ( int s = 0; s < n; s++ ) {
            const char* t = row.segs[s].text;
            if ( !t || !t[0] ) continue;
            int fi = row.segs[s].fontIndex;
            if ( fi < 0 || fi >= kFontCount ) fi = 18;
            int16_t w = 0, h = 0;
            getTextBoundsWithFont( getDisplay(), fontList[fi].font, t, &w, &h );
            segW[r][s] = w;
            segH[r][s] = h;
            if ( h > maxH ) maxH = h;
            if ( row.segs[s].align == OLED_ALIGN_INHERIT ) {
                if ( flowDrawn > 0 ) flowTotal += gap;
                flowTotal += w;
                flowDrawn++;
            }
        }
        flowW[r] = flowTotal;
        rowH[r] = ( row.fixedH > 0 ) ? row.fixedH : maxH;
        blockHeight += rowH[r];
        if ( r > 0 ) blockHeight += rowGap;
    }

    int yTop = topAnchor ? 0 : ( displayHeight - blockHeight ) / 2;
    if ( yTop < 0 ) yTop = 0;

    // Resolve a left-edge x for a given alignment and content width.
    // Right-aligned segments keep a margin so the last glyphs don't sit
    // hard against (or clip at) the panel edge.
    auto anchorX = [&]( OledAlign a, int w ) -> int {
        int x;
        switch ( a ) {
            case OLED_ALIGN_CENTER: x = ( displayWidth - w ) / 2; break;
            case OLED_ALIGN_RIGHT:  x = displayWidth - w - 5;      break;
            case OLED_ALIGN_LEFT:
            default:                x = LEFT_JUSTIFY_TOP;          break;
        }
        if ( x < 0 ) x = 0;
        return x;
    };

    int y = yTop;
    for ( int r = 0; r < rowCount; r++ ) {
        if ( topAnchor && r == 1 ) {
            // Header stays pinned at the top; center the VALUE rows in the
            // space below it. Without this, a single big value row sat
            // right under the header with dead space at the bottom.
            int rest = 0;
            for ( int rr = 1; rr < rowCount; rr++ ) {
                rest += rowH[rr];
                if ( rr > 1 ) rest += rowGap;
            }
            int pad = ( displayHeight - y - rest ) / 2;
            if ( pad > 0 ) y += pad;
        }
        const OledTextRow& row = rows[r];
        int n = row.segCount;
        if ( n > OLED_RICH_MAX_SEGS ) n = OLED_RICH_MAX_SEGS;
        int gap = row.segGap;
        int baseline = y + rowH[r];

        // Flowing (INHERIT) segments pack together and the group is placed
        // using the row's alignment.
        int flowX = anchorX( row.align, flowW[r] );

        int flowDrawn = 0;
        for ( int s = 0; s < n; s++ ) {
            const char* t = row.segs[s].text;
            if ( !t || !t[0] ) continue;
            int fi = row.segs[s].fontIndex;
            if ( fi < 0 || fi >= kFontCount ) fi = 18;
            getDisplay().setFont( fontList[fi].font );
            currentFont = fontList[fi].font;
            currentFontFamily = fontList[fi].family;

            int x;
            if ( row.segs[s].align == OLED_ALIGN_INHERIT ) {
                if ( flowDrawn > 0 ) flowX += gap;
                x = flowX;
                flowX += segW[r][s];
                flowDrawn++;
            } else {
                // Independently-anchored segment: pin to its own edge and
                // leave the flowing group's layout untouched.
                x = anchorX( row.segs[s].align, segW[r][s] );
            }
            // Adafruit GFX uses a baseline cursor for custom fonts; placing
            // every segment at the shared baseline keeps mixed sizes aligned.
            setCursor( x, baseline, POS_BASELINE );
            getDisplay().print( t );
        }

        y += rowH[r] + rowGap;
    }

    if ( show && oledConnected ) {
        priorityFlushHeldFrame();
    }

    // Restore prior font state so callers that rely on it aren't surprised.
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    currentTextSize = savedTextSize;
    getDisplay().setFont( currentFont );
    getDisplay().setTextSize( currentTextSize );
}

void oled::priorityFlushHeldFrame() {
    // PRIORITY FLUSH: bypass the hold gate so back-to-back toasts (e.g. rapid
    // undo / redo) always display the most recent frame immediately, replacing
    // whatever was painted by an earlier toast that is still inside its hold
    // window. Clearing the dirty flag here means oledPeriodic won't
    // double-flush at hold expiry.
    //
    // This bypasses show(), so it must honor show()'s shared-I2C0 gate
    // itself: no panel transmit while wavegen streams the DAC on Wire from
    // core 1 (no cross-core bus lock exists). Defer: the toast stays in the
    // live framebuffer and oledPeriodic flushes it once streaming stops.
    {
        extern WaveGen wavegen;
        if ( _currentDisplayWire == 0 && wavegen.isRunning( ) ) {
            oledNeedsFlushAfterWavegen = true;
            return;
        }
    }
    if ( _displayPtr ) {
        _displayPtr->display();
    }
    oledNeedsFlushAfterHold = false;
    // If oledHoldBegin armed a snapshot, the panel now shows the held message
    // but the live framebuffer still contains those toast pixels. Restore the
    // snapshot so subsequent writes during the hold accumulate against the
    // pre-toast background; when the hold expires, the panel transitions
    // cleanly to background+writes with no toast residue. No-op if not armed.
    // The stash also copies live -> oledPanelShadow first, so the dump call
    // below sees the toast via oledGetDumpBuffer.
    oledHoldStashAfterPriorityFlush();
    // Mirror what show() does for normal flushes: if terminal mirroring is
    // enabled, dump the just-shipped frame so the user sees the toast in the
    // terminal too. Without this, the toast hits the panel via the priority
    // flush above but never appears in the terminal stream (this path bypasses
    // show() and thus its dumpFrameBufferQuarterSize hook). oledGetDumpBuffer
    // returns the panel shadow during the now-active hold, so the dump matches
    // the panel.
    if ( jumperlessConfig.top_oled.show_in_terminal > 0 ) {
        dumpFrameBufferQuarterSize( 1 );
        Serial.flush();
    }
    streamFrameToSer3();
}

// Backward-compatible two-line helper. Splits `text` on a single '\n'
// into a top row (font topFontIndex) and bottom row (font botFontIndex),
// then defers to the granular clearPrintShowRich. Top row honors
// leftJustifyTop; bottom row is always centered (matching prior behavior).
void oled::clearPrintShowSmall( const char* text, bool clear, bool show,
                                int topFontIndex, int botFontIndex,
                                int line_gap, bool leftJustifyTop ) {
    if ( !text ) return;

    char line0[48] = {0};
    char line1[48] = {0};
    int idx = 0;
    int slot = 0;
    char* slots[2] = { line0, line1 };
    while ( text[idx] != '\0' && slot < 2 ) {
        if ( text[idx] == '\n' ) {
            slot++;
            idx++;
            continue;
        }
        size_t curLen = strlen( slots[slot] );
        if ( curLen < sizeof(line0) - 1 ) {
            slots[slot][curLen] = text[idx];
            slots[slot][curLen + 1] = '\0';
        }
        idx++;
    }
    int lineCount = (line1[0] != '\0') ? 2 : (line0[0] != '\0' ? 1 : 0);
    if ( lineCount == 0 ) {
        // Empty input: still honor clear/show by rendering one empty row
        // (draws nothing but performs the clear + priority flush).
        OledTextRow empty = {};
        empty.segs[0] = { "", (int16_t)topFontIndex, OLED_ALIGN_INHERIT };
        empty.segCount = 1;
        empty.align = leftJustifyTop ? OLED_ALIGN_LEFT : OLED_ALIGN_CENTER;
        clearPrintShowRich( &empty, 1, line_gap, clear, show );
        return;
    }

    OledTextRow rows[2] = {};
    rows[0].segs[0] = { line0, (int16_t)topFontIndex, OLED_ALIGN_INHERIT };
    rows[0].segCount = 1;
    rows[0].align = leftJustifyTop ? OLED_ALIGN_LEFT : OLED_ALIGN_CENTER;
    rows[0].segGap = 0;
    if ( lineCount == 2 ) {
        rows[1].segs[0] = { line1, (int16_t)botFontIndex, OLED_ALIGN_INHERIT };
        rows[1].segCount = 1;
        rows[1].align = OLED_ALIGN_CENTER;
        rows[1].segGap = 0;
    }

    clearPrintShowRich( rows, lineCount, line_gap, clear, show );
}

// Main display function with font family selection
void oled::clearPrintShow( const char* text, int textSize, FontFamily family, bool clear, bool showOled, bool center, int x_pos, int y_pos, int waitToFinish ) {
    // Early exit if OLED is disabled in config - prevents null pointer crash
    if ( jumperlessConfig.top_oled.enabled == 0 ) {
        return;
    }
    if ( ( !oledConnected || !text ) && stillWriteToFramebuffer == false )
        return;

    // Drawing our own content: let a persistent idle GUI page step aside.
    OledGui::getInstance().notePanelTakenByOther();

    if ( clear ) {
        charPos = 0;
        getDisplay().clearDisplay( );
    }

    // Use smart font selection with specified family
    setFontForSize( family, textSize );

    // Keep wrapping disabled during fit checks and rendering for consistent
    // single-line behavior.
    getDisplay().setTextWrap( false );

    // Always use text size 1 since we're using native font sizes
    setTextSize( 1 );
    this->currentTextSize = 1; // Track that we're using native fonts

    // Keep a small width margin to avoid inconsistent near-threshold scaling.
    int16_t safeWidth = displayWidth;
    if ( displayWidth > 32 ) {
        safeWidth -= 4;
    } else if ( displayWidth > 16 ) {
        safeWidth -= 2;
    }

    // For explicit multi-line text at small sizes, preflight each line at minimum
    // point size. If any line still won't fit, use compact scrolling text mode.
    bool isMultiLine = ( strchr( text, '\n' ) != nullptr );
    if ( isMultiLine && textSize <= 1 ) {
        setFontPointSize( family, 5 );
        setTextSize( 1 );
        this->currentTextSize = 1;

        const char* lineStart = text;
        const char* p = text;
        bool fallbackToSmallText = false;
        while ( true ) {
            if ( *p == '\n' || *p == '\0' ) {
                size_t len = (size_t)( p - lineStart );
                if ( len > 0 ) {
                    char lineBuf[ 96 ];
                    size_t copyLen = ( len < sizeof( lineBuf ) - 1 ) ? len : ( sizeof( lineBuf ) - 1 );
                    strncpy( lineBuf, lineStart, copyLen );
                    lineBuf[ copyLen ] = '\0';
                    TextBounds b = getTextBounds( lineBuf );
                    if ( b.width > safeWidth ) {
                        fallbackToSmallText = true;
                        break;
                    }
                }
                if ( *p == '\0' ) {
                    break;
                }
                lineStart = p + 1;
            }
            p++;
        }

        if ( fallbackToSmallText ) {
            showMultiLineSmallText( text, clear, showOled );
            getDisplay().setTextWrap( true );
            return;
        }
    }

    auto fitsAtCurrentFont = [&]( const char* candidate ) {
        TextBounds bounds = getTextBounds( candidate );
        return ( bounds.width <= safeWidth && bounds.height <= displayHeight );
    };

    bool textFitsDisplay = fitsAtCurrentFont( text );
    while ( !textFitsDisplay && textSize > 1 ) {
        textSize--;
        setFontForSize( family, textSize );
        textFitsDisplay = fitsAtCurrentFont( text );
    }

    if ( !isMultiLine && !textFitsDisplay ) {
        showMultiLineSmallText( text, clear, showOled );
        getDisplay().setTextWrap( true );
        return;
    }

    // Handle multi-line text
    if ( isMultiLine ) {
        displayMultiLineText( text, center );
    } else {
        // Single line text
        int16_t x, y;

        if ( center || x_pos == -1 || y_pos == -1 ) {
            getCenteredPosition( text, &x, &y );
        } else {
            x = x_pos;
            y = y_pos;
        }

        setCursor( x, y );
        getDisplay().print( text );
        charPos += strlen( text );
    }

    getDisplay().setTextWrap( true );

    if ( showOled ) {
        show( waitToFinish );
        // getDisplay().display();
        // dumpFrameBufferQuarterSize(1);
    }
}

// Helper for multi-line text display
void oled::displayMultiLineText( const char* text, bool center ) {
    // Use fixed array instead of vector for embedded compatibility
    String lines[ 8 ]; // Support up to 8 lines
    int lineCount = 0;
    String textStr = String( text );
    int start = 0;

    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    int pos = 0;

    String textQuotedStr;
    if ( strchr( text, '\31' ) != nullptr ) {
        textQuotedStr = String( text );
        textQuotedStr.replace( '\31', '\n' );
        text = textQuotedStr.c_str( );
    }

    // Split by newlines
    while ( pos < textStr.length( ) && lineCount < 8 ) {
        if ( textStr[ pos ] == '\n' || pos == textStr.length( ) - 1 ) {
            int end = ( textStr[ pos ] == '\n' ) ? pos : pos + 1;
            String line = textStr.substring( start, end );
            if ( line.length( ) > 0 ) {
                lines[ lineCount++ ] = line;
            }
            start = pos + 1;
        }
        pos++;
    }

    // Calculate positioning - fit all lines with bottom baseline at screen bottom
    FontMetrics metrics = getFontMetrics( );
    FontFamilyCharacteristics chars = getFontCharacteristics( currentFontFamily );

    // Apply font-specific line spacing multiplier (e.g., Iosevka uses 0.85 for tighter spacing)
    int16_t lineSpacing = (int16_t)( metrics.lineHeight * chars.lineSpacingMultiplier );

    // Calculate actual ascent and descent for each line and find maximum
    int16_t maxActualAscent = 0;
    int16_t maxActualDescent = 0;
    for ( int i = 0; i < lineCount; i++ ) {
        TextBounds lineBounds = getTextBounds( lines[ i ].c_str( ) );
        if ( lineBounds.ascent > maxActualAscent ) {
            maxActualAscent = lineBounds.ascent;
        }
        if ( lineBounds.descent > maxActualDescent ) {
            maxActualDescent = lineBounds.descent;
        }
    }

    // Use actual measurements if reasonable, otherwise fall back to font metrics
    int16_t ascentToUse = ( maxActualAscent > 0 ) ? maxActualAscent : metrics.ascent;
    int16_t descentToUse = ( maxActualDescent > 0 ) ? maxActualDescent : metrics.descent;

    // Calculate total ascent span needed
    int16_t totalAscentSpan = ( lineCount - 1 ) * lineSpacing + ascentToUse;

    // If it doesn't fit, compress line spacing to fit all ascent areas
    if ( totalAscentSpan > displayHeight ) {
        // Compress spacing: available space minus ascent, divided by number of gaps
        if ( lineCount > 1 ) {
            lineSpacing = ( displayHeight - ascentToUse ) / ( lineCount - 1 ) - 3;
            // Ensure minimum spacing of at least ascent height
            if ( lineSpacing < ascentToUse ) {
                lineSpacing = ascentToUse - 3;
            }
        }
    }

    // CENTER the entire text block vertically
    // Calculate total height: from top of first line to bottom of last line
    int16_t totalTextHeight = ascentToUse + ( lineCount - 1 ) * lineSpacing + abs( descentToUse );

    // Calculate first line baseline to center the text block
    // Center of block should be at displayHeight/2
    // Top of text is at: firstLineBaseline - ascentToUse
    // Bottom of text is at: firstLineBaseline + (lineCount-1)*lineSpacing + abs(descentToUse)
    // Center = (top + bottom) / 2 = firstLineBaseline + ((lineCount-1)*lineSpacing + abs(descentToUse) - ascentToUse) / 2
    // We want: center = displayHeight/2
    // So: firstLineBaseline = displayHeight/2 - ((lineCount-1)*lineSpacing + abs(descentToUse) - ascentToUse) / 2
    int16_t firstLineBaseline = ( displayHeight / 2 ) + ( ascentToUse - ( lineCount - 1 ) * lineSpacing - abs( descentToUse ) ) / 2;

    // Apply font-specific topRowOffset for fine-tuning (usually 0 for most fonts)
    for ( int i = 0; i < numFonts; i++ ) {
        if ( fontList[ i ].font == currentFont && fontList[ i ].topRowOffset != 0 ) {
            if ( displayHeight >= 24 ) { // Multi-line capable display
                firstLineBaseline += fontList[ i ].topRowOffset;
            }
            break;
        }
    }

    // Display each line
#if OLED_SCALE_LINES_INDEPENDENTLY
    // Calculate vertical nudge only when ANY line needs to be scaled down
    int16_t verticalNudge = 0;
    uint8_t originalPt = this->currentPointSize;
    FontFamily originalFamily = currentFontFamily;
    uint8_t minScaledPt = originalPt; // Track the smallest font we scale to
    
    // CRITICAL: Disable text wrapping before measuring - this must match the rendering loop
    getDisplay().setTextWrap( false );
    
    // Check ALL lines to see if any need scaling
    for (int i = 0; i < lineCount; i++) {
        uint8_t linePt = originalPt;
        bool lineFits = false;
        uint8_t lastTestedPt = 255; // Track to avoid infinite loop if font doesn't exist
        FontFamily currentCheckFamily = originalFamily;
        
        while ( linePt >= 5 && !lineFits ) {
            setFontPointSize( currentCheckFamily, linePt );
            
            // Check if we actually got a different font (some families don't have all sizes)
            uint8_t actualPt = this->currentPointSize;
            if (actualPt == lastTestedPt) {
                // Font size didn't change, this family doesn't have smaller fonts
                // Switch to Andale Mono (5pt) for very small text
                if (currentCheckFamily != FONT_ANDALE_MONO) {
                    currentCheckFamily = FONT_ANDALE_MONO;
                    linePt = 5; // Andale Mono 5pt
                    lastTestedPt = 255; // Reset to allow checking Andale Mono
                    continue;
                } else {
                    // Already tried Andale Mono, text will just have to clip
                    break;
                }
            }
            lastTestedPt = actualPt;
            
            TextBounds bounds = getTextBounds( lines[i].c_str() );
            lineFits = ( bounds.width <= displayWidth );
            if ( !lineFits ) {
                linePt--;
            }
        }
        
        // Track the smallest font size we need across all lines
        if (linePt < minScaledPt) {
            minScaledPt = linePt;
        }

        // Serial.print("linePt: ");
        // Serial.println(linePt);
        // Serial.flush();
        // Serial.print("minScaledPt: ");
        // Serial.println(minScaledPt);
        // Serial.flush();
        // Serial.print("originalPt: ");
        // Serial.println(originalPt);
        // Serial.flush();
    }
    
    // Only add nudge if ANY line was scaled down
    if (minScaledPt < originalPt) {
        verticalNudge = (originalPt - minScaledPt) / 2 - 2; // 2 base pixels plus half the reduction
        // Serial.print("verticalNudge: ");
        // Serial.println(verticalNudge);
        // Serial.flush();
    }
    
    // Restore original font for main loop
    setFontPointSize( originalFamily, originalPt );
#endif
    
    for ( int i = 0; i < lineCount; i++ ) {
        int16_t lineX = 0;
        int16_t lineY = firstLineBaseline + ( i * lineSpacing );

#if OLED_SCALE_LINES_INDEPENDENTLY
        // Apply vertical nudge to all lines
        lineY += verticalNudge;
        
        // Scale each line independently to fit horizontally
        // Store original font settings (already stored above, reuse variables)
        uint8_t originalPtForLine = this->currentPointSize;
        FontFamily originalFamilyForLine = currentFontFamily;
        
        // Disable text wrapping - we want to scale, not wrap
        getDisplay().setTextWrap( false );
        
        // Find largest font size that fits this line horizontally
        uint8_t linePt = originalPtForLine;
        bool lineFits = false;
        uint8_t lastTestedPt = 255; // Track to avoid infinite loop if font doesn't exist
        FontFamily currentRenderFamily = originalFamilyForLine;
        
        while ( linePt >= 5 && !lineFits ) {
            setFontPointSize( currentRenderFamily, linePt );
            setTextSize( 1 );
            this->currentTextSize = 1;
            
            // Check if we actually got a different font (some families don't have all sizes)
            uint8_t actualPt = this->currentPointSize;
            if (actualPt == lastTestedPt) {
                // Font size didn't change, this family doesn't have smaller fonts
                // Switch to Andale Mono (5pt) for very small text
                if (currentRenderFamily != FONT_ANDALE_MONO) {
                    currentRenderFamily = FONT_ANDALE_MONO;
                    linePt = 5; // Andale Mono 5pt
                    lastTestedPt = 255; // Reset to allow checking Andale Mono
                    continue;
                } else {
                    // Already tried Andale Mono, text will just have to clip
                    break;
                }
            }
            lastTestedPt = actualPt;
            
            // Check if this line fits horizontally
            TextBounds bounds = getTextBounds( lines[ i ].c_str( ) );
            lineFits = ( bounds.width <= displayWidth );
            
            if ( !lineFits ) {
                linePt--;
            }
        }
        
        // If we had to scale down, recalculate metrics for this line
        if ( linePt != originalPt ) {
            FontMetrics lineMetrics = getFontMetrics( );
            // Adjust Y position for this specific font size to maintain baseline alignment
            // The baseline should remain consistent, so we don't need to adjust lineY
        }
#else
        // Standard mode: disable wrapping globally for multi-line display
        getDisplay().setTextWrap( false );
#endif

        if ( center ) {
            // Get horizontal centering only
            int16_t tempY;
            getCenteredPosition( lines[ i ].c_str( ), &lineX, &tempY );
            if ( lineX < 0 ) {
                lineX = 0;
            }
            // Keep our calculated Y position for proper line spacing
        }

        setCursor( lineX, lineY, POS_BASELINE ); // Use baseline for multi-line
        getDisplay().print( lines[ i ] );
        charPos += lines[ i ].length( );

#if OLED_SCALE_LINES_INDEPENDENTLY
        // Restore original font for next line (so each line is evaluated independently)
        setFontPointSize( originalFamilyForLine, originalPtForLine );
        setTextSize( 1 );
        this->currentTextSize = 1;
#endif
    }
    
    // Re-enable text wrapping after multi-line display
    getDisplay().setTextWrap( true );
}

// Simplified overloads
void oled::clearPrintShow( const char* text, int textSize, int waitToFinish ) {
    clearPrintShow( text, textSize, true, true, true, -1, -1, waitToFinish );
}

void oled::clearPrintShow( const String& text, int textSize, int waitToFinish ) {
    clearPrintShow( text.c_str( ), textSize, true, true, true, -1, -1, waitToFinish );
}

void oled::clearPrintShow( const String& text, int textSize, bool clear, bool showOled, bool center, int x_pos, int y_pos, int waitToFinish ) {
    clearPrintShow( text.c_str( ), textSize, clear, showOled, center, x_pos, y_pos, waitToFinish );
}

void oled::clearPrintShow( const String& text, int textSize, FontFamily family, bool clear, bool showOled, bool center, int x_pos, int y_pos, int waitToFinish ) {
    clearPrintShow( text.c_str( ), textSize, family, clear, showOled, center, x_pos, y_pos, waitToFinish );
}

// BASIC PRINT FUNCTIONS
// ====================

void oled::print( const char* s ) {
    if ( ( !oledConnected || !s ) && stillWriteToFramebuffer == false )
        return;

    // Auto-adjust cursor if at top of screen - simplified
    int16_t currentY = getDisplay().getCursorY( );
    if ( currentFont && currentY <= 4 ) {
        setCursor( getDisplay().getCursorX( ), 0, POS_TIGHT );
    }

    if ( currentTextSize > 2 ) {
        currentTextSize = 2;
        setFontForSize( currentFontFamily, currentTextSize );
    }

    String processedStr;
    if ( strchr( s, '\31' ) != nullptr ) {
        processedStr = String( s );
        processedStr.replace( '\31', '\n' );
        s = processedStr.c_str( );
    }

    getDisplay().print( s );
    charPos += strlen( s );
}

void oled::print( const String& s ) {
    print( s.c_str( ) );
}

void oled::print( int i ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;
    getDisplay().print( i );
    charPos += String( i ).length( );
}

void oled::print( float f ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;
    getDisplay().print( f );
    charPos += String( f ).length( );
}

void oled::print( char c ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;
    getDisplay().print( c );
    charPos += 1;
}

// Println functions
void oled::println( const char* s ) {
    print( s );
    moveToNextLine( );
}

void oled::println( const String& s ) {
    print( s );
    moveToNextLine( );
}

void oled::println( int i ) {
    print( i );
    moveToNextLine( );
}

void oled::println( float f ) {
    print( f );
    moveToNextLine( );
}

// UTILITY FUNCTIONS
// ================

bool oled::clear( int waitToFinish ) {
    // Early exit if OLED is disabled in config - prevents null pointer crash
    if ( jumperlessConfig.top_oled.enabled == 0 ) {
        return false;
    }
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false ) {
        charPos = 0;
        return false;
    }
    charPos = 0;
    getDisplay().clearDisplay( );
    setCursor( 0, 0 ); // Auto-positioning for clear
    // int waited = 0;
    //  if (waitToFinish > 0) {
    //      while (Wire1.finishedAsync() == false) {
    //          delayMicroseconds(1);
    //          waited += 1;
    //          if (waited > waitToFinish) {
    //              break;
    //          }
    //      }
    //  }
    return true;
}

bool oled::show( int waitToFinish ) {

    if ( jumperlessConfig.top_oled.show_in_terminal > 0 ) {
        dumpFrameBufferQuarterSize( 1 );
    }
    // USBSer3 machine stream (independent of the terminal mirror above). Runs
    // before the connected check so the host sees the framebuffer even when no
    // physical panel is attached.
    streamFrameToSer3();
    if ( !oledConnected ) {
        // Serial.println( "OLED not connected" );
        return false;
    }
    // Hold gate: keep the panel frozen on whatever we last pushed. The
    // framebuffer can still mutate (callers above us may have just done
    // so); we just skip the I2C transmit. oledPeriodic flushes the final
    // framebuffer state when the hold expires.
    if ( oledHoldActive() ) {
        oledNeedsFlushAfterHold = true;
        return true;
    }
    // Shared-bus gate: on Wire (I2C0, connection_type 2) this panel shares
    // the bus with the MCP4728 that wavegen streams from core 1 - with no
    // lock between the cores. OledGui::renderNow() already defers for this,
    // but measure mode and the highlight sink call show()/clearPrintShow()
    // directly and bypassed that gate; putting it here covers every
    // ship-to-panel path. Skip the transmit and let oledPeriodic flush the
    // (still-mutating) framebuffer once streaming stops.
    {
        extern WaveGen wavegen;
        if ( _currentDisplayWire == 0 && wavegen.isRunning( ) ) {
            oledNeedsFlushAfterWavegen = true;
            return true;
        }
    }
    // Opportunistic liveness check: checkConnection() caches for 1s
    // internally, so this is a 1-byte I2C ping at most once per second
    // and a no-op otherwise. The point is to flip oledConnected to false
    // *before* we ship a 1024-byte display() write down a dead bus and
    // pay 15ms x N transactions worth of UI lag before oledPeriodic
    // notices on its own.
    if ( !checkConnection( false ) ) {
        return false;
    }
    if (_displayPtr != nullptr) {
        _displayPtr->display( );
    }

    // Post-write liveness verification with hysteresis.
    //
    // Adafruit_SSD1306::display() unconditionally discards every
    // wire->endTransmission() return value, so we can't directly observe
    // whether the ~33 I2C transactions that make up a frame actually
    // landed. Instead, we do one zero-byte addressed ping right after
    // display() returns: if the device is still there it ACKs in ~25us;
    // if it's gone (or wedged), the 15ms Wire timeout we set in
    // oled::init() bounds the cost.
    //
    // Why hysteresis (N=3) instead of mark-dead-immediately:
    //   * Real-world I2C buses have occasional single-transaction glitches
    //     (bus crosstalk, ESD, the user touching a node). A single NACK
    //     should not pull the display offline and pay the full reinit
    //     dance on the next frame.
    //   * Three consecutive failures across multiple frames is a strong
    //     "device is actually gone" signal that's still detected well
    //     inside the ~750ms periodic-poll latency.
    //
    // The counter is reset on any success so transient failures decay
    // away rather than accumulating across hours of operation.
    {
        static int consecutiveWriteFailures = 0;
        constexpr int kWriteFailureThreshold = 3;
        TwoWire& wire = ( _currentDisplayWire == 0 ) ? Wire : Wire1;
        wire.beginTransmission( address );
        int err = wire.endTransmission( );
        if ( err != 0 ) {
            consecutiveWriteFailures++;
            #if OLED_DEBUG
            Serial.printf( "[OLED] show(): post-write ping NACK (err=%d) "
                           "consec=%d/%d\n",
                err, consecutiveWriteFailures, kWriteFailureThreshold );
            #endif
            if ( consecutiveWriteFailures >= kWriteFailureThreshold ) {
                #if OLED_DEBUG
                Serial.println( "[OLED] show(): marking disconnected after "
                                "N consecutive write failures" );
                #endif
                oledConnected = false;
                consecutiveWriteFailures = 0;
                // Force the next checkConnection() / oledPeriodic() to
                // re-probe immediately rather than waiting on the 1s
                // cache; the periodic-task reconnect path handles full
                // reinit when the device returns.
                lastConnectionCheck = 0;
                return false;
            }
        } else if ( consecutiveWriteFailures != 0 ) {
            consecutiveWriteFailures = 0;
        }
    }
    // int waited = 0;
    // if (waitToFinish > 0) {
    //     while (Wire1.finishedAsync() == false) {
    //         delayMicroseconds(1);
    //         waited += 1;
    //         if (waited > waitToFinish) {
    //             Serial.println("OLED show timed out");
    //             break;
    //         }
    //     }
    // }
    return true;
}

void oled::moveToNextLine( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    FontMetrics metrics = getFontMetrics( );
    int16_t currentY = getDisplay().getCursorY( );
    int16_t nextY = currentY + metrics.lineHeight;

    if ( nextY >= displayHeight ) {
        nextY = metrics.ascent; // Wrap to top
    }

    setCursor( 0, nextY, POS_BASELINE );
}

// Display settings
void oled::setTextSize( uint8_t size ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    if ( size > 2 ) {
        size = 2;
    }
    this->currentTextSize = size;
    getDisplay().setTextSize( size );
}

void oled::setTextColor( uint32_t color ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().setTextColor( color );
}

void oled::invertDisplay( bool inv ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().invertDisplay( inv );
}

// Small text functions for file browser and detailed display
void oled::printSmallText( const char* text, int16_t x, int16_t y, bool clear ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    if ( clear ) {
        getDisplay().clearDisplay( );
    }

    // Store current font before changing
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;

    // Use default small font
    setSmallFont( DEFAULT_SMALL_FONT );
    setCursor( x, y + 8 ); // Adjust Y for 4-5pt font baseline
    getDisplay().print( text );

    // Restore previous font
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    getDisplay().setFont( currentFont );
    usingSmallFont = false;

    if ( clear  ) {
        show();
        //getDisplay().display( );
    }
}

void oled::printSmallTextLine( const char* text, int line, bool clear ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    if ( clear ) {
        getDisplay().clearDisplay( );
    }

    String textStr;
    if ( String( text ).indexOf( '\31' ) != -1 ) {
        textStr = String( text );
        textStr.replace( '\31', '\n' );
        text = textStr.c_str( );
    }
    // Store current font before changing
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;

    // Use default small font
    setSmallFont( DEFAULT_SMALL_FONT );
    setCursor( 0, ( line * 8 ) + 8 ); // Adjust Y for 4-5pt font baseline
    getDisplay().print( text );

    // Restore previous font
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    getDisplay().setFont( currentFont );
    usingSmallFont = false;

    if ( clear && oledConnected ) {
        show();
        //getDisplay().display( );
    }
}

void oled::clearLine( int line ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    // Clear a specific line by drawing a black rectangle
    getDisplay().fillRect( 0, line * 8, displayWidth, 8, SSD1306_BLACK );
}

void oled::showFileStatus( const char* currentPath, int fileCount, const char* selectedFile ) {
    if ( !oledConnected )
        return;

    getDisplay().clearDisplay( );

    // Store current font before changing
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;

    // Use Andale Mono font like eKilo for consistent display
    setSmallFont( SMALL_FONT_ANDALE_MONO );

    // Path display without truncation
    getDisplay().setTextWrap( false );
    setCursor( 0, 8 );
    String path = String( currentPath );
    getDisplay().print( path.c_str( ) );
    getDisplay().print( "/" );

    // Selected file (if provided) with cursor indicator
    if ( selectedFile && strlen( selectedFile ) > 0 ) {
        String selected = String( selectedFile );
        getDisplay().print( selected.c_str( ) );

        // Add cursor indicator - draw underline under the selected file
        TextBounds bounds = getTextBounds( selected.c_str( ) );
        int16_t pathWidth = getTextBounds( ( path + "/" ).c_str( ) ).width;

        // Draw underline to show cursor position
        getDisplay().drawLine( pathWidth, 8 + 2, pathWidth + bounds.width - 1, 8 + 2, SSD1306_WHITE );
    }

    getDisplay().setTextWrap( true );
    show();
    //getDisplay().display( );

    // Restore previous font
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    getDisplay().setFont( currentFont );
    usingSmallFont = false;
}

void oled::showFileStatusBreadboard( const char* lineTop7, const char* lineBottom7 ) {
    if ( !oledConnected )
        return;

    getDisplay().clearDisplay( );

    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;
    setSmallFont( SMALL_FONT_ANDALE_MONO );
    getDisplay().setTextWrap( false );

    static const int BREADBOARD_CHARS = 7;
    char top[ BREADBOARD_CHARS + 1 ] = { 0 };
    char bot[ BREADBOARD_CHARS + 1 ] = { 0 };
    if ( lineTop7 ) {
        strncpy( top, lineTop7, BREADBOARD_CHARS );
        top[ BREADBOARD_CHARS ] = '\0';
    }
    if ( lineBottom7 ) {
        strncpy( bot, lineBottom7, BREADBOARD_CHARS );
        bot[ BREADBOARD_CHARS ] = '\0';
    }

    drawText( 0, 8, top );
    drawText( 0, 20, bot );

    show();
    //getDisplay().display( );

    currentFont = savedFont;
    currentFontFamily = savedFamily;
    getDisplay().setFont( currentFont );
    usingSmallFont = false;
}

void oled::showFileStatusScrolled( const char* visibleText, int fileCount, int cursorPosition ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;

    // Clear display
    clearFramebuffer( );

    // Store current font before changing
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;

    // Use Andale Mono font like eKilo for consistent display
    setSmallFont( SMALL_FONT_ANDALE_MONO );
    getDisplay().setTextWrap( false );

    String text = String( visibleText );
    int newlinePos = text.indexOf( '\n' );

    if ( newlinePos != -1 ) {
        // Multi-line display: path on first line, filename on second line
        String pathLine = text.substring( 0, newlinePos );
        String filenameLine = text.substring( newlinePos + 1 );

        // Draw path on first line
        drawText( 0, 8, pathLine.c_str( ) );

        // Draw filename on second line
        drawText( 0, 20, filenameLine.c_str( ) );

        // Highlight cursor character
        if ( cursorPosition >= 0 && cursorPosition < text.length( ) ) {
            int charWidth = getCharacterWidth( );

            if ( cursorPosition <= newlinePos ) {
                // Cursor is on the path line
                char cursorChar = ( cursorPosition < pathLine.length( ) ) ? pathLine[ cursorPosition ] : ' ';
                int cursorX = cursorPosition * charWidth;
                int cursorY = 8;
                drawHighlightedChar( cursorX, cursorY, cursorChar );
            } else {
                // Cursor is on the filename line
                int filenameCursorPos = cursorPosition - newlinePos - 1;
                char cursorChar = ( filenameCursorPos < filenameLine.length( ) ) ? filenameLine[ filenameCursorPos ] : ' ';
                int cursorX = filenameCursorPos * charWidth;
                int cursorY = 20;
                drawHighlightedChar( cursorX, cursorY, cursorChar );
            }
        }
    } else {
        // Single line display (root directory case)
        drawText( 0, 8, visibleText );

        // Highlight cursor character
        if ( cursorPosition >= 0 && cursorPosition < strlen( visibleText ) ) {
            char cursorChar = visibleText[ cursorPosition ];
            if ( cursorChar == '\0' ) {
                cursorChar = ' '; // Show space for end of text
            }

            int charWidth = getCharacterWidth( );
            int cursorX = cursorPosition * charWidth;
            int cursorY = 8;

            drawHighlightedChar( cursorX, cursorY, cursorChar );
        }
    }

    // Flush to display
    flushFramebuffer( );

    // Restore previous font
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    getDisplay().setFont( currentFont );
    usingSmallFont = false;
}

void oled::showMultiLineSmallText( const char* text, bool clear, bool show ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;

    // Static line buffer for terminal-like scrolling
    static char lineBuffer[8][32];  // Max 8 lines, 32 chars each
    static int lineCount = 0;
    static int currentLinePos = 0;  // Current position in the current line
    
    if ( clear ) {
        clearFramebuffer( );
        // Clear line buffer
        for (int i = 0; i < 8; i++) {
            memset(lineBuffer[i], 0, 32);
        }
        lineCount = 0;
        currentLinePos = 0;
    }

    // Store current font before changing
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;
    uint8_t savedTextSize = currentTextSize;

    // Set Andale Mono 5pt directly (font index 12) - most reliable approach
    // CRITICAL: Must set font on display BEFORE drawing
    currentFont = fontList[ 12 ].font;  // ANDALEMO5pt7b
    currentFontFamily = fontList[ 12 ].family;
    getDisplay().setFont( currentFont );
    
    // Ensure text size is 1 for small fonts (critical!)
    getDisplay().setTextSize( 1 );
    this->currentTextSize = 1;
    
    // Use 8-pixel line height for compact display (allows 4 lines on 32px display)
    int lineHeight = 8;
    int maxVisibleLines = displayHeight / lineHeight;
    
    // Calculate max chars per line based on display width
    int charWidth = getCharacterWidth();
    const int maxCharsPerLine = charWidth > 0 ? (displayWidth / charWidth) : 21;
    
    // Ensure we have at least one line
    if (lineCount == 0) {
        lineCount = 1;
        lineBuffer[0][0] = '\0';
        currentLinePos = 0;
    }
    
    // Process incoming text character by character
    for (size_t i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        
        // Handle newline - move to next line
        if (c == '\n') {
            // Finalize current line
            lineBuffer[lineCount - 1][currentLinePos] = '\0';
            
            // Start new line
            if (lineCount >= 8) {
                // Scroll buffer up
                for (int j = 0; j < 7; j++) {
                    memcpy(lineBuffer[j], lineBuffer[j + 1], 32);
                }
                lineCount = 7;
            }
            lineCount++;
            currentLinePos = 0;
            lineBuffer[lineCount - 1][0] = '\0';
            continue;
        }
        
        // Add character to current line
        if (currentLinePos < 31) {
            lineBuffer[lineCount - 1][currentLinePos++] = c;
            lineBuffer[lineCount - 1][currentLinePos] = '\0';
        }
        
        // Check if we need to wrap (line is full)
        if (currentLinePos >= maxCharsPerLine) {
            // Find last space for word wrapping
            int wrapPos = currentLinePos;
            int lastSpace = -1;
            
            for (int j = currentLinePos - 1; j >= currentLinePos / 2; j--) {
                if (lineBuffer[lineCount - 1][j] == ' ') {
                    lastSpace = j;
                    break;
                }
            }
            
            // If we found a space, wrap there
            if (lastSpace > 0) {
                // Save characters after the space for next line
                char overflow[32];
                int overflowLen = 0;
                for (int j = lastSpace + 1; j < currentLinePos; j++) {
                    overflow[overflowLen++] = lineBuffer[lineCount - 1][j];
                }
                overflow[overflowLen] = '\0';
                
                // Truncate current line at space
                lineBuffer[lineCount - 1][lastSpace] = '\0';
                
                // Start new line with overflow
                if (lineCount >= 8) {
                    for (int j = 0; j < 7; j++) {
                        memcpy(lineBuffer[j], lineBuffer[j + 1], 32);
                    }
                    lineCount = 7;
                }
                lineCount++;
                strcpy(lineBuffer[lineCount - 1], overflow);
                currentLinePos = overflowLen;
            } else {
                // No space found, hard wrap
                lineBuffer[lineCount - 1][currentLinePos] = '\0';
                
                if (lineCount >= 8) {
                    for (int j = 0; j < 7; j++) {
                        memcpy(lineBuffer[j], lineBuffer[j + 1], 32);
                    }
                    lineCount = 7;
                }
                lineCount++;
                currentLinePos = 0;
                lineBuffer[lineCount - 1][0] = '\0';
            }
        }
    }
    
    // Clear framebuffer and redraw all visible lines
    clearFramebuffer();
    
    // Calculate which lines to display (show last N lines that fit)
    int startLine = 0;
    if ( lineCount > maxVisibleLines ) {
        startLine = lineCount - maxVisibleLines;
    }
    
    // Draw visible lines
    // Start at y=7 to maximize space usage (baseline for first line)
    // With 8px spacing, lines are at: 7, 15, 23, 31 (all fit within 32px)
    for (int i = startLine; i < lineCount && (i - startLine) < maxVisibleLines; i++) {
        int displayLine = i - startLine;
        int y = (displayLine * lineHeight) + 7;
        
        if ( lineBuffer[i][0] != '\0' ) {
            setCursor( 0, y, POS_BASELINE );
            getDisplay().print( lineBuffer[i] );
        }
    }

    if ( show && oledConnected ) {
        flushFramebuffer( );
    }

    // Restore previous font and text size
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    currentTextSize = savedTextSize;
    getDisplay().setFont( currentFont );
    getDisplay().setTextSize( currentTextSize );
    usingSmallFont = false;
}

// Reset the scroll position for showMultiLineSmallText (useful when starting a new display session)
void oled::resetMultiLineSmallText() {
    // Access the static variable from showMultiLineSmallText by calling with clear=true
    // This is handled by the clear parameter, so this function is just for API clarity
    // Users can also just call showMultiLineSmallText("", true, false) to reset
}

// Advanced small text buffer display with editing support
void oled::showSmallTextBuffer( const SmallTextDisplayConfig& config ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;
    
    if ( !config.text )
        return;
    
    // Clear framebuffer if requested
    if ( config.clear_before ) {
        clearFramebuffer( );
    }
    
    // Store current font before changing
    const GFXfont* savedFont = currentFont;
    FontFamily savedFamily = currentFontFamily;
    uint8_t savedTextSize = currentTextSize;
    
    // Use specified small font
    setSmallFont( config.font );
    
    // Ensure text size is 1 for small fonts (critical!)
    getDisplay().setTextSize( 1 );
    this->currentTextSize = 1;
    
    // Calculate display parameters from actual font metrics
    FontMetrics metrics = getFontMetrics();
    int charWidth = getCharacterWidth();
    int lineHeight = metrics.lineHeight; // Use actual font line height
    int maxLines = config.max_lines > 0 ? config.max_lines : (displayHeight / lineHeight);
    
    // Split text into lines
    const char* lineStart = config.text;
    const char* lineEnd;
    int lineIndex = 0;
    int displayLine = 0;
    
    // Process each line
    while ( lineStart && *lineStart && displayLine < maxLines ) {
        // Find end of line (newline or end of string)
        lineEnd = strchr( lineStart, '\n' );
        
        // Calculate line length
        size_t lineLen = lineEnd ? (lineEnd - lineStart) : strlen( lineStart );
        
        // Only display lines starting from start_line
        if ( lineIndex >= config.start_line ) {
            int currentY = (displayLine * lineHeight) + lineHeight; // Baseline position
            
            // Determine if this is the cursor line
            bool isCursorLine = config.enable_cursor && (lineIndex == config.cursor_line);
            
            // Calculate horizontal offset for this line
            int horizontalOffset = isCursorLine ? config.horizontal_offset : 0;
            
            // Draw line with optional horizontal scrolling
            if ( lineLen > 0 ) {
                // Use stack buffer to avoid heap allocation
                char lineBuffer[64];
                int startPos = horizontalOffset;
                
                // Clamp startPos to valid range
                if ( startPos < 0 ) startPos = 0;
                if ( startPos > lineLen ) startPos = lineLen;
                
                int availableLen = lineLen - startPos;
                size_t maxChars = min( (size_t)availableLen, sizeof(lineBuffer) - 1 );
                
                if ( maxChars > 0 ) {
                    memcpy( lineBuffer, lineStart + startPos, maxChars );
                    lineBuffer[maxChars] = '\0';
                    
                    // Highlight cursor line if requested
                    if ( isCursorLine && config.highlight_cursor_line ) {
                        int textWidth = strlen(lineBuffer) * charWidth;
                        fillRect( 0, currentY - lineHeight + 1, min(textWidth, displayWidth), lineHeight - 1, SSD1306_WHITE );
                        setTextColor( SSD1306_BLACK );
                        setCursor( 0, currentY, POS_BASELINE );
                        getDisplay().print( lineBuffer );
                        setTextColor( SSD1306_WHITE );
                    } else {
                        setCursor( 0, currentY, POS_BASELINE );
                        getDisplay().print( lineBuffer );
                    }
                }
            } else if ( isCursorLine && config.enable_cursor ) {
                // Empty line with cursor - show space
                setCursor( 0, currentY, POS_BASELINE );
                getDisplay().print( " " );
            }
            
            // Draw cursor if enabled and on this line
            if ( isCursorLine && config.enable_cursor ) {
                int visibleCursorPos = config.cursor_col - horizontalOffset;
                int maxVisibleChars = displayWidth / charWidth;
                
                if ( visibleCursorPos >= 0 && visibleCursorPos < maxVisibleChars ) {
                    // Get character at cursor position
                    char cursorChar = ' ';
                    int actualCursorPos = config.cursor_col;
                    
                    if ( actualCursorPos < lineLen ) {
                        cursorChar = lineStart[actualCursorPos];
                        if ( cursorChar == '\n' || cursorChar == '\0' ) {
                            cursorChar = ' ';
                        }
                    }
                    
                    int cursorX = visibleCursorPos * charWidth;
                    drawHighlightedChar( cursorX, currentY, cursorChar );
                }
            }
            
            displayLine++;
        }
        
        lineIndex++;
        
        // Advance to next line
        if ( lineEnd ) {
            lineStart = lineEnd + 1; // Skip newline
        } else {
            break; // No more lines
        }
    }
    
    // Draw status text at bottom if provided
    if ( config.status_text && strlen(config.status_text) > 0 ) {
        int statusY = displayHeight - 1; // Bottom of screen (baseline)
        
        // Clear bottom area
        fillRect( 0, statusY - lineHeight, displayWidth, lineHeight, SSD1306_BLACK );
        
        // Draw status text
        setCursor( 0, statusY, POS_BASELINE );
        getDisplay().print( config.status_text );
    }
    
    // Show display if requested
    if ( config.show_after && oledConnected ) {
        flushFramebuffer( );
    }
    
    // Restore previous font and text size
    currentFont = savedFont;
    currentFontFamily = savedFamily;
    currentTextSize = savedTextSize;
    getDisplay().setFont( currentFont );
    getDisplay().setTextSize( currentTextSize );
    usingSmallFont = false;
}

// Connection status
bool oled::isConnected( ) const {
    return oledConnected;
}


// int eevblog = 1;
int startupDisplayed = 0;
// Logo display
void oled::showJogo32h( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    // If a persistent OLED-GUI screen is registered (oledgui show(persist=True)),
    // it takes the logo's place as the idle display. Drawing it here is what
    // makes it reappear when the UI returns to idle (leaving a menu, etc.)
    // instead of constantly fighting other OLED output in the background.
    if ( OledGui::getInstance().showIdle() ) return;

    getDisplay().clearDisplay( );

    // Check if startup_message is set and looks like a file path
    if ( strlen( jumperlessConfig.top_oled.startup_message ) > 0 &&
         looksLikeFilePath( jumperlessConfig.top_oled.startup_message ) && startupDisplayed == 0 ) {
        // Try to load bitmap from file
        if ( loadBitmapFromFile( jumperlessConfig.top_oled.startup_message ) ) {
            // Successfully loaded custom bitmap, display it
            int x = ( displayWidth - customBitmapWidth ) / 2;
            int y = ( displayHeight - customBitmapHeight ) / 2;
            getDisplay().drawBitmap( x, y, customBitmapBuffer, customBitmapWidth, customBitmapHeight, SSD1306_WHITE );
            show();
            startupDisplayed = 2;
            return;
        }
        // If loading failed, fall through to default image
    }

    // Try loading default bubbleJumpThin.bin first
    if ( loadBitmapFromFile( "images/bubbleJumpThin.bin" ) ) {
        int x = ( displayWidth - customBitmapWidth ) / 2;
        int y = ( displayHeight - customBitmapHeight ) / 2;
        getDisplay().drawBitmap( x, y, customBitmapBuffer, customBitmapWidth, customBitmapHeight, SSD1306_WHITE );
        show();
        //getDisplay().display( );
        return;
    }

    // Fallback to embedded jogo bitmap if file not found
    int x = ( displayWidth - jumperlessConfig.top_oled.width ) / 2;
    int y = ( displayHeight - jumperlessConfig.top_oled.height ) / 2;

    getDisplay().drawBitmap( x, y, jogo32h, jumperlessConfig.top_oled.width, jumperlessConfig.top_oled.height, SSD1306_WHITE );
    show();
    //getDisplay().display( );
}

// =====================================================================
// OLED display hold. While `holdUntilMs` is in the future, every code
// path that would push pixels to the panel (show / flushFramebuffer /
// raw getDisplay().display()) is short-circuited. The framebuffer can
// still mutate in place - we just don't flush. When the hold expires,
// oledPeriodic() pushes whatever the framebuffer settled on.
//
// Two entry points:
//
//   oledHold(durationMs):
//     Plain hold. Caller has already painted+flushed the held content
//     (or doesn't care about background-vs-overlay separation). Panel
//     stays frozen on the last flushed frame; intervening writes
//     accumulate in the live framebuffer; final flush at expiry is
//     whatever the framebuffer ended up looking like.
//
//   oledHoldBegin(durationMs):
//     Snapshot-aware hold for the toast pattern. Caller invokes this
//     BEFORE painting the held message. We snapshot the current live
//     framebuffer (the "background") into oledHoldShadow and arm
//     oledHoldArmed. The caller then paints the toast and triggers a
//     priority flush via clearPrintShowSmall. The priority-flush path
//     ships the toast to the panel, then calls
//     oledHoldStashAfterPriorityFlush(), which memcpy's
//     oledHoldShadow back into the live framebuffer. From that point,
//     the live framebuffer is the background again - the panel still
//     shows the toast (already on the wire) but the in-RAM framebuffer
//     is ready to accept probe-mode/clearHighlights writes that
//     accumulate against the background. At expiry, oledPeriodic()
//     flushes the live framebuffer (background + writes, no toast
//     residue) and the panel transitions cleanly.
//
// Calling oledHoldBegin again during an active hold re-snapshots the
// current live framebuffer (which by then is bg + any intervening
// writes), so the latest underlying state is preserved across the
// next toast's paint cycle. The hold timer extends - never shortens.
// =====================================================================
static volatile uint32_t holdUntilMs = 0;
volatile bool oledNeedsFlushAfterHold = false;

// Snapshot buffer for oledHoldBegin. Allocated lazily on first use,
// sized to match Adafruit_SSD1306's internal buffer formula
// (WIDTH * ((HEIGHT + 7) / 8)). Kept around between holds so a hot
// path (rapid undo/redo) doesn't pay malloc/free on every toast.
static uint8_t* oledHoldShadow = nullptr;
static int oledHoldShadowSize = 0;
// Panel shadow. Mirrors the bytes that were last shipped to the
// physical OLED via I2C while a hold is active. Updated at:
//   * oledHoldBegin: panel currently equals the live framebuffer, so
//     we seed the panel shadow with a copy of live.
//   * oledHoldStashAfterPriorityFlush: live has the toast that was
//     just transmitted to the panel, so we copy it into the panel
//     shadow right BEFORE the stash overwrites live with the
//     pre-toast background.
// During a hold, dumpFrameBuffer / dumpFrameBufferQuarterSize read
// from the panel shadow instead of the live framebuffer, so the
// terminal-side dump matches what the user actually sees on the OLED
// rather than the post-stash snapshot+writes accumulator.
// Same size/lifecycle as oledHoldShadow.
static uint8_t* oledPanelShadow = nullptr;
static int oledPanelShadowSize = 0;
// True between oledHoldBegin and the next priority flush. The flush
// path checks this and, if set, copies oledHoldShadow back into the
// live framebuffer and clears the flag. Single-threaded with respect
// to the GFX buffer (oled writes happen on the same core), so no
// atomicity concerns beyond the volatile read.
static volatile bool oledHoldArmed = false;

static int oledHoldComputeBufferSize( int w, int h ) {
    if ( w <= 0 || h <= 0 ) return 0;
    // Match Adafruit_SSD1306's allocation: WIDTH * ((HEIGHT + 7) / 8).
    // For 128x32 this is 512; for 128x64 it's 1024.
    return w * ( ( h + 7 ) / 8 );
}

bool oledHoldActive( ) {
    uint32_t until = holdUntilMs;
    return until != 0 && (int32_t)(until - millis()) > 0;
}

void oled::oledHold( uint32_t durationMs ) {
    uint32_t target = millis() + durationMs;
    if ( target > holdUntilMs ) holdUntilMs = target;  // extend, never shorten
}

void oled::oledHoldBegin( uint32_t durationMs ) {
    // No panel or no display object - no point snapshotting; degrade to
    // a plain hold so callers can use this API unconditionally.
    if ( !oledConnected || _displayPtr == nullptr ) {
        oledHold( durationMs );
        return;
    }
    int needSize = oledHoldComputeBufferSize( displayWidth, displayHeight );
    if ( needSize <= 0 ) {
        oledHold( durationMs );
        return;
    }
    // (Re)allocate shadow on first use or if display geometry changed.
    if ( oledHoldShadow == nullptr || oledHoldShadowSize != needSize ) {
        if ( oledHoldShadow != nullptr ) {
            delete[] oledHoldShadow;
            oledHoldShadow = nullptr;
            oledHoldShadowSize = 0;
        }
        // Arduino-Pico runs with exceptions disabled, so operator new
        // returns nullptr on heap exhaustion rather than throwing -
        // matching how the rest of this file treats new (see the
        // Adafruit_SSD1306 allocation at the top, which also assumes
        // a non-throwing new and checks for nullptr).
        oledHoldShadow = new uint8_t[ (size_t) needSize ];
        if ( oledHoldShadow == nullptr ) {
            // OOM - fall back to plain hold. Toast still works, just
            // without background preservation.
            oledHold( durationMs );
            return;
        }
        oledHoldShadowSize = needSize;
    }
    // Panel shadow: same lifecycle as the hold shadow. OOM here is
    // tolerable - the dump-during-hold path will fall back to the live
    // framebuffer if oledPanelShadow stays nullptr.
    if ( oledPanelShadow == nullptr || oledPanelShadowSize != needSize ) {
        if ( oledPanelShadow != nullptr ) {
            delete[] oledPanelShadow;
            oledPanelShadow = nullptr;
            oledPanelShadowSize = 0;
        }
        oledPanelShadow = new uint8_t[ (size_t) needSize ];
        if ( oledPanelShadow != nullptr ) {
            oledPanelShadowSize = needSize;
        }
    }
    uint8_t* live = _displayPtr->getBuffer();
    if ( live != nullptr ) {
        memcpy( oledHoldShadow, live, (size_t) oledHoldShadowSize );
        // Seed the panel shadow with the current live framebuffer.
        // At this instant panel == live (the caller hasn't painted the
        // toast yet); a dump during the brief armed-but-not-flushed
        // window will show the pre-toast frame, which is what's
        // actually on the panel.
        if ( oledPanelShadow != nullptr && oledPanelShadowSize == oledHoldShadowSize ) {
            memcpy( oledPanelShadow, live, (size_t) oledPanelShadowSize );
        }
        oledHoldArmed = true;
    }
    oledHold( durationMs );
}

static void oledHoldStashAfterPriorityFlush() {
    if ( !oledHoldArmed ) return;
    // Defensive: clear the latch on any abnormal state so a stale arm
    // doesn't poison the next priority flush.
    if ( oledHoldShadow == nullptr || oledHoldShadowSize == 0 ||
         _displayPtr == nullptr ) {
        oledHoldArmed = false;
        return;
    }
    uint8_t* live = _displayPtr->getBuffer();
    if ( live != nullptr ) {
        // Capture the just-shipped toast into the panel shadow BEFORE
        // we overwrite live with the pre-toast snapshot. After this,
        // oledPanelShadow tracks what's actually on the panel and
        // dumpFrameBuffer* during the hold will read it instead of
        // live (which is about to become the snapshot).
        if ( oledPanelShadow != nullptr && oledPanelShadowSize == oledHoldShadowSize ) {
            memcpy( oledPanelShadow, live, (size_t) oledPanelShadowSize );
        }
        memcpy( live, oledHoldShadow, (size_t) oledHoldShadowSize );
    }
    oledHoldArmed = false;
}

bool oled::oledIsHeld( ) const {
    return oledHoldActive();
}

// Pick the buffer that dumpFrameBuffer / dumpFrameBufferQuarterSize
// should read from. During a hold, the live framebuffer has been
// stashed to the pre-toast snapshot and no longer matches the panel,
// so we read from the panel shadow (updated at every priority flush)
// to make the terminal-side dump match what the user actually sees on
// the OLED. When no hold is active or the panel shadow isn't allocated
// (OOM, hold never started), fall back to the live framebuffer - which
// in those states equals the panel.
static uint8_t* oledGetDumpBuffer() {
    if ( oledHoldActive() && oledPanelShadow != nullptr &&
         oledPanelShadowSize > 0 ) {
        return oledPanelShadow;
    }
    if ( _displayPtr != nullptr ) {
        return _displayPtr->getBuffer();
    }
    return nullptr;
}

void oled::oledPeriodic( ) {
    // If the hold just expired, do one flush so the panel catches up to
    // whatever the framebuffer was painted with during the held window.
    // (probeMode does its own hold-end polling in its main loop because
    // OLEDService is a low-priority service that probeMode's tight
    // loop doesn't run; this path handles every other context.)
    //
    // Routed through show() rather than _displayPtr->display() so the
    // post-hold flush picks up the terminal-mirror dump and the
    // checkConnection liveness probe consistently with every other
    // ship-to-panel path. holdUntilMs is cleared above so show()'s
    // own hold gate passes through without short-circuiting.
    if ( holdUntilMs != 0 && (int32_t)(holdUntilMs - millis()) <= 0 ) {
        holdUntilMs = 0;
        if ( oledNeedsFlushAfterHold ) {
            oledNeedsFlushAfterHold = false;
            show();
        }
    }

    // CRITICAL FIX: Don't attempt OLED operations during command processing
    // OLED connect() calls refreshConnections() which can cause recursive refresh
    // issues and potentially stack overflow if commands are being processed rapidly.
    extern volatile bool refreshInProgress;
    extern volatile bool refreshLocalInProgress;
    extern volatile bool core1busy;
    
    if (refreshInProgress || refreshLocalInProgress || core1busy || jumperlessConfig.top_oled.enabled == 0) {
        // Skip OLED maintenance while command processing is active
        return;
    }

    // Frames skipped by show()'s shared-I2C0 gate: flush the accumulated
    // framebuffer once wavegen's core-1 DAC streaming has stopped.
    {
        extern WaveGen wavegen;
        if ( oledNeedsFlushAfterWavegen && !wavegen.isRunning( ) ) {
            oledNeedsFlushAfterWavegen = false;
            show( );
        }
    }

    // Same shared-I2C0 rule as show(): even the 1-byte liveness ping below is
    // this core poking the Wire controller that core 1's wavegen streaming is
    // actively driving - no cross-core lock exists. Defer all connection
    // maintenance until streaming stops.
    {
        extern WaveGen wavegen;
        if ( _currentDisplayWire == 0 && wavegen.isRunning( ) ) {
            return;
        }
    }

    // Adaptive check interval. Faster polling when connected lets us catch a
    // hot-unplug *before* dozens of slow display() writes pile up and stall
    // the UI; the cost is one 1-byte I2C ping every ~750ms, which is far
    // cheaper than a single 1024-byte frame to a missing device.
    //
    // When disconnected: check every 2 seconds (responsive reconnection)
    // When max retries hit: check every 4 seconds (back off, allow retry reset)
    // When connected: check every 750ms (bound disconnect detection latency)
    unsigned long currentCheckInterval;
    if (oledConnected) {
        currentCheckInterval = 750;
    } else if (connectionRetries >= maxConnectionRetries) {
        currentCheckInterval = 4000;
    } else {
        currentCheckInterval = 2000;
    }
    
    if (millis() - lastConnectionCheck < currentCheckInterval) {
        return;  // Not time to check yet
    }
    lastConnectionCheck = millis();

    // Periodically reset connectionRetries to allow reconnection after hardware is plugged back in
    // Reset every 4 seconds when max retries hit - this allows catching a re-plugged OLED
    static unsigned long lastRetryReset = 0;
    const unsigned long RETRY_RESET_INTERVAL_MS = 4000;  // Reset retry counter every 4 seconds
    
    if (connectionRetries >= maxConnectionRetries) {
        if (millis() - lastRetryReset > RETRY_RESET_INTERVAL_MS) {
            #if OLED_DEBUG
            Serial.printf("[OLED] Retry reset: retries %d->0 after %lums backoff\n", connectionRetries, millis() - lastRetryReset);
            #endif
            connectionRetries = 0;
            lastRetryReset = millis();
        } else {
            // Still in backoff period, skip this check
            return;
        }
    }
    
    // Handle crossbar-connected OLED with lock_connection
    if (jumperlessConfig.top_oled.lock_connection == 1 && !oledUsingHardwiredPins) {
        bool hasI2CConnections = 
            globalState.hasConnection(jumperlessConfig.top_oled.sda_row, jumperlessConfig.top_oled.gpio_sda) &&
            globalState.hasConnection(jumperlessConfig.top_oled.scl_row, jumperlessConfig.top_oled.gpio_scl);
        
        #if OLED_DEBUG
        Serial.printf("[OLED] lock_connection=1, hasI2CConnections=%d\n", hasI2CConnections);
        #endif
        
        if (!hasI2CConnections) {
            // Re-establish crossbar connections
            #if OLED_DEBUG
            Serial.println("[OLED] No I2C connections, calling connect()");
            #endif
            connect();
        } else if (!checkConnection(true)) {
            // Connections exist but OLED not responding - try reconnecting
            #if OLED_DEBUG
            Serial.println("[OLED] I2C connections exist but OLED not responding, calling connect()");
            #endif
            connect();
        }
    }
    
    // Check and reconnect OLED - works for both crossbar and hardwired pins
    bool hasI2CPath = oledUsingHardwiredPins || 
        (globalState.hasConnection(jumperlessConfig.top_oled.sda_row, jumperlessConfig.top_oled.gpio_sda) &&
         globalState.hasConnection(jumperlessConfig.top_oled.scl_row, jumperlessConfig.top_oled.gpio_scl));
    
    #if OLED_DEBUG
    Serial.printf("[OLED] Periodic check: hardwired=%d, hasI2CPath=%d, connected=%d, retries=%d/%d\n", 
        oledUsingHardwiredPins, hasI2CPath, oledConnected, connectionRetries, maxConnectionRetries);
    #endif
    
    if (!hasI2CPath) {
        // No I2C path available, nothing to do
        #if OLED_DEBUG
        Serial.println("[OLED] No I2C path available");
        #endif
        return;
    }
    
    // Track previous connection state to detect transitions
    // This catches both hot-plug reconnection AND manual disable/enable via commands
    bool wasConnectedBefore = oledConnected;
    
    // Force fresh I2C check, don't use cached result
    bool connected = checkConnection(true);
    #if OLED_DEBUG
    Serial.printf("[OLED] checkConnection(force=true) = %d (was %d)\n", connected, wasConnectedBefore);
    #endif
    
    if (connected) {
        // OLED responding
        if (connectionRetries > 0) {
            #if OLED_DEBUG
            Serial.printf("[OLED] Reconnected during retry! Resetting retries %d->0\n", connectionRetries);
            #endif
            connectionRetries = 0;
        }
        
        // If we transitioned from disconnected to connected, reinitialize the display
        // This handles both hot-plug and manual disable/enable via commands
        if (!wasConnectedBefore) {
            #if OLED_DEBUG
            Serial.println("[OLED] Transition: disconnected -> connected, reinitializing display...");
            #endif
            
            // Reset the I2C bus before reinit - this clears any stuck state from hot-unplug
            // Wire1 is used for GPIO 6/7 (connection types 0, 1, 3)
            // Wire is used for GPIO 4/5 (connection type 2)
            int wireNum = (jumperlessConfig.top_oled.connection_type == 2) ? 0 : 1;
            if (wireNum == 0) {
                #if OLED_DEBUG
                Serial.println("[OLED] Resetting Wire (I2C0) before reinit...");
                #endif
                Wire.end();
                delay(50);
            } else {
                #if OLED_DEBUG
                Serial.println("[OLED] Resetting Wire1 (I2C1) before reinit...");
                #endif
                Wire1.end();
                delay(50);
            }
            
            // Full reinit to get display in a known state
            int result = init();
            #if OLED_DEBUG
            Serial.printf("[OLED] Reconnect init() returned %d\n", result);
            #endif
            
            if (result != 0 && checkConnection(true)) {
                #if OLED_DEBUG
                Serial.println("[OLED] Reconnection complete! Refreshing display...");
                #endif
                getDisplay().clearDisplay();
                showJogo32h();
                show();
                //getDisplay().display();
                
                // Give the display time to stabilize after refresh
                delay(150);
                
                // Reset the check timer so we don't immediately re-check
                lastConnectionCheck = millis();
            }
        }
        
        lastRetryReset = millis();
        return;
    }
    
    // Not connected - attempt reconnection with retry limiting
    oledConnected = false;
    
    if (connectionRetries < maxConnectionRetries) {
        connectionRetries++;
        #if OLED_DEBUG
        Serial.printf("[OLED] Disconnected, retry count: %d/%d (waiting for OLED to return)\n", connectionRetries, maxConnectionRetries);
        #endif
        
        // Don't spam init() while OLED is unplugged - just wait for it to come back
        // The reconnection logic above will handle reinit when checkConnection() succeeds
    }
    // Max retries reached - just waiting for backoff timer to reset
    // The OLED will be reinitialized when it's detected as connected again
}

// TEST AND DEBUG FUNCTIONS
// ========================

void oled::test( ) {
    const char* testText = "Test";

    // Test with different sizes
    for ( int size = 1; size <= 3; size++ ) {
        clearPrintShow( testText, size, true, true, true, -1, -1 );
        delay( 1000 );
    }

    showJogo32h( );
    delay( 2000 );
}

// Test function to debug menu positioning issues
void oled::testMenuPositioning( ) {
    // Test the exact same calls the menu system uses
    const char* menuItems[] = { "$Rails$", "Apps", "Slots", "Show", "Output" };

    for ( int i = 0; i < 5; i++ ) {
        // Test with the exact same parameters as the menu system
        clearPrintShow( menuItems[ i ], 2, true, true, true, -1, -1 );
        delay( 2000 );

        // Also test with the submenu parameters for comparison
        clearPrintShow( menuItems[ i ], 2, true, true, true, 5, 8 );
        delay( 2000 );
    }

    // Test specifically with "Apps" to see if there's something special about it
    clearPrintShow( "Apps", 2, true, true, true, -1, -1 );
    delay( 2000 );
    clearPrintShow( "Test", 2, true, true, true, -1, -1 );
    delay( 2000 );
}

void oled::testSmallFonts( ) {
    if ( !oledConnected )
        return;

    // Test each small font
    const char* testTexts[] = { "Ubuntu 5pt", "DotGothic 4pt", "Jokerman 4pt" };
    SmallFont fonts[] = { SMALL_FONT_UBUNTU, SMALL_FONT_DOTGOTHIC, SMALL_FONT_JOKERMAN };

    for ( int i = 0; i < 3; i++ ) {
        clear( );

        // Test single line
        useSmallFontAndRestore( fonts[ i ], testTexts[ i ], 0, 0, false, false );

        // Test multiple lines
        String multiText = String( testTexts[ i ] ) + "\nLine 2\nLine 3\nLine 4";
        showMultiLineSmallText( multiText.c_str( ), false );

        show( );
        delay( 3000 );

        restoreNormalFont( ); // Make sure font is restored
    }

    // Test file status display
    clear( );
    showFileStatus( "/test/path", 15, "example_file.py" );
    delay( 3000 );
    restoreNormalFont( );

    // Return to logo
    showJogo32h( );
}

// Debug frame buffer dump function (simplified)

unsigned long lastDumpTime = 0;
unsigned long clearInterval = 2000;

void oled::dumpFrameBufferQuarterSize( int clearFirst, int x_pos, int y_pos, int border ) {
    // if (!oledConnected) {
    //    // Serial.println("OLED not connected");
    //     return;
    // }

    // During a hold, oledGetDumpBuffer returns the panel shadow (what's
    // actually on the OLED) instead of the live framebuffer (which is
    // the pre-toast snapshot post-stash). This keeps the terminal dump
    // synchronized with the panel during undo toasts and similar holds.
    uint8_t* buffer = oledGetDumpBuffer();
    if ( !buffer ) {
        Serial.println( "No framebuffer available" );
        return;
    }

    if ( dumpingToSerial == false ) {
        dumpingToSerial = true;
    } else {
        return;
    }

    // Skip manual positioning if windowing system is active
    // extern struct config jumperlessConfig;
    // bool useWindowing = jumperlessConfig.windowing.enabled &&
    //                     jumperlessConfig.windowing.show_oled;

    // if (!useWindowing) {
    // Legacy positioning for non-windowing mode
    saveCursorPosition( &Jerial );
    Jerial.printf( "\033[%d;%dH", y_pos - 1, x_pos + 1 );
    Jerial.printf( "\033[0K" );
    Jerial.printf( "\033[1B" );
    Jerial.printf( "\033[0K" );
    // }
    // Windowing mode: WindowManager handles all positioning

    if ( border == 1 ) {
        Jerial.println( "╭────────────────────────────────────────────────────────────────╮" );
    } else if ( border == 2 ) {
        Jerial.println( "▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖" );
    } else {
        Jerial.println( "                                                                  " );
    }

    // Quarter block characters for different pixel combinations
    const char* quarterBlocks[] = {
        " ", // 0000 - No pixels
        "▘", // 0001 - Upper left only
        "▝", // 0010 - Upper right only
        "▀", // 0011 - Upper half
        "▖", // 0100 - Lower left only
        "▌", // 0101 - Left half
        "▞", // 0110 - Upper right + lower left
        "▛", // 0111 - Upper half + lower left
        "▗", // 1000 - Lower right only
        "▚", // 1001 - Upper left + lower right
        "▐", // 1010 - Right half
        "▜", // 1011 - Upper half + lower right
        "▄", // 1100 - Lower half
        "▙", // 1101 - Upper left + lower half
        "▟", // 1110 - Upper right + lower half
        "█"  // 1111 - Full block
    };

    // Process framebuffer in 2x2 blocks to create 64x16 output
    for ( int blockRow = 0; blockRow < displayHeight / 2; blockRow++ ) {
        Jerial.printf( "\033[%dC", x_pos );
        Jerial.printf( "\033[0K" );
        if ( border == 1 ) {
            Jerial.print( "│" ); // Left border
        } else if ( border == 2 ) {
            Jerial.print( "▐" ); // Left border
        } else {
            Jerial.print( " " ); // Left border
        }

        for ( int blockCol = 0; blockCol < displayWidth / 2; blockCol++ ) {
            uint8_t pixelMask = 0;

            // Check each pixel in the 2x2 block
            for ( int dy = 0; dy < 2; dy++ ) {
                for ( int dx = 0; dx < 2; dx++ ) {
                    int row = blockRow * 2 + dy;
                    int col = blockCol * 2 + dx;

                    // Calculate buffer position for this pixel
                    int page = row / 8;
                    int bit = row % 8;
                    int bufferIndex = page * displayWidth + col;

                    // Extract the pixel value
                    uint8_t pixelByte = buffer[ bufferIndex ];
                    bool pixelOn = ( pixelByte >> bit ) & 0x01;

                    // Set bit in pixelMask
                    if ( pixelOn ) {
                        pixelMask |= ( 1 << ( dy * 2 + dx ) );
                    }
                }
            }

            // Print the appropriate quarter block character
            Jerial.print( quarterBlocks[ pixelMask ] );
        }

        if ( border == 1 ) {
            Jerial.println( "│" ); // Right border and newline
        } else if ( border == 2 ) {
            Jerial.println( "▌" ); // Right border and newline
        } else {
            Jerial.println( " " ); // Right border and newline
        }
    }
    Jerial.printf( "\033[%dC", x_pos );
    Jerial.printf( "\033[0K" );
    if ( border == 1 ) {
        Jerial.println( "╰────────────────────────────────────────────────────────────────╯" );
    } else if ( border == 2 ) {
        Jerial.println( "▝▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▘" );
    } else {
        Jerial.println( "                                                                  " );
    }
    // if (!useWindowing) {
    // Legacy cursor restoration for non-windowing mode
    Jerial.printf( "\033[%dB", y_pos - ( displayHeight / 2 ) + 2 );
    Jerial.printf( "\033[50B" );
    // }
    // Windowing mode: WindowManager handles all cursor management
    dumpingToSerial = false;
}

void oled::dumpFrameBuffer( Stream* stream ) {
    if ( stream == nullptr ) {
        stream = &Jerial;
    }

    // See note on oledGetDumpBuffer in dumpFrameBufferQuarterSize:
    // dumps the panel shadow when a hold is active so the terminal
    // matches the OLED rather than the post-stash snapshot.
    uint8_t* buffer = oledGetDumpBuffer();
    if ( !buffer ) {
        stream->println( "No framebuffer available" );
        return;
    }

    stream->printf( "OLED Framebuffer Dump (%dx%d):\n\r", displayWidth, displayHeight );
    stream->println( "┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐" );

    for ( int row = 0; row < displayHeight; row++ ) {
        stream->print( "│" ); // Left border

        for ( int col = 0; col < displayWidth; col++ ) {
            int page = row / 8;
            int bit = row % 8;
            int bufferIndex = page * displayWidth + col;

            uint8_t pixelByte = buffer[ bufferIndex ];
            bool pixelOn = ( pixelByte >> bit ) & 0x01;

            if ( pixelOn ) {
                stream->print( "█" ); // Full block for lit pixel
            } else {
                stream->print( " " ); // Space for dark pixel
            }
        }

        stream->println( "│" ); // Right border and newline
    }

    stream->println( "└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘" );
}

// Machine-readable framebuffer dump. quarter/full = ASCII art; b64/raw emit
// the raw SSD1306 page buffer (width * ceil(height/8) bytes) so a host can
// reconstruct pixels directly. Each frame is wrapped in a tagged envelope.
void oled::dumpFrameBufferEncoded( Stream* stream, uint8_t enc ) {
    if ( stream == nullptr ) stream = &Jerial;

    if ( enc == OLED_ENC_FULL ) {
        dumpFrameBuffer( stream );
        return;
    }

    uint8_t* buffer = oledGetDumpBuffer();
    if ( !buffer ) {
        stream->print( "oled{none}\r\n" );
        return;
    }

    const int w = displayWidth;
    const int h = displayHeight;
    const size_t nbytes = (size_t)w * ( ( h + 7 ) / 8 );

    if ( enc == OLED_ENC_QUARTER ) {
        // Quarter-block art, no ANSI positioning (machine/log friendly).
        static const char* qb[ 16 ] = {
            " ", "▘", "▝", "▀", "▖", "▌", "▞", "▛",
            "▗", "▚", "▐", "▜", "▄", "▙", "▟", "█" };
        stream->printf( "oled_q{%dx%d:\r\n", w, h );
        for ( int blockRow = 0; blockRow < h / 2; blockRow++ ) {
            for ( int blockCol = 0; blockCol < w / 2; blockCol++ ) {
                uint8_t mask = 0;
                for ( int dy = 0; dy < 2; dy++ ) {
                    for ( int dx = 0; dx < 2; dx++ ) {
                        int row = blockRow * 2 + dy;
                        int col = blockCol * 2 + dx;
                        int idx = ( row / 8 ) * w + col;
                        if ( ( buffer[ idx ] >> ( row % 8 ) ) & 0x01 )
                            mask |= ( 1 << ( dy * 2 + dx ) );
                    }
                }
                stream->print( qb[ mask ] );
            }
            stream->print( "\r\n" );
        }
        stream->print( "}\r\n" );
        return;
    }

    if ( enc == OLED_ENC_RAW ) {
        stream->printf( "oled_raw{%dx%d:%u:", w, h, (unsigned)nbytes );
        stream->write( buffer, nbytes );
        stream->print( "}\r\n" );
        return;
    }

    // OLED_ENC_B64 (default): base64 of the page buffer.
    static const char b64tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    stream->printf( "oled_b64{%dx%d:", w, h );
    size_t i = 0;
    char out[ 4 ];
    while ( i + 3 <= nbytes ) {
        uint32_t v = ( (uint32_t)buffer[ i ] << 16 ) |
                     ( (uint32_t)buffer[ i + 1 ] << 8 ) | buffer[ i + 2 ];
        out[ 0 ] = b64tab[ ( v >> 18 ) & 63 ];
        out[ 1 ] = b64tab[ ( v >> 12 ) & 63 ];
        out[ 2 ] = b64tab[ ( v >> 6 ) & 63 ];
        out[ 3 ] = b64tab[ v & 63 ];
        stream->write( (const uint8_t*)out, 4 );
        i += 3;
    }
    size_t rem = nbytes - i;
    if ( rem == 1 ) {
        uint32_t v = (uint32_t)buffer[ i ] << 16;
        out[ 0 ] = b64tab[ ( v >> 18 ) & 63 ];
        out[ 1 ] = b64tab[ ( v >> 12 ) & 63 ];
        out[ 2 ] = '=';
        out[ 3 ] = '=';
        stream->write( (const uint8_t*)out, 4 );
    } else if ( rem == 2 ) {
        uint32_t v = ( (uint32_t)buffer[ i ] << 16 ) | ( (uint32_t)buffer[ i + 1 ] << 8 );
        out[ 0 ] = b64tab[ ( v >> 18 ) & 63 ];
        out[ 1 ] = b64tab[ ( v >> 12 ) & 63 ];
        out[ 2 ] = b64tab[ ( v >> 6 ) & 63 ];
        out[ 3 ] = '=';
        stream->write( (const uint8_t*)out, 4 );
    }
    stream->print( "}\r\n" );
}

// Push one frame to the USBSer3 backchannel if streaming is enabled. Cheap
// FNV-1a change detection skips identical frames; a full TX FIFO (host not
// draining) drops the frame rather than stalling the OLED/UI render path.
void oled::streamFrameToSer3() {
    if ( !oledSer3Stream || oledSer3Target == nullptr ) return;
    uint8_t* sbuf = oledGetDumpBuffer();
    if ( !sbuf ) return;
    size_t n = (size_t)displayWidth * ( ( displayHeight + 7 ) / 8 );
    uint32_t hsh = 2166136261u;
    for ( size_t i = 0; i < n; i++ ) {
        hsh ^= sbuf[ i ];
        hsh *= 16777619u;
    }
    if ( hsh == oledSer3StreamHash ) return;        // unchanged frame, skip
    if ( USBSer3.availableForWrite() <= 0 ) return;  // host not draining, drop
    dumpFrameBufferEncoded( oledSer3Target, oledSer3Enc );
    oledSer3StreamHash = hsh;
}

// SMALL FONT FUNCTIONS
// ===================

void oled::setSmallFont( SmallFont smallFont ) {
    if ( !oledConnected && stillWriteToFramebuffer == false )
        return;

    // Store current font and family for restoration
    if ( !usingSmallFont ) {
        previousFont = currentFont;
        previousFontFamily = currentFontFamily;
    }

    currentSmallFont = smallFont;
    usingSmallFont = true;

    // Map small font enum to font index
    int fontIndex;
    switch ( smallFont ) {
    case SMALL_FONT_UBUNTU:
        fontIndex = 20; // ubuntu5pt7b
        break;
    case SMALL_FONT_DOTGOTHIC:
        fontIndex = 21; // DotGothic16_Regular4pt7b
        break;
    case SMALL_FONT_JOKERMAN:
        fontIndex = 2; // Jokerman8pt7b
        break;
    case SMALL_FONT_ANDALE_MONO:
        fontIndex = 12; // ANDALEMO5pt7b - monospaced for text highlighting
        break;
    case SMALL_FONT_IOSEVKA_REGULAR:
        fontIndex = 18; // IosevkaSS08_Regular9pt
        break;
    case SMALL_FONT_IOSEVKA_5PT:
        fontIndex = 22; // IosevkaSS08_Light5pt
        break;
    case SMALL_FONT_PRAGMATISM_5PT:
        fontIndex = 23; // Pragmatism5pt
        break;
    case SMALL_FONT_FREEMONO_5PT:
        fontIndex = 24; // FreeMono5pt
        break;
    case SMALL_FONT_ENVYCODE_5PT:
        fontIndex = 25; // EnvyCodeRNerdFont_Regular5pt
        break;
    default:
        fontIndex = 12; // Default to Andale Mono
        break;
    }

    setFont( fontIndex );
    setTextSize( 1 ); // Always use size 1 with small fonts
}

void oled::useSmallFont( SmallFont smallFont, const char* text, int16_t x, int16_t y, bool clear ) {
    if ( ( !oledConnected || !text ) && stillWriteToFramebuffer == false )
        return;

    if ( clear ) {
        getDisplay().clearDisplay( );
    }

    setSmallFont( smallFont );
    setCursor( x, y );
    getDisplay().print( text );
}

void oled::useSmallFontAndRestore( SmallFont smallFont, const char* text, int16_t x, int16_t y, bool clear, bool showw ) {
    if ( ( !oledConnected || !text ) && stillWriteToFramebuffer == false )
        return;

    if ( clear ) {
        getDisplay().clearDisplay( );
    }

    setSmallFont( smallFont );

    // Auto-adjust Y position for small fonts (add baseline offset if y is too small)
    int16_t adjustedY = y;
    if ( y < 8 ) {
        adjustedY = y + 8; // Add baseline offset for 4-5pt fonts
    }

    setCursor( x, adjustedY );
    getDisplay().print( text );

    if ( showw ) {
        show();
        //getDisplay().display( );
    }

    restoreNormalFont( );
}

void oled::restoreNormalFont( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    if ( usingSmallFont && previousFont ) {
        currentFont = previousFont;
        currentFontFamily = previousFontFamily;
        getDisplay().setFont( currentFont );
        usingSmallFont = false;
        previousFont = nullptr;
    }
}

// Drawing primitives
void oled::drawLine( int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().drawLine( x0, y0, x1, y1, color );
}

void oled::fillRect( int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().fillRect( x, y, w, h, color );
}

// Simple framebuffer management
void oled::clearFramebuffer( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().clearDisplay( );
}

void oled::setPixel( int16_t x, int16_t y, uint16_t color ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().drawPixel( x, y, color );
}

void oled::drawChar( int16_t x, int16_t y, char c ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    int16_t savedX = getDisplay().getCursorX( );
    int16_t savedY = getDisplay().getCursorY( );
    getDisplay().setCursor( x, y );
    getDisplay().print( c );
    getDisplay().setCursor( savedX, savedY );
}

void oled::drawText( int16_t x, int16_t y, const char* text ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    int16_t savedX = getDisplay().getCursorX( );
    int16_t savedY = getDisplay().getCursorY( );
    getDisplay().setCursor( x, y );
    getDisplay().print( text );
    getDisplay().setCursor( savedX, savedY );
}

void oled::drawHighlightedChar( int16_t x, int16_t y, char c ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;

    // Get character bounds for background rectangle
    int16_t x1, y1;
    uint16_t w, h;
    char charStr[ 2 ] = { c, '\0' };
    getDisplay().getTextBounds( charStr, x, y, &x1, &y1, &w, &h );

    // Draw larger white background for border effect (1 pixel larger on all sides)
    getDisplay().fillRect( x1 - 1, y1 - 1, w + 2, h + 2, SSD1306_WHITE );

    // Draw character with black text on white background (inverted)
    int16_t savedX = getDisplay().getCursorX( );
    int16_t savedY = getDisplay().getCursorY( );
    getDisplay().setCursor( x, y );
    getDisplay().setTextColor( SSD1306_BLACK, SSD1306_WHITE ); // Black text on white background
    getDisplay().print( c );

    // Restore default colors and cursor
    getDisplay().setTextColor( SSD1306_WHITE, SSD1306_BLACK ); // Default colors
    getDisplay().setCursor( savedX, savedY );
}

void oled::flushFramebuffer( ) {
    // Thin delegate to show(). show() already handles the connection
    // check, the hold-gate / oledNeedsFlushAfterHold latch, the
    // checkConnection liveness probe, and (when configured) the
    // terminal-mirror dump - all of which this function used to
    // either duplicate or silently skip. Kept as a public API for the
    // many external callers (Apps.cpp, Jerial.cpp, MeasureMode.cpp,
    // EkiloEditor.cpp, BitmapEditor.cpp, ...) that already use this
    // name; they pick up the missing dump + liveness check for free.
    show();
}

uint8_t* oled::getFramebuffer( ) {
    return getDisplay().getBuffer( );
}

// ============================================================================
// DUAL FRAMEBUFFER MODE MANAGEMENT
// ============================================================================

void oled::setMode( OLEDMode mode ) {
    if ( !dualFramebufferEnabled ) {
        // Dual framebuffer not enabled, just track mode
        currentMode = mode;
        return;
    }

    if ( mode == currentMode ) {
        return; // Already in this mode
    }

    // Save current framebuffer contents before switching
    uint8_t* currentBuffer = getDisplay().getBuffer( );
    if ( currentBuffer ) {
        if ( currentMode == MODE_LARGE_TEXT && largeTextFramebuffer ) {
            memcpy( largeTextFramebuffer, currentBuffer, framebufferSize );
        } else if ( currentMode == MODE_SMALL_TEXT && smallTextFramebuffer ) {
            memcpy( smallTextFramebuffer, currentBuffer, framebufferSize );
        }
    }

    // Switch to new mode
    currentMode = mode;

    // Load new framebuffer contents
    if ( currentBuffer ) {
        if ( currentMode == MODE_LARGE_TEXT && largeTextFramebuffer ) {
            memcpy( currentBuffer, largeTextFramebuffer, framebufferSize );
        } else if ( currentMode == MODE_SMALL_TEXT && smallTextFramebuffer ) {
            memcpy( currentBuffer, smallTextFramebuffer, framebufferSize );
        }
    }
}

void oled::autoDetectMode( int textSize ) {
    if ( !dualFramebufferEnabled ) {
        return; // Dual framebuffer not enabled
    }

    // Automatic mode detection based on text size
    if ( textSize >= 2 ) {
        setMode( MODE_LARGE_TEXT );
    } else {
        setMode( MODE_SMALL_TEXT );
    }
}

// ============================================================================
// TERMINAL EMULATION - ANSI/CONTROL SEQUENCE HANDLING
// ============================================================================

void oled::parseControlSequence( char c ) {
    if ( !inEscapeSequence ) {
        if ( c == '\x1b' ) { // ESC character
            inEscapeSequence = true;
            escapeBufferPos = 0;
            memset( escapeBuffer, 0, sizeof( escapeBuffer ) );
            return;
        }

        // Handle regular control characters
        switch ( c ) {
        case '\r': // Carriage return
            termCursorX = 0;
            break;
        case '\n': // Line feed
            termCursorY++;
            if ( termCursorY >= displayHeight / 8 ) {
                termCursorY = displayHeight / 8 - 1;
            }
            break;
        case '\t': // Tab
            termCursorX = ( ( termCursorX / 8 ) + 1 ) * 8;
            break;
        case '\b': // Backspace
            if ( termCursorX > 0 )
                termCursorX--;
            break;
        default:
            // Printable character - handle normally
            break;
        }
        return;
    }

    // Build escape sequence
    if ( escapeBufferPos < sizeof( escapeBuffer ) - 1 ) {
        escapeBuffer[ escapeBufferPos++ ] = c;
        escapeBuffer[ escapeBufferPos ] = '\0';
    }

    // Check if sequence is complete
    if ( c >= 'A' && c <= 'Z' ) { // Final character
        executeEscapeSequence( );
        inEscapeSequence = false;
        escapeBufferPos = 0;
    } else if ( c >= 'a' && c <= 'z' ) { // Some sequences end with lowercase
        executeEscapeSequence( );
        inEscapeSequence = false;
        escapeBufferPos = 0;
    }
}

void oled::executeEscapeSequence( ) {
    if ( escapeBufferPos == 0 )
        return;

    // Parse CSI sequences (starting with '[')
    if ( escapeBuffer[ 0 ] == '[' ) {
        char finalChar = escapeBuffer[ escapeBufferPos - 1 ];

        switch ( finalChar ) {
        case 'H':
        case 'f': { // Cursor position
            // Format: ESC[row;colH or ESC[H (home)
            int row = 0, col = 0;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d;%d", &row, &col );
            }
            moveCursorTerm( col, row );
            break;
        }

        case 'A': { // Cursor up
            int n = 1;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d", &n );
            }
            termCursorY -= n;
            if ( termCursorY < 0 )
                termCursorY = 0;
            break;
        }

        case 'B': { // Cursor down
            int n = 1;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d", &n );
            }
            termCursorY += n;
            if ( termCursorY >= displayHeight / 8 )
                termCursorY = displayHeight / 8 - 1;
            break;
        }

        case 'C': { // Cursor forward
            int n = 1;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d", &n );
            }
            termCursorX += n;
            if ( termCursorX >= displayWidth / 6 )
                termCursorX = displayWidth / 6 - 1;
            break;
        }

        case 'D': { // Cursor back
            int n = 1;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d", &n );
            }
            termCursorX -= n;
            if ( termCursorX < 0 )
                termCursorX = 0;
            break;
        }

        case 'J': { // Erase in display
            int n = 0;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d", &n );
            }
            if ( n == 0 )
                clearToEndOfScreen( );
            else if ( n == 2 )
                clearScreen( );
            break;
        }

        case 'K': { // Erase in line
            int n = 0;
            if ( escapeBufferPos > 1 ) {
                sscanf( &escapeBuffer[ 1 ], "%d", &n );
            }
            if ( n == 0 )
                clearToEndOfLine( );
            else if ( n == 2 )
                clearLine( );
            break;
        }

        case 'm': // SGR (color/style) - ignore for monochrome display
            break;

        default:
            // Unknown sequence - ignore
            break;
        }
    }
}

void oled::moveCursorTerm( int x, int y ) {
    termCursorX = x;
    termCursorY = y;

    // Clamp to display bounds
    if ( termCursorX < 0 )
        termCursorX = 0;
    if ( termCursorY < 0 )
        termCursorY = 0;
    if ( termCursorX >= displayWidth / 6 )
        termCursorX = displayWidth / 6 - 1;
    if ( termCursorY >= displayHeight / 8 )
        termCursorY = displayHeight / 8 - 1;
}

void oled::clearToEndOfLine( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    int x = termCursorX * 6;
    int y = termCursorY * 8;
    getDisplay().fillRect( x, y, displayWidth - x, 8, SSD1306_BLACK );
}

void oled::clearLine( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    int y = termCursorY * 8;
    getDisplay().fillRect( 0, y, displayWidth, 8, SSD1306_BLACK );
}

void oled::clearToEndOfScreen( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    clearToEndOfLine( );

    // Clear all lines below current
    for ( int line = termCursorY + 1; line < displayHeight / 8; line++ ) {
        getDisplay().fillRect( 0, line * 8, displayWidth, 8, SSD1306_BLACK );
    }
}

void oled::clearScreen( ) {
    if ( ( !oledConnected ) && stillWriteToFramebuffer == false )
        return;
    getDisplay().clearDisplay( );
    termCursorX = 0;
    termCursorY = 0;
}

// CONNECTION MANAGEMENT
// ====================

int oled::connect( void ) {
    // if (jumperlessConfig.top_oled.enabled == 0) {
    //     return 0;
    // }
    int found = -1;

    #if OLED_DEBUG
    Serial.printf("[OLED] connect(): hardwired=%d, sda_pin=%d, scl_pin=%d, sda_row=%d, scl_row=%d\n",
        oledUsingHardwiredPins, jumperlessConfig.top_oled.sda_pin, jumperlessConfig.top_oled.scl_pin,
        jumperlessConfig.top_oled.sda_row, jumperlessConfig.top_oled.scl_row);
    #endif

    // If using hardwired RP6/RP7 (GPIO 6/7), skip crossbar bridge management
    if ( !oledUsingHardwiredPins ) {
        // Reserve pins on net map so UI shows them as I2C (not generic GPIO)
        // gpioNet[jumperlessConfig.top_oled.sda_pin - 20] = -2;
        // gpioNet[jumperlessConfig.top_oled.scl_pin - 20] = -2;
        // removeBridgeFromNodeFile(jumperlessConfig.top_oled.gpio_sda, -1, netSlot, 0);
        // removeBridgeFromNodeFile(jumperlessConfig.top_oled.gpio_scl, -1, netSlot, 0);

        #if OLED_DEBUG
        Serial.printf("[OLED] Setting up crossbar: gpio_sda=%d->row%d, gpio_scl=%d->row%d\n",
            jumperlessConfig.top_oled.gpio_sda, jumperlessConfig.top_oled.sda_row,
            jumperlessConfig.top_oled.gpio_scl, jumperlessConfig.top_oled.scl_row);
        #endif

        // Use RAM-based state system for crossbar connections
        addBridgeToState( jumperlessConfig.top_oled.gpio_sda, jumperlessConfig.top_oled.sda_row, 0 );
        addBridgeToState( jumperlessConfig.top_oled.gpio_scl, jumperlessConfig.top_oled.scl_row, 0 );

        // Extra refresh to ensure OLED connections are applied
        #if OLED_DEBUG
        Serial.println("[OLED] Calling refreshConnections()...");
        #endif
        refreshConnections( 1, 0, 0 );
        waitCore2( );
        #if OLED_DEBUG
        Serial.println("[OLED] refreshConnections() done");
        #endif
    }

    #if OLED_DEBUG
    Serial.printf("[OLED] Calling initI2C(sda=%d, scl=%d, 400000)...\n", 
        jumperlessConfig.top_oled.sda_pin, jumperlessConfig.top_oled.scl_pin);
    #endif
    found = initI2C( jumperlessConfig.top_oled.sda_pin, jumperlessConfig.top_oled.scl_pin, 400000 );
    #if OLED_DEBUG
    Serial.printf("[OLED] initI2C returned %d\n", found);
    #endif

    // Mark function map so scan/UI reflect I2C role
    if ( jumperlessConfig.top_oled.sda_pin >= 20 ) {
        gpio_function_map[ jumperlessConfig.top_oled.sda_pin - 20 ] = GPIO_FUNC_I2C;
        gpioState[ jumperlessConfig.top_oled.sda_pin - 20 ] = 6;
    }
    if ( jumperlessConfig.top_oled.scl_pin >= 20 ) {
        gpio_function_map[ jumperlessConfig.top_oled.scl_pin - 20 ] = GPIO_FUNC_I2C;
        gpioState[ jumperlessConfig.top_oled.scl_pin - 20 ] = 6;
    }

    if ( found == -1 ) {
        #if OLED_DEBUG
        Serial.println("[OLED] connect() failed: initI2C returned -1");
        #endif
        oledConnected = false;
        return 0;
    } else {
        #if OLED_DEBUG
        Serial.println("[OLED] connect() success");
        #endif
        oledConnected = true;
        return found;
    }
}

// =====================================================================
// OLED connection-type helpers (declared in oled.h)
// =====================================================================

const char* getOledConnectionTypeShortName(int connectionType) {
    switch (connectionType) {
        case 0: return "GPIO 7/8";
        case 1: return "RP6/RP7";
        case 2: return "I2C0";
        case 3: return "Custom";
        default: return "Unknown";
    }
}

// Quickly probe Wire (I2C0, GPIO 4/5) for an OLED at `address`.
//
// We do a one-byte addressed ping (beginTransmission/endTransmission) which
// the SSD1306 ACKs without needing any register write. The bus is shared
// with the MCP4728 DAC (0x60) and INA219 current sensors (0x40, 0x41); a
// plain OLED probe at 0x3C won't collide with any of those.
//
// Why two attempts: the very first transaction on Wire after the DAC/INA
// initialization sequence has been observed to NACK spuriously on some
// boards (bus settling). A single follow-up after a brief delay is enough
// to disambiguate a real "no device" from a transient hiccup.
//
// Why a tight setTimeout: the Earle pico Wire default lets a hung bus stall
// for hundreds of milliseconds, which directly translates into UI lag if
// the OLED disappears between checkConnection() polls. We set 5ms during
// the probe to fail fast, then leave the timeout at 15ms (matching the
// existing Wire1 setting in oled::init()) so any subsequent I2C0 OLED
// traffic that hits a dead bus also bails out within a single frame.
bool probeOledOnInternalI2C0(uint8_t address) {
    // Snap to 7-bit and reject obviously-invalid addresses.
    address &= 0x7F;
    if (address == 0 || address > 0x77) {
        return false;
    }

    // Caller must have initialized Wire (initDAC()/initINA219() do so during
    // boot). We don't call Wire.begin() here on purpose -- we don't want to
    // step on the DAC/INA bus configuration that's already up and running.
    // reset_with_timeout=true so a wedged bus gets clocked free here too.
    Wire.setTimeout(5, true);

    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Wire.beginTransmission(address);
        int err = Wire.endTransmission();
        if (err == 0) {
            found = true;
            break;
        }
        delayMicroseconds(250);
    }

    // Leave Wire with a bounded timeout so a later disconnect of the OLED
    // can't wedge a 1024-byte display() write for an unbounded amount of
    // time. 15ms matches the Wire1 timeout the OLED code sets elsewhere
    // and is plenty for any single DAC/INA transaction on the same bus.
    Wire.setTimeout(15, true);

    return found;
}

// Boot-time auto-detect + config migration for an OLED on the internal I2C0
// bus. See oled.h for the contract; see the commented policy below for the
// full reasoning.
//
// Policy when the probe ACKs:
//   * Always force `enabled = 1` and `connect_on_boot = 1`. The probe ACK
//     is proof the user has actually wired up an OLED; respecting stale
//     `enabled = 0` / `connect_on_boot = 0` flags from a previous hardware
//     setup would mean firstLoop==2 silently skips oled.init() and the
//     auto-detect is useless.
//   * If stored revision < 7, additionally promote to rev 7 and switch
//     connection_type to 2 (internal I2C0). This is the "first time
//     seeing a rev 7 board" migration path -- the user never explicitly
//     chose pins, so auto-routing the OLED to the internal bus is the
//     right default.
//   * If stored revision >= 7, leave revision and connection_type alone.
//     A rev 7 user who has manually routed their OLED via GPIO 7/8 (with
//     a separate phantom on the internal bus) should not have that
//     decision silently overridden every reboot.
//
// Policy when the probe NACKs:
//   * Never touch any field. A user who has deliberately unplugged their
//     OLED, or whose hardware doesn't have one, keeps their config
//     intact.
//
// The probe itself uses a 5ms transaction timeout, so a missing OLED costs
// ~10ms of boot time at worst. We also leave Wire with a 15ms timeout
// after the probe to bound any future "OLED disappeared" stalls on this
// bus (see probeOledOnInternalI2C0()).
bool autoDetectAndConfigureOled(void) {
    uint8_t oledAddr = (uint8_t)( jumperlessConfig.top_oled.i2c_address & 0x7F );
    if ( oledAddr == 0 ) {
        oledAddr = 0x3C; // SSD1306 default
    }

    if ( !probeOledOnInternalI2C0( oledAddr ) ) {
        return false;
    }

    // Hardware-revision migration: a working OLED on the internal I2C0 bus
    // is conclusive evidence this is rev 7+ silicon. Bump the stored
    // revision (which is EEPROM-backed by saveConfig()) and switch the
    // OLED to connection_type = 2 so it uses the dedicated GPIO 4/5 path
    // instead of tying up crossbar nodes.
    bool migrateRev = ( jumperlessConfig.hardware.revision < 7 );
    if ( migrateRev ) {
        Serial.printf( "[OLED] Detected OLED at 0x%02X on internal I2C0 "
                       "- promoting hardware revision %d -> 7 and "
                       "switching OLED to internal bus\n",
            oledAddr, jumperlessConfig.hardware.revision );
        jumperlessConfig.hardware.revision = 7;
        // Keep the legacy global mirror in sync; loadHardwareFromEEPROM()
        // and other call sites both read this value.
        extern int revisionNumber;
        revisionNumber = 7;
        // updateOledPinsForConnectionType() updates pins/rows/connection_type
        // and flips oledUsingHardwiredPins, so no crossbar bridges will be
        // created when oled.init() runs later in firstLoop==2.
        updateOledPinsForConnectionType( 2 );
    }

    // Force the enable + connect-on-boot flags any time we detect a real
    // device, even on already-rev-7 boards. See the policy block at the
    // top of this function for the asymmetry rationale ("probe ACK is
    // proof of presence, so the user clearly wants this device to come
    // up at boot").
    bool flagsChanged = false;
    if ( jumperlessConfig.top_oled.enabled != 1 ) {
        jumperlessConfig.top_oled.enabled = 1;
        flagsChanged = true;
    }
    if ( jumperlessConfig.top_oled.connect_on_boot != 1 ) {
        jumperlessConfig.top_oled.connect_on_boot = 1;
        flagsChanged = true;
    }

    // Single save covers all dirty fields. Skipped entirely if nothing
    // actually changed, so a board that's already correctly configured
    // doesn't pay a flash write on every reboot.
    if ( migrateRev || flagsChanged ) {
        saveConfig( );
    }

    return true;
}

// V5 hardware (revision 5) routes the OLED through the crossbar on GPIO 7/8.
// V5 rev 7+ exposes a dedicated internal I2C0 bus on GPIO 4/5, which is
// preferred because it doesn't tie up two crossbar nodes. Rev 6 is treated as
// "between" but defaults to the rev 5 behavior, since the I2C0 bus wasn't
// broken out until rev 7.
int defaultOledConnectionTypeForRevision(int revision) {
    if (revision >= 7) {
        return 2; // internal I2C0 (hardwired GPIO 4/5)
    }
    return 0; // GPIO 7/8 via crossbar
}

// Tear down the OLD I2C peripheral if (and ONLY if) it was Wire1.
//
// Wire1 is OLED-exclusive on V5, so ending it is safe and is required to
// avoid the same-bus pin-swap hang -- Earle Philhower's setSDA/setSCL/begin
// path PANICs when called on a running Wire1 with new pins, which on real
// hardware shows up as a silent USB disconnect on the second cycle press.
//
// Wire (I2C0) is *shared* with the MCP4728 DAC (0x60) and both INA219 current
// sensors (0x40, 0x41) on hardwired GPIO 4/5. Calling Wire.end() kills those
// peripherals and leaves the system retrying dead I2C transactions, which
// surfaces as "sluggish" OLED writes elsewhere because the system is bogged
// down on failed I2C0 traffic. So we leave Wire alone -- the OLED is just
// one of multiple devices on it, and initI2C() reconfigures pins/clock as
// needed for the new connection_type.
static void teardownOldOledBus(int oldConnectionType) {
    bool wasOnWire1 = (oldConnectionType != 2);
    if (wasOnWire1) {
        Wire1.end();
        delay(50);
    }
}

bool applyOledConnectionType(int newConnectionType, bool reinitDisplay, bool persist) {
    // Clamp out-of-range values to GPIO 7/8 so we never persist garbage to
    // flash that would re-create a bad state on the next boot.
    if (newConnectionType < 0 || newConnectionType > 3) {
        newConnectionType = 0;
    }

    // Capture the OLD bus assignment before we touch the config fields.
    int oldConnType = jumperlessConfig.top_oled.connection_type;

    // Release crossbar bridges (if any) and mark the OLED disconnected.
    oled.disconnect();

    // End Wire1 if appropriate (Wire stays alive for DAC + INA219 sensors).
    teardownOldOledBus(oldConnType);

    // Single source of truth for pins/rows/hardwired-flag/connection_type.
    updateOledPinsForConnectionType(newConnectionType);
    configChanged = true;

    if (persist) {
        // Async save: the LOW-priority ConfigSaveService will pick this up.
        // Avoid blocking saveConfig() here because rapid menu presses would
        // stack 100-500ms flash writes inside this hot path.
        requestConfigSave();
    }

    if (!reinitDisplay) {
        return true;
    }

    // Brief settle so the I2C peripheral on the new bus is fully reset before
    // we hammer it with the SSD1306 init sequence.
    delay(100);
    oled.init();
    return oled.oledConnected;
}

int cycleOledConnectionType(bool reinitDisplay, bool persist) {
    // Skip connection_type 3 (custom) when cycling: it requires the user to
    // pick sda_pin / scl_pin manually, so cycling onto it would silently break
    // the display.
    int next = (jumperlessConfig.top_oled.connection_type + 1) % OLED_CYCLEABLE_CONNECTION_TYPES;
    applyOledConnectionType(next, reinitDisplay, persist);
    return next;
}

void oled::disconnect( void ) {
    // if (jumperlessConfig.top_oled.enabled == 0) {
    //     return;
    // }
    // oledConnected = false;
    // clear( 1000 );
    // show( 1000 );

    // Only remove crossbar bridges if not using hardwired pins
    if ( !oledUsingHardwiredPins ) {
        // Use RAM-based state system
        removeBridgeFromState( jumperlessConfig.top_oled.gpio_sda, jumperlessConfig.top_oled.sda_row );
        removeBridgeFromState( jumperlessConfig.top_oled.gpio_scl, jumperlessConfig.top_oled.scl_row );
        // Restore pins to unassigned in net map so they show as normal when disconnected
        // gpioNet[jumperlessConfig.top_oled.sda_pin - 20] = -1;
        // gpioNet[jumperlessConfig.top_oled.scl_pin - 20] = -1;
        if ( jumperlessConfig.top_oled.sda_pin >= 20 ) {
            gpioState[ jumperlessConfig.top_oled.sda_pin - 20 ] = 4;
        }
        if ( jumperlessConfig.top_oled.scl_pin >= 20 ) {
            gpioState[ jumperlessConfig.top_oled.scl_pin - 20 ] = 4;
        }
        refreshConnections( -1, 0, 0 );
    }

    oledConnected = false;
}

char scratchPad[ 40 ];

char* oled::getScratchPad( void ) {
    return scratchPad;
}

// GLOBAL FUNCTIONS
// ===============

int initOLED( void ) {
    return oled.init( );
}

int oledTest( int sdaRow, int sclRow, int sdaPin, int sclPin, int leaveConnections ) {
    oled.clear( );

    int delayTime = 8000;
    resetEncoderPosition = true;
    long lastEncoderPosition = 0;

    // Set font and get metrics for proper positioning
    oled.setFont( 0 ); // Use default font
    oled.setTextSize( 1 );
    FontMetrics metrics = oled.getFontMetrics( );

    while ( 1 ) {
        if ( encoderPosition != lastEncoderPosition ) {
            lastEncoderPosition = encoderPosition;
            oled.clear( );

            // Create encoder display string
            String encoderText = "Encoder: " + String( (int)encoderPosition );

            // Use new positioning system for centered display
            int16_t x, y;
            oled.getCenteredPosition( encoderText.c_str( ), &x, &y );

            // Set cursor with proper baseline positioning
            TextBounds bounds = oled.getTextBounds( encoderText.c_str( ) );
            oled.setCursor( x, y + bounds.ascent );
            oled.print( encoderText );

            oled.show( );
        }

        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) {
            encoderButtonState = IDLE;
            break;
        }

        if ( delayTime < 1 ) {
            delayTime = 1;
        }
    }

    oled.show( );
    return 0;
}

void testOLEDSmallFonts( void ) {
    oled.testSmallFonts( );
}

// BITMAP DATA
// ===========

// // 'Jogo255', 128x32px
const unsigned char jogo32h[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xfb, 0xff, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x8b, 0xfe, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0xfd, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x03, 0xfb, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x03, 0xf7, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x81, 0xee, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x81, 0xde, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x60, 0xbe, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x0e, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0, 0x03, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfc, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfb, 0x80, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xf7, 0xc3, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xef, 0xe0, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xdf, 0xf8, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xbf, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xc3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// Embedded binary images for filesystem provisioning
// These will be written to filesystem on firmware update if they don't exist

// bubbleJumpThin.bin - default startup image (128x32, 516 bytes with header)
const unsigned char bubbleJumpThin_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xe0, 0x3f, 0x87, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xc0, 0xe0, 0x06,
    0x18, 0xc0, 0xc8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x80, 0x7c,
    0x78, 0x31, 0x18, 0x18, 0x09, 0x00, 0x50, 0x03, 0x00, 0xe0, 0x07, 0x00,
    0x1c, 0x70, 0x43, 0x82, 0x80, 0x1a, 0x08, 0x20, 0x05, 0x00, 0x30, 0x01,
    0x03, 0x13, 0x08, 0x9c, 0x26, 0x80, 0x24, 0x01, 0x00, 0x0c, 0x0c, 0x40,
    0x06, 0x00, 0x20, 0xc1, 0x04, 0x1c, 0x88, 0xa4, 0x43, 0x00, 0x38, 0x01,
    0x00, 0x04, 0x0c, 0xc0, 0x0c, 0x18, 0x21, 0xe1, 0x08, 0x08, 0x50, 0x42,
    0x42, 0x00, 0x10, 0x01, 0x1f, 0x04, 0x0c, 0x80, 0x34, 0x3c, 0x21, 0xa3,
    0x08, 0x08, 0x50, 0x42, 0x42, 0x1e, 0x10, 0x03, 0x0f, 0x84, 0x0c, 0x83,
    0xc4, 0x34, 0x61, 0x9e, 0x08, 0x08, 0x50, 0x42, 0x82, 0x3e, 0x10, 0x3d,
    0x0e, 0x84, 0x0c, 0x87, 0x04, 0x37, 0xe0, 0xe0, 0x0c, 0x08, 0x50, 0x41,
    0x82, 0x39, 0x10, 0xf1, 0x0c, 0xc4, 0x1c, 0x86, 0x04, 0x18, 0x30, 0x30,
    0x04, 0x08, 0x50, 0x41, 0x82, 0x31, 0x10, 0xc1, 0x88, 0x44, 0x18, 0x87,
    0xf6, 0x0c, 0x10, 0x08, 0x06, 0x08, 0x50, 0x40, 0x82, 0x21, 0x10, 0x81,
    0x88, 0x44, 0x18, 0xc6, 0x1a, 0x03, 0x18, 0x04, 0x02, 0x08, 0x48, 0x40,
    0x02, 0x21, 0x10, 0xbc, 0x84, 0x44, 0x18, 0xc0, 0x0b, 0x00, 0x8e, 0x06,
    0x02, 0x08, 0x48, 0x40, 0x02, 0x21, 0x10, 0xc6, 0x84, 0x44, 0x10, 0x40,
    0x09, 0x80, 0x47, 0x82, 0x02, 0x08, 0x48, 0x40, 0x02, 0x21, 0x10, 0x02,
    0x84, 0x8e, 0x10, 0x40, 0x10, 0xf0, 0x60, 0xc1, 0x02, 0x0c, 0x28, 0x40,
    0x02, 0x12, 0x10, 0x02, 0xc7, 0x0a, 0x10, 0x41, 0xe0, 0x18, 0x27, 0x61,
    0x01, 0x04, 0x28, 0x40, 0x22, 0x1c, 0x10, 0x04, 0xc0, 0x12, 0x10, 0x43,
    0x80, 0x0c, 0x29, 0xe1, 0x01, 0x04, 0x28, 0x62, 0x23, 0x00, 0x30, 0x38,
    0x40, 0x22, 0x10, 0x43, 0x38, 0xfc, 0x30, 0xc1, 0x01, 0x04, 0x38, 0x62,
    0x63, 0x00, 0x30, 0x70, 0x40, 0x32, 0x11, 0xc3, 0xcd, 0x18, 0x20, 0x01,
    0x79, 0x04, 0x10, 0x63, 0x61, 0x00, 0x78, 0x60, 0x46, 0x1a, 0x16, 0x61,
    0x86, 0x00, 0x20, 0x02, 0xcd, 0x06, 0x00, 0x63, 0xa1, 0x0f, 0x98, 0x7f,
    0x47, 0x0e, 0x18, 0x20, 0x06, 0x00, 0x20, 0x02, 0x87, 0x06, 0x00, 0xa1,
    0x21, 0x0e, 0x18, 0x31, 0xc3, 0x06, 0x00, 0x20, 0x06, 0x00, 0x70, 0x04,
    0x82, 0x06, 0x00, 0xa1, 0x21, 0x04, 0x18, 0x00, 0xc3, 0x82, 0x00, 0x20,
    0x0f, 0x00, 0x78, 0x0c, 0x80, 0x0f, 0x00, 0xa1, 0x21, 0x04, 0x18, 0x00,
    0xc2, 0x83, 0x00, 0x70, 0x39, 0x80, 0xdf, 0xf8, 0x80, 0x0b, 0x01, 0xa1,
    0x21, 0x04, 0x0c, 0x01, 0xe2, 0xc7, 0x81, 0xdf, 0xf1, 0xff, 0x8f, 0xf0,
    0x80, 0x19, 0x81, 0xa1, 0x33, 0x8c, 0x0e, 0x0f, 0x7e, 0xfd, 0xff, 0x9f,
    0xe0, 0xff, 0x07, 0xe0, 0xc0, 0x19, 0xc3, 0x33, 0x3f, 0xfc, 0x07, 0xfe,
    0x7c, 0x7c, 0xff, 0x0f, 0xc0, 0x7e, 0x00, 0x00, 0x60, 0x30, 0xff, 0x3f,
    0x3e, 0xf8, 0x07, 0xf8, 0x38, 0x38, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x70, 0x7e, 0x1e, 0x1c, 0x70, 0x03, 0xe0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3f, 0xe0, 0x3c, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xc0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned int bubbleJumpThin_bin_len = 516;

// bubbleJump.bin - standard thickness (128x32, 516 bytes with header)
const unsigned char bubbleJump_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x3f, 0x86, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xe1, 0xf0, 0x0e,
    0x18, 0xc0, 0xc8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xc0, 0x7e,
    0x78, 0x33, 0x18, 0x18, 0x09, 0x00, 0x70, 0x03, 0x01, 0xf0, 0x07, 0x00,
    0x3c, 0xf0, 0x67, 0x82, 0x80, 0x1a, 0x08, 0x20, 0x06, 0x00, 0x30, 0x01,
    0x03, 0x17, 0x89, 0x9c, 0x67, 0x00, 0x38, 0x01, 0x00, 0x0c, 0x0c, 0x40,
    0x06, 0x00, 0x20, 0xc1, 0x0c, 0x0c, 0xd0, 0xe6, 0x42, 0x00, 0x38, 0x01,
    0x00, 0x0c, 0x0c, 0xc0, 0x0c, 0x18, 0x21, 0xe1, 0x18, 0x08, 0x60, 0xc2,
    0x42, 0x00, 0x10, 0x01, 0x1f, 0x04, 0x0c, 0x80, 0x3c, 0x3c, 0x21, 0xe3,
    0x18, 0x08, 0x60, 0x42, 0x42, 0x1e, 0x10, 0x03, 0x0f, 0x84, 0x0c, 0x83,
    0xec, 0x3c, 0x61, 0xfe, 0x18, 0x08, 0x60, 0x42, 0xc2, 0x3e, 0x10, 0x7f,
    0x0f, 0x84, 0x0c, 0x87, 0x84, 0x3f, 0xe0, 0xf8, 0x0c, 0x08, 0x60, 0x41,
    0x82, 0x3f, 0x10, 0xf9, 0x08, 0xc4, 0x1c, 0x86, 0x04, 0x1e, 0xf0, 0x38,
    0x0c, 0x08, 0x70, 0x41, 0x82, 0x33, 0x10, 0xe1, 0x88, 0x44, 0x18, 0x87,
    0xf6, 0x0e, 0x10, 0x0c, 0x06, 0x08, 0x70, 0x40, 0x82, 0x21, 0x10, 0xc1,
    0x84, 0x44, 0x18, 0xc6, 0x1e, 0x03, 0x98, 0x04, 0x06, 0x08, 0x70, 0x40,
    0x02, 0x21, 0x10, 0xfd, 0x84, 0x44, 0x18, 0xc0, 0x0b, 0x00, 0xce, 0x06,
    0x06, 0x08, 0x78, 0x40, 0x02, 0x21, 0x10, 0xc7, 0x84, 0xc4, 0x18, 0xc0,
    0x09, 0xc0, 0x47, 0x83, 0x02, 0x08, 0x78, 0x40, 0x02, 0x13, 0x10, 0x02,
    0x87, 0x8e, 0x10, 0xc0, 0x18, 0x78, 0x63, 0xc1, 0x02, 0x0c, 0x38, 0x40,
    0x02, 0x1e, 0x10, 0x02, 0xc7, 0x0e, 0x10, 0x41, 0xf0, 0x3c, 0x2f, 0xe1,
    0x03, 0x04, 0x38, 0x40, 0x22, 0x1c, 0x10, 0x06, 0xc0, 0x1e, 0x10, 0x43,
    0xc0, 0x1c, 0x39, 0xe1, 0x01, 0x04, 0x38, 0x62, 0x23, 0x00, 0x30, 0x3c,
    0xc0, 0x3a, 0x10, 0x43, 0xf9, 0xfc, 0x30, 0xc1, 0x01, 0x04, 0x38, 0x62,
    0x63, 0x00, 0x30, 0x70, 0x40, 0x32, 0x13, 0xc3, 0xcf, 0x18, 0x20, 0x01,
    0x79, 0x04, 0x10, 0x63, 0x61, 0x00, 0x78, 0x60, 0x46, 0x1a, 0x1e, 0x61,
    0x86, 0x00, 0x20, 0x03, 0xcd, 0x06, 0x00, 0x63, 0xe1, 0x0f, 0xd8, 0x7f,
    0x47, 0x0e, 0x18, 0x20, 0x06, 0x00, 0x20, 0x02, 0x87, 0x06, 0x00, 0xe1,
    0xe1, 0x0f, 0x98, 0x31, 0xc3, 0x06, 0x00, 0x20, 0x06, 0x00, 0x70, 0x06,
    0x82, 0x06, 0x00, 0xe1, 0x61, 0x06, 0x18, 0x00, 0xc3, 0x82, 0x00, 0x20,
    0x0f, 0x00, 0x78, 0x0c, 0x80, 0x0f, 0x00, 0xa1, 0x21, 0x04, 0x18, 0x00,
    0xc3, 0x83, 0x00, 0x70, 0x3f, 0x80, 0xdf, 0xf8, 0x80, 0x0b, 0x01, 0xa1,
    0x21, 0x04, 0x0c, 0x01, 0xe3, 0xc7, 0x81, 0xff, 0xfb, 0xff, 0x8f, 0xf8,
    0x80, 0x19, 0x81, 0xa1, 0x33, 0x8c, 0x0e, 0x0f, 0xfe, 0xff, 0xff, 0xdf,
    0xe1, 0xff, 0x07, 0xf0, 0xc0, 0x19, 0xc3, 0x33, 0x3f, 0xfc, 0x07, 0xff,
    0x3e, 0x7c, 0xff, 0x8f, 0xc0, 0x7e, 0x00, 0x00, 0x60, 0x30, 0xff, 0x3f,
    0x3e, 0xf8, 0x07, 0xfc, 0x1c, 0x38, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0x70, 0x7e, 0x1e, 0x1c, 0x70, 0x03, 0xe0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3f, 0xe0, 0x3c, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xc0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned int bubbleJump_bin_len = 516;

// jogo32h.bin - Jogo logo image (128x32, 500 bytes with header)
const unsigned char jogo32h_file_bin[] PROGMEM = {
    0x80, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0xfb, 0xff, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x8b, 0xfe, 0xe0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03,
    0xfd, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x06, 0x03, 0xfb, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x03, 0xf7, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x81,
    0xee, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x81, 0xde, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x60, 0xbe, 0xf8, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0,
    0x0e, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x03, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfc, 0x00, 0x38, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfb,
    0x80, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0xf7, 0xc3, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xef, 0xe0, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xdf,
    0xf8, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0xbf, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xc3, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
    0xff, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned int jogo32h_file_bin_len = 500;

// bubbleJumpThiccWhite.bin - thick white version (128x32, 516 bytes with header)
const unsigned char bubbleJumpThiccWhite_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x3f, 0x86, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xe1, 0xf0, 0x0e,
    0x18, 0xc0, 0xc8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xc0, 0x7e,
    0x78, 0x33, 0x18, 0x18, 0x09, 0x00, 0x70, 0x03, 0x01, 0xf0, 0x07, 0x00,
    0x3c, 0xf0, 0x67, 0x82, 0x80, 0x1a, 0x08, 0x20, 0x06, 0x00, 0x30, 0x01,
    0x03, 0x17, 0x89, 0x9c, 0x67, 0x00, 0x38, 0x01, 0x00, 0x0c, 0x0c, 0x40,
    0x06, 0x00, 0x20, 0xc1, 0x0c, 0x0c, 0xd0, 0xe6, 0x42, 0x00, 0x38, 0x01,
    0x00, 0x0c, 0x0c, 0xc0, 0x0c, 0x18, 0x21, 0xe1, 0x18, 0x08, 0x60, 0xc2,
    0x42, 0x00, 0x10, 0x01, 0x1f, 0x04, 0x0c, 0x80, 0x3c, 0x3c, 0x21, 0xe3,
    0x18, 0x08, 0x60, 0x42, 0x42, 0x1e, 0x10, 0x03, 0x0f, 0x84, 0x0c, 0x83,
    0xec, 0x3c, 0x61, 0xfe, 0x18, 0x08, 0x60, 0x42, 0xc2, 0x3e, 0x10, 0x7f,
    0x0f, 0x84, 0x0c, 0x87, 0x84, 0x3f, 0xe0, 0xf8, 0x0c, 0x08, 0x60, 0x41,
    0x82, 0x3f, 0x10, 0xf9, 0x08, 0xc4, 0x1c, 0x86, 0x04, 0x1e, 0xf0, 0x38,
    0x0c, 0x08, 0x70, 0x41, 0x82, 0x33, 0x10, 0xe1, 0x88, 0x44, 0x18, 0x87,
    0xf6, 0x0e, 0x10, 0x0c, 0x06, 0x08, 0x70, 0x40, 0x82, 0x21, 0x10, 0xc1,
    0x84, 0x44, 0x18, 0xc6, 0x1e, 0x03, 0x98, 0x04, 0x06, 0x08, 0x70, 0x40,
    0x02, 0x21, 0x10, 0xfd, 0x84, 0x44, 0x18, 0xc0, 0x0b, 0x00, 0xce, 0x06,
    0x06, 0x08, 0x78, 0x40, 0x02, 0x21, 0x10, 0xc7, 0x84, 0xc4, 0x18, 0xc0,
    0x09, 0xc0, 0x47, 0x83, 0x02, 0x08, 0x78, 0x40, 0x02, 0x13, 0x10, 0x02,
    0x87, 0x8e, 0x10, 0xc0, 0x18, 0x78, 0x63, 0xc1, 0x02, 0x0c, 0x38, 0x40,
    0x02, 0x1e, 0x10, 0x02, 0xc7, 0x0e, 0x10, 0x41, 0xf0, 0x3c, 0x2f, 0xe1,
    0x03, 0x04, 0x38, 0x40, 0x22, 0x1c, 0x10, 0x06, 0xc0, 0x1e, 0x10, 0x43,
    0xc0, 0x1c, 0x39, 0xe1, 0x01, 0x04, 0x38, 0x62, 0x23, 0x00, 0x30, 0x3c,
    0xc0, 0x3a, 0x10, 0x43, 0xf9, 0xfc, 0x30, 0xc1, 0x01, 0x04, 0x38, 0x62,
    0x63, 0x00, 0x30, 0x70, 0x40, 0x32, 0x13, 0xc3, 0xcf, 0x18, 0x20, 0x01,
    0x79, 0x04, 0x10, 0x63, 0x61, 0x00, 0x78, 0x60, 0x46, 0x1a, 0x1e, 0x61,
    0x86, 0x00, 0x20, 0x03, 0xcd, 0x06, 0x00, 0x63, 0xe1, 0x0f, 0xd8, 0x7f,
    0x47, 0x0e, 0x18, 0x20, 0x06, 0x00, 0x20, 0x02, 0x87, 0x06, 0x00, 0xe1,
    0xe1, 0x0f, 0x98, 0x31, 0xc3, 0x06, 0x00, 0x20, 0x06, 0x00, 0x70, 0x06,
    0x82, 0x06, 0x00, 0xe1, 0x61, 0x06, 0x18, 0x00, 0xc3, 0x82, 0x00, 0x20,
    0x0f, 0x00, 0x78, 0x0c, 0x80, 0x0f, 0x00, 0xa1, 0x21, 0x04, 0x18, 0x00,
    0xc3, 0x83, 0x00, 0x70, 0x3f, 0x80, 0xdf, 0xf8, 0x80, 0x0b, 0x01, 0xa1,
    0x21, 0x04, 0x0c, 0x01, 0xe3, 0xc7, 0x81, 0xff, 0xfb, 0xff, 0x8f, 0xf8,
    0x80, 0x19, 0x81, 0xa1, 0x33, 0x8c, 0x0e, 0x0f, 0xfe, 0xff, 0xff, 0xdf,
    0xe1, 0xff, 0x07, 0xf0, 0xc0, 0x19, 0xc3, 0x33, 0x3f, 0xfc, 0x07, 0xff,
    0x3e, 0x7c, 0xff, 0x8f, 0xc0, 0x7e, 0x00, 0x00, 0x60, 0x30, 0xff, 0x3f,
    0x3e, 0xf8, 0x07, 0xfc, 0x1c, 0x38, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0x70, 0x7e, 0x1e, 0x1c, 0x70, 0x03, 0xe0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3f, 0xe0, 0x3c, 0x0c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xc0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned int bubbleJumpThiccWhite_bin_len = 516;


// dayglow.bin - 128x32 image (516 bytes with header)
const unsigned char dayglow_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x1f, 0xff, 0xe0, 0x30, 0x03, 0x04, 0x00, 0x40,
    0x0f, 0x01, 0xc0, 0x40, 0x18, 0x18, 0x00, 0x40, 0x30, 0x00, 0x38, 0x20,
    0x01, 0x8c, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x60, 0x30, 0x08, 0x03, 0x80,
    0xc0, 0x00, 0x0c, 0x41, 0x00, 0xc8, 0x20, 0x00, 0x00, 0x00, 0x08, 0x3f,
    0xe0, 0x8c, 0x06, 0x04, 0x80, 0x00, 0x03, 0x41, 0x80, 0x68, 0x70, 0x18,
    0xf0, 0x60, 0x38, 0x00, 0x03, 0x84, 0x08, 0x0c, 0x00, 0x7c, 0x01, 0xc3,
    0xcc, 0x38, 0xf0, 0x38, 0xe0, 0xf8, 0xf0, 0x00, 0x07, 0x84, 0x18, 0x1c,
    0x07, 0xff, 0x80, 0xc3, 0xce, 0x19, 0xe0, 0x38, 0xc1, 0xfc, 0xf0, 0x00,
    0x07, 0x87, 0xf0, 0x78, 0x1f, 0xff, 0xf0, 0x03, 0xc6, 0x09, 0xe0, 0x31,
    0x87, 0xf0, 0xe0, 0x0e, 0x07, 0x86, 0x60, 0x78, 0x3f, 0xc7, 0xfc, 0x03,
    0xe7, 0x01, 0xe0, 0x31, 0x87, 0x01, 0xe0, 0x3f, 0xc3, 0x84, 0x00, 0x70,
    0x3e, 0x00, 0xfe, 0x03, 0x67, 0x01, 0xe0, 0x31, 0x8e, 0x01, 0xe0, 0x7f,
    0xe3, 0x80, 0x00, 0x70, 0x1e, 0x00, 0x3f, 0x83, 0x63, 0x81, 0xe0, 0x61,
    0x8c, 0x01, 0xe0, 0x7f, 0xe3, 0x80, 0x00, 0xf0, 0x1e, 0x00, 0x0f, 0x87,
    0x33, 0x81, 0xc0, 0x63, 0x1c, 0x00, 0xe0, 0x78, 0xf3, 0x80, 0x00, 0xe0,
    0x1e, 0x00, 0x07, 0xc7, 0x31, 0xc1, 0xc0, 0x63, 0x38, 0x00, 0xe0, 0x78,
    0x33, 0x80, 0x01, 0xe0, 0x0e, 0x0c, 0x07, 0xc6, 0x18, 0xe1, 0xc0, 0xe3,
    0x38, 0x04, 0xe0, 0x70, 0x19, 0x81, 0x01, 0xc1, 0x0e, 0x0f, 0x03, 0xc6,
    0x18, 0x71, 0xc0, 0xc7, 0x70, 0x0c, 0xe0, 0x70, 0x19, 0xc3, 0x81, 0xc3,
    0x0e, 0x09, 0x81, 0xce, 0x18, 0x7d, 0xc0, 0xc6, 0x70, 0xfc, 0xe0, 0x70,
    0x1d, 0xc3, 0x83, 0x82, 0x87, 0x08, 0xc1, 0xce, 0x1c, 0x3f, 0xc1, 0xc6,
    0x71, 0xfc, 0xe0, 0x71, 0x0d, 0xc3, 0xc3, 0x82, 0x47, 0x08, 0x41, 0xcc,
    0x0c, 0x1f, 0xc1, 0xce, 0x73, 0xdc, 0xe0, 0x71, 0x8c, 0xe3, 0xc3, 0x84,
    0x47, 0x08, 0x41, 0xcc, 0x0c, 0x0f, 0xc1, 0x8c, 0xe2, 0x18, 0x60, 0x70,
    0x0c, 0xe6, 0x63, 0x04, 0x47, 0x08, 0xc1, 0x8f, 0xfe, 0x0f, 0xc3, 0x9c,
    0xe0, 0x18, 0x60, 0x70, 0x0c, 0xe6, 0x73, 0x04, 0x47, 0x0d, 0x81, 0x9f,
    0xfe, 0x07, 0xc3, 0x9c, 0xe0, 0x18, 0x60, 0x30, 0x1c, 0x76, 0x77, 0x0c,
    0x43, 0x0e, 0x03, 0x1c, 0x06, 0x03, 0xc3, 0x18, 0xe0, 0x18, 0x60, 0x38,
    0x1c, 0x7c, 0x3e, 0x08, 0x43, 0x80, 0x03, 0x38, 0x06, 0x01, 0xc7, 0x38,
    0xe0, 0x18, 0x60, 0x18, 0x3c, 0x7c, 0x3e, 0x08, 0x43, 0x80, 0x07, 0x38,
    0x07, 0x01, 0xc7, 0x38, 0x60, 0x18, 0x60, 0x9f, 0xfc, 0x7c, 0x1e, 0x08,
    0xc3, 0x80, 0x0e, 0x30, 0x03, 0x03, 0xc7, 0x38, 0x70, 0x38, 0x61, 0xcf,
    0xfc, 0x38, 0x1e, 0x08, 0x83, 0x80, 0x3c, 0x70, 0x03, 0x03, 0xce, 0x30,
    0x70, 0x78, 0x67, 0xef, 0xf8, 0x38, 0x0e, 0x10, 0x83, 0x80, 0xfc, 0x70,
    0x03, 0x83, 0x8e, 0x70, 0x7f, 0xf8, 0xff, 0x83, 0xf0, 0x38, 0x0c, 0x10,
    0x03, 0x81, 0xf0, 0x60, 0x63, 0x83, 0x8e, 0x70, 0x3f, 0xf8, 0xfc, 0x00,
    0x00, 0x38, 0x0c, 0x10, 0x03, 0xcf, 0xe0, 0xe0, 0xf3, 0x87, 0x0c, 0x70,
    0x1f, 0x18, 0xf0, 0x00, 0x00, 0x38, 0x0c, 0x10, 0x1f, 0xff, 0xc1, 0xe1,
    0x91, 0xc7, 0x0c, 0x70, 0x00, 0x09, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x1f, 0xff, 0x01, 0xc1, 0x11, 0xc7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x30, 0x1f, 0xfc, 0x03, 0x82, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x7f, 0xff, 0x80, 0xe0, 0x20, 0x01, 0xe0, 0x00, 0x06,
    0x10, 0x00, 0x06, 0x00, 0xff, 0xf8, 0x00, 0xc0, 0x00, 0x81, 0xa0, 0x20
};
const unsigned int dayglow_bin_len = 516;

// eevblog.bin - 128x32 image (516 bytes with header)
const unsigned char eevblog_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0xff, 0xff, 0x9f, 0xff, 0xf7, 0xf8, 0x3f, 0xdf,
    0xff, 0xc0, 0x7f, 0x80, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x9f, 0xff,
    0xf7, 0xf8, 0x3f, 0xdf, 0xff, 0xf0, 0x7f, 0x80, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0x9f, 0xff, 0xf7, 0xf8, 0x3f, 0xdf, 0xff, 0xf8, 0x7f, 0x80,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x9f, 0xff, 0xf7, 0xf8, 0x3f, 0xdf,
    0xff, 0xf8, 0x7f, 0x80, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x9f, 0xff,
    0xf3, 0xf8, 0x3f, 0x9f, 0xff, 0xfc, 0x7f, 0x80, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0x9f, 0xff, 0xf3, 0xf8, 0x3f, 0x9f, 0xe3, 0xfc, 0x7f, 0x80,
    0x00, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x1f, 0xe0, 0x03, 0xfc, 0x7f, 0x9f,
    0xe1, 0xfc, 0x7f, 0x81, 0xff, 0x00, 0x7f, 0x7f, 0xfe, 0x00, 0x1f, 0xe0,
    0x03, 0xfc, 0x7f, 0x9f, 0xe1, 0xfc, 0x7f, 0x87, 0xff, 0xc0, 0xff, 0xff,
    0xfe, 0x00, 0x1f, 0xe0, 0x03, 0xfc, 0x7f, 0x1f, 0xe1, 0xfc, 0x7f, 0x8f,
    0xff, 0xe1, 0xff, 0xff, 0xfe, 0x00, 0x1f, 0xe0, 0x01, 0xfc, 0x7f, 0x1f,
    0xe3, 0xfc, 0x7f, 0x8f, 0xff, 0xe1, 0xff, 0xff, 0xff, 0xff, 0x1f, 0xff,
    0xe1, 0xfc, 0x7f, 0x1f, 0xff, 0xf8, 0x7f, 0x9f, 0xef, 0xf1, 0xff, 0xff,
    0xff, 0xff, 0x1f, 0xff, 0xe1, 0xfc, 0x7f, 0x1f, 0xff, 0xf0, 0x7f, 0x9f,
    0xc7, 0xf3, 0xfc, 0xff, 0xff, 0xff, 0x1f, 0xff, 0xe1, 0xfc, 0x7e, 0x1f,
    0xff, 0xe0, 0x7f, 0xbf, 0xc7, 0xfb, 0xf8, 0x7f, 0xff, 0xff, 0x1f, 0xff,
    0xe0, 0xfe, 0xfe, 0x1f, 0xff, 0xf8, 0x7f, 0xbf, 0xc7, 0xfb, 0xf8, 0x7f,
    0xff, 0xff, 0x1f, 0xff, 0xe0, 0xfe, 0xfe, 0x1f, 0xff, 0xfc, 0x7f, 0xbf,
    0xc7, 0xfb, 0xf8, 0x7f, 0xff, 0xff, 0x1f, 0xff, 0xe0, 0xfe, 0xfe, 0x1f,
    0xe3, 0xfc, 0x7f, 0xbf, 0xc7, 0xfb, 0xf8, 0x7f, 0xfe, 0x00, 0x1f, 0xe0,
    0x00, 0xfe, 0xfc, 0x1f, 0xe1, 0xfe, 0x7f, 0xbf, 0xc7, 0xfb, 0xf8, 0x7f,
    0xfe, 0x00, 0x1f, 0xe0, 0x00, 0xff, 0xfc, 0x1f, 0xe1, 0xfe, 0x7f, 0xbf,
    0xc7, 0xfb, 0xf8, 0x7f, 0xfe, 0x00, 0x1f, 0xe0, 0x00, 0x7f, 0xfc, 0x1f,
    0xe1, 0xfe, 0x7f, 0xbf, 0xc7, 0xfb, 0xf8, 0x7f, 0xfe, 0x00, 0x1f, 0xe0,
    0x00, 0x7f, 0xfc, 0x1f, 0xe1, 0xfe, 0x7f, 0xbf, 0xc7, 0xfb, 0xf8, 0x7f,
    0xff, 0xff, 0x9f, 0xff, 0xf0, 0x7f, 0xfc, 0x1f, 0xe3, 0xfe, 0x7f, 0x9f,
    0xc7, 0xfb, 0xfc, 0xff, 0xff, 0xff, 0x9f, 0xff, 0xf0, 0x7f, 0xf8, 0x1f,
    0xff, 0xfc, 0x7f, 0x9f, 0xc7, 0xf1, 0xff, 0xff, 0xff, 0xff, 0x9f, 0xff,
    0xf0, 0x3f, 0xf8, 0x1f, 0xff, 0xfc, 0x7f, 0x9f, 0xef, 0xf1, 0xff, 0xff,
    0xff, 0xff, 0x9f, 0xff, 0xf0, 0x3f, 0xf8, 0x1f, 0xff, 0xf8, 0x7f, 0x8f,
    0xff, 0xe0, 0xff, 0xff, 0xff, 0xff, 0x9f, 0xff, 0xf0, 0x1f, 0xf0, 0x1f,
    0xff, 0xf8, 0x7f, 0x87, 0xff, 0xc0, 0x7f, 0x7f, 0xff, 0xff, 0x9f, 0xff,
    0xf0, 0x1f, 0xf0, 0x1f, 0xff, 0xe0, 0x7f, 0x83, 0xff, 0x80, 0x3e, 0x7f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xfe, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xf8, 0x7f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xfc, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xfc
};
const unsigned int eevblog_bin_len = 516;

// excelGUI.bin - 128x32 image (516 bytes with header)
const unsigned char excelGUI_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0xe0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x28, 0x07, 0xff, 0xe0, 0x00, 0x00,
    0x00, 0x01, 0x80, 0x0f, 0x81, 0x80, 0xcf, 0xff, 0x10, 0x00, 0x24, 0x07,
    0xff, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x80, 0x1f, 0xc1, 0x80, 0xcf, 0xff,
    0x10, 0x00, 0x22, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x38,
    0xe1, 0x80, 0xc0, 0x60, 0x10, 0x00, 0x21, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x80, 0x30, 0x61, 0x80, 0xc0, 0x60, 0x10, 0x00, 0x20, 0x86,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x70, 0x31, 0x80, 0xc0, 0x60,
    0x10, 0x00, 0x3f, 0xc6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x70,
    0x31, 0x80, 0xc0, 0x60, 0x10, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x80, 0x60, 0x01, 0x80, 0xc0, 0x60, 0x10, 0x00, 0x00, 0x46,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0xe0, 0x01, 0x80, 0xc0, 0x60,
    0xff, 0xf8, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0xc0,
    0x01, 0x80, 0xc0, 0x60, 0xff, 0xf8, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x80, 0xc0, 0x01, 0x80, 0xc0, 0x60, 0xc0, 0x18, 0x00, 0x46,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0xc0, 0x01, 0x80, 0xc0, 0x60,
    0xc0, 0x18, 0x00, 0x47, 0xff, 0x06, 0x01, 0x87, 0x01, 0xc1, 0x80, 0xc0,
    0x01, 0x80, 0xc0, 0x60, 0xcf, 0x9b, 0xde, 0x47, 0xff, 0x07, 0x03, 0x8f,
    0x83, 0xe1, 0x80, 0xc0, 0x01, 0x80, 0xc0, 0x60, 0xcf, 0x9b, 0xde, 0x46,
    0x00, 0x03, 0x87, 0x18, 0xc7, 0x31, 0x80, 0xc3, 0xf9, 0x80, 0xc0, 0x60,
    0xc3, 0x18, 0x00, 0x46, 0x00, 0x01, 0xce, 0x30, 0x4c, 0x19, 0x80, 0xc3,
    0xf9, 0x80, 0xc0, 0x60, 0xc3, 0x1b, 0xde, 0x46, 0x00, 0x00, 0xfc, 0x30,
    0x0c, 0x09, 0x80, 0xc0, 0x61, 0x80, 0xc0, 0x60, 0xc3, 0x1b, 0xde, 0x46,
    0x00, 0x00, 0x78, 0x60, 0x08, 0x19, 0x80, 0xc0, 0x61, 0x80, 0xc0, 0x60,
    0xcb, 0x18, 0x00, 0x46, 0x00, 0x00, 0x30, 0x60, 0x1f, 0xf1, 0x80, 0xe0,
    0x61, 0x80, 0xc0, 0x60, 0xcf, 0x1b, 0xde, 0x46, 0x00, 0x00, 0x30, 0x60,
    0x1f, 0xe1, 0x80, 0xe0, 0x61, 0x81, 0x80, 0x60, 0xc6, 0x1b, 0xde, 0x46,
    0x00, 0x00, 0x78, 0x60, 0x08, 0x01, 0x80, 0x70, 0x61, 0x81, 0x80, 0x60,
    0xc0, 0x18, 0x00, 0x46, 0x00, 0x00, 0xfc, 0x30, 0x0c, 0x09, 0x80, 0x70,
    0xc1, 0x83, 0x80, 0x60, 0xc0, 0x18, 0x00, 0x46, 0x00, 0x01, 0xce, 0x30,
    0x4c, 0x19, 0x86, 0x39, 0xc1, 0xc3, 0xc0, 0x60, 0xff, 0xf8, 0x00, 0x46,
    0x00, 0x03, 0x87, 0x18, 0xc6, 0x31, 0xce, 0x3f, 0x80, 0xfe, 0xc0, 0x60,
    0xff, 0xf8, 0x00, 0x47, 0xff, 0xe7, 0x03, 0x8f, 0x83, 0xe0, 0xfc, 0x1f,
    0x80, 0x7e, 0x67, 0xff, 0x10, 0x00, 0x00, 0x47, 0xff, 0xe6, 0x01, 0x87,
    0x01, 0xc0, 0x78, 0x0f, 0x00, 0x3c, 0x67, 0xff, 0x10, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
const unsigned int excelGUI_bin_len = 516;

// jogo32h.bin - 128x31 image (500 bytes with header)
const unsigned char jogo32h_bin[] PROGMEM = {
    0x80, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0xfb, 0xff, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x8b, 0xfe, 0xe0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03,
    0xfd, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x06, 0x03, 0xfb, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x03, 0xf7, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x81,
    0xee, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x81, 0xde, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x60, 0xbe, 0xf8, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xf0,
    0x0e, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x03, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfc, 0x00, 0x38, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xfb,
    0x80, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0xf7, 0xc3, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xef, 0xe0, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xdf,
    0xf8, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0xbf, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xc3, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f,
    0xff, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
const unsigned int jogo32h_bin_len = 500;

// jogotextInv.bin - 128x32 image (516 bytes with header)
const unsigned char jogotextInv_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc6, 0x7f, 0x30,
    0xff, 0x18, 0x03, 0x80, 0x18, 0x03, 0xcf, 0xf0, 0x07, 0x80, 0xf8, 0x07,
    0xff, 0xc6, 0x7f, 0x30, 0xfe, 0x18, 0x01, 0x80, 0x18, 0x01, 0xcf, 0xf0,
    0x07, 0x00, 0x70, 0x07, 0xff, 0xc6, 0x7f, 0x30, 0xfe, 0x18, 0x01, 0x80,
    0x18, 0x01, 0xcf, 0xf0, 0x06, 0x00, 0x70, 0x03, 0xff, 0xc6, 0x7f, 0x30,
    0xfe, 0x18, 0x00, 0x80, 0x18, 0x00, 0xcf, 0xf0, 0x06, 0x00, 0x30, 0x03,
    0xff, 0xc6, 0x7f, 0x30, 0x7e, 0x18, 0x00, 0x8f, 0xf8, 0x00, 0xcf, 0xf3,
    0xfe, 0x3e, 0x21, 0xe3, 0xff, 0xc6, 0x7f, 0x30, 0x7c, 0x19, 0xf8, 0x8f,
    0xf9, 0xf8, 0xcf, 0xf3, 0xfe, 0x7e, 0x23, 0xf3, 0xff, 0xc6, 0x7f, 0x30,
    0x7c, 0x19, 0xfc, 0x8f, 0xf9, 0xfc, 0xcf, 0xf3, 0xfe, 0x7e, 0x23, 0xf3,
    0xff, 0xc6, 0x7f, 0x32, 0x7c, 0x99, 0xfc, 0x8f, 0xf9, 0xfc, 0xcf, 0xf3,
    0xfe, 0x7f, 0xe3, 0xff, 0xff, 0xc6, 0x7f, 0x32, 0x3c, 0x99, 0xfc, 0x8f,
    0xf9, 0xfc, 0xcf, 0xf3, 0xfe, 0x3f, 0xe3, 0xff, 0xff, 0xc6, 0x7f, 0x32,
    0x38, 0x99, 0xfc, 0x80, 0x19, 0xfc, 0xcf, 0xf0, 0x06, 0x00, 0xe0, 0x07,
    0xff, 0xc6, 0x7f, 0x33, 0x38, 0x99, 0xf8, 0x80, 0x19, 0xf8, 0xcf, 0xf0,
    0x06, 0x00, 0x70, 0x03, 0xff, 0xc6, 0x7f, 0x33, 0x39, 0x98, 0x00, 0x80,
    0x18, 0x00, 0xcf, 0xf0, 0x06, 0x00, 0x30, 0x03, 0xff, 0xc6, 0x7f, 0x33,
    0x19, 0x98, 0x00, 0x80, 0x18, 0x01, 0xcf, 0xf0, 0x07, 0x00, 0x30, 0x03,
    0xcf, 0xc6, 0x7f, 0x33, 0x11, 0x98, 0x01, 0x8f, 0xf8, 0x01, 0xcf, 0xf3,
    0xff, 0xc0, 0x3c, 0x03, 0xc7, 0xc6, 0x7f, 0x33, 0x91, 0x98, 0x01, 0x8f,
    0xf8, 0x01, 0xcf, 0xf3, 0xff, 0xfe, 0x3f, 0xf3, 0xc7, 0xc6, 0x7f, 0x33,
    0x93, 0x98, 0x03, 0x8f, 0xf8, 0x00, 0xcf, 0xf3, 0xfe, 0x7f, 0x37, 0xf3,
    0xc7, 0xc6, 0x7f, 0x33, 0x83, 0x99, 0xff, 0x8f, 0xf9, 0xf8, 0xcf, 0xf3,
    0xfe, 0x7f, 0x23, 0xf3, 0xc7, 0xc6, 0x3f, 0x33, 0x83, 0x99, 0xff, 0x8f,
    0xf9, 0xf8, 0xcf, 0xf3, 0xfe, 0x7e, 0x23, 0xf3, 0xc1, 0x06, 0x08, 0x33,
    0xc3, 0x99, 0xff, 0x8f, 0xf9, 0xf8, 0xc0, 0x33, 0xfe, 0x3c, 0x23, 0xe3,
    0xc0, 0x0e, 0x00, 0x33, 0xc7, 0x99, 0xff, 0x80, 0x19, 0xf8, 0xc0, 0x30,
    0x02, 0x00, 0x30, 0x03, 0xc0, 0x0e, 0x00, 0x33, 0xc7, 0x99, 0xff, 0x80,
    0x19, 0xf8, 0xc0, 0x30, 0x02, 0x00, 0x30, 0x03, 0xe0, 0x0f, 0x00, 0x73,
    0xc7, 0x99, 0xff, 0x80, 0x19, 0xf8, 0xc0, 0x30, 0x03, 0x00, 0x70, 0x03,
    0xf0, 0x1f, 0x80, 0xf3, 0xe7, 0x99, 0xff, 0x80, 0x19, 0xf8, 0xc0, 0x30,
    0x07, 0x00, 0xf8, 0x07, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
const unsigned int jogotextInv_bin_len = 516;

// jumperless_text.bin - 128x32 image (516 bytes with header)
const unsigned char jumperless_text_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0x80, 0xcf,
    0x00, 0xe7, 0xfc, 0x7f, 0xe7, 0xfc, 0x30, 0x0f, 0xf8, 0x7f, 0x07, 0xf8,
    0x00, 0x39, 0x80, 0xcf, 0x01, 0xe7, 0xfe, 0x7f, 0xe7, 0xfe, 0x30, 0x0f,
    0xf8, 0xff, 0x8f, 0xf8, 0x00, 0x39, 0x80, 0xcf, 0x01, 0xe7, 0xfe, 0x7f,
    0xe7, 0xfe, 0x30, 0x0f, 0xf9, 0xff, 0x8f, 0xfc, 0x00, 0x39, 0x80, 0xcf,
    0x01, 0xe7, 0xff, 0x7f, 0xe7, 0xff, 0x30, 0x0f, 0xf9, 0xff, 0xcf, 0xfc,
    0x00, 0x39, 0x80, 0xcf, 0x81, 0xe7, 0xff, 0x70, 0x07, 0xff, 0x30, 0x0c,
    0x01, 0xc1, 0xde, 0x1c, 0x00, 0x39, 0x80, 0xcf, 0x83, 0xe6, 0x07, 0x70,
    0x06, 0x07, 0x30, 0x0c, 0x01, 0x81, 0xdc, 0x0c, 0x00, 0x39, 0x80, 0xcf,
    0x83, 0xe6, 0x03, 0x70, 0x06, 0x03, 0x30, 0x0c, 0x01, 0x81, 0xdc, 0x0c,
    0x00, 0x39, 0x80, 0xcd, 0x83, 0x66, 0x03, 0x70, 0x06, 0x03, 0x30, 0x0c,
    0x01, 0x80, 0x1c, 0x00, 0x00, 0x39, 0x80, 0xcd, 0xc3, 0x66, 0x03, 0x70,
    0x06, 0x03, 0x30, 0x0c, 0x01, 0xc0, 0x1c, 0x00, 0x00, 0x39, 0x80, 0xcd,
    0xc7, 0x66, 0x03, 0x7f, 0xe6, 0x03, 0x30, 0x0f, 0xf9, 0xff, 0x1f, 0xf8,
    0x00, 0x39, 0x80, 0xcc, 0xc7, 0x66, 0x07, 0x7f, 0xe6, 0x07, 0x30, 0x0f,
    0xf9, 0xff, 0x8f, 0xfc, 0x00, 0x39, 0x80, 0xcc, 0xc6, 0x67, 0xff, 0x7f,
    0xe7, 0xff, 0x30, 0x0f, 0xf9, 0xff, 0xcf, 0xfc, 0x00, 0x39, 0x80, 0xcc,
    0xe6, 0x67, 0xff, 0x7f, 0xe7, 0xfe, 0x30, 0x0f, 0xf8, 0xff, 0xcf, 0xfc,
    0x30, 0x39, 0x80, 0xcc, 0xee, 0x67, 0xfe, 0x70, 0x07, 0xfe, 0x30, 0x0c,
    0x00, 0x3f, 0xc3, 0xfc, 0x38, 0x39, 0x80, 0xcc, 0x6e, 0x67, 0xfe, 0x70,
    0x07, 0xfe, 0x30, 0x0c, 0x00, 0x01, 0xc0, 0x0c, 0x38, 0x39, 0x80, 0xcc,
    0x6c, 0x67, 0xfc, 0x70, 0x07, 0xff, 0x30, 0x0c, 0x01, 0x80, 0xc8, 0x0c,
    0x38, 0x39, 0x80, 0xcc, 0x7c, 0x66, 0x00, 0x70, 0x06, 0x07, 0x30, 0x0c,
    0x01, 0x80, 0xdc, 0x0c, 0x38, 0x39, 0xc0, 0xcc, 0x7c, 0x66, 0x00, 0x70,
    0x06, 0x07, 0x30, 0x0c, 0x01, 0x81, 0xdc, 0x0c, 0x3e, 0xf9, 0xf7, 0xcc,
    0x3c, 0x66, 0x00, 0x70, 0x06, 0x07, 0x3f, 0xcc, 0x01, 0xc3, 0xdc, 0x1c,
    0x3f, 0xf1, 0xff, 0xcc, 0x38, 0x66, 0x00, 0x7f, 0xe6, 0x07, 0x3f, 0xcf,
    0xfd, 0xff, 0xcf, 0xfc, 0x3f, 0xf1, 0xff, 0xcc, 0x38, 0x66, 0x00, 0x7f,
    0xe6, 0x07, 0x3f, 0xcf, 0xfd, 0xff, 0xcf, 0xfc, 0x1f, 0xf0, 0xff, 0x8c,
    0x38, 0x66, 0x00, 0x7f, 0xe6, 0x07, 0x3f, 0xcf, 0xfc, 0xff, 0x8f, 0xfc,
    0x0f, 0xe0, 0x7f, 0x0c, 0x18, 0x66, 0x00, 0x7f, 0xe6, 0x07, 0x3f, 0xcf,
    0xf8, 0xff, 0x07, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
const unsigned int jumperless_text_bin_len = 516;

// ksc.bin - 128x32 image (516 bytes with header)
const unsigned char ksc_bin[] PROGMEM = {
    0x80, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xfc, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x1c, 0xff, 0xec, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xf0, 0x3f, 0x22,
    0x27, 0x00, 0x38, 0x02, 0x00, 0x00, 0x00, 0x00, 0x03, 0xfe, 0x78, 0x70,
    0x08, 0x08, 0x41, 0xa3, 0x23, 0x00, 0x10, 0xe3, 0x70, 0x71, 0xfe, 0x70,
    0x74, 0x01, 0x88, 0x88, 0x10, 0x04, 0x80, 0xa1, 0xa3, 0x80, 0x31, 0xf1,
    0x88, 0x8a, 0x01, 0x88, 0x8c, 0x01, 0x84, 0x88, 0x21, 0xc5, 0x98, 0xe0,
    0xe2, 0xf1, 0xf1, 0xd1, 0x89, 0x0c, 0x00, 0x88, 0x8b, 0x8e, 0x86, 0x88,
    0x23, 0x25, 0x3e, 0x20, 0x62, 0x31, 0xd1, 0x91, 0x8a, 0x1c, 0x3f, 0x88,
    0x88, 0x8c, 0x81, 0x88, 0x21, 0xfa, 0x32, 0x20, 0x22, 0x11, 0x91, 0x11,
    0x8c, 0x34, 0x70, 0x89, 0x88, 0x88, 0x81, 0x88, 0x30, 0x0e, 0x22, 0x22,
    0x02, 0x11, 0x11, 0x11, 0x80, 0x64, 0x7c, 0x8b, 0x88, 0x88, 0x80, 0x88,
    0x1c, 0x06, 0x3e, 0x23, 0x02, 0x11, 0x11, 0xb1, 0x80, 0xe4, 0x02, 0x87,
    0x08, 0x88, 0x88, 0x08, 0x07, 0x82, 0x00, 0x23, 0x82, 0x11, 0x10, 0xe1,
    0x81, 0xc4, 0x02, 0xc2, 0x09, 0x88, 0x8c, 0x08, 0x1f, 0xc2, 0x1c, 0x23,
    0x82, 0x11, 0x18, 0x03, 0x80, 0xc4, 0x3c, 0x40, 0x1b, 0x88, 0x8e, 0x08,
    0x23, 0x82, 0x3a, 0x23, 0x42, 0x21, 0x08, 0x06, 0x88, 0x44, 0x70, 0x60,
    0x37, 0x8e, 0x8e, 0x08, 0x20, 0x06, 0x32, 0x32, 0x46, 0x21, 0x07, 0x1c,
    0x8c, 0x24, 0x3f, 0x30, 0x64, 0x01, 0x8d, 0x18, 0x30, 0x0e, 0x22, 0x2e,
    0x3c, 0x1f, 0x79, 0xf0, 0x8e, 0x14, 0x00, 0x98, 0xc4, 0x03, 0x49, 0xb0,
    0x18, 0x19, 0xe1, 0xe0, 0x00, 0x00, 0xcf, 0x00, 0x8f, 0x0e, 0x00, 0x8d,
    0x83, 0xfe, 0x38, 0xe0, 0x0f, 0xf0, 0x00, 0xfc, 0x7f, 0xf3, 0x81, 0x80,
    0x8d, 0x85, 0x83, 0x07, 0x00, 0x00, 0x00, 0x00, 0x07, 0x0f, 0xc3, 0x82,
    0x80, 0x0a, 0x38, 0xc0, 0x88, 0xcc, 0xfc, 0x0f, 0xc7, 0xfc, 0x7f, 0xc7,
    0x08, 0xb0, 0x26, 0x01, 0x80, 0x0e, 0x7c, 0x40, 0x70, 0x7b, 0x86, 0x10,
    0x48, 0x02, 0x80, 0x28, 0x88, 0xe0, 0x1c, 0x30, 0xf0, 0x74, 0x74, 0x40,
    0x00, 0x06, 0x03, 0x30, 0x28, 0x71, 0x87, 0x18, 0x88, 0xc3, 0x0c, 0x78,
    0x88, 0xe4, 0x64, 0x40, 0x00, 0x0c, 0x01, 0x67, 0x18, 0xf8, 0x8f, 0x88,
    0x88, 0x87, 0x88, 0xe7, 0x08, 0xc4, 0x44, 0x40, 0x00, 0x08, 0x71, 0x8f,
    0x88, 0xc8, 0x8c, 0x88, 0x88, 0x8e, 0xf8, 0xc0, 0x08, 0x84, 0x44, 0x40,
    0x00, 0x08, 0xee, 0x8c, 0x88, 0x88, 0x88, 0x88, 0x88, 0x8c, 0x08, 0x80,
    0x08, 0x84, 0x44, 0x40, 0x00, 0x08, 0xc0, 0x88, 0x88, 0x88, 0x88, 0x88,
    0xc8, 0x88, 0x08, 0x80, 0x18, 0x84, 0x6c, 0x40, 0x00, 0x08, 0x80, 0x8f,
    0x88, 0xf9, 0x8f, 0x08, 0xe8, 0x8c, 0x78, 0x4e, 0x38, 0x86, 0x38, 0xc0,
    0x00, 0x08, 0x87, 0x80, 0x08, 0x01, 0x80, 0x18, 0x70, 0xc7, 0x8c, 0x31,
    0xf0, 0x73, 0x01, 0x80, 0x00, 0x08, 0x48, 0x80, 0x08, 0x03, 0x80, 0x34,
    0x01, 0xc0, 0x0a, 0x00, 0xc0, 0x09, 0x83, 0x00, 0x00, 0x0c, 0x30, 0x87,
    0x08, 0x7e, 0x87, 0xe4, 0x03, 0xb0, 0x19, 0x01, 0x40, 0x08, 0xfe, 0x00,
    0x00, 0x06, 0x01, 0x8e, 0x88, 0xe0, 0x8e, 0x03, 0x07, 0x10, 0x31, 0x83,
    0x3f, 0xf0, 0x00, 0x00, 0x00, 0x03, 0x03, 0xcc, 0x98, 0xc0, 0x8c, 0x01,
    0xfe, 0x0f, 0xe0, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xfe, 0x78,
    0xe7, 0x80, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
const unsigned int ksc_bin_len = 516;



