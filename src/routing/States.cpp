// SPDX-License-Identifier: MIT
#include "States.h"
#include "MatrixState.h"
#include "NetManager.h"
#include "FileParsing.h"
#include "Commands.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "EkiloEditor.h"
#include "Colors.h"
#include "config.h"
#include "FakeGpio.h"
#include "GraphicOverlays.h"
#include "PartPlacement.h"  // parts: section serializer/parser + expansion
#include <string.h>
#include <vector>
#include <FatFS.h>
#include <hardware/gpio.h>
#include "externVars.h"  // For fs_mutex filesystem synchronization
#include "FilesystemStuff.h"  // For safe file operations
#include "FileCache.h"  // fileCacheFlushNowAll() big-event triggers
#include "Undo.h"  // Phase 4.1 - undo log mutation hooks
#include "InfraPaths.h"  // infraIsBridge / infraScrubLoadedBridges / system allowance

// ============================================================================
// CRITICAL MEMORY SAFETY NOTES
// ============================================================================
//
// JumperlessState and ConnectionState contain MASSIVE arrays:
//   - bridges[192][3]        = 2,304 bytes
//   - bridgeColors[192]      = 768 bytes
//   - nets[60]               = ~30 KB (depends on netStruct size)
//   - paths[192]             = ~20 KB (depends on pathStruct size)
//   
// TOTAL SIZE: ~50+ KB per JumperlessState instance!
//
// **NEVER COPY THESE OBJECTS!**
//   - Copying exhausts limited stack memory on embedded systems
//   - Results in crashes, hard faults, or unpredictable behavior
//   - Always use references (&) or pointers (*)
//   - Copy constructor and assignment operator are DELETED to enforce this
//
// CORRECT:   JumperlessState& state = SlotManager::getInstance().getActiveState();
// WRONG:     JumperlessState state = SlotManager::getInstance().getActiveState();  // CRASH!
//
// If you need to pass state to a function:
// CORRECT:   void processState(JumperlessState& state) { ... }
// WRONG:     void processState(JumperlessState state) { ... }  // CRASH!
//
// ============================================================================

extern struct config jumperlessConfig;
extern volatile bool core1busy;
extern volatile bool core2busy;
extern int netSlot;  // Global slot number (defined in RotaryEncoder.cpp)
extern const int gpioDef[10][3];  // GPIO pin definitions (defined in Peripherals.h)
extern uint8_t gpioState[50];  // GPIO state for animations - 10 real + 8 fake out + 32 fake in (defined in Peripherals.cpp)
extern bool debugFP;  // Debug flag for file parsing (defined in FileParsing.cpp)

// Forward declarations for YAML parsing helpers
int parseNodeName(const String& nodeName);
String nodeValueToString(int nodeValue);
uint32_t parseColorValue(const String& colorStr, bool& success);
String colorValueToName(uint32_t color);
bool parseBoolean(const String& val, bool& success);
String booleanToString(bool value);

// Global singleton - THE single source of truth for all Jumperless state
JumperlessState globalState;

// Set custom net name - stored by NET NUMBER in DisplayState
// Pass empty string or nullptr to clear
// Tracking by net number means names follow nets when they shift!
void setCustomNetName(int netNum, const char* name) {
    if (netNum < 0 || netNum >= MAX_NETS) return;
    
    if (name == nullptr || name[0] == '\0') {
        // Clear custom name
        globalState.display.removeNetName(netNum);
    } else {
        // Store custom name (tracked by net number)
        globalState.display.setNetName(netNum, name);
    }
}

// Check if net has custom name
bool hasCustomNetName(int netNum) {
    if (netNum < 0 || netNum >= MAX_NETS) return false;
    return (globalState.display.getNetName(netNum) != nullptr);
}

// Clear all custom net names
void clearAllCustomNetNames() {
    globalState.display.numCustomNames = 0;
}

// ============================================================================
// ConnectionState Implementation
// ============================================================================

ConnectionState::ConnectionState() {
    clear();
}

void ConnectionState::clear() {
    memset(bridges, 0, sizeof(bridges));
    // Initialize bridgeColors to 0xFFFFFFFF (no color sentinel)
    for (int i = 0; i < MAX_BRIDGES; i++) {
        bridgeColors[i] = 0xFFFFFFFF;
    }
    numBridges = 0;
    numNets = 0;
    numPaths = 0;
    pathsCacheValid = false;
    chipStatesCacheValid = false;
    clearAllNTCC();

    // Clear nets
    for (int i = 0; i < MAX_NETS; i++) {
        memset(&nets[i], 0, sizeof(netStruct));
    }
    
    // Clear paths
    memset(paths, 0, sizeof(paths));
    
    // Clear chip states
    for (int i = 0; i < 12; i++) {
        memset(&chipStates[i], 0, sizeof(chipStatus));
        memset(&chipXY[i], 0, sizeof(struct justXY));
    }

    // Restore locked connections after all state has been reset.
    // (locked connections are re-added by infraEvaluate at the next rebuild)
}

void ConnectionState::invalidateCache(bool autoRefresh) {
    pathsCacheValid = false;
    chipStatesCacheValid = false;
    
    // Optionally trigger hardware refresh
    if (autoRefresh) {
        refreshConnections(-1);
    }
}

void ConnectionState::recomputePaths() {
    // This will be called to rebuild paths from bridges
    // The actual computation is done by bridgesToPaths() in NetsToChipConnections.cpp
    pathsCacheValid = false;
    chipStatesCacheValid = false;
}

void ConnectionState::syncBridgesFromNets() {
    // Generate bridges from net node lists
    // Used when NETS_PRIMARY mode is active
    numBridges = 0;

    for (int netIdx = 0; netIdx < numNets && netIdx < MAX_NETS; netIdx++) {
        netStruct& currentNet = nets[netIdx];

        // Skip empty nets
        if (currentNet.nodes[0] == 0) continue;

        // Connect all nodes in this net
        for (int i = 0; i < MAX_NODES && currentNet.nodes[i] != 0; i++) {
            for (int j = i + 1; j < MAX_NODES && currentNet.nodes[j] != 0; j++) {
                if (numBridges >= MAX_BRIDGES) {
                    Serial.println("Warning: Maximum bridges reached during sync");
                    return;
                }

                // Add bridge
                bridges[numBridges][0] = currentNet.nodes[i];
                bridges[numBridges][1] = currentNet.nodes[j];
                bridges[numBridges][2] = currentNet.numberOfDuplicates;
                // bridgeColors is index-parallel to bridges and this rebuild
                // just re-derived every row - a leftover color from whatever
                // used to sit at this index would attach to the wrong bridge.
                bridgeColors[numBridges] = 0xFFFFFFFF;
                numBridges++;
            }
        }
    }
    for (int i = numBridges; i < MAX_BRIDGES; i++) {
        bridgeColors[i] = 0xFFFFFFFF;
    }

    invalidateCache(false);
}

void ConnectionState::syncNetsFromBridges() {
    // Generate nets from bridges
    // Used when BRIDGES_PRIMARY mode is active (default)
    // This is typically done by the existing net management code
    // We just invalidate to trigger recomputation
    invalidateCache(false);
}

// ============================================================================
// PowerState Implementation
// ============================================================================

PowerState::PowerState() {
    setDefaults();
}

void PowerState::setDefaults() {
    topRail = 0.0f;
    bottomRail = 0.0f;
    dac0 = 3.33f;
    dac1 = 0.0f;

    // If probe auto-connect is enabled, DAC0 - the probe feed, the only path
    // INA1's switch sensing can see - must sit at the calibrated
    // measure_mode_output_voltage so the probe sense voltage divider reads
    // correctly (and so the feed's viability window [2.80, 3.90] passes at
    // boot). DAC1 is never a feed; it keeps its plain default.
    //
    // Gating: configLoaded guards against reading jumperlessConfig before
    // it loads; auto_connect_probe <= 0 means probe auto-wiring is off (the
    // same gate infra's enProbePower() applies), so leave both DACs at
    // their non-probe defaults.
    extern volatile bool configLoaded;
    if (configLoaded && jumperlessConfig.dacs.auto_connect_probe > 0) {
        dac0 = jumperlessConfig.calibration.measure_mode_output_voltage;
    }
}

bool PowerState::validate(String& errorMsg) const {
    // Check voltage limits (±8V typical for DACs)
    if (dac0 < -8.0f || dac0 > 8.0f) {
        errorMsg = "DAC 0 voltage out of range (-8V to +8V): " + String(dac0, 2) + "V";
        return false;
    }
    if (dac1 < -8.0f || dac1 > 8.0f) {
        errorMsg = "DAC 1 voltage out of range (-8V to +8V): " + String(dac1, 2) + "V";
        return false;
    }
    if (topRail < -8.0f || topRail > 8.0f) {
        errorMsg = "Top rail voltage out of range (-8V to +8V): " + String(topRail, 2) + "V";
        return false;
    }
    if (bottomRail < -8.0f || bottomRail > 8.0f) {
        errorMsg = "Bottom rail voltage out of range (-8V to +8V): " + String(bottomRail, 2) + "V";
        return false;
    }
    return true;
}

// ============================================================================
// DisplayState Implementation
// ============================================================================

DisplayState::DisplayState() {
    clear();
}

void DisplayState::clear() {
    memset(customColors, 0, sizeof(customColors));
    numCustomColors = 0;
    memset(customNames, 0, sizeof(customNames));
    numCustomNames = 0;
}

void DisplayState::setNetColor(int netNum, rgbColor color, uint32_t raw, const char* name) {
    // Get the first node of this net for reconciliation after rebuilds
    int firstNode = 0;
    if (netNum >= 0 && netNum < MAX_NETS && globalState.connections.nets[netNum].nodes[0] != 0) {
        firstNode = globalState.connections.nets[netNum].nodes[0];
    }
    
    // Check if already exists
    for (int i = 0; i < numCustomColors; i++) {
        if (customColors[i].netNumber == netNum) {
            customColors[i].color = color;
            customColors[i].rawColor = raw;
            customColors[i].firstNode = firstNode;
            strncpy(customColors[i].colorName, name, 31);
            customColors[i].colorName[31] = '\0';
            return;
        }
    }
    
    // Add new if space available
    if (numCustomColors < MAX_NETS) {
        customColors[numCustomColors].netNumber = netNum;
        customColors[numCustomColors].firstNode = firstNode;
        customColors[numCustomColors].color = color;
        customColors[numCustomColors].rawColor = raw;
        strncpy(customColors[numCustomColors].colorName, name, 31);
        customColors[numCustomColors].colorName[31] = '\0';
        numCustomColors++;
    }
}

bool DisplayState::getNetColor(int netNum, rgbColor& color, uint32_t& raw, char* name) const {
    for (int i = 0; i < numCustomColors; i++) {
        if (customColors[i].netNumber == netNum) {
            color = customColors[i].color;
            raw = customColors[i].rawColor;
            if (name) {
                strcpy(name, customColors[i].colorName);
            }
            return true;
        }
    }
    return false;
}

void DisplayState::removeNetColor(int netNum) {
    for (int i = 0; i < numCustomColors; i++) {
        if (customColors[i].netNumber == netNum) {
            // Shift remaining entries down
            for (int j = i; j < numCustomColors - 1; j++) {
                customColors[j] = customColors[j + 1];
            }
            numCustomColors--;
            return;
        }
    }
}

void DisplayState::setNetName(int netNum, const char* name) {
    // Get the first node of this net for reconciliation after rebuilds
    int firstNode = 0;
    if (netNum >= 0 && netNum < MAX_NETS && globalState.connections.nets[netNum].nodes[0] != 0) {
        firstNode = globalState.connections.nets[netNum].nodes[0];
    }
    
    // Check if already exists - update it
    for (int i = 0; i < numCustomNames; i++) {
        if (customNames[i].netNumber == netNum) {
            strncpy(customNames[i].name, name, 31);
            customNames[i].name[31] = '\0';
            customNames[i].firstNode = firstNode;
            return;
        }
    }
    
    // Add new entry if space available
    if (numCustomNames < MAX_NETS) {
        customNames[numCustomNames].netNumber = netNum;
        customNames[numCustomNames].firstNode = firstNode;
        strncpy(customNames[numCustomNames].name, name, 31);
        customNames[numCustomNames].name[31] = '\0';
        numCustomNames++;
    }
}

const char* DisplayState::getNetName(int netNum) const {
    for (int i = 0; i < numCustomNames; i++) {
        if (customNames[i].netNumber == netNum) {
            return customNames[i].name;
        }
    }
    return nullptr;  // Not found
}

void DisplayState::removeNetName(int netNum) {
    for (int i = 0; i < numCustomNames; i++) {
        if (customNames[i].netNumber == netNum) {
            // Shift remaining entries down
            for (int j = i; j < numCustomNames - 1; j++) {
                customNames[j] = customNames[j + 1];
            }
            numCustomNames--;
            return;
        }
    }
}

// Debug flag for DisplayState reconciliation
static bool debugReconcile = false;

void DisplayState::reconcileAfterRebuild() {
    // After nets are rebuilt from bridges, net numbers may have changed.
    // Use firstNode to find where each custom entry's net ended up.
    extern int findNodeInNet(int node);
    
    if (debugReconcile) Serial.printf("reconcileAfterRebuild: %d names, %d colors\n", numCustomNames, numCustomColors);
    
    // Reconcile custom names
    for (int i = numCustomNames - 1; i >= 0; i--) {
        if (debugReconcile) Serial.printf("  Name[%d]: net=%d, firstNode=%d, name=%s\n", 
            i, customNames[i].netNumber, customNames[i].firstNode, customNames[i].name);
        
        if (customNames[i].firstNode <= 0) {
            // Entry has no firstNode (loaded from YAML before nets were built)
            // Check if the net still exists at this number - if so, fix the firstNode
            int netNum = customNames[i].netNumber;
            if (netNum >= 0 && netNum < MAX_NETS && 
                globalState.connections.nets[netNum].number > 0 &&
                globalState.connections.nets[netNum].nodes[0] != 0) {
                // Net exists - fix the firstNode
                customNames[i].firstNode = globalState.connections.nets[netNum].nodes[0];
                if (debugReconcile) Serial.printf("    -> FIX firstNode to %d (net exists)\n", customNames[i].firstNode);
            } else {
                if (debugReconcile) Serial.println("    -> SKIP (no firstNode, net doesn't exist)");
            }
            continue;
        }
        
        int newNetNum = findNodeInNet(customNames[i].firstNode);
        if (debugReconcile) Serial.printf("    -> findNodeInNet(%d) = %d\n", customNames[i].firstNode, newNetNum);
        
        if (newNetNum <= 0) {
            if (debugReconcile) Serial.println("    -> REMOVE (node not in any net)");
            for (int j = i; j < numCustomNames - 1; j++) {
                customNames[j] = customNames[j + 1];
            }
            numCustomNames--;
        } else if (newNetNum != customNames[i].netNumber) {
            if (debugReconcile) Serial.printf("    -> UPDATE netNumber %d -> %d\n", customNames[i].netNumber, newNetNum);
            customNames[i].netNumber = newNetNum;
        } else {
            if (debugReconcile) Serial.println("    -> KEEP (unchanged)");
        }
    }
    
    // Reconcile custom colors
    for (int i = numCustomColors - 1; i >= 0; i--) {
        if (debugReconcile) Serial.printf("  Color[%d]: net=%d, firstNode=%d, color=%s\n", 
            i, customColors[i].netNumber, customColors[i].firstNode, customColors[i].colorName);
        
        if (customColors[i].firstNode <= 0) {
            // Entry has no firstNode (loaded from YAML before nets were built)
            // Check if the net still exists at this number - if so, fix the firstNode
            int netNum = customColors[i].netNumber;
            if (netNum >= 0 && netNum < MAX_NETS && 
                globalState.connections.nets[netNum].number > 0 &&
                globalState.connections.nets[netNum].nodes[0] != 0) {
                // Net exists - fix the firstNode
                customColors[i].firstNode = globalState.connections.nets[netNum].nodes[0];
                if (debugReconcile) Serial.printf("    -> FIX firstNode to %d (net exists)\n", customColors[i].firstNode);
            } else {
                if (debugReconcile) Serial.println("    -> SKIP (no firstNode, net doesn't exist)");
            }
            continue;
        }
        
        int newNetNum = findNodeInNet(customColors[i].firstNode);
        if (debugReconcile) Serial.printf("    -> findNodeInNet(%d) = %d\n", customColors[i].firstNode, newNetNum);
        
        if (newNetNum <= 0) {
            if (debugReconcile) Serial.println("    -> REMOVE (node not in any net)");
            for (int j = i; j < numCustomColors - 1; j++) {
                customColors[j] = customColors[j + 1];
            }
            numCustomColors--;
        } else if (newNetNum != customColors[i].netNumber) {
            if (debugReconcile) Serial.printf("    -> UPDATE netNumber %d -> %d\n", customColors[i].netNumber, newNetNum);
            customColors[i].netNumber = newNetNum;
        } else {
            if (debugReconcile) Serial.println("    -> KEEP (unchanged)");
        }
    }
    
    // Clean up stale entries: remove entries with firstNode=0 if a proper entry exists for same net
    for (int i = numCustomNames - 1; i >= 0; i--) {
        if (customNames[i].firstNode != 0) continue;  // Skip proper entries
        // Check if there's a better entry for this netNumber
        for (int j = 0; j < numCustomNames; j++) {
            if (j != i && customNames[j].netNumber == customNames[i].netNumber && customNames[j].firstNode > 0) {
                // Remove the stale entry
                if (debugReconcile) Serial.printf("  Removing stale name entry for net %d (has no firstNode)\n", customNames[i].netNumber);
                for (int k = i; k < numCustomNames - 1; k++) {
                    customNames[k] = customNames[k + 1];
                }
                numCustomNames--;
                break;
            }
        }
    }
    
    for (int i = numCustomColors - 1; i >= 0; i--) {
        if (customColors[i].firstNode != 0) continue;  // Skip proper entries
        // Check if there's a better entry for this netNumber
        for (int j = 0; j < numCustomColors; j++) {
            if (j != i && customColors[j].netNumber == customColors[i].netNumber && customColors[j].firstNode > 0) {
                // Remove the stale entry
                if (debugReconcile) Serial.printf("  Removing stale color entry for net %d (has no firstNode)\n", customColors[i].netNumber);
                for (int k = i; k < numCustomColors - 1; k++) {
                    customColors[k] = customColors[k + 1];
                }
                numCustomColors--;
                break;
            }
        }
    }
    
    if (debugReconcile) Serial.printf("reconcileAfterRebuild DONE: %d names, %d colors\n", numCustomNames, numCustomColors);
}

// ============================================================================
// ConfigState Implementation
// ============================================================================

ConfigState::ConfigState() {
    setDefaults();
}

void ConfigState::setDefaults() {
    // Source of truth defaults
    sourceOfTruth = BRIDGES_PRIMARY;  // Default: bridges define connections
    
    // Routing preferences
    stackPaths = 2;
    stackRails = 3;
    stackDacs = 0;
    railPriority = 1;
    
    // Default GPIO to inputs with pull-down
    for (int i = 0; i < 10; i++) {
        gpioDirection[i] = 1;  // input
        gpioPulls[i] = 0;      // pull down
        gpioPwmFrequency[i] = 1.0f;
        gpioPwmDutyCycle[i] = 0.5f;
        gpioPwmEnabled[i] = false;
        // Pins 1-8 (indices 0-7) default to floating-read enabled; UART TX/RX (8-9) disabled
        gpioReadFloating[i] = (i < 8) ? 1 : 0;
        gpioPythonOwned[i] = false;  // No pins owned by MicroPython by default
    }
    
    // UART defaults
    uartTxFunction = 0;  // TX
    uartRxFunction = 1;  // RX
    
    // OLED defaults
    oledConnected = false;
    oledLockConnection = false;
    
    // Auto-refresh defaults
    autoRefreshOnChange = false;  // Default: don't auto-refresh (user controls when)
}

// ============================================================================
// FakeGPIO Restoration
// ============================================================================

// Global list of pending FakeGPIO restorations (populated during YAML load, applied after bridges are restored)
std::vector<FakeGpioRestorationInfo> pendingFakeGpioRestorations;

// ============================================================================
// JumperlessState Implementation
// ============================================================================

JumperlessState::JumperlessState() {
    version = 2;  // Current format version (2 = YAML format)
    dirty = false;
    lastModifiedTime = 0;
    numEphemeralConnections = 0;
    clear();
}

void JumperlessState::clear() {
    connections.clear();
    power.setDefaults();
    display.clear();
    config.setDefaults();
    parts.clear();             // parts table + guideProgress scalars
    runSource[0] = '\0';       // per-run project origin (see States.h)
    clearAllCustomNetNames();  // Reset all custom net names to defaults
    dirty = false;
    lastModifiedTime = 0;
}

void JumperlessState::markDirty() {
    if (!dirty && debugWaitLoopTiming) {
        Serial.printf("DEBUG: State marked dirty at %lu ms\n", millis());
        // Print stack trace to help debug what's causing spurious dirty marks
        Serial.println("DEBUG: markDirty() called from:");
        // Print return address to help identify caller
        void* caller = __builtin_return_address(0);
        Serial.printf("  Caller address: %p\n", caller);
    }
    dirty = true;
    lastModifiedTime = millis();
}

