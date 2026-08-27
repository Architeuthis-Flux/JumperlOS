// SPDX-License-Identifier: MIT
#ifndef LEDS_H
#define LEDS_H

//#include "Adafruit_NeoMatrix.h"
#include "JeoPixel.h"
#include "JumperlessDefines.h"
#include "NetsToChipConnections.h"
#include "RotaryEncoder.h"
#include <Arduino.h>
#include "config.h"
//#include <FastLED.h>

#if defined(OG_JUMPERLESS)
// OG Jumperless: a single WS2812 chain of 111 LEDs on GPIO 25 (matches the
// stock OG firmware: Adafruit_NeoPixel leds(111, 25)). 1 LED per breadboard row.
#define LED_PIN 25
#define LED_PIN_TOP 25   // unused on OG (single strip), kept defined
#define PROBE_LED_PIN 2
#else
#define LED_PIN 17
#define LED_PIN_TOP 3
#define PROBE_LED_PIN 2
#endif


  #define RST_0_LED 402
  #define RST_1_LED 427

  #define VIN_LED 429
  #define GND_B_LED 428
  #define GND_T_LED 403

  #define V3V3_LED 416
  #define V5V_LED 426


  #define ADC_LED_0 430
  #define ADC_LED_1 431
  #define DAC_LED_0 432
  #define DAC_LED_1 433
  #define GPIO_LED_0 434
  #define GPIO_LED_1 435

  #define LOGO_LED_START 436
  #define LOGO_LED_END 445

#if defined(OG_JUMPERLESS)
// Keep the same size LED buffer even if we're only showing the middle row
#define LED_COUNT 300
#define LED_COUNT_TOP 145
#else
#define LED_COUNT 300
#define LED_COUNT_TOP 145
#endif

// LEDs per breadboard row: V5 has 5 (wire-drawing, per-column effects); OG has 1.
#if defined(OG_JUMPERLESS)
#define LEDS_PER_ROW 1
#else
#define LEDS_PER_ROW 5
#endif
extern bool splitLEDs;

#define DEFAULTMENUBRIGHTNESS 100

#define DEFAULTBRIGHTNESS 50
#define DEFAULTRAILBRIGHTNESS 55
#define DEFAULTSPECIALNETBRIGHTNESS 60

//extern volatile bool core2busy;
// #define PCBEXTINCTION 0 //extra brightness for to offset the extinction
// through pcb
//this stuff is unused
#define PCBEXTINCTION                                                          \
  30 // extra brightness for to offset the extinction through pcb
#define PCBREDSHIFTPINK                                                        \
  -18 // extra hue shift to offset the hue shift through pcb
#define PCBGREENSHIFTPINK -25
#define PCBBLUESHIFTPINK 35

#define PCBREDSHIFTBLUE                                                        \
  -25 // extra hue shift to offset the hue shift through pcb
#define PCBGREENSHIFTBLUE -25
#define PCBBLUESHIFTBLUE 42

extern volatile uint8_t LEDbrightnessRail;
extern volatile uint8_t LEDbrightness;
extern volatile uint8_t LEDbrightnessSpecial;

  extern JeoPixel bbleds;
extern JeoPixel probeLEDs;
extern uint8_t probeLEDstateMachine;

extern volatile uint32_t logoColorOverride;

extern volatile uint32_t logoColorOverrideTop;
extern volatile uint32_t logoColorOverrideBottom;


extern volatile uint32_t ADCcolorOverride0;
extern volatile uint32_t ADCcolorOverride1;
extern volatile uint32_t DACcolorOverride0;
extern volatile uint32_t DACcolorOverride1;
extern volatile uint32_t GPIOcolorOverride0;
extern volatile uint32_t GPIOcolorOverride1;

extern  uint32_t RST0colorOverride;
extern  uint32_t RST1colorOverride;
extern  uint32_t GNDTcolorOverride;
extern  uint32_t GNDBcolorOverride;
extern  uint32_t VINcolorOverride;
extern  uint32_t V3V3colorOverride;
extern  uint32_t V5VcolorOverride;




