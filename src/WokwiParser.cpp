// SPDX-License-Identifier: MIT
#include "WokwiParser.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"  // For safe file operations
#include "JumperlessDefines.h"
#include "USBfs.h"
#include "FatFS.h"
#include "Colors.h"
#include "Probing.h"

extern bool debugFP;

// Simple JSON helper functions - no external library needed
int findNextChar(const String& str, char searchChar, int startIdx) {
    for (int i = startIdx; i < str.length(); i++) {
        if (str[i] == searchChar) return i;
    }
    return -1;
}

String extractJsonString(const String& json, const String& key, int startIdx) {
    // Find "key": "value" pattern
    String searchPattern = "\"" + key + "\"";
    int keyIdx = json.indexOf(searchPattern, startIdx);
    if (keyIdx == -1) return "";
    
    // Find the colon after the key
    int colonIdx = json.indexOf(':', keyIdx + searchPattern.length());
    if (colonIdx == -1) return "";
    
    // Skip whitespace after colon
    int valueIdx = colonIdx + 1;
    while (valueIdx < json.length() && (json[valueIdx] == ' ' || json[valueIdx] == '\t' || json[valueIdx] == '\n' || json[valueIdx] == '\r')) {
        valueIdx++;
    }
    
    // Check if it's a string (starts with quote)
    if (valueIdx >= json.length()) return "";
    
    if (json[valueIdx] == '"') {
        // Extract string value
        int startQuote = valueIdx;
        int endQuote = json.indexOf('"', startQuote + 1);
        if (endQuote == -1) return "";
        
        String result = json.substring(startQuote + 1, endQuote);
        
        // Unescape JSON escape sequences (critical for handling \n in multi-line text!)
        // This is essential for correctly parsing multi-line labels like:
        // "text": "top rail 4.3V\nbottom rail -6.5V"
        // The \n needs to be converted from two characters (backslash, n) to one newline
        String unescapedStr;
        unescapedStr.reserve(result.length()); // Pre-allocate for efficiency
        
        for (unsigned int i = 0; i < result.length(); i++) {
            if (result[i] == '\\' && i + 1 < result.length()) {
                char nextChar = result[i + 1];
                switch (nextChar) {
                    case 'n':
                        unescapedStr += '\n';
                        i++; // Skip next char
                        break;
                    case 'r':
                        unescapedStr += '\r';
                        i++; // Skip next char
                        break;
                    case 't':
                        unescapedStr += '\t';
                        i++; // Skip next char
                        break;
                    case '\\':
                        unescapedStr += '\\';
                        i++; // Skip next char
                        break;
                    case '"':
                        unescapedStr += '"';
                        i++; // Skip next char
                        break;
                    default:
                        // Unknown escape, keep the backslash
                        unescapedStr += result[i];
                        break;
                }
            } else {
                unescapedStr += result[i];
            }
        }
        
        return unescapedStr;
    }
    
    return "";
}

// Extract array from JSON
int findJsonArray(const String& json, const String& arrayName, int& startIdx, int& endIdx) {
    String searchPattern = "\"" + arrayName + "\"";
    int keyIdx = json.indexOf(searchPattern);
    if (keyIdx == -1) return -1;
    
    // Find the colon
    int colonIdx = json.indexOf(':', keyIdx);
    if (colonIdx == -1) return -1;
    
    // Find the opening bracket
    startIdx = json.indexOf('[', colonIdx);
    if (startIdx == -1) return -1;
    
    // Find matching closing bracket (count nested brackets)
    int bracketCount = 1;
    endIdx = startIdx + 1;
    while (endIdx < json.length() && bracketCount > 0) {
        if (json[endIdx] == '[') bracketCount++;
        else if (json[endIdx] == ']') bracketCount--;
        endIdx++;
    }
    
    if (bracketCount != 0) return -1;
    
    return 0;
}

// Breadboard mapping: Jumperless uses rows 1-30, columns a-j
// Top section (t): rows 1-30, columns a-e map to nodes 1-30 (column a) through ? 
// Bottom section (b): rows 1-30, columns f-j
// This is a simplified mapping - adjust based on actual Jumperless hardware
int wokwiPinToJumperlessNode(const String& pinStr) {
    // Format: "bb1:8t.c" or "bb1:21b.h" or "bb1:tp.2" (top rail) or "bb1:bp.3" (bottom rail)
    // bb1 = breadboard 1
    // 8 = row number (or 't'/'b' for rails)
    // t/b = top or bottom section (or 'p' for power rail)
    // c/h = column letter
    
    int colonIdx = pinStr.indexOf(':');
    if (colonIdx == -1) return -1;
    
    String pinPart = pinStr.substring(colonIdx + 1);
    
    // Handle special cases for power rails
    if (pinPart.startsWith("tp") || pinPart.startsWith("VCC")) {
        // Top power rail (positive)
        return TOP_RAIL; // Node 101
    }
    if (pinPart.startsWith("bp")) {
        // Bottom power rail (positive)
        return BOTTOM_RAIL; // Node 102
    }
    if (pinPart.startsWith("tn")) {
        // Top negative rail (ground)
        return GND; // Node 100
    }
    if (pinPart.startsWith("bn")) {
        // Bottom negative rail (ground)
        return GND; // Node 100
    }
    
    // Extract row number
    int row = 0;
    int i = 0;
    while (i < pinPart.length() && isDigit(pinPart[i])) {
        row = row * 10 + (pinPart[i] - '0');
        i++;
    }
    
    if (row < 1 || row > 30) return -1;
    
    // Extract section (t/b)
    char section = pinPart[i];
    if (section != 't' && section != 'b') return -1;
    i++;
    
    // Skip the dot and column letter - in Jumperless, all columns in a row are connected!
    // Column letters (a-e, f-j) are just physical positions but share the same electrical node
    
    // Map to Jumperless node numbers
    // Jumperless breadboard: nodes 1-60
    // TOP section (rows 1-30): nodes 1-30 (one node per row, all columns connected)
    // BOTTOM section (rows 1-30): nodes 31-60 (one node per row, all columns connected)
    
    if (section == 't') {
        // Top section: row N → node N
        // TOP_1 = 1, TOP_2 = 2, ..., TOP_30 = 30
        return row;
    } else {
        // Bottom section: row N → node (30 + N)
        // BOTTOM_1 = 31, BOTTOM_2 = 32, ..., BOTTOM_30 = 60
        return 30 + row;
    }
}

