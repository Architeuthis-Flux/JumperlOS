# Core 0 Freeze Debug Status

## Current Status: UNDER INVESTIGATION

**Note:** The UART framing/crash-on-boot issue is now fixed (see ASYNC_PASSTHROUGH_CRASH_DEBUG.md). This document tracks a separate, intermittent Core 0 freeze issue that can occur during rapid command processing.

---

## The Problem

**Core 0 freezes randomly during UART command processing, while Core 2 continues running.**

- USB disconnects because `tud_task()` stops being called
- LEDs keep animating (Core 2 still works)
- Freeze happens at unpredictable times - sometimes after 10 commands, sometimes after 200+
- NOT a hard crash - MCU is still running, just Core 0 is stuck
- More stable with longer delays between commands (~400ms+)
- Faster commands = more likely to freeze

---

## What We've Ruled Out

### ✅ UART Framing Issues (FIXED)
- Aggressive startup resync sequence
- Runtime framing error detection
- Garbage byte filtering
- See ASYNC_PASSTHROUGH_CRASH_DEBUG.md for details

### ✅ NOT USB Debug Output Pressure
- Removed ALL debug prints
- Freeze still happens with zero debug output

### ✅ NOT Command Relay Race Conditions
- Refactored to synchronous `CommandBuffer` system
- Single pending command slot, no queues
- Freeze still happens

### ✅ NOT RelayedCommandService Racing
- Disabled `RelayedCommandService` entirely
- Commands now processed synchronously in main loop
- Freeze still happens

### ✅ NOT UART IRQ Priority
- Tested with priority 0 (highest) - freezes
- Tested with priority 64 (lower) - freezes
- Tested with shared handler mode - freezes

---

## Current Architecture

### Command Flow (Working)
```
Arduino sends: <p>adc_get(0)</p>
         ↓
UART IRQ → uartReceived[] ring buffer (4KB)
         ↓
AsyncPassthrough::task() → parses tags
         ↓
CommandBuffer::setPendingPCommand("adc_get(0)")
         ↓
Main loop → CommandBuffer::hasPendingCommand()
         ↓
consumePendingCommand() → executeCommand()
         ↓
MicroPython executes → output to Serial AND UART
```

### UART Response (Working!)
- Arduino receives ADC values correctly
- Response capture in `mp_hal_stdout_tx_strn_cooked()` works

---

## Likely Causes (Still to Investigate)

### 1. MicroPython Execution Blocking
- `mp_embed_exec_str()` runs Python code synchronously
- `gc.collect()` is called after each command
- Could GC be causing issues? Heap exhaustion?
- Could something in MicroPython be blocking indefinitely?

### 2. Something in the Service Loop
- `jOS.serviceAll()` runs all services
- One of the services might be blocking or corrupting state
- Services: TermSerial, AsyncPassthrough, TinyUSB, USBPeriodic, OLED

### 3. Cross-Core Synchronization
- `waitCore2()` type functions could deadlock
- Spinlocks between cores
- `core2busy`, `sendAllPathsCore2`, `showLEDsCore2` flags

### 4. Memory Corruption / Stack Overflow
- Something corrupting return addresses
- Stack overflow on Core 0
- Heap corruption from MicroPython

### 5. TinyUSB Internal State
- `tud_task()` might be getting into bad state
- CDC buffer corruption
- USB stack internal deadlock

### 6. Flash/Filesystem Operations
- Other hidden flash writes slowing things down?

---

## Debug Approach Suggestions

### Add Watchdog
```cpp
// In main loop - detect if stuck
static unsigned long lastLoopTime = 0;
if (millis() - lastLoopTime > 1000) {
    // Core 0 was stuck, try to recover
}
lastLoopTime = millis();
```

### Isolate Services
Disable services one by one to find culprit.

### Check Stack Usage
Monitor stack high water mark periodically.

### MicroPython Isolation Test
Try running commands WITHOUT MicroPython - have `<j>` tags run a simple C function instead.

### UART-Only Test
Disable USB Serial output entirely - removes TinyUSB from the equation.

---

## Key Observations

1. **Timing matters** - Longer delays = more stable
2. **Core 2 keeps running** - Not a full system crash
3. **USB disconnects** - Because tud_task() stops
4. **Random location** - Not in any specific code path
5. **Debug output masks it** - Slowing down hides the bug
6. **UART responses work** - Data flows both directions correctly

---

## Test Sketch (Arduino)

```cpp
#define OPENJCOMMAND Serial.print("<p>");
#define CLOSEJCOMMAND Serial.println("</p>");

int lastNode = 8;
int node = 8;

void loop() {
    node++;
    if (node > 60) node = 1;

    OPENJCOMMAND
    Serial.print("disconnect( ADC0," + String(lastNode) + ")");
    CLOSEJCOMMAND
    delay(400);  // Stable at 400ms, crashes faster

    OPENJCOMMAND
    Serial.print("connect(ADC0 ," + String(node) + ")");
    CLOSEJCOMMAND
    delay(400);

    OPENJCOMMAND
    Serial.print("adc_get(0)");
    CLOSEJCOMMAND
    delay(40);

    // Read response
    char response[30] = {0};
    int idx = 0;
    while(Serial.available() > 0 && idx < 29) {
        response[idx++] = Serial.read();
        delay(5);
    }
    Serial.println(response);
    Serial.print("num chars read = ");
    Serial.println(idx);

    lastNode = node;
}
```

---

## Key File Locations

- **Tag parsing**: `AsyncPassthrough.cpp` ~line 430
- **UART IRQ handler**: `AsyncPassthrough.cpp` `async_uart_irq_handler()`
- **Command buffer**: `CommandBuffer.cpp/h`
- **Main loop execution**: `main.cpp` ~line 794
- **Python execution**: `SingleCharCommands.cpp` `cmd_pythonCommand()`
- **Core 2 main loop**: `main.cpp` `loop1()` and `core2stuff()`

---

*Last updated: Current session*
*Status: UART framing fixed, intermittent freeze still under investigation*