typedef enum {
  ADC_0,
  ADC_1,
  DAC_0,
  DAC_1,
  GPIO_0,
  GPIO_1,
  LOGO_TOP,
  LOGO_BOTTOM,
  LOGO,

} logoOverrideNames;


void setLogoOverride(logoOverrideNames ledNumber, uint32_t colorOverride);
uint32_t getLogoOverride(logoOverrideNames ledNumber);
uint32_t getLogoOverrideUnlocked(logoOverrideNames ledNumber);
// extern logoOverrideNames overrideNames;

struct logoColorOverrideMap {
  int ledNumber;
  uint32_t colorOverride;
  uint32_t defaultOverride;
  
};

extern struct logoColorOverrideMap logoOverrideMap[15];

extern volatile bool logoOverriden;

void clearColorOverrides(bool logo = true, bool pads = true, bool header = true);


// ============================================================================
// LogoRing — reusable ring indicator (6 ring LEDs + 1 center)
// ----------------------------------------------------------------------------
// Logical ring slots 0..5 start at top-right and advance clockwise; the
// physical LEDs are wired counter-clockwise from LOGO_LED_START, so the
// renderer flips the mapping (slot 0 = LED +0, slot 1 = LED +5, …) to make
// the indicator orbit with the encoder. The center LED is LOGO_LED_START+6.
//
// Items are laid out at true evenly spaced angles: item k of N sits at
// k/N-th of the circle, in both hue (palette position, shifted by hueOffset)
// and angle (slot position k*6/N). When an item's angle falls between two
// LEDs, both light up with brightness split by proximity.
//
// Driven from any context (menus, voltage selectors, …): the caller sets the
// state fields and renderLogoRing() (called by logoSwirl) paints it. State
// updates follow the existing volatile core1->core2 pattern.
// ============================================================================
enum LogoRingColorMode {
  RING_RAINBOW_BY_POSITION = 0, // sample PALETTE_RAINBOW evenly across the ring
  RING_PALETTE             = 1, // sample a caller-supplied palette instead
};

struct LogoRing {
  volatile bool    enabled            = false; // master gate: ring owns the logo
  volatile int     itemCount          = 0;     // number of selectable items
  volatile int     selectedIndex      = 0;     // currently highlighted item
  volatile int     hueOffset          = 0;     // granular palette shift (0..LOGO_COLOR_LENGTH-1)
  volatile uint8_t baseBrightness     = 30;    // available-but-unselected slots
  volatile uint8_t selectedBrightness = 250;   // highlighted slot
  volatile uint8_t centerBrightness   = 254;   // center press-indicator peak brightness
  volatile int     colorMode          = RING_RAINBOW_BY_POSITION;
  const uint32_t*  palette            = nullptr; // used when colorMode == RING_PALETTE

  // Hold-to-back stepping (set by the menu): while > 0 the ring keeps owning
  // the logo through HELD and the center "V" indicator replays once per
  // back-step, phase-anchored at holdStepStartMs. 0 = not stepping — HELD
  // hands the logo to the hold/reboot sweep as usual.
  volatile unsigned long holdStepStartMs  = 0;
  volatile unsigned long holdStepLengthMs = 0;
};

extern LogoRing logoRing;

// Convenience: update the three navigation fields in one call.
void setLogoRing(int itemCount, int selectedIndex, int hueOffset);

// Paint the ring + center button indicator. Returns true if the ring fully
// owned the logo this frame (caller should stop drawing the logo), false to
// hand off (e.g. while the encoder hold/reboot sweep is running).
bool renderLogoRing(void);

