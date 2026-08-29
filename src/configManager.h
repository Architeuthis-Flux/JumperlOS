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

// ============================================================================
// Table-driven config descriptors - generated from JL_CONFIG_ALL_OPTIONS in
// config.h. ONE X() line there is the only thing you touch to add an option.
// ============================================================================

// File sections, in file/print order. [config] (the firmware_version stamp)
// is virtual and handled outside the table.
enum ConfigSectionId : uint8_t {
    JLSECT_firmware = 0,
    JLSECT_hardware,
    JLSECT_probe,
    JLSECT_clickwheel,
    JLSECT_measurement,
    JLSECT_terminal,
    JLSECT_undo,
    JLSECT_dacs,
    JLSECT_debug,
    JLSECT_routing,
    JLSECT_slots,
    JLSECT_calibration,
    JLSECT_logo_pads,
    JLSECT_display,
    JLSECT_serial_1,
    JLSECT_serial_2,
    JLSECT_top_oled,
    JLSECT_usb_cdc,
    JLSECT_usb_audio,
    JLSECT_COUNT,
};

enum ConfigValueType : uint8_t {
    JLT_BOOL, JLT_INT, JLT_VINT, JLT_FLOAT, JLT_HEX, JLT_FONT, JLT_STR16, JLT_STR33,
};

// Side effects applied when a value changes LIVE (paste / TUI / MicroPython).
// File-load parsing stores values only; boot apply happens in loadConfig() /
// readSettingsFromConfig().
enum ConfigApplyHook : uint8_t {
    HOOK_NONE = 0,
    HOOK_PSRAM,               // applyPsramModeChange + MicroPython reinit
    HOOK_PROBE_AUTOCONNECT,   // routableBufferPower on/off now
    HOOK_PROBE_POWER_SOURCE,  // clamp 0/1 + infraNudge when the order changed
    HOOK_ENCODER_PIO,         // clamp -1..2 (applied next boot)
    HOOK_LED_REFRESH_US,      // clamp >= 0
    HOOK_USB_CDC_DTR,         // usb_cdc_set_ignore_dtr + DTR lockout + apply
    HOOK_OLED_CONNECTION,     // applyOledConnectionType (owns the whole set)
    HOOK_OLED_PIN,            // validated pin swap w/ I2C bus teardown; may refuse
    HOOK_OLED_FONT,           // set font family + redraw
    HOOK_LINE_BUFFERING,      // pushLineBufferingToApp
    HOOK_SERIAL_FUNCTION,     // initArduino() re-applies UART roles
    HOOK_LINES_WIRES,         // OG hardware forces lines mode
    HOOK_MENU_FX,             // sync menuTransitionConfig for live preview
    HOOK_TERM_COLORS,         // sync disableTerminalColors
    HOOK_UNDO,                // clamp + sync undo persistence knobs
};

// Option flags.
enum : uint16_t {
    JLC_NONE      = 0,
    JLC_CAL       = 1u << 0,  // calibration: survives resets + update migration
    JLC_BOOT_ONLY = 1u << 1,  // stored now, takes effect next boot
    JLC_DEBUG     = 1u << 2,  // debug print gate
    JLC_LEDS      = 1u << 3,  // refresh the LEDs after a live change
    JLC_HIDDEN    = 1u << 4,  // not shown by ~ or the TUI (bookkeeping keys)
    // Cross-list a [debug] flag into another TUI category (single source of
    // truth stays [debug]):
    JLC_SHOW_PROBE   = 1u << 8,
    JLC_SHOW_MEASURE = 1u << 9,
};

struct JlTableRef {
    const StringIntEntry* t;
    int n;
};
template <size_t N>
constexpr JlTableRef jlTableRef(const StringIntEntry (&t)[N]) { return { t, (int)N }; }
constexpr JlTableRef jlTableRef(decltype(nullptr)) { return { nullptr, 0 }; }

struct ConfigOptionDesc {
    uint8_t section;         // ConfigSectionId
    const char* key;
    ConfigValueType type;
    void* ptr;               // into jumperlessConfig
    float minv, maxv, step;  // TUI editor bounds; 0,0 = unconstrained
    JlTableRef table;        // value names, or {nullptr,0}
    uint8_t hook;            // ConfigApplyHook
    uint16_t flags;          // JLC_*
    const char* desc;        // one-sentence help text
};

