// SPDX-License-Identifier: MIT
/**
 * @file GraphicOverlays.cpp
 * @brief Graphic overlay system for breadboard LED visualization
 * 
 * Uses a 10x30 coordinate system for the breadboard:
 *   Rows 0-4 = Top half (E, D, C, B, A)
 *   Rows 5-9 = Bottom half (F, G, H, I, J)
 *   Columns 0-29 = Breadboard columns 1-30
 */

#include "GraphicOverlays.h"
#include "Colors.h"
#include "Commands.h"
#include "Graphics.h"
#include "LEDs.h"
#include "JumperlessDefines.h"
#include "JumperlOS.h"
#include "RotaryEncoder.h"
#include "States.h"
#include "Probing.h"
#include "JsonState.h"   // escapeJson() for the overlay serialiser

// Global overlay state
GraphicOverlayState graphicOverlayState;

// External screen mapping
extern const int screenMap[445];

// ============================================================================
// LED Mapping Helper
// ============================================================================

/**
 * @brief Convert 10x30 coordinate to LED index
 * @param row Row (0-9)
 * @param col Column (0-29)
 * @return LED index, or -1 if invalid
 * 
 * screenMap layout:
 *   [0-59]    = Top rail (60 LEDs)
 *   [60-209]  = Top half breadboard, rows E-A (5 rows x 30 cols = 150)
 *   [210-359] = Bottom half breadboard, rows F-J (5 rows x 30 cols = 150)
 *   [360-419] = Bottom rail (60 LEDs)
 */
static int coordToLedIndex(int row, int col) {
    if (row < 1 || row > 10 || col < 1 || col > 30) {
        return -1;
    }
    
    int screenIdx;
    if (row <= 5) {
        // Top half: rows 1-5 map to rows A,B,C,D,E
        // row 1 = E (index 4 in 0-based), row 5 = A (index 0 in 0-based) ?? 
        // Wait, original: 0=A, 1=B.. 4=E.
        // User map: 1=A? No, "1 indexed breadboard labels". 
        // Breadboard rows are usually A-E (top), F-J (bottom).
        // Jumperless convention: Row 1 is top?
        // Let's assume Row 1 = Topmost row (A or E depending on view, but consistent with 0=Top).
        // Original code: row 0 = top half.
        
        // Let's stick to the arithmetic:
        // Input 1-10. 
        // Top half 1-5. Input 1 -> Old 0. Input 5 -> Old 4.
        screenIdx = 60 + ((row - 1) * 30) + (col - 1);
    } else {
        // Bottom half: rows 6-10 map to rows F,G,H,I,J
        screenIdx = 210 + ((row - 6) * 30) + (col - 1);
    }
    
    if (screenIdx >= 0 && screenIdx < 445) {
        return screenMap[screenIdx];
    }
    return -1;
}

// ============================================================================
// GraphicOverlay Methods
// ============================================================================

void GraphicOverlay::clear() {
    memset(name, 0, sizeof(name));
    startRow = 0;
    startCol = 0;
    width = 0;
    height = 0;
    memset(colors, 0, sizeof(colors));
    enabled = false;
    showLEDsCore2 = -2;
}

// ============================================================================
// GraphicOverlayState Methods
// ============================================================================

void GraphicOverlayState::clear() {
    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
        overlays[i].clear();
    }
    numOverlays = 0;
    needsRender = false;
}

int GraphicOverlayState::addOverlay(const char* name, int startRow, int startCol,
                                     int width, int height, const uint32_t* colors) {
    if (!name || width <= 0 || height <= 0) {
        return -1;
    }
    
    // Validate range (1-based)
    if (startRow < 1 || startRow > 10 || startCol < 1 || startCol > 30) {
        return -1;
    }
    if (width > MAX_OVERLAY_WIDTH || height > MAX_OVERLAY_HEIGHT) {
        return -1;
    }
    int numPixels = width * height;
    if (numPixels > MAX_OVERLAY_PIXELS) {
        return -1;
    }
    
    // Check if overlay with this name already exists
    int existing = findByName(name);
    if (existing >= 0) {
        // Update existing overlay
        overlays[existing].startRow = startRow;
        overlays[existing].startCol = startCol;
        overlays[existing].width = width;
        overlays[existing].height = height;
        memcpy(overlays[existing].colors, colors, numPixels * sizeof(uint32_t));
        overlays[existing].enabled = true;
        needsRender = true;
        globalState.markDirty();
        return existing;
    }
    
    // Find free slot
    if (numOverlays >= MAX_GRAPHIC_OVERLAYS) {
        return -1;  // No room
    }
    
    int slot = -1;
    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
        if (!overlays[i].enabled && overlays[i].name[0] == '\0') {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        return -1;
    }
    
    // Initialize overlay
    strncpy(overlays[slot].name, name, sizeof(overlays[slot].name) - 1);
    overlays[slot].name[sizeof(overlays[slot].name) - 1] = '\0';
    overlays[slot].startRow = startRow;
    overlays[slot].startCol = startCol;
    overlays[slot].width = width;
    overlays[slot].height = height;
    memcpy(overlays[slot].colors, colors, numPixels * sizeof(uint32_t));
    overlays[slot].enabled = true;
    
    numOverlays++;
    needsRender = true;
    
    globalState.markDirty();

    return slot;
}

