// SPDX-License-Identifier: MIT
#ifndef NETVOLTAGESCAN_H
#define NETVOLTAGESCAN_H

#include <Arduino.h>
#include "JumperlessDefines.h"

// ============================================================================
// Net Voltage Scan - board-wide passive voltage / current sensing
// ============================================================================
//
// Round-robins a free ADC (0-3) across every node of every routed net using
// momentary raw crosspoint taps (no bridge refreshes), keeping a per-node
// voltage field fresh. Because every routed path goes through a known number
// of CH446Q crosspoints, the current through each connection falls out of
// Ohm's law on the voltage difference between its two end nodes:
//
//     I = (V(node1) - V(node2)) / (crosspoints * crosspoint_resistance)
//
// The ADC tap is high impedance (~1M divider), so it neither disturbs the
// circuit nor drops any voltage across its own sense path.
//
// Runs on core 2 (call serviceNetVoltageScan() next to readFakeGPIO()), in
// the same loop that processes sendAllPathsCore2, so taps can't race a
// full crossbar refresh. Enabled by jumperlessConfig.display.net_currents.

// The OG has a different fabric and lines-only rendering; the scanner is
// V5-only, so its arrays shrink to single dummy slots there to save SRAM.
#if defined(OG_JUMPERLESS)
#define NODE_VOLTAGE_MAX 1
#define NET_CURRENT_INFO_COUNT 1
#else
#define NODE_VOLTAGE_MAX 141 // real nodes are 1..140 (ROUTABLE_BUFFER_OUT)
#define NET_CURRENT_INFO_COUNT MAX_NETS
#endif

struct NetCurrentInfo {
    bool valid = false;
    float current_mA = 0.0f; // magnitude of the largest-|I| path in this net
    float voltage = 0.0f;    // midpoint voltage of that path (for coloring)
    int fromNode = -1;       // conventional current flows fromNode -> toNode
    int toNode = -1;
};

extern float nodeVoltage[NODE_VOLTAGE_MAX];
extern uint32_t nodeVoltageMs[NODE_VOLTAGE_MAX]; // millis() of last sample, 0 = never
extern NetCurrentInfo netCurrentInfo[NET_CURRENT_INFO_COUNT];

bool nodeVoltageValid(int node);

// Current magnitude flowing in a net's dominant path, 0.0 when unknown.
// Safe to call from core 0 (values are written on core 2; a torn float read
// is cosmetic only - these feed displays, not control loops).
float netCurrent_mA(int netIndex);

// Signed current through one live routing path (same index space as
// globalState.connections.paths). Positive = conventional current flowing
// node1 -> node2. 0.0 / false when unknown. Same torn-read caveat as above.
bool pathCurrentKnown(int pathIndex);
float pathCurrentSigned_mA(int pathIndex);

// Call frequently from the core 2 loop. Self-throttled.
void serviceNetVoltageScan(void);

// Call from the CORE 0 loop. Prints the debug.net_voltage_scan report once a
// second (Serial I/O must stay off core 1/2 - dual-core USB hazard).
void serviceNetVoltageScanDebug(void);

// One-shot report of scan nodes, sense routes, and per-path currents (the
// same output debug.net_voltage_scan prints once a second).
void printNetVoltageScanStats(Stream* out = &Serial);

#endif // NETVOLTAGESCAN_H
