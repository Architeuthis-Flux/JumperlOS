// SPDX-License-Identifier: MIT
#include "LEDs.h"
#include "Commands.h"
#include "FileParsing.h"
#include "Graphics.h"
#include "MatrixState.h"
#include "States.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "Probing.h"
#include "Colors.h"  // Centralized color handling
//#include <Adafruit_GFX.h>
// #include <Adafruit_NeoMatrix.h>
#include <JeoPixel.h>
#include "config.h"
// #include <FastLED.h>
#include "Highlighting.h"
#include "RotaryEncoder.h"  // For buttonPressAnimActive, pressAnimLogoColors
#include "externVars.h"  // For filesystemActive indicator
// CRGB probeLEDs[1];

// bool splitLEDs;
volatile int hideNets = 0;

///-2 will set to default
volatile uint32_t logoColorOverride = -1;
///-2 will set to default
volatile uint32_t logoColorOverrideTop = -1;
///-2 will set to default
volatile uint32_t logoColorOverrideBottom = -1;

uint32_t logoColorOverrideDefault = 0x4050b0;
uint32_t logoColorOverrideTopDefault = 0x302000;
uint32_t logoColorOverrideBottomDefault = 0x002540;

///-2 will set to default
volatile uint32_t ADCcolorOverride0 = -1;
///-2 will set to default
volatile uint32_t ADCcolorOverride1 = -1;
///-2 will set to default
volatile uint32_t DACcolorOverride0 = -1;
///-2 will set to default
volatile uint32_t DACcolorOverride1 = -1;
///-2 will set to default
volatile uint32_t GPIOcolorOverride0 = -1;
///-2 will set to default
volatile uint32_t GPIOcolorOverride1 = -1;

uint32_t RST0colorOverride = -1;
uint32_t RST1colorOverride = -1;
uint32_t GNDTcolorOverride = -1;
uint32_t GNDBcolorOverride = -1;
uint32_t VINcolorOverride = -1;
uint32_t V3V3colorOverride = -1;
uint32_t V5VcolorOverride = -1;

uint32_t ADCcolorOverride0Default = 0x4050b0;
uint32_t ADCcolorOverride1Default = 0x453080;
uint32_t DACcolorOverride0Default = 0x4050b0;
uint32_t DACcolorOverride1Default = 0x458040;
uint32_t GPIOcolorOverride0Default = 0x4050b0;
uint32_t GPIOcolorOverride1Default = 0x2560a0;

// Highlighting variables moved to Highlighting.cpp

struct logoColorOverrideMap logoOverrideMap[15] = {

    {ADC_LED_0, (uint32_t)-1, ADCcolorOverride0Default},
  {ADC_LED_1, (uint32_t)-1, ADCcolorOverride1Default},
  {DAC_LED_0, (uint32_t)-1, DACcolorOverride0Default},
  {DAC_LED_1, (uint32_t)-1, DACcolorOverride1Default},
  {GPIO_LED_0, (uint32_t)-1, GPIOcolorOverride0Default},
  {GPIO_LED_1, (uint32_t)-1, GPIOcolorOverride1Default},

  {LOGO_LED_START+0, (uint32_t)-1, logoColorOverrideTopDefault},
  {LOGO_LED_START+1, (uint32_t)-1, logoColorOverrideTopDefault},
  {LOGO_LED_START+2, (uint32_t)-1, logoColorOverrideTopDefault},
  {LOGO_LED_START+3, (uint32_t)-1, logoColorOverrideBottomDefault},
  {LOGO_LED_START+4, (uint32_t)-1, logoColorOverrideBottomDefault},
  {LOGO_LED_START+5, (uint32_t)-1, logoColorOverrideBottomDefault},
  {LOGO_LED_START+6, (uint32_t)-1, logoColorOverrideDefault},
  

  


};

volatile bool logoOverriden = false;


// CRITICAL: Initialize strips with correct sizes from the start
// Rev 3 and below: Single strip with all LEDs
// Rev 4+: Split into breadboard (LED_COUNT) and top (LED_COUNT_TOP)
bool splitLEDs = 1;
JeoPixel bbleds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);  // Start with correct size!
JeoPixel topleds(LED_COUNT_TOP, LED_PIN_TOP, NEO_GRB + NEO_KHZ800);

// #include "RamMacros.h"

uint8_t *ram_bb_pixels = NULL;
uint8_t *ram_top_pixels = NULL;

JeoPixel probeLEDs(1, PROBE_LED_PIN, NEO_GRB + NEO_KHZ800);
// Adafruit_NeoPixel probeLEDs(1, 9, NEO_GRB + NEO_KHZ800);

void ledClass::end(void) {
  bbleds.~JeoPixel();
  topleds.~JeoPixel();

  }

void ledClass::begin(void) {


 

#if defined(OG_JUMPERLESS)
  // OG: one physical chain on GPIO 25. We size the buffer to LED_COUNT + LED_COUNT_TOP (445 pixels)
  // to prevent out-of-bounds reads/writes on special LEDs.
  splitLEDs = 0;
  bbleds.updateLength(LED_COUNT + LED_COUNT_TOP);
#else
  if (jumperlessConfig.hardware.revision <= 3) {
    // Rev 3 and below use single strip for all LEDs
    splitLEDs = 0;
    bbleds.updateLength(LED_COUNT + LED_COUNT_TOP);
    } else {
    // Rev 4+ already initialized with correct sizes
    splitLEDs = 1;
    }
#endif

  // CRITICAL: Call begin() AFTER updateLength() so DMA is set up with correct buffer size
  if (splitLEDs == 1) {
    topleds.begin();
    }
  bbleds.begin();
  bbleds.setBrightness(254);
  topleds.setBrightness(254);

  // Get pointers to pixel buffers for RAM-resident operations
  ram_bb_pixels = bbleds.getPixels();
  if (splitLEDs == 1) {
    ram_top_pixels = topleds.getPixels();
  }
  }

int topDirtyCount = 0;
int bbDirtyCount = 0; 

unsigned long lastShowTime = 0;
#define SHOW_INTERVAL 100

// FOLLOW-UP (deep refactor): the per-strip dirty flags (bbDirty/topDirty and the
// unused markBBDirty/markTopDirty/markAllDirty) are written on every setPixelColor
// but ignored here (the `if (bbDirty)` guards are commented out), so both strips are
// pushed unconditionally. Either re-enable per-strip skipping or delete the flags +
// mark* helpers outright. Also consolidate the overlapping clear mechanisms
// (clearBeforeSend / clearLEDsExceptRails / hideNets / leds.clear / bread::clear)
// into one path. See the showLEDsCore2 typed-request note in Commands.cpp.
void __not_in_flash_func(ledClass::show)(void) {
  // OPTIMIZATION: Only refresh LED strips that have actually changed
  // This can reduce 13ms LED refresh time to 0ms when idle, or ~6.5ms if only one strip changed
  
  // CRITICAL: Check if DMA is still busy from previous transfer
  // With async DMA, show() returns immediately but transfer continues in background
  // If DMA is busy, DROP THE FRAME instead of spin-waiting to avoid deadlocks
  // The double-buffering in the driver will queue the next frame automatically
  if (splitLEDs == 1 ){//&& topDirty) {
    // Only update if DMA is not busy - otherwise drop this frame
    if (!topleds.isDMABusy() && !bbleds.isDMABusy()) {
      topleds.show();
      topDirty = false;
    }
    // If busy, leave topDirty=true so we try again next time
  }
  
  //if (bbDirty) {
    // Only update if DMA is not busy - otherwise drop this frame
    if (!bbleds.isDMABusy()) {
      bbleds.show();
      bbDirty = false;
    }
    // If busy, leave bbDirty=true so we try again next time
  //}
}

void __not_in_flash_func(ledClass::showBBBlocking)(void) {
  // CRITICAL FIX FOR PROBE MENU DEADLOCK:
  // Force synchronous LED update by bypassing DMA and using blocking PIO transfers
  // This MUST be called before entering blocking probe menu loops
  // otherwise the menu won't display and the code will spin forever
  
  // SIMPLIFIED: Bypass DMA entirely and use reliable blocking PIO method
  // This is the same method used when DMA is unavailable - guaranteed to work
  
  if (splitLEDs == 1 ){//&& topDirty) {
    topleds.showBlocking();  // Force blocking PIO transfer
    topDirty = false;
    topDirtyCount = 0;
  //} else {
    //topDirtyCount++;
    //if (topDirtyCount > 100) {
    //  Serial.print("topDirtyCount :");
    //  Serial.println(topDirtyCount);
    //  Serial.flush();
    //  // topDirty = false;
    //  // topDirtyCount = 0;
    //}
  }
  
  //if (bbDirty) {
    bbleds.showBlocking();  // Force blocking PIO transfer
    bbDirty = false;
    bbDirtyCount = 0;
  //} else {
    //bbDirtyCount++;
    //if (bbDirtyCount > 100) {
    //  Serial.print("bbDirtyCount :");
    //  Serial.println(bbDirtyCount);
    //  Serial.flush();
    //  // bbDirty = false;
    //  // bbDirtyCount = 0;
    //}
  //}
}

// Dirty flag management
void ledClass::markBBDirty(void) {
  bbDirty = true;
}

void ledClass::markTopDirty(void) {
  topDirty = true;
}

void ledClass::markAllDirty(void) {
  bbDirty = true;
  topDirty = true;
}

bool ledClass::isBBDirty(void) {
  return bbDirty;
}

bool ledClass::isTopDirty(void) {
  return topDirty;
}

// RAM-resident helper to set pixel color directly in buffer
// Avoids calling flash-resident JeoPixel::setPixelColor
// ASSUMES NEO_GRB format!
inline void __not_in_flash_func(setPixelColorRamHelper)(uint8_t* pixels, uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
  if (!pixels) return;
  uint8_t *p = &pixels[n * 3];
  p[0] = g; // Green
  p[1] = r; // Red
  p[2] = b; // Blue
}

// One-past-the-last valid pixel index for the active strip configuration.
// The RAM-resident fast path (setPixelColorRamHelper) writes the raw NeoPixel
// buffer with no bounds check, so an out-of-range index walks straight into the
// heap. This matters on the OG, whose 111-pixel buffer is far smaller than the
// V5 image/animation data that shared graphics paths emit (a frame indexing up
// to ~445 was overwriting heap singletons and hard-faulting on boot).
static inline uint16_t __not_in_flash_func(ledMaxPixels)() {
  return (splitLEDs == 1) ? (uint16_t)(LED_COUNT + topleds.numPixels())
                          : bbleds.numPixels();
}

void __not_in_flash_func(ledClass::setPixelColor)(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
  if (n >= ledMaxPixels()) return;
  if (n >= LED_COUNT && splitLEDs == 1) {
    // topleds.setPixelColor(n - LED_COUNT, r, g, b);
    if (ram_top_pixels) {
        setPixelColorRamHelper(ram_top_pixels, n - LED_COUNT, r, g, b);
    } else {
        // Fallback if pointers not set (shouldn't happen after begin)
        topleds.setPixelColor(n - LED_COUNT, r, g, b);
    }
    topDirty = true; // Mark top strip as needing refresh
    } else {
    // bbleds.setPixelColor(n, r, g, b);
    if (ram_bb_pixels) {
        setPixelColorRamHelper(ram_bb_pixels, n, r, g, b);
    } else {
        bbleds.setPixelColor(n, r, g, b);
    }
    bbDirty = true; // Mark breadboard strip as needing refresh
    }
  }

void __not_in_flash_func(ledClass::setPixelColor)(uint16_t n, uint32_t c) {
  if (n >= ledMaxPixels()) return;
  uint8_t r = (uint8_t)(c >> 16);
  uint8_t g = (uint8_t)(c >> 8);
  uint8_t b = (uint8_t)c;

  if (n >= LED_COUNT && splitLEDs == 1) {

    // topleds.setPixelColor(n - LED_COUNT, c);
    if (ram_top_pixels) {
        setPixelColorRamHelper(ram_top_pixels, n - LED_COUNT, r, g, b);
    } else {
        topleds.setPixelColor(n - LED_COUNT, c);
    }
    topDirty = true; // Mark top strip as needing refresh

    } else {
    // bbleds.setPixelColor(n, c);
    if (ram_bb_pixels) {
        setPixelColorRamHelper(ram_bb_pixels, n, r, g, b);
    } else {
        bbleds.setPixelColor(n, c);
    }
    bbDirty = true; // Mark breadboard strip as needing refresh
    }
  }

// Direct buffer access - NO dirty marking
// Used by clear functions to avoid triggering premature LED updates
void __not_in_flash_func(ledClass::setPixelColorDirect)(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
  if (n >= ledMaxPixels()) return;
  if (n >= LED_COUNT && splitLEDs == 1) {
    if (ram_top_pixels) {
        setPixelColorRamHelper(ram_top_pixels, n - LED_COUNT, r, g, b);
    } else {
        topleds.setPixelColor(n - LED_COUNT, r, g, b);
    }
    // NO dirty flag set - buffer updated but not marked for refresh
  } else {
    if (ram_bb_pixels) {
        setPixelColorRamHelper(ram_bb_pixels, n, r, g, b);
    } else {
        bbleds.setPixelColor(n, r, g, b);
    }
    // NO dirty flag set - buffer updated but not marked for refresh
  }
}

void __not_in_flash_func(ledClass::setPixelColorDirect)(uint16_t n, uint32_t c) {
  uint8_t r = (uint8_t)(c >> 16);
  uint8_t g = (uint8_t)(c >> 8);
  uint8_t b = (uint8_t)c;
  
  setPixelColorDirect(n, r, g, b); // Call RGB version above
}

// blendType: 0 = no blend, 1 = only if brighter than current color, 2 = blend with current color
// Note: This uses flash functions (RgbToHsv, blendColors) so it might still stall if blendType != 0
// But most calls are blendType 0
void __not_in_flash_func(ledClass::setPixelColor)(uint16_t n, uint32_t c, int blendType) {
  if (blendType == 0) {
    // bbleds.setPixelColor(n, c);
    setPixelColor(n, c); // Calls RAM-resident overload above
  } else if (blendType == 1) {
    hsvColor currentColor = RgbToHsv(leds.getPixelColor(n));
    hsvColor newColor = RgbToHsv(c);
    if (currentColor.v < newColor.v) {
      // bbleds.setPixelColor(n, c);
      setPixelColor(n, c);
    }
  } else if (blendType == 2) {
    uint32_t currentColor = leds.getPixelColor(n);
    uint32_t newColor = c;
    uint32_t blendedColor = blendColors(currentColor, newColor, 0.5);
    // bbleds.setPixelColor(n, blendedColor);
    setPixelColor(n, blendedColor);
  }
}
void __not_in_flash_func(ledClass::fill)(uint32_t c, uint16_t first, uint16_t count) {
  // Fallback to simple iteration using our RAM-resident setPixelColor
  uint16_t end = first + count;
  for (uint16_t i = first; i < end; i++) {
    setPixelColor(i, c);
  }
  }

void ledClass::setBrightness(uint8_t b) {
  if (splitLEDs == 1) {
    topleds.setBrightness(b);
    topDirty = true; // Brightness change requires refresh
    }
  bbleds.setBrightness(b);
  bbDirty = true; // Brightness change requires refresh
  }

void __not_in_flash_func(ledClass::clear)(void) {
  if (splitLEDs == 1) {
    if (ram_top_pixels) {
        memset(ram_top_pixels, 0, LED_COUNT_TOP * 3);
    } else {
        topleds.clear();
    }
    topDirty = true; // Mark top strip as needing refresh
    }
  
  // Clear exactly the buffer that was allocated. On the OG the single strip is
  // LED_COUNT pixels (not LED_COUNT + LED_COUNT_TOP like V5 rev<=3), so deriving
  // the count from the strip itself avoids running past the buffer.
  int bb_count = bbleds.numPixels();
  if (ram_bb_pixels) {
      memset(ram_bb_pixels, 0, bb_count * 3);
  } else {
      bbleds.clear();
  }
  bbDirty = true; // Mark breadboard strip as needing refresh
  }

uint32_t __not_in_flash_func(ledClass::getPixelColor)(uint16_t n) {
  if (n >= LED_COUNT && splitLEDs == 1) {
    if (ram_top_pixels) {
        uint8_t *p = &ram_top_pixels[(n - LED_COUNT) * 3];
        // GRB
        return ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 8) | (uint32_t)p[2];
    }
    return topleds.getPixelColor(n - LED_COUNT);
    } else {
    if (ram_bb_pixels) {
        uint8_t *p = &ram_bb_pixels[n * 3];
        return ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 8) | (uint32_t)p[2];
    }
    return bbleds.getPixelColor(n);
    }
  }

uint16_t ledClass::numPixels(void) {
  if (splitLEDs == 1) {
    return topleds.numPixels();
    }
  return bbleds.numPixels();
  }

ledClass leds;



struct changedNetColors changedNetColors[MAX_NETS];
rgbColor netColors[MAX_NETS] = { 0 };

uint8_t saturation = 254;
volatile uint8_t LEDbrightness = DEFAULTBRIGHTNESS;
volatile uint8_t LEDbrightnessRail = DEFAULTRAILBRIGHTNESS;
volatile uint8_t LEDbrightnessSpecial = DEFAULTSPECIALNETBRIGHTNESS;

int netNumberC2 = 0;
int onOffC2 = 0;
int nodeC2 = 0;
int brightnessC2 = 0;
int hueShiftC2 = 0;
int lightUpNetCore2 = 0;

// More highlighting variables moved to Highlighting.cpp

int logoFlash = 0;
int numberOfShownNets = 0;
#ifdef EEPROMSTUFF
#include <EEPROM.h>
bool debugLEDs = 0; //= EEPROM.read(DEBUG_LEDSADDRESS);

#else
bool debugLEDs = 1;
#endif

int netColorMode = jumperlessConfig.display.net_color_mode;

uint32_t rawSpecialNetColors[8] = // dim
  { 0x000000, 0x001C04, 0x1C0702, 0x1C0107,
   0x231111, 0x230913, 0x232323, 0x232323 };

uint32_t rawOtherColors[15] = { 
    0x010006, // headerglow
    0x6000A8, // logo / status
    0x0055AA, // logoflash / statusflash
    0x301A02, // +8V
    0x120932, // -8V
    0x443434, // UART TX
    0x324244, // UART RX
    0x232323,

    0x380303, // ADC inner
    0x166800, // DAC inner
    0x0005E5, // GPIO inner

    0x400048, // ADC outer
    0x453800, // DAC outer
    0x00a045, // GPIO outer

  };


// Default colors for the non-PSRAM header layout (top = ADC, bottom = GPIO)
static const uint32_t rawOtherColorsDefault[15] = {
    0x010006, 0x6000A8, 0x0055AA, 0x301A02, 0x120932, 0x443434, 0x324244, 0x232323,
    0x380303, 0x166800, 0x0005E5,  // ADC inner, DAC inner, GPIO inner
    0x400048, 0x453800, 0x00a045,  // ADC outer, DAC outer, GPIO outer
};

// PSRAM layout swaps ADC ↔ GPIO (top slot lost to PSRAM CS, bottom stays)
static const uint32_t rawOtherColorsPSRAM[15] = {
    0x010006, 0x6000A8, 0x0055AA, 0x301A02, 0x120932, 0x443434, 0x324244, 0x232323,
    0x0005E5, 0x166800, 0x380303,  // ADC inner, DAC inner, GPIO inner (swapped ADC and GPIO)
    0x00a045, 0x453800, 0x400048,  // ADC outer, DAC outer, GPIO outer (swapped ADC and GPIO)
};

void applyHeaderColorsForPsram(void) {
  const uint32_t *src = jumperlessConfig.hardware.psram_installed
      ? rawOtherColorsPSRAM : rawOtherColorsDefault;
  memcpy(rawOtherColors, src, sizeof(rawOtherColors));
}

rgbColor specialNetColors[8] = { {00, 00, 00},       {0x00, 0xFF, 0x30},
                                {0xFF, 0x41, 0x14}, {0xFF, 0x10, 0x40},
                                {0xeF, 0x78, 0x7a}, {0xeF, 0x40, 0x7f},
                                {0xFF, 0xff, 0xff}, {0xff, 0xFF, 0xff} };

rgbColor railColors[4] = { {0xFF, 0x32, 0x30},
                          {0x00, 0xFF, 0x30},
                          {0xFF, 0x32, 0x30},
                          {0x00, 0xFF, 0x30} };

struct headerStruct headerMap[30] = {

{NANO_D1, 400, "D1"}, {NANO_D0, 401, "D0"}, 
{NANO_RESET_1, 402, "RST"}, {NANO_GND_1, 403, "GND"}, 
{NANO_D2, 404, "D2"}, {NANO_D3, 405, "D3"},
{NANO_D4, 406, "D4"}, {NANO_D5, 407, "D5"}, 
{NANO_D6, 408, "D6"}, {NANO_D7, 409, "D7"}, 
{NANO_D8, 410, "D8"}, {NANO_D9, 411, "D9"},
{NANO_D10, 412, "D10"}, {NANO_D11, 413, "D11"}, 
{NANO_D12, 414, "D12"}, 

{NANO_D13, 415, "D13"},
{NANO_3V3, 416, "3V3"}, {NANO_AREF, 417, "REF"}, 
{NANO_A0, 418, "A0"}, {NANO_A1, 419, "A1"}, 
{NANO_A2, 420, "A2"},{NANO_A3, 421, "A3"}, 
{NANO_A4, 422, "A4"}, {NANO_A5, 423, "A5"}, 
{NANO_A6, 424, "A6"}, {NANO_A7, 425, "A7"}, 
{NANO_5V, 426, "5V"}, {NANO_RESET_0, 427, "RST"}, 
{NANO_GND_0, 428, "GND"}, {NANO_VIN, 429, "VIN"}

};

/**
 * @brief Find LED pixel index for a nano header node
 * @param node The nano header node number (NANO_D0, NANO_A0, etc.)
 * @return LED pixel index, or -1 if not found
 */
int getNanoHeaderPixel(int node) {
    // Search through headerMap to find the pixel for this node
    for (int i = 0; i < 30; i++) {
        if (headerMap[i].node == node) {
            return headerMap[i].pixel;
        }
    }
    return -1;  // Not found
}

int headerMapPrintOrder[30] = {
  14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 
  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29
};

