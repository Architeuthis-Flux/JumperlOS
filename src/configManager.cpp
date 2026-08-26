// ============================================================================
// Table-driven config manager.
//
// The option list lives in config.h (JL_CONFIG_ALL_OPTIONS) - ONE X() line
// per option generates the struct field AND the descriptor entry below, which
// drives parse, save, print, diff, help text, the config TUI, and migration.
// Renamed/moved keys stay parseable through jlConfigAliases; removed keys
// parse as silent no-ops so old config.txt files and pasted lines never
// error out.
// ============================================================================

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
#include "USBAudio.h"              // For restoring the saved USB audio setup
#include "AsyncPassthrough.h"
#include "Probing.h"
#include "Python_Proper.h"
#include "SelfTest.h"
#include "FileCache.h"  // fileCacheFlushNow / fileCacheSpiftlSync - force config durable
#include "InfraPaths.h" // infraNudge - re-evaluate the probe feed on probe.power_source
#include "eyecandy/MenuTransitions.h" // menuTransitionConfig <-> [clickwheel] fx_*

#ifdef DONOTUSE_SERIALWRAPPER
    #include "SerialWrapper.h"
    #define Serial SerialWrap
#endif

// Which I2C port a pin can serve, and in which role - a mirror of the
// gpioI2Cmap table initI2C() consults (src/Peripherals.cpp), so keep the two
// in sync. role 0 = SDA, 1 = SCL. Returns the port (0 or 1), or -1 if the pin
// cannot serve that role at all.
static int oledI2CPortForPin(int pin, int role) {
    static const int i2cPinPorts[14][3] = {
        {  0, 0, 0 }, {  1, 1, 0 }, {  4, 0, 0 }, {  5, 1, 0 },
        {  6, 0, 1 }, {  7, 1, 1 }, { 20, 0, 0 }, { 21, 1, 0 },
        { 22, 0, 1 }, { 23, 1, 1 }, { 24, 0, 0 }, { 25, 1, 0 },
        { 26, 0, 1 }, { 27, 1, 1 }
    };
    for (int i = 0; i < 14; i++) {
        if (i2cPinPorts[i][0] == pin && i2cPinPorts[i][1] == role) {
            return i2cPinPorts[i][2];
        }
    }
    return -1;
}

// Define the global configuration instance

bool configChanged = false;
bool autoCalibrationNeeded = false;
// Set by migration when the probe droop calibration has never run
// (probe.droop_ohms == 0) - main loop prompts for Switch Calib / tip test.
bool probeCalibrationNeeded = false;
// Flag for async config save - set true to request background save
volatile bool configSavePending = false;

struct config jumperlessConfig;
// Shadow copy of last saved config for dirty tracking (avoids unnecessary writes)
struct config lastSavedConfig;
bool shadowConfigValid = false;

// A pristine defaults instance - "reset to default" copies out of this, so
// defaults live in exactly one place (the struct initializers config.h
// generates from the option list).
static const struct config jlConfigDefaults;

// ============================================================================
// THE descriptor table - second expansion of the single option list.
// ============================================================================
#define JL_XDESC(sect, key, type, def, minv, maxv, step, table, hook, flags, desc) \
    { JLSECT_##sect, #key, JLT_##type, (void*)&jumperlessConfig.sect.key, \
      (float)(minv), (float)(maxv), (float)(step), jlTableRef(table), \
      (uint8_t)(hook), (uint16_t)(flags), desc },

const ConfigOptionDesc jlConfigOptions[] = {
    JL_CONFIG_ALL_OPTIONS(JL_XDESC)
};
const int jlConfigOptionCount = (int)(sizeof(jlConfigOptions) / sizeof(jlConfigOptions[0]));

const ConfigSectionDesc jlConfigSections[JLSECT_COUNT] = {
    { "firmware",    "Firmware",     "Update tracking (bookkeeping - hidden)." },
    { "hardware",    "Hardware",     "Board identity and memory layout; preserved across resets." },
    { "probe",       "Probe",        "Probe behavior, power feed, and calibration." },
    { "clickwheel",  "Clickwheel",   "Rotary encoder behavior and menu transition eye candy." },
    { "measurement", "Measurement",  "Net voltage/current scanning and how results are shown." },
    { "terminal",    "Terminal",     "Serial terminal colors and input buffering." },
    { "undo",        "Undo",         "Connection-history undo log persistence." },
    { "dacs",        "DACs",         "DAC and rail output voltage limits." },
    { "debug",       "Debug",        "Debug print gates (the single source of truth)." },
    { "routing",     "Routing",      "Crosspoint path stacking for lower resistance." },
    { "slots",       "Slots",        "Which slot the board boots into." },
    { "calibration", "Calibration",  "DAC/rail/ADC transfer curves (written by the DAC calibration)." },
    { "logo_pads",   "Logo Pads",    "Functions bound to the capacitive logo/building pads." },
    { "display",     "Display",      "Breadboard LED rendering and brightness." },
    { "serial_1",    "Serial 1",     "UART 1 role, baud, and passthrough behavior." },
    { "serial_2",    "Serial 2",     "UART 2 role, baud, and passthrough behavior." },
    { "top_oled",    "OLED",         "The top OLED: wiring, font, and boot behavior." },
    { "usb_cdc",     "USB CDC",      "USB serial port flow control quirks." },
    { "usb_audio",   "USB Audio",    "USB microphone streaming ADC channels to the host." },
};