void JumperlessState::clearDirty() {
    if (dirty && debugWaitLoopTiming) {
        Serial.printf("DEBUG: State cleared dirty at %lu ms (was dirty for %lu ms)\n", 
                     millis(), millis() - lastModifiedTime);
    }
    dirty = false;
}

// Connection management
bool JumperlessState::addConnection(int node1, int node2, String& errorMsg, int duplicates) {
    // Validate nodes
    if (!isNodeValid(node1)) {
        errorMsg = "Invalid node 1: " + String(node1);
        return false;
    }
    if (!isNodeValid(node2)) {
        errorMsg = "Invalid node 2: " + String(node2);
        return false;
    }
    
    // Check if connection is allowed
    if (!isConnectionAllowed(node1, node2, errorMsg)) {
        return false;
    }
    // Serial.println("Adding connection: " + String(node1) + " - " + String(node2));
    // Serial.println("numBridges: " + String(connections.numBridges));
    // Serial.flush();
    // Check for duplicate - if it exists, update the duplicate count
    for (int i = 0; i < connections.numBridges; i++) {
        if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
            (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
            // Connection exists - update duplicates based on parameter
            if (duplicates >= 0) {
                // Specific duplicate count provided - replace the value
                // Serial.println("Updating duplicate count for connection: " + String(node1) + " - " + String(node2) + " to " + String(duplicates));
                connections.bridges[i][2] = duplicates;
            } else {
                // No duplicate count specified - increment existing count
                connections.bridges[i][2]++;
            }
            connections.invalidateCache(config.autoRefreshOnChange);
            markDirty();
            return true;
        }
    }
    
    // Check if we have space
    if (connections.numBridges >= MAX_BRIDGES) {
        errorMsg = "Maximum number of connections reached (" + String(MAX_BRIDGES) + ")";
        return false;
    }
    
    // Determine number of duplicates: -1 means use default from config
    int numDuplicates;
    if (duplicates < 0) {
        numDuplicates = config.stackPaths;  // Use default from config
    } else {
        numDuplicates = duplicates;
    }
    
    // Add the bridge
    int idx = connections.numBridges;
    connections.bridges[idx][0] = node1;
    connections.bridges[idx][1] = node2;
    connections.bridges[idx][2] = numDuplicates;  // Store duplicates
    connections.numBridges++;
    
    // Update FakeGPIO state - reassigns ADC if we just claimed one that was in use
    updateFakeGpioAfterConnectionChange(node1, node2);
    
    // Invalidate caches - paths need to be recalculated
    connections.invalidateCache(config.autoRefreshOnChange);
    markDirty();

    // Phase 4.1: undo log hook. Skip while we're applying a delta -
    // we don't want to record our own undo/redo as new history.
    if (!g_undoApplying) {
        uint32_t color = connections.bridgeColors[idx];
        undoRecordConnect(node1, node2, color);
    }
    
    return true;
}

bool JumperlessState::removeConnection(int node1, int node2, String& errorMsg) {
    // Find the connection
    int foundIdx = -1;
    for (int i = 0; i < connections.numBridges; i++) {
        if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
            (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
            foundIdx = i;
            break;
        }
    }
    
    if (foundIdx == -1) {
        errorMsg = "Connection " + String(node1) + "-" + String(node2) + " not found";
        return false;
    }

    // Phase 4.1: capture color before we drop it for undo.
    uint32_t removedColor = connections.bridgeColors[foundIdx];

    // Remove by shifting remaining entries
    for (int i = foundIdx; i < connections.numBridges - 1; i++) {
        connections.bridges[i][0] = connections.bridges[i + 1][0];
        connections.bridges[i][1] = connections.bridges[i + 1][1];
        connections.bridges[i][2] = connections.bridges[i + 1][2];
        connections.bridgeColors[i] = connections.bridgeColors[i + 1];  // CRITICAL: Shift colors too!
    }
    connections.numBridges--;
    connections.bridgeColors[connections.numBridges] = 0xFFFFFFFF;  // Clear the last color slot
    
    // Update FakeGPIO state - deactivates any fake GPIO that was using this connection
    // This ensures removed fake GPIOs won't be serialized to YAML
    updateFakeGpioAfterConnectionChange(node1, node2);
    
    // Invalidate caches
    connections.invalidateCache(config.autoRefreshOnChange);
    markDirty();

    if (!g_undoApplying) {
        undoRecordDisconnect(node1, node2, removedColor);
    }
    
    return true;
}

bool JumperlessState::hasConnection(int node1, int node2) const {
    for (int i = 0; i < connections.numBridges; i++) {
        if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
            (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
            return true;
        }
    }
    return false;
}

void JumperlessState::clearAllConnections() {
    if (!g_undoApplying) {
        undoRecordClearAll();
    }

    connections.clear();
    
    // Clear all fake GPIO state so they won't be serialized to YAML
    clearAllFakeGpio();
    
    markDirty();
    
    // Restore locked connections after clearing
    // (locked connections are re-added by infraEvaluate at the next rebuild)

    // Big-event flush: clear-all is destructive and the user already
    // expects a brief moment of "saving" - we may as well commit now so
    // a power loss can't lose the clear itself.
    fileCacheFlushNowAll("clear_all");
}

int JumperlessState::getConnectionDuplicates(int node1, int node2) const {
    for (int i = 0; i < connections.numBridges; i++) {
        if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
            (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
            return connections.bridges[i][2];  // Return duplicates from third element
        }
    }
    return 0;  // Not found
}

bool JumperlessState::setConnectionDuplicates(int node1, int node2, int duplicates, String& errorMsg) {
    if (duplicates < 0) {
        errorMsg = "Duplicates must be non-negative: " + String(duplicates);
        return false;
    }
    
    for (int i = 0; i < connections.numBridges; i++) {
        if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
            (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
            connections.bridges[i][2] = duplicates;  // Store duplicates in third element
            connections.invalidateCache(config.autoRefreshOnChange);
            markDirty();
            return true;
        }
    }
    
    errorMsg = "Connection " + String(node1) + "-" + String(node2) + " not found";
    return false;
}

// ============================================================================
// Ephemeral Connection Management
// ============================================================================
// Ephemeral connections are temporary - they are added to the bridges array
// for routing but are NEVER saved to YAML and do NOT trigger markDirty().
//
// Key features:
// - Are physically routed through the crossbar switches (so signals pass through)
// - Are NEVER saved to flash/YAML (filtered out in serializeBridges)
// - Do NOT trigger markDirty() (won't cause auto-save)
// - Can optionally apply routing immediately with optional LED updates
// - Support optional color for visual feedback during measurements

bool JumperlessState::addEphemeralConnection(int node1, int node2, String& errorMsg,
                                             bool applyRouting, int ledShowOption,
                                             uint32_t color) {
    // Validate nodes
    if (!isNodeValid(node1)) {
        errorMsg = "Invalid node 1: " + String(node1);
        return false;
    }
    if (!isNodeValid(node2)) {
        errorMsg = "Invalid node 2: " + String(node2);
        return false;
    }
    
    // Check if connection is allowed
    if (!isConnectionAllowed(node1, node2, errorMsg)) {
        return false;
    }
    
    // Check if already exists as regular connection
    if (hasConnection(node1, node2)) {
        // Connection already exists - just track it as ephemeral
        // (might have been added by user before measure mode)
        // Still apply routing if requested (in case hardware state is stale)
        if (applyRouting) {
            refreshLocalConnections(ledShowOption, 0, 0);
            waitCore2();
        }
        return true;
    }
    
    // Check if already tracked as ephemeral
    for (int i = 0; i < numEphemeralConnections; i++) {
        if ((ephemeralConnections[i].node1 == node1 && ephemeralConnections[i].node2 == node2) ||
            (ephemeralConnections[i].node1 == node2 && ephemeralConnections[i].node2 == node1)) {
            // Already tracked - still apply routing if requested
            if (applyRouting) {
                refreshLocalConnections(ledShowOption, 0, 0);
                waitCore2();
            }
            return true;  // Already tracked
        }
    }
    
    // Check if we have space for ephemeral tracking
    if (numEphemeralConnections >= MAX_EPHEMERAL_CONNECTIONS) {
        errorMsg = "Maximum ephemeral connections reached";
        return false;
    }
    
    // Check if we have space in bridges array
    if (connections.numBridges >= MAX_BRIDGES) {
        errorMsg = "Maximum number of connections reached";
        return false;
    }
    
    // Add the bridge (with 0 duplicates for minimal routing overhead)
    int idx = connections.numBridges;
    connections.bridges[idx][0] = node1;
    connections.bridges[idx][1] = node2;
    connections.bridges[idx][2] = 0;  // No duplicate paths needed for measurement
    connections.bridgeColors[idx] = color;  // Set optional color for visual feedback
    connections.numBridges++;
    
    // Track as ephemeral
    ephemeralConnections[numEphemeralConnections] = EphemeralConnection(node1, node2, idx);
    numEphemeralConnections++;
    
    // Invalidate caches to trigger routing, but DON'T mark dirty
    // Note: We disable autoRefresh here since we'll handle it manually if requested
    connections.invalidateCache(false);
    // NOTE: No markDirty() call - this is the key difference from addConnection()
    
    // Apply routing to hardware if requested
    if (applyRouting) {
        refreshLocalConnections(ledShowOption, 0, 0);
        waitCore2();
    }
    
    return true;
}

bool JumperlessState::removeEphemeralConnection(int node1, int node2, String& errorMsg,
                                                bool applyRouting, int ledShowOption) {
    // Find in ephemeral tracking
    int ephIdx = -1;
    for (int i = 0; i < numEphemeralConnections; i++) {
        if ((ephemeralConnections[i].node1 == node1 && ephemeralConnections[i].node2 == node2) ||
            (ephemeralConnections[i].node1 == node2 && ephemeralConnections[i].node2 == node1)) {
            ephIdx = i;
            break;
        }
    }
    
    if (ephIdx == -1) {
        errorMsg = "Ephemeral connection not found";
        return false;
    }
    
    // Find and remove from bridges array
    int bridgeIdx = -1;
    for (int i = 0; i < connections.numBridges; i++) {
        if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
            (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
            bridgeIdx = i;
            break;
        }
    }
    
    if (bridgeIdx != -1) {
        // Remove bridge by shifting remaining entries
        for (int i = bridgeIdx; i < connections.numBridges - 1; i++) {
            connections.bridges[i][0] = connections.bridges[i + 1][0];
            connections.bridges[i][1] = connections.bridges[i + 1][1];
            connections.bridges[i][2] = connections.bridges[i + 1][2];
            connections.bridgeColors[i] = connections.bridgeColors[i + 1];
        }
        connections.numBridges--;
        connections.bridgeColors[connections.numBridges] = 0xFFFFFFFF;
        
        // Update bridge indices in remaining ephemeral connections
        for (int i = 0; i < numEphemeralConnections; i++) {
            if (ephemeralConnections[i].bridgeIndex > bridgeIdx) {
                ephemeralConnections[i].bridgeIndex--;
            }
        }
    }
    
    // Remove from ephemeral tracking
    for (int i = ephIdx; i < numEphemeralConnections - 1; i++) {
        ephemeralConnections[i] = ephemeralConnections[i + 1];
    }
    numEphemeralConnections--;
    
    // Invalidate caches, but DON'T mark dirty
    // Note: We disable autoRefresh here since we'll handle it manually if requested
    connections.invalidateCache(false);
    
    // Apply routing to hardware if requested
    if (applyRouting) {
        refreshLocalConnections(ledShowOption, 0, 0);
        waitCore2();
    }
    
    return true;
}

void JumperlessState::clearAllEphemeralConnections(bool applyRouting, int ledShowOption) {
    // Track if we actually removed anything
    int removedCount = numEphemeralConnections;
    
    // Remove all ephemeral connections from bridges (in reverse order to avoid index issues)
    for (int e = numEphemeralConnections - 1; e >= 0; e--) {
        int node1 = ephemeralConnections[e].node1;
        int node2 = ephemeralConnections[e].node2;
        
        // Find and remove from bridges array
        for (int i = 0; i < connections.numBridges; i++) {
            if ((connections.bridges[i][0] == node1 && connections.bridges[i][1] == node2) ||
                (connections.bridges[i][0] == node2 && connections.bridges[i][1] == node1)) {
                // Remove bridge by shifting
                for (int j = i; j < connections.numBridges - 1; j++) {
                    connections.bridges[j][0] = connections.bridges[j + 1][0];
                    connections.bridges[j][1] = connections.bridges[j + 1][1];
                    connections.bridges[j][2] = connections.bridges[j + 1][2];
                    connections.bridgeColors[j] = connections.bridgeColors[j + 1];
                }
                connections.numBridges--;
                connections.bridgeColors[connections.numBridges] = 0xFFFFFFFF;
                break;
            }
        }
    }
    
    numEphemeralConnections = 0;
    
    // Only invalidate and apply routing if we actually removed something
    if (removedCount > 0) {
        connections.invalidateCache(false);  // Don't auto-refresh, we'll handle it
        
        if (applyRouting) {
            refreshLocalConnections(ledShowOption, 0, 0);
            waitCore2();
        }
    }
}

bool JumperlessState::isEphemeralConnection(int node1, int node2) const {
    for (int i = 0; i < numEphemeralConnections; i++) {
        if ((ephemeralConnections[i].node1 == node1 && ephemeralConnections[i].node2 == node2) ||
            (ephemeralConnections[i].node1 == node2 && ephemeralConnections[i].node2 == node1)) {
            return true;
        }
    }
    return false;
}

int JumperlessState::getEphemeralConnectionCount() const {
    return numEphemeralConnections;
}

// Power management
void JumperlessState::setDacVoltage(int dacNum, float voltage) {
    float prev = (dacNum == 0) ? power.dac0 : power.dac1;
    if (dacNum == 0) {
        power.dac0 = voltage;
    } else if (dacNum == 1) {
        power.dac1 = voltage;
    }
    markDirty();

    if (!g_undoApplying && prev != voltage) {
        undoRecordDacSet(dacNum, prev, voltage);
    }
}

float JumperlessState::getDacVoltage(int dacNum) const {
    return (dacNum == 0) ? power.dac0 : power.dac1;
}

void JumperlessState::setRailVoltage(bool isTopRail, float voltage) {
    float prev = isTopRail ? power.topRail : power.bottomRail;
    if (isTopRail) {
        power.topRail = voltage;
    } else {
        power.bottomRail = voltage;
    }
    markDirty();

    // Rails reuse the DAC undo op with extended channel encoding:
    //   2 = top rail, 3 = bottom rail. The apply/revert path in Undo.cpp
    //   dispatches channels >= 2 back to setRailVoltage.
    if (!g_undoApplying && prev != voltage) {
        undoRecordDacSet(isTopRail ? 2 : 3, prev, voltage);
    }
}

float JumperlessState::getRailVoltage(bool isTopRail) const {
    return isTopRail ? power.topRail : power.bottomRail;
}

// Configuration
void JumperlessState::setPathStacking(int paths, int rails, int dacs) {
    config.stackPaths = paths;
    config.stackRails = rails;
    config.stackDacs = dacs;
    markDirty();
}

void JumperlessState::getPathStacking(int& paths, int& rails, int& dacs) const {
    paths = config.stackPaths;
    rails = config.stackRails;
    dacs = config.stackDacs;
}

// GPIO
void JumperlessState::setGpioDirection(int gpio, int direction) {
    if (gpio >= 0 && gpio < 10) {
        config.gpioDirection[gpio] = direction;
        markDirty();
    }
}

int JumperlessState::getGpioDirection(int gpio) const {
    if (gpio >= 0 && gpio < 10) {
        return config.gpioDirection[gpio];
    }
    return 1;  // default to input
}

void JumperlessState::setGpioPull(int gpio, int pull) {
    if (gpio >= 0 && gpio < 10) {
        config.gpioPulls[gpio] = pull;
        markDirty();
    }
}

int JumperlessState::getGpioPull(int gpio) const {
    if (gpio >= 0 && gpio < 10) {
        return config.gpioPulls[gpio];
    }
    return 0;  // default to pull-down
}

void JumperlessState::setGpioPwmFrequency(int gpio, float frequency) {
    if (gpio >= 0 && gpio < 10) {
        config.gpioPwmFrequency[gpio] = frequency;
        markDirty();
    }
}

float JumperlessState::getGpioPwmFrequency(int gpio) const {
    if (gpio >= 0 && gpio < 10) {
        return config.gpioPwmFrequency[gpio];
    }
    return 1.0f;  // default frequency
}

void JumperlessState::setGpioPwmDutyCycle(int gpio, float dutyCycle) {
    if (gpio >= 0 && gpio < 10) {
        config.gpioPwmDutyCycle[gpio] = dutyCycle;
        markDirty();
    }
}

float JumperlessState::getGpioPwmDutyCycle(int gpio) const {
    if (gpio >= 0 && gpio < 10) {
        return config.gpioPwmDutyCycle[gpio];
    }
    return 0.5f;  // default 50% duty cycle
}

void JumperlessState::setGpioPwmEnabled(int gpio, bool enabled) {
    if (gpio >= 0 && gpio < 10) {
        config.gpioPwmEnabled[gpio] = enabled;
        markDirty();
    }
}

bool JumperlessState::getGpioPwmEnabled(int gpio) const {
    if (gpio >= 0 && gpio < 10) {
        return config.gpioPwmEnabled[gpio];
    }
    return false;  // default disabled
}

// UART
void JumperlessState::setUartTxFunction(int function) {
    config.uartTxFunction = function;
    markDirty();
}

int JumperlessState::getUartTxFunction() const {
    return config.uartTxFunction;
}

void JumperlessState::setUartRxFunction(int function) {
    config.uartRxFunction = function;
    markDirty();
}

int JumperlessState::getUartRxFunction() const {
    return config.uartRxFunction;
}

// Display
void JumperlessState::setNetColor(int netNum, rgbColor color, uint32_t raw, const char* name) {
    display.setNetColor(netNum, color, raw, name);
    markDirty();
}

bool JumperlessState::getNetColor(int netNum, rgbColor& color, uint32_t& raw, char* name) const {
    return display.getNetColor(netNum, color, raw, name);
}

// Validation
bool JumperlessState::validate(String& errorMsg) const {
    // Validate power settings
    if (!power.validate(errorMsg)) {
        return false;
    }
    
    // Validate connections
    for (int i = 0; i < connections.numBridges; i++) {
        int n1 = connections.bridges[i][0];
        int n2 = connections.bridges[i][1];
        
        if (!isNodeValid(n1)) {
            errorMsg = "Invalid node in bridge " + String(i) + ": " + String(n1);
            return false;
        }
        if (!isNodeValid(n2)) {
            errorMsg = "Invalid node in bridge " + String(i) + ": " + String(n2);
            return false;
        }
    }
    
    return true;
}

// Validation helpers
bool JumperlessState::isNodeValid(int node) const {
    // Use the existing validation from FileParsing
    extern int isNodeValid(int node);
    return ::isNodeValid(node) == 1;
}

void JumperlessState::adjustEphemeralIndicesAfterBridgeRemoval(int removedBridgeIdx) {
    for (int i = 0; i < numEphemeralConnections; i++) {
        if (ephemeralConnections[i].bridgeIndex > removedBridgeIdx) {
            ephemeralConnections[i].bridgeIndex--;
        }
    }
}

bool JumperlessState::isConnectionAllowed(int node1, int node2, String& errorMsg) const {
    // Check for same node
    if (node1 == node2) {
        errorMsg = "Cannot connect node to itself: " + String(node1);
        return false;
    }

    // (ROUTABLE_BUFFER_IN was briefly hard-reserved here; connections to it
    // are ALLOWED now - the probe-power infra function YIELDS to a user
    // bridge on its node instead of the system blocking the user. See the
    // userOverridden hook in routing/InfraPaths.cpp.)

    // Use existing validation logic
    extern bool connectionAllowed(int node1, int node2);
    if (!::connectionAllowed(node1, node2)) {
        errorMsg = "Connection not allowed between " + String(node1) + " and " + String(node2) + 
                   " (likely power/ground conflict)";
        return false;
    }
    
    return true;
}

// State comparison
bool JumperlessState::operator==(const JumperlessState& other) const {
    // Compare number of bridges
    if (connections.numBridges != other.connections.numBridges) {
        return false;
    }
    
    // Compare bridges
    for (int i = 0; i < connections.numBridges; i++) {
        if (connections.bridges[i][0] != other.connections.bridges[i][0] ||
            connections.bridges[i][1] != other.connections.bridges[i][1]) {
            return false;
        }
    }
    
    // Compare power
    if (power.dac0 != other.power.dac0 || power.dac1 != other.power.dac1 ||
        power.topRail != other.power.topRail || power.bottomRail != other.power.bottomRail) {
        return false;
    }
    
    // Compare config (just the important bits)
    if (config.stackPaths != other.config.stackPaths ||
        config.stackRails != other.config.stackRails ||
        config.stackDacs != other.config.stackDacs) {
        return false;
    }
    
    return true;
}