// int x = matrix.width();
int pass = 0;
// #define DATA_PIN 2
void initLEDs(void) {
  applyHeaderColorsForPsram();

  leds.begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
  // delay(1);
  leds.show();

  // if (splitLEDs == 1) {
  //   topleds.begin();
  //   topleds.show();
  // }

  //int claimedSms[2][4] = { {0, 0, 0, 0}, {0, 0, 0, 0} };

  // for (int i = 0; i < 4; i++) {
  //   if (pio_sm_is_claimed(pio0, i)) {
  //     claimedSms[0][i] = 1;
  //   }
  //   Serial.print("SM ");
  //   Serial.print(i);
  //   Serial.print(" is claimed: ");
  //   Serial.println(claimedSms[0][i]);
  // }


#if !defined(OG_JUMPERLESS)
  // OG has no dedicated probe LED (V5-only). Skipping avoids claiming
  // a PIO SM that the OG's oversubscribed PIO blocks can't spare.
  //
  // GPIO2 (PROBE_LED_PIN) and GPIO9 (BUTTON_PIN) land on the same TRRS plug
  // ring, and the GPIO2-side jack contact is the common flaky one - so by
  // default the WS2812 data (and the shared-SM button sampling, which follows
  // probeLEDs.getPin()) lives on GPIO9 and GPIO2 becomes the parked spare.
  // configLoaded is guaranteed by setupCore2stuff() before we get here.
  if (jumperlessConfig.hardware.probe_led_on_button_pin) {
    probeLEDs.setPin(BUTTON_PIN);
  }
  // PIO layout (T3.2, CH446Q.cpp): PIO2 is GPIOBASE 16 for the crossbar
  // chip-select strobe (+ the breadboard strip, GPIO 17), PIO1 gets the top
  // strip and the encoder (24 instructions at origin 0), and this SM - which
  // also hosts the 15-instruction probe BUTTON program (Probing.cpp) - only
  // fits next to the CH446Q shifter on PIO0 (10 + 4 + 15 = 29 of 32). Left to
  // the SDK's first-fit search it lands on PIO1 next to the top strip, the
  // encoder then fills PIO1, and the button program falls back to CPU
  // polling. So ask for PIO0; if that is refused the search runs as before
  // and X ("cs strobe" line) shows where it went.
  probeLEDs.setPreferredPIO(pio0);
  probeLEDs.begin();
  probeLEDs.setPixelColor(0, 0x000000);
  probeLEDs.show();
#endif


  // Serial.println("\n\rprobeLEDs.begin()\n\r");

  //   for (int i = 0; i < 4; i++) {
  //   if (pio_sm_is_claimed(pio0,i)) {
  //     claimedSms[1][i] = 1;
  //   }
  //   Serial.print("SM ");
  //   Serial.print(i);
  //   Serial.print(" is claimed: ");
  //   Serial.println(claimedSms[1][i]);
  // }

  // EEPROM.commit();
  // delay(20);
  }

//uint32_t savedLEDcolors[NUM_SLOTS][LED_COUNT + 1];

int slotLEDpositions[20] = {
    418, 419, 420, 421, 422, 423, 424, 425, 426,
  };
int rotaryEncoderPositions[6] = {
    97, // AREF
    95, // D13 (button)
    94, // D12 (encoder A)
    93, // D11 (encoder GND)
    92, // D10 (encoder B)
  };

uint32_t slotSelectionColors[12] = {
    0x084080, // preview
    0x005555, // active
    0x102000, // inactive
    0x000000, // off
    0x881261, // preview+active

    0x253500, // rotary encoder High
    0x000060, // rotary encoder Low

    0x550011, // button High
    0x001C05, // button Low

  };


int colorDistance(rgbColor a, rgbColor b) {
  int dr = (int)a.r - (int)b.r;
  int dg = (int)a.g - (int)b.g;
  int db = (int)a.b - (int)b.b;
  return dr * dr + dg * dg + db * db;
  }

// Color functions now in Colors.cpp - using centralized implementation


// Declare global variables for scroll acceleration to avoid scope issues
static int scrollLastDirection = 0;  // Previous scrolling direction
static float scrollAccelerationFactor = 1.0f;  // Current acceleration multiplier
static unsigned long scrollAccelerationLastTime = 0;  // Last time we updated acceleration

uint32_t colorPicker(uint8_t startHue, uint8_t brightness) {
  return HsvToRaw(hsvColor{ (uint8_t)(startHue % 255), 255, brightness });
  // Static precomputed data for all 256 possible hue values (0-255)
  clearColorOverrides(1, 1, 0);

  unsigned long holdConfirmTime = 1000;

  static bool initialized = false;
  static char allNames[256][12];
  static uint32_t allColorValues[256];
  static bool nameToggleState[256];
  static char emptyStr[2] = " ";

  if (startHue < 0) {
    startHue = 225;
    } else if (startHue > 255) {
      startHue = 225;
      }
    int rangeStart = 0;
    int rangeEnd = 255;
    // One-time initialization of all possible hue data
    if (!initialized) {
      bool toggle = false;
      char* lastName = nullptr;

      // Precompute all color names, colors, and toggle states for all 256 hues
      for (int h = 0; h < 256; h++) {
        // Get color and name
        hsvColor hsv = { (uint8_t)h, 254, brightness };
        allColorValues[h] = HsvToRaw(hsv);

        char* tempName = colorToName(allColorValues[h], 10);
        strncpy(allNames[h], tempName, 11);
        allNames[h][11] = '\0';

        // Calculate toggle state based on name changes
        if (lastName == nullptr || strcmp(allNames[h], lastName) != 0) {
          toggle = !toggle;
          }
        nameToggleState[h] = toggle;
        lastName = allNames[h];
        }

      initialized = true;
      }

    // Check if there's input in Serial to parse a range
    if (Serial.available() > 0) {
      String input = Serial.readStringUntil('\n');
      input.trim();

      // Look for a dash character
      int dashIndex = input.indexOf('-');
      if (dashIndex > 0 && dashIndex < input.length() - 1) {
        // Get the numbers before and after the dash
        String startStr = input.substring(0, dashIndex);
        String endStr = input.substring(dashIndex + 1);

        // Convert to integers
        rangeStart = startStr.toInt();
        rangeEnd = endStr.toInt();

        // Validate the range
        if (rangeStart >= 0 && rangeStart <= 255 && rangeEnd >= 0 && rangeEnd <= 255) {
          Serial.print("Displaying colors for range: ");
          Serial.print(rangeStart);
          Serial.print("-");
          Serial.println(rangeEnd);
          } else {
          Serial.println("Invalid range. Using default range (0-255)");
          rangeStart = 0;
          rangeEnd = 255;
          }
        } else {
        // No dash found or invalid format
        Serial.println("Invalid format. Using default range (0-255)");
        }
      }

    b.clear();
    // int lastNetSlot = netSlot;

    // netSlot = 8;
    // createSlots(netSlot, 1);
  //!dont show nets
    hideNets = 1;
    requestLedShow( -2 );
    resetEncoderPosition = 1;

    // Store original range for restoring when zooming out
    static int originalRangeStart = -1;
    static int originalRangeEnd = -1;
    if (originalRangeStart == -1) {
      // Always use full spectrum 0-255 as the base range for consistency
      originalRangeStart = 0;
      originalRangeEnd = 255;
      }

    // Center position and display state

    int centerRow = 45; // Start at middle row (0-59)
    int zoom = 0;       // Initial zoom level (0-255)
    bool zoomOrScroll = false;
    int cursorRow = 0;

    // If startHue is specified, find the row closest to it
    if (startHue >= 0 && startHue <= 255) {
      // Formula to convert hue to row: row = (hue * 60.0f) / 255.0f
      // This is the inverse of the formula: hue = row * 255.0f / 60.0f
      centerRow = round((startHue * 60.0f) / 255.0f);

      // Ensure centerRow stays within valid bounds (0-59)
      if (centerRow < 0) centerRow = 0;
      if (centerRow > 59) centerRow = 59;
      }

    // Use floating-point for precise positioning to avoid integer rounding errors
    float preciseHue = centerRow * 255.0f / 60.0f;
    int centerHue = (int)preciseHue; // Initial hue at center row

    float zoomCursorWidthf = 1.0f;
    int zoomCursorWidth = 1;

    // Acceleration variables - using function local variables not static to avoid linter errors
    int lastDirection = 0;  // Previous scrolling direction
    float accelerationFactor = 1.0f;  // Current acceleration multiplier
    unsigned long lastAccelerationTime = 0;  // Last time we updated acceleration
    const float maxAcceleration = 6.0f;  // Maximum acceleration factor
    const float accelerationRate = 0.0003f;  // How quickly acceleration builds up
    const unsigned long accelerationTimeout = 10;  // Reset acceleration after this many ms of no movement
    int zoomWithProbe = 0;
    Serial.println(" row\t hue\t name\t\t row\t hue\t name\n\r");



    do {
      // Calculate total range and manage zoom
      int originalTotalRange = 255; // Use the full hue range (0-255)

      // Map from row number to hue value - this is the key function
      auto getHueForRow = [&](int row) -> int {
        // Calculate zoom factor - non-linear for better control at low zoom levels
        float zoomFactor = zoom * zoom / 100.0f;

        // Convert zoom factor to visible percentage of the color range
        float visiblePercentage = 1.0f - (zoomFactor / 255.0f);
        if (visiblePercentage < 0.01f) visiblePercentage = 0.01f; // Prevent division by zero

        // Current visible range based on zoom with floating point for smoother transitions
        float zoomedRange = (float)originalTotalRange * visiblePercentage;

        // Calculate relative row position from centerRow
        // Use floating point for the entire calculation to avoid rounding errors
        float rowFraction = (float)(row - centerRow) / 60.0f; // -0.5 to 0.5 based on distance from center

        // Calculate final hue value using floating point precision
        // Use preciseHue as the anchor to ensure exact center positioning
        float hueFloat = preciseHue + (rowFraction * zoomedRange);

        // Wrap around properly in the 0-255 range
        while (hueFloat < 0) hueFloat += 255.0f;
        while (hueFloat >= 255.0f) hueFloat -= 255.0f;

        // Convert to integer without additional rounding
        int hue = (int)hueFloat;

        // Wrapping already handled in floating point, no need for additional wrapping here

        return hue;

        };

      // Get hue ranges from the first and last rows for display
      int displayRangeStart = getHueForRow(0);
      int displayRangeEnd = getHueForRow(59);

      // Render all rows using precomputed data
      for (int row = 0; row < 60; row++) {
        int hue = getHueForRow(row);

        // Get precomputed data for this hue
        uint32_t color = allColorValues[hue];
        bool toggle = nameToggleState[hue];

        // Apply alternating row pattern
        if (toggle != nameToggleState[(row - 1) % 255]) {
          b.printRawRow(0b00001110, row, color, 0xffffff);
          } else {
          b.printRawRow(0b00001110, row, color, 0xffffff);
          b.printRawRow(0b00001110, row, color, 0xffffff);
          }

        // Highlight the center row
        if (zoomOrScroll || zoomWithProbe != 0) {
          // zoomCursorWidth = (zoom + 1 / 20)%60;
          if (row == centerRow) {
            b.printRawRow(0b00011111, row, scaleBrightness(color, 0), 0xffffff);

            logoColorOverride = scaleBrightness(color, 0);

            } else if (row < (centerRow + zoomCursorWidth) && row >(centerRow - zoomCursorWidth)) {
              b.printRawRow(0b00010001, row, scaleBrightness(color, 0), 0xffffff);
              b.printRawRow(0b00001110, row, color, 0xfffffe);
              }
            // else if (row == (centerRow+ zoomCursorWidth)) {
            // b.printRawRow(0b00011111, row, scaleBrightness(color, 200), 0xffffff);
            // } else if (row == (centerRow- zoomCursorWidth)) {
            // b.printRawRow(0b00011111, row, scaleBrightness(color, 200), 0xffffff);
            // }
          } else if (zoomOrScroll == false) {
            if (row == centerRow) {
              b.printRawRow(0b00011111, row, scaleBrightness(color, 300), 0xffffff);
              logoColorOverride = scaleBrightness(color, 0);
              }
            }
        }

      // Clear the previous output
      Serial.print("\033[J");

      // Display information for all rows
      for (int i = 0; i < 30; i++) {
        // Top rows (0-29)
        int topHue = getHueForRow(i);
        int bottomHue = getHueForRow(i + 30);

        // Print row marker for center
        if (i == centerRow && centerRow < 30) {
          Serial.print("> ");
          } else {
          Serial.print("  ");
          }

        // Print top row info
        Serial.print(i + 1);
        Serial.print(":\t ");
        Serial.print(topHue);
        Serial.print("\t");

        // if (!nameToggleState[topHue]) {
        //   Serial.print(" ");
        //   }
        Serial.print(allNames[topHue]);

        // Print bottom row marker for center
        if (i + 30 == centerRow) {
          Serial.print("\t> ");
          } else {
          Serial.print("\t  ");
          }

        // Print bottom row info
        //Serial.print("\t ");
        Serial.print(i + 31);
        Serial.print(":\t  ");
        Serial.print(bottomHue);
        Serial.print("\t");

        // if (!nameToggleState[bottomHue]) {
        //   Serial.print(" ");
        //   }
        Serial.print(allNames[bottomHue]);

        Serial.println();
        }

      // Print status information
      int displayCenterHue = getHueForRow(centerRow);
      Serial.print("Center: ");
      Serial.print(centerRow + 1);
      Serial.print(" (hue: ");
      Serial.print(centerHue); // Display the anchor hue value that stays fixed
      Serial.print(")");

      if (zoom >= 0) {
        // Calculate a smooth zoom percentage for display based on non-linear curve
        // Higher zoom values produce increasingly finer granularity
        float zoomPercentage = 100.0f * (1.0f - (zoom / 255.0f));

        // // Special case for very low zoom values to make initial zoom smoother
        // if (zoom < 10) {
        //   // Slower initial decrease for finest control at beginning
        //   zoomPercentage = 100.0f - (zoom * zoom / 25.0f);
        // }

        Serial.print(" | Zoom: ");
        Serial.print(zoom);
        Serial.print(" (");
        Serial.print((int)zoomPercentage);
        Serial.print("% range: ");

        // Show more helpful range display, especially for wraparound ranges
        if (zoom < 5) {
          Serial.print("FULL SPECTRUM");
          } else if (displayRangeStart > displayRangeEnd) {
            Serial.print(displayRangeStart);
            Serial.print(" - ");
            Serial.print(displayRangeEnd);
            } else {
            Serial.print(displayRangeStart);
            Serial.print("-");
            Serial.print(displayRangeEnd);
            }
          Serial.print(")");
        }

      Serial.print(" | Mode: ");
      Serial.print(zoomOrScroll ? "ZOOM" : "SCROLL");

      Serial.println();
      Serial.println();
      Serial.flush();

      // Handle encoder input for scrolling and zooming
      int oldEncoderPosition = encoderPosition;
      bool redraw = false;
      unsigned long lastChangeTime = millis();
      unsigned long lastMicroAdjust = millis();
      const int encoderSensitivity = 3; // Base sensitivity for scroll mode

      // Use acceleration variables defined at function scope

      while (!redraw) {



        // Poll encoder
        //rotaryEncoderStuff();
        int newPosition = encoderPosition;
        zoomWithProbe = 0;
        // Check for acceleration timeout
        if (millis() - scrollAccelerationLastTime > accelerationTimeout) {
          // Reset acceleration if user hasn't scrolled for a while
          scrollAccelerationFactor = 1.0f;
          scrollLastDirection = 0;
          }

        // Check for serial input to exit
        if (Serial.available() > 0) {
          redraw = true;
          break;
          }

        // Handle button press with debounce
        static unsigned long lastButtonPress = 0;
        static bool buttonWasPressed = false;  // Track if button was previously pressed
        static unsigned long buttonPressStartTime = 0; // Track when button was initially pressed
        static int probeButtonWasPressed = 2; // Track if probe button was previously pressed
        // Get current button state (LOW is pressed)
        bool buttonIsPressed = (digitalRead(BUTTON_ENC) == LOW);

        int probeButtonIsPressed = (checkProbeButton());




        // Detect initial button press
        if ((buttonIsPressed && !buttonWasPressed) || (probeButtonIsPressed > 0 && probeButtonWasPressed == 0)) {
          // Button was just pressed (transition from released to pressed)

          if (buttonIsPressed) {
            zoomOrScroll = !zoomOrScroll;
            }
          lastButtonPress = millis();

          // if (probeButtonIsPressed == 2) {
          //   // Serial.print("probe button pressed: ");
          //   // Serial.println(probeButtonIsPressed);
          //   }

          redraw = true;
          // requestLedShow( 2 );
          // waitCore2();
          probeButtonWasPressed = probeButtonIsPressed;
          buttonWasPressed = buttonIsPressed;
          buttonPressStartTime = millis(); // Start timing the press

          if (probeButtonIsPressed > 0)
            {
            if (probeButtonIsPressed == 2) {
              zoomWithProbe = -1;
              } else {
              zoomWithProbe = 1;
              }
            // Serial.print("zoomWithProbe: ");
            // Serial.println(zoomWithProbe);
            holdConfirmTime = 800;
            } else {
            holdConfirmTime = 1500;
            }

          //lastButtonPress = millis();
          while (buttonIsPressed || probeButtonIsPressed > 0) {
            uint32_t color = allColorValues[centerHue];
            uint32_t timerColor = 0x203050;

            buttonIsPressed = (digitalRead(BUTTON_ENC) == LOW);
            probeButtonIsPressed = (checkProbeButton());
            logoColorOverride = scaleBrightness(color, 100);




            if (millis() - buttonPressStartTime > (holdConfirmTime / 6) * 5 + 30) {

              ADCcolorOverride1 = scaleBrightness(timerColor, 100);

              } else if (millis() - buttonPressStartTime > (holdConfirmTime / 6) * 5) {

                ADCcolorOverride0 = scaleBrightness(timerColor, 100);

                } else if (millis() - buttonPressStartTime > (holdConfirmTime / 6) * 3 + 30) {

                  DACcolorOverride1 = scaleBrightness(timerColor, 100);

                  } else if (millis() - buttonPressStartTime > (holdConfirmTime / 6) * 3) {

                    DACcolorOverride0 = scaleBrightness(timerColor, 100);

                    } else if (millis() - buttonPressStartTime > (holdConfirmTime / 6) * 1 + 30) {

                      GPIOcolorOverride1 = scaleBrightness(timerColor, 100);

                      } else if (millis() - buttonPressStartTime > (holdConfirmTime / 6) * 1) {

                        GPIOcolorOverride0 = scaleBrightness(timerColor, 100);
                        }


                      if (millis() - buttonPressStartTime > holdConfirmTime) {
                        hideNets = 0;
                        requestLedShow( -1 );
                        clearColorOverrides(true, true, true);
                        blockProbeButton = 5000;
                        blockProbeButtonTimer = millis();
                        // blockProbing = 1000;
                        // blockProbingTimer = millis();
                        //delay(100);
                        return HsvToRaw(hsvColor{ (uint8_t)(centerHue % 255), 255, brightness });
                        break;
                        }

            }
          clearColorOverrides(false, true, true);


          //break;

          } else {

          if (probeButtonIsPressed == 1 && probeButtonWasPressed == 0) {
            zoomOrScroll = !zoomOrScroll;
            redraw = true;
            // break;
            }
          buttonWasPressed = buttonIsPressed;
          probeButtonWasPressed = probeButtonIsPressed;

          }






        int probeReading = justReadProbe(true);

        // if (probeReading != -1){
        //   // Serial.print("probeReading: ");
        //   // Serial.println(probeReading);
        // }

        if (probeReading != -1 && probeReading > 0 && probeReading <= 60) {
          // Calculate the row difference
          int rowDiff = probeReading - 1 - centerRow;
          //if (zoomOrScroll == 1){
          // Move the centerRow to the tapped position


          if (zoom > 0 && probeReading == 1)
            {
            centerRow = 59;
            } else if (zoom > 0 && probeReading == 60) {
              centerRow = 0;
              } else {
              centerRow = probeReading - 1;
              }

            // Calculate current visible range based on zoom level
            float visiblePercentage = 1.0f - ((zoom * zoom / 100.0f) / 255.0f);
            if (visiblePercentage < 0.01f) visiblePercentage = 0.01f;
            float visibleRange = originalTotalRange * visiblePercentage;

            // Calculate how much the hue needs to shift to keep colors in place
            float hueIncrement = (visibleRange / 60.0f) * rowDiff;

            // Adjust preciseHue to maintain color positions 
            preciseHue += hueIncrement;

            // Ensure proper wrapping of hue values
            while (preciseHue < 0) preciseHue += 255.0f;
            while (preciseHue >= 255.0f) preciseHue -= 255.0f;

            // Update centerHue to match the new preciseHue
            centerHue = (int)preciseHue;
            // } else {



            redraw = true;
            // Serial.print("Probe reading: ");
            // Serial.print(probeReading);

          //}
          }


        // Handle encoder rotation
        if (newPosition != oldEncoderPosition || zoomWithProbe != 0) {
          // Adjust encoder sensitivity based on mode
          int delta;
          if (zoomOrScroll || zoomWithProbe != 0) {
            // Higher sensitivity (smaller steps) for zoom mode
            // Get raw encoder change first
            if (zoomWithProbe != 0) {
              delta = zoomWithProbe * 4;
              } else {
              delta = (newPosition - oldEncoderPosition);
              }

            if (delta > 0 && zoomWithProbe == 0) {
              zoomCursorWidthf -= 0.28f;
              } else if (delta < 0 && zoomWithProbe == 0) {
                zoomCursorWidthf += 0.28f;
                } else if (zoomWithProbe != 0 && delta > 0) {
                  zoomCursorWidthf -= 1.1f;
                  } else if (zoomWithProbe != 0 && delta < 0) {
                    zoomCursorWidthf += 1.1f;
                    }

                  if (zoomCursorWidthf > 60) zoomCursorWidthf = 60;
                  if (zoomCursorWidthf < 2) zoomCursorWidthf = 2;

                  zoomCursorWidth = (int)zoomCursorWidthf % 60;

                  // Progressive zoom rate - faster at beginning, slower at higher zoom
                 // if (zoomWithProbe == 0) {
                  if (zoom >= 100) {
                    // Very slow changes at high zoom for precise control

                    if (zoomWithProbe == 0) {

                      delta = delta / 6;
                      } else {
                      if (delta < 0) {
                        delta = delta / 2;
                        } else {
                        delta = delta * 3;
                        }
                      }

                    if (delta == 0 && (newPosition != oldEncoderPosition)) {
                      delta = (newPosition > oldEncoderPosition) ? 1 : -1;
                      }


                    } else {

                    delta = delta * (((100 - zoom) / 10) + 1);

                    }
                  // } else if (zoom > 50) {
                  //   // Medium changes in the middle-high range
                  //   delta = delta * 4;
                  //   } else if (zoom > 25){
                  //     delta = delta * 6;
                  //   } else {
                  //   // Fast changes at lower zoom levels for quick adjustments
                  //   // Amplify delta for faster initial zooming
                  //   delta = delta * 10;
                  //   // Cap maximum change to prevent huge jumps
                  //   // if (delta > 15) delta = 15;
                  //   // if (delta < -15) delta = -15;
                  //   }
               // }
            } else if (zoomWithProbe == 0) {
              // Normal sensitivity for scroll mode
              int rawDelta = (newPosition - oldEncoderPosition);

              // Determine current scrolling direction
              int currentDirection = (rawDelta > 0) ? 1 : ((rawDelta < 0) ? -1 : 0);

              // Update acceleration based on direction consistency
              if (currentDirection != 0) {
                if (currentDirection == scrollLastDirection) {
                  // Same direction - increase acceleration
                  scrollAccelerationFactor += accelerationRate;
                  if (scrollAccelerationFactor > maxAcceleration) {
                    scrollAccelerationFactor = maxAcceleration;
                    }
                  } else {
                  // Direction changed - reset acceleration
                  scrollAccelerationFactor = 1.0f;
                  }

                // Remember current direction and time

                scrollAccelerationLastTime = millis();
                }
              scrollLastDirection = currentDirection;
              // Apply base sensitivity first
              delta = rawDelta / encoderSensitivity;

              // Then apply acceleration for larger movements

                // Apply non-linear acceleration that affects larger movements more
              float acceleratedDelta = (float)delta * scrollAccelerationFactor;

              // Round to integer and ensure minimum movement of 1
              delta = (int)acceleratedDelta;
              if (delta == 0 && rawDelta != 0) {
                delta = currentDirection;
                }

              }

            if (delta != 0) {

              if (zoomOrScroll || zoomWithProbe != 0) {
                // Zoom mode - much more granular for smoother zooming
                zoom -= delta;

                // Clamp zoom value
                if (zoom < 0) zoom = 0;
                if (zoom > 255) zoom = 255;

                // In zoom mode, we keep the center hue fixed
                // preciseHue stays unchanged, ensuring the center color remains stable

                redraw = true;
                } else if (zoomWithProbe == 0) {
                  // Scroll mode - move one row at a time
                  centerRow -= delta;
                  cursorRow -= delta;

                  if (cursorRow < 0) cursorRow = 59;
                  if (cursorRow > 59) cursorRow = 0;
                  // Handle wrapping
                  if (centerRow < 0) centerRow = 59;
                  if (centerRow > 59) centerRow = 0;

                  // Update preciseHue (and subsequently centerHue) when scrolling
                  // The exact increment depends on the current zoom level

                  // Calculate current visible range based on zoom level
                  float visiblePercentage = 1.0f - ((zoom * zoom / 100.0f) / 255.0f);
                  if (visiblePercentage < 0.01f) visiblePercentage = 0.01f;
                  float visibleRange = originalTotalRange * visiblePercentage;

                  // At full zoom out (zoom near 0), one row = 255/60 hue units
                  // As we zoom in, the amount each row represents becomes smaller
                  float hueIncrement = (visibleRange / 60.0f) * delta;

                  // Update precise hue value using floating point to avoid rounding errors
                  preciseHue -= hueIncrement;

                  // Wrap around the color wheel properly using floating point
                  while (preciseHue < 0) preciseHue += 255.0f;
                  while (preciseHue >= 255.0f) preciseHue -= 255.0f;

                  // Update integer centerHue for display purposes
                  centerHue = (int)preciseHue;

                  redraw = true;
                  }

                oldEncoderPosition = newPosition;
                lastChangeTime = millis();
              }
          }

        // // Adaptive delay to prevent CPU hogging while still being responsive
        // unsigned long now = millis();
        // if (now - lastChangeTime > 200) {
        //   // Long delay when idle
        //   delay(10);
        //   } else if (now - lastChangeTime > 50) {
        //     // Medium delay when recently active
        //     delay(5);
        //     } else {
        //     // Short delay when very active
        //     delayMicroseconds(500);
        //     }
        // }
        }
      // Move cursor up to overwrite previous output
      Serial.print("\033[32A");

      } while (Serial.available() == 0);

    requestLedShow( -1 );
    hideNets = 0;
    clearColorOverrides();

    return HsvToRaw(hsvColor{ (uint8_t)(centerHue % 255), 255, brightness });
  }

