#include <FatFS.h>
#include "Graphics.h"
#include "MatrixState.h"
#include "config.h"
#include "PersistentStuff.h"
#include "LEDs.h"
#include "Commands.h"
#include "FileParsing.h"
#include "configManager.h"
#include "NetManager.h"
#include "Peripherals.h"
#include "FilesystemStuff.h"
#include "oled.h"
#include "ArduinoStuff.h"
#include "Apps.h"
#include "Jerial.h" // TermControl is now part of Jerial
#include "externVars.h"  // For fs_mutex filesystem synchronization
#include "usb_interface_config.h"  // For USB CDC DTR ignore configuration
#include "AsyncPassthrough.h"
#include "Probing.h"
#include "Python_Proper.h"
#include "SelfTest.h"
#include "FileCache.h"  // fileCacheFlushNow / fileCacheSpiftlSync - force config durable

#ifdef DONOTUSE_SERIALWRAPPER
    #include "SerialWrapper.h"
    #define Serial SerialWrap
#endif


// Define the global configuration instance

bool configChanged = false;
bool autoCalibrationNeeded = false;
// Flag for async config save - set true to request background save
volatile bool configSavePending = false;

struct config jumperlessConfig;
// Shadow copy of last saved config for dirty tracking (avoids unnecessary writes)
struct config lastSavedConfig;
bool shadowConfigValid = false;

// ============================================================================
// ConfigSaveService - Background config save service
// ============================================================================
ConfigSaveService* ConfigSaveService::instance = nullptr;

ConfigSaveService& ConfigSaveService::getInstance() {
    if (!instance) {
        instance = new ConfigSaveService();
    }
    return *instance;
}

// Request async config save (non-blocking)
void requestConfigSave() {
    configSavePending = true;
}


ServiceStatus ConfigSaveService::service() {
    // Check both explicit request AND configChanged flag
    // This allows saves from anywhere in the UI, not just main menu
    if (!configSavePending && !configChanged) {
        return ServiceStatus::IDLE;
    }

    // Don't save during early boot
    if (millis() < 3000) {
        return ServiceStatus::IDLE;
    }

    // Implicit dirty-flag saves (configChanged with no explicit request)
    // never run during active probing, and debounce on recent input. The
    // probe droop tracker marks the config dirty DURING probing (every tap
    // re-anchors V0), and saving right then stalls the whole probe pipeline
    // mid-tap: pauseCore2 held up to ~700ms per save with storms of several
    // full config.txt writes per second (hardware-confirmed over SWD) - the
    // user sees missed pads, a dimmed probe LED, and taps registering rows
    // late. The flag stays set, so the value persists in ONE save right
    // after the probe session / input burst ends. Deliberately NARROWER than
    // systemIdleForFlush(): menu and app settings changes still save ~2s
    // after the last click (a menu left open must not strand a dirty config
    // past a power cycle). Explicit requestConfigSave() still saves
    // promptly.
    if (!configSavePending) {
        extern volatile int probeActive;
        if (probeActive) {
            return ServiceStatus::IDLE;
        }
        if ((uint32_t)(millis() - lastUserInputMs) < 2000) {
            return ServiceStatus::IDLE;
        }
    }
    
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSaveService] Triggered - configChanged=");
        Serial.print(configChanged ? "true" : "false");
        Serial.print(" configSavePending=");
        Serial.println(configSavePending ? "true" : "false");
        Serial.println("[ConfigSaveService] Starting background save...");
        Serial.flush();
    }
    
    // Check if filesystem is busy before starting save
    // This prevents Core 0 from blocking if Core 1/MicroPython is using the FS
    if (!fs_mutex_try_acquire()) {
        if (debugConfigSaveTiming) {
            Serial.println("[ConfigSaveService] POSTPONED - Filesystem busy (fs_mutex)");
            Serial.flush();
        }
        return ServiceStatus::IDLE;
    }
    fs_mutex_release(); // Release immediately, saveConfig() will re-acquire as needed
    
    // Do the actual save
    bool saved = saveConfig();
    
    if (saved) {
        // Clear flags only after successful save
        configSavePending = false;
        configChanged = false;
        
        if (debugConfigSaveTiming) {
            Serial.println("[ConfigSaveService] Background save complete - flags cleared");
            Serial.flush();
        }
    } else {
        if (debugConfigSaveTiming) {
            Serial.println("[ConfigSaveService] Save postponed - Core 2 busy or write failed");
            Serial.flush();
        }
        // Flags remain set, will retry next loop
    }
    
    return ServiceStatus::IDLE;
}

int showNames = 1;
int lastShowNames = 1;

// Helper function to convert string to lowercase
void toLower(char* str) {
    for(int i = 0; str[i]; i++) {
        str[i] = tolower(str[i]);
    }
}

// Helper function to trim whitespace (in-place)
void trim(char* str) {
    if (str == nullptr || *str == '\0') return;
    
    // Find first non-whitespace character
    char* start = str;
    while(isspace((unsigned char)*start)) start++;
    
    // If all whitespace, make empty string
    if(*start == '\0') {
        str[0] = '\0';
        return;
    }
    
    // Find last non-whitespace character
    char* end = start + strlen(start) - 1;
    while(end > start && isspace((unsigned char)*end)) end--;
    
    // Calculate new length and move string to beginning if needed
    size_t newLen = (size_t)(end - start + 1);
    if (start != str) {
        memmove(str, start, newLen);
    }
    str[newLen] = '\0';
}

// Parse comma-separated integers into an array
void parseCommaSeparatedInts(const char* str, int* array, int maxValues) {
    char buffer[32];
    strncpy(buffer, str, sizeof(buffer)-1);
    buffer[sizeof(buffer)-1] = '\0';
    
    char* token = strtok(buffer, ",");
    int i = 0;
    while(token != NULL && i < maxValues) {
        trim(token);
        array[i++] = atoi(token);
        token = strtok(NULL, ",");
    }
}

// Parse comma-separated floats into an array
void parseCommaSeparatedFloats(const char* str, float* array, int maxValues) {
    char buffer[256];
    strncpy(buffer, str, sizeof(buffer)-1);
    buffer[sizeof(buffer)-1] = '\0';
    
    char* token = strtok(buffer, ",");
    int i = 0;
    while(token != NULL && i < maxValues) {
        trim(token);
        array[i++] = atof(token);
        token = strtok(NULL, ",");
    }
}

// Parse comma-separated booleans into an array
void parseCommaSeparatedBools(const char* str, bool* array, int maxValues) {
    char buffer[256];
    strncpy(buffer, str, sizeof(buffer)-1);
    buffer[sizeof(buffer)-1] = '\0';
    
    char* token = strtok(buffer, ",");
    int i = 0;
    while(token != NULL && i < maxValues) {
        trim(token);
        array[i++] = parseBool(token);
        token = strtok(NULL, ",");
    }
}



// Helper for all parse functions
static int parseFromTable(const StringIntEntry* table, int tableSize, const char* str, int fallbackIsAtoi = 1, int fallbackValue = -1) {
    char lower[32];
    strncpy(lower, str, sizeof(lower)-1);
    lower[sizeof(lower)-1] = '\0';
    toLower(lower);
    for (int i = 0; i < tableSize; ++i) {
        if (strcmp(lower, table[i].name) == 0) {
            return table[i].value;
        }
    }
    if (fallbackIsAtoi)
        return atoi(str);
    else
        return fallbackValue;
}

static int printFromTable(const StringIntEntry* table, int tableSize, const char* str) {
    char lower[32];
    strncpy(lower, str, sizeof(lower)-1);
    lower[sizeof(lower)-1] = '\0';
    toLower(lower);
    for (int i = 0; i < tableSize; ++i) {
        if (strcmp(lower, table[i].name) == 0) {
            return Serial.print(table[i].name);
        }
    }
    return -1;
}

int parseHex(const char* str) {
    if (str[0] == '0' && str[1] == 'x') {
        return strtol(str, NULL, 16);
    }
    return atoi(str);
}

bool parseBool(const char* str) {
    int result = parseFromTable(boolTable, boolTableSize, str, 1, 0);
    return result;
}

int parseUartFunction(const char* str) {
    return parseFromTable(uartFunctionTable, uartFunctionTableSize, str);
}

int parseLinesWires(const char* str) {
#if defined(OG_JUMPERLESS)
    // OG renders 1 LED/row, so "wires" mode is meaningless and breaks the display.
    // Force lines regardless of what the config file or a `set` command requests.
    (void)str;
    return 0;
#else
    return parseFromTable(linesWiresTable, linesWiresTableSize, str);
#endif
}

int parseNetColorMode(const char* str) {
    return parseFromTable(netColorModeTable, netColorModeTableSize, str);
}

int parseCurrentFlow(const char* str) {
    return parseFromTable(currentFlowTable, currentFlowTableSize, str);
}

int parseArbitraryFunction(const char* str) {
    return parseFromTable(arbitraryFunctionTable, arbitraryFunctionTableSize, str);
}

int parseTagParsing(const char* str) {
    return parseFromTable(tagParsingTable, tagParsingTableSize, str);
}

int parseFlashType(const char* str) {
    return parseFromTable(flashTypeTable, flashTypeTableSize, str);
}

// Parse font name from config - reads directly from fontList in oled.cpp
// Returns FontFamily enum value (0-10)
int parseFont(const char* str) {
    // Convert to lowercase for case-insensitive matching
    char lower[32];
    strncpy(lower, str, sizeof(lower)-1);
    lower[sizeof(lower)-1] = '\0';
    toLower(lower);
    
    // Search through fontList for matching shortName or longName
    for (int i = 0; i < numFonts; i++) {
        char shortLower[32];
        char longLower[32];
        
        // Check shortName (case-insensitive)
        strncpy(shortLower, fontList[i].shortName, sizeof(shortLower)-1);
        shortLower[sizeof(shortLower)-1] = '\0';
        toLower(shortLower);
        if (strcmp(lower, shortLower) == 0) {
            return (int)fontList[i].family;  // Return FontFamily enum value
        }
        
        // Check longName (case-insensitive, spaces removed)
        strncpy(longLower, fontList[i].longName, sizeof(longLower)-1);
        longLower[sizeof(longLower)-1] = '\0';
        toLower(longLower);
        
        // Remove spaces from longName for flexible matching
        char* src = longLower;
        char* dst = longLower;
        while (*src) {
            if (*src != ' ') {
                *dst++ = *src;
            }
            src++;
        }
        *dst = '\0';
        
        if (strcmp(lower, longLower) == 0) {
            return (int)fontList[i].family;  // Return FontFamily enum value
        }
    }
    
    // Fallback: try parsing as integer (backwards compatibility)
    int value = atoi(str);
    if (value >= 0 && value <= FONT_PRAGMATISM) {
        return value;
    }
    
    // Default to Eurostile if nothing matches
    return FONT_EUROSTILE;
}

// Get font name string from FontFamily value - reads directly from fontList
// Returns shortName for the given FontFamily enum value
const char* getFontString(int fontFamily) {
    // Find the first font in fontList that matches this family
    for (int i = 0; i < numFonts; i++) {
        if (fontList[i].family == (FontFamily)fontFamily) {
            return fontList[i].shortName;  // Return shortName for config display
        }
    }
    
    // Fallback to numeric string
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d", fontFamily);
    return buf;
}

int parseSerialPort(const char* str) {
    return parseFromTable(serialPortTable, serialPortTableSize, str);
}

int parseDumpFormat(const char* str) {
    return parseFromTable(dumpFormatTable, dumpFormatTableSize, str);
}

int parseConnectionType(const char* str) {
    return parseFromTable(connectionTypeTable, connectionTypeTableSize, str);
}

// Get connection type string from value
const char* getConnectionTypeString(int connectionType) {
    for (int i = 0; i < connectionTypeTableSize; i++) {
        if (connectionTypeTable[i].value == connectionType) {
            return connectionTypeTable[i].name;
        }
    }
    return "unknown";
}

// Update OLED pins based on connection_type
// Type 0 = GPIO 7/8 (via crossbar, uses GPIO 26/27 -> rows D2/D3)
// Type 1 = RP6/RP7 (hardwired, GPIO 6/7 - no row needed)
// Type 2 = internal I2C0 (hardwired, GPIO 4/5 - no row needed)
// Type 3 = custom (via crossbar, use existing sda_pin/scl_pin)
void updateOledPinsForConnectionType(int connectionType) {
    switch (connectionType) {
        case 0: // GPIO 7/8 (via crossbar using GPIO 26/27)
            jumperlessConfig.top_oled.sda_pin = 26;
            jumperlessConfig.top_oled.scl_pin = 27;
            jumperlessConfig.top_oled.gpio_sda = RP_GPIO_26;
            jumperlessConfig.top_oled.gpio_scl = RP_GPIO_27;
            jumperlessConfig.top_oled.sda_row = NANO_D2;
            jumperlessConfig.top_oled.scl_row = NANO_D3;
            oledUsingHardwiredPins = false;
            break;
        case 1: // RP6/RP7 (hardwired GPIO 6/7)
            jumperlessConfig.top_oled.sda_pin = 6;
            jumperlessConfig.top_oled.scl_pin = 7;
            jumperlessConfig.top_oled.gpio_sda = RP_GPIO_6;
            jumperlessConfig.top_oled.gpio_scl = RP_GPIO_7;
            // No row needed for hardwired connection
            jumperlessConfig.top_oled.sda_row = -1;
            jumperlessConfig.top_oled.scl_row = -1;
            oledUsingHardwiredPins = true;
            break;
        case 2: // Internal I2C0 (hardwired GPIO 4/5)
            jumperlessConfig.top_oled.sda_pin = 4;
            jumperlessConfig.top_oled.scl_pin = 5;
            jumperlessConfig.top_oled.gpio_sda = RP_GPIO_4;
            jumperlessConfig.top_oled.gpio_scl = RP_GPIO_5;
            // No row needed for hardwired connection
            jumperlessConfig.top_oled.sda_row = -1;
            jumperlessConfig.top_oled.scl_row = -1;
            oledUsingHardwiredPins = true;
            break;
        case 3: // Custom - don't change pins, user sets them manually
            // Keep existing values
            break;
        default:
            // Fall back to GPIO 7/8
            jumperlessConfig.top_oled.sda_pin = 26;
            jumperlessConfig.top_oled.scl_pin = 27;
            jumperlessConfig.top_oled.gpio_sda = RP_GPIO_26;
            jumperlessConfig.top_oled.gpio_scl = RP_GPIO_27;
            jumperlessConfig.top_oled.sda_row = NANO_D2;
            jumperlessConfig.top_oled.scl_row = NANO_D3;
            break;
    }
    jumperlessConfig.top_oled.connection_type = connectionType;
    
    // Update global hardwired pins flag - types 1 (RP6/RP7) and 2 (internal I2C0) are hardwired
    oledUsingHardwiredPins = (connectionType == 1 || connectionType == 2);
}

// NOTE: Higher-level helpers (applyOledConnectionType, cycleOledConnectionType,
// defaultOledConnectionTypeForRevision, getOledConnectionTypeShortName) now
// live in oled.cpp/oled.h alongside the rest of the OLED bus management.

void printArbitraryFunctionTable(void) {
    for (int i = 0; i < arbitraryFunctionTableSize; i++) {
        Serial.print(arbitraryFunctionTable[i].name);
        Serial.print(" = ");
        Serial.println(arbitraryFunctionTable[i].value);
    }
}

int printArbitraryFunction(int function) {
    for (int i = 0; i < arbitraryFunctionTableSize; i++) {
        if (arbitraryFunctionTable[i].value == function) {
            return Serial.print(arbitraryFunctionTable[i].name);
        }
    }
    return -1;
}

float parseFloat(const char* str) {
    return atof(str);
}

int parseInt(const char* str) {
    return atoi(str);
}

void resetConfigToDefaults(int clearCalibration, int clearHardware) {
    // Save current hardware version values
    int saved_generation = jumperlessConfig.hardware.generation;
    int saved_revision = jumperlessConfig.hardware.revision;
    int saved_probe_revision = jumperlessConfig.hardware.probe_revision;
    int saved_psram_installed = jumperlessConfig.hardware.psram_installed;
    int saved_psram_app_size_kb = jumperlessConfig.hardware.psram_app_size_kb;

    //save calibration values
    int saved_top_rail_zero = jumperlessConfig.calibration.top_rail_zero;
    int saved_bottom_rail_zero = jumperlessConfig.calibration.bottom_rail_zero;
    int saved_dac_0_zero = jumperlessConfig.calibration.dac_0_zero;
    int saved_dac_1_zero = jumperlessConfig.calibration.dac_1_zero;
    float saved_top_rail_spread = jumperlessConfig.calibration.top_rail_spread;
    float saved_bottom_rail_spread = jumperlessConfig.calibration.bottom_rail_spread;
    float saved_dac_0_spread = jumperlessConfig.calibration.dac_0_spread;
    float saved_dac_1_spread = jumperlessConfig.calibration.dac_1_spread;
    float saved_adc_0_zero = jumperlessConfig.calibration.adc_0_zero;
    float saved_adc_0_spread = jumperlessConfig.calibration.adc_0_spread;
    float saved_adc_1_zero = jumperlessConfig.calibration.adc_1_zero;
    float saved_adc_1_spread = jumperlessConfig.calibration.adc_1_spread;
    float saved_adc_2_zero = jumperlessConfig.calibration.adc_2_zero;
    float saved_adc_2_spread = jumperlessConfig.calibration.adc_2_spread;
    float saved_adc_3_zero = jumperlessConfig.calibration.adc_3_zero;
    float saved_adc_3_spread = jumperlessConfig.calibration.adc_3_spread;
    float saved_adc_4_zero = jumperlessConfig.calibration.adc_4_zero;
    float saved_adc_4_spread = jumperlessConfig.calibration.adc_4_spread;
    float saved_adc_7_zero = jumperlessConfig.calibration.adc_7_zero;
    float saved_adc_7_spread = jumperlessConfig.calibration.adc_7_spread;
    int saved_probe_max = jumperlessConfig.calibration.probe_max;
    int saved_probe_min = jumperlessConfig.calibration.probe_min;
    float saved_probe_switch_threshold = jumperlessConfig.calibration.probe_switch_threshold;
    float saved_measure_mode_output_voltage = jumperlessConfig.calibration.measure_mode_output_voltage;
    float saved_probe_current_zero = jumperlessConfig.calibration.probe_current_zero;
    int saved_minimum_probe_reading = jumperlessConfig.calibration.minimum_probe_reading;
    // Serial.print("saved_probe_min = ");
    // Serial.println(saved_probe_min);
    // Serial.print("saved_probe_max = ");
    // Serial.println(saved_probe_max);
    
    
    // Initialize with default values from config.h
    jumperlessConfig = config();
    
    // Restore hardware version values
    if (clearHardware == 0) {
    jumperlessConfig.hardware.generation = saved_generation;
    jumperlessConfig.hardware.revision = saved_revision;
    jumperlessConfig.hardware.probe_revision = saved_probe_revision;
    jumperlessConfig.hardware.psram_installed = saved_psram_installed;
    jumperlessConfig.hardware.psram_app_size_kb = saved_psram_app_size_kb;
    }
    // Restore calibration values

        if (saved_probe_min == 0 || saved_probe_max == 0) {
        jumperlessConfig.calibration.probe_min = 15;
        jumperlessConfig.calibration.probe_max = 4040;
    } 


    if (clearCalibration == 0) {
        


    jumperlessConfig.calibration.top_rail_zero = saved_top_rail_zero;
    jumperlessConfig.calibration.bottom_rail_zero = saved_bottom_rail_zero;
    jumperlessConfig.calibration.dac_0_zero = saved_dac_0_zero;
    jumperlessConfig.calibration.dac_1_zero = saved_dac_1_zero;
    jumperlessConfig.calibration.probe_max = saved_probe_max;
    jumperlessConfig.calibration.probe_min = saved_probe_min;
    jumperlessConfig.calibration.top_rail_spread = saved_top_rail_spread;
    jumperlessConfig.calibration.bottom_rail_spread = saved_bottom_rail_spread;
    jumperlessConfig.calibration.dac_0_spread = saved_dac_0_spread;
    jumperlessConfig.calibration.dac_1_spread = saved_dac_1_spread;
    jumperlessConfig.calibration.adc_0_zero = saved_adc_0_zero;
    jumperlessConfig.calibration.adc_0_spread = saved_adc_0_spread;
    jumperlessConfig.calibration.adc_1_zero = saved_adc_1_zero;
    jumperlessConfig.calibration.adc_1_spread = saved_adc_1_spread;
    jumperlessConfig.calibration.adc_2_zero = saved_adc_2_zero;
    jumperlessConfig.calibration.adc_2_spread = saved_adc_2_spread;
    jumperlessConfig.calibration.adc_3_zero = saved_adc_3_zero;
    jumperlessConfig.calibration.adc_3_spread = saved_adc_3_spread;
    jumperlessConfig.calibration.adc_4_zero = saved_adc_4_zero;
    jumperlessConfig.calibration.adc_4_spread = saved_adc_4_spread;
    jumperlessConfig.calibration.adc_7_zero = saved_adc_7_zero;
    jumperlessConfig.calibration.adc_7_spread = saved_adc_7_spread;
    jumperlessConfig.calibration.probe_switch_threshold = saved_probe_switch_threshold;
    jumperlessConfig.calibration.measure_mode_output_voltage = saved_measure_mode_output_voltage;
    jumperlessConfig.calibration.probe_current_zero = saved_probe_current_zero;
    jumperlessConfig.calibration.minimum_probe_reading = saved_minimum_probe_reading;
    } 

    // Pick a sensible OLED connection_type default based on the (preserved or
    // freshly-loaded) hardware revision. Without this, every reset on V5 rev 7
    // hardware would silently put the OLED back on GPIO 7/8 and the user would
    // have to re-pick "Internal I2C0" from the menu after every restore.
    int defaultConnType = defaultOledConnectionTypeForRevision(
        jumperlessConfig.hardware.revision);
    jumperlessConfig.top_oled.connection_type = defaultConnType;
    // Sync pin/row/hardwired-flag fields to match the chosen connection_type.
    // We don't reinit the display here because the OLED stack may not exist
    // yet at this point (resetConfigToDefaults runs before oled.init() on a
    // fresh boot, and the live menu reset path will reinit explicitly).
    updateOledPinsForConnectionType(defaultConnType);
    // defaultOledConnectionTypeForRevision is declared in oled.h, which
    // configManager.cpp already includes via #include "oled.h" near the top.

    // NOTE: Don't call saveConfig() here - callers are responsible for saving
    // after they've had a chance to restore user settings they want to preserve
}

