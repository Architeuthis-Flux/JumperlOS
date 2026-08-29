#ifndef OLED_H
#define OLED_H

#include "Adafruit_SSD1306.h"
#include "JumperlessDefines.h"
#include "Arduino.h"
#include "Wire.h"
#include <vector>
#include "config.h"

// Core fonts - small and large sizes for auto-fallback
#include "fonts/Eurostile8pt7b.h"
#include "fonts/Eurostile12pt7b.h"
#include "fonts/Jokerman8pt7b.h"
#include "fonts/Jokerman12pt7b.h"
#include "fonts/Comic_Sans8pt7b.h"
#include "fonts/Comic_Sans12pt7b.h"
#include "fonts/Courier_New8pt7b.h"
#include "fonts/Courier_New12pt7b.h"
#include "fonts/new_science_medium8pt7b.h"
#include "fonts/new_science_medium12pt7b.h"
#include "fonts/new_science_medium_extended8pt7b.h"
#include "fonts/new_science_medium_extended12pt7b.h"
#include "fonts/ANDALEMO5pt7b.h"
#include "fonts/FreeMono4pt7b.h"
#include "fonts/FreeMono5pt7b.h"

// New fonts - granular sizes (5pt to 15pt for smooth scaling)
#include "fonts/BerkeleyMono8pt7b.h"    
#include "fonts/BerkeleyMono12pt7b.h"
#include "fonts/Pragmatism5pt7b.h"
#include "fonts/Pragmatism6pt7b.h"
#include "fonts/Pragmatism7pt7b.h"
#include "fonts/Pragmatism8pt7b.h"
#include "fonts/Pragmatism9pt7b.h"
#include "fonts/Pragmatism10pt7b.h"
#include "fonts/Pragmatism11pt7b.h"
#include "fonts/Pragmatism12pt7b.h"
#include "fonts/IosevkaSS08_Regular9pt7b.h"
#include "fonts/IosevkaSS08_Regular11pt7b.h"
#include "fonts/IosevkaSS08_Regular12pt7b.h"
#include "fonts/IosevkaSS08_Regular13pt7b.h"
#include "fonts/IosevkaSS08_Regular14pt7b.h"
#include "fonts/IosevkaSS08_Regular15pt7b.h"

// Small fonts for file manager and detailed displays (4-5pt for better readability)
#include "fonts/ubuntu5pt7b.h"
#include "fonts/DotGothic16_Regular4pt7b.h"
#include "fonts/IosevkaSS08_Light5pt7b.h"
#include "fonts/EnvyCodeRNerdFont_Regular5pt7b.h"

//#include "fonts/JumperlessLowerc12pt7b.h"

class Adafruit_SSD1306;

extern bool oledConnected;
extern bool oledUsingHardwiredPins; // Global flag: true if using RP6/RP7 (GPIO 6/7), false if using crossbar

// Get reference to the display object - allows runtime switching between Wire/Wire1
Adafruit_SSD1306& getDisplay();



#define OLED_RESET -1

// Forward declarations for font bounds structures
struct CharBounds;
struct TextBounds;
struct FontMetrics;

// Font family enumeration for automatic size selection
enum FontFamily {
    FONT_EUROSTILE = 0,
    FONT_JOKERMAN = 1,
    FONT_COMIC_SANS = 2,
    FONT_COURIER_NEW = 3,
    FONT_NEW_SCIENCE_MEDIUM = 4,
    FONT_NEW_SCIENCE_MEDIUM_EXTENDED = 5,
    FONT_ANDALE_MONO = 6,
    FONT_FREE_MONO = 7,
    FONT_IOSEVKA_REGULAR = 8,
    FONT_BERKELEY_MONO = 9,
    FONT_PRAGMATISM = 10
};

// Small font enumeration for file manager and detailed displays
enum SmallFont {
    SMALL_FONT_UBUNTU = 0,
    SMALL_FONT_DOTGOTHIC = 1,
    SMALL_FONT_JOKERMAN = 2,
    SMALL_FONT_ANDALE_MONO = 3,
    SMALL_FONT_IOSEVKA_REGULAR = 4,
    SMALL_FONT_IOSEVKA_5PT = 5,
    SMALL_FONT_PRAGMATISM_5PT = 6,
    SMALL_FONT_FREEMONO_5PT = 7,
    SMALL_FONT_ENVYCODE_5PT = 8
};