// Per-hue-range tweak applied to every ring-palette color (ring LEDs, menu
// text, depth pads). Edit the ringHueTweaks table in LEDs.cpp to shift the
// hue or scale saturation/value of an inclusive HSV hue range. Hues live on
// the mod-256 wheel (0..255, 0 = red); a range with hueStart > hueEnd wraps
// around the red boundary (e.g. 240..10 covers 240..255 and 0..10).
struct RingHueTweak {
  uint8_t hueStart;  // inclusive range start (HSV hue 0..255)
  uint8_t hueEnd;    // inclusive range end (may be < hueStart to wrap)
  int     hueShift;  // added to hue (wraps mod 256 around the wheel)
  int     satPercent; // 100 = unchanged
  int     valPercent; // 100 = unchanged
};

// Run a packed RGB color through the tweak table; returns it unchanged when
// no range matches (or the color is off).
uint32_t applyRingHueTweaks(uint32_t color);


class ledClass { //I'm literally copying this from Adafruit_NeoPixel.h so I can split leds.show() into 2 strips without modifying the library 
  public:
  void begin(void);
  void show(void);
  void showBBBlocking(void); // Force synchronous LED update - waits for DMA to complete
  void setPin(int16_t p);
  void setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b);
  void setPixelColor(uint16_t n, uint32_t c);
  void setPixelColor(uint16_t n, uint32_t c, int blendType);
  uint32_t getPixelColor(uint16_t n);
  uint16_t numPixels(void);
  void fill(uint32_t c = 0, uint16_t first = 0, uint16_t count = 0);
  void setBrightness(uint8_t = 254);
  void clear(void);
  void end(void);

  // Dirty flag optimization: only refresh strips that changed
  void markBBDirty(void);
  void markTopDirty(void);
  void markAllDirty(void);
  bool isBBDirty(void);
  bool isTopDirty(void);
  
  // Direct buffer access (no dirty marking) - for clear functions
  void setPixelColorDirect(uint16_t n, uint32_t c);
  void setPixelColorDirect(uint16_t n, uint8_t r, uint8_t g, uint8_t b);

  //void clear(int start = 0, int end = LED_COUNT+LED_COUNT_TOP);

private:
  volatile bool bbDirty = true;   // Breadboard LEDs need refresh
  volatile bool topDirty = true;  // Top LEDs need refresh
};
  

  typedef struct rgbColor {
  unsigned char r;
  unsigned char g;
  unsigned char b;
} rgbColor;

typedef struct hsvColor {
  unsigned char h;
  unsigned char s;
  unsigned char v;
} hsvColor;

extern uint32_t headerColors[7];
extern uint32_t rstColors[2];
extern ledClass leds;

//extern CRGB probeLEDs[1];
// extern Adafruit_NeoMatrix matrix;
//extern bool debugLEDs;
// extern int highlightedNet;
// extern int highlightedRow;
// extern rgbColor highlightedOriginalColor;
// extern int probeConnectHighlight;

// extern int warningRow;
// extern int warningNet;
// extern rgbColor warningOriginalColor;

// extern int brightenedNode;
// extern int brightenedNet;
// extern int brightenedRail;
// extern rgbColor brightenedOriginalColor;
// extern int brightenedAmount;
extern bool lightUpName;

extern int netColorMode; // 0 = rainbow, 1 = shuffle
//extern int displayMode;
extern int numberOfShownNets;
//extern int showLEDsCore2;
extern int logoFlash;



const uint8_t jumperlessText[301] = {
    1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0,
    1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1,
    0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1,
    1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0,

    1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1,
    0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0,
    1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0,
    0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1,
    1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1,

    1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1,

    1, 0, 0, 0, 1, 1,
};

