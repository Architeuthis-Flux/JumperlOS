"""
Autocomplete Verification Script
=================================

This script tests that your IDE autocomplete is properly configured.
Run this in your IDE and check that:

1. No red squiggles appear under function calls
2. Hovering over functions shows type hints
3. Autocomplete suggestions appear when typing
4. Constants autocomplete (TOP_RAIL, D13, GPIO_1, etc.)

NOTE: This script is for IDE testing only. On the device, the 'if False' 
block is skipped and everything is already globally imported.
"""

# ============================================================================
# IDE Autocomplete Setup (required for IDE support)
# ============================================================================
# type: ignore
#if False:  # Never executes - tells IDE what's globally available
from jumperless import *
import jumperless

# ============================================================================
# Test 1: Basic Function Calls
# ============================================================================
print("Test 1: Basic function calls")

# These should show NO errors in your IDE:
connect(1, 5)
disconnect(1, 5)
nodes_clear()

# ============================================================================
# Test 2: DAC/ADC Functions
# ============================================================================
print("Test 2: DAC/ADC functions")

# Autocomplete should work when you type 'dac_' or 'adc_':
dac_set(DAC0, 3.3)
voltage = adc_get(0)
dac_set(TOP_RAIL, 5.0)

# ============================================================================
# Test 3: GPIO Functions
# ============================================================================
print("Test 3: GPIO functions")

# Type 'gpio_' and you should see all GPIO functions:
gpio_set_dir(GPIO_1, OUTPUT)
gpio_set(GPIO_1, HIGH)
state = gpio_get(GPIO_1)  # Hover: should show '-> GPIOState'

# Aliases should also work:
set_gpio(GPIO_1, LOW)
direction = get_gpio_dir(GPIO_1)  # Hover: should show '-> GPIODirection'

# ============================================================================
# Test 4: PWM Functions
# ============================================================================
print("Test 4: PWM functions")

# Type 'pwm' and see all PWM functions:
pwm(GPIO_1, 1000, 0.5)  # Hover: should show parameters
pwm_set_frequency(GPIO_1, 2000)
pwm_stop(GPIO_1)

# Aliases:
set_pwm(GPIO_2, 500, 0.25)
stop_pwm(GPIO_2)

# ============================================================================
# Test 5: Wavegen Functions
# ============================================================================
print("Test 5: Wavegen functions")

# Type 'wavegen_' and see all wavegen functions:
wavegen_set_output(DAC1)
wavegen_set_wave(SINE)  # SINE should autocomplete
wavegen_set_freq(100)
wavegen_start()

# Check waveform constants autocomplete:
# Type 'SIN' and you should see: SINE
# Type 'TRI' and you should see: TRIANGLE
# Type 'SAW' and you should see: SAWTOOTH
# Type 'SQU' and you should see: SQUARE

wavegen_stop()

# ============================================================================
# Test 6: Node Constants
# ============================================================================
print("Test 6: Node constants")

# Type these prefixes and check autocomplete:
# 'TOP_' → TOP_RAIL, TOP_RAIL_PAD, TOP_GND_PAD
# 'DAC' → DAC0, DAC1, DAC_0, DAC_1, DAC_PAD
# 'GPIO_' → GPIO_1 through GPIO_8, GPIO_20 through GPIO_27
# 'D1' → D10, D11, D12, D13
# 'A' → A0 through A7

rail = TOP_RAIL
ground = GND
dac_output = DAC0
gpio_pin = GPIO_1
arduino_pin = D13
analog_pin = A0

# ============================================================================
# Test 7: Custom Return Types
# ============================================================================
print("Test 7: Custom return types")

# Hover over these variables - IDE should show custom types:
state = gpio_get(1)          # Type: GPIOState
direction = gpio_get_dir(1)  # Type: GPIODirection
pull = gpio_get_pull(1)      # Type: GPIOPull
connected = is_connected(1, 5)  # Type: ConnectionState
pad = probe_read_nonblocking()  # Type: ProbePad
button = probe_button_nonblocking()  # Type: ProbeButton

# These types should work in conditionals:
if state:  # GPIOState is truthy when HIGH
    print("Pin is HIGH")

if connected:  # ConnectionState is truthy when CONNECTED
    print("Nodes are connected")

# ============================================================================
# Test 8: Probe Functions
# ============================================================================
print("Test 8: Probe functions")

# Type 'probe_' and see all probe functions:
# probe_read, probe_read_blocking, probe_read_nonblocking
# probe_button, probe_button_blocking, probe_button_nonblocking
# probe_wait, probe_touch, probe_tap

# Non-blocking version for testing:
pad_touched = probe_read_nonblocking()
button_pressed = check_button()

# ============================================================================
# Test 9: Net Information
# ============================================================================
print("Test 9: Net information")

# Type 'get_net_' and see net functions:
num_nets = get_num_nets()
net_name = get_net_name(0)
net_color = get_net_color_name(0)

# Set operations:
set_net_name(0, "VCC")
set_net_color(0, "red")

# ============================================================================
# Test 10: Helper Functions
# ============================================================================
print("Test 10: Helper functions")

# Python-level helpers should autocomplete:
quick_connect(1, 2, 3, 4)
disconnect_all(1, 2, 3)
voltage_divider(TOP_RAIL, ADC0)

# ============================================================================
# Test 11: OLED and Display
# ============================================================================
print("Test 11: OLED functions")

oled_connect()
oled_print("Test")
oled_print(voltage)  # Can print variables
oled_print(state)    # Can print custom types
oled_clear()
oled_disconnect()

# ============================================================================
# Test 13: Filesystem (JFS)
# ============================================================================
print("Test 13: Filesystem")

# JFS module should autocomplete:
if jfs.exists("/test.txt"):
    f = jfs.open("/test.txt", "r")
    content = f.read()
    f.close()

# Or use context manager:
# with jfs.open("/test.txt", "w") as f:
#     f.write("Hello World\n")

# ============================================================================
# Verification Results
# ============================================================================

print("\n" + "=" * 60)
print("Autocomplete Verification Results")
print("=" * 60)
print("\nIf you see NO RED SQUIGGLES above, your IDE is configured correctly!")
print("\nCheck these features:")
print("  ✓ No 'undefined variable' errors")
print("  ✓ Function signatures appear on hover")
print("  ✓ Autocomplete suggestions when typing")
print("  ✓ Constants autocomplete (TOP_RAIL, D13, etc.)")
print("  ✓ Type hints show for return values")
print("\nTry typing these prefixes and watch autocomplete:")
print("  - 'gpio_' (should show all GPIO functions)")
print("  - 'TOP_' (should show TOP_RAIL and variants)")
print("  - 'DAC' (should show DAC0, DAC1, etc.)")
print("  - 'wavegen_' (should show all wavegen functions)")
print("\n" + "=" * 60)

# ============================================================================
# Next Steps
# ============================================================================

"""
If autocomplete is NOT working:

1. Check that jumperless.pyi is in your project root
2. Verify .vscode/settings.json or pyrightconfig.json is present
3. Reload VS Code window (Cmd+Shift+P → "Reload Window")
4. Check Python language server is set to "Pylance"
5. See IDE_SETUP.md for detailed troubleshooting

If autocomplete IS working:

You're all set! You can now write Jumperless scripts with full IDE support.

Remember:
- Add 'if False: from jumperless import *' to all your scripts
- This line never runs on device - it's only for IDE autocomplete
- On device, everything is already globally imported automatically
- Use help() and nodes_help() in REPL for quick reference
"""

