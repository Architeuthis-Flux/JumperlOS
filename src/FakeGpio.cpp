// SPDX-License-Identifier: MIT
/*
 * Fake GPIO Implementation (rewrite)
 *
 * OUTPUT pins: switch chip K between two voltage sources (~50us fast path).
 * INPUT pins: delegate to TimeDomainMultiplexer for ADC reads, apply thresholds here.
 *
 * See FakeGpio.h for API documentation.
 */

#include "FakeGpio.h"
#include "TimeDomainMultiplexer.h"
#include "JumperlessDefines.h"
#include "States.h"
#include "CH446Q.h"
#include "FileParsing.h"
#include "NetManager.h"
#include "Peripherals.h"
#include "LEDs.h"
#include "Commands.h"
#include <Arduino.h>
#include <cmath>
#include <hardware/sync.h>
#include <vector>

// Forward declarations for MicroPython API
extern "C" {
    void jl_dac_set(int channel, float voltage, int save);
}

// ============================================================================
// Global Storage
// ============================================================================

FakeGpioOutput fakeGpioOutputs[MAX_FAKE_GP_OUT];
FakeGpioInput fakeGpioInputs[MAX_FAKE_GP_IN];
TimeDomainMultiplexer tdmInputs;
bool debugFakeGpio = false;

// Aliases for extern compatibility (NetsToChipConnections.cpp, NetManager.cpp)
// These are kept in sync with tdmInputs fields.
int fakeGpioInputAdcChannel = -1;
int fakeGpioCurrentlyConnectedInput = -1;

static bool initialized = false;

// ============================================================================
// Internal Helpers
// ============================================================================

static bool isBreadboardNode(int node) {
    return (node >= 1 && node <= 60);
}

static bool isVoltageSourceNode(int node) {
    return (node == TOP_RAIL || node == BOTTOM_RAIL ||
            node == DAC0 || node == DAC1 || node == GND);
}

static int8_t voltageNodeToChipKX(int node) {
    switch (node) {
        case TOP_RAIL:    return 4;
        case BOTTOM_RAIL: return 5;
        case DAC1:        return 6;
        case DAC0:        return 7;
        case GND:         return 15;
        default:          return -1;
    }
}

// Sync alias globals from TDM state
static void syncAliases() {
    fakeGpioInputAdcChannel = tdmInputs.adcChannel;
    fakeGpioCurrentlyConnectedInput = tdmInputs.activeChannel;
}

// Fast voltage source switch on chip K for outputs
static inline void fastSwitchChipK(int8_t oldX, int8_t newX, int8_t y) {
    if (oldX >= 0 && oldX != newX) {
        sendXYrawUnchecked(CHIP_K, oldX, y, 0);
        lastChipXY[CHIP_K].connected[y] &= ~(1 << oldX);
    }
    if (newX >= 0) {
        sendXYrawUnchecked(CHIP_K, newX, y, 1);
        lastChipXY[CHIP_K].connected[y] |= (1 << newX);
    }
}

// Find chip K Y position for a node from current routed paths
static int8_t findChipKYForNode(int node) {
    for (int i = 0; i < globalState.connections.numPaths; i++) {
        const pathStruct& path = globalState.connections.paths[i];
        if (path.node1 == node || path.node2 == node) {
            for (int j = 0; j < 4; j++) {
                if (path.chip[j] == CHIP_K) {
                    // Serial.print("findChipKYForNode: node=");
                    // Serial.print(node);
                    // Serial.print(" y=");
                    // Serial.println(path.y[j]);
                    // Serial.flush();
                    return path.y[j];
                }
            }
        }
    }
    return -1;
}

// Extract all path hops for a node's path to chip K (for TDM full-path switching).
// Returns number of hops stored (0 if path not found).
// Stores ALL hops INCLUDING the chip K hop (the ADC crosspoint).
// outChips/outX/outY must have room for TDM_MAX_HOPS entries.
static int8_t extractPathHopsForNode(int node, int8_t* outChips, int8_t* outX, int8_t* outY) {
    for (int i = 0; i < globalState.connections.numPaths; i++) {
        const pathStruct& path = globalState.connections.paths[i];
        if (path.node1 != node && path.node2 != node) continue;

        int8_t count = 0;
        for (int j = 0; j < 4 && count < TDM_MAX_HOPS; j++) {
            if (path.chip[j] < 0) continue;
            // Skip the ADC crosspoint on chip K (TDM handles that separately)
            if (path.chip[j] == CHIP_K) continue;
            outChips[count] = (int8_t)path.chip[j];
            outX[count] = (int8_t)path.x[j];
            outY[count] = (int8_t)path.y[j];
            count++;
        }
        return count;
    }
    return 0;
}

// Find net index for a node from current routed paths
// For FakeGPIO inputs, the userNode is in node1 position, node2 is the shared ADC
static int findNetForNode(int node) {
    // First pass: look for exact match in node1 (breadboard node position)
    for (int i = 0; i < globalState.connections.numPaths; i++) {
        const pathStruct& path = globalState.connections.paths[i];
        if (path.node1 == node) {
            if (debugFakeGpio) {
                Serial.print("  findNetForNode: node=");
                Serial.print(node);
                Serial.print(" found at path ");
                Serial.print(i);
                Serial.print(" (node1), net=");
                Serial.println(path.net);
            }
            return path.net;
        }
    }
    // Second pass: check node2 as fallback
    for (int i = 0; i < globalState.connections.numPaths; i++) {
        const pathStruct& path = globalState.connections.paths[i];
        if (path.node2 == node) {
            if (debugFakeGpio) {
                Serial.print("  findNetForNode: node=");
                Serial.print(node);
                Serial.print(" found at path ");
                Serial.print(i);
                Serial.print(" (node2), net=");
                Serial.println(path.net);
            }
            return path.net;
        }
    }
    if (debugFakeGpio) {
        Serial.print("  findNetForNode: node=");
        Serial.print(node);
        Serial.println(" NOT FOUND");
    }
    return -1;
}


// Check if a DAC is connected to anything (except exclude_node)
static bool isDacConnected(int dac_node, int exclude_node = -1) {
    for (int i = 0; i < globalState.connections.numPaths; i++) {
        const pathStruct& path = globalState.connections.paths[i];
        bool has_dac = (path.node1 == dac_node || path.node2 == dac_node);
        bool has_excluded = (path.node1 == exclude_node || path.node2 == exclude_node);
        if (has_dac && !has_excluded) return true;
    }
    return false;
}

// Find available DAC for a target voltage
static int findAvailableDAC(float target_voltage, int exclude_node = -1, float tolerance = 0.1f) {
    if (fabs(globalState.power.dac0 - target_voltage) < tolerance) return 0;
    if (fabs(globalState.power.dac1 - target_voltage) < tolerance) return 1;
    if (!isDacConnected(DAC0, exclude_node)) return 0;
    if (!isDacConnected(DAC1, exclude_node)) return 1;
    return -1;
}

