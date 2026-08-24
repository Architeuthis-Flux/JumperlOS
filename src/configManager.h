#pragma once

#include "config.h"
#include <FatFS.h>
#include "oled.h"
#include "JumperlOS.h"  // For Service base class

extern bool configChanged;
extern bool autoCalibrationNeeded;
// Flag to request async config save (set true to trigger background save)
extern volatile bool configSavePending;
// Global configuration instance
extern struct config jumperlessConfig;
// Shadow copy of last saved config for dirty tracking
extern struct config lastSavedConfig;
// Flag indicating if shadow config is valid
extern bool shadowConfigValid;


struct StringIntEntry {
    const char* name;
    int value;
};

// Core configuration functions
void loadConfig(void);
bool saveConfig(void);
void resetConfigToDefaults(int clearCalibration = 0, int clearHardware = 0);
void loadHardwareFromEEPROM(void);  // Load hardware revision from EEPROM (survives config reset)

// Firmware versioning and file provisioning
bool checkAndHandleFirmwareUpdate(void);
void provisionFirmwareFiles(bool print = false);
bool provisionEmbeddedFile(const char* filename, const unsigned char* data, unsigned int dataLen);
int compareVersions(const char* v1, const char* v2);  // Compare version strings (X.Y.Z.W)

// File operations
void updateConfigFromFile(const char* filename);
bool saveConfigToFile(const char* filename);
bool saveConfigIncremental(const char* filename);  // Optimized save - only writes changed values
bool configHasChanges();  // Returns true if config differs from last saved
void updateShadowConfig();  // Copy current config to shadow

// Debug timing for config save operations
extern bool debugConfigSaveTiming;
void setConfigSaveDebug(bool enable);  // Enable/disable timing debug output

// Request async config save (non-blocking) - use this instead of saveConfig() for UI responsiveness
void requestConfigSave();

/**
 * @brief Background config save service
 * LOW priority - saves config to flash without blocking UI
 * Set configSavePending = true to trigger a save
 */
class ConfigSaveService : public Service {
public:
    static ConfigSaveService& getInstance();
    ConfigSaveService(const ConfigSaveService&) = delete;
    ConfigSaveService& operator=(const ConfigSaveService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "ConfigSave"; }
    ServicePriority getPriority() const override { return ServicePriority::LOW; }
    // Not an existing gate: implicit (configChanged) saves are debounced 2 s
    // after the last input inside service(); 100 ms bounds how late that
    // check runs. An explicit requestConfigSave() also requestRun()s, so it
    // is picked up on the next pass exactly as before.
    uint32_t periodUs() const override { return 100000; }

private:
    ConfigSaveService() = default;
    ~ConfigSaveService() = default;
};

// Global service reference (defined in JumperlOS.cpp)
extern ConfigSaveService& configSaveService;

// Serial operations
void printConfigSectionToSerial(int section, bool showNames = true, bool pasteable = true);
void readConfigFromSerial(void);
void printConfigToSerial(bool showNames = true);
void printConfigStructToSerial(bool showNames = true);
void printConfigHelp(void);
bool parseSetting(const char* line, char* section, char* key, char* value);
void parseCommaSeparatedInts(const char* str, int* array, int maxValues);
void parseCommaSeparatedFloats(const char* str, float* array, int maxValues);
void parseCommaSeparatedBools(const char* str, bool* array, int maxValues);
bool parseBool(const char* str);
float parseFloat(const char* str);
int parseInt(const char* str);
int parseFont(const char* str);                // Now reads from fontList in oled.cpp
const char* getFontString(int fontFamily);     // Get font name from FontFamily enum
int parseSerialPort(const char* str);
int parseDumpFormat(const char* str);
int parseConnectionType(const char* str);
const char* getConnectionTypeString(int connectionType);
void updateOledPinsForConnectionType(int connectionType);
// Higher-level OLED connection-type helpers (apply / cycle / defaults / names)
// live in oled.h, since they own the I2C bus tear-down and reinit dance.

// External variables from main.cpp
extern const char firmwareVersion[];
extern bool newConfigOptions;
void trim(char* str);
void toLower(char* str); 
void updateConfigValue(const char* section, const char* key, const char* value);
int parseTrueFalse(const char* value);
void printArbitraryFunctionTable(void);
// Fast config parsing function for tight loops - returns quickly if invalid
bool fastParseAndUpdateConfig(const char* configString);
// Template function to get string from a value and a table (auto-deduces table size)
template <size_t N>
const char* getStringFromTable(int value, const StringIntEntry (&table)[N]);