// logoOverrideNames overrideNames = {
//   ADC_0,
//   ADC_1,
//   DAC_0,
//   DAC_1,
//   GPIO_0,
//   GPIO_1,
//   LOGO_TOP_0,
//   LOGO_TOP_1,
//   LOGO_TOP_2,
//   LOGO_BOTTOM_0,
//   LOGO_BOTTOM_1,
//   LOGO_BOTTOM_2,
// };

static uint32_t getLogoOverrideInternal(logoOverrideNames ledNumber) {
  switch (ledNumber) {
    case ADC_0:   return ADCcolorOverride0;
    case ADC_1:   return ADCcolorOverride1;
    case DAC_0:   return DACcolorOverride0;
    case DAC_1:   return DACcolorOverride1;
    case GPIO_0:  return GPIOcolorOverride0;
    case GPIO_1:  return GPIOcolorOverride1;
    case LOGO_TOP:    return logoColorOverrideTop;
    case LOGO_BOTTOM: return logoColorOverrideBottom;
    case LOGO:        return logoColorOverride;
    default:      return (uint32_t)-1;
  }
}

uint32_t getLogoOverride(logoOverrideNames ledNumber) {
  if (logoLedAccess == true) {
    return -1;
  }
  logoLedAccess = true;
  uint32_t color = getLogoOverrideInternal(ledNumber);
  logoLedAccess = false;
  return color;
}

uint32_t getLogoOverrideUnlocked(logoOverrideNames ledNumber) {
  return getLogoOverrideInternal(ledNumber);
}


void setLogoOverride(logoOverrideNames ledNumber, uint32_t colorOverride) {
  if  (logoLedAccess == true) {
    return;
    }
  

  if (colorOverride == -1){

  switch (ledNumber) {
    case ADC_0...GPIO_1:
      clearColorOverrides(false, true, false);
      break;
    case LOGO_TOP...LOGO:
      clearColorOverrides(true, false, false);
      break;
    default:
      break;
  }

  } else {
logoLedAccess = true;
  logoOverrideMap[ledNumber].colorOverride = colorOverride;
  switch (ledNumber) {
    case ADC_0:
    if (colorOverride == -3) {
      ADCcolorOverride0 = -1;
    } else 
      if (colorOverride == -2) {
        ADCcolorOverride0 = logoOverrideMap[ADC_0].defaultOverride;
      } else {
        ADCcolorOverride0 = colorOverride;
      }
      break;
    case ADC_1:
    if (colorOverride == -3) {
      ADCcolorOverride1 = -1;
    } else 
      if (colorOverride == -2) {
        ADCcolorOverride1 = logoOverrideMap[ADC_1].defaultOverride;
      } else {
        ADCcolorOverride1 = colorOverride;
      }
      break;
    case DAC_0:
    if (colorOverride == -3) {
      DACcolorOverride0 = -1;
    } else 
      if (colorOverride == -2) {
        DACcolorOverride0 = logoOverrideMap[DAC_0].defaultOverride;
      } else {
        DACcolorOverride0 = colorOverride;
      }
      break;
    case DAC_1:
    if (colorOverride == -3) {
      DACcolorOverride1 = -1;
    } else 
      if (colorOverride == -2) {
        DACcolorOverride1 = logoOverrideMap[DAC_1].defaultOverride;
      } else {
        DACcolorOverride1 = colorOverride;
      }
      break;
    case GPIO_0:  
    if (colorOverride == -3) {
      GPIOcolorOverride0 = -1;
    } else 
      if (colorOverride == -2) {
        GPIOcolorOverride0 = logoOverrideMap[GPIO_0].defaultOverride;
      } else {
        GPIOcolorOverride0 = colorOverride;
      }
      break;
    case GPIO_1:
    if (colorOverride == -3) {
      GPIOcolorOverride1 = -1;
    } else 
      if (colorOverride == -2) {
        GPIOcolorOverride1 = logoOverrideMap[GPIO_1].defaultOverride;
      } else {
        GPIOcolorOverride1 = colorOverride;
      }
      break;
    case LOGO_TOP:
    if (colorOverride == -3) {
      logoColorOverrideTop = -1;
    } else 
      if (colorOverride == -2) {
        logoColorOverrideTop = logoOverrideMap[LOGO_TOP].defaultOverride;
      } else {
        logoColorOverrideTop = colorOverride;
      }
      break;
    case LOGO_BOTTOM:
    if (colorOverride == -3) {
      logoColorOverrideBottom = -1;
    } else 
      if (colorOverride == -2) {
        logoColorOverrideBottom = logoOverrideMap[LOGO_BOTTOM].defaultOverride;
      } else {
          logoColorOverrideBottom = colorOverride;
      }
      break;
    case LOGO:
    if (colorOverride == -3) {
      logoColorOverride = -1;
    } else 
      if (colorOverride == -2) {
        logoColorOverride = logoOverrideMap[LOGO].defaultOverride;
      } else {
        logoColorOverride = colorOverride;
      }
      logoColorOverrideTop = colorOverride;
      logoColorOverrideBottom = colorOverride;
      break;
    default:
      break;
  }
  }

  logoOverriden = true;
  logoLedAccess = false;
  }



void clearColorOverrides(bool logo, bool pads, bool header) {

  if (logoLedAccess == true) {
    return;
    }
  logoLedAccess = true;

  if (logo) {
    logoColorOverride = -1;
    logoColorOverrideTop = -1;
    logoColorOverrideBottom = -1;
    logoOverriden = false;
    logoOverrideMap[6].colorOverride = -1;
    logoOverrideMap[7].colorOverride = -1;
    logoOverrideMap[8].colorOverride = -1;
    logoOverrideMap[9].colorOverride = -1;
    logoOverrideMap[10].colorOverride = -1;
    logoOverrideMap[11].colorOverride = -1;
    logoOverrideMap[12].colorOverride = -1;
    }
  if (pads) {
    ADCcolorOverride0 = -1;
    ADCcolorOverride1 = -1;
    DACcolorOverride0 = -1;
    DACcolorOverride1 = -1;
    GPIOcolorOverride0 = -1;
    GPIOcolorOverride1 = -1;
    logoOverriden = false;
    logoOverrideMap[0].colorOverride = -1;
    logoOverrideMap[1].colorOverride = -1;
    logoOverrideMap[2].colorOverride = -1;
    logoOverrideMap[3].colorOverride = -1;
    logoOverrideMap[4].colorOverride = -1;
    logoOverrideMap[5].colorOverride = -1;
    
    }
  if (header) {
    RST0colorOverride = -1;
    RST1colorOverride = -1;
    GNDTcolorOverride = -1;
    GNDBcolorOverride = -1;
    VINcolorOverride = -1;
    V3V3colorOverride = -1;
    V5VcolorOverride = -1;
    }
  logoLedAccess = false;
  }

void printColorName(int hue) {

  b.clear();
  for (int i = 0; i < 60; i++) {
    hue = i * 4.4;
    hsvColor hsv = { (uint8_t)hue, 254, 10 };
    rgbColor rgb = HsvToRgb(hsv);
    Serial.print(i + 1);
    Serial.print("\t");
    Serial.print(hue % 254);
    Serial.print("\t");
    char colorName[14];
    switch (hue % 255) {
      case 0 ... 11:
        sprintf(colorName, "red");
        break;
      case 12 ... 22:
        sprintf(colorName, "orange");
        break;
      case 23 ... 36:
        sprintf(colorName, "amber");
        break;
      case 37 ... 64:
        sprintf(colorName, "yellow");
        break;
      case 65 ... 79:
        sprintf(colorName, "chartreuse");
        break;
      case 80 ... 94:
        sprintf(colorName, "green");
        break;
      case 95 ... 122:
        sprintf(colorName, "aqua");
        break;
      case 123 ... 145:
        sprintf(colorName, "teal");
        break;
      case 146 ... 175:
        sprintf(colorName, "blue");
        break;
      case 176 ... 191:
        sprintf(colorName, "indigo");
        break;
      case 192 ... 210:
        sprintf(colorName, "purple");
        break;
      case 211 ... 223:
        sprintf(colorName, "violet");
        break;
      case 224 ... 235:
        sprintf(colorName, "pink");
        break;
      case 236 ... 255:
        sprintf(colorName, "magenta");
        break;
        // case 246 ... 255:
        //   sprintf(colorName, "white");
        //   break;

      default:
        sprintf(colorName, "grey");
        break;
      }
    Serial.println(colorName);
    if (i == 29) {
      Serial.println();
      }

    // Serial.print(hue/16);
    // Serial.print("\t");
    // Serial.println(colorNames[hue/16]);
    uint32_t color = packRgb(rgb.r, rgb.g, rgb.b);

    b.printRawRow(0b00011111, i, color, 0xffffff);
    }
  }



void previewSlotColors(int slot, bool showVoltages) {
  // NEW: Preview a slot using SlotManager - just loads into globalState
  // No state copying needed! Just track which slot to return to
  if (slot == -1) {
    slot = netSlot;
  }
  
  SlotManager& mgr = SlotManager::getInstance();
  String errorMsg;
  
  // Enter preview mode - loads slot directly into globalState
  if (!mgr.enterPreviewMode(slot, errorMsg)) {
    Serial.println("Error previewing slot " + String(slot) + ": " + errorMsg);
    return;
  }
  
  // Now globalState has the preview slot loaded
  // globalState.power is already updated by enterPreviewMode()
  
  // Compute paths and colors (but don't apply to hardware)
  clearAllNTCC();
  
  loadBridgesFromState();  // Copy bridges from globalState to newBridge[] array
  getNodesToConnect();     // Process newBridge[] array into nets
  rebuildChangedNetColorsFromBridges();  // Recompute net colors from bridges after net regeneration
  bridgesToPaths();        // Computes paths from nets
  clearLEDs();  // Clear everything including rails
  // Prepare colors and light up LEDs (but don't call leds.show() - core2stuff does that)
  checkChangedNetColors(-1);
  // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
  lightUpRail();  // Set rail LED colors based on globalState.power
  
  // Display voltage info if requested
  if (showVoltages) {
    Serial.println("\n╔════════════════════════════════╗");
    Serial.println("║  Slot " + String(slot) + " Preview (not applied)  ║");
    Serial.println("╚════════════════════════════════╝");
    Serial.println("  Connections: " + String(globalState.connections.numBridges));
    Serial.println("  Top Rail:    " + String(globalState.power.topRail, 2) + "V");
    Serial.println("  Bottom Rail: " + String(globalState.power.bottomRail, 2) + "V");
    Serial.println("  DAC 0:       " + String(globalState.power.dac0, 2) + "V");
    Serial.println("  DAC 1:       " + String(globalState.power.dac1, 2) + "V");
    Serial.println("════════════════════════════════\n");
  }
  
  // Trigger LED update - core2stuff will call showNets() and leds.show()
  requestLedShow( 1 );
  
  // NOTE: We stay in preview mode! Call exitPreview() to commit or cancel
}

void applyPreviewedSlot() {
  // Helper to apply the previewed slot and refresh hardware
  SlotManager& mgr = SlotManager::getInstance();
  String errorMsg;
  
  if (!mgr.isPreviewMode()) {
    Serial.println("Not in preview mode");
    return;
  }
  
  // Exit preview with apply=true (keeps the current slot)
  if (mgr.exitPreview(true, errorMsg)) {
    // Now apply to hardware
    refreshConnections();
    //Serial.println("✓ Applied slot " + String(netSlot));
  } else {
    Serial.println("Error applying preview: " + errorMsg);
  }
}

void cancelPreview() {
  // Helper to cancel preview and return to original slot
  SlotManager& mgr = SlotManager::getInstance();
  String errorMsg;
  
  if (!mgr.isPreviewMode()) {
    Serial.println("Not in preview mode");
    return;
  }
  
  int originalSlot = mgr.getOriginalSlotNumber();
  
  // Exit preview with apply=false (restores original slot)
  if (mgr.exitPreview(false, errorMsg)) {
    // Refresh to show original slot
    clearAllNTCC();
    clearLEDsExceptRails();
    getNodesToConnect();
    bridgesToPaths();
    clearLEDsExceptRails();
    // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
    requestLedShow( -1 );
    Serial.println("✓ Cancelled preview, returned to slot " + String(originalSlot));
  } else {
    Serial.println("Error cancelling preview: " + errorMsg);
  }
}

void clearChangedNetColors(int saveToFile) {
  for (int i = 0; i < MAX_NETS; i++) {
    changedNetColors[i].net = 0;
    changedNetColors[i].color = 0x000000;
    changedNetColors[i].node1 = 0;
    changedNetColors[i].node2 = 0;
    changedNetColors[i].fromBridge = false;
    }
  // saveToFile parameter is now ignored - colors are saved via YAML state system
  // if (saveToFile == 1) {
  //   saveChangedNetColorsToFile(netSlot, 0); // DEPRECATED: Colors now saved via YAML state
  // }
  }

void rebuildChangedNetColorsFromBridges(void) {
  // This function rebuilds changedNetColors from bridge colors after nets are regenerated
  // Bridge colors are stable (indexed by node pairs), but net numbers shift when nets change
  // We preserve manual user color changes (fromBridge==false) and only rebuild bridge-based colors
  
  // Step 1: Clear all bridge-based colors (keep manual user colors)
  for (int i = 0; i < MAX_NETS; i++) {
    if (changedNetColors[i].fromBridge) {
      changedNetColors[i].net = 0;
      changedNetColors[i].color = 0x000000;
      changedNetColors[i].node1 = 0;
      changedNetColors[i].node2 = 0;
      changedNetColors[i].fromBridge = false;
    }
  }
  
  // Step 2: Rebuild bridge-based colors using O(1) node→net index lookup
  // OPTIMIZATION: Use nodeToNetIndex[] for direct lookup instead of triple nested loop
  // Old: O(bridges × nets × nodes) = 192 × 60 × 100 = 1,152,000 checks worst case!
  // New: O(bridges) = 192 checks
  extern int8_t nodeToNetIndex[256];  // From NetManager.cpp
  
  for (int b = 0; b < globalState.connections.numBridges && b < MAX_BRIDGES; b++) {
    int n1 = globalState.connections.bridges[b][0];
    int n2 = globalState.connections.bridges[b][1];
    uint32_t bColor = globalState.connections.bridgeColors[b];
    
    if (bColor == 0xFFFFFFFF) continue;  // No color for this bridge
    
    // O(1) lookup: Find which net each node belongs to
    if (n1 >= 0 && n1 < 256 && n2 >= 0 && n2 < 256) {
      int net1 = nodeToNetIndex[n1];
      int net2 = nodeToNetIndex[n2];
      
      // Both nodes must be in the same net (and in a valid net)
      if (net1 >= 0 && net1 == net2 && net1 < MAX_NETS) {
        // This bridge belongs to this net - assign the color if not already manually set
        if (changedNetColors[net1].net != net1 || changedNetColors[net1].fromBridge) {
          changedNetColors[net1].net = net1;
          changedNetColors[net1].color = bColor;
          changedNetColors[net1].node1 = n1;
          changedNetColors[net1].node2 = n2;
          changedNetColors[net1].fromBridge = true;
        }
      }
    }
  }
}

int removeChangedNetColors(int node, int saveToFile) {
  int ret = 0;
  for (int i = 0; i < numberOfNets; i++) {
    if (changedNetColors[i].node1 == node || changedNetColors[i].node2 == node) {
      changedNetColors[i].net = 0;
      changedNetColors[i].color = 0x000000;
      changedNetColors[i].node1 = 0;
      changedNetColors[i].node2 = 0;
      changedNetColors[i].fromBridge = false;
      ret = 1;
      }
    }
  // saveToFile parameter is now ignored - colors are saved via YAML state system
  // if (saveToFile == 1 && ret == 1) {
  //   saveChangedNetColorsToFile(netSlot, 0); // DEPRECATED: Colors now saved via YAML state
  // }
  return ret;
  }

int checkChangedNetColors(int netIndex) {
  // OPTIMIZATION: Use nodeToNetIndex for O(1) lookups instead of quadruple nested loops
  // Old: O(nets × nodes × changedColors × nodes) = ~60 × 100 × 60 × 100 = 36M checks worst case!
  // New: O(changedColors) = ~60 checks
  
  extern int8_t nodeToNetIndex[256];  // From NetManager.cpp
  int ret = 0;
  bool loop = (netIndex < 0);  // If netIndex < 0, process all nets
  
  // Process changed net colors using direct lookup
  for (int j = 5; j < numberOfNets; j++) {
    if (changedNetColors[j].node1 <= 0) continue;  // Skip empty entries
    
    // O(1) lookup: Find which net this color belongs to
    int n1 = changedNetColors[j].node1;
    int n2 = changedNetColors[j].node2;
    
    if (n1 < 0 || n1 >= 256) continue;
    
    int net1 = nodeToNetIndex[n1];
    
    // If only one node specified, use that net
    if (n2 <= 0 || n2 >= 256) {
      if (net1 >= 0 && net1 < MAX_NETS) {
        // Check if we're processing this specific net or all nets
        if (!loop && net1 != netIndex) continue;
        
        // Move color to correct net index if needed
        if (j != net1) {
          changedNetColors[net1].net = net1;
          changedNetColors[net1].color = changedNetColors[j].color;
          changedNetColors[net1].node1 = changedNetColors[j].node1;
          changedNetColors[net1].node2 = changedNetColors[j].node2;
          changedNetColors[net1].fromBridge = changedNetColors[j].fromBridge;
          
          // Clear old entry
          changedNetColors[j].net = -1;
          changedNetColors[j].color = 0x000000;
          changedNetColors[j].node1 = 0;
          changedNetColors[j].node2 = 0;
          changedNetColors[j].fromBridge = false;
          ret = 1;
        }
        
        // Apply color to net
        globalState.connections.nets[net1].color = unpackRgb(changedNetColors[net1].color);
        netColors[net1] = globalState.connections.nets[net1].color;
        changedNetColors[net1].net = net1;
      }
    } else {
      // Both nodes specified - verify they're in the same net
      int net2 = nodeToNetIndex[n2];
      
      if (net1 >= 0 && net1 == net2 && net1 < MAX_NETS) {
        // Check if we're processing this specific net or all nets
        if (!loop && net1 != netIndex) continue;
        
        // Move color to correct net index if needed
        if (j != net1) {
          changedNetColors[net1].net = net1;
          changedNetColors[net1].color = changedNetColors[j].color;
          changedNetColors[net1].node1 = changedNetColors[j].node1;
          changedNetColors[net1].node2 = changedNetColors[j].node2;
          changedNetColors[net1].fromBridge = changedNetColors[j].fromBridge;
          
          // Clear old entry
          changedNetColors[j].net = -1;
          changedNetColors[j].color = 0x000000;
          changedNetColors[j].node1 = 0;
          changedNetColors[j].node2 = 0;
          changedNetColors[j].fromBridge = false;
          ret = 1;
        }
        
        // Apply color to net
        globalState.connections.nets[net1].color = unpackRgb(changedNetColors[net1].color);
        netColors[net1] = globalState.connections.nets[net1].color;
        changedNetColors[net1].net = net1;
      }
    }
  }
  
  return ret;
  }


uint32_t railNetColors[3] = // dim
  {  0x000f04, 0x0f0202, 0x0f0202 };