// ============================================================================
// Initialization
// ============================================================================

void initFakeGpio() {
    for (int i = 0; i < MAX_FAKE_GP_OUT; i++) {
        fakeGpioOutputs[i].active = false;
        fakeGpioOutputs[i].userNode = -1;
        fakeGpioOutputs[i].highVoltageNode = -1;
        fakeGpioOutputs[i].lowVoltageNode = -1;
        fakeGpioOutputs[i].currentState = 0;
        fakeGpioOutputs[i].netIndex = -1;
        fakeGpioOutputs[i].thresholdHigh = 2.0f;
        fakeGpioOutputs[i].thresholdLow = 0.8f;
        fakeGpioOutputs[i].chipKY = -1;
        fakeGpioOutputs[i].highVoltageX = -1;
        fakeGpioOutputs[i].lowVoltageX = -1;
        fakeGpioOutputs[i].fastPathReady = false;
    }
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        fakeGpioInputs[i].active = false;
        fakeGpioInputs[i].userNode = -1;
        fakeGpioInputs[i].tdmSlot = -1;
        fakeGpioInputs[i].thresholdHigh = 2.0f;
        fakeGpioInputs[i].thresholdLow = 0.8f;
        fakeGpioInputs[i].currentState = -1;
        fakeGpioInputs[i].netIndex = -1;
    }
    tdmInputs.init();
    syncAliases();
    initialized = true;
}


// ============================================================================
// Display Functions
// ============================================================================

void updateFakeGpioOutputDisplay(int slot) {
    if (slot < 0 || slot >= MAX_FAKE_GP_OUT) return;
    if (!fakeGpioOutputs[slot].active) return;
    // return;

    int idx = GPIO_INDEX_FAKE_OUT(slot);
    FakeGpioOutput& out = fakeGpioOutputs[slot];
    gpioState[idx] = out.currentState;
    gpioReading[idx] = out.currentState;
    gpioNet[idx] = out.netIndex;
    gpioReadingColors[idx] = (out.currentState == 1) ? 0x230205 : 0x052302;
}

void updateFakeGpioInputDisplay(int slot) {
    if (slot < 0 || slot >= MAX_FAKE_GP_IN) return;
    if (!fakeGpioInputs[slot].active) return;

    int idx = GPIO_INDEX_FAKE_IN(slot);
    FakeGpioInput& in = fakeGpioInputs[slot];
    // Don't use bus keeper mode (7) for fake GPIO inputs -- it causes animation
    // slot collisions in Graphics.cpp when there are many inputs. Instead, leave
    // gpioState as 0xFF (no special animation). The gpioReadingColors are set to
    // a voltage-based color from the TDM's last reading, matching showLEDmeasurements().
    gpioState[idx] = 0xFF;
    gpioReading[idx] = in.currentState;
    gpioNet[idx] = in.netIndex;

    // Mirror what showLEDmeasurements() does for ADC nets:
    // voltage → color → brightness → lightUpNet → scaleBrightness → gpioReadingColors
    if (in.tdmSlot >= 0 && in.tdmSlot < TDM_MAX_CHANNELS && in.netIndex > 0) {
        float voltage = tdmInputs.channels[in.tdmSlot].lastVoltage;

        uint32_t color = measurementToColor(voltage, -7.0f, 7.0f);

        int brightness = LEDbrightnessSpecial + (int)fabsf(voltage * 2.0f);
        if (brightness <= 4) brightness = 4;
        else if (brightness > 100) brightness = 100;

        if (jumperlessConfig.display.lines_wires == 0 ||
            numberOfShownNets > MAX_NETS_FOR_WIRES) {
            lightUpNet(in.netIndex, -1, 1, brightness, 0, 0, color);
        }

        int scaleVoltage = map((int)fabsf(voltage), 0, 8, -30, 70);
        color = scaleBrightness(color, scaleVoltage);

        gpioReadingColors[idx] = color;
    } else {
        gpioReadingColors[idx] = 0x040408;  // Gray for unconfigured
    }
}

static void clearOutputDisplay(int slot) {
    int idx = GPIO_INDEX_FAKE_OUT(slot);
    gpioState[idx] = 0xff;
    gpioReading[idx] = 0xff;
    gpioNet[idx] = -1;
    gpioReadingColors[idx] = 0x040408;
}

static void clearInputDisplay(int slot) {
    int idx = GPIO_INDEX_FAKE_IN(slot);
    gpioState[idx] = 0xff;
    gpioReading[idx] = 0xff;
    gpioNet[idx] = -1;
    gpioReadingColors[idx] = 0x040408;
}

void clearAllFakeGpio() {
    if (!initialized) return;
    
    // Clear all outputs
    for (int i = 0; i < MAX_FAKE_GP_OUT; i++) {
        if (fakeGpioOutputs[i].active) {
            fakeGpioOutputs[i].active = false;
            fakeGpioOutputs[i].userNode = -1;
            fakeGpioOutputs[i].fastPathReady = false;
            clearOutputDisplay(i);
        }
    }
    
    // Clear all inputs and TDM channels
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        if (fakeGpioInputs[i].active) {
            if (fakeGpioInputs[i].tdmSlot >= 0) {
                tdmInputs.removeChannel(fakeGpioInputs[i].userNode);
            }
            fakeGpioInputs[i].active = false;
            fakeGpioInputs[i].userNode = -1;
            fakeGpioInputs[i].tdmSlot = -1;
            fakeGpioInputs[i].currentState = -1;
            clearInputDisplay(i);
        }
    }
    
    syncAliases();
}

// ============================================================================
// Output Configuration
// ============================================================================

