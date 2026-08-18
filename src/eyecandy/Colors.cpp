// SPDX-License-Identifier: MIT
#include "Colors.h"
#include "LEDs.h"
#include <Arduino.h>


// Buffer for color name conversion
static char colorNameBuffer[32];

// ============================================================================
// Wokwi Color Definitions
// ============================================================================

/**
 * @brief Complete Wokwi color palette
 * Based on: https://docs.wokwi.com/guides/diagram-editor
 * 
 * All Wokwi colors are mapped to unique internal colors for proper distinction
 */
const WokwiColor wokwiColors[] = {
    // Standard resistor color code + extensions
    {"black",     0x808080, "grey",       '0'},  // Black → Gray (LEDs can't show black!)
    {"brown",     0xA52A2A, "orange",      '1'},  // Brown
    {"#8f4814",     0xA52A2A, "orange",      '1'},  // Brown idk wokwi uses this for some reason
    {"red",       0xFF0000, "red",        '2'},  // Red
    {"orange",    0xFFA500, "orange",     '3'},  // Orange
    {"gold",      0xFFA500, "amber",     '4'},  // Gold → Orange (similar hue, LEDs)
    {"green",     0x00FF00, "green",      '5'},  // Green (pure green)
    {"blue",      0x0000FF, "blue",       '6'},  // Blue
    {"violet",    0x8A2BE2, "violet",     '7'},  // Violet (blue-violet)
    {"gray",      0x808080, "grey",       '8'},  // Gray
    {"white",     0xAAAAAA, "white",      '9'},  // White
    
    // Extended colors
    {"cyan",      0x00FFFF, "cyan",       'C'},  // Cyan
    {"limegreen", 0x32CD32, "chartreuse", 'L'},  // Limegreen → Chartreuse (yellow-green)
    {"magenta",   0xFF00FF, "magenta",    'M'},  // Magenta
    {"purple",    0x800080, "purple",     'P'},  // Purple (different from violet)
    {"yellow",    0xFFFF00, "yellow",     'Y'},  // Yellow
    
    // Aliases for compatibility
    {"pink",      0xFF00FF, "magenta",    ' '},  // Pink → Magenta
    {"grey",      0x808080, "grey",       ' '},  // Grey (alternate spelling)
};

const int wokwiColorCount = sizeof(wokwiColors) / sizeof(wokwiColors[0]);

uint32_t wokwiColorToRGB(const String& colorName) {
    String color = colorName;
    color.toLowerCase();
    color.trim();
    
    // Search for exact match
    for (int i = 0; i < wokwiColorCount; i++) {
        if (color == wokwiColors[i].name) {
            // Scale down to 3% brightness for dimmer wire appearance
            return scaleBrightness(wokwiColors[i].rgb, -97);
        }
    }
    
    // Default to white if unknown
    return 0x0a0a0a;
}

String wokwiColorToInternalName(const String& wokwiColor) {
    String color = wokwiColor;
    color.toLowerCase();
    color.trim();
    
    // Search for match
    for (int i = 0; i < wokwiColorCount; i++) {
        if (color == wokwiColors[i].name) {
            return String(wokwiColors[i].internalName);
        }
    }
    
    return "white     "; // Default
}

String rgbToWokwiColorName(uint32_t rgb) {
    // Search for exact RGB match in Wokwi colors
    // Prefer primary names (with keyboard shortcuts) over aliases
    String foundName = "";
    char foundShortcut = ' ';
    
    for (int i = 0; i < wokwiColorCount; i++) {
        if (wokwiColors[i].rgb == rgb) {
            // If this has a keyboard shortcut, it's a primary color - use it immediately
            if (wokwiColors[i].keyboardShortcut != ' ') {
                return String(wokwiColors[i].name);
            }
            // Otherwise save it as a fallback (alias)
            if (foundName.length() == 0) {
                foundName = String(wokwiColors[i].name);
            }
        }
    }
    
    // Return alias if that's all we found
    if (foundName.length() > 0) {
        return foundName;
    }
    
    // No exact match - fall back to closest palette color
    char* name = colorToName(rgb, -1);
    if (name != nullptr) {
        String result = String(name);
        result.trim();
        return result;
    }
    return "white     ";
}