int arduinoPinToJumperlessNode(const String& pinStr) {
    // Format: "nano:13" or "nano:A0" or "nano:GND"
    int colonIdx = pinStr.indexOf(':');
    if (colonIdx == -1) return -1;
    
    String pin = pinStr.substring(colonIdx + 1);
    pin.trim();
    
    // Handle special pins
    if (pin == "GND" || pin == "gnd") {
        return GND; // Ground node
    }
    if (pin == "5V" || pin == "VCC" || pin == "vcc") {
        return TOP_RAIL; // 5V supply
    }
    if (pin == "3V3" || pin == "3.3V") {
        return BOTTOM_RAIL; // 3.3V supply
    }
    
    // Handle analog pins (A0-A7)
    if (pin[0] == 'A' || pin[0] == 'a') {
        int analogNum = pin.substring(1).toInt();
        if (analogNum >= 0 && analogNum <= 7) {
            return NANO_A0 + analogNum; // NANO_A0 through NANO_A7
        }
        return -1;
    }
    
    // Handle digital pins (0-13)
    int digitalNum = pin.toInt();
    if (digitalNum >= 0 && digitalNum <= 13) {
        return NANO_D0 + digitalNum; // NANO_D0 through NANO_D13
    }
    
    return -1;
}

int logicAnalyzerPinToGPIO(const String& pinStr) {
    // Format: "logic1:D0" through "logic1:D7"
    // Maps to RP_GPIO_1 through RP_GPIO_8 (nodes 131-138)
    int colonIdx = pinStr.indexOf(':');
    if (colonIdx == -1) return -1;
    
    String pin = pinStr.substring(colonIdx + 1);
    pin.trim();
    
    if (pin.length() >= 2 && (pin[0] == 'D' || pin[0] == 'd')) {
        int channelNum = pin.substring(1).toInt();
        if (channelNum >= 0 && channelNum <= 7) {
            // D0→RP_GPIO_1 (131), D1→RP_GPIO_2 (132), ..., D7→RP_GPIO_8 (138)
            return RP_GPIO_1 + channelNum;
        }
    }
    
    return -1;
}

