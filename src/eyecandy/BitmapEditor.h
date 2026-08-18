/**
 * Bitmap Editor - Simple bitmap editor for .bin files
 * 
 * Allows editing OLED bitmap files using block characters.
 * Supports both full-block and quarter-block display modes.
 */

#ifndef BITMAP_EDITOR_H
#define BITMAP_EDITOR_H

#include <Arduino.h>
#include <FatFS.h>
#include "MenuBars.h"



#define CURSOR_COLOR_ON 198
#define CURSOR_COLOR_OFF 63

#define PROBE_MENU_TEXT_COLOR 122
#define ENCODER_MENU_TEXT_COLOR 221
#define TERMINAL_MENU_TEXT_COLOR 171


// Display modes
enum BitmapDisplayMode {
    MODE_FULL_BLOCKS,    // Each pixel = one block character (may need scrolling)
    MODE_HALF_BLOCKS,    // Each char = 2 pixels vertically (fits 128×32 on 80×16)
    MODE_QUARTER_BLOCKS  // Each char = 2×2 pixels with crosshair cursor
};

// Encoder movement modes
enum EncoderMovementMode {
    ENCODER_HORIZONTAL,  // Encoder moves cursor left/right
    ENCODER_VERTICAL     // Encoder moves cursor up/down
};

// Draw modes
enum DrawMode {
    DRAW_SET,     // Always set pixels (draw)
    DRAW_CLEAR,   // Always clear pixels (erase)
    DRAW_TOGGLE   // Toggle pixels (default)
};

class BitmapEditor {
public:
    BitmapEditor();
    ~BitmapEditor();
    bool active;
    static BitmapEditor& getInstance() {
        static BitmapEditor instance;
        return instance;
    }
    
    // Load bitmap from file
    bool loadFile(const String& filepath);
    
    // Create new bitmap with given dimensions
    bool newFile(const String& filepath, int width = 0, int height = 0);
    
    // Main editor loop
    void run();
    
    // Save bitmap back to file
    bool save();
    
private:
    // Bitmap data
    uint8_t* bitmapData;
    int width;
    int height;
    size_t dataSize;
    String filepath;
    bool hasHeader;  // Whether file has 4-byte header
    
    // Editor state
    int cursorX;
    int cursorY;
    int prevCursorX;  // Previous cursor position for incremental updates
    int prevCursorY;
    int prevScrollX;  // Previous scroll positions to detect changes
    int prevScrollY;
    int scrollX;  // Horizontal scroll offset
    int scrollY;  // Vertical scroll offset
    BitmapDisplayMode displayMode;
    bool modified;
    bool running;
    bool needFullRedraw;  // Flag to force full screen redraw
    
    // Terminal size (queried on startup)
    int termWidth;
    int termHeight;
    
    // Encoder state (like eKilo)
    long last_encoder_position;
    unsigned long last_encoder_update;
    bool last_button_state;
    unsigned long button_debounce_time;
    EncoderMovementMode encoderMode;  // Horizontal or vertical movement
    DrawMode drawMode;                // Set, clear, or toggle pixels
    
    // Menu bar navigation
    MenuBar menuBar;
    bool inMenuMode;                  // True when navigating menu bar
    int menuSelectedIndex;            // Currently selected menu item
    unsigned long lastSwitchCheck;    // Last time we checked probe switch
    
    // Menu bar state tracking (static storage for menu values)
    int menuViewModeValue;
    int menuEncModeValue;
    int menuDrawModeValue;
    
    // Display functions
    void drawScreen();
    void drawScreenIncremental();  // Only redraw changed lines
    void drawLine(int lineY);      // Draw a single line
    void drawStatus();
    void drawBitmapFullBlocks();
    void drawBitmapHalfBlocks();
    void drawBitmapQuarterBlocks();
    void drawOLED();
    
    // Terminal utilities
    void queryTerminalSize();
    
    // Pixel operations
    bool getPixel(int x, int y);
    void setPixel(int x, int y, bool value);
    void togglePixel(int x, int y);
    
    // Quarter block helpers
    const char* getQuarterBlockString(bool tl, bool tr, bool bl, bool br);
    void cycleQuarterBlock(int blockX, int blockY);
    
    // Input handling
    void handleInput(int ch);
    void processEncoderInput();  // Like eKilo
    void moveCursor(int dx, int dy);
    void toggleCurrentPixel();
    void switchDisplayMode();
    
    // Utility
    void updateScroll();
    void showHelp();
    String formatSize(size_t bytes);
    void setupMenuBar();              // Initialize menu bar items
    void handleMenuNavigation(int ch); // Handle menu bar navigation
};

// Launch bitmap editor (called from file manager)
bool launchBitmapEditor(const String& filepath);

#endif // BITMAP_EDITOR_H