// Default small font for file manager and detailed displays
#define DEFAULT_SMALL_FONT SMALL_FONT_PRAGMATISM_5PT

// Positioning mode enum for simplified positioning system
enum PositionMode {
    POS_AUTO,      // Automatically choose best positioning
    POS_TIGHT,     // Force tight positioning (topmost pixels at exact y)
    POS_BASELINE   // Force baseline positioning (traditional font baseline)
};

// OLED framebuffer mode for dual framebuffer support
enum OLEDMode {
    MODE_LARGE_TEXT,    // Menus, highlighting, big messages (textSize >= 2 or fonts > 8pt)
    MODE_SMALL_TEXT     // File editing, terminal output (textSize == 1 or small fonts)
};

// Horizontal alignment for a rendered row in clearPrintShowRich().
enum OledAlign {
    OLED_ALIGN_LEFT = 0,    // flush left (small left pad)
    OLED_ALIGN_CENTER = 1,  // horizontally centered
    OLED_ALIGN_RIGHT = 2,   // flush right
    // Segment-only: flow with the row's packed group using the row's align.
    // (Not valid as a row alignment - rows always pick a concrete anchor.)
    OLED_ALIGN_INHERIT = 3,
    // Row-only: spread the row's flowing segments evenly across the panel -
    // first flush left, last flush right, the rest interpolated between
    // (one segment just centers). The even spacing comes from measured
    // pixel widths, so nothing can leak off the panel the way fixed
    // character-count layouts did. Segments with their own LEFT/CENTER/
    // RIGHT anchor still break out and anchor independently.
    OLED_ALIGN_JUSTIFY = 4
};

// A single text segment within a row. Each segment carries its own font
// (index into fontList[] in oled.cpp), so a single visual line can mix
// font sizes - e.g. a tiny "connect" verb followed by larger node ids.
//
// `align` lets a segment break out of the row's left->right flow and anchor
// itself independently on the panel. OLED_ALIGN_INHERIT (the default) keeps
// the segment in the row's packed, flowing group; LEFT/CENTER/RIGHT pin the
// segment to that panel edge regardless of the other segments - e.g. center
// the verb but flush the node ids left.
struct OledTextSeg {
    const char* text;   // segment text (no '\n')
    int16_t fontIndex;  // index into fontList[]
    OledAlign align;    // per-segment anchor; INHERIT = flow with the row
};

// Max segments laid out horizontally on one row.
#define OLED_RICH_MAX_SEGS 4

// One rendered row: up to OLED_RICH_MAX_SEGS segments drawn left->right
// sharing a common baseline, with `segGap` px between them. The whole row
// is positioned horizontally per `align`.
struct OledTextRow {
    OledTextSeg segs[OLED_RICH_MAX_SEGS];
    uint8_t     segCount;
    OledAlign   align;
    int8_t      segGap;     // px between adjacent segments
    int8_t      fixedH;     // >0: use this as the row height instead of the
                            // measured ink box, so rows with different text
                            // (descenders, ascender mixes) share a stable
                            // baseline; descender tails draw into the gap
};

// Comprehensive text information structure
struct TextInfo {
    uint8_t textSize;       // Current text size multiplier
    const GFXfont* font;    // Current font pointer
    int16_t height;         // Current text height
    int16_t lineHeight;     // Current line spacing
    int16_t ascent;         // Current ascent
    int16_t cursorX;        // Current cursor X position
    int16_t cursorY;        // Current cursor Y position
};

void OLEDprintFromTerminal( void );

class oled {
public:
    oled();
    int init();
    void test();
    void testMenuPositioning();
    void testSmallFonts();
    
    // Basic print functions
    void print(const char* s);
    void print(const String& s);
    void print(int i);
    void print(char c);
    void print(float f);
    void println(const char* s);
    void println(const String& s);
    void println(int i);
    void println(float f);
    
    // Display functions
    void displayBitmap(int x, int y, const unsigned char* bitmap, int width, int height);
    void showJogo32h();
    // Periodic maintenance (connection health check, auto-reinit)
    void oledPeriodic();

    // Hold the current frame for `durationMs`. show()/flushFramebuffer()
    // calls during this window mutate the live framebuffer normally but
    // skip the I2C transmit, so the panel stays frozen on whatever was
    // last pushed. When the hold expires, oledPeriodic() flushes the
    // final framebuffer state. Calling again during an active hold
    // extends the timer (never shortens it).
    void oledHold(uint32_t durationMs);