bool GraphicOverlayState::removeOverlay(const char* name) {
    int idx = findByName(name);
    if (idx >= 0) {
        return removeOverlay(idx);
    }
    return false;
}

bool GraphicOverlayState::removeOverlay(int index) {
    if (index < 0 || index >= MAX_GRAPHIC_OVERLAYS) {
        return false;
    }
    
    if (overlays[index].enabled || overlays[index].name[0] != '\0') {
        overlays[index].clear();
        numOverlays--;
        if (numOverlays < 0) numOverlays = 0;
        needsRender = true;
        globalState.markDirty();
        return true;
    }
    
    return false;
}

void GraphicOverlayState::clearAll() {
    clear();
    needsRender = true;
    globalState.markDirty();
}

int GraphicOverlayState::findByName(const char* name) const {
    if (!name) return -1;
    
    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
        if (overlays[i].enabled && strcmp(overlays[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void GraphicOverlayState::setPixel(int row, int col, uint32_t color) {
    if (row < 1 || row > 10 || col < 1 || col > 30) {
        return;
    }
    

    // Find or create a "direct" overlay for single pixel control
    const char* directName = "_DIRECT_PIXELS_";
    int idx = findByName(directName);
    
    if (idx < 0) {
        // Create the direct pixel overlay covering entire breadboard
        uint32_t emptyColors[MAX_OVERLAY_PIXELS] = {0};
        idx = addOverlay(directName, 1, 1, 30, 10, emptyColors);
        if (idx < 0) return;
    }
    
    // Calculate pixel index: (row-1) * 30 + (col-1)
    int pixelIdx = (row - 1) * 30 + (col - 1);
    if (pixelIdx >= 0 && pixelIdx < MAX_OVERLAY_PIXELS) {
        overlays[idx].colors[pixelIdx] = color;
        needsRender = true;
    }
}

bool GraphicOverlayState::shiftOverlay(const char* name, int deltaRow, int deltaCol) {
    int idx = findByName(name);
    if (idx < 0) return false;
    
    int newRow = overlays[idx].startRow + deltaRow;
    int newCol = overlays[idx].startCol + deltaCol;
    
    // Wrap around edges (1-based)
    while (newRow < 1) newRow += 10;
    while (newRow > 10) newRow -= 10;
    while (newCol < 1) newCol += 30;
    while (newCol > 30) newCol -= 30;
    
    // Trigger LED refresh to clear old position
    showLEDsCore2 = -2;
    
    overlays[idx].startRow = newRow;
    overlays[idx].startCol = newCol;
    needsRender = true;
    
    return true;
}

bool GraphicOverlayState::placeOverlay(const char* name, int newRow, int newCol) {
    int idx = findByName(name);
    if (idx < 0) return false;
    

    
    // Wrap around edges (1-based)
    while (newRow < 1) newRow += 10;
    while (newRow > 10) newRow -= 10;
    while (newCol < 1) newCol += 30;
    while (newCol > 30) newCol -= 30;


    
    // Trigger LED refresh to clear old position
    showLEDsCore2 = -2;
    
    overlays[idx].startRow = newRow;
    overlays[idx].startCol = newCol;
    needsRender = true;
    
    return true;
}

// ============================================================================
// Global Functions
// ============================================================================

void initGraphicOverlays() {
    graphicOverlayState.clear();
}

/**
 * @brief Render all overlays on top of existing LED state
 */
void __not_in_flash_func(renderGraphicOverlays)() {
#if defined(OG_JUMPERLESS)
    // Overlays are a breadboard-LED-matrix feature; the OG has 1 LED per row and
    // can't render them. Taken out on OG (storage shrunk to 1 slot in the header).
    return;
#endif
    if (graphicOverlayState.numOverlays == 0) {
        return;
    }
    
    for (int o = 0; o < MAX_GRAPHIC_OVERLAYS; o++) {
        GraphicOverlay& overlay = graphicOverlayState.overlays[o];
        if (!overlay.enabled) continue;
        
        int colorIdx = 0;
        for (int r = 0; r < overlay.height; r++) {
            int row = overlay.startRow + r;
            // Wrap row around (1-10)
            while (row > 10) row -= 10;
            while (row < 1) row += 10;
            
            for (int c = 0; c < overlay.width; c++) {
                int col = overlay.startCol + c;
                // Wrap column around (1-30)
                while (col > 30) col -= 30;
                while (col < 1) col += 30;
                
                uint32_t color = overlay.colors[colorIdx++];
                if (color == 0) continue;  // Transparent
                
                int ledIndex = coordToLedIndex(row, col);
                if (ledIndex >= 0 && ledIndex < 400) {
                    leds.setPixelColor(ledIndex, color);
                }
            }
        }
    }
}

// ============================================================================
// YAML Serialization
// ============================================================================

void serializeOverlaysToYAML(String& output, int withANSI ) {
    // Self-test results are session-only (must clear on reset) - never
    // persist them into a slot file.
    int persistable = 0;
    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
        const GraphicOverlay& overlay = graphicOverlayState.overlays[i];
        if (overlay.enabled && strcmp(overlay.name, "_SELFTEST_") != 0) persistable++;
    }
    if (persistable == 0) {
        return;
    }
    
    output += "\noverlays:\n";
    
    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
        const GraphicOverlay& overlay = graphicOverlayState.overlays[i];
        if (!overlay.enabled) continue;
        if (strcmp(overlay.name, "_SELFTEST_") == 0) continue;

        output += "  - name: \"";
        output += overlay.name;
        output += "\"\n";
        output += "    row: ";
        output += String(overlay.startRow);
        output += "\n";
        output += "    col: ";
        output += String(overlay.startCol);
        output += "\n";
        output += "    width: ";
        output += String(overlay.width);
        output += "\n";
        output += "    height: ";
        output += String(overlay.height);
        output += "\n";
        output += "    colors:\n           ";
        if (withANSI != 2) output += "[";
        int numPixels = overlay.width * overlay.height;
        const char* block = "\xE2\x96\x88";  // UTF-8 FULL BLOCK (█)
        for (int j = 0; j < numPixels; j++) {
            if (j > 0) {
                if (j % overlay.width == 0)
                    output += (withANSI == 2) ? "\n           " : ",\n            ";
                else if (withANSI != 2)
                    output += ", ";
            }
            uint32_t rgb = overlay.colors[j] & 0xFFFFFF;
            if (withANSI) {
                int ansi = colorToAnsi(rgb);
                output += "\033[38;5;";
                output += String(ansi);
                output += "m";
            }
            if (withANSI == 2) {
                output += block;
            } else {
                char hexBuf[8];
                snprintf(hexBuf, sizeof(hexBuf), "%06lX", (unsigned long)rgb);
                output += "0x";
                output += hexBuf;
            }
            if (withANSI) output += "\033[0m";
        }
        if (withANSI != 2) output += "]";
        output += "\n";
    }
}

bool deserializeOverlaysFromYAML(const char* yamlContent, String& errorMsg) {
    graphicOverlayState.clearAll();
    
    const char* overlaysSection = strstr(yamlContent, "overlays:");
    if (!overlaysSection) {
        return true;  // No overlays is fine
    }
    
    const char* pos = overlaysSection;
    
    while ((pos = strstr(pos, "- name:")) != nullptr) {
        char name[32] = {0};
        int startRow = 0, startCol = 0, width = 1, height = 1;
        uint32_t colors[MAX_OVERLAY_PIXELS] = {0};
        int numColors = 0;
        
        // Parse name
        const char* nameStart = strstr(pos, "\"");
        if (nameStart) {
            nameStart++;
            const char* nameEnd = strchr(nameStart, '\"');
            if (nameEnd) {
                int len = nameEnd - nameStart;
                if (len > 31) len = 31;
                strncpy(name, nameStart, len);
            }
        }
        
        // Find extent of this overlay entry (next "- name:" or end of overlays section)
        const char* nextEntry = strstr(pos + 7, "- name:");
        const char* entryEnd = nextEntry ? nextEntry : overlaysSection + strlen(overlaysSection);

        // Parse row/col/width/height (must be within this overlay)
        const char* rowPos = strstr(pos, "row:");
        if (rowPos && rowPos < entryEnd) startRow = atoi(rowPos + 4);
        
        const char* colPos = strstr(pos, "col:");
        if (colPos && colPos < entryEnd) startCol = atoi(colPos + 4);
        
        const char* widthPos = strstr(pos, "width:");
        if (widthPos && widthPos < entryEnd) width = atoi(widthPos + 6);
        
        const char* heightPos = strstr(pos, "height:");
        if (heightPos && heightPos < entryEnd) height = atoi(heightPos + 7);
        
        // Parse colors
        const char* colorsPos = strstr(pos, "colors:");
        if (colorsPos && colorsPos < entryEnd) {
            const char* bracketStart = strchr(colorsPos, '[');
            if (bracketStart && bracketStart < entryEnd) {
                const char* p = bracketStart + 1;
                while (*p && *p != ']' && numColors < MAX_OVERLAY_PIXELS && p < entryEnd) {
                    while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                    if (*p == ']' || !*p) break;
                    if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) {
                        colors[numColors++] = strtoul(p, nullptr, 16);
                    }
                    while (*p && *p != ',' && *p != ']') p++;
                }
            }
        }
        
        if (name[0] != '\0' && width > 0 && height > 0) {
            graphicOverlayState.addOverlay(name, startRow, startCol, width, height, colors);
        }
        
        pos++;
    }
    
    return true;
}

// ============================================================================
// JSON Serialization
// ============================================================================

void serializeOverlaysToJSON(String& output) {
    output += "  \"overlays\": [";
    
    bool first = true;
    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
        const GraphicOverlay& overlay = graphicOverlayState.overlays[i];
        if (!overlay.enabled) continue;
        
        if (!first) output += ",";
        first = false;
        
        output += "\n    {";
        output += "\"name\":\"";
        // Bounded and escaped: name is a fixed char[32], and this JSON is handed to
        // MicroPython's mp_obj_new_str(), which rejects the entire document if any
        // byte is not valid UTF-8. An unescaped quote here also broke the JSON outright.
        output += escapeJson(overlay.name, sizeof(overlay.name) - 1);
        output += "\",\"row\":";
        output += String(overlay.startRow);
        output += ",\"col\":";
        output += String(overlay.startCol);
        output += ",\"width\":";
        output += String(overlay.width);
        output += ",\"height\":";
        output += String(overlay.height);
        output += ",\"colors\":[";
        int numPixels = overlay.width * overlay.height;
        for (int j = 0; j < numPixels; j++) {
            if (j > 0) output += ",";
            char hexBuf[12];
            snprintf(hexBuf, sizeof(hexBuf), "\"%06X\"", overlay.colors[j] & 0xFFFFFF);
            output += hexBuf;
        }
        output += "]}";
    }
    
    output += "\n  ]";
}

