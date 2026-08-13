// SPDX-License-Identifier: MIT
#ifndef STATES_H
#define STATES_H

#include <Arduino.h>
#include <string.h>
#include <vector>
#include "JumperlessDefines.h"
#include "JumperlOS.h"
#include "MatrixState.h"
#include "LEDs.h"
#include "CH446Q.h"
#include <YAMLDuino.h>


/*
This is the new target YAML format for the states system

everything is optional, it'll default to the config values

make sure it's easy to add new fields in the future

Example YAML format:
version: 2
sourceOfTruth: bridges  # or 'nets'

bridges:
  - {n1: 1, n2: 5, dup: 2}
  - {n1: 10, n2: 20, dup: 3}

nets:  # optional, for colors/names
  - {num: 6, name: "Signal A", color: 0xFF0000, nodes: [1, 5]}
  - {num: 7, name: "Signal B", nodes: [10, 20]}

power:
  topRail: 5.0
  bottomRail: 0.0
  dac0: 3.33
  dac1: 0.0

config:
  routing: {stackPaths: 2, stackRails: 3, stackDacs: 0, railPriority: 1}
  gpio:
    direction: [1,1,1,1,1,1,1,1,1,1]
    pulls: [0,0,0,0,0,0,0,0,0,0]
  uart: {txFunction: 0, rxFunction: 1}
  oled: {connected: false, lockConnection: false}

*/




// History buffer size (adjustable for undo/redo)
#ifndef STATE_HISTORY_SIZE
#define STATE_HISTORY_SIZE 0
#endif

// Forward declarations
class JumperlessState;
class SlotManager;

// FakeGPIO restoration info (for loading from YAML)
struct FakeGpioRestorationInfo {
    int slot;
    int node;
    int mode;
    float v_high;
    float v_low;
    float threshold_high;
    float threshold_low;
    int high_voltage_node;  // For OUTPUT: TOP_RAIL, BOTTOM_RAIL, DAC0, DAC1, GND (-1 if not specified)
    int low_voltage_node;   // For OUTPUT: TOP_RAIL, BOTTOM_RAIL, DAC0, DAC1, GND (-1 if not specified)
};

// Source of truth for state reconciliation
enum SourceOfTruth {
    BRIDGES_PRIMARY,  // Bridges define connections, nets computed from bridges
    NETS_PRIMARY      // Nets define connections, bridges generated from nets
};

/**
 * @brief Ephemeral connection tracking
 * 
 * Ephemeral connections are temporary connections that:
 * - Are added to the bridges array for routing
 * - Are NEVER saved to YAML files  
 * - Do NOT trigger markDirty() (won't cause slot to save)
 * - Are tracked separately for cleanup
 * 
 * Use cases: Measure mode ADC connections, temporary probe connections
 */
struct EphemeralConnection {
    int node1;
    int node2;
    int bridgeIndex;  // Index in bridges array, -1 if not yet added
    
    EphemeralConnection() : node1(-1), node2(-1), bridgeIndex(-1) {}
    EphemeralConnection(int n1, int n2, int idx = -1) : node1(n1), node2(n2), bridgeIndex(idx) {}
};

/**
 * @brief Stores the connection topology: bridges, nets, and routing paths
 * This is the core of what makes a "slot" unique
 * 
 * WARNING: Contains large arrays (several KB). Avoid copying.
 */
struct ConnectionState {
    // STORED: Essential data (saved to YAML)
    int16_t bridges[MAX_BRIDGES][3];  // [bridge_index][node1, node2, duplicates]
    int16_t numBridges;
    
    netStruct nets[MAX_NETS];  // User-defined nets (optional, for colors/names/reference)
    int16_t numNets;  // Derived count
    
    // Bridge color hints (NOT saved to YAML directly, but used to color nets)
    // Indexed by bridge index, stores RGB color
    // Use 0xFFFFFFFF (all bits set) as sentinel for "no color specified"
    // This allows black (0x000000) to be a valid color
    uint32_t bridgeColors[MAX_BRIDGES];
    
    // COMPUTED: Runtime caches (NOT saved to YAML, computed from bridges/nets)
    pathStruct paths[MAX_BRIDGES];
    int16_t numPaths;
    bool pathsCacheValid;  // Set to false when connections change
    
    chipStatus chipStates[12];  // Derived from paths
    struct justXY chipXY[12];   // Crossbar switch states (reconstructed from paths)
    bool chipStatesCacheValid;
    