size_t JumperlessState::estimateRAMUsage() const {
    return sizeof(JumperlessState);
}

// ============================================================================
// YAML Serialization
// ============================================================================

bool JumperlessState::toYAML(String& output, int showANSI) const {
    output = "";
    
    // Pre-allocate buffer to avoid repeated reallocations during concatenation
    // Estimate: ~50 bytes per bridge + ~450 per part (header lines + pins)
    // + ~500 bytes overhead = ~3KB typical
    output.reserve(connections.numBridges * 50 + parts.numParts * 450 + 500);
    
    // Header
    output += "version: " + String(version) + "\n";
    output += "sourceOfTruth: " + String(config.sourceOfTruth == BRIDGES_PRIMARY ? "bridges" : "nets") + "\n";

    // guideProgress scalar - ONE flow-map line, the only shape the parser
    // accepts (see the parts format comment in States.h). Emitted only while
    // a guide is bound to this slot.
    if (parts.guideSource[0] != '\0') {
        output += "guideProgress: {source: \"" + String(parts.guideSource) +
                  "\", step: " + String(parts.guideStep) + "}\n";
    }

    // runSource scalar - ONE flow line, matched with the parser below.
    // Emitted only when non-empty (see States.h: an unemitted-but-parsed
    // scalar is destroyed by the idle auto-save).
    if (runSource[0] != '\0') {
        output += "runSource: \"" + String(runSource) + "\"\n";
    }
    output += "\n";

    // Bridges section
    serializeBridges(output);

    // Nets section (optional, for colors/names)
    serializeNets(output);

    // Power section
    serializePower(output);

    // Config section
    serializeConfig(output);

    // Parts section. The old "MUST stay before overlays:" note is retired:
    // deserializeOverlaysFromYAML used to strstr forward from the FIRST
    // "overlays:" substring anywhere with no section-end bound, so parts
    // emitted after it were eaten as garbage overlays. It now anchors on an
    // un-indented `overlays:` line and stops at the next un-indented header,
    // so the two sections are order-independent. Still load-bearing for
    // persistence, though: toYAML is a wholesale rewrite - a section not
    // emitted here is destroyed by the SlotManager idle auto-save.
    serializeParts(*this, output);

    // Graphic overlays section
    serializeOverlaysToYAML(output, showANSI);

    return true;
}

bool JumperlessState::fromYAML(const String& input, String& errorMsg) {
    // YAML restore is a wholesale state replacement, not a sequence of
    // individually-undoable user actions. Suppress the undo-record hooks
    // for the duration of the parse using the same flag the apply/revert
    // path uses; otherwise boot-time slot loads would resurrect the
    // previous session's bridges as fresh undo entries even though we
    // don't persist history to flash anymore.
    struct UndoSuppressGuard {
        bool prev;
        UndoSuppressGuard() : prev(g_undoApplying) { g_undoApplying = true; }
        ~UndoSuppressGuard() { g_undoApplying = prev; }
    } undoGuard;

    // Simple YAML parser for our specific format
    // Parse line by line
    int lineStart = 0;
    String currentSection = "";
    
    clear();
    
    // Clear pending FakeGPIO restorations from previous load
    pendingFakeGpioRestorations.clear();
    
    while (lineStart < (int)input.length()) {
        int lineEnd = input.indexOf('\n', lineStart);
        if (lineEnd == -1) lineEnd = input.length();
        
        String line = input.substring(lineStart, lineEnd);
        // INDENT-HARDENING (see the parts format comment in States.h):
        // capture whether the raw line was indented BEFORE trimming. Top-
        // level section headers are only recognized on un-indented lines -
        // the serializer never indents them - so a nested key like `config:`
        // inside a contained section can't hijack the section state. The one
        // exception is `fakeGpio:`, which the serializer nests under config.
        bool indented = (line.length() > 0 &&
                         (line.charAt(0) == ' ' || line.charAt(0) == '\t'));
        line.trim();

        // Skip empty lines and comments
        if (line.length() == 0 || line.startsWith("#")) {
            lineStart = lineEnd + 1;
            continue;
        }

        // Check for section headers
        if (!indented && line.startsWith("version:")) {
            int colonIdx = line.indexOf(':');
            String val = line.substring(colonIdx + 1);
            val.trim();
            version = val.toInt();
        }
        else if (!indented && line.startsWith("sourceOfTruth:")) {
            int colonIdx = line.indexOf(':');
            String val = line.substring(colonIdx + 1);
            val.trim();
            config.sourceOfTruth = (val == "nets") ? NETS_PRIMARY : BRIDGES_PRIMARY;
        }
        else if (!indented && line.startsWith("guideProgress:")) {
            // ONE shape only (matched with toYAML - see States.h):
            //   guideProgress: {source: "/projects/555/wiring.yaml", step: 3}
            int q1 = line.indexOf('"');
            int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
            if (q1 >= 0 && q2 > q1) {
                String src = line.substring(q1 + 1, q2);
                strncpy(parts.guideSource, src.c_str(), sizeof(parts.guideSource) - 1);
                parts.guideSource[sizeof(parts.guideSource) - 1] = '\0';
            }
            int stepIdx = line.indexOf("step:", (q2 > 0) ? q2 : 0);
            if (stepIdx >= 0) {
                int endIdx = line.indexOf('}', stepIdx);
                if (endIdx == -1) endIdx = line.length();
                String val = line.substring(stepIdx + 5, endIdx);
                val.trim();
                parts.guideStep = (int16_t)val.toInt();
            }
        }
        else if (!indented && line.startsWith("runSource:")) {
            // ONE shape only (matched with toYAML above):
            //   runSource: "/projects/555/wiring.yaml"
            int q1 = line.indexOf('"');
            int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
            if (q1 >= 0 && q2 > q1) {
                String src = line.substring(q1 + 1, q2);
                strncpy(runSource, src.c_str(), sizeof(runSource) - 1);
                runSource[sizeof(runSource) - 1] = '\0';
            }
        }
        else if (!indented && line.startsWith("bridges:")) {
            currentSection = "bridges";
        }
        else if (!indented && line.startsWith("nets:")) {
            currentSection = "nets";
        }
        else if (!indented && line.startsWith("power:")) {
            currentSection = "power";
        }
        else if (!indented && line.startsWith("config:")) {
            currentSection = "config";
        }
        else if ((!indented || currentSection == "config") && line.startsWith("fakeGpio:")) {
            // The serializer nests fakeGpio under config ("  fakeGpio:") -
            // recognize it there too, or every existing fakeGpio entry would
            // be silently dropped (and destroyed by the next auto-save).
            currentSection = "fakeGpio";
        }
        else if (!indented && line.startsWith("overlays:")) {
            currentSection = "overlays";
        }
        else if (!indented && (line.startsWith("meta:") ||
                               line.startsWith("parts:") ||
                               line.startsWith("guide:"))) {
            // Contained sections. parts: is parsed whole in a post-loop pass
            // (deserializeParts, next to the overlays pass). meta:/guide: are
            // swallowed entirely and NOT round-tripped - the launcher and the
            // guide runtime re-read the project file themselves.
            currentSection = "contained";
        }
        else if (!indented && line.endsWith(":") && line.indexOf(' ') == -1) {
            // Unknown top-level section header - contain it so its indented
            // content can't corrupt the section before it (future sections
            // parse as ignored on older firmware).
            currentSection = "ignored";
        }
        // Parse section content
        else if (line.startsWith("- {") || line.startsWith("-{")) {
            // Try to parse line even if incomplete (missing closing brace)
            // The deserializer will extract whatever fields it can find
            
            // Bridge or net entry
            if (currentSection == "bridges") {
                String tempError;
                if (!deserializeBridges(line.c_str(), tempError)) {
                    // Skip invalid lines in preview mode - don't fail entire parse
                    // This allows partially edited files to still work
                    lineStart = lineEnd + 1;
                    continue;
                }
            } else if (currentSection == "nets") {
                String tempError;
                if (!deserializeNets(line.c_str(), tempError)) {
                    // Skip invalid lines in preview mode
                    lineStart = lineEnd + 1;
                    continue;
                }
            } else if (currentSection == "fakeGpio") {
                String tempError;
                if (!deserializeFakeGpio(line.c_str(), tempError)) {
                    // Skip invalid fakeGpio lines - don't fail entire parse
                    lineStart = lineEnd + 1;
                    continue;
                }
            } else if (currentSection == "overlays") {
                // Overlays are parsed in a single pass from the entire YAML content
                // after the main parsing loop - skip individual lines here
            }
        }
        else if (currentSection == "power") {
            String tempError;
            if (!deserializePower(line.c_str(), tempError)) {
                // Skip invalid power lines - don't fail entire parse
                lineStart = lineEnd + 1;
                continue;
            }
        }
        else if (currentSection == "config") {
            String tempError;
            if (!deserializeConfig(line.c_str(), tempError)) {
                // Skip invalid config lines - don't fail entire parse
                lineStart = lineEnd + 1;
                continue;
            }
        }
    
        lineStart = lineEnd + 1;
    }
    
    // Parse the parts: section whole in a single pass (multi-line list items
    // need their own scanner - same pattern as the overlays pass below)
    {
        String partsError;
        deserializeParts(*this, input.c_str(), partsError);
        if (partsError.length() > 0 && jumperlessConfig.debug.show_node_errors) {
            Serial.print("parts: parse warnings: ");
            Serial.println(partsError);
        }
    }

    // Parse graphic overlays from the entire YAML content
    String overlayError;
    deserializeOverlaysFromYAML(input.c_str(), overlayError);
    // Ignore overlay parsing errors - overlays are optional
    
    // Reconcile bridges and nets based on source of truth
    if (config.sourceOfTruth == NETS_PRIMARY) {
        connections.syncBridgesFromNets();
    } else {
        connections.syncNetsFromBridges();
    }

    // Drop any bridge whose node doesn't physically exist on the ACTIVE board.
    // This is what lets a slot saved on a V5 (which may reference BUF_IN / node
    // 139, the routable analog buffer) load on the OG, where that node has no
    // hardware. Without it, validate() below would reject the whole slot and the
    // load would fail. isNodeValid() consults board::currentBoard(), so this is a
    // no-op on a board where every saved node is real (e.g. a V5 slot on a V5).
    {
        extern int isNodeValid(int node);
        int writeIdx = 0;
        int dropped = 0;
        for (int readIdx = 0; readIdx < connections.numBridges; readIdx++) {
            if (isNodeValid(connections.bridges[readIdx][0]) != 1 ||
                isNodeValid(connections.bridges[readIdx][1]) != 1) {
                dropped++;
                continue;
            }
            if (writeIdx != readIdx) {
                connections.bridges[writeIdx][0] = connections.bridges[readIdx][0];
                connections.bridges[writeIdx][1] = connections.bridges[readIdx][1];
                connections.bridges[writeIdx][2] = connections.bridges[readIdx][2];
                connections.bridgeColors[writeIdx] = connections.bridgeColors[readIdx];
            }
            writeIdx++;
        }
        if (dropped > 0) {
            connections.numBridges = writeIdx;
            // The nets were just derived from the now-stale bridge list; rebuild
            // them from the sanitized bridges so routing/colors stay consistent.
            connections.syncNetsFromBridges();
            markDirty(); // rewrite the cleaned slot on next save
            if (jumperlessConfig.debug.show_node_errors) {
                Serial.print("Dropped ");
                Serial.print(dropped);
                Serial.println(" bridge(s) referencing nodes not present on this board");
            }
        }
    }

    // System-owned bridges don't belong to a loaded slot: drop stale probe
    // power claims (anything touching ROUTABLE_BUFFER_IN, persisted by older
    // firmware) and lock-function pairs (re-added fresh by infraEvaluate at
    // the next rebuild). Only for the ACTIVE state - a previewed slot goes
    // through validate() too, and scrubbing is idempotent/read-only-safe
    // there (it only drops rows and restores stale pin config).
    if (this == &globalState) {
        infraScrubLoadedBridges();
    }

    bool isValid = validate(errorMsg);
    if (isValid) {
        clearDirty();
    }
    return isValid;
}

// YAML serialization helpers
void JumperlessState::serializeBridges(String& output) const {
    // Count non-ephemeral bridges first
    int persistentBridges = 0;
    for (int i = 0; i < connections.numBridges; i++) {
        int node1 = connections.bridges[i][0];
        int node2 = connections.bridges[i][1];
        // Skip ephemeral + infra connections - they should NEVER be saved to
        // flash (infra bridges are re-created by infraEvaluate every rebuild;
        // persisting them is how phantom power claims got into slots). A
        // USER's deliberate BUFFER_IN bridge is real user data and persists.
        if (infraIsBridge(node1, node2)) {
            continue;
        }
        if (!isEphemeralConnection(node1, node2)) {
            persistentBridges++;
        }
    }
    
    if (persistentBridges == 0) {
        return;  // Don't output empty bridges section
    }
    
    output += "bridges:\n";
    for (int i = 0; i < connections.numBridges; i++) {
        int node1 = connections.bridges[i][0];
        int node2 = connections.bridges[i][1];

        // Skip ephemeral + infra connections - see the count loop above.
        if (infraIsBridge(node1, node2)) {
            continue;
        }
        if (isEphemeralConnection(node1, node2)) {
            continue;
        }
        
        // Use node names instead of raw numbers for readability
        String n1Name = nodeValueToString(node1);
        String n2Name = nodeValueToString(node2);
        
        output += "  - {n1: " + n1Name + 
                  ", n2: " + n2Name + 
                  ", dup: " + String(connections.bridges[i][2]);
        
        // Add color field if this bridge has a color (0xFFFFFFFF = no color)
        if (connections.bridgeColors[i] != 0xFFFFFFFF) {
            // Use rgbToWokwiColorName to preserve original Wokwi color names
            String colorName = rgbToWokwiColorName(connections.bridgeColors[i]);
            output += ", color: " + colorName;
        }
        
        output += "}\n";
    }
    output += "\n";
}

bool JumperlessState::deserializeBridges(const char* yamlContent, String& errorMsg) {
    // Parse bridge entry: - {n1: TOP_RAIL, n2: NANO_D5, dup: 2, color: red}
    // Also accepts: - {n1: 101, n2: 75, dup: 2}
    // Tolerant of incomplete lines for live editing
    String line = String(yamlContent);
    line.trim();
    
    // Validate line has opening brace at minimum
    if (line.indexOf('{') == -1) {
        errorMsg = "Missing opening brace in bridge entry";
        return false;
    }
    
    // Extract values (tolerant of incomplete/missing fields)
    int n1 = -1, n2 = -1, dup = -1;
    uint32_t color = 0xFFFFFFFF;  // Default to "no color"
    
    // Parse n1 field
    int n1Idx = line.indexOf("n1:");
    if (n1Idx >= 0) {
        // Find delimiter: comma, closing brace, or end of line
        int commaIdx = line.indexOf(',', n1Idx);
        int braceIdx = line.indexOf('}', n1Idx);
        int endIdx = line.length();
        
        // Use the first valid delimiter found
        if (commaIdx > n1Idx + 3 && (braceIdx == -1 || commaIdx < braceIdx)) {
            endIdx = commaIdx;
        } else if (braceIdx > n1Idx + 3) {
            endIdx = braceIdx;
        }
        
        if (endIdx > n1Idx + 3) {
            String val = line.substring(n1Idx + 3, endIdx);
            val.trim();
            
            // Only parse if we have actual content
            if (val.length() > 0) {
                n1 = parseNodeName(val);
            }
        }
    }
    
    // Parse n2 field
    int n2Idx = line.indexOf("n2:");
    if (n2Idx >= 0) {
        // Find delimiter: comma, closing brace, or end of line
        int commaIdx = line.indexOf(',', n2Idx);
        int braceIdx = line.indexOf('}', n2Idx);
        int endIdx = line.length();
        
        // Use the first valid delimiter found
        if (commaIdx > n2Idx + 3 && (braceIdx == -1 || commaIdx < braceIdx)) {
            endIdx = commaIdx;
        } else if (braceIdx > n2Idx + 3) {
            endIdx = braceIdx;
        }
        
        if (endIdx > n2Idx + 3) {
            String val = line.substring(n2Idx + 3, endIdx);
            val.trim();
            
            // Only parse if we have actual content
            if (val.length() > 0) {
                n2 = parseNodeName(val);
            }
        }
    }
    
    // Parse dup field (optional)
    int dupIdx = line.indexOf("dup:");
    if (dupIdx >= 0) {
        int commaIdx = line.indexOf(',', dupIdx);
        int braceIdx = line.indexOf('}', dupIdx);
        int endIdx = line.length();
        
        // Use the first valid delimiter
        if (commaIdx > dupIdx + 4 && (braceIdx == -1 || commaIdx < braceIdx)) {
            endIdx = commaIdx;
        } else if (braceIdx > dupIdx + 4) {
            endIdx = braceIdx;
        }
        
        if (endIdx > dupIdx + 4) {
            String val = line.substring(dupIdx + 4, endIdx);
            val.trim();
            if (val.length() > 0) {
                dup = val.toInt();
            }
        }
    }
    
    // Parse color field (optional)
    int colorIdx = line.indexOf("color:");
    if (colorIdx >= 0) {
        int braceIdx = line.indexOf('}', colorIdx);
        int endIdx = (braceIdx > colorIdx + 6) ? braceIdx : line.length();
        
        if (endIdx > colorIdx + 6) {
            String val = line.substring(colorIdx + 6, endIdx);
            val.trim();
            if (val.length() > 0) {
                bool parseSuccess;
                color = parseColorValue(val, parseSuccess);
                if (!parseSuccess) {
                    color = 0;  // Ignore invalid colors
                }
            }
        }
    }
    
    // Only add connection if we have both nodes
    if (n1 < 0 || n2 < 0) {
        errorMsg = "Incomplete bridge (missing n1 or n2): " + line;
        return false;
    }
    
    // Add the connection
    bool success = addConnection(n1, n2, errorMsg, dup);
    
    // Store the color if provided and connection was added
    // 0xFFFFFFFF means no color was specified
    if (success && color != 0xFFFFFFFF) {
        // The bridge was just added, so it's at index (numBridges - 1)
        int bridgeIdx = connections.numBridges - 1;
        if (bridgeIdx >= 0 && bridgeIdx < MAX_BRIDGES) {
            connections.bridgeColors[bridgeIdx] = color;
        }
    }
    
    return success;
}

void JumperlessState::serializeNets(String& output) const {
    // Serialize ALL computed nets with full metadata
    // This allows users to edit colors and see what nets exist
    
    // ARCHITECTURE NOTE: We use globalState.connections.nets[] which is where
    // all net computation is now done. The old global net[] array is deprecated.
    // The nets are computed by getNodesToConnect() which populates globalState.
    // This approach is optimal because:
    //   1. Nets are computed once on load, not repeatedly on save
    //   2. We reuse the existing optimized net computation code
    //   3. Serialization is fast (just reading already-computed data)
    
    // Use the state's nets (access globalState since this is a const method)
    JumperlessState& state = const_cast<JumperlessState&>(globalState);
    
    // Count the number of active nets (since numNets might not be set by NetManager)
    // Scan through nets array to find the highest numbered net
    int maxNetNumber = 0;
    for (int i = 1; i < MAX_NETS; i++) {
        if (state.connections.nets[i].number > 0 && state.connections.nets[i].nodes[0] != 0) {
            maxNetNumber = i;
        }
    }
    
    // Update numNets for the state
    state.connections.numNets = maxNetNumber + 1;
    
    // If no nets were found, nothing to serialize
    if (maxNetNumber == 0) {
        return;  // Don't output anything for empty nets section
    }
    
    output += "nets:\n";
    
    // Animation order for detecting animated nets
    extern int animationOrder[26];
    
    for (int i = 1; i <= maxNetNumber; i++) {  // Start at 1, skip net 0
        if (state.connections.nets[i].number == 0 || state.connections.nets[i].nodes[0] == 0) {
            continue;  // Skip empty nets
        }
        
        // Count nodes in this net
        int nodeCount = 0;
        for (int j = 0; j < MAX_NODES && state.connections.nets[i].nodes[j] != 0; j++) {
            nodeCount++;
        }

        // Skip nets with only one node
        if (nodeCount <= 1) {
            continue;
        }

        // Skip system-owned nets, mirroring serializeBridges' infra skip:
        // any net that is exactly one active infra pair. Persisting them was
        // how stale power-claim entries got into slot files. (A net where a
        // USER bridged BUFFER_IN is user data and persists - the function
        // yields to them at evaluation time.)
        if (nodeCount == 2 &&
            infraIsBridge(state.connections.nets[i].nodes[0],
                          state.connections.nets[i].nodes[1])) {
            continue;
        }

        // Collect node list for this net (using SHORT names)
        String nodesList = "[";
        bool firstNode = true;
        for (int j = 0; j < MAX_NODES && state.connections.nets[i].nodes[j] != 0; j++) {
            if (!firstNode) nodesList += ", ";
            // Use short name (0 = short, 1 = long)
            const char* shortName = definesToChar(state.connections.nets[i].nodes[j], 0);
            if (shortName && strlen(shortName) > 0) {
                nodesList += String(shortName);
            } else {
                nodesList += String(state.connections.nets[i].nodes[j]);
            }
            firstNode = false;
        }
        nodesList += "]";
        
        // Get color information
        String colorName = "";
        bool userAssigned = false;
        bool animated = false;
        uint32_t netColor = 0;
        
        // Check if this net has a custom color
        for (int j = 0; j < display.numCustomColors; j++) {
            if (display.customColors[j].netNumber == state.connections.nets[i].number) {
                colorName = String(display.customColors[j].colorName);
                colorName.trim();
                netColor = display.customColors[j].rawColor;
                userAssigned = true;  // Custom colors are user-assigned
                break;
            }
        }
        
        // If no custom color, get the auto-generated color
        if (colorName.length() == 0) {
            // Use the net's assigned color
            rgbColor netRgb = state.connections.nets[i].color;
            netColor = packRgb(netRgb.r, netRgb.g, netRgb.b);
            colorName = colorValueToName(netColor);
        }
        
        // Check if net is animated (check if any of its nodes are in the animation order)
        for (int j = 0; j < 26; j++) {
            for (int k = 0; k < MAX_NODES && state.connections.nets[i].nodes[k] != 0; k++) {
                if (state.connections.nets[i].nodes[k] == animationOrder[j]) {
                    animated = true;
                    break;
                }
            }
            if (animated) break;
        }
        
        // Build the output line with required fields first
        output += "  - {num: " + String(state.connections.nets[i].number);
        output += ", nodes: " + nodesList;
        
        // Only print color if not animated
        if (!animated) {
            output += ", color: " + colorName;
        }
        
        // Optional fields at the end (only if not default)
        
        // Check DisplayState for custom name first
        String netName = "";
        bool hasCustomName = false;
        for (int j = 0; j < display.numCustomNames; j++) {
            if (display.customNames[j].netNumber == state.connections.nets[i].number) {
                netName = String(display.customNames[j].name);
                hasCustomName = true;
                break;
            }
        }
        
        // Only print name if it's a custom name from DisplayState
        if (hasCustomName && netName.length() > 0) {
            output += ", name: \"" + netName + "\"";
        }
        
        // Only print user flag if true
        if (userAssigned) {
            output += ", user: true";
        }
        
        // Only print anim flag if true
        if (animated) {
            output += ", anim: true";
        }
        
        output += "}\n";
    }
    output += "\n";
}