// List of all string options for parseArbitraryFunction
extern const char* arbitraryFunctionStrings[];

// Generic struct for mapping string to int value

// REMOVED: Font table now reads directly from fontList in oled.cpp
// This eliminates duplicate data and ensures config always matches available fonts
// Table for parseBool
const StringIntEntry boolTable[] = {
    {"true", 1},
    {"false", 0},
    {"1", 1},
    {"0", 0},
    {"yes", 1},
    {"no", 0},
    {"on", 1},
    {"off", 0},
    {"enable", 1},
    {"disable", 0},
    {"enabled", 1},
    {"disabled", 0},
    {"t", 1},
    {"f", 0},
    {"y", 1},
    {"n", 0}
};
const int boolTableSize = sizeof(boolTable) / sizeof(boolTable[0]);

// Table for parseUartFunction
const StringIntEntry uartFunctionTable[] = {
    {"off", 0},
    {"disable", 0},
    //{"pass", 1},
    {"passthrough", 1},
    {"port_2", 1},
    {"main", 2},
    {"control", 2},
    {"port_1", 2},
    {"micropython", 3},
    {"python", 3},
    {"oled", 4},
    {"leds", 5},
    {"led", 5},
    {"oled_leds", 6},
    {"leds_oled", 6},
};
const int uartFunctionTableSize = sizeof(uartFunctionTable) / sizeof(uartFunctionTable[0]);

const StringIntEntry connectionTypeTable[] = {
    {"gpio_7_8", 0},
    {"rp6_rp7", 1},
    {"i2c0", 2},
    {"internal_i2c0", 2},
    {"internal", 2},      // Alias for internal_i2c0
    {"intrnal", 2},
    {"7_8", 0},
    {"6_7", 1},
    {"gpio78", 0},
    {"rp67", 1},
    {"rp", 1},
    {"gpio", 0},
    {"custom", 3},
};
const int connectionTypeTableSize = sizeof(connectionTypeTable) / sizeof(connectionTypeTable[0]);


const StringIntEntry serialPortTable[] = {
    {"false", 0},
    {"off", 0},
    {"disable", 0},

    {"main", 1},
    {"usb0", 1},
    {"usb_0", 1},

    {"usb_1", 2},
    {"usb_2", 3},
    {"usb_3", 4},
    {"usb_4", 5},
    {"usb_5", 6},
    {"usb_6", 7},
    {"usb_7", 8},
    {"usb_8", 9},

    {"usb1", 2},
    {"usb2", 3},
    {"usb3", 4},
    {"usb4", 5},
    {"usb5", 6},
    {"usb6", 7},
    {"usb7", 8},
    {"usb8", 9},



    {"port1", 2},
    {"port2", 3},
    {"port3", 4},
    {"port4", 5},
    {"port5", 6},
    {"port6", 7},
    {"port7", 8},
    {"port8", 9},
    
    {"uart1", 11},
    {"uart2", 12},
    {"uart3", 13},



};
const int serialPortTableSize = sizeof(serialPortTable) / sizeof(serialPortTable[0]);

// Table for parseDumpFormat
const StringIntEntry dumpFormatTable[] = {
    {"image", 0},
    {"terminal", 0},
    {"rgb", 1},
    {"raw", 2},
    {"uint32", 2}
};
const int dumpFormatTableSize = sizeof(dumpFormatTable) / sizeof(dumpFormatTable[0]);

// Table for parseLinesWires
const StringIntEntry linesWiresTable[] = {
    {"lines", 0},
    {"l", 0},
    {"wires", 1},
    {"w", 1},
    {"0", 0},
    {"1", 1}
};
const int linesWiresTableSize = sizeof(linesWiresTable) / sizeof(linesWiresTable[0]);

// Table for parseNetColorMode
const StringIntEntry netColorModeTable[] = {
    {"rainbow", 0},
    {"shuffle", 1},
    {"random", 1},
    {"set_from_serial", 2},
    {"set_from_serial_random", 3},
    {"set_from_serial_shuffle", 4},
    {"set_from_serial_rainbow", 5}
};
const int netColorModeTableSize = sizeof(netColorModeTable) / sizeof(netColorModeTable[0]);