const int textMap[301] = {
    0,   30,  60,  90,  120, 1,   31,  61,  91,  121, 2,   32,  62,  92,  122,
    3,   33,  63,  93,  123, 4,   34,  64,  94,  124, 5,   35,  65,  95,  125,
    6,   36,  66,  96,  126, 7,   37,  67,  97,  127, 8,   38,  68,  98,  128,
    9,   39,  69,  99,  129, 10,  40,  70,  100, 130, 11,  41,  71,  101, 131,
    12,  42,  72,  102, 132, 13,  43,  73,  103, 133, 14,  44,  74,  104, 134,
    15,  45,  75,  105, 135, 16,  46,  76,  106, 136, 17,  47,  77,  107, 137,
    18,  48,  78,  108, 138, 19,  49,  79,  109, 139, 20,  50,  80,  110, 140,
    21,  51,  81,  111, 141, 22,  52,  82,  112, 142, 23,  53,  83,  113, 143,
    24,  54,  84,  114, 144, 25,  55,  85,  115, 145, 26,  56,  86,  116, 146,
    27,  57,  87,  117, 147, 28,  58,  88,  118, 148, 29,  59,  89,  119, 149,
    150, 180, 210, 240, 270, 151, 181, 211, 241, 271, 152, 182, 212, 242, 272,
    153, 183, 213, 243, 273, 154, 184, 214, 244, 274, 155, 185, 215, 245, 275,
    156, 186, 216, 246, 276, 157, 187, 217, 247, 277, 158, 188, 218, 248, 278,
    159, 189, 219, 249, 279, 160, 190, 220, 250, 280, 161, 191, 221, 251, 281,
    162, 192, 222, 252, 282, 163, 193, 223, 253, 283, 164, 194, 224, 254, 284,
    165, 195, 225, 255, 285, 166, 196, 226, 256, 286, 167, 197, 227, 257, 287,
    168, 198, 228, 258, 288, 169, 199, 229, 259, 289, 170, 200, 230, 260, 290,
    171, 201, 231, 261, 291, 172, 202, 232, 262, 292, 173, 203, 233, 263, 293,
    174, 204, 234, 264, 294, 175, 205, 235, 265, 295, 176, 206, 236, 266, 296,
    177, 207, 237, 267, 297, 178, 208, 238, 268, 298, 179, 209, 239, 269, 299,

};

const int numbers[120] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
    75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119};


const int nodesToPixelMap[120] = {
    0,  0,  1,  2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
    14, 15, 16, 17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,
    29, 30, 31, 32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44, 45, 46, 47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59, 60, 61, 0,   0,   0,   0,   0,   0,   0,

    81, 80, 84, 85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  82,
    97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, -1,
    -1, -1, -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,

};

#define LOGO_COLOR_LENGTH 60
#if defined(OG_JUMPERLESS)
// The OG has ONE logo LED, so it only ever needs the rainbow palette (index 0).
// Sizing the table to a single row reclaims ~3.1 KB SRAM and is self-consistent:
// generateLogoPalette() already guards its writes with (paletteIndex <
// LOGO_PALETTE_COUNT) and setLogoFromPaletteIndex() folds any index via
// (paletteIndex % LOGO_PALETTE_COUNT) → 0, so the undo/fs/measure indicator
// swirls simply reuse the rainbow. The lone direct non-rainbow write
// (PALETTE_8V_SELECT, in setupSwirlColors) is OG-guarded there.
#define LOGO_PALETTE_COUNT 1
#else
#define LOGO_PALETTE_COUNT 12  // Total number of logo color palettes
#endif

// ============================================================================
// LOGO COLOR PALETTE INDICES - Use these to select palettes by name
// ============================================================================
enum LogoPalette {
  PALETTE_RAINBOW    = 0,   // Full spectrum rainbow
  PALETTE_COLD       = 1,   // Cyan/Blue tones
  PALETTE_HOT        = 2,   // Red/Orange tones
  PALETTE_PINK       = 3,   // Pink/Magenta tones
  PALETTE_YELLOW     = 4,   // Yellow tones
  PALETTE_GREEN      = 5,   // Green tones
  PALETTE_8V_SELECT  = 6,   // Special 8V selection gradient
  PALETTE_ORANGE     = 7,   // Orange tones
  PALETTE_TURQUOISE  = 8,   // Turquoise/Teal tones
  PALETTE_CHARTREUSE = 9,   // Chartreuse (yellow-green) tones
  PALETTE_PURPLE     = 10,  // Purple/Violet tones
  PALETTE_WHITE      = 11,  // White/neutral tones
};