void updateConfigFromFile(const char* filename) {
    // Check if file exists using safe function
    if (!safeFileExists(filename, 1000)) {
        Serial.println("updateConfigFromFile: File NOT FOUND, resetting to defaults!");
        firstStart = 1;
        resetConfigToDefaults();
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] TRIGGER: first boot - creating config file");
        saveConfig();  // Create config file with defaults
        return;
    }
    Serial.println("updateConfigFromFile: Found " + String(filename));

    // Open config file using safe function
    File file = safeFileOpen(filename, "r", 2000);
    if (!file) {
        Serial.println("Failed to open config file");
        return;
    }

    char line[128];
    char section[32] = "";
    char key[32];
    char value[64];
    // Config version tracking  
    const char* currentFirmwareVersion = firmwareVersion;

    
    bool foundConfigVersion = false;
    char configFirmwareVersion[16] = {0};
    bool needsReset = false;
    delay(200);//!son of a bitch
    while (file.available()) {
        int bytesRead = file.readBytesUntil('\n', line, sizeof(line)-1);
        line[bytesRead] = '\0';
        trim(line);
        
        if (line[0] == '\0' || line[0] == '#' || (line[0] == '/' && line[1] == '/')) continue;

        if (line[0] == '[' && line[strlen(line)-1] == ']') {
            // Bounded copy: a section name longer than the buffer (user/tool
            // editable config.txt) must truncate, not smash the stack.
            size_t secLen = strlen(line) - 2;
            if (secLen >= sizeof(section)) secLen = sizeof(section) - 1;
            memcpy(section, line+1, secLen);
            section[secLen] = '\0';
            toLower(section);
            continue;
        }

        char* equalsPos = strchr(line, '=');
        if (!equalsPos) continue;
        
        *equalsPos = '\0';
        // Bounded copies: line[] is 128 bytes but key/value are 32/64 —
        // unbounded strcpy here was a stack smash on long config lines.
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        strncpy(value, equalsPos + 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
        trim(key);
        trim(value);
        
        // Strip trailing semicolon from value (config file uses semicolons as line terminators)
        size_t valueLen = strlen(value);
        if (valueLen > 0 && value[valueLen - 1] == ';') {
            value[valueLen - 1] = '\0';
        }
        
        toLower(key);

        // Update config based on section and key
        if (strcmp(section, "config") == 0) {
            if (strcmp(key, "firmware_version") == 0) {
                // Strip trailing semicolon from version string first
                if (value[strlen(value)-1] == ';') {
                    value[strlen(value)-1] = '\0';
                }
                // Trim leading and trailing whitespace more robustly
                char* trimmed = value;
                while (isspace(*trimmed)) trimmed++;  // Skip leading spaces
                char* end = trimmed + strlen(trimmed) - 1;
                while (end > trimmed && isspace(*end)) end--;  // Find end of non-space chars
                *(end + 1) = '\0';  // Null terminate
                
                strncpy(configFirmwareVersion, trimmed, sizeof(configFirmwareVersion)-1);
                configFirmwareVersion[sizeof(configFirmwareVersion)-1] = '\0';  // Ensure null termination
                // Serial.print("configFirmwareVersion = ");
                // Serial.println(configFirmwareVersion);
                // Serial.print("firmwareVersion = ");
                // Serial.println(firmwareVersion);
                // Serial.print("strcmp(configFirmwareVersion, firmwareVersion) = ");
                //Serial.println(strcmp(configFirmwareVersion, firmwareVersion));
                foundConfigVersion = true;
            }
            //! this is a place to add new config options
        } else if (strcmp(section, "firmware") == 0) {
            if (strcmp(key, "last_version") == 0) {
                strncpy(jumperlessConfig.firmware.last_version, value, sizeof(jumperlessConfig.firmware.last_version)-1);
                jumperlessConfig.firmware.last_version[sizeof(jumperlessConfig.firmware.last_version)-1] = '\0';
            }
            else if (strcmp(key, "files_provisioned") == 0) jumperlessConfig.firmware.files_provisioned = parseBool(value);
        } else if (strcmp(section, "hardware") == 0) {
            if (strcmp(key, "generation") == 0) jumperlessConfig.hardware.generation = parseInt(value);
            else if (strcmp(key, "revision") == 0) jumperlessConfig.hardware.revision = parseInt(value);
            else if (strcmp(key, "probe_revision") == 0) jumperlessConfig.hardware.probe_revision = parseInt(value);
            else if (strcmp(key, "psram_installed") == 0) jumperlessConfig.hardware.psram_installed = parseBool(value);
            else if (strcmp(key, "psram_app_size_kb") == 0) jumperlessConfig.hardware.psram_app_size_kb = parseInt(value);
            else if (strcmp(key, "probe_led_on_button_pin") == 0) jumperlessConfig.hardware.probe_led_on_button_pin = parseBool(value);
        } else if (strcmp(section, "dacs") == 0) {
            // Voltage state (top_rail, bottom_rail, dac_0, dac_1) moved to globalState.power
            if (strcmp(key, "set_dacs_on_boot") == 0) jumperlessConfig.dacs.set_dacs_on_boot = parseBool(value);
            else if (strcmp(key, "set_rails_on_boot") == 0) jumperlessConfig.dacs.set_rails_on_boot = parseBool(value);
            else if (strcmp(key, "probe_power_dac") == 0) jumperlessConfig.dacs.probe_power_dac = parseInt(value);
            else if (strcmp(key, "auto_connect_probe") == 0) jumperlessConfig.dacs.auto_connect_probe = parseInt(value);
            else if (strcmp(key, "limit_max") == 0) jumperlessConfig.dacs.limit_max = parseFloat(value);
            else if (strcmp(key, "limit_min") == 0) jumperlessConfig.dacs.limit_min = parseFloat(value);
        } else if (strcmp(section, "debug") == 0) {
            if (strcmp(key, "file_parsing") == 0) jumperlessConfig.debug.file_parsing = parseBool(value);
            else if (strcmp(key, "net_manager") == 0) jumperlessConfig.debug.net_manager = parseBool(value);
            else if (strcmp(key, "nets_to_chips") == 0) jumperlessConfig.debug.nets_to_chips = parseBool(value);
            else if (strcmp(key, "nets_to_chips_alt") == 0) jumperlessConfig.debug.nets_to_chips_alt = parseBool(value);
            else if (strcmp(key, "leds") == 0) jumperlessConfig.debug.leds = parseBool(value);
            else if (strcmp(key, "probing") == 0) jumperlessConfig.debug.probing = parseBool(value);
            else if (strcmp(key, "oled") == 0) jumperlessConfig.debug.oled = parseBool(value);
            else if (strcmp(key, "logo_pads") == 0) jumperlessConfig.debug.logo_pads = parseBool(value);
            else if (strcmp(key, "logic_analyzer") == 0) jumperlessConfig.debug.logic_analyzer = parseBool(value);
            else if (strcmp(key, "arduino") == 0) jumperlessConfig.debug.arduino = parseInt(value);
            else if (strcmp(key, "usb_mass_storage") == 0) jumperlessConfig.debug.usb_mass_storage = parseBool(value);
            else if (strcmp(key, "show_probe_current") == 0) jumperlessConfig.debug.show_probe_current = parseInt(value);
            else if (strcmp(key, "show_node_errors") == 0) jumperlessConfig.debug.show_node_errors = parseBool(value);
            else if (strcmp(key, "probe_power_gpio") == 0) jumperlessConfig.debug.probe_power_gpio = parseBool(value);
            else if (strcmp(key, "probe_switch_stats") == 0) jumperlessConfig.debug.probe_switch_stats = parseBool(value);
            else if (strcmp(key, "net_voltage_scan") == 0) jumperlessConfig.debug.net_voltage_scan = parseBool(value);
        } else if (strcmp(section, "routing") == 0) {
            if (strcmp(key, "stack_paths") == 0) {
                jumperlessConfig.routing.stack_paths = parseInt(value);
                // Serial.print("Updated stack_paths to: ");
                // Serial.println(jumperlessConfig.routing.stack_paths);
            }
            else if (strcmp(key, "stack_rails") == 0) jumperlessConfig.routing.stack_rails = parseInt(value);
            else if (strcmp(key, "stack_dacs") == 0) jumperlessConfig.routing.stack_dacs = parseInt(value);
            else if (strcmp(key, "rail_priority") == 0) jumperlessConfig.routing.rail_priority = parseInt(value);
        } else if (strcmp(section, "calibration") == 0) {
            if (strcmp(key, "top_rail_zero") == 0) jumperlessConfig.calibration.top_rail_zero = parseInt(value);
            else if (strcmp(key, "top_rail_spread") == 0) jumperlessConfig.calibration.top_rail_spread = parseFloat(value);
            else if (strcmp(key, "bottom_rail_zero") == 0) jumperlessConfig.calibration.bottom_rail_zero = parseInt(value);
            else if (strcmp(key, "bottom_rail_spread") == 0) jumperlessConfig.calibration.bottom_rail_spread = parseFloat(value);
            else if (strcmp(key, "dac_0_zero") == 0) jumperlessConfig.calibration.dac_0_zero = parseInt(value);
            else if (strcmp(key, "dac_0_spread") == 0) jumperlessConfig.calibration.dac_0_spread = parseFloat(value);
            else if (strcmp(key, "dac_1_zero") == 0) jumperlessConfig.calibration.dac_1_zero = parseInt(value);
            else if (strcmp(key, "dac_1_spread") == 0) jumperlessConfig.calibration.dac_1_spread = parseFloat(value);
            else if (strcmp(key, "adc_0_zero") == 0) jumperlessConfig.calibration.adc_0_zero = parseFloat(value);
            else if (strcmp(key, "adc_0_spread") == 0) jumperlessConfig.calibration.adc_0_spread = parseFloat(value);
            else if (strcmp(key, "adc_1_zero") == 0) jumperlessConfig.calibration.adc_1_zero = parseFloat(value);
            else if (strcmp(key, "adc_1_spread") == 0) jumperlessConfig.calibration.adc_1_spread = parseFloat(value);
            else if (strcmp(key, "adc_2_zero") == 0) jumperlessConfig.calibration.adc_2_zero = parseFloat(value);
            else if (strcmp(key, "adc_2_spread") == 0) jumperlessConfig.calibration.adc_2_spread = parseFloat(value);
            else if (strcmp(key, "adc_3_zero") == 0) jumperlessConfig.calibration.adc_3_zero = parseFloat(value);
            else if (strcmp(key, "adc_3_spread") == 0) jumperlessConfig.calibration.adc_3_spread = parseFloat(value);
            else if (strcmp(key, "adc_4_zero") == 0) jumperlessConfig.calibration.adc_4_zero = parseFloat(value);
            else if (strcmp(key, "adc_4_spread") == 0) jumperlessConfig.calibration.adc_4_spread = parseFloat(value);
            else if (strcmp(key, "adc_7_zero") == 0) jumperlessConfig.calibration.adc_7_zero = parseFloat(value);
            else if (strcmp(key, "adc_7_spread") == 0) jumperlessConfig.calibration.adc_7_spread = parseFloat(value);
            else if (strcmp(key, "probe_max") == 0) jumperlessConfig.calibration.probe_max = parseInt(value);
            else if (strcmp(key, "probe_min") == 0) jumperlessConfig.calibration.probe_min = parseInt(value);
            else if (strcmp(key, "probe_max_measure") == 0) jumperlessConfig.calibration.probe_max_measure = parseInt(value);
            else if (strcmp(key, "probe_min_measure") == 0) jumperlessConfig.calibration.probe_min_measure = parseInt(value);
            else if (strcmp(key, "probe_switch_threshold_high") == 0) jumperlessConfig.calibration.probe_switch_threshold_high = parseFloat(value);
            else if (strcmp(key, "probe_switch_threshold_low") == 0) jumperlessConfig.calibration.probe_switch_threshold_low = parseFloat(value);
            else if (strcmp(key, "probe_switch_threshold") == 0) jumperlessConfig.calibration.probe_switch_threshold = parseFloat(value);
            else if (strcmp(key, "measure_mode_output_voltage") == 0) jumperlessConfig.calibration.measure_mode_output_voltage = parseFloat(value);
            else if (strcmp(key, "probe_current_zero") == 0) jumperlessConfig.calibration.probe_current_zero = parseFloat(value);
            else if (strcmp(key, "minimum_probe_reading") == 0) jumperlessConfig.calibration.minimum_probe_reading = parseInt(value);
            else if (strcmp(key, "probe_droop_v0") == 0) jumperlessConfig.calibration.probe_droop_v0 = parseFloat(value);
            else if (strcmp(key, "probe_droop_ohms") == 0) jumperlessConfig.calibration.probe_droop_ohms = parseFloat(value);
            else if (strcmp(key, "probe_pad_ohms") == 0) jumperlessConfig.calibration.probe_pad_ohms = parseFloat(value);
            else if (strcmp(key, "crosspoint_resistance") == 0) jumperlessConfig.calibration.crosspoint_resistance = parseFloat(value);
        } else if (strcmp(section, "logo_pads") == 0) {
            if (strcmp(key, "top_guy") == 0) jumperlessConfig.logo_pads.top_guy = parseArbitraryFunction(value);
            else if (strcmp(key, "bottom_guy") == 0) jumperlessConfig.logo_pads.bottom_guy = parseArbitraryFunction(value);
            else if (strcmp(key, "building_pad_top") == 0) jumperlessConfig.logo_pads.building_pad_top = parseArbitraryFunction(value);
            else if (strcmp(key, "building_pad_bottom") == 0) jumperlessConfig.logo_pads.building_pad_bottom = parseArbitraryFunction(value);
            else if (strcmp(key, "repeat_ms") == 0) jumperlessConfig.logo_pads.repeat_ms = parseInt(value);
        } else if (strcmp(section, "display") == 0) {
            if (strcmp(key, "lines_wires") == 0) jumperlessConfig.display.lines_wires = parseLinesWires(value);
            else if (strcmp(key, "menu_brightness") == 0) jumperlessConfig.display.menu_brightness = parseInt(value);
            else if (strcmp(key, "led_brightness") == 0) jumperlessConfig.display.led_brightness = parseInt(value);
            else if (strcmp(key, "rail_brightness") == 0) jumperlessConfig.display.rail_brightness = parseInt(value);
            else if (strcmp(key, "special_net_brightness") == 0) jumperlessConfig.display.special_net_brightness = parseInt(value);
            else if (strcmp(key, "net_color_mode") == 0) jumperlessConfig.display.net_color_mode = parseNetColorMode(value);
            else if (strcmp(key, "net_currents") == 0) jumperlessConfig.display.net_currents = parseBool(value);
            else if (strcmp(key, "current_flow") == 0) jumperlessConfig.display.current_flow = parseCurrentFlow(value);
            else if (strcmp(key, "dump_leds") == 0) jumperlessConfig.display.dump_leds = parseSerialPort(value);
            else if (strcmp(key, "dump_format") == 0) jumperlessConfig.display.dump_format = parseDumpFormat(value);
            else if (strcmp(key, "terminal_line_buffering") == 0) jumperlessConfig.display.terminal_line_buffering = parseBool(value);
        } else if (strcmp(section, "serial_1") == 0) {
            if (strcmp(key, "function") == 0) jumperlessConfig.serial_1.function = parseUartFunction(value);
            else if (strcmp(key, "baud_rate") == 0) jumperlessConfig.serial_1.baud_rate = parseInt(value);
            else if (strcmp(key, "print_passthrough") == 0) jumperlessConfig.serial_1.print_passthrough = parseBool(value);
            else if (strcmp(key, "connect_on_boot") == 0) jumperlessConfig.serial_1.connect_on_boot = parseBool(value);
            else if (strcmp(key, "lock_connection") == 0) jumperlessConfig.serial_1.lock_connection = parseBool(value);
            else if (strcmp(key, "autoconnect_flashing") == 0) jumperlessConfig.serial_1.autoconnect_flashing = parseBool(value);
            else if (strcmp(key, "async_passthrough") == 0) jumperlessConfig.serial_1.async_passthrough = parseBool(value);
            else if (strcmp(key, "tag_parsing") == 0) jumperlessConfig.serial_1.tag_parsing = parseTagParsing(value);
            else if (strcmp(key, "flash_reset_type") == 0) jumperlessConfig.serial_1.flash_reset_type = parseFlashType(value);
        } else if (strcmp(section, "serial_2") == 0) {
            if (strcmp(key, "function") == 0) jumperlessConfig.serial_2.function = parseUartFunction(value);
            else if (strcmp(key, "baud_rate") == 0) jumperlessConfig.serial_2.baud_rate = parseInt(value);
            else if (strcmp(key, "print_passthrough") == 0) jumperlessConfig.serial_2.print_passthrough = parseBool(value);
            else if (strcmp(key, "connect_on_boot") == 0) jumperlessConfig.serial_2.connect_on_boot = parseBool(value);
            else if (strcmp(key, "lock_connection") == 0) jumperlessConfig.serial_2.lock_connection = parseBool(value);
            else if (strcmp(key, "autoconnect_flashing") == 0) jumperlessConfig.serial_2.autoconnect_flashing = parseBool(value);
        } else if (strcmp(section, "top_oled") == 0) {
            if (strcmp(key, "enabled") == 0) jumperlessConfig.top_oled.enabled = parseBool(value);
            else if (strcmp(key, "i2c_address") == 0) jumperlessConfig.top_oled.i2c_address = parseInt(value);
            else if (strcmp(key, "display_type") == 0) jumperlessConfig.top_oled.display_type = value; // Store string directly
            else if (strcmp(key, "width") == 0) jumperlessConfig.top_oled.width = parseInt(value);
            else if (strcmp(key, "height") == 0) jumperlessConfig.top_oled.height = parseInt(value);
            else if (strcmp(key, "rotation") == 0) jumperlessConfig.top_oled.rotation = parseInt(value);
            else if (strcmp(key, "connection_type") == 0) {
                int connType = parseConnectionType(value);
                updateOledPinsForConnectionType(connType);
            }
            else if (strcmp(key, "sda_pin") == 0) jumperlessConfig.top_oled.sda_pin = parseInt(value);
            else if (strcmp(key, "scl_pin") == 0) jumperlessConfig.top_oled.scl_pin = parseInt(value);
            else if (strcmp(key, "gpio_sda") == 0) jumperlessConfig.top_oled.gpio_sda = parseInt(value);
            else if (strcmp(key, "gpio_scl") == 0) jumperlessConfig.top_oled.gpio_scl = parseInt(value);
            else if (strcmp(key, "sda_row") == 0) jumperlessConfig.top_oled.sda_row = parseInt(value);
            else if (strcmp(key, "scl_row") == 0) jumperlessConfig.top_oled.scl_row = parseInt(value);
            else if (strcmp(key, "connect_on_boot") == 0) jumperlessConfig.top_oled.connect_on_boot = parseBool(value);
            else if (strcmp(key, "lock_connection") == 0) jumperlessConfig.top_oled.lock_connection = parseBool(value);
            else if (strcmp(key, "show_in_terminal") == 0) jumperlessConfig.top_oled.show_in_terminal = parseSerialPort(value);
            else if (strcmp(key, "font") == 0) jumperlessConfig.top_oled.font = parseFont(value);
            else if (strcmp(key, "startup_message") == 0) {
                // Strip leading/trailing whitespace and quotes
                const char* start = value;
                size_t valueLen = strlen(value);
                if (valueLen == 0) {
                    jumperlessConfig.top_oled.startup_message[0] = '\0';
                } else {
                    const char* end = value + valueLen - 1;
                    
                    // Skip leading whitespace and quotes
                    while (*start && (isspace((unsigned char)*start) || *start == '"' || *start == '\'')) {
                        start++;
                    }
                    
                    // Skip trailing whitespace and quotes
                    while (end > start && (isspace((unsigned char)*end) || *end == '"' || *end == '\'')) {
                        end--;
                    }
                    
                    // Calculate length and copy
                    size_t len = (size_t)(end - start + 1);
                    if (len > 32) len = 32;
                    
                    strncpy(jumperlessConfig.top_oled.startup_message, start, len);
                    jumperlessConfig.top_oled.startup_message[len] = '\0';
                }
            }
        } else if (strcmp(section, "usb_cdc") == 0) {
            // USB CDC flow control settings
            if (strcmp(key, "ignore_dtr") == 0) jumperlessConfig.usb_cdc.ignore_dtr = parseBool(value);
        }
    }
    safeFileClose(file, false);  // Read-only, no flush

    // Check if config needs to be reset due to version differences
    if (!foundConfigVersion) {
        // Old config without version tracking - reset to be safe
        Serial.println("Config file missing version info. Resetting to defaults (preserving hardware/calibration)...");
        needsReset = true;
    } else {
        // Parse version numbers to compare
        int configGen = 5, configMajor = 0, configMinor = 0, configPatch = 0;
        int currentGen = 5, currentMajor = 0, currentMinor = 0, currentPatch = 0;
        
        sscanf(configFirmwareVersion, "%d.%d.%d.%d", &configGen, &configMajor, &configMinor, &configPatch);
        sscanf(currentFirmwareVersion, "%d.%d.%d.%d", &currentGen, &currentMajor, &currentMinor, &currentPatch);
        
        // Check if current firmware is newer than config firmware
        bool isNewerFirmware = (currentMajor > configMajor) || 
                              (currentMajor == configMajor && currentMinor > configMinor) ||
                              (currentMajor == configMajor && currentMinor == configMinor && currentPatch > configPatch);
        
        if (isNewerFirmware && newConfigOptions) {
            Serial.print("Firmware updated from ");
            Serial.print(configFirmwareVersion);
            Serial.print(" to ");
            Serial.print(currentFirmwareVersion);
            Serial.println(" with new config options. Reloading config...");
            
            // Save ALL current config values before reset
            struct config savedConfig = jumperlessConfig;
            
            // Reset to defaults to get any new options
            resetConfigToDefaults(0, 0);  // Don't clear calibration or hardware
            
            // Check if there are new calibration options by comparing calibration sections
            bool hasNewCalibrationOptions = false;
            
            // Compare key calibration parameters to detect new options
            if (jumperlessConfig.calibration.top_rail_zero != savedConfig.calibration.top_rail_zero ||
                jumperlessConfig.calibration.top_rail_spread != savedConfig.calibration.top_rail_spread ||
                jumperlessConfig.calibration.bottom_rail_zero != savedConfig.calibration.bottom_rail_zero ||
                jumperlessConfig.calibration.bottom_rail_spread != savedConfig.calibration.bottom_rail_spread ||
                jumperlessConfig.calibration.dac_0_zero != savedConfig.calibration.dac_0_zero ||
                jumperlessConfig.calibration.dac_0_spread != savedConfig.calibration.dac_0_spread ||
                jumperlessConfig.calibration.dac_1_zero != savedConfig.calibration.dac_1_zero ||
                jumperlessConfig.calibration.dac_1_spread != savedConfig.calibration.dac_1_spread ||
                jumperlessConfig.calibration.adc_0_zero != savedConfig.calibration.adc_0_zero ||
                jumperlessConfig.calibration.adc_0_spread != savedConfig.calibration.adc_0_spread ||
                jumperlessConfig.calibration.adc_1_zero != savedConfig.calibration.adc_1_zero ||
                jumperlessConfig.calibration.adc_1_spread != savedConfig.calibration.adc_1_spread ||
                jumperlessConfig.calibration.adc_2_zero != savedConfig.calibration.adc_2_zero ||
                jumperlessConfig.calibration.adc_2_spread != savedConfig.calibration.adc_2_spread ||
                jumperlessConfig.calibration.adc_3_zero != savedConfig.calibration.adc_3_zero ||
                jumperlessConfig.calibration.adc_3_spread != savedConfig.calibration.adc_3_spread ||
                jumperlessConfig.calibration.adc_4_zero != savedConfig.calibration.adc_4_zero ||
                jumperlessConfig.calibration.adc_4_spread != savedConfig.calibration.adc_4_spread ||
                jumperlessConfig.calibration.adc_7_zero != savedConfig.calibration.adc_7_zero ||
                jumperlessConfig.calibration.adc_7_spread != savedConfig.calibration.adc_7_spread ||
                jumperlessConfig.calibration.probe_switch_threshold != savedConfig.calibration.probe_switch_threshold ||
                jumperlessConfig.calibration.measure_mode_output_voltage != savedConfig.calibration.measure_mode_output_voltage ||
                jumperlessConfig.calibration.probe_current_zero != savedConfig.calibration.probe_current_zero ||
                jumperlessConfig.calibration.minimum_probe_reading != savedConfig.calibration.minimum_probe_reading) {
                hasNewCalibrationOptions = true;
            }
            
            // Restore all saved values (this preserves user settings while adding any new defaults)
            jumperlessConfig.firmware = savedConfig.firmware;
            jumperlessConfig.hardware = savedConfig.hardware;
            jumperlessConfig.dacs = savedConfig.dacs;
            jumperlessConfig.debug = savedConfig.debug;
            jumperlessConfig.routing = savedConfig.routing;
            jumperlessConfig.calibration = savedConfig.calibration;
            jumperlessConfig.logo_pads = savedConfig.logo_pads;
            jumperlessConfig.display = savedConfig.display;
            jumperlessConfig.serial_1 = savedConfig.serial_1;
            jumperlessConfig.serial_2 = savedConfig.serial_2;
            jumperlessConfig.top_oled = savedConfig.top_oled;
            
            // Save the updated config with current firmware version
            if (debugConfigSaveTiming) Serial.println("[ConfigSave] TRIGGER: firmware version update");
            saveConfig();
            //Serial.println("Config updated with new firmware version.");
            
            // Set flag to run calibration later if there are new calibration options
            if (hasNewCalibrationOptions) {
                Serial.println("New calibration options detected. Calibration will run after initialization...");
                autoCalibrationNeeded = true;
            }
            
            // Reset the flag so this only happens once per firmware update
            newConfigOptions = false;
            return;
        }
        
        // Check if firmware is significantly older (for backward compatibility warnings)
        bool majorVersionDiff = (currentMajor > configMajor);
        bool minorVersionDiff = (currentMajor == configMajor && currentMinor > configMinor + 1);
        
        if (majorVersionDiff || minorVersionDiff) {
            Serial.print("Config from firmware ");
            Serial.print(configFirmwareVersion);
            Serial.print(" is significantly older than current firmware ");
            Serial.print(currentFirmwareVersion);
            Serial.println(". Resetting to defaults (preserving hardware/calibration)...");
            needsReset = true;
        }
    }
    
    if (needsReset) {
        // Save ALL current config values before reset
        struct config savedConfig = jumperlessConfig;
        
        // Reset to defaults to get any new options
        resetConfigToDefaults(1, 1);  // Clear calibration and hardware too, we'll restore them
        
        // Restore all saved values (this preserves user settings while adding any new defaults)
        jumperlessConfig.firmware = savedConfig.firmware;
        jumperlessConfig.hardware = savedConfig.hardware;
        jumperlessConfig.dacs = savedConfig.dacs;
        jumperlessConfig.debug = savedConfig.debug;
        jumperlessConfig.routing = savedConfig.routing;
        jumperlessConfig.calibration = savedConfig.calibration;
        jumperlessConfig.logo_pads = savedConfig.logo_pads;
        jumperlessConfig.display = savedConfig.display;
        jumperlessConfig.serial_1 = savedConfig.serial_1;
        jumperlessConfig.serial_2 = savedConfig.serial_2;
        jumperlessConfig.top_oled = savedConfig.top_oled;
        
        // Save the updated config with preserved user settings + any new defaults
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] TRIGGER: major version diff reset");
        saveConfig();
        return;
    }
    
    readSettingsFromConfig();
    //initChipStatus();
}