    ConnectionState();
    void clear();
    void invalidateCache(bool autoRefresh = true);  // Optional auto-refresh on invalidate
    void recomputePaths();  // Calls bridgesToPaths() to rebuild paths from current state
    void syncBridgesFromNets();  // Generate bridges from net node lists
    void syncNetsFromBridges();  // Generate nets from bridges
    
    // Note: Copying this struct copies all arrays (~several KB). Avoid when possible.
};

/**
 * @brief Power supply voltages for all DACs and rails
 */
struct PowerState {
    float topRail;      // Top breadboard rail voltage
    float bottomRail;   // Bottom breadboard rail voltage
    float dac0;         // DAC 0 voltage
    float dac1;         // DAC 1 voltage
    
    PowerState();
    void setDefaults();
    bool validate(String& errorMsg) const;
};

/**
 * @brief Display-related state (colors, brightness, etc.)
 */
struct DisplayState {
    // Net colors - only stored if manually changed from default
    // Tracks firstNode so we can reconcile after net rebuilds
    struct NetColorEntry {
        int netNumber;
        int firstNode;   // First node of net when color was set - for reconciliation
        rgbColor color;
        uint32_t rawColor;
        char colorName[32];
    };
    
    // Custom net names - tracked with firstNode so names follow nets during rebuilds
    struct NetNameEntry {
        int netNumber;
        int firstNode;   // First node of net when name was set - for reconciliation
        char name[32];
    };
    
    NetColorEntry customColors[MAX_NETS];
    int numCustomColors;
    
    NetNameEntry customNames[MAX_NETS];
    int numCustomNames;
    
    DisplayState();
    void clear();
    bool hasCustomColors() const { return numCustomColors > 0; }
    void setNetColor(int netNum, rgbColor color, uint32_t raw, const char* name);
    bool getNetColor(int netNum, rgbColor& color, uint32_t& raw, char* name) const;
    void removeNetColor(int netNum);
    
    // Custom net name functions
    void setNetName(int netNum, const char* name);
    const char* getNetName(int netNum) const;  // Returns nullptr if not set
    void removeNetName(int netNum);
    
    // Reconcile entries after nets are rebuilt - uses firstNode to find correct net numbers
    void reconcileAfterRebuild();
};

/**
 * @brief Configuration options that affect routing and behavior
 */
struct ConfigState {
    // State reconciliation mode
    SourceOfTruth sourceOfTruth;  // Which representation is authoritative
    
    // Routing preferences
    int stackPaths;     // How many paths to stack (0-3)
    int stackRails;     // How many rail connections to stack (0-3)
    int stackDacs;      // How many DAC connections to stack (0-3)
    int railPriority;   // Priority for rail connections
    
    // GPIO configuration (10 GPIOs: GP1-GP8, UART_TX, UART_RX)
    int gpioDirection[10];      // 0 = output, 1 = input
    int gpioPulls[10];          // 0 = pull down, 1 = pull up, 2 = none, 3 = bus keeper
    float gpioPwmFrequency[10]; // PWM frequency in Hz
    float gpioPwmDutyCycle[10]; // PWM duty cycle (0.0 to 1.0)
    bool gpioPwmEnabled[10];    // PWM enabled flag
    uint8_t gpioReadFloating[10]; // 1 = detect floating (tri-state), 0 = skip floating read
    volatile bool gpioPythonOwned[10];   // VOLATILE: True if MicroPython has exclusive control
                                         // Must be volatile since readGPIO() runs on Core 2 
                                         // and claiming happens on Core 1 (MicroPython)
    
    // UART configuration
    int uartTxFunction;  // 0 = tx, 1 = rx, 2 = gpio_in, 3 = gpio_out
    int uartRxFunction;  // 0 = tx, 1 = rx, 2 = gpio_in, 3 = gpio_out
    
    // OLED state
    bool oledConnected;
    bool oledLockConnection;
    
    // Auto-refresh behavior
    bool autoRefreshOnChange;  // Automatically call refreshConnections() when cache is invalidated
    
    ConfigState();
    void setDefaults();
};

/**
 * @brief Complete Jumperless state for a single slot
 * This is the main state container that gets saved/loaded
 * 
 * WARNING: This class contains MASSIVE arrays (tens of KB) and must NEVER be copied!
 * Always use references (&) or pointers (*) when passing this object around.
 * Copy constructor and copy assignment operator are deleted to enforce this.
 */
class JumperlessState {
public:
    ConnectionState connections;
    PowerState power;
    DisplayState display;
    ConfigState config;
    
    // Metadata
    int version;  // State format version for future compatibility
    
    JumperlessState();
    
