#pragma once
#include <Arduino.h>
#include "JumperlessDefines.h"
extern int8_t nodeToNetIndex[256];
extern int newBridgeLength;

int printNodeOrName(int node, int longOrShort = 0, int netIndex = -1, Stream *stream = &Serial);
void printBridgeArray(Stream *stream = &Serial);
const char* definesToChar(int defined, int longOrShort = 0);
bool connectionAllowed(int, int);
void assignTermColor(int startIndex = 0);
