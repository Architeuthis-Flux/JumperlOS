# Jumperless MicroPython API - Quick Reference

All functions and constants are globally available without imports.

## 🔌 Hardware Control

### DAC (Digital-to-Analog Converter)
```python
dac_set(channel, voltage, save=True)  # Set voltage (-8.0 to 8.0V)
dac_get(channel)                       # Get voltage
# Aliases: set_dac, get_dac
# Channels: 0-3, DAC0, DAC1, TOP_RAIL, BOTTOM_RAIL
```

### ADC (Analog-to-Digital Converter)
```python
adc_get(channel)  # Read voltage
# Alias: get_adc
# Channels: 0-4 (0-3: 8V range, 4: 5V range)
```

### INA (Current/Power Monitor)
```python
ina_get_current(sensor)      # Read current (A)
ina_get_voltage(sensor)      # Read shunt voltage
ina_get_bus_voltage(sensor)  # Read bus voltage
ina_get_power(sensor)        # Read power (W)
# Aliases: get_current, get_voltage, get_bus_voltage, get_power
# Sensors: 0 or 1
```

### GPIO (General Purpose I/O)
```python
gpio_set(pin, value)          # Set pin HIGH/LOW
gpio_get(pin)                 # Read pin state → GPIOState
gpio_set_dir(pin, direction)  # Set INPUT/OUTPUT
gpio_get_dir(pin)             # Get direction → GPIODirection
gpio_set_pull(pin, pull)      # Set pull resistor (1/0/-1)
gpio_get_pull(pin)            # Get pull config → GPIOPull
# Aliases: set_gpio, get_gpio, set_gpio_dir, etc.
# Pins: 1-8 (GPIO_1-GPIO_8), 9 (UART_TX), 10 (UART_RX)
```

### PWM (Pulse Width Modulation)
```python
pwm(pin, frequency=1000, duty_cycle=0.5)  # Start PWM
pwm_set_frequency(pin, freq)              # Change frequency
pwm_set_duty_cycle(pin, duty)             # Change duty cycle
pwm_stop(pin)                             # Stop PWM
# Aliases: set_pwm, set_pwm_frequency, set_pwm_duty_cycle, stop_pwm
# Pins: 1-8 only (GPIO_1-GPIO_8)
# Frequency: 0.001Hz to 62.5MHz
```

### WaveGen (Waveform Generator)
```python
wavegen_set_output(channel)     # Select output: DAC0/DAC1/TOP_RAIL/BOTTOM_RAIL
wavegen_set_wave(shape)         # SINE/TRIANGLE/SAWTOOTH/SQUARE
wavegen_set_freq(hz)            # 0.0001-10000 Hz
wavegen_set_amplitude(vpp)      # 0-16 Vpp
wavegen_set_offset(v)           # -8 to +8V
wavegen_set_sweep(start, end, sec)  # Frequency sweep
wavegen_start()                 # Start generation
wavegen_stop()                  # Stop generation
wavegen_is_running()            # Check status
# Getters: wavegen_get_output, wavegen_get_freq, etc.
# Aliases: set_wavegen_*, get_wavegen_*
```

## 🔗 Connections

### Node Operations
```python
connect(node1, node2, save=False)  # Connect two nodes
disconnect(node1, node2)           # Disconnect (-1 for all)
nodes_clear()                      # Clear all connections
is_connected(node1, node2)         # Check connection → ConnectionState
node(name_or_id)                   # Create node object
```

### Slot Management
```python
nodes_save(slot=-1)      # Save to slot (default: current)
nodes_discard()          # Discard unsaved changes
nodes_has_changes()      # Check if unsaved changes exist
switch_slot(slot)        # Switch slots (0-7)
CURRENT_SLOT             # Current slot number
```

### Context Control
```python
context_toggle()   # Toggle global/python mode
context_get()      # Get current mode name
# global mode: changes persist after exit
# python mode: changes restored on exit
```

## 📊 Net Information

```python
get_net_name(netNum)            # Get net name
set_net_name(netNum, name)      # Set net name
get_net_color(netNum)           # Get color (0xRRGGBB)
get_net_color_name(netNum)      # Get color name
set_net_color(netNum, color)    # Set by name/hex/RGB
get_num_nets()                  # Count nets
get_num_bridges()               # Count bridges
get_net_nodes(netNum)           # Get nodes in net
get_bridge(idx)                 # Get bridge info
get_net_info(netNum)            # Get full info dict
# Aliases: net_name, net_color, net_info
```

## 🖥️ Display & Interaction

### OLED Display
```python
oled_print(text, size=2)  # Display text (size: 1 or 2)
oled_clear()              # Clear display
oled_show()               # Refresh (usually automatic)
oled_connect()            # Connect I2C
oled_disconnect()         # Disconnect I2C
```

### Probe
```python
probe_read(blocking=True)           # Read pad → ProbePad
probe_read_blocking()               # Wait for touch
probe_read_nonblocking()            # Check immediately
probe_tap(node)                     # Simulate tap
# Aliases: read_probe, probe_wait, wait_probe, probe_touch, wait_touch
```