    // Preferred entry point when a held message will be priority-flushed
    // immediately after this call (e.g. the undo toast). Snapshots the
    // current live framebuffer into a shadow buffer BEFORE the caller
    // paints the held message, then arms a post-flush latch. The next
    // priority flush (clearPrintShowSmall with show=true) transmits the
    // held message to the panel and then restores the shadow back into
    // the live framebuffer, so any probe-mode / clearHighlighting / etc.
    // writes during the hold window accumulate against the pre-toast
    // background instead of layering on top of toast pixels. When the
    // hold expires, oledPeriodic() flushes the accumulated live buffer
    // (background + intervening writes, no toast residue) cleanly.
    //
    // If the OLED isn't connected or shadow allocation fails, this
    // degrades silently to a plain oledHold(durationMs).
    void oledHoldBegin(uint32_t durationMs);

    bool oledIsHeld() const;
    // Force-render path that bypasses clearPrintShow's auto-fit ladder
    // and renders directly with two fixed fonts (top + bottom line),
    // regardless of any active hold. Used by the undo toast and history
    // scrub menu to keep a stable 2-line layout that doesn't size-flicker
    // when the label width straddles a tier boundary. Default font
    // indices are tuned for the toast: a larger top line (the verb) and
    // a more compact bottom line (the detail) - callers that want
    // matched line sizes can pass the same index for both.
    //
    //   topFontIndex / botFontIndex: indices into fontList[] in oled.cpp.
    //   line_gap: pixels between the two lines (vertical block centered).
    //   leftJustifyTop: when true, the top line is flush-left at x=0
    //     instead of horizontally centered. Bottom line stays centered.
    void clearPrintShowSmall(const char* text,
                             bool clear = true,
                             bool show = true,
                             int topFontIndex = 21,   // Pragmatism 10pt
                             int botFontIndex = 18,   // Pragmatism 7pt
                             int line_gap = 4,
                             bool leftJustifyTop = true);

    // Granular multi-row / multi-font renderer. Draws up to `rowCount`
    // rows, vertically centered as a block with `rowGap` px between rows.
    // Each row may contain several segments (see OledTextRow) that share a
    // baseline so mixed font sizes line up, and each row is aligned
    // independently. Shares the same priority-flush + hold-stash behavior
    // as clearPrintShowSmall, so it is safe for back-to-back toasts.
    // clearPrintShowSmall is implemented on top of this.
    // topAnchor: pin the block to the top of the panel instead of
    // vertically centering it (highlight readings put their small header
    // row flush against the top edge).
    void clearPrintShowRich(const OledTextRow* rows,
                            int rowCount,
                            int rowGap = 2,
                            bool clear = true,
                            bool show = true,
                            bool topAnchor = false);

    // Priority-flush the CURRENT live framebuffer to the panel, bypassing the
    // hold gate, then run the oledHoldBegin stash so any writes during the hold
    // window accumulate against the pre-toast background (see oledHoldBegin).
    // This is the same flush that clearPrintShowRich performs internally, split
    // out so a caller that paints the framebuffer by another means (e.g. an
    // OledScreen rendered via the retained oledgui scene graph) gets identical
    // back-to-back-toast + probe-during-hold behavior. Call oledHoldBegin()
    // before painting and this immediately after, exactly like the rich path.
    void priorityFlushHeldFrame();

    bool clear(int waitToFinish = 0);
    bool show(int waitToFinish = 0);
    void moveToNextLine();
    
    // Unified cursor positioning
    void setCursor(int16_t x, int16_t y, PositionMode mode);
    void setCursor(int16_t x, int16_t y);  // Auto mode
    
    // Display settings
    void setTextColor(uint32_t color);
    void setTextSize(uint8_t size);
    void invertDisplay(bool inv);
    void setDisplayClock(uint8_t divideRatio, uint8_t oscillatorFreq = 0x8);
    void setDisplayClockRaw(uint8_t clockDivOscSetting);
    void setDisplayClockDivideRatio(int divideRatio);
    
    // Connection management
    bool isConnected() const;
    int connect(void);
    void disconnect(void);
    bool checkConnection(bool force = false);
    
