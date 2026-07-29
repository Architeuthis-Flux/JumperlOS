# MicroPython Examples for Jumperless

This directory contains example Python scripts that demonstrate various Jumperless features. These examples are automatically embedded into the firmware and can be accessed from the MicroPython REPL.

## 📁 Directory Structure

```
pythonStuff/
├── ex/                      # Example scripts (this directory)
│   ├── adc_basics.py
│   ├── dac_basics.py
│   ├── gpio_basics.py
│   ├── interaction_demo.py
│   ├── led_brightness_control.py
│   ├── node_connections.py
│   ├── stylophone.py
│   ├── uart_basics.py
│   ├── uart_loopback.py
│   └── voltage_monitor.py
├── jumperless_module.py     # Main jumperless module wrapper
└── jumperless.pyi           # Type stub for IDE autocomplete
```

## 🔄 Automatic Generation Workflow

The examples in this directory are automatically converted to C string literals and embedded in the firmware.

### How It Works

1. **Edit Python files** in `pythonStuff/ex/`
2. **Run the generator script**:
   ```bash
   python3 scripts/generate_micropython_examples.py
   ```
3. **Rebuild the firmware** with PlatformIO

The script will:
- Read all `.py` files from `pythonStuff/ex/`
- Convert them to C raw string literals (`R"(...)"`)
- Generate `src/micropythonExamples.h` with all examples
- Include the `jumperless_module.py` wrapper
- Include the `jumperless.pyi` type stub

### Excluded Files

The following files are automatically excluded from the firmware:
- `fuck.py` - Old version of interaction_demo
- `batt.py` - Incomplete battery test
- `lipo_char.py` - Incomplete battery test  
- `listFiles.py` - ViperIDE utility script

To exclude additional files, edit the `exclude_files` set in `scripts/generate_micropython_examples.py`.

## 📝 Example Structure

Each example should follow this structure. Note that the jumperless API (functions and node constants) is auto-imported into globals on the device, so call it bare — no `import jumperless` needed:

```python
"""
Example Title
Brief description of what this example demonstrates.

Hardware Setup:
1. Connection instructions
2. Required components
3. Expected behavior
"""

import time

# Your example code here
print("Example Demo")

# Main loop or demonstration
while True:
    # Do something interesting
    time.sleep(0.1)
```

## 🎯 Available Examples

### Hardware Basics
- **`adc_basics.py`** - Read analog voltages from ADC channels
- **`dac_basics.py`** - Set DAC output voltages
- **`gpio_basics.py`** - Digital I/O, direction control, pull resistors
- **`machine_pin_basics.py`** - The TWO ways to control a GPIO (jumperless `gpio_*()` builtins vs stock `machine.Pin`), ending with a `pin.irq()` interrupt demo
- **`node_connections.py`** - Connect/disconnect nodes, check connections

### Communication
- **`uart_basics.py`** - Basic UART communication
- **`uart_loopback.py`** - UART loopback test

### Interactive Demos
- **`pin_irq_reaction_game.py`** - Reaction-timer game: tap a wire into the breadboard when the LEDs flash; a hard GPIO interrupt timestamps your tap with microsecond precision
- **`pin_irq_freq_counter.py`** - Frequency counter: PWM routed through the crossbar, measured by counting edges in a `Pin.irq(hard=True)` handler; tune with the click wheel
- **`interaction_demo.py`** - Use probe, encoder, and buttons together
- **`led_brightness_control.py`** - Touch-controlled LED brightness
- **`voltage_monitor.py`** - Real-time voltage monitoring with OLED
- **`stylophone.py`** - Musical instrument using probe and GPIO

### System & Hardware Tests
- **`psram_test.py`** - Test PSRAM availability and run memory diagnostics

## 🛠️ Creating New Examples

1. Create a new `.py` file in `pythonStuff/ex/`
2. Add a docstring at the top describing the example
3. Import any needed modules (`time`, `machine`, etc.) — the jumperless API is already in globals
4. Write your example code
5. Run the generator script to update the firmware header
6. Rebuild the firmware

Example template:

```python
"""
My New Example
Description of what this demonstrates.

Hardware Setup:
- List any required connections
"""

import time

print("My New Example")

# Your code here
```

## 🔧 Compile-Time Configuration

You can disable specific examples at compile time by commenting out their `#define` in the generated header:

```c
// In src/micropythonExamples.h
// #define INCLUDE_STYLOPHONE  // Comment out to exclude
#define INCLUDE_GPIO_BASICS    // Keep to include
```

Or use convenience macros to disable entire categories:

```c
#define DISABLE_DEMO_EXAMPLES      // Disables LED, Voltage, Stylophone
#define DISABLE_HARDWARE_EXAMPLES  // Disables DAC, ADC, GPIO, Node
#define DISABLE_MACHINE_EXAMPLES   // Disables UART examples
#define DISABLE_ALL_EXAMPLES       // Disables everything
```

## 📚 Accessing Examples in Firmware

Once the firmware is built and flashed, examples are available in the MicroPython filesystem:

```python
>>> import os
>>> os.listdir('/python_scripts/examples')
['adc_basics.py', 'dac_basics.py', 'gpio_basics.py', ...]

>>> exec(open('/python_scripts/examples/gpio_basics.py').read())
GPIO Basics Demo
Testing GPIO pin 1
...
```

## 🐛 Troubleshooting

### Example not showing up in firmware?
1. Check that the file isn't in the `exclude_files` list
2. Verify the generator script ran successfully
3. Rebuild the firmware completely (`pio run -t clean && pio run`)

### Syntax errors in generated header?
- Check for `)"` sequence in your Python code (breaks raw string literal)
- Ensure your Python file is valid UTF-8
- Look for special characters that might need escaping

### Out of memory when building?
- Disable some examples using `#define DISABLE_*_EXAMPLES`
- Remove large or unused examples from `pythonStuff/ex/`

## 📖 Related Documentation

- [Jumperless API Quick Reference](../JUMPERLESS_API_QUICKREF.md)
- [MicroPython Module Documentation](../MICROPYTHON_MODULE_COMPLETE.md)
- [ViperIDE Setup](../VIPER_IDE_SETUP.md)

## 🔗 Links

- [Jumperless Documentation](https://architeuthis-flux.github.io/Jumperless-docs/)
- [MicroPython Documentation](https://docs.micropython.org/)
- [RP2350 SDK Documentation](https://www.raspberrypi.com/documentation/pico-sdk/)

