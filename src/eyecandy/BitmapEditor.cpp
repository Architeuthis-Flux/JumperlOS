/**
 * Bitmap Editor Implementation
 * 
 * Edit OLED bitmap files using block characters on terminal and OLED display.
 */

#include "BitmapEditor.h"
#include "oled.h"
#include "Jerial.h"
#include "RotaryEncoder.h"
#include "JumperlessDefines.h"
#include "Probing.h"
#include "JumperlOS.h"
#include <cstring>
#include <new>
#include "FilesystemStuff.h"  // For safe file operations
#include "externVars.h"       // For pauseCore2ForFlash
// External objects
extern class oled oled;

BitmapEditor::BitmapEditor() 
    : active(false)
    , bitmapData(nullptr)
    , width(0)
    , height(0)
    , dataSize(0)
    , hasHeader(false)
    , cursorX(0)
    , cursorY(0)
    , prevCursorX(0)
    , prevCursorY(0)
    , prevScrollX(0)
    , prevScrollY(0)
    , scrollX(0)
    , scrollY(0)
    , displayMode(MODE_FULL_BLOCKS)
    , modified(false)
    , running(false)
    , needFullRedraw(true)
    , termWidth(128)
    , termHeight(22)
    , last_encoder_position(0)
    , last_encoder_update(0)
    , last_button_state(true)
    , button_debounce_time(0)
    , encoderMode(ENCODER_HORIZONTAL)
    , drawMode(DRAW_TOGGLE)
    , inMenuMode(false)
    , menuSelectedIndex(0)
    , lastSwitchCheck(0)
    , menuViewModeValue(0)
    , menuEncModeValue(0)
    , menuDrawModeValue(0)
{
}

BitmapEditor::~BitmapEditor() {
    if (bitmapData) {
        delete[] bitmapData;
        bitmapData = nullptr;
    }
}

bool BitmapEditor::loadFile(const String& filepath) {
    this->filepath = filepath;

    // A second load through the same editor object used to leak the previous
    // buffer - every path below assigns bitmapData without freeing it - and
    // inherit the previous file's format verdict through a stale hasHeader.
    delete[] bitmapData;
    bitmapData = nullptr;
    hasHeader = false;

    if (!safeFileExists(filepath.c_str(), 500)) {
        Jerial.println("File not found: " + filepath);
        return false;
    }
    
    File file = safeFileOpen(filepath.c_str(), "r+", 1000);
    if (!file) {
        Jerial.println("Failed to open file");
        return false;
    }
    
    size_t fileSize = file.size();
    
    // Handle empty file - create new bitmap with OLED dimensions
    if (fileSize == 0) {
        safeFileClose(file, false);
        Jerial.println("Empty file detected - creating new bitmap");
        
        // Use OLED dimensions
        width = oled.displayWidth;
        height = oled.displayHeight;
        hasHeader = true;
        dataSize = (width * height + 7) / 8;
        
        // Allocate and clear bitmap data
        bitmapData = new uint8_t[dataSize];

        // Clear to all zeros (all pixels off)
        memset(bitmapData, 0, dataSize);
        
        Jerial.print("Created new bitmap: ");
        Jerial.print(width);
        Jerial.print("x");
        Jerial.println(height);
        
        modified = true;  // Mark as modified so it will be saved with header
        return true;
    }
    
    // Try to detect format with header
    if (fileSize >= 4) {
        uint8_t wl = file.read();
        uint8_t wh = file.read();
        uint8_t hl = file.read();
        uint8_t hh = file.read();
        
        width = wl | (wh << 8);
        height = hl | (hh << 8);
        
        // Validate header dimensions
        if (width > 0 && width <= 256 && height > 0 && height <= 256) {
            size_t expectedSize = (width * height + 7) / 8;
            if (fileSize == expectedSize + 4) {
                // Valid header format
                hasHeader = true;
                dataSize = expectedSize;
                
                Jerial.print("Loaded bitmap with header: ");
                Jerial.print(width);
                Jerial.print("x");
                Jerial.println(height);
            }
        }
    }
    
    // If header format failed, try raw format
    if (!hasHeader) {
        file.seek(0); // Reset to beginning
        
        // Auto-detect common sizes
        if (fileSize == 512) {
            width = 128;
            height = 32;
        } else if (fileSize == 1024) {
            width = 128;
            height = 64;
        } else if (fileSize == 256) {
            width = 64;
            height = 32;
        } else {
            // Unknown format - assume square-ish based on pixel count
            int pixelCount = fileSize * 8;
            width = (int)sqrt(pixelCount);
            height = (pixelCount + width - 1) / width;
        }
        
        // Raw format means dataSize IS the file size - bound it, or opening a
        // big file here attempts a same-sized allocation on a ~181KB heap.
        // 32768 matches ImagesApp's ceiling (256x256 four times over).
        if (fileSize > 32768) {
            Jerial.println("File too large for a bitmap (max 32768 bytes)");
            safeFileClose(file, false);
            return false;
        }

        dataSize = fileSize;
        Jerial.print("Loaded raw bitmap: ");
        Jerial.print(width);
        Jerial.print("x");
        Jerial.println(height);
    }
    
    // Allocate and read bitmap data
    bitmapData = new uint8_t[dataSize];

    size_t bytesRead = file.read(bitmapData, dataSize);
    safeFileClose(file, false);
    
    if (bytesRead != dataSize) {
        Jerial.println("Failed to read bitmap data");
        delete[] bitmapData;
        bitmapData = nullptr;
        return false;
    }
    
    return true;
}

bool BitmapEditor::newFile(const String& filepath, int w, int h) {
    this->filepath = filepath;
    
    // Use OLED dimensions as default if not specified
    if (w <= 0 || h <= 0) {
        w = oled.displayWidth;
        h = oled.displayHeight;
    }
    
    width = w;
    height = h;
    hasHeader = true;  // New files get headers
    
    // Calculate data size
    dataSize = (width * height + 7) / 8;

    // Allocate and clear bitmap data (freeing any bitmap this editor object
    // already held - newFile used to leak it on re-entry)
    delete[] bitmapData;
    bitmapData = new uint8_t[dataSize];

    // Clear to all zeros (all pixels off)
    memset(bitmapData, 0, dataSize);
    
    Jerial.print("Created new bitmap: ");
    Jerial.print(width);
    Jerial.print("x");
    Jerial.println(height);
    
    modified = true;  // Mark as modified so it will be saved
    return true;
}