// ============================================================================
// HSV BASE HUES FOR EACH PALETTE (0-255 scale)
// Adjust these values to tweak the color of each palette
// ============================================================================
#define HUE_COLD        130   // Cyan
#define HUE_HOT         230   // Red-orange
#define HUE_PINK        155   // Magenta-pink
#define HUE_YELLOW      28    // Distinct gold-yellow (away from chartreuse 70 / green 85)
#define HUE_GREEN       85    // Green
#define HUE_ORANGE      20    // Orange
#define HUE_TURQUOISE   125   // Turquoise/teal
#define HUE_CHARTREUSE  70    // Yellow-green
#define HUE_PURPLE      190   // Purple/violet

// The named single-hue palettes are file-local to LEDs.cpp now: they either
// alias rows of logoColorsAll (V5) or exist only on the OG (cold/hot/pink).
// Everything else reads logoColorsAll[LogoPalette] directly.
extern uint32_t logoColors8vSelect[LOGO_COLOR_LENGTH+11];
extern uint32_t logoColorsAll[LOGO_PALETTE_COUNT][LOGO_COLOR_LENGTH + 11];

// Helper to generate a single-hue palette at runtime
void generateLogoPalette(uint32_t* dest, uint8_t baseHue, int paletteIndex, float step = 3.0f);


const int bbPixelToNodesMap[120] = {
    0,         1,          2,        3,        4,          5,         6,
    7,         8,          9,        10,       11,         12,        13,
    14,        15,         16,       17,       18,         19,        20,
    21,        22,         23,       24,       25,         26,        27,
    28,        29,         30,       32,       33,         34,        35,
    36,        37,         38,       39,       40,         41,        42,
    43,        44,         45,       46,       47,         48,        49,
    50,        51,         52,       53,       54,         55,        56,
    57,        58,         59,       60,       61,         0,         0,
    0,         0,          0,        0,        0,          0,         0,
    0,         0,          0,        0,        0,          0,         0,
    0,         0,          NANO_D1,  NANO_D0,  NANO_RESET_1, GND,       NANO_D2,
    NANO_D3,   NANO_D4,    NANO_D5,  NANO_D6,  NANO_D7,    NANO_D8,   NANO_D9,
    NANO_D10,  NANO_D11,   NANO_D12, NANO_D13, SUPPLY_3V3, NANO_AREF, NANO_A0,
    NANO_A1,   NANO_A2,    NANO_A3,  NANO_A4,  NANO_A5,    NANO_A6,   NANO_A7,
    SUPPLY_5V, NANO_RESET_0, GND,      NANO_VIN, 

};

struct headerStruct {
  int node;
  int pixel;
  const char* name;
};

extern struct headerStruct headerMap[30];

extern int headerMapPrintOrder[30];

const int bbPixelToNodesMapV5[35][2] = {
{NANO_D1, 400}, {NANO_D0, 401}, {NANO_RESET_1, 402}, {NANO_GND_1, 403}, {NANO_D2, 404}, {NANO_D3, 405},
{NANO_D4, 406}, {NANO_D5, 407}, {NANO_D6, 408}, {NANO_D7, 409}, {NANO_D8, 410}, 
{NANO_D9, 411}, {NANO_D10, 412}, {NANO_D11, 413}, {NANO_D12, 414}, {NANO_D13, 415},
{NANO_3V3, 416}, {NANO_AREF, 417}, {NANO_A0, 418}, {NANO_A1, 419}, {NANO_A2, 420},
{NANO_A3, 421}, {NANO_A4, 422}, {NANO_A5, 423}, {NANO_A6, 424}, {NANO_A7, 425}, {NANO_5V, 426},
{NANO_RESET_0, 427}, {NANO_GND_0, 428}, {NANO_VIN, 429}
};