// ============================================================================
// Internal Color Palette
// ============================================================================

/**
 * @brief Reference palette for internal color management
 * Includes full brightness reference, dim color for matching, and terminal codes
 */
const NamedColor namedColors[] = {
    {0xFF0000, 0x400000, "red       ", 253, 12, 196, 31},  // Red wraps around 0
    {0xFFA500, 0x401000, "orange    ", 13, 28, 208, 91},
    {0xFFBF00, 0x403000, "amber     ", 29, 35, 214, 33},
    {0xFFFF00, 0x404000, "yellow    ", 36, 60, 226, 93},
    {0x7FFF00, 0x104000, "chartreuse", 61, 72, 154, 92},
    {0x00FF00, 0x003000, "green     ", 73, 94, 82, 32},
    {0x2E8B57, 0x042040, "seafoam   ", 95, 109, 84, 96},
    {0x00FFFF, 0x004040, "cyan      ", 110, 135, 86, 96},
    {0x0000FF, 0x000040, "blue      ", 136, 164, 33, 36},
    {0x4169E1, 0x050040, "royal blue", 165, 175, 27, 34},
    {0x8A2BE2, 0x100040, "indigo    ", 176, 190, 21, 34},
    {0x800080, 0x200040, "violet    ", 191, 205, 57, 35},
    {0x800080, 0x200040, "purple    ", 206, 215, 12, 35},
    {0xFFC0CB, 0x400010, "pink      ", 216, 235, 164, 95},
    {0xFF00FF, 0x400020, "magenta   ", 236, 252, 198, 95},
    {0xA52A2A, 0x200000, "brown     ", 0, 0, 130, 31},     // Brown (for Wokwi)
    {0xFFFFFF, 0x404040, "white     ", 0, 0, 15, 97},      // Special case
    {0x000000, 0x000000, "black     ", 0, 0, 0, 30},       // Special case
    {0x808080, 0x202020, "grey      ", 0, 0, 8, 37}        // Special case
};

const int namedColorsCount = sizeof(namedColors) / sizeof(namedColors[0]);

// ============================================================================
// High Saturation Spectrum Colors (for terminal cycling)
// ============================================================================

const int highSaturationSpectrumColors[51] = {
    // Red hues (0-30°)
    160, 196, 202, 166,
    // Orange hues (30-60°)
    208, 214, 178, 220,
    // Yellow hues (60-90°)
    184, 226, 190, 148, 154, 112, 118,
    // Yellow-Green hues (90-120°)
    76, 82,
    // Green hues (120-150°)
    40, 46, 47, 41,
    // Green-Cyan hues (150-180°)
    48, 42, 49, 43, 50,
    // Cyan hues (180-210°)
    44, 51, 45, 38, 39, 32, 33,
    // Cyan-Blue hues (210-240°)
    26, 27,
    // Blue hues (240-270°)
    63, 62,
    // Blue-Magenta hues (270-300°)
    93, 92, 129, 128, 165,
    // Magenta hues (300-330°)
    164, 201, 200, 163, 199, 162, 198,
    // Red-Magenta hues (330-360°)
    161, 197
};

const int highSaturationSpectrumColorsCount = 51;

const int highSaturationBrightColors[29] = {
    // Red hues (0-30°)
    196, 202,
    // Orange hues (30-60°)
    208, 214, 220,
    // Yellow hues (60-90°)
    226, 190, 154, 118,
    // Yellow-Green hues (90-120°)
    82,
    // Green hues (120-150°)
    46, 47,
    // Green-Cyan hues (150-180°)
    48, 49, 50,
    // Cyan hues (180-210°)
    51, 45, 39, 33,
    // Cyan-Blue hues (210-240°)
    27,
    // Blue hues (240-270°)
    63,
    // Blue-Magenta hues (270-300°)
    99, 129, 165,
    // Magenta hues (300-330°)
    201, 200, 199, 198,
    // Red-Magenta hues (330-360°)
    197
};