extern const ConfigOptionDesc jlConfigOptions[];
extern const int jlConfigOptionCount;

// Section metadata (name = file/[section] token; title/desc for the TUI).
struct ConfigSectionDesc {
    const char* name;
    const char* title;
    const char* desc;
};
extern const ConfigSectionDesc jlConfigSections[JLSECT_COUNT];

// Old `[section] key` names that moved/renamed - parsed transparently.
struct ConfigAlias {
    const char* oldSection;
    const char* oldKey;
    uint8_t section;         // new ConfigSectionId
    const char* newKey;
};
extern const ConfigAlias jlConfigAliases[];
extern const int jlConfigAliasCount;

// Lookup / access helpers used by the config TUI and MicroPython bridge.
const ConfigOptionDesc* configFindOption(const char* sectionName, const char* key);
const ConfigOptionDesc* configFindOption(uint8_t section, const char* key);
int  configSectionFromName(const char* sectionName);  // -1 unknown, -2 = [config]
void configFormatValue(const ConfigOptionDesc* opt, char* out, size_t outLen, bool names);
// Returns false if the change was refused (e.g. an illegal OLED pin pair).
bool configSetValue(const ConfigOptionDesc* opt, const char* value, bool liveApply);
void configResetOptionToDefault(const ConfigOptionDesc* opt, bool liveApply);
bool configOptionIsDefault(const ConfigOptionDesc* opt);

// Round-trips every option (format -> parse -> compare), checks key uniqueness
// and alias resolution. Prints failures; returns true when everything holds.
bool configTableSelfCheck(void);

// [clickwheel] fx_* <-> menuTransitionConfig sync (boot apply + tuner capture).
void configApplyMenuFx(void);
void configCaptureMenuFx(void);

// Set by migration when the probe droop calibration has never run
// (probe.droop_ohms == 0); the main loop prompts for Switch Calib once.
extern bool probeCalibrationNeeded;

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

// Get the FIRST table name matching a value (bools -> "true"/"false", enums
// -> their canonical name). Falls back to nullptr when no entry matches.
template <size_t N>
const char* getStringFromTable(int value, const StringIntEntry (&table)[N]) {
    for (size_t i = 0; i < N; i++) {
        if (table[i].value == value) return table[i].name;
    }
    return nullptr;
}
const char* getStringFromTableRef(int value, const JlTableRef& ref);


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

// [probe] auto_connect: -1 = off (persistent), 0 = off (until reboot), 1 = on
const StringIntEntry autoConnectTable[] = {
    {"on", 1},
    {"off", -1},
    {"off_until_reboot", 0},
};
const int autoConnectTableSize = sizeof(autoConnectTable) / sizeof(autoConnectTable[0]);

// [probe] power_source
const StringIntEntry probePowerSourceTable[] = {
    {"dac0_first", 0},
    {"dac0", 0},
    {"gpio_first", 1},
    {"gpio", 1},
};
const int probePowerSourceTableSize = sizeof(probePowerSourceTable) / sizeof(probePowerSourceTable[0]);

// [clickwheel] encoder_pio
const StringIntEntry encoderPioTable[] = {
    {"auto", -1},
    {"pio0", 0},
    {"pio1", 1},
    {"pio2", 2},
};
const int encoderPioTableSize = sizeof(encoderPioTable) / sizeof(encoderPioTable[0]);

// [clickwheel] rail_click_adjust
const StringIntEntry railClickAdjustTable[] = {
    {"off", 0},
    {"oled_only", 1},
    {"always", 2},
};
const int railClickAdjustTableSize = sizeof(railClickAdjustTable) / sizeof(railClickAdjustTable[0]);

// [clickwheel] fx_type - mirrors MenuTransitionType in eyecandy/MenuTransitions.h
const StringIntEntry menuFxTypeTable[] = {
    {"off", 0},
    {"dither", 1},
    {"fade", 2},
    {"sparkle", 3},
    {"wipe", 4},
    {"tint", 5},
    {"color_dither", 6},
    {"color_wipe", 7},
    {"glow", 8},
    {"ripple", 9},
};
const int menuFxTypeTableSize = sizeof(menuFxTypeTable) / sizeof(menuFxTypeTable[0]);

