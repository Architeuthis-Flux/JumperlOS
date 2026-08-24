#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>

bool debugFlagsMenu();

// Interactive status & diagnostics menu with arrow key navigation
bool statusDiagnosticsMenu();

void action_psramTest();

void action_resourceStatus();
void action_gpioState();
void action_netlist();
void action_bridgeArray();
void action_crossbar();
void action_pioStatus();
void action_memoryUsage();
void action_memoryMap();

// ── Boot heap ledger ────────────────────────────────────────────────────────
//
// "Used Heap: 153196" tells you there is a problem and nothing about where it
// came from. heapMark() records free-heap after an init stage into a static
// table (no allocation, and it works long before USB enumerates, which is why
// it records instead of printing); heapLedgerPrint() renders the deltas
// largest-consumer-first once there is a terminal to read them. The point is
// that a resident-usage regression becomes attributable to a stage instead of
// a suspect list.
void heapMark(const char* label);
void heapLedgerPrint();
void action_i2cScan();
void action_speedTest();
void action_colorSpectrum();
















#endif