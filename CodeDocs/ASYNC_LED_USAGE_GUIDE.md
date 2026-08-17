# Async LED System Usage Guide

## Quick Reference for Developers

### Safe Operations

✅ **These are ALWAYS safe:**
- `setPixelColor()` - modifies pixel buffer
- `setBrightness()` - modifies brightness
- `clear()` - clears pixel buffer
- `fill()` - fills pixel buffer
- `show()` - call as often as you want, even while DMA busy
- `showBlocking()` - guaranteed synchronous display (for critical UI)

### Operations Requiring Care

⚠️ **Check DMA before these:**
```cpp
// Before changing LED pin function:
if (!leds.isDMABusy()) {
    gpio_set_function(LED_PIN, GPIO_FUNC_SIO);
    // ... do stuff ...
}

// Before calling updateLength() (if ever needed dynamically):
if (!leds.isDMABusy()) {
    leds.updateLength(new_length);
}
```

### When to Use showBlocking() vs show()

#### Use `show()` (async) for:
- Normal LED updates in main loop
- High-frequency animations
- Status indicators
- Network/connection displays
- Any non-critical UI

#### Use `showBlocking()` (sync) for:
- Probe menu displays (REQUIRED - menu waits for completion)
- Voltage adjuster UI (ensures frame completeness)
- Critical error displays
- Bootup animations where timing matters
- Any UI that blocks waiting for button input

### Checking DMA Status

```cpp
// Check if previous transfer still running:
bool busy = leds.isDMABusy();

// Wait for DMA to complete (if needed):
while (leds.isDMABusy()) {
    tight_loop_contents();
}

// Check if we can start new transfer:
if (leds.canShow()) {
    leds.show();  // Safe to call
}
```

### Performance Characteristics

#### Async Mode (show())
- **Returns**: Immediately (~200-500μs including buffer prep)
- **Actual transfer time**: 3-15ms depending on LED count
- **Frame rate**: Limited by WS2812 timing, not CPU
- **CPU overhead**: Minimal - copy + DMA setup only

#### Blocking Mode (showBlocking())  
- **Returns**: After complete transfer (3-15ms)
- **Transfer time**: Same as async
- **Frame rate**: Limited by transfer time
- **CPU overhead**: Same as async, but blocks execution

### Common Patterns

#### High-Frequency Updates
```cpp
void loop() {
    // Update pixel data
    for (int i = 0; i < LED_COUNT; i++) {
        bbleds.setPixelColor(i, colors[i]);
    }
    
    // Show with timing check
    if (micros() - lastShow > LED_SHOW_MIN_TIME) {
        leds.show();  // Async - returns immediately
        lastShow = micros();
    }
    
    // Continue with other work while DMA runs
    doOtherStuff();
}
```

#### Blocking Menu Display
```cpp
void displayMenu() {
    // Update menu LEDs
    for (int i = 0; i < MENU_LEDS; i++) {
        topleds.setPixelColor(i, menuColors[i]);
    }
    
    // CRITICAL: Use blocking mode before button wait
    leds.showBlocking();
    
    // Now safe to wait for button - LEDs fully updated
    while (!button.pressed()) {
        delay(10);
    }
}
```

#### Probe Button Interaction
```cpp
int checkButton() {
    // Wait for LED updates to complete
    if (showingProbeLEDs != 0) {
        return 0;
    }
    
    // CRITICAL: Check DMA before GPIO manipulation
    if (probeLEDs.isDMABusy()) {
        return 0;  // Try again next poll
    }
    
    // Now safe to change pin function for button read
    gpio_function_t lastFunc = gpio_get_function(PROBE_LED_PIN);
    gpio_set_function(PROBE_LED_PIN, GPIO_FUNC_SIO);
    
    // ... button reading ...
    
    gpio_set_function(PROBE_LED_PIN, lastFunc);
    return buttonState;
}
```

### Debugging LED Issues

#### Symptom: LEDs shifting or glitching randomly
**Likely causes:**
1. GPIO function changed while DMA active → Add `isDMABusy()` check
2. Memory corruption in pixel buffer → Check for buffer overruns
3. Power supply issues → Check 5V rail stability

#### Symptom: LEDs not updating at all
**Likely causes:**
1. `show()` never called → Add call to `show()`
2. DMA stuck in busy state → Check for deadlock, use `showBlocking()`
3. PIO program not loaded → Check `begin()` was called