bool BitmapEditor::getPixel(int x, int y) {
    if (x < 0 || x >= width || y < 0 || y >= height) {
        return false;
    }
    
    // Calculate bit position (MSB-first, row-major)
    int byteIndex = y * ((width + 7) / 8) + (x / 8);
    int bitIndex = 7 - (x % 8);  // MSB is leftmost pixel
    
    if (byteIndex >= dataSize) {
        return false;
    }
    
    return (bitmapData[byteIndex] >> bitIndex) & 1;
}

void BitmapEditor::setPixel(int x, int y, bool value) {
    if (x < 0 || x >= width || y < 0 || y >= height) {
        return;
    }
    
    // Calculate bit position (MSB-first, row-major)
    int byteIndex = y * ((width + 7) / 8) + (x / 8);
    int bitIndex = 7 - (x % 8);  // MSB is leftmost pixel
    
    if (byteIndex >= dataSize) {
        return;
    }
    
    if (value) {
        bitmapData[byteIndex] |= (1 << bitIndex);
    } else {
        bitmapData[byteIndex] &= ~(1 << bitIndex);
    }
    
    modified = true;
}

void BitmapEditor::togglePixel(int x, int y) {
    setPixel(x, y, !getPixel(x, y));
}

const char* BitmapEditor::getQuarterBlockString(bool tl, bool tr, bool bl, bool br) {
    // Map 2x2 pixel pattern to quarter block character (UTF-8 strings)
    int pattern = (tl ? 8 : 0) | (tr ? 4 : 0) | (bl ? 2 : 0) | (br ? 1 : 0);
    
    switch (pattern) {
        case 0:  return " ";      // ····
        case 1:  return "▗";      // ··▗
        case 2:  return "▖";      // ·▖·
        case 3:  return "▄";      // ·▖▗ -> ▄
        case 4:  return "▝";      // ▝··
        case 5:  return "▐";      // ▝·▗ -> ▐
        case 6:  return "▞";      // ▝▖· -> ▞
        case 7:  return "▟";      // ▝▖▗ -> ▟
        case 8:  return "▘";      // ▘··
        case 9:  return "▚";      // ▘·▗ -> ▚
        case 10: return "▌";      // ▘▖· -> ▌
        case 11: return "▙";      // ▘▖▗ -> ▙
        case 12: return "▀";      // ▘▝· -> ▀
        case 13: return "▜";      // ▘▝▗ -> ▜
        case 14: return "▛";      // ▘▝▖ -> ▛
        case 15: return "█";      // ▘▝▖▗ -> █
        default: return " ";
    }
}

void BitmapEditor::cycleQuarterBlock(int blockX, int blockY) {
    // blockX, blockY are in quarter-block coordinates (each represents 2x2 pixels)
    int pixelX = blockX * 2;
    int pixelY = blockY * 2;
    
    // Get current 2x2 pixel state
    bool tl = getPixel(pixelX, pixelY);
    bool tr = getPixel(pixelX + 1, pixelY);
    bool bl = getPixel(pixelX, pixelY + 1);
    bool br = getPixel(pixelX + 1, pixelY + 1);
    
    // Cycle through the 4 pixels in order: TL -> TR -> BL -> BR -> (back to TL)
    // Toggle the first "off" pixel we find, or turn off the last "on" pixel
    if (!tl) {
        setPixel(pixelX, pixelY, true);
    } else if (!tr) {
        setPixel(pixelX + 1, pixelY, true);
    } else if (!bl) {
        setPixel(pixelX, pixelY + 1, true);
    } else if (!br) {
        setPixel(pixelX + 1, pixelY + 1, true);
    } else {
        // All on, turn all off
        setPixel(pixelX, pixelY, false);
        setPixel(pixelX + 1, pixelY, false);
        setPixel(pixelX, pixelY + 1, false);
        setPixel(pixelX + 1, pixelY + 1, false);
    }
}

void BitmapEditor::drawBitmapFullBlocks() {
    // Clear screen
    Serial.print("\x1b[2J\x1b[H");
    
    // Calculate visible area
    int startX = scrollX;
    int startY = scrollY;
    int endX = min(startX + termWidth, width);
    int endY = min(startY + termHeight, height);
    
    // Draw bitmap
    for (int y = startY; y < endY; y++) {
        for (int x = startX; x < endX; x++) {
            bool pixel = getPixel(x, y);
            
            // Highlight cursor position with color based on pixel state
            if (x == cursorX && y == cursorY) {
                // Green for OFF (will turn ON), Red for ON (will turn OFF)
                int color = pixel ? CURSOR_COLOR_ON : CURSOR_COLOR_OFF;
                Serial.print("\x1b[48;5;");  // Background color
                Serial.print(color);
                Serial.print("m");
            }
            
            Serial.print(pixel ? "█" : " ");
            
            if (x == cursorX && y == cursorY) {
                Serial.print("\x1b[0m");  // Reset
            }
        }
        Serial.println();
    }
}

void BitmapEditor::drawBitmapHalfBlocks() {
    // Clear screen
    Serial.print("\x1b[2J\x1b[H");
    
    // Calculate visible area
    // Each terminal character represents 1 pixel horizontally × 2 pixels vertically
    // We use ▀/▄ to show top and bottom pixels in each column
    int startX = scrollX;
    int startY = scrollY / 2;  // Each char is 2 pixels tall
    int endX = min(startX + termWidth, width);
    int endY = min(startY + termHeight, height / 2);
    
    // Draw bitmap using half blocks with color highlighting
    // Each character shows a vertical pair of pixels (top and bottom)
    for (int cy = startY; cy < endY; cy++) {
        for (int cx = startX; cx < endX; cx++) {
            int px = cx;
            int pyTop = cy * 2;
            int pyBottom = cy * 2 + 1;
            
            bool pixelTop = getPixel(px, pyTop);
            bool pixelBottom = getPixel(px, pyBottom);
            
            // Check if cursor is on either pixel in this column
            bool cursorTop = (cursorX == px && cursorY == pyTop);
            bool cursorBottom = (cursorX == px && cursorY == pyBottom);
            bool hasCursor = cursorTop || cursorBottom;
            
            if (hasCursor) {
                // Determine colors for top pixel (FG for ▀, BG for ▄)
                int topColor;
                if (cursorTop) {
                    topColor = pixelTop ? CURSOR_COLOR_ON : CURSOR_COLOR_OFF;  // Red if ON, green if OFF
                } else {
                    topColor = pixelTop ? 255 : 0;   // White if ON, black if OFF
                }
                
                // Determine colors for bottom pixel (BG for ▀, FG for ▄)
                int bottomColor;
                if (cursorBottom) {
                    bottomColor = pixelBottom ? CURSOR_COLOR_ON : CURSOR_COLOR_OFF;  // Red if ON, green if OFF
                } else {
                    bottomColor = pixelBottom ? 255 : 0;   // White if ON, black if OFF
                }
                
                // Use ▄ (lower half block): FG = bottom pixel, BG = top pixel
                Serial.print("\x1b[38;5;");
                Serial.print(bottomColor);
                Serial.print(";48;5;");
                Serial.print(topColor);
                Serial.print("m▄\x1b[0m");
            } else {
                // No cursor - use simple representation
                if (pixelTop && pixelBottom) {
                    Serial.print("█");  // Both on - full block
                } else if (pixelTop && !pixelBottom) {
                    Serial.print("▀");  // Top on - upper half
                } else if (!pixelTop && pixelBottom) {
                    Serial.print("▄");  // Bottom on - lower half
                } else {
                    Serial.print(" ");  // Both off - space
                }
            }
        }
        Serial.println();
    }
    Serial.flush();  // Flush output for snappier updates
}