bool JumperlessState::deserializeNets(const char* yamlContent, String& errorMsg) {
    // Parse net entry: - {num: 6, nodes: [TOP_RAIL, NANO_D5], color: red, user: true, anim: false, name: "Power"}
    String line = String(yamlContent);
    line.trim();
    
    int netNum = -1;
    String netName = "";
    String colorStr = "";
    bool userAssigned = false;
    bool animated = false;
    
    // Parse num field
    int numIdx = line.indexOf("num:");
    if (numIdx >= 0) {
        int commaIdx = line.indexOf(',', numIdx);
        String val = line.substring(numIdx + 4, commaIdx);
        val.trim();
        netNum = val.toInt();
    }
    
    // Parse nodes field (we don't use this for loading, just for reference)
    // The bridges define the actual connections
    
    // Parse color field (can be name or hex)
    int colorIdx = line.indexOf("color:");
    if (colorIdx >= 0) {
        int endIdx = line.indexOf(',', colorIdx);
        if (endIdx == -1) endIdx = line.indexOf('}', colorIdx);
        String val = line.substring(colorIdx + 6, endIdx);
        val.trim();
        colorStr = val;
    }
    
    // Parse user field
    int userIdx = line.indexOf("user:");
    if (userIdx >= 0) {
        int endIdx = line.indexOf(',', userIdx);
        if (endIdx == -1) endIdx = line.indexOf('}', userIdx);
        String val = line.substring(userIdx + 5, endIdx);
        val.trim();
        bool parseSuccess;
        userAssigned = parseBoolean(val, parseSuccess);
    }
    
    // Parse anim field
    int animIdx = line.indexOf("anim:");
    if (animIdx >= 0) {
        int endIdx = line.indexOf(',', animIdx);
        if (endIdx == -1) endIdx = line.indexOf('}', animIdx);
        String val = line.substring(animIdx + 5, endIdx);
        val.trim();
        bool parseSuccess;
        animated = parseBoolean(val, parseSuccess);
    }
    
    // Parse name field (optional)
    int nameIdx = line.indexOf("name:");
    if (nameIdx >= 0) {
        int startQuote = line.indexOf('"', nameIdx);
        int endQuote = line.indexOf('"', startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
            netName = line.substring(startQuote + 1, endQuote);
        }
    }
    
    // Store the custom net name if provided
    // This allows user-defined names like "Power", "Signal", etc.
    if (netNum >= 0 && netName.length() > 0) {
        setCustomNetName(netNum, netName.c_str());
    }
    
    // Only store color if it was user-assigned
    // Auto-generated colors will be recomputed
    if (netNum >= 0 && userAssigned && colorStr.length() > 0) {
        bool parseSuccess;
        uint32_t rawColor = parseColorValue(colorStr, parseSuccess);
        
        if (parseSuccess) {
            rgbColor color;
            color.r = (rawColor >> 16) & 0xFF;
            color.g = (rawColor >> 8) & 0xFF;
            color.b = rawColor & 0xFF;
            
            // Use color name for the display system
            String colorName = colorValueToName(rawColor);
            display.setNetColor(netNum, color, rawColor, colorName.c_str());
        }
    }
    
    return true;
}

void JumperlessState::serializePower(String& output) const {
    output += "power:\n";
    output += "  topRail: " + String(power.topRail, 2) + "\n";
    output += "  bottomRail: " + String(power.bottomRail, 2) + "\n";
    output += "  dac0: " + String(power.dac0, 2) + "\n";
    output += "  dac1: " + String(power.dac1, 2) + "\n\n";
}

bool JumperlessState::deserializePower(const char* yamlContent, String& errorMsg) {
    String line = String(yamlContent);
    line.trim();
    
    if (line.startsWith("topRail:")) {
        int colonIdx = line.indexOf(':');
        String val = line.substring(colonIdx + 1);
        val.trim();
        power.topRail = val.toFloat();
    }
    else if (line.startsWith("bottomRail:")) {
        int colonIdx = line.indexOf(':');
        String val = line.substring(colonIdx + 1);
        val.trim();
        power.bottomRail = val.toFloat();
    }
    else if (line.startsWith("dac0:")) {
        int colonIdx = line.indexOf(':');
        String val = line.substring(colonIdx + 1);
        val.trim();
        power.dac0 = val.toFloat();
    }
    else if (line.startsWith("dac1:")) {
        int colonIdx = line.indexOf(':');
        String val = line.substring(colonIdx + 1);
        val.trim();
        power.dac1 = val.toFloat();
    }
    
    return power.validate(errorMsg);
}

void JumperlessState::serializeConfig(String& output) const {
    output += "config:\n";
    output += "  routing: {stackPaths: " + String(config.stackPaths) + 
              ", stackRails: " + String(config.stackRails) + 
              ", stackDacs: " + String(config.stackDacs) + 
              ", railPriority: " + String(config.railPriority) + "}\n";
    
    // GPIO direction array
    output += "  gpio:\n";
    output += "    direction:    [";
    for (int i = 0; i < 10; i++) {
        output += String(config.gpioDirection[i]);
        if (i < 9) output += ",";
    }
    output += "]\n";
    
    // GPIO pulls array
    output += "    pulls:        [";
    for (int i = 0; i < 10; i++) {
        output += String(config.gpioPulls[i]);
        if (i < 9) output += ",";
    }
    output += "]\n";
    
    // PWM frequency array
    output += "    pwmFrequency: [";
    for (int i = 0; i < 10; i++) {
        output += String(config.gpioPwmFrequency[i], 2);
        if (i < 9) output += ",";
    }
    output += "]\n";
    
    // PWM duty cycle array
    output += "    pwmDutyCycle: [";
    for (int i = 0; i < 10; i++) {
        output += String(config.gpioPwmDutyCycle[i], 2);
        if (i < 9) output += ",";
    }
    output += "]\n";
    
    // PWM enabled array
    output += "    pwmEnabled:   [";
    for (int i = 0; i < 10; i++) {
        output += String(config.gpioPwmEnabled[i] ? "1" : "0");
        if (i < 9) output += ",";
    }
    output += "]\n";

    // Floating-read enabled array (1 = detect tri-state, 0 = skip)
    output += "    readFloating: [";
    for (int i = 0; i < 10; i++) {
        output += String(config.gpioReadFloating[i]);
        if (i < 9) output += ",";
    }
    output += "]\n";
    
    // UART and OLED
    output += "  uart: {txFunction: " + String(config.uartTxFunction) + 
              ", rxFunction: " + String(config.uartRxFunction) + "}\n";
    output += "  oled: {connected: " + String(config.oledConnected ? "true" : "false") + 
              ", lockConnection: " + String(config.oledLockConnection ? "true" : "false") + "}\n";
    
    // Fake GPIO configurations
    serializeFakeGpio(output);
}

bool JumperlessState::deserializeConfig(const char* yamlContent, String& errorMsg) {
    String line = String(yamlContent);
    line.trim();
    
    // Parse routing line
    if (line.startsWith("routing:")) {
        int stackPathsIdx = line.indexOf("stackPaths:");
        if (stackPathsIdx >= 0) {
            int commaIdx = line.indexOf(',', stackPathsIdx);
            String val = line.substring(stackPathsIdx + 11, commaIdx);
            val.trim();
            config.stackPaths = val.toInt();
        }
        
        int stackRailsIdx = line.indexOf("stackRails:");
        if (stackRailsIdx >= 0) {
            int commaIdx = line.indexOf(',', stackRailsIdx);
            String val = line.substring(stackRailsIdx + 11, commaIdx);
            val.trim();
            config.stackRails = val.toInt();
        }
        
        int stackDacsIdx = line.indexOf("stackDacs:");
        if (stackDacsIdx >= 0) {
            int commaIdx = line.indexOf(',', stackDacsIdx);
            String val = line.substring(stackDacsIdx + 10, commaIdx);
            val.trim();
            config.stackDacs = val.toInt();
        }
        
        int railPriorityIdx = line.indexOf("railPriority:");
        if (railPriorityIdx >= 0) {
            int endIdx = line.indexOf('}', railPriorityIdx);
            String val = line.substring(railPriorityIdx + 13, endIdx);
            val.trim();
            config.railPriority = val.toInt();
        }
    }
    // Parse GPIO direction array
    else if (line.startsWith("direction:")) {
        int startIdx = line.indexOf('[');
        int endIdx = line.indexOf(']');
        if (startIdx >= 0 && endIdx > startIdx) {
            String arrayStr = line.substring(startIdx + 1, endIdx);
            int idx = 0;
            int pos = 0;
            while (pos < (int)arrayStr.length() && idx < 10) {
                int commaIdx = arrayStr.indexOf(',', pos);
                if (commaIdx == -1) commaIdx = arrayStr.length();
                String val = arrayStr.substring(pos, commaIdx);
                val.trim();
                config.gpioDirection[idx++] = val.toInt();
                pos = commaIdx + 1;
            }
        }
    }
    // Parse GPIO pulls array
    else if (line.startsWith("pulls:")) {
        int startIdx = line.indexOf('[');
        int endIdx = line.indexOf(']');
        if (startIdx >= 0 && endIdx > startIdx) {
            String arrayStr = line.substring(startIdx + 1, endIdx);
            int idx = 0;
            int pos = 0;
            while (pos < (int)arrayStr.length() && idx < 10) {
                int commaIdx = arrayStr.indexOf(',', pos);
                if (commaIdx == -1) commaIdx = arrayStr.length();
                String val = arrayStr.substring(pos, commaIdx);
                val.trim();
                config.gpioPulls[idx++] = val.toInt();
                pos = commaIdx + 1;
            }
        }
    }
    // Parse PWM frequency array
    else if (line.startsWith("pwmFrequency:")) {
        int startIdx = line.indexOf('[');
        int endIdx = line.indexOf(']');
        if (startIdx >= 0 && endIdx > startIdx) {
            String arrayStr = line.substring(startIdx + 1, endIdx);
            int idx = 0;
            int pos = 0;
            while (pos < (int)arrayStr.length() && idx < 10) {
                int commaIdx = arrayStr.indexOf(',', pos);
                if (commaIdx == -1) commaIdx = arrayStr.length();
                String val = arrayStr.substring(pos, commaIdx);
                val.trim();
                config.gpioPwmFrequency[idx++] = val.toFloat();
                pos = commaIdx + 1;
            }
        }
    }
    // Parse PWM duty cycle array
    else if (line.startsWith("pwmDutyCycle:")) {
        int startIdx = line.indexOf('[');
        int endIdx = line.indexOf(']');
        if (startIdx >= 0 && endIdx > startIdx) {
            String arrayStr = line.substring(startIdx + 1, endIdx);
            int idx = 0;
            int pos = 0;
            while (pos < (int)arrayStr.length() && idx < 10) {
                int commaIdx = arrayStr.indexOf(',', pos);
                if (commaIdx == -1) commaIdx = arrayStr.length();
                String val = arrayStr.substring(pos, commaIdx);
                val.trim();
                config.gpioPwmDutyCycle[idx++] = val.toFloat();
                pos = commaIdx + 1;
            }
        }
    }
    // Parse PWM enabled array
    else if (line.startsWith("pwmEnabled:")) {
        int startIdx = line.indexOf('[');
        int endIdx = line.indexOf(']');
        if (startIdx >= 0 && endIdx > startIdx) {
            String arrayStr = line.substring(startIdx + 1, endIdx);
            int idx = 0;
            int pos = 0;
            while (pos < (int)arrayStr.length() && idx < 10) {
                int commaIdx = arrayStr.indexOf(',', pos);
                if (commaIdx == -1) commaIdx = arrayStr.length();
                String val = arrayStr.substring(pos, commaIdx);
                val.trim();
                bool parseSuccess;
                config.gpioPwmEnabled[idx++] = parseBoolean(val, parseSuccess);
                pos = commaIdx + 1;
            }
        }
    }
    // Parse floating-read enabled array
    else if (line.startsWith("readFloating:")) {
        int startIdx = line.indexOf('[');
        int endIdx = line.indexOf(']');
        if (startIdx >= 0 && endIdx > startIdx) {
            String arrayStr = line.substring(startIdx + 1, endIdx);
            int idx = 0;
            int pos = 0;
            while (pos < (int)arrayStr.length() && idx < 10) {
                int commaIdx = arrayStr.indexOf(',', pos);
                if (commaIdx == -1) commaIdx = arrayStr.length();
                String val = arrayStr.substring(pos, commaIdx);
                val.trim();
                config.gpioReadFloating[idx++] = (uint8_t)val.toInt();
                pos = commaIdx + 1;
            }
            // Sync to the global array in Peripherals.cpp so it takes effect immediately
            extern uint8_t gpioReadFloating[10];
            for (int i = 0; i < 10; i++) {
                gpioReadFloating[i] = config.gpioReadFloating[i];
            }
        }
    }
    // Parse UART
    else if (line.startsWith("uart:")) {
        int txIdx = line.indexOf("txFunction:");
        if (txIdx >= 0) {
            int commaIdx = line.indexOf(',', txIdx);
            String val = line.substring(txIdx + 11, commaIdx);
            val.trim();
            config.uartTxFunction = val.toInt();
        }
        
        int rxIdx = line.indexOf("rxFunction:");
        if (rxIdx >= 0) {
            int endIdx = line.indexOf('}', rxIdx);
            String val = line.substring(rxIdx + 11, endIdx);
            val.trim();
            config.uartRxFunction = val.toInt();
        }
    }
    // Parse OLED
    else if (line.startsWith("oled:")) {
        int connectedIdx = line.indexOf("connected:");
        if (connectedIdx >= 0) {
            int commaIdx = line.indexOf(',', connectedIdx);
            String val = line.substring(connectedIdx + 10, commaIdx);
            val.trim();
            bool parseSuccess;
            config.oledConnected = parseBoolean(val, parseSuccess);
        }
        
        int lockIdx = line.indexOf("lockConnection:");
        if (lockIdx >= 0) {
            int endIdx = line.indexOf('}', lockIdx);
            String val = line.substring(lockIdx + 15, endIdx);
            val.trim();
            bool parseSuccess;
            config.oledLockConnection = parseBoolean(val, parseSuccess);
        }
    }
    
    return true;
}

void JumperlessState::serializeFakeGpio(String& output) const {
    // Count active fake GPIO pins (outputs + inputs)
    int activeCount = 0;
    for (int i = 0; i < MAX_FAKE_GP_OUT; i++) {
        if (fakeGpioOutputs[i].active) activeCount++;
    }
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        if (fakeGpioInputs[i].active) activeCount++;
    }
    
    if (activeCount == 0) {
        return;  // Don't output empty section
    }
    
    output += "  fakeGpio:\n";
    
    // Serialize outputs
    for (int slot = 0; slot < MAX_FAKE_GP_OUT; slot++) {
        if (!fakeGpioOutputs[slot].active) continue;
        const FakeGpioOutput& out = fakeGpioOutputs[slot];
        
        output += "    - {slot: " + String(slot);
        output += ", node: " + String(out.userNode);
        output += ", mode: 1";
        output += ", high_node: " + String(out.highVoltageNode);
        output += ", low_node: " + String(out.lowVoltageNode);
        output += ", th_high: " + String(out.thresholdHigh, 2);
        output += ", th_low: " + String(out.thresholdLow, 2);
        output += "}\n";
    }
    
    // Serialize inputs
    for (int slot = 0; slot < MAX_FAKE_GP_IN; slot++) {
        if (!fakeGpioInputs[slot].active) continue;
        const FakeGpioInput& in = fakeGpioInputs[slot];
        
        output += "    - {slot: " + String(MAX_FAKE_GP_OUT + slot);
        output += ", node: " + String(in.userNode);
        output += ", mode: 0";
        output += ", th_high: " + String(in.thresholdHigh, 2);
        output += ", th_low: " + String(in.thresholdLow, 2);
        output += "}\n";
    }
}

bool JumperlessState::deserializeFakeGpio(const char* yamlContent, String& errorMsg) {
    // Parse fake GPIO entry: - {slot: 0, node: 20, mode: 1, v_high: 8.0, v_low: -8.0, high_node: 101, low_node: 102, th_high: 2.0, th_low: 0.8}
    String line = String(yamlContent);
    line.trim();
    
    int slot = -1;
    int node = -1;
    int mode = -1;
    float v_high = 0.0;
    float v_low = 0.0;
    float th_high = 2.0;
    float th_low = 0.8;
    int high_voltage_node = -1;
    int low_voltage_node = -1;
    
    // Parse slot field
    int slotIdx = line.indexOf("slot:");
    if (slotIdx >= 0) {
        int commaIdx = line.indexOf(',', slotIdx);
        String val = line.substring(slotIdx + 5, commaIdx);
        val.trim();
        slot = val.toInt();
    }
    
    // Parse node field
    int nodeIdx = line.indexOf("node:");
    if (nodeIdx >= 0) {
        int commaIdx = line.indexOf(',', nodeIdx);
        String val = line.substring(nodeIdx + 5, commaIdx);
        val.trim();
        node = val.toInt();
    }
    
    // Parse mode field
    int modeIdx = line.indexOf("mode:");
    if (modeIdx >= 0) {
        int commaIdx = line.indexOf(',', modeIdx);
        if (commaIdx == -1) commaIdx = line.indexOf('}', modeIdx);
        String val = line.substring(modeIdx + 5, commaIdx);
        val.trim();
        mode = val.toInt();
    }
    
    // Parse v_high field (optional, for OUTPUT mode)
    int vHighIdx = line.indexOf("v_high:");
    if (vHighIdx >= 0) {
        int commaIdx = line.indexOf(',', vHighIdx);
        if (commaIdx == -1) commaIdx = line.indexOf('}', vHighIdx);
        String val = line.substring(vHighIdx + 7, commaIdx);
        val.trim();
        v_high = val.toFloat();
    }
    
    // Parse v_low field (optional, for OUTPUT mode)
    int vLowIdx = line.indexOf("v_low:");
    if (vLowIdx >= 0) {
        int commaIdx = line.indexOf(',', vLowIdx);
        if (commaIdx == -1) commaIdx = line.indexOf('}', vLowIdx);
        String val = line.substring(vLowIdx + 6, commaIdx);
        val.trim();
        v_low = val.toFloat();
    }
    
    // Parse high_node field (optional, for OUTPUT mode - voltage source node ID)
    int highNodeIdx = line.indexOf("high_node:");
    if (highNodeIdx >= 0) {
        int commaIdx = line.indexOf(',', highNodeIdx);
        if (commaIdx == -1) commaIdx = line.indexOf('}', highNodeIdx);
        String val = line.substring(highNodeIdx + 10, commaIdx);
        val.trim();
        high_voltage_node = val.toInt();
    }
    
    // Parse low_node field (optional, for OUTPUT mode - voltage source node ID)
    int lowNodeIdx = line.indexOf("low_node:");
    if (lowNodeIdx >= 0) {
        int commaIdx = line.indexOf(',', lowNodeIdx);
        if (commaIdx == -1) commaIdx = line.indexOf('}', lowNodeIdx);
        String val = line.substring(lowNodeIdx + 9, commaIdx);
        val.trim();
        low_voltage_node = val.toInt();
    }
    
    // Parse th_high field
    int thHighIdx = line.indexOf("th_high:");
    if (thHighIdx >= 0) {
        int commaIdx = line.indexOf(',', thHighIdx);
        if (commaIdx == -1) commaIdx = line.indexOf('}', thHighIdx);
        String val = line.substring(thHighIdx + 8, commaIdx);
        val.trim();
        th_high = val.toFloat();
    }
    
    // Parse th_low field
    int thLowIdx = line.indexOf("th_low:");
    if (thLowIdx >= 0) {
        int endIdx = line.indexOf('}', thLowIdx);
        String val = line.substring(thLowIdx + 7, endIdx);
        val.trim();
        th_low = val.toFloat();
    }
    
    // Store in temporary structure for later restoration
    // We can't directly configure here because bridges might not be loaded yet
    if (slot >= 0 && slot < MAX_FAKE_GPIO && node >= 0 && mode >= 0) {
        // Store in config for later restoration
        FakeGpioRestorationInfo info;
        info.slot = slot;
        info.node = node;
        info.mode = mode;
        info.v_high = v_high;
        info.v_low = v_low;
        info.threshold_high = th_high;
        info.threshold_low = th_low;
        info.high_voltage_node = high_voltage_node;
        info.low_voltage_node = low_voltage_node;
        
        // Add to restoration list
        pendingFakeGpioRestorations.push_back(info);
    }
    
    return true;
}