// Old `[section] key` -> new home. Parsed transparently everywhere (file
// load, pasted lines, dot notation), so old config files and muscle memory
// keep working. Only renamed/moved keys need entries; removed keys just
// fall through as no-ops.
const ConfigAlias jlConfigAliases[] = {
    { "dacs", "auto_connect_probe",               JLSECT_probe,       "auto_connect" },
    { "dacs", "probe_power_source",               JLSECT_probe,       "power_source" },
    { "dacs", "rail_click_adjust",                JLSECT_clickwheel,  "rail_click_adjust" },
    { "hardware", "use_pio_probe_button",         JLSECT_probe,       "use_pio_button" },
    { "hardware", "probe_led_on_button_pin",      JLSECT_probe,       "led_on_button_pin" },
    { "hardware", "probe_led_refresh_us",         JLSECT_probe,       "led_refresh_us" },
    { "hardware", "encoder_pio",                  JLSECT_clickwheel,  "encoder_pio" },
    { "calibration", "probe_max",                 JLSECT_probe,       "pad_max" },
    { "calibration", "probe_min",                 JLSECT_probe,       "pad_min" },
    { "calibration", "probe_max_measure",         JLSECT_probe,       "pad_max_measure" },
    { "calibration", "probe_max_measure_gpio",    JLSECT_probe,       "pad_max_measure_gpio" },
    { "calibration", "probe_min_measure",         JLSECT_probe,       "pad_min_measure" },
    { "calibration", "probe_switch_threshold_high", JLSECT_probe,     "switch_threshold_high" },
    { "calibration", "probe_switch_threshold_low",  JLSECT_probe,     "switch_threshold_low" },
    { "calibration", "probe_switch_select_max_ma",  JLSECT_probe,     "switch_select_max_ma" },
    { "calibration", "probe_switch_blink_hold_pct", JLSECT_probe,     "switch_blink_hold_pct" },
    { "calibration", "measure_mode_output_voltage", JLSECT_probe,     "measure_voltage" },
    { "calibration", "probe_current_zero",        JLSECT_probe,       "current_zero" },
    { "calibration", "minimum_probe_reading",     JLSECT_probe,       "min_valid_reading" },
    { "calibration", "probe_droop_v0",            JLSECT_probe,       "droop_v0" },
    { "calibration", "probe_droop_ohms",          JLSECT_probe,       "droop_ohms" },
    { "calibration", "probe_pad_ohms",            JLSECT_probe,       "pad_ohms" },
    { "calibration", "crosspoint_resistance",     JLSECT_measurement, "crosspoint_resistance" },
    { "display", "net_currents",                  JLSECT_measurement, "net_currents" },
    { "display", "current_flow",                  JLSECT_measurement, "current_flow" },
    { "debug", "show_probe_current",              JLSECT_measurement, "show_probe_current" },
    { "display", "terminal_line_buffering",       JLSECT_terminal,    "line_buffering" },
};
const int jlConfigAliasCount = (int)(sizeof(jlConfigAliases) / sizeof(jlConfigAliases[0]));

// ============================================================================
// ConfigSaveService - Background config save service
// ============================================================================
ConfigSaveService& ConfigSaveService::getInstance() {
    static ConfigSaveService inst;
    return inst;
}

