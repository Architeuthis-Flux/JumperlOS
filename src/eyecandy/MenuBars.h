/**
 * MenuBars - Reusable menu and status bar system
 * 
 * Provides interactive menu bars that can be:
 * - Displayed at top or bottom of terminal
 * - Navigated with cursor keys
 * - Toggled with Enter/Space
 * - Used for status display
 * 
 * Used by BitmapEditor, eKilo, and other terminal applications
 */

#ifndef MENUBARS_H
#define MENUBARS_H

#include <Arduino.h>
#include "Colors.h"  // For cycleTerminalColor

// Maximum number of menu items in a bar
#define MAX_MENU_ITEMS 12

/**
 * @brief Types of menu items
 */
enum MenuItemType {
    ITEM_TOGGLE,    // Boolean toggle (shown as name:ON/OFF or name:value1/value2)
    ITEM_CYCLE,     // Cycle through multiple values
    ITEM_ACTION,    // Execute an action when activated
    ITEM_DISPLAY    // Read-only display field
};

/**
 * @brief Single menu item
 */
struct MenuItem {
    const char* label;           // Display label
    MenuItemType type;           // Item type
    
    // For TOGGLE items
    bool* boolValue;             // Pointer to boolean value
    const char* onText;          // Text to show when true (default: "ON")
    const char* offText;         // Text to show when false (default: "OFF")
    
    // For CYCLE items
    int* cycleValue;             // Pointer to current cycle index
    const char** cycleOptions;   // Array of option strings
    int cycleCount;              // Number of options
    
    // For ACTION items
    void (*action)(); // Function to call when activated
    
    // For DISPLAY items
    String (*displayFunc)(); // Function that returns display string
    
    // Callback for when value changes (optional)
    void (*onChange)();
    
    // Visual properties
    bool enabled;                // Can this item be interacted with?
    int width;                   // Minimum width for this item (0 = auto)
};

/**
 * @brief Menu bar that can display status and handle interaction
 */
class MenuBar {
public:
    MenuBar();
    ~MenuBar();
    
    // Add menu items
    bool addToggle(const char* label, bool* value, 
                   const char* onText = "ON", const char* offText = "OFF",
                   void (*onChange)() = nullptr);
    
    bool addCycle(const char* label, int* value, 
                  const char** options, int optionCount,
                  void (*onChange)() = nullptr);
    
    bool addAction(const char* label, void (*action)());
    
    bool addDisplay(const char* label, String (*displayFunc)());
    
    // Clear all items
    void clear();
    
    // Draw the menu bar (returns number of lines drawn)
    int draw(bool isActive = false, int highlightIndex = -1);
    
    // Navigation
    int getItemCount() const { return itemCount; }
    const MenuItem& getItem(int index) const { return items[index]; }
    
    // Activate current item (toggle, cycle, or execute action)
    void activateItem(int index);
    
    // Get display string for an item
    String getItemDisplay(int index) const;
    
    // Set whether menu bar is at top or bottom of screen
    void setPosition(bool atBottom) { positionAtBottom = atBottom; }
    
private:
    MenuItem items[MAX_MENU_ITEMS];
    int itemCount;
    bool positionAtBottom;
    
    // Helper to format item for display
    String formatItem(int index, bool highlighted) const;
};

#endif // MENUBARS_H