    // Font management
    int cycleFont(void);
    FontFamily getFontFamily(const char* fontName);   // allocation-free
    FontFamily getFontFamily(String fontName);
    int setFont(String fontName, int justGetIndex = 0);
    int setFont(char* fontName, int justGetIndex = 0);
    void setFont(const GFXfont* font);
    void setFont(FontFamily fontFamily);
    void setFont(int fontIndex);
    void setFontForSize(FontFamily family, int textSize);  // Backwards compatible (textSize 1/2)
    void setFontPointSize(FontFamily family, uint8_t pointSize);  // New: granular point size control
    String getFontName(FontFamily fontFamily);
    
    // Current font point size tracking
    uint8_t currentPointSize = 12;  // Track current font point size for intelligent scaling
    
    // Simplified display functions
    void clearPrintShow(const char* text, int textSize = 2, bool clear = true, bool show = true, bool center = true, int x_pos = -1, int y_pos = -1, int waitToFinish = 0);
    void clearPrintShow(const char* text, int textSize, FontFamily family, bool clear = true, bool show = true, bool center = true, int x_pos = -1, int y_pos = -1, int waitToFinish = 0);
    
    void clearPrintShow(const char* text, int textSize = 2, int waitToFinish = 0);
    void clearPrintShow(const String& text, int textSize, int waitToFinish = 0);
    void clearPrintShow(const String& text, int textSize, bool clear, bool show, bool center, int x_pos = -1, int y_pos = -1, int waitToFinish = 0);
    void clearPrintShow(const String& text, int textSize, FontFamily family, bool clear = true, bool show = true, bool center = true, int x_pos = -1, int y_pos = -1, int waitToFinish = 0);

    
    // Helper for multi-line text
    void displayMultiLineText(const char* text, bool center);
    
    // Debug functions
    void dumpFrameBufferQuarterSize(int clearFirst = 0, int x_pos = 40, int y_pos = 24, int border = 1);
    void dumpFrameBuffer(Stream* stream = nullptr);
    // Machine-readable framebuffer dump in a selectable encoding (see OledDumpEnc):
    // quarter/full = ASCII art, b64/raw = compact SSD1306 page-buffer bytes for host UIs.
    void dumpFrameBufferEncoded(Stream* stream, uint8_t enc);
    // Push the current framebuffer to oledSer3Target if streaming is enabled.
    // Change-detected (skips identical frames) and back-pressure aware (drops
    // when the host isn't draining), so it is safe to call on every show().
    void streamFrameToSer3();
    
    // Positioning functions (simplified)
    void getCenteredPosition(const char* str, int16_t* x, int16_t* y, PositionMode mode);
    void getCenteredPosition(const char* str, int16_t* x, int16_t* y);  // Auto mode
    
    // Font metrics helpers
    int getCharacterWidth();
    
    // Font bounds and measurement functions
    FontMetrics getFontMetrics();
    TextBounds getTextBounds(const char* str);
    TextBounds getTextBounds(const String& str);
    bool textFits(const char* str);
    
    // Small text and file system display functions
    void printSmallText(const char* text, int16_t x, int16_t y, bool clear = false);
    void printSmallTextLine(const char* text, int line, bool clear = false);
    void clearLine(int line);
    void showFileStatus(const char* currentPath, int fileCount, const char* selectedFile);
    void showFileStatusScrolled(const char* visibleText, int fileCount, int cursorPosition);
    /** Breadboard-style: two lines of up to 7 chars each (top and bottom) */
    void showFileStatusBreadboard(const char* lineTop7, const char* lineBottom7);
    void showMultiLineSmallText(const char* text, bool clear = true, bool show = true);
    void resetMultiLineSmallText(); // Reset scroll position for showMultiLineSmallText
    
    // Advanced small text buffer display with editing support
    struct SmallTextDisplayConfig {
        const char* text;              // Text buffer to display
        SmallFont font;                // Font to use
        bool clear_before;             // Clear display before drawing
        bool show_after;               // Show display after drawing
        bool enable_cursor;            // Show cursor (for editing mode)
        int cursor_line;               // Cursor line position (0-based)
        int cursor_col;                // Cursor column position (0-based)
        int start_line;                // First line to display (for scrolling)
        int max_lines;                 // Maximum lines to show (-1 = auto-calculate)
        int horizontal_offset;         // Horizontal scroll offset for current line
        bool highlight_cursor_line;    // Highlight entire cursor line
        const char* status_text;       // Optional status text at bottom
    };
    
