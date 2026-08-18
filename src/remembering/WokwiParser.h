// SPDX-License-Identifier: MIT
#ifndef WOKWI_PARSER_H
#define WOKWI_PARSER_H

#include <Arduino.h>
#include "States.h"

/**
 * @brief Parse a Wokwi diagram.json file and convert to Jumperless YAML state
 * 
 * Supported features:
 * - Breadboard connections (bb1:XYt/b.Z format)
 * - Logic analyzer mapping (D0-7 → GPIO 1-8)
 * - VCC/GND rail voltage detection
 * - Text label parsing for rail voltages (e.g., "top rail 3.3V")
 * - Wire color preservation
 * - Arduino Nano pin mappings
 * 
 * @param jsonContent The raw JSON content from diagram.json
 * @param outState Output JumperlessState to populate
 * @param slotNum Target slot number (for display)
 * @param errorMsg Output error message if parsing fails
 * @param quietMode If true, suppress non-error output (for app usage)
 * @return true if parsing succeeded, false otherwise
 */
bool parseWokwiDiagram(const String& jsonContent, JumperlessState& outState, 
                       int slotNum, String& errorMsg, bool quietMode = false);

/**
 * @brief Parse a Wokwi diagram from a file and save to a slot
 * 
 * @param filename Path to diagram.json file (e.g., "/diagram.json")
 * @param slotNum Slot number to save to
 * @param errorMsg Output error message if operation fails
 * @return true if successful, false otherwise
 */
bool parseWokwiDiagramFromFile(const String& filename, int slotNum, String& errorMsg);

/**
 * @brief Parse Wokwi diagram directly to YAML file without creating JumperlessState
 * 
 * This is the most memory-efficient way to save a diagram to an inactive slot.
 * It parses the JSON and writes YAML directly to the file without ever creating
 * a full JumperlessState object (which is ~50KB). Only stores bridges and power
 * settings - nets and paths will be computed when the slot is loaded and made active.
 * 
 * @param jsonContent Raw JSON content from diagram.json
 * @param slotNum Target slot number to save to
 * @param errorMsg Output error message if operation fails
 * @param quietMode If true, suppress non-error output (for app usage)
 * @return true if successful, false otherwise
 */
bool parseWokwiDiagramDirectToFile(const String& jsonContent, int slotNum, 
                                    String& errorMsg, bool quietMode = false);

/**
 * @brief Convert Wokwi breadboard pin notation to Jumperless node number
 * 
 * Examples:
 * - "bb1:8t.c" → row 8, top section, column c
 * - "bb1:21b.h" → row 21, bottom section, column h
 * 
 * @param pinStr Wokwi pin string (e.g., "bb1:8t.c")
 * @return Jumperless node number (1-60 for breadboard), or -1 if invalid
 */
int wokwiPinToJumperlessNode(const String& pinStr);

/**
 * @brief Convert Arduino Nano pin name to Jumperless node number
 * 
 * Examples:
 * - "nano:13" → GPIO 13
 * - "nano:A0" → Analog 0
 * - "nano:GND" → Ground
 * 
 * @param pinStr Wokwi Arduino pin string (e.g., "nano:13")
 * @return Jumperless node number, or -1 if invalid
 */
int arduinoPinToJumperlessNode(const String& pinStr);

/**
 * @brief Convert logic analyzer pin to GPIO number
 * 
 * Logic analyzer D0-7 maps to GPIO 1-8
 * 
 * @param pinStr Logic analyzer pin string (e.g., "logic1:D0")
 * @return GPIO node number (1-8), or -1 if invalid
 */
int logicAnalyzerPinToGPIO(const String& pinStr);

/**
 * @brief Parse voltage from a string (e.g., "3.3V", "5V", "2.5V")
 * 
 * @param voltageStr String containing voltage (e.g., "3.3V")
 * @return Voltage in millivolts (e.g., 3300 for "3.3V"), or -1 if invalid
 */
int parseVoltageString(const String& voltageStr);

// Note: Color conversion functions (wokwiColorToRGB, wokwiColorToInternalName, shiftColorHue)
// are now in Colors.h for centralized color management

#endif // WOKWI_PARSER_H