void BitmapEditor::drawBitmapQuarterBlocks() {
    // Clear screen
    Serial.print("\x1b[2J\x1b[H");
    
    // Calculate visible area
    // Each terminal character represents 2×2 pixels
    int startX = scrollX / 2;
    int startY = scrollY / 2;
    int endX = min(startX + termWidth, (width + 1) / 2);
    int endY = min(startY + termHeight, (height + 1) / 2);
    
    // Draw bitmap using quarter blocks
    for (int by = startY; by < endY; by++) {
        for (int bx = startX; bx < endX; bx++) {
            int px = bx * 2;
            int py = by * 2;
            
            bool tl = getPixel(px, py);
            bool tr = getPixel(px + 1, py);
            bool bl = getPixel(px, py + 1);
            bool br = getPixel(px + 1, py + 1);
            
            // Check if cursor is in this 2x2 block
            bool hasCursor = (cursorX >= px && cursorX < px + 2 && 
                             cursorY >= py && cursorY < py + 2);
            
            if (hasCursor) {
                // Draw with crosshair - invert the cursor pixel, show others normally
                bool cursorTL = (cursorX == px && cursorY == py);
                bool cursorTR = (cursorX == px + 1 && cursorY == py);
                bool cursorBL = (cursorX == px && cursorY == py + 1);
                bool cursorBR = (cursorX == px + 1 && cursorY == py + 1);
                
                // Invert cursor pixel
                if (cursorTL) tl = !tl;
                if (cursorTR) tr = !tr;
                if (cursorBL) bl = !bl;
                if (cursorBR) br = !br;
            }
            
            // Draw the quarter block character
            const char* blockStr = getQuarterBlockString(tl, tr, bl, br);
            Serial.print(blockStr);
        }
        Serial.println();
    }
    Serial.flush();
}

void BitmapEditor::drawScreen() {
    // Check if scroll changed - if so, force full redraw
    if (scrollX != prevScrollX || scrollY != prevScrollY) {
        needFullRedraw = true;
        prevScrollX = scrollX;
        prevScrollY = scrollY;
    }
    
    if (needFullRedraw) {
        // Full redraw
        if (displayMode == MODE_FULL_BLOCKS) {
            drawBitmapFullBlocks();
        } else if (displayMode == MODE_HALF_BLOCKS) {
            drawBitmapHalfBlocks();
        } else {
            drawBitmapQuarterBlocks();
        }
        drawStatus();
        needFullRedraw = false;
    } else {
        // Incremental update
        drawScreenIncremental();
    }
    drawOLED();
}

void BitmapEditor::drawScreenIncremental() {
    // Only redraw lines that changed (cursor moved or pixel toggled)
    
    if (displayMode == MODE_HALF_BLOCKS) {
        // Calculate which character rows changed (each char row = 2 pixel rows)
        int prevCharRow = prevCursorY / 2;
        int currCharRow = cursorY / 2;
        
        int startCharRow = scrollY / 2;
        
        // Redraw previous cursor line if different from current
        if (prevCharRow != currCharRow && prevCharRow >= startCharRow && prevCharRow < startCharRow + termHeight) {
            int screenLine = prevCharRow - startCharRow;
            drawLine(screenLine);
        }
        
        // Redraw current cursor line
        if (currCharRow >= startCharRow && currCharRow < startCharRow + termHeight) {
            int screenLine = currCharRow - startCharRow;
            drawLine(screenLine);
        }
    } else if (displayMode == MODE_QUARTER_BLOCKS) {
        // Quarter block mode - each char is 2x2 pixels
        int prevCharRow = prevCursorY / 2;
        int currCharRow = cursorY / 2;
        
        int startCharRow = scrollY / 2;
        
        // Redraw previous cursor line if different from current
        if (prevCharRow != currCharRow && prevCharRow >= startCharRow && prevCharRow < startCharRow + termHeight) {
            int screenLine = prevCharRow - startCharRow;
            drawLine(screenLine);
        }
        
        // Redraw current cursor line
        if (currCharRow >= startCharRow && currCharRow < startCharRow + termHeight) {
            int screenLine = currCharRow - startCharRow;
            drawLine(screenLine);
        }
    } else {
        // Full block mode - just redraw the two lines
        int startY = scrollY;
        
        if (prevCursorY != cursorY && prevCursorY >= startY && prevCursorY < startY + termHeight) {
            int screenLine = prevCursorY - startY;
            drawLine(screenLine);
        }
        
        if (cursorY >= startY && cursorY < startY + termHeight) {
            int screenLine = cursorY - startY;
            drawLine(screenLine);
        }
    }
    
    // Redraw status line (only once!)
    Serial.print("\x1b[");
    Serial.print(termHeight + 1);
    Serial.print(";1H");
    drawStatus();
    
    // Update previous cursor position
    prevCursorX = cursorX;
    prevCursorY = cursorY;
}