// ============================================================================
// YAML Parsing Helpers - Handle Aliases and Variations
// ============================================================================

/**
 * @brief Parse node name with alias support
 * Accepts variations like: TOP_RAIL, topRail, t_rAiL, 101
 * Returns the defined integer value or -1 if not found
 */
int parseNodeName(const String& nodeName) {
    String normalized = nodeName;
    normalized.trim();
    normalized.toUpperCase();
    normalized.replace(" ", "_");
    
    // Try to parse as integer first
    if (normalized.length() > 0 && (isdigit(normalized.charAt(0)) || normalized.charAt(0) == '-')) {
        int val = normalized.toInt();
        if (val >= 0 && val <= 200) {  // Valid node range
            return val;
        }
    }
    
    // Search through nano defines
    extern const DefineInfo nanoDefines[];
    // Count elements in nanoDefines - it has 35 elements based on the defNanoToCharShort array
    const int numNanoDefines = 35;
    for (int i = 0; i < numNanoDefines; i++) {
        String longName = String(nanoDefines[i].longName);
        String shortName = String(nanoDefines[i].shortName);
        longName.toUpperCase();
        shortName.toUpperCase();
        
        if (normalized == longName || normalized == shortName) {
            return nanoDefines[i].defineValue;
        }
    }
    
    // Search through special defines
    extern const DefineInfo specialDefines[];
    // Count elements in specialDefines - updated to 70 elements (added 32 FAKE_GPIO, removed 7 PAD defines, was 49)
    const int numSpecialDefines = 70;
    for (int i = 0; i < numSpecialDefines; i++) {
        String longName = String(specialDefines[i].longName);
        String shortName = String(specialDefines[i].shortName);
        longName.toUpperCase();
        shortName.toUpperCase();
        
        if (normalized == longName || normalized == shortName) {
            return specialDefines[i].defineValue;
        }
    }
    
    // Handle common variations not in the arrays
    if (normalized == "TOPRAIL" || normalized == "T_R" || normalized == "TOP_R") return TOP_RAIL;
    if (normalized == "BOTTOMRAIL" || normalized == "BOTRAIL" || normalized == "B_R" || normalized == "BOT_R") return BOTTOM_RAIL;
    if (normalized == "GROUND") return GND;
    if (normalized == "3V3" || normalized == "3.3V") return SUPPLY_3V3;
    if (normalized == "5V" || normalized == "+5V") return SUPPLY_5V;
    
    return -1;  // Not found
}

/**
 * @brief Convert node value back to canonical string name
 * Returns the long form name (e.g., TOP_RAIL)
 */
String nodeValueToString(int nodeValue) {
    // Use the existing definesToChar function to get the long name
    const char* name = definesToChar(nodeValue, 1);  // 1 = long name
    if (name != nullptr && strlen(name) > 0) {
        String result = String(name);
        result.trim();
        return result;
    }
    
    // Fallback to number if name not found
    return String(nodeValue);
}

/**
 * @brief Parse color value from name or hex
 * Accepts: "red", "0xFF0000", "16711680"
 * Returns uint32_t color value
 */
uint32_t parseColorValue(const String& colorStr, bool& success) {
    
    String normalized = colorStr;
    normalized.trim();
    normalized.toLowerCase();
    
    success = true;

   

    
    // Try hex format first
    if (normalized.startsWith("0x") || normalized.startsWith("#")) {
        uint32_t color = scaleBrightness(strtoul(normalized.c_str() + 2, NULL, 16), -80   );
        if (debugFP) {
            Serial.printf("parsed 0x color value: %14s 0x%06X\n", normalized.c_str(), color);
        }
        return color;
    
    }
    
    // Try decimal number
    if (normalized.length() > 0 && isdigit(normalized.charAt(0))) {
        uint32_t color = scaleBrightness(normalized.toInt(), -50);
        if (debugFP) {  
            Serial.printf("parsed decimal color value: %14s 0x%06X\n", normalized.c_str(), color);
        }
        return color;
        
    }
    
    // FIRST: Try Wokwi colors (with proper brightness scaling for wire colors)
    // This ensures consistency between Wokwi JSON import and YAML reload
//     uint32_t wokwiColor = wokwiColorToRGB(normalized);
//    // if (wokwiColor != 0x0a0a0a) {  // 0x0a0a0a is the "not found" default from wokwiColorToRGB
//         return wokwiColor;
//     //}
    
    // SECOND: Try internal named colors for user-assigned colors
    int numColors = sizeof(namedColors) / sizeof(namedColors[0]);
    for (int i = 0; i < numColors; i++) {
        String colorName = String(namedColors[i].name);
        colorName.trim();
        colorName.toLowerCase();
        
        if (normalized == colorName) {
            
            uint32_t color = scaleBrightness(namedColors[i].dimColor, -50);  // Use dimColor for LED display
            if (debugFP) {
                Serial.printf("parsed named color value: %14s 0x%06X\n", colorName.c_str(), color);
            }
            return color;
            
        }
    }
    
    // Not found
    success = false;
    if (debugFP) {
        Serial.println("parsed color value: " + normalized);
    }
    return 0x000000;
}

/**
 * @brief Get color name from value
 * Returns the canonical name from namedColors[]
 */
String colorValueToName(uint32_t color) {
    // Use existing colorToName function
    char* name = colorToName(color, -1);
    if (name != nullptr) {
        String result = String(name);
        result.trim();
        return result;
    }
    return "0x" + String(color, HEX);
}

/**
 * @brief Parse boolean with aliases
 * Accepts: true/false, 1/0, on/off, yes/no, enabled/disabled
 */
bool parseBoolean(const String& val, bool& success) {
    String normalized = val;
    normalized.trim();
    normalized.toLowerCase();
    
    success = true;
    
    // True values
    if (normalized == "true" || normalized == "1" || normalized == "on" || 
        normalized == "yes" || normalized == "enabled") {
        return true;
    }
    
    // False values
    if (normalized == "false" || normalized == "0" || normalized == "off" || 
        normalized == "no" || normalized == "disabled") {
        return false;
    }
    
    // Invalid
    success = false;
    return false;
}

/**
 * @brief Convert boolean to canonical string
 */
String booleanToString(bool value) {
    return value ? "true" : "false";
}

// ============================================================================
// Legacy Format Support
// ============================================================================

bool JumperlessState::fromLegacyNodeFile(const String& nodeFileContent, String& errorMsg) {
    clear();
    
    // Legacy format is just: { node1-node2, node3-node4, ... }
    // We need to parse this and extract connections
    
    String content = nodeFileContent;
    content.trim();
    
    // Remove curly braces
    int openBrace = content.indexOf('{');
    int closeBrace = content.lastIndexOf('}');
    
    if (openBrace == -1 || closeBrace == -1) {
        errorMsg = "Invalid legacy format: missing curly braces";
        return false;
    }
    
    content = content.substring(openBrace + 1, closeBrace);
    content.trim();
    
    if (content.length() == 0) {
        // Empty slot is valid
        return true;
    }
    
    // Parse connections
    int startIdx = 0;
    while (startIdx < (int)content.length()) {
        int commaIdx = content.indexOf(',', startIdx);
        if (commaIdx == -1) {
            commaIdx = content.length();
        }
        
        String bridge = content.substring(startIdx, commaIdx);
        bridge.trim();
        
        if (bridge.length() > 0) {
            int dashIdx = bridge.indexOf('-');
            if (dashIdx == -1) {
                errorMsg = "Invalid bridge format in legacy file: " + bridge;
                return false;
            }
            
            int node1 = bridge.substring(0, dashIdx).toInt();
            int node2 = bridge.substring(dashIdx + 1).toInt();
            
            if (!addConnection(node1, node2, errorMsg)) {
                errorMsg = "Error adding connection from legacy file: " + errorMsg;
                return false;
            }
        }
        
        startIdx = commaIdx + 1;
    }
    
    return true;
}

// ============================================================================
// Helper function to apply loaded state to hardware
// ============================================================================

/**
 * @brief Apply the current globalState to all hardware peripherals
 * 
 * This function is called after loading a slot to ensure that:
 * - DAC voltages match the loaded power state
 * - GPIO directions and pull resistors match the loaded config
 * - All hardware reflects the state loaded from the file
 */
void applyStateToHardware() {
    // Apply power settings (DACs and rails)
    // Note: Pass save=0 to avoid updating globalState (it's already loaded)
    //       and saveEEPROM=0 to avoid writing to EEPROM
    setRailsAndDACs(0);  // This applies topRail, bottomRail, dac0, dac1 from globalState



#if !defined(OG_JUMPERLESS)
    // Apply GPIO configurations from globalState to hardware.
    // SKIPPED on OG: the gpioDef bank (pins 20-27) is the V5 routable-GPIO map.
    // On the OG those pins are the CH446Q chip selects for chips I/J/K/L
    // (20-23), RESET (24), the WS2812 LED data line (25) and ADC inputs (26-27).
    // applyStateToHardware() runs at boot (slot load) and on every slot load, so
    // driving this bank with gpio_set_dir()/gpio_set_pulls() here re-muxes the
    // SF chip-select pins into GPIO inputs -- after which setCSex() can no longer
    // assert them and every breadboard<->SF (nano/DAC/ADC) connection silently
    // fails to program while A-H (CS 6-13) keep working. Matches the OG guards in
    // initGPIO()/setGPIO()/updateGPIOConfigFromState().
    for (int i = 0; i < 10; i++) {
        uint8_t gpio_pin = gpioDef[i][0];
        
        // Apply direction to hardware
        if (globalState.config.gpioDirection[i] == 0) {
            gpio_set_dir(gpio_pin, true);  // output
        } else {
            gpio_set_dir(gpio_pin, false);  // input
        }
        
        // Apply pull resistors to hardware and update gpioState for animations
        switch (globalState.config.gpioPulls[i]) {
            case 0: // pulldown
                gpio_set_pulls(gpio_pin, false, true);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 4;  // input with pulldown
                }
                break;
            case 1: // pullup
                gpio_set_pulls(gpio_pin, true, false);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 3;  // input with pullup
                }
                break;
            case 2: // no pull
                gpio_set_pulls(gpio_pin, false, false);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 2;  // input with no pull
                }
                break;
            case 3: // bus keeper
                gpio_set_pulls(gpio_pin, true, true);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 7;  // bus keeper mode
                }
                break;
            default:
                gpio_set_pulls(gpio_pin, false, false);
                if (globalState.config.gpioDirection[i] == 1) {
                    gpioState[i] = 2;  // input with no pull
                }
                break;
        }
        
        // Set initial output state for output pins
        if (globalState.config.gpioDirection[i] == 0) {
            gpio_put(gpio_pin, gpioState[i]);
        }
    }
#endif
    
    if (debugFP) {
        Serial.println("✓ Applied state to hardware (power, GPIO)");
    }
}

// ============================================================================
// SlotManager Implementation
// ============================================================================

SlotManager::SlotManager()
    : activeState(globalState), activeSlotNumber(0), historySize(STATE_HISTORY_SIZE),
      historyHead(0), historyCount(0), historyPosition(0),
      previewModeActive(false), previewSlotNumber(-1), originalSlotNumber(-1),
      temporarySlotActive(false), temporarySlotOriginal(-1),
      restoringContext(false) {
    // Always initialize to slot 0, sync with netSlot on first use.
    // This is the PRE-BOOT default only - seedBootContext() (main.cpp, called
    // once configLoaded is true) decides the real boot context.
    netSlot = 0;  // Ensure global is also 0
    strncpy(activeSlotPath, "/slots/slot0.yaml", sizeof(activeSlotPath) - 1);
    activeSlotPath[sizeof(activeSlotPath) - 1] = '\0';
    previewOriginalPath[0] = '\0';
    temporarySlotOriginalPath[0] = '\0';
    initHistory();
}

SlotManager& SlotManager::getInstance() {
    static SlotManager instance;
    return instance;
}

void SlotManager::initHistory() {
    historyBuffer = new JumperlessState[historySize];
}

void SlotManager::cleanupHistory() {
    if (historyBuffer) {
        delete[] historyBuffer;
        historyBuffer = nullptr;
    }
}

JumperlessState& SlotManager::getActiveState() {
    return activeState;
}

const JumperlessState& SlotManager::getActiveState() const {
    return activeState;
}

String SlotManager::getSlotFilename(int slotNum) const {
    // Special case for Python slot (slot 99)
    if (slotNum == 99) {
        return "/slots/slotPython.yaml";
    }
    return "/slots/slot" + String(slotNum) + ".yaml";
}

String SlotManager::getLegacySlotFilename(int slotNum) const {
    return "/nodeFileSlot" + String(slotNum) + ".txt";
}

String SlotManager::getJSONSlotFilename(int slotNum) const {
    return "/slots/slot" + String(slotNum) + ".json";
}

// THE canonical-slot-file matcher. This single strict function replaces the
// two loose extractors that used to live here (extractSlotNumberFromFilename
// and extractSlotNumberFromPath, design-slots.md §3).
//
// What the loose ones did wrong: they matched any BASENAME starting "slot" and
// ending ".yaml" and toInt()'d whatever sat between, so "slot_555.yaml" became
// "_555".toInt() == 0 -> slot 0 adopted -> the idle auto-save wrote a project
// file's content over the user's /slots/slot0.yaml. That trap is what this
// wave retires.
//
// Requirements, all three necessary:
//   - FULL-path match, not basename: /projects/foo/slot3.yaml is a file
//     context, not slot 3.
//   - Every character of the number is a digit.
//   - Round-trip checked: String(numStr.toInt()) == numStr, which kills
//     "slot007", "slot_555", "slot3b", "slot+3", "slot ".
int SlotManager::slotNumberForCanonicalPath(const String& path) {
    if (path == "/slots/slotPython.yaml") return 99;
    if (!path.startsWith("/slots/slot") || !path.endsWith(".yaml")) return -1;

    // Exactly the span between the "/slots/slot" prefix and the ".yaml" suffix.
    const int numStart = 11;                       // strlen("/slots/slot")
    const int numEnd = (int)path.length() - 5;     // strlen(".yaml")
    if (numEnd <= numStart) return -1;

    String numStr = path.substring(numStart, numEnd);
    for (unsigned int i = 0; i < numStr.length(); i++) {
        if (!isDigit(numStr.charAt(i))) return -1;
    }
    long slotNum = numStr.toInt();
    if (String(slotNum) != numStr) return -1;      // no leading zeros / overflow
    if (slotNum < 0 || slotNum >= NUM_SLOTS) return -1;  // 99 handled above
    return (int)slotNum;
}

/**
 * Is this path a shipped PROJECT TEMPLATE?
 *
 * True for /projects/<dir>/wiring*.yaml - the same family listVariantFiles
 * matches (basename startsWith "wiring", endsWith ".yaml"), so it covers
 * wiring.yaml, wiring_v2.yaml and the wiring_original*.yaml backups too.
 *
 * Per-run project files are named <dir>_<N>.yaml and deliberately do NOT
 * match, so nothing about the run flow is affected.
 */
bool SlotManager::isTemplatePath(const char* path) {
    if (!path || path[0] == '\0') return false;
    String p(path);
    if (!p.startsWith("/projects/")) return false;
    int lastSlash = p.lastIndexOf('/');
    if (lastSlash < 0) return false;
    String base = p.substring(lastSlash + 1);
    return base.startsWith("wiring") && base.endsWith(".yaml");
}

void SlotManager::setActivePathFromSlot(int slotNum) {
    String fn = getSlotFilename(slotNum);
    strncpy(activeSlotPath, fn.c_str(), sizeof(activeSlotPath) - 1);
    activeSlotPath[sizeof(activeSlotPath) - 1] = '\0';
}

// Where "last active" lives. Deliberately NOT config.txt: config saves are
// full-file rewrites behind a diff gate, and writing config on every slot
// switch would churn flash and the diff cache. One tiny file, one line, the
// active path.
const char* LAST_ACTIVE_PATH = "/slots/last_active.txt";

// What /slots/last_active.txt is known to contain. Dedups the write, which
// runs on every load and save. Seeded by seedBootContext() from the value it
// reads at boot, so the first updateLastActive() after boot does not rewrite
// the file with the value it just read. Semantics are "what the FILE holds",
// not "what the context is", so seeding with the read value stays correct on
// the fallback paths too (those differ, so they still rewrite).
static char g_lastActiveWritten[128] = {0};

/**
 * Decide what context the board boots into. Called once from main.cpp's
 * firstLoop == 1 block, AFTER configLoaded and BEFORE the first `loadfile:`
 * pass - it only sets up netSlot / activeSlotPath; loadfile: does the actual
 * load, so there is exactly one load path.
 *
 * netSlot = 0 (RotaryEncoder.cpp) stays the pre-config default, which is also
 * the fresh-board and missing-file answer - i.e. a board that has never
 * switched slots boots exactly the way it always did.
 */
void seedBootContext(void) {
    SlotManager& mgr = SlotManager::getInstance();

    // boot_mode 0: pin a fixed slot. The escape hatch for the old "always
    // load slot 0" behavior.
    if (jumperlessConfig.slots.boot_mode == 0) {
        int s = jumperlessConfig.slots.boot_slot;
        if (s < 0 || s >= NUM_SLOTS) s = 0;
        netSlot = s;
        mgr.setActiveSlot(s);
        return;
    }

    // boot_mode 1 (default): boot into the last-active slot FILE.
    String path;
    if (safeFileExists(LAST_ACTIVE_PATH)) {
        File f = safeFileOpen(LAST_ACTIVE_PATH, "r", 2000);
        if (f) {
            path = f.readString();
            safeFileClose(f, false);
            path.trim();
            // Seed the dedup cache with what the file actually holds, so the
            // first updateLastActive() of the session does not rewrite it with
            // the value just read. The fallback paths below deliberately DO
            // differ from it, so they still rewrite as intended.
            strncpy(g_lastActiveWritten, path.c_str(), sizeof(g_lastActiveWritten) - 1);
            g_lastActiveWritten[sizeof(g_lastActiveWritten) - 1] = '\0';
        }
    }

    if (path.length() == 0) {
        netSlot = 0;              // no memory yet - old behavior exactly
        mgr.setActiveSlot(0);
        return;
    }

    int n = SlotManager::slotNumberForCanonicalPath(path);
    if (n >= 0) {
        netSlot = n;
        mgr.setActiveSlot(n);
        return;
    }

    // A file context. Only seed it if the file is actually still there;
    // otherwise fall back to slot 0 and rewrite last_active.txt so a deleted
    // run file doesn't cost a failed load on every boot from here on. (A file
    // that EXISTS but fails to load is handled in loadfile:, which is where
    // the load happens.)
    if (safeFileExists(path.c_str())) {
        netSlot = SLOT_FILE_CONTEXT;
        mgr.adoptBootPath(path);
    } else {
        Serial.println("Last-active slot file is gone (" + path + ") - booting slot 0");
        netSlot = 0;
        mgr.setActiveSlot(0);     // setActiveSlot -> updateLastActive rewrites the file
    }
}

/**
 * Point tracking at a path WITHOUT loading it - the boot seed's one use.
 * loadfile: performs the actual load on the first pass.
 */
void SlotManager::adoptBootPath(const String& path) {
    activeSlotNumber = SLOT_FILE_CONTEXT;
    netSlot = SLOT_FILE_CONTEXT;
    strncpy(activeSlotPath, path.c_str(), sizeof(activeSlotPath) - 1);
    activeSlotPath[sizeof(activeSlotPath) - 1] = '\0';
}