int fakeGpioConfigOutput(int node, int highNode, int lowNode,
                         float thresholdHigh, float thresholdLow) {
    if (!initialized) initFakeGpio();
    // return -1;
    // Serial.println("fakeGpioConfigOutput");
    // Serial.print("node: ");
    // Serial.println(node);
    // Serial.print("highNode: ");
    // Serial.println(highNode);
    // Serial.print("lowNode: ");
    // Serial.println(lowNode);
    // Serial.print("thresholdHigh: ");
    // Serial.println(thresholdHigh);
    // Serial.print("thresholdLow: ");
    // Serial.println(thresholdLow);
    // Serial.println("--------------------------------");
    // Serial.flush();
    Serial.print("fake GPIO output disabled (until I fix it)");
    Serial.flush();
    return -1;

    if (!isBreadboardNode(node)) {
        Serial.println("ERROR: fakeGpioConfigOutput: node must be 1-60");
        return -1;
    }
    if (!isVoltageSourceNode(highNode) || !isVoltageSourceNode(lowNode)) {
        Serial.println("ERROR: fakeGpioConfigOutput: invalid voltage source node");
        return -1;
    }
    if (highNode == lowNode) {
        Serial.println("ERROR: fakeGpioConfigOutput: high and low must differ");
        return -1;
    }

    int slot = findFakeGpioOutputSlot(node);
    if (slot == -1) slot = findFreeFakeGpioOutputSlot();
    if (slot == -1) {
        Serial.println("ERROR: fakeGpioConfigOutput: no free output slots");
        return -1;
    }

    FakeGpioOutput& out = fakeGpioOutputs[slot];
    int virtualNode = FAKE_GP_OUT_0 + slot;

    // Remove old bridge if reconfiguring
    if (out.active && out.userNode > 0) {
        removeBridgeFromState(out.userNode, FAKE_GP_OUT_0 + slot, false);
    }
    // Clean up any pre-existing voltage source bridges
    removeBridgeFromState(node, TOP_RAIL, false);
    removeBridgeFromState(node, BOTTOM_RAIL, false);
    removeBridgeFromState(node, DAC0, false);
    removeBridgeFromState(node, DAC1, false);
    removeBridgeFromState(node, GND, false);
    refreshLocalConnections(0, 1, 0);
    waitCore2();
    // Serial.println("after removeBridgeFromState");
    // printPathsCompact();
    // printChipStateArray();


    // Initialize slot BEFORE adding bridge (router reads this for expansion)
    out.active = true;
    out.userNode = node;
    out.highVoltageNode = highNode;
    out.lowVoltageNode = lowNode;
    out.thresholdHigh = thresholdHigh;
    out.thresholdLow = thresholdLow;
    out.currentState = 0;  // Start LOW
    out.netIndex = -1;
    out.highVoltageX = voltageNodeToChipKX(highNode);
    out.lowVoltageX = voltageNodeToChipKX(lowNode);
    out.chipKY = -1;
    out.fastPathReady = false;

    // Add bridge and route
    addBridgeToState(node, virtualNode, 0, false);
    refreshLocalConnections(0, 1, 0);
    waitCore2();
    // Serial.println("\n\rafter refreshLocalConnections");
    // printPathsCompact();
    // printChipStateArray();

    // Extract chip K Y and net from routed paths
    out.chipKY = findChipKYForNode(node);
    out.netIndex = findNetForNode(node);
    Serial.print("findNetForNode: node=");
    Serial.print(node);
    Serial.print(" netIndex=");
    Serial.println(out.netIndex);
    Serial.flush();
    //out.fastPathReady = (out.chipKY >= 0 && out.highVoltageX >= 0 && out.lowVoltageX >= 0);

    // if (debugFakeGpio) {
        Serial.print("fakeGpioConfigOutput: slot=");
        Serial.print(slot);
        Serial.print(" node=");
        Serial.print(node);
        Serial.print(" chipKY=");
        Serial.print(out.chipKY);
        Serial.print(" fastPath=");
        Serial.println(out.fastPathReady);
    // }

 updateFakeGpioOutputDisplay(slot);

 

//  removeBridgeFromState(node, virtualNode, true);
//  refreshLocalConnections(0, 1, 0);
//  waitCore2();
//  Serial.println("\n\rafter removeBridgeFromState");
//  printPathsCompact();
//  printChipStateArray();

    extern volatile int sendAllPathsCore2;
    sendAllPathsCore2 = 3;
    __dmb();

    return slot;
}

int fakeGpioConfigOutputVoltage(int node, float vHigh, float vLow,
                                float thresholdHigh, float thresholdLow) {

                                    return -1;
                                    Serial.println("fakeGpioConfigOutputVoltage");
                                    Serial.print("node: ");
                                    Serial.println(node);
                                    Serial.print("vHigh: ");
                                    Serial.println(vHigh);
                                    Serial.print("vLow: ");
                                    Serial.println(vLow);
                                    Serial.print("thresholdHigh: ");
                                    Serial.println(thresholdHigh);
                                    Serial.print("thresholdLow: ");
                                    Serial.println(thresholdLow);
                                    Serial.println("--------------------------------");
                                    Serial.flush();
                                    return -1;
    if (!initialized) initFakeGpio();

    // Map voltages to nodes
    int highNode = -1, lowNode = -1;

    // HIGH source
    if (vHigh >= 4.5f)                           highNode = TOP_RAIL;
    else if (vHigh >= 3.0f && vHigh <= 3.5f)     highNode = BOTTOM_RAIL;
    else if (vHigh >= 0.0f && vHigh < 5.0f) {
        int dac = findAvailableDAC(vHigh, node);
        if (dac == 0)      { highNode = DAC0; jl_dac_set(0, vHigh, 1); }
        else if (dac == 1)  { highNode = DAC1; jl_dac_set(1, vHigh, 1); }
        else { Serial.println("ERROR: No DAC for HIGH voltage"); return -1; }
    } else { Serial.println("ERROR: Invalid HIGH voltage"); return -1; }

    // LOW source
    if (vLow <= 0.1f)                            lowNode = GND;
    else if (vLow >= 3.0f && vLow <= 3.5f)       lowNode = BOTTOM_RAIL;
    else if (vLow >= 0.0f && vLow < 5.0f) {
        int dac = findAvailableDAC(vLow, node);
        if (dac == 0 && highNode != DAC0)      { lowNode = DAC0; jl_dac_set(0, vLow, 1); }
        else if (dac == 1 && highNode != DAC1)  { lowNode = DAC1; jl_dac_set(1, vLow, 1); }
        else { Serial.println("ERROR: No DAC for LOW voltage"); return -1; }
    } else { Serial.println("ERROR: Invalid LOW voltage"); return -1; }

    if (highNode == lowNode) {
        Serial.println("ERROR: HIGH and LOW resolved to same node");
        return -1;
    }

    return fakeGpioConfigOutput(node, highNode, lowNode, thresholdHigh, thresholdLow);
}

int fakeGpioRemoveOutput(int node) {
    int slot = findFakeGpioOutputSlot(node);
    if (slot == -1) return 0;

    FakeGpioOutput& out = fakeGpioOutputs[slot];
    int virtualNode = FAKE_GP_OUT_0 + slot;
    removeBridgeFromState(out.userNode, virtualNode, true);

    out.active = false;
    out.userNode = -1;
    out.fastPathReady = false;
    clearOutputDisplay(slot);
    return 1;
}

// ============================================================================
// Output Read/Write
// ============================================================================