void BitmapEditor::drawLine(int lineY) {
    // Build line in buffer first for faster output
    String lineBuffer = "";
    lineBuffer.reserve(512);  // Pre-allocate for performance
    
    // Position cursor at start of line
    lineBuffer = "\x1b[" + String(lineY + 1) + ";1H\x1b[K";
    
    if (displayMode == MODE_HALF_BLOCKS) {
        int startX = scrollX;
        int endX = min(startX + termWidth, width);
        int cy = scrollY / 2 + lineY;  // Character row
        int pyTop = cy * 2;
        int pyBottom = cy * 2 + 1;
        
        if (cy < height / 2) {
            for (int cx = startX; cx < endX; cx++) {
                int px = cx;
                
                bool pixelTop = getPixel(px, pyTop);
                bool pixelBottom = getPixel(px, pyBottom);
                
                // Check if cursor is on either pixel in this column
                bool cursorTop = (cursorX == px && cursorY == pyTop);
                bool cursorBottom = (cursorX == px && cursorY == pyBottom);
                bool hasCursor = cursorTop || cursorBottom;
                
                if (hasCursor) {
                    // Determine colors
                    int topColor = cursorTop ? (pixelTop ? CURSOR_COLOR_ON : CURSOR_COLOR_OFF) : (pixelTop ? 255 : 0);
                    int bottomColor = cursorBottom ? (pixelBottom ? CURSOR_COLOR_ON : CURSOR_COLOR_OFF) : (pixelBottom ? 255 : 0);
                    
                    // Use ▄: FG = bottom, BG = top
                    lineBuffer += "\x1b[38;5;" + String(bottomColor) + ";48;5;" + String(topColor) + "m▄\x1b[0m";
                } else {
                    // No cursor - simple display
                    if (pixelTop && pixelBottom) {
                        lineBuffer += "█";
                    } else if (pixelTop && !pixelBottom) {
                        lineBuffer += "▀";
                    } else if (!pixelTop && pixelBottom) {
                        lineBuffer += "▄";
                    } else {
                        lineBuffer += " ";
                    }
                }
            }
        }
    } else if (displayMode == MODE_QUARTER_BLOCKS) {
        int startX = scrollX / 2;
        int endX = min(startX + termWidth, (width + 1) / 2);
        int by = scrollY / 2 + lineY;
        
        if (by < (height + 1) / 2) {
            for (int bx = startX; bx < endX; bx++) {
                int px = bx * 2;
                int py = by * 2;
                
                bool tl = getPixel(px, py);
                bool tr = getPixel(px + 1, py);
                bool bl = getPixel(px, py + 1);
                bool br = getPixel(px + 1, py + 1);
                
                // Check if cursor is in this block and invert that pixel
                bool hasCursor = (cursorX >= px && cursorX < px + 2 && 
                                 cursorY >= py && cursorY < py + 2);
                
                if (hasCursor) {
                    bool cursorTL = (cursorX == px && cursorY == py);
                    bool cursorTR = (cursorX == px + 1 && cursorY == py);
                    bool cursorBL = (cursorX == px && cursorY == py + 1);
                    bool cursorBR = (cursorX == px + 1 && cursorY == py + 1);
                    
                    if (cursorTL) tl = !tl;
                    if (cursorTR) tr = !tr;
                    if (cursorBL) bl = !bl;
                    if (cursorBR) br = !br;
                }
                
                const char* blockStr = getQuarterBlockString(tl, tr, bl, br);
                lineBuffer += blockStr;
            }
        }
    } else {
        // Full block mode
        int startX = scrollX;
        int endX = min(startX + termWidth, width);
        int y = scrollY + lineY;
        
        if (y < height) {
            for (int x = startX; x < endX; x++) {
                bool pixel = getPixel(x, y);
                
                if (x == cursorX && y == cursorY) {
                    // Green for OFF, Red for ON
                    int color = pixel ? CURSOR_COLOR_ON : CURSOR_COLOR_OFF;
                    if (pixel) {
                        lineBuffer += "\x1b[38;5;" + String(color) + "m";
                    } else {
                        lineBuffer += "\x1b[48;5;" + String(color) + "m";
                    }
                    
                }
                
                lineBuffer += pixel ? "█" : " ";
                
                if (x == cursorX && y == cursorY) {
                    lineBuffer += "\x1b[0m";
                }
            }
        }
    }
    
    // Output entire line at once for better performance
    Serial.print(lineBuffer);
    Serial.flush();
}

void BitmapEditor::drawStatus() {
    // Update menu bar values to reflect current state
    menuViewModeValue = (int)displayMode;
    menuEncModeValue = (int)encoderMode;
    menuDrawModeValue = (int)drawMode;
    
    // Draw info line
    Serial.print("\x1b[48;5;243m\x1b[38;5;232m");  // Gray background, black text
    Serial.print(" ");
    Serial.print(filepath);
    Serial.print("   |   ");
    Serial.print(width);
    Serial.print("x");
    Serial.print(height);
    Serial.print("   |   (");
    Serial.print(cursorX);
    Serial.print(",");
    Serial.print(cursorY);
    Serial.print(")   |   ");
    Serial.print(modified ? "MODIFIED" : "Saved");
    Serial.print(" ");
    
    // Pad to full width
    int infoLen = filepath.length() + 50;  // Approximate
    for (int i = infoLen; i < 128; i++) {
        Serial.print(" ");
    }
    Serial.println("\x1b[0m");  // Normal video
    Serial.flush();
    
    // Draw interactive menu bar
    menuBar.draw(inMenuMode, menuSelectedIndex);

    // Draw concise help line
    changeTerminalColor(ENCODER_MENU_TEXT_COLOR, true, &Serial);
      Serial.print("\n\r⟨Clickwheel > ↺ / ↻: move H/V | Click: toggle pixel ⟩ ");
      changeTerminalColor(PROBE_MENU_TEXT_COLOR, true, &Serial);
                                                          Serial.println("⟨ Probe Buttons >  Connect:set | Remove:clear  | Switch > Select:H | Measure:V ⟩");
      changeTerminalColor(TERMINAL_MENU_TEXT_COLOR, true, &Serial);
    Serial.println("⟨Terminal >     [z]:set [x]:clear [c]:toggle pixel   | [m]:Cycle View | [/]: Enc H/V |  ctrl+S:Save  |    ctrl+Q:Quit    | [?]:Help  ⟩");
    changeTerminalColor(-1, true, &Serial);
    Serial.println();
    
    Serial.flush();  // Flush status line immediately
}