// ---------------------------------------------------------------------------
// Snake game - runnable as app or from overlay debug menu
// Exit: 'q' on serial, or hold clickwheel (encoder button HELD)
// ---------------------------------------------------------------------------
void runSnakeGame(void) {
    Jerial.println( "▶ Snake - WASD/Arrows, encoder turn left/right, probe (connect=right/remove=left), hold clickwheel or q to quit" );
    oled.showMultiLineSmallText("WASD / Arrows / encoder to turn\nq or hold clickwheel to quit");
    Jerial.flush();

    int snakeX[30], snakeY[30];
    int snakeLen = 5;
    snakeX[0] = 16; snakeY[0] = 6;
    snakeX[1] = 15; snakeY[1] = 6;
    snakeX[2] = 14; snakeY[2] = 6;
    snakeX[3] = 13; snakeY[3] = 6;
    snakeX[4] = 12; snakeY[4] = 6;
    int dx = 1, dy = 0;
    int foodX = random(1, 31), foodY = random(1, 11);

    uint8_t baseHue = 0;
    uint32_t foodColor = 0xFFFFFF;
    int speed = 120;
    const unsigned long ENCODER_TURN_COOLDOWN_MS = 250;
    unsigned long lastEncoderTurnMs = 0;
    long lastEncoderPosition = encoderPosition;  // raw position for left/right
    int lastRotaryDivider = rotaryDivider;
    // rotaryDivider = 2;
    setTerminalLineBuffering(true); // game needs raw input

    while (true) {
        // rotaryEncoderStuff();  // updates encoderPosition and button
        if (encoderButtonState == HELD) {
            encoderButtonState = IDLE;
            break;
        }
        if (Jerial.available() > 0) {
            char key = Jerial.read();
            if (key == 'q') break;
            if (key == 27) {
                if (Jerial.available() > 0 && Jerial.read() == '[' && Jerial.available() > 0) {
                    char arrow = Jerial.read();
                    if (arrow == 'A' && dy != 1) { dx = 0; dy = -1; }
                    if (arrow == 'B' && dy != -1) { dx = 0; dy = 1; }
                    if (arrow == 'D' && dx != 1) { dx = -1; dy = 0; }
                    if (arrow == 'C' && dx != -1) { dx = 1; dy = 0; }
                }
            }
            if (key == 'w' && dy != 1) { dx = 0; dy = -1; }
            if (key == 's' && dy != -1) { dx = 0; dy = 1; }
            if (key == 'a' && dx != 1) { dx = -1; dy = 0; }
            if (key == 'd' && dx != -1) { dx = 1; dy = 0; }
        }
        int probeBtn = ProbeButton::getInstance().getButtonPress(true);
        if (probeBtn == 2) {
            int ndx = -dy, ndy = dx;
            if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
        } else if (probeBtn == 1) {
            int ndx = dy, ndy = -dx;
            if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
        }

        for (int i = snakeLen - 1; i > 0; i--) {
            snakeX[i] = snakeX[i-1];
            snakeY[i] = snakeY[i-1];
        }
        snakeX[0] += dx;
        snakeY[0] += dy;

        if (snakeX[0] < 1) snakeX[0] = 30;
        if (snakeX[0] > 30) snakeX[0] = 1;
        if (snakeY[0] < 1) snakeY[0] = 10;
        if (snakeY[0] > 10) snakeY[0] = 1;

        if (snakeX[0] == foodX && snakeY[0] == foodY) {
            if (snakeLen < 30) snakeLen++;
            foodX = random(1, 31);
            foodY = random(1, 11);
        }

        graphicOverlayState.clearAll();
        for (int i = 0; i < snakeLen; i++) {
            uint8_t segmentHue = (int)(baseHue + (((float)i / (float)snakeLen) * 255.0)) % 255;
            uint32_t segmentColor = HsvToRaw({segmentHue, 255, 100});
            graphicOverlayState.setPixel(snakeY[i], snakeX[i], segmentColor);
        }
        graphicOverlayState.setPixel(foodY, foodX, foodColor);

        // Encoder: use raw position delta for left/right turn, with cooldown
        unsigned long now = millis();
        long pos = encoderPosition;
        long delta = pos - lastEncoderPosition;
       
        // if (now - lastEncoderTurnMs >= ENCODER_TURN_COOLDOWN_MS &&( delta > 4 || delta < -4)) {
        //     Serial.print("delta: ");
        //     Serial.print(delta);
        //     Serial.print("   encoderPosition: ");
        //     Serial.println(encoderPosition);
        //     if (delta > 0) {
        //         int ndx = -dy, ndy = dx;
        //         if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
        //     } else {
        //         int ndx = dy, ndy = -dx;
        //         if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
        //     }
        //     lastEncoderTurnMs = now;
        //     lastEncoderPosition = pos;
        // }

        unsigned long startTime = millis();
        while (millis() - startTime < (unsigned long)speed) {
            jOS.serviceInner();
            // rotaryEncoderStuff();
            if (encoderButtonState == HELD) break;
            now = millis();


rotaryEncoderStuff();
            pos = encoderPosition;
            delta = pos - lastEncoderPosition;
            if (( delta > 2 || delta < -2) && millis() - lastEncoderTurnMs > ENCODER_TURN_COOLDOWN_MS) {
                if (delta > 0) {
                    int ndx = -dy, ndy = dx;
                    if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
                } else {
                    int ndx = dy, ndy = -dx;
                    if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
                }
                lastEncoderTurnMs = now;
                lastEncoderPosition = pos;
                break;
            } else if (millis() - lastEncoderTurnMs > ENCODER_TURN_COOLDOWN_MS) {
                lastEncoderTurnMs = now;
                lastEncoderPosition = pos;
                
            }

            if (Jerial.available() > 0) {
                char k = Jerial.read();
                if (k == 'q') goto snake_exit;
                if (k == 27 && Jerial.available() > 0 && Jerial.read() == '[' && Jerial.available() > 0) {
                    char arrow = Jerial.read();
                    if (arrow == 'A' && dy != 1) { dx = 0; dy = -1; }
                    if (arrow == 'B' && dy != -1) { dx = 0; dy = 1; }
                    if (arrow == 'D' && dx != 1) { dx = -1; dy = 0; }
                    if (arrow == 'C' && dx != -1) { dx = 1; dy = 0; }
                }
                if (k == 'w' && dy != 1) { dx = 0; dy = -1; }
                if (k == 's' && dy != -1) { dx = 0; dy = 1; }
                if (k == 'a' && dx != 1) { dx = -1; dy = 0; }
                if (k == 'd' && dx != -1) { dx = 1; dy = 0; }
                break;
            }
            probeBtn = ProbeButton::getInstance().getButtonPress(true);
            if (probeBtn == 2) {
                int ndx = -dy, ndy = dx;
                if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
                break;
            }
            if (probeBtn == 1) {
                int ndx = dy, ndy = -dx;
                if (ndx != 0 || ndy != 0) { dx = ndx; dy = ndy; }
                break;
            }
        }
    }
snake_exit:
    rotaryDivider = lastRotaryDivider;
    pushLineBufferingToApp(); // resync app to the user's config on exit
    if (encoderButtonState == HELD) encoderButtonState = IDLE;
    graphicOverlayState.clearAll();
    Jerial.println( "✓ Snake ended" );
    Jerial.flush();
}