const int highSaturationBrightColorsCount = 29;

// ============================================================================
// Color Manipulation
// ============================================================================

uint32_t shiftColorHue(uint32_t baseColor, int shiftAmount) {
    // Extract RGB components
    uint8_t r = (baseColor >> 16) & 0xFF;
    uint8_t g = (baseColor >> 8) & 0xFF;
    uint8_t b = baseColor & 0xFF;
    
    // Convert to HSV
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    
    float maxc = max(max(rf, gf), bf);
    float minc = min(min(rf, gf), bf);
    float v = maxc;
    
    if (maxc == minc) {
        // Grayscale - can't shift hue
        return baseColor;
    }
    
    float s = (maxc - minc) / maxc;
    float rc = (maxc - rf) / (maxc - minc);
    float gc = (maxc - gf) / (maxc - minc);
    float bc = (maxc - bf) / (maxc - minc);
    
    float h;
    if (rf == maxc) {
        h = bc - gc;
    } else if (gf == maxc) {
        h = 2.0f + rc - bc;
    } else {
        h = 4.0f + gc - rc;
    }
    
    h = fmod(h / 6.0f, 1.0f);
    if (h < 0) h += 1.0f;
    
    // Shift hue by shiftAmount (scaled to 0-1 range)
    h = fmod(h + (shiftAmount / 255.0f), 1.0f);
    
    // Convert back to RGB
    int i = (int)(h * 6.0f);
    float f = (h * 6.0f) - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    
    float r2, g2, b2;
    switch (i % 6) {
        case 0: r2 = v; g2 = t; b2 = p; break;
        case 1: r2 = q; g2 = v; b2 = p; break;
        case 2: r2 = p; g2 = v; b2 = t; break;
        case 3: r2 = p; g2 = q; b2 = v; break;
        case 4: r2 = t; g2 = p; b2 = v; break;
        case 5: r2 = v; g2 = p; b2 = q; break;
        default: r2 = v; g2 = v; b2 = v; break;
    }
    
    uint8_t rOut = (uint8_t)(r2 * 255.0f);
    uint8_t gOut = (uint8_t)(g2 * 255.0f);
    uint8_t bOut = (uint8_t)(b2 * 255.0f);
    
    return ((uint32_t)rOut << 16) | ((uint32_t)gOut << 8) | bOut;
}