void BitmapEditor::drawOLED() {
    if (!oled.isConnected()) {
        return;
    }
    
    // Display the bitmap on OLED
    oled.clearFramebuffer();
    
    // Center the bitmap if it's smaller than display
    int xOffset = (oled.displayWidth - width) / 2;
    int yOffset = (oled.displayHeight - height) / 2;
    
    // Draw each pixel
    for (int y = 0; y < height && y < oled.displayHeight; y++) {
        for (int x = 0; x < width && x < oled.displayWidth; x++) {
            if (getPixel(x, y)) {
                oled.setPixel(x + xOffset, y + yOffset, SSD1306_WHITE);
            }
        }
    }
    
    // Draw 5-pixel crosshair cursor that inverts pixels
    if (cursorX >= 0 && cursorX < width && cursorY >= 0 && cursorY < height) {
        int cx = cursorX + xOffset;
        int cy = cursorY + yOffset;
        
        // Get framebuffer to invert pixels
        uint8_t* buffer = oled.getFramebuffer();
        if (buffer) {
            // Helper lambda to invert a pixel
            auto invertPixel = [&](int x, int y) {
                if (x >= 0 && x < oled.displayWidth && y >= 0 && y < oled.displayHeight) {
                    // Calculate buffer position
                    int page = y / 8;
                    int bit = y % 8;
                    int bufferIndex = page * oled.displayWidth + x;
                    
                    // Invert the bit
                    buffer[bufferIndex] ^= (1 << bit);
                }
            };
            
            // Draw crosshair (9 pixels total: center + 2 pixels in each cardinal direction)
            // Center pixel
            invertPixel(cx, cy);
            
            // Horizontal line (left and right 2 pixels each)
            invertPixel(cx - 1, cy);
            invertPixel(cx - 2, cy);
            invertPixel(cx + 1, cy);
            invertPixel(cx + 2, cy);
            
            // Vertical line (up and down 2 pixels each)
            invertPixel(cx, cy - 1);
            invertPixel(cx, cy - 2);
            invertPixel(cx, cy + 1);
            invertPixel(cx, cy + 2);
        }
    }
    
    oled.flushFramebuffer();
}

void BitmapEditor::moveCursor(int dx, int dy) {
    cursorX += dx;
    cursorY += dy;
    
    // Clamp to bitmap bounds
    if (cursorX < 0) cursorX = 0;
    if (cursorX >= width) cursorX = width - 1;
    if (cursorY < 0) cursorY = 0;
    if (cursorY >= height) cursorY = height - 1;
    
    updateScroll();
}

void BitmapEditor::updateScroll() {
    // Update scroll position to keep cursor visible
    int visibleWidth = termWidth;
    int visibleHeight = termHeight * 2;  // Default for half blocks
    
    if (displayMode == MODE_FULL_BLOCKS) {
        visibleHeight = termHeight;  // In full block mode, 1:1 mapping
    } else if (displayMode == MODE_QUARTER_BLOCKS) {
        visibleWidth = termWidth * 2;   // Each char is 2 pixels wide
        visibleHeight = termHeight * 2; // Each char is 2 pixels tall
    }
    
    // Horizontal scroll (same for both modes)
    if (cursorX < scrollX + 5) {
        scrollX = max(0, cursorX - 5);
    }
    if (cursorX > scrollX + visibleWidth - 5) {
        scrollX = min(width - visibleWidth, cursorX - visibleWidth + 5);
    }
    
    // Vertical scroll
    if (cursorY < scrollY + 3) {
        scrollY = max(0, cursorY - 3);
    }
    if (cursorY > scrollY + visibleHeight - 3) {
        scrollY = min(height - visibleHeight, cursorY - visibleHeight + 3);
    }
    
    // Keep scroll in bounds
    if (scrollX < 0) scrollX = 0;
    if (scrollY < 0) scrollY = 0;
}

void BitmapEditor::toggleCurrentPixel() {
    // Apply pixel operation based on current draw mode
    bool currentValue = getPixel(cursorX, cursorY);
    
    switch (drawMode) {
        case DRAW_SET:
            setPixel(cursorX, cursorY, true);  // Always set (draw)
            break;
        case DRAW_CLEAR:
            setPixel(cursorX, cursorY, false);  // Always clear (erase)
            break;
        case DRAW_TOGGLE:
        default:
            togglePixel(cursorX, cursorY);  // Toggle
            break;
    }
}

void BitmapEditor::switchDisplayMode() {
    // Clear the old display completely
    Serial.print("\x1b[2J\x1b[H");
    Serial.print("\x1b[0J");  // Clear from cursor to end of screen
    
    if (displayMode == MODE_FULL_BLOCKS) {
        displayMode = MODE_HALF_BLOCKS;
    } else if (displayMode == MODE_HALF_BLOCKS) {
        displayMode = MODE_QUARTER_BLOCKS;
    } else {
        displayMode = MODE_FULL_BLOCKS;
    }
    
    // Update terminal height based on new mode
    if (displayMode == MODE_FULL_BLOCKS) {
        termHeight = height;
    } else if (displayMode == MODE_HALF_BLOCKS) {
        termHeight = (height + 1) / 2;
    } else {
        termHeight = (height + 1) / 2;
    }
    
    updateScroll();
    needFullRedraw = true;  // Force full redraw after mode switch
}

