// SPDX-License-Identifier: MIT
#ifndef FAKEGPIO_H
#define FAKEGPIO_H

#include "JumperlessDefines.h"
#include "TimeDomainMultiplexer.h"

// ============================================================================
// Constants
// ============================================================================
#define FAKE_GPIO_INPUT  0
#define FAKE_GPIO_OUTPUT 1

// ============================================================================
// Fake GPIO Output
// ============================================================================
// Uses virtual node expansion: bridge stays as userNode-FAKE_GP_OUT_x.
// Router expands FAKE_GP_OUT_x to actual voltage source based on currentState.
// Fast path: switch chip K X between highVoltageX and lowVoltageX (~50us).

struct FakeGpioOutput {
    bool active;
    int userNode;               // Breadboard node (e.g., 10)
    int highVoltageNode;        // TOP_RAIL, DAC0, DAC1, BOTTOM_RAIL, or GND
    int lowVoltageNode;
    int currentState;           // 0=LOW, 1=HIGH
    int netIndex;               // Net index for LED display
    float thresholdHigh;        // Readback threshold (API compat)
    float thresholdLow;
    int8_t chipKY;              // Chip K Y position
    int8_t highVoltageX;        // Chip K X for HIGH voltage source
    int8_t lowVoltageX;         // Chip K X for LOW voltage source
    bool fastPathReady;         // True when chip K coords are cached
};

// ============================================================================
// Fake GPIO Input
// ============================================================================
// Thin wrapper over a TDM channel. The TDM handles switching and raw voltage
// reads; this struct adds digital thresholds and state tracking.

struct FakeGpioInput {
    bool active;
    int userNode;               // Breadboard node
    int tdmSlot;                // Index into tdmInputs.channels[]
    float thresholdHigh;        // Voltage above which = HIGH
    float thresholdLow;         // Voltage below which = LOW
    int currentState;           // -1=unknown, 0=LOW, 1=HIGH
    int netIndex;               // Net index for LED display
};

// ============================================================================
// Global Storage
// ============================================================================

extern FakeGpioOutput fakeGpioOutputs[MAX_FAKE_GP_OUT];
extern FakeGpioInput fakeGpioInputs[MAX_FAKE_GP_IN];
extern TimeDomainMultiplexer tdmInputs;  // TDM instance for all fake GPIO inputs
extern int fakeGpioInputAdcChannel;      // Alias for tdmInputs.adcChannel (extern compat)
extern int fakeGpioCurrentlyConnectedInput;  // Alias for tdmInputs.activeChannel
extern bool debugFakeGpio;

// ============================================================================
// Initialization and State Management
// ============================================================================

void initFakeGpio();
void clearAllFakeGpio();  // Deactivate all fake GPIO inputs/outputs
void initializeFakeGpioFromLoadedState();  // Phase 1: pre-routing (populate slot structs)
void finalizeFakeGpioAfterRouting();       // Phase 2: post-routing (extract paths, register TDM)
void readFakeGPIO(void);

// ============================================================================
// Configuration Functions
// ============================================================================

// OUTPUT mode (node-based, preferred)
int fakeGpioConfigOutput(int node, int highNode, int lowNode,
                         float thresholdHigh = 2.0f, float thresholdLow = 0.8f);
// OUTPUT mode (voltage-based, auto-selects sources)
int fakeGpioConfigOutputVoltage(int node, float vHigh, float vLow,
                                float thresholdHigh = 2.0f, float thresholdLow = 0.8f);
// INPUT mode
int fakeGpioConfigInput(int node, float thresholdHigh = 2.0f, float thresholdLow = 0.8f);

// Remove configurations
int fakeGpioRemoveOutput(int node);
int fakeGpioRemoveInput(int node);

// ============================================================================
// Read/Write Functions
// ============================================================================

int fakeGpioWrite(int node, int state);
int fakeGpioWriteBatch(const int* nodes, const int* states, int count);
int fakeGpioRead(int node);
int fakeGpioReadOutput(int node);

// ============================================================================
// Helper Functions
// ============================================================================

int findFakeGpioOutputSlot(int userNode);
int findFakeGpioInputSlot(int userNode);
int findFreeFakeGpioOutputSlot();
int findFreeFakeGpioInputSlot();

int fakeGpioOutputToGpioIndex(int slot);
int fakeGpioInputToGpioIndex(int slot);

inline bool isFakeGpioOutputNode(int node) { return IS_FAKE_GP_OUT(node); }
inline bool isFakeGpioInputNode(int node)  { return IS_FAKE_GP_IN(node); }

void updateFakeGpioOutputDisplay(int slot);
void updateFakeGpioInputDisplay(int slot);

// Debug
bool getDebugFakeGpio();
void setDebugFakeGpio(bool value);

// Connection change hook (called from FileParsing)
void updateFakeGpioAfterConnectionChange(int node1, int node2);

// Fast toggle (quick connect/disconnect without full routing)
int fakeGpioDisconnect(int node1, int node2);
int fakeGpioReconnect(int node1, int node2);

int fakeGpioRemoveInput(int node);

// ============================================================================
// Legacy Compatibility Wrappers
// ============================================================================

// Unified config (auto-detects mode from v_high/v_low)
int fakeGpioConfig(int node, float v_high, float v_low,
                   float threshold_high, float threshold_low, int mode = -1);

// Old node-based output config (returns 1/0 instead of slot/-1)
int fakeGpioConfigOutputNodes(int node, int high_node, int low_node,
                              float threshold_high, float threshold_low);

// Old voltage-based output config (returns 1/0)
int fakeGpioConfigOutput(int node, float v_high, float v_low,
                         float threshold_high, float threshold_low);

// Legacy helper functions used by States.cpp serialization
int fakeGpioSlotToAnimationIndex(int slot);
int fakeGpioSlotToNode(int slot);
bool isFakeGpioVirtualNode(int node);
int fakeGpioNodeToSlot(int node);

// Legacy struct kept for States.cpp deserialization compatibility
// (pendingFakeGpioRestorations still uses FakeGpioRestorationInfo which
//  references slot indices in the old scheme; we remap during restore)
#define MAX_FAKE_GPIO 32

#endif // FAKEGPIO_H