### Probe Buttons
```python
get_button(blocking=True)      # Get button → ProbeButton
probe_button(blocking=True)    # Same as get_button
probe_button_blocking()        # Wait for press
probe_button_nonblocking()     # Check immediately
check_button()                 # Non-blocking check
# Aliases: button_read, read_button, button_check
```

### Clickwheel
```python
clickwheel_up(clicks=1)    # Scroll up
clickwheel_down(clicks=1)  # Scroll down
clickwheel_press()         # Press button
```

## 📁 Filesystem (JFS)

### File Operations
```python
f = jfs.open(path, mode)   # Open file ('r', 'w', 'a', 'rb', 'wb')
content = f.read(size)     # Read from file
f.write(data)              # Write to file
f.print(*args)             # Print with newline
f.seek(pos, whence)        # Seek in file
pos = f.tell()             # Get position
size = f.size()            # Get file size
avail = f.available()      # Bytes available
f.flush()                  # Flush buffers
f.close()                  # Close file

# Context manager support:
with jfs.open("/test.txt", "w") as f:
    f.write("Hello World")
```

### Directory Operations
```python
jfs.exists(path)           # Check existence
jfs.listdir(path)          # List directory → List[str]
jfs.mkdir(path)            # Create directory
jfs.rmdir(path)            # Remove directory
jfs.remove(path)           # Remove file
jfs.rename(old, new)       # Rename/move
jfs.stat(path)             # Get file stats
total, used, free = jfs.info()  # Filesystem info
```

### Module-Level Functions
```python
fs_exists(path)       # Check if exists
fs_listdir(path)      # List directory
fs_read(path)         # Read file
fs_write(path, data)  # Write file
fs_cwd()              # Get current directory
```

## 🔍 Status & Debug

```python
print_bridges()      # Show all bridges
print_paths()        # Show resolved paths
print_crossbars()    # Show crossbar matrix
print_nets()         # Show nets
print_chip_status()  # Show CH446Q status
```

## ⚙️ System Control

```python
arduino_reset()                # Reset Arduino Nano
run_app(appName)               # Run built-in app
pause_core2(pause)             # Pause/resume Core2
send_raw(chip, x, y, set=1)    # Direct chip control
change_terminal_color(color)   # Change terminal color
cycle_term_color()             # Cycle colors
```

## ❓ Help

```python
help()              # Show all sections
help("GPIO")        # Show GPIO section
help("DAC")         # Show DAC section
help("WAVEGEN")     # Show wavegen section
nodes_help()        # Show node reference
```

## 📌 Constants Reference

### Power Rails
```python
TOP_RAIL, T_RAIL, BOTTOM_RAIL, BOT_RAIL, B_RAIL, GND
```

### DAC Outputs
```python
DAC0, DAC_0, DAC1, DAC_1
```

### ADC Inputs
```python
ADC0, ADC1, ADC2, ADC3, ADC4, ADC7
```

### Current Sense
```python
ISENSE_PLUS, ISENSE_P, I_P, CURRENT_SENSE_PLUS
ISENSE_MINUS, ISENSE_N, I_N, CURRENT_SENSE_MINUS
```

### GPIO Pins
```python
GPIO_1, GPIO_2, GPIO_3, GPIO_4, GPIO_5, GPIO_6, GPIO_7, GPIO_8
GP1, GP2, GP3, GP4, GP5, GP6, GP7, GP8
GPIO_20, GPIO_21, GPIO_22, GPIO_23, GPIO_24, GPIO_25, GPIO_26, GPIO_27
```

### UART
```python
UART_TX, TX, UART_RX, RX
```

### Buffer
```python
BUFFER_IN, BUF_IN, BUFFER_OUT, BUF_OUT
```

### Arduino Pins
```python
D0, D1, D2, D3, D4, D5, D6, D7, D8, D9, D10, D11, D12, D13
A0, A1, A2, A3, A4, A5, A6, A7
NANO_D0-NANO_D13, NANO_A0-NANO_A7  # NANO prefixed versions
```

### GPIO States
```python
HIGH, LOW, FLOATING
```

### GPIO Directions
```python
INPUT, OUTPUT
```

### Waveforms
```python
SINE, TRIANGLE, SAWTOOTH, SQUARE, RAMP, ARBITRARY
```

### Probe Buttons
```python
BUTTON_NONE, BUTTON_CONNECT, BUTTON_REMOVE
CONNECT_BUTTON, REMOVE_BUTTON
```

### Probe Pads
```python
NO_PAD, LOGO_PAD_TOP, LOGO_PAD_BOTTOM
GPIO_PAD, DAC_PAD, ADC_PAD
BUILDING_PAD_TOP, BUILDING_PAD_BOTTOM
D0_PAD-D13_PAD, A0_PAD-A7_PAD
TOP_RAIL_PAD, BOTTOM_RAIL_PAD
# Plus many more nano header pad constants
```

## 🎯 Common Patterns

