/**
 * Images App - OLED Image Viewer
 * 
 * Browse and display images stored in /images folder on the filesystem.
 * Supports .bin format bitmaps compatible with Adafruit GFX.
 */

#include "ImagesApp.h"
#include "Jerial.h"
#include "JumperlessDefines.h"
#include "Menus.h"
#include "oled.h"
#include "Peripherals.h"
#include "RotaryEncoder.h"
#include <FatFS.h>
#include <cmath>
#include <cstring>
#include "FilesystemStuff.h"  // For safe file operations

// Display is accessed via getDisplay() from oled.h

// Image viewer state
static int currentImageIndex = 0;
static int totalImages = 0;
static String imageFiles[32];  // Support up to 32 images
static const char* IMAGES_DIR = "/images";

/**
 * Scan /images directory and populate image list
 */
void scanImageDirectory() {
    totalImages = 0;
    
    if (!safeFileExists(IMAGES_DIR, 500)) {
        Jerial.println("Images directory not found. Creating /images...");
        safeMkdir(IMAGES_DIR, 1000);
    }
    
    Dir dir = FatFS.openDir(IMAGES_DIR);
    
    while (dir.next() && totalImages < 32) {
        String filename = dir.fileName();
        
        // Skip hidden files (starting with .)
        if (filename.startsWith(".")) {
            continue;
        }
        
        // Check if it's a .bin file
        if (filename.endsWith(".bin")) {
            imageFiles[totalImages] = filename;
            totalImages++;
        }
    }
    
    Jerial.print("Found ");
    Jerial.print(totalImages);
    Jerial.println(" images in /images directory");
}

/**
 * Load and display an OLED image from filesystem
 */
bool loadAndDisplayImage(const char* filename) {
    // Strip any existing path prefix to get just the filename
    String filenameStr = String(filename);
    
    // Remove "/images/" prefix if present
    if (filenameStr.startsWith("/images/")) {
        filenameStr = filenameStr.substring(8); // "/images/" is 8 chars
    } else if (filenameStr.startsWith("images/")) {
        filenameStr = filenameStr.substring(7); // "images/" is 7 chars
    }
    
    // Now build the full path cleanly
    String fullPath = String(IMAGES_DIR) + "/" + filenameStr;

    if (!safeFileExists(fullPath.c_str(), 500)) {
        Jerial.print("Image not found: ");
        Jerial.println(fullPath);
        return false;
    }
    
    File file = safeFileOpen(fullPath.c_str(), "r", 1000);
    if (!file) {
        Jerial.println("Failed to open image file");
        return false;
    }
    
    size_t fileSize = file.size();
    
    // Read header if present (4 bytes)
    int width = 128;
    int height = 32;
    uint8_t* bitmapData = nullptr;
    size_t bitmapSize = 0;
    
    if (fileSize >= 4) {
        // Try reading as format with header
        uint8_t wl = file.read();
        uint8_t wh = file.read();
        uint8_t hl = file.read();
        uint8_t hh = file.read();
        
        int testWidth = wl | (wh << 8);
        int testHeight = hl | (hh << 8);
        
        // Check if dimensions are valid
        if (testWidth > 0 && testWidth <= 128 && testHeight > 0 && testHeight <= 64) {
            size_t expectedSize = (testWidth * testHeight + 7) / 8;
            if (fileSize - 4 == expectedSize) {
                // Has header
                width = testWidth;
                height = testHeight;
                bitmapSize = expectedSize;
            } else {
                // No header, rewind
                file.seek(0);
                bitmapSize = fileSize;
            }
        } else {
            // No header, rewind
            file.seek(0);
            bitmapSize = fileSize;
        }
    } else {
        bitmapSize = fileSize;
    }
    
    // Guess dimensions from size if no header
    if (fileSize == bitmapSize) {
        if (bitmapSize == 512) {
            width = 128;
            height = 32;
        } else if (bitmapSize == 1024) {
            width = 128;
            height = 64;
        } else if (bitmapSize == 256) {
            width = 64;
            height = 32;
        } else if (bitmapSize == 496) {
            width = 128;
            height = 31;
        }
    }
    
    // Allocate buffer (limit to 1KB)
    if (bitmapSize > 32768) {
        Jerial.println("Image too large");
        safeFileClose(file, false);
        return false;
    }
    
    // Both draw paths below walk width x height rows out of this buffer, and a
    // file whose size isn't one of the guessed dimensions (or whose header
    // width isn't byte-aligned) is shorter than that. Size the allocation to
    // what the renderer touches and zero the tail, so short files show blank
    // rows instead of reading off the end of the heap.
    size_t drawBytes = (size_t)((width + 7) / 8) * (size_t)height;
    size_t allocSize = (drawBytes > bitmapSize) ? drawBytes : bitmapSize;

    bitmapData = new uint8_t[allocSize];
    if (!bitmapData) {
        Jerial.println("Failed to allocate memory");
        safeFileClose(file, false);
        return false;
    }
    memset(bitmapData, 0, allocSize);
    
    // Read bitmap data
    size_t bytesRead = file.readBytes((char*)bitmapData, bitmapSize);
    safeFileClose(file, false);
    
    if (bytesRead != bitmapSize) {
        Jerial.println("Failed to read image data");
        delete[] bitmapData;
        return false;
    }
    
    // Display on OLED
    // Clear display completely first to prevent garbage
    getDisplay().clearDisplay();
    
    // Check if bitmap needs padding to match display dimensions
    bool needsPadding = (width != oled.displayWidth || height != oled.displayHeight);
    
    if (needsPadding) {
        // Create padded buffer matching display dimensions
        size_t paddedSize = (oled.displayWidth * oled.displayHeight + 7) / 8;
        uint8_t* paddedData = new uint8_t[paddedSize];
        
        if (paddedData) {
            // Initialize padded buffer with zeros (black pixels)
            memset(paddedData, 0, paddedSize);
            
            // Calculate centering offsets
            int xOffset = (oled.displayWidth - width) / 2;
            int yOffset = (oled.displayHeight - height) / 2;
            
            // Copy bitmap data into padded buffer with offsets
            size_t srcBytesPerRow = (width + 7) / 8;
            size_t dstBytesPerRow = (oled.displayWidth + 7) / 8;
            
            for (int row = 0; row < height; row++) {
                int srcRow = row;
                int dstRow = row + yOffset;
                
                if (dstRow >= 0 && dstRow < oled.displayHeight) {
                    // Simple row copy for byte-aligned widths
                    if (xOffset == 0 && width % 8 == 0) {
                        memcpy(paddedData + dstRow * dstBytesPerRow,
                               bitmapData + srcRow * srcBytesPerRow,
                               srcBytesPerRow);
                    } else {
                        // Bit-level copy for non-aligned offsets (simplified for now)
                        // Just center horizontally by byte boundary
                        size_t dstByteOffset = (xOffset / 8);
                        if (dstByteOffset + srcBytesPerRow <= dstBytesPerRow) {
                            memcpy(paddedData + dstRow * dstBytesPerRow + dstByteOffset,
                                   bitmapData + srcRow * srcBytesPerRow,
                                   srcBytesPerRow);
                        }
                    }
                }
            }
            
            // Display padded bitmap
            getDisplay().drawBitmap(0, 0, paddedData, oled.displayWidth, oled.displayHeight, SSD1306_WHITE);
            getDisplay().display();
            
            delete[] paddedData;
            delete[] bitmapData;
            
            Jerial.print("Padded from ");
            Jerial.print(width);
            Jerial.print("x");
            Jerial.print(height);
            Jerial.print(" to ");
            Jerial.print(oled.displayWidth);
            Jerial.print("x");
            Jerial.println(oled.displayHeight);
            Jerial.flush();
        } else {
            Jerial.println("Failed to allocate padding buffer");
            // Fall back to centering without padding
            int x = (oled.displayWidth - width) / 2;
            int y = (oled.displayHeight - height) / 2;
            getDisplay().drawBitmap(x, y, bitmapData, width, height, SSD1306_WHITE);
            getDisplay().display();
            delete[] bitmapData;
        }
    } else {
        // No padding needed, display as-is
        getDisplay().drawBitmap(0, 0, bitmapData, width, height, SSD1306_WHITE);
        getDisplay().display();
        delete[] bitmapData;
    }
    
    Jerial.print("Displayed: ");
    Jerial.print(filename);
    Jerial.print(" (");
    Jerial.print(width);
    Jerial.print("x");
    Jerial.print(height);
    Jerial.println(")");
    Jerial.flush();
    return true;
}

/**
 * Interactive image selector for menu configuration (wrapper)
 */
String selectImageFromMenu(void) {
    Jerial.println("selectImageFromMenu called");
    Jerial.flush();
    return imagesApp(true); // Call imagesApp in selection mode
}

/**
 * Wrapper for Apps menu - discards return value
 */
void imagesAppLauncher(void) {
    imagesApp(false); // Launch in viewer mode, discard return
}

/**
 * Main Images App
 */