    void showSmallTextBuffer(const SmallTextDisplayConfig& config);
    
    // Small font functions
    void setSmallFont(SmallFont smallFont);
    void useSmallFont(SmallFont smallFont, const char* text, int16_t x = 0, int16_t y = 0, bool clear = false);
    void useSmallFontAndRestore(SmallFont smallFont, const char* text, int16_t x = 0, int16_t y = 0, bool clear = false, bool show = false);
    void restoreNormalFont();
    
    // Drawing primitives
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color = 1);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color = 1);
    
    // Simple framebuffer management
    void clearFramebuffer();
    void setPixel(int16_t x, int16_t y, uint16_t color = 1);
    void drawChar(int16_t x, int16_t y, char c);
    void drawText(int16_t x, int16_t y, const char* text);
    void drawHighlightedChar(int16_t x, int16_t y, char c); // Highlight a single character
    void flushFramebuffer();
    uint8_t* getFramebuffer(); // Get direct access to framebuffer
    
    // Dual framebuffer mode management
    void setMode(OLEDMode mode);
    OLEDMode getMode() const { return currentMode; }
    void autoDetectMode(int textSize); // Automatically detect and switch mode
    
    // Store config
    int address = -1;
    int sda_pin = -1;
    int scl_pin = -1;
    int sda_row = -1;
    int scl_row = -1;
    int textSize = 2;
    int charPos = 0;
    uint8_t currentTextSize = 1; // Track current text size
    
    unsigned long lastConnectionCheck = 0;
    unsigned long connectionCheckInterval = 2500;
    
    const GFXfont* currentFont;
    int currentFontIndex = 0;

    int fontIndex = 0;

    FontFamily currentFontFamily = FONT_EUROSTILE;
    
    // Small font state tracking
    const GFXfont* previousFont = nullptr;
    FontFamily previousFontFamily = FONT_EUROSTILE;
    SmallFont currentSmallFont = SMALL_FONT_UBUNTU;
    bool usingSmallFont = false;
    
    bool oledConnected = false;
    bool stillWriteToFramebuffer = true;
    
    int connectionRetries = 0;
    int maxConnectionRetries = 4;


    char scratchPad[40];
    char* getScratchPad(void);
    
    // Dynamic display dimensions (from config)
    int displayWidth = jumperlessConfig.top_oled.width;
    int displayHeight = jumperlessConfig.top_oled.height;
    
    // Dual framebuffer support
    OLEDMode currentMode = MODE_LARGE_TEXT;
    uint8_t* largeTextFramebuffer = nullptr;
    uint8_t* smallTextFramebuffer = nullptr;
    int framebufferSize = 0;
    bool dualFramebufferEnabled = false; // Enable when needed for performance
    
    // Terminal emulation state for ANSI/control sequence handling
    int termCursorX = 0;
    int termCursorY = 0;
    bool inEscapeSequence = false;
    char escapeBuffer[32];
    int escapeBufferPos = 0;
    
    // Control sequence parsing helpers (can be made public if needed)
private:
    void parseControlSequence(char c);
    void executeEscapeSequence();
    void moveCursorTerm(int x, int y);
    void clearToEndOfLine();
    void clearLine();
    void clearToEndOfScreen();
    void clearScreen();
};

// Global functions
void oledExample(void);
int initOLED(void);
int oledTest(int sdaRow = NANO_D2, int sclRow = NANO_D3, int sdaPin = 26, int sclPin = 27, int leaveConnections = 1);
void testOLEDSmallFonts(void);
FontFamily mapConfigValueToFontFamily(int configValue);

// =====================================================================
// OLED connection-type helpers
// =====================================================================
// These wrap the disconnect -> pin update -> bus tear-down -> reinit
// sequence in one place. They live alongside the OLED driver because the
// dance is inherently I2C/display-state aware (which Wire to release,
// which to leave alone, when to refresh the framebuffer).

// Number of "cycleable" connection types (skips type 3 = custom, which the
// user must configure manually via sda_pin / scl_pin).
#define OLED_CYCLEABLE_CONNECTION_TYPES 3

// Short, human-friendly name for the connection type (e.g. "GPIO 7/8").
const char* getOledConnectionTypeShortName(int connectionType);

// Default OLED connection type for a given hardware revision.
// Rev <= 6 uses GPIO 7/8 (via crossbar); rev >= 7 uses internal I2C0 (GPIO 4/5).
int defaultOledConnectionTypeForRevision(int revision);