void SlotManager::updateLastActive() {
    // SELF-GATING - one choke point, so no caller has to remember the rules:
    //   - never mid-preview (a wheel detent through the Slots menu must not
    //     stamp a slot the user only glanced at; power loss mid-preview would
    //     then boot into it)
    //   - never mid-temp-slot (a calibration app's scratch slot is not a
    //     context anyone wants to boot into)
    //   - never slot 99 (isolated MicroPython) or temp slot 8
    //   - never an empty path
    if (previewModeActive || temporarySlotActive) return;
    if (activeSlotNumber == 99 || activeSlotNumber == 8) return;
    if (activeSlotNumber < 0 && activeSlotNumber != SLOT_FILE_CONTEXT) return;
    if (activeSlotPath[0] == '\0') return;
    // A project TEMPLATE is read-only (see saveStateToActivePath) - a board
    // must never boot into a context it cannot save.
    if (isTemplatePath(activeSlotPath)) return;

    // Skip the write when nothing changed - this runs on every load and save.
    if (strncmp(g_lastActiveWritten, activeSlotPath, sizeof(g_lastActiveWritten)) == 0) return;

    if (!safeMkdir("/slots", 2000)) return;
    if (safeFileWriteAll(LAST_ACTIVE_PATH, activeSlotPath, strlen(activeSlotPath), 2000)) {
        strncpy(g_lastActiveWritten, activeSlotPath, sizeof(g_lastActiveWritten) - 1);
        g_lastActiveWritten[sizeof(g_lastActiveWritten) - 1] = '\0';
    }
}

String SlotManager::activeContextLabel7() const {
    // Numbered slots keep the name the whole UI already uses.
    if (activeSlotNumber == 99) return String("Python");
    if (activeSlotNumber >= 0) return String("Slot ") + String(activeSlotNumber);
    if (activeSlotPath[0] == '\0') return String("-");

    String p(activeSlotPath);
    int lastSlash = p.lastIndexOf('/');
    String base = (lastSlash >= 0) ? p.substring(lastSlash + 1) : p;
    if (base.endsWith(".yaml")) base = base.substring(0, base.length() - 5);
    if (base.length() > 7) base = base.substring(0, 7);
    return base;
}

bool SlotManager::slotExists(int slotNum) const {
    // Allow slot 99 (Python slot) in addition to 0-7
    if (slotNum < 0 || (slotNum >= NUM_SLOTS && slotNum != 99)) {
        return false;
    }
    
    String filename = getSlotFilename(slotNum);
    if (safeFileExists(filename.c_str())) {
        return true;
    }

    // Check for legacy format (not applicable to Python slot)
    if (slotNum != 99) {
        String legacyFilename = getLegacySlotFilename(slotNum);
        return safeFileExists(legacyFilename.c_str());
    }
    
    return false;
}

bool SlotManager::loadSlot(int slotNum, String& errorMsg) {
    // Allow slot 99 (Python slot) in addition to 0-7
    if (slotNum < 0 || (slotNum >= NUM_SLOTS && slotNum != 99)) {
        errorMsg = "Invalid slot number: " + String(slotNum);
        return false;
    }

    // Big-event: if we're switching to a different slot, flush any pending
    // changes for the currently-active one to flash NOW. Otherwise the
    // user's work since their last commit would be lost when we replace
    // activeState below. No-op if nothing is dirty.
    if (slotNum != activeSlotNumber) {
        fileCacheFlushNowAll("slot_switch");
    }

    String content;
    String filename = getSlotFilename(slotNum);

    // Serial.println("Loading slot " + String(slotNum) + " from " + filename);
    // Serial.flush();
    
    // Try loading YAML slot file
    if (safeFileExists(filename.c_str())) {
        // Serial.println("  File exists, reading...");
        // Serial.flush();
        
        if (!readSlotFile(slotNum, content, errorMsg)) {
            Serial.println("  Read failed: " + errorMsg);
            Serial.flush();
            return false;
        }
        
        // Serial.println("  Parsing YAML...");
        // Serial.flush();
        
        if (!activeState.fromYAML(content, errorMsg)) {
            // Canonical parse failed - this can happen after a power-loss
            // mid-save left the canonical file partially written. The ABA
            // flusher mirrors every save to /.bak/<path>, so try that
            // copy before giving up.
            String bakPath = "/.bak" + filename;
            Serial.print("  Canonical parse failed, trying mirror at ");
            Serial.println(bakPath);
            Serial.flush();
            String bakContent;
            // Slot YAMLs cap at the same size used elsewhere in this file.
            constexpr size_t MAX_SLOT_YAML = 65536;
            char* bakBuf = (char*)malloc(MAX_SLOT_YAML);
            size_t bakRead = 0;
            extern bool safeFileReadAllRaw(const char* path, char* buffer, size_t buffer_size,
                                           size_t* bytes_read, uint32_t timeout_ms);
            bool recovered = false;
            if (bakBuf && safeFileExists(bakPath.c_str()) &&
                safeFileReadAllRaw(bakPath.c_str(), bakBuf, MAX_SLOT_YAML, &bakRead, 2000) &&
                bakRead > 0) {
                bakContent = bakBuf;
                String bakErr;
                if (activeState.fromYAML(bakContent, bakErr)) {
                    Serial.println("  ✓ Recovered slot from mirror - canonical will be rewritten on next save");
                    Serial.flush();
                    // Mark dirty so the next idle flush rewrites the
                    // canonical from the mirror's content.
                    activeState.markDirty();
                    content = bakContent;
                    recovered = true;
                } else {
                    errorMsg = "Failed to parse YAML slot " + String(slotNum) + " (mirror also failed): " + bakErr;
                }
            } else {
                errorMsg = "Failed to parse YAML slot " + String(slotNum) + " (no usable mirror): " + errorMsg;
            }
            if (bakBuf) free(bakBuf);
            if (!recovered) {
                Serial.println("  Parse failed: " + errorMsg);
                Serial.flush();
                return false;
            }
        }
        
        // Serial.println("  ✓ Loaded " + String(activeState.connections.numBridges) + " connections");
        // Serial.flush();

        // Re-expand placed parts into bridges BEFORE routing (idempotent -
        // exact duplicates are skipped, so a slot whose expansion bridges
        // already live in bridges: loads clean). Net names re-assert inside
        // refreshConnections' reconcile pass (partsReassertNetNames).
        if (activeState.parts.numParts > 0) {
            String partsErr;
            expandPartsToBridges(activeState, partsErr);
            if (partsErr.length() > 0 && jumperlessConfig.debug.show_node_errors) {
                Serial.print("parts expansion warnings: ");
                Serial.println(partsErr);
            }
        }

        // Populate FakeGPIO slot structs BEFORE routing so the router can expand
        // FAKE_GP_OUT_x/FAKE_GP_IN_x virtual nodes to real voltage sources/ADCs.
        // Without this, the router sees inactive slots and can't find chip positions.
        initializeFakeGpioFromLoadedState();
        
        // Compute nets from bridges and commit to hardware
        // refreshLocalConnections does: loadBridgesFromState, getNodesToConnect, 
        // bridgesToPaths, commitPaths, sendPaths, and LED updates
        extern void refreshConnections(int ledShowOption, int fillUnused, int clean);
        refreshConnections(-1, 1, 1);  // Update connections, show LEDs, clean commit
        
        // Finalize FakeGPIO after routing (extract chipKY, register TDM, disconnect paths)
        finalizeFakeGpioAfterRouting();
        
        // In normal mode (not preview), also apply DAC/GPIO settings
        if (!previewModeActive) {
            applyStateToHardware();  // Apply power rails and GPIO configs
            if (debugFP) {
                Serial.println("  ✓ Applied state to hardware (connections, LEDs, power, GPIO)");
            }
        } else {
            if (debugFP) {
                Serial.println("  ✓ Preview mode - connections and LEDs updated (power/GPIO unchanged)");
            }
        }
        
        activeSlotNumber = slotNum;
        netSlot = slotNum;  // Sync global slot tracker
        setActivePathFromSlot(slotNum);
        updateLastActive();
        return true;
    }

    // Slot file doesn't exist - start with empty state
    // Serial.println("  File doesn't exist, using empty slot");
    // Serial.flush();

    activeState.clear();
    activeSlotNumber = slotNum;
    netSlot = slotNum;  // Sync global slot tracker
    setActivePathFromSlot(slotNum);

    // POWER RE-ASSERT (design addition #3). The file-exists branch above ends
    // in applyStateToHardware(); this one used to just swap the trackers and
    // return, so switching to an empty slot left the PREVIOUS context's rail
    // and DAC voltages energized. activeState.clear() has just reset power to
    // defaults, so push those to hardware too - "loading a context always
    // asserts that context's power" must have no exceptions, and
    // boot-last-active depends on it.
    if (!previewModeActive) {
        applyStateToHardware();
    }

    updateLastActive();

    // DON'T create the file yet - only create when something is saved
    // This prevents crashes from creating large objects on the stack

    return true;
}

/**
 * Load a slot YAML from an arbitrary path and ADOPT it as the active context.
 *
 * This is the heart of the "slots become files" rework. What used to happen:
 * an unrecognized path left activeSlotNumber pointing at the PREVIOUS slot,
 * so the next idle auto-save wrote the loaded file's content over that slot.
 * Now the context follows the file, and the auto-save writes back to the file
 * itself (saveActiveSlot -> saveStateToActivePath).
 *
 * ATOMIC ON FAILURE - the contract the launcher's exit table is written
 * against. Tracking (activeSlotNumber / netSlot / activeSlotPath) is adopted
 * ONLY on a successful open AND parse. There are TWO failure modes, not one:
 *
 *   - Open failure (missing file, open refused): nothing happened at all.
 *     The read is kept strictly BEFORE fromYAML (which clear()s the state),
 *     and that ordering IS the atomicity for this case.
 *
 *   - Parse/validate failure: fromYAML has ALREADY replaced globalState by
 *     the time it returns false, so "leave the trackers alone" is NOT enough -
 *     it would leave foreign remnants in RAM under the PRIOR context's number,
 *     and the next dirty auto-save would write them into that numbered slot
 *     with no -2 sentinel to trip the loud saveSlot guard. So the prior
 *     context is RE-LOADED from its own file here, and a false return again
 *     means "nothing happened".
 *
 *     (An earlier version of this contract claimed fromYAML never returns
 *     false. It does: fromYAML ends in `return validate(errorMsg)`, and
 *     PowerState::validate rejects any rail/DAC outside +/-8 V. Host-editing
 *     a run file over MSC, or clicking a hand-written YAML in the Files
 *     browser, reaches it.)
 *
 *   - EXCEPTION, double failure: if that restoring re-load ALSO fails, the
 *     manager enters the NO-ACTIVE-CONTEXT state: activeSlotNumber ==
 *     netSlot == -1, empty path, cleared state, and the board cleared to
 *     match (nothing routed, nothing powered beyond defaults). Nothing can be
 *     auto-saved from there (saveActiveSlot returns "No active slot to
 *     save"), which is the point - it is the only terminal state that cannot
 *     write foreign content into anyone's file.
 *
 *     This is NOT limited to "the prior file vanished meanwhile". For a FILE
 *     context it is DETERMINISTIC whenever the caller passed the active path
 *     itself: USBfs's MSC-eject reload calls
 *     loadSlotFromPath(getActiveSlotPath()), so savedPath == path and the
 *     restore re-reads the same bad file. A host writing an invalid YAML over
 *     the active run file therefore lands in the terminal state every time.
 *     Numbered contexts are rescued by loadSlot's /.bak mirror; arbitrary
 *     paths have no mirror. A reboot recovers via the boot slot-0 fallback.
 */
bool SlotManager::loadSlotFromPath(const String& path, String& errorMsg) {
    // Captured BEFORE anything is touched, so the parse-failure path can put
    // the prior context back. Same (number, path) pair pattern the preview,
    // temp-slot, printSlotInfo and Wokwi dances use.
    const int savedSlot = activeSlotNumber;
    char savedPath[128];
    strncpy(savedPath, activeSlotPath, sizeof(savedPath) - 1);
    savedPath[sizeof(savedPath) - 1] = '\0';

    if (!safeFileExists(path.c_str())) {
        errorMsg = "File not found: " + path;
        return false;   // tracking untouched - prior context still active
    }
    File file = safeFileOpen(path.c_str(), "r");
    if (!file) {
        errorMsg = "Failed to open: " + path;
        return false;   // tracking untouched - prior context still active
    }
    String content = file.readString();
    safeFileClose(file, false);

    // Big-event flush of the OUTGOING context before its state is replaced -
    // the loadSlot precedent (:2921). Switching away must not drop the
    // previous context's pending writes. Only after the read succeeded, so a
    // failed open costs nothing.
    if (path != String(activeSlotPath)) {
        fileCacheFlushNowAll("slot_switch");
    }

    if (!activeState.fromYAML(content, errorMsg)) {
        errorMsg = "Failed to parse YAML: " + errorMsg;
        Serial.println("Load failed for " + path + ": " + errorMsg);

        // globalState is ALREADY clobbered (fromYAML clear()s before parsing),
        // so restoring the prior context means genuinely re-loading it. Reuse
        // the public loaders rather than duplicating their tails (parts
        // expansion, fakeGpio, refresh, power re-assert, updateLastActive) -
        // `restoringContext` bounds the recursion to depth 1, so a restore
        // whose own file is also bad cannot re-enter this logic.
        bool restored = false;
        if (!restoringContext) {
            restoringContext = true;
            String restoreErr;
            if (savedSlot >= 0) {
                restored = loadSlot(savedSlot, restoreErr);
            } else if (savedSlot == SLOT_FILE_CONTEXT && savedPath[0] != '\0') {
                restored = loadSlotFromPath(String(savedPath), restoreErr);
            }
            restoringContext = false;
        }

        if (!restored) {
            // Double failure (or there was no prior context to restore).
            // Terminal state: NO ACTIVE CONTEXT.
            //
            // WHEN THIS HAPPENS. For a numbered prior context it is genuinely
            // rare - loadSlot has a /.bak mirror rescue, and a missing slot
            // file is a legitimate empty slot that restores fine. For a FILE
            // prior context it is DETERMINISTIC in one important case: the
            // USBfs eject reload calls loadSlotFromPath(getActiveSlotPath()),
            // so savedPath == path and the restore re-reads the SAME bad file,
            // guaranteeing the second failure. A host writing an invalid YAML
            // over the active run file over MSC - an explicitly endorsed flow
            // in this design - lands here every time. Arbitrary paths have no
            // mirror to fall back to. Nothing is written and a reboot recovers
            // through the boot slot-0 fallback, so this is a safe stop, not
            // data loss - but it is a normal outcome, not an exotic one.
            //
            // netSlot is set to -1, NOT 0. deleteSlot's historical -1/0 split
            // is exactly what must not be copied here: with netSlot == 0 and
            // activeSlotNumber == -1 the next service() pass would
            // syncFromGlobalNetSlot() to 0 and write this cleared/foreign
            // state straight over /slots/slot0.yaml. Keeping the pair EQUAL at
            // -1 means the sync is a no-op and saveActiveSlot refuses. -1 is
            // also an already-live netSlot value here (attractMode's defcon
            // position; main.cpp's loadfile: fails it through to
            // clearActiveSlot), so this lands on handled ground.
            //
            // updateLastActive() self-gates on -1, so last_active.txt still
            // names the saved context; if that file really is gone, boot's
            // existing fallback rewrites it.
            activeState.clear();
            activeSlotNumber = -1;
            netSlot = -1;
            activeSlotPath[0] = '\0';

            // THE INVARIANT: terminal state means NOTHING ROUTED and NOTHING
            // POWERED BEYOND DEFAULTS. Clearing RAM alone would leave the
            // crossbar still holding the vanished context's bridges and the
            // rails still energized at its voltages while the state reads
            // empty - the same RAM/hardware divergence that made the original
            // parse-failure bug dangerous. The common parse-failure path gets
            // this for free from the restoring loader; the terminal path has
            // to do it itself. (clear() has just reset power to defaults:
            // rails 0 V, DAC1 0 V, DAC0 at the probe feed voltage.)
            extern void refreshConnections(int ledShowOption, int fillUnused, int clean);
            refreshConnections(-1, 1, 1);
            if (!previewModeActive) {
                applyStateToHardware();
            }

            Serial.println("Prior context could not be restored - no active "
                           "context; board cleared and nothing will be "
                           "auto-saved until a slot or file is loaded.");
            Serial.flush();
        }
        return false;
    }
    // Re-expand placed parts into bridges before routing (idempotent - see
    // the identical hook in loadSlot).
    if (activeState.parts.numParts > 0) {
        String partsErr;
        expandPartsToBridges(activeState, partsErr);
        if (partsErr.length() > 0 && jumperlessConfig.debug.show_node_errors) {
            Serial.print("parts expansion warnings: ");
            Serial.println(partsErr);
        }
    }
    initializeFakeGpioFromLoadedState();
    extern void refreshConnections(int ledShowOption, int fillUnused, int clean);
    refreshConnections(-1, 1, 1);
    finalizeFakeGpioAfterRouting();
    if (!previewModeActive) {
        applyStateToHardware();   // re-assert this context's power: rails + DACs
    }

    // ---- ADOPT --------------------------------------------------------
    // The old "leave tracking alone unless the name looks like a slot file"
    // guard is GONE. A canonical /slots/slotN.yaml keeps its number; anything
    // else becomes SLOT_FILE_CONTEXT with the path as its only identity.
    int slotNum = slotNumberForCanonicalPath(path);
    activeSlotNumber = (slotNum >= 0) ? slotNum : SLOT_FILE_CONTEXT;
    netSlot = activeSlotNumber;                    // the load-bearing invariant
    strncpy(activeSlotPath, path.c_str(), sizeof(activeSlotPath) - 1);
    activeSlotPath[sizeof(activeSlotPath) - 1] = '\0';

    // Tell the app a file context is now active. Numbered slots announce
    // themselves through cmd_cycleSlots' SLOT_CHANGED; entering a file
    // context has no other announcement point.
    // The WIRE value for "a file context" is -1, not -2: -1 already means
    // "no numbered slot" to every existing integer parser, and -2 is an
    // internal sentinel that never leaves the firmware.
    // Suppressed while restoring: a failed load of X must not announce a
    // spurious context change for the prior context it just put back.
    if (activeSlotNumber == SLOT_FILE_CONTEXT && !restoringContext) {
        Jerial.println("SLOT_CHANGED:-1");
        Jerial.print("ACTIVE_PATH:");
        Jerial.println(activeSlotPath);
        Jerial.flush();
    }

    updateLastActive();
    return true;
}

// Static cache for /slots directory existence (avoid repeated FS checks)
static bool slotsDirectoryExists = false;

bool SlotManager::saveSlot(int slotNum, String& errorMsg, bool skipValidation) {
    // LOUD FAILURE GUARD - the safety net the whole path-context design leans
    // on. Because netSlot == activeSlotNumber holds in both worlds, ANY call
    // site that still forwards the raw slot number while a file context is
    // active arrives here as -2. Turning that into a visible error (instead of
    // letting it fall through to the generic "Invalid slot number") is what
    // converts a missed conversion from a silent slot clobber into a bug
    // report. If you see this, the fix is saveActiveSlot(), never a cast.
    if (slotNum == SLOT_FILE_CONTEXT) {
        errorMsg = "BUG: saveSlot(-2) - use saveActiveSlot";
        Serial.println("BUG: saveSlot(-2) - use saveActiveSlot");
        Serial.flush();
        return false;
    }

    // Allow slot 99 (Python slot) in addition to 0-7
    if (slotNum < 0 || (slotNum >= NUM_SLOTS && slotNum != 99)) {
        errorMsg = "Invalid slot number: " + String(slotNum);
        return false;
    }

    // Cache /slots directory existence check (avoid FS call on every save)
    if (!slotsDirectoryExists) {
        if (!safeMkdir("/slots")) {
            errorMsg = "Failed to create /slots directory";
            return false;
        }
        slotsDirectoryExists = true;
    }
    
    // Optionally skip validation for auto-saves (state is validated on connection add/remove)
    if (!skipValidation) {
        if (!activeState.validate(errorMsg)) {
            errorMsg = "Cannot save invalid state: " + errorMsg;
            Serial.println("  Validation failed: " + errorMsg);
            Serial.flush();
            return false;
        }
    }
    
    // Convert to YAML
    String yamlContent;
    if (!activeState.toYAML(yamlContent)) {
        errorMsg = "Failed to serialize state to YAML";
        Serial.println("  Serialization failed");
        Serial.flush();
        return false;
    }
    
    // Write to file (this will create the file if it doesn't exist)
    if (!writeSlotFile(slotNum, yamlContent, errorMsg)) {
        Serial.println("  Write failed: " + errorMsg);
        Serial.flush();
        return false;
    }
    
    // saveSlot MOVES the active context to slotNum (that has always been its
    // behavior). The path has to move with it or the two identities disagree -
    // an explicit `W 3` or a Menus save-to-slot issued while a run file was
    // active would otherwise leave activeSlotPath pointing at the run file
    // while the number says 3, and the next auto-save would write slot 3's
    // content into the run file.
    activeSlotNumber = slotNum;
    netSlot = slotNum;  // Sync global slot tracker
    setActivePathFromSlot(slotNum);
    updateLastActive();
    activeState.clearDirty();  // Mark as saved

    // Persist undo history alongside the slot. This is an instant PSRAM
    // memcpy into the write-back cache (coalesced to flash in the background),
    // and a no-op when the history hasn't changed since the last save, so it's
    // cheap to do on every save - including autosaves.
    undoPersistHistory();
    return true;
}