String imagesApp(bool selectionMode) {
    // Jerial.print("imagesApp called with selectionMode=");
    // Jerial.println(selectionMode);
    // Jerial.print("OLED isConnected: ");
    // Jerial.println(oled.isConnected());
    // Jerial.flush();
    
    // if (!oled.isConnected()) {
    //     Jerial.println("OLED not connected - returning empty string");
    //     Jerial.flush();
    //     return String("");
    // }
    
    if (selectionMode) {
        Jerial.println("\n=== Select Image ===");
        Jerial.println("Rotary: Navigate | Short Click: Select | Long Press: Cancel");
        Jerial.flush();
    } else {
        Jerial.println("\n=== Images App ===");
        Jerial.println("Rotary: Navigate | Long Press: Exit");
        Jerial.flush();
    }
    
    // Scan for images
    scanImageDirectory();
    
    if (totalImages == 0) {
        oled.clearPrintShow("No images\nfound", 2, true, true, true);
        delay(1000);
        return String("");
    }
    
    // Display first image
    currentImageIndex = 0;
    loadAndDisplayImage(imageFiles[currentImageIndex].c_str());
    
    long lastEncoderPosition = encoderPosition;
    unsigned long lastUpdate = millis();
    const unsigned long SCROLL_DEBOUNCE = 10; // ms between image changes
    const int ENCODER_THRESHOLD = 8; // Require 3 clicks to change image
    int accumulatedDelta = 0;
    
    // Wait for button to be released if in selection mode (menu context)
    if (selectionMode) {
        //Jerial.println("Waiting for button release from menu...");
        // Wait for physical button release
      //  while (digitalRead(BUTTON_ENC) == LOW) {
         //   delay(10);
       // }
       // delay(100); // Debounce
        
        // Reset encoder state after physical release
        encoderButtonState = IDLE;
        lastButtonEncoderState = IDLE;
        
        //Jerial.println("Button released, starting selection");
    }
    delay(1000);
    while (true) {
        // CRITICAL: Update encoder state machine
        //rotaryEncoderStuff();
        jOS.serviceInner();
        
        // Check for encoder rotation with debouncing and threshold
        if (encoderPosition != lastEncoderPosition) {
            int delta = encoderPosition - lastEncoderPosition;
            lastEncoderPosition = encoderPosition;
            
            // Accumulate encoder steps
            accumulatedDelta += delta;
            
            // Only change image when threshold is reached and debounce time has passed
            if (abs(accumulatedDelta) >= ENCODER_THRESHOLD && millis() - lastUpdate > SCROLL_DEBOUNCE) {
                // Navigate through images
                if (accumulatedDelta > 0) {
                    currentImageIndex++;
                } else if (accumulatedDelta < 0) {
                    currentImageIndex--;
                }
                
                // Reset accumulated delta
                accumulatedDelta = 0;
                
                if (currentImageIndex < 0) currentImageIndex = totalImages - 1;
                if (currentImageIndex >= totalImages) currentImageIndex = 0;
                
                // Load and display new image
                loadAndDisplayImage(imageFiles[currentImageIndex].c_str());
                
                lastUpdate = millis();
            }
        }
        //rotaryEncoderStuff();
        
        // Selection mode: Use proper state machine pattern
        if (selectionMode) {


            // Serial.print("encoderButtonState: ");
            // Serial.println(encoderButtonState);
            // Serial.print(" lastButtonEncoderState: ");
            // Serial.println(lastButtonEncoderState);
            // Serial.println();
            // Serial.flush();
            // Check for long press/HELD (cancel)
            if (encoderButtonState == HELD) {
                //Jerial.println("Image selection canceled (long press)");
                oled.clear();
                oled.showJogo32h();
                return String("");
            }
            
            // Check for short press (select)
            if (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED) {
                // Return full path: /images/filename.bin
                String selectedFile = String(IMAGES_DIR) + "/" + imageFiles[currentImageIndex];
                
                // Jerial.print("Selected image (full path): ");
                // Jerial.println(selectedFile);
                
                oled.clear();
                oled.showJogo32h();
                
                return selectedFile;
            }
        } else {
            // Viewer mode: Manual long press tracking
            static unsigned long buttonHoldStart = 0;
            static bool wasPressed = false;
            
            if (encoderButtonState == PRESSED && !wasPressed) {
                wasPressed = true;
                buttonHoldStart = millis();
            }
            
            if (encoderButtonState == RELEASED || encoderButtonState == IDLE) {
                wasPressed = false;
            }
            
            if (wasPressed && (millis() - buttonHoldStart > 1000)) {
                //Jerial.println("Exiting Images App");
                break;
            }
        }
        
        delay(1);
    }
    
    // Clear display on exit
    oled.clear();
    oled.showJogo32h();
    
    return String(""); // Empty string for cancel or normal viewer exit
}