// Switch the OLED to a new connection type in one shot.
// - Disconnects the current OLED (releases crossbar bridges if any).
// - Tears down the OLD I2C peripheral *only when it's safe* (Wire1 is
//   OLED-exclusive on V5 and must be ended before pin swaps; Wire is
//   shared with the MCP4728 DAC and INA219 sensors so we never end it).
// - Updates pins, rows, hardwired flag, and connection_type via
//   updateOledPinsForConnectionType().
// - Marks configChanged so the dirty-tracker will eventually persist.
// - If reinitDisplay is true, calls oled.init() so the change takes effect now.
// - If persist is true, requests an async config save (non-blocking).
// Returns true if the OLED reconnected (or reinit was skipped).
bool applyOledConnectionType(int newConnectionType, bool reinitDisplay = true, bool persist = false);

// Cycle to the next cycleable connection type (skips type 3 = custom).
// Returns the new connection type.
int cycleOledConnectionType(bool reinitDisplay = true, bool persist = false);

// Quickly probe the internal I2C0 bus (Wire on GPIO 4/5) for an OLED at the
// given 7-bit address. Returns true if a device ACKs.
//
// Caller is responsible for ensuring Wire has been initialized (initDAC()
// does this on boot). Uses a tight, short timeout internally so a missing
// device costs only a few milliseconds rather than the platform default.
// Restores Wire's timeout to a bounded "OLED-friendly" value (15ms) on exit
// so that future I2C0 transactions can't stall the main loop indefinitely
// even if the OLED is later unplugged.
bool probeOledOnInternalI2C0(uint8_t address);

// Run the boot-time OLED auto-detect + config-migration sequence.
//
// Call once from setup() AFTER initDAC()/initINA219() have brought Wire up,
// and BEFORE the firstLoop==2 path that decides whether to oled.init().
//
// Behavior when an OLED is detected on the internal I2C0 bus:
//   * Promotes hardware.revision to 7 if it's currently lower (rev 7 is
//     the silicon that broke out GPIO 4/5 as a usable I2C bus).
//   * Switches top_oled.connection_type to 2 (internal I2C0) on first
//     migration; leaves it alone on already-rev-7 boards so a manual user
//     choice isn't silently overridden.
//   * Forces top_oled.enabled = 1 and top_oled.connect_on_boot = 1 so
//     the existing firstLoop==2 gate will actually call oled.init().
//   * Persists every change in a single saveConfig() call.
//
// Behavior when no OLED is detected: nothing is modified.
//
// Returns true iff a device was detected on the bus.
bool autoDetectAndConfigureOled(void);

// Bitmap buffer access - exposed for MicroPython
extern uint8_t customBitmapBuffer[1024];
extern int customBitmapWidth;
extern int customBitmapHeight;
extern bool customBitmapLoaded;
bool loadBitmapFromFile(const char* filepath);

// Bitmap data
extern const unsigned char jogo255[];
extern const unsigned char jogo32h[];
extern const unsigned char logo[];
extern const unsigned char ColorJumpLogo[];

// Embedded binary images for filesystem provisioning
extern const unsigned char bubbleJumpThin_bin[];
extern const unsigned int bubbleJumpThin_bin_len;
extern const unsigned char bubbleJump_bin[];
extern const unsigned int bubbleJump_bin_len;
extern const unsigned char jogo32h_file_bin[];
extern const unsigned int jogo32h_file_bin_len;
extern const unsigned char bubbleJumpThiccWhite_bin[];
extern const unsigned int bubbleJumpThiccWhite_bin_len;
extern const unsigned char excelGUI_bin[];
extern const unsigned int excelGUI_bin_len;
// ---- Paste into src/oled.h (extern declarations) ----

// Embedded binary images for filesystem provisioning (generated)
extern const unsigned char dayglow_bin[];
extern const unsigned int dayglow_bin_len;
extern const unsigned char eevblog_bin[];
extern const unsigned int eevblog_bin_len;
extern const unsigned char excelGUI_bin[];
extern const unsigned int excelGUI_bin_len;
extern const unsigned char jogo32h_bin[];
extern const unsigned int jogo32h_bin_len;
extern const unsigned char jogotextInv_bin[];
extern const unsigned int jogotextInv_bin_len;
extern const unsigned char jumperless_text_bin[];
extern const unsigned int jumperless_text_bin_len;
extern const unsigned char ksc_bin[];
extern const unsigned int ksc_bin_len;