/**
 * Write the active state to an arbitrary path. Generalized writeSlotFile:
 * same validate -> toYAML -> safeFileWriteAll sequence, no /slots assumption
 * and no slot-tracking side effects.
 */
bool SlotManager::writeStateToPath(const char* path, String& errorMsg, bool skipValidation) {
    if (!path || path[0] == '\0') {
        errorMsg = "writeStateToPath: empty path";
        return false;
    }

    // TEMPLATE PROTECTION (design-slots.md §3) - enforced HERE, at the single
    // door every state-write goes through, rather than only in
    // saveStateToActivePath. A shipped /projects/<dir>/wiring*.yaml is a
    // TEMPLATE, not a workspace, and must never be written with serialized
    // state by anyone.
    //
    // This is not hypothetical - it was caught on the bench. Adoption made
    // `load_project("eeprom")` point the context at the template, the state
    // went dirty, and the idle auto-save wrote globalState back over it. The
    // damage is worse than a normal overwrite because toYAML is a WHOLESALE
    // REWRITE and the `guide:` / `meta:` sections are swallowed on parse and
    // never re-emitted: three shipped templates came back with their entire
    // guide sections gone.
    //
    // No legitimate caller writes STATE to a template path - provisioning
    // installs templates by copying bytes through safeFileWriteAll, not
    // through this API - so putting the refusal at the bottom costs nothing
    // and closes the last door. The launcher's per-run files are
    // <dir>_<N>.yaml and deliberately do not match the predicate.
    if (isTemplatePath(path)) {
        errorMsg = "Refusing to write over project template " + String(path);
        Serial.print("REFUSED: not writing over project template ");
        Serial.println(path);
        Serial.flush();
        return false;
    }

    if (!skipValidation) {
        if (!activeState.validate(errorMsg)) {
            errorMsg = "Cannot save invalid state: " + errorMsg;
            return false;
        }
    }

    String yamlContent;
    if (!activeState.toYAML(yamlContent)) {
        errorMsg = "Failed to serialize state to YAML";
        return false;
    }

    if (!safeFileWriteAll(path, yamlContent.c_str(), yamlContent.length(), 2000)) {
        errorMsg = "Failed to write file: " + String(path);
        return false;
    }
    return true;
}

/** saveActiveSlot's SLOT_FILE_CONTEXT branch: write back to the active file. */
bool SlotManager::saveStateToActivePath(String& errorMsg, bool skipValidation) {
    if (activeSlotPath[0] == '\0') {
        errorMsg = "No active slot file to save";
        return false;
    }

    // The template refusal lives inside writeStateToPath (the single door),
    // so a template context simply fails to save here - loudly, and WITHOUT
    // clearing dirty, so nothing is silently lost.
    if (!writeStateToPath(activeSlotPath, errorMsg, skipValidation)) {
        return false;
    }
    activeState.clearDirty();
    // Same undo-history companion every saveSlot does. Per design §1-Undo the
    // file-context bucket's transactions are skipped inside undoPersistHistory
    // itself, so this is a cheap no-op when only file-context history exists.
    undoPersistHistory();
    return true;
}

/**
 * THE single save dispatch point for "persist whatever context is active"
 * (design-slots.md §3). Every converted call site routes through here; the
 * saveSlot(-2) guard above catches the ones that didn't.
 */
bool SlotManager::saveActiveSlot(String& errorMsg, bool skipValidation) {
    if (activeSlotNumber >= 0) {
        return saveSlot(activeSlotNumber, errorMsg, skipValidation);
    }
    if (activeSlotNumber == SLOT_FILE_CONTEXT && activeSlotPath[0] != '\0') {
        return saveStateToActivePath(errorMsg, skipValidation);
    }
    // -1 ("no active slot", e.g. right after deleteSlot) is unchanged.
    errorMsg = "No active slot to save";
    return false;
}

bool SlotManager::deleteSlot(int slotNum, String& errorMsg) {
    // Allow slot 99 (Python slot) in addition to 0-7
    if (slotNum < 0 || (slotNum >= NUM_SLOTS && slotNum != 99)) {
        errorMsg = "Invalid slot number: " + String(slotNum);
        return false;
    }
    
    String filename = getSlotFilename(slotNum);
    if (safeFileExists(filename.c_str())) {
        if (!safeFileDelete(filename.c_str())) {
            errorMsg = "Failed to delete slot file: " + filename;
            return false;
        }
    }
    
    // // Also delete legacy format if it exists
    // String legacyFilename = getLegacySlotFilename(slotNum);
    // if (FatFS.exists(legacyFilename.c_str())) {
    //     FatFS.remove(legacyFilename.c_str());
    // }
    
    // If this was the active slot, clear it
    if (activeSlotNumber == slotNum) {
        activeState.clear();
        activeSlotNumber = -1;
        netSlot = 0;  // Reset to slot 0
        activeSlotPath[0] = '\0';  // don't leave a path pointing at a dead file
    }

    return true;
}

void SlotManager::clearActiveSlot() {
    activeState.clear();
}

void SlotManager::setActiveSlot(int slotNum) {
    activeSlotNumber = slotNum;
    netSlot = slotNum;  // Keep global slot tracker in sync
    // Every caller passes an explicit 0-7 (Menus SLOTSACTION, FileParsing's
    // paste-into-slot-N, ProjectsApp) or 99 (MicroPython isolated entry), so
    // the canonical filename is always the right path here. The pairing is
    // mandatory: leaving a stale path behind is the number/path disagreement
    // that lets an auto-save write into the wrong file.
    setActivePathFromSlot(slotNum);
    updateLastActive();
}

void SlotManager::syncFromGlobalNetSlot() {
    // Call this when netSlot changes externally (e.g., rotary encoder)
    if (activeSlotNumber != netSlot) {
        activeSlotNumber = netSlot;
        
        // Optionally auto-load the new slot
        // String errorMsg;
        // loadSlot(netSlot, errorMsg);
    }
}

// ============================================================================
// Temporary Slot Mode - For apps that need a working slot and restore when done
// Unlike preview mode, this clears the temp slot and applies changes to hardware
// Optimized for speed - skips unnecessary file I/O and LED updates
// ============================================================================

bool SlotManager::enterTemporarySlot(int tempSlot, bool saveCurrentFirst) {
    // Validate temp slot number (allow slot 8 and slot 99 as special temp slots)
    if (tempSlot < 0 || tempSlot >= NUM_SLOTS) {
        // Allow slot 8 specifically as the app temporary slot
        if (tempSlot != 8 && tempSlot != 99) {
            return false;
        }
    }
    
    // Don't nest temporary slot modes
    if (temporarySlotActive) {
        return false;
    }
    
    // Only save if state has actually changed (skip expensive filesystem write)
    if (saveCurrentFirst && activeState.isDirty()) {
        String errorMsg;
        saveActiveSlot(errorMsg);
    }
    
    // Remember where we came from - number AND path. temporarySlotOriginal
    // may be SLOT_FILE_CONTEXT now (an app launched from a project run file),
    // in which case the path is the only way back.
    temporarySlotOriginal = activeSlotNumber;
    strncpy(temporarySlotOriginalPath, activeSlotPath, sizeof(temporarySlotOriginalPath) - 1);
    temporarySlotOriginalPath[sizeof(temporarySlotOriginalPath) - 1] = '\0';

    // Switch to the temporary slot - fast, just updates slot tracking
    netSlot = tempSlot;
    activeSlotNumber = tempSlot;
    setActivePathFromSlot(tempSlot);
    // (no updateLastActive: temp 8 / 99 must never become the boot context -
    // updateLastActive() gates on temporarySlotActive too, belt and braces)

    // Clear the active state for fresh start (fast - no file I/O)
    // Skip loading temp slot since apps will set up their own connections anyway
    clearActiveSlot();
    
    temporarySlotActive = true;
    return true;
}

bool SlotManager::exitTemporarySlot(bool refreshHardware) {
    if (!temporarySlotActive) {
        return false;
    }
    
    // Restore slot tracking - number AND path together.
    netSlot = temporarySlotOriginal;
    activeSlotNumber = temporarySlotOriginal;
    strncpy(activeSlotPath, temporarySlotOriginalPath, sizeof(activeSlotPath) - 1);
    activeSlotPath[sizeof(activeSlotPath) - 1] = '\0';

    // Reload the original context's data from ITS OWN PATH. This used to read
    // getSlotFilename(temporarySlotOriginal), which for a run file would have
    // been "/slots/slot-2.yaml"; going through the remembered path is what
    // lets a calibration app launched from /projects/555/555_1.yaml come back
    // to that run file.
    String content;
    String errorMsg;

    if (activeSlotPath[0] != '\0' && safeFileExists(activeSlotPath)) {
        File f = safeFileOpen(activeSlotPath, "r", 5000);
        if (f) {
            content = f.readString();
            safeFileClose(f, false);
            activeState.fromYAML(content, errorMsg);
        }
    }

    temporarySlotActive = false;
    temporarySlotOriginal = -1;
    temporarySlotOriginalPath[0] = '\0';

    // POWER RE-ASSERT (design addition #3). This path used to end at
    // refreshConnections(), which restores BRIDGES but not rails/DACs - so
    // exiting a calibration app left whatever voltages the app had set (often
    // 0 V, or a calibration ramp's last value) while the restored slot's
    // power: said otherwise. Every other context-load path asserts power;
    // this one now does too.
    if (!previewModeActive) {
        applyStateToHardware();
    }

    // Refresh hardware connections (caller can skip if they'll do it manually)
    if (refreshHardware) {
        extern void refreshConnections(int ledShowOption, int fillUnused, int clean);
        refreshConnections(-1, 0, 1);
    }

    return true;
}

bool SlotManager::ensureSlotExists(int slotNum) {
    if (slotExists(slotNum)) {
        return true;
    }
    
    // Create empty slot by serializing the current globalState
    // This avoids creating a large JumperlessState on the stack
    String errorMsg;
    String yamlContent;
    
    if (!globalState.toYAML(yamlContent)) {
        return false;
    }
    
    return writeSlotFile(slotNum, yamlContent, errorMsg);
}

// File I/O helpers
bool SlotManager::readSlotFile(int slotNum, String& content, String& errorMsg) {
    String filename = getSlotFilename(slotNum);

    extern bool mscModeEnabled;
    if (mscModeEnabled && debugUSB) {
        Serial.print("USB: readSlotFile() called for slot ");
        Serial.print(slotNum);
        Serial.print(" (");
        Serial.print(filename);
        Serial.println(")");
        Serial.flush();
    }

    // CRITICAL: In USB mode, add extra delay before opening
    if (mscModeEnabled && debugUSB) {
        delay(50);
    }

    File file = safeFileOpen(filename.c_str(), "r", 5000);
    if (!file) {
        errorMsg = "Failed to open slot file: " + filename;
        if (mscModeEnabled && debugUSB) {
            Serial.print("USB: Failed to open file: ");
            Serial.println(errorMsg);
            Serial.flush();
        }
        return false;
    }

    if (mscModeEnabled && debugUSB) {
        Serial.println("USB: File opened successfully, reading content...");
        Serial.flush();
    }

    content = "";
    size_t bytesRead = 0;
    const size_t maxBytes = 65536;  // 64KB safety limit

    while (file.available() && bytesRead < maxBytes) {
        int c = file.read();
        if (c >= 0) {
            content += (char)c;
            bytesRead++;
        } else {
            // Read error
            if (mscModeEnabled && debugUSB) {
                Serial.println("USB: Read error during file read");
                Serial.flush();
            }
            break;
        }
    }

    if (mscModeEnabled && debugUSB) {
        Serial.print("USB: Read ");
        Serial.print(bytesRead);
        Serial.println(" bytes, closing file...");
        Serial.flush();
    }

    safeFileClose(file, false);  // Read-only

    if (mscModeEnabled && debugUSB) {
        Serial.println("USB: File closed successfully");
        Serial.flush();
    }

    return true;
}

bool SlotManager::writeSlotFile(int slotNum, const String& content, String& errorMsg) {
    // Ensure slots directory exists using safe function
    if (!safeMkdir("/slots", 2000)) {
        errorMsg = "Failed to create /slots directory";
        return false;
    }
    
    String filename = getSlotFilename(slotNum);
    
    // Use safe file write which handles Core2 pause and mutex internally
    if (!safeFileWriteAll(filename.c_str(), content.c_str(), content.length(), 2000)) {
        errorMsg = "Failed to write slot file: " + filename;
        return false;
    }
    
    return true;
}

bool SlotManager::migrateOldSlotFile(int slotNum, String& errorMsg) {
    String legacyFilename = getLegacySlotFilename(slotNum);

    File file = safeFileOpen(legacyFilename.c_str(), "r", 5000);
    if (!file) {
        errorMsg = "Failed to open legacy slot file: " + legacyFilename;
        return false;
    }

    String content = "";
    while (file.available()) {
        content += (char)file.read();
    }
    safeFileClose(file, false);  // Read-only
    
    // Parse legacy format directly into the active state. JumperlessState is
    // ~33-50KB and is non-copyable (see States.h) - a stack/temp copy would
    // overflow the RP2040's tiny stack. Migration replaces the active slot with
    // this slot anyway (activeSlotNumber=slotNum below), so parsing in place is
    // correct. On failure the active state is left dirty and we return an error.
    if (!activeState.fromLegacyNodeFile(content, errorMsg)) {
        activeState.clear();
        errorMsg = "Failed to migrate legacy slot " + String(slotNum) + ": " + errorMsg;
        return false;
    }
    
    // Set default voltages for migrated old slot files (voltages no longer in config)
    activeState.setDacVoltage(0, 3.3);  // Default DAC0 voltage
    activeState.setDacVoltage(1, 0.0);  // Default DAC1 voltage
    activeState.setRailVoltage(true, 0.0);   // Default top rail voltage
    activeState.setRailVoltage(false, 0.0);  // Default bottom rail voltage
    activeState.setPathStacking(jumperlessConfig.routing.stack_paths, 
                             jumperlessConfig.routing.stack_rails,
                             jumperlessConfig.routing.stack_dacs);
    
    // Save in new format (active state already holds the migrated content).
    if (!saveSlot(slotNum, errorMsg)) {
        return false;
    }
    
    activeSlotNumber = slotNum;
    netSlot = slotNum;  // Sync global slot tracker
    setActivePathFromSlot(slotNum);

    Serial.println("✓ Migrated legacy slot " + String(slotNum) + " to new format");
    return true;
}

// History management.
// This legacy full-state-snapshot history is disabled (STATE_HISTORY_SIZE == 0)
// and superseded by the delta-based undo log in Undo.cpp. Snapshotting copied
// the entire ~50KB JumperlessState, which is now non-copyable (see States.h) and
// unaffordable on the RP2040. These are kept as no-ops for API compatibility.
void SlotManager::pushHistory() {
    if (historySize <= 0 || historyBuffer == nullptr) return;
    // Disabled: see note above. (Was: historyBuffer[historyHead] = activeState;)
}

bool SlotManager::canUndo() const {
    return historyCount > 0;
}

bool SlotManager::canRedo() const {
    return historyPosition != historyHead;
}

bool SlotManager::undo(String& errorMsg) {
    // Disabled legacy snapshot history (see pushHistory). The active undo system
    // is the delta log in Undo.cpp.
    errorMsg = "Nothing to undo";
    return false;
}

bool SlotManager::redo(String& errorMsg) {
    errorMsg = "Nothing to redo";
    return false;
}

void SlotManager::clearHistory() {
    historyHead = 0;
    historyCount = 0;
    historyPosition = 0;
}

int SlotManager::getHistoryDepth() const {
    if (historyPosition <= historyHead) {
        return historyHead - historyPosition;
    } else {
        return historySize - historyPosition + historyHead;
    }
}

// Utility
void SlotManager::printSlotInfo(int slotNum) {
    Serial.println("=== Slot " + String(slotNum) + " Info ===");
    
    if (!slotExists(slotNum)) {
        Serial.println("Slot does not exist");
        return;
    }
    
    String errorMsg;
    // JumperlessState is non-copyable (~50KB), so we can't stash a copy of the
    // active state. Instead: if inspecting a different slot, persist any unsaved
    // edits, load the target in place to print it, then reload the original
    // CONTEXT from disk. No struct copy.
    //
    // The save/restore dance is captured as a (number, path) PAIR: savedSlot
    // may be SLOT_FILE_CONTEXT, in which case restoring by number is
    // impossible and the path is the only way back.
    int savedSlot = activeSlotNumber;
    char savedPath[128];
    strncpy(savedPath, activeSlotPath, sizeof(savedPath) - 1);
    savedPath[sizeof(savedPath) - 1] = '\0';
    bool switched = (slotNum != savedSlot);

    // Restore helper: by path when the saved context was a file, by number
    // otherwise. Used on both the error and the success exit.
    auto restoreSaved = [&]() {
        String e;
        if (savedSlot == SLOT_FILE_CONTEXT && savedPath[0] != '\0') {
            loadSlotFromPath(String(savedPath), e);
        } else if (savedSlot >= 0) {
            loadSlot(savedSlot, e);
        }
    };

    if (switched) {
        if (activeState.isDirty()) {
            saveActiveSlot(errorMsg);  // path-aware; so the reload restores edits
        }
        if (!loadSlot(slotNum, errorMsg)) {
            Serial.println("Error loading slot: " + errorMsg);
            restoreSaved();  // best-effort restore
            return;
        }
    }

    Serial.println("Connections: " + String(activeState.connections.numBridges));
    Serial.println("Power:");
    Serial.println("  Top Rail:    " + String(activeState.power.topRail, 2) + "V");
    Serial.println("  Bottom Rail: " + String(activeState.power.bottomRail, 2) + "V");
    Serial.println("  DAC 0:       " + String(activeState.power.dac0, 2) + "V");
    Serial.println("  DAC 1:       " + String(activeState.power.dac1, 2) + "V");
    Serial.println("Custom Colors: " + String(activeState.display.numCustomColors));
    Serial.println("RAM Usage: ~" + String(activeState.estimateRAMUsage()) + " bytes");
    
    // Restore the original active context from disk (no struct copy).
    if (switched) {
        restoreSaved();
    }
}

void SlotManager::listSlots() {
    Serial.println("\n=== Available Slots ===");
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (slotExists(i)) {
            String marker = (i == activeSlotNumber) ? " [ACTIVE]" : "";
            Serial.println("Slot " + String(i) + marker);
        }
    }
    // A file context isn't in the 0-7 list at all - name it explicitly so
    // "which slot am I on" has an honest answer.
    if (isPathContext()) {
        Serial.println("Active file: " + String(activeSlotPath));
    }
    Serial.println("=======================\n");
}

size_t SlotManager::getActiveStateRAMUsage() const {
    size_t total = sizeof(SlotManager);
    total += activeState.estimateRAMUsage();
    total += historySize * sizeof(JumperlessState);
    return total;
}

// ============================================================================
// Preview Mode - Load slots without applying to hardware
// Just tracks which slot to return to when done (no state copying!)
// ============================================================================