    // CRITICAL: This object is HUGE (~33-50KB). Copying it exhausts the RP2040's
    // tiny stack/heap (the OG has only ~50KB TOTAL free RAM) and is wasteful on
    // V5. Copying is therefore DELETED - the compiler will reject any copy. Work
    // in place on globalState / a JumperlessState& reference, or serialize via
    // toYAML/fromYAML to move state between slots without a struct copy.
    //
    // SAFE:   JumperlessState& state = globalState;           (reference, no copy)
    // SAFE:   void func(JumperlessState& state);              (pass by reference)
    // COMPILE ERROR: JumperlessState state = globalState;     (copy - deleted)
    // COMPILE ERROR: void func(JumperlessState state);        (pass by value - deleted)
    JumperlessState(const JumperlessState&) = delete;
    JumperlessState& operator=(const JumperlessState&) = delete;
    
    // Connection management
    bool addConnection(int node1, int node2, String& errorMsg, int duplicates = -1);  // -1 = use default from config
    bool removeConnection(int node1, int node2, String& errorMsg);
    bool hasConnection(int node1, int node2) const;
    int getConnectionDuplicates(int node1, int node2) const;  // Get number of parallel copies
    bool setConnectionDuplicates(int node1, int node2, int duplicates, String& errorMsg);
    void clearAllConnections();
    
    // Ephemeral connection management (temporary connections that are NEVER saved)
    // Use for measure mode, temporary probing, etc.
    // Parameters:
    //   node1, node2: The nodes to connect
    //   errorMsg: Output error message on failure
    //   applyRouting: If true, immediately route and send to hardware (calls refreshLocalConnections)
    //   ledShowOption: LED display option (0=none, 1=show, -1=default). Only used if applyRouting=true
    //   color: Optional bridge color (0xFFFFFFFF = use default). Useful for visual feedback during measurements
    bool addEphemeralConnection(int node1, int node2, String& errorMsg, 
                                bool applyRouting = false, int ledShowOption = 0, 
                                uint32_t color = 0xFFFFFFFF);
    bool removeEphemeralConnection(int node1, int node2, String& errorMsg,
                                   bool applyRouting = false, int ledShowOption = -1);
    void clearAllEphemeralConnections(bool applyRouting = false, int ledShowOption = -1);
    bool isEphemeralConnection(int node1, int node2) const;
    int getEphemeralConnectionCount() const;
    
    // Power management
    void setDacVoltage(int dacNum, float voltage);
    float getDacVoltage(int dacNum) const;
    void setRailVoltage(bool isTopRail, float voltage);
    float getRailVoltage(bool isTopRail) const;
    
    // Configuration
    void setPathStacking(int paths, int rails, int dacs);
    void getPathStacking(int& paths, int& rails, int& dacs) const;
    void setAutoRefresh(bool enabled) { config.autoRefreshOnChange = enabled; }
    bool getAutoRefresh() const { return config.autoRefreshOnChange; }
    void setSourceOfTruth(SourceOfTruth source) { config.sourceOfTruth = source; markDirty(); }
    SourceOfTruth getSourceOfTruth() const { return config.sourceOfTruth; }
    
    // GPIO
    void setGpioDirection(int gpio, int direction);
    int getGpioDirection(int gpio) const;
    void setGpioPull(int gpio, int pull);
    int getGpioPull(int gpio) const;
    void setGpioPwmFrequency(int gpio, float frequency);
    float getGpioPwmFrequency(int gpio) const;
    void setGpioPwmDutyCycle(int gpio, float dutyCycle);
    float getGpioPwmDutyCycle(int gpio) const;
    void setGpioPwmEnabled(int gpio, bool enabled);
    bool getGpioPwmEnabled(int gpio) const;
    
    // UART
    void setUartTxFunction(int function);
    int getUartTxFunction() const;
    void setUartRxFunction(int function);
    int getUartRxFunction() const;
    
    // Display
    void setNetColor(int netNum, rgbColor color, uint32_t raw, const char* name);
    bool getNetColor(int netNum, rgbColor& color, uint32_t& raw, char* name) const;
    
    // Dirty flag management (for lazy writes)
    void markDirty();
    bool isDirty() const { return dirty; }
    void clearDirty();
    unsigned long getLastModifiedTime() const { return lastModifiedTime; }
    
    // Validation
    bool validate(String& errorMsg) const;
    
    // Serialization (YAML format)
    // showANSI: 0=plain, 1=colored hex, 2=colored blocks only (for terminal preview)
    bool toYAML(String& output, int showANSI = 0) const;
    bool fromYAML(const String& input, String& errorMsg);
    