void printColorJogo(void) {
   Jerial.println ("\033[49m                                                                                                                                                                                                                                                \033[48;5;0m          \033[49m      \033[m");
Jerial.println ("\033[49m                                                                                                                                                                                              \033[48;5;0m        \033[49m              \033[48;5;0m              \033[49m        \033[48;5;0m      \033[48;5;202m          \033[48;5;0m  \033[49m    \033[m");
Jerial.println ("\033[49m                                                                                                                                          \033[48;5;0m          \033[49m            \033[48;5;0m      \033[49m                    \033[48;5;0m    \033[48;5;11m        \033[48;5;0m    \033[49m      \033[48;5;0m    \033[48;5;214m            \033[48;5;0m    \033[49m    \033[48;5;0m  \033[48;5;202m                  \033[48;5;0m  \033[49m  \033[m");
Jerial.println ("\033[49m                                                                                        \033[48;5;0m          \033[49m                \033[48;5;0m          \033[49m      \033[48;5;0m        \033[48;5;49m          \033[48;5;0m    \033[49m      \033[48;5;0m  \033[48;5;154m      \033[48;5;0m    \033[49m            \033[48;5;0m    \033[48;5;11m              \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;214m                  \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;202m                    \033[48;5;0m    \033[m");
Jerial.println ("\033[49m                \033[48;5;0m      \033[49m                    \033[48;5;0m      \033[49m                      \033[48;5;0m      \033[49m      \033[48;5;0m      \033[48;5;63m          \033[48;5;0m  \033[49m        \033[48;5;0m      \033[48;5;39m          \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;49m                    \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;154m          \033[48;5;0m  \033[49m          \033[48;5;0m  \033[48;5;11m                    \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;214m                    \033[48;5;0m    \033[48;5;202m                      \033[48;5;0m  \033[m");
Jerial.println ("\033[49m          \033[48;5;0m  \033[48;5;197m          \033[48;5;0m      \033[48;5;199m    \033[48;5;0m  \033[49m      \033[48;5;0m  \033[48;5;199m      \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;165m    \033[48;5;0m  \033[49m      \033[48;5;0m  \033[48;5;165m        \033[48;5;0m    \033[48;5;63m                    \033[48;5;0m      \033[48;5;39m                    \033[48;5;0m  \033[48;5;49m                          \033[48;5;0m  \033[48;5;154m            \033[48;5;0m    \033[49m    \033[48;5;0m    \033[48;5;11m                    \033[48;5;0m    \033[48;5;214m          \033[48;5;0m    \033[48;5;214m          \033[48;5;0m  \033[48;5;202m        \033[48;5;0m        \033[48;5;202m        \033[48;5;0m  \033[m");
Jerial.println ("\033[49m        \033[48;5;0m  \033[48;5;197m              \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m                        \033[48;5;0m  \033[48;5;39m                      \033[48;5;0m  \033[48;5;49m      \033[48;5;0m          \033[48;5;49m          \033[48;5;0m  \033[48;5;154m            \033[48;5;0m    \033[49m    \033[48;5;0m  \033[48;5;11m                  \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;214m        \033[48;5;0m        \033[48;5;214m        \033[48;5;0m  \033[48;5;202m        \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;202m      \033[48;5;0m    \033[m");
Jerial.println ("\033[49m        \033[48;5;0m  \033[48;5;197m              \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m        \033[48;5;0m        \033[48;5;63m        \033[48;5;0m  \033[48;5;39m                    \033[48;5;0m    \033[48;5;49m        \033[48;5;0m          \033[48;5;49m        \033[48;5;0m  \033[48;5;154m            \033[48;5;0m    \033[49m    \033[48;5;0m  \033[48;5;11m          \033[48;5;0m        \033[49m      \033[48;5;0m  \033[48;5;214m        \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;214m      \033[48;5;0m    \033[48;5;202m        \033[48;5;0m    \033[49m    \033[48;5;0m        \033[49m  \033[m");
Jerial.println ("\033[49m        \033[48;5;0m  \033[48;5;197m              \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;165m          \033[48;5;0m  \033[48;5;63m      \033[48;5;0m          \033[48;5;63m        \033[48;5;0m  \033[48;5;39m            \033[48;5;0m        \033[49m  \033[48;5;0m  \033[48;5;49m        \033[48;5;0m      \033[49m  \033[48;5;0m  \033[48;5;49m        \033[48;5;0m  \033[48;5;154m            \033[48;5;0m    \033[49m    \033[48;5;0m  \033[48;5;11m        \033[48;5;0m      \033[49m          \033[48;5;0m  \033[48;5;214m        \033[48;5;0m    \033[49m  \033[48;5;0m            \033[48;5;202m          \033[48;5;0m      \033[49m          \033[m");
Jerial.println ("\033[49m        \033[48;5;0m    \033[48;5;197m            \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;165m          \033[48;5;0m    \033[48;5;165m          \033[48;5;0m  \033[48;5;63m      \033[48;5;0m      \033[49m    \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[48;5;39m        \033[48;5;0m        \033[49m      \033[48;5;0m  \033[48;5;49m        \033[48;5;0m    \033[49m    \033[48;5;0m    \033[48;5;49m      \033[48;5;0m  \033[48;5;154m          \033[48;5;0m      \033[49m    \033[48;5;0m  \033[48;5;11m        \033[48;5;0m    \033[49m            \033[48;5;0m  \033[48;5;214m          \033[48;5;0m    \033[49m          \033[48;5;0m    \033[48;5;202m            \033[48;5;0m    \033[49m        \033[m");
Jerial.println ("\033[49m          \033[48;5;0m  \033[48;5;197m            \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;165m          \033[48;5;0m    \033[48;5;165m          \033[48;5;0m  \033[48;5;63m      \033[48;5;0m    \033[49m      \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[48;5;39m        \033[48;5;0m    \033[49m          \033[48;5;0m    \033[48;5;49m      \033[48;5;0m  \033[49m        \033[48;5;0m  \033[48;5;49m      \033[48;5;0m  \033[48;5;154m          \033[48;5;0m    \033[49m      \033[48;5;0m  \033[48;5;11m        \033[48;5;0m              \033[49m  \033[48;5;0m    \033[48;5;214m          \033[48;5;0m    \033[49m          \033[48;5;0m  \033[48;5;202m                \033[48;5;0m  \033[49m      \033[m");
Jerial.println ("\033[49m          \033[48;5;0m    \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;165m            \033[48;5;0m  \033[48;5;165m          \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[49m        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[48;5;39m        \033[48;5;0m  \033[49m            \033[48;5;0m    \033[48;5;49m      \033[48;5;0m  \033[49m        \033[48;5;0m  \033[48;5;49m      \033[48;5;0m  \033[48;5;154m          \033[48;5;0m    \033[49m      \033[48;5;0m    \033[48;5;11m      \033[48;5;0m    \033[48;5;11m        \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;214m              \033[48;5;0m    \033[49m      \033[48;5;0m    \033[48;5;202m                \033[48;5;0m  \033[49m    \033[m");
Jerial.println ("\033[49m            \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[48;5;165m                        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[49m        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[48;5;39m        \033[48;5;0m  \033[49m  \033[48;5;0m        \033[49m    \033[48;5;0m  \033[48;5;49m        \033[48;5;0m  \033[49m      \033[48;5;0m  \033[48;5;49m      \033[48;5;0m  \033[48;5;154m          \033[48;5;0m    \033[49m      \033[48;5;0m    \033[48;5;11m                    \033[48;5;0m  \033[49m  \033[48;5;0m    \033[48;5;214m                \033[48;5;0m  \033[49m      \033[48;5;0m      \033[48;5;202m            \033[48;5;0m    \033[49m  \033[m");
Jerial.println ("\033[49m            \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[48;5;165m                        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[49m        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[48;5;39m        \033[48;5;0m    \033[48;5;39m      \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;49m        \033[48;5;0m  \033[49m      \033[48;5;0m  \033[48;5;49m      \033[48;5;0m  \033[48;5;154m          \033[48;5;0m  \033[49m          \033[48;5;0m  \033[48;5;11m                    \033[48;5;0m  \033[49m    \033[48;5;0m    \033[48;5;214m                \033[48;5;0m  \033[49m      \033[48;5;0m        \033[48;5;202m          \033[48;5;0m  \033[49m  \033[m");
Jerial.println ("\033[49m            \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[48;5;165m                        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[49m        \033[48;5;0m  \033[48;5;63m      \033[48;5;0m  \033[48;5;39m                    \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;49m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;49m      \033[48;5;0m      \033[48;5;154m        \033[48;5;0m  \033[49m          \033[48;5;0m  \033[48;5;11m                  \033[48;5;0m  \033[49m        \033[48;5;0m        \033[48;5;214m          \033[48;5;0m    \033[49m          \033[48;5;0m    \033[48;5;202m          \033[48;5;0m  \033[m");
Jerial.println ("\033[49m            \033[48;5;0m  \033[48;5;197m          \033[48;5;0m    \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[48;5;165m                        \033[48;5;0m  \033[48;5;63m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;63m        \033[48;5;0m  \033[48;5;39m                    \033[48;5;0m  \033[49m  \033[48;5;0m    \033[48;5;49m      \033[48;5;0m      \033[48;5;49m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;154m        \033[48;5;0m  \033[49m          \033[48;5;0m  \033[48;5;11m          \033[48;5;0m        \033[49m                \033[48;5;0m    \033[48;5;214m          \033[48;5;0m  \033[49m    \033[48;5;0m      \033[49m  \033[48;5;0m    \033[48;5;202m        \033[48;5;0m  \033[m");
Jerial.println ("\033[49m              \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[48;5;165m                \033[48;5;0m  \033[48;5;165m      \033[48;5;0m  \033[48;5;63m        \033[48;5;0m      \033[48;5;63m          \033[48;5;0m  \033[48;5;39m                  \033[48;5;0m  \033[49m    \033[48;5;0m    \033[48;5;49m                  \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;154m        \033[48;5;0m  \033[49m          \033[48;5;0m  \033[48;5;11m        \033[48;5;0m      \033[49m                      \033[48;5;0m    \033[48;5;214m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;202m    \033[48;5;0m        \033[48;5;202m        \033[48;5;0m  \033[m");
Jerial.println ("\033[49m              \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;199m        \033[48;5;0m    \033[48;5;165m      \033[48;5;0m  \033[48;5;165m      \033[48;5;0m  \033[48;5;165m      \033[48;5;0m    \033[48;5;63m                    \033[48;5;0m    \033[48;5;39m            \033[48;5;0m      \033[49m        \033[48;5;0m  \033[48;5;49m                \033[48;5;0m  \033[49m      \033[48;5;0m  \033[48;5;154m        \033[48;5;0m  \033[49m          \033[48;5;0m  \033[48;5;11m        \033[48;5;0m    \033[49m    \033[48;5;0m      \033[49m      \033[48;5;0m            \033[48;5;214m        \033[48;5;0m    \033[48;5;202m        \033[48;5;0m    \033[48;5;202m          \033[48;5;0m  \033[m");
Jerial.println ("\033[49m              \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m        \033[48;5;0m      \033[48;5;199m        \033[48;5;0m    \033[48;5;165m      \033[48;5;0m  \033[48;5;165m    \033[48;5;0m    \033[48;5;165m      \033[48;5;0m    \033[48;5;63m                    \033[48;5;0m    \033[48;5;39m          \033[48;5;0m      \033[49m          \033[48;5;0m  \033[48;5;49m                \033[48;5;0m    \033[49m    \033[48;5;0m  \033[48;5;154m        \033[48;5;0m  \033[49m      \033[48;5;0m      \033[48;5;11m        \033[48;5;0m        \033[48;5;11m    \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;214m      \033[48;5;0m    \033[48;5;214m          \033[48;5;0m  \033[48;5;202m                        \033[48;5;0m  \033[m");
Jerial.println ("\033[49m  \033[48;5;0m        \033[49m    \033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;199m          \033[48;5;0m  \033[48;5;199m          \033[48;5;0m    \033[48;5;165m      \033[48;5;0m    \033[48;5;165m  \033[48;5;0m    \033[48;5;165m        \033[48;5;0m  \033[48;5;63m                  \033[48;5;0m        \033[48;5;39m        \033[48;5;0m    \033[49m            \033[48;5;0m  \033[48;5;49m      \033[48;5;0m    \033[48;5;49m        \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;154m        \033[48;5;0m  \033[49m  \033[48;5;0m    \033[48;5;154m    \033[48;5;0m    \033[48;5;11m        \033[48;5;0m    \033[48;5;11m        \033[48;5;0m    \033[48;5;214m                      \033[48;5;0m  \033[48;5;202m                      \033[48;5;0m  \033[49m  \033[m");
Jerial.println ("\033[48;5;0m    \033[48;5;197m    \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;197m          \033[48;5;0m    \033[48;5;199m                    \033[48;5;0m    \033[48;5;165m      \033[48;5;0m      \033[49m  \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m        \033[48;5;0m          \033[49m    \033[48;5;0m    \033[48;5;39m        \033[48;5;0m              \033[49m  \033[48;5;0m  \033[48;5;49m      \033[48;5;0m      \033[48;5;49m        \033[48;5;0m      \033[48;5;154m        \033[48;5;0m    \033[48;5;154m          \033[48;5;0m  \033[48;5;11m                    \033[48;5;0m    \033[48;5;214m                      \033[48;5;0m  \033[48;5;202m                      \033[48;5;0m  \033[49m  \033[m");
Jerial.println ("\033[48;5;0m  \033[48;5;197m        \033[48;5;0m      \033[48;5;197m          \033[48;5;0m    \033[48;5;199m                  \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m        \033[48;5;0m      \033[49m        \033[48;5;0m    \033[48;5;39m          \033[48;5;0m    \033[48;5;39m      \033[48;5;0m      \033[48;5;49m        \033[48;5;0m    \033[48;5;49m          \033[48;5;0m    \033[48;5;154m                      \033[48;5;0m  \033[48;5;11m                    \033[48;5;0m    \033[48;5;214m                    \033[48;5;0m      \033[48;5;202m                  \033[48;5;0m  \033[49m    \033[m");
Jerial.println ("\033[48;5;0m  \033[48;5;197m          \033[48;5;0m  \033[48;5;197m            \033[48;5;0m    \033[48;5;199m                  \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m          \033[48;5;0m  \033[49m          \033[48;5;0m    \033[48;5;39m                      \033[48;5;0m    \033[48;5;49m        \033[48;5;0m      \033[48;5;49m          \033[48;5;0m  \033[48;5;154m                      \033[48;5;0m  \033[48;5;11m                  \033[48;5;0m        \033[48;5;214m                  \033[48;5;0m        \033[48;5;202m              \033[48;5;0m    \033[49m    \033[m");
Jerial.println ("\033[48;5;0m  \033[48;5;197m                      \033[48;5;0m        \033[48;5;199m                \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m          \033[48;5;0m  \033[49m          \033[48;5;0m    \033[48;5;39m                      \033[48;5;0m    \033[48;5;49m        \033[48;5;0m  \033[49m  \033[48;5;0m  \033[48;5;49m          \033[48;5;0m    \033[48;5;154m                  \033[48;5;0m      \033[48;5;11m            \033[48;5;0m      \033[49m    \033[48;5;0m    \033[48;5;214m              \033[48;5;0m    \033[49m  \033[48;5;0m                    \033[49m      \033[m");
Jerial.println ("\033[48;5;0m  \033[48;5;197m                      \033[48;5;0m  \033[49m  \033[48;5;0m    \033[48;5;199m              \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[48;5;63m          \033[48;5;0m  \033[49m            \033[48;5;0m    \033[48;5;39m                  \033[48;5;0m        \033[48;5;49m      \033[48;5;0m  \033[49m  \033[48;5;0m    \033[48;5;49m      \033[48;5;0m        \033[48;5;154m            \033[48;5;0m      \033[49m  \033[48;5;0m                  \033[49m      \033[48;5;0m                    \033[49m      \033[48;5;0m                \033[49m        \033[m");
Jerial.println ("\033[48;5;0m  \033[48;5;197m                    \033[48;5;0m    \033[49m    \033[48;5;0m    \033[48;5;199m            \033[48;5;0m    \033[49m  \033[48;5;0m  \033[48;5;165m        \033[48;5;0m  \033[49m    \033[48;5;0m    \033[48;5;165m    \033[48;5;0m      \033[48;5;63m      \033[48;5;0m    \033[49m            \033[48;5;0m      \033[48;5;39m          \033[48;5;0m        \033[49m  \033[48;5;0m            \033[49m  \033[48;5;0m            \033[49m  \033[48;5;0m                    \033[49m    \033[48;5;0m                \033[49m          \033[48;5;0m                \033[49m          \033[48;5;0m            \033[49m          \033[m");
Jerial.println ("\033[48;5;0m    \033[48;5;197m                  \033[48;5;0m    \033[49m    \033[48;5;0m      \033[48;5;199m        \033[48;5;0m    \033[49m    \033[48;5;0m    \033[48;5;165m    \033[48;5;0m    \033[49m    \033[48;5;0m                        \033[49m              \033[48;5;0m                    \033[49m    \033[48;5;0m          \033[49m      \033[48;5;0m          \033[49m    \033[48;5;0m                \033[49m        \033[48;5;0m            \033[49m              \033[48;5;0m            \033[49m                                  \033[m");
Jerial.println ("\033[49m  \033[48;5;0m    \033[48;5;197m              \033[48;5;0m    \033[49m        \033[48;5;0m                \033[49m    \033[48;5;0m            \033[49m    \033[48;5;0m          \033[49m  \033[48;5;0m          \033[49m                \033[48;5;0m                \033[49m          \033[48;5;0m      \033[49m          \033[48;5;0m      \033[49m        \033[48;5;0m            \033[49m                                                                                  \033[m");
Jerial.println ("\033[49m    \033[48;5;0m    \033[48;5;197m          \033[48;5;0m      \033[49m          \033[48;5;0m            \033[49m        \033[48;5;0m        \033[49m        \033[48;5;0m      \033[49m      \033[48;5;0m      \033[49m                    \033[48;5;0m          \033[49m                                                                                                                                          \033[m");
Jerial.println ("\033[49m    \033[48;5;0m                  \033[49m              \033[48;5;0m        \033[49m            \033[48;5;0m    \033[49m                                                                                                                                                                                                    \033[m");
Jerial.println ("\033[49m      \033[48;5;0m              \033[49m                                                                                                                                                                                                                                            \033[m");

   }

   void printColorJogoSmall(void){


    Jerial.println("\033[49m                                                                                               \033[38;5;0;49m▄▄▄▄\033[49m       \033[38;5;0;49m▄▄▄▄▄▄▄\033[49m    \033[38;5;0;49m▄▄▄\033[38;5;202;48;5;0m▄▄▄▄▄\033[38;5;0;49m▄\033[49m  \033[m");
    Jerial.println("\033[49m                                            \033[38;5;0;49m▄▄▄▄▄\033[49m        \033[38;5;0;49m▄▄▄▄▄\033[49m   \033[38;5;0;49m▄▄▄▄\033[38;5;49;48;5;0m▄▄▄▄▄\033[38;5;0;49m▄▄\033[49m   \033[38;5;0;49m▄\033[38;5;154;48;5;0m▄▄▄\033[38;5;0;49m▄▄\033[49m      \033[38;5;0;49m▄▄\033[38;5;11;48;5;0m▄▄\033[48;5;11m    \033[38;5;11;48;5;0m▄\033[48;5;0m \033[49m  \033[38;5;0;49m▄\033[38;5;214;48;5;0m▄▄\033[48;5;214m      \033[38;5;214;48;5;0m▄\033[48;5;0m \033[49m \033[38;5;0;49m▄\033[38;5;202;48;5;0m▄\033[48;5;202m         \033[48;5;0m \033[38;5;0;49m▄\033[m");
    Jerial.println("\033[49m      \033[38;5;0;49m▄▄\033[38;5;197;48;5;0m▄▄▄\033[38;5;0;49m▄\033[49m  \033[38;5;0;49m▄▄\033[49m    \033[38;5;0;49m▄\033[38;5;199;48;5;0m▄▄▄\033[38;5;0;49m▄\033[49m  \033[38;5;0;49m▄▄▄\033[49m    \033[38;5;0;49m▄\033[38;5;165;48;5;0m▄▄\033[48;5;0m \033[38;5;0;49m▄\033[49m \033[38;5;0;49m▄\033[38;5;63;48;5;0m▄▄▄\033[48;5;63m     \033[38;5;63;48;5;0m▄\033[38;5;0;49m▄\033[49m  \033[38;5;0;49m▄\033[38;5;39;48;5;0m▄▄▄\033[48;5;39m     \033[38;5;39;48;5;0m▄\033[38;5;0;49m▄\033[38;5;49;48;5;0m▄\033[48;5;49m          \033[38;5;49;48;5;0m▄\033[48;5;0m \033[38;5;0;49m▄\033[38;5;154;48;5;0m▄\033[48;5;154m     \033[48;5;0m \033[38;5;0;49m▄\033[49m   \033[38;5;0;49m▄\033[38;5;11;48;5;0m▄\033[48;5;11m          \033[48;5;0m \033[38;5;0;49m▄\033[38;5;214;48;5;0m▄\033[48;5;214m          \033[48;5;0m \033[38;5;202;48;5;0m▄\033[48;5;202m    \033[38;5;0;48;5;202m▄▄\033[48;5;202m     \033[48;5;0m \033[m");
    Jerial.println("\033[49m    \033[38;5;0;49m▄\033[38;5;197;48;5;0m▄\033[48;5;197m     \033[38;5;197;48;5;0m▄\033[48;5;0m \033[38;5;199;48;5;0m▄\033[48;5;199m  \033[38;5;199;48;5;0m▄\033[38;5;0;49m▄\033[49m \033[38;5;0;49m▄\033[38;5;199;48;5;0m▄\033[48;5;199m   \033[38;5;199;48;5;0m▄\033[38;5;0;49m▄\033[38;5;165;48;5;0m▄\033[48;5;165m  \033[38;5;165;48;5;0m▄\033[38;5;0;49m▄\033[49m  \033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[38;5;63;48;5;0m▄\033[48;5;63m          \033[38;5;63;48;5;0m▄\033[48;5;0m \033[38;5;39;48;5;0m▄\033[48;5;39m          \033[48;5;0m \033[48;5;49m   \033[38;5;0;48;5;49m▄▄▄▄▄\033[48;5;49m     \033[48;5;0m \033[48;5;154m      \033[48;5;0m  \033[49m  \033[48;5;0m \033[38;5;11;48;5;0m▄\033[48;5;11m        \033[38;5;0;48;5;11m▄▄\033[49;38;5;0m▀\033[48;5;0m \033[48;5;214m    \033[38;5;0;48;5;214m▄\033[48;5;0m  \033[38;5;0;48;5;214m▄\033[48;5;214m    \033[48;5;0m \033[48;5;202m    \033[48;5;0m  \033[49;38;5;0m▀\033[48;5;0m \033[48;5;202m   \033[38;5;0;48;5;202m▄\033[48;5;0m \033[m");
    Jerial.println("\033[49m    \033[48;5;0m \033[48;5;197m       \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[49m \033[48;5;0m \033[48;5;199m     \033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[49m \033[38;5;0;49m▄\033[38;5;165;48;5;0m▄\033[48;5;165m    \033[48;5;0m \033[48;5;63m   \033[38;5;0;48;5;63m▄\033[48;5;0m    \033[48;5;63m    \033[48;5;0m \033[48;5;39m      \033[38;5;0;48;5;39m▄▄▄▄\033[49;38;5;0m▀\033[48;5;0m \033[48;5;49m    \033[48;5;0m   \033[49;38;5;0m▀\033[48;5;0m \033[48;5;49m    \033[48;5;0m \033[48;5;154m      \033[48;5;0m  \033[49m  \033[48;5;0m \033[48;5;11m    \033[38;5;0;48;5;11m▄\033[48;5;0m  \033[49;38;5;0m▀▀\033[49m   \033[48;5;0m \033[48;5;214m    \033[48;5;0m  \033[49m \033[48;5;0m \033[38;5;0;48;5;214m▄▄▄\033[48;5;0m  \033[48;5;202m    \033[38;5;202;48;5;0m▄\033[48;5;0m \033[38;5;0;49m▄▄\033[49;38;5;0m▀▀▀▀\033[49m \033[m");
    Jerial.println("\033[49m    \033[49;38;5;0m▀\033[48;5;0m \033[48;5;197m      \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[49m \033[48;5;0m \033[48;5;199m     \033[48;5;0m \033[48;5;165m     \033[48;5;0m  \033[48;5;165m     \033[48;5;0m \033[48;5;63m   \033[48;5;0m  \033[49;38;5;0m▀\033[49m  \033[48;5;0m \033[48;5;63m   \033[48;5;0m \033[48;5;39m    \033[48;5;0m  \033[49;38;5;0m▀▀\033[49m   \033[48;5;0m \033[38;5;0;48;5;49m▄\033[48;5;49m   \033[48;5;0m \033[49;38;5;0m▀\033[49m  \033[49;38;5;0m▀\033[48;5;0m \033[48;5;49m   \033[48;5;0m \033[48;5;154m     \033[48;5;0m  \033[49;38;5;0m▀\033[49m  \033[48;5;0m \033[48;5;11m    \033[48;5;0m  \033[38;5;0;49m▄▄▄▄▄\033[49m \033[48;5;0m \033[38;5;0;48;5;214m▄\033[48;5;214m    \033[38;5;214;48;5;0m▄\033[48;5;0m \033[38;5;0;49m▄\033[49m    \033[49;38;5;0m▀\033[48;5;0m \033[48;5;202m      \033[38;5;202;48;5;0m▄▄\033[38;5;0;49m▄\033[49m   \033[m");
    Jerial.println("\033[49m     \033[49;38;5;0m▀\033[48;5;0m \033[48;5;197m     \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[49m \033[49;38;5;0m▀\033[38;5;0;48;5;199m▄\033[48;5;199m    \033[48;5;0m \033[48;5;165m      \033[38;5;165;48;5;0m▄\033[48;5;165m     \033[48;5;0m \033[48;5;63m   \033[48;5;0m \033[49m    \033[48;5;0m \033[48;5;63m   \033[48;5;0m \033[48;5;39m    \033[48;5;0m \033[49m \033[38;5;0;49m▄▄▄▄\033[49m \033[49;38;5;0m▀\033[48;5;0m \033[48;5;49m   \033[38;5;49;48;5;0m▄\033[38;5;0;49m▄\033[49m   \033[48;5;0m \033[48;5;49m   \033[48;5;0m \033[48;5;154m     \033[48;5;0m  \033[49m   \033[48;5;0m  \033[48;5;11m   \033[38;5;11;48;5;0m▄▄\033[48;5;11m    \033[38;5;11;48;5;0m▄\033[48;5;0m \033[49m \033[48;5;0m \033[38;5;0;48;5;214m▄\033[48;5;214m      \033[38;5;214;48;5;0m▄▄\033[38;5;0;49m▄\033[49m  \033[49;38;5;0m▀\033[48;5;0m \033[38;5;0;48;5;202m▄▄\033[48;5;202m      \033[48;5;0m \033[38;5;0;49m▄\033[49m \033[m");
    Jerial.println("\033[49m      \033[48;5;0m \033[48;5;197m     \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[49m  \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[48;5;165m            \033[48;5;0m \033[48;5;63m   \033[48;5;0m \033[49m    \033[48;5;0m \033[48;5;63m   \033[48;5;0m \033[48;5;39m    \033[38;5;39;48;5;0m▄▄\033[48;5;39m   \033[38;5;39;48;5;0m▄\033[48;5;0m \033[49m \033[48;5;0m \033[48;5;49m    \033[48;5;0m \033[49m  \033[38;5;0;49m▄\033[38;5;49;48;5;0m▄\033[48;5;49m  \033[38;5;0;48;5;49m▄\033[48;5;0m \033[38;5;0;48;5;154m▄\033[48;5;154m    \033[48;5;0m \033[49m     \033[48;5;0m \033[48;5;11m         \033[38;5;0;48;5;11m▄\033[49;38;5;0m▀\033[49m  \033[49;38;5;0m▀\033[48;5;0m \033[38;5;0;48;5;214m▄▄▄\033[48;5;214m     \033[48;5;0m \033[38;5;0;49m▄\033[49m  \033[49;38;5;0m▀▀▀\033[48;5;0m \033[38;5;0;48;5;202m▄\033[48;5;202m    \033[38;5;202;48;5;0m▄\033[38;5;0;49m▄\033[m");
    Jerial.println("\033[49m      \033[49;38;5;0m▀\033[38;5;0;48;5;197m▄\033[48;5;197m    \033[38;5;197;48;5;0m▄\033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[49m \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[48;5;165m        \033[38;5;0;48;5;165m▄\033[48;5;165m   \033[48;5;0m \033[48;5;63m    \033[48;5;0m \033[38;5;0;49m▄▄\033[38;5;63;48;5;0m▄\033[48;5;63m    \033[48;5;0m \033[48;5;39m         \033[38;5;0;48;5;39m▄\033[49;38;5;0m▀\033[49m \033[48;5;0m  \033[48;5;49m   \033[38;5;49;48;5;0m▄▄▄\033[48;5;49m   \033[38;5;0;48;5;49m▄\033[49;38;5;0m▀\033[49m \033[48;5;0m \033[48;5;154m    \033[48;5;0m \033[49m     \033[48;5;0m \033[48;5;11m    \033[38;5;0;48;5;11m▄\033[48;5;0m  \033[49;38;5;0m▀▀\033[49m        \033[49;38;5;0m▀\033[48;5;0m \033[38;5;0;48;5;214m▄\033[48;5;214m    \033[48;5;0m \033[49m \033[38;5;0;49m▄\033[38;5;202;48;5;0m▄▄\033[48;5;0m \033[38;5;0;49m▄\033[48;5;0m  \033[48;5;202m    \033[48;5;0m \033[m");
    Jerial.println("\033[49m       \033[48;5;0m \033[48;5;197m     \033[48;5;0m \033[48;5;199m    \033[48;5;0m \033[38;5;0;49m▄\033[48;5;0m \033[48;5;199m    \033[48;5;0m  \033[48;5;165m   \033[48;5;0m \033[48;5;165m  \033[38;5;0;48;5;165m▄\033[48;5;0m \033[48;5;165m   \033[48;5;0m  \033[48;5;63m          \033[48;5;0m  \033[48;5;39m     \033[38;5;0;48;5;39m▄\033[48;5;0m  \033[49;38;5;0m▀\033[49m    \033[48;5;0m \033[48;5;49m        \033[48;5;0m \033[38;5;0;49m▄\033[49m  \033[48;5;0m \033[48;5;154m    \033[48;5;0m \033[49m   \033[38;5;0;49m▄▄\033[48;5;0m \033[48;5;11m    \033[48;5;0m  \033[38;5;0;49m▄▄\033[38;5;11;48;5;0m▄▄\033[48;5;0m \033[38;5;0;49m▄\033[49m \033[38;5;0;49m▄\033[38;5;214;48;5;0m▄▄▄\033[48;5;0m  \033[38;5;214;48;5;0m▄\033[48;5;214m    \033[48;5;0m \033[38;5;202;48;5;0m▄\033[48;5;202m    \033[38;5;202;48;5;0m▄▄\033[48;5;202m     \033[48;5;0m \033[m");
    Jerial.println("\033[38;5;0;49m▄\033[48;5;0m \033[38;5;197;48;5;0m▄▄\033[48;5;0m \033[38;5;0;49m▄\033[49m \033[48;5;0m \033[48;5;197m     \033[48;5;0m \033[38;5;0;48;5;199m▄\033[48;5;199m    \033[38;5;199;48;5;0m▄\033[48;5;199m     \033[48;5;0m  \033[48;5;165m   \033[48;5;0m  \033[38;5;0;48;5;165m▄\033[49;38;5;0m▀\033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[48;5;63m    \033[38;5;0;48;5;63m▄▄▄▄▄\033[49;38;5;0m▀▀\033[48;5;0m  \033[48;5;39m    \033[48;5;0m  \033[38;5;0;49m▄▄▄▄▄\033[49m \033[48;5;0m \033[48;5;49m   \033[48;5;0m  \033[38;5;0;48;5;49m▄\033[48;5;49m   \033[38;5;49;48;5;0m▄\033[48;5;0m \033[38;5;0;49m▄\033[48;5;0m \033[48;5;154m    \033[48;5;0m \033[38;5;0;49m▄\033[38;5;154;48;5;0m▄▄\033[48;5;154m  \033[38;5;154;48;5;0m▄\033[48;5;0m \033[48;5;11m    \033[38;5;11;48;5;0m▄▄\033[48;5;11m    \033[48;5;0m  \033[48;5;214m           \033[48;5;0m \033[48;5;202m           \033[48;5;0m \033[49m \033[m");
    Jerial.println("\033[48;5;0m \033[48;5;197m    \033[38;5;197;48;5;0m▄\033[48;5;0m \033[38;5;197;48;5;0m▄\033[48;5;197m     \033[48;5;0m  \033[48;5;199m         \033[48;5;0m \033[49m \033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[49m  \033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[48;5;63m    \033[38;5;63;48;5;0m▄\033[48;5;0m \033[49;38;5;0m▀\033[49m    \033[48;5;0m  \033[48;5;39m     \033[38;5;39;48;5;0m▄▄\033[48;5;39m   \033[38;5;39;48;5;0m▄\033[48;5;0m  \033[48;5;49m    \033[48;5;0m  \033[38;5;0;48;5;49m▄\033[48;5;49m    \033[38;5;49;48;5;0m▄\033[48;5;0m \033[48;5;154m           \033[48;5;0m \033[48;5;11m         \033[38;5;0;48;5;11m▄\033[48;5;0m  \033[38;5;0;48;5;214m▄\033[48;5;214m         \033[48;5;0m   \033[38;5;0;48;5;202m▄\033[48;5;202m       \033[38;5;0;48;5;202m▄\033[48;5;0m \033[49m  \033[m");
    Jerial.println("\033[48;5;0m \033[48;5;197m           \033[48;5;0m \033[49;38;5;0m▀\033[48;5;0m  \033[48;5;199m       \033[38;5;0;48;5;199m▄\033[48;5;0m \033[49m \033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[49m  \033[48;5;0m \033[48;5;165m    \033[48;5;0m \033[48;5;63m     \033[48;5;0m \033[49m     \033[49;38;5;0m▀\033[48;5;0m \033[38;5;0;48;5;39m▄\033[48;5;39m         \033[38;5;0;48;5;39m▄\033[48;5;0m  \033[38;5;0;48;5;49m▄\033[48;5;49m   \033[48;5;0m \033[49m \033[48;5;0m \033[38;5;0;48;5;49m▄\033[48;5;49m   \033[38;5;0;48;5;49m▄\033[48;5;0m  \033[38;5;0;48;5;154m▄\033[48;5;154m      \033[38;5;0;48;5;154m▄▄\033[48;5;0m \033[49;38;5;0m▀\033[48;5;0m \033[38;5;0;48;5;11m▄▄▄▄▄▄\033[48;5;0m  \033[49;38;5;0m▀\033[49m  \033[48;5;0m  \033[38;5;0;48;5;214m▄▄▄▄▄▄▄\033[48;5;0m \033[49;38;5;0m▀\033[49m \033[49;38;5;0m▀\033[48;5;0m        \033[49;38;5;0m▀\033[49m   \033[m");
    Jerial.println("\033[48;5;0m \033[38;5;0;48;5;197m▄\033[48;5;197m         \033[48;5;0m  \033[49m  \033[48;5;0m  \033[38;5;0;48;5;199m▄\033[48;5;199m    \033[38;5;0;48;5;199m▄\033[48;5;0m \033[49;38;5;0m▀\033[49m \033[48;5;0m \033[38;5;0;48;5;165m▄\033[48;5;165m  \033[38;5;0;48;5;165m▄\033[48;5;0m \033[49m  \033[48;5;0m  \033[38;5;0;48;5;165m▄▄\033[48;5;0m   \033[38;5;0;48;5;63m▄▄▄\033[48;5;0m  \033[49m      \033[49;38;5;0m▀\033[48;5;0m  \033[38;5;0;48;5;39m▄▄▄▄▄\033[48;5;0m   \033[49;38;5;0m▀\033[49m \033[48;5;0m     \033[49;38;5;0m▀\033[49m \033[49;38;5;0m▀\033[48;5;0m     \033[49m \033[49;38;5;0m▀\033[48;5;0m        \033[49;38;5;0m▀\033[49m  \033[49;38;5;0m▀\033[48;5;0m      \033[49;38;5;0m▀\033[49m     \033[49;38;5;0m▀\033[48;5;0m      \033[49;38;5;0m▀\033[49m     \033[49;38;5;0m▀▀▀▀▀▀\033[49m     \033[m");
    Jerial.println("\033[49m \033[49;38;5;0m▀\033[48;5;0m \033[38;5;0;48;5;197m▄\033[48;5;197m     \033[38;5;0;48;5;197m▄\033[48;5;0m  \033[49m    \033[49;38;5;0m▀\033[48;5;0m      \033[49;38;5;0m▀\033[49m  \033[49;38;5;0m▀\033[48;5;0m    \033[49;38;5;0m▀\033[49m  \033[49;38;5;0m▀\033[48;5;0m   \033[49;38;5;0m▀\033[49m \033[49;38;5;0m▀\033[48;5;0m   \033[49;38;5;0m▀\033[49m        \033[49;38;5;0m▀\033[48;5;0m     \033[49;38;5;0m▀▀\033[49m     \033[49;38;5;0m▀▀▀\033[49m     \033[49;38;5;0m▀▀▀\033[49m    \033[49;38;5;0m▀▀▀▀▀▀\033[49m                                         \033[m");
    Jerial.println("\033[49m  \033[49;38;5;0m▀\033[48;5;0m       \033[49;38;5;0m▀\033[49m       \033[49;38;5;0m▀▀▀▀\033[49m      \033[49;38;5;0m▀▀\033[49m                                                                                                  \033[m");
    // Jerial.println("\n\n\n\r");
    // Jerial.println("\e[49m                                                                                               \e[38;2;0;0;0;49m▄▄▄▄\e[49m       \e[38;2;0;0;0;49m▄▄▄▄▄▄▄\e[49m    \e[38;2;0;0;0;49m▄▄▄\e[38;2;254;80;12;48;2;0;0;0m▄▄▄▄▄\e[38;2;0;0;0;49m▄\e[49m  \e[m");
    // Jerial.println("\e[49m                                            \e[38;2;0;0;0;49m▄▄▄▄▄\e[49m        \e[38;2;0;0;0;49m▄▄▄▄▄\e[49m   \e[38;2;0;0;0;49m▄▄▄▄\e[38;2;24;255;175;48;2;0;0;0m▄▄▄▄▄\e[38;2;0;0;0;49m▄▄\e[49m   \e[38;2;0;0;0;49m▄\e[38;2;177;255;8;48;2;0;0;0m▄▄▄\e[38;2;0;0;0;49m▄▄\e[49m      \e[38;2;0;0;0;49m▄▄\e[38;2;254;255;16;48;2;0;0;0m▄▄\e[48;2;254;255;16m    \e[38;2;254;255;16;48;2;0;0;0m▄\e[48;2;0;0;0m \e[49m  \e[38;2;0;0;0;49m▄\e[38;2;255;168;0;48;2;0;0;0m▄▄\e[48;2;255;168;0m      \e[38;2;255;168;0;48;2;0;0;0m▄\e[48;2;0;0;0m \e[49m \e[38;2;0;0;0;49m▄\e[38;2;254;80;12;48;2;0;0;0m▄\e[48;2;254;80;12m         \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[m");
    // Jerial.println("\e[49m      \e[38;2;0;0;0;49m▄▄\e[38;2;255;0;74;48;2;0;0;0m▄▄▄\e[38;2;0;0;0;49m▄\e[49m  \e[38;2;0;0;0;49m▄▄\e[49m    \e[38;2;0;0;0;49m▄\e[38;2;255;43;178;48;2;0;0;0m▄▄▄\e[38;2;0;0;0;49m▄\e[49m  \e[38;2;0;0;0;49m▄▄▄\e[49m    \e[38;2;0;0;0;49m▄\e[38;2;205;7;255;48;2;0;0;0m▄▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m \e[38;2;0;0;0;49m▄\e[38;2;84;56;255;48;2;0;0;0m▄▄▄\e[48;2;84;56;255m     \e[38;2;84;56;255;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[49m  \e[38;2;0;0;0;49m▄\e[38;2;0;173;255;48;2;0;0;0m▄▄▄\e[48;2;0;173;255m     \e[38;2;0;173;255;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[38;2;24;255;175;48;2;0;0;0m▄\e[48;2;24;255;175m          \e[38;2;24;255;175;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[38;2;177;255;8;48;2;0;0;0m▄\e[48;2;177;255;8m     \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m   \e[38;2;0;0;0;49m▄\e[38;2;254;255;16;48;2;0;0;0m▄\e[48;2;254;255;16m          \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[38;2;255;168;0;48;2;0;0;0m▄\e[48;2;255;168;0m          \e[48;2;0;0;0m \e[38;2;254;80;12;48;2;0;0;0m▄\e[48;2;254;80;12m    \e[38;2;0;0;0;48;2;254;80;12m▄▄\e[48;2;254;80;12m     \e[48;2;0;0;0m \e[m");
    // Jerial.println("\e[49m    \e[38;2;0;0;0;49m▄\e[38;2;255;0;74;48;2;0;0;0m▄\e[48;2;255;0;74m     \e[38;2;255;0;74;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;255;43;178;48;2;0;0;0m▄\e[48;2;255;43;178m  \e[38;2;255;43;178;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[49m \e[38;2;0;0;0;49m▄\e[38;2;255;43;178;48;2;0;0;0m▄\e[48;2;255;43;178m   \e[38;2;255;43;178;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[38;2;205;7;255;48;2;0;0;0m▄\e[48;2;205;7;255m  \e[38;2;205;7;255;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[49m  \e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[38;2;84;56;255;48;2;0;0;0m▄\e[48;2;84;56;255m          \e[38;2;84;56;255;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;0;173;255;48;2;0;0;0m▄\e[48;2;0;173;255m          \e[48;2;0;0;0m \e[48;2;24;255;175m   \e[38;2;0;0;0;48;2;24;255;175m▄▄▄▄▄\e[48;2;24;255;175m     \e[48;2;0;0;0m \e[48;2;177;255;8m      \e[48;2;0;0;0m  \e[49m  \e[48;2;0;0;0m \e[38;2;254;255;16;48;2;0;0;0m▄\e[48;2;254;255;16m        \e[38;2;0;0;0;48;2;254;255;16m▄▄\e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;255;168;0m    \e[38;2;0;0;0;48;2;255;168;0m▄\e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;255;168;0m▄\e[48;2;255;168;0m    \e[48;2;0;0;0m \e[48;2;254;80;12m    \e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;254;80;12m   \e[38;2;0;0;0;48;2;254;80;12m▄\e[48;2;0;0;0m \e[m");
    // Jerial.println("\e[49m    \e[48;2;0;0;0m \e[48;2;255;0;74m       \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[48;2;255;43;178m     \e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[49m \e[38;2;0;0;0;49m▄\e[38;2;205;7;255;48;2;0;0;0m▄\e[48;2;205;7;255m    \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[38;2;0;0;0;48;2;84;56;255m▄\e[48;2;0;0;0m    \e[48;2;84;56;255m    \e[48;2;0;0;0m \e[48;2;0;173;255m      \e[38;2;0;0;0;48;2;0;173;255m▄▄▄▄\e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;24;255;175m    \e[48;2;0;0;0m   \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;24;255;175m    \e[48;2;0;0;0m \e[48;2;177;255;8m      \e[48;2;0;0;0m  \e[49m  \e[48;2;0;0;0m \e[48;2;254;255;16m    \e[38;2;0;0;0;48;2;254;255;16m▄\e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀▀\e[49m   \e[48;2;0;0;0m \e[48;2;255;168;0m    \e[48;2;0;0;0m  \e[49m \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;168;0m▄▄▄\e[48;2;0;0;0m  \e[48;2;254;80;12m    \e[38;2;254;80;12;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄▄\e[49;38;2;0;0;0m▀▀▀▀\e[49m \e[m");
    // Jerial.println("\e[49m    \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;255;0;74m      \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[48;2;255;43;178m     \e[48;2;0;0;0m \e[48;2;205;7;255m     \e[48;2;0;0;0m  \e[48;2;205;7;255m     \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀\e[49m  \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[48;2;0;0;0m \e[48;2;0;173;255m    \e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀▀\e[49m   \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;24;255;175m   \e[48;2;0;0;0m \e[49;38;2;0;0;0m▀\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;24;255;175m   \e[48;2;0;0;0m \e[48;2;177;255;8m     \e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀\e[49m  \e[48;2;0;0;0m \e[48;2;254;255;16m    \e[48;2;0;0;0m  \e[38;2;0;0;0;49m▄▄▄▄▄\e[49m \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;168;0m▄\e[48;2;255;168;0m    \e[38;2;255;168;0;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m    \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;254;80;12m      \e[38;2;254;80;12;48;2;0;0;0m▄▄\e[38;2;0;0;0;49m▄\e[49m   \e[m");
    // Jerial.println("\e[49m     \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;255;0;74m     \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[49m \e[49;38;2;0;0;0m▀\e[38;2;0;0;0;48;2;255;43;178m▄\e[48;2;255;43;178m    \e[48;2;0;0;0m \e[48;2;205;7;255m      \e[38;2;205;7;255;48;2;0;0;0m▄\e[48;2;205;7;255m     \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[48;2;0;0;0m \e[49m    \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[48;2;0;0;0m \e[48;2;0;173;255m    \e[48;2;0;0;0m \e[49m \e[38;2;0;0;0;49m▄▄▄▄\e[49m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;24;255;175m   \e[38;2;24;255;175;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[49m   \e[48;2;0;0;0m \e[48;2;24;255;175m   \e[48;2;0;0;0m \e[48;2;177;255;8m     \e[48;2;0;0;0m  \e[49m   \e[48;2;0;0;0m  \e[48;2;254;255;16m   \e[38;2;254;255;16;48;2;0;0;0m▄▄\e[48;2;254;255;16m    \e[38;2;254;255;16;48;2;0;0;0m▄\e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;168;0m▄\e[48;2;255;168;0m      \e[38;2;255;168;0;48;2;0;0;0m▄▄\e[38;2;0;0;0;49m▄\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;254;80;12m▄▄\e[48;2;254;80;12m      \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m \e[m");
    // Jerial.println("\e[49m      \e[48;2;0;0;0m \e[48;2;255;0;74m     \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[49m  \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[48;2;205;7;255m            \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[48;2;0;0;0m \e[49m    \e[48;2;0;0;0m \e[48;2;84;56;255m   \e[48;2;0;0;0m \e[48;2;0;173;255m    \e[38;2;0;173;255;48;2;0;0;0m▄▄\e[48;2;0;173;255m   \e[38;2;0;173;255;48;2;0;0;0m▄\e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[48;2;24;255;175m    \e[48;2;0;0;0m \e[49m  \e[38;2;0;0;0;49m▄\e[38;2;24;255;175;48;2;0;0;0m▄\e[48;2;24;255;175m  \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;177;255;8m▄\e[48;2;177;255;8m    \e[48;2;0;0;0m \e[49m     \e[48;2;0;0;0m \e[48;2;254;255;16m         \e[38;2;0;0;0;48;2;254;255;16m▄\e[49;38;2;0;0;0m▀\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;168;0m▄▄▄\e[48;2;255;168;0m     \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m  \e[49;38;2;0;0;0m▀▀▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;254;80;12m▄\e[48;2;254;80;12m    \e[38;2;254;80;12;48;2;0;0;0m▄\e[38;2;0;0;0;49m▄\e[m");
    // Jerial.println("\e[49m      \e[49;38;2;0;0;0m▀\e[38;2;0;0;0;48;2;255;0;74m▄\e[48;2;255;0;74m    \e[38;2;255;0;74;48;2;0;0;0m▄\e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[48;2;205;7;255m        \e[38;2;0;0;0;48;2;205;7;255m▄\e[48;2;205;7;255m   \e[48;2;0;0;0m \e[48;2;84;56;255m    \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄▄\e[38;2;84;56;255;48;2;0;0;0m▄\e[48;2;84;56;255m    \e[48;2;0;0;0m \e[48;2;0;173;255m         \e[38;2;0;0;0;48;2;0;173;255m▄\e[49;38;2;0;0;0m▀\e[49m \e[48;2;0;0;0m  \e[48;2;24;255;175m   \e[38;2;24;255;175;48;2;0;0;0m▄▄▄\e[48;2;24;255;175m   \e[38;2;0;0;0;48;2;24;255;175m▄\e[49;38;2;0;0;0m▀\e[49m \e[48;2;0;0;0m \e[48;2;177;255;8m    \e[48;2;0;0;0m \e[49m     \e[48;2;0;0;0m \e[48;2;254;255;16m    \e[38;2;0;0;0;48;2;254;255;16m▄\e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀▀\e[49m        \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;168;0m▄\e[48;2;255;168;0m    \e[48;2;0;0;0m \e[49m \e[38;2;0;0;0;49m▄\e[38;2;254;80;12;48;2;0;0;0m▄▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[48;2;0;0;0m  \e[48;2;254;80;12m    \e[48;2;0;0;0m \e[m");
    // Jerial.println("\e[49m       \e[48;2;0;0;0m \e[48;2;255;0;74m     \e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[48;2;0;0;0m \e[48;2;255;43;178m    \e[48;2;0;0;0m  \e[48;2;205;7;255m   \e[48;2;0;0;0m \e[48;2;205;7;255m  \e[38;2;0;0;0;48;2;205;7;255m▄\e[48;2;0;0;0m \e[48;2;205;7;255m   \e[48;2;0;0;0m  \e[48;2;84;56;255m          \e[48;2;0;0;0m  \e[48;2;0;173;255m     \e[38;2;0;0;0;48;2;0;173;255m▄\e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀\e[49m    \e[48;2;0;0;0m \e[48;2;24;255;175m        \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m  \e[48;2;0;0;0m \e[48;2;177;255;8m    \e[48;2;0;0;0m \e[49m   \e[38;2;0;0;0;49m▄▄\e[48;2;0;0;0m \e[48;2;254;255;16m    \e[48;2;0;0;0m  \e[38;2;0;0;0;49m▄▄\e[38;2;254;255;16;48;2;0;0;0m▄▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m \e[38;2;0;0;0;49m▄\e[38;2;255;168;0;48;2;0;0;0m▄▄▄\e[48;2;0;0;0m  \e[38;2;255;168;0;48;2;0;0;0m▄\e[48;2;255;168;0m    \e[48;2;0;0;0m \e[38;2;254;80;12;48;2;0;0;0m▄\e[48;2;254;80;12m    \e[38;2;254;80;12;48;2;0;0;0m▄▄\e[48;2;254;80;12m     \e[48;2;0;0;0m \e[m");
    // Jerial.println("\e[38;2;0;0;0;49m▄\e[48;2;0;0;0m \e[38;2;255;0;74;48;2;0;0;0m▄▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[49m \e[48;2;0;0;0m \e[48;2;255;0;74m     \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;43;178m▄\e[48;2;255;43;178m    \e[38;2;255;43;178;48;2;0;0;0m▄\e[48;2;255;43;178m     \e[48;2;0;0;0m  \e[48;2;205;7;255m   \e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;205;7;255m▄\e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[48;2;84;56;255m    \e[38;2;0;0;0;48;2;84;56;255m▄▄▄▄▄\e[49;38;2;0;0;0m▀▀\e[48;2;0;0;0m  \e[48;2;0;173;255m    \e[48;2;0;0;0m  \e[38;2;0;0;0;49m▄▄▄▄▄\e[49m \e[48;2;0;0;0m \e[48;2;24;255;175m   \e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;24;255;175m   \e[38;2;24;255;175;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[48;2;0;0;0m \e[48;2;177;255;8m    \e[48;2;0;0;0m \e[38;2;0;0;0;49m▄\e[38;2;177;255;8;48;2;0;0;0m▄▄\e[48;2;177;255;8m  \e[38;2;177;255;8;48;2;0;0;0m▄\e[48;2;0;0;0m \e[48;2;254;255;16m    \e[38;2;254;255;16;48;2;0;0;0m▄▄\e[48;2;254;255;16m    \e[48;2;0;0;0m  \e[48;2;255;168;0m           \e[48;2;0;0;0m \e[48;2;254;80;12m           \e[48;2;0;0;0m \e[49m \e[m");
    // Jerial.println("\e[48;2;0;0;0m \e[48;2;255;0;74m    \e[38;2;255;0;74;48;2;0;0;0m▄\e[48;2;0;0;0m \e[38;2;255;0;74;48;2;0;0;0m▄\e[48;2;255;0;74m     \e[48;2;0;0;0m  \e[48;2;255;43;178m         \e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[49m  \e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[48;2;84;56;255m    \e[38;2;84;56;255;48;2;0;0;0m▄\e[48;2;0;0;0m \e[49;38;2;0;0;0m▀\e[49m    \e[48;2;0;0;0m  \e[48;2;0;173;255m     \e[38;2;0;173;255;48;2;0;0;0m▄▄\e[48;2;0;173;255m   \e[38;2;0;173;255;48;2;0;0;0m▄\e[48;2;0;0;0m  \e[48;2;24;255;175m    \e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;24;255;175m    \e[38;2;24;255;175;48;2;0;0;0m▄\e[48;2;0;0;0m \e[48;2;177;255;8m           \e[48;2;0;0;0m \e[48;2;254;255;16m         \e[38;2;0;0;0;48;2;254;255;16m▄\e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;255;168;0m▄\e[48;2;255;168;0m         \e[48;2;0;0;0m   \e[38;2;0;0;0;48;2;254;80;12m▄\e[48;2;254;80;12m       \e[38;2;0;0;0;48;2;254;80;12m▄\e[48;2;0;0;0m \e[49m  \e[m");
    // Jerial.println("\e[48;2;0;0;0m \e[48;2;255;0;74m           \e[48;2;0;0;0m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m  \e[48;2;255;43;178m       \e[38;2;0;0;0;48;2;255;43;178m▄\e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[49m  \e[48;2;0;0;0m \e[48;2;205;7;255m    \e[48;2;0;0;0m \e[48;2;84;56;255m     \e[48;2;0;0;0m \e[49m     \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;0;173;255m▄\e[48;2;0;173;255m         \e[38;2;0;0;0;48;2;0;173;255m▄\e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;24;255;175m   \e[48;2;0;0;0m \e[49m \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;24;255;175m   \e[38;2;0;0;0;48;2;24;255;175m▄\e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;177;255;8m▄\e[48;2;177;255;8m      \e[38;2;0;0;0;48;2;177;255;8m▄▄\e[48;2;0;0;0m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;254;255;16m▄▄▄▄▄▄\e[48;2;0;0;0m  \e[49;38;2;0;0;0m▀\e[49m  \e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;255;168;0m▄▄▄▄▄▄▄\e[48;2;0;0;0m \e[49;38;2;0;0;0m▀\e[49m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m        \e[49;38;2;0;0;0m▀\e[49m   \e[m");
    // Jerial.println("\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;0;74m▄\e[48;2;255;0;74m         \e[48;2;0;0;0m  \e[49m  \e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;255;43;178m▄\e[48;2;255;43;178m    \e[38;2;0;0;0;48;2;255;43;178m▄\e[48;2;0;0;0m \e[49;38;2;0;0;0m▀\e[49m \e[48;2;0;0;0m \e[38;2;0;0;0;48;2;205;7;255m▄\e[48;2;205;7;255m  \e[38;2;0;0;0;48;2;205;7;255m▄\e[48;2;0;0;0m \e[49m  \e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;205;7;255m▄▄\e[48;2;0;0;0m   \e[38;2;0;0;0;48;2;84;56;255m▄▄▄\e[48;2;0;0;0m  \e[49m      \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m  \e[38;2;0;0;0;48;2;0;173;255m▄▄▄▄▄\e[48;2;0;0;0m   \e[49;38;2;0;0;0m▀\e[49m \e[48;2;0;0;0m     \e[49;38;2;0;0;0m▀\e[49m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m     \e[49m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m        \e[49;38;2;0;0;0m▀\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m      \e[49;38;2;0;0;0m▀\e[49m     \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m      \e[49;38;2;0;0;0m▀\e[49m     \e[49;38;2;0;0;0m▀▀▀▀▀▀\e[49m     \e[m");
    // Jerial.println("\e[49m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m \e[38;2;0;0;0;48;2;255;0;74m▄\e[48;2;255;0;74m     \e[38;2;0;0;0;48;2;255;0;74m▄\e[48;2;0;0;0m  \e[49m    \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m      \e[49;38;2;0;0;0m▀\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m    \e[49;38;2;0;0;0m▀\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m   \e[49;38;2;0;0;0m▀\e[49m \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m   \e[49;38;2;0;0;0m▀\e[49m        \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m     \e[49;38;2;0;0;0m▀▀\e[49m     \e[49;38;2;0;0;0m▀▀▀\e[49m     \e[49;38;2;0;0;0m▀▀▀\e[49m    \e[49;38;2;0;0;0m▀▀▀▀▀▀\e[49m                                         \e[m");
    // Jerial.println("\e[49m  \e[49;38;2;0;0;0m▀\e[48;2;0;0;0m       \e[49;38;2;0;0;0m▀\e[49m       \e[49;38;2;0;0;0m▀▀▀▀\e[49m      \e[49;38;2;0;0;0m▀▀\e[49m                                                                                                  \e[m");
    
   }