// Request async config save (non-blocking). The service has a 100 ms period;
// requestRun() makes an explicit request land on the very next pass anyway.
void requestConfigSave() {
    configSavePending = true;
    ConfigSaveService::getInstance().requestRun();
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
    // mid-tap: the core-1 pause held up to ~700ms per save with storms of several
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

const char* getStringFromTableRef(int value, const JlTableRef& ref) {
    for (int i = 0; i < ref.n; i++) {
        if (ref.t[i].value == value) return ref.t[i].name;
    }
    return nullptr;
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

// ============================================================================
// Generic option access
// ============================================================================

static size_t cfgFieldSize(const ConfigOptionDesc* o) {
    switch (o->type) {
        case JLT_BOOL:  return sizeof(bool);
        case JLT_FLOAT: return sizeof(float);
        case JLT_STR16: return 16;
        case JLT_STR33: return 33;
        default:        return sizeof(int);
    }
}

static int cfgGetInt(const ConfigOptionDesc* o) {
    switch (o->type) {
        case JLT_BOOL:  return *(bool*)o->ptr ? 1 : 0;
        case JLT_VINT:  return *(volatile int*)o->ptr;
        case JLT_FLOAT: return (int)(*(float*)o->ptr);
        default:        return *(int*)o->ptr;
    }
}

static void cfgSetIntRaw(const ConfigOptionDesc* o, int v) {
    switch (o->type) {
        case JLT_BOOL:  *(bool*)o->ptr = (v != 0); break;
        case JLT_VINT:  *(volatile int*)o->ptr = v; break;
        case JLT_FLOAT: *(float*)o->ptr = (float)v; break;
        default:        *(int*)o->ptr = v; break;
    }
}

int configSectionFromName(const char* sectionName) {
    if (strcmp(sectionName, "config") == 0) return -2; // virtual version stamp
    for (int i = 0; i < JLSECT_COUNT; i++) {
        if (strcmp(sectionName, jlConfigSections[i].name) == 0) return i;
    }
    return -1;
}

const ConfigOptionDesc* configFindOption(uint8_t section, const char* key) {
    for (int i = 0; i < jlConfigOptionCount; i++) {
        if (jlConfigOptions[i].section == section && strcmp(jlConfigOptions[i].key, key) == 0) {
            return &jlConfigOptions[i];
        }
    }
    return nullptr;
}

const ConfigOptionDesc* configFindOption(const char* sectionName, const char* key) {
    int sect = configSectionFromName(sectionName);
    if (sect >= 0) {
        const ConfigOptionDesc* opt = configFindOption((uint8_t)sect, key);
        if (opt) return opt;
    }
    // Renamed/moved keys: resolve through the alias map.
    for (int i = 0; i < jlConfigAliasCount; i++) {
        if (strcmp(jlConfigAliases[i].oldSection, sectionName) == 0 &&
            strcmp(jlConfigAliases[i].oldKey, key) == 0) {
            return configFindOption(jlConfigAliases[i].section, jlConfigAliases[i].newKey);
        }
    }
    return nullptr;
}

// Format an option's current value. names=true prints enum/bool names
// ("true", "passthrough", "uart_tx"); names=false prints raw numbers.
void configFormatValue(const ConfigOptionDesc* opt, char* out, size_t outLen, bool names) {
    out[0] = '\0';
    switch (opt->type) {
        case JLT_STR16:
        case JLT_STR33:
            snprintf(out, outLen, "%s", (const char*)opt->ptr);
            return;
        case JLT_FLOAT: {
            // Calibration values get extra digits so a save/load round trip
            // doesn't quietly shave precision off a measured constant.
            int dec = (opt->flags & JLC_CAL) ? 4 : 2;
            snprintf(out, outLen, "%.*f", dec, (double)(*(float*)opt->ptr));
            return;
        }
        case JLT_HEX:
            snprintf(out, outLen, "0x%02X", (unsigned)cfgGetInt(opt));
            return;
        case JLT_FONT:
            snprintf(out, outLen, "%s", getFontString(cfgGetInt(opt)));
            return;
        case JLT_BOOL:
            if (names) snprintf(out, outLen, "%s", cfgGetInt(opt) ? "true" : "false");
            else snprintf(out, outLen, "%d", cfgGetInt(opt));
            return;
        default: {
            int v = cfgGetInt(opt);
            if (names && opt->table.t != nullptr) {
                const char* name = getStringFromTableRef(v, opt->table);
                if (name) { snprintf(out, outLen, "%s", name); return; }
            }
            snprintf(out, outLen, "%d", v);
            return;
        }
    }
}

// Copy a string value in with whitespace/quote stripping (startup_message
// paths and version stamps both want this).
static void cfgSetString(const ConfigOptionDesc* o, const char* value) {
    size_t cap = cfgFieldSize(o);
    char* dst = (char*)o->ptr;
    const char* start = value;
    size_t valueLen = strlen(value);
    if (valueLen == 0) { dst[0] = '\0'; return; }
    const char* end = value + valueLen - 1;
    while (*start && (isspace((unsigned char)*start) || *start == '"' || *start == '\'')) start++;
    while (end > start && (isspace((unsigned char)*end) || *end == '"' || *end == '\'')) end--;
    size_t len = (size_t)(end - start + 1);
    if (len > cap - 1) len = cap - 1;
    strncpy(dst, start, len);
    dst[len] = '\0';
}

// Sync [clickwheel] fx_* into the live (volatile, cross-core) transition
// config. Called from the hook on live changes and from boot apply.
void configApplyMenuFx(void) {
    int t = jumperlessConfig.clickwheel.fx_type;
    if (t < 0 || t >= MENU_TRANSITION_TYPE_COUNT) t = MENU_TRANSITION_GLOW;
    menuTransitionConfig.type = (uint8_t)t;
    int d = jumperlessConfig.clickwheel.fx_duration_ms;
    if (d < 0) d = 0; if (d > 1000) d = 1000;
    menuTransitionConfig.durationMs = (uint16_t)d;
    menuTransitionConfig.tintColor = (uint32_t)jumperlessConfig.clickwheel.fx_tint & 0xFFFFFF;
    int dens = jumperlessConfig.clickwheel.fx_density;
    if (dens < 0) dens = 0; if (dens > 255) dens = 255;
    menuTransitionConfig.density = (uint8_t)dens;
}

// The Menu FX tuner mutates menuTransitionConfig directly; call this on its
// way out to persist what the user dialed in.
void configCaptureMenuFx(void) {
    jumperlessConfig.clickwheel.fx_type = menuTransitionConfig.type;
    jumperlessConfig.clickwheel.fx_duration_ms = menuTransitionConfig.durationMs;
    jumperlessConfig.clickwheel.fx_tint = (int)(menuTransitionConfig.tintColor & 0xFFFFFF);
    jumperlessConfig.clickwheel.fx_density = menuTransitionConfig.density;
    configChanged = true;
}

// Set an option from a string value. liveApply runs the option's side-effect
// hook (file loads pass false; boot apply happens in loadConfig /
// readSettingsFromConfig). Returns false if the change was REFUSED (illegal
// OLED pin pair) - in that case nothing was written.
bool configSetValue(const ConfigOptionDesc* opt, const char* value, bool liveApply) {
    int prevInt = (opt->type == JLT_STR16 || opt->type == JLT_STR33) ? 0 : cfgGetInt(opt);

    // --- hooks that own the whole assignment -------------------------------
    if (opt->hook == HOOK_OLED_CONNECTION) {
        int connType = parseConnectionType(value);
        if (liveApply) {
            // Single helper handles disconnect, pin update, save, and reinit.
            applyOledConnectionType(connType, /*reinitDisplay=*/true, /*persist=*/true);
        } else {
            updateOledPinsForConnectionType(connType);
        }
        return true;
    }
    if (opt->hook == HOOK_OLED_PIN && liveApply) {
        // arduino-pico PANICS on setSDA/setSCL of a RUNNING Wire with
        // different pins, and oled.init() does exactly that. Two rules,
        // in order:
        //   1. Reject pins initI2C() cannot program. Each pin is legal for
        //      exactly ONE port in one role, so both pins have to resolve
        //      to the SAME port that gpioI2Cmap picks.
        //   2. Only re-pin a bus that is the OLED's ALONE - that is I2C1.
        //      A pin change on I2C0 is refused outright (see below).
        int newPin = parseInt(value);
        bool wantSda = (strcmp(opt->key, "sda_pin") == 0);
        int newSda = wantSda ? newPin : jumperlessConfig.top_oled.sda_pin;
        int newScl = wantSda ? jumperlessConfig.top_oled.scl_pin : newPin;
        int sdaPort = oledI2CPortForPin(newSda, 0);
        int sclPort = oledI2CPortForPin(newScl, 1);
        if (sdaPort < 0 || sclPort < 0 || sdaPort != sclPort) {
            Serial.print("  refused: sda ");
            Serial.print(newSda);
            Serial.print(" / scl ");
            Serial.print(newScl);
            Serial.println(" is not a legal pair on one I2C port");
            Serial.println("  I2C0: sda 0/4/20/24 with scl 1/5/21/25   I2C1: sda 6/22/26 with scl 7/23/27");
            return false; // refused - don't save or echo a change that didn't happen
        }
        // I2C0 is the SYSTEM bus: the MCP4728 DAC (0x60) and both INA219s
        // (0x40/0x41) come up on it at boot (initDAC/initINA219 begin Wire on
        // GPIO 4/5) and wavegen streams the DAC on it. Re-pinning it live means
        // Wire.end()/setSDA() underneath devices that are using it - the
        // teardown teardownOldOledBus() documents as forbidden (it orphans the
        // DAC and both current sensors until reboot), and the
        // setSDA-while-running panic the pair check above only half-avoids.
        // Persisting it for the next boot would be WORSE, not safer: Wire is
        // already up on 4/5 by the time oled.init() runs, so any port-0 pair
        // other than the hardwired (4,5) panics on EVERY boot, out of flash.
        // And there is nothing useful to persist - the internal bus is
        // hardwired. So refuse, and point at the control that does move the
        // OLED. Keyed on the port the NEW pins resolve to (what initI2C() will
        // program), with connection_type 2 as the second half: a display
        // already talking on Wire must not have its bus moved either.
        if (sdaPort == 0 || jumperlessConfig.top_oled.connection_type == 2) {
            Serial.println("  refused: the internal I2C0 OLED bus is hardwired to GPIO 4/5,");
            Serial.println("  shared with the DAC and both current sensors - it can't be re-pinned");
            Serial.println("  use top_oled.connection_type to move the OLED to another bus");
            return false;
        }
        // I2C1 is the OLED's alone, so ending it and re-pinning it is safe and
        // applies now. Unconditional Wire1.end(): the pair check proved the new
        // pins are port 1, and end() returns early on a bus nobody started -
        // i2cScan() can leave Wire1 running even when the OLED lives elsewhere,
        // which is exactly the case that panicked before.
        oled.disconnect();
        Wire1.end();
        delay(50);
        *(int*)opt->ptr = newPin;
        delay(50);
        oled.init();
        return true;
    }

    // --- generic parse + store ---------------------------------------------
    switch (opt->type) {
        case JLT_STR16:
        case JLT_STR33:
            cfgSetString(opt, value);
            break;
        case JLT_FLOAT: {
            float v = parseFloat(value);
            if (!(opt->minv == 0.0f && opt->maxv == 0.0f)) {
                if (v < opt->minv) v = opt->minv;
                if (v > opt->maxv) v = opt->maxv;
            }
            *(float*)opt->ptr = v;
            break;
        }
        case JLT_HEX: {
            int v = parseHex(value);
            if (!(opt->minv == 0.0f && opt->maxv == 0.0f)) {
                if (v < (int)opt->minv) v = (int)opt->minv;
                if (v > (int)opt->maxv) v = (int)opt->maxv;
            }
            *(int*)opt->ptr = v;
            break;
        }
        case JLT_FONT:
            *(int*)opt->ptr = parseFont(value);
            break;
        case JLT_BOOL:
            *(bool*)opt->ptr = parseBool(value);
            break;
        default: {
            int v;
            if (opt->hook == HOOK_LINES_WIRES) v = parseLinesWires(value);
            else if (opt->table.t != nullptr) v = parseFromTable(opt->table.t, opt->table.n, value);
            else v = parseInt(value);
            if (!(opt->minv == 0.0f && opt->maxv == 0.0f)) {
                if (v < (int)opt->minv) v = (int)opt->minv;
                if (v > (int)opt->maxv) v = (int)opt->maxv;
            }
            cfgSetIntRaw(opt, v);
            break;
        }
    }

    // --- side-effect hooks (live changes only) ------------------------------
    switch (opt->hook) {
        case HOOK_ENCODER_PIO: {
            // -1 = auto (PIO2 then PIO1, PIO0 last); 0..2 = try that block
            // first. Applied at the next boot (core 1 claims before live
            // changes could land).
            int v = cfgGetInt(opt);
            if (v < -1 || v > 2) cfgSetIntRaw(opt, -1);
            break;
        }
        case HOOK_LED_REFRESH_US:
            if (cfgGetInt(opt) < 0) cfgSetIntRaw(opt, 0);
            break;
        case HOOK_UNDO:
            // Persist cap is bounded by the static ring in Undo.cpp.
            if (strcmp(opt->key, "max_saved_actions") == 0) {
                int v = cfgGetInt(opt);
                if (v < 1) v = 1;
                if (v > 256) v = 256;
                cfgSetIntRaw(opt, v);
            }
            break;
        default: break;
    }

    if (liveApply) {
        switch (opt->hook) {
            case HOOK_PSRAM:
                applyPsramModeChange(cfgGetInt(opt));
                reinitMicroPythonForPsramChange();
                break;
            case HOOK_PROBE_AUTOCONNECT:
                // Symmetric with jl_probe_autoconnect(): turning it back on must
                // re-enable the feed too - it used to leave s_probePowerOn false,
                // so auto_connect = 1 after a 0 left the probe unpowered until
                // reboot.
                if (jumperlessConfig.probe.auto_connect <= 0) {
                    probing.routableBufferPower(0, 0, 1);
                } else {
                    probing.routableBufferPower(1, 0, 1);
                }
                break;
            case HOOK_PROBE_POWER_SOURCE: {
                int v = cfgGetInt(opt);
                if (v != 0 && v != 1) { v = 0; cfgSetIntRaw(opt, v); }
                // The order is read at every evaluation; a change only becomes
                // real at the next rebuild, so ask for one now instead of
                // waiting for an unrelated connect/disconnect.
                if (v != prevInt) infraNudge();
                break;
            }
            case HOOK_USB_CDC_DTR:
                usb_cdc_set_ignore_dtr(jumperlessConfig.usb_cdc.ignore_dtr);
                AsyncPassthrough::setDTRLockout(3000);
                usb_cdc_apply_config();
                break;
            case HOOK_OLED_FONT: {
                // Apply font from config value (config value IS the FontFamily enum)
                int f = jumperlessConfig.top_oled.font;
                if (f >= 0 && f <= FONT_PRAGMATISM) {
                    FontFamily family = (FontFamily)f;
                    oled.setFontForSize(family, 2);  // size 2 (large/12pt) default
                    oled.currentFontFamily = family;
                }
                oled.show();
                break;
            }
            case HOOK_LINE_BUFFERING:
                pushLineBufferingToApp();
                break;
            case HOOK_SERIAL_FUNCTION:
                initArduino();
                break;
            case HOOK_MENU_FX:
                configApplyMenuFx();
                break;
            case HOOK_TERM_COLORS:
                disableTerminalColors = !jumperlessConfig.terminal.colors;
                break;
            default: break;
        }
        // Settings that change what's on the board: push the config into the
        // runtime globals and repaint.
        if (opt->flags & JLC_LEDS) {
            readSettingsFromConfig();
            requestLedShow(-1);
        }
    }
    return true;
}

void configResetOptionToDefault(const ConfigOptionDesc* opt, bool liveApply) {
    // Format the DEFAULT value, then set it like any other change so hooks
    // fire and clamps apply.
    size_t off = (size_t)((char*)opt->ptr - (char*)&jumperlessConfig);
    ConfigOptionDesc defOpt = *opt;
    defOpt.ptr = (void*)((const char*)&jlConfigDefaults + off);
    char defVal[64];
    configFormatValue(&defOpt, defVal, sizeof(defVal), true);
    configSetValue(opt, defVal, liveApply);
}

bool configOptionIsDefault(const ConfigOptionDesc* opt) {
    size_t off = (size_t)((char*)opt->ptr - (char*)&jumperlessConfig);
    return memcmp(opt->ptr, (const char*)&jlConfigDefaults + off, cfgFieldSize(opt)) == 0;
}

void resetConfigToDefaults(int clearCalibration, int clearHardware) {
    // Preserve hardware by struct copy and calibration by JLC_CAL flag - the
    // flag can't forget a field the way the old hand-maintained list did
    // (probe droop values silently zeroed on every reset until 2026-08-16).
    struct config saved = jumperlessConfig;

    // Initialize with default values from config.h
    jumperlessConfig = config();

    // Bookkeeping, not a user setting: keep update detection working.
    memcpy(jumperlessConfig.firmware.last_version, saved.firmware.last_version,
           sizeof(jumperlessConfig.firmware.last_version));

    if (clearHardware == 0) {
        jumperlessConfig.hardware = saved.hardware;
    }
    if (clearCalibration == 0) {
        for (int i = 0; i < jlConfigOptionCount; i++) {
            const ConfigOptionDesc* opt = &jlConfigOptions[i];
            if (!(opt->flags & JLC_CAL)) continue;
            size_t off = (size_t)((char*)opt->ptr - (char*)&jumperlessConfig);
            memcpy(opt->ptr, (const char*)&saved + off, cfgFieldSize(opt));
        }
    }
    // Zero-sentinel fixup for a board that never calibrated the pads (the
    // copy above would restore the zeros).
    if (jumperlessConfig.probe.pad_min == 0 || jumperlessConfig.probe.pad_max == 0) {
        jumperlessConfig.probe.pad_min = 15;
        jumperlessConfig.probe.pad_max = 4040;
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

    // NOTE: Don't call saveConfig() here - callers are responsible for saving
    // after they've had a chance to restore user settings they want to preserve
}

// ============================================================================
// File load (+ firmware-version migration)
// ============================================================================

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
    
    bool foundConfigVersion = false;
    char configFirmwareVersion[16] = {0};
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
        // Bounded copies: line[] is 128 bytes but key/value are 32/64 -
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
            trim(value);
        }
        
        toLower(key);

        if (strcmp(section, "config") == 0) {
            if (strcmp(key, "firmware_version") == 0) {
                strncpy(configFirmwareVersion, value, sizeof(configFirmwareVersion)-1);
                configFirmwareVersion[sizeof(configFirmwareVersion)-1] = '\0';
                foundConfigVersion = true;
            }
            continue;
        }

        // Everything else: descriptor table (alias-aware). Unknown/removed
        // keys are silent no-ops so old files load cleanly.
        const ConfigOptionDesc* opt = configFindOption(section, key);
        if (opt) {
            configSetValue(opt, value, /*liveApply=*/false);
        }
    }
    safeFileClose(file, false);  // Read-only, no flush

    // ------------------------------------------------------------------
    // Firmware-version migration. Any version change resets everything to
    // defaults EXCEPT hardware identity and JLC_CAL-flagged calibration -
    // the defaults are the correct values for everything else, and this is
    // what makes recategorized/renamed keys land cleanly in the new format.
    // ------------------------------------------------------------------
    bool versionChanged = !foundConfigVersion ||
                          (strcmp(configFirmwareVersion, firmwareVersion) != 0);
    if (versionChanged && newConfigOptions) {
        Serial.print("Config file is from firmware ");
        Serial.print(foundConfigVersion ? configFirmwareVersion : "(unversioned)");
        Serial.print("; running ");
        Serial.print(firmwareVersion);
        Serial.println(". Migrating: keeping hardware + calibration, defaults for the rest.");

        resetConfigToDefaults(0, 0);  // keeps hardware struct + JLC_CAL options

        // The probe droop calibration writes droop_ohms; 0 is the firmware's
        // own "never ran" sentinel. Boards that predate the pad/switch
        // calibration should run it once after this update.
        if (jumperlessConfig.probe.droop_ohms == 0.0f) {
            probeCalibrationNeeded = true;
        }

        if (debugConfigSaveTiming) Serial.println("[ConfigSave] TRIGGER: firmware version migration");
        saveConfig();  // writes the new-format file (and re-syncs globals)
            newConfigOptions = false;
        return;
    }
    
    readSettingsFromConfig();
}