bool saveConfigToFile(const char* filename) {
    uint32_t startTime = micros();
    if (debugConfigSaveTiming) {
        Serial.println("[ConfigSave] FULL SAVE starting...");
    }
    
    // CRITICAL: Ensure Core 2 is actually paused before writing to flash
    // Increase timeout to 1000ms to allow long operations to finish
    bool was_paused = pauseCore2ForFlash(1000);
    
    // Suspend UART IRQ before flash ops to prevent starvation of USB stack
    AsyncPassthrough::suspendUARTRxIRQ();
    
    extern volatile bool core2busy;
    if (core2busy) {
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] Core 2 busy! Aborting full save.");
        else Serial.println("[ConfigSave] Full save aborted: Core 2 busy");
        unpauseCore2ForFlash(was_paused);
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }
    
    // Mirror identity + calibration into the durable EEPROM store. This only
    // dirties the in-RAM EEPROM buffer and flags a pending commit; the actual
    // flash commit is deferred to the FileCache flush window (coalesced with a
    // file save) - NOT done synchronously here (that synchronous commit while
    // Core 1 was running was the hardfault path).
    eepromPersistFromConfig();
    
    // Write to a temp file first so a power cut mid-save can't destroy the
    // only copy of config.txt (a missing/truncated config silently resets to
    // defaults on the next boot - see updateConfigFromFile).
    String tempPath = String(filename) + ".tmp";
    File file = safeFileOpen(tempPath.c_str(), "w", 2000);
    if (!file) {
        Serial.println("Failed to create config file");
        unpauseCore2ForFlash(was_paused);
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }
 //! this is a place to add new config options
    // Write config metadata section
    file.println("[config]");
    file.print("firmware_version = "); file.print(firmwareVersion); file.println(";");
    file.println();

    // Write firmware tracking section
    file.println("[firmware]");
    file.print("last_version = "); file.print(jumperlessConfig.firmware.last_version); file.println(";");
    file.print("files_provisioned = "); file.print(jumperlessConfig.firmware.files_provisioned ? 1:0); file.println(";");
    file.println();

    // Write hardware version section
    file.println("[hardware]");
    file.print("generation = "); file.print(jumperlessConfig.hardware.generation); file.println(";");
    file.print("revision = "); file.print(jumperlessConfig.hardware.revision); file.println(";");
    file.print("probe_revision = "); file.print(jumperlessConfig.hardware.probe_revision); file.println(";");
    file.print("psram_installed = "); file.print(jumperlessConfig.hardware.psram_installed); file.println(";");
    file.print("psram_app_size_kb = "); file.print(jumperlessConfig.hardware.psram_app_size_kb); file.println(";");
    file.print("probe_led_on_button_pin = "); file.print(jumperlessConfig.hardware.probe_led_on_button_pin ? 1:0); file.println(";");
    file.println();

    // Write DAC settings section (voltage state moved to globalState.power in YAML files)
    file.println("[dacs]");
    file.print("set_dacs_on_boot = "); file.print(jumperlessConfig.dacs.set_dacs_on_boot ? 1:0); file.println(";");
    file.print("set_rails_on_boot = "); file.print(jumperlessConfig.dacs.set_rails_on_boot ? 1:0); file.println(";");
    file.print("probe_power_dac = "); file.print(jumperlessConfig.dacs.probe_power_dac == 0 ? 0 : 1); file.println(";");
    file.print("auto_connect_probe = "); file.print(jumperlessConfig.dacs.auto_connect_probe == -1 ? -1 : 1); file.println(";");
    file.print("limit_max = "); file.print(jumperlessConfig.dacs.limit_max); file.println(";");
    file.print("limit_min = "); file.print(jumperlessConfig.dacs.limit_min); file.println(";");
    file.println();

    // Write debug flags section
    file.println("[debug]");
    file.print("file_parsing = "); file.print(jumperlessConfig.debug.file_parsing ? 1:0); file.println(";");
    file.print("net_manager = "); file.print(jumperlessConfig.debug.net_manager ? 1:0); file.println(";");
    file.print("nets_to_chips = "); file.print(jumperlessConfig.debug.nets_to_chips ? 1:0); file.println(";");
    file.print("nets_to_chips_alt = "); file.print(jumperlessConfig.debug.nets_to_chips_alt ? 1:0); file.println(";");
    file.print("leds = "); file.print(jumperlessConfig.debug.leds ? 1:0); file.println(";");
    file.print("probing = "); file.print(jumperlessConfig.debug.probing ? 1:0); file.println(";");
    file.print("oled = "); file.print(jumperlessConfig.debug.oled ? 1:0); file.println(";");
    file.print("logo_pads = "); file.print(jumperlessConfig.debug.logo_pads ? 1:0); file.println(";");
    file.print("logic_analyzer = "); file.print(jumperlessConfig.debug.logic_analyzer ? 1:0); file.println(";");
    file.print("arduino = "); file.print(jumperlessConfig.debug.arduino); file.println(";");
    file.print("usb_mass_storage = "); file.print(jumperlessConfig.debug.usb_mass_storage ? 1:0); file.println(";");
    file.print("show_probe_current = "); file.print(jumperlessConfig.debug.show_probe_current); file.println(";");
    file.print("show_node_errors = "); file.print(jumperlessConfig.debug.show_node_errors ? 1:0); file.println(";");
    file.print("probe_power_gpio = "); file.print(jumperlessConfig.debug.probe_power_gpio ? 1:0); file.println(";");
    file.print("probe_switch_stats = "); file.print(jumperlessConfig.debug.probe_switch_stats ? 1:0); file.println(";");
    file.print("net_voltage_scan = "); file.print(jumperlessConfig.debug.net_voltage_scan ? 1:0); file.println(";");
    file.println();

    // Write routing settings section
    file.println("[routing]");
    file.print("stack_paths = "); file.print(jumperlessConfig.routing.stack_paths); file.println(";");
    file.print("stack_rails = "); file.print(jumperlessConfig.routing.stack_rails); file.println(";");
    file.print("stack_dacs = "); file.print(jumperlessConfig.routing.stack_dacs); file.println(";");
    file.print("rail_priority = "); file.print(jumperlessConfig.routing.rail_priority); file.println(";");
    file.println();

    // Write calibration section
    file.println("[calibration]");
    file.print("top_rail_zero = "); file.print(jumperlessConfig.calibration.top_rail_zero); file.println(";");
    file.print("top_rail_spread = "); file.print(jumperlessConfig.calibration.top_rail_spread); file.println(";");
    file.print("bottom_rail_zero = "); file.print(jumperlessConfig.calibration.bottom_rail_zero); file.println(";");
    file.print("bottom_rail_spread = "); file.print(jumperlessConfig.calibration.bottom_rail_spread); file.println(";");
    file.print("dac_0_zero = "); file.print(jumperlessConfig.calibration.dac_0_zero); file.println(";");
    file.print("dac_0_spread = "); file.print(jumperlessConfig.calibration.dac_0_spread); file.println(";");
    file.print("dac_1_zero = "); file.print(jumperlessConfig.calibration.dac_1_zero); file.println(";");
    file.print("dac_1_spread = "); file.print(jumperlessConfig.calibration.dac_1_spread); file.println(";");
    file.print("adc_0_zero = "); file.print(jumperlessConfig.calibration.adc_0_zero); file.println(";");
    file.print("adc_0_spread = "); file.print(jumperlessConfig.calibration.adc_0_spread); file.println(";");
    file.print("adc_1_zero = "); file.print(jumperlessConfig.calibration.adc_1_zero); file.println(";");
    file.print("adc_1_spread = "); file.print(jumperlessConfig.calibration.adc_1_spread); file.println(";");
    file.print("adc_2_zero = "); file.print(jumperlessConfig.calibration.adc_2_zero); file.println(";");
    file.print("adc_2_spread = "); file.print(jumperlessConfig.calibration.adc_2_spread); file.println(";");
    file.print("adc_3_zero = "); file.print(jumperlessConfig.calibration.adc_3_zero); file.println(";");
    file.print("adc_3_spread = "); file.print(jumperlessConfig.calibration.adc_3_spread); file.println(";");
    file.print("adc_4_zero = "); file.print(jumperlessConfig.calibration.adc_4_zero); file.println(";");
    file.print("adc_4_spread = "); file.print(jumperlessConfig.calibration.adc_4_spread); file.println(";");
    file.print("adc_7_zero = "); file.print(jumperlessConfig.calibration.adc_7_zero); file.println(";");
    file.print("adc_7_spread = "); file.print(jumperlessConfig.calibration.adc_7_spread); file.println(";");
    file.print("probe_max = "); file.print(jumperlessConfig.calibration.probe_max); file.println(";");
    file.print("probe_min = "); file.print(jumperlessConfig.calibration.probe_min); file.println(";");
    file.print("probe_max_measure = "); file.print(jumperlessConfig.calibration.probe_max_measure); file.println(";");
    file.print("probe_min_measure = "); file.print(jumperlessConfig.calibration.probe_min_measure); file.println(";");
    file.print("probe_switch_threshold_high = "); file.print(jumperlessConfig.calibration.probe_switch_threshold_high); file.println(";");
    file.print("probe_switch_threshold_low = "); file.print(jumperlessConfig.calibration.probe_switch_threshold_low); file.println(";");
    file.print("probe_switch_threshold = "); file.print(jumperlessConfig.calibration.probe_switch_threshold); file.println(";");
    file.print("measure_mode_output_voltage = "); file.print(jumperlessConfig.calibration.measure_mode_output_voltage); file.println(";");
    file.print("probe_current_zero = "); file.print(jumperlessConfig.calibration.probe_current_zero); file.println(";");
    file.print("minimum_probe_reading = "); file.print(jumperlessConfig.calibration.minimum_probe_reading); file.println(";");
    file.print("probe_droop_v0 = "); file.print(jumperlessConfig.calibration.probe_droop_v0, 3); file.println(";");
    file.print("probe_droop_ohms = "); file.print(jumperlessConfig.calibration.probe_droop_ohms, 1); file.println(";");
    file.print("probe_pad_ohms = "); file.print(jumperlessConfig.calibration.probe_pad_ohms, 1); file.println(";");
    file.print("crosspoint_resistance = "); file.print(jumperlessConfig.calibration.crosspoint_resistance); file.println(";");
    file.println();

    // Write logo pad settings section
    file.println("[logo_pads]");
    file.print("top_guy = "); file.print(jumperlessConfig.logo_pads.top_guy); file.println(";");
    file.print("bottom_guy = "); file.print(jumperlessConfig.logo_pads.bottom_guy); file.println(";");
    file.print("building_pad_top = "); file.print(jumperlessConfig.logo_pads.building_pad_top); file.println(";");
    file.print("building_pad_bottom = "); file.print(jumperlessConfig.logo_pads.building_pad_bottom); file.println(";");
    file.print("repeat_ms = "); file.print(jumperlessConfig.logo_pads.repeat_ms); file.println(";");
    file.println();

    // Write display settings section
    file.println("[display]");
    file.print("lines_wires = "); file.print(jumperlessConfig.display.lines_wires); file.println(";");
    file.print("menu_brightness = "); file.print(jumperlessConfig.display.menu_brightness); file.println(";");
    file.print("led_brightness = "); file.print(jumperlessConfig.display.led_brightness); file.println(";");
    file.print("rail_brightness = "); file.print(jumperlessConfig.display.rail_brightness); file.println(";");
    file.print("special_net_brightness = "); file.print(jumperlessConfig.display.special_net_brightness); file.println(";");
    file.print("net_color_mode = "); file.print(jumperlessConfig.display.net_color_mode); file.println(";");
    file.print("net_currents = "); file.print(jumperlessConfig.display.net_currents); file.println(";");
    file.print("current_flow = "); file.print(jumperlessConfig.display.current_flow); file.println(";");
    file.print("dump_leds = "); file.print(jumperlessConfig.display.dump_leds); file.println(";");
    file.print("dump_format = "); file.print(jumperlessConfig.display.dump_format); file.println(";");
    file.print("terminal_line_buffering = "); file.print(jumperlessConfig.display.terminal_line_buffering); file.println(";");
    file.println();

    // Write serial section
    file.println("[serial_1]");
    file.print("function = "); file.print(jumperlessConfig.serial_1.function); file.println(";");
    file.print("baud_rate = "); file.print(jumperlessConfig.serial_1.baud_rate); file.println(";");
    file.print("print_passthrough = "); file.print(jumperlessConfig.serial_1.print_passthrough); file.println(";");
    file.print("connect_on_boot = "); file.print(jumperlessConfig.serial_1.connect_on_boot); file.println(";");
    file.print("lock_connection = "); file.print(jumperlessConfig.serial_1.lock_connection); file.println(";");
    file.print("autoconnect_flashing = "); file.print(jumperlessConfig.serial_1.autoconnect_flashing); file.println(";");
    file.print("async_passthrough = "); file.print(jumperlessConfig.serial_1.async_passthrough ? 1:0); file.println(";");
    file.print("tag_parsing = "); file.print(jumperlessConfig.serial_1.tag_parsing); file.println(";");
    file.print("flash_reset_type = "); file.print(jumperlessConfig.serial_1.flash_reset_type); file.println(";");
    file.println();

    file.println("[serial_2]");
    file.print("function = "); file.print(jumperlessConfig.serial_2.function); file.println(";");
    file.print("baud_rate = "); file.print(jumperlessConfig.serial_2.baud_rate); file.println(";");
    file.print("print_passthrough = "); file.print(jumperlessConfig.serial_2.print_passthrough); file.println(";");
    file.print("connect_on_boot = "); file.print(jumperlessConfig.serial_2.connect_on_boot); file.println(";");
    file.print("lock_connection = "); file.print(jumperlessConfig.serial_2.lock_connection); file.println(";");
    file.print("autoconnect_flashing = "); file.print(jumperlessConfig.serial_2.autoconnect_flashing); file.println(";");
    file.println();
    // Write top_oled section
    file.println("[top_oled]");
    file.print("enabled = "); file.print(jumperlessConfig.top_oled.enabled); file.println(";");
    file.print("i2c_address = "); file.print(jumperlessConfig.top_oled.i2c_address); file.println(";");
    file.print("width = "); file.print(jumperlessConfig.top_oled.width); file.println(";");
    file.print("height = "); file.print(jumperlessConfig.top_oled.height); file.println(";");
    file.print("connection_type = "); file.print(getConnectionTypeString(jumperlessConfig.top_oled.connection_type)); file.println(";");
    file.print("sda_pin = "); file.print(jumperlessConfig.top_oled.sda_pin); file.println(";");
    file.print("scl_pin = "); file.print(jumperlessConfig.top_oled.scl_pin); file.println(";");
    file.print("gpio_sda = "); file.print(jumperlessConfig.top_oled.gpio_sda); file.println(";");
    file.print("gpio_scl = "); file.print(jumperlessConfig.top_oled.gpio_scl); file.println(";");
    file.print("sda_row = "); file.print(jumperlessConfig.top_oled.sda_row); file.println(";");
    file.print("scl_row = "); file.print(jumperlessConfig.top_oled.scl_row); file.println(";");
    file.print("connect_on_boot = "); file.print(jumperlessConfig.top_oled.connect_on_boot ? 1:0); file.println(";");
    file.print("lock_connection = "); file.print(jumperlessConfig.top_oled.lock_connection ? 1:0); file.println(";");
    file.print("show_in_terminal = "); file.print(jumperlessConfig.top_oled.show_in_terminal ? 1:0); file.println(";");
    file.print("font = "); file.print(jumperlessConfig.top_oled.font); file.println(";");
    file.print("startup_message = "); file.print(jumperlessConfig.top_oled.startup_message); file.println("");
    file.println();
    
    // Write usb_cdc section
    file.println("[usb_cdc]");
    file.print("ignore_dtr = "); file.print(jumperlessConfig.usb_cdc.ignore_dtr ? 1 : 0); file.println(";");
    
    // ponytail: f_write failures are sticky (volume-full stays full; hard I/O
    // errors set FIL.err and abort the file), so this final write failing
    // catches any earlier print() failure without checking all ~180 of them.
    bool writeOk = (file.println() == 2);
    size_t written = file.size();
    file.flush();
    safeFileClose(file, true);  // Write mode, needs flush
    
    // ponytail: 1KB sanity floor (a full save is ~3KB); the exact check would
    // be summing every print() return value above.
    if (!writeOk || written < 1024) {
        Serial.println("Config write failed - keeping existing config.txt");
        safeFileDelete(tempPath.c_str(), 1000);
        unpauseCore2ForFlash(was_paused);
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }
    
    // Temp file is fully written and flushed - replace the canonical file.
    // (FatFS rename won't overwrite, so delete first. A power cut in this
    // tiny window leaves the full config in config.txt.tmp; the old code's
    // loss window was the entire multi-ms rewrite.)
    safeFileDelete(filename, 1000);
    if (!safeFileRename(tempPath.c_str(), filename, 2000)) {
        Serial.println("Config rename failed");
        unpauseCore2ForFlash(was_paused);
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }
    unpauseCore2ForFlash(was_paused);
    AsyncPassthrough::resumeUARTRxIRQ();
    
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave] FULL SAVE complete: ");
        Serial.print(micros() - startTime);
        Serial.println(" us");
    }
    return true;
}

// Copy current config to shadow (call after successful save)
void updateShadowConfig() {
    memcpy(&lastSavedConfig, &jumperlessConfig, sizeof(struct config));
    // Note: display_type is a const char* pointer, not a char array
    // The pointer itself gets copied by memcpy, which is fine since
    // it points to string literals that don't change
    shadowConfigValid = true;
}


//! This is where we add new config options
// Compare current config with last saved to detect changes
// Returns true if config has been modified since last save
bool configHasChanges() {
    if (!shadowConfigValid) return true;  // No shadow = need to save
    
    // Compare each section individually for clarity and debuggability
    // Note: We compare individual fields rather than memcmp to handle floats properly
    
    // Firmware section
    if (strcmp(jumperlessConfig.firmware.last_version, lastSavedConfig.firmware.last_version) != 0) return true;
    if (jumperlessConfig.firmware.files_provisioned != lastSavedConfig.firmware.files_provisioned) return true;
    
    // Hardware section
    if (jumperlessConfig.hardware.generation != lastSavedConfig.hardware.generation) return true;
    if (jumperlessConfig.hardware.revision != lastSavedConfig.hardware.revision) return true;
    if (jumperlessConfig.hardware.probe_revision != lastSavedConfig.hardware.probe_revision) return true;
    if (jumperlessConfig.hardware.psram_installed != lastSavedConfig.hardware.psram_installed) return true;
    if (jumperlessConfig.hardware.psram_app_size_kb != lastSavedConfig.hardware.psram_app_size_kb) return true;
    if (jumperlessConfig.hardware.probe_led_on_button_pin != lastSavedConfig.hardware.probe_led_on_button_pin) return true;
    
    // DACs section
    if (jumperlessConfig.dacs.set_dacs_on_boot != lastSavedConfig.dacs.set_dacs_on_boot) return true;
    if (jumperlessConfig.dacs.set_rails_on_boot != lastSavedConfig.dacs.set_rails_on_boot) return true;
    if (jumperlessConfig.dacs.probe_power_dac != lastSavedConfig.dacs.probe_power_dac) return true;
    if (jumperlessConfig.dacs.auto_connect_probe == -1 && lastSavedConfig.dacs.auto_connect_probe != -1) return true;
    if (jumperlessConfig.dacs.auto_connect_probe != -1 && lastSavedConfig.dacs.auto_connect_probe == -1) return true;
    if (jumperlessConfig.dacs.limit_max != lastSavedConfig.dacs.limit_max) return true;
    if (jumperlessConfig.dacs.limit_min != lastSavedConfig.dacs.limit_min) return true;
    
    // Debug section
    if (jumperlessConfig.debug.file_parsing != lastSavedConfig.debug.file_parsing) return true;
    if (jumperlessConfig.debug.net_manager != lastSavedConfig.debug.net_manager) return true;
    if (jumperlessConfig.debug.nets_to_chips != lastSavedConfig.debug.nets_to_chips) return true;
    if (jumperlessConfig.debug.nets_to_chips_alt != lastSavedConfig.debug.nets_to_chips_alt) return true;
    if (jumperlessConfig.debug.leds != lastSavedConfig.debug.leds) return true;
    if (jumperlessConfig.debug.probing != lastSavedConfig.debug.probing) return true;
    if (jumperlessConfig.debug.oled != lastSavedConfig.debug.oled) return true;
    if (jumperlessConfig.debug.logo_pads != lastSavedConfig.debug.logo_pads) return true;
    if (jumperlessConfig.debug.logic_analyzer != lastSavedConfig.debug.logic_analyzer) return true;
    if (jumperlessConfig.debug.arduino != lastSavedConfig.debug.arduino) return true;
    if (jumperlessConfig.debug.usb_mass_storage != lastSavedConfig.debug.usb_mass_storage) return true;
    if (jumperlessConfig.debug.show_probe_current != lastSavedConfig.debug.show_probe_current) return true;
    if (jumperlessConfig.debug.show_node_errors != lastSavedConfig.debug.show_node_errors) return true;
    if (jumperlessConfig.debug.probe_power_gpio != lastSavedConfig.debug.probe_power_gpio) return true;
    if (jumperlessConfig.debug.probe_switch_stats != lastSavedConfig.debug.probe_switch_stats) return true;
    if (jumperlessConfig.debug.net_voltage_scan != lastSavedConfig.debug.net_voltage_scan) return true;
    
    // Routing section
    if (jumperlessConfig.routing.stack_paths != lastSavedConfig.routing.stack_paths) return true;
    if (jumperlessConfig.routing.stack_rails != lastSavedConfig.routing.stack_rails) return true;
    if (jumperlessConfig.routing.stack_dacs != lastSavedConfig.routing.stack_dacs) return true;
    if (jumperlessConfig.routing.rail_priority != lastSavedConfig.routing.rail_priority) return true;
    
    // Calibration section
    if (jumperlessConfig.calibration.top_rail_zero != lastSavedConfig.calibration.top_rail_zero) return true;
    if (jumperlessConfig.calibration.top_rail_spread != lastSavedConfig.calibration.top_rail_spread) return true;
    if (jumperlessConfig.calibration.bottom_rail_zero != lastSavedConfig.calibration.bottom_rail_zero) return true;
    if (jumperlessConfig.calibration.bottom_rail_spread != lastSavedConfig.calibration.bottom_rail_spread) return true;
    if (jumperlessConfig.calibration.dac_0_zero != lastSavedConfig.calibration.dac_0_zero) return true;
    if (jumperlessConfig.calibration.dac_0_spread != lastSavedConfig.calibration.dac_0_spread) return true;
    if (jumperlessConfig.calibration.dac_1_zero != lastSavedConfig.calibration.dac_1_zero) return true;
    if (jumperlessConfig.calibration.dac_1_spread != lastSavedConfig.calibration.dac_1_spread) return true;
    if (jumperlessConfig.calibration.adc_0_zero != lastSavedConfig.calibration.adc_0_zero) return true;
    if (jumperlessConfig.calibration.adc_0_spread != lastSavedConfig.calibration.adc_0_spread) return true;
    if (jumperlessConfig.calibration.adc_1_zero != lastSavedConfig.calibration.adc_1_zero) return true;
    if (jumperlessConfig.calibration.adc_1_spread != lastSavedConfig.calibration.adc_1_spread) return true;
    if (jumperlessConfig.calibration.adc_2_zero != lastSavedConfig.calibration.adc_2_zero) return true;
    if (jumperlessConfig.calibration.adc_2_spread != lastSavedConfig.calibration.adc_2_spread) return true;
    if (jumperlessConfig.calibration.adc_3_zero != lastSavedConfig.calibration.adc_3_zero) return true;
    if (jumperlessConfig.calibration.adc_3_spread != lastSavedConfig.calibration.adc_3_spread) return true;
    if (jumperlessConfig.calibration.adc_4_zero != lastSavedConfig.calibration.adc_4_zero) return true;
    if (jumperlessConfig.calibration.adc_4_spread != lastSavedConfig.calibration.adc_4_spread) return true;
    if (jumperlessConfig.calibration.adc_7_zero != lastSavedConfig.calibration.adc_7_zero) return true;
    if (jumperlessConfig.calibration.adc_7_spread != lastSavedConfig.calibration.adc_7_spread) return true;
    if (jumperlessConfig.calibration.probe_max != lastSavedConfig.calibration.probe_max) return true;
    if (jumperlessConfig.calibration.probe_min != lastSavedConfig.calibration.probe_min) return true;
    if (jumperlessConfig.calibration.probe_max_measure != lastSavedConfig.calibration.probe_max_measure) return true;
    if (jumperlessConfig.calibration.probe_min_measure != lastSavedConfig.calibration.probe_min_measure) return true;
    if (jumperlessConfig.calibration.probe_switch_threshold_high != lastSavedConfig.calibration.probe_switch_threshold_high) return true;
    if (jumperlessConfig.calibration.probe_switch_threshold_low != lastSavedConfig.calibration.probe_switch_threshold_low) return true;
    if (jumperlessConfig.calibration.probe_switch_threshold != lastSavedConfig.calibration.probe_switch_threshold) return true;
    if (jumperlessConfig.calibration.measure_mode_output_voltage != lastSavedConfig.calibration.measure_mode_output_voltage) return true;
    if (jumperlessConfig.calibration.probe_current_zero != lastSavedConfig.calibration.probe_current_zero) return true;
    if (jumperlessConfig.calibration.minimum_probe_reading != lastSavedConfig.calibration.minimum_probe_reading) return true;
    if (jumperlessConfig.calibration.probe_droop_v0 != lastSavedConfig.calibration.probe_droop_v0) return true;
    if (jumperlessConfig.calibration.probe_droop_ohms != lastSavedConfig.calibration.probe_droop_ohms) return true;
    if (jumperlessConfig.calibration.probe_pad_ohms != lastSavedConfig.calibration.probe_pad_ohms) return true;
    if (jumperlessConfig.calibration.crosspoint_resistance != lastSavedConfig.calibration.crosspoint_resistance) return true;
    
    // Logo pads section
    if (jumperlessConfig.logo_pads.top_guy != lastSavedConfig.logo_pads.top_guy) return true;
    if (jumperlessConfig.logo_pads.bottom_guy != lastSavedConfig.logo_pads.bottom_guy) return true;
    if (jumperlessConfig.logo_pads.building_pad_top != lastSavedConfig.logo_pads.building_pad_top) return true;
    if (jumperlessConfig.logo_pads.building_pad_bottom != lastSavedConfig.logo_pads.building_pad_bottom) return true;
    if (jumperlessConfig.logo_pads.repeat_ms != lastSavedConfig.logo_pads.repeat_ms) return true;
    
    // Display section
    if (jumperlessConfig.display.lines_wires != lastSavedConfig.display.lines_wires) return true;
    if (jumperlessConfig.display.menu_brightness != lastSavedConfig.display.menu_brightness) return true;
    if (jumperlessConfig.display.led_brightness != lastSavedConfig.display.led_brightness) return true;
    if (jumperlessConfig.display.rail_brightness != lastSavedConfig.display.rail_brightness) return true;
    if (jumperlessConfig.display.special_net_brightness != lastSavedConfig.display.special_net_brightness) return true;
    if (jumperlessConfig.display.net_color_mode != lastSavedConfig.display.net_color_mode) return true;
    if (jumperlessConfig.display.net_currents != lastSavedConfig.display.net_currents) return true;
    if (jumperlessConfig.display.current_flow != lastSavedConfig.display.current_flow) return true;
    if (jumperlessConfig.display.dump_leds != lastSavedConfig.display.dump_leds) return true;
    if (jumperlessConfig.display.dump_format != lastSavedConfig.display.dump_format) return true;
    if (jumperlessConfig.display.terminal_line_buffering != lastSavedConfig.display.terminal_line_buffering) return true;
    
    // Serial 1 section
    if (jumperlessConfig.serial_1.function != lastSavedConfig.serial_1.function) return true;
    if (jumperlessConfig.serial_1.baud_rate != lastSavedConfig.serial_1.baud_rate) return true;
    if (jumperlessConfig.serial_1.print_passthrough != lastSavedConfig.serial_1.print_passthrough) return true;
    if (jumperlessConfig.serial_1.connect_on_boot != lastSavedConfig.serial_1.connect_on_boot) return true;
    if (jumperlessConfig.serial_1.lock_connection != lastSavedConfig.serial_1.lock_connection) return true;
    if (jumperlessConfig.serial_1.autoconnect_flashing != lastSavedConfig.serial_1.autoconnect_flashing) return true;
    if (jumperlessConfig.serial_1.async_passthrough != lastSavedConfig.serial_1.async_passthrough) return true;
    if (jumperlessConfig.serial_1.tag_parsing != lastSavedConfig.serial_1.tag_parsing) return true;
    if (jumperlessConfig.serial_1.flash_reset_type != lastSavedConfig.serial_1.flash_reset_type) return true;
    
    // Serial 2 section
    if (jumperlessConfig.serial_2.function != lastSavedConfig.serial_2.function) return true;
    if (jumperlessConfig.serial_2.baud_rate != lastSavedConfig.serial_2.baud_rate) return true;
    if (jumperlessConfig.serial_2.print_passthrough != lastSavedConfig.serial_2.print_passthrough) return true;
    if (jumperlessConfig.serial_2.connect_on_boot != lastSavedConfig.serial_2.connect_on_boot) return true;
    if (jumperlessConfig.serial_2.lock_connection != lastSavedConfig.serial_2.lock_connection) return true;
    if (jumperlessConfig.serial_2.autoconnect_flashing != lastSavedConfig.serial_2.autoconnect_flashing) return true;
    
    // Top OLED section
    if (jumperlessConfig.top_oled.enabled != lastSavedConfig.top_oled.enabled) return true;
    if (jumperlessConfig.top_oled.i2c_address != lastSavedConfig.top_oled.i2c_address) return true;
    if (jumperlessConfig.top_oled.width != lastSavedConfig.top_oled.width) return true;
    if (jumperlessConfig.top_oled.height != lastSavedConfig.top_oled.height) return true;
    if (jumperlessConfig.top_oled.connection_type != lastSavedConfig.top_oled.connection_type) return true;
    if (jumperlessConfig.top_oled.sda_pin != lastSavedConfig.top_oled.sda_pin) return true;
    if (jumperlessConfig.top_oled.scl_pin != lastSavedConfig.top_oled.scl_pin) return true;
    if (jumperlessConfig.top_oled.gpio_sda != lastSavedConfig.top_oled.gpio_sda) return true;
    if (jumperlessConfig.top_oled.gpio_scl != lastSavedConfig.top_oled.gpio_scl) return true;
    if (jumperlessConfig.top_oled.sda_row != lastSavedConfig.top_oled.sda_row) return true;
    if (jumperlessConfig.top_oled.scl_row != lastSavedConfig.top_oled.scl_row) return true;
    if (jumperlessConfig.top_oled.connect_on_boot != lastSavedConfig.top_oled.connect_on_boot) return true;
    if (jumperlessConfig.top_oled.lock_connection != lastSavedConfig.top_oled.lock_connection) return true;
    if (jumperlessConfig.top_oled.show_in_terminal != lastSavedConfig.top_oled.show_in_terminal) return true;
    if (jumperlessConfig.top_oled.font != lastSavedConfig.top_oled.font) return true;
    if (strcmp(jumperlessConfig.top_oled.startup_message, lastSavedConfig.top_oled.startup_message) != 0) return true;
    
    return false;  // No changes detected
}