void assignNetColors(int preview) {
  // numberOfNets = 60;


  uint16_t colorDistance = (254 / (numberOfShownNets));
  if (numberOfShownNets < 4) {
    colorDistance = (254 / (4));
    }

  /* rgbColor specialNetColors[8] =
       {0x000000,
        0x00FF80,
        0xFF4114,
        0xFF0040,
        0xFF7800,
        0xFF4078,
        0xFFC8C8,
        0xC8FFC8};
*/
// leds.setPixelColor(110, rawOtherColors[2]);
// logoFlash = 2;
// requestLedShow( 1 );
//  if (debugLEDs) {
// Serial.print("\n\rcolorDistance: ");
// Serial.print(colorDistance);
// Serial.print("\n\r");
// Serial.print("numberOfNets: ");
// Serial.println(numberOfNets);
// Serial.print("numberOfShownNets: ");
// Serial.println(numberOfShownNets);
// Serial.print("\n\rassigning net colors\n\r");
//   Serial.print("\n\rNet\t\tR\tG\tB\t\tH\tS\tV");
//  delay(1);
//  }
  auto applySpecialColor = [&](int slot, int netIdx) {
    if (netIdx <= 0 || netIdx >= MAX_NETS) {
      return;
    }

    // if (globalState.connections.nets[netIdx].machine == true) {
    //   rgbColor specialNetRgb = unpackRgb(rawSpecialNetColors[slot]);
    //   globalState.connections.nets[netIdx].color = specialNetRgb;
    //   specialNetColors[slot] = specialNetRgb;
    //   netColors[netIdx] = specialNetRgb;
    //   return;
    // }

    uint32_t railColor = 0;
    switch (slot) {
      case 1:
        netColors[netIdx] = unpackRgb(railNetColors[0]);
        globalState.connections.nets[netIdx].color = netColors[netIdx];
        specialNetColors[slot] = netColors[netIdx];
        break;
      case 2:
        netColors[netIdx] = unpackRgb(railNetColors[1]);
        globalState.connections.nets[netIdx].color = netColors[netIdx];
        specialNetColors[slot] = netColors[netIdx];
        break;
      case 3:
        netColors[netIdx] = unpackRgb(railNetColors[2]);
        globalState.connections.nets[netIdx].color = netColors[netIdx];
        specialNetColors[slot] = netColors[netIdx];
        break;
      case 4:
        railColor = logoColors8vSelect[map((long)(globalState.power.dac0 * 10), -80, 80, 0, 59)];
        netColors[netIdx] = unpackRgb(railColor);
        globalState.connections.nets[netIdx].color = netColors[netIdx];
        specialNetColors[slot] = netColors[netIdx];
        break;
      case 5:
        railColor = logoColors8vSelect[map((long)(globalState.power.dac1 * 10), -80, 80, 0, 59)];
        netColors[netIdx] = unpackRgb(railColor);
        globalState.connections.nets[netIdx].color = netColors[netIdx];
        specialNetColors[slot] = netColors[netIdx];
        break;
      default:
        return;
    }
  };

  // Nets 1-5 are always special nets in this firmware build.
  for (int i = 1; i < 6; i++) {
    applySpecialColor(i, i);
  }

  int skipSpecialColors = 0;
  uint8_t hue = 1;

  int colorSlots[60] = { -1 };

  int colorSlots1[20] = { -1 };
  int colorSlots2[20] = { -1 };
  int colorSlots3[20] = { -1 };
  // Serial.print("number of nets: ");
  // Serial.println(numberOfNets);
  // Serial.print("number of shown nets: ");
  // Serial.println(numberOfShownNets);
  if (numberOfNets < 60 && numberOfShownNets > 0) {
    for (int i = 0; i <= numberOfShownNets; i++) {

      colorSlots[i] = abs(224 - ((((i)*colorDistance)))) % 254;
      }

    int backIndex = numberOfShownNets;

    int index1 = 0;
    int index2 = 0;
    int index3 = 0;
    //   int third = (numberOfNets - 8) / 3;

    for (int i = 0; i <= (numberOfShownNets); i++) {
      // Serial.print(colorSlots[i]);
      // Serial.print(" ");
      switch (i % 3) {
        case 0:
          colorSlots1[index1] = colorSlots[i];
          index1++;
          break;
        case 1:
          colorSlots2[index2] = colorSlots[i];
          index2++;
          break;
        case 2:
          colorSlots3[index3] = colorSlots[i];
          index3++;
          // backIndex--;
          break;
        }
      }
    //  if (debugLEDs) {
    // Serial.print("\n\n\rnumber of shown nets: ");
    // Serial.println(numberOfShownNets);
    // Serial.print("colorDistance: ");
    // Serial.println(colorDistance);
    // for (int i = 0; i < index1; i++) {
    //   Serial.print(colorSlots1[i]);
    //   Serial.print(" ");
    // }
    // Serial.println();
    // for (int i = 0; i < index2; i++) {
    //   // colorSlots2[i] = colorSlots2[(i + third) % index2];
    //   Serial.print(colorSlots2[i]);
    //   Serial.print(" ");
    // }

    // Serial.println();
    // for (int i = 0; i < index3; i++) {
    //   Serial.print(colorSlots3[i]);
    //   Serial.print(" ");
    // }
    // Serial.println("\n\r");
    //  }
    //   for (int i = 0; i < index1; i++) {
    //     colorSlots[i] = colorSlots1[(i)%index1];
    //   }
    //   for (int i = 0; i < index2; i++) {
    //     colorSlots[i + index1] = colorSlots2[(i+third)%index2];
    //   }
    //   for (int i = 0; i < index3; i++) {
    //     colorSlots[i + index1 + index2] = colorSlots3[(i+third*2)%index3];
    //   }
    int loop1index = 0;
    int loop2index = 0;
    int loop3index = 0;

    if (netColorMode == 0) {
      loop1index = 0;
      loop2index = 0;
      loop3index = 0;
      }
    if (netColorMode == 1) {
      loop1index = 0;
      loop2index = index2 / 2;
      loop3index = index3 - 1;
      }
    // Serial.print("netColorMode: ");
    // Serial.println(netColorMode);

    // Serial.print("loopInecies: ");
    // Serial.println(index1 + index2 + index3);

    for (int i = 0; i < index1 + index2 + index3; i++) {
      switch (i % 3) {
        case 0:
          colorSlots[i] = colorSlots1[loop1index];
          loop1index++;
          loop1index = loop1index % index1;
          break;
        case 1:

          colorSlots[i] = colorSlots2[loop2index];
          loop2index++;
          loop2index = loop2index % index2;
          break;
        case 2:

          colorSlots[i] = colorSlots3[loop3index];
          loop3index++;
          loop3index = loop3index % index3;
          // loop3index = loop3index % index3;
          // backIndex--;
          break;
        }
      }
    }
  //  if (debugLEDs) {
  // for (int i = 0; i < numberOfShownNets; i++) {

  //   Serial.print(colorSlots[i]);
  //   Serial.print(" ");
  // }
  // Serial.println("\n\n\n\r");
  // }
  // for (int i = 0; i < numberOfNets; i++) {
  //   Serial.println(colorSlots[i]);
  // }
  // Serial.println();
  // Serial.println();
  // int lastColor = numberOfNets - 8;
  // for (int i=0; i<(numberOfNets-8)/2; i++){
  //   int tempColor = colorSlots[i];
  //   colorSlots[i] = colorSlots[lastColor];
  //   colorSlots[lastColor] = tempColor;
  //   lastColor--;

  // }
  // for (int i = 0; i < numberOfShownNets; i++) {
  //   Serial.print("colorSlots[");
  //   Serial.print(i);
  //   Serial.print("]: ");
  //   Serial.println(colorSlots[i]);
  // }
  int frontIndex = 0;
  for (int i = 6; i <= numberOfNets; i++) {
    if (globalState.connections.nets[i].visible == 0) {
      // Serial.print("net ");
      // Serial.print(i);
      // Serial.println(" is not visible");

      continue;
      }

    int showingReading = 0;
    //bool manuallyChanged = false;

    if (preview == 0) {

      // HIGHEST PRIORITY: Real-time measurements (ADC and GPIO readings)
      // These show actual hardware state and override EVERYTHING else
      for (int a = 0; a < 8; a++) {
        if (i == showADCreadings[a]) {
          // netColors[i] = unpackRgb(rawOtherColors[8]);
          globalState.connections.nets[i].color = unpackRgb(adcReadingColors[a]);
          netColors[i] = globalState.connections.nets[i].color;
          showingReading = 1;
          // Serial.print("showing reading: ");
          // Serial.println(i);
          continue; // Skip all other color sources
          }
        }
      for (int a = 0; a < 10; a++) {
        if (i == gpioNet[a]) {
          globalState.connections.nets[i].color = unpackRgb(gpioReadingColors[a]);
          netColors[i] = globalState.connections.nets[i].color;
          // Serial.print("showing gpio: ");
          // Serial.println(i);
          // Serial.print("gpioReadingColors[");
          // Serial.print(a);
          // Serial.print("]: ");
          // Serial.println(gpioReadingColors[a], HEX);

          showingReading = 1;
          continue; // Skip all other color sources
          }
        }

      // SECOND: Check if this net has any bridges with colors (from Wokwi import)
      // Note: WokwiParser already handles "all green" and "black" by marking them as 0xFFFFFFFF
      uint32_t bridgeColorForNet = 0xFFFFFFFF;
      int bridgeColorCount = 0;
      
      // Check all bridges to see if any belong to this net
      for (int b = 0; b < globalState.connections.numBridges && b < MAX_BRIDGES; b++) {
        int n1 = globalState.connections.bridges[b][0];
        int n2 = globalState.connections.bridges[b][1];
        uint32_t bColor = globalState.connections.bridgeColors[b];
        
        // Skip if no color (0xFFFFFFFF set by parser for black/all-green cases)
        if (bColor == 0xFFFFFFFF) continue;
        
        // Check if either node of this bridge is in the current net
        bool inThisNet = false;
        for (int n = 0; n < MAX_NODES && globalState.connections.nets[i].nodes[n] != 0; n++) {
          if (globalState.connections.nets[i].nodes[n] == n1 || 
              globalState.connections.nets[i].nodes[n] == n2) {
            inThisNet = true;
            break;
          }
        }
        
        if (inThisNet) {
          // This bridge is in this net and has a color
          if (bridgeColorForNet == 0xFFFFFFFF || bridgeColorForNet == bColor) {
            bridgeColorForNet = bColor;
            bridgeColorCount++;
          }
          // If there are multiple different colors, use the first one found
        }
      }
      
      // If we found a bridge color for this net, use it
      if (bridgeColorForNet != 0xFFFFFFFF) {
        globalState.connections.nets[i].color = unpackRgb(bridgeColorForNet);
        netColors[i] = globalState.connections.nets[i].color;
        continue;
      }

      // THIRD: Check DisplayState for user-set custom colors (from Python API / YAML)
      {
        rgbColor customColor;
        uint32_t customRaw;
        char customName[32];
        if (globalState.display.getNetColor(i, customColor, customRaw, customName)) {
          globalState.connections.nets[i].color = customColor;
          netColors[i] = customColor;
          continue;
        }
      }

      // FOURTH: Legacy changedNetColors array (older system)
      if (changedNetColors[i].net == i) {
        globalState.connections.nets[i].color = unpackRgb(changedNetColors[i].color);
        netColors[i] = globalState.connections.nets[i].color;
        continue;
        //break;
        }

      }
    if (showingReading == 0 || preview != 0) {

      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;

      // int foundColor = 0;
      // Serial.print("\n\ri: ");
      // Serial.println(i);
      // Serial.print("frontIndex: ");
      // Serial.println(frontIndex);

      hue = colorSlots[frontIndex];
      // Serial.print("hue: ");
      // Serial.println(hue);
      frontIndex++;

      hsvColor netHsv = { hue, 255, LEDbrightness };

      //This was the old way, directly using index, prone to errors if nets are removed/added
      // if (changedNetColors[i].uniqueID == globalState.connections.nets[i].uniqueID) {
      //   hsvColor changedNetHsv = RgbToHsv(unpackRgb(changedNetColors[i].color));
      //   netHsv = changedNetHsv;
      // } else {
      //   netHsv = { hue, 254, LEDbrightness };
      // }


      if (brightenedNet != 0 && i == brightenedNet) {
        // Skip brightness boost for current sense nets - they'll be brightened in the overlay
       // bool isCurrentSenseNet = currentSenseState.plusConnected && currentSenseState.minusConnected &&
        //                         (i == currentSenseState.plusNet || i == currentSenseState.minusNet);
      //  if (!isCurrentSenseNet) {
      // Serial.println("brightenedNet != 0 && i == brightenedNet");
      // Serial.print("brightenedNet: ");
      // Serial.println(brightenedNet);
      // Serial.print("i: ");
      // Serial.println(i);
      // Serial.print("brightenedAmount: ");
      // Serial.println(brightenedAmount);
          netHsv.v += brightenedAmount;
      //  }
      }

      // if (warningNet != 0 && i == warningNet) {
      //   netHsv.h = netHsv.h /10;
      //   }
      // netHsv.v = 200;


      globalState.connections.nets[i].color = HsvToRgb(netHsv);
      netColors[i] = globalState.connections.nets[i].color;

      //  netColors[i] = globalState.connections.nets[i].color;

        // leds.setPixelColor(i, netColors[i]);

        // globalState.connections.nets[i].color.r = netColors[i].r;
        // globalState.connections.nets[i].color.g = netColors[i].g;
        // globalState.connections.nets[i].color.b = netColors[i].b;
        // if (debugLEDs) {
        //   Serial.print("\n\r");
        //   Serial.print(globalState.connections.nets[i].name);
        //   Serial.print("\t\t");
        //   Serial.print(globalState.connections.nets[i].color.r, DEC);
        //   Serial.print("\t");
        //   Serial.print(globalState.connections.nets[i].color.g, DEC);
        //   Serial.print("\t");
        //   Serial.print(globalState.connections.nets[i].color.b, DEC);
        //   Serial.print("\t\t");
        //   Serial.print(hue);
        //   Serial.print("\t");
        //   Serial.print(saturation);
        //   Serial.print("\t");
        //   Serial.print(LEDbrightness);
        //   delay(3);
        // }
      }
    }
  // listSpecialNets();
  // listNets();
  // logoFlash = 0;
  }



void lightUpNet(int netNumber, int node, int onOff, int brightness2,
                int hueShift, int dontClear, uint32_t forceColor) {
  uint32_t color;
  int pcbExtinction = 0;
  int colorCorrection = 0;
  int pcbHueShift = 0;
  // Serial.print("netNumber: ");
  // Serial.println(netNumber);

  //     Serial.print(" node: ");
  //     Serial.print(node);
  //     Serial.print(" onOff: ");
  //     Serial.print(onOff);
  // return;


  if (netNumber <= 0 || netNumber >= MAX_NETS) {
    return;
    }
  if (globalState.connections.nets[netNumber].nodes[1] != 0 &&
      globalState.connections.nets[netNumber].nodes[1] <= 141) { // NANO_A7) {

    for (int j = 0; j < MAX_NODES; j++) {
      if (globalState.connections.nets[netNumber].nodes[j] <= 0) {
        break;
        }

      // if (globalState.connections.nets[netNumber].machine == true) {
      //   //Serial.println("machine");
      //   if (globalState.connections.nets[netNumber].nodes[j] == node || node == -1) {
      //     if (onOff == 1) {
      //       if (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]] > 0) {
      //         leds.setPixelColor(
      //             (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 0,
      //             scaleDownBrightness(globalState.connections.nets[netNumber].rawColor));
      //         leds.setPixelColor(
      //             (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 1,
      //             scaleDownBrightness(globalState.connections.nets[netNumber].rawColor));
      //         leds.setPixelColor(
      //             (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 2,
      //             scaleDownBrightness(globalState.connections.nets[netNumber].rawColor));
      //         leds.setPixelColor(
      //             (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 3,
      //             scaleDownBrightness(globalState.connections.nets[netNumber].rawColor));
      //         leds.setPixelColor(
      //             (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 4,
      //             scaleDownBrightness(globalState.connections.nets[netNumber].rawColor));

      //         if (debugLEDs) {
      //           Serial.print("net: ");
      //           Serial.print(netNumber);
      //           Serial.print(" node: ");
      //           Serial.print(globalState.connections.nets[netNumber].nodes[j]);
      //           Serial.print(" mapped to LED:");
      //           Serial.println(nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]);

      //           Serial.print("rawColor: ");
      //           Serial.println(globalState.connections.nets[netNumber].rawColor, HEX);
      //           }
      //         }
      //       }
      //     }
      //   } else {

        if (globalState.connections.nets[netNumber].nodes[j] <= NANO_A7) {

          if (globalState.connections.nets[netNumber].nodes[j] == node || node == -1) {
            if (onOff == 1) {

              pcbExtinction = 0;
              colorCorrection = 0;
              pcbHueShift = 0;

              if (globalState.connections.nets[netNumber].nodes[j] >= NANO_D0 &&
                  globalState.connections.nets[netNumber].nodes[j] <= NANO_A7) {
                // pcbExtinction = PCBEXTINCTION;

                // Serial.println (brightness2);
                // hueShift += PCBHUESHIFT;
                colorCorrection = 1;
                }
              // pcbExtinction += (brightness2-DEFAULTBRIGHTNESS);

              struct rgbColor colorToShift = { globalState.connections.nets[netNumber].color.r,
                                              globalState.connections.nets[netNumber].color.g,
                                              globalState.connections.nets[netNumber].color.b };

              if (forceColor != 0xffffff) {
                // Serial.println("force color");
                colorToShift = unpackRgb(forceColor);
                }

              struct rgbColor shiftedColor =
                shiftHue(colorToShift, hueShift, pcbExtinction, 254);

              if (colorCorrection != 0) {
                // shiftedColor = pcbColorCorrect(shiftedColor);
                //Serial.println("color correction"); 
                uint32_t correctedColor =
                  packRgb(shiftedColor.r, shiftedColor.g, shiftedColor.b);

                shiftedColor = unpackRgb(scaleBrightness(correctedColor, 100));
                }

              hsvColor shiftedColorHsv = RgbToHsv(shiftedColor);

              if (globalState.connections.nets[netNumber].specialFunction >= 100 &&
                  globalState.connections.nets[netNumber].specialFunction <= 105) {
                // Serial.println("rail color");
                if (brightness2 != DEFAULTBRIGHTNESS) {
                  shiftedColorHsv.v = brightness2;
                  } else {
                  shiftedColorHsv.v = LEDbrightnessRail;
                  }
                if (brightenedNet == netNumber) {
                  // Skip brightness boost for current sense nets
                  bool isCurrentSenseNet = currentSenseState.plusConnected && currentSenseState.minusConnected &&
                                           (netNumber == currentSenseState.plusNet || netNumber == currentSenseState.minusNet);
                  if (!isCurrentSenseNet) {
                    shiftedColorHsv.v += brightenedAmount;
                  }
                }
                shiftedColor = HsvToRgb(shiftedColorHsv);

                color = packRgb(shiftedColor.r, shiftedColor.g, shiftedColor.b);
                // color = packRgb((shiftedColor.r * LEDbrightnessRail) >> 8,
                // (shiftedColor.g * LEDbrightnessRail) >> 8, (shiftedColor.b
                // * LEDbrightnessRail) >> 8);
                //  Serial.print("rail color: ");
                //  Serial.print(color, HEX);
                } else if (globalState.connections.nets[netNumber].specialFunction >= 100 &&
                           globalState.connections.nets[netNumber].specialFunction <= 120) {
                  // Serial.println("special function");
                  if (brightness2 != DEFAULTBRIGHTNESS) {
                    shiftedColorHsv.v = brightness2;
                    } else {
                    shiftedColorHsv.v = LEDbrightnessSpecial;
                    }
                  if (brightenedNet == netNumber) {
                    // Skip brightness boost for current sense nets
                    bool isCurrentSenseNet = currentSenseState.plusConnected && currentSenseState.minusConnected &&
                                             (netNumber == currentSenseState.plusNet || netNumber == currentSenseState.minusNet);
                    if (!isCurrentSenseNet) {
                      shiftedColorHsv.v += brightenedAmount;
                      // shiftedColorHsv.v *= 8;
                      shiftedColorHsv.v =
                        constrain(shiftedColorHsv.v, 0, 255);
                    }
                  }
                  shiftedColor = HsvToRgb(shiftedColorHsv);

                  color = packRgb(shiftedColor.r, shiftedColor.g, shiftedColor.b);
                  // color = packRgb((shiftedColor.r * LEDbrightnessSpecial) >>
                  // 8, (shiftedColor.g * LEDbrightnessSpecial) >> 8,
                  // (shiftedColor.b
                  // * LEDbrightnessSpecial) >> 8);
                  } else {
                  //Serial.println("normal color");
                  // Serial.print("shiftedColorHsv.v: ");
                  // Serial.println(shiftedColorHsv.v);
                  if (brightness2 != DEFAULTBRIGHTNESS) {
                    // shiftedColorHsv.v = brightness2; //maybe put an averaging thing here
                    } else {
                    // shiftedColorHsv.v = LEDbrightness;
                    }
                  if (brightenedNet == netNumber) {
                    // Skip brightness boost for current sense nets
                    bool isCurrentSenseNet = currentSenseState.plusConnected && currentSenseState.minusConnected &&
                                             (netNumber == currentSenseState.plusNet || netNumber == currentSenseState.minusNet);
                    if (!isCurrentSenseNet) {
                      shiftedColorHsv.v += brightenedAmount;
                    }
                  }
                  shiftedColor = HsvToRgb(shiftedColorHsv);

                  color = packRgb(shiftedColor.r, shiftedColor.g, shiftedColor.b);

                  // color = packRgb((shiftedColor.r * LEDbrightness) >> 8,
                  // (shiftedColor.g * LEDbrightness) >> 8, (shiftedColor.b *
                  // LEDbrightness) >> 8);
                  }
                int allOnTop = 1;
                if (globalState.connections.nets[netNumber].nodes[j] >= NANO_D0) {
                  rgbColor colorToShift = unpackRgb(color);
                  // colorToShift = shiftHue(colorToShift, hueShift);
                  hsvColor brighterColor = RgbToHsv(colorToShift);
                  if (brightenedNet == netNumber) {
                    // Skip brightness boost for current sense nets
                    bool isCurrentSenseNet = currentSenseState.plusConnected && currentSenseState.minusConnected &&
                                             (netNumber == currentSenseState.plusNet || netNumber == currentSenseState.minusNet);
                    if (!isCurrentSenseNet) {
                      // shiftedColorHsv.v += brightenedAmount;
                      shiftedColorHsv.v = 255;
                      brighterColor.v = 255;
                      // brighterColor.v =
                      //     constrain(shiftedColorHsv.v, 0, 255);
                      // shiftedColorHsv.v =
                      //     constrain(shiftedColorHsv.v, 0, 255);
                    }
                  }
                  brighterColor.v += PCBEXTINCTION;
                  rgbColor bright = HsvToRgb(brighterColor);

                  color = packRgb(bright.r, bright.g, bright.b);

                  for (int k = 0; k < MAX_NODES; k++) {
                    if (globalState.connections.nets[netNumber].nodes[k] < NANO_D0 &&
                        globalState.connections.nets[netNumber].nodes[k] > 0) {
                      allOnTop = 0;
                      break;
                      }
                    }
                  }
                if (allOnTop == 1) {
                  netColors[netNumber] = unpackRgb(color);
                  }
                // globalState.connections.nets[netNumber].rawColor = color;
                // globalState.connections.nets[netNumber].color = unpackRgb(color);

                if (probeHighlight - 1 != (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]])) {
                  // Serial.print("nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]] = ");
                  // Serial.println(nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]);
                  if (globalState.connections.nets[netNumber].nodes[j] >= NANO_D0) {
                    if (brightenedNet == netNumber) {
                      // Skip brightness boost for current sense nets
                      bool isCurrentSenseNet = currentSenseState.plusConnected && currentSenseState.minusConnected &&
                                               (netNumber == currentSenseState.plusNet || netNumber == currentSenseState.minusNet);
                      if (!isCurrentSenseNet) {
                        if (brightenedNode == globalState.connections.nets[netNumber].nodes[j]) {
                          color = scaleBrightness(color, brightenedNodeAmount);
                        } else {
                          color = scaleBrightness(color, brightenedNetAmount);
                        }
                      } else {
                        color = scaleBrightness(color, 0);
                      }
                    } else {
                      color = scaleBrightness(color, 0);
                    }
#if defined(OG_JUMPERLESS)
                    // OG: single LED chain. Nano/SF header LEDs live at their
                    // plain nodesToPixelMap index (80-110), NOT at the V5 +320
                    // special-LED offset. Using +320 here writes off the 111-LED
                    // strip so the LED never lit; map straight through instead.
                    leds.setPixelColor(
                        nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]],
                        color);
#else
                    leds.setPixelColor(
                        (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) + 320, color);
#endif

                    } else {
#if defined(OG_JUMPERLESS)
                    // OG: 1 LED per row. nodesToPixelMap[node] is the OG pixel index.
                    leds.setPixelColor(
                        nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]],
                        color);
#else
                    leds.setPixelColor(
                        (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 0,
                        color);
                    leds.setPixelColor(
                        (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 1,
                        color);
                    leds.setPixelColor(
                        (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 2,
                        color);
                    leds.setPixelColor(
                        (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 3,
                        color);
                    leds.setPixelColor(
                        (nodesToPixelMap[globalState.connections.nets[netNumber].nodes[j]]) * 5 + 4,
                        color);
#endif

                    // if (logoTopSetting[
                    }
                  } else {

                  }


              } else {

              }
            }
          }
        // }
      }
    // turnOffSkippedNodes();
    /*                                                            Serial.print("color:
       "); Serial.print(color,HEX); Serial.print(" r: ");
                                                        Serial.print(shiftedColor.r);
                                                        Serial.print(" g: ");
                                                        Serial.print(shiftedColor.g);
                                                        Serial.print(" b: ");
                                                        Serial.print(shiftedColor.b);
                                                        Serial.print("
       hueShift:
       "); Serial.print(hueShift); Serial.print(" pcbExtinction: ");
                                                        Serial.print(pcbExtinction);
                                                        Serial.print("
       brightness2: "); Serial.println(brightness2);*/
    }
  // showRowAnimation(-1, GND);
  //  requestLedShow( 1 );
  showSkippedNodes();
  }