int parseVoltageString(const String& voltageStr) {
    // Parse voltage strings in various formats:
    // "3.3V", "-5V", "5.0V", "=3.3V", "= -5V", "3V3", "3300mV", "3.3", "4,5V" (European comma)
    // Supports range: -8V to +8V
    // Returns voltage in millivolts, or -9999 if parsing failed
    
    String str = voltageStr;
    str.trim();
    
    // Stop at newline or other common delimiters to avoid parsing multiple values
    // This is CRITICAL to prevent digit contamination from multi-line text like:
    // "top rail 4.3V\nbottom rail -6.5V" where we must stop at the \n
    int newlineIdx = str.indexOf('\n');
    int carriageReturnIdx = str.indexOf('\r');
    int commaIdx = str.indexOf(',', 1); // Skip first char in case of European decimal comma
    int spaceIdx = str.indexOf("  "); // Double space often indicates separate values
    
    // Take substring up to first delimiter found (excluding European comma decimal)
    // Use >= 0 not > 0 because indexOf returns -1 if not found, and we want to catch position 0 too
    int stopIdx = str.length();
    if (newlineIdx >= 0 && newlineIdx < stopIdx) stopIdx = newlineIdx;
    if (carriageReturnIdx >= 0 && carriageReturnIdx < stopIdx) stopIdx = carriageReturnIdx;
    // Only use comma as delimiter if it's not in position 1-2 (where it would be a decimal)
    if (commaIdx > 2 && commaIdx < stopIdx) stopIdx = commaIdx;
    if (spaceIdx >= 0 && spaceIdx < stopIdx) stopIdx = spaceIdx;
    
    if (stopIdx < str.length()) {
        str = str.substring(0, stopIdx);
        str.trim();
    }
    
    str.toUpperCase();
    
    // Remove leading '=' and any whitespace after it
    while (str.length() > 0 && (str[0] == '=' || str[0] == ' ')) {
        str = str.substring(1);
        str.trim();
    }
    
    // Check if this is in millivolts format (e.g., "3300mV")
    bool isMillivolts = false;
    if (str.endsWith("MV")) {
        isMillivolts = true;
        str = str.substring(0, str.length() - 2);
        str.trim();
    } else if (str.endsWith("V")) {
        // Remove 'V' suffix
        str = str.substring(0, str.length() - 1);
        str.trim();
    }
    
    // Handle European notation like "3V3" → "3.3"
    // Look for pattern like "3V3" or "-5V2" where V is in the middle
    int vIndex = str.indexOf('V');
    if (vIndex > 0 && vIndex < str.length() - 1) {
        // Check if there are digits before and after the V
        bool digitBefore = (vIndex > 0 && isdigit(str[vIndex - 1]));
        bool digitAfter = (vIndex < str.length() - 1 && isdigit(str[vIndex + 1]));
        
        if (digitBefore && digitAfter) {
            // Replace the 'V' with a decimal point
            String beforeV = str.substring(0, vIndex);
            String afterV = str.substring(vIndex + 1);
            str = beforeV + "." + afterV;
        }
    }
    
    // Replace European comma with period for decimal point (e.g., "4,5" → "4.5")
    // Only replace the first comma that's between digits
    for (unsigned int i = 1; i < str.length() - 1; i++) {
        if (str[i] == ',' && isdigit(str[i-1]) && isdigit(str[i+1])) {
            str.setCharAt(i, '.');
            break; // Only replace the first one
        }
    }
    
    // Clean up any remaining non-numeric characters except '-' at start and '.'
    String cleaned = "";
    bool hasDecimal = false;
    bool hasNegative = false;
    
    for (unsigned int i = 0; i < str.length(); i++) {
        char c = str[i];
        
        // Allow negative sign only at the start
        if (c == '-' && i == 0) {
            hasNegative = true;
            cleaned += c;
        }
        // Allow one decimal point
        else if (c == '.' && !hasDecimal) {
            hasDecimal = true;
            cleaned += c;
        }
        // Allow digits
        else if (isdigit(c)) {
            cleaned += c;
        }
        // Skip other characters (including commas at this point)
    }
    
    // Parse as float
    float voltage = cleaned.toFloat();
    
    // If it was in millivolts, convert to volts
    if (isMillivolts) {
        voltage = voltage / 1000.0;
    }
    
    // Check if parsing was successful (non-zero or explicitly zero)
    if (cleaned.length() == 0 || (voltage == 0.0 && cleaned != "0" && cleaned != "0.0" && cleaned != "-0")) {
        return -9999; // Parse failed - use -9999 as error (outside valid -8V to +8V range)
    }
    
    // Enforce range: -8V to +8V
    if (voltage < -8.0 || voltage > 8.0) {
        return -9999; // Out of acceptable range
    }
    
    // Apply DAC limits from config
    if (voltage < jumperlessConfig.dacs.limit_min) {
        voltage = jumperlessConfig.dacs.limit_min;
    }
    if (voltage > jumperlessConfig.dacs.limit_max) {
        voltage = jumperlessConfig.dacs.limit_max;
    }
    
    // Convert to millivolts and return
    return (int)(voltage * 1000);
}