int fakeGpioWrite(int node, int state) {
    return -1;
    Serial.println("fakeGpioWrite");
    Serial.print("node: ");
    Serial.println(node);
    Serial.print("state: ");
    Serial.println(state);
    Serial.println("--------------------------------");
    Serial.flush();
    int slot = findFakeGpioOutputSlot(node);
    if (slot == -1) return 0;

    FakeGpioOutput& out = fakeGpioOutputs[slot];
    state = (state != 0) ? 1 : 0;
    //if (out.currentState == state) return 1;

    // FAST PATH: direct chip K switching
    if (out.fastPathReady) {
        int8_t oldX = (out.currentState == 1) ? out.highVoltageX : out.lowVoltageX;
        int8_t newX = (state == 1) ? out.highVoltageX : out.lowVoltageX;
        fastSwitchChipK(oldX, newX, out.chipKY);
        out.currentState = state;
        updateFakeGpioOutputDisplay(slot);
        return 1;
    }

    // SLOW PATH: full routing refresh
    out.currentState = state;
    refreshLocalConnections(0, 1, 0);

    // Try to enable fast path
    if (out.highVoltageX >= 0 && out.lowVoltageX >= 0) {
        out.chipKY = findChipKYForNode(node);
        out.fastPathReady = (out.chipKY >= 0);
    }

    updateFakeGpioOutputDisplay(slot);

    extern volatile int sendAllPathsCore2;
    extern volatile bool pauseCore2;
    if (!pauseCore2) {
        sendAllPathsCore2 = 3;
        __dmb();
        waitCore2();
    }
    return 1;
}

int fakeGpioWriteBatch(const int* nodes, const int* states, int count) {
    int success = 0;
    bool anySlowPath = false;

    // First pass: fast path writes
    for (int i = 0; i < count; i++) {
        int slot = findFakeGpioOutputSlot(nodes[i]);
        if (slot == -1) continue;
        FakeGpioOutput& out = fakeGpioOutputs[slot];
        int st = (states[i] != 0) ? 1 : 0;
        if (out.currentState == st) { success++; continue; }
        if (out.fastPathReady) {
            int8_t oldX = (out.currentState == 1) ? out.highVoltageX : out.lowVoltageX;
            int8_t newX = (st == 1) ? out.highVoltageX : out.lowVoltageX;
            fastSwitchChipK(oldX, newX, out.chipKY);
            out.currentState = st;
            updateFakeGpioOutputDisplay(slot);
            success++;
        } else {
            anySlowPath = true;
        }
    }

    // Second pass: single refresh for all slow-path writes
    if (anySlowPath) {
        for (int i = 0; i < count; i++) {
            int slot = findFakeGpioOutputSlot(nodes[i]);
            if (slot == -1) continue;
            FakeGpioOutput& out = fakeGpioOutputs[slot];
            if (out.fastPathReady) continue;
            out.currentState = (states[i] != 0) ? 1 : 0;
        }
        refreshLocalConnections(0, 1, 0);
        for (int i = 0; i < count; i++) {
            int slot = findFakeGpioOutputSlot(nodes[i]);
            if (slot == -1) continue;
            FakeGpioOutput& out = fakeGpioOutputs[slot];
            if (!out.fastPathReady && out.highVoltageX >= 0 && out.lowVoltageX >= 0) {
                out.chipKY = findChipKYForNode(nodes[i]);
                out.fastPathReady = (out.chipKY >= 0);
            }
            updateFakeGpioOutputDisplay(slot);
            success++;
        }
    }
    return success;
}

int fakeGpioReadOutput(int node) {
    int slot = findFakeGpioOutputSlot(node);
    if (slot == -1) return -1;
    return fakeGpioOutputs[slot].currentState;
}

// ============================================================================
// Input Configuration
// ============================================================================
int fakeGpioRemoveInput(int node) {
    int slot = findFakeGpioInputSlot(node);
    if (slot == -1) return 0;

    FakeGpioInput& in = fakeGpioInputs[slot];
    int virtualNode = FAKE_GP_IN_0 + slot;

    tdmInputs.removeChannel(in.userNode);
    removeBridgeFromState(in.userNode, virtualNode, false);

    in.active = false;
    in.userNode = -1;
    in.tdmSlot = -1;
    in.currentState = -1;
    in.netIndex = -1;
    gpioReadingColors[slot] = 0;

    clearInputDisplay(slot);
    refreshLocalConnections(0, 1, 0);
    syncAliases();
    return 1;
}


int fakeGpioConfigInput(int node, float thresholdHigh, float thresholdLow) {
    if (!initialized) initFakeGpio();

    if (!isBreadboardNode(node)) {
        Serial.println("ERROR: fakeGpioConfigInput: node must be 1-60");
        return -1;
    }

    // Assign ADC if not yet assigned
    if (tdmInputs.adcChannel < 0) {
        if (tdmInputs.assignFreeAdc() < 0) {
            Serial.println("ERROR: fakeGpioConfigInput: no free ADC (0-3 all in use)");
            return -1;
        }
        syncAliases();
        if (debugFakeGpio) {
            Serial.print("  Assigned ADC");
            Serial.println(tdmInputs.adcChannel);
        }
    }

    // Find or allocate input slot
    int slot = findFakeGpioInputSlot(node);
    bool isReconfig = (slot >= 0);
    if (slot == -1) slot = findFreeFakeGpioInputSlot();
    if (slot == -1) {
        Serial.println("ERROR: fakeGpioConfigInput: no free input slots");
        return -1;
    }

    FakeGpioInput& in = fakeGpioInputs[slot];
    int virtualNode = FAKE_GP_IN_0 + slot;

    // Remove old bridge if reconfiguring - do a full cleanup
  //  if (isReconfig && in.userNode >= 0) {
        // tdmInputs.removeChannel(in.userNode);
        // Serial.print("  Removing old bridge for node ");
        // Serial.println(in.userNode);
        // Serial.print("  Removing old bridge for virtual node ");
        // Serial.println(FAKE_GP_IN_0 + slot);
        // Serial.flush();
       // fakeGpioRemoveInput(in.userNode);
        // removeBridgeFromState(in.userNode, FAKE_GP_IN_0 + slot, true);
        // // Clear the slot completely
        // in.active = false;
        // in.userNode = -1;
        // in.netIndex = -1;
        // in.tdmSlot = -1;

        // Refresh connections to clean up paths before re-adding
        // refreshLocalConnections(0, 1, 0);
        // waitCore2();
   // }

    // Initialize slot BEFORE adding bridge
    in.active = true;
    in.userNode = node;
    in.thresholdHigh = thresholdHigh;
    in.thresholdLow = thresholdLow;
    in.currentState = 0;  // Default LOW until first read (avoids flicker from -1/unknown)
    in.netIndex = -1;
    in.tdmSlot = -1;

    // Add bridge and route (router expands FAKE_GP_IN_x to selected ADC)
    addBridgeToState(node, virtualNode, 0, false);
    refreshLocalConnections(0, 1, 0);

    // Extract chip K Y, net, and full path hops for TDM full-path switching
    int8_t chipKY = findChipKYForNode(node);
    in.netIndex = findNetForNode(node);

    // Extract non-chip-K hops (breadboard → intermediate chips)
    int8_t hopChips[TDM_MAX_HOPS], hopX[TDM_MAX_HOPS], hopY[TDM_MAX_HOPS];
    int8_t numHops = extractPathHopsForNode(node, hopChips, hopX, hopY);

    // Register with TDM (includes full path hops for isolation during switching)
    int tdmSlot = tdmInputs.addChannel(node, chipKY, in.netIndex,
                                        hopChips, hopX, hopY, numHops);
    in.tdmSlot = tdmSlot;

    // Disconnect the ENTIRE path after routing so this input doesn't short with
    // other inputs that may share the same chip K Y position. The TDM will
    // reconnect the full path only when it's this channel's turn to be read.
    if (chipKY >= 0) {
        int8_t adcX = tdmInputs.getChipKX();
        // Disconnect ADC crosspoint
        sendXYrawUnchecked(CHIP_K, adcX, chipKY, 0);
        lastChipXY[CHIP_K].connected[chipKY] &= ~(1 << adcX);
    }
    // Disconnect breadboard-side hops too
    for (int h = 0; h < numHops; h++) {
        if (hopChips[h] >= 0 && hopX[h] >= 0 && hopY[h] >= 0) {
            sendXYrawUnchecked(hopChips[h], hopX[h], hopY[h], 0);
            lastChipXY[hopChips[h]].connected[hopY[h]] &= ~(1 << hopX[h]);
        }
    }

    syncAliases();

    if (debugFakeGpio) {
        Serial.print("fakeGpioConfigInput: slot=");
        Serial.print(slot);
        Serial.print(" node=");
        Serial.print(node);
        Serial.print(" chipKY=");
        Serial.print(chipKY);
        Serial.print(" tdmSlot=");
        Serial.print(tdmSlot);
        Serial.print(" adc=");
        Serial.println(tdmInputs.adcChannel);
    }

    updateFakeGpioInputDisplay(slot);

    extern volatile int sendAllPathsCore2;
    sendAllPathsCore2 = 3;
    __dmb();

    return slot;
}