void GraphicOverlayState::debugMenu(void) {
    
        Jerial.println( "\n\r╭────────────────────────────────────╮" );
    Jerial.println( "│     Overlay Debug Menu             │" );
    Jerial.println( "├────────────────────────────────────┤" );
    Jerial.println( "│ Static Patterns:                   │" );
    Jerial.println( "│   1 - Cross-gap test (row 4→5)     │" );
    Jerial.println( "│   2 - Full row test (row 0)        │" );
    Jerial.println( "│   3 - Full column test (col 0)     │" );
    Jerial.println( "│   4 - Rainbow gradient             │" );
    Jerial.println( "│   5 - Checkerboard pattern         │" );
    Jerial.println( "│   6 - Border outline               │" );
    Jerial.println( "├────────────────────────────────────┤" );
    Jerial.println( "│ Animations (q to stop):            │" );
    Jerial.println( "│   b - Bouncing ball                │" );
    Jerial.println( "│   w - Wave animation               │" );
    Jerial.println( "│   n - Snake                        │" );
    Jerial.println( "├────────────────────────────────────┤" );
    Jerial.println( "│ m - Move overlay (arrow keys)      │" );
    Jerial.println( "│ c - Clear all    s - Status        │" );
    Jerial.println( "│ q - Quit                           │" );
    Jerial.println( "╰────────────────────────────────────╯\n\r" );
    setTerminalLineBuffering( true ); // needs raw input
    
    // Track currently selected overlay for arrow key movement
    int selectedOverlay = -1;
    
    while (true) {
        if (Jerial.available() > 0) {
            char choice = Jerial.read();
            
            // Check for ESC sequence (arrow keys)
            if (choice == 27) {
                delay(2);  // Wait for rest of sequence
                if (Jerial.available() > 0 && Jerial.read() == '[') {
                    if (Jerial.available() > 0) {
                        char arrow = Jerial.read();
                        // Move selected overlay with arrow keys
                        if (selectedOverlay >= 0 && selectedOverlay < MAX_GRAPHIC_OVERLAYS &&
                            graphicOverlayState.overlays[selectedOverlay].enabled) {
                            const char* name = graphicOverlayState.overlays[selectedOverlay].name;
                            switch (arrow) {
                                case 'A':  // Up
                                    graphicOverlayState.shiftOverlay(name, -1, 0);
                                    // Jerial.print("\r↑ "); Jerial.print(name);
                                    // Jerial.print(" → row "); Jerial.println(graphicOverlayState.overlays[selectedOverlay].startRow);
                                    break;
                                case 'B':  // Down
                                    graphicOverlayState.shiftOverlay(name, 1, 0);
                                    // Jerial.print("\r↓ "); Jerial.print(name);
                                    // Jerial.print(" → row "); Jerial.println(graphicOverlayState.overlays[selectedOverlay].startRow);
                                    break;
                                case 'C':  // Right
                                    graphicOverlayState.shiftOverlay(name, 0, 1);
                                    // Jerial.print("\r→ "); Jerial.print(name);
                                    // Jerial.print(" → col "); Jerial.println(graphicOverlayState.overlays[selectedOverlay].startCol);
                                    break;
                                case 'D':  // Left
                                    graphicOverlayState.shiftOverlay(name, 0, -1);
                                    // Jerial.print("\r← "); Jerial.print(name);
                                    // Jerial.print(" → col "); Jerial.println(graphicOverlayState.overlays[selectedOverlay].startCol);
                                    break;
                            }
                            Jerial.flush();
                        } else {
                            Jerial.println( "No overlay selected. Press 'm' first." );
                            Jerial.flush();
                        }
                    }
                }
                continue;
            }
            
            if (choice == 'q') {
                Jerial.println( "Exiting overlay debug menu" );
                break;
            }
            
            switch (choice) {
                case '1': {
                    uint32_t colors[] = {
                        0x440000, 0x004400, 0x000044, 0x444400, 0x004444,
                        0x440044, 0x444444, 0x222222, 0x111111, 0x333333
                    };
                    // Row 4->5 (Row E), Col 12->13
                    graphicOverlayState.addOverlay("cross_gap", 5, 13, 5, 2, colors);
                    Jerial.println( "✓ Cross-gap overlay at row 5-6, col 13-17" );
                    Jerial.flush();
                    selectedOverlay = graphicOverlayState.findByName("cross_gap");
                    break;
                }
                case '2': {
                    uint32_t colors[30];
                    for (int i = 0; i < 30; i++) {
                        colors[i] = 0x220000 + (i * 0x000808);
                    }
                    // Row 0->1
                    graphicOverlayState.addOverlay("full_row", 1, 1, 30, 1, colors);
                    Jerial.println( "✓ Full row overlay at row 1" );
                    Jerial.flush();
                    selectedOverlay = graphicOverlayState.findByName("full_row");
                    break;
                }
                case '3': {
                    uint32_t colors[10];
                    for (int i = 0; i < 10; i++) {
                        colors[i] = 0x002200 + (i * 0x001100);
                    }
                    // Col 0->1
                    graphicOverlayState.addOverlay("full_col", 1, 1, 1, 10, colors);
                    Jerial.println( "✓ Full column overlay at col 1" );
                    Jerial.flush();
                    selectedOverlay = graphicOverlayState.findByName("full_col");
                    break;
                }
                case '4': {
                    uint32_t rainbowColors[] = {
                        0x440000, 0x442200, 0x444400, 0x004400, 0x000044, 0x220044,
                        0x440000, 0x000000, 0x000000, 0x000000, 0x000000, 0x220044,
                        0x440000, 0x000000, 0x000000, 0x000000, 0x000000, 0x220044,
                        0x440000, 0x000000, 0x000000, 0x000000, 0x000000, 0x220044,
                        0x440000, 0x442200, 0x444400, 0x004400, 0x000044, 0x220044
                    };
                    // Row 2->3, Col 12->13
                    graphicOverlayState.addOverlay("rainbow", 3, 13, 6, 5, rainbowColors);
                    Jerial.println( "✓ Rainbow gradient at center" );
                    Jerial.flush();
                    selectedOverlay = graphicOverlayState.findByName("rainbow");
                    break;
                }
                case '5': {
                    uint32_t checkColors[36];
                    for (int r = 0; r < 6; r++) {
                        for (int co = 0; co < 6; co++) {
                            checkColors[r * 6 + co] = ((r + co) % 2) ? 0x333333 : 0x000000;
                        }
                    }
                    // Row 2->3, Col 12->13
                    graphicOverlayState.addOverlay("checker", 3, 13, 6, 6, checkColors);
                    Jerial.println( "✓ Checkerboard at center" );
                    Jerial.flush();
                    selectedOverlay = graphicOverlayState.findByName("checker");
                    break;
                }
                case '6': {
                    uint32_t borderColors[300] = {0};
                    for (int col = 0; col < 30; col++) {
                        // Top row 1
                        borderColors[col] = 0x003333;
                        // Bottom row 10 (index 9 -> 270)
                        borderColors[9 * 30 + col] = 0x003333;
                    }
                    for (int r = 0; r < 10; r++) {
                        // Left col 1
                        borderColors[r * 30] = 0x003333;
                        // Right col 30
                        borderColors[r * 30 + 29] = 0x003333;
                    }
                    graphicOverlayState.addOverlay("border", 1, 1, 30, 10, borderColors);
                    Jerial.println( "✓ Border outline" );
                    Jerial.flush();
                    selectedOverlay = graphicOverlayState.findByName("border");
                    break;
                }
                case 'm':
                case 'M': {
                    // Select overlay to move
                    if (graphicOverlayState.numOverlays == 0) {
                        Jerial.println( "No overlays to move. Create one first!" );
                        Jerial.flush();
                        break;
                    }
                    Jerial.println( "Select overlay to move:" );
                    int count = 0;
                    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
                        if (graphicOverlayState.overlays[i].enabled) {
                            Jerial.print( "  " );
                            Jerial.print( count );
                            Jerial.print( " - " );
                            Jerial.println( graphicOverlayState.overlays[i].name );
                            count++;
                        }
                    }
                    Jerial.println( "Use arrow keys to move, any number to select" );
                    Jerial.flush();
                    
                    // Wait for selection
                    while (!Jerial.available()) delay(10);
                    char sel = Jerial.read();
                    if (sel >= '0' && sel <= '9') {
                        int target = sel - '0';
                        count = 0;
                        for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
                            if (graphicOverlayState.overlays[i].enabled) {
                                if (count == target) {
                                    selectedOverlay = i;
                                    Jerial.print( "✓ Selected: " );
                                    Jerial.println( graphicOverlayState.overlays[i].name );
                                    Jerial.println( "  Use arrow keys to move" );
                                    Jerial.flush();
                                    break;
                                }
                                count++;
                            }
                        }
                    }
                    break;
                }
                case 'b':
                case 'B': {
                    Jerial.println( "▶ Bouncing ball - q to stop" );
                    uint32_t ballColor = 0xaaaaaa;
                    graphicOverlayState.addOverlay("ball", 1, 1, 1, 1, &ballColor);
                    
                    int row = 1, col = 1;
                    int dRow = 1, dCol = 1;
                    
                    while (true) {
                        if (Jerial.available() > 0) break;
                        
                        row += dRow;
                        col += dCol;
                        
                        if (row <= 1 || row >= 10) dRow = -dRow;
                        if (col <= 1 || col >= 30) dCol = -dCol;
                        
                        row = constrain(row, 1, 10);
                        col = constrain(col, 1, 30);
                        
                        graphicOverlayState.placeOverlay("ball", row, col);
                        delay(80);
                    }
                    graphicOverlayState.removeOverlay("ball");
                    Jerial.println( "✓ Ball stopped" );
                    Jerial.flush();
                    break;
                }
                case 'w':
                case 'W': {
                    Jerial.println( "▶ Wave animation - q to stop" );
                    uint32_t waveColors[30];
                    for (int i = 0; i < 30; i++) waveColors[i] = 0x0044FF;
                    // Row 4->5 (Row E)
                    graphicOverlayState.addOverlay("wave", 5, 1, 30, 1, waveColors);
                    
                    float phase = 0;
                    while (true) {
                        if (Jerial.available() > 0 && Jerial.read() == 'q') break;
                        
                        for (int i = 0; i < 30; i++) {
                            int brightness = (int)(127 + 127 * sin(phase + i * 0.3));
                            waveColors[i] = (brightness << 16) | (brightness / 2);
                        }
                        graphicOverlayState.addOverlay("wave", 5, 1, 30, 1, waveColors);
                        phase += 0.2;
                        delay(30);
                    }
                    graphicOverlayState.removeOverlay("wave");
                    Jerial.println( "✓ Wave stopped" );
                    Jerial.flush();
                    break;
                }
                case 'n':
                case 'N':
                    runSnakeGame();
                    break;
                case 'c':
                case 'C':
                    graphicOverlayState.clearAll();
                    selectedOverlay = -1;
                    Jerial.println( "✓ All overlays cleared" );
                    Jerial.flush();
                    break;
                case 's':
                case 'S':
                    Jerial.print( "Active overlays: " );
                    Jerial.println( graphicOverlayState.numOverlays );
                    if (selectedOverlay >= 0) {
                        Jerial.print( "  Selected: " );
                        Jerial.println( graphicOverlayState.overlays[selectedOverlay].name );
                    }
                    Jerial.flush();
                    for (int i = 0; i < MAX_GRAPHIC_OVERLAYS; i++) {
                        if (graphicOverlayState.overlays[i].enabled) {
                            Jerial.print( "  - " );
                            Jerial.print( graphicOverlayState.overlays[i].name );
                            Jerial.print( " @ (" );
                            Jerial.print( graphicOverlayState.overlays[i].startRow );
                            Jerial.print( "," );
                            Jerial.print( graphicOverlayState.overlays[i].startCol );
                            Jerial.print( ") " );
                            Jerial.print( graphicOverlayState.overlays[i].width );
                            Jerial.print( "x" );
                            Jerial.println( graphicOverlayState.overlays[i].height );
                        }
                    }
                    break;
            }
        }
        delay(10);
    }
    pushLineBufferingToApp( ); // resync app to the user's config on exit
    
}