extern uint32_t
    rawSpecialNetColors[8]; // = {0x000000, 0x001C04, 0x1C0702, 0x1C0107,
                            // 0x231111, 0x230913, 0x232323, 0x232323};

extern uint32_t rawOtherColors[15];

extern uint32_t rawRailColors[3][4];

const int railsToPixelMap[4][25] = {
    {300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312,
     313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324},
    {325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337,
     338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349},
    {350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362,
     363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374},
    {375, 376, 377, 378, 379, 380, 381, 382, 383, 384, 385, 386, 387,
     388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399}};

// int nodeColors[MAX_PATHS] = {0};



const int pixelsToRails[20] = {B_RAIL_NEG, B_RAIL_POS, B_RAIL_POS, B_RAIL_NEG,
                               B_RAIL_NEG, B_RAIL_POS, B_RAIL_POS, B_RAIL_NEG,
                               B_RAIL_NEG, B_RAIL_POS, T_RAIL_POS, T_RAIL_NEG,
                               T_RAIL_NEG, T_RAIL_POS, T_RAIL_POS, T_RAIL_NEG,
                               T_RAIL_NEG, T_RAIL_POS, T_RAIL_POS, T_RAIL_NEG};



struct changedNetColors {
    int net;
    uint32_t color;
    int node1;
    int node2 = -1;
    bool fromBridge = false;  // true if color came from Wokwi bridge, false if manually set by user
};

extern struct changedNetColors changedNetColors[MAX_NETS];


// Struct to keep color and name together
struct NamedColor {
  uint32_t color;    // Full brightness reference color
  uint32_t dimColor; // Specially calibrated color for dim matching
  const char* name;
  uint8_t hueStart;  // Start of hue range (0-255)
  uint8_t hueEnd;    // End of hue range (0-255)
  int termColor256;
  int termColor16;
  };

extern const NamedColor namedColors[20];


// #define LAYER_COUNT 11
// enum blendingMode {
//   BLEND_NORMAL,
//   BLEND_OVERRIDE,
//   BLEND_ALPHA,
//   BLEND_ADDITIVE,
//   BLEND_BRIGHTNESS,
//   BLEND_DIM_OTHERS, // dim all other layers below this one and show this one
//   BLEND_COLOR, // color the nonblack pixels in the layer with a specific color
//   BLEND_MASK, // mask the layer with on or off pixels
// };

// enum layerType {
// LAYER_DEFAULT = 0, // default layer for everything else
// LAYER_WIRES = 1, // wires (or lines if its set to draw lines)
// LAYER_RAILS = 2, // just the rails, leds 301-400
// LAYER_LOGO = 3, // the last 7 LEDs for the logo, the adc/dac/gpio pads, and the hardwired connections on the nano header (5V, 3V3, GND, RST0, RST1, VIN)/\ 
// LAYER_ANIMATIONS = 4, // animations for rails, special nets, etc.
// LAYER_READINGS = 5, // readings for gpio, adc, dac, etc.
// LAYER_BRIGHTNESS = 6, // brightness overlay user brightness setting
// LAYER_TEXT = 7, // text for menus and b.print()
// LAYER_IMAGES = 8, // images for startup screen and (future) arbitrary images
// LAYER_HIGHLIGHTING = 9, // highlighting for nodes, nets, etc.
// LAYER_CURSORS = 10, // cursors for clickwheel connections and stuff

// };


// struct ledLayer {
//   layerType layerType; // layer number
//   int level; // layer order
//   blendingMode blending; // blending mode
//   int ledCount = LED_COUNT; // number of LEDs in the layer
//   int ledStart; // starting LED index
//   int ledEnd; // ending LED index
//   uint32_t leds[ledCount]; // entire LED frame buffer

// };

// extern ledLayer layers[LAYER_COUNT];