unsigned long lastSkippedNodesTime = 0;
int toggleSkippedNodes = 0;
void showSkippedNodes(uint32_t onColor, uint32_t offColor) {
  // return;
  // onColor = 0x1f1f2f;
  // uint32_t offColor = 0x0f000f;

  static int colorCycleOn = 0;
  colorCycleOn++;
  if (colorCycleOn > 254) {
    colorCycleOn = 0;
    }
  static int colorCycleOff = 200;
  colorCycleOff--;
  if (colorCycleOff < 0) {
    colorCycleOff = 254;
    }

  rgbColor onColorRgb = unpackRgb(onColor);
  rgbColor offColorRgb = unpackRgb(offColor);
  hsvColor onColorHsv = RgbToHsv(onColorRgb);
  hsvColor offColorHsv = RgbToHsv(offColorRgb);
  onColorHsv.h = (onColorHsv.h + colorCycleOn) % 254;
  offColorHsv.h = (offColorHsv.h + colorCycleOff) % 254;
  onColorRgb = HsvToRgb(onColorHsv);
  offColorRgb = HsvToRgb(offColorHsv);
  onColor = packRgb(onColorRgb.r, onColorRgb.g, onColorRgb.b);
  offColor = packRgb(offColorRgb.r, offColorRgb.g, offColorRgb.b);

  if (millis() - lastSkippedNodesTime > (1111)) {
    // Serial.println("skipped nodes");

    toggleSkippedNodes = !toggleSkippedNodes;

    lastSkippedNodesTime = millis();

    } else {
    // onColor = 0x1f1f2f;
    // onColor = 0;
    // onColor = 0x1f1f2f;
    // onColor = 0;
    // onColor = 0x1f1f2f;
    }

  for (int i = 0; i < numberOfPaths; i++) {

    if (globalState.connections.paths[i].skip == 1) {
#if defined(OG_JUMPERLESS)
      // A dropped DUPLICATE (stack_paths) is marked skip=1 with cleared coords
      // when no free parallel lane exists (e.g. a BB<->NANO route, where the
      // BB chip reaches the nano chip on a single crossbar lane). That is NOT a
      // routing failure - the primary path routed fine - so it must not blink
      // the endpoint like an unconnectable node (the row 42 "couldn't find a
      // path" artifact). Only render genuine unrouted (non-duplicate) skips.
      if (globalState.connections.paths[i].duplicate == 1) {
        continue;
      }
      // OG has 1 LED per breadboard row. The V5 blink pattern below writes
      // pixels (node-1)*5 + 0..4 (the 5-LEDs-per-row physical layout), which on
      // the OG's single-pixel-per-row strip lights five rows' worth of LEDs for
      // one skipped node. Render each skipped breadboard endpoint as its single
      // mapped pixel (nodesToPixelMap[node]) with the same on/off alternation,
      // then skip the V5 column logic.
      {
        int sn1 = globalState.connections.paths[i].node1;
        int sn2 = globalState.connections.paths[i].node2;
        if (sn1 > 0 && sn1 <= 60) {
          leds.setPixelColor(nodesToPixelMap[sn1], toggleSkippedNodes ? onColor : offColor);
        }
        if (sn2 > 0 && sn2 <= 60) {
          leds.setPixelColor(nodesToPixelMap[sn2], toggleSkippedNodes ? offColor : onColor);
        }
      }
      continue;
#endif
      // colorCycleOff = (colorCycleOff) % 254;
      // colorCycleOn = (colorCycleOn + (numberOfUnconnectablePaths)) % 254;
      if (globalState.connections.paths[i].node1 > 0 && globalState.connections.paths[i].node1 <= 60) {

        if ((toggleSkippedNodes == 1 && globalState.connections.paths[i].node1 % 2 == 0) || (toggleSkippedNodes == 0 && globalState.connections.paths[i].node1 % 2 == 1)) {

          leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 0, onColor);

          // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 0, offColor);

          // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 1, onColor);

          leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 1, offColor);

          leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 2, onColor);

          // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 2, offColor);

          // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 3, onColor);

          leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 3, offColor);

          leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 4, onColor);

          // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 4, offColor);
          //toggleSkippedNodes = !toggleSkippedNodes;

          } else if ((toggleSkippedNodes == 0 && globalState.connections.paths[i].node1 % 2 == 0) || (toggleSkippedNodes == 1 && globalState.connections.paths[i].node1 % 2 == 1)) {

            // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 0, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 0, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 1, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 1, offColor);

            // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 2, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 2, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 3, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 3, offColor);

            // leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 4, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node1 - 1) * 5 + 4, offColor);
            // toggleSkippedNodes = !toggleSkippedNodes;
             //}
            }

        } else if (globalState.connections.paths[i].node1 >= NANO_D0 && globalState.connections.paths[i].node1 <= NANO_5V) {
          hsvColor onColorHsv = RgbToHsv(onColorRgb);
          hsvColor offColorHsv = RgbToHsv(offColorRgb);
          onColorHsv.h = (onColorHsv.h + colorCycleOn + 40) % 254;
          offColorHsv.h = (offColorHsv.h + colorCycleOff + 40) % 254;
          onColorHsv.v += 90;
          offColorHsv.v += 40;
          onColorHsv.s = 80;
          offColorHsv.s = 120;
          onColorRgb = HsvToRgb(onColorHsv);
          offColorRgb = HsvToRgb(offColorHsv);
          uint32_t onColorHeader = packRgb(onColorRgb.r / 3, onColorRgb.g / 2, onColorRgb.b);
          uint32_t offColorHeader = packRgb(offColorRgb.r / 3, offColorRgb.g / 2, offColorRgb.b);
          if (toggleSkippedNodes == 1) {
            for (int j = 0; j < 35; j++) {
              if (bbPixelToNodesMapV5[j][0] == globalState.connections.paths[i].node1) {
                leds.setPixelColor(bbPixelToNodesMapV5[j][1], onColorHeader);
                }
              }
            } else {
            for (int j = 0; j < 35; j++) {
              if (bbPixelToNodesMapV5[j][0] == globalState.connections.paths[i].node1) {
                leds.setPixelColor(bbPixelToNodesMapV5[j][1], offColorHeader);
                }
              }
            }
          }

        if (globalState.connections.paths[i].node2 > 0 && globalState.connections.paths[i].node2 <= 60) {
          if (toggleSkippedNodes == 0) {

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 0, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 0, offColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 1, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 1, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 2, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 2, offColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 3, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 3, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 4, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 4, offColor);

            } else {

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 0, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 0, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 1, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 1, offColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 2, onColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 2, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 3, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 3, offColor);

            leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 4, onColor);

            // leds.setPixelColor((globalState.connections.paths[i].node2 - 1) * 5 + 4, offColor);
            //}
            }

          } else if (globalState.connections.paths[i].node1 >= NANO_D0 && globalState.connections.paths[i].node2 <= NANO_5V) {
            hsvColor onColorHsv = RgbToHsv(onColorRgb);
            hsvColor offColorHsv = RgbToHsv(offColorRgb);
            onColorHsv.h = (onColorHsv.h + colorCycleOn + 40) % 254;
            offColorHsv.h = (offColorHsv.h + colorCycleOff + 40) % 254;
            onColorHsv.v += 90;
            offColorHsv.v += 40;
            onColorHsv.s = 80;
            offColorHsv.s = 120;
            onColorRgb = HsvToRgb(onColorHsv);
            offColorRgb = HsvToRgb(offColorHsv);
            uint32_t onColorHeader = packRgb(onColorRgb.r / 3, onColorRgb.g / 2, onColorRgb.b);
            uint32_t offColorHeader = packRgb(offColorRgb.r / 3, offColorRgb.g / 2, offColorRgb.b);
            if (toggleSkippedNodes == 0) {
              for (int j = 0; j < 35; j++) {
                if (bbPixelToNodesMapV5[j][0] == globalState.connections.paths[i].node2) {
                  leds.setPixelColor(bbPixelToNodesMapV5[j][1], onColorHeader);
                  }
                }
              } else {
              for (int j = 0; j < 35; j++) {
                if (bbPixelToNodesMapV5[j][0] == globalState.connections.paths[i].node2) {
                  leds.setPixelColor(bbPixelToNodesMapV5[j][1], offColorHeader);
                  }
                }
              }
            }

          // leds.show();
      }
    }
  }

/// @brief  scale the brightness of a color
/// @param hexColor - the color to scale
/// @param scaleFactor - -100 (completely off) to 1000 (10x brighter), 0 is no change
/// @return the scaled color
uint32_t scaleBrightness(uint32_t hexColor, int scaleFactor) {

  if (scaleFactor == 0) {
    return hexColor;
    }
  float scaleFactorF = scaleFactor / 100.0;

  // if (scaleFactor > 0) {
  scaleFactorF += 1.0;
  //}
  // Serial.print("scaleFactorF: ");
  // Serial.println(scaleFactorF);
  // Serial.print("hexColor: ");
  // Serial.println(hexColor, HEX);

  hsvColor colorToShiftHsv = RgbToHsv(unpackRgb(hexColor));

  float hsvF = colorToShiftHsv.v * scaleFactorF;

  colorToShiftHsv.v = (unsigned char)hsvF;

  rgbColor colorToShiftRgb = HsvToRgb(colorToShiftHsv);
  // Serial.print("shifted: ");
  // Serial.println(packRgb(colorToShiftRgb.r, colorToShiftRgb.g,
  // colorToShiftRgb.b), HEX);
  return packRgb(colorToShiftRgb.r, colorToShiftRgb.g, colorToShiftRgb.b);
  }

uint32_t scaleDownBrightness(uint32_t hexColor, int scaleFactor,
                             int maxBrightness) {
  int maxR = maxBrightness;
  int maxG = maxBrightness;
  int maxB = maxBrightness;

  int r = (hexColor >> 16) & 0xFF;
  int g = (hexColor >> 8) & 0xFF;
  int b = hexColor & 0xFF;

  int scaledBrightness = hexColor;

  if (r > maxR || g > maxG || b > maxB) {
    scaledBrightness = 0;
    r = r / scaleFactor;
    g = g / scaleFactor;
    b = b / scaleFactor;

    scaledBrightness = scaledBrightness | (r << 16);
    scaledBrightness = scaledBrightness | (g << 8);
    scaledBrightness = scaledBrightness | b;
    }

  return scaledBrightness;
  }

uint32_t scaleUpBrightness(uint32_t hexColor, int scaleFactor,
                           int minBrightness) {
  int minR = minBrightness;
  int minG = minBrightness;
  int minB = minBrightness;

  int r = (hexColor >> 16) & 0xFF;
  int g = (hexColor >> 8) & 0xFF;
  int b = hexColor & 0xFF;

  int scaledBrightness = hexColor;

  if (r < minR && g < minG && b < minB) {
    scaledBrightness = 0;
    r = r * scaleFactor;
    g = g * scaleFactor;
    b = b * scaleFactor;

    if (r > 254) {
      r = 254;
      }
    if (g > 254) {
      g = 254;
      }
    if (b > 254) {
      b = 254;
      }

    scaledBrightness = scaledBrightness | (r << 16);
    scaledBrightness = scaledBrightness | (g << 8);
    scaledBrightness = scaledBrightness | b;
    }

  return scaledBrightness;
  }

struct rgbColor pcbColorCorrect(rgbColor colorToShift) { //unused

  uint8_t redShift = 0;
  uint8_t greenShift = 0;
  uint8_t blueShift = 0;

  int testNeg = 0;

  struct rgbColor colorToShiftRgb = colorToShift;

  struct hsvColor colorToShiftHsv = RgbToHsv(colorToShiftRgb);

  colorToShiftHsv.v += PCBEXTINCTION;

  if (colorToShiftHsv.h > 100 && colorToShiftHsv.h < 150) {

    // Serial.print("hue: ");
    // Serial.print(colorToShiftHsv.h);
    // Serial.print("\tcolorToShift.r: ");
    // Serial.print(colorToShift.r);
    // Serial.print("\tcolorToShift.g: ");
    // Serial.print(colorToShift.g);
    // Serial.print("\tcolorToShift.b: ");
    // Serial.print(colorToShift.b);

    if (PCBREDSHIFTBLUE < 0) {
      testNeg = colorToShiftRgb.r;
      testNeg -= abs(PCBREDSHIFTBLUE);

      if (testNeg < 0) {
        colorToShiftRgb.r = 0;
        } else {

        colorToShiftRgb.r = colorToShiftRgb.r - abs(PCBREDSHIFTBLUE);
        }
      } else {

      colorToShiftRgb.r = colorToShiftRgb.r + abs(PCBREDSHIFTBLUE);

      if (colorToShiftRgb.r > 254) {
        colorToShiftRgb.r = 254;
        }
      }

    if (PCBGREENSHIFTBLUE < 0) {

      testNeg = colorToShiftRgb.g;
      testNeg -= abs(PCBGREENSHIFTBLUE);

      if (testNeg < 0) {
        colorToShiftRgb.g = 0;
        } else {
        colorToShiftRgb.g = colorToShiftRgb.g - abs(PCBGREENSHIFTBLUE);
        }
      } else {
      colorToShiftRgb.g = colorToShiftRgb.g + abs(PCBGREENSHIFTBLUE);
      if (colorToShiftRgb.g > 254) {
        colorToShiftRgb.g = 254;
        }
      }

    if (PCBBLUESHIFTBLUE < 0) {

      testNeg = colorToShiftRgb.b;

      testNeg -= abs(PCBBLUESHIFTBLUE);

      if (testNeg < 0) {
        colorToShiftRgb.b = 0;
        } else {
        colorToShiftRgb.b = colorToShiftRgb.b - abs(PCBBLUESHIFTBLUE);
        }
      } else {
      colorToShiftRgb.b = colorToShiftRgb.b + abs(PCBBLUESHIFTBLUE);
      if (colorToShiftRgb.b > 254) {
        colorToShiftRgb.b = 254;
        }
      }

    // Serial.print("\t\tShifted.r: ");

    // Serial.print(colorToShiftRgb.r);
    // Serial.print("\tShifted.g: ");
    // Serial.print(colorToShiftRgb.g);
    // Serial.print("\tShifted.b: ");
    // Serial.println(colorToShiftRgb.b);
    // Serial.print("\n\n\r");
    } else if (colorToShiftHsv.h >= 150 && colorToShiftHsv.h < 255) {

      // Serial.print("hue: ");
      // Serial.print(colorToShiftHsv.h);
      // Serial.print("\tcolorToShift.r: ");
      // Serial.print(colorToShift.r);
      // Serial.print("\tcolorToShift.g: ");
      // Serial.print(colorToShift.g);
      // Serial.print("\tcolorToShift.b: ");
      // Serial.print(colorToShift.b);

      if (PCBREDSHIFTPINK < 0) {
        testNeg = colorToShiftRgb.r;
        testNeg -= abs(PCBREDSHIFTPINK);

        if (testNeg < 0) {
          colorToShiftRgb.r = 0;
          } else {

          colorToShiftRgb.r = colorToShiftRgb.r - abs(PCBREDSHIFTPINK);
          }
        } else {

        colorToShiftRgb.r = colorToShiftRgb.r + abs(PCBREDSHIFTPINK);

        if (colorToShiftRgb.r > 254) {
          colorToShiftRgb.r = 254;
          }
        }

      if (PCBGREENSHIFTPINK < 0) {

        testNeg = colorToShiftRgb.g;
        testNeg -= abs(PCBGREENSHIFTPINK);

        if (testNeg < 0) {
          colorToShiftRgb.g = 0;
          } else {
          colorToShiftRgb.g = colorToShiftRgb.g - abs(PCBGREENSHIFTPINK);
          }
        } else {
        colorToShiftRgb.g = colorToShiftRgb.g + abs(PCBGREENSHIFTPINK);
        if (colorToShiftRgb.g > 254) {
          colorToShiftRgb.g = 254;
          }
        }

      if (PCBBLUESHIFTPINK < 0) {

        testNeg = colorToShiftRgb.b;

        testNeg -= abs(PCBBLUESHIFTPINK);

        if (testNeg < 0) {
          colorToShiftRgb.b = 0;
          } else {
          colorToShiftRgb.b = colorToShiftRgb.b - abs(PCBBLUESHIFTPINK);
          }
        } else {
        colorToShiftRgb.b = colorToShiftRgb.b + abs(PCBBLUESHIFTPINK);
        if (colorToShiftRgb.b > 254) {
          colorToShiftRgb.b = 254;
          }
        }
      }
    return colorToShiftRgb;
  }

struct rgbColor shiftHue(struct rgbColor colorToShift, int hueShift,
  int brightnessShift, int saturationShift,
                         int specialFunction)

  {

  struct hsvColor colorToShiftHsv = RgbToHsv(colorToShift);

  colorToShiftHsv.h = colorToShiftHsv.h + hueShift;
  colorToShiftHsv.s = colorToShiftHsv.s + saturationShift;

  colorToShiftHsv.v = colorToShiftHsv.v + brightnessShift;

  if (colorToShiftHsv.v > 255) {
    colorToShiftHsv.v = 255;
    }

  if (colorToShiftHsv.s > 255) {
    colorToShiftHsv.s = 255;
    }

  if (colorToShiftHsv.h > 255) {
    colorToShiftHsv.h = colorToShiftHsv.h - 255;
    }

  struct rgbColor colorToShiftRgb = HsvToRgb(colorToShiftHsv);

  return colorToShiftRgb;
  }
bool photos = false; //this lets me adjust the brightness for better photos

void lightUpNode(int node, uint32_t color) {

  leds.setPixelColor(nodesToPixelMap[node], color);
  requestLedShow( 1 );
  }
uint32_t dimLogoColor(uint32_t color, int brightness) {
  // return color;
  rgbColor dimColor = unpackRgb(color);
  // if (dimColor.b != 0)
  // {
  //     dimColor.b = dimColor.b * 2;
  // }

  hsvColor colorHsv = RgbToHsv(dimColor);

  colorHsv.v = brightness;
  if (photos == true ) {
    return packRgb(HsvToRgb(colorHsv).r / 3, HsvToRgb(colorHsv).g / 3,
                   HsvToRgb(colorHsv).b);
    } else {
      colorHsv.v = brightness -5;
    return packRgb(HsvToRgb(colorHsv).r, HsvToRgb(colorHsv).g * 1,
                   HsvToRgb(colorHsv).b * 3);
    }
  }

// ============================================================================
// LOGO COLOR PALETTE ARRAYS
// ============================================================================
uint32_t logoColors[LOGO_COLOR_LENGTH + 11] = {
    0x800058, 0x750053, 0x700068, 0x650063, 0x600078, 0x550073, 0x500088,
    0x450083, 0x400098, 0x350093, 0x3000A8, 0x2500A3, 0x2000B8, 0x1500B3,
    0x1000C8, 0x0502C3, 0x0204D8, 0x0007E3, 0x0010E8, 0x0015F3, 0x0020F8,
    0x0025FA, 0x0030FF, 0x0035E0, 0x0240BF, 0x0545A0, 0x10509F, 0x15558F,
    0x20607F, 0x25656F, 0x30705F, 0x35754F, 0x40803F, 0x45722F, 0x506518,
    0x55481A, 0x603A2A, 0x653332, 0x702538, 0x751948, 0x791052, 0x7E0562,
  };