void BitmapEditor::showHelp() {
    Serial.print("\x1b[2J\x1b[H");
    Serial.println("=== Bitmap Editor Help ===\n");
    Serial.println("Navigation:");
    Serial.println("  Encoder wheel       - Move cursor (H or V mode)");
    Serial.println("  Arrow keys / WASD   - Move cursor");
    Serial.println("  j/k/l (vim)         - Move cursor");
    Serial.println("  Down at bottom edge - Enter menu bar");
    Serial.println();
    Serial.println("Editing:");
    Serial.println("  Encoder click       - Apply current draw mode at cursor");
    Serial.println("  Enter / Space       - Apply current draw mode at cursor");
    Serial.println("  Connect button HOLD - Set pixels while held (draw lines)");
    Serial.println("  Remove button HOLD  - Clear pixels while held (erase lines)");
    Serial.println();
    Serial.println("Direct Pixel Actions (keyboard):");
    Serial.println("  z                   - Set pixel at cursor (draw)");
    Serial.println("  x                   - Clear pixel at cursor (erase)");
    Serial.println("  c                   - Toggle pixel at cursor");
    Serial.println();
    Serial.println("Draw Mode Control:");
    Serial.println("  .                   - Cycle draw modes (Toggle/Set/Clear)");
    Serial.println();
    Serial.println("Hardware Controls:");
    Serial.println("  Probe switch SELECT - Encoder horizontal movement");
    Serial.println("  Probe switch MEASURE- Encoder vertical movement");
    Serial.println();
    Serial.println("Display:");
    Serial.println("  m                   - Cycle view mode (Full/Half/Quarter)");
    Serial.println("  /                   - Toggle encoder H/V movement");
    Serial.println();
    Serial.println("Menu Bar (Down at bottom edge):");
    Serial.println("  Left/Right arrows   - Navigate menu items");
    Serial.println("  Enter / Space       - Activate menu item (cycle/Save/Quit)");
    Serial.println("  Up / Escape         - Exit menu bar");
    Serial.println();
    Serial.println("Menu Bar Items:");
    Serial.println("  View      - Cycle display mode (Full/Half/Quarter)");
    Serial.println("  Enc       - Toggle encoder direction (H/V)");
    Serial.println("  Draw      - Cycle draw mode (Toggle/Set/Clear)");
    Serial.println("  «Save»    - [Button] Save file and exit menu");
    Serial.println("  «Quit»    - [Button] Quit editor (prompts if modified)");
    Serial.println();
    Serial.println("File:");
    Serial.println("  Ctrl+S              - Save file");
    Serial.println("  Ctrl+Q / ESC        - Quit (prompts if modified)");
    Serial.println("  h / ?               - Show this help");
    Serial.println();
    Serial.println("Cursor Colors:");
    Serial.println("  Green background - Pixel is OFF");
    Serial.println("  Red background   - Pixel is ON");
    Serial.println();
    Serial.println("Press any key to continue...");
    Serial.flush();
    
    while (!Serial.available()) {
        delay(10);
    }
    Serial.read();
    needFullRedraw = true;  // Force full redraw after help
}

bool BitmapEditor::save() {
    // CRITICAL: Pause Core2 during flash write operations
    bool was_paused = pauseCore2ForFlash(100);
    
    File file = safeFileOpen(filepath.c_str(), "w", 1000);
    if (!file) {
        unpauseCore2ForFlash(was_paused);
        Jerial.println("Failed to open file for writing");
        return false;
    }
    
    // Write header if original file had one
    if (hasHeader) {
        file.write((uint8_t)(width & 0xFF));
        file.write((uint8_t)((width >> 8) & 0xFF));
        file.write((uint8_t)(height & 0xFF));
        file.write((uint8_t)((height >> 8) & 0xFF));
    }
    
    // Write bitmap data
    size_t written = file.write(bitmapData, dataSize);
    safeFileClose(file, true);  // Write mode, needs flush
    unpauseCore2ForFlash(was_paused);
    
    if (written == dataSize) {
        modified = false;
        Jerial.println("File saved successfully");
        return true;
    } else {
        Jerial.println("Failed to write all data");
        return false;
    }
}

void BitmapEditor::handleInput(int ch) {
    // If in menu mode, handle menu navigation
    if (inMenuMode) {
        handleMenuNavigation(ch);
        return;
    }
    
    // Handle arrow keys (they come as single chars A/B/C/D after ESC [)
    // Handle movement keys w/a/s/d and vim keys j/k/l (removed h for help)
    
    // Up: 'A' (arrow) or 'w' or 'k'
    if (ch == 'A' || ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
        moveCursor(0, -1);
        return;
    }
    
    // Down: 'B' (arrow) or 's' or 'j'
    // Check if cursor is at bottom - if so, enter menu mode
    if (ch == 'B' || ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
        if (cursorY >= height - 1) {
            // At bottom edge - enter menu mode
            inMenuMode = true;
            menuSelectedIndex = 0;
            needFullRedraw = true;
        } else {
            moveCursor(0, 1);
        }
        return;
    }
    
    // Left: 'D' (arrow) or 'a'
    if (ch == 'D' || ch == 'a') {
        moveCursor(-1, 0);
        return;
    }
    
    // Right: 'C' (arrow) or 'd' or 'l'
    if (ch == 'C' || ch == 'd' || ch == 'l' || ch == 'L') {
        moveCursor(1, 0);
        return;
    }
    switch (ch) {
        
        // Toggle pixel
        case '\r':
        case '\n':
        case ' ':
            toggleCurrentPixel();
            break;
        
        // Switch display mode
        case 'm':
        case 'M':
            switchDisplayMode();
            break;
        
        // Toggle encoder direction (horizontal/vertical)
        case '/':
            encoderMode = (encoderMode == ENCODER_HORIZONTAL) ? ENCODER_VERTICAL : ENCODER_HORIZONTAL;
            needFullRedraw = true;
            break;
        
        // Cycle draw mode (toggle -> set -> clear -> toggle)
        case '.':
            if (drawMode == DRAW_TOGGLE) {
                drawMode = DRAW_SET;
            } else if (drawMode == DRAW_SET) {
                drawMode = DRAW_CLEAR;
            } else {
                drawMode = DRAW_TOGGLE;
            }
            needFullRedraw = true;
            break;
        
        // Direct pixel manipulation (not mode toggles)
        case 'z':
        case 'Z':
            // Set pixel at cursor (draw)
            setPixel(cursorX, cursorY, true);
            break;
        
        case 'x':
        case 'X':
            // Clear pixel at cursor (erase)
            setPixel(cursorX, cursorY, false);
            break;
        
        case 'c':
        case 'C':
            // Toggle pixel at cursor
            togglePixel(cursorX, cursorY);
            break;
        
        // Save (Ctrl+S)
        case 19:
            save();
            return;  // Return immediately after handling
        
        // Help
        case '?':
        case 'h':
        case 'H':
            showHelp();
            return;
        
        // Quit (ESC or Ctrl+Q)
        case 27:
        case 17:
            if (modified) {
                Serial.println("\nFile has unsaved changes. Save before quitting? (y/n/c): ");
                Serial.flush();
                // Wait for response
                unsigned long timeout = millis() + 30000; // 30 second timeout
                while (!Serial.available() && millis() < timeout) {
                    delay(10);
                }
                if (Serial.available()) {
                    char response = Serial.read();
                    // Consume any extra characters (like newline)
                    while (Serial.available()) Serial.read();
                    
                    if (response == 'y' || response == 'Y' || response == 19) {
                        save();
                        running = false;
                    } else if (response == 'n' || response == 'N' || response == 27 || response == 17) {
                        running = false;
                    } else {
                        needFullRedraw = true;   // 'c' / anything else: stay (the prompt overwrote the canvas)
                    }
                } else {
                    needFullRedraw = true;       // timed out: stay, keep the edits
                }
            } else {
                running = false;                 // nothing to lose
            }
            return;
    }
}