bool SlotManager::enterPreviewMode(int slotToPreview, String& errorMsg) {
    // Allow slot 99 (Python slot) in addition to 0-7
    if (slotToPreview < 0 || (slotToPreview >= NUM_SLOTS && slotToPreview != 99)) {
        errorMsg = "Invalid slot number: " + String(slotToPreview);
        return false;
    }
    
    // Remember which context we're currently on (to return to it later) -
    // number AND path, because originalSlotNumber may be SLOT_FILE_CONTEXT.
    const bool wasAlreadyPreviewing = previewModeActive;
    const int prevPreviewSlotNumber = previewSlotNumber;
    if (!previewModeActive) {
        originalSlotNumber = activeSlotNumber;
        strncpy(previewOriginalPath, activeSlotPath, sizeof(previewOriginalPath) - 1);
        previewOriginalPath[sizeof(previewOriginalPath) - 1] = '\0';
        // Save original rail voltage settings
        originalRailVoltages[0] = activeState.power.topRail;
        originalRailVoltages[1] = activeState.power.bottomRail;
    }

    String filename = getSlotFilename(slotToPreview);

    // ORDERING TRAP (design-slots.md §2). previewModeActive is raised BEFORE
    // the internal loadSlot, not after it. Two things gate on that flag inside
    // loadSlot, and both were being checked while it was still false:
    //
    //   1. updateLastActive() - so every wheel detent through the Slots
    //      preview menu (Menus -> previewSlotColors -> enterPreviewMode)
    //      stamped the merely-GLANCED-AT slot into last_active.txt. Power loss
    //      mid-preview would then boot a slot the user never chose.
    //   2. applyStateToHardware() - so a preview, which is documented as
    //      "loads into globalState WITHOUT applying to hardware", was in fact
    //      applying the previewed slot's rails and DACs. Raising the flag
    //      first makes preview match its own contract; exitPreview's cancel
    //      path restores rails either way, and its apply path calls
    //      applyStateToHardware() explicitly.
    previewModeActive = true;
    previewSlotNumber = slotToPreview;

    // Check if slot file exists
    if (safeFileExists(filename.c_str())) {
        // Slot exists - load it normally
        if (!loadSlot(slotToPreview, errorMsg)) {
            // Unwind exactly what we changed, so a failed preview neither
            // leaves the manager stuck in preview mode nor discards an
            // in-progress preview's original-context bookkeeping.
            previewModeActive = wasAlreadyPreviewing;
            previewSlotNumber = wasAlreadyPreviewing ? prevPreviewSlotNumber : -1;
            if (!wasAlreadyPreviewing) {
                originalSlotNumber = -1;
                previewOriginalPath[0] = '\0';
            }
            return false;
        }
    } else {
        // Slot doesn't exist - just show empty state (DON'T create file!)
        activeState.clear();
        activeSlotNumber = slotToPreview;
        netSlot = slotToPreview;
        setActivePathFromSlot(slotToPreview);
    }

    // activeState.power is now the single source of truth

    return true;
}

void SlotManager::clearPreviewMode() {
    // Just clear the preview flag without loading anything
    // Used when user selects a slot from menu - we want to keep the previewed state
    // and let the normal load path handle it
    if (previewModeActive) {
        previewModeActive = false;
        previewSlotNumber = -1;
        originalSlotNumber = -1;
        previewOriginalPath[0] = '\0';
    }
}

bool SlotManager::exitPreview(bool applyPreview, String& errorMsg) {
    if (!previewModeActive) {
        errorMsg = "Not in preview mode";
        return false;
    }
    
    if (applyPreview) {
        // User wants to keep the previewed slot
        // If the slot file doesn't exist yet, create it now
        String filename = getSlotFilename(previewSlotNumber);
        if (!safeFileExists(filename.c_str())) {
            // Slot was empty during preview - save it now
            if (!saveSlot(previewSlotNumber, errorMsg)) {
                previewModeActive = false;
                previewSlotNumber = -1;
                originalSlotNumber = -1;
                previewOriginalPath[0] = '\0';
                return false;
            }
        }

        // Exit preview mode and apply the previewed state to hardware
        previewModeActive = false;
        previewSlotNumber = -1;
        originalSlotNumber = -1;
        previewOriginalPath[0] = '\0';

        // Now apply the previewed state to hardware (power, GPIO, etc.)
        applyStateToHardware();

        // activeSlotNumber and netSlot are already set to the previewed slot -
        // but nothing has told last_active.txt yet. The internal saveSlot
        // above ran while previewModeActive was still true, so its
        // updateLastActive() was gated off by design. Without this call the
        // context becomes the previewed slot while the BOOT context still
        // points at the pre-preview one. Menus' caller happens to follow with
        // setActiveSlot (which updates it); LEDs' previewSlotColors caller
        // does not, so it has to be done here.
        updateLastActive();

        return true;
    } else {
        // User wants to cancel - restore original slot AND rail voltages
        previewModeActive = false;
        previewSlotNumber = -1;
        
        // Restore original rail voltage settings
        activeState.power.topRail = originalRailVoltages[0];
        activeState.power.bottomRail = originalRailVoltages[1];
        
        // Load the original context back - by PATH when it was a file
        // context (loadSlot(-2) would just fail), by number otherwise.
        int slotToRestore = originalSlotNumber;
        char pathToRestore[128];
        strncpy(pathToRestore, previewOriginalPath, sizeof(pathToRestore) - 1);
        pathToRestore[sizeof(pathToRestore) - 1] = '\0';
        originalSlotNumber = -1;
        previewOriginalPath[0] = '\0';

        if (slotToRestore == SLOT_FILE_CONTEXT && pathToRestore[0] != '\0') {
            return loadSlotFromPath(String(pathToRestore), errorMsg);
        }
        return loadSlot(slotToRestore, errorMsg);
    }
}

// ============================================================================
// State Backup/Restore Functions (for MicroPython entry/exit, undo, etc.)
// Uses compressed YAML format - only stores actual connections, not empty array slots
// ============================================================================

static String* stateBackupYamlPtr = nullptr;
static bool stateBackupStored = false;

void storeStateBackup(void) {
    // Store a compressed YAML snapshot of the current globalState
    // This only includes actual connections, not empty array slots
    if (stateBackupYamlPtr == nullptr) {
        stateBackupYamlPtr = new String();
    }
    
    *stateBackupYamlPtr = "";
    
    // Serialize current state to compressed YAML
    if (!globalState.toYAML(*stateBackupYamlPtr)) {
        Serial.println("Warning: Failed to serialize state backup");
        stateBackupStored = false;
        return;
    }
    
    stateBackupStored = true;
}

void restoreStateBackup(bool autoSave) {
    // Restore globalState from compressed YAML backup
    if (stateBackupStored && stateBackupYamlPtr != nullptr && stateBackupYamlPtr->length() > 0) {
        String errorMsg;
        
        if (!globalState.fromYAML(*stateBackupYamlPtr, errorMsg)) {
            Serial.println("✗ Error restoring state backup: " + errorMsg);
            return;
        }
        
        if (autoSave) {
            // Path-aware: from a file context saveSlot(netSlot) would be
            // saveSlot(-2) and hit the BUG guard.
            SlotManager& mgr = SlotManager::getInstance();
            String err;
            mgr.saveActiveSlot(err);
        }
    }
}

void restoreAndSaveStateBackup(void) {
    // Restore and immediately save to current slot
    restoreStateBackup(true);
}

void clearStateBackup(void) {
    // Clear the backup
    stateBackupStored = false;
    if (stateBackupYamlPtr != nullptr) {
        *stateBackupYamlPtr = "";
        delete stateBackupYamlPtr;
        stateBackupYamlPtr = nullptr;
    }
}

bool hasStateBackup(void) {
    return stateBackupStored && (stateBackupYamlPtr != nullptr) && (stateBackupYamlPtr->length() > 0);
}

bool hasStateChanges(void) {
    // Compare current state with backup by serializing and comparing YAML
    if (!hasStateBackup()) {
        return false;  // No backup means no changes to compare against
    }
    
    String currentYaml;
    if (!globalState.toYAML(currentYaml)) {
        return false;  // Can't compare if serialization fails
    }
    
    // Compare YAML representations (ignores whitespace differences in serialization)
    // For more robust comparison, we could deserialize and compare connection counts
    return (currentYaml != *stateBackupYamlPtr);
}

size_t getStateBackupSize(void) {
    // Return the size of the backup in bytes (for diagnostics/debugging)
    if (!hasStateBackup()) {
        return 0;
    }
    
    return stateBackupYamlPtr->length();
}

void printStateBackupInfo(void) {
    // Print diagnostic information about state backup memory usage
    Serial.println("\n╔═══════════════════════════════╗");
    Serial.println("║  State Backup Memory Info     ║");
    Serial.println("╚═══════════════════════════════╝");
    
    if (!hasStateBackup()) {
        Serial.println("  No backup stored");
        return;
    }
    
    size_t backupSize = stateBackupYamlPtr->length();
    size_t fullStateSize = sizeof(JumperlessState);
    size_t savings = fullStateSize - backupSize;
    float savingsPercent = (float)savings / fullStateSize * 100.0f;
    
    Serial.println("  Connections: " + String(globalState.connections.numBridges));
    Serial.println("  Nets:        " + String(globalState.connections.numNets));
    Serial.println("");
    Serial.println("  Compressed YAML: " + String(backupSize) + " bytes");
    Serial.println("  Full state:      " + String(fullStateSize) + " bytes");
    Serial.println("  Memory saved:    " + String(savings) + " bytes (" + String(savingsPercent, 1) + "%)");
    
    Serial.println("═══════════════════════════════\n");
}

/**
 * @brief Service method for auto-saving dirty state
 * 
 * This handles automatic state persistence after modifications.
 * Called periodically by the ServiceManager in the main loop.
 * 
 * IMPORTANT USB BEHAVIOR:
 * - When USB is mounted: Allows READ operations (for live file monitoring)
 *                        Blocks WRITE operations (host has exclusive write access)
 * - When USB is not mounted: Normal operation (both read and write)
 * 
 * This enables live updates when host edits files while preventing corruption.
 */
ServiceStatus SlotManager::service() {
    // return ServiceStatus::IDLE; //< this isn't the problem, not running slot manager service still freezes
    //return ServiceStatus::IDLE;
    lastStatus = ServiceStatus::IDLE;
    unsigned long serviceStart = micros();
    
    
    extern bool usbMountedByHost;
    extern bool usbFilesystemBusy;
    extern bool mscModeEnabled;
    extern bool debugUSB;
    
    // ============================================================================
    // FAST PATH: Early exit when there's absolutely nothing to do
    // This makes the common case extremely fast (< 1 microsecond)
    // ============================================================================
    
    // Check if we have any work to do (ordered by likelihood)
    bool hasDirtyState = activeState.isDirty();
    bool hasEditorOpen = (ekilo_get_currently_editing_file() != nullptr);
    bool inPreviewMode = previewModeActive;
    
    // CRITICAL FIX: Only monitor files when host is ACTUALLY mounted, not just when MSC is enabled
    // mscModeEnabled = USB MSC feature is on (always true after boot)
    // usbMountedByHost = host computer is actually accessing files (only when connected to PC)
    // File monitoring is EXPENSIVE (~100ms blocking) and should ONLY run when:
    // 1. Host is editing files via USB MSC (usbMountedByHost)
    // 2. User is editing files via Ekilo (hasEditorOpen)
    bool needsFileMonitoring = usbMountedByHost || hasEditorOpen;
    
    // OPTIMIZATION: During rapid command processing, skip expensive file monitoring
    // Only check files if it's been >2s since last check (prevents 600-700ms delays)
    // CRITICAL FIX: Check BOTH refreshInProgress AND refreshLocalInProgress!
    // Commands call refreshConnections() which sets refreshInProgress,
    // NOT refreshLocalInProgress. Missing this caused the 1-second freeze bug.
    extern volatile bool refreshLocalInProgress;
    extern volatile bool refreshInProgress;
    extern volatile bool core1busy;
    static unsigned long lastFileMonitorTime = 0;
    bool skipFileMonitoring = (millis() - lastFileMonitorTime < 2000) || 
                              refreshLocalInProgress || 
                              refreshInProgress ||
                              core1busy;  // Skip if ANY command processing is active
    
    // FAST EXIT: If nothing is dirty, no editor, no preview, and no need for file monitoring
    // This is the common case during UART command processing - exit immediately
    if (!hasDirtyState && !hasEditorOpen && !inPreviewMode && !needsFileMonitoring) {
        return ServiceStatus::IDLE;  // Ultra-fast return for the common case
    }
    
    // FAST EXIT: If only monitoring files but we're in rapid command mode, skip it
    if (!hasDirtyState && skipFileMonitoring) {
        return ServiceStatus::IDLE;  // Skip expensive file monitoring during rapid commands
    }
    
    // DIAGNOSTIC: Track what work we're doing if service is slow
    const char* slowReason = nullptr;
    
    // Debug output (only when we're actually doing work)
    static unsigned long lastServiceDebug = 0;
    if (mscModeEnabled && debugUSB && (millis() - lastServiceDebug > 2000)) {
        #if debugFP
        Serial.println("◆ SlotManager::service() - active work detected");
        Serial.flush();
        #endif
        lastServiceDebug = millis();
    }
    
    // Mark that we're about to do filesystem operations (if we're doing file work)
    // This prevents host from mounting mid-operation via driveReady callback
    if (needsFileMonitoring) {
        usbFilesystemBusy = true;
    }
    
    static unsigned long lastFileCheckTime = 0;
    static int lastEditingSlotNumber = -1;  // Track which slot is being edited
    
    // Get editor state once at the start (used by multiple sections below)
    const char* currentlyEditing = hasEditorOpen ? ekilo_get_currently_editing_file() : nullptr;
    // Same strict matcher everything else uses. This caller only ever wants
    // canonical slot files (editing /slots/slot3.yaml previews slot 3), so
    // behavior is unchanged - it just no longer has its own loose copy.
    // Guard the nullptr: currentlyEditing is null whenever no editor is open.
    int editingSlotNumber = currentlyEditing
                                ? slotNumberForCanonicalPath(String(currentlyEditing))
                                : -1;
    
    // ============================================================================
    // PREVIEW MODE MANAGEMENT: Only run if editor is open or already in preview mode
    // ============================================================================
    if (hasEditorOpen || inPreviewMode) {
        
        // Handle preview mode state transitions
        if (editingSlotNumber >= 0) {
            // A slot file is being edited
            if (!previewModeActive) {
                // Enter preview mode
                #if debugFP
                Serial.print("Slot ");
                Serial.print(editingSlotNumber);
                Serial.println(" opened in editor - entering preview mode");
                #endif
                
                String errorMsg;
                if (enterPreviewMode(editingSlotNumber, errorMsg)) {
                    lastEditingSlotNumber = editingSlotNumber;
                } else {
                    #if debugFP
                    Serial.print("Failed to enter preview mode: ");
                    Serial.println(errorMsg);
                    #endif
                }
            }
        } else {
            // No slot file is being edited
            if (previewModeActive && lastEditingSlotNumber >= 0) {
                // Exit preview mode (user closed editor without applying)
                #if debugFP
                Serial.print("Editor closed - exiting preview mode for slot ");
                Serial.println(lastEditingSlotNumber);
                #endif
                
                String errorMsg;
                exitPreview(false, errorMsg);  // Don't apply changes
                lastEditingSlotNumber = -1;
            }
        }
    }
    
    // ============================================================================
    // The live file-monitor (f_stat polling + auto-reload) that used to live here
    // was permanently disabled with `&& false` and has been DELETED. Host changes
    // are picked up on eject/unmount instead (usbUnplugCallback reloads the slot).
    // The bookkeeping below is kept because skipFileMonitoring/lastFileMonitorTime
    // still throttle this service's early-exit cadence.
    // ============================================================================
    unsigned long timeSinceLastFileCheck = millis() - lastFileCheckTime;
    
    bool shouldMonitorFiles = (needsFileMonitoring || inPreviewMode) && !skipFileMonitoring;
    
    // Update file monitor time if we're actually checking
    if (shouldMonitorFiles && timeSinceLastFileCheck > 1000) {
        lastFileMonitorTime = millis();
    }
    
    if (mscModeEnabled && debugUSB && shouldMonitorFiles && timeSinceLastFileCheck > 5000) {
        Serial.print("USB: ⚠ File check interval: ");
        Serial.print(timeSinceLastFileCheck);
        Serial.println("ms (should be ~1000ms)");
        Serial.flush();
    }
    
    
    // ============================================================================
    // AUTO-SAVE: Only runs if state is dirty and USB is not mounted
    // This section was already optimized with the early exit check above
    // ============================================================================
    
    // CRITICAL: Don't auto-save while command processing is active (prevents deadlock)
    // Both refreshLocalConnections() and saveSlot() need core synchronization
    extern volatile bool refreshLocalInProgress;
    extern volatile bool core1busy;

    // GATING NEW MODEL:
    //   - Drop the old "2000ms since last mutation" debounce. The user's
    //     window of unsaved work is now bounded by the cache flush gate
    //     (systemIdleForFlush + 60s emergency backstop) instead of a
    //     fixed timer.
    //   - Defer toYAML + cache write to genuine idle. toYAML is ~ms of
    //     CPU work on Core 0; running it on every dirty tick during a
    //     probe burst is wasted effort because we'd just re-serialize
    //     and re-overwrite the cache entry on the next mutation.
    //   - Big events (clear-all, slot switch, USB mount, probe-exit,
    //     shutdown) already call fileCacheFlushNowAll() which invokes
    //     saveSlot synchronously through this same path - those bypass
    //     the gate.
    // A read-only template context can never be auto-saved (see
    // saveStateToActivePath). Skip the ATTEMPT here rather than letting it
    // fail every idle pass: the dirty flag deliberately stays set, so an
    // unguarded attempt would print its refusal ~every 750 ms forever. The
    // per-call loud refusal still fires for user-initiated saves.
    bool activeIsReadOnlyTemplate = isPathContext() && isTemplatePath(activeSlotPath);
    if (hasDirtyState && activeIsReadOnlyTemplate) {
        static unsigned long lastTemplateNote = 0;
        if (millis() - lastTemplateNote > 10000) {
            lastTemplateNote = millis();
            Serial.print("(unsaved edits: ");
            Serial.print(activeSlotPath);
            Serial.println(" is a project template and is not written back)");
        }
    } else if (hasDirtyState && systemIdleForFlush() && !refreshLocalInProgress && !core1busy) {
            slowReason = "auto-save";
            unsigned long saveStart = micros();

            if (debugWaitLoopTiming) {
                Serial.printf("DEBUG: Auto-save triggered (system idle, dirty for %lu ms)\n",
                              (unsigned long)(millis() - activeState.getLastModifiedTime()));
            }
            
            String errorMsg;
            
            // Ensure we're using the current slot (sync with netSlot)
            syncFromGlobalNetSlot();
            
            // saveActiveSlot handles core synchronization and clearDirty, and
            // dispatches on the context's identity: numbered slot -> saveSlot,
            // file context -> write back to activeSlotPath. THIS is the call
            // that makes an arbitrary slot file persist its own edits instead
            // of writing them into whatever number happened to be tracked.
            // Skip validation on auto-save (state is validated when connections are added/removed)
            static unsigned long lastAutoSaveFailPrint = 0;
            if (saveActiveSlot(errorMsg, true)) {
                lastAutoSaveFailPrint = 0;   // next failure prints immediately
                unsigned long saveTime = micros() - saveStart;
                if (debugWaitLoopTiming) {
                if (saveTime > 100000) {
                    Serial.printf("⏱️  saveSlot took %lu ms\n", saveTime / 1000);
                }
                }
                // Successfully auto-saved
                lastStatus = ServiceStatus::BUSY;
                if (debugWaitLoopTiming) {
                    Serial.printf("DEBUG: Auto-save completed\n");
                }
            } else {
                // Don't clear dirty on failure - retry on next loop.
                //
                // THROTTLED, for the same reason the template branch above is:
                // the dirty flag deliberately survives a failure, so a context
                // that cannot be saved re-attempts on every idle pass. The
                // no-active-context terminal state is exactly that - once the
                // user mutates, saveActiveSlot returns "No active slot to
                // save" forever - and an unthrottled print would bury the
                // console. The timestamp resets on any successful save, so a
                // genuinely new failure still prints immediately.
                if (millis() - lastAutoSaveFailPrint > 10000) {
                    lastAutoSaveFailPrint = millis();
                    Serial.print("✗ Auto-save failed (");
                    Serial.print(activeSlotNumber < 0 && !isPathContext()
                                     ? String("no active context")
                                     : (isPathContext()
                                            ? String(activeSlotPath)
                                            : ("slot " + String(activeSlotNumber))));
                    Serial.print("): ");
                    Serial.println(errorMsg);
                }
                lastStatus = ServiceStatus::ERROR;
            }
    } else if (hasDirtyState && usbMountedByHost) {
        // Dirty state exists but can't save while USB is mounted
        // This is normal - we'll save when USB is unmounted
        if (debugWaitLoopTiming) {
            static unsigned long lastUsbBlockMsg = 0;
            if (millis() - lastUsbBlockMsg > 5000) {  // Print every 5 seconds
                Serial.println("DEBUG: Auto-save blocked - USB mounted by host");
                lastUsbBlockMsg = millis();
            }
        }
    }
    
    // Clear busy flag to allow host mounting if requested (only if we set it)
    if (needsFileMonitoring) {
        usbFilesystemBusy = false;
    }
    
    if (mscModeEnabled && debugUSB) {
        static unsigned long lastServiceCompleteDebug = 0;
        if (millis() - lastServiceCompleteDebug > 5000) {
            Serial.println("◆ SlotManager::service() completed normally");
            Serial.flush();
            lastServiceCompleteDebug = millis();
        }
    }
    
    // DIAGNOSTIC: Report if service took too long
    unsigned long serviceTime = micros() - serviceStart;
    if (debugWaitLoopTiming) {
    if (serviceTime > 100000) {  // > 100ms
        Serial.printf("⏱️  States::service() took %lu ms", serviceTime / 1000);
        if (slowReason) {
            Serial.printf(" [%s]", slowReason);
        }
        Serial.println();
        Serial.printf("    hasDirty=%d hasEditor=%d preview=%d needsFileMon=%d skipFile=%d\n",
            hasDirtyState, hasEditorOpen, inPreviewMode, needsFileMonitoring, skipFileMonitoring);
        Serial.flush();
    }
    }
    return lastStatus;
}