    // Legacy format support
    bool fromLegacyNodeFile(const String& nodeFileContent, String& errorMsg);
    
    // State comparison
    bool operator==(const JumperlessState& other) const;
    bool operator!=(const JumperlessState& other) const { return !(*this == other); }
    
    // Utility
    void clear();
    size_t estimateRAMUsage() const;
    
private:
    // Dirty flag infrastructure
    bool dirty;
    unsigned long lastModifiedTime;
    
    // Ephemeral connections tracking (never saved to YAML)
    static constexpr int MAX_EPHEMERAL_CONNECTIONS = 8;
    EphemeralConnection ephemeralConnections[MAX_EPHEMERAL_CONNECTIONS];
    int numEphemeralConnections;
    
    // Helper for validation
    bool isNodeValid(int node) const;
    bool isConnectionAllowed(int node1, int node2, String& errorMsg) const;
    
    // YAML serialization helpers
    void serializeBridges(String& output) const;
    bool deserializeBridges(const char* yamlContent, String& errorMsg);
    void serializeNets(String& output) const;
    bool deserializeNets(const char* yamlContent, String& errorMsg);
    void serializePower(String& output) const;
    bool deserializePower(const char* yamlContent, String& errorMsg);
    void serializeConfig(String& output) const;
    bool deserializeConfig(const char* yamlContent, String& errorMsg);
    void serializeFakeGpio(String& output) const;
    bool deserializeFakeGpio(const char* yamlContent, String& errorMsg);
};

// Global list of pending FakeGPIO restorations (populated during YAML load, applied after bridges are restored)
extern std::vector<FakeGpioRestorationInfo> pendingFakeGpioRestorations;

/**
 * @brief Manages slot files and active state
 * Singleton pattern - use SlotManager::getInstance()
 */
class SlotManager : public Service {
public:
    // Get singleton instance
    static SlotManager& getInstance();
    
    // Prevent copying
    SlotManager(const SlotManager&) = delete;
    SlotManager& operator=(const SlotManager&) = delete;
    