// Table for parseCurrentFlow
const StringIntEntry currentFlowTable[] = {
    {"conventional", 0},
    {"electron", 1},
    {"0", 0},
    {"1", 1}
};
const int currentFlowTableSize = sizeof(currentFlowTable) / sizeof(currentFlowTable[0]);


const StringIntEntry displayTypeTable[] = {
    {"ssd1306", 0},
    {"sh1106", 1},
    {"ssd1309", 2},
    {"sh1107", 3},
    {"ssd1312", 4},
    {"sh1108", 5},
};


const int displayTypeTableSize = sizeof(displayTypeTable) / sizeof(displayTypeTable[0]);


// Table for parseTagParsing
const StringIntEntry tagParsingTable[] = {
    {"off", 0},
    {"disable", 0},
    {"enabled", 1},
    {"enabled_passthrough", 1},
    {"passthrough", 1},
    {"strip_tags", 2},
    {"parse + strip", 2},
    {"strip + parse", 2},

};
const int tagParsingTableSize = sizeof(tagParsingTable) / sizeof(tagParsingTable[0]);

// Table for parseFlashType
const StringIntEntry flashTypeTable[] = {

    {"none", 0},
    {"off", 0},
    {"disable", 0},
    {"avr", 1},
    {"atmega328p", 1},
    {"esp32", 2},
    {"rp2040", 3},
};
const int flashTypeTableSize = sizeof(flashTypeTable) / sizeof(flashTypeTable[0]);

// Table for parseArbitraryFunction
const StringIntEntry arbitraryFunctionTable[] = {
    {"off", -1},
    {"none", -1},
    {"uart_tx", 0},
    {"tx", 0},
    {"uart_rx", 1},
    {"rx", 1},
    {"adc_0", 2},
    {"adc_1", 3},
    {"adc_2", 4},
    {"adc_3", 5},
    {"adc_4", 6},
    {"adc_5", 7},
    {"gpio_0", 8},
    {"gpio_1", 9},
    {"gpio_2", 10},
    {"gpio_3", 11},
    {"gpio_4", 12},
    {"gpio_5", 13},
    {"gpio_6", 14},
    {"gpio_7", 15},
    {"gpio_8", 16},
    {"app_1", 17},
    {"app_2", 18},
    {"app_3", 19},
    {"app_4", 20},
    {"app_5", 21},
    {"app_6", 22},
    {"app_7", 23},
    {"app_8", 24},
    {"isense_pos", 25},
    {"isense+", 25},
    {"isense-", 26},
    {"isense_neg", 26},
    {"gpio_1_toggle", 27},
    {"gpio_2_toggle", 28},
    {"gpio_3_toggle", 29},
    {"gpio_4_toggle", 30},
    {"gpio_5_toggle", 31},
    {"gpio_6_toggle", 32},
    {"gpio_7_toggle", 33},
    {"gpio_8_toggle", 34},
    {"gpio_1_high", 35},
    {"gpio_2_high", 36},
    {"gpio_3_high", 37},
    {"gpio_4_high", 38},
    {"gpio_5_high", 39},
    {"gpio_6_high", 40},
    {"gpio_7_high", 41},
    {"gpio_8_high", 42},
    {"gpio_1_low", 43},
    {"gpio_2_low", 44},
    {"gpio_3_low", 45},
    {"gpio_4_low", 46},
    {"gpio_5_low", 47},
    {"gpio_6_low", 48},
    {"gpio_7_low", 49},
    {"gpio_8_low", 50},
    {"dac_0_+", 51},
    {"dac_0_inc", 51},
    {"dac_0_increase", 51},
    {"dac_0_-", 52},
    {"dac_0_dec", 52},
    {"dac_0_decrease", 52},
    {"dac_1_+", 53},
    {"dac_1_inc", 53},
    {"dac_1_increase", 53},
    {"dac_1_-", 54},
    {"dac_1_dec", 54},
    {"dac_1_decrease", 54},
    {"pwm_increase_frequency", 55},
    {"pwm_decrease_frequency", 56},
    {"pwm_increase_duty_cycle", 57},
    {"pwm_decrease_duty_cycle", 57},

    {"pwm_stop", 59},
    {"pwm_stop_all", 59},
};
const int arbitraryFunctionTableSize = sizeof(arbitraryFunctionTable) / sizeof(arbitraryFunctionTable[0]);