### Blink LED
```python
gpio_set_dir(GPIO_1, OUTPUT)
for i in range(10):
    gpio_set(GPIO_1, HIGH)
    time.sleep(0.5)
    gpio_set(GPIO_1, LOW)
    time.sleep(0.5)
```

### Read Button
```python
gpio_set_dir(GPIO_2, INPUT)
gpio_set_pull(GPIO_2, PULLUP)
while True:
    if not gpio_get(GPIO_2):  # Pressed when LOW
        print("Button pressed!")
    time.sleep(0.1)
```

### Voltage Monitoring
```python
dac_set(TOP_RAIL, 5.0)
connect(TOP_RAIL, ADC0)
while True:
    v = adc_get(0)
    oled_print(f"{v:.2f}V")
    time.sleep(1)
```

### Generate Waveform
```python
wavegen_set_output(DAC1)
wavegen_set_wave(SINE)
wavegen_set_freq(100)  # 100 Hz
wavegen_set_amplitude(3.3)
wavegen_start()
time.sleep(5)
wavegen_stop()
```

### Interactive Probe
```python
oled_print("Touch pad 1")
pad1 = probe_read()
oled_print("Touch pad 2")
pad2 = probe_read()
connect(pad1, pad2)
oled_print("Connected!")
```

### File I/O
```python
# Write file
with jfs.open("/data.txt", "w") as f:
    f.print("Sensor", "Voltage")
    for i in range(10):
        v = adc_get(0)
        f.print(i, v)

# Read file
with jfs.open("/data.txt", "r") as f:
    content = f.read()
    print(content)
```

## 💡 Pro Tips

### Use Constants for Readability
```python
connect(D13, TOP_RAIL)           # ✓ Clear and autocompletes
connect(83, 101)                 # ✗ Hard to read
```

### Check Return Types
```python
state = gpio_get(1)
if state:                        # Works - GPIOState is truthy when HIGH
    print("HIGH!")

print(state)                     # Prints "HIGH" or "LOW" or "FLOATING"
```

### Use Aliases That Feel Natural
```python
# All equivalent:
gpio_set(1, True)
set_gpio(1, HIGH)
gpio_set(GPIO_1, 1)
```

### Leverage Type Hints (in IDE)
```python
# Hover to see what functions return:
voltage = adc_get(0)      # float
state = gpio_get(1)       # GPIOState
pad = probe_read()        # ProbePad
info = get_net_info(0)    # Dict[str, Union[str, int]]
```

### Use Help System
```python
help()              # Overview
help("GPIO")        # GPIO section
nodes_help()        # All node names
dir()               # List everything
```

## � Board State
```python
state_json = get_state()
set_state(state_json)
# from_wokwi parameter when needed
```

## �🔧 Advanced GPIO & Pin Utilities
```python
# Floating-read enable/disable
gpio_set_read_floating(pin, True)
state = gpio_get_read_floating(pin)
# Pin ownership for timing-critical operations
gpio_claim_pin(pin)
gpio_release_pin(pin)
gpio_release_all_pins()
```

## ⚡ Fast Node Operations
```python
fast_connect(node1, node2)       # skip LED update
fast_disconnect(node1, node2)    # skip LED update
```

## 🌐 Network & Path Queries
```python
set_net_color_hsv(netNum, h, s, v)
get_all_nets()
get_num_paths()
get_path_info(idx)
get_all_paths()
get_path_between(node1, node2)
```

## 🧪 Fake GPIO Support
```python
# Class constructors
fg = FakeGpioPin( node )
# Mode constants
FAKE_GPIO_INPUT
FAKE_GPIO_OUTPUT
```

## 🖼️ OLED Extras
```python
oled_set_text_size(size)
oled_get_text_size()
oled_copy_print(True)
oled_get_fonts()
oled_set_font("fontname")
oled_get_current_font()
oled_load_bitmap("file.bmp")
oled_display_bitmap(x, y, w, h, data)
oled_show_bitmap_file("file.bmp", x, y)
oled_get_framebuffer()
oled_set_framebuffer(data, len)
oled_get_framebuffer_size()
oled_set_pixel(x,y,color)
oled_get_pixel(x,y)
```

## 🖥️ Graphic Overlays
```python
overlay_set(name, row, col, w, h, colors)
overlay_clear(name)
overlay_clear_all()
overlay_set_pixel(row, col, color)
overlay_count()
overlay_shift(name, dr, dc)
overlay_place(name, row, col)
overlay_serialize()
```

## ⚙️ System & Service Helpers
```python
force_service("name")
force_service_by_index(idx)
get_service_index("name")
get_switch_position()
set_switch_position(pos)
check_switch_position()
```

## 🎛️ Clickwheel Extras
```python
clickwheel_get_position()
clickwheel_reset_position()
clickwheel_get_direction()
clickwheel_get_button()
clickwheel_is_initialized()
```

---

**Full Documentation:** See `docs/09.5-micropythonAPIreference.md`  
**User Guide:** See `docs/08-micropython.md`  
**IDE Setup:** See `IDE_SETUP.md`