// ============================================================================
// Input Read
// ============================================================================

int fakeGpioRead(int node) {
    // Check outputs first (reading an output returns stored state)
    int outSlot = findFakeGpioOutputSlot(node);
    if (outSlot >= 0) return fakeGpioOutputs[outSlot].currentState;

    // Check inputs
    int inSlot = findFakeGpioInputSlot(node);
    if (inSlot < 0) return -1;

    FakeGpioInput& in = fakeGpioInputs[inSlot];
    if (in.tdmSlot < 0) return -1;

    // Switch to this input's TDM channel and read raw voltage
    float voltage = tdmInputs.switchAndRead(in.tdmSlot, 2);
    syncAliases();

    // Apply thresholds with hysteresis
    int reading;
    if (voltage >= in.thresholdHigh)      reading = 1;
    else if (voltage <= in.thresholdLow)  reading = 0;
    else reading = (in.currentState >= 0) ? in.currentState : 0;

    in.currentState = reading;
    updateFakeGpioInputDisplay(inSlot);
    return reading;
}

// ============================================================================
// Background Reading
// ============================================================================

static unsigned long lastReadFakeGPIO = 0;
static unsigned long readFakeGPIOInterval = 50;  // microseconds
static unsigned long updateFakeGpioCrossbarInterval = 111111;  // microseconds
static unsigned long lastUpdateFakeGpioCrossbar = 0;  // microseconds

void readFakeGPIO(void) {
    if (micros() - lastReadFakeGPIO < readFakeGPIOInterval) return;
    lastReadFakeGPIO = micros();

    if (tdmInputs.activeCount <= 0) return;

    // Round-robin: read next TDM channel
    int tdmSlot = tdmInputs.pollNext(4);
    if (tdmSlot < 0) return;

    syncAliases();

    // Find which FakeGpioInput owns this TDM channel and apply thresholds
    TdmChannel& ch = tdmInputs.channels[tdmSlot];
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        FakeGpioInput& in = fakeGpioInputs[i];
        if (!in.active || in.tdmSlot != tdmSlot) continue;

        // Apply thresholds with hysteresis
        if (ch.lastVoltage >= in.thresholdHigh)       in.currentState = 1;
        else if (ch.lastVoltage <= in.thresholdLow)    in.currentState = 0;
        // else: in hysteresis zone, keep previous state

        updateFakeGpioInputDisplay(i);
        break;
    }

    if (micros() - lastUpdateFakeGpioCrossbar > updateFakeGpioCrossbarInterval) {
        lastUpdateFakeGpioCrossbar = micros();
        updateLiveCrossbarDisplay();
    }
}

// ============================================================================
// Connection Change Hook
// ============================================================================

void updateFakeGpioAfterConnectionChange(int node1, int node2) {
    if (!initialized) return;
    // return;

    // --- ADC reassignment: if someone claimed our ADC, find a new one ---
    if (tdmInputs.adcChannel >= 0) {
        int currentAdcNode = ADC0 + tdmInputs.adcChannel;
        if (!IS_FAKE_GP_IN(node1) && !IS_FAKE_GP_IN(node2)) {
            if (node1 == currentAdcNode || node2 == currentAdcNode) {
                Serial.printf("FakeGPIO: ADC %d claimed by connection, reassigning...\n", tdmInputs.adcChannel);
                int newAdc = tdmInputs.reassignAdc();
                Serial.printf("FakeGPIO: ADC reassigned to %d\n", newAdc);
                syncAliases();
            }
        }
    }

    // --- Update inputs ---
    for (int slot = 0; slot < MAX_FAKE_GP_IN; slot++) {
        FakeGpioInput& in = fakeGpioInputs[slot];
        if (!in.active) continue;

        // Check if bridge still exists
        int virtualNode = FAKE_GP_IN_0 + slot;
        bool bridgeExists = false;
        for (int i = 0; i < globalState.connections.numBridges; i++) {
            int n1 = globalState.connections.bridges[i][0];
            int n2 = globalState.connections.bridges[i][1];
            if ((n1 == virtualNode && n2 == in.userNode) ||
                (n2 == virtualNode && n1 == in.userNode)) {
                bridgeExists = true;
                break;
            }
        }

        if (!bridgeExists) {
            // Deactivate
            tdmInputs.removeChannel(in.userNode);
            in.active = false;
            in.userNode = -1;
            in.tdmSlot = -1;
            clearInputDisplay(slot);
            syncAliases();
            continue;
        }

        // Update chip K Y and path hops if affected
        bool affected = (node1 == in.userNode || node2 == in.userNode ||
                         IS_FAKE_GP_IN(node1) || IS_FAKE_GP_IN(node2));
        if (affected && in.tdmSlot >= 0) {
            int8_t newY = findChipKYForNode(in.userNode);
            tdmInputs.updateChannelChipKY(in.tdmSlot, newY);

            // Also refresh full path hops (routing may have changed intermediate chips)
            int8_t hopChips[TDM_MAX_HOPS], hopX[TDM_MAX_HOPS], hopY[TDM_MAX_HOPS];
            int8_t numHops = extractPathHopsForNode(in.userNode, hopChips, hopX, hopY);
            tdmInputs.updateChannelPath(in.tdmSlot, hopChips, hopX, hopY, numHops);

            syncAliases();
        }
    }

    // --- Update outputs ---
    for (int slot = 0; slot < MAX_FAKE_GP_OUT; slot++) {
        FakeGpioOutput& out = fakeGpioOutputs[slot];
        if (!out.active) continue;

        int virtualNode = FAKE_GP_OUT_0 + slot;
        bool bridgeExists = false;
        for (int i = 0; i < globalState.connections.numBridges; i++) {
            int n1 = globalState.connections.bridges[i][0];
            int n2 = globalState.connections.bridges[i][1];
            if ((n1 == virtualNode && n2 == out.userNode) ||
                (n2 == virtualNode && n1 == out.userNode)) {
                bridgeExists = true;
                break;
            }
        }

        if (!bridgeExists) {
            out.active = false;
            out.userNode = -1;
            out.fastPathReady = false;
            clearOutputDisplay(slot);
            continue;
        }

        bool affected = (node1 == out.userNode || node2 == out.userNode ||
                         node1 == out.highVoltageNode || node2 == out.highVoltageNode ||
                         node1 == out.lowVoltageNode || node2 == out.lowVoltageNode);
        if (affected) {
            int8_t oldY = out.chipKY;
            out.chipKY = findChipKYForNode(out.userNode);
            out.fastPathReady = (out.chipKY >= 0 && out.highVoltageX >= 0 && out.lowVoltageX >= 0);
        }
    }
}