// ============================================================================
// Save
// ============================================================================

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

    // [config] version stamp
    file.println("[config]");
    file.print("firmware_version = "); file.print(firmwareVersion); file.println(";");
    file.println();

    // Every section, every option, straight from the table.
    char valBuf[64];
    for (int s = 0; s < JLSECT_COUNT; s++) {
        file.print("[");
        file.print(jlConfigSections[s].name);
        file.println("]");
        for (int i = 0; i < jlConfigOptionCount; i++) {
            const ConfigOptionDesc* opt = &jlConfigOptions[i];
            if (opt->section != s) continue;
            configFormatValue(opt, valBuf, sizeof(valBuf), /*names=*/true);
            file.print(opt->key);
            file.print(" = ");
            file.print(valBuf);
            file.println(";");
        }
    file.println();
    }
    
    // ponytail: f_write failures are sticky (volume-full stays full; hard I/O
    // errors set FIL.err and abort the file), so this final write failing
    // catches any earlier print() failure without checking all of them.
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

    updateShadowConfig();
    
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
    shadowConfigValid = true;
}

// Compare current config with last saved to detect changes - per-option
// through the table (handles floats and strings correctly, and can't drift
// from the struct).
bool configHasChanges() {
    if (!shadowConfigValid) return true;  // No shadow = need to save
    for (int i = 0; i < jlConfigOptionCount; i++) {
        const ConfigOptionDesc* opt = &jlConfigOptions[i];
        size_t off = (size_t)((char*)opt->ptr - (char*)&jumperlessConfig);
        if (opt->type == JLT_STR16 || opt->type == JLT_STR33) {
            if (strcmp((const char*)opt->ptr, (const char*)&lastSavedConfig + off) != 0) return true;
        } else {
            if (memcmp(opt->ptr, (const char*)&lastSavedConfig + off, cfgFieldSize(opt)) != 0) return true;
        }
    }
            return false;
}