void BitmapEditor::run() {
    active = true;
    if (!bitmapData) {
        Jerial.println("No bitmap loaded");
        active = false;
        return;
    }
    
    // Query terminal size first
    queryTerminalSize();
    
    // Set terminal height based on display mode
    // We have 2 lines for status + menu, so available height = termHeight - 2
    // Set to match image height (will adjust based on display mode)
    if (displayMode == MODE_FULL_BLOCKS) {
        termHeight = height;  // 1:1 mapping
    } else if (displayMode == MODE_HALF_BLOCKS) {
        termHeight = (height + 1) / 2;  // 2 pixels per char vertically
    } else {
        termHeight = (height + 1) / 2;  // 2x2 pixels per char
    }
    
    running = true;
    cursorX = width / 2;
    cursorY = height / 2;
    prevCursorX = cursorX;
    prevCursorY = cursorY;
    prevScrollX = 0;
    prevScrollY = 0;
    needFullRedraw = true;
    updateScroll();
    
    // Setup menu bar
    setupMenuBar();
    inMenuMode = false;
    menuSelectedIndex = 0;
    
    // Initialize encoder tracking (like eKilo)
    last_encoder_position = encoderPosition;
    last_encoder_update = millis();
    last_button_state = digitalRead(BUTTON_ENC);
    button_debounce_time = millis();
    lastSwitchCheck = millis();
    
    // Clear serial input buffer
    while (Serial.available()) {
        Serial.read();
    }
    
    drawScreen();
    
    bool screen_dirty = false;
    int lastSwitchPos = -1;

    // Long-hold quit classifier (synced to the pin so the click that
    // launched the editor isn't misread as a fresh press)
    EncoderClickTracker clickTracker;
    clickTracker.reset();
    
    while (running) {
        // Run critical services (probe button, menus, etc.)
        jOS.serviceInner();
        

        // Switch now controls encoder direction (H/V)

            probeSwitch.service();
            int currentSwitchPos = switchPosition;  // Read global switch position (updated by ProbeSwitch service)
            if (currentSwitchPos != lastSwitchPos) {
            // Update encoder mode based on switch position
            if (currentSwitchPos == 1) {
                // Select position = horizontal encoder
                if (encoderMode != ENCODER_HORIZONTAL) {
                    encoderMode = ENCODER_HORIZONTAL;
                    screen_dirty = true;
                }
            } else if (currentSwitchPos == 0) {
                // Measure position = vertical encoder
                if (encoderMode != ENCODER_VERTICAL) {
                    encoderMode = ENCODER_VERTICAL;
                    screen_dirty = true;
                }
            }
            }
            lastSwitchPos = currentSwitchPos;
         
        
        
        // Check probe buttons: connect = set pixel, remove = clear pixel
        // Use button STATE (held down) not press event, so it only draws while held
        int probeBtnState = probeButton.getButtonState();
        if (probeBtnState == connectPress) {
            // Connect button held = set pixel at cursor
            setPixel(cursorX, cursorY, true);
            screen_dirty = true;
        } else if (probeBtnState == disconnectPress) {
            // Disconnect/Remove button held = clear pixel at cursor
            setPixel(cursorX, cursorY, false);
            screen_dirty = true;
        }
        
        // Process encoder input (like eKilo)
        processEncoderInput();

        // Long hold = hardware Ctrl-Q: save and quit from anywhere. (This
        // used to live in handleInput(), where it only ran when a serial
        // key happened to arrive - the hold did nothing on its own.)
        if (clickTracker.poll() == ENC_LONG_HOLD) {
            save();
            // Swallow the rest of the hold so the caller / main screen
            // doesn't see it as its own hold gesture (3s bail-out).
            unsigned long releaseWaitStart = millis();
            while (isEncoderButtonPhysicallyPressed() &&
                   millis() - releaseWaitStart < 3000) {
                delayMicroseconds(500);
            }
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            running = false;
            continue;
        }

        // Process serial keyboard input
        if (Serial.available()) {
            int ch = Serial.read();
            
            // Handle escape sequences for arrow keys
            if (ch == 27) {  // ESC
                delay(10);
                if (Serial.available() && Serial.read() == '[') {
                    if (Serial.available()) {
                        ch = Serial.read();  // A/B/C/D for arrows
                    }
                } else {
                    ch = 27;  // Just ESC key
                }
            }
            
            handleInput(ch);
            screen_dirty = true;
        }
        
        // Only redraw if something changed
        if (screen_dirty) {
            drawScreen();
            screen_dirty = false;
        }
        
        //delay(10);
    }
    
    // Clear entire screen including status area before exiting
    Serial.print("\x1b[2J\x1b[H");
    Serial.print("\x1b[0J");  // Clear from cursor to end of screen
    
    active = false;  // Mark editor as inactive
}

// Process encoder input for cursor movement (like eKilo)
void BitmapEditor::processEncoderInput() {
    unsigned long currentTime = millis();
    int cursor_moved = 0;
    // Read current encoder position
    long currentPosition = encoderPosition;
    
    // Calculate position delta
    long deltaPosition = (currentPosition - last_encoder_position) / 4;
    deltaPosition = -deltaPosition;  // Invert for natural scrolling
    
    // Only process if there's a significant change and enough time has passed
    if (deltaPosition != 0 && (currentTime - last_encoder_update >= 10)) {
        last_encoder_position = currentPosition;
        last_encoder_update = currentTime;
        
        // Use delta for responsive cursor movement
        int steps = max(1, min(abs(deltaPosition), 4));
        int direction = (deltaPosition > 0) ? 1 : -1;
        
        for (int i = 0; i < steps; i++) {
            if (encoderMode == ENCODER_HORIZONTAL) {
                // Horizontal movement
                if (direction > 0) {
                    moveCursor(1, 0);
                    cursor_moved = 1;
                } else {
                    moveCursor(-1, 0);
                    cursor_moved = -1;
                }
            } else {
                // Vertical movement
                if (direction > 0) {
                    moveCursor(0, 1);
                    cursor_moved = 1;
                } else {
                    moveCursor(0, -1);
                    cursor_moved = -1;
                }
            }
        }
        
        drawScreen();  // Redraw after encoder movement
    }
    
    // Handle button press with debouncing (like eKilo)
    bool current_button_state = digitalRead(BUTTON_ENC);
    
    // Check for button press (HIGH to LOW transition) with debouncing
    if (!current_button_state && last_button_state && (currentTime - button_debounce_time > 10)) {
        button_debounce_time = currentTime;
        
        // Toggle pixel at cursor
        toggleCurrentPixel();
        drawScreen();
    } else if (!current_button_state && cursor_moved != 0) {
        toggleCurrentPixel();
        drawScreen();
    }
    
    last_button_state = current_button_state;
}

