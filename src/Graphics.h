#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <Arduino.h>
#include "LEDs.h"
#include "Jerial.h"

extern const int screenMap[445];
extern const int screenMapNoRails[445];

extern volatile bool dumpingToSerial;

extern bool disableTerminalColors;

class bread {
public:
  bread();
  void print(const char c);
  void print(const char c, int position);
  void print(const char c, uint32_t color);
  void print(const char c, uint32_t color, int position, int topBottom);
  void print(const char c, uint32_t color, int position);
  void print(const char c, uint32_t color, uint32_t backgroundColor);
  void print(const char c, uint32_t color, uint32_t backgroundColor,
             int position, int topBottom);
  void print(const char c, uint32_t color, uint32_t backgroundColor,
             int position, int topBottom, int nudge);
  void print(const char c, uint32_t color, uint32_t backgroundColor,
             int position, int topBottom, int nudge, int lowercase);
  void print(const char c, uint32_t color, uint32_t backgroundColor,
             int position);

  void print(const char *s);
  void print(const char *s, int position);
  void print(const char *s, uint32_t color);
  void print(const char *s, uint32_t color, int position);
  void print(const char *s, uint32_t color, int position, int topBottom);
  void print(const char *s, uint32_t color, uint32_t backgroundColor);
  void print(const char *s, uint32_t color, uint32_t backgroundColor,
             int position, int topBottom);
  void print(const char *s, uint32_t color, uint32_t backgroundColor,
             int position, int topBottom, int nudge);
  void print(const char *s, uint32_t color, uint32_t backgroundColor,
             int position, int topBottom, int nudge, int lowercaseNumber);
  void print(const char *s, uint32_t color, uint32_t backgroundColor,
             int position);

  void print(int i);
  void print(int i, int position);
  void print(int i, uint32_t color);
  void print(int i, uint32_t color, int position);
  void print(int i, uint32_t color, int position, int topBottom);
  void print(int i, uint32_t color, int position, int topBottom, int nudge);
  void print(int i, uint32_t color, int position, int topBottom, int nudge,
             int lowercase);
  void print(int i, uint32_t color, uint32_t backgroundColor);

  void barGraph(int position, int value, int maxValue, int leftRight,
                uint32_t color, uint32_t bg);

  void printMenuReminder(int menuDepth, uint32_t color);
  void printRawRow(uint8_t data, int row, uint32_t color, uint32_t bg,
                   int scale = 1);

 // int lightUpNode(int node, uint32_t color);
  void clear(int topBottom = -1);
  void lightUpNode(int node, uint32_t color);
};



struct logoLedAssoc {
  int ledNumber;
  const char* text;
  int type;
  int defaultColor;
  int overrideMap;
  logoOverrideNames overrideName;
};

extern logoLedAssoc logoLedAssociations[20];
extern volatile bool logoLedAccess;
struct specialRowAnimation {
  int index;
  int net;
  int row;
  unsigned long currentFrame;
  int direction;
  int numberOfFrames = 8;
  uint32_t frames[15] = {0xffffff};
  unsigned long lastFrameTime;
  unsigned long frameInterval = 100;
  int type;
};

// initRowAnimations() fills a fixed set of slots: indices 0..37 (3 rails +
// 10 gpio idle + 20 gpio keeper hi/lo + warning/net/row/probe-connect) and
// showAllRowAnimations() paints them across the 5-LED-per-row matrix. The OG
// has 1 LED/row and renders nets as plain lines, so that matrix animation can't
// render: the whole subsystem early-returns on OG (initRowAnimations /
// assignRowAnimations / showAllRowAnimations / showRowAnimation, see
// Graphics.cpp) and the table is sized to a single dummy slot to reclaim
// ~3.7 KB SRAM. The [1] slot keeps the symbol valid for Debugs' sizeof() and
// Highlighting's reset loop (bounded by numberOfRowAnimations, which stays 0 on
// OG because initRowAnimations early-returns). V5 keeps the full [50].
#if defined(OG_JUMPERLESS)
#define ROW_ANIMATION_COUNT 1
#else
// initRowAnimations() writes slots 0-37 (3 rails + 10 idle + 20 keeper + the
// four tail animations, whose data lands one past their .index stamp); 40
// leaves slack. Was 50 with 12 slots never touched (~1.2 KB of .bss).
#define ROW_ANIMATION_COUNT 40
#endif