uint32_t blendColors(uint32_t color1, uint32_t color2, float ratio) {
    if (ratio <= 0.0f) return color1;
    if (ratio >= 1.0f) return color2;
    
    uint8_t r1 = (color1 >> 16) & 0xFF;
    uint8_t g1 = (color1 >> 8) & 0xFF;
    uint8_t b1 = color1 & 0xFF;
    
    uint8_t r2 = (color2 >> 16) & 0xFF;
    uint8_t g2 = (color2 >> 8) & 0xFF;
    uint8_t b2 = color2 & 0xFF;
    
    uint8_t r = (uint8_t)(r1 * (1.0f - ratio) + r2 * ratio);
    uint8_t g = (uint8_t)(g1 * (1.0f - ratio) + g2 * ratio);
    uint8_t b = (uint8_t)(b1 * (1.0f - ratio) + b2 * ratio);
    
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ============================================================================
// Color Naming
// ============================================================================

int closestPaletteHueIdx(int hue) {
    // First, try to find a direct match using the hue ranges
    for (int i = 0; i < namedColorsCount; i++) {
        // Skip special cases (white, black, grey)
        if (namedColors[i].hueStart == 0 && namedColors[i].hueEnd == 0) {
            continue;
        }
        
        // Handle normal range
        if (namedColors[i].hueStart < namedColors[i].hueEnd) {
            if (hue >= namedColors[i].hueStart && hue <= namedColors[i].hueEnd) {
                return i;
            }
        }
        // Handle wrapping range (e.g., red spans 250-10)
        else if (namedColors[i].hueStart > namedColors[i].hueEnd) {
            if (hue >= namedColors[i].hueStart || hue <= namedColors[i].hueEnd) {
                return i;
            }
        }
    }
    
    // If no direct match found, use the closest hue distance
    int minDist = 256;
    int minIdx = 0;
    // Only consider non-special colors (skip white, black, grey, brown)
    for (int i = 0; i < 14; i++) {
        // Find the center of the hue range for this color
        int centerHue;
        if (namedColors[i].hueStart < namedColors[i].hueEnd) {
            centerHue = (namedColors[i].hueStart + namedColors[i].hueEnd) / 2;
        } else {
            // Handle wrapping range (e.g., red spans 250-10)
            centerHue = (namedColors[i].hueStart + namedColors[i].hueEnd + 255) / 2;
            if (centerHue > 255) centerHue -= 255;
        }
        
        int dh = abs((int)hue - centerHue);
        if (dh > 127) dh = 255 - dh; // wrap around hue circle
        
        if (dh < minDist) {
            minDist = dh;
            minIdx = i;
        }
    }
    return minIdx;
}

char* colorToName(uint32_t color, int length) {
    rgbColor input = unpackRgb(color);
    
    // Only return black if the color is exactly 0x000000
    if (color == 0x000000) {
        const char* black = "black     ";
        strncpy(colorNameBuffer, black, strlen(black));
        colorNameBuffer[strlen(black)] = '\0';
        return colorNameBuffer;
    }
    
    // Return white if all channels are equal (and not zero)
    if (input.r == input.g && input.g == input.b && input.r != 0) {
        const char* white = "white     ";
        strncpy(colorNameBuffer, white, strlen(white));
        colorNameBuffer[strlen(white)] = '\0';
        return colorNameBuffer;
    }
    
    // Convert input to HSV
    hsvColor inputHsv = RgbToHsv(input);
    
    // Determine if color is dim (low brightness)
    bool isDim = inputHsv.v < 70;
    
    int minDist = 0x7FFFFFFF;
    int minIdx = 0;
    
    // Check if hue directly falls within a defined range
    bool foundRange = false;
    for (int i = 0; i < namedColorsCount; i++) {
        // Skip special cases (white, black, grey) if we have color information
        if (namedColors[i].hueStart == 0 && namedColors[i].hueEnd == 0) {
            if (inputHsv.s > 40 && inputHsv.v > 30) continue;
        }
        
        // Handle normal range
        if (namedColors[i].hueStart < namedColors[i].hueEnd) {
            if (inputHsv.h >= namedColors[i].hueStart && inputHsv.h <= namedColors[i].hueEnd) {
                minIdx = i;
                foundRange = true;
                break;
            }
        }
        // Handle wrapping range (e.g., red spans 253-12)
        else if (namedColors[i].hueStart > namedColors[i].hueEnd) {
            if (inputHsv.h >= namedColors[i].hueStart || inputHsv.h <= namedColors[i].hueEnd) {
                minIdx = i;
                foundRange = true;
                break;
            }
        }
    }
    
    // If no range match was found, fall back to distance calculation
    if (!foundRange) {
        for (int i = 0; i < namedColorsCount; i++) {
            uint32_t refColor;
            if (isDim) {
                refColor = namedColors[i].dimColor;
            } else {
                refColor = namedColors[i].color;
            }
            
            if (isDim) {
                rgbColor refRgb = unpackRgb(refColor);
                int dr = (int)input.r - (int)refRgb.r;
                int dg = (int)input.g - (int)refRgb.g;
                int db = (int)input.b - (int)refRgb.b;
                int dist = dr * dr + dg * dg + db * db;
                
                if (dist < minDist) {
                    minDist = dist;
                    minIdx = i;
                }
            } else {
                // Force brightness to max for matching to avoid brightness bias
                hsvColor compareHsv = inputHsv;
                compareHsv.v = 254;
                
                rgbColor refRgb = unpackRgb(refColor);
                hsvColor refHsv = RgbToHsv(refRgb);
                
                // Compare hue and saturation only
                int dh = (int)compareHsv.h - (int)refHsv.h;
                if (dh > 127) dh = 255 - dh;
                if (dh < -127) dh = 255 + dh;
                
                int ds = (int)compareHsv.s - (int)refHsv.s;
                int dist = dh * dh + ds * ds;
                
                if (dist < minDist) {
                    minDist = dist;
                    minIdx = i;
                }
            }
        }
    }
    
    const char* src = namedColors[minIdx].name;
    int len = strlen(src);
    if (length == -1) {
        // Trim trailing spaces only
        int end = len - 1;
        while (end >= 0 && src[end] == ' ') end--;
        int trimmedLen = end + 1;
        strncpy(colorNameBuffer, src, trimmedLen);
        colorNameBuffer[trimmedLen] = '\0';
        return colorNameBuffer;
    } else {
        int padLen = length > len ? length : len;
        memset(colorNameBuffer, ' ', padLen);
        strncpy(colorNameBuffer, src, padLen);
        colorNameBuffer[padLen] = '\0';
        return colorNameBuffer;
    }
}

char* colorToName(rgbColor color, int length) {
    return colorToName(packRgb(color.r, color.g, color.b), length);
}

char* colorToName(int hue, int length) {
    hue = (hue) % 255;
    if (hue < 0) return colorToName(0x000000, length);
    
    // Find the color for this hue using direct range matching
    for (int i = 0; i < namedColorsCount; i++) {
        // Skip special cases (white, black, grey)
        if (namedColors[i].hueStart == 0 && namedColors[i].hueEnd == 0) {
            continue;
        }
        
        // Handle normal range
        if (namedColors[i].hueStart < namedColors[i].hueEnd) {
            if (hue >= namedColors[i].hueStart && hue <= namedColors[i].hueEnd) {
                const char* src = namedColors[i].name;
                int len = strlen(src);
                if (length == -1) {
                    int end = len - 1;
                    while (end >= 0 && src[end] == ' ') end--;
                    int trimmedLen = end + 1;
                    strncpy(colorNameBuffer, src, trimmedLen);
                    colorNameBuffer[trimmedLen] = '\0';
                    return colorNameBuffer;
                } else {
                    int padLen = length > len ? length : len;
                    memset(colorNameBuffer, ' ', padLen);
                    strncpy(colorNameBuffer, src, padLen);
                    colorNameBuffer[padLen] = '\0';
                    return colorNameBuffer;
                }
            }
        }
        // Handle wrapping range
        else if (namedColors[i].hueStart > namedColors[i].hueEnd) {
            if (hue >= namedColors[i].hueStart || hue <= namedColors[i].hueEnd) {
                const char* src = namedColors[i].name;
                int len = strlen(src);
                if (length == -1) {
                    int end = len - 1;
                    while (end >= 0 && src[end] == ' ') end--;
                    int trimmedLen = end + 1;
                    strncpy(colorNameBuffer, src, trimmedLen);
                    colorNameBuffer[trimmedLen] = '\0';
                    return colorNameBuffer;
                } else {
                    int padLen = length > len ? length : len;
                    memset(colorNameBuffer, ' ', padLen);
                    strncpy(colorNameBuffer, src, padLen);
                    colorNameBuffer[padLen] = '\0';
                    return colorNameBuffer;
                }
            }
        }
    }
    
    // If we get here, use the closest match
    int idx = closestPaletteHueIdx(hue);
    const char* src = namedColors[idx].name;
    int len = strlen(src);
    if (length == -1) {
        int end = len - 1;
        while (end >= 0 && src[end] == ' ') end--;
        int trimmedLen = end + 1;
        strncpy(colorNameBuffer, src, trimmedLen);
        colorNameBuffer[trimmedLen] = '\0';
        return colorNameBuffer;
    } else {
        int padLen = length > len ? length : len;
        memset(colorNameBuffer, ' ', padLen);
        strncpy(colorNameBuffer, src, padLen);
        colorNameBuffer[padLen] = '\0';
        return colorNameBuffer;
    }
}

// ============================================================================
// Terminal Colors
// ============================================================================

int colorToVT100(uint32_t color, int colorDepth) {
    if (colorDepth == 256) {
        return colorToAnsi(color);
    }
    
    rgbColor input = unpackRgb(color);
    hsvColor inputHsv = RgbToHsv(input);
    
    if (inputHsv.s < 140) {
        if (inputHsv.v > 6) {
            return 15;
        } else {
            return 0;
        }
    }
    
    int hue = inputHsv.h;
    int hueIdx = closestPaletteHueIdx(hue);
    
    return namedColors[hueIdx].termColor256;
}

int colorToAnsi(uint32_t color) {
    if (color == 0x000000) {
        return 0;
    }
    if (color == 0xffffff) {
        return 15;
    }
    
    rgbColor input = unpackRgb(color);
    hsvColor hsv = RgbToHsv(input);
    
    hsv.v = 232;
    input = HsvToRgb(hsv);
    
    // Standard 16 colors (0-15)
    static const rgbColor ansi16[16] = {
        {0, 0, 0},       // 0: black
        {128, 0, 0},     // 1: dark red
        {0, 128, 0},     // 2: dark green
        {128, 128, 0},   // 3: dark yellow
        {0, 0, 128},     // 4: dark blue
        {128, 0, 128},   // 5: dark magenta
        {0, 128, 128},   // 6: dark cyan
        {192, 192, 192}, // 7: light gray
        {128, 128, 128}, // 8: dark gray
        {255, 0, 0},     // 9: bright red
        {0, 255, 0},     // 10: bright green
        {255, 255, 0},   // 11: bright yellow
        {0, 0, 255},     // 12: bright blue
        {255, 0, 255},   // 13: bright magenta
        {0, 255, 255},   // 14: bright cyan
        {255, 255, 255}  // 15: white
    };
    
    // 6x6x6 RGB cube levels (for colors 16-231)
    static const uint8_t cubeLevels[6] = {0, 95, 135, 175, 215, 255};
    
    int bestColor = 0;
    int minDistance = INT_MAX;
    
    // Check standard 16 colors (0-15)
    for (int i = 0; i < 16; i++) {
        int dr = input.r - ansi16[i].r;
        int dg = input.g - ansi16[i].g;
        int db = input.b - ansi16[i].b;
        int distance = dr*dr + dg*dg + db*db;
        
        if (distance < minDistance) {
            minDistance = distance;
            bestColor = i;
        }
    }
    
    // Check 6x6x6 RGB cube (colors 16-231)
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                uint8_t cubeR = cubeLevels[r];
                uint8_t cubeG = cubeLevels[g];
                uint8_t cubeB = cubeLevels[b];
                
                int dr = input.r - cubeR;
                int dg = input.g - cubeG;
                int db = input.b - cubeB;
                int distance = dr*dr + dg*dg + db*db;
                
                if (distance < minDistance) {
                    minDistance = distance;
                    bestColor = 16 + 36*r + 6*g + b;
                }
            }
        }
    }
    
    // Check grayscale ramp (colors 232-255)
    for (int i = 0; i < 24; i++) {
        uint8_t gray = 8 + i * 10;
        
        int dr = input.r - gray;
        int dg = input.g - gray;
        int db = input.b - gray;
        int distance = dr*dr + dg*dg + db*db;
        
        if (distance < minDistance) {
            minDistance = distance;
            bestColor = 232 + i;
        }
    }
    
    return bestColor;
}