// ============================================================================
// State Restoration (from YAML)
// ============================================================================

// ============================================================================
// State Restoration - Phase 1: Pre-routing
// ============================================================================
// Populates in-memory FakeGpio slot structs from saved YAML state.
// Does NOT add bridges (already loaded from YAML) and does NOT route.
// Must be called BEFORE refreshConnections() so the router can expand
// FAKE_GP_OUT_x virtual nodes to the correct voltage sources.

void initializeFakeGpioFromLoadedState() {
    if (!initialized) initFakeGpio();
    extern std::vector<FakeGpioRestorationInfo> pendingFakeGpioRestorations;

    if (!pendingFakeGpioRestorations.empty()) {
        // --- Restore from YAML fakeGpio config ---
        for (const auto& info : pendingFakeGpioRestorations) {
            if (info.node < 0) continue;

            if (info.mode == 1) {
                // OUTPUT: populate slot struct so router can expand the virtual node.
                // The bridge (node ↔ FAKE_GP_OUT_x) is already in the bridge array.
                // Use the saved slot number (0-7 = index into fakeGpioOutputs[])
                continue;
                int slot = info.slot;
                if (slot < 0 || slot >= MAX_FAKE_GP_OUT) {
                    // Fallback: find by node or auto-assign
                    slot = findFakeGpioOutputSlot(info.node);
                    if (slot == -1) slot = findFreeFakeGpioOutputSlot();
                }
                if (slot == -1 || slot >= MAX_FAKE_GP_OUT) continue;

                FakeGpioOutput& out = fakeGpioOutputs[slot];
                out.active = true;
                out.userNode = info.node;
                out.highVoltageNode = (info.high_voltage_node > 0) ? info.high_voltage_node : TOP_RAIL;
                out.lowVoltageNode = (info.low_voltage_node > 0) ? info.low_voltage_node : GND;
                out.thresholdHigh = info.threshold_high;
                out.thresholdLow = info.threshold_low;
                out.currentState = 0;
                out.netIndex = -1;
                out.highVoltageX = voltageNodeToChipKX(out.highVoltageNode);
                out.lowVoltageX = voltageNodeToChipKX(out.lowVoltageNode);
                out.chipKY = -1;
                out.fastPathReady = false;
            } else if (info.mode == 0) {
                // INPUT: populate slot struct. ADC assignment is below.
                // Saved slot = MAX_FAKE_GP_OUT + input_index (e.g. slot 8 = input[0])
                int slot = info.slot - MAX_FAKE_GP_OUT;
                if (slot < 0 || slot >= MAX_FAKE_GP_IN) {
                    // Fallback: find by node or auto-assign
                    slot = findFakeGpioInputSlot(info.node);
                    if (slot == -1) slot = findFreeFakeGpioInputSlot();
                }
                if (slot == -1 || slot >= MAX_FAKE_GP_IN) continue;

                FakeGpioInput& in = fakeGpioInputs[slot];
                in.active = true;
                in.userNode = info.node;
                in.thresholdHigh = info.threshold_high;
                in.thresholdLow = info.threshold_low;
                in.currentState = 0;
                in.netIndex = -1;
                in.tdmSlot = -1;
            }

            if (debugFakeGpio) {
                Serial.print("Pre-route restore FakeGPIO node ");
                Serial.print(info.node);
                Serial.print(" as ");
                Serial.println(info.mode == 0 ? "INPUT" : "OUTPUT");
            }
        }
        // Assign ADC early so the router can expand FAKE_GP_IN_x correctly
        bool hasInputs = false;
        for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
            if (fakeGpioInputs[i].active) { hasInputs = true; break; }
        }
        if (hasInputs && tdmInputs.adcChannel < 0) {
            tdmInputs.assignFreeAdc();
            syncAliases();
        }

        // Don't clear pendingFakeGpioRestorations yet -- Phase 2 needs to know
        // which pins were restored so it can finalize them.
        return;
    }

    // --- Fallback: scan bridges for FAKE_GP_OUT_x / FAKE_GP_IN_x virtual nodes ---
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        int n1 = globalState.connections.bridges[i][0];
        int n2 = globalState.connections.bridges[i][1];

        if (IS_FAKE_GP_OUT(n1) || IS_FAKE_GP_OUT(n2)) {
            continue;
            int virtualNode = IS_FAKE_GP_OUT(n1) ? n1 : n2;
            int userNode = (virtualNode == n1) ? n2 : n1;
            int slot = FAKE_GP_OUT_SLOT(virtualNode);
            if (slot >= 0 && slot < MAX_FAKE_GP_OUT && !fakeGpioOutputs[slot].active) {
                // Infer voltage sources from other bridges on this node
                int highNode = TOP_RAIL, lowNode = GND;
                for (int j = 0; j < globalState.connections.numBridges; j++) {
                    int bn1 = globalState.connections.bridges[j][0];
                    int bn2 = globalState.connections.bridges[j][1];
                    if (bn1 == userNode || bn2 == userNode) {
                        int other = (bn1 == userNode) ? bn2 : bn1;
                        if (isVoltageSourceNode(other) && other != GND) highNode = other;
                        if (other == GND || other == BOTTOM_RAIL) lowNode = other;
                    }
                }
                FakeGpioOutput& out = fakeGpioOutputs[slot];
                out.active = true;
                out.userNode = userNode;
                out.highVoltageNode = highNode;
                out.lowVoltageNode = lowNode;
                out.thresholdHigh = 2.0f;
                out.thresholdLow = 0.8f;
                out.currentState = 0;
                out.netIndex = -1;
                out.highVoltageX = voltageNodeToChipKX(highNode);
                out.lowVoltageX = voltageNodeToChipKX(lowNode);
                out.chipKY = -1;
                out.fastPathReady = false;
            }
        }

        if (IS_FAKE_GP_IN(n1) || IS_FAKE_GP_IN(n2)) {
            int virtualNode = IS_FAKE_GP_IN(n1) ? n1 : n2;
            int userNode = (virtualNode == n1) ? n2 : n1;
            if (!IS_FAKE_GP_IN(userNode)) {
                int slot = FAKE_GP_IN_SLOT(virtualNode);
                if (slot >= 0 && slot < MAX_FAKE_GP_IN) {
                    // Check if we should init this slot:
                    // - If inactive, init it
                    // - If active but same userNode, reinit it (reuse)
                    bool shouldInit = !fakeGpioInputs[slot].active || 
                                     fakeGpioInputs[slot].userNode == userNode;
                    if (shouldInit) {
                        FakeGpioInput& in = fakeGpioInputs[slot];
                        in.active = true;
                        in.userNode = userNode;
                        in.thresholdHigh = 2.0f;
                        in.thresholdLow = 0.8f;
                        in.currentState = 0;
                        in.netIndex = -1;
                        in.tdmSlot = -1;
                    }
                }
            }
        }
    }

    // Assign ADC early so the router can expand FAKE_GP_IN_x correctly
    bool hasInputsFallback = false;
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        if (fakeGpioInputs[i].active) { hasInputsFallback = true; break; }
    }
    if (hasInputsFallback && tdmInputs.adcChannel < 0) {
        tdmInputs.assignFreeAdc();
        syncAliases();
    }
}