// The current-sense overlay animates a path across the 5-LED-per-row breadboard
// matrix. On the OG (1 LED/row) it can't render, so the three pixel arrays in
// CurrentSenseOverlayState (3 x this x 4 bytes ~= 3.8 KB) are shrunk to reclaim
// scarce RP2040 SRAM (heap headroom for boot).
#if defined(OG_JUMPERLESS)
static constexpr int kCurrentSenseMaxPathLength = 8;
#else
static constexpr int kCurrentSenseMaxPathLength = 320; // Allow full breadboard coverage (60 rows × 5 columns + margin)
#endif
static constexpr int kCurrentSensePatternLength = 5;
static constexpr float kCurrentSenseMinMotionCurrent_mA = 0.05f;
static constexpr uint8_t kCurrentSenseRowTintAlpha = 50;
static constexpr uint8_t kCurrentSensePathTintAlpha = 50;

struct CurrentSenseOverlayState {
  bool pathValid = false;
  int plusRow = -1;
  int minusRow = -1;
  
  // Separate pixel collections for cleaner animation
  int plusNetPixels[kCurrentSenseMaxPathLength] = {0};
  int plusNetLength = 0;
  
  int virtualWirePixels[kCurrentSenseMaxPathLength] = {0};
  int virtualWireLength = 0;
  
  int minusNetPixels[kCurrentSenseMaxPathLength] = {0};
  int minusNetLength = 0;
  
  float accumulator = 0.0f;
  int patternOffset = 0;
  unsigned long lastUpdateMs = 0;
  int virtualWireNode1 = -1;  // Track virtual wire endpoints
  int virtualWireNode2 = -1;
};

// Single shared instance, defined in Graphics.cpp. This used to be `static`
// right here in the header, which gave every including TU its OWN 3.8 KB
// copy - NetsToChipConnections' debug path was reading a permanently-default
// instance while Graphics.cpp animated a different one.
extern CurrentSenseOverlayState currentSenseOverlayState;

extern int defNudge;

extern specialRowAnimation rowAnimations[ROW_ANIMATION_COUNT];

extern specialRowAnimation warningRowAnimation;
extern specialRowAnimation warningNetAnimation;

extern int numberOfRowAnimations;
extern bool animationsEnabled;
extern char defconString[16];
extern const uint8_t font[][3];
extern volatile int doomOn;

extern uint32_t gpioReadingColors[50];  // 10 real + 8 fake out + 32 fake in
extern uint8_t gpioAnimationBaseHues[10];

extern int menuBrightnessSetting;
extern bread b;

extern const int highSaturationSpectrumColors[51];
extern const int highSaturationSpectrumColorsCount;
extern const int highSaturationBrightColors[29];
extern const int highSaturationBrightColorsCount;

void showSwitchPosition(int position, String text = " ", uint32_t color = 0x000000, uint32_t textColor = 0x000000);
void printSpectrumOrderedColorCube(void);

void playDoom(void);
void showArray(uint8_t *array, int size);
void initRowAnimations(void);
void showAllRowAnimations(void);
void assignRowAnimations(void);
void showRowAnimation(int net);
void showRowAnimation(int index, int net);
void animateBrightenedRow(int net);
void printGraphicsRow(uint8_t data, int row, uint32_t color = 0xFFFFFF,
                      uint32_t bg = 0xFFFFFF);

void scrollFont(void);

void printChar(const char c, uint32_t color = 0xFFFFFF, uint32_t bg = 0xFFFFFF,
               int position = 0, int topBottom = -1, int nudge = 0,
               int lowercase = 0);