//extern uint32_t changedNetColors[MAX_NETS];
extern rgbColor netColors[MAX_NETS];
extern uint32_t savedLEDcolors[NUM_SLOTS][LED_COUNT + 1];
extern rgbColor specialNetColors[8];

int checkChangedNetColors(int netIndex = -1);
int removeChangedNetColors(int node, int saveToFile = 0);
void clearChangedNetColors(int saveToFile = 0);
void findChangedNetColors(void);
void rebuildChangedNetColorsFromBridges(void);  // Rebuild changedNetColors from bridge colors after net regeneration
void printColorName(uint32_t color);
void printColorName(int hue);
uint32_t colorPicker(uint8_t startHue = 225, uint8_t brightness=jumperlessConfig.display.led_brightness);
char* colorToName(uint32_t color, int length = -1);
char* colorToName(int hue, int length = -1);
char* colorToName(rgbColor color, int length = -1);
void dumpLEDdata(void);
int colorToVT100(uint32_t color, int colorDepth = 256);
int colorToAnsi(uint32_t color);
// Highlighting functions moved to Highlighting.h

struct rgbColor shiftHue(struct rgbColor colorToShift, int hueShift = 0,
                         int brightnessShift = 0, int saturationShift = 0,
                         int specialFunction = -1);
void initLEDs(void);
void applyHeaderColorsForPsram(void);
char LEDbrightnessMenu(void);



void previewSlotColors(int slot, bool showVoltages = true);  // NEW: Preview slot with voltages (stays in preview mode!)
void applyPreviewedSlot();  // Helper to apply the previewed slot and refresh hardware
void cancelPreview();  // Helper to cancel preview and return to original slot
void clearLEDs(void);
void randomColors(void);
void rainbowy(int, int, int wait);
void showNets(void);
int getNanoHeaderPixel(int node);  // Find LED pixel index for nano header node
void assignNetColors(int preview = 0);
void lightUpRail(int logo = -1, int railNumber = -1, int onOff = 1,
                 int brightness = -1,
                 int supplySwitchPosition = 0);
void setupSwirlColors(void);
void logoSwirl(int start = 0, int spread = 5, int probe = 0);
#if defined(OG_JUMPERLESS)
void ogStartupAnimation(void); // OG-only rainbow swirl boot animation
#endif
uint32_t dimLogoColor(uint32_t color, int brightness = 20);
void lightUpNet(int netNumber = 0, int node = -1, int onOff = 1,
                int brightness = DEFAULTBRIGHTNESS,
                int hueShift = 0, int dontClear = 0, uint32_t forceColor = 0xffffff); //-1 means all nodes (default)
void lightUpNode(int node, uint32_t color);
rgbColor pcbColorCorrect(rgbColor colorToCorrect);
hsvColor RgbToHsv(rgbColor rgb);
hsvColor RgbToHsv(uint32_t color);
rgbColor HsvToRgb(hsvColor hsv);
void applyBrightness(int brightness);
rgbColor unpackRgb(uint32_t color);

//uint32_t packRgb(uint8_t r, uint8_t g, uint8_t b); 


uint32_t scaleUpBrightness(uint32_t hexColor, int scaleFactor = 8,
                           int minBrightness = 25);
uint32_t scaleDownBrightness(uint32_t hexColor, int scaleFactor = 8,
                             int maxBrightness = 15);
void showSkippedNodes(uint32_t onColor = 0x0f1f2f, uint32_t offColor =  0x040007);
void clearLEDsExceptRails();
uint32_t HsvToRaw(hsvColor hsv);

uint32_t packRgb(rgbColor color);
uint32_t packRgb(uint8_t r, uint8_t g, uint8_t b);

void startupColors(void);
void startupColorsV5(void);
void rainbowBounce(int wait, int logo = 0);
uint32_t scaleBrightness(uint32_t color, int scaleFactor);


void clearLEDsExceptMiddle(int start = 1, int end = 60);
void clearLEDsMiddle(int start = 1, int end = 60);
#endif