// Font structure - now includes point size and spacing info for granular control
struct font {
    const GFXfont* font;
    const char* shortName;
    const char* longName;
    int16_t topRowOffset;  // Y offset to apply when text is on top row with multiple lines
    FontFamily family;
    uint8_t pointSize;     // Actual point size (5-15pt) for granular scaling
};

// Per-family font characteristics for intelligent scaling
struct FontFamilyCharacteristics {
    FontFamily family;
    float lineSpacingMultiplier;  // Multiply lineHeight by this (1.0 = normal, 1.2 = 20% more space)
    int8_t verticalClipTolerance; // Pixels that can be clipped without triggering scaling (for descenders)
    int8_t horizontalClipTolerance; // Pixels that can be clipped horizontally
};

// FontManager - Lightweight font lookup system stored in flash
// Provides granular font scaling with minimal RAM usage
class FontManager {
public:
    // Find best font for family and desired point size
    // Returns font index, automatically selects closest available size
    static int getFontForPointSize(FontFamily family, uint8_t desiredPointSize);
    
    // Convert old textSize (1/2) to point size for backwards compatibility
    // Now display-size aware for better scaling
    static uint8_t textSizeToPointSize(int textSize, int displayHeight = 32);
    
    // Find largest font that fits given text width
    // Returns optimal point size for the text to fit within maxWidth
    static uint8_t findBestFitPointSize(FontFamily family, const char* text, int16_t maxWidth, uint8_t maxPointSize = 15, uint8_t minPointSize = 5);
    
    // Get all available point sizes for a font family (stored in PROGMEM)
    static void getAvailableSizes(FontFamily family, uint8_t* sizes, int* count);
    
private:
    // Helper: find closest font index for a specific point size
    static int findClosestFont(FontFamily family, uint8_t pointSize);
};

// Character bounds structure for returning detailed character metrics
struct CharBounds {
    int16_t width;      // Character width in pixels
    int16_t height;     // Character height in pixels
    int16_t xAdvance;   // Horizontal advance (cursor movement)
    int16_t xOffset;    // X offset from cursor position
    int16_t yOffset;    // Y offset from cursor position
    int16_t x1, y1;     // Top-left bounding box corner
    int16_t x2, y2;     // Bottom-right bounding box corner
};

// String bounds structure for returning text measurements
struct TextBounds {
    int16_t width;      // Total string width in pixels
    int16_t height;     // Total string height in pixels
    int16_t x1, y1;     // Top-left bounding box corner
    int16_t x2, y2;     // Bottom-right bounding box corner
    int16_t baseline;   // Baseline position
    int16_t ascent;     // Distance from baseline to top
    int16_t descent;    // Distance from baseline to bottom
};

// Font metrics structure for returning font characteristics
struct FontMetrics {
    int16_t lineHeight;     // Line spacing (yAdvance)
    int16_t ascent;         // Maximum ascent in font
    int16_t descent;        // Maximum descent in font  
    int16_t maxWidth;       // Maximum character width
};

// Font list
extern struct font fontList[];
extern int numFonts;

// Global instance
extern class oled oled;

// =====================================================================
// USBSer3 framebuffer streaming (push-on-show)
// =====================================================================
// When oledSer3Stream is true, oled::show() / priorityFlushHeldFrame()
// emit one machine-readable frame to oledSer3Target (the USBSer3
// backchannel) in the oledSer3Enc encoding. Driven from the backchannel
// verb :oled:stream:on[:enc] / :oled:stream:off (SingleCharCommands.cpp).
enum OledDumpEnc {
    OLED_ENC_QUARTER = 0,  // 2x2 quarter-block ASCII art (default, human-readable)
    OLED_ENC_FULL    = 1,  // full per-pixel block ASCII art
    OLED_ENC_B64     = 2,  // base64 of the raw SSD1306 page buffer
    OLED_ENC_RAW     = 3   // raw bytes of the SSD1306 page buffer
};
extern volatile bool oledSer3Stream;   // push a frame on every show()
extern volatile uint8_t oledSer3Enc;   // OledDumpEnc encoding for the stream
extern Stream* oledSer3Target;         // destination (set to &USBSer3 by the backchannel)

#endif