void printString(const char *s, uint32_t color = 0xFFFFFF,
                 uint32_t bg = 0xFFFFFF, int position = 0, int topBottom = -1,
                 int nudge = 0, int lowercase = 0);

void drawWires(int net = -1);
void renderNetCurrentAnts(void);
// Debug: verify every animated path's ant pixel sequence is physically
// continuous (printed by the 'i!' net voltage scan report).
void printAntPathContinuity(Stream* out);
void printWireStatus(Stream* target = nullptr);

// void defcon(int start, int spread, int color = 0, int nudge = 1);

void printTextFromMenu(int print = 1);
int attractMode(void);

void changeTerminalColor(int termColor = -1, bool flush = true,
                         Stream *stream = &Jerial);

// void cycleTerminalColor(bool reset = false, bool reverse = false, int step = -1, bool flush = true, Stream *stream = &Serial);
extern void cycleTerminalColor(bool reset = false, float step = 100.0, bool flush = true, Stream *stream = &Jerial, int startColorIndex = 0, int bright = 1);

extern "C" {
extern void cycleTermColor(bool reset = false, float step = 100.0, bool flush = true);
}

// Printf-like function for menu items with automatic color cycling
int printMenuLine(const char* format, ...);

// Printf-like function for menu items with automatic color cycling and conditional display
int printMenuLine(int showExtraMenu, int minLevel, const char* format, ...);

void changeTerminalColorHighSat(int colorIndex = -1, bool flush = true, Stream *stream = &Jerial, int bright = 0);

void drawImage(int imageIndex = 0);
void drawAnimatedImage(int imageIndex = 0, int speed = 2000);
void printRLEimageData(int imageIndex);
void printAllRLEimageData(void);

// Draw current LED state to a terminal stream.  Normally the function
// saves the cursor, clears and/or repositions to a fixed area and then
// renders a 30×14 grid of coloured blocks.  However if *both* `posX` and
// `posY` are negative the "inline mode" is enabled: the screen is *not*
// cleared, the cursor is left where it is, and the ANSI sequences are
// simply printed consecutively.  This is useful when you want to log an LED
// snapshot inline with other text.
//
// The parameter values are mostly ignored in inline mode; they exist for
// backwards compatibility with other dump routines.
void dumpLEDs( int posX = 50, int posY = 27, int pixelsOrRows = 0,
               int header = 0, int rgbOrRaw = 0, int logo = 0,
               Stream* stream = &Jerial );

// Free dumpLEDs screen buffer (call when LED dumping is disabled to save 34KB)
void freeDumpLEDsBuffer( );

// LED dump scrolling region control (similar to live crossbar display)
void setLedDumpEnabled( bool enabled, Stream* target = &Jerial );
extern bool ledDumpEnabled;

// Clear any non-scrolling region from previous session (call at startup)
void clearNonScrollingRegion(void);
void dumpHeader(int posX = 50, int posY = 20, int absolute = 1, int wide = 0,
                Stream *stream = &Jerial);
void dumpHeaderHex(Stream *stream = &Jerial);
void moveCursor(int posX = -1, int posY = -1, int absolute = 1,
                Stream *stream = &Jerial, bool flush = false);
void saveCursorPosition(Stream *stream = &Jerial);
void restoreCursorPosition(Stream *stream = &Jerial);
int getCursorPositionX(void);
int getCursorPositionY(void);

// Alternate screen buffer functions for saving/restoring entire screen state
void saveScreenState(void);
void restoreScreenState(void);

void dumpHeaderMain(int posX = 50, int posY = 20, int absolute = 1,
                    int wide = 0);
void dumpLEDsMain(int posX = 50, int posY = 27, int pixelsOrRows = 0,
                  int header = 0, int rgbOrRaw = 0, int logo = 0);

// Helper that maps a defined node (e.g., TOP_1..BOTTOM_30) to the 0-based row
// index expected by bread.printRawRow(). Returns -1 if the node is not a
// breadboard row.
int nodeToPrintRow(int node);

#endif
