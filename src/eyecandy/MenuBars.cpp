/**
 * MenuBars Implementation
 */

#include "MenuBars.h"

MenuBar::MenuBar() 
    : itemCount(0)
    , positionAtBottom(true)
{
    // Initialize all items
    for (int i = 0; i < MAX_MENU_ITEMS; i++) {
        items[i].label = nullptr;
        items[i].type = ITEM_DISPLAY;
        items[i].boolValue = nullptr;
        items[i].cycleValue = nullptr;
        items[i].cycleOptions = nullptr;
        items[i].cycleCount = 0;
        items[i].enabled = true;
        items[i].width = 0;
    }
}

MenuBar::~MenuBar() {
    // Nothing to delete - we don't own the pointed-to data
}

bool MenuBar::addToggle(const char* label, bool* value, 
                        const char* onText, const char* offText,
                        void (*onChange)()) {
    if (itemCount >= MAX_MENU_ITEMS || label == nullptr || value == nullptr) {
        return false;
    }
    
    MenuItem& item = items[itemCount];
    item.label = label;
    item.type = ITEM_TOGGLE;
    item.boolValue = value;
    item.onText = onText ? onText : "ON";
    item.offText = offText ? offText : "OFF";
    item.onChange = onChange;
    item.enabled = true;
    item.width = 0;
    
    itemCount++;
    return true;
}

bool MenuBar::addCycle(const char* label, int* value, 
                       const char** options, int optionCount,
                       void (*onChange)()) {
    if (itemCount >= MAX_MENU_ITEMS || label == nullptr || 
        value == nullptr || options == nullptr || optionCount < 2) {
        return false;
    }
    
    MenuItem& item = items[itemCount];
    item.label = label;
    item.type = ITEM_CYCLE;
    item.cycleValue = value;
    item.cycleOptions = options;
    item.cycleCount = optionCount;
    item.onChange = onChange;
    item.enabled = true;
    item.width = 0;
    
    itemCount++;
    return true;
}

bool MenuBar::addAction(const char* label, void (*action)()) {
    if (itemCount >= MAX_MENU_ITEMS || label == nullptr) {
        return false;
    }
    
    MenuItem& item = items[itemCount];
    item.label = label;
    item.type = ITEM_ACTION;
    item.action = action;  // Can be nullptr if handled manually
    item.enabled = true;   // Always enabled for buttons
    item.width = 0;
    
    itemCount++;
    return true;
}

bool MenuBar::addDisplay(const char* label, String (*displayFunc)()) {
    if (itemCount >= MAX_MENU_ITEMS || label == nullptr) {
        return false;
    }
    
    MenuItem& item = items[itemCount];
    item.label = label;
    item.type = ITEM_DISPLAY;
    item.displayFunc = displayFunc;
    item.enabled = false;  // Display items can't be activated
    item.width = 0;
    
    itemCount++;
    return true;
}

void MenuBar::clear() {
    itemCount = 0;
}

String MenuBar::getItemDisplay(int index) const {
    if (index < 0 || index >= itemCount) {
        return "";
    }
    
    const MenuItem& item = items[index];
    String display = String(item.label);
    
    switch (item.type) {
        case ITEM_TOGGLE:
            if (item.boolValue) {
                display += ":";
                display += *item.boolValue ? item.onText : item.offText;
            }
            break;
            
        case ITEM_CYCLE:
            if (item.cycleValue && item.cycleOptions) {
                int idx = *item.cycleValue;
                if (idx >= 0 && idx < item.cycleCount) {
                    display += ":";
                    display += item.cycleOptions[idx];
                }
            }
            break;
            
        case ITEM_ACTION:
            // Action buttons - add visual indicator
            display = "«" + display + "»";
            break;
            
        case ITEM_DISPLAY:
            if (item.displayFunc) {
                String value = item.displayFunc();
                if (value.length() > 0) {
                    display += ":";
                    display += value;
                }
            }
            break;
    }
    
    return display;
}

String MenuBar::formatItem(int index, bool highlighted) const {
    String display = getItemDisplay(index);
    
    if (highlighted && items[index].enabled) {
        // Inverse video for highlighted items - reset all attributes after
        return "\x1b[7m[" + display + "]\x1b[0m";
    } else if (highlighted) {
        // Just brackets for disabled highlighted items
        return "[" + display + "]";
    } else {
        return " " + display + " ";
    }
}

int MenuBar::draw(bool isActive, int highlightIndex) {
    if (itemCount == 0) {
        return 0;
    }
    
    // Start with inverse video background
    //Serial.print("\x1b[7m");  // Inverse video
    
    // Draw each item with color cycling
    float colorStep = 4.0;  // Rainbow step size
    cycleTerminalColor(true, colorStep, false, &Serial, -2, 1);  // Reset colors
    
    for (int i = 0; i < itemCount; i++) {
        // Cycle to next color for this item
        cycleTerminalColor(false, colorStep, false, &Serial, 0, 1);
        
        bool shouldHighlight = isActive && (i == highlightIndex);
        String itemStr = formatItem(i, shouldHighlight);
        Serial.print(itemStr);
        
        // Add separator between items
        if (i < itemCount - 1) {
            Serial.print("  |  ");  // Separator with reset
        }
    }
    
    // Fill rest of line with spaces
    // Calculate approximate visible length (rough estimate)
    int approxLen = 0;
    for (int i = 0; i < itemCount; i++) {
        String display = getItemDisplay(i);
        approxLen += display.length() + 4;  // +4 for brackets/spaces
    }
    approxLen += (itemCount - 1) * 3;  // Separators
    
    // Pad to full width
    Serial.print("\x1b[0m");  // Reset colors
    for (int i = approxLen; i < 125; i++) {
        Serial.print(" ");
    }
    
    Serial.println("\x1b[0m");  // Reset all
    Serial.flush();
    
    return 1;  // One line drawn
}

void MenuBar::activateItem(int index) {
    if (index < 0 || index >= itemCount) {
        return;
    }
    
    MenuItem& item = items[index];
    
    if (!item.enabled) {
        return;  // Can't activate disabled items
    }
    
    switch (item.type) {
        case ITEM_TOGGLE:
            if (item.boolValue) {
                *item.boolValue = !(*item.boolValue);
                if (item.onChange) {
                    item.onChange();
                }
            }
            break;
            
        case ITEM_CYCLE:
            if (item.cycleValue) {
                *item.cycleValue = (*item.cycleValue + 1) % item.cycleCount;
                if (item.onChange) {
                    item.onChange();
                }
            }
            break;
            
        case ITEM_ACTION:
            if (item.action) {
                item.action();
            }
            break;
            
        case ITEM_DISPLAY:
            // Display items don't do anything when activated
            break;
    }
}

