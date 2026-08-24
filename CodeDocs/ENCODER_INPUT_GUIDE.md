# Encoder Input Functions Guide

## Overview

The JumperlOS firmware provides two generalized functions for getting user input via the rotary encoder: **integer input** and **string input**. These functions are designed to be reusable throughout the codebase for any situation where you need to get a value from the user.

## Integer Input: `getActionInt()`

### Function Signature
```cpp
int getActionInt(int minVal, int maxVal, int currentValue = -1);
```

### Parameters
- **minVal**: Minimum allowed value
- **maxVal**: Maximum allowed value  
- **currentValue**: Starting value (optional, defaults to middle of range if -1)

### Returns
- The selected integer value
- Returns original value if canceled (long press or serial input)

### Usage Example
```cpp
// Get brightness value from 0-100, starting at current brightness
int newBrightness = getActionInt(0, 100, currentBrightness);

// Get a value from 16-2048, starting at midpoint
int dimension = getActionInt(16, 2048);
```

### User Controls
- **Rotate encoder**: Adjust value (with acceleration for large ranges)
- **Short press**: Confirm and return value
- **Long press**: Cancel and return original value
- **Serial input**: Cancel and return original value

### Features
- Smooth acceleration for fast scrolling through large ranges
- Fractional accumulation for precise control at slow speeds
- Color-coded display (low=cyan, mid=yellow, high=magenta)
- Displays on breadboard LEDs, OLED, and serial

### Menu Integration
To add integer input to a menu:
```
--$Custom Width$
---*16**32**64**128**Custom*
---->i(16)(2048)
```
- `>i` indicates integer input action
- First parentheses `(16)` = minimum value
- Second parentheses `(2048)` = maximum value

## String Input: `getActionString()`

### Function Signature
```cpp
String getActionString(int maxLength = 32);
```

### Parameters
- **maxLength**: Maximum string length (default 32, max 128)

### Returns
- The entered string
- Returns empty string if canceled

### Usage Example
```cpp
// Get a filename (max 32 chars)
String filename = getActionString(32);

// Get a short label (max 16 chars)
String label = getActionString(16);
```

### User Controls

**Via Rotary Encoder:**
- **Rotate encoder**: Select character from character set (clockwise = forward through alphabet)
- **Short press**: Confirm character and advance to next position
- **Double-click**: Delete last character (backspace)
- **Long press**: Finish and return string

**Via Serial Terminal (Interactive Mode):**
- **Type normally**: Characters appear instantly on all displays
- **Backspace/Delete**: Removes last character
- **Enter/Return**: Finish and return string
- **ESC**: Cancel and return empty string
- **Live updates**: Breadboard, OLED, and serial all update in real-time

### Features
- **Dual input modes**: Use encoder OR type from serial (mix both!)
- **Live serial typing**: Interactive mode with instant visual feedback
- **Dual-row breadboard display**: Uses both top and bottom rows (16 chars visible)
- **Smart scrolling**: Automatically scrolls text when exceeding 16 characters
- Full printable ASCII character set plus special control characters
- **Accelerated character browsing** - quickly scroll through alphabet
- **Special characters**: `<BS>`, `<TAB>`, `<ENTER>` for control codes
- **Smart cursor positioning**: Next character starts where last one was
- Visual feedback showing current character on breadboard LEDs
- Shows previously entered characters dimmed
- Displays full string with cursor on OLED and serial
- Backspace via the `<BS>` character in the set, or the serial backspace key.
  (There is **no** wheel double-click for it — the encoder has no double-click
  gesture at all. The commented-out `DOUBLECLICKED` branch that once did this
  was removed on 2026-08-22 along with the state itself.)
- Smooth wraparound at start/end of character set

### Character Set
```
Printable: " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:',.<>?/\""
Special:   <BS> (backspace), <TAB> (tab), <ENTER> (newline)
```

### Menu Integration
To add string input to a menu:
```
-StartUpMessage
-->t(32)
```
- `>t` indicates text input action
- Parentheses `(32)` = maximum length

## Integration Architecture

### Menu Action Types
The menu system recognizes these action types:
- `>n` = nodes (action 1)
- `>b` = baud (action 2)
- `>v` = voltage (action 3)
- `>i` = integer input (action 7)
- `>t` = text input (action 8)
- `>c` = connect (action 9)

### Data Flow

#### Integer Input (Action 7)
1. Menu parser detects `>i(min)(max)` in menu string
2. Sets `actions[menuPosition] = 7`
3. When menu item selected, calls `getActionInt(min, max, currentVal)`
4. Result stored in `currentAction.integerValue`
5. `doMenuAction()` applies value to appropriate config field

#### String Input (Action 8)
1. Menu parser detects `>t(maxLength)` in menu string
2. Sets `actions[menuPosition] = 8`
3. When menu item selected, calls `getActionString(maxLength)`
4. Result stored in `currentAction.stringValue`
5. `doMenuAction()` applies string to appropriate config field

## State Management

Both functions follow consistent state management patterns:

### Initialization
```cpp
// Save current rotary divider
int lastDivider = rotaryDivider;
rotaryDivider = 4; // Set for input mode

// Set menu state to block other inputs
Menus::getInstance().inClickMenu = 1;

// Reset button states to wait for new presses
encoderButtonState = IDLE;
lastButtonEncoderState = IDLE;
```

### Cleanup
```cpp
// Restore rotary divider
rotaryDivider = lastDivider;

// Clear displays
b.clear();
Serial.println();

// Release menu state
Menus::getInstance().inClickMenu = 0;
```

## Adding New Input Types

To add a new menu-based input:

1. **Add to menu tree** (`menuTree.h`):
```cpp
"-YourNewSetting",
"-->i(0)(100)", // or -->t(maxLength)
```

2. **Handle in `doMenuAction()`** (`Menus.cpp`):
```cpp
// For integer input (already handled by action 7)
if (menuLines[currentAction.previousMenuPositions[1]].indexOf("YourNewSetting") != -1) {
    yourConfig.setting = currentAction.integerValue;
    configChanged = true;
    saveConfig();
}

// For string input (already handled by action 8)
if (menuLines[currentAction.previousMenuPositions[1]].indexOf("YourNewSetting") != -1) {
    strncpy(yourConfig.setting, currentAction.stringValue.c_str(), MAX_LEN);
    yourConfig.setting[MAX_LEN] = '\0';
    configChanged = true;
    saveConfig();
}
```

## Direct Usage (Non-Menu)

You can also call these functions directly from anywhere in the code:

### Integer Input
```cpp
void adjustSomeValue() {
    int current = someGlobalValue;
    int newValue = getActionInt(0, 255, current);
    
    if (newValue != current) {
        someGlobalValue = newValue;
        // Do something with new value
    }
}
```

### String Input
```cpp
void getSomeName() {
    String name = getActionString(20);
    
    if (name.length() > 0) {
        // User entered something
        processName(name);
    } else {
        // User canceled
        Serial.println("Input canceled");
    }
}
```

### Custom Acceleration in Your Code

If you want different acceleration for a specific use case, you can use the `EncoderAccelerator` class directly:

```cpp
// Create custom accelerator
EncoderAccelerator myAccel(0.3f, 20.0f, 5.0f);  // Gentle acceleration

// Or use a preset
EncoderAccelerator myAccel = EncoderAccelerator::Medium();

// In your input loop:
long currentPos = encoderPosition;
long delta = currentPos - lastPos;

if (delta != 0) {
    float accelDelta = myAccel.getAcceleratedDelta(delta);
    myValue += accelDelta;
    // ... update display ...
}

// Reset when confirming or changing context
if (userConfirmed) {
    myAccel.reset();
}
```

**Choosing the Right Configuration:**

| Use Case | Suggested Config | Reasoning |
|----------|-----------------|-----------|
| Large ranges (0-10000) | `::Fast()` or custom aggressive | Need to scroll quickly |
| Small ranges (0-100) | `::Slow()` | Precision more important |
| Character selection | `::Medium()` | Balance speed and precision |
| Fine tuning (0.0-5.0V) | Custom with baseSpeed=1.0 | Maximum precision needed |
| Menu navigation | `::Fast()` | Quick scrolling preferred |

## Best Practices

1. **Range Selection (Integer)**:
   - Keep ranges reasonable (0-100 is better than 0-10000)
   - Use appropriate starting values to minimize user effort
   - Consider logarithmic ranges for very large spans

2. **Length Limits (String)**:
   - Match maxLength to your display and storage constraints
   - Remember to account for null terminator in storage
   - OLED can typically show ~16 characters per line

3. **User Feedback**:
   - Always show confirmation after saving
   - Display current value before allowing changes
   - Use Serial output for debugging and non-OLED setups

4. **Error Handling**:
   - Check return values for empty/canceled inputs
   - Validate ranges before storing to config
   - Ensure null termination for strings

## Common Patterns

### Preset Values with Custom Option
```cpp
// Menu shows presets: *16**32**64**Custom*
// If "Custom" selected, show integer input
if (selectedOption == "custom") {
    currentAction.integerValue = getActionInt(minVal, maxVal, currentVal);
} else {
    currentAction.integerValue = parsePresetValue(selectedOption);
}
```

### Optional String Input
```cpp
String input = getActionString(32);
if (input.length() == 0) {
    // Use default or keep existing
    input = defaultValue;
}
```

### Constrained Integer with Validation
```cpp
int value = getActionInt(1, 100, currentValue);
// Additional validation if needed
if (value % 2 != 0) {
    value++; // Force even values
}
```

## Technical Details

### Encoder Acceleration (Used in Both Functions)

Both `getActionInt()` and `getActionString()` now use a shared `EncoderAccelerator` class that provides intelligent, **configurable** acceleration.

#### Configurable Parameters

```cpp
EncoderAccelerator(
    float baseSpeed = 0.1f,      // Starting multiplier (precision)
    float maxSpeed = 50.0f,      // Maximum multiplier (speed)
    float rampRate = 12.0f,      // Acceleration rate
    float decayRate = 0.9f,      // Deceleration rate
    int fastThreshold = 4,       // Delta for "fast" rotation
    int timeoutMs = 120          // Reset timeout
);
```