// [slots] boot_mode
const StringIntEntry bootModeTable[] = {
    {"fixed_slot", 0},
    {"last_active", 1},
};
const int bootModeTableSize = sizeof(bootModeTable) / sizeof(bootModeTable[0]);

// [logo_pads] connect-mode node bindings: the node a probe pad stands for
// when tapped in connect/clear mode. Every routable "special" node EXCEPT
// the rails (they have their own pads). Values keep the legacy
// arbitraryFunctionTable numbering where one existed (old config files and
// pasted lines stay valid); nodes that never had a number get 60+.
// "choose" (-2) opens the on-board chooser on tap - the old building-pad
// behavior.
const StringIntEntry padNodeTable[] = {
    {"off", -1},
    {"choose", -2},
    {"uart_tx", 0},
    {"uart_rx", 1},
    {"adc_0", 2},
    {"adc_1", 3},
    {"adc_2", 4},
    {"adc_3", 5},
    {"adc_4", 6},
    {"gpio_1", 9},
    {"gpio_2", 10},
    {"gpio_3", 11},
    {"gpio_4", 12},
    {"gpio_5", 13},
    {"gpio_6", 14},
    {"gpio_7", 15},
    {"gpio_8", 16},
    {"isense_pos", 25},
    {"isense+", 25},
    {"isense_neg", 26},
    {"isense-", 26},
    {"dac_0", 60},
    {"dac_1", 61},
    {"gnd", 62},
    {"buffer_in", 63},
    {"buffer_out", 64},
};
const int padNodeTableSize = sizeof(padNodeTable) / sizeof(padNodeTable[0]);

// [logo_pads] idle-mode tap actions: what a pad tap DOES when the probe is
// idle in the SELECT switch position (outside connect/clear mode and the
// pad menus). Fresh numbering - this option never existed before.
const StringIntEntry padActionTable[] = {
    {"off", 0},
    {"gpio_1_toggle", 1},
    {"gpio_2_toggle", 2},
    {"gpio_3_toggle", 3},
    {"gpio_4_toggle", 4},
    {"gpio_5_toggle", 5},
    {"gpio_6_toggle", 6},
    {"gpio_7_toggle", 7},
    {"gpio_8_toggle", 8},
    {"gpio_1_high", 9},
    {"gpio_2_high", 10},
    {"gpio_3_high", 11},
    {"gpio_4_high", 12},
    {"gpio_5_high", 13},
    {"gpio_6_high", 14},
    {"gpio_7_high", 15},
    {"gpio_8_high", 16},
    {"gpio_1_low", 17},
    {"gpio_2_low", 18},
    {"gpio_3_low", 19},
    {"gpio_4_low", 20},
    {"gpio_5_low", 21},
    {"gpio_6_low", 22},
    {"gpio_7_low", 23},
    {"gpio_8_low", 24},
    {"dac_0_up", 25},
    {"dac_0_down", 26},
    {"dac_1_up", 27},
    {"dac_1_down", 28},
};
const int padActionTableSize = sizeof(padActionTable) / sizeof(padActionTable[0]);

// [top_oled] show_in_terminal - where the quarter-block OLED mirror goes.
// 1..4 are the four USB CDC ports as they enumerate on the host
// (JLV5port1/3/5/7); 5 is the hardware UART. Old serial-port names and the
// 0/1 toggle values stay as parse aliases.
const StringIntEntry oledMirrorTable[] = {
    {"off", 0},
    {"false", 0},
    {"disable", 0},
    {"port_1", 1},
    {"main", 1},
    {"terminal", 1},
    {"on", 1},
    {"true", 1},
    {"port_3", 2},
    {"usb_1", 2},
    {"port_5", 3},
    {"usb_2", 3},
    {"port_7", 4},
    {"usb_3", 4},
    {"uart", 5},
    {"uart1", 5},
};
const int oledMirrorTableSize = sizeof(oledMirrorTable) / sizeof(oledMirrorTable[0]);

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

// Kept for callers that still parse these strings directly.
int parseUartFunction(const char* str);
int parseLinesWires(const char* str);
int parseNetColorMode(const char* str);
int parseCurrentFlow(const char* str);
int parseArbitraryFunction(const char* str);
int parseTagParsing(const char* str);
int parseFlashType(const char* str);
int parseHex(const char* str);
int printArbitraryFunction(int function);