bool debugConfigSaveTiming = false;
void setConfigSaveDebug(bool enable) {
    debugConfigSaveTiming = enable;
    if (enable) {
        Serial.println("[ConfigSave] Debug timing ENABLED");
        } else {
        Serial.println("[ConfigSave] Debug timing DISABLED");
    }
}

bool saveConfig(void) {
    if (jumperlessConfig.probe.pad_min == 0 || jumperlessConfig.probe.pad_max == 0) {
        jumperlessConfig.probe.pad_min = 15;
        jumperlessConfig.probe.pad_max = 4040;
    }

    // Diff gate: skip the flash write entirely when nothing changed.
    if (shadowConfigValid && !configHasChanges()) {
        if (debugConfigSaveTiming) Serial.println("[ConfigSave] No changes - skipping write");
        // Still keep EEPROM/global sync coherent for callers that expect it.
        readSettingsFromConfig();
        return true;
    }
    
    bool success = saveConfigToFile("/config.txt");

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
        // to config.txt silently reverts to the stale EEPROM value on the next
        // reboot. Mirror the kept fields and commit them here so the explicit save
        // is fully durable. Both calls self-gate, so ordinary saves pay nothing
        // extra.
        eepromPersistFromConfig();
        eepromCommitSafe();

        readSettingsFromConfig();
    }
    return success;
}