// ============================================================================
// Terminal Output Helpers
// ============================================================================

extern bool disableTerminalColors;

int currentTerminalColor = -1;

void changeTerminalColor(int termColor, bool flush, Stream *stream, bool force) {
    if (disableTerminalColors) {
        return;
    }
    if (currentTerminalColor == termColor && !force) {
        return;
    }
    
    // Output the color escape sequence - only flush ONCE at the end if requested
    // Previously this was flushing before AND after, causing double-flush overhead
    if (termColor != -1) {
        currentTerminalColor = termColor;
        stream->printf("\033[38;5;%dm", termColor);
    } else {
        stream->print("\033[0m"); // Reset all colors and formatting
        currentTerminalColor = -1;
    }
    
    // Single flush at the end if requested
    if (flush) {
        stream->flush();
    }
}

void cycleTerminalColor(bool reset, float step, bool flush, Stream *stream, int startColorIndex, int bright) {
    if (disableTerminalColors) {
        return;
    }
    
    static float stepDistance = 5.0f;
    static float colorAccumulator = 0.0f;
    static int currentColor = 0;
    
    if (stream == NULL) {
        stream = &Serial;
    }
    
    if (step < 80.0f) {
        stepDistance = step;
    }
    
    if (reset) {
        currentColor = startColorIndex;
        colorAccumulator = 0.0f;
    } else {
        colorAccumulator += stepDistance;
        
        // Only update currentColor when we've accumulated enough for a full step
        while (colorAccumulator >= 1.0f) {
            currentColor++;
            colorAccumulator -= 1.0f;
            if (bright == 1) {
                if (currentColor >= highSaturationBrightColorsCount) {
                    currentColor = 0;
                }
            } else {
                if (currentColor >= highSaturationSpectrumColorsCount) {
                    currentColor = 0;
                }
            }
        }
        while (colorAccumulator <= -1.0f) {
            currentColor--;
            colorAccumulator += 1.0f;
            if (currentColor < 0) {
                if (bright == 1) {
                    currentColor = highSaturationBrightColorsCount - 1;
                } else {
                    currentColor = highSaturationSpectrumColorsCount - 1;
                }
            }
        }
    }
    
    int color = highSaturationSpectrumColors[currentColor];
    if (bright == 1) {
        color = highSaturationBrightColors[currentColor];
    }
    if (currentTerminalColor == color) {
        return;
    }
    currentTerminalColor = color;
    stream->printf("\033[38;5;%dm", color);
    if (flush) {
        stream->flush();
    }
}

void changeTerminalColorHighSat(int colorIndex, bool flush, Stream *stream, int bright) {
    static int currentColorIndex = 0;
    if (colorIndex == -1) {
        currentColorIndex++;
        if (bright == 1) {
            if (currentColorIndex >= highSaturationBrightColorsCount) {
                currentColorIndex = 0;
            }
        } else {
            if (currentColorIndex >= highSaturationSpectrumColorsCount) {
                currentColorIndex = 0;
            }
        }
        colorIndex = currentColorIndex;
    }
    
    int color = highSaturationSpectrumColors[colorIndex];
    if (bright == 1) {
        color = highSaturationBrightColors[colorIndex];
    }
    if (currentTerminalColor == color) {
        return;
    }
    currentTerminalColor = color;
    stream->printf("\033[38;5;%dm", color);
    if (flush) {
        stream->flush();
    }
}

// ============================================================================
// C Interface
// ============================================================================

extern "C" {
    void changeTerminalColorC(int color, bool flush) {
        changeTerminalColor(color, flush, &Serial);
    }
    
    void cycleTermColor(bool reset, float step, bool flush) {
        cycleTerminalColor(reset, step, flush, &Serial, 0, 0);
    }
}