bool parseWokwiDiagram(const String& jsonContent, JumperlessState& outState, 
                       int slotNum, String& errorMsg, bool quietMode) {
    // Preserve power settings to prevent LED flickering during updates
    // Only clear connections, not power - parser will update voltages if found in diagram
    float savedTopRail = outState.power.topRail;
    float savedBottomRail = outState.power.bottomRail;
    float savedDac0 = outState.power.dac0;
    float savedDac1 = outState.power.dac1;
    
    // Clear only connections, preserve power and other settings
    outState.connections.clear();
    
    // Restore power settings - will be overwritten if diagram specifies new values
    outState.power.topRail = savedTopRail;
    outState.power.bottomRail = savedBottomRail;
    outState.power.dac0 = savedDac0;
    outState.power.dac1 = savedDac1;
    
    if (debugFP) {
        Serial.println("◆ JSON length: " + String(jsonContent.length()) + " bytes");
        Serial.println("◆ First 100 chars: " + jsonContent.substring(0, 100));
        Serial.println("◆ Last 100 chars: " + jsonContent.substring(jsonContent.length() > 100 ? jsonContent.length() - 100 : 0));
    }
    
    // Custom JSON parser - no external library needed
    // We only need to extract connections and text labels from Wokwi diagram format
    
    // Extract rail voltages from text labels
    // Use -9999 as "not found" sentinel (clearly outside valid -8V to +8V range)
    int topRailVoltage = -9999;
    int bottomRailVoltage = -9999;
    
    // Find all text parts and parse voltage info
    int textIdx = 0;
    while ((textIdx = jsonContent.indexOf("\"wokwi-text\"", textIdx)) != -1) {
        // Find the "text": "..." value for this part
        int textKeyIdx = jsonContent.indexOf("\"text\"", textIdx);
        if (textKeyIdx != -1 && textKeyIdx < textIdx + 500) { // Within 500 chars of part type
            String textValue = extractJsonString(jsonContent, "text", textKeyIdx);
            textValue.toLowerCase();
            
            if (debugFP) {
                Serial.println("  Found text: " + textValue);
            }
            
            // Parse rail voltage labels - handle various formats:
            // "top rail 3.3V", "TOP RAIL = 5V", "top_rail: 4.5V", "toprail 3v3"
            // "bottom rail -6.5V", "bot rail 2.5V", "BOTTOM_RAIL = -5V"
            // Work directly with textValue (already lowercased) for consistent indexing
            
            // Replace underscores with spaces for flexible matching
            String workText = textValue;
            workText.replace("_", " ");
            
            // Check for top rail patterns (search in workText, extract from workText)
            int topRailIdx = -1;
            int topVoltStartIdx = -1;
            
            // Try "top rail" or "top  rail" (with underscore removed)
            if ((topRailIdx = workText.indexOf("top rail")) >= 0) {
                topVoltStartIdx = topRailIdx + 8; // After "top rail"
            } else if ((topRailIdx = workText.indexOf("toprail")) >= 0) {
                topVoltStartIdx = topRailIdx + 7; // After "toprail"
            } else if ((topRailIdx = workText.indexOf("top")) >= 0) {
                // "top" found - check if "rail" follows within reasonable distance
                int railIdx = workText.indexOf("rail", topRailIdx + 3);
                if (railIdx >= 0 && railIdx - topRailIdx < 12) {
                    topVoltStartIdx = railIdx + 4; // After "rail"
                }
            }
            
            if (topVoltStartIdx > 0) {
                String voltPart = workText.substring(topVoltStartIdx);
                topRailVoltage = parseVoltageString(voltPart);
                if (debugFP && topRailVoltage != -9999) {
                    Serial.println("  Top rail: " + String(topRailVoltage) + "mV");
                }
            }
            
            // Check for bottom rail patterns
            int bottomRailIdx = -1;
            int bottomVoltStartIdx = -1;
            
            // Try various bottom/bot patterns
            if ((bottomRailIdx = workText.indexOf("bottom rail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 11; // After "bottom rail"
            } else if ((bottomRailIdx = workText.indexOf("bot rail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 8; // After "bot rail"
            } else if ((bottomRailIdx = workText.indexOf("bottomrail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 10; // After "bottomrail"
            } else if ((bottomRailIdx = workText.indexOf("botrail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 7; // After "botrail"
            } else if ((bottomRailIdx = workText.indexOf("bottom")) >= 0) {
                // "bottom" found - check if "rail" follows
                int railIdx = workText.indexOf("rail", bottomRailIdx + 6);
                if (railIdx >= 0 && railIdx - bottomRailIdx < 12) {
                    bottomVoltStartIdx = railIdx + 4; // After "rail"
                }
            } else if ((bottomRailIdx = workText.indexOf("bot")) >= 0) {
                // "bot" found - check if "rail" follows
                int railIdx = workText.indexOf("rail", bottomRailIdx + 3);
                if (railIdx >= 0 && railIdx - bottomRailIdx < 12) {
                    bottomVoltStartIdx = railIdx + 4; // After "rail"
                }
            }
            
            if (bottomVoltStartIdx > 0) {
                String voltPart = workText.substring(bottomVoltStartIdx);
                bottomRailVoltage = parseVoltageString(voltPart);
                if (debugFP && bottomRailVoltage != -9999) {
                    Serial.println("  Bottom rail: " + String(bottomRailVoltage) + "mV");
                }
            }
            
            // Parse "VCC = 5V" or "VCC=5V" style
            if (textValue.indexOf("vcc") >= 0) {
                int eqIdx = textValue.indexOf('=');
                if (eqIdx >= 0) {
                    String voltPart = textValue.substring(eqIdx + 1);
                    int voltage = parseVoltageString(voltPart);
                    if (voltage != -9999 && topRailVoltage == -9999) {
                        topRailVoltage = voltage;
                        if (debugFP) {
                            Serial.println("  VCC: " + String(topRailVoltage) + "mV");
                        }
                    }
                }
            }
        }
        textIdx++;
    }
    
    // Parse connections array
    int connectionsStart = 0, connectionsEnd = 0;
    if (findJsonArray(jsonContent, "connections", connectionsStart, connectionsEnd) != 0) {
        errorMsg = "No connections array found in diagram";
        return false;
    }
    
    int connCount = 0;
    int skippedCount = 0;
    
    // Track colors for "all green" detection (Wokwi's default when user doesn't customize)
    int userColorCount = 0;  // Non-GND/VCC bridges with colors
    int greenColorCount = 0; // How many are green
    
    // Parse each connection: [ "source", "target", "color", [...] ]
    int connIdx = connectionsStart + 1; // Skip opening '['
    
    while (connIdx < connectionsEnd) {
        // Find next connection array (starts with '[')
        int connStart = jsonContent.indexOf('[', connIdx);
        if (connStart == -1 || connStart >= connectionsEnd) break;
        
        // Find end of this connection array
        int connEnd = jsonContent.indexOf(']', connStart);
        if (connEnd == -1 || connEnd >= connectionsEnd) break;
        
        // Extract the 4 values: source, target, color, wire placement
        String connStr = jsonContent.substring(connStart + 1, connEnd);
        
        // Parse quoted strings from the connection array
        String source = "", target = "", colorName = "";
        int quoteCount = 0;
        int quoteStart = -1;
        
        for (int i = 0; i < connStr.length(); i++) {
            if (connStr[i] == '"') {
                if (quoteStart == -1) {
                    quoteStart = i;
                } else {
                    // End of quoted string
                    String value = connStr.substring(quoteStart + 1, i);
                    if (quoteCount == 0) source = value;
                    else if (quoteCount == 1) target = value;
                    else if (quoteCount == 2) colorName = value;
                    
                    quoteCount++;
                    quoteStart = -1;
                    
                    if (quoteCount >= 3) break; // Got all we need
                }
            }
        }
        
        if (source.length() == 0 || target.length() == 0) {
            // Couldn't parse the connection strings
            connIdx = connEnd + 1;
            skippedCount++;
            continue;
        }
        
        if (debugFP) {
            Serial.println("  Connection: " + source + " → " + target + " (" + colorName + ")");
        }
        
        // Convert source and target to Jumperless node numbers
        int sourceNode = -1;
        int targetNode = -1;
        
        // Detect component types and map accordingly
        if (source.startsWith("bb") || source.startsWith("BB")) {
            sourceNode = wokwiPinToJumperlessNode(source);
        } else if (source.startsWith("nano") || source.startsWith("NANO")) {
            sourceNode = arduinoPinToJumperlessNode(source);
        } else if (source.startsWith("logic")) {
            sourceNode = logicAnalyzerPinToGPIO(source);
        } else if (source.startsWith("vcc") || source.indexOf("VCC") >= 0) {
            sourceNode = TOP_RAIL; // or use detected topRailVoltage
        } else if (source.startsWith("gnd") || source.indexOf("GND") >= 0) {
            sourceNode = GND;
        }
        
        if (target.startsWith("bb") || target.startsWith("BB")) {
            targetNode = wokwiPinToJumperlessNode(target);
        } else if (target.startsWith("nano") || target.startsWith("NANO")) {
            targetNode = arduinoPinToJumperlessNode(target);
        } else if (target.startsWith("logic")) {
            targetNode = logicAnalyzerPinToGPIO(target);
        } else if (target.startsWith("vcc") || target.indexOf("VCC") >= 0) {
            targetNode = TOP_RAIL;
        } else if (target.startsWith("gnd") || target.indexOf("GND") >= 0) {
            targetNode = GND;
        }
        
        if (sourceNode > 0 && targetNode > 0 && sourceNode != targetNode) {
            // Add connection to state
            // bridges is int[MAX_BRIDGES][3] where [i][0]=node1, [i][1]=node2, [i][2]=duplicates
            if (outState.connections.numBridges < MAX_BRIDGES) {
                int bridgeIdx = outState.connections.numBridges;
                outState.connections.bridges[bridgeIdx][0] = sourceNode;
                outState.connections.bridges[bridgeIdx][1] = targetNode;
                outState.connections.bridges[bridgeIdx][2] = -1; // duplicates
                outState.connections.numBridges++;
                connCount++;
                
                if (debugFP) {
                    Serial.println("    ✓ Mapped: " + String(sourceNode) + " ↔ " + String(targetNode));
                }
                
                // Store wire color for this bridge if color was specified
                if (colorName.length() > 0) {
                    // Track color names for "all green" detection
                    // Skip GND/VCC connections (Wokwi sets these to red/black by default)
                    bool isSpecialNet = (sourceNode == GND || targetNode == GND ||
                                        sourceNode == TOP_RAIL || targetNode == TOP_RAIL ||
                                        sourceNode == BOTTOM_RAIL || targetNode == BOTTOM_RAIL);
                    
                    if (!isSpecialNet) {
                        userColorCount++;
                        if (colorName.equalsIgnoreCase("green") || colorName.equalsIgnoreCase("limegreen")) {
                            greenColorCount++;
                        }
                    }
                    
                    uint32_t color = wokwiColorToRGB(colorName);
                    
                    // Convert black to "no color" for better visibility
                    if (colorName.equalsIgnoreCase("black") || color == 0x000000) {
                        outState.connections.bridgeColors[bridgeIdx] = 0xFFFFFFFF;
                        if (debugFP) {
                            Serial.println("      Color: black → no color (for visibility)");
                        }
                    } else {
                        outState.connections.bridgeColors[bridgeIdx] = color;
                        if (debugFP) {
                            String internalName = wokwiColorToInternalName(colorName);
                            Serial.printf("      Color: %s → 0x%06X (%s)\n", 
                                        colorName.c_str(), color, internalName.c_str());
                        }
                    }
                } else {
                    if (debugFP) {
                        Serial.println("      No color specified");
                    }
                }
            } else {
                Serial.println("    ✗ Max bridges reached!");
                break;
            }
        } else {
            if (debugFP) {
                Serial.println("    ✗ Skipped: couldn't map nodes (" + 
                             String(sourceNode) + ", " + String(targetNode) + ")");
            }
            skippedCount++;
        }
        
        // Move to next connection
        connIdx = connEnd + 1;
    }
    
    // Check if all user-defined colors are green (Wokwi's default)
    // If so, clear all bridge colors to trigger auto-color assignment
    if (userColorCount > 0 && greenColorCount == userColorCount) {
        if (debugFP || !quietMode) {
            Serial.println("◆ All wires are green (Wokwi default) - enabling auto-color assignment");
        }
        // Clear all bridge colors except GND/VCC connections
        for (int b = 0; b < outState.connections.numBridges; b++) {
            int n1 = outState.connections.bridges[b][0];
            int n2 = outState.connections.bridges[b][1];
            bool isSpecialNet = (n1 == GND || n2 == GND ||
                                n1 == TOP_RAIL || n2 == TOP_RAIL ||
                                n1 == BOTTOM_RAIL || n2 == BOTTOM_RAIL);
            if (!isSpecialNet) {
                outState.connections.bridgeColors[b] = 0xFFFFFFFF; // Mark as "no color"
            }
        }
    }
    
    // Only show parsing summary if not in quiet mode or if debugFP is on
    if (!quietMode || debugFP) {
        Serial.println("◆ Wokwi parsing complete:");
        Serial.println("  Connections: " + String(connCount) + " added, " + String(skippedCount) + " skipped");
        if (userColorCount > 0) {
            Serial.println("  Colors: " + String(userColorCount) + " user-defined (" + 
                         String(greenColorCount) + " green)");
        }
        if (topRailVoltage != -9999) {
            Serial.println("  Top rail: " + String(topRailVoltage) + "mV");
        }
        if (bottomRailVoltage != -9999) {
            Serial.println("  Bottom rail: " + String(bottomRailVoltage) + "mV");
        }
    }
    
    // Apply rail voltages to PowerState if detected (check for -9999 which means parse failed)
    if (topRailVoltage != -9999) {
        outState.power.topRail = topRailVoltage / 1000.0f; // Convert mV to V
        if (debugFP) {
            Serial.println("  Set top rail to " + String(outState.power.topRail, 2) + "V");
        }
    }
    if (bottomRailVoltage != -9999) {
        outState.power.bottomRail = bottomRailVoltage / 1000.0f; // Convert mV to V
        if (debugFP) {
            Serial.println("  Set bottom rail to " + String(outState.power.bottomRail, 2) + "V");
        }
    }
    
    // Debug: Show all bridge colors that were stored
    if (debugFP) {
        Serial.println("◆ Bridge colors stored:");
        for (int i = 0; i < outState.connections.numBridges; i++) {
            if (outState.connections.bridgeColors[i] != 0xFFFFFFFF) {
                Serial.printf("  Bridge %d: 0x%06X (%s)\n", i, 
                            outState.connections.bridgeColors[i],
                            rgbToWokwiColorName(outState.connections.bridgeColors[i]).c_str());
            }
        }
    }
    
    // Bridge colors are now stored in outState.connections.bridgeColors[]
    // The actual net-to-color mapping will be done after nets are generated
    // by the system when the state is loaded and nets are computed
    
    // (Locked OLED / probe-power connections are infra functions now -
    // routing/InfraPaths.cpp re-adds them at the rebuild that applies this
    // import. The inline add-if-missing clones are gone; a BUFFER_IN pair
    // in an imported diagram gets scrubbed by the load sanitizer anyway.)

    return connCount > 0;
}

bool parseWokwiDiagramFromFile(const String& filename, int slotNum, String& errorMsg) {
    // Open and read the file using safe functions
    if (!safeFileExists(filename.c_str(), 500)) {
        errorMsg = "File not found: " + filename;
        return false;
    }
    
    File file = safeFileOpen(filename.c_str(), "r", 1000);
    if (!file) {
        errorMsg = "Failed to open file: " + filename;
        return false;
    }
    
    // Read entire file content
    String jsonContent;
    while (file.available()) {
        jsonContent += (char)file.read();
    }
    safeFileClose(file, false);
    
    if (debugFP) {
        Serial.println("◆ Read " + String(jsonContent.length()) + " bytes from " + filename);
    }
    
    // NEVER copy JumperlessState - work directly with the singleton
    SlotManager& mgr = SlotManager::getInstance();
    
    // Remember which slot was active
    int savedActiveSlot = mgr.getActiveSlot();
    bool needToRestore = (savedActiveSlot != slotNum);
    
    // Clear the active state and parse directly into it (NO COPIES!)
    mgr.getActiveState().clear();
    mgr.setActiveSlot(slotNum);
    
    // Parse directly into the active state (no temp object!)
    if (!parseWokwiDiagram(jsonContent, mgr.getActiveState(), slotNum, errorMsg)) {
        // Restore original slot on parse failure
        if (needToRestore) {
            mgr.loadSlot(savedActiveSlot, errorMsg);
        }
        return false;
    }
    
    // Save it
    bool success = false;
    if (mgr.saveSlot(slotNum, errorMsg)) {
        Serial.println("  ✓ Saved Wokwi diagram to slot " + String(slotNum));
        success = true;
        
        // If we modified a non-active slot, reload the original active slot
        if (needToRestore) {
            mgr.loadSlot(savedActiveSlot, errorMsg);
        } else {
            // We just saved the active slot, reload it to apply changes
            Serial.println("  ↻ Reloading active slot to apply changes...");
            if (mgr.loadSlot(slotNum, errorMsg)) {
                Serial.println("  ✓ Applied to hardware");
            } else {
                Serial.println("  ✗ Failed to apply: " + errorMsg);
            }
        }
    } else {
        Serial.println("  ✗ Failed to save: " + errorMsg);
        // Restore the original slot on failure
        if (needToRestore) {
            mgr.loadSlot(savedActiveSlot, errorMsg);
        }
    }
    
    return success;
}

bool parseWokwiDiagramDirectToFile(const String& jsonContent, int slotNum, 
                                    String& errorMsg, bool quietMode) {
    // Parse Wokwi JSON and write YAML directly to file without creating JumperlessState
    // This is the most memory-efficient approach for inactive slots
    
    extern bool debugFP;
    
    if (debugFP && !quietMode) {
        Serial.println("◆ Parsing Wokwi diagram directly to file (zero-copy)");
        Serial.println("  JSON length: " + String(jsonContent.length()) + " bytes");
    }
    
    // Extract rail voltages from text labels
    // Use -9999 as "not found" sentinel (clearly outside valid -8V to +8V range)
    int topRailVoltage = -9999;
    int bottomRailVoltage = -9999;
    
    // Find all text parts and parse voltage info
    int textIdx = 0;
    while ((textIdx = jsonContent.indexOf("\"wokwi-text\"", textIdx)) != -1) {
        int textKeyIdx = jsonContent.indexOf("\"text\"", textIdx);
        if (textKeyIdx != -1 && textKeyIdx < textIdx + 500) {
            String textValue = extractJsonString(jsonContent, "text", textKeyIdx);
            textValue.toLowerCase();
            
            // Parse rail voltage labels - handle various formats:
            // "top rail 3.3V", "TOP RAIL = 5V", "top_rail: 4.5V", "toprail 3v3"
            // "bottom rail -6.5V", "bot rail 2.5V", "BOTTOM_RAIL = -5V"
            // Work directly with textValue (already lowercased) for consistent indexing
            
            // Replace underscores with spaces for flexible matching
            String workText = textValue;
            workText.replace("_", " ");
            
            // Check for top rail patterns (search in workText, extract from workText)
            int topRailIdx = -1;
            int topVoltStartIdx = -1;
            
            // Try "top rail" or "top  rail" (with underscore removed)
            if ((topRailIdx = workText.indexOf("top rail")) >= 0) {
                topVoltStartIdx = topRailIdx + 8; // After "top rail"
            } else if ((topRailIdx = workText.indexOf("toprail")) >= 0) {
                topVoltStartIdx = topRailIdx + 7; // After "toprail"
            } else if ((topRailIdx = workText.indexOf("top")) >= 0) {
                // "top" found - check if "rail" follows within reasonable distance
                int railIdx = workText.indexOf("rail", topRailIdx + 3);
                if (railIdx >= 0 && railIdx - topRailIdx < 12) {
                    topVoltStartIdx = railIdx + 4; // After "rail"
                }
            }
            
            if (topVoltStartIdx > 0) {
                String voltPart = workText.substring(topVoltStartIdx);
                topRailVoltage = parseVoltageString(voltPart);
            }
            
            // Check for bottom rail patterns
            int bottomRailIdx = -1;
            int bottomVoltStartIdx = -1;
            
            // Try various bottom/bot patterns
            if ((bottomRailIdx = workText.indexOf("bottom rail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 11; // After "bottom rail"
            } else if ((bottomRailIdx = workText.indexOf("bot rail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 8; // After "bot rail"
            } else if ((bottomRailIdx = workText.indexOf("bottomrail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 10; // After "bottomrail"
            } else if ((bottomRailIdx = workText.indexOf("botrail")) >= 0) {
                bottomVoltStartIdx = bottomRailIdx + 7; // After "botrail"
            } else if ((bottomRailIdx = workText.indexOf("bottom")) >= 0) {
                // "bottom" found - check if "rail" follows
                int railIdx = workText.indexOf("rail", bottomRailIdx + 6);
                if (railIdx >= 0 && railIdx - bottomRailIdx < 12) {
                    bottomVoltStartIdx = railIdx + 4; // After "rail"
                }
            } else if ((bottomRailIdx = workText.indexOf("bot")) >= 0) {
                // "bot" found - check if "rail" follows
                int railIdx = workText.indexOf("rail", bottomRailIdx + 3);
                if (railIdx >= 0 && railIdx - bottomRailIdx < 12) {
                    bottomVoltStartIdx = railIdx + 4; // After "rail"
                }
            }
            
            if (bottomVoltStartIdx > 0) {
                String voltPart = workText.substring(bottomVoltStartIdx);
                bottomRailVoltage = parseVoltageString(voltPart);
            }
            
            if (textValue.indexOf("vcc") >= 0 && topRailVoltage == -9999) {
                int eqIdx = textValue.indexOf('=');
                if (eqIdx >= 0) {
                    String voltPart = textValue.substring(eqIdx + 1);
                    topRailVoltage = parseVoltageString(voltPart);
                }
            }
        }
        textIdx++;
    }
    
    // Parse connections array
    int connectionsStart = 0, connectionsEnd = 0;
    if (findJsonArray(jsonContent, "connections", connectionsStart, connectionsEnd) != 0) {
        errorMsg = "No connections array found in diagram";
        return false;
    }
    
    // Build YAML content incrementally (string building is OK, it's on heap)
    String yamlContent = "version: 2\n";
    yamlContent += "sourceOfTruth: bridges\n";
    yamlContent += "\nbridges:\n";
    
    int connCount = 0;
    int skippedCount = 0;
    
    // Parse each connection and add to YAML
    int connIdx = connectionsStart + 1;
    
    while (connIdx < connectionsEnd) {
        int connStart = jsonContent.indexOf('[', connIdx);
        if (connStart == -1 || connStart >= connectionsEnd) break;
        
        int connEnd = jsonContent.indexOf(']', connStart);
        if (connEnd == -1 || connEnd >= connectionsEnd) break;
        
        String connStr = jsonContent.substring(connStart + 1, connEnd);
        
        // Parse quoted strings from the connection array
        String source = "", target = "", colorName = "";
        int quoteCount = 0;
        int quoteStart = -1;
        
        for (int i = 0; i < connStr.length(); i++) {
            if (connStr[i] == '"') {
                if (quoteStart == -1) {
                    quoteStart = i;
                } else {
                    String value = connStr.substring(quoteStart + 1, i);
                    if (quoteCount == 0) source = value;
                    else if (quoteCount == 1) target = value;
                    else if (quoteCount == 2) colorName = value;
                    
                    quoteCount++;
                    quoteStart = -1;
                    
                    if (quoteCount >= 3) break;
                }
            }
        }
        
        if (source.length() == 0 || target.length() == 0) {
            connIdx = connEnd + 1;
            skippedCount++;
            continue;
        }
        
        // Convert source and target to Jumperless node numbers
        int sourceNode = -1;
        int targetNode = -1;
        
        if (source.startsWith("bb") || source.startsWith("BB")) {
            sourceNode = wokwiPinToJumperlessNode(source);
        } else if (source.startsWith("nano") || source.startsWith("NANO")) {
            sourceNode = arduinoPinToJumperlessNode(source);
        } else if (source.startsWith("logic")) {
            sourceNode = logicAnalyzerPinToGPIO(source);
        } else if (source.startsWith("vcc") || source.indexOf("VCC") >= 0) {
            sourceNode = TOP_RAIL;
        } else if (source.startsWith("gnd") || source.indexOf("GND") >= 0) {
            sourceNode = GND;
        }
        
        if (target.startsWith("bb") || target.startsWith("BB")) {
            targetNode = wokwiPinToJumperlessNode(target);
        } else if (target.startsWith("nano") || target.startsWith("NANO")) {
            targetNode = arduinoPinToJumperlessNode(target);
        } else if (target.startsWith("logic")) {
            targetNode = logicAnalyzerPinToGPIO(target);
        } else if (target.startsWith("vcc") || target.indexOf("VCC") >= 0) {
            targetNode = TOP_RAIL;
        } else if (target.startsWith("gnd") || target.indexOf("GND") >= 0) {
            targetNode = GND;
        }
        
        if (sourceNode > 0 && targetNode > 0 && sourceNode != targetNode) {
            // Add bridge to YAML
            yamlContent += "  - {n1: " + String(sourceNode) + ", n2: " + String(targetNode);
            
            // Add color if specified
            if (colorName.length() > 0) {
                uint32_t color = wokwiColorToRGB(colorName);
                yamlContent += ", color: 0x" + String(color, HEX);
            }
            
            yamlContent += "}\n";
            connCount++;
        } else {
            skippedCount++;
        }
        
        connIdx = connEnd + 1;
    }
    
    // (Locked OLED / probe-power connections are infra functions now -
    // routing/InfraPaths.cpp re-adds them at the rebuild that applies this
    // import, so nothing needs to be injected into the YAML here.)

    // Add power section if voltages were detected (check for -9999 which means parse failed)
    yamlContent += "\npower:\n";
    if (topRailVoltage != -9999) {
        yamlContent += "  topRail: " + String(topRailVoltage / 1000.0f, 2) + "\n";
    } else {
        yamlContent += "  topRail: 0.0\n";
    }
    if (bottomRailVoltage != -9999) {
        yamlContent += "  bottomRail: " + String(bottomRailVoltage / 1000.0f, 2) + "\n";
    } else {
        yamlContent += "  bottomRail: 0.0\n";
    }
    yamlContent += "  dac0: 3.33\n";
    yamlContent += "  dac1: 0.0\n";
    
    // Show summary
    if (!quietMode || debugFP) {
        Serial.println("◆ Wokwi parsing complete:");
        Serial.println("  Connections: " + String(connCount) + " added, " + String(skippedCount) + " skipped");
        if (topRailVoltage != -9999) {
            Serial.println("  Top rail: " + String(topRailVoltage) + "mV");
        }
        if (bottomRailVoltage != -9999) {
            Serial.println("  Bottom rail: " + String(bottomRailVoltage) + "mV");
        }
    }
    
    // Write directly to file
    SlotManager& mgr = SlotManager::getInstance();
    if (!mgr.writeSlotFile(slotNum, yamlContent, errorMsg)) {
        return false;
    }
    
    if (!quietMode || debugFP) {
        Serial.println("  ✓ Wrote " + String(yamlContent.length()) + " bytes to slot file");
    }
    
    return connCount > 0;
}