// ============================================================================
// Firmware versioning and file provisioning system
// ============================================================================

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
    // fragmented heap. Skip entirely.
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

    provisionEmbeddedFile("images/dayglow.bin", dayglow_bin, dayglow_bin_len);
    provisionEmbeddedFile("images/eevblog.bin", eevblog_bin, eevblog_bin_len);
    provisionEmbeddedFile("images/jogo32h.bin", jogo32h_bin, jogo32h_bin_len);
    provisionEmbeddedFile("images/jogotextInv.bin", jogotextInv_bin, jogotextInv_bin_len);
    provisionEmbeddedFile("images/jumperless_text.bin", jumperless_text_bin, jumperless_text_bin_len);
    provisionEmbeddedFile("images/ksc.bin", ksc_bin, ksc_bin_len);
    
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
    
    // Refresh built-in examples AND projects on every firmware update.
    // The hash system in initializeMicroPythonExamples protects user-edited files:
    // unmodified defaults are updated in-place; user-modified files are left alone
    // and a new firmware default is written as _original / _original1 / etc.
    if (debugFP) {
        Serial.println("  - Refreshing built-in MicroPython examples and projects "
                       "(preserving user edits)...");
    }
    initializeMicroPythonExamples(true);
    initializeProjects(true);   // same hash contract for /projects/<dir>/
    if (debugFP) {
        Serial.println("  ✓ Python examples and projects refreshed\n\r");
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
    
    if (isFirstBoot) {
        changeTerminalColor( 164, true );
        Serial.println("\n\r╔═══════════════════════════════════════╗");
        Serial.println("║  First Boot Detected                  ║");
        Serial.println("╚═══════════════════════════════════════╝\n\r");
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

    if (jumperlessConfig.probe.pad_min == 0 || jumperlessConfig.probe.pad_max == 0) {
        jumperlessConfig.probe.pad_min = 15;
        jumperlessConfig.probe.pad_max = 4060;
    }

    // Seed the MEASURE-position pad endpoints from the base (select) pair so
    // the config holds real, individually-adjustable numbers instead of a 0
    // sentinel. They are stored in the 3.3V frame; the decode scales them by
    // the live tip voltage (ADC7) at runtime, so seeding with the base pair
    // is correct for a fresh board.
    if (jumperlessConfig.probe.pad_min_measure <= 0) {
        jumperlessConfig.probe.pad_min_measure = jumperlessConfig.probe.pad_min;
    }
    if (jumperlessConfig.probe.pad_max_measure <= 0) {
        jumperlessConfig.probe.pad_max_measure = jumperlessConfig.probe.pad_max;
    }
    // The GPIO-fed endpoint seeds from the DAC-fed one: a board upgrading
    // from a single-endpoint firmware has a calibration that was taken on
    // whichever feed was live (almost always DAC0, the default), and the
    // convergence step in the probe calibration app is what separates them.
    if (jumperlessConfig.probe.pad_max_measure_gpio <= 0) {
        jumperlessConfig.probe.pad_max_measure_gpio =
            jumperlessConfig.probe.pad_max_measure;
    }

    // Feed-blink hold percentage: a ratio, so 1..99; anything else is a
    // corrupt/hand-edited file - back to the physics-derived default.
    if (jumperlessConfig.probe.switch_blink_hold_pct < 1 ||
        jumperlessConfig.probe.switch_blink_hold_pct > 99) {
        jumperlessConfig.probe.switch_blink_hold_pct = 50;
    }
    // A negative select ceiling means "disabled" was mistyped; 0 is the
    // real disabled value.
    if (jumperlessConfig.probe.switch_select_max_ma < 0.0f) {
        jumperlessConfig.probe.switch_select_max_ma = 0.0f;
    }

    // Probe feed order is a two-way switch; anything else means an old or
    // hand-edited file - fall back to DAC0-first (the sensed path).
    if (jumperlessConfig.probe.power_source != 0 &&
        jumperlessConfig.probe.power_source != 1) {
        jumperlessConfig.probe.power_source = 0;
    }

    // GPIO droop V0: persisted unloaded tip voltage for switch detection.
    if (jumperlessConfig.probe.droop_v0 < 3.0f ||
        jumperlessConfig.probe.droop_v0 > 3.6f) {
        jumperlessConfig.probe.droop_v0 = 3.35f;
    }
    // Same sanity band for the measure-mode tip drive: a corrupted-high
    // value (a tip-voltage servo chasing a bad reference can save up to
    // 5.2V) makes infra park the probe DAC above 3.3V logic, which pushes
    // the top of the pad ladder past ADC full-scale - hardware-observed as
    // "measure mode off at the high end / logo pads" plus a self-test
    // reference stuck at the bad parked level.
    if (jumperlessConfig.probe.measure_voltage < 3.0f ||
        jumperlessConfig.probe.measure_voltage > 3.6f) {
        jumperlessConfig.probe.measure_voltage = 3.33f;
    }

    readSettingsFromConfig();
    
    // Initialize shadow config for dirty tracking
    // This allows saveConfig() to skip writes when nothing has changed
    updateShadowConfig();
    
    // Apply USB CDC configuration (DTR ignore mode)
    // This must be called after config is loaded to apply the ignore_dtr setting
    usb_cdc_apply_config();

#if USB_AUDIO_ENABLE
    // Restore the saved USB audio setup. Runs here, right after the config
    // loads and before the USB stack enumerates, so a saved-enabled mic is
    // present in the very first configuration descriptor the host reads - no
    // re-enumeration, no dropped CDC ports. That is the only way to have the
    // audio device without paying the 2s port drop.
    usb_audio_apply_config();
#endif
}

// ============================================================================
// Printing (~ command)
// ============================================================================

void printConfigSectionToSerial(int section, bool showNamesArg, bool pasteable) {
    bool names = showNamesArg;

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
    
    char valBuf[64];
    for (int s = 0; s < JLSECT_COUNT; s++) {
        if (section != -1 && section != s) continue;
        // Bookkeeping sections stay out of the printout unless asked for
        // by name.
        if (section == -1 && s == JLSECT_firmware) continue;

    cycleTerminalColor();
        bool first = true;
        for (int i = 0; i < jlConfigOptionCount; i++) {
            const ConfigOptionDesc* opt = &jlConfigOptions[i];
            if (opt->section != s) continue;
            if (section == -1 && (opt->flags & JLC_HIDDEN)) continue;
            if (first) {
                Serial.print("\n`[");
                Serial.print(jlConfigSections[s].name);
                Serial.print("] ");
        if (pasteable == false) Serial.println();
                first = false;
            } else if (pasteable == true) {
                Serial.print("`[");
                Serial.print(jlConfigSections[s].name);
                Serial.print("] ");
            }
            configFormatValue(opt, valBuf, sizeof(valBuf), names);
            Serial.print(opt->key);
            Serial.print(" = ");
            Serial.print(valBuf);
        Serial.println(";");
        }
    }
    changeTerminalColor(-1, true);
}

// Helper function to parse setting.
// All callers pass section[32], key[32], value[64] (see call sites) while the
// input line can be up to 256 bytes - every copy below is capped to those
// sizes so a long/garbled line truncates instead of smashing the stack.
#define PARSE_SECTION_CAP (31)
#define PARSE_KEY_CAP     (31)
#define PARSE_VALUE_CAP   (63)
bool parseSetting(const char* line, char* section, char* key, char* value) {
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

// Print a human-readable before/after line for a setting change.
static void printSettingChange(const ConfigOptionDesc* opt, const char* oldValue, const char* newValue) {
    Serial.print("Changed [");
    Serial.print(jlConfigSections[opt->section].name);
    Serial.print("] ");
    Serial.print(opt->key);
    Serial.print(" from ");
    Serial.print(oldValue);
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
    Serial.println("                          ` = open the interactive config menu (arrow keys)");
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
    Serial.println("\n\r");
    delayMicroseconds(3000);
}

void printConfigToSerial(bool showNamesArg) {
    char line[128] = {0};
    int lineIndex = 0;
    unsigned long lastCharTime = millis();
    const unsigned long timeout = 1000;

    // Check if we already have a command line from line buffering mode
    // ONLY use buffered mode if terminal line_buffering is enabled
    if (jumperlessConfig.terminal.line_buffering == 1 && currentCommandLine.length() > 1) {
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
                int section = configSectionFromName(sectionName.c_str());
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
    Stream* inputStream = (jumperlessConfig.terminal.line_buffering == 1) ? (Stream*)&Jerial : (Stream*)&Serial;
    
    while (true) {
        if (inputStream->available() > 0) {
            char c = inputStream->read();
            if (lineIndex < (int)sizeof(line) - 1) {
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

                    int section = configSectionFromName(sectionName);
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

// Interactive config editor (ConfigTui.cpp).
extern void configTuiRun(void);

// Handles the shared reset/self-test commands typed at the ` prompt.
// Returns true when the line was one of them (handled here).
static bool handleConfigCommandLine(const char* line) {
            if (strcmp(line, "?") == 0 || strcmp(line, "-h") == 0 || strcmp(line, "--help") == 0 || strcmp(line, "help") == 0) {
                printConfigHelp();
        return true;
    }
    if (strcmp(line, "menu") == 0 || strcmp(line, "tui") == 0) {
        configTuiRun();
        return true;
    }
    if (strcmp(line, "reset") == 0) {
                resetConfigToDefaults();
                saveConfigToFile("/config.txt");
                Serial.println("Done. Settings have been reset to defaults");
        return true;
            }
    if (strcmp(line, "clear_calibration") == 0 || strcmp(line, "clear_cal") == 0 ||
                     strcmp(line, "reset_calibration") == 0 || strcmp(line, "reset_cal") == 0) {
                resetConfigToDefaults(1, 0);
                saveConfig();
                Serial.println("Done. Calibration has been cleared");
        return true;
            }
    if (strcmp(line, "clear_hardware") == 0 || strcmp(line, "clear_hw") == 0 ||
                     strcmp(line, "reset_hardware") == 0 || strcmp(line, "reset_hw") == 0) {
                resetConfigToDefaults(0, 1);
                saveConfig();
                Serial.println("Done. Hardware has been cleared");
        return true;
            }
    if (strcmp(line, "clear_all") == 0 || strcmp(line, "reset_all") == 0) {
                resetConfigToDefaults(1, 1);
                saveConfig();
                Serial.println("Done. All settings have been cleared");
        return true;
            }
    if (strcmp(line, "clear_filesystem") == 0 || strcmp(line, "reset_filesystem") == 0) {
                Serial.println("Deleting all filesystem contents...");
        deleteDirectoryContents("/");
                Serial.println("Filesystem contents deleted.");
        return true;
            }
    if (strcmp(line, "self_test") == 0 || strcmp(line, "selftest") == 0) {
                // Non-destructive hardware self test (no FS wipe, no reboot)
                runFullSelfTest(false);
        return true;
            }
    if (strcmp(line, "self_test_report") == 0 || strcmp(line, "selftest_report") == 0) {
                // Re-print the stored report without re-running the tests
                selfTestPrintStoredReport();
        return true;
    }
    if (strcmp(line, "check") == 0 || strcmp(line, "selfcheck") == 0) {
        configTableSelfCheck();
        return true;
    }
    if (strcmp(line, "force_first_start") == 0 || strcmp(line, "factory_reset") == 0) {
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
        return true;
    }
    return false;
}

void readConfigFromSerial() {
    char line[128] = {0};
    int lineIndex = 0;
    char currentSection[32] = {0};
    bool inSection = false;

    unsigned long lastCharTime = millis();
    const unsigned long timeout = 10;

    // Check if we already have a command line from line buffering mode
    // ONLY use buffered mode if terminal line_buffering is enabled
    if (jumperlessConfig.terminal.line_buffering == 1 && currentCommandLine.length() > 1) {
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
            if (handleConfigCommandLine(line)) {
                return;
            }
            
            // If not a special command, try to parse as a config setting
            char section[32], key[32], value[64];
            if (parseSetting(line, section, key, value)) {
                updateConfigValue(section, key, value);
                Serial.println("Config updated");
                readSettingsFromConfig();
                setRailsAndDACs(0);
                requestLedShow( -1 );
                
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
        } else {
            // Bare ` with nothing after it: open the interactive editor.
            configTuiRun();
        }
        return;
    }

    // Use Serial directly when line buffering is disabled, Jerial when enabled
    Stream* inputStream = (jumperlessConfig.terminal.line_buffering == 1) ? (Stream*)&Jerial : (Stream*)&Serial;

    // Decide between "user is pasting settings" and "bare ` - open the
    // editor". Leftover line terminators from the dispatch (the \n of a
    // \r\n, a straggling blank line) must not count as pasted input, or the
    // editor never opens for terminals that send CRLF.
    unsigned long waitStart = millis();
    for (;;) {
        if (inputStream->available() > 0) {
            int p = inputStream->peek();
            if (p == '\r' || p == '\n' || p == ' ') {
                inputStream->read();
                continue;
            }
            break;  // real content - fall into the paste-parse loop below
        }
        if (millis() - waitStart > 400) {
            // Bare ` with no pasted settings: open the interactive editor.
            configTuiRun();
            return;
        }
    }

    Serial.println("\n\renter config settings (? for help)\n\r");

    int timedOut = 0;
    while (true) {
        if (inputStream->available() > 0) {
            char c = inputStream->read();

            // Handle backspace
            if (c == '\b' || c == 0x7F) {
                if (lineIndex > 0) {
                    lineIndex--;
                    Serial.print(" \b"); // Erase character
                }
                continue;
            }

            // Add character to line buffer if there's space
            if (lineIndex < (int)sizeof(line) - 1) {
                line[lineIndex++] = c;
                line[lineIndex] = '\0'; // Keep string null-terminated

                // Special commands act as soon as they're fully typed
                if (handleConfigCommandLine(line)) {
                    memset(line, 0, sizeof(line));
                    lineIndex = 0;
                    continue;
                }
            }

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
        if (timedOut > (int)timeout) {
            memset(line, 0, sizeof(line));
            lineIndex = 0;
            break;
        }
    }

    while (inputStream->available() > 0) {
        inputStream->read();
        delayMicroseconds(100);
    }
   readSettingsFromConfig();
    setRailsAndDACs(0);
    requestLedShow( -1 );
}

int parseTrueFalse(const char* value) {
    if (strcmp(value, "true") == 0) return 1;
    else if (strcmp(value, "false") == 0) return 0;
    else if (strcmp(value, "1") == 0) return 1;
    else if (strcmp(value, "0") == 0) return 0;
    else return -1;
}

void updateConfigValue(const char* section, const char* key, const char* value) {
    // [firmware] last_version arrives from checkAndHandleFirmwareUpdate's
    // save path via file, never live - but accept it anyway.
    const ConfigOptionDesc* opt = configFindOption(section, key);
    if (!opt) {
        Serial.print("Unknown or removed option [");
        Serial.print(section);
        Serial.print("] ");
        Serial.print(key);
        Serial.println(" (ignored)");
        return;
    }

    char oldValue[64];
    configFormatValue(opt, oldValue, sizeof(oldValue), true);

    if (!configSetValue(opt, value, /*liveApply=*/true)) {
                return; // refused - don't save or echo a change that didn't happen
            }

    // Turning probe auto-connect off "until reboot" (0) is deliberately
    // transient - don't persist it.
    bool skipSave = (opt->section == JLSECT_probe &&
                     strcmp(opt->key, "auto_connect") == 0 &&
                     jumperlessConfig.probe.auto_connect == 0);
    if (!skipSave) {
        saveConfigToFile("/config.txt");
    }

    char newValue[64];
    configFormatValue(opt, newValue, sizeof(newValue), true);
    printSettingChange(opt, oldValue, newValue);
}

// Fast config parsing function optimized for tight loops
// Returns true if valid config setting was parsed and updated, false otherwise
// Supported formats:
// - Dot notation: "config.section.key = value"
// - Bracket notation: "`[section]key = value" (backtick optional)
bool fastParseAndUpdateConfig(const char* configString) {
    // Quick validation - must have minimum length and contain '='
    if (!configString || strlen(configString) < 5) {
        return false;
    }
    
    const char* equals = strchr(configString, '=');
    if (!equals) {
        return false;
    }
    
    // Quick check for valid config formats
    bool isDotNotation = (strncmp(configString, "config.", 7) == 0);
    bool isBracketNotation = (configString[0] == '`' && configString[1] == '[') ||
                             (configString[0] == '[');
    
    if (!isDotNotation && !isBracketNotation) {
        return false;
    }
    
    // Strip the leading backtick if present
    const char* line = (configString[0] == '`') ? configString + 1 : configString;

    char section[32], key[32], value[64];
    if (!parseSetting(line, section, key, value)) {
            return false;
        }
    toLower(section);
    toLower(key);
    const ConfigOptionDesc* opt = configFindOption(section, key);
    if (!opt) {
            return false;
        }
    if (!configSetValue(opt, value, /*liveApply=*/true)) {
            return false;
        }
    configChanged = true;  // async save picks it up
    return true;
}

// ============================================================================
// Self-check: the one runnable check that fails if the table machinery breaks.
// Round-trips every option (format -> parse -> compare), checks key
// uniqueness, and resolves every alias. Run with `check at the config prompt.
// ============================================================================
bool configTableSelfCheck(void) {
    int failures = 0;
    char before[64], after[64];

    // Key uniqueness within each section
    for (int i = 0; i < jlConfigOptionCount; i++) {
        for (int j = i + 1; j < jlConfigOptionCount; j++) {
            if (jlConfigOptions[i].section == jlConfigOptions[j].section &&
                strcmp(jlConfigOptions[i].key, jlConfigOptions[j].key) == 0) {
                Serial.print("DUPLICATE key: [");
                Serial.print(jlConfigSections[jlConfigOptions[i].section].name);
                Serial.print("] ");
                Serial.println(jlConfigOptions[i].key);
                failures++;
            }
        }
    }

    // Every option round-trips through its own formatter/parser.
    // OLED pins/connection get liveApply=false so no bus dance happens.
    for (int i = 0; i < jlConfigOptionCount; i++) {
        const ConfigOptionDesc* opt = &jlConfigOptions[i];
        configFormatValue(opt, before, sizeof(before), true);
        configSetValue(opt, before, /*liveApply=*/false);
        configFormatValue(opt, after, sizeof(after), true);
        if (strcmp(before, after) != 0) {
            Serial.print("ROUND-TRIP failed: [");
            Serial.print(jlConfigSections[opt->section].name);
            Serial.print("] ");
            Serial.print(opt->key);
            Serial.print("  '");
            Serial.print(before);
            Serial.print("' -> '");
            Serial.print(after);
            Serial.println("'");
            failures++;
        }
    }

    // Every alias resolves to a real option.
    for (int i = 0; i < jlConfigAliasCount; i++) {
        if (!configFindOption(jlConfigAliases[i].section, jlConfigAliases[i].newKey)) {
            Serial.print("ALIAS target missing: ");
            Serial.print(jlConfigAliases[i].oldSection);
            Serial.print(".");
            Serial.println(jlConfigAliases[i].oldKey);
            failures++;
        }
    }

    // Every section has a name and every option a description.
    for (int i = 0; i < jlConfigOptionCount; i++) {
        if (jlConfigOptions[i].desc == nullptr || jlConfigOptions[i].desc[0] == '\0') {
            Serial.print("MISSING description: ");
            Serial.println(jlConfigOptions[i].key);
            failures++;
        }
    }

    Serial.print("configTableSelfCheck: ");
    Serial.print(jlConfigOptionCount);
    Serial.print(" options, ");
    Serial.print(jlConfigAliasCount);
    Serial.print(" aliases - ");
    if (failures == 0) {
        Serial.println("PASS");
    } else {
        Serial.print(failures);
        Serial.println(" FAILURES");
    }
    return failures == 0;
}