// Original palettes
uint32_t logoColorsCold[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsHot[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsPink[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsGreen[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsYellow[LOGO_COLOR_LENGTH + 1];

// New palettes
uint32_t logoColorsOrange[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsTurquoise[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsChartreuse[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsPurple[LOGO_COLOR_LENGTH + 1];
uint32_t logoColorsWhite[LOGO_COLOR_LENGTH + 1];

uint32_t logoColors8vSelect[LOGO_COLOR_LENGTH + 11];

// Master array holding all palettes - use LogoPalette enum to index
uint32_t logoColorsAll[LOGO_PALETTE_COUNT][LOGO_COLOR_LENGTH + 11];

uint8_t eightSelectHues[LOGO_COLOR_LENGTH + 11] = {
    195, 191, 187, 183, 179, 175, 171, 168, 166, 164, 162, 160, 158, 156, 153,
    148, 140, 130, 120, 111, 104, 99,  96,  93,  91,  89,  87,  85,  83,  81,
    80,  79,  78,  76,  73,  70,  67,  64,  59,  54,  49,  44,  39,  35,  31,
    27,  23,  19,  16,  13,  10,  7,   4,   3,   2,   1,   0,   254, 253, 251,
    248, 242, 236, 230, 224, 218, 212, 207, 202, 199, 197 };

// ============================================================================
// HELPER: Generate a single-hue palette from a base hue value
// Creates a symmetric gradient that shifts hue slightly across the palette
// ============================================================================
void generateLogoPalette(uint32_t* dest, uint8_t baseHue, int paletteIndex, float step) {
  rgbColor tempRGB;
  hsvColor hsv;
  hsv.s = 254;
  hsv.v = 254;
  
  for (int i = 0; i < (LOGO_COLOR_LENGTH / 2) + 1; i++) {
    hsv.h = ((int)(i * step) + baseHue) % 255;
    tempRGB = HsvToRgb(hsv);
    
    uint32_t color = packRgb(tempRGB.r / 8, tempRGB.g / 8, tempRGB.b / 8);
    dest[i] = color;
    dest[LOGO_COLOR_LENGTH - i] = color;
    
    if (paletteIndex >= 0 && paletteIndex < LOGO_PALETTE_COUNT) {
      logoColorsAll[paletteIndex][i] = color;
      logoColorsAll[paletteIndex][LOGO_COLOR_LENGTH - i] = color;
    }
  }
}
// void generateLogoPaletteYellow(uint32_t* dest, uint8_t baseHue, int paletteIndex) {
//   rgbColor tempRGB;
//   hsvColor hsv;
//   hsv.s = 254;
//   hsv.v = 254;
  
//   for (int i = 0; i < (LOGO_COLOR_LENGTH / 2) + 1; i++) {
//     hsv.h = (i * 4 + baseHue) % 255;
//     tempRGB = HsvToRgb(hsv);
    
//     uint32_t color = packRgb(tempRGB.r / 8, tempRGB.g / 8, tempRGB.b / 8);
//     dest[i] = color;
//     dest[LOGO_COLOR_LENGTH - i] = color;
    
//     if (paletteIndex >= 0 && paletteIndex < LOGO_PALETTE_COUNT) {
//       logoColorsAll[paletteIndex][i] = color;
//       logoColorsAll[paletteIndex][LOGO_COLOR_LENGTH - i] = color;
//     }
//   }
// }


// ============================================================================
// HELPER: Generate white/neutral palette (special case - low saturation)
// ============================================================================
static void generateWhitePalette(uint32_t* dest, int paletteIndex) {
  rgbColor tempRGB;
  hsvColor hsv;
  hsv.s = 30;  // Low saturation for near-white
  hsv.v = 254;
  
  for (int i = 0; i < (LOGO_COLOR_LENGTH / 2) + 1; i++) {
    hsv.h = (i * 4) % 255;  // Subtle rainbow tint
    tempRGB = HsvToRgb(hsv);
    
    uint32_t color = packRgb(tempRGB.r / 8, tempRGB.g / 8, tempRGB.b / 8);
    dest[i] = color;
    dest[LOGO_COLOR_LENGTH - i] = color;
    
    if (paletteIndex >= 0 && paletteIndex < LOGO_PALETTE_COUNT) {
      logoColorsAll[paletteIndex][i] = color;
      logoColorsAll[paletteIndex][LOGO_COLOR_LENGTH - i] = color;
    }
  }
}

// ============================================================================
// SETUP ALL LOGO SWIRL COLORS
// To add a new palette:
//   1. Add entry to LogoPalette enum in LEDs.h
//   2. Add HUE_XXXX define in LEDs.h  
//   3. Add extern array declaration in LEDs.h
//   4. Add array definition above
//   5. Add generateLogoPalette() call below
// ============================================================================
void setupSwirlColors(void) {
  rgbColor logoColorsRGB[LOGO_COLOR_LENGTH + 12];

  // Generate all single-hue palettes using the helper function
  // Each palette uses HUE_XXX defined in LEDs.h for easy tweaking
  generateLogoPalette(logoColorsCold,       HUE_COLD,       PALETTE_COLD);
  generateLogoPalette(logoColorsHot,        HUE_HOT,        PALETTE_HOT);
  generateLogoPalette(logoColorsPink,       HUE_PINK,       PALETTE_PINK);
  generateLogoPalette(logoColorsYellow,     HUE_YELLOW,     PALETTE_YELLOW);
  generateLogoPalette(logoColorsGreen,      HUE_GREEN,      PALETTE_GREEN);
  generateLogoPalette(logoColorsOrange,     HUE_ORANGE,     PALETTE_ORANGE);
  generateLogoPalette(logoColorsTurquoise,  HUE_TURQUOISE,  PALETTE_TURQUOISE);
  generateLogoPalette(logoColorsChartreuse, HUE_CHARTREUSE, PALETTE_CHARTREUSE);
  generateLogoPalette(logoColorsPurple,     HUE_PURPLE,     PALETTE_PURPLE);
  
  // White palette uses special low-saturation generation
  generateWhitePalette(logoColorsWhite, PALETTE_WHITE);

  // Generate the rainbow palette (full spectrum)
  for (int i = 0; i <= LOGO_COLOR_LENGTH + 10; i++) {
    hsvColor connectHSV;
    connectHSV.h = i * (255 / LOGO_COLOR_LENGTH);
    connectHSV.s = 254;
    connectHSV.v = 254;
    rgbColor connectRGB = HsvToRgb(connectHSV);
    logoColorsRGB[i] = connectRGB;

    logoColors[i] = packRgb(logoColorsRGB[i].r / 8, logoColorsRGB[i].g / 8,
                            logoColorsRGB[i].b / 8);
    logoColorsAll[PALETTE_RAINBOW][i] = logoColors[i];
    
    // Generate 8V select palette (special reversed hue sequence)
    connectHSV.h = eightSelectHues[i];
    connectHSV.s = 254;
    connectHSV.v = 254;
    connectRGB = HsvToRgb(connectHSV);
    logoColorsRGB[i] = connectRGB;

    logoColors8vSelect[i] = packRgb(
        logoColorsRGB[i].r / 8, logoColorsRGB[i].g / 8, logoColorsRGB[i].b / 8);

#if !defined(OG_JUMPERLESS)
    // OG's logoColorsAll is a single rainbow row (LOGO_PALETTE_COUNT == 1); the
    // 8V-select palette index is out of bounds there. The standalone
    // logoColors8vSelect[] above is still populated for any direct consumer.
    logoColorsAll[PALETTE_8V_SELECT][(LOGO_COLOR_LENGTH + 10) - i] = logoColors8vSelect[i];
#endif
  }
}

// ============================================================================
// LOGO LED HELPER FUNCTIONS
// ============================================================================

// Set all 8 logo LEDs using a palette array with given start offset and spread
// brightness: optional dimming (default 20 from dimLogoColor)
static inline void setLogoFromPalette(uint32_t* palette, int start, int spread, int brightness = 20) {
  for (int i = 0; i < 8; i++) {
    leds.setPixelColor(
        LOGO_LED_START + i,
        dimLogoColor(palette[(start + (spread * i)) % (LOGO_COLOR_LENGTH - 1)], brightness));
  }
}

// Set all 8 logo LEDs using a LogoPalette enum index
static inline void setLogoFromPaletteIndex(int paletteIndex, int start, int spread, int brightness = 20) {
  uint32_t* palette = logoColorsAll[paletteIndex % LOGO_PALETTE_COUNT];
  setLogoFromPalette(palette, start, spread, brightness);
}

// Set all 8 logo LEDs to a single solid color
static inline void setLogoSolidColor(uint32_t color) {
  for (int i = 0; i < 8; i++) {
    leds.setPixelColor(LOGO_LED_START + i, color);
  }
}

// Set top 3 logo LEDs (indices 0-2)
static inline void setLogoTop(uint32_t color) {
  leds.setPixelColor(LOGO_LED_START + 0, color);
  leds.setPixelColor(LOGO_LED_START + 1, color);
  leds.setPixelColor(LOGO_LED_START + 2, color);
}

// Set bottom 3 logo LEDs (indices 3-5)
static inline void setLogoBottom(uint32_t color) {
  leds.setPixelColor(LOGO_LED_START + 3, color);
  leds.setPixelColor(LOGO_LED_START + 4, color);
  leds.setPixelColor(LOGO_LED_START + 5, color);
}

// ============================================================================
// LogoRing — reusable ring indicator (6 ring LEDs + center button LED)
// ============================================================================
LogoRing logoRing;

void setLogoRing(int itemCount, int selectedIndex, int hueOffset) {
  logoRing.itemCount = itemCount;
  logoRing.selectedIndex = selectedIndex;
  logoRing.hueOffset = hueOffset;
}

// ============================================================================
// Ring hue tweak table — adjust how specific hue ranges render anywhere the
// ring palette shows up (ring LEDs, menu text, depth pads). Each entry covers
// an inclusive HSV hue range on the mod-256 wheel (0..255, 0 = red) and can
// shift the hue and/or scale saturation and value (percent, 100 = unchanged).
// First matching range wins. A range with hueStart > hueEnd wraps around the
// red boundary: { 240, 10, ... } covers 240..255 and 0..10.
// Examples:
//   { 31, 50, -6, 100, 100 },   // pull muddy yellows toward orange
//   { 126, 160, 0, 80, 100 },   // desaturate blues 20%
//   { 245, 8, 0, 100, 80 },     // dim the reds across the wrap
// ============================================================================
static const RingHueTweak ringHueTweaks[] = {
  // add entries here
  { 0, 10, -1, 90, 100 },
  { 50, 60, 5, 100, 100 },
};

uint32_t applyRingHueTweaks(uint32_t color) {
  if (color == 0) return 0;
  hsvColor hsv = RgbToHsv(color);
  for (unsigned int i = 0; i < sizeof(ringHueTweaks) / sizeof(ringHueTweaks[0]); i++) {
    const RingHueTweak& t = ringHueTweaks[i];
    // Inclusive range; hueStart > hueEnd means it wraps around red (255->0).
    bool inRange = (t.hueStart <= t.hueEnd)
                       ? (hsv.h >= t.hueStart && hsv.h <= t.hueEnd)
                       : (hsv.h >= t.hueStart || hsv.h <= t.hueEnd);
    if (!inRange) continue;
    // The HSV wheel used by HsvToRgb/RgbToHsv is mod-256, so shifts wrap with
    // & 0xFF: 0 - 1 -> 255, 255 + 1 -> 0.
    hsv.h = (uint8_t)(((int)hsv.h + t.hueShift) & 0xFF);
    int s = ((int)hsv.s * t.satPercent) / 100;
    int v = ((int)hsv.v * t.valPercent) / 100;
    hsv.s = (uint8_t)(s > 255 ? 255 : s);
    hsv.v = (uint8_t)(v > 255 ? 255 : v);
    return HsvToRaw(hsv);
  }
  return color;
}

// Per-hue brightness compensation for the yellowish PCB: warm hues (red->green)
// transmit brighter, cool hues (cyan->violet) get filtered down, so we trim the
// warm slots and boost the cool ones to balance perceived ring brightness.
static inline int pcbHueBrightnessScale(uint8_t hue) {
  switch (hue) {
    case 0 ... 15:    return 100; // red
    case 16 ... 30:   return 78;  // orange
    case 31 ... 50:   return 52;  // yellow
    case 51 ... 75:   return 58;  // chartreuse
    case 76 ... 100:  return 66;  // green
    case 101 ... 125: return 92;  // aqua/cyan
    case 126 ... 160: return 145; // blue
    case 161 ... 185: return 138; // indigo
    case 186 ... 215: return 118; // violet/purple
    default:          return 100; // magenta/pink
  }
}

// Pick the color for item `item` of `count`. Hues are spaced evenly across the
// whole palette by item count — item k of N sits at k/N-th of the circle — so a
// 4-item menu shows 4 evenly spaced hues, not 4 of the 6 fixed slot hues. The
// granular hueOffset shifts the whole set. Brightness depends on selection,
// is scaled by the angular weight (weightNum/weightDenom — how much of the
// item's position lands on this LED), and is PCB-corrected per hue after the
// per-range tweak table is applied.
static inline uint32_t ringItemColor(int item, int count, bool selected,
                                     int weightNum, int weightDenom) {
  int bright = selected ? logoRing.selectedBrightness : logoRing.baseBrightness;
  if (weightDenom > 0 && weightNum < weightDenom) {
    bright = (bright * weightNum) / weightDenom;
  }
  const uint32_t* palette =
      (logoRing.colorMode == RING_PALETTE && logoRing.palette != nullptr)
          ? logoRing.palette
          : logoColorsAll[PALETTE_RAINBOW];
  if (count < 1) count = 1;
  int idx = (((item * LOGO_COLOR_LENGTH) / count) + logoRing.hueOffset) % LOGO_COLOR_LENGTH;
  if (idx < 0) idx += LOGO_COLOR_LENGTH;
  uint32_t base = applyRingHueTweaks(palette[idx]);
  // Balance perceived brightness across the ring for the PCB tint.
  uint8_t hue = RgbToHsv(unpackRgb(base)).h;
  // Brightness saturates — never wrap it. Hues are circular; brightness is
  // not: the PCB boost for cool hues (up to 145%) can push a selected slot
  // past 254, and wrapping would fold the brightest slots down to near-dark
  // (selected violet: 250 * 118 / 100 = 295 -> 40).
  int corrected = (bright * pcbHueBrightnessScale(hue)) / 100;
  if (corrected < 1) corrected = 1;
  if (corrected > 254) corrected = 254;
  return dimLogoColor(base, corrected);
}

// One LED's winning claim on the ring: which item lights it, whether that
// item is the selection, and how much of the item's angular position falls
// on this LED (weight/denominator). Selected items always beat unselected
// ones; among equals the larger weight wins.
struct RingContrib {
  int  item;     // -1 = unlit
  bool selected;
  int  weight;   // numerator, out of the layout's denominator (itemCount)
};

static inline void ringAddContrib(RingContrib* c, int item, bool selected, int weight) {
  if (weight <= 0) return;
  if (c->item < 0 || (selected && !c->selected) ||
      (selected == c->selected && weight > c->weight)) {
    c->item = item;
    c->selected = selected;
    c->weight = weight;
  }
}

// The ring LEDs are wired counter-clockwise from LOGO_LED_START, which made
// the indicator orbit against the encoder's rotation. Flip the logical slot
// order onto the physical LEDs (slot 0 stays at top-right) so increasing
// slots — i.e. clockwise encoder turns — advance clockwise on the board.
static inline int ringSlotToLed(int slot) {
  return LOGO_LED_START + ((6 - slot) % 6);
}

// Center LED: "V" press indicator. Starts at full-brightness white the moment
// the press lands, fades to black by the midpoint of the hold window, then
// rises back to full brightness in the active color (the selected item's ring
// hue) — the white->color flip telegraphs that the hold action is about to
// fire. During menu hold-to-back stepping (HELD with holdStepLengthMs set)
// the same V replays once per back-step, anchored to the last step time. Off
// when idle.
static inline uint32_t ringCenterColor(void) {
  unsigned long start, length;
  if (encoderButtonState == PRESSED) {
    start = buttonHoldStart;
    length = buttonHoldLength;
  } else if ((encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
              encoderButtonState == LONG_HELD) &&
             logoRing.holdStepLengthMs > 0) {
    start = logoRing.holdStepStartMs;
    length = logoRing.holdStepLengthMs;
  } else {
    return 0;
  }

  float frac = (length > 0) ? ((float)(millis() - start) / (float)length) : 1.0f;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;

  if (frac < 0.5f) {
    // Downstroke of the V: white fading out.
    uint8_t val = (uint8_t)((1.0f - frac * 2.0f) * (float)logoRing.centerBrightness);
    hsvColor hsv = { 0, 0, val };
    return HsvToRaw(hsv);
  }

  // Upstroke: the complement of the selected item's ring color fading in
  // (same hue math as ringItemColor, then flipped halfway around the mod-256
  // wheel) — the center always contrasts with the selected slot beside it.
  const uint32_t* palette =
      (logoRing.colorMode == RING_PALETTE && logoRing.palette != nullptr)
          ? logoRing.palette
          : logoColorsAll[PALETTE_RAINBOW];
  int count = logoRing.itemCount < 1 ? 1 : logoRing.itemCount;
  int idx = (((logoRing.selectedIndex * LOGO_COLOR_LENGTH) / count) + logoRing.hueOffset) % LOGO_COLOR_LENGTH;
  if (idx < 0) idx += LOGO_COLOR_LENGTH;
  hsvColor hsv = RgbToHsv(applyRingHueTweaks(palette[idx]));
  hsv.h = (uint8_t)(hsv.h + 128); // inverse hue (uint8_t wraps mod 256)
  hsv.v = (uint8_t)((frac - 0.5f) * 2.0f * (float)logoRing.centerBrightness);
  return HsvToRaw(hsv);
}

bool renderLogoRing(void) {
  if (!logoRing.enabled) {
    return false;
  }

  // While the encoder is held past the press window, hand the logo back to the
  // existing hold/reboot sweep so its countdown stays visible — unless the
  // menu is hold-stepping back through levels (holdStepLengthMs set), where
  // the ring stays up and the center V recharges once per step instead.
  if (encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
      encoderButtonState == LONG_HELD) {
    if (logoRing.holdStepLengthMs == 0) {
      return false;
    }
  }

  int itemCount = logoRing.itemCount;
  int selectedIndex = logoRing.selectedIndex;

  // Winning contribution per ring slot. Each slot is painted in its item's
  // own hue (evenly spaced by item count) at a brightness proportional to how
  // much of the item's angle falls on that LED.
  RingContrib contrib[6] = {
    { -1, false, 0 }, { -1, false, 0 }, { -1, false, 0 },
    { -1, false, 0 }, { -1, false, 0 }, { -1, false, 0 },
  };
  int weightDenom = 1;

  if (itemCount <= 0) {
    // nothing to lay out — ring stays dark, only the center indicator shows
  } else if (itemCount <= 6) {
    // True angular spread: item k of N sits at k/N-th of the circle, i.e. at
    // slot position k*6/N. When that lands between two LEDs the item lights
    // both, split by proximity — 5 items really look like 5 evenly spaced
    // points, not 5 of the 6 fixed slots.
    weightDenom = itemCount;
    for (int k = 0; k < itemCount; k++) {
      int num = k * 6;
      int slotA = num / itemCount;          // 0..5
      int rem = num % itemCount;            // fractional part, /itemCount
      bool sel = (k == selectedIndex);
      ringAddContrib(&contrib[slotA % 6], k, sel, itemCount - rem);
      ringAddContrib(&contrib[(slotA + 1) % 6], k, sel, rem);
    }
  } else {
    // More items than slots: light all 6, selection rotates/wraps (relative).
    // Each slot shows the item that occupies it in the selection's window, so
    // neighbor hues stay attached to their items as the ring scrolls.
    int selectedSlot = ((selectedIndex % 6) + 6) % 6;
    for (int s = 0; s < 6; s++) {
      int item = (selectedIndex + (s - selectedSlot)) % itemCount;
      if (item < 0) item += itemCount;
      ringAddContrib(&contrib[s], item, s == selectedSlot, 1);
    }
  }

  for (int slot = 0; slot < 6; slot++) {
    if (contrib[slot].item >= 0) {
      leds.setPixelColor(ringSlotToLed(slot),
                         ringItemColor(contrib[slot].item, itemCount,
                                       contrib[slot].selected,
                                       contrib[slot].weight, weightDenom));
    } else {
      leds.setPixelColor(ringSlotToLed(slot), (uint32_t)0);
    }
  }

  leds.setPixelColor(LOGO_LED_START + 6, ringCenterColor());
  return true;
}

// Mark to run from RAM to avoid flash contention during saves
#if defined(OG_JUMPERLESS)
// Rainbow swirl boot animation, ported from the OG reference firmware
// (JumperlessNano startupColors()). The OG strip is a single chain of 111 LEDs
// (1 per breadboard row + rails + header), with the logo at pixel 110. Runs once
// on core0 at boot AFTER core1 finishes initLEDs() (core0 waits on
// core2initFinished first), so core0 owns the strip exclusively here and can
// drive leds.show() directly.
void ogStartupAnimation(void) {
  const int count     = 111; // physical OG LED count
  const int logoPixel = 110; // single logo LED
  hsvColor hsv;
  int offset = 1;
  int fade = 0;
  int done = 0;

  for (long j = 4; j < 162; j += 2) {
    // Fade in for the first frames, hold at full, then fade out and stop.
    if (j < DEFAULTBRIGHTNESS / 3) {
      fade = j * 3;
    } else {
      int fadeout = j - DEFAULTBRIGHTNESS;
      if (fadeout < 0) fadeout = 0;
      if (fadeout > DEFAULTBRIGHTNESS) {
        fadeout = DEFAULTBRIGHTNESS;
        done = 1;
      }
      fade = DEFAULTBRIGHTNESS - fadeout;
    }

    for (int i = 0; i < count; i++) {
      float i2 = i;
      float j2 = j + 50;
      float huef = (i2 * j2) * 0.1f;
      hsv.h = ((int)huef) % 254;
      hsv.s = 254;
      if (((i + offset) % count) == logoPixel) {
        hsv.v = 85; // logo stays brighter than the swirl
        hsv.h = (189 + j);
      } else {
        hsv.v = fade;
      }
      rgbColor rgb = HsvToRgb(hsv);
      leds.setPixelColor((i + offset) % count, packRgb(rgb.r, rgb.g, rgb.b));
    }

    offset += 1;
    leds.show();
    if (done) break;
    delayMicroseconds(14000);
  }
  clearLEDs();
}
#endif

void __not_in_flash_func(logoSwirl)(int start, int spread, int probe) {




if (logoLedAccess == true) {
  return;
}
logoLedAccess = true;

  // ========================================================================
  // MENU RING INDICATOR - Highest precedence while a click-menu owns the logo.
  // Paints the 6 ring LEDs (item layout + selection) and the center button
  // fade. Suppresses the undo/filesystem/measure indicators below. When the
  // encoder is held past the press window renderLogoRing() returns false and we
  // fall through to the hold/reboot sweep (button press animation) for handoff.
  // ========================================================================
  if (logoRing.enabled) {
    // Once past the press window the hold/reboot sweep owns the logo ring
    // (rainbow + state flashes). renderLogoRing() already returns false for
    // that when holdStepLengthMs==0; during menu hold-to-back the ring would
    // otherwise stay up and block buttonPressAnimActive entirely.
    bool holdAnimOwnsLogo = buttonPressAnimActive &&
        (encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ||
         encoderButtonState == LONG_HELD);
    if (!holdAnimOwnsLogo && renderLogoRing()) {
      logoLedAccess = false;
      return;
    }
  }

  // ========================================================================
  // BUTTON PRESS ANIMATION - White flash / rainbow during press-to-hold
  // ========================================================================
  if (buttonPressAnimActive) {
    for (int i = 0; i < 8; i++) {
      leds.setPixelColor(LOGO_LED_START + i, pressAnimLogoColors[i]);
    }
    logoLedAccess = false;
    return;
  }

  unsigned long now = millis();

  // ========================================================================
  // UNDO/HISTORY ACTIVITY INDICATOR - Yellow when undo fires, while the
  // disconnect button is held long enough to arm the scroll-history
  // gesture, or while the History scrub menu is open.
  // Takes precedence over the filesystem indicator so the user sees the
  // undo signal even when the persist worker just kicked a flash write.
  // ========================================================================
  if (undoActivityUntil > 0 && now < undoActivityUntil) {
    setLogoFromPaletteIndex(undoIndicatorPalette, start, spread);
    logoLedAccess = false;
    return;
  }

  // ========================================================================
  // FILESYSTEM ACTIVITY INDICATOR - Shows during flash operations
  // Uses configurable palette (filesystemIndicatorPalette in externVars.cpp)
  // ========================================================================
  bool showFilesystemIndicator = filesystemActive || (filesystemActiveUntil > 0 && now < filesystemActiveUntil);

  if (showFilesystemIndicator) {
    setLogoFromPaletteIndex(filesystemIndicatorPalette, start, spread);
    logoLedAccess = false;
    return;
  }

  // ========================================================================
  // MEASURE MODE INDICATOR - Shows pink during voltage measurement
  // Uses configurable palette (measureModeIndicatorPalette in externVars.cpp)
  // ========================================================================
  if (measureModeActive) {
    setLogoFromPaletteIndex(measureModeIndicatorPalette, start, spread);
    logoLedAccess = false;
    return;
  }

  // ========================================================================
  // PROBE MODE - Different colors based on probe state
  // ========================================================================
  if (probe == 1) {
    if (connectOrClearProbe == 1 && node1or2 == 0) {
      // Connect mode, no node selected yet -> Cold (cyan)
      setLogoFromPalette(logoColorsCold, start, spread);
    } else if (connectOrClearProbe == 1 && node1or2 != 0) {
      // Connect mode, node selected -> Pink (brighter)
      setLogoFromPalette(logoColorsPink, start, spread, 33);
    } else {
      // Clear mode -> Hot (red/orange)
      setLogoFromPalette(logoColorsHot, start, spread);
    }
  } else {
    // ========================================================================
    // DEFAULT MODE - Rainbow swirl
    // ========================================================================
    setLogoFromPalette(logoColors, start, spread);
  }

  // ========================================================================
  // OVERRIDES - Applied on top of the above
  // ========================================================================
  if (logoColorOverride != -1) {
    setLogoSolidColor(logoColorOverride);
  }

  if (logoColorOverrideTop == -2) {
    setLogoTop(logoColorOverrideTopDefault);
  } else if (logoColorOverrideTop != -1) {
    setLogoTop(logoColorOverrideTop);
  }

  if (logoColorOverrideBottom == -2) {
    setLogoBottom(logoColorOverrideBottomDefault);
  } else if (logoColorOverrideBottom != -1) {
    setLogoBottom(logoColorOverrideBottom);
  }

logoLedAccess = false;

  }





bool lightUpName = false;


uint32_t rstColors[2] = { 0x2000b9, 0x0020f9, };

///0 = rst_b 1 = rst_t  2 = vin  3 = 3v3  4 = 5v  5 = gnd_t  6 = gnd_b
uint32_t headerColors[7] = { 0x2000b9, 0x0020f9, 0xa0a000,  0x1C0107,0x1C0702, 0x001C04,0x001C04 };
unsigned long colorFlash[2] = { 0,0 };

// uint32_t railColors[4] = {0x2000b9, 0x0020f9, 0xa0a000, 0x1C0107};

void __not_in_flash_func(lightUpHeader)(void) {

// Bounded wait -- the old infinite spin could hold Core 2 executing from flash
// indefinitely, causing XIP crashes when Core 0 writes to flash.
{
  unsigned long _t = micros();
  while (logoLedAccess == true) {
    if (core1FramesHeld() || (micros() - _t > 5000)) return;
  }
}
logoLedAccess = true;
  if (rstColors[0] != headerColors[0] && colorFlash[0] == 0) {
    colorFlash[0] = millis();
    } else if (rstColors[0] != headerColors[0] && millis() - colorFlash[0] > 150) {
      colorFlash[0] = 0;
      rstColors[0] = headerColors[0];
      }
    if (rstColors[1] != headerColors[1] && colorFlash[1] == 0) {
      colorFlash[1] = millis();
      } else if (rstColors[1] != headerColors[1] && millis() - colorFlash[1] > 150) {
        colorFlash[1] = 0;
        rstColors[1] = headerColors[1];
        }

      if (RST0colorOverride != -1) {
        leds.setPixelColor(RST_0_LED, RST0colorOverride);
        } else {
        leds.setPixelColor(RST_0_LED, scaleDownBrightness(rstColors[0], 4, 55));
        }
      if (RST1colorOverride != -1) {
        leds.setPixelColor(RST_1_LED, RST1colorOverride);
        } else {
        leds.setPixelColor(RST_1_LED, scaleDownBrightness(rstColors[1], 4, 55));
        }
      if (GNDTcolorOverride != -1) {
        leds.setPixelColor(GND_T_LED, GNDTcolorOverride);
        } else {
        leds.setPixelColor(GND_T_LED, scaleDownBrightness(headerColors[5], 2, 35));
        }
      if (GNDBcolorOverride != -1) {
        leds.setPixelColor(GND_B_LED, GNDBcolorOverride);
        } else {
        leds.setPixelColor(GND_B_LED, scaleDownBrightness(headerColors[6], 2, 35));
        }
      if (VINcolorOverride != -1) {
        leds.setPixelColor(VIN_LED, VINcolorOverride);
        } else {
        leds.setPixelColor(VIN_LED, scaleDownBrightness(headerColors[2], 5, 35));
        }
      if (V3V3colorOverride != -1) {
        leds.setPixelColor(V3V3_LED, V3V3colorOverride);
        } else {
        leds.setPixelColor(V3V3_LED, scaleDownBrightness(headerColors[3], 5, 35));
        }
      if (V5VcolorOverride != -1) {
        leds.setPixelColor(V5V_LED, V5VcolorOverride);
        } else {
        leds.setPixelColor(V5V_LED, scaleDownBrightness(headerColors[4], 5, 35));
        }

      if (sfProbeMenu == 1) {
        leds.setPixelColor(ADC_LED_0, scaleBrightness(rawOtherColors[8], -40));
        leds.setPixelColor(ADC_LED_1, scaleBrightness(rawOtherColors[11], -40));
        leds.setPixelColor(DAC_LED_0, 0);
        leds.setPixelColor(DAC_LED_1, 0);
        leds.setPixelColor(GPIO_LED_0, 0);
        leds.setPixelColor(GPIO_LED_1, 0);

        } else if (sfProbeMenu == 2) {
          leds.setPixelColor(ADC_LED_0, 0);
          leds.setPixelColor(ADC_LED_1, 0);
          leds.setPixelColor(DAC_LED_0, scaleBrightness(rawOtherColors[9], -40));
          leds.setPixelColor(DAC_LED_1, scaleBrightness(rawOtherColors[12], -40));
          leds.setPixelColor(GPIO_LED_0, 0);
          leds.setPixelColor(GPIO_LED_1, 0);
          } else if (sfProbeMenu == 3) {
            leds.setPixelColor(ADC_LED_0, 0);
            leds.setPixelColor(ADC_LED_1, 0);
            leds.setPixelColor(DAC_LED_0, 0);
            leds.setPixelColor(DAC_LED_1, 0);
            leds.setPixelColor(GPIO_LED_0, scaleBrightness(rawOtherColors[10], -40));
            leds.setPixelColor(GPIO_LED_1, scaleBrightness(rawOtherColors[13], -40));

            } else {
            if (photos == true && false) {
              leds.setPixelColor(ADC_LED_0, scaleBrightness(rawOtherColors[8], -40));
              leds.setPixelColor(ADC_LED_1, scaleBrightness(rawOtherColors[11], -40));
              leds.setPixelColor(DAC_LED_0, scaleBrightness(rawOtherColors[9], -40));
              leds.setPixelColor(DAC_LED_1, scaleBrightness(rawOtherColors[12], -40));
              leds.setPixelColor(GPIO_LED_0, scaleBrightness(rawOtherColors[10], -40));
              leds.setPixelColor(GPIO_LED_1, scaleBrightness(rawOtherColors[13], -40));
              } else {

              if (ADCcolorOverride0 == -1) {
                leds.setPixelColor(ADC_LED_0, scaleBrightness(rawOtherColors[8], -40));
                } else if (ADCcolorOverride0 == -2) {
                  leds.setPixelColor(ADC_LED_0, ADCcolorOverride0Default);
                  } else {
                  leds.setPixelColor(ADC_LED_0, ADCcolorOverride0);
                  }

                if (ADCcolorOverride1 == -1) {
                  leds.setPixelColor(ADC_LED_1, scaleBrightness(rawOtherColors[11], -40));
                  } else if (ADCcolorOverride1 == -2) {
                    leds.setPixelColor(ADC_LED_1, ADCcolorOverride1Default);
                    } else {
                    leds.setPixelColor(ADC_LED_1, ADCcolorOverride1);
                    }

                  if (DACcolorOverride0 == -1) {
                    leds.setPixelColor(DAC_LED_0, scaleBrightness(rawOtherColors[9], -40));
                    } else if (DACcolorOverride0 == -2) {
                      leds.setPixelColor(DAC_LED_0, DACcolorOverride0Default);
                      } else {
                      leds.setPixelColor(DAC_LED_0, DACcolorOverride0);
                      }

                    if (DACcolorOverride1 == -1) {
                      leds.setPixelColor(DAC_LED_1, scaleBrightness(rawOtherColors[12], -40));
                      } else if (DACcolorOverride1 == -2) {
                        leds.setPixelColor(DAC_LED_1, DACcolorOverride1Default);
                        } else {
                        leds.setPixelColor(DAC_LED_1, DACcolorOverride1);
                        }

                      if (GPIOcolorOverride0 == -1) {
                        leds.setPixelColor(GPIO_LED_0, scaleBrightness(rawOtherColors[10], -40));
                        } else if (GPIOcolorOverride0 == -2) {
                          leds.setPixelColor(GPIO_LED_0, GPIOcolorOverride0Default);
                          } else {
                          leds.setPixelColor(GPIO_LED_0, GPIOcolorOverride0);
                          }

                        if (GPIOcolorOverride1 == -1) {
                          leds.setPixelColor(GPIO_LED_1, scaleBrightness(rawOtherColors[13], -40));
                          } else if (GPIOcolorOverride1 == -2) {
                            leds.setPixelColor(GPIO_LED_1, GPIOcolorOverride1Default);
                            } else {
                            leds.setPixelColor(GPIO_LED_1, GPIOcolorOverride1);
                            }
              }
            }

  logoLedAccess = false;
  }


uint32_t rawRailColors[3][4] =
  { {0x1C0110, 0x001C04, 0x1C0110, 0x001C04},

   {0x210904, 0x001C04, 0x210904, 0x001C04},

   {0x301A02, 0x001C04, 0x120932, 0x001C04} };


uint32_t railColorsV5[4][5] = {
  {0x1b010b, 0x21030b, 0x301A02, 0x210806, 0x16051b},//top dim, top bright, top danger, top brightened, top negative
  {0x001C05, 0x002C14, 0x0f2C0f, 0x0f2C0f, 0x002C14},//gnd dim, gnd bright, gnd danger, gnd brightened, gnd negative
  {0x1a020e, 0x20040a, 0x301A02, 0x210a08, 0x15061a},//bottom dim, bottom bright, bottom danger, bottom brightened, bottom negative
  {0x001C05, 0x002514, 0x0f2C0f, 0x0a2C0a, 0x002C14}//gnd dim, gnd bright, gnd danger, gnd brightened, gnd negative
  };



uint32_t dotColor = 0x06061f;
uint32_t dangerDot = 0x10101f;
uint32_t negDot = 0x1f0006;


int scaleScale(int value) {
  int ledBrightnessDelta = LEDbrightnessRail - (DEFAULTRAILBRIGHTNESS);

  int scaleFactor = ledBrightnessDelta;
  // Serial.print("scaleFactor ");
  // Serial.println(scaleFactor);
  // Serial.print("value ");
  // Serial.println(value);

  int scaled = value;
  //scaled = value / (abs(scaleFactor) + 1);

  scaled = value + (int)(scaleFactor * (abs((float)value) / 8.0));


  // Serial.print("\n\rscaled ");
  // Serial.println(scaled);

  if (scaled < -94) {
    scaled = -94;
    } else if (scaled > 400) {
      scaled = 400;
      }


    return scaled;


  }

// Mark to run from RAM to avoid flash contention during saves
void __not_in_flash_func(lightUpRail)(int logo, int rail, int onOff, int brightness2,
                 int switchPosition) {


  lightUpHeader();
  for (int j = 0; j < 4; j++) {
    if (j == rail || rail == -1) {


      uint32_t color = railColorsV5[j][0];



      for (int i = 0; i < 25; i++) {

        if (onOff == 1) {
          // uint32_t color = packRgb((railColors[j].r * brightness2) >> 8,
          // (railColors[j].g * brightness2) >> 8, (railColors[j].b *
          // brightness2) >> 8);

          /// Serial.println(color,HEX);
          // Serial.print("brightenedRail ");
          // Serial.println(brightenedRail);
          if (brightenedRail == j || ((j == 1 || j == 3) && (brightenedRail == 1 || brightenedRail == 3))) { //this is just when the rails are highlighted
            // color = scaleBrightness(color, brightness2);
            if (j % 2 == 0) { //only the top and bottom rails 
              int powerRail = j / 2;
              float currentRailVoltage = (powerRail == 0) ? globalState.power.topRail : globalState.power.bottomRail;

              if (currentRailVoltage < 0.0) { //flipped when the voltage is negative
                if ((i == 24 - (abs((int)(currentRailVoltage * 5)))) && (abs((int)currentRailVoltage) <= 5.0)) {

                  leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dotColor, 250));

                  } else if (i == 49 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) >= 5.0)) {

                    leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dangerDot, 280));


                    } else if (i > 24 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) < 5.0)) {

                      leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][3], scaleScale(0)));

                      } else if (i > 49 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) >= 5.0)) {

                        leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][2], scaleScale(30)));

                        } else if (i < 49 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) >= 5.0)) {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][3], scaleScale(0)));

                          } else {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][3], scaleScale(-40)));

                          }



                } else if ((i == abs((int)((currentRailVoltage - 0.1) * 5))) && (abs((int)currentRailVoltage) <= 5.0)) {

                  leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dotColor, scaleScale(250)));

                  } else if ((i == (abs((int)((currentRailVoltage - 5.1) * 5)))) && (abs((int)currentRailVoltage) >= 5.0)) {

                    leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dangerDot, scaleScale(280)));

                    } else if (i < abs((int)(((currentRailVoltage + 0.1) * 5) - 1)) && (abs((int)currentRailVoltage) < 5.0)) {

                      leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][3], scaleScale(15)));

                      } else if (i < (abs((int)(((currentRailVoltage - 4.9) * 5) - 1))) && (abs(currentRailVoltage) >= 5.0)) {

                        leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][2], scaleScale(30)));
                        } else if (i > (abs((int)(((currentRailVoltage - 4.9) * 5) - 1))) && (abs(currentRailVoltage) >= 5.0)) {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][3], scaleScale(15)));

                          } else {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][1], scaleScale(-40)));
                          }
              } else {
              leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][1], scaleScale(-50)));
              }

            } else { //this is when the rails are not highlighted
            // Serial.println("brightness2");
            // Serial.println(brightness2);
            if (j % 2 == 0) { //only the top and bottom rails 
              int powerRail = j / 2;
              float currentRailVoltage = (powerRail == 0) ? globalState.power.topRail : globalState.power.bottomRail;

              if (currentRailVoltage < -0.1) { //flipped when the voltage is negative
                if ((i == 25 - (abs((int)(currentRailVoltage * 5)))) && (abs((int)currentRailVoltage) < 5.0)) {

                  leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(negDot, 250));

                  } else if (i == 50 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) >= 5.0)) {

                    leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dangerDot, 250));


                    } else if (i > 25 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) < 5.0)) {

                      leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][4], scaleScale(-20)));

                      } else if (i > 50 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) >= 5.0)) {

                        leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][2], scaleScale(-30)));

                        } else if (i < 50 - (abs((int)((currentRailVoltage * 5)))) && (abs(currentRailVoltage) >= 5.0)) {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][4], scaleScale(-20)));

                          } else {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][0], scaleScale(-85)));

                          }



                } else if ((i == abs((int)((currentRailVoltage - 0.1) * 5))) && (abs((int)currentRailVoltage) < 5.0)) {

                  leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dotColor, scaleScale(150)));

                  } else if ((i == (abs((int)((currentRailVoltage - 5.1) * 5)))) && (abs((int)currentRailVoltage) >= 5.0)) {

                    leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(dangerDot, scaleScale(250)));

                    } else if (i < abs((int)(((currentRailVoltage + 0.1) * 5) - 1)) && (abs((int)currentRailVoltage) < 5.0)) {

                      leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][1], scaleScale(-20)));

                      } else if (i < (abs((int)(((currentRailVoltage - 4.9) * 5) - 1))) && (abs(currentRailVoltage) >= 5.0)) {

                        leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][2], scaleScale(-30)));

                        } else if (i > (abs((int)(((currentRailVoltage - 4.9) * 5) - 1))) && (abs(currentRailVoltage) >= 5.0)) {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][1], scaleScale(-20)));

                          } else {

                          leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][0], scaleScale(-85)));
                          }
              } else {
              leds.setPixelColor(railsToPixelMap[j][i], scaleBrightness(railColorsV5[j][0], scaleScale(-80)));
              }
            }

          } else {
          leds.setPixelColor(railsToPixelMap[j][i], 0);
          }
        }
      }
    }


  if (lightUpName == true) {
    leds.setPixelColor(railsToPixelMap[0][20], 0x0010a0);
    leds.setPixelColor(railsToPixelMap[0][21], 0x1f0050);
    leds.setPixelColor(railsToPixelMap[0][22], 0x1d0030);
    leds.setPixelColor(railsToPixelMap[0][23], 0x1e0020);
    leds.setPixelColor(railsToPixelMap[0][24], 0x2f0010);

    leds.setPixelColor(railsToPixelMap[1][20], 0x0040f0);
    leds.setPixelColor(railsToPixelMap[1][21], 0x0f0050);
    leds.setPixelColor(railsToPixelMap[1][22], 0x0d0030);
    leds.setPixelColor(railsToPixelMap[1][23], 0x0e0020);
    leds.setPixelColor(railsToPixelMap[1][24], 0x2f0000);
    }
  // leds.show();
  // requestLedShow( 1 );
  // delay(3);
  }