#### How It Works

- **Starts slow**: `baseSpeed` multiplier for precise control
- **Ramps up fast**: Up to `maxSpeed` multiplier for quick browsing
- **Direction-aware**: Resets to `baseSpeed` when direction changes
- **Speed-sensitive**: Delta >= `fastThreshold` triggers acceleration
- **Time-based decay**: Resets after `timeoutMs` of no movement

#### Preset Configurations

```cpp
// Fast preset - for large ranges (0-2048)
EncoderAccelerator::Fast()    // baseSpeed=0.1, maxSpeed=50.0, rampRate=12.0

// Medium preset - balanced for most uses
EncoderAccelerator::Medium()  // baseSpeed=0.2, maxSpeed=25.0, rampRate=8.0

// Slow preset - maximum precision (0-100)
EncoderAccelerator::Slow()    // baseSpeed=0.5, maxSpeed=10.0, rampRate=3.0
```

#### Custom Configuration Examples

```cpp
// Very aggressive for huge ranges
EncoderAccelerator aggressive(0.05f, 100.0f, 20.0f);

// Ultra-precise for small adjustments
EncoderAccelerator precise(1.0f, 5.0f, 1.0f, 0.7f, 6, 250);

// Character selection optimized
EncoderAccelerator charSelect(0.2f, 30.0f, 10.0f);
```

#### Benefits

- **Tunable UX**: Adjust feel for different contexts
- **Consistent behavior**: Same acceleration logic everywhere
- **Sub-unit precision**: Fractional accumulation prevents jumpy movement
- **Easy presets**: Use `::Fast()`, `::Medium()`, or `::Slow()`

### Memory Safety (String Input)
- Uses fixed 129-byte buffer (128 chars + null)
- Prevents buffer overflows with size limits
- Validates maxLength parameter
- Ensures null termination

### Character Selection Performance (String Input)
- Uses fractional character index (float) for smooth acceleration
- Wraps around smoothly at character set boundaries  
- Acceleration resets when confirming character or backspacing
- Can quickly jump from 'a' to 'z' with fast rotation
- Still precise enough to select one character at a time
- **Smart positioning**: Next character starts where last was (e.g., type "Hello" and encoder stays near 'o')

### Interactive Mode for Live Typing
- Automatically enables interactive mode (0x0E) on entry
- Provides live character echo in terminal
- Updates breadboard and OLED in real-time as you type
- Properly disables interactive mode (0x0F) on exit
- Works seamlessly with all exit paths (cancel, finish, max length)

### Breadboard Display with Scrolling
- **Dual-row layout**: Top row (0-7), bottom row (8-15)
- **16-character window**: Shows entered text in scrolling window
- **Smart scroll offset**: Keeps cursor visible and centered
- **Flow pattern**: Fills top row → fills bottom row → scrolls
- Current character always highlighted
- Previous characters shown dimmed
- Special characters displayed as compact names (BS, TAB, RET)

### Thread Safety
Both functions:
- Set `inClickMenu` flag to block concurrent menu access
- Poll encoder state in tight loop
- Restore state on all exit paths

## Troubleshooting

**Integer input not responding:**
- Check rotaryDivider is being restored
- Verify encoder is initialized
- Check for conflicting encoder modes

**String input missing characters:**
- Verify character set includes needed symbols
- Check maxLength isn't too restrictive
- Ensure display can show all characters
- For special chars, select `<BS>`, `<TAB>`, `<ENTER>` from encoder

**Can't type from serial:**
- Ensure Serial is connected and initialized
- Check that input isn't being consumed elsewhere
- Use ESC to cancel if stuck in input mode

**Values not saving:**
- Verify doMenuAction() handler is present
- Check config field exists and has correct type
- Confirm saveConfig() is being called
- Use Serial.print() to debug value flow

## Examples in Codebase

### OLED Width/Height (Integer)
```cpp
// menuTree.h
"--$Width$",
"---*32**64**128**256**Custom*",
"---->i(16)(2048)",

// Menus.cpp doMenuAction()
if (menuLines[currentAction.previousMenuPositions[2]].indexOf("Width") != -1) {
    jumperlessConfig.top_oled.width = currentAction.integerValue;
    oled.displayWidth = currentAction.integerValue;
    configChanged = true;
    saveConfig();
}
```

### Startup Message (String)
```cpp
// menuTree.h
"-StartUpMessge",
"-->t(32)",

// Menus.cpp doMenuAction()
if (menuLines[currentAction.previousMenuPositions[1]].indexOf("StartUpMessge") != -1) {
    strncpy(jumperlessConfig.top_oled.startup_message, 
            currentAction.stringValue.c_str(), 32);
    jumperlessConfig.top_oled.startup_message[32] = '\0';
    configChanged = true;
    saveConfig();
}
```

## Summary

These generalized input functions provide a consistent, user-friendly way to get values from the user via the rotary encoder. They handle all the low-level encoder interaction, state management, and display updates, allowing you to focus on your application logic. By following the integration patterns shown in this guide, you can easily add new settings and user inputs throughout the firmware.