// Structure to hold a config key-value pair for incremental updates
struct ConfigKeyValue {
    char section[32];
    char key[32];
    char value[64];
};

// Helper to format a config value for writing
static void formatConfigValue(char* buf, size_t bufSize, const char* key, int value) {
    snprintf(buf, bufSize, "%s = %d;", key, value);
}

static void formatConfigValueFloat(char* buf, size_t bufSize, const char* key, float value) {
    snprintf(buf, bufSize, "%s = %.2f;", key, value);
}

static void formatConfigValueBool(char* buf, size_t bufSize, const char* key, bool value) {
    snprintf(buf, bufSize, "%s = %d;", key, value ? 1 : 0);
}

static void formatConfigValueStr(char* buf, size_t bufSize, const char* key, const char* value) {
    snprintf(buf, bufSize, "%s = %s;", key, value);
}

// Check if config file has all required sections and keys
// Returns true if file is complete, false if new options need to be added
static bool configFileIsComplete(const char* fileContent) {
    // Check for essential section headers - if any missing, need full rewrite
    // This list should be updated when new sections are added
    const char* requiredSections[] = {
        "[config]", "[firmware]", "[hardware]", "[dacs]", "[debug]",
        "[routing]", "[calibration]", "[logo_pads]", "[display]",
        "[serial_1]", "[serial_2]", "[top_oled]", "[usb_cdc]"
    };
    const int numRequired = sizeof(requiredSections) / sizeof(requiredSections[0]);
    
    for (int i = 0; i < numRequired; i++) {
        if (strstr(fileContent, requiredSections[i]) == NULL) {
            Serial.print("Config missing section: ");
            Serial.println(requiredSections[i]);
            return false;
        }
    }
    
    // Check for some key fields that might be new in recent firmware
    // Add new keys here when they're added to config.h
    const char* requiredKeys[] = {
        "firmware_version",
        "probe_switch_threshold_high",
        "probe_switch_threshold_low",
        "measure_mode_output_voltage",
        "probe_current_zero",
        "probe_droop_v0",
        "probe_droop_ohms",
        "probe_pad_ohms",
        "async_passthrough"
    };
    const int numKeys = sizeof(requiredKeys) / sizeof(requiredKeys[0]);
    
    for (int i = 0; i < numKeys; i++) {
        if (strstr(fileContent, requiredKeys[i]) == NULL) {
            Serial.print("Config missing key: ");
            Serial.println(requiredKeys[i]);
            return false;
        }
    }
    
    return true;
}

// Debug flag for config save timing - set to true to see performance metrics
// Enable with setConfigSaveDebug(true) or via serial command
bool debugConfigSaveTiming = false;

void setConfigSaveDebug(bool enable) {
    debugConfigSaveTiming = enable;
    Serial.print("[ConfigSave] Debug timing ");
    Serial.println(enable ? "ENABLED" : "DISABLED");
}

/*
// Optimized incremental save - reads existing file, updates changed values, writes once
// This minimizes flash writes by only updating when necessary
void saveConfigIncremental(const char* filename) { return false; }
*/

bool saveConfigIncremental(const char* filename) {
    uint32_t totalStartTime = micros();
    uint32_t stepTime;
    
    // First check if anything actually changed
    stepTime = micros();
    bool hasChanges = configHasChanges();
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave] hasChanges check: ");
        Serial.print(micros() - stepTime);
        Serial.print(" us (shadow valid: ");
        Serial.print(shadowConfigValid ? "yes" : "NO - first save");
        Serial.println(")");
    }
    
    if (!hasChanges) {
        if (debugConfigSaveTiming) {
            Serial.print("[ConfigSave] SKIPPED - no changes. Total: ");
            Serial.print(micros() - totalStartTime);
            Serial.println(" us");
        }
        return true; // Considered success since nothing needed saving
    }
    
    // Allocate buffer for file content (config is ~4KB)
    // Use smaller buffers to reduce heap pressure - allocate sequentially
    const size_t MAX_CONFIG_SIZE = 3000;  // 6KB should fit config
    char* fileContent = (char*)malloc(MAX_CONFIG_SIZE);
    if (!fileContent) {
        Serial.println("saveConfigIncremental: malloc1 failed, falling back to full save");
        if (saveConfigToFile(filename)) {
            updateShadowConfig();
            return true; // Fallback succeeded
        }
        return false; // Fallback failed
    }
    
    char* newContent = (char*)malloc(MAX_CONFIG_SIZE);
    if (!newContent) {
        Serial.println("saveConfigIncremental: malloc2 failed, falling back to full save");
        free(fileContent);
        if (saveConfigToFile(filename)) {
            updateShadowConfig();
            return true;
        }
        return false;
    }
    
    // Read existing file content
    stepTime = micros();
    size_t bytesRead = 0;
    bool fileExists = safeFileReadAll(filename, fileContent, MAX_CONFIG_SIZE, &bytesRead, 2000);
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave] File read (");
        Serial.print(bytesRead);
        Serial.print(" bytes): ");
        Serial.print(micros() - stepTime);
        Serial.println(" us");
    }
    
    if (!fileExists || bytesRead == 0) {
        // File doesn't exist or is empty - do full save
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] No existing file, doing full save");
        free(fileContent);
        free(newContent);
        if (saveConfigToFile(filename)) {
            updateShadowConfig();
            return true;
        }
        return false;
    }
    
    // Check if file has all required sections and keys
    // If any are missing (e.g., new options from firmware update), do full save
    stepTime = micros();
    bool isComplete = configFileIsComplete(fileContent);
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave] Completeness check: ");
        Serial.print(micros() - stepTime);
        Serial.println(" us");
    }
    
    if (!isComplete) {
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] File incomplete, doing full save");
        Serial.println("Config file incomplete, doing full save to add new options");
        free(fileContent);
        free(newContent);
        if (saveConfigToFile(filename)) {
            updateShadowConfig();
            return true;
        }
        return false;
    }
    
    // Parse and rebuild file with updated values
    // We process line by line, updating values as needed
    stepTime = micros();
    char* srcPos = fileContent;
    char* dstPos = newContent;
    char* dstEnd = newContent + MAX_CONFIG_SIZE - 256;  // Leave room for additions
    
    char currentSection[32] = "";
    char line[256];
    
    // Track which keys we've seen to detect missing new options
    bool seenKeys[128] = {false};  // Simple bitset for tracking
    int keyIndex = 0;
    
    // Helper lambda-like structure for key tracking
    #define MARK_KEY_SEEN(section, key) \
        do { if (keyIndex < 128) seenKeys[keyIndex++] = true; } while(0)
    
    while (*srcPos && dstPos < dstEnd) {
        // Extract one line
        char* lineEnd = strchr(srcPos, '\n');
        size_t lineLen;
        if (lineEnd) {
            lineLen = lineEnd - srcPos;
            if (lineLen > sizeof(line) - 1) lineLen = sizeof(line) - 1;
            memcpy(line, srcPos, lineLen);
            line[lineLen] = '\0';
            srcPos = lineEnd + 1;
        } else {
            lineLen = strlen(srcPos);
            if (lineLen > sizeof(line) - 1) lineLen = sizeof(line) - 1;
            memcpy(line, srcPos, lineLen);
            line[lineLen] = '\0';
            srcPos += lineLen;
        }
        
        // Trim the line for parsing
        char trimmedLine[256];
        strncpy(trimmedLine, line, sizeof(trimmedLine) - 1);
        trimmedLine[sizeof(trimmedLine) - 1] = '\0';
        trim(trimmedLine);
        
        // Check for section header
        if (trimmedLine[0] == '[' && trimmedLine[strlen(trimmedLine)-1] == ']') {
            // Bounded copy: section name can be up to 253 chars of a garbled
            // line but currentSection is 32 bytes — truncate, don't overflow.
            size_t secLen = strlen(trimmedLine) - 2;
            if (secLen >= sizeof(currentSection)) secLen = sizeof(currentSection) - 1;
            memcpy(currentSection, trimmedLine + 1, secLen);
            currentSection[secLen] = '\0';
            toLower(currentSection);
            
            // Copy section header as-is
            int written = snprintf(dstPos, dstEnd - dstPos, "%s\n", line);
            dstPos += written;
            continue;
        }
        
        // Check for key=value
        char* equalsPos = strchr(trimmedLine, '=');
        if (equalsPos && trimmedLine[0] != '#' && !(trimmedLine[0] == '/' && trimmedLine[1] == '/')) {
            char key[64];
            size_t keyLen = equalsPos - trimmedLine;
            if (keyLen > sizeof(key) - 1) keyLen = sizeof(key) - 1;
            memcpy(key, trimmedLine, keyLen);
            key[keyLen] = '\0';
            trim(key);
            toLower(key);
            
//! This is where we add new config options
            // Generate updated line based on section and key
            char newLine[256];
            bool updated = false;
            
            // Match section and key to generate new value
            //! [config] section
            if (strcmp(currentSection, "config") == 0) {
                if (strcmp(key, "firmware_version") == 0) {
                    snprintf(newLine, sizeof(newLine), "firmware_version = %s;", firmwareVersion);
                    updated = true;
                }
            }
            //! [firmware] section
            else if (strcmp(currentSection, "firmware") == 0) {
                if (strcmp(key, "last_version") == 0) {
                    snprintf(newLine, sizeof(newLine), "last_version = %s;", jumperlessConfig.firmware.last_version);
                    updated = true;
                } else if (strcmp(key, "files_provisioned") == 0) {
                    snprintf(newLine, sizeof(newLine), "files_provisioned = %d;", jumperlessConfig.firmware.files_provisioned ? 1 : 0);
                    updated = true;
                }
            }
            //! [hardware] section
            else if (strcmp(currentSection, "hardware") == 0) {
                if (strcmp(key, "generation") == 0) {
                    snprintf(newLine, sizeof(newLine), "generation = %d;", jumperlessConfig.hardware.generation);
                    updated = true;
                } else if (strcmp(key, "revision") == 0) {
                    snprintf(newLine, sizeof(newLine), "revision = %d;", jumperlessConfig.hardware.revision);
                    updated = true;
                } else if (strcmp(key, "probe_revision") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_revision = %d;", jumperlessConfig.hardware.probe_revision);
                    updated = true;
                } else if (strcmp(key, "psram_installed") == 0) {
                    snprintf(newLine, sizeof(newLine), "psram_installed = %d;", jumperlessConfig.hardware.psram_installed);
                    updated = true;
                } else if (strcmp(key, "psram_app_size_kb") == 0) {
                    snprintf(newLine, sizeof(newLine), "psram_app_size_kb = %d;", jumperlessConfig.hardware.psram_app_size_kb);
                    updated = true;
                } else if (strcmp(key, "probe_led_on_button_pin") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_led_on_button_pin = %d;", jumperlessConfig.hardware.probe_led_on_button_pin ? 1 : 0);
                    updated = true;
                }
            }
            //! [dacs] section
            else if (strcmp(currentSection, "dacs") == 0) {
                if (strcmp(key, "set_dacs_on_boot") == 0) {
                    snprintf(newLine, sizeof(newLine), "set_dacs_on_boot = %d;", jumperlessConfig.dacs.set_dacs_on_boot ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "set_rails_on_boot") == 0) {
                    snprintf(newLine, sizeof(newLine), "set_rails_on_boot = %d;", jumperlessConfig.dacs.set_rails_on_boot ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "probe_power_dac") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_power_dac = %d;", jumperlessConfig.dacs.probe_power_dac == 0 ? 0 : 1);
                    updated = true;
                } else if (strcmp(key, "auto_connect_probe") == 0) {
                    snprintf(newLine, sizeof(newLine), "auto_connect_probe = %d;", jumperlessConfig.dacs.auto_connect_probe == -1 ? -1 : 1);
                    updated = true;
                } else if (strcmp(key, "limit_max") == 0) {
                    snprintf(newLine, sizeof(newLine), "limit_max = %.2f;", jumperlessConfig.dacs.limit_max);
                    updated = true;
                } else if (strcmp(key, "limit_min") == 0) {
                    snprintf(newLine, sizeof(newLine), "limit_min = %.2f;", jumperlessConfig.dacs.limit_min);
                    updated = true;
                }
            }
            //! [debug] section
            else if (strcmp(currentSection, "debug") == 0) {
                if (strcmp(key, "file_parsing") == 0) {
                    snprintf(newLine, sizeof(newLine), "file_parsing = %d;", jumperlessConfig.debug.file_parsing ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "net_manager") == 0) {
                    snprintf(newLine, sizeof(newLine), "net_manager = %d;", jumperlessConfig.debug.net_manager ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "nets_to_chips") == 0) {
                    snprintf(newLine, sizeof(newLine), "nets_to_chips = %d;", jumperlessConfig.debug.nets_to_chips ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "nets_to_chips_alt") == 0) {
                    snprintf(newLine, sizeof(newLine), "nets_to_chips_alt = %d;", jumperlessConfig.debug.nets_to_chips_alt ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "leds") == 0) {
                    snprintf(newLine, sizeof(newLine), "leds = %d;", jumperlessConfig.debug.leds ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "probing") == 0) {
                    snprintf(newLine, sizeof(newLine), "probing = %d;", jumperlessConfig.debug.probing ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "oled") == 0) {
                    snprintf(newLine, sizeof(newLine), "oled = %d;", jumperlessConfig.debug.oled ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "logo_pads") == 0) {
                    snprintf(newLine, sizeof(newLine), "logo_pads = %d;", jumperlessConfig.debug.logo_pads ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "logic_analyzer") == 0) {
                    snprintf(newLine, sizeof(newLine), "logic_analyzer = %d;", jumperlessConfig.debug.logic_analyzer ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "arduino") == 0) {
                    snprintf(newLine, sizeof(newLine), "arduino = %d;", jumperlessConfig.debug.arduino);
                    updated = true;
                } else if (strcmp(key, "usb_mass_storage") == 0) {
                    snprintf(newLine, sizeof(newLine), "usb_mass_storage = %d;", jumperlessConfig.debug.usb_mass_storage ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "probe_power_gpio") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_power_gpio = %d;", jumperlessConfig.debug.probe_power_gpio ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "probe_switch_stats") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_switch_stats = %d;", jumperlessConfig.debug.probe_switch_stats ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "net_voltage_scan") == 0) {
                    snprintf(newLine, sizeof(newLine), "net_voltage_scan = %d;", jumperlessConfig.debug.net_voltage_scan ? 1 : 0);
                    updated = true;
                }
            }
            //! [routing] section
            else if (strcmp(currentSection, "routing") == 0) {
                if (strcmp(key, "stack_paths") == 0) {
                    snprintf(newLine, sizeof(newLine), "stack_paths = %d;", jumperlessConfig.routing.stack_paths);
                    updated = true;
                } else if (strcmp(key, "stack_rails") == 0) {
                    snprintf(newLine, sizeof(newLine), "stack_rails = %d;", jumperlessConfig.routing.stack_rails);
                    updated = true;
                } else if (strcmp(key, "stack_dacs") == 0) {
                    snprintf(newLine, sizeof(newLine), "stack_dacs = %d;", jumperlessConfig.routing.stack_dacs);
                    updated = true;
                } else if (strcmp(key, "rail_priority") == 0) {
                    snprintf(newLine, sizeof(newLine), "rail_priority = %d;", jumperlessConfig.routing.rail_priority);
                    updated = true;
                }
            }
            //! [calibration] section
            else if (strcmp(currentSection, "calibration") == 0) {
                if (strcmp(key, "top_rail_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "top_rail_zero = %d;", jumperlessConfig.calibration.top_rail_zero);
                    updated = true;
                } else if (strcmp(key, "top_rail_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "top_rail_spread = %.2f;", jumperlessConfig.calibration.top_rail_spread);
                    updated = true;
                } else if (strcmp(key, "bottom_rail_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "bottom_rail_zero = %d;", jumperlessConfig.calibration.bottom_rail_zero);
                    updated = true;
                } else if (strcmp(key, "bottom_rail_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "bottom_rail_spread = %.2f;", jumperlessConfig.calibration.bottom_rail_spread);
                    updated = true;
                } else if (strcmp(key, "dac_0_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "dac_0_zero = %d;", jumperlessConfig.calibration.dac_0_zero);
                    updated = true;
                } else if (strcmp(key, "dac_0_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "dac_0_spread = %.2f;", jumperlessConfig.calibration.dac_0_spread);
                    updated = true;
                } else if (strcmp(key, "dac_1_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "dac_1_zero = %d;", jumperlessConfig.calibration.dac_1_zero);
                    updated = true;
                } else if (strcmp(key, "dac_1_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "dac_1_spread = %.2f;", jumperlessConfig.calibration.dac_1_spread);
                    updated = true;
                } else if (strcmp(key, "adc_0_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_0_zero = %.2f;", jumperlessConfig.calibration.adc_0_zero);
                    updated = true;
                } else if (strcmp(key, "adc_0_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_0_spread = %.2f;", jumperlessConfig.calibration.adc_0_spread);
                    updated = true;
                } else if (strcmp(key, "adc_1_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_1_zero = %.2f;", jumperlessConfig.calibration.adc_1_zero);
                    updated = true;
                } else if (strcmp(key, "adc_1_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_1_spread = %.2f;", jumperlessConfig.calibration.adc_1_spread);
                    updated = true;
                } else if (strcmp(key, "adc_2_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_2_zero = %.2f;", jumperlessConfig.calibration.adc_2_zero);
                    updated = true;
                } else if (strcmp(key, "adc_2_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_2_spread = %.2f;", jumperlessConfig.calibration.adc_2_spread);
                    updated = true;
                } else if (strcmp(key, "adc_3_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_3_zero = %.2f;", jumperlessConfig.calibration.adc_3_zero);
                    updated = true;
                } else if (strcmp(key, "adc_3_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_3_spread = %.2f;", jumperlessConfig.calibration.adc_3_spread);
                    updated = true;
                } else if (strcmp(key, "adc_4_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_4_zero = %.2f;", jumperlessConfig.calibration.adc_4_zero);
                    updated = true;
                } else if (strcmp(key, "adc_4_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_4_spread = %.2f;", jumperlessConfig.calibration.adc_4_spread);
                    updated = true;
                } else if (strcmp(key, "adc_7_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_7_zero = %.2f;", jumperlessConfig.calibration.adc_7_zero);
                    updated = true;
                } else if (strcmp(key, "adc_7_spread") == 0) {
                    snprintf(newLine, sizeof(newLine), "adc_7_spread = %.2f;", jumperlessConfig.calibration.adc_7_spread);
                    updated = true;
                } else if (strcmp(key, "probe_max") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_max = %d;", jumperlessConfig.calibration.probe_max);
                    updated = true;
                } else if (strcmp(key, "probe_min") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_min = %d;", jumperlessConfig.calibration.probe_min);
                    updated = true;
                } else if (strcmp(key, "probe_max_measure") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_max_measure = %d;", jumperlessConfig.calibration.probe_max_measure);
                    updated = true;
                } else if (strcmp(key, "probe_min_measure") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_min_measure = %d;", jumperlessConfig.calibration.probe_min_measure);
                    updated = true;
                } else if (strcmp(key, "probe_switch_threshold_high") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_switch_threshold_high = %.2f;", jumperlessConfig.calibration.probe_switch_threshold_high);
                    updated = true;
                } else if (strcmp(key, "probe_switch_threshold_low") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_switch_threshold_low = %.2f;", jumperlessConfig.calibration.probe_switch_threshold_low);
                    updated = true;
                } else if (strcmp(key, "probe_switch_threshold") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_switch_threshold = %.2f;", jumperlessConfig.calibration.probe_switch_threshold);
                    updated = true;
                } else if (strcmp(key, "measure_mode_output_voltage") == 0) {
                    snprintf(newLine, sizeof(newLine), "measure_mode_output_voltage = %.2f;", jumperlessConfig.calibration.measure_mode_output_voltage);
                    updated = true;
                } else if (strcmp(key, "probe_current_zero") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_current_zero = %.2f;", jumperlessConfig.calibration.probe_current_zero);
                    updated = true;
                } else if (strcmp(key, "minimum_probe_reading") == 0) {
                    snprintf(newLine, sizeof(newLine), "minimum_probe_reading = %d;", jumperlessConfig.calibration.minimum_probe_reading);
                    updated = true;
                } else if (strcmp(key, "probe_droop_v0") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_droop_v0 = %.3f;", jumperlessConfig.calibration.probe_droop_v0);
                } else if (strcmp(key, "probe_droop_ohms") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_droop_ohms = %.1f;", jumperlessConfig.calibration.probe_droop_ohms);
                } else if (strcmp(key, "probe_pad_ohms") == 0) {
                    snprintf(newLine, sizeof(newLine), "probe_pad_ohms = %.1f;", jumperlessConfig.calibration.probe_pad_ohms);
                    updated = true;
                } else if (strcmp(key, "crosspoint_resistance") == 0) {
                    snprintf(newLine, sizeof(newLine), "crosspoint_resistance = %.2f;", jumperlessConfig.calibration.crosspoint_resistance);
                    updated = true;
                }
            }
            //! [logo_pads] section
            else if (strcmp(currentSection, "logo_pads") == 0) {
                if (strcmp(key, "top_guy") == 0) {
                    snprintf(newLine, sizeof(newLine), "top_guy = %d;", jumperlessConfig.logo_pads.top_guy);
                    updated = true;
                } else if (strcmp(key, "bottom_guy") == 0) {
                    snprintf(newLine, sizeof(newLine), "bottom_guy = %d;", jumperlessConfig.logo_pads.bottom_guy);
                    updated = true;
                } else if (strcmp(key, "building_pad_top") == 0) {
                    snprintf(newLine, sizeof(newLine), "building_pad_top = %d;", jumperlessConfig.logo_pads.building_pad_top);
                    updated = true;
                } else if (strcmp(key, "building_pad_bottom") == 0) {
                    snprintf(newLine, sizeof(newLine), "building_pad_bottom = %d;", jumperlessConfig.logo_pads.building_pad_bottom);
                    updated = true;
                } else if (strcmp(key, "repeat_ms") == 0) {
                    snprintf(newLine, sizeof(newLine), "repeat_ms = %d;", jumperlessConfig.logo_pads.repeat_ms);
                    updated = true;
                }
            }
            //! [display] section
            else if (strcmp(currentSection, "display") == 0) {
                if (strcmp(key, "lines_wires") == 0) {
                    snprintf(newLine, sizeof(newLine), "lines_wires = %d;", jumperlessConfig.display.lines_wires);
                    updated = true;
                } else if (strcmp(key, "menu_brightness") == 0) {
                    snprintf(newLine, sizeof(newLine), "menu_brightness = %d;", jumperlessConfig.display.menu_brightness);
                    updated = true;
                } else if (strcmp(key, "led_brightness") == 0) {
                    snprintf(newLine, sizeof(newLine), "led_brightness = %d;", jumperlessConfig.display.led_brightness);
                    updated = true;
                } else if (strcmp(key, "rail_brightness") == 0) {
                    snprintf(newLine, sizeof(newLine), "rail_brightness = %d;", jumperlessConfig.display.rail_brightness);
                    updated = true;
                } else if (strcmp(key, "special_net_brightness") == 0) {
                    snprintf(newLine, sizeof(newLine), "special_net_brightness = %d;", jumperlessConfig.display.special_net_brightness);
                    updated = true;
                } else if (strcmp(key, "net_color_mode") == 0) {
                    snprintf(newLine, sizeof(newLine), "net_color_mode = %d;", jumperlessConfig.display.net_color_mode);
                    updated = true;
                } else if (strcmp(key, "net_currents") == 0) {
                    snprintf(newLine, sizeof(newLine), "net_currents = %d;", jumperlessConfig.display.net_currents);
                    updated = true;
                } else if (strcmp(key, "current_flow") == 0) {
                    snprintf(newLine, sizeof(newLine), "current_flow = %d;", jumperlessConfig.display.current_flow);
                    updated = true;
                } else if (strcmp(key, "dump_leds") == 0) {
                    snprintf(newLine, sizeof(newLine), "dump_leds = %d;", jumperlessConfig.display.dump_leds);
                    updated = true;
                } else if (strcmp(key, "dump_format") == 0) {
                    snprintf(newLine, sizeof(newLine), "dump_format = %d;", jumperlessConfig.display.dump_format);
                    updated = true;
                } else if (strcmp(key, "terminal_line_buffering") == 0) {
                    snprintf(newLine, sizeof(newLine), "terminal_line_buffering = %d;", jumperlessConfig.display.terminal_line_buffering);
                    updated = true;
                }
            }
            //! [serial_1] section
            else if (strcmp(currentSection, "serial_1") == 0) {
                if (strcmp(key, "function") == 0) {
                    snprintf(newLine, sizeof(newLine), "function = %d;", jumperlessConfig.serial_1.function);
                    updated = true;
                } else if (strcmp(key, "baud_rate") == 0) {
                    snprintf(newLine, sizeof(newLine), "baud_rate = %d;", jumperlessConfig.serial_1.baud_rate);
                    updated = true;
                } else if (strcmp(key, "print_passthrough") == 0) {
                    snprintf(newLine, sizeof(newLine), "print_passthrough = %d;", jumperlessConfig.serial_1.print_passthrough);
                    updated = true;
                } else if (strcmp(key, "connect_on_boot") == 0) {
                    snprintf(newLine, sizeof(newLine), "connect_on_boot = %d;", jumperlessConfig.serial_1.connect_on_boot);
                    updated = true;
                } else if (strcmp(key, "lock_connection") == 0) {
                    snprintf(newLine, sizeof(newLine), "lock_connection = %d;", jumperlessConfig.serial_1.lock_connection);
                    updated = true;
                } else if (strcmp(key, "autoconnect_flashing") == 0) {
                    snprintf(newLine, sizeof(newLine), "autoconnect_flashing = %d;", jumperlessConfig.serial_1.autoconnect_flashing);
                    updated = true;
                } else if (strcmp(key, "async_passthrough") == 0) {
                    snprintf(newLine, sizeof(newLine), "async_passthrough = %d;", jumperlessConfig.serial_1.async_passthrough ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "tag_parsing") == 0) {
                    snprintf(newLine, sizeof(newLine), "tag_parsing = %d;", jumperlessConfig.serial_1.tag_parsing);
                    updated = true;
                } else if (strcmp(key, "flash_reset_type") == 0) {
                    snprintf(newLine, sizeof(newLine), "flash_reset_type = %s;", getStringFromTable(jumperlessConfig.serial_1.flash_reset_type, flashTypeTable));
                    updated = true;
                }
            }
            //! [serial_2] section
            else if (strcmp(currentSection, "serial_2") == 0) {
                if (strcmp(key, "function") == 0) {
                    snprintf(newLine, sizeof(newLine), "function = %d;", jumperlessConfig.serial_2.function);
                    updated = true;
                } else if (strcmp(key, "baud_rate") == 0) {
                    snprintf(newLine, sizeof(newLine), "baud_rate = %d;", jumperlessConfig.serial_2.baud_rate);
                    updated = true;
                } else if (strcmp(key, "print_passthrough") == 0) {
                    snprintf(newLine, sizeof(newLine), "print_passthrough = %d;", jumperlessConfig.serial_2.print_passthrough);
                    updated = true;
                } else if (strcmp(key, "connect_on_boot") == 0) {
                    snprintf(newLine, sizeof(newLine), "connect_on_boot = %d;", jumperlessConfig.serial_2.connect_on_boot);
                    updated = true;
                } else if (strcmp(key, "lock_connection") == 0) {
                    snprintf(newLine, sizeof(newLine), "lock_connection = %d;", jumperlessConfig.serial_2.lock_connection);
                    updated = true;
                } else if (strcmp(key, "autoconnect_flashing") == 0) {
                    snprintf(newLine, sizeof(newLine), "autoconnect_flashing = %d;", jumperlessConfig.serial_2.autoconnect_flashing);
                    updated = true;
                }
            }
            //! [top_oled] section
            else if (strcmp(currentSection, "top_oled") == 0) {
                if (strcmp(key, "enabled") == 0) {
                    snprintf(newLine, sizeof(newLine), "enabled = %d;", jumperlessConfig.top_oled.enabled);
                    updated = true;
                } else if (strcmp(key, "i2c_address") == 0) {
                    snprintf(newLine, sizeof(newLine), "i2c_address = %d;", jumperlessConfig.top_oled.i2c_address);
                    updated = true;
                } else if (strcmp(key, "width") == 0) {
                    snprintf(newLine, sizeof(newLine), "width = %d;", jumperlessConfig.top_oled.width);
                    updated = true;
                } else if (strcmp(key, "height") == 0) {
                    snprintf(newLine, sizeof(newLine), "height = %d;", jumperlessConfig.top_oled.height);
                    updated = true;
                } else if (strcmp(key, "connection_type") == 0) {
                    snprintf(newLine, sizeof(newLine), "connection_type = %s;", getConnectionTypeString(jumperlessConfig.top_oled.connection_type));
                    updated = true;
                } else if (strcmp(key, "sda_pin") == 0) {
                    snprintf(newLine, sizeof(newLine), "sda_pin = %d;", jumperlessConfig.top_oled.sda_pin);
                    updated = true;
                } else if (strcmp(key, "scl_pin") == 0) {
                    snprintf(newLine, sizeof(newLine), "scl_pin = %d;", jumperlessConfig.top_oled.scl_pin);
                    updated = true;
                } else if (strcmp(key, "gpio_sda") == 0) {
                    snprintf(newLine, sizeof(newLine), "gpio_sda = %d;", jumperlessConfig.top_oled.gpio_sda);
                    updated = true;
                } else if (strcmp(key, "gpio_scl") == 0) {
                    snprintf(newLine, sizeof(newLine), "gpio_scl = %d;", jumperlessConfig.top_oled.gpio_scl);
                    updated = true;
                } else if (strcmp(key, "sda_row") == 0) {
                    snprintf(newLine, sizeof(newLine), "sda_row = %d;", jumperlessConfig.top_oled.sda_row);
                    updated = true;
                } else if (strcmp(key, "scl_row") == 0) {
                    snprintf(newLine, sizeof(newLine), "scl_row = %d;", jumperlessConfig.top_oled.scl_row);
                    updated = true;
                } else if (strcmp(key, "connect_on_boot") == 0) {
                    snprintf(newLine, sizeof(newLine), "connect_on_boot = %d;", jumperlessConfig.top_oled.connect_on_boot ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "lock_connection") == 0) {
                    snprintf(newLine, sizeof(newLine), "lock_connection = %d;", jumperlessConfig.top_oled.lock_connection ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "show_in_terminal") == 0) {
                    snprintf(newLine, sizeof(newLine), "show_in_terminal = %d;", jumperlessConfig.top_oled.show_in_terminal ? 1 : 0);
                    updated = true;
                } else if (strcmp(key, "font") == 0) {
                    snprintf(newLine, sizeof(newLine), "font = %d;", jumperlessConfig.top_oled.font);
                    updated = true;
                } else if (strcmp(key, "startup_message") == 0) {
                    snprintf(newLine, sizeof(newLine), "startup_message = %s", jumperlessConfig.top_oled.startup_message);
                    updated = true;
                }
            }
            //! [usb_cdc] section
            else if (strcmp(currentSection, "usb_cdc") == 0) {
                if (strcmp(key, "ignore_dtr") == 0) {
                    snprintf(newLine, sizeof(newLine), "ignore_dtr = %d;", jumperlessConfig.usb_cdc.ignore_dtr ? 1 : 0);
                    updated = true;
                }
            }
            
            if (updated) {
                int written = snprintf(dstPos, dstEnd - dstPos, "%s\n", newLine);
                dstPos += written;
            } else {
                // Unknown key, keep original line
                int written = snprintf(dstPos, dstEnd - dstPos, "%s\n", line);
                dstPos += written;
            }
        } else {
            // Comment, empty line, or other - copy as-is
            int written = snprintf(dstPos, dstEnd - dstPos, "%s\n", line);
            dstPos += written;
        }
    }
    
    *dstPos = '\0';
    size_t writeSize = dstPos - newContent;
    
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave] Parse & rebuild: ");
        Serial.print(micros() - stepTime);
        Serial.println(" us");
    }
    
    // Check if content actually changed before writing
    // This avoids flash writes when the rebuilt content is identical
    bool contentChanged = (writeSize != bytesRead) || (memcmp(fileContent, newContent, writeSize) != 0);
    
    if (!contentChanged) {
        if (debugConfigSaveTiming) {
            Serial.println("[ConfigSave] Content unchanged after rebuild, skipping write");
        }
        free(fileContent);
        free(newContent);
        updateShadowConfig();
        return true;
    }
    
    // Write the updated content using r+ mode (overwrite in place, no truncate)
    // This is MUCH faster than "w" mode which reallocates clusters
    stepTime = micros();
    uint32_t opTime;
    
    // CRITICAL: Ensure Core 2 is actually paused before writing to flash
    // Increase timeout to 1000ms to allow long operations (like LED updates) to finish
    // This prevents the XIP crash when leaving probe mode
    bool was_paused = pauseCore2ForFlash(1000);
    
    // Suspend UART IRQ before flash ops to prevent starvation of USB stack
    AsyncPassthrough::suspendUARTRxIRQ();
    
    extern volatile bool core2busy; // Check if it actually stopped
    if (core2busy) {
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] Core 2 still busy after timeout! Aborting save to prevent crash.");
        else Serial.println("[ConfigSave] Aborted: Core 2 busy");
        unpauseCore2ForFlash(was_paused);
        AsyncPassthrough::resumeUARTRxIRQ();
        free(fileContent);
        free(newContent);
        return false; // Retry later
    }
    
    // Use "r+" mode to overwrite in place - avoids cluster reallocation
    // This is significantly faster than truncate + write
    opTime = micros();
    File file = safeFileOpen(filename, "r+", 1000);
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave]   safeFileOpen(r+): ");
        Serial.print(micros() - opTime);
        Serial.println(" us");
    }
    if (!file) {
        // Fall back to "w" mode if r+ fails
        if (debugConfigSaveTiming) Serial.println("[ConfigSave]   r+ failed, trying w mode");
        file = safeFileOpen(filename, "w", 1000);
        if (!file) {
            Serial.println("saveConfigIncremental: failed to open file for writing");
            unpauseCore2ForFlash(was_paused);
            AsyncPassthrough::resumeUARTRxIRQ();
            free(fileContent);
            free(newContent);
            return false;
        }
    }
    
    // Seek to beginning and overwrite
    file.seek(0);
    
    // Write content directly to file handle
    opTime = micros();
    size_t written = file.write((const uint8_t*)newContent, writeSize);
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave]   file.write: ");
        Serial.print(micros() - opTime);
        Serial.println(" us");
    }
    
    // If new content is shorter, clear remaining bytes to prevent garbage data
    // CRITICAL: This prevents config corruption when values shrink (e.g., long font name → short)
    // Writing spaces makes the file human-readable if opened mid-write
    if (writeSize < bytesRead) {
        size_t remainingBytes = bytesRead - writeSize;
        // Write spaces to clear old data - much faster than "w" mode truncate
        static const char spaces[64] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        while (remainingBytes > 0) {
            size_t chunkSize = (remainingBytes > sizeof(spaces)) ? sizeof(spaces) : remainingBytes;
            file.write((const uint8_t*)spaces, chunkSize);
            remainingBytes -= chunkSize;
        }
        if (debugConfigSaveTiming) {
            Serial.print("[ConfigSave]   cleared ");
            Serial.print(bytesRead - writeSize);
            Serial.println(" trailing bytes with spaces");
        }
    }
    
    // Skip explicit flush() - it does BOTH f_sync AND _fs->sync (full filesystem sync)
    // FatFS f_close() internally calls f_sync which is sufficient for the file data
    // This avoids the expensive _fs->sync() full filesystem sync
    
    opTime = micros();
    safeFileClose(file, true);  // true = write mode, will pause Core2 during close
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave]   safeFileClose: ");
        Serial.print(micros() - opTime);
        Serial.println(" us");
    }
    
    unpauseCore2ForFlash(was_paused);
    AsyncPassthrough::resumeUARTRxIRQ();
    
    bool success = (written == writeSize);
    
    if (debugConfigSaveTiming) {
        Serial.print("[ConfigSave] File write total (");
        Serial.print(writeSize);
        Serial.print(" bytes): ");
        Serial.print(micros() - stepTime);
        Serial.println(" us");
    }
    
    free(fileContent);
    free(newContent);
    
    if (success) {
        updateShadowConfig();
        if (debugConfigSaveTiming) {
            Serial.print("[ConfigSave] SUCCESS - Total: ");
            Serial.print(micros() - totalStartTime);
            Serial.println(" us");
        }
    } else {
        Serial.println("saveConfigIncremental: write failed");
        return false;
    }
    return true; 
}