//int displayMode = jumperlessConfig.display.lines_wires; // 0 = lines 1= wires

// Mark to run from RAM to avoid flash contention during saves
void __not_in_flash_func(showNets)(void) {
  // Serial.println(rp2040.cpuid());
  // core2busy = true;
  //                if (debugNTCC > 0) {
  //      Serial.println(debugNTCC);
  //    }
  //  if (rp2040.cpuid() == 0) {
  //    core1busy = true;
  //   } else {
  core2busy = true;
  int skipShow = 0;
  //}
  // if (debugNTCC > 0) {
  //   Serial.println(debugNTCC);
  // }
  //displayMode = jumperlessConfig.display.lines_wires;

    if (jumperlessConfig.display.lines_wires == 0 || numberOfShownNets > MAX_NETS_FOR_WIRES) {
    assignNetColors();
    for (int i = 0; i <= numberOfNets; i++) {
      // Serial.print(i);
      skipShow = 0;
      for (int j = 0; j < 5; j++) {
        if (showADCreadings[j] == i) {
          //skipShow = 1;
          continue;
          }
        for (int k = 0; k < 10; k++) {
          if (gpioNet[k] == i) {
            //skipShow = 1;
            continue;
            }
          }
        }
      if (skipShow == 1) {
        continue;
        }

      lightUpNet(i);
      }
      } else if (jumperlessConfig.display.lines_wires == 1) {
      // if (i==1){
      //   continue;
      // }
      drawWires();
      }
    //               if (debugNTCC > 0) {
    //   Serial.println(debugNTCC);
    // }
    // requestLedShow( 1 );
    //     if (rp2040.cpuid() == 0) {
    //   core1busy = false;
    //  } else {

#if defined(OG_JUMPERLESS)
    // OG nano-header + logo overlay (mirrors the stock OG firmware). Runs after
    // the nets are drawn so it only fills what a net did NOT light, and after
    // logoSwirl() (which ran earlier this frame) so the swirl buffer is current.
    {
      const int kOgNanoHeaderFirst = 80;
      const int kOgNanoHeaderLast  = 109;
      const int kOgLogoPixel       = 110;
      const uint32_t kOgHeaderGlow = 0x0C0028; // dim purple "headerglow"

      // 1) Every still-unlit nano-header LED gets a dim purple glow so the
      //    header is always faintly visible.
      for (int p = kOgNanoHeaderFirst; p <= kOgNanoHeaderLast; p++) {
        if (leds.getPixelColor(p) == 0) {
          leds.setPixelColor(p, kOgHeaderGlow);
        }
      }

      // 2) Hardwired power/ground/reset header pins are physically tied to the
      //    Arduino's rails, so always light them in a recognizable color. Pixel
      //    layout confirmed from the OG header LED order: top row 80-94 =
      //    D1,D0,RST,GND,D2..D12; bottom row 95-109 = D13,3V3,AREF,A0..A7,5V,
      //    RST,GND,VIN. These are painted UNCONDITIONALLY: the bottom-right pins
      //    (5V/RST/GND/VIN, pixels 106-109) are the nodesToPixelMap targets of
      //    the RESET/GND nodes, so lightUpNet() pre-paints them and a
      //    "only-if-unlit" check would skip them (the "just blue" bug). They are
      //    never part of a user-routable net, so overwriting is always correct.
      struct OgHardwiredPin { int pixel; uint32_t color; };
      static const OgHardwiredPin kOgHardwired[] = {
          { 83,  0x00C030 }, // GND (top)    - green
          { 108, 0x00C030 }, // GND (bottom) - green
          { 96,  0xC05A00 }, // 3V3          - amber
          { 106, 0xC01400 }, // 5V           - red-orange
          { 109, 0xC00010 }, // VIN          - deep red
          { 82,  0x3000B0 }, // RST (top)    - blue/violet
          { 107, 0x3000B0 }, // RST (bottom) - blue/violet
          { 97,  0x008060 }, // AREF         - teal
      };
      for (const auto& hp : kOgHardwired) {
        leds.setPixelColor(hp.pixel, hp.color);
      }

      // 3) Breadboard power rails. The shared lightUpRail() renders into the V5
      //    railsToPixelMap (pixels 300-399), which are off the 111-px OG strip,
      //    so the physical OG rails never lit. Mirror each rail's color onto its
      //    OG pixels. Positions from the OG hardware LED order (top+/top-/bot+/
      //    bot-); railColorsV5[j][1] is the per-rail "bright" color, scaled up
      //    since the raw values are tuned dim for V5's 5-LEDs-per-rail-segment.
      static const int kOgRailPixels[4][5] = {
          { 70, 73, 74, 77, 78 }, // rail 0: top positive
          { 71, 72, 75, 76, 79 }, // rail 1: top negative
          { 69, 66, 65, 62, 61 }, // rail 2: bottom positive
          { 68, 67, 64, 63, 60 }, // rail 3: bottom negative
      };
      for (int j = 0; j < 4; j++) {
        // Use the dim rail variant directly (no scale-up): the rails read too
        // bright otherwise on the OG's single LED per rail segment.
        uint32_t railColor = railColorsV5[j][0];
        for (int i = 0; i < 5; i++) {
          // Only fill unlit rail pixels: a rail that is part of a net was
          // already painted its net color by lightUpNet() (pixels 60/61 are the
          // nodesToPixelMap targets of the rail nodes), so leave those alone.
          if (leds.getPixelColor(kOgRailPixels[j][i]) == 0) {
            leds.setPixelColor(kOgRailPixels[j][i], railColor);
          }
        }
      }

      // 4) Logo is a SINGLE LED (pixel 110). The shared V5 swirl writes 7 LEDs
      //    (LOGO_LED_START..+6), each at a different point on the rainbow --
      //    those buffer slots are off the 111-px OG strip, and averaging them
      //    would wash out to grey. Sample ONE (LOGO_LED_START+0, which logoSwirl
      //    cycles through the whole spectrum over time) and brighten it (the
      //    swirl runs dim because V5 clusters 7 LEDs; the OG shows just one).
      leds.setPixelColor(kOgLogoPixel,
                         scaleUpBrightness(leds.getPixelColor(LOGO_LED_START + 0), 12, 0x90));
    }
#endif

    core2busy = false;
    // }
  }