// ============================================================================
// State Restoration - Phase 2: Post-routing
// ============================================================================
// Called AFTER refreshConnections() has routed all bridges. Extracts chipKY,
// net index, and path hops from the now-routed paths. Registers TDM channels
// for inputs and disconnects their paths for TDM isolation.

void finalizeFakeGpioAfterRouting() {
    if (!initialized) return;

    extern std::vector<FakeGpioRestorationInfo> pendingFakeGpioRestorations;

    // --- Finalize OUTPUTS: extract chipKY, net, enable fast path ---
    for (int slot = 0; slot < MAX_FAKE_GP_OUT; slot++) {
        FakeGpioOutput& out = fakeGpioOutputs[slot];
        if (!out.active || out.chipKY >= 0) continue;  // Already finalized or inactive

        out.chipKY = findChipKYForNode(out.userNode);
        out.netIndex = findNetForNode(out.userNode);
        out.fastPathReady = (out.chipKY >= 0 && out.highVoltageX >= 0 && out.lowVoltageX >= 0);
        updateFakeGpioOutputDisplay(slot);

        if (debugFakeGpio) {
            Serial.print("Finalized output slot=");
            Serial.print(slot);
            Serial.print(" node=");
            Serial.print(out.userNode);
            Serial.print(" chipKY=");
            Serial.print(out.chipKY);
            Serial.print(" fastPath=");
            Serial.println(out.fastPathReady);
        }
    }

    // --- Finalize INPUTS: assign ADC, register TDM, extract hops, disconnect ---
    bool anyInputs = false;
    for (int slot = 0; slot < MAX_FAKE_GP_IN; slot++) {
        if (fakeGpioInputs[slot].active) { anyInputs = true; break; }
    }

    if (anyInputs) {
        // Assign ADC if not yet assigned
        if (tdmInputs.adcChannel < 0) {
            if (tdmInputs.assignFreeAdc() < 0) {
                Serial.println("WARNING: finalizeFakeGpioAfterRouting: no free ADC");
            }
            syncAliases();
        }

        for (int slot = 0; slot < MAX_FAKE_GP_IN; slot++) {
            FakeGpioInput& in = fakeGpioInputs[slot];
            if (!in.active || in.tdmSlot >= 0) continue;  // Already finalized

            int8_t chipKY = findChipKYForNode(in.userNode);
            in.netIndex = findNetForNode(in.userNode);

            // Extract path hops for TDM full-path switching
            int8_t hopChips[TDM_MAX_HOPS], hopX[TDM_MAX_HOPS], hopY[TDM_MAX_HOPS];
            int8_t numHops = extractPathHopsForNode(in.userNode, hopChips, hopX, hopY);

            // Register with TDM
            int tdmSlot = tdmInputs.addChannel(in.userNode, chipKY, in.netIndex,
                                                hopChips, hopX, hopY, numHops);
            in.tdmSlot = tdmSlot;

            // Disconnect the entire path for TDM isolation
            if (chipKY >= 0) {
                int8_t adcX = tdmInputs.getChipKX();
                sendXYrawUnchecked(CHIP_K, adcX, chipKY, 0);
                lastChipXY[CHIP_K].connected[chipKY] &= ~(1 << adcX);
            }
            for (int h = 0; h < numHops; h++) {
                if (hopChips[h] >= 0 && hopX[h] >= 0 && hopY[h] >= 0) {
                    sendXYrawUnchecked(hopChips[h], hopX[h], hopY[h], 0);
                    lastChipXY[hopChips[h]].connected[hopY[h]] &= ~(1 << hopX[h]);
                }
            }

            updateFakeGpioInputDisplay(slot);

            if (debugFakeGpio) {
                Serial.print("Finalized input slot=");
                Serial.print(slot);
                Serial.print(" node=");
                Serial.print(in.userNode);
                Serial.print(" chipKY=");
                Serial.print(chipKY);
                Serial.print(" tdmSlot=");
                Serial.print(tdmSlot);
                Serial.print(" hops=");
                Serial.println(numHops);
            }
        }

        syncAliases();
    }

    // Now safe to clear pending restorations
    pendingFakeGpioRestorations.clear();
}

// ============================================================================
// Helper Functions
// ============================================================================

int findFakeGpioOutputSlot(int userNode) {
    for (int i = 0; i < MAX_FAKE_GP_OUT; i++) {
        if (fakeGpioOutputs[i].active && fakeGpioOutputs[i].userNode == userNode) return i;
    }
    return -1;
}

int findFakeGpioInputSlot(int userNode) {
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        if (fakeGpioInputs[i].active && fakeGpioInputs[i].userNode == userNode) return i;
    }
    return -1;
}