#### Symptom: LEDs update slowly
**Likely causes:**
1. Using `showBlocking()` unnecessarily → Switch to async `show()`
2. Calling `show()` too frequently → Add `canShow()` check
3. Too many LEDs → This is physics - WS2812 protocol is ~30μs per LED

#### Symptom: Colors wrong
**Likely causes:**
1. Pixel format mismatch (RGB vs GRB) → Check NEO_GRB flag
2. Brightness too high causing color shift → Reduce brightness
3. Voltage drop on long LED strips → Add power relay

### Performance Tuning

#### Maximize Frame Rate
```cpp
// Use dirty flags to skip unnecessary updates
if (netChanged) {
    updateLEDColors();
    leds.show();  // Async - non-blocking
    netChanged = false;
}

// Don't wait for completion unless necessary
// DMA runs in background while you do other work
```

#### Minimize Glitches
```cpp
// Always check DMA before GPIO manipulation
if (!leds.isDMABusy()) {
    // Safe to manipulate LED pins
}

// Use blocking mode for critical displays
leds.showBlocking();  // Guaranteed complete before return
```

#### Balance CPU Load
```cpp
// Async updates for background status
statusLEDs.show();  // Returns immediately

// Do processing while DMA runs
processNetworkUpdates();
handleButtons();

// Blocking only when user is waiting
if (menuActive) {
    menuLEDs.showBlocking();  // Ensure display before input
}
```

### Thread Safety (Dual-Core)

The LED system is safe for dual-core use:
- Core 0: Can modify pixel buffers freely
- Core 1: Can modify different pixel buffers
- DMA: Runs independently on DMA controller

**Important**: Don't modify the same pixel buffer from both cores simultaneously!

```cpp
// CORE 0
void core0Loop() {
    // Modify breadboard LEDs
    bbleds.setPixelColor(0, 0xFF0000);
    bbleds.show();  // Safe - only Core 0 touches bbleds
}

// CORE 1  
void core1Loop() {
    // Modify top LEDs
    topleds.setPixelColor(0, 0x00FF00);
    topleds.show();  // Safe - only Core 1 touches topleds
}
```

### Best Practices Summary

1. ✅ Use `show()` (async) by default for performance
2. ✅ Use `showBlocking()` for critical UI that blocks on user input
3. ✅ Check `isDMABusy()` before GPIO function changes
4. ✅ Check `canShow()` before high-frequency updates
5. ✅ Use dirty flags to skip unnecessary LED updates
6. ✅ Keep each LED strip managed by one core only
7. ❌ Don't call `updateLength()` while DMA is active
8. ❌ Don't change LED pin functions while DMA is busy
9. ❌ Don't busy-wait on `isDMABusy()` in main loop
10. ❌ Don't assume `show()` has completed when it returns

### Migration from Blocking to Async

If upgrading old code that used blocking LEDs:

```cpp
// OLD CODE (blocking):
leds.show();  // Would block 3-15ms
// LEDs definitely updated here

// NEW CODE (async) - usually works as-is:
leds.show();  // Returns immediately
// LEDs MAY NOT be updated yet - DMA still running

// If you need guaranteed completion:
leds.showBlocking();  // Still blocks like old code
// LEDs definitely updated here
```

Most code works fine with async updates. Only change to `showBlocking()` if you have:
- Menus that wait for button input
- Timing-sensitive sequences
- Displays that must be complete before continuing

## Troubleshooting Checklist

- [ ] Called `begin()` on all LED strips?
- [ ] Using correct pin numbers?
- [ ] Power supply adequate (5V, enough current)?
- [ ] Checking `isDMABusy()` before GPIO manipulation?
- [ ] Using `showBlocking()` for blocking menus?
- [ ] Not calling `show()` too frequently (respect `canShow()`)?
- [ ] Not modifying same pixels from multiple cores?
- [ ] Memory barriers present in custom DMA code?

## When to Contact Support

If you experience LED issues after checking the above:
1. Document the exact glitch pattern (video if possible)
2. Note when it occurs (specific operations, timing, frequency)
3. Check recent code changes that might affect LEDs
4. Test with `showBlocking()` to see if issue disappears
5. Monitor `isDMABusy()` state during glitches

Issues that indicate system-level problems:
- All LEDs fail simultaneously
- DMA stuck in busy state (never completes)
- Consistent color corruption across all strips
- LEDs work with `showBlocking()` but not `show()`

These likely indicate hardware or SDK issues, not application bugs.