bool saveConfig(void) {
    if (jumperlessConfig.calibration.probe_min == 0 || jumperlessConfig.calibration.probe_max == 0) {
        jumperlessConfig.calibration.probe_min = 15;
        jumperlessConfig.calibration.probe_max = 4040;
    }

    // Use optimized incremental save - only writes if config has changed
    // Falls back to full save if file doesn't exist or incremental fails
    bool success = saveConfigIncremental("/config.txt");

    if (success) {
        // Force the save all the way to flash NOW. FatFS runs SPIFTL in
        // lazy-persist mode, so the write above only programmed the data
        // sector - the L2P metadata that makes it reachable after reboot is
        // still in RAM. Without forcing it here, config changes (calibration,
        // settings) silently revert on the next power cycle. A brief UI pause
        // is acceptable for an explicit config save.
        //   1) drain any dirty PSRAM-cache entry for the file (PSRAM units), then
        //   2) force the SPIFTL metadata persist (force=true so it fires even
        //      for direct/cache-bypassing writes, e.g. on no-PSRAM units).
        // forceSync() self-gates on SPIFTL's metadataAge, so this is a cheap
        // no-op when nothing was actually written.
        fileCacheFlushNow("/config.txt");
        fileCacheSpiftlSync("saveConfig", true);

        // Keep the durable EEPROM store (FS-wipe survivor: calibration + identity)
        // in sync with config.txt. eepromReconcileAfterConfig() lets the EEPROM
        // store WIN over config.txt on boot, so a calibration change written only
        // to config.txt - which is exactly what the common saveConfigIncremental()
        // path does - silently reverts to the stale EEPROM value on the next
        // reboot. Mirror the kept fields and commit them here so the explicit save
        // is fully durable. Both calls self-gate: eepromPersistFromConfig() only
        // flags a commit when a kept field actually changed, and eepromCommitSafe()
        // no-ops when nothing is pending, so ordinary (non-calibration) saves pay
        // nothing extra. A calibration save trades a brief Core-2 pause for
        // correctness, which is acceptable for an explicit config save.
        eepromPersistFromConfig();
        eepromCommitSafe();

        readSettingsFromConfig();
    }
    return success;
}

// Firmware versioning and file provisioning system
// ================================================

/**
 * Helper function to write embedded binary data to filesystem
 * Returns true on success, false on failure
 */
bool provisionEmbeddedFile(const char* filename, const unsigned char* data, unsigned int dataLen) {
    // Check if file already exists using safe function
    if (safeFileExists(filename, 500)) {
        return true; // Already provisioned
    }
    
    // CRITICAL: Pause Core2 during flash write operations
    bool was_paused = pauseCore2ForFlash(100);
    
    // Suspend UART IRQ before flash ops
    AsyncPassthrough::suspendUARTRxIRQ();
    
    // Write file using safe function
    File file = safeFileOpen(filename, "w", 2000);
    if (!file) {
        Serial.print("Failed to create file: ");
        Serial.println(filename);
        unpauseCore2ForFlash(was_paused);
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }
    
    // Write data from PROGMEM
    uint8_t buffer[550];
    unsigned int bytesWritten = 0;
    while (bytesWritten < dataLen) {
        unsigned int chunkSize = min(sizeof(buffer), dataLen - bytesWritten);
        memcpy_P(buffer, data + bytesWritten, chunkSize);
        size_t written = file.write(buffer, chunkSize);
        if (written != chunkSize) {
            Serial.print("Write error for: ");
            Serial.println(filename);
            safeFileClose(file, true);
            unpauseCore2ForFlash(was_paused);
            AsyncPassthrough::resumeUARTRxIRQ();
            return false;
        }
        bytesWritten += written;
    }
    
    file.flush();
    safeFileClose(file, true);  // Write mode, needs flush
    unpauseCore2ForFlash(was_paused);
    AsyncPassthrough::resumeUARTRxIRQ();
    changeTerminalColor( 163, true );
    Serial.print("Provisioned: ");
    Serial.println(filename);
    Serial.flush();
    changeTerminalColor( -1, true );
    return true;
}

/**
 * Provision embedded image files to filesystem
 * This is called on first boot or firmware update
 */
void provisionFirmwareFiles(bool print) {
#if defined(OG_JUMPERLESS)
    // These are all images/*.bin OLED/breadboard-LED display assets. The OG has
    // no OLED and 1 LED per row, so they're unused - and writing them at boot
    // (each file open make_shared's a ~4KB FatFS FIL) aborts on the OG's tight,
    // fragmented heap. Skip entirely; mark provisioned so we don't retry.
    jumperlessConfig.firmware.files_provisioned = true;
    return;
#endif

   if (print) {
    changeTerminalColor( 162, true );
    Serial.println("\n\r╔═══════════════════════════════════════╗");
    Serial.println("║  Provisioning Firmware Files          ║");
    Serial.println("╚═══════════════════════════════════════╝\n\r");  
    Serial.flush();
   }
    // Provision image files
    provisionEmbeddedFile("images/bubbleJumpThin.bin", bubbleJumpThin_bin, bubbleJumpThin_bin_len);
    provisionEmbeddedFile("images/bubbleJump.bin", bubbleJump_bin, bubbleJump_bin_len);
    provisionEmbeddedFile("images/jogo32h.bin", jogo32h_file_bin, jogo32h_file_bin_len);
    provisionEmbeddedFile("images/bubbleJumpThiccWhite.bin", bubbleJumpThiccWhite_bin, bubbleJumpThiccWhite_bin_len);
    provisionEmbeddedFile("images/excelGUI.bin", excelGUI_bin, excelGUI_bin_len);
    // ---- Paste into provisionFirmwareFiles() in src/configManager.cpp ----

    provisionEmbeddedFile("images/dayglow.bin", dayglow_bin, dayglow_bin_len);
    provisionEmbeddedFile("images/eevblog.bin", eevblog_bin, eevblog_bin_len);
    provisionEmbeddedFile("images/jogo32h.bin", jogo32h_bin, jogo32h_bin_len);
    provisionEmbeddedFile("images/jogotextInv.bin", jogotextInv_bin, jogotextInv_bin_len);
    provisionEmbeddedFile("images/jumperless_text.bin", jumperless_text_bin, jumperless_text_bin_len);
    provisionEmbeddedFile("images/ksc.bin", ksc_bin, ksc_bin_len);
    
    // Mark as provisioned
    jumperlessConfig.firmware.files_provisioned = true;
    
    if (debugFP) {
    Serial.println("\n\rFile provisioning complete!\n\r");
    }
}

/**
 * Compare two version strings (format: "X.Y.Z.W")
 * Returns: -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2
 */
int compareVersions(const char* v1, const char* v2) {
    int v1_parts[4] = {0, 0, 0, 0};
    int v2_parts[4] = {0, 0, 0, 0};
    
    // Parse v1
    sscanf(v1, "%d.%d.%d.%d", &v1_parts[0], &v1_parts[1], &v1_parts[2], &v1_parts[3]);
    
    // Parse v2
    sscanf(v2, "%d.%d.%d.%d", &v2_parts[0], &v2_parts[1], &v2_parts[2], &v2_parts[3]);
    
    // Compare each part
    for (int i = 0; i < 4; i++) {
        if (v1_parts[i] < v2_parts[i]) return -1;
        if (v1_parts[i] > v2_parts[i]) return 1;
    }
    
    return 0; // Equal
}

/**
 * Perform one-time config migrations for this firmware version
 * Only changes config values if they haven't been modified from defaults
 */
void performConfigMigrations(const char* oldVersion, const char* newVersion) {
    if (debugFP) {
    Serial.print("Migrating config from ");
    Serial.print(oldVersion);
    Serial.print(" to ");
        Serial.println(newVersion);
    }
    
    // Example: Set default startup image to bubbleJumpThin.bin if startup_message is empty
    if (strlen(jumperlessConfig.top_oled.startup_message) == 0) {
        strncpy(jumperlessConfig.top_oled.startup_message, "images/bubbleJumpThin.bin", 
                sizeof(jumperlessConfig.top_oled.startup_message) - 1);
        jumperlessConfig.top_oled.startup_message[sizeof(jumperlessConfig.top_oled.startup_message) - 1] = '\0';
        Serial.println("  - Set default startup image to images/bubbleJumpThin.bin");
    }
    
    // Refresh built-in examples on every firmware update.
    // The hash system in initializeMicroPythonExamples protects user-edited files:
    // unmodified defaults are updated in-place; user-modified files are left alone
    // and a new firmware default is written as _original / _original1 / etc.
    if (debugFP) {
        Serial.println("  - Refreshing built-in MicroPython examples (preserving user edits)...");
    }
    initializeMicroPythonExamples(true);
    if (debugFP) {
        Serial.println("  ✓ Python examples refreshed\n\r");
    }

    // Add one-time migrations here as needed for specific version transitions.
    // Example:
    // if (compareVersions(oldVersion, "5.5.0.4") <= 0) {
    //     // Migration for versions <= 5.5.0.4
    // }
}

/**
 * Check if firmware was updated and handle provisioning
 * Should be called during startup after config is loaded
 * Returns true if firmware was updated
 */
bool checkAndHandleFirmwareUpdate(void) {
    const char* currentVersion = firmwareVersion;
    const char* lastVersion = jumperlessConfig.firmware.last_version;
    
    // First boot (no version stored) or firmware was updated
    bool isFirstBoot = (strlen(lastVersion) == 0);
    bool wasUpdated = !isFirstBoot && (strcmp(lastVersion, currentVersion) != 0);
    // delay(1000);
    // changeTerminalColor( 166, true );
    // Serial.print("Current version: ");
    // Serial.println(currentVersion);
    // Serial.print("Last version: ");
    // Serial.println(lastVersion);
    // Serial.flush();
    
    if (isFirstBoot) {
        // if (debugFP) {
        changeTerminalColor( 164, true );
        Serial.println("\n\r╔═══════════════════════════════════════╗");
        Serial.println("║  First Boot Detected                  ║");
        Serial.println("╚═══════════════════════════════════════╝\n\r");
        // }
        Serial.flush();
        Serial.print("Previous version: ");
        Serial.println(lastVersion);
        Serial.print("Current version:  ");
        Serial.println(currentVersion);
        Serial.flush();
        changeTerminalColor( -1, true );
        // Provision files
        provisionFirmwareFiles(true);
        
        // Set default config values for first boot
        if (strlen(jumperlessConfig.top_oled.startup_message) == 0) {
            strncpy(jumperlessConfig.top_oled.startup_message, "images/bubbleJumpThin.bin", 
                    sizeof(jumperlessConfig.top_oled.startup_message) - 1);
            jumperlessConfig.top_oled.startup_message[sizeof(jumperlessConfig.top_oled.startup_message) - 1] = '\0';
        }
        
    } else if (wasUpdated) {
        // if (debugFP) {
        changeTerminalColor( 164, true );
        Serial.println("\n\r╔═══════════════════════════════════════╗");
        Serial.println("║  Firmware Update Detected             ║");
        Serial.println("╚═══════════════════════════════════════╝\n\r");
        Serial.print("Previous version: ");
        Serial.println(lastVersion);
        Serial.print("Current version:  ");
        Serial.println(currentVersion);
        Serial.println();
        Serial.flush();
        changeTerminalColor( -1, true );
        // Provision new files (will skip existing ones)
        provisionFirmwareFiles(false);
        
        // Perform config migrations (respects user changes)
        performConfigMigrations(lastVersion, currentVersion);
    }
    
    // Update stored version if changed
    if (isFirstBoot || wasUpdated) {
        strncpy(jumperlessConfig.firmware.last_version, currentVersion, 
                sizeof(jumperlessConfig.firmware.last_version) - 1);
        jumperlessConfig.firmware.last_version[sizeof(jumperlessConfig.firmware.last_version) - 1] = '\0';
        
        // Mirror the new last_version into the durable EEPROM store so update
        // sensing survives an FS wipe (deferred commit, coalesced with the
        // config save below).
        eepromPersistFromConfig();

        // Save config with new version
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] TRIGGER: checkAndHandleFirmwareUpdate");
        saveConfig();
        if (debugFP) {
        Serial.println("\n\rFirmware version updated in config.\n\r");
        }
    }
    
    return wasUpdated || isFirstBoot;
}

/**
 * Load hardware revision from EEPROM into config
 * This ensures hardware revision survives config resets and first boots
 * Should be called BEFORE loadConfig() to set hardware defaults
 */
void loadHardwareFromEEPROM(void) {
    // Load the packed EEPROM store (magic+version tagged) and apply hardware
    // identity to jumperlessConfig BEFORE loadConfig() runs. The full
    // identity+calibration reconciliation (EEPROM wins for the kept fields, so
    // they survive an FS wipe) happens in eepromReconcileAfterConfig() AFTER
    // loadConfig(). See PersistentStuff.cpp for the store layout + migration.
    eepromStoreLoadAndApplyIdentity();
}

void loadConfig(void) {
    updateConfigFromFile("/config.txt");

    if (jumperlessConfig.calibration.probe_min == 0 || jumperlessConfig.calibration.probe_max == 0) {
        jumperlessConfig.calibration.probe_min = 15;
        jumperlessConfig.calibration.probe_max = 4060;
    }

    // Seed the MEASURE-position pad endpoints from the base (select) pair so
    // the config holds real, individually-adjustable numbers instead of a 0
    // sentinel. They are stored in the 3.3V frame; the decode scales them by
    // the live tip voltage (ADC7) at runtime, so seeding with the base pair
    // is correct for a fresh board.
    if (jumperlessConfig.calibration.probe_min_measure <= 0) {
        jumperlessConfig.calibration.probe_min_measure = jumperlessConfig.calibration.probe_min;
    }
    if (jumperlessConfig.calibration.probe_max_measure <= 0) {
        jumperlessConfig.calibration.probe_max_measure = jumperlessConfig.calibration.probe_max;
    }

    // GPIO droop V0: persisted unloaded tip voltage for switch detection.
    if (jumperlessConfig.calibration.probe_droop_v0 < 3.0f ||
        jumperlessConfig.calibration.probe_droop_v0 > 3.6f) {
        jumperlessConfig.calibration.probe_droop_v0 = 3.35f;
    }

    readSettingsFromConfig();
    
    // Initialize shadow config for dirty tracking
    // This allows saveConfig() to skip writes when nothing has changed
    updateShadowConfig();
    
    // Apply USB CDC configuration (DTR ignore mode)
    // This must be called after config is loaded to apply the ignore_dtr setting
    usb_cdc_apply_config();
    
    // Defer initChipStatus to reduce startup time - it can be done later
    // initChipStatus();
}

//! This is where we add new config SECTIONS
int parseSectionName(const char* sectionName) {
    if (strcmp(sectionName, "config") == 0) return -2; // Special case for config section
    else if (strcmp(sectionName, "firmware") == 0) return -3; // Firmware tracking section
    else if (strcmp(sectionName, "hardware") == 0) return 0;
    else if (strcmp(sectionName, "dacs") == 0) return 1;
    else if (strcmp(sectionName, "debug") == 0) return 2;
    else if (strcmp(sectionName, "routing") == 0) return 3;
    else if (strcmp(sectionName, "calibration") == 0) return 4;
    else if (strcmp(sectionName, "logo_pads") == 0) return 5;
    else if (strcmp(sectionName, "display") == 0) return 6;
    else if (strcmp(sectionName, "serial_1") == 0) return 7;
    else if (strcmp(sectionName, "serial_2") == 0) return 8;
    else if (strcmp(sectionName, "top_oled") == 0) return 9;
    else if (strcmp(sectionName, "usb_cdc") == 0) return 11;
    return -1;
}