int findFreeFakeGpioOutputSlot() {
    for (int i = 0; i < MAX_FAKE_GP_OUT; i++) {
        if (!fakeGpioOutputs[i].active) return i;
    }
    return -1;
}

int findFreeFakeGpioInputSlot() {
    for (int i = 0; i < MAX_FAKE_GP_IN; i++) {
        if (!fakeGpioInputs[i].active) return i;
    }
    return -1;
}

int fakeGpioOutputToGpioIndex(int slot) { return GPIO_INDEX_FAKE_OUT(slot); }
int fakeGpioInputToGpioIndex(int slot)  { return GPIO_INDEX_FAKE_IN(slot); }

bool getDebugFakeGpio() { return debugFakeGpio; }
void setDebugFakeGpio(bool value) { debugFakeGpio = value; }

// Legacy helpers (still used by States.cpp, Debugs.cpp)
int fakeGpioSlotToAnimationIndex(int slot) { return 10 + slot; }
int fakeGpioSlotToNode(int slot) { return FAKE_GP_OUT_0 + slot; }
bool isFakeGpioVirtualNode(int node) { return IS_FAKE_GP_OUT(node) || IS_FAKE_GP_IN(node); }
int fakeGpioNodeToSlot(int node) {
    if (IS_FAKE_GP_OUT(node)) return FAKE_GP_OUT_SLOT(node);
    if (IS_FAKE_GP_IN(node))  return FAKE_GP_IN_SLOT(node);
    return -1;
}

// ============================================================================
// Legacy Compatibility Wrappers
// ============================================================================

int fakeGpioConfigOutput(int node, float v_high, float v_low,
                         float threshold_high, float threshold_low) {
    int slot = fakeGpioConfigOutputVoltage(node, v_high, v_low, threshold_high, threshold_low);
    return (slot >= 0) ? 1 : 0;
}

int fakeGpioConfigOutputNodes(int node, int high_node, int low_node,
                              float threshold_high, float threshold_low) {
    int slot = fakeGpioConfigOutput(node, high_node, low_node, threshold_high, threshold_low);
    return (slot >= 0) ? 1 : 0;
}

int fakeGpioConfig(int node, float v_high, float v_low,
                   float threshold_high, float threshold_low, int mode) {
    if (mode == -1) {
        mode = (v_high == 0.0f && v_low == 0.0f) ? FAKE_GPIO_INPUT : FAKE_GPIO_OUTPUT;
    }
    if (mode == FAKE_GPIO_INPUT) {
        return fakeGpioConfigInput(node, threshold_high, threshold_low);
    } else {
        Serial.println("fake GPIO output disabled (until I fix it)");
        Serial.flush();
        return -1;
        return fakeGpioConfigOutput(node, v_high, v_low, threshold_high, threshold_low);
    }
}

// ============================================================================
// Fast Toggle Functions
// ============================================================================

struct FakeGpioToggle {
    bool active;
    int node1, node2;
    int chips[4];
    int x[6], y[6];
    int num_connections;
};

static FakeGpioToggle fakeGpioToggles[16];
static bool fakeGpioTogglesInitialized = false;

static void initFakeGpioToggles() {
    if (fakeGpioTogglesInitialized) return;
    for (int i = 0; i < 16; i++) {
        fakeGpioToggles[i].active = false;
        fakeGpioToggles[i].node1 = -1;
        fakeGpioToggles[i].node2 = -1;
        fakeGpioToggles[i].num_connections = 0;
    }
    fakeGpioTogglesInitialized = true;
}

static int findToggleSlot(int node1, int node2) {
    for (int i = 0; i < 16; i++) {
        if (fakeGpioToggles[i].active &&
            ((fakeGpioToggles[i].node1 == node1 && fakeGpioToggles[i].node2 == node2) ||
             (fakeGpioToggles[i].node1 == node2 && fakeGpioToggles[i].node2 == node1)))
            return i;
    }
    return -1;
}

static int findFreeToggleSlot() {
    for (int i = 0; i < 16; i++) {
        if (!fakeGpioToggles[i].active) return i;
    }
    return -1;
}

int fakeGpioDisconnect(int node1, int node2) {
    return -1;
    Serial.println("fakeGpioDisconnect");
    Serial.print("node1: ");
    Serial.println(node1);
    Serial.print("node2: ");
    Serial.println(node2);
    Serial.println("--------------------------------");
    Serial.flush();
    initFakeGpioToggles();
    if (findToggleSlot(node1, node2) != -1) return 0;

    extern void bridgesToPaths(int fillUnused, int allowStacking, int startIndex);
    bridgesToPaths(1, 0, 0);

    int pathIdx = -1;
    for (int i = 0; i < globalState.connections.numPaths; i++) {
        const pathStruct& path = globalState.connections.paths[i];
        if ((path.node1 == node1 && path.node2 == node2) ||
            (path.node1 == node2 && path.node2 == node1)) {
            pathIdx = i;
            break;
        }
    }
    if (pathIdx == -1) return 0;

    int slot = findFreeToggleSlot();
    if (slot == -1) return 0;

    const pathStruct& path = globalState.connections.paths[pathIdx];
    FakeGpioToggle& toggle = fakeGpioToggles[slot];
    toggle.active = true;
    toggle.node1 = node1;
    toggle.node2 = node2;
    toggle.num_connections = 0;

    for (int i = 0; i < 4; i++) {
        toggle.chips[i] = path.chip[i];
        if (path.chip[i] >= 0) toggle.num_connections++;
    }
    for (int i = 0; i < 6; i++) {
        toggle.x[i] = path.x[i];
        toggle.y[i] = path.y[i];
    }

    for (int i = 0; i < 4 && i < toggle.num_connections; i++) {
        if (toggle.chips[i] >= 0 && toggle.x[i] >= 0 && toggle.y[i] >= 0)
            sendXYrawUnchecked(toggle.chips[i], toggle.x[i], toggle.y[i], 0);
    }
    return 1;
}

int fakeGpioReconnect(int node1, int node2) {
    return -1;
    Serial.println("fakeGpioReconnect");
    Serial.print("node1: ");
    Serial.println(node1);
    Serial.print("node2: ");
    Serial.println(node2);
    Serial.println("--------------------------------");
    Serial.flush();
    initFakeGpioToggles();
    int slot = findToggleSlot(node1, node2);
    if (slot == -1) return 0;

    FakeGpioToggle& toggle = fakeGpioToggles[slot];
    for (int i = 0; i < 4 && i < toggle.num_connections; i++) {
        if (toggle.chips[i] >= 0 && toggle.x[i] >= 0 && toggle.y[i] >= 0)
            sendXYrawUnchecked(toggle.chips[i], toggle.x[i], toggle.y[i], 1);
    }
    toggle.active = false;
    toggle.node1 = -1;
    toggle.node2 = -1;
    toggle.num_connections = 0;
    return 1;
}
