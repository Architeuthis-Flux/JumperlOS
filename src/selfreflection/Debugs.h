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
void action_i2cScan();
void action_speedTest();
void action_colorSpectrum();
















#endif