void printConfigSectionToSerial(int section, bool showNames, bool pasteable) {
    // If section is -1, try to parse input
    if (showNames) {
        showNames = 1;
    }
    else {
        showNames = 0;
    }
 //! this is a place to add new config options
    if (pasteable == true) {
        Serial.println("\n\rcopy / edit / paste any of these lines \n\rinto the main menu to change a setting\n\r");
    }
    if (section == -1) {
        Serial.println("Jumperless Config:\n\r");
    }
    cycleTerminalColor(true, (highSaturationBrightColorsCount/8.0), true, &Serial, 0, 1);
    // Print config metadata section
    if (section == -1 || section == -2) {
        Serial.print("\n`[config] ");
        if (pasteable == false) Serial.println();
        Serial.print("firmware_version = "); Serial.print(firmwareVersion); Serial.println(";");
    }
    
    // Print hardware version section
    if (section == -1 || section == 0) {
        Serial.print("\n`[hardware] ");
        if (pasteable == false) Serial.println();
        Serial.print("generation = "); Serial.print(jumperlessConfig.hardware.generation); Serial.println(";");
        if (pasteable == true) Serial.print("`[hardware] ");
        Serial.print("revision = "); Serial.print(jumperlessConfig.hardware.revision); Serial.println(";");
        if (pasteable == true) Serial.print("`[hardware] ");
        Serial.print("probe_revision = "); Serial.print(jumperlessConfig.hardware.probe_revision); Serial.println(";");
        if (pasteable == true) Serial.print("`[hardware] ");
        Serial.print("psram_installed = "); Serial.print(jumperlessConfig.hardware.psram_installed); Serial.println(";");
        if (pasteable == true) Serial.print("`[hardware] ");
        Serial.print("psram_app_size_kb = "); Serial.print(jumperlessConfig.hardware.psram_app_size_kb); Serial.println(";");
        if (pasteable == true) Serial.print("`[hardware] ");
        Serial.print("probe_led_on_button_pin = "); Serial.print(getStringFromTable(jumperlessConfig.hardware.probe_led_on_button_pin, boolTable)); Serial.println(";");
    }
    cycleTerminalColor();
    // Print DAC settings section
    if (section == -1 || section == 1) {
        Serial.print("\n`[dacs] ");
        if (pasteable == false) Serial.println();
        // Voltage state (top_rail, bottom_rail, dac_0, dac_1) moved to globalState.power
        Serial.print("set_dacs_on_boot = "); Serial.print(getStringFromTable(jumperlessConfig.dacs.set_dacs_on_boot, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[dacs] ");
        Serial.print("set_rails_on_boot = "); Serial.print(getStringFromTable(jumperlessConfig.dacs.set_rails_on_boot, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[dacs] ");
        Serial.print("probe_power_dac = "); Serial.print(jumperlessConfig.dacs.probe_power_dac); Serial.println(";");
        if (pasteable == true) Serial.print("`[dacs] ");
        Serial.print("auto_connect_probe = "); Serial.print(jumperlessConfig.dacs.auto_connect_probe); Serial.println(";");
        if (pasteable == true) Serial.print("`[dacs] ");
        Serial.print("limit_max = "); Serial.print(jumperlessConfig.dacs.limit_max); Serial.println(";");
        if (pasteable == true) Serial.print("`[dacs] ");
        Serial.print("limit_min = "); Serial.print(jumperlessConfig.dacs.limit_min); Serial.println(";");
    }
    cycleTerminalColor();
    // Print debug flags section
    if (section == -1 || section == 2) {
        Serial.print("\n`[debug] ");
        if (pasteable == false) Serial.println();
        Serial.print("file_parsing = "); Serial.print(getStringFromTable(jumperlessConfig.debug.file_parsing, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("net_manager = "); Serial.print(getStringFromTable(jumperlessConfig.debug.net_manager, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("nets_to_chips = "); Serial.print(getStringFromTable(jumperlessConfig.debug.nets_to_chips, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("nets_to_chips_alt = "); Serial.print(getStringFromTable(jumperlessConfig.debug.nets_to_chips_alt, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("leds = "); Serial.print(getStringFromTable(jumperlessConfig.debug.leds, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("probing = "); Serial.print(getStringFromTable(jumperlessConfig.debug.probing, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("oled = "); Serial.print(getStringFromTable(jumperlessConfig.debug.oled, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("logo_pads = "); Serial.print(getStringFromTable(jumperlessConfig.debug.logo_pads, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("logic_analyzer = "); Serial.print(getStringFromTable(jumperlessConfig.debug.logic_analyzer, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("arduino = "); Serial.print(jumperlessConfig.debug.arduino); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("usb_mass_storage = "); Serial.print(getStringFromTable(jumperlessConfig.debug.usb_mass_storage, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("show_probe_current = "); Serial.print(jumperlessConfig.debug.show_probe_current); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("show_node_errors = "); Serial.print(getStringFromTable(jumperlessConfig.debug.show_node_errors, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("probe_power_gpio = "); Serial.print(getStringFromTable(jumperlessConfig.debug.probe_power_gpio, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("probe_switch_stats = "); Serial.print(getStringFromTable(jumperlessConfig.debug.probe_switch_stats, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[debug] ");
        Serial.print("net_voltage_scan = "); Serial.print(getStringFromTable(jumperlessConfig.debug.net_voltage_scan, boolTable)); Serial.println(";");
    }
    cycleTerminalColor();
    // Print routing settings section
    if (section == -1 || section == 3) {
        Serial.print("\n`[routing] ");
        if (pasteable == false) Serial.println();
        Serial.print("stack_paths = "); Serial.print(jumperlessConfig.routing.stack_paths); Serial.println(";");
        if (pasteable == true) Serial.print("`[routing] ");
        Serial.print("stack_rails = "); Serial.print(jumperlessConfig.routing.stack_rails); Serial.println(";");
        if (pasteable == true) Serial.print("`[routing] ");
        Serial.print("stack_dacs = "); Serial.print(jumperlessConfig.routing.stack_dacs); Serial.println(";");
        if (pasteable == true) Serial.print("`[routing] ");
        Serial.print("rail_priority = "); Serial.print(jumperlessConfig.routing.rail_priority); Serial.println(";");
    }
    cycleTerminalColor();
    // Print calibration section
    if (section == -1 || section == 4) {
        Serial.print("\n`[calibration] ");
        if (pasteable == false) Serial.println();
        Serial.print("top_rail_zero = "); Serial.print(jumperlessConfig.calibration.top_rail_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("top_rail_spread = "); Serial.print(jumperlessConfig.calibration.top_rail_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("bottom_rail_zero = "); Serial.print(jumperlessConfig.calibration.bottom_rail_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("bottom_rail_spread = "); Serial.print(jumperlessConfig.calibration.bottom_rail_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("dac_0_zero = "); Serial.print(jumperlessConfig.calibration.dac_0_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("dac_0_spread = "); Serial.print(jumperlessConfig.calibration.dac_0_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("dac_1_zero = "); Serial.print(jumperlessConfig.calibration.dac_1_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("dac_1_spread = "); Serial.print(jumperlessConfig.calibration.dac_1_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_0_zero = "); Serial.print(jumperlessConfig.calibration.adc_0_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_0_spread = "); Serial.print(jumperlessConfig.calibration.adc_0_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_1_zero = "); Serial.print(jumperlessConfig.calibration.adc_1_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_1_spread = "); Serial.print(jumperlessConfig.calibration.adc_1_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_2_zero = "); Serial.print(jumperlessConfig.calibration.adc_2_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_2_spread = "); Serial.print(jumperlessConfig.calibration.adc_2_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_3_zero = "); Serial.print(jumperlessConfig.calibration.adc_3_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_3_spread = "); Serial.print(jumperlessConfig.calibration.adc_3_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_4_zero = "); Serial.print(jumperlessConfig.calibration.adc_4_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_4_spread = "); Serial.print(jumperlessConfig.calibration.adc_4_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_7_zero = "); Serial.print(jumperlessConfig.calibration.adc_7_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("adc_7_spread = "); Serial.print(jumperlessConfig.calibration.adc_7_spread); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_max = "); Serial.print(jumperlessConfig.calibration.probe_max); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_min = "); Serial.print(jumperlessConfig.calibration.probe_min); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_max_measure = "); Serial.print(jumperlessConfig.calibration.probe_max_measure); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_min_measure = "); Serial.print(jumperlessConfig.calibration.probe_min_measure); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_switch_threshold_high = "); Serial.print(jumperlessConfig.calibration.probe_switch_threshold_high); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_switch_threshold_low = "); Serial.print(jumperlessConfig.calibration.probe_switch_threshold_low); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_switch_threshold = "); Serial.print(jumperlessConfig.calibration.probe_switch_threshold); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("measure_mode_output_voltage = "); Serial.print(jumperlessConfig.calibration.measure_mode_output_voltage); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_current_zero = "); Serial.print(jumperlessConfig.calibration.probe_current_zero); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("minimum_probe_reading = "); Serial.print(jumperlessConfig.calibration.minimum_probe_reading); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("probe_droop_v0 = "); Serial.print(jumperlessConfig.calibration.probe_droop_v0, 3); Serial.println(";");
        Serial.print("probe_droop_ohms = "); Serial.print(jumperlessConfig.calibration.probe_droop_ohms, 1); Serial.println(";");
        Serial.print("probe_pad_ohms = "); Serial.print(jumperlessConfig.calibration.probe_pad_ohms, 1); Serial.println(";");
        if (pasteable == true) Serial.print("`[calibration] ");
        Serial.print("crosspoint_resistance = "); Serial.print(jumperlessConfig.calibration.crosspoint_resistance); Serial.println(";");
    }
    cycleTerminalColor();
    // Print logo pad settings section
    if (section == -1 || section == 5) {
        Serial.print("\n`[logo_pads] ");
        if (pasteable == false) Serial.println();   
        Serial.print("top_guy = "); Serial.print(getStringFromTable(jumperlessConfig.logo_pads.top_guy, arbitraryFunctionTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[logo_pads] ");
        Serial.print("bottom_guy = "); Serial.print(getStringFromTable(jumperlessConfig.logo_pads.bottom_guy, arbitraryFunctionTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[logo_pads] ");
        Serial.print("building_pad_top = "); Serial.print(getStringFromTable(jumperlessConfig.logo_pads.building_pad_top, arbitraryFunctionTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[logo_pads] ");
        Serial.print("building_pad_bottom = "); Serial.print(getStringFromTable(jumperlessConfig.logo_pads.building_pad_bottom, arbitraryFunctionTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[logo_pads] ");
        Serial.print("repeat_ms = "); Serial.print(jumperlessConfig.logo_pads.repeat_ms); Serial.println(";");
    }
    cycleTerminalColor();
    // Print display settings section
    if (section == -1 || section == 6) {
        Serial.print("\n`[display] ");
        if (pasteable == false) Serial.println();
        Serial.print("lines_wires = "); Serial.print(getStringFromTable(jumperlessConfig.display.lines_wires, linesWiresTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("menu_brightness = "); Serial.print(jumperlessConfig.display.menu_brightness); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("led_brightness = "); Serial.print(jumperlessConfig.display.led_brightness); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("rail_brightness = "); Serial.print(jumperlessConfig.display.rail_brightness); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("special_net_brightness = "); Serial.print(jumperlessConfig.display.special_net_brightness); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("net_color_mode = "); Serial.print(getStringFromTable(jumperlessConfig.display.net_color_mode, netColorModeTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("net_currents = "); Serial.print(getStringFromTable(jumperlessConfig.display.net_currents, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("current_flow = "); Serial.print(getStringFromTable(jumperlessConfig.display.current_flow, currentFlowTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("dump_leds = "); Serial.print(getStringFromTable(jumperlessConfig.display.dump_leds, serialPortTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("dump_format = "); Serial.print(getStringFromTable(jumperlessConfig.display.dump_format, dumpFormatTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[display] ");
        Serial.print("terminal_line_buffering = "); Serial.print(jumperlessConfig.display.terminal_line_buffering); Serial.println(";");
    }
    cycleTerminalColor();
    // Print serial_1 section
    if (section == -1 || section == 7) {
        Serial.print("\n`[serial_1] ");
        if (pasteable == false) Serial.println();
        Serial.print("function = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.function, uartFunctionTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("baud_rate = "); Serial.print(jumperlessConfig.serial_1.baud_rate); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("print_passthrough = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.print_passthrough, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("connect_on_boot = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.connect_on_boot, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("lock_connection = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.lock_connection, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("autoconnect_flashing = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.autoconnect_flashing, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("async_passthrough = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.async_passthrough, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("tag_parsing = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.tag_parsing, tagParsingTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_1] ");
        Serial.print("flash_reset_type = "); Serial.print(getStringFromTable(jumperlessConfig.serial_1.flash_reset_type, flashTypeTable)); Serial.println(";");
    }
    cycleTerminalColor();
    // Print serial_2 section
    if (section == -1 || section == 8) {
        Serial.print("\n`[serial_2] ");
        if (pasteable == false) Serial.println();
        Serial.print("function = "); Serial.print(getStringFromTable(jumperlessConfig.serial_2.function, uartFunctionTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_2] ");
        Serial.print("baud_rate = "); Serial.print(jumperlessConfig.serial_2.baud_rate); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_2] ");
        Serial.print("print_passthrough = "); Serial.print(getStringFromTable(jumperlessConfig.serial_2.print_passthrough, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_2] ");
        Serial.print("connect_on_boot = "); Serial.print(getStringFromTable(jumperlessConfig.serial_2.connect_on_boot, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_2] ");
        Serial.print("lock_connection = "); Serial.print(getStringFromTable(jumperlessConfig.serial_2.lock_connection, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[serial_2] ");
        Serial.print("autoconnect_flashing = "); Serial.print(getStringFromTable(jumperlessConfig.serial_2.autoconnect_flashing, boolTable)); Serial.println(";");
    }
    cycleTerminalColor();
    // Print top_oled section
    if (section == -1 || section == 10) {
        Serial.print("\n`[top_oled] ");
        if (pasteable == false) Serial.println();
        Serial.print("enabled = "); Serial.print(getStringFromTable(jumperlessConfig.top_oled.enabled, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("i2c_address = 0x"); Serial.print(jumperlessConfig.top_oled.i2c_address, HEX); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("width = "); Serial.print(jumperlessConfig.top_oled.width); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("height = "); Serial.print(jumperlessConfig.top_oled.height); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("connection_type = "); Serial.print(getConnectionTypeString(jumperlessConfig.top_oled.connection_type)); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("sda_pin = ");
        if (showNames) Serial.print(definesToChar(jumperlessConfig.top_oled.sda_pin, 0));
        else Serial.print(jumperlessConfig.top_oled.sda_pin);
        Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("scl_pin = ");
        if (showNames) Serial.print(definesToChar(jumperlessConfig.top_oled.scl_pin, 0));
        else Serial.print(jumperlessConfig.top_oled.scl_pin);
        Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("gpio_sda = ");
        if (showNames) Serial.print(definesToChar(jumperlessConfig.top_oled.gpio_sda, 0));
        else Serial.print(jumperlessConfig.top_oled.gpio_sda);
        Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("gpio_scl = ");
        if (showNames) Serial.print(definesToChar(jumperlessConfig.top_oled.gpio_scl, 0));
        else Serial.print(jumperlessConfig.top_oled.gpio_scl);
        Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("sda_row = ");
        if (showNames) Serial.print(definesToChar(jumperlessConfig.top_oled.sda_row, 0));
        else Serial.print(jumperlessConfig.top_oled.sda_row);
        Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("scl_row = ");
        if (showNames) Serial.print(definesToChar(jumperlessConfig.top_oled.scl_row, 0));
        else Serial.print(jumperlessConfig.top_oled.scl_row);
        Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("connect_on_boot = "); Serial.print(getStringFromTable(jumperlessConfig.top_oled.connect_on_boot, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("lock_connection = "); Serial.print(getStringFromTable(jumperlessConfig.top_oled.lock_connection, boolTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("show_in_terminal = "); Serial.print(getStringFromTable(jumperlessConfig.top_oled.show_in_terminal, serialPortTable)); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("font = "); Serial.print(getFontString(jumperlessConfig.top_oled.font)); Serial.println(";");
        if (pasteable == true) Serial.print("`[top_oled] ");
        Serial.print("startup_message = "); Serial.print(jumperlessConfig.top_oled.startup_message); Serial.println("");
    }
    cycleTerminalColor();
    // Print usb_cdc section
    if (section == -1 || section == 11) {
        Serial.print("\n`[usb_cdc] ");
        if (pasteable == false) Serial.println();
        Serial.print("ignore_dtr = "); Serial.print(getStringFromTable(jumperlessConfig.usb_cdc.ignore_dtr, boolTable)); Serial.println(";");
    }
    cycleTerminalColor();
    // if (section == -1) {
    //     Serial.println("\nEND\n\r");
    // }
}

// Helper function to clean whitespace
void cleanWhitespace(const char* input, char* output) {
    int j = 0;
    for (int i = 0; input[i]; i++) {
        if (!isspace(input[i])) {
            output[j++] = tolower(input[i]);
        }
    }
    output[j] = '\0';
}

// Helper function to parse setting.
// All callers pass section[32], key[32], value[64] (see call sites) while the
// input line can be up to 256 bytes — every copy below is capped to those
// sizes so a long/garbled line truncates instead of smashing the stack.
#define PARSE_SECTION_CAP (31)
#define PARSE_KEY_CAP     (31)
#define PARSE_VALUE_CAP   (63)
bool parseSetting(const char* line, char* section, char* key, char* value) {
    // Serial.print("Parsing line: '");
    // Serial.print(line);
    // Serial.println("'");
    
    // Check if this is dot notation format (config.section.key = value)
    if (strncmp(line, "config.", 7) == 0) {
        const char* start = line + 7;  // Skip "config."
        const char* firstDot = strchr(start, '.');
        const char* equals = strchr(start, '=');
        
        if (firstDot && equals && firstDot < equals) {
            // Extract section
            size_t secLen = (size_t)(firstDot - start);
            if (secLen > PARSE_SECTION_CAP) secLen = PARSE_SECTION_CAP;
            memcpy(section, start, secLen);
            section[secLen] = '\0';
            
            // Extract key
            const char* keyStart = firstDot + 1;
            size_t keyLen = (size_t)(equals - keyStart);
            if (keyLen > PARSE_KEY_CAP) keyLen = PARSE_KEY_CAP;
            memcpy(key, keyStart, keyLen);
            key[keyLen] = '\0';
            trim(key);
            
            // Extract value
            const char* valueStart = equals + 1;
            while (isspace(*valueStart)) valueStart++; // Skip leading whitespace
            strncpy(value, valueStart, PARSE_VALUE_CAP);
            value[PARSE_VALUE_CAP] = '\0';
            
            // Trim trailing whitespace and semicolon from value
            char* end = value + strlen(value) - 1;
            while (end > value && (isspace(*end) || *end == ';')) {
                *end = '\0';
                end--;
            }
            
            // Serial.print("Dot notation - Section: '");
            // Serial.print(section);
            // Serial.print("', Key: '");
            // Serial.print(key);
            // Serial.print("', Value: '");
            // Serial.print(value);
            // Serial.println("'");
            
            return true;
        }
    }
    
    // Original bracket notation format
    const char* sectionEnd = strchr(line, ']');
    if (!sectionEnd) {
        Serial.println("No ] found and not dot notation format");
        return false;
    }
    
    // Extract section (skip the [)
    size_t bsecLen = (size_t)(sectionEnd - line - 1);
    if (bsecLen > PARSE_SECTION_CAP) bsecLen = PARSE_SECTION_CAP;
    memcpy(section, line + 1, bsecLen);
    section[bsecLen] = '\0';
    
    // Convert section to lowercase for comparison
    char sectionLower[32];
    strncpy(sectionLower, section, sizeof(sectionLower) - 1);
    sectionLower[sizeof(sectionLower) - 1] = '\0';
    for(int i = 0; sectionLower[i]; i++) {
        sectionLower[i] = tolower(sectionLower[i]);
    }
    
    // Find the equals sign
    const char* equalsPos = strchr(sectionEnd, '=');
    if (!equalsPos) {
        Serial.println("No = found");
        return false;
    }
    
    // Extract key (skip the ])
    const char* keyStart = sectionEnd + 1;
    while (isspace(*keyStart)) keyStart++; // Skip leading whitespace
    
    // Find the end of the key (before the =)
    const char* keyEnd = equalsPos;
    while (keyEnd > keyStart && isspace(*(keyEnd-1))) keyEnd--; // Skip trailing whitespace
    
    size_t bkeyLen = (size_t)(keyEnd - keyStart);
    if (bkeyLen > PARSE_KEY_CAP) bkeyLen = PARSE_KEY_CAP;
    memcpy(key, keyStart, bkeyLen);
    key[bkeyLen] = '\0';
    
    // Extract value (skip the =)
    const char* valueStart = equalsPos + 1;
    while (isspace(*valueStart)) valueStart++; // Skip leading whitespace
    strncpy(value, valueStart, PARSE_VALUE_CAP);
    value[PARSE_VALUE_CAP] = '\0';
    
    // Trim trailing whitespace and semicolon from value
    char* end = value + strlen(value) - 1;
    while (end > value && (isspace(*end) || *end == ';')) {
        *end = '\0';
        end--;
    }
    
    return true;
}

    

// Helper function to print setting change
void printSettingChange(const char* section, const char* key, const char* oldValue, const char* newValue) {
    // Try to print names for enums/bools if possible
    const char* oldName = nullptr;
    const char* newName = nullptr;
    if (strcmp(section, "display") == 0 && strcmp(key, "lines_wires") == 0) {
        oldName = getStringFromTable(atoi(oldValue), linesWiresTable);
        newName = getStringFromTable(atoi(newValue), linesWiresTable);
    } else if (strcmp(section, "display") == 0 && strcmp(key, "net_color_mode") == 0) {
        oldName = getStringFromTable(atoi(oldValue), netColorModeTable);
        newName = getStringFromTable(atoi(newValue), netColorModeTable);
    } else if (strcmp(section, "display") == 0 && strcmp(key, "current_flow") == 0) {
        oldName = getStringFromTable(atoi(oldValue), currentFlowTable);
        newName = getStringFromTable(atoi(newValue), currentFlowTable);
    } else if ((strcmp(section, "serial_1") == 0 || strcmp(section, "serial_2") == 0 || strcmp(section, "gpio") == 0) && (strstr(key, "function") != NULL)) {
        oldName = getStringFromTable(atoi(oldValue), uartFunctionTable);
        newName = getStringFromTable(atoi(newValue), uartFunctionTable);
        initArduino();
    } else if (strcmp(section, "logo_pads") == 0) {
        oldName = getStringFromTable(atoi(oldValue), arbitraryFunctionTable);
        newName = getStringFromTable(atoi(newValue), arbitraryFunctionTable);
    } else if (strcmp(section, "top_oled") == 0 && strcmp(key, "font") == 0) {
        oldName = getFontString(atoi(oldValue));
        newName = getFontString(atoi(newValue));
    } else if (strcmp(section, "top_oled") == 0 && strcmp(key, "show_in_terminal") == 0) {
        oldName = getStringFromTable(atoi(oldValue), serialPortTable);
        newName = getStringFromTable(atoi(newValue), serialPortTable);
        initArduino();
    } else if (strcmp(section, "display") == 0 && strcmp(key, "dump_leds") == 0) {
        oldName = getStringFromTable(atoi(oldValue), serialPortTable);
        newName = getStringFromTable(atoi(newValue), serialPortTable);
        initArduino();
    } else if (strcmp(section, "display") == 0 && strcmp(key, "dump_format") == 0) {
        oldName = getStringFromTable(atoi(oldValue), dumpFormatTable);
        newName = getStringFromTable(atoi(newValue), dumpFormatTable);
    } else if (strcmp(section, "debug") == 0 && strcmp(key, "logic_analyzer") == 0) {
        oldName = getStringFromTable(atoi(oldValue), boolTable);
        newName = getStringFromTable(atoi(newValue), boolTable);
    } else if (
        (strcmp(section, "dacs") == 0 && (strcmp(key, "set_dacs_on_startup") == 0 || strcmp(key, "set_rails_on_startup") == 0)) ||
        (strcmp(section, "debug") == 0) ||
        (strcmp(section, "serial_1") == 0 && (strcmp(key, "print_passthrough") == 0 || strcmp(key, "connect_on_boot") == 0 || strcmp(key, "lock_connection") == 0)) ||
        (strcmp(section, "serial_2") == 0 && (strcmp(key, "print_passthrough") == 0 || strcmp(key, "connect_on_boot") == 0 || strcmp(key, "lock_connection") == 0))
    ) {
        oldName = getStringFromTable(atoi(oldValue), boolTable);
        newName = getStringFromTable(atoi(newValue), boolTable);
    }
    Serial.print("Changed [");
    Serial.print(section);
    Serial.print("] ");
    Serial.print(key);
    Serial.print(" from ");
    if (oldName) Serial.print(oldName); else Serial.print(oldValue);
    Serial.print(" to ");
    Serial.println(newValue);
}

void printConfigHelp() {

    
    Serial.println("\n\r");
    cycleTerminalColor(true, 8.0, true,  &Serial, 12, 1);
    Serial.println("                              Read config ");
    cycleTerminalColor(false, 2.0, true,  &Serial, 0, 1);
    Serial.println("                          ~ = show current config");
    Serial.println("                     ~names = show names for settings");
    Serial.println("                   ~numbers = show numbers for settings");
    Serial.println("                 ~[section] = show specific section (e.g. ~[routing])");
    cycleTerminalColor(true, 15.0, true,  &Serial, 22, 1);
    Serial.println("\n\r");
    Serial.println("                              Write config ");
    cycleTerminalColor(false, 2.0, true,  &Serial, 0, 1);
    Serial.println("`[section] setting = value; = enter config settings (pro tip: copy/paste setting from ~ output and just change the value)");
    // Serial.println("\n\r    config setting format (prefix with ` to paste from main menu)\n\r");    
    // Serial.println("    Example: ");
    // Serial.println("`[serial_1]connect_on_boot = true;");
    cycleTerminalColor(true, 15.0, true,  &Serial, 1, 1);

    Serial.println("\n\r");
    Serial.println("                              Reset config");
    cycleTerminalColor(false, 2.0, true,  &Serial, 0, 1);
    Serial.println("                     `reset = reset to defaults (keeps calibration and hardware version)");
    Serial.println("            `reset_hardware = reset hardware settings (keeps calibration)");
    Serial.println("         `reset_calibration = reset calibration settings (keeps hardware version)");
    Serial.println("                 `reset_all = reset to defaults and clear all settings");
    cycleTerminalColor(false, 1.0, true,  &Serial, 0, 1);
    Serial.println("         `force_first_start = clears everything to factory settings and runs first startup calibration");
    Serial.println("                 `self_test = run the hardware self test (non-destructive)");
    Serial.println("          `self_test_report = re-print the stored self test report");

    cycleTerminalColor(true, 15.0, true,  &Serial, 18, 1);
    Serial.println("\n\r");
    Serial.println("                              Help");
    cycleTerminalColor(false, 1.0, true,  &Serial, 0, 1);
    Serial.println("                         ~? = show this help\n\r");
    // Serial.println("\n\r\tor you can use dot notation\n\r");
    // Serial.println("`config.routing.stack_paths = 1;");
    // Serial.println("\n\r\tor paste a whole section\n\r");
    // Serial.println("`[dacs]");
    // Serial.println("top_rail = 5.0;");
    // Serial.println("bottom_rail = 3.3;");
    // Serial.println("dac_0 = -2.0;");
    // Serial.println("dac_1 = 3.33;");
    Serial.println("\n\r");
    delayMicroseconds(3000);
}

void printConfigToSerial(bool showNamesArg) {
    char line[128] = {0};
    int lineIndex = 0;
    unsigned long lastCharTime = millis();
    const unsigned long timeout = 1000; // 100ms timeout

    // Check if we already have a command line from line buffering mode
    // ONLY use buffered mode if terminal_line_buffering is enabled
    if (jumperlessConfig.display.terminal_line_buffering == 1 && currentCommandLine.length() > 1) {
        // Capture and clear immediately to prevent reuse
        String configCmd = currentCommandLine;
        currentCommandLine = ""; // Clear NOW before any processing
        
        // Remove the leading tilde character
        configCmd = configCmd.substring(1);
        configCmd.trim();
        
        // Check for ~names or ~numbers
        if (configCmd.startsWith("names")) {
            showNames = 1;
            Serial.println("showing names");
            currentCommandLine = "";
            return;
        } else if (configCmd.startsWith("numbers")) {
            showNames = 0;
            Serial.println("showing numbers");
            currentCommandLine = "";
            return;
        } else if (configCmd.startsWith("help") || configCmd == "?" || configCmd == "-h" || configCmd == "--help") {
            printConfigHelp();
            currentCommandLine = "";
            return;
        }
        
        // Check if we have a section like ~[display]
        if (configCmd.length() > 0 && configCmd[0] == '[') {
            int endBracket = configCmd.indexOf(']');
            if (endBracket > 0) {
                String sectionName = configCmd.substring(1, endBracket);
                int section = parseSectionName(sectionName.c_str());
                if (section != -1) {
                    printConfigSectionToSerial(section, showNamesArg);
                } else {
                    Serial.print("Unknown section: ");
                    Serial.println(sectionName);
                }
                currentCommandLine = "";
                return;
            }
        }
        
        // Default: print all config
        printConfigSectionToSerial(-1, showNamesArg);
        Serial.println("\n\n");
        currentCommandLine = "";
        return;
    }

    // Wait for input with timeout (character-by-character mode)
    // Use Serial directly when line buffering is disabled, Jerial when enabled
    Stream* inputStream = (jumperlessConfig.display.terminal_line_buffering == 1) ? (Stream*)&Jerial : (Stream*)&Serial;
    
    while (true) {
        if (inputStream->available() > 0) {
            char c = inputStream->read();
            if (lineIndex < sizeof(line) - 1) {
                line[lineIndex++] = c;
                line[lineIndex] = '\0';
                lastCharTime = millis();
            }

            // Check for ~names or ~numbers
            if (strncmp(line, "names", 5) == 0) {
                showNames = 1;
                Serial.println("showing names");
                lineIndex = 0;
                line[0] = '\0';
                continue;
            } else if (strncmp(line, "numbers", 7) == 0) {
                showNames = 0;
                Serial.println("showing numbers");
                lineIndex = 0;
                line[0] = '\0';
                continue;
            } else if (strncmp(line, "help", 4) == 0 || strncmp(line, "?", 1) == 0 || strncmp(line, "-h", 2) == 0 || strncmp(line, "--help", 6) == 0) {
                printConfigHelp();
                lineIndex = 0;
                line[0] = '\0';
                continue;
            }

            // Check if we have a section
            if (lineIndex >= 2 && line[0] == '[') {
                char* endBracket = strchr(line, ']');
                if (endBracket) {
                    char sectionName[32] = {0};
                    strncpy(sectionName, line + 1, endBracket - (line + 1));
                    sectionName[endBracket - (line + 1)] = '\0';

                    int section = parseSectionName(sectionName);
                    if (section != -1) {
                        printConfigSectionToSerial(section, showNamesArg);
                    } else {
                        Serial.print("Unknown section: ");
                        Serial.println(sectionName);
                    }
                    return;
                }
            }
        }

        // Check for timeout
        if (millis() - lastCharTime > timeout) {
            printConfigSectionToSerial(-1, showNamesArg);
            Serial.println("\n\n");
            return;
        }
    }
}

void readConfigFromSerial() {
    char line[128] = {0};
    int lineIndex = 0;
    char currentSection[32] = {0};
    bool inSection = false;
    

bool ledChange = false;
bool dacChange = false;
    unsigned long lastCharTime = millis();
    const unsigned long timeout = 10;

    // Check if we already have a command line from line buffering mode
    // ONLY use buffered mode if terminal_line_buffering is enabled
    if (jumperlessConfig.display.terminal_line_buffering == 1 && currentCommandLine.length() > 1) {
        // Capture and clear immediately to prevent reuse
        String configCmd = currentCommandLine;
        currentCommandLine = ""; // Clear NOW before any processing
        
        // Remove the leading backtick character
        configCmd = configCmd.substring(1);
        configCmd.trim();
        
        if (configCmd.length() > 0) {
            // Copy to our line buffer for processing
            strncpy(line, configCmd.c_str(), sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            lineIndex = strlen(line);
            
            // Check for special commands first (before trying to parse as settings)
            if (strcmp(line, "?") == 0 || strcmp(line, "-h") == 0 || strcmp(line, "--help") == 0 || strcmp(line, "help") == 0) {
                printConfigHelp();
                return;
            }
            else if (strcmp(line, "reset") == 0) {
                resetConfigToDefaults();
                saveConfigToFile("/config.txt");
                Serial.println("Done. Settings have been reset to defaults");
                ledChange = true;
                dacChange = true;
                return;
            }
            else if (strcmp(line, "clear_calibration") == 0 || strcmp(line, "clear_cal") == 0 || 
                     strcmp(line, "reset_calibration") == 0 || strcmp(line, "reset_cal") == 0) {
                resetConfigToDefaults(1, 0);
                saveConfig();
                Serial.println("Done. Calibration has been cleared");
                ledChange = true;
                dacChange = true;
                return;
            }
            else if (strcmp(line, "clear_hardware") == 0 || strcmp(line, "clear_hw") == 0 || 
                     strcmp(line, "reset_hardware") == 0 || strcmp(line, "reset_hw") == 0) {
                resetConfigToDefaults(0, 1);
                saveConfig();
                Serial.println("Done. Hardware has been cleared");
                ledChange = true;
                dacChange = true;
                return;
            }
            else if (strcmp(line, "clear_all") == 0 || strcmp(line, "reset_all") == 0) {
                resetConfigToDefaults(1, 1);
                saveConfig();
                Serial.println("Done. All settings have been cleared");
                ledChange = true;
                dacChange = true;
                return;
            }
            else if (strcmp(line, "clear_filesystem") == 0 || strcmp(line, "reset_filesystem") == 0) {
                Serial.println("Deleting all filesystem contents...");
                bool deleteSuccess = deleteDirectoryContents("/");
                Serial.println("Filesystem contents deleted.");
                return;
            }
            else if (strcmp(line, "self_test") == 0 || strcmp(line, "selftest") == 0) {
                // Non-destructive hardware self test (no FS wipe, no reboot)
                runFullSelfTest(false);
                return;
            }
            else if (strcmp(line, "self_test_report") == 0 || strcmp(line, "selftest_report") == 0) {
                // Re-print the stored report without re-running the tests
                selfTestPrintStoredReport();
                return;
            }
            else if (strcmp(line, "force_first_start") == 0 || strcmp(line, "factory_reset") == 0) {
                cycleTerminalColor(true, 100.0, true, &Serial, 0, 1);
                safeFileDelete("/config.txt");
                Serial.println("Config file deleted.");
                Serial.flush();

                bool deleteSuccess = deleteDirectoryContents("/");

                cycleTerminalColor(false, 100.0, true, &Serial, 0, 1);
                if (deleteSuccess) {
                    Serial.println("All filesystem contents deleted successfully.");
                } else {
                    Serial.println("Some files/directories could not be deleted (this may be normal).");
                }
                Serial.flush();
                
                EEPROM.write(FIRSTSTARTUPADDRESS, 0x00);
                eepromMarkDirty();
                eepromCommitSafe();  // reboot imminent - commit synchronously (safe envelope)
                cycleTerminalColor(false, 100.0, true, &Serial, 0, 1);
                Serial.println("First startup flag cleared.");
                Serial.flush();
                
                cycleTerminalColor(false, 100.0, true, &Serial, 0, 1);
                Serial.println("Done. All settings have been cleared");
                delay(200);
                
                unsigned long startTime = millis() + 1000;
                int dots = 0;
                while (millis() < 3000) {
                    if (millis() - startTime > 500) {
                        Serial.print("\r                                           \r");
                        Serial.print("Power cycling");
                        dots++;
                        for (int i = 0; i < dots; i++) {
                            Serial.print(".");
                        }
                        startTime = millis();
                    }
                    if (dots >= 3) {
                        dots = 0;
                    }
                    Serial.flush();
                }
                
                rp2040.reboot();
                return;
            }
            
            // If not a special command, try to parse as a config setting
            char section[32], key[32], value[64];
            if (parseSetting(line, section, key, value)) {
                updateConfigValue(section, key, value);
                Serial.println("Config updated");
                readSettingsFromConfig();
                setRailsAndDACs(0);
                showLEDsCore2 = -1;
                
                // Clear any leftover characters from Jerial buffer
                while (Jerial.available() > 0) {
                    Jerial.read();
                }
                // Also clear any completed lines waiting
                if (Jerial.hasCompletedLine()) {
                    Jerial.clearCompletedLine();
                }
                
                return;
            } else {
                Serial.println("Failed to parse config setting");
            }
        }
        return;
    }

    Serial.println("\n\renter config settings (? for help)\n\r");

    // Use Serial directly when line buffering is disabled, Jerial when enabled
    Stream* inputStream = (jumperlessConfig.display.terminal_line_buffering == 1) ? (Stream*)&Jerial : (Stream*)&Serial;
    
    while (inputStream->available() == 0) {
        // delayMicroseconds(10);
        if (millis() - lastCharTime > 400) {
            //Serial.println("No input detected. Showing help.");
            printConfigHelp();
            return;
        }
    }
    int timedOut = 0;
    while (true) {
        if (inputStream->available() > 0) {
            char c = inputStream->read();
            if (c == '\n' || c == '\r') {
               // parseSetting(line);
                // Serial.println("New line");
            }

            //lastCharTime = millis();

            // Handle backspace
            if (c == '\b' || c == 0x7F) {
                if (lineIndex > 0) {
                    lineIndex--;
                    Serial.print(" \b"); // Erase character
                }
                continue;
            }

            // Add character to line buffer if there's space
            if (lineIndex < sizeof(line) - 1) {
                line[lineIndex++] = c;
                line[lineIndex] = '\0'; // Keep string null-terminated

                // Check for help commands as soon as they're typed
                if (strcmp(line, "?") == 0 || strcmp(line, "-h") == 0 || strcmp(line, "--help") == 0 || strcmp(line, "help") == 0) {
                    printConfigHelp();
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    continue;
                }

                // Check for reset
                if (strcmp(line, "reset") == 0) {
                    resetConfigToDefaults();
                    saveConfigToFile("/config.txt");
                    Serial.println("Done. Settings have been reset to defaults");
                    ledChange = true;
                    dacChange = true;
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    continue;
                } else if (strcmp(line, "clear_calibration") == 0 || strcmp(line, "clear_cal") == 0 || strcmp(line, "reset_calibration") == 0 || strcmp(line, "reset_cal") == 0) {
                    resetConfigToDefaults(1, 0);
                    saveConfig();
                    Serial.println("Done. Calibration has been cleared");
                    ledChange = true;
                    dacChange = true;
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    continue;
                } else if (strcmp(line, "clear_hardware") == 0 || strcmp(line, "clear_hw") == 0 || strcmp(line, "reset_hardware") == 0 || strcmp(line, "reset_hw") == 0) {
                    resetConfigToDefaults(0, 1);
                    saveConfig();
                    Serial.println("Done. Hardware has been cleared");
                    ledChange = true;
                    dacChange = true;
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    continue;
                } else if (strcmp(line, "clear_all") == 0 || strcmp(line, "reset_all") == 0) {
                    resetConfigToDefaults(1, 1);
                    saveConfig();
                    Serial.println("Done. All settings have been cleared");
                    ledChange = true;
                    dacChange = true;
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    continue;
                } else if (strcmp(line, "clear_filesystem") == 0 || strcmp(line, "reset_filesystem") == 0) {
                    Serial.println("Deleting all filesystem contents...");
                    bool deleteSuccess = deleteDirectoryContents("/");
                    Serial.println("Filesystem contents deleted.");
                    continue;
                
                } else if (strcmp(line, "self_test") == 0 || strcmp(line, "selftest") == 0) {
                    runFullSelfTest(false);
                    continue;

                } else if (strcmp(line, "self_test_report") == 0 || strcmp(line, "selftest_report") == 0) {
                    selfTestPrintStoredReport();
                    continue;

                } else if (strcmp(line, "force_first_start") == 0 || strcmp(line, "factory_reset") == 0) {
                    //firstStart = 1;
                    cycleTerminalColor(true, 100.0, true,  &Serial, 0, 1);
                    safeFileDelete("/config.txt");

                    Serial.println("Config file deleted.");
                    Serial.flush();

                    // // Delete all contents of the filesystem recursively
                    bool deleteSuccess = deleteDirectoryContents("/");
                    
                    cycleTerminalColor(false, 100.0, true,  &Serial, 0, 1);
                    if (deleteSuccess) {
                        Serial.println("All filesystem contents deleted successfully.");
                    } else {
                        Serial.println("Some files/directories could not be deleted (this may be normal).");
                    }
                    Serial.flush();


                    

                    EEPROM.write(FIRSTSTARTUPADDRESS, 0x00);
                    eepromMarkDirty();
                    eepromCommitSafe();  // reboot imminent - commit synchronously (safe envelope)
                    cycleTerminalColor(false, 100.0, true,  &Serial, 0, 1);
                    Serial.println("First startup flag cleared.");
                    Serial.flush();
                    

                    cycleTerminalColor(false, 100.0, true,  &Serial, 0, 1);
                    Serial.println("Done. All settings have been cleared");
                    delay(200);
                    cycleTerminalColor(false, 100.0, true,  &Serial, 0, 1);
                   // Serial.println("\n\rPower cycle your Jumperless to reset config and force startup calibration.");

                    unsigned long startTime = millis()+1000;
                    int dots = 0;
//return;
                    while (millis() < 3000) {
                         if (millis() - startTime > 500) {
                        Serial.print("\r                                           \r");
                        Serial.print("Power cycling");
                       
                            dots++;
                            for (int i = 0; i < dots; i++) {
                                Serial.print(".");
                            }
                            startTime = millis();
                        }
                        if (dots >= 3) {
                            
                            dots = 0;
                        }
                        Serial.flush();
            
                    }
                    // saveConfigToFile("/config.txt");
                    // Serial.println("Done. All settings have been reset to defaults");
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    rp2040.reboot();
                    continue;
                }
            }
            // ... existing code ...
                        // Process line when newline or semicolon is received
            if (c == '\n' || c == '\r' || c == ';') {
                if (lineIndex > 0) {
                    line[lineIndex] = '\0';
                    
                    // Check if this is a section header
                    if (line[0] == '[') {
                        char* endBracket = strchr(line, ']');
                        if (endBracket) {
                            // If there's content after the closing bracket, split it into section header and setting
                            if (endBracket[1] != '\0') {
                                // Save the section header
                                *endBracket = '\0';
                                strncpy(currentSection, line + 1, sizeof(currentSection) - 1);
                                inSection = true;
                                
                                // Process the setting part if it exists
                                const char* settingPart = endBracket + 1;
                                while (*settingPart == ' ' || *settingPart == '\t') settingPart++; // Skip whitespace
                                if (*settingPart != '\0') {
                                    char section[32], key[32], value[64];
                                    char tempLine[256];
                                    snprintf(tempLine, sizeof(tempLine), "[%s]%s", currentSection, settingPart);
                                    if (parseSetting(tempLine, section, key, value)) {
                                        updateConfigValue(section, key, value);
                                    }
                                }
                            } else {
                                // Pure section header
                                *endBracket = '\0';
                                strncpy(currentSection, line + 1, sizeof(currentSection) - 1);
                                inSection = true;
                            }
                        }
                    }
                    // Check if this is dot notation format
                    else if (strncmp(line, "config.", 7) == 0) {
                        char section[32], key[32], value[64];
                        if (parseSetting(line, section, key, value)) {
                            updateConfigValue(section, key, value);
                        }
                    }
                    // Process key=value pair if we're in a section
                    else if (inSection && strchr(line, '=')) {
                        char section[32], key[32], value[64];
                        strcpy(section, currentSection);
                        
                        // Create a temporary line with section header for parsing
                        char tempLine[256];
                        snprintf(tempLine, sizeof(tempLine), "[%s]%s", section, line);
                        
                        if (parseSetting(tempLine, section, key, value)) {
                            updateConfigValue(section, key, value);
                        }
                    }
                    
                    // Clear line buffer but maintain section context
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                }
            }
        } else if (millis() - lastCharTime > 10) {
            lastCharTime = millis();
            timedOut++;
           

        }
 //Serial.println(timedOut);
        if (timedOut > timeout) {
            // printConfigHelp();
            // Serial.println("\n\r");
            // Serial.flush();
            memset(line, 0, sizeof(line));
            lineIndex = 0;

            break;
        }
    }

    while (inputStream->available() > 0) {
        inputStream->read();
        delayMicroseconds(100);
    }
   // configChanged = true;
   readSettingsFromConfig();
//    Serial.println(globalState.power.topRail);
//    Serial.println(globalState.power.bottomRail);
//    Serial.println(globalState.power.dac0);
//    Serial.println(globalState.power.dac1);
    setRailsAndDACs(0);
    showLEDsCore2 = -1;
}

int parseTrueFalse(const char* value) {
    if (strcmp(value, "true") == 0) return 1;
    else if (strcmp(value, "false") == 0) return 0;
    else if (strcmp(value, "1") == 0) return 1;
    else if (strcmp(value, "0") == 0) return 0;
    else return -1;
}

void updateConfigValue(const char* section, const char* key, const char* value) {
    char oldValue[64] = {0};
     //! this is a place to add new config options
    // Get old value
    if (strcmp(section, "firmware") == 0) {
        if (strcmp(key, "last_version") == 0) sprintf(oldValue, "%s", jumperlessConfig.firmware.last_version);
        else if (strcmp(key, "files_provisioned") == 0) sprintf(oldValue, "%d", jumperlessConfig.firmware.files_provisioned);
    }
    else if (strcmp(section, "hardware") == 0) {
        if (strcmp(key, "generation") == 0) sprintf(oldValue, "%d", jumperlessConfig.hardware.generation);
        else if (strcmp(key, "revision") == 0) sprintf(oldValue, "%d", jumperlessConfig.hardware.revision);
        else if (strcmp(key, "probe_revision") == 0) sprintf(oldValue, "%d", jumperlessConfig.hardware.probe_revision);
        else if (strcmp(key, "psram_installed") == 0) sprintf(oldValue, "%d", jumperlessConfig.hardware.psram_installed);
        else if (strcmp(key, "psram_app_size_kb") == 0) sprintf(oldValue, "%d", jumperlessConfig.hardware.psram_app_size_kb);
        else if (strcmp(key, "probe_led_on_button_pin") == 0) sprintf(oldValue, "%d", jumperlessConfig.hardware.probe_led_on_button_pin);
    }
    else if (strcmp(section, "dacs") == 0) {
        // Voltage state (top_rail, bottom_rail, dac_0, dac_1) moved to globalState.power
        if (strcmp(key, "set_dacs_on_boot") == 0) sprintf(oldValue, "%d", jumperlessConfig.dacs.set_dacs_on_boot);
        else if (strcmp(key, "set_rails_on_boot") == 0) sprintf(oldValue, "%d", jumperlessConfig.dacs.set_rails_on_boot);
        else if (strcmp(key, "probe_power_dac") == 0) sprintf(oldValue, "%d", jumperlessConfig.dacs.probe_power_dac);
        else if (strcmp(key, "auto_connect_probe") == 0) sprintf(oldValue, "%d", jumperlessConfig.dacs.auto_connect_probe);
        else if (strcmp(key, "limit_max") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.dacs.limit_max);
        else if (strcmp(key, "limit_min") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.dacs.limit_min);
    }
    else if (strcmp(section, "debug") == 0) {
        if (strcmp(key, "file_parsing") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.file_parsing);
        else if (strcmp(key, "net_manager") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.net_manager);
        else if (strcmp(key, "nets_to_chips") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.nets_to_chips);
        else if (strcmp(key, "nets_to_chips_alt") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.nets_to_chips_alt);
        else if (strcmp(key, "leds") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.leds);
        else if (strcmp(key, "probing") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.probing);
        else if (strcmp(key, "oled") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.oled);
        else if (strcmp(key, "logo_pads") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.logo_pads);
        else if (strcmp(key, "logic_analyzer") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.logic_analyzer);
        else if (strcmp(key, "arduino") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.arduino);
        else if (strcmp(key, "usb_mass_storage") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.usb_mass_storage);
        else if (strcmp(key, "show_probe_current") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.show_probe_current);
        else if (strcmp(key, "show_node_errors") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.show_node_errors);
        else if (strcmp(key, "probe_power_gpio") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.probe_power_gpio);
        else if (strcmp(key, "probe_switch_stats") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.probe_switch_stats);
        else if (strcmp(key, "net_voltage_scan") == 0) sprintf(oldValue, "%d", jumperlessConfig.debug.net_voltage_scan);
    }
    else if (strcmp(section, "routing") == 0) {
        if (strcmp(key, "stack_paths") == 0) sprintf(oldValue, "%d", jumperlessConfig.routing.stack_paths);
        else if (strcmp(key, "stack_rails") == 0) sprintf(oldValue, "%d", jumperlessConfig.routing.stack_rails);
        else if (strcmp(key, "stack_dacs") == 0) sprintf(oldValue, "%d", jumperlessConfig.routing.stack_dacs);
        else if (strcmp(key, "rail_priority") == 0) sprintf(oldValue, "%d", jumperlessConfig.routing.rail_priority);
    }
    else if (strcmp(section, "usb_cdc") == 0) {
        if (strcmp(key, "ignore_dtr") == 0) sprintf(oldValue, "%d", jumperlessConfig.usb_cdc.ignore_dtr);
    }
    else if (strcmp(section, "calibration") == 0) {
        if (strcmp(key, "top_rail_zero") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.top_rail_zero);
        else if (strcmp(key, "top_rail_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.top_rail_spread);
        else if (strcmp(key, "bottom_rail_zero") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.bottom_rail_zero);
        else if (strcmp(key, "bottom_rail_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.bottom_rail_spread);
        else if (strcmp(key, "dac_0_zero") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.dac_0_zero);
        else if (strcmp(key, "dac_0_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.dac_0_spread);
        else if (strcmp(key, "dac_1_zero") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.dac_1_zero);
        else if (strcmp(key, "dac_1_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.dac_1_spread);
        else if (strcmp(key, "adc_0_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_0_zero);
        else if (strcmp(key, "adc_0_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_0_spread);
        else if (strcmp(key, "adc_1_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_1_zero);
        else if (strcmp(key, "adc_1_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_1_spread);
        else if (strcmp(key, "adc_2_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_2_zero);
        else if (strcmp(key, "adc_2_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_2_spread);
        else if (strcmp(key, "adc_3_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_3_zero);
        else if (strcmp(key, "adc_3_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_3_spread);
        else if (strcmp(key, "adc_4_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_4_zero);
        else if (strcmp(key, "adc_4_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_4_spread);
        else if (strcmp(key, "adc_7_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_7_zero);
        else if (strcmp(key, "adc_7_spread") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.adc_7_spread);
        else if (strcmp(key, "probe_max") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.probe_max);
        else if (strcmp(key, "probe_min") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.probe_min);
        else if (strcmp(key, "probe_max_measure") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.probe_max_measure);
        else if (strcmp(key, "probe_min_measure") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.probe_min_measure);
        else if (strcmp(key, "probe_switch_threshold_high") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.probe_switch_threshold_high);
        else if (strcmp(key, "probe_switch_threshold_low") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.probe_switch_threshold_low);
        else if (strcmp(key, "probe_switch_threshold") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.probe_switch_threshold);
        else if (strcmp(key, "probe_current_zero") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.probe_current_zero);
        else if (strcmp(key, "minimum_probe_reading") == 0) sprintf(oldValue, "%d", jumperlessConfig.calibration.minimum_probe_reading);
        else if (strcmp(key, "probe_droop_v0") == 0) sprintf(oldValue, "%.3f", jumperlessConfig.calibration.probe_droop_v0);
        else if (strcmp(key, "crosspoint_resistance") == 0) sprintf(oldValue, "%.2f", jumperlessConfig.calibration.crosspoint_resistance);
        }
    else if (strcmp(section, "logo_pads") == 0) {
        if (strcmp(key, "top_guy") == 0) sprintf(oldValue, "%d", jumperlessConfig.logo_pads.top_guy);
        else if (strcmp(key, "bottom_guy") == 0) sprintf(oldValue, "%d", jumperlessConfig.logo_pads.bottom_guy);
        else if (strcmp(key, "building_pad_top") == 0) sprintf(oldValue, "%d", jumperlessConfig.logo_pads.building_pad_top);
        else if (strcmp(key, "building_pad_bottom") == 0) sprintf(oldValue, "%d", jumperlessConfig.logo_pads.building_pad_bottom);
        else if (strcmp(key, "repeat_ms") == 0) sprintf(oldValue, "%d", jumperlessConfig.logo_pads.repeat_ms);
    }
    else if (strcmp(section, "display") == 0) {
        if (strcmp(key, "lines_wires") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.lines_wires);
        else if (strcmp(key, "menu_brightness") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.menu_brightness);
        else if (strcmp(key, "led_brightness") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.led_brightness);
        else if (strcmp(key, "rail_brightness") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.rail_brightness);
        else if (strcmp(key, "special_net_brightness") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.special_net_brightness);
        else if (strcmp(key, "net_color_mode") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.net_color_mode);
        else if (strcmp(key, "net_currents") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.net_currents);
        else if (strcmp(key, "current_flow") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.current_flow);
        else if (strcmp(key, "dump_leds") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.dump_leds);
        else if (strcmp(key, "dump_format") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.dump_format);
        else if (strcmp(key, "terminal_line_buffering") == 0) sprintf(oldValue, "%d", jumperlessConfig.display.terminal_line_buffering);
    }
    else if (strcmp(section, "serial_1") == 0) {
        if (strcmp(key, "function") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.function);
        else if (strcmp(key, "baud_rate") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.baud_rate);
        else if (strcmp(key, "print_passthrough") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.print_passthrough);
        else if (strcmp(key, "connect_on_boot") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.connect_on_boot);
        else if (strcmp(key, "lock_connection") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.lock_connection);
        else if (strcmp(key, "autoconnect_flashing") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.autoconnect_flashing);
        else if (strcmp(key, "async_passthrough") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.async_passthrough);
        else if (strcmp(key, "tag_parsing") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.tag_parsing);
        else if (strcmp(key, "flash_reset_type") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_1.flash_reset_type);
    }
    else if (strcmp(section, "serial_2") == 0) {
        if (strcmp(key, "function") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_2.function);
        else if (strcmp(key, "baud_rate") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_2.baud_rate);
        else if (strcmp(key, "print_passthrough") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_2.print_passthrough);
        else if (strcmp(key, "connect_on_boot") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_2.connect_on_boot);
        else if (strcmp(key, "lock_connection") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_2.lock_connection);
        else if (strcmp(key, "autoconnect_flashing") == 0) sprintf(oldValue, "%d", jumperlessConfig.serial_2.autoconnect_flashing);
    }
    else if (strcmp(section, "top_oled") == 0) {
        if (strcmp(key, "enabled") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.enabled);
        else if (strcmp(key, "i2c_address") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.i2c_address);
        else if (strcmp(key, "display_type") == 0) sprintf(oldValue, "%s", jumperlessConfig.top_oled.display_type);
        else if (strcmp(key, "width") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.width);
        else if (strcmp(key, "height") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.height);
        else if (strcmp(key, "rotation") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.rotation);
        else if (strcmp(key, "connection_type") == 0) sprintf(oldValue, "%s", getConnectionTypeString(jumperlessConfig.top_oled.connection_type));
        else if (strcmp(key, "sda_pin") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.sda_pin);
        else if (strcmp(key, "scl_pin") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.scl_pin);
        else if (strcmp(key, "gpio_sda") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.gpio_sda);
        else if (strcmp(key, "gpio_scl") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.gpio_scl);
        else if (strcmp(key, "sda_row") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.sda_row);
        else if (strcmp(key, "scl_row") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.scl_row);
        else if (strcmp(key, "connect_on_boot") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.connect_on_boot);
        else if (strcmp(key, "lock_connection") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.lock_connection);
        else if (strcmp(key, "show_in_terminal") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.show_in_terminal);
        else if (strcmp(key, "font") == 0) sprintf(oldValue, "%d", jumperlessConfig.top_oled.font);
        else if (strcmp(key, "startup_message") == 0) sprintf(oldValue, "%s", jumperlessConfig.top_oled.startup_message);
    }
    // Update the config structure
    // Accept string names for enums/bools and convert to int
    if (strcmp(section, "firmware") == 0) {
        if (strcmp(key, "last_version") == 0) {
            strncpy(jumperlessConfig.firmware.last_version, value, sizeof(jumperlessConfig.firmware.last_version) - 1);
            jumperlessConfig.firmware.last_version[sizeof(jumperlessConfig.firmware.last_version) - 1] = '\0';
        }
        else if (strcmp(key, "files_provisioned") == 0) jumperlessConfig.firmware.files_provisioned = parseBool(value);
    }
    else if (strcmp(section, "hardware") == 0) {
        if (strcmp(key, "generation") == 0) jumperlessConfig.hardware.generation = parseInt(value);
        else if (strcmp(key, "revision") == 0) jumperlessConfig.hardware.revision = parseInt(value);
        else if (strcmp(key, "probe_revision") == 0) jumperlessConfig.hardware.probe_revision = parseInt(value);
        else if (strcmp(key, "psram_installed") == 0) {
            int newValue = parseBool(value);
            jumperlessConfig.hardware.psram_installed = newValue;
            applyPsramModeChange(newValue);
            reinitMicroPythonForPsramChange();
        }
        else if (strcmp(key, "psram_app_size_kb") == 0) {
            // Changing the app/MP partition only takes effect on next boot
            // since MicroPython has already mapped its heap.
            jumperlessConfig.hardware.psram_app_size_kb = parseInt(value);
        }
        else if (strcmp(key, "probe_led_on_button_pin") == 0) {
            // Pin binding happens once at initLEDs(); takes effect on next boot.
            jumperlessConfig.hardware.probe_led_on_button_pin = parseBool(value);
        }
    }
    else if (strcmp(section, "dacs") == 0) {
        // Voltage state (top_rail, bottom_rail, dac_0, dac_1) moved to globalState.power
        if (strcmp(key, "set_dacs_on_boot") == 0) jumperlessConfig.dacs.set_dacs_on_boot = parseBool(value);
        else if (strcmp(key, "set_rails_on_boot") == 0) jumperlessConfig.dacs.set_rails_on_boot = parseBool(value);
        else if (strcmp(key, "probe_power_dac") == 0) jumperlessConfig.dacs.probe_power_dac = parseInt(value);
        else if (strcmp(key, "auto_connect_probe") == 0) {
            jumperlessConfig.dacs.auto_connect_probe = parseInt(value);
            if (jumperlessConfig.dacs.auto_connect_probe <= 0) {
                routableBufferPower(0, 0, 1);
            }
        }
        else if (strcmp(key, "limit_max") == 0) jumperlessConfig.dacs.limit_max = parseFloat(value);
        else if (strcmp(key, "limit_min") == 0) jumperlessConfig.dacs.limit_min = parseFloat(value);
    }
    else if (strcmp(section, "debug") == 0) {
        if (strcmp(key, "file_parsing") == 0) jumperlessConfig.debug.file_parsing = parseBool(value);
        else if (strcmp(key, "net_manager") == 0) jumperlessConfig.debug.net_manager = parseBool(value);
        else if (strcmp(key, "nets_to_chips") == 0) jumperlessConfig.debug.nets_to_chips = parseBool(value);
        else if (strcmp(key, "nets_to_chips_alt") == 0) jumperlessConfig.debug.nets_to_chips_alt = parseBool(value);
        else if (strcmp(key, "leds") == 0) jumperlessConfig.debug.leds = parseBool(value);
        else if (strcmp(key, "probing") == 0) jumperlessConfig.debug.probing = parseBool(value);
        else if (strcmp(key, "oled") == 0) jumperlessConfig.debug.oled = parseBool(value);
        else if (strcmp(key, "logo_pads") == 0) jumperlessConfig.debug.logo_pads = parseBool(value);
        else if (strcmp(key, "logic_analyzer") == 0) jumperlessConfig.debug.logic_analyzer = parseBool(value);
        else if (strcmp(key, "arduino") == 0) jumperlessConfig.debug.arduino = parseInt(value);
        else if (strcmp(key, "usb_mass_storage") == 0) jumperlessConfig.debug.usb_mass_storage = parseBool(value);
        else if (strcmp(key, "show_probe_current") == 0) jumperlessConfig.debug.show_probe_current = parseInt(value);
        else if (strcmp(key, "show_node_errors") == 0) jumperlessConfig.debug.show_node_errors = parseBool(value);
        else if (strcmp(key, "probe_power_gpio") == 0) {
            jumperlessConfig.debug.probe_power_gpio = parseBool(value);
            // Re-apply so the buffer power source swaps immediately (and a
            // stale GPIO bridge is torn down when turning the flag off).
            routableBufferPower(1, 0, 1);
        }
        else if (strcmp(key, "probe_switch_stats") == 0) jumperlessConfig.debug.probe_switch_stats = parseBool(value);
        else if (strcmp(key, "net_voltage_scan") == 0) jumperlessConfig.debug.net_voltage_scan = parseBool(value);
    }
    else if (strcmp(section, "routing") == 0) {
        if (strcmp(key, "stack_paths") == 0) jumperlessConfig.routing.stack_paths = parseInt(value);
        else if (strcmp(key, "stack_rails") == 0) jumperlessConfig.routing.stack_rails = parseInt(value);
        else if (strcmp(key, "stack_dacs") == 0) jumperlessConfig.routing.stack_dacs = parseInt(value);
        else if (strcmp(key, "rail_priority") == 0) jumperlessConfig.routing.rail_priority = parseInt(value);
    }
    else if (strcmp(section, "usb_cdc") == 0) {
        if (strcmp(key, "ignore_dtr") == 0) {
            jumperlessConfig.usb_cdc.ignore_dtr = parseBool(value);
            usb_cdc_set_ignore_dtr(jumperlessConfig.usb_cdc.ignore_dtr);
            // Apply USB CDC config immediately so the change takes effect
            // extern void setDTRLockout(uint32_t ms);
            AsyncPassthrough::setDTRLockout(3000);
            
            usb_cdc_apply_config();
        }
    }
    else if (strcmp(section, "calibration") == 0) {
        if (strcmp(key, "top_rail_zero") == 0) jumperlessConfig.calibration.top_rail_zero = parseInt(value);
        else if (strcmp(key, "top_rail_spread") == 0) jumperlessConfig.calibration.top_rail_spread = parseFloat(value);
        else if (strcmp(key, "bottom_rail_zero") == 0) jumperlessConfig.calibration.bottom_rail_zero = parseInt(value);
        else if (strcmp(key, "bottom_rail_spread") == 0) jumperlessConfig.calibration.bottom_rail_spread = parseFloat(value);
        else if (strcmp(key, "dac_0_zero") == 0) jumperlessConfig.calibration.dac_0_zero = parseInt(value);
        else if (strcmp(key, "dac_0_spread") == 0) jumperlessConfig.calibration.dac_0_spread = parseFloat(value);
        else if (strcmp(key, "dac_1_zero") == 0) jumperlessConfig.calibration.dac_1_zero = parseInt(value);
        else if (strcmp(key, "dac_1_spread") == 0) jumperlessConfig.calibration.dac_1_spread = parseFloat(value);
        else if (strcmp(key, "adc_0_zero") == 0) jumperlessConfig.calibration.adc_0_zero = parseFloat(value);
        else if (strcmp(key, "adc_0_spread") == 0) jumperlessConfig.calibration.adc_0_spread = parseFloat(value);
        else if (strcmp(key, "adc_1_zero") == 0) jumperlessConfig.calibration.adc_1_zero = parseFloat(value);
        else if (strcmp(key, "adc_1_spread") == 0) jumperlessConfig.calibration.adc_1_spread = parseFloat(value);
        else if (strcmp(key, "adc_2_zero") == 0) jumperlessConfig.calibration.adc_2_zero = parseFloat(value);
        else if (strcmp(key, "adc_2_spread") == 0) jumperlessConfig.calibration.adc_2_spread = parseFloat(value);
        else if (strcmp(key, "adc_3_zero") == 0) jumperlessConfig.calibration.adc_3_zero = parseFloat(value);
        else if (strcmp(key, "adc_3_spread") == 0) jumperlessConfig.calibration.adc_3_spread = parseFloat(value);
        else if (strcmp(key, "adc_4_zero") == 0) jumperlessConfig.calibration.adc_4_zero = parseFloat(value);
        else if (strcmp(key, "adc_4_spread") == 0) jumperlessConfig.calibration.adc_4_spread = parseFloat(value);
        else if (strcmp(key, "adc_7_zero") == 0) jumperlessConfig.calibration.adc_7_zero = parseFloat(value);
        else if (strcmp(key, "adc_7_spread") == 0) jumperlessConfig.calibration.adc_7_spread = parseFloat(value);
        else if (strcmp(key, "probe_max") == 0) jumperlessConfig.calibration.probe_max = parseInt(value);
        else if (strcmp(key, "probe_min") == 0) jumperlessConfig.calibration.probe_min = parseInt(value);
        else if (strcmp(key, "probe_max_measure") == 0) jumperlessConfig.calibration.probe_max_measure = parseInt(value);
        else if (strcmp(key, "probe_min_measure") == 0) jumperlessConfig.calibration.probe_min_measure = parseInt(value);
        else if (strcmp(key, "measure_mode_output_voltage") == 0) jumperlessConfig.calibration.measure_mode_output_voltage = parseFloat(value);
        else if (strcmp(key, "probe_switch_threshold_high") == 0) jumperlessConfig.calibration.probe_switch_threshold_high = parseFloat(value);
        else if (strcmp(key, "probe_switch_threshold_low") == 0) jumperlessConfig.calibration.probe_switch_threshold_low = parseFloat(value);
        else if (strcmp(key, "probe_switch_threshold") == 0) jumperlessConfig.calibration.probe_switch_threshold = parseFloat(value);
        else if (strcmp(key, "probe_current_zero") == 0) jumperlessConfig.calibration.probe_current_zero = parseFloat(value);
        else if (strcmp(key, "minimum_probe_reading") == 0) jumperlessConfig.calibration.minimum_probe_reading = parseInt(value);
        else if (strcmp(key, "probe_droop_v0") == 0) jumperlessConfig.calibration.probe_droop_v0 = parseFloat(value);
        else if (strcmp(key, "crosspoint_resistance") == 0) jumperlessConfig.calibration.crosspoint_resistance = parseFloat(value);
        }
    else if (strcmp(section, "logo_pads") == 0) {
        if (strcmp(key, "top_guy") == 0) jumperlessConfig.logo_pads.top_guy = parseArbitraryFunction(value);
        else if (strcmp(key, "bottom_guy") == 0) jumperlessConfig.logo_pads.bottom_guy = parseArbitraryFunction(value);
        else if (strcmp(key, "building_pad_top") == 0) jumperlessConfig.logo_pads.building_pad_top = parseArbitraryFunction(value);
        else if (strcmp(key, "building_pad_bottom") == 0) jumperlessConfig.logo_pads.building_pad_bottom = parseArbitraryFunction(value);
        else if (strcmp(key, "repeat_ms") == 0) jumperlessConfig.logo_pads.repeat_ms = parseInt(value);
    }
    else if (strcmp(section, "display") == 0) {
        if (strcmp(key, "lines_wires") == 0) jumperlessConfig.display.lines_wires = parseLinesWires(value);
        else if (strcmp(key, "menu_brightness") == 0) jumperlessConfig.display.menu_brightness = parseInt(value);
        else if (strcmp(key, "led_brightness") == 0) jumperlessConfig.display.led_brightness = parseInt(value);
        else if (strcmp(key, "rail_brightness") == 0) jumperlessConfig.display.rail_brightness = parseInt(value);
        else if (strcmp(key, "special_net_brightness") == 0) jumperlessConfig.display.special_net_brightness = parseInt(value);
        else if (strcmp(key, "net_color_mode") == 0) jumperlessConfig.display.net_color_mode = parseNetColorMode(value);
        else if (strcmp(key, "net_currents") == 0) jumperlessConfig.display.net_currents = parseBool(value);
        else if (strcmp(key, "current_flow") == 0) jumperlessConfig.display.current_flow = parseCurrentFlow(value);
        else if (strcmp(key, "dump_leds") == 0) jumperlessConfig.display.dump_leds = parseSerialPort(value);
        else if (strcmp(key, "dump_format") == 0) jumperlessConfig.display.dump_format = parseDumpFormat(value);
        else if (strcmp(key, "terminal_line_buffering") == 0) jumperlessConfig.display.terminal_line_buffering = parseBool(value);
    }
    else if (strcmp(section, "serial_1") == 0) {
        if (strcmp(key, "function") == 0) jumperlessConfig.serial_1.function = parseUartFunction(value);
        else if (strcmp(key, "baud_rate") == 0) jumperlessConfig.serial_1.baud_rate = parseInt(value);
        else if (strcmp(key, "print_passthrough") == 0) jumperlessConfig.serial_1.print_passthrough = parseBool(value);
        else if (strcmp(key, "connect_on_boot") == 0) jumperlessConfig.serial_1.connect_on_boot = parseBool(value);
        else if (strcmp(key, "lock_connection") == 0) jumperlessConfig.serial_1.lock_connection = parseBool(value);
        else if (strcmp(key, "autoconnect_flashing") == 0) jumperlessConfig.serial_1.autoconnect_flashing = parseBool(value);
        else if (strcmp(key, "async_passthrough") == 0) jumperlessConfig.serial_1.async_passthrough = parseBool(value);
        else if (strcmp(key, "tag_parsing") == 0) jumperlessConfig.serial_1.tag_parsing = parseBool(value);
        else if (strcmp(key, "flash_reset_type") == 0) jumperlessConfig.serial_1.flash_reset_type = parseFlashType(value);
    }
    else if (strcmp(section, "serial_2") == 0) {
        if (strcmp(key, "function") == 0) jumperlessConfig.serial_2.function = parseUartFunction(value);
        else if (strcmp(key, "baud_rate") == 0) jumperlessConfig.serial_2.baud_rate = parseInt(value);
        else if (strcmp(key, "print_passthrough") == 0) jumperlessConfig.serial_2.print_passthrough = parseBool(value);
        else if (strcmp(key, "connect_on_boot") == 0) jumperlessConfig.serial_2.connect_on_boot = parseBool(value);
        else if (strcmp(key, "lock_connection") == 0) jumperlessConfig.serial_2.lock_connection = parseBool(value);
        else if (strcmp(key, "autoconnect_flashing") == 0) jumperlessConfig.serial_2.autoconnect_flashing = parseBool(value);
    }
    else if (strcmp(section, "top_oled") == 0) {
        if (strcmp(key, "enabled") == 0) jumperlessConfig.top_oled.enabled = parseBool(value);
        if (strcmp(key, "i2c_address") == 0) jumperlessConfig.top_oled.i2c_address = parseHex(value);
        else if (strcmp(key, "display_type") == 0) jumperlessConfig.top_oled.display_type = value; // Store string directly
        else if (strcmp(key, "width") == 0) jumperlessConfig.top_oled.width = parseInt(value);
        else if (strcmp(key, "height") == 0) jumperlessConfig.top_oled.height = parseInt(value);
        else if (strcmp(key, "rotation") == 0) jumperlessConfig.top_oled.rotation = parseInt(value);
        else if (strcmp(key, "connection_type") == 0) {
            int connType = parseConnectionType(value);
            // Single helper handles disconnect, pin update, save, and reinit.
            applyOledConnectionType(connType, /*reinitDisplay=*/true, /*persist=*/true);
        }
        else if (strcmp(key, "sda_pin") == 0) {
            oled.disconnect();
            jumperlessConfig.top_oled.sda_pin = parseInt(value);
            delay(50);
            oled.init();
        }
        else if (strcmp(key, "scl_pin") == 0) {
            oled.disconnect();
            jumperlessConfig.top_oled.scl_pin = parseInt(value);
            delay(50);
            oled.init();
        }
        else if (strcmp(key, "gpio_sda") == 0) jumperlessConfig.top_oled.gpio_sda = parseInt(value);
        else if (strcmp(key, "gpio_scl") == 0) jumperlessConfig.top_oled.gpio_scl = parseInt(value);
        else if (strcmp(key, "sda_row") == 0) jumperlessConfig.top_oled.sda_row = parseInt(value);
        else if (strcmp(key, "scl_row") == 0) jumperlessConfig.top_oled.scl_row = parseInt(value);
        else if (strcmp(key, "connect_on_boot") == 0) jumperlessConfig.top_oled.connect_on_boot = parseBool(value);
        else if (strcmp(key, "lock_connection") == 0) jumperlessConfig.top_oled.lock_connection = parseBool(value);
        else if (strcmp(key, "show_in_terminal") == 0) jumperlessConfig.top_oled.show_in_terminal = parseSerialPort(value);
        else if (strcmp(key, "font") == 0) {
            jumperlessConfig.top_oled.font = parseFont(value);
            
            // Apply font from config value (config value IS the FontFamily enum)
            if (jumperlessConfig.top_oled.font >= 0 && jumperlessConfig.top_oled.font <= FONT_PRAGMATISM) {
                FontFamily family = (FontFamily)jumperlessConfig.top_oled.font;
                oled.setFontForSize(family, 2);  // Use size 2 (large/12pt) as default
                oled.currentFontFamily = family;
            }
            oled.show();
        }
        else if (strcmp(key, "startup_message") == 0) {
            // Strip leading/trailing whitespace and quotes
            const char* start = value;
            size_t valueLen = strlen(value);
            if (valueLen == 0) {
                jumperlessConfig.top_oled.startup_message[0] = '\0';
            } else {
                const char* end = value + valueLen - 1;
                
                // Skip leading whitespace and quotes
                while (*start && (isspace((unsigned char)*start) || *start == '"' || *start == '\'')) {
                    start++;
                }
                
                // Skip trailing whitespace and quotes
                while (end > start && (isspace((unsigned char)*end) || *end == '"' || *end == '\'')) {
                    end--;
                }
                
                // Calculate length and copy
                size_t len = (size_t)(end - start + 1);
                if (len > 32) len = 32;
                
                strncpy(jumperlessConfig.top_oled.startup_message, start, len);
                jumperlessConfig.top_oled.startup_message[len] = '\0';
            }
        }
    }
    bool skipSave = (strcmp(section, "dacs") == 0 && strcmp(key, "auto_connect_probe") == 0
                     && jumperlessConfig.dacs.auto_connect_probe == 0);
    if (!skipSave) {
        saveConfigToFile("/config.txt");
    }
    printSettingChange(section, key, oldValue, value);
    
    // If we changed terminal_line_buffering, notify the app (the config value is
    // already updated above; push it through the single control path so the app
    // is told once, only when the state actually changed).
    if (strcmp(section, "display") == 0 && strcmp(key, "terminal_line_buffering") == 0) {
        pushLineBufferingToApp();
    }
}

// Fast config parsing function optimized for tight loops
// Returns true if valid config setting was parsed and updated, false otherwise
// Designed to return quickly for invalid strings to minimize loop overhead
//
// Usage example in a tight loop:
// while (someCondition) {
//     char* inputString = getNextString(); // Your string source
//     if (fastParseAndUpdateConfig(inputString)) {
//         // Config was successfully updated
//         Serial.println("Config updated");
//     }
//     // Function returns quickly for invalid strings, minimizing loop overhead
// }
//
// Supported formats:
// - Dot notation: "config.section.key = value"
// - Bracket notation: "`[section]key = value"
bool fastParseAndUpdateConfig(const char* configString) {
    // Quick validation - must have minimum length and contain '='
    if (!configString || strlen(configString) < 5) {
        Serial.println("too short");
        return false;
    }
    
    const char* equals = strchr(configString, '=');
    if (!equals) {
        Serial.println("no equals");
        return false;
    }
    
    // Quick check for valid config formats
    bool isDotNotation = (strncmp(configString, "config.", 7) == 0);
    bool isBracketNotation = (configString[0] == '`' && configString[1] == '[');
    
    if (!isDotNotation && !isBracketNotation) {
       // Serial.println(configString);
        Serial.println("not a dot or bracket notation");
        Serial.println(configString);
        return false;
    }
    
    // Use existing parsing logic but with early returns for efficiency
    char section[32], key[32], value[64];
    
    if (isDotNotation) {
        // Parse dot notation: config.section.key = value
        const char* start = configString + 7;  // Skip "config."
        const char* firstDot = strchr(start, '.');
        
        if (!firstDot || firstDot >= equals) {
            Serial.println("not a valid dot notation");
            return false;
        }
        
        // Extract section
        int sectionLen = firstDot - start;
        if (sectionLen >= sizeof(section)) {
            Serial.println("section too long");
            return false;
        }
        strncpy(section, start, sectionLen);
        section[sectionLen] = '\0';
        
        // Extract key
        const char* keyStart = firstDot + 1;
        int keyLen = equals - keyStart;
        if (keyLen >= sizeof(key) || keyLen <= 0) {
            Serial.println("key too long");
            return false;
        }
        strncpy(key, keyStart, keyLen);
        key[keyLen] = '\0';
        trim(key);
        
        // Extract value
        const char* valueStart = equals + 1;
        while (isspace(*valueStart)) valueStart++; // Skip leading whitespace
        if (strlen(valueStart) >= sizeof(value)) {
            Serial.println("value too long");
            return false;
        }
        strcpy(value, valueStart);
        
        // Trim trailing whitespace and semicolon from value
        char* end = value + strlen(value) - 1;
        while (end > value && (isspace(*end) || *end == ';')) {
            *end = '\0';
            end--;
        }
        
    } else if (isBracketNotation) {
        // Parse bracket notation: [section]key = value
        const char* sectionEnd = strchr(configString, ']');
        if (!sectionEnd || sectionEnd >= equals) {
            Serial.println("not a valid bracket notation");
            return false;
        }
        
        // Extract section (skip the `[)
        int sectionLen = sectionEnd - configString - 2;  // -2 to account for `[
        if (sectionLen >= sizeof(section) || sectionLen <= 0) {
            Serial.println("section too long");
            return false;
        }
        strncpy(section, configString + 2, sectionLen);
        section[sectionLen] = '\0';
        
        // Extract key (skip the ])
        const char* keyStart = sectionEnd + 1;
        while (isspace(*keyStart)) keyStart++; // Skip leading whitespace
        
        int keyLen = equals - keyStart;
        if (keyLen >= sizeof(key) || keyLen <= 0) {
            Serial.println("key too long");
            return false;
        }
        strncpy(key, keyStart, keyLen);
        key[keyLen] = '\0';
        
        // Remove trailing whitespace from key
        char* keyEnd = key + strlen(key) - 1;
        while (keyEnd > key && isspace(*keyEnd)) {
            *keyEnd = '\0';
            keyEnd--;
        }
        
        // Extract value (skip the =)
        const char* valueStart = equals + 1;
        while (isspace(*valueStart)) valueStart++; // Skip leading whitespace
        if (strlen(valueStart) >= sizeof(value)) {
            Serial.println("value too long");
                    return false;
        }
        strcpy(value, valueStart);
        
        // Trim trailing whitespace and semicolon from value
        char* end = value + strlen(value) - 1;
        while (end > value && (isspace(*end) || *end == ';')) {
            *end = '\0';
            end--;
        }
    }
    
    // Quick validation that we have non-empty section, key, and value
    if (strlen(section) == 0 || strlen(key) == 0 || strlen(value) == 0) {
        Serial.println("section, key, or value is empty");
        return false;
    }
    
    // Convert section to lowercase for comparison
    for(int i = 0; section[i]; i++) {
        section[i] = tolower(section[i]);
    }
    
    // Quick section validation - only proceed if it's a known section
    if (strcmp(section, "config") != 0 &&
        strcmp(section, "hardware") != 0 && 
        strcmp(section, "dacs") != 0 && 
        strcmp(section, "debug") != 0 && 
        strcmp(section, "routing") != 0 && 
        strcmp(section, "calibration") != 0 && 
        strcmp(section, "logo_pads") != 0 && 
        strcmp(section, "display") != 0 && 
        strcmp(section, "gpio") != 0 && 
        strcmp(section, "serial_1") != 0 && 
        strcmp(section, "serial_2") != 0 && 
        strcmp(section, "top_oled") != 0 &&
        strcmp(section, "usb_cdc") != 0) {
        Serial.println("section not found");
        Serial.println(section);
        return false;
    }
    
    // Update the config value using existing function
    //updateConfigValue(section, key, value);
    // Serial.print("section: ");
    // Serial.println(section);
    // Serial.print("key: ");
    // Serial.println(key);
    // Serial.print("value: ");
    // Serial.println(value);
    updateConfigValue(section, key, value);
    configChanged = true;
    return true;
}

const char* getArbitraryFunctionString(int function) {
    for (int i = 0; i < arbitraryFunctionTableSize; i++) {
        if (arbitraryFunctionTable[i].value == function) {
            return arbitraryFunctionTable[i].name;
        }
    }
    return NULL; // or some default string
}

template <size_t N>
const char* getStringFromTable(int value, const StringIntEntry (&table)[N]) {
    if (showNames) {
        for (size_t i = 0; i < N; i++) {
            if (table[i].value == value) {
                // Serial.print("getStringFromTable: ");
                // Serial.println(table[i].name);
                return table[i].name;
            }
        }
    } else {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%d", value);
        return buf;
    }
    return NULL; // or some default string
}
