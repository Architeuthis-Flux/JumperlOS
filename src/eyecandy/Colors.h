// SPDX-License-Identifier: MIT
#ifndef COLORS_H
#define COLORS_H

#include <Arduino.h>
#include "LEDs.h"

/**
 * @file Colors.h
 * @brief Centralized color management for Jumperless
 * 
 * This module handles all color conversions between:
 * - Wokwi color names (for diagram imports)
 * - Internal RGB colors
 * - Terminal VT100 colors
 * - Named color strings
 */

// ============================================================================
// Wokwi Color Support
// ============================================================================

/**
 * @brief Wokwi wire color definitions (from Wokwi documentation)
 * These are the exact colors supported by Wokwi's diagram editor
 */
struct WokwiColor {
    const char* name;           // Wokwi color name
    uint32_t rgb;              // RGB color value
    const char* internalName;  // Our internal color name
    char keyboardShortcut;     // Wokwi keyboard shortcut
};

// All Wokwi colors with their RGB values and keyboard shortcuts
extern const WokwiColor wokwiColors[];
extern const int wokwiColorCount;

/**
 * @brief Convert Wokwi color name to RGB value
 * @param colorName Wokwi color name (case insensitive)
 * @return RGB color as uint32_t, or 0xFFFFFF if unknown
 */
uint32_t wokwiColorToRGB(const String& colorName);

/**
 * @brief Convert Wokwi color name to internal color name
 * @param wokwiColor Wokwi color name
 * @return Internal color name for display/logging
 */
String wokwiColorToInternalName(const String& wokwiColor);

/**
 * @brief Convert RGB value back to Wokwi color name (reverse lookup)
 * Preserves original Wokwi color names when round-tripping
 * @param rgb RGB color value
 * @return Wokwi color name if exact match, otherwise closest palette name
 */
String rgbToWokwiColorName(uint32_t rgb);

// ============================================================================
// Color Manipulation
// ============================================================================

/**
 * @brief Shift a color's hue by a specified amount
 * Useful for distinguishing multiple nets with the same base color
 * 
 * @param baseColor Original RGB color
 * @param shiftAmount Amount to shift hue (0-255, wraps around)
 * @return Shifted RGB color
 */
uint32_t shiftColorHue(uint32_t baseColor, int shiftAmount);

/**
 * @brief Blend two colors
 * @param color1 First color
 * @param color2 Second color
 * @param ratio Blend ratio (0.0 = all color1, 1.0 = all color2)
 * @return Blended color
 */
uint32_t blendColors(uint32_t color1, uint32_t color2, float ratio);

// ============================================================================
// Terminal Colors
// ============================================================================

/**
 * @brief Convert RGB color to VT100 terminal color code
 * Supports both 256-color and 16-color modes
 * 
 * @param color RGB color value
 * @param colorDepth Color depth (256 or 16)
 * @return VT100 color code
 */
int colorToVT100(uint32_t color, int colorDepth);

/**
 * @brief Convert RGB color to ANSI 256-color code
 * @param color RGB color value
 * @return ANSI 256-color code (0-255)
 */
int colorToAnsi(uint32_t color);

// ============================================================================
// Color Naming
// ============================================================================

/**
 * @brief Convert RGB color to human-readable color name
 * Uses the internal color palette to find the closest match
 * 
 * @param color RGB color value
 * @param length Desired output length (-1 for trimmed, >0 for padded)
 * @return Color name (uses static buffer)
 */
char* colorToName(uint32_t color, int length);

/**
 * @brief Convert rgbColor struct to color name
 */
char* colorToName(rgbColor color, int length);

/**
 * @brief Convert hue value to color name
 */
char* colorToName(int hue, int length);

/**
 * @brief Find the closest named color index for a given hue
 * @param hue Hue value (0-255)
 * @return Index into namedColors array
 */
int closestPaletteHueIdx(int hue);

// ============================================================================
// Terminal Output Helpers
// ============================================================================

/**
 * @brief Change terminal color for subsequent output
 * @param termColor VT100 color code (-1 to reset)
 * @param flush Whether to flush the stream after changing color
 * @param stream Output stream
 */
void changeTerminalColor(int termColor, bool flush, Stream *stream, bool force = true);

/**
 * @brief Cycle through high saturation spectrum colors
 * @param reset If true, reset to start color
 * @param step Step size for color cycling (0.1-100.0)
 * @param flush Whether to flush after color change
 * @param stream Output stream
 * @param startColorIndex Starting color index (if reset=true)
 * @param bright Use bright colors only (1) or full spectrum (0)
 */
void cycleTerminalColor(bool reset, float step, bool flush, Stream *stream, int startColorIndex, int bright);

/**
 * @brief Change to a specific high saturation color by index
 * @param colorIndex Index in spectrum array (-1 for next)
 * @param flush Whether to flush after change
 * @param stream Output stream
 * @param bright Use bright colors (1) or full spectrum (0)
 */
void changeTerminalColorHighSat(int colorIndex, bool flush, Stream *stream, int bright);

// ============================================================================
// Color Palette
// ============================================================================

/**
 * @brief Named color palette with terminal color mappings
 * Defined in Colors.cpp
 */
extern const NamedColor namedColors[];
extern const int namedColorsCount;

// High saturation spectrum colors for terminal cycling
extern const int highSaturationSpectrumColors[];
extern const int highSaturationSpectrumColorsCount;
extern const int highSaturationBrightColors[];
extern const int highSaturationBrightColorsCount;

// ============================================================================
// C Interface for MicroPython
// ============================================================================

extern "C" {
    void changeTerminalColorC(int color, bool flush);
    void cycleTermColor(bool reset, float step, bool flush);
}

#endif // COLORS_H