rgbColor HsvToRgb(hsvColor hsv) {
  rgbColor rgb;
  unsigned char region, p, q, t;
  unsigned int h, s, v, remainder;

  if (hsv.s == 0) {
    rgb.r = hsv.v;
    rgb.g = hsv.v;
    rgb.b = hsv.v;
    return rgb;
    }

  // converting to 16 bit to prevent overflow
  h = hsv.h;
  s = hsv.s;
  v = hsv.v;

  region = h / 43;
  remainder = (h - (region * 43)) * 6;

  p = (v * (255 - s)) >> 8;
  q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

  switch (region) {
    case 0:
      rgb.r = v;
      rgb.g = t;
      rgb.b = p;
      break;
    case 1:
      rgb.r = q;
      rgb.g = v;
      rgb.b = p;
      break;
    case 2:
      rgb.r = p;
      rgb.g = v;
      rgb.b = t;
      break;
    case 3:
      rgb.r = p;
      rgb.g = q;
      rgb.b = v;
      break;
    case 4:
      rgb.r = t;
      rgb.g = p;
      rgb.b = v;
      break;
    default:
      rgb.r = v;
      rgb.g = p;
      rgb.b = q;
      break;
    }

  return rgb;
  }

uint32_t HsvToRaw(hsvColor hsv) {
  rgbColor rgb = HsvToRgb(hsv);
  return rgb.r << 16 | rgb.g << 8 | rgb.b;
  }

hsvColor RgbToHsv(rgbColor rgb) {
  hsvColor hsv;
  unsigned char rgbMin, rgbMax;

  rgbMin = rgb.r < rgb.g ? (rgb.r < rgb.b ? rgb.r : rgb.b)
    : (rgb.g < rgb.b ? rgb.g : rgb.b);
  rgbMax = rgb.r > rgb.g ? (rgb.r > rgb.b ? rgb.r : rgb.b)
    : (rgb.g > rgb.b ? rgb.g : rgb.b);

  hsv.v = rgbMax;
  if (hsv.v == 0) {
    hsv.h = 0;
    hsv.s = 0;
    return hsv;
    }

  hsv.s = 255 * ((long)(rgbMax - rgbMin)) / hsv.v;
  if (hsv.s == 0) {
    hsv.h = 0;
    return hsv;
    }

  if (rgbMax == rgb.r)
    hsv.h = 0 + 43 * (rgb.g - rgb.b) / (rgbMax - rgbMin);
  else if (rgbMax == rgb.g)
    hsv.h = 85 + 43 * (rgb.b - rgb.r) / (rgbMax - rgbMin);
  else
    hsv.h = 171 + 43 * (rgb.r - rgb.g) / (rgbMax - rgbMin);

  return hsv;
  }

hsvColor RgbToHsv(uint32_t color) {
  rgbColor rgb;
  rgb.r = (color >> 16) & 0xFF;
  rgb.g = (color >> 8) & 0xFF;
  rgb.b = color & 0xFF;
  return RgbToHsv(rgb);
  }

void randomColors(void) {

  int count = 0;
  uint32_t color = 0;
  for (int i = 0; i < 445; i++) {

    count = random(0, 25);
    // if (i > 80)
    // {
    //     count = random(0, 22);
    // }

    byte colorValR = random(0, 52);
    byte colorValG = random(0, 52);
    byte colorValB = random(0, 52);

    color = colorValR << 16 | colorValG << 8 | colorValB;
    switch (count) {
      case 0:
        color = color & 0x00ffff;
        break;
      case 1:
        color = color & 0xff00ff;
        break;
      case 2:
        color = color & 0xffff00;
        break;
      case 3:
        color = color & 0x0000ff;
        break;
      case 4:
        color = color & 0x00ff00;
        break;
      case 5:
        color = color & 0xff0000;
        break;
      case 6:
        color = color & 0x000f0f;
        break;
      case 7:
        color = color & 0x0f000f;
        break;
      case 8:
        color = color & 0x0f0f00;
        break;
      case 9:
        color = color & 0x0000f0;
        break;
      case 10:
        color = color & 0x00f000;
        break;
      case 11:
        color = color & 0xf00000;
        break;
      case 12:
        color = color & 0xaaaaaa;
        break;
      case 13:
        color = color & 0x555555;
        break;
      case 14:
        break;


      default:
        color = color & 0x000000;
        break;
      }
    // color = color | (color >> 1);

    leds.setPixelColor(i, color); //  Set pixel's color (in RAM)
    // lightUpRail(-1, -1, 1, LEDbrightnessRail);
    requestLedShow( 3 ); //  Update strip to match
    //  Pause for a moment
    }
  // delay(500);
 // delay(wait);
  }

void rainbowy(int saturation, int brightness, int wait) {

  hsvColor hsv;
  int bounce = 0;
  int offset = 1;

  for (long j = 0; j < 60; j += 1) {

    for (int i = 0; i < LED_COUNT; i++) {
      float huef;
      float i2 = i;
      float j2 = j;

      // huef = ((i * j)) * 0.1f; //*254.1;
      //  hsv.h = (j*(i*j))%255;
      // hsv.h = (j*(int((sin(j+i)*4))))%254;
      //
      huef = sinf((i2 * (j2)) * 3.0f); //*(sinf((j2*i2))*19.0);
      hsv.h = ((int)(huef)) % 254;

      hsv.s = 254;
      hsv.v = 80;
      rgbColor rgb = HsvToRgb(hsv);
      uint32_t rgbPacked = packRgb(rgb.r, rgb.g, rgb.b);
      // rgbPacked = rgbPacked * i
      leds.setPixelColor((i + offset) % LED_COUNT, rgbPacked);
      }

    offset += 1;
    // offset = offset % 80;
    requestLedShow( 1 );
    delayMicroseconds((wait * 1000)); //*((j/20.0)));
    }
  }

void startupColors(void) {
  hsvColor hsv;
  int bounce = 0;
  int offset = 1;
  int fade = 0;
  int done = 0;
  // int logoColor = 66;
  // int logoStep = 255/66;

  for (long j = 4; j < 162; j += 2) {
    if (j < DEFAULTBRIGHTNESS / 3) {
      fade = j * 3;
      } else {
      int fadeout = j - DEFAULTBRIGHTNESS;
      if (fadeout < 0) {
        fadeout = 0;
        }
      if (fadeout > DEFAULTBRIGHTNESS) {
        fadeout = DEFAULTBRIGHTNESS;
        done = 1;
        // Serial.println(j);
        //  break;
        }
      fade = DEFAULTBRIGHTNESS - fadeout;
      }

    for (int i = 0; i < LED_COUNT; i++) {
      float huef;
      float i2 = i;
      float j2 = j + 50;

      huef = ((i2 * j2)) * 0.1f; //*254.1;
      // hsv.h = (j*(i*j))%255;
      // hsv.h = (j*(int((sin(j+i)*4))))%254;
      hsv.h = ((int)(huef)) % 254;
      hsv.s = 254;
      // if (((i + offset) % LED_COUNT) == 110)
      // {
      //     hsv.v = 85;
      //     hsv.h = (189 + j);
      // }
      // else
      // {

      //     hsv.v = fade;
      // }
      rgbColor rgb = HsvToRgb(hsv);
      uint32_t rgbPacked = packRgb(rgb.r, rgb.g, rgb.b);
      // rgbPacked = rgbPacked * i

      leds.setPixelColor((i + offset) % LED_COUNT, rgbPacked);
      }

    offset += 1;
    // offset = offset % 80;
    // lightUpRail(1);
    // requestLedShow( 1 );
    leds.show();
    if (done == 0) {
      delayMicroseconds((14000)); //*((j/20.0)));
      } else {
      break;
      }
    // Serial.println(j);
    }
  // clearLEDs();
  //  lightUpRail();
  //   requestLedShow( 1 );
  }

// Fixed capacity sized to its initializer (was LED_COUNT+200 = 500 on V5). Kept
// at 500 regardless of board so the OG's smaller LED_COUNT doesn't truncate the
// initializer; the animation only ever indexes up to LED_COUNT (<= 500).
uint32_t chillinColors[500] = {
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x050003,
    0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003,
    0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003,
    0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003,
    0x050003, 0x050003, 0x050003, 0x000500, 0x000500, 0x000500, 0x000500,
    0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500,
    0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500,
    0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500,
    0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003,
    0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003,
    0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003, 0x050003,
    0x050003, 0x050003, 0x050003, 0x050003, 0x000500, 0x000500, 0x000500,
    0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500,
    0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500,
    0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500, 0x000500,
    0x000500, 0x010006, 0x010006, 0x010006, 0x001C04, 0x010006, 0x010006,
    0x010006, 0x010006, 0x010006, 0x010006, 0x010006, 0x010006, 0x010006,
    0x010006, 0x010006, 0x010006, 0x1C0107, 0x010006, 0x010006, 0x010006,
    0x010006, 0x010006, 0x010006, 0x010006, 0x010006, 0x010006, 0x1C0702,
    0x010006, 0x001C04, 0x1C0702, 0x380303, 0x380303, 0x253803, 0x253803,
    0x0000E5, 0x0000E5, 0x07000F, 0x030015, 0x000015, 0x000015, 0x010815,
    0x050E03, 0x07000F, 0x030015, 0x000000 };

void startupColorsV5(void) {
  int logo = 1;

  hsvColor hsv;
  int bounce = 0;

  long jStart = 250;
  for (long j = 250; j >= 0; j -= 1) {

    for (int i = 0; i < LED_COUNT - 9; i++) {
      float huef;

      float i2 = i;
      float j2 = j;

      if (i >= 300) {
        i2 = (LED_COUNT - 9) - i;
        j2 = jStart - j;
        }

      huef = sinf((i2 / (j2)) * 2.0f); //*(sinf((j2/i2))*19.0);
      // hsv.h = (j*(i*j))%255;
      // hsv.h = (j*(int((sin(j+i)*4))))%254;
      hsv.h = ((int)(huef * 255)) % 254;
      hsv.s = 254;
      if (i <= 400) {
        hsv.v = 6;
        } else {
        hsv.v = 16;
        }
      rgbColor rgb = HsvToRgb(hsv);
      if (i >= 300) {

        rgbColor chillin = unpackRgb(chillinColors[i]);
        rgb.r = (((rgb.r) * (j)) + (chillin.r) * ((jStart - j))) / (jStart);
        rgb.g = (((rgb.g) * (j)) + (chillin.g) * ((jStart - j))) / (jStart);
        rgb.b = (((rgb.b) * (j)) + (chillin.b) * ((jStart - j))) / (jStart);
        } else if (j < 60) {
          rgb.r = ((rgb.r * j)) / 60;
          rgb.g = ((rgb.g * j)) / 60;
          rgb.b = ((rgb.b * j)) / 60;

          //   rgb.g = rgb.g;
          //   rgb.b = rgb.b;
          }
        //   rgb.r = (rgb.r + chillin.r) / 2;

        //     rgb.g = (rgb.g + chillin.g) / 2;

        //     rgb.b = (rgb.b + chillin.b) / 2;

        //   rgb.r = ((rgb.r * (jStart - j)) + (chillin.r * j) / jStart) / 2;

        //   rgb.g = ((rgb.g * j) + (chillin.g * (jStart - j)) / jStart) / 2;

        //   rgb.b = ((rgb.b * j) + (chillin.b * (jStart - j)) / jStart) / 2;

        uint32_t rgbPacked = packRgb(rgb.r, rgb.g, rgb.b);

        // rgbPacked = rgbPacked * i
        if (i < 300 && logo == 1) {

          if (jumperlessText[textMap[i]] == 0) {
            leds.setPixelColor(i, rgbPacked);
            } else {
            leds.setPixelColor(i, 0);
            }
          } else {
          leds.setPixelColor(i, rgbPacked);
          }
        // leds.setPixelColor(i, rgbPacked);
      }
    // leds.setPixelColor(399, 0x151515);

    requestLedShow( 3 );
    delayMicroseconds((12));
    }
  // lightUpRail();
  requestLedShow( 1 );
  // delay(1000);
  }
void rainbowBounce(int wait, int logo) {
  hsvColor hsv;
  int bounce = 0;
  // leds.clear();
  for (long j = 0; j < 140; j += 1) {

    for (int i = 0; i < LED_COUNT - 9; i++) {
      float huef;
      float i2 = i;
      float j2 = j;

      huef = sinf((i2 / (j2)) * 1.5f); //*(sinf((j2/i2))*19.0);
      // hsv.h = (j*(i*j))%255;
      // hsv.h = (j*(int((sin(j+i)*4))))%254;
      hsv.h = ((int)(huef * 255)) % 254;
      hsv.s = 254;
      if (i < 400) {
        hsv.v = 6;
        } else {
        hsv.v = 16;
        }
      // hsv.v = 6;
      rgbColor rgb = HsvToRgb(hsv);
      uint32_t rgbPacked = packRgb(rgb.r, rgb.g, rgb.b);
      // rgbPacked = rgbPacked * i
      if (i < 300 && logo == 1) {
        if (jumperlessText[textMap[i]] == 0) {
          leds.setPixelColor(i, rgbPacked);
          } else {
          leds.setPixelColor(i, 0);
          }
        } else {
        leds.setPixelColor(i, rgbPacked);
        }
      }

    requestLedShow( 3 );
    delayMicroseconds((wait * 1000) * ((j / 20.0)));
    }
  for (long j = 140; j >= 0; j -= 1) {

    for (int i = 0; i < LED_COUNT - 9; i++) {
      float huef;
      float i2 = i;
      float j2 = j;

      huef = sinf((i2 / (j2)) * 1.5f); //*(sinf((j2/i2))*19.0);
      // hsv.h = (j*(i*j))%255;
      // hsv.h = (j*(int((sin(j+i)*4))))%254;
      hsv.h = ((int)(huef * 255)) % 254;
      hsv.s = 254;
      if (i < 400) {
        hsv.v = 6;
        } else {
        hsv.v = 16;
        }
      rgbColor rgb = HsvToRgb(hsv);
      uint32_t rgbPacked = packRgb(rgb.r, rgb.g, rgb.b);
      // rgbPacked = rgbPacked * i
      if (i < 300 && logo == 1) {
        if (jumperlessText[textMap[i]] == 0) {
          leds.setPixelColor(i, rgbPacked);
          } else {
          leds.setPixelColor(i, 0);
          }
        } else {
        leds.setPixelColor(i, rgbPacked);
        }
      // leds.setPixelColor(i, rgbPacked);
      }

    requestLedShow( 3 );
    delayMicroseconds((wait * 1000) * ((j / 20.0)));
    }
  }

struct rgbColor unpackRgb(uint32_t color) {
  struct rgbColor rgb;
  rgb.r = (color >> 16) & 0xFF;
  rgb.g = (color >> 8) & 0xFF;
  rgb.b = color & 0xFF;
  /* Serial.print("r: ");
   Serial.print(rgb.r);
   Serial.print(" g: ");
   Serial.print(rgb.g);
   Serial.print(" b: ");
   Serial.println(rgb.b);*/
  return rgb;
  }

// uint32_t packRgb(uint8_t r, uint8_t g, uint8_t b) {
//   return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
// }
uint32_t packRgb(rgbColor rgb) {
  return ((uint32_t)rgb.r << 16) | ((uint32_t)rgb.g << 8) | rgb.b;
  }

uint32_t packRgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }

void clearLEDs(void) {
  for (int i = 0; i <= 436; i++) { // For each pixel in strip...

    leds.setPixelColor(i, 0); //  Set pixel's color (in RAM)
    //  Update strip to match
    // requestLedShow( 1 );
    }
  // lightUpRail();
  //  for (int i = 80; i <= 109; i++)
  //  { // For each pixel in strip...
  //      if (nodesToPixelMap[i] > NANO_D0 && nodesToPixelMap[i] < NANO_A7)
  //      {
  //          leds.setPixelColor(i, 0); //  Set pixel's color (in RAM)
  //      }
  //      // leds.setPixelColor(i, 0); //  Set pixel's color (in RAM)
  //      //  Update strip to match
  //  }

  // requestLedShow( 1 );
  }


// Mark to run from RAM to avoid flash contention during saves
// CRITICAL: Use setPixelColorDirect to write to buffer WITHOUT marking dirty
// This prevents Core 2 from showing partial clears before new content is drawn
void __not_in_flash_func(clearLEDsExceptRails)(void) {
  for (int i = 0; i < 300; i++) {
    leds.setPixelColorDirect(i, 0);
  }

  for (int i = 400; i < 430; i++) {
    if (i != 403 && i != 402 && i != 428 && i != 429 && i != 416 && i != 426 &&
        i != 427) {
      leds.setPixelColorDirect(i, 0);
    }
  }
  // Note: Buffer is cleared but NOT marked dirty
  // Call showLEDsCore2 = 12 (or similar) to actually display
}

void clearLEDsMiddle(int start, int end) {
  for (int i = start - 1; i < end; i++) {
    if (i > 60) {
      return;
    }
    leds.setPixelColorDirect(i * 5 + 2, 0);  // No dirty marking
  }
}

void clearLEDsExceptMiddle(int start, int end) {

  if (end != -1) {
    for (int i = start; i < end; i++) {
      leds.setPixelColorDirect(i * 5, 0);      // No dirty marking
      leds.setPixelColorDirect(i * 5 + 1, 0);  // No dirty marking
      leds.setPixelColorDirect(i * 5 + 3, 0);  // No dirty marking
      leds.setPixelColorDirect(i * 5 + 4, 0);  // No dirty marking
    }
  } else {
    if (start > 0 && start <= 60)
      start = start - 1;
    leds.setPixelColorDirect(start * 5, 0);      // No dirty marking
    leds.setPixelColorDirect(start * 5 + 1, 0);  // No dirty marking
    leds.setPixelColorDirect(start * 5 + 3, 0);  // No dirty marking
    leds.setPixelColorDirect(start * 5 + 4, 0);  // No dirty marking
  }
}

// char* colorToName(uint32_t color, int length)
// {
//     int numColors = sizeof(colorNames) / sizeof(colorNames[0]);
//     rgbColor input = unpackRgb(color);
//     // Only return black if the color is exactly 0x000000
//     if (color == 0x000000) {
//         const char* black = "black";
//         strncpy(colorNameBuffer, black, strlen(black));
//         colorNameBuffer[strlen(black)] = '\0';
//         return colorNameBuffer;
//     }
//     // Return white if all channels are equal (and not zero)
//     if (input.r == input.g && input.g == input.b && input.r != 0) {
//         const char* white = "white";
//         strncpy(colorNameBuffer, white, strlen(white));
//         colorNameBuffer[strlen(white)] = '\0';
//         return colorNameBuffer;
//     }
//     // Convert input to HSV
//     hsvColor inputHsv = RgbToHsv(input);
//     int minDist = 0x7FFFFFFF;
//     int minIdx = 0;
//     for (int i = 0; i < numColors; i++) {
//         rgbColor refRgb = unpackRgb(colorNames[i].color);
//         hsvColor refHsv = RgbToHsv(refRgb);
//         // Compare hue and saturation only
//         int dh = (int)inputHsv.h - (int)refHsv.h;
//         int ds = (int)inputHsv.s - (int)refHsv.s;
//         int dist = dh * dh + ds * ds;
//         if (dist < minDist) {
//             minDist = dist;
//             minIdx = i;
//         }
//     }
//     const char* src = colorNames[minIdx].name;
//     int len = strlen(src);
//     if (length == -1) {
//         // Trim trailing spaces only
//         int end = len - 1;
//         while (end >= 0 && src[end] == ' ') end--;
//         int trimmedLen = end + 1;
//         strncpy(colorNameBuffer, src, trimmedLen);
//         colorNameBuffer[trimmedLen] = '\0';
//         return colorNameBuffer;
//     } else {
//         int padLen = length > len ? length : len;
//         memset(colorNameBuffer, ' ', padLen);
//         strncpy(colorNameBuffer, src, padLen);
//         colorNameBuffer[padLen] = '\0';
//         return colorNameBuffer;
//     }
// }

// Function to parse range from Serial and display color names
void checkSerialForColorRange() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    // Look for a dash character
    int dashIndex = input.indexOf('-');
    if (dashIndex > 0 && dashIndex < input.length() - 1) {
      // Get the numbers before and after the dash
      String startStr = input.substring(0, dashIndex);
      String endStr = input.substring(dashIndex + 1);

      // Convert to integers
      int rangeStart = startStr.toInt();
      int rangeEnd = endStr.toInt();

      // Validate the range
      if (rangeStart >= 0 && rangeStart <= 255 && rangeEnd >= 0 && rangeEnd <= 255) {
        Serial.print("Displaying colors for range: ");
        Serial.print(rangeStart);
        Serial.print("-");
        Serial.println(rangeEnd);

        // Call the demo function with the parsed range
        colorPicker();
        } else {
        Serial.println("Invalid range. Please use format: 0-255 (values between 0-255)");
        }
      } else {
      // No dash found or invalid format
      Serial.println("Invalid format. Please use: start-end (e.g., 0-255)");
      }
    }
  }


