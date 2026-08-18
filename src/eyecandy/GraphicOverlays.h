// SPDX-License-Identifier: MIT
#ifndef GRAPHIC_OVERLAYS_H
#define GRAPHIC_OVERLAYS_H

#include <Arduino.h>
#include <stdint.h>

// ============================================================================
// Breadboard Coordinate System (10x30)
// ============================================================================
//
// The breadboard is treated as a single 10-row × 30-column matrix:
//
//   Row 1 = Physical row A (top half, furthest from gap) or E?
//   Let's stick to the map:
//   Row 1-5 = Top half (E, D, C, B, A)
//   Row 6-10 = Bottom half (F, G, H, I, J)
//
//   Columns 1-30 = Breadboard columns 1-30
//
// ============================================================================

// Maximum overlays and size constraints.
// Graphic overlays are a breadboard-LED-matrix feature (10 rows x 30 cols of
// addressable color). The OG Jumperless has a single LED per row, so overlays
// can't render there; we shrink the storage to one slot to reclaim ~8.7 KB of
// the RP2040's scarce SRAM (graphicOverlayState was ~10 KB) and no-op the
// renderer (see renderGraphicOverlays). The struct/API stay intact so the ~12
// files that reference the overlay system still compile unchanged.
#if defined(OG_JUMPERLESS)
#define MAX_GRAPHIC_OVERLAYS 1
#else
#define MAX_GRAPHIC_OVERLAYS 8
#endif
#define MAX_OVERLAY_WIDTH 30   // Max columns
#define MAX_OVERLAY_HEIGHT 10  // Max rows
#define MAX_OVERLAY_PIXELS (MAX_OVERLAY_WIDTH * MAX_OVERLAY_HEIGHT)  // 300 pixels

/**
 * @brief A single graphic overlay that can be rendered on breadboard LEDs
 * 
 * Overlays render on top of all other LED visualizations (after showNets,
 * showLEDmeasurements, showAllRowAnimations). Color 0 is treated as transparent.
 * 
 * Colors are stored in row-major order: colors[row * width + col]
 */
struct GraphicOverlay {
    char name[32];           // Overlay identifier
    int startRow;            // Starting row (1-10)
    int startCol;            // Starting column (1-30)
    int width;               // Width in columns
    int height;              // Height in rows
    uint32_t colors[MAX_OVERLAY_PIXELS];  // RGB colors, 0 = transparent
    bool enabled;
    
    void clear();
};

/**
 * @brief Global state for all graphic overlays
 */
struct GraphicOverlayState {
    GraphicOverlay overlays[MAX_GRAPHIC_OVERLAYS];
    int numOverlays;
    bool needsRender;
    
    void clear();
    
    /**
     * @brief Add a 2D overlay
     * @param name Unique identifier for the overlay
     * @param startRow Starting row (1-10)
     * @param startCol Starting column (1-30)
     * @param width Width in columns
     * @param height Height in rows
     * @param colors Array of RGB colors in row-major order (0 = transparent)
     * @return Overlay index on success, -1 on failure
     */
    int addOverlay(const char* name, int startRow, int startCol,
                   int width, int height, const uint32_t* colors);
    
    /**
     * @brief Remove overlay by name
     * @return true if found and removed
     */
    bool removeOverlay(const char* name);
    
    /**
     * @brief Remove overlay by index
     * @return true if valid index and removed
     */
    bool removeOverlay(int index);
    
    /**
     * @brief Clear all overlays
     */
    void clearAll();
    
    /**
     * @brief Find overlay by name
     * @return Index if found, -1 otherwise
     */
    int findByName(const char* name) const;
    
    /**
     * @brief Set a single pixel in the overlay buffer
     * @param row Breadboard row (1-10)
     * @param col Column (1-30)
     * @param color RGB color (0 = transparent)
     */
    void setPixel(int row, int col, uint32_t color);
    
    /**
     * @brief Shift overlay position by delta
     * @param name Overlay to move
     * @param deltaRow Rows to shift (negative = up, positive = down)
     * @param deltaCol Columns to shift (negative = left, positive = right)
     * @return true if overlay found and moved
     */
    bool shiftOverlay(const char* name, int deltaRow, int deltaCol);
    
    /**
     * @brief Place overlay at absolute position
     * @param name Overlay to move
     * @param newRow New starting row (1-10)
     * @param newCol New starting column (1-30)
     * @return true if overlay found and moved
     */
    bool placeOverlay(const char* name, int newRow, int newCol);



    
    void debugMenu(void);


};

// Global overlay state
extern GraphicOverlayState graphicOverlayState;

// Initialize the overlay system
void initGraphicOverlays();

// Render all overlays - called after showAllRowAnimations() in Core2 loop
void renderGraphicOverlays();

// YAML serialization helpers (called from States.cpp)
void serializeOverlaysToYAML(String& output, int withANSI = 0);
bool deserializeOverlaysFromYAML(const char* yamlContent, String& errorMsg);

// JSON serialization helper (called from JsonState.cpp)
void serializeOverlaysToJSON(String& output);

// Run the Snake game (breadboard LED overlay). Used as app and from overlay debug menu.
// Exit: serial 'q', or hold clickwheel.
void runSnakeGame(void);

// TODO: Future enhancements
// - Support for rails (power rail LEDs)
// - Support for nano header LEDs



#endif // GRAPHIC_OVERLAYS_H