    // Service interface
    ServiceStatus service() override;
    const char* getName() const override { return "States"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    
    // Active state access
    JumperlessState& getActiveState();
    const JumperlessState& getActiveState() const;
    int getActiveSlot() const { return activeSlotNumber; }
    
    // Slot management
    bool loadSlot(int slotNum, String& errorMsg);
    /** Load slot state from an arbitrary YAML file path (e.g. /slots/slot3.yaml or any path). */
    bool loadSlotFromPath(const String& path, String& errorMsg);
    bool saveSlot(int slotNum, String& errorMsg, bool skipValidation = false);  // skipValidation for faster auto-saves
    bool saveActiveSlot(String& errorMsg, bool skipValidation = false);
    bool slotExists(int slotNum) const;
    bool deleteSlot(int slotNum, String& errorMsg);
    void clearActiveSlot();
    
    // Preview mode - loads slot into globalState without applying to hardware
    // Just tracks which slot we should return to when done
    bool enterPreviewMode(int slotToPreview, String& errorMsg);
    bool isPreviewMode() const { return previewModeActive; }
    int getPreviewedSlotNumber() const { return previewSlotNumber; }
    int getOriginalSlotNumber() const { return originalSlotNumber; }
    void clearPreviewMode();  // Clear preview flag without loading anything
    bool exitPreview(bool applyPreview, String& errorMsg);  // Exit preview, optionally applying or reverting
    
    // Slot tracking synchronization
    void setActiveSlot(int slotNum);  // Sets activeSlotNumber and syncs with global netSlot
    void syncFromGlobalNetSlot();     // Updates activeSlotNumber from netSlot (for external changes)
    
    // Temporary slot mode - for apps that need a working slot and want to restore when done
    // This is different from preview mode: temp mode clears the slot and applies to hardware
    bool enterTemporarySlot(int tempSlot, bool saveCurrentFirst = true);  // Enter temp slot, returns false on error
    bool exitTemporarySlot(bool refreshHardware = true);                   // Return to original slot
    bool isTemporarySlotMode() const { return temporarySlotActive; }
    int getTemporarySlotOriginal() const { return temporarySlotOriginal; }
    
    // Create slot files on-demand (not pre-created)
    bool ensureSlotExists(int slotNum);
    
    // History management (undo/redo)
    void pushHistory();
    bool canUndo() const;
    bool canRedo() const;
    bool undo(String& errorMsg);
    bool redo(String& errorMsg);
    void clearHistory();
    int getHistorySize() const { return STATE_HISTORY_SIZE; }
    int getHistoryDepth() const;  // How many undo steps are available
    
    // Legacy support
    bool migrateOldSlotFile(int slotNum, String& errorMsg);
    
    // Utility
    void printSlotInfo(int slotNum);
    void listSlots();
    size_t getActiveStateRAMUsage() const;
    
    // File I/O helpers (public for direct slot file manipulation)
    bool readSlotFile(int slotNum, String& content, String& errorMsg);
    bool writeSlotFile(int slotNum, const String& content, String& errorMsg);
    
private:
    SlotManager();
    ~SlotManager() = default;
    
    // State storage - reference to globalState (no duplication!)
    JumperlessState& activeState;
    int activeSlotNumber;
    
    // Preview mode state
    bool previewModeActive;
    int previewSlotNumber;       // Which slot we're previewing
    int originalSlotNumber;      // Which slot to return to when done
    float originalRailVoltages[2]; // Save rail voltages (topRail, bottomRail) during preview
    
    // Temporary slot mode state (for apps using a working slot)
    bool temporarySlotActive;
    int temporarySlotOriginal;   // Which slot to return to when exiting temp mode
    
    // History buffer (circular buffer for undo/redo)
    JumperlessState* historyBuffer;
    int historySize;
    int historyHead;       // Points to next write position
    int historyCount;      // Number of valid history entries
    int historyPosition;   // Current position in history (for redo)
    
    // File name helpers
    String getSlotFilename(int slotNum) const;  // Returns .yaml filename
    String getLegacySlotFilename(int slotNum) const;  // Returns old .txt filename
    String getJSONSlotFilename(int slotNum) const;  // Returns old .json filename
    
    // History helpers
    void initHistory();
    void cleanupHistory();
    int historyIndex(int offset) const;  // Helper for circular buffer indexing
};

// Global singleton declaration - THE single source of truth for all Jumperless state
// This replaces the old global arrays (net[], path[], ch[]) with a unified state object
extern JumperlessState globalState;

// Global helpers for easy access
namespace StateHelpers {
    // Quick access to common operations
    inline JumperlessState& getState() { return SlotManager::getInstance().getActiveState(); }
    inline bool addConnection(int n1, int n2, int dup = -1) { String err; return getState().addConnection(n1, n2, err, dup); }
    inline bool removeConnection(int n1, int n2) { String err; return getState().removeConnection(n1, n2, err); }
    inline void setDac(int dac, float voltage) { getState().setDacVoltage(dac, voltage); }
    inline void setRail(bool top, float voltage) { getState().setRailVoltage(top, voltage); }
    inline int getDuplicates(int n1, int n2) { return getState().getConnectionDuplicates(n1, n2); }
    inline bool setDuplicates(int n1, int n2, int dup) { String err; return getState().setConnectionDuplicates(n1, n2, dup, err); }
    
    // Undo/redo shortcuts
    inline bool undo() { String err; return SlotManager::getInstance().undo(err); }
    inline bool redo() { String err; return SlotManager::getInstance().redo(err); }
    
    // Slot operations
    inline bool saveSlot(int slot) { String err; return SlotManager::getInstance().saveSlot(slot, err); }
    inline bool loadSlot(int slot) { String err; return SlotManager::getInstance().loadSlot(slot, err); }
}

// ============================================================================
// Custom Net Name Functions  
// ============================================================================
// Custom names are stored in DisplayState by NET NUMBER (not array index)
// This means names persist correctly when nets are added/deleted/reordered

void setCustomNetName(int netNum, const char* name);  // Set custom name (nullptr/empty to clear)
bool hasCustomNetName(int netNum);                     // Check if net has custom name
void clearAllCustomNetNames(void);                     // Reset all names to defaults

// ============================================================================
// Hardware Application Function
// ============================================================================

void applyStateToHardware(void);  // Apply globalState settings to hardware (DACs, GPIO, etc.)

// ============================================================================
// State Backup/Restore Functions (for MicroPython entry/exit, undo, etc.)
// Uses compressed YAML format - only stores actual connections, not empty array slots
// ============================================================================

void storeStateBackup(void);                    // Store compressed YAML snapshot of globalState
void restoreStateBackup(bool autoSave = false); // Restore from backup (optionally save to slot)
void restoreAndSaveStateBackup(void);           // Restore and immediately save to current slot
void clearStateBackup(void);                    // Clear the backup
bool hasStateBackup(void);                      // Check if backup exists
bool hasStateChanges(void);                     // Compare current state with backup
size_t getStateBackupSize(void);                // Get backup size in bytes (for diagnostics)
void printStateBackupInfo(void);                // Print detailed memory usage statistics

#endif // STATES_H