void BitmapEditor::queryTerminalSize() {
    // Query terminal size using ANSI escape codes
    // Send cursor position query
    Serial.print("\x1b[999;999H");  // Move cursor to far bottom-right
    Serial.print("\x1b[6n");        // Query cursor position
    Serial.flush();
    
    // Wait for response: ESC[rows;colsR
    unsigned long timeout = millis() + 500;
    String response = "";
    
    while (millis() < timeout) {
        if (Serial.available()) {
            char c = Serial.read();
            response += c;
            if (c == 'R') break;  // End of response
        }
        delay(1);
    }
    
    // Parse response: ESC[rows;colsR
    if (response.startsWith("\x1b[") && response.endsWith("R")) {
        int semicolon = response.indexOf(';');
        if (semicolon > 0) {
            String rowsStr = response.substring(2, semicolon);
            String colsStr = response.substring(semicolon + 1, response.length() - 1);
            
            int rows = rowsStr.toInt();
            int cols = colsStr.toInt();
            
            if (rows > 0 && cols > 0) {
                termHeight = rows - 2;  // Leave room for status bar
                termWidth = cols;
                Jerial.print("Terminal size detected: ");
                Jerial.print(cols);
                Jerial.print("×");
                Jerial.println(rows);
            }
        }
    }
    
    // Fallback to reasonable defaults if query failed
    if (termWidth < 40 || termWidth > 300) {
        termWidth = 128;
    }
    if (termHeight < 10 || termHeight > 100) {
        termHeight = 22;
    }
}

void BitmapEditor::setupMenuBar() {
    menuBar.clear();
    
    // Update member variables with current state
    menuViewModeValue = (int)displayMode;
    menuEncModeValue = (int)encoderMode;
    menuDrawModeValue = (int)drawMode;
    
    // Add menu items (we don't use callbacks - just check which item in handleMenuNavigation)
    static const char* viewModes[3] = {"Full", "Half", "Qtr"};
    menuBar.addCycle("View", &menuViewModeValue, viewModes, 3, nullptr);
    
    static const char* encModes[2] = {"H", "V"};
    menuBar.addCycle("Enc", &menuEncModeValue, encModes, 2, nullptr);
    
    static const char* drawModes[3] = {"TGL", "SET", "CLR"};
    menuBar.addCycle("Draw", &menuDrawModeValue, drawModes, 3, nullptr);
    
    // Save action (callbacks won't work, handle in handleMenuNavigation)
    menuBar.addAction("Save", nullptr);
    
    // Quit action
    menuBar.addAction("Quit", nullptr);
}

void BitmapEditor::handleMenuNavigation(int ch) {
    if (!inMenuMode) {
        return;
    }
    
    // Navigate menu with left/right
    if (ch == 'D' || ch == 'a') {  // Left
        menuSelectedIndex--;
        if (menuSelectedIndex < 0) {
            menuSelectedIndex = menuBar.getItemCount() - 1;
        }
        needFullRedraw = true;
    } else if (ch == 'C' || ch == 'd' || ch == 'l' || ch == 'L') {  // Right
        menuSelectedIndex++;
        if (menuSelectedIndex >= menuBar.getItemCount()) {
            menuSelectedIndex = 0;
        }
        needFullRedraw = true;
    } else if (ch == '\r' || ch == '\n' || ch == ' ') {  // Activate
        // Handle menu item activation manually
        const MenuItem& item = menuBar.getItem(menuSelectedIndex);
        
        if (strcmp(item.label, "View") == 0) {
            // Cycle view mode (switchDisplayMode advances it itself; the
            // activateItem call advanced it a second time, so View cycled
            // backwards and the label was momentarily wrong)
            switchDisplayMode();
        } else if (strcmp(item.label, "Enc") == 0) {
            // Cycle encoder mode
            menuBar.activateItem(menuSelectedIndex);
            encoderMode = (EncoderMovementMode)menuEncModeValue;
        } else if (strcmp(item.label, "Draw") == 0) {
            // Cycle draw mode
            menuBar.activateItem(menuSelectedIndex);
            drawMode = (DrawMode)menuDrawModeValue;
        } else if (strcmp(item.label, "Save") == 0) {
            // Save file
            save();
            inMenuMode = false;  // Exit menu after save
        } else if (strcmp(item.label, "Quit") == 0) {
            // Quit
            if (modified) {
                Serial.println("\nSave before quitting? (y/n): ");
                Serial.flush();
                unsigned long timeout = millis() + 30000;
                while (!Serial.available() && millis() < timeout) {
                    delay(10);
                }
                if (Serial.available()) {
                    char response = Serial.read();
                    while (Serial.available()) Serial.read();
                    if (response == 'y' || response == 'Y') {
                        save();
                    }
                }
            }
            running = false;
        }
        
        needFullRedraw = true;
    } else if (ch == 27 || ch == 'A' || ch == 'w' || ch == 'W') {  // Exit menu mode (up/escape)
        inMenuMode = false;
        needFullRedraw = true;
    }
}

// Public function to launch bitmap editor
bool launchBitmapEditor(const String& filepath) {
    BitmapEditor& editor = BitmapEditor::getInstance();
    editor.active = true;
    
    if (!editor.loadFile(filepath)) {
        editor.active = false;
        return false;
    }
    
    // save/override only on the path that reaches the restore below
    unsigned long originalInterval = ProbeSwitch::getInstance().interval_ms;
    ProbeSwitch::getInstance().interval_ms = 300;
    editor.run();
    editor.active = false;
    ProbeSwitch::getInstance().interval_ms = originalInterval;
    return true;
}

