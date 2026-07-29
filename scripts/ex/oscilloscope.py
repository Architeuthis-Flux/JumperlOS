"""
Jumperless Oscilloscope Demo
=============================

Controls:
- Probe Touch: Connect ADC0 to touched breadboard node (1-60)
- Clickwheel Rotate: Adjust current setting
- Clickwheel Click: Cycle modes (TIME → V/DIV → LEVEL → TRIG → AUTO)
- Probe Button (front): Reset to defaults
- Probe Button (back): Exit

Display:
- Grid with 8x8 divisions
- Status bar: [T] V L M A  value  voltage  @node
- Adjustment overlay (appears for 1 second when changing settings)
- Trigger level indicator (arrow on right, full line when adjusting)

Modes:
1. TIME - Timebase adjustment (50us to 1000ms)
2. V/DIV - Volts per division (0.1V to 5V, disables auto-range)
3. LEVEL - Trigger level (0.0V to 3.3V)
4. TRIG - Trigger mode (FREE/RISE/FALL)
5. AUTO - Auto-ranging toggle (ON/OFF)

API Demonstrations:
- probe_read(False) - Non-blocking probe touch detection
- probe_button(False, True) - Non-blocking button with consume
- clickwheel_get_direction(True) - Rotary encoder with one-shot detection
- clickwheel_get_button() - Button state detection
- connect()/disconnect() - Dynamic node routing
- adc_get() - High-speed analog input sampling
- oled_set_pixel()/oled_show() - Efficient display with single update
- time.ticks_ms()/ticks_diff() - Precise timing for overlays

"""

import time

# ============================================================================
# CONFIGURATION AND STATE
# ============================================================================

# Display parameters
WIDTH, HEIGHT, _ = oled_get_framebuffer_size()
PLOT_HEIGHT = HEIGHT - 8  # Leave room for status bar
PLOT_WIDTH = WIDTH

# Oscilloscope settings
class OscopeState:
    def __init__(self):
        # Sampling settings
        self.timebase_ms = 20.0      # Time per screen in milliseconds
        self.v_per_div = 1.0         # Volts per division (8 divisions vertical)
        self.auto_range = True       # Auto-adjust v_per_div to fit signal
        
        # Trigger settings
        self.trigger_mode = 0        # 0=Free-run, 1=Rising, 2=Falling
        self.trigger_level = 1.65    # Trigger voltage level
        self.trigger_timeout_ms = 50 # Max wait for trigger (reduced for speed)
        
        # Display settings
        self.current_mode = 0        # 0=Timebase, 1=V/div, 2=Trig Level, 3=Trig Mode, 4=Auto
        self.mode_names = ["TIME", "V/DIV", "LEVEL", "TRIG", "AUTO"]
        self.trigger_names = ["FREE", "RISE", "FALL"]
        
        # Overlay settings (for showing adjustments)
        self.show_overlay = False    # Whether to show adjustment overlay
        self.overlay_timer = 0       # Timestamp when overlay was triggered
        self.overlay_duration_ms = 3000  # How long to show overlay
        
        # Statistics printing
        self.last_stats_print = 0    # Last time we printed stats
        self.stats_interval_ms = 25000  # Print stats every 2 seconds
        
        # Connection state
        self.connected_node = 11     # Default connection (row 11)
        self.last_node = None        # Track connection changes
        
        # Sample buffer
        self.samples = [0.0] * PLOT_WIDTH
        self.min_v = 0.0
        self.max_v = 3.3
        self.avg_v = 1.65
        
        # Pre-calculated values for speed
        self.center_y = PLOT_HEIGHT // 2

# Initialize state
state = OscopeState()

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

def init_connections():
    """Initialize ADC connection to default node."""
    print(f"Connecting ADC0 to node {state.connected_node}")
    connect(ADC0, state.connected_node)

def voltage_to_pixel(voltage):
    """Convert voltage to Y pixel coordinate (inverted for display).
    Returns pixel position, or -1 if off-screen (for clipping optimization)."""
    # Center is at PLOT_HEIGHT/2, each division is PLOT_HEIGHT/8
    pixels_per_volt = PLOT_HEIGHT / (state.v_per_div * 8)
    center_voltage = (state.max_v + state.min_v) / 2
    y = int(state.center_y - (voltage - center_voltage) * pixels_per_volt)
    
    # Return -1 if completely off-screen (for clipping detection)
    if y < -2 or y >= PLOT_HEIGHT + 2:
        return -1
    
    # Clamp to valid range
    return max(0, min(PLOT_HEIGHT - 1, y))

def draw_grid():
    """Draw oscilloscope grid with center line and divisions (optimized)."""
    center_y = state.center_y
    
    # Vertical divisions (every 16 pixels for 128 width = 8 divisions)
    # Draw dots at intersections for speed
    for x in range(0, PLOT_WIDTH, 16):
        for y in range(0, PLOT_HEIGHT, 3):
            oled_set_pixel(x, y, 1)
    
    # Horizontal center line (dotted) - most important reference
    for x in range(0, PLOT_WIDTH, 4):
        oled_set_pixel(x, center_y, 1)
    
    # Horizontal quarter divisions
    y_quarter = PLOT_HEIGHT // 4
    y_3quarter = 3 * PLOT_HEIGHT // 4
    for x in range(0, PLOT_WIDTH, 4):
        oled_set_pixel(x, y_quarter, 1)
        oled_set_pixel(x, y_3quarter, 1)

def draw_char(char, x, y):
    """Draw a simple 3x5 character using basic patterns."""
    # Mini font patterns (3x5 pixels) - implementing digits and letters we need
    patterns = {
        'A': [[1,1,1], [1,0,1], [1,1,1], [1,0,1], [1,0,1]],
        'U': [[1,0,1], [1,0,1], [1,0,1], [1,0,1], [1,1,1]],
        'T': [[1,1,1], [0,1,0], [0,1,0], [0,1,0], [0,1,0]],
        'V': [[1,0,1], [1,0,1], [1,0,1], [0,1,0], [0,1,0]],
        'L': [[1,0,0], [1,0,0], [1,0,0], [1,0,0], [1,1,1]],
        'M': [[1,0,1], [1,1,1], [1,0,1], [1,0,1], [1,0,1]],
        'F': [[1,1,1], [1,0,0], [1,1,0], [1,0,0], [1,0,0]],
        'R': [[1,1,0], [1,0,1], [1,1,0], [1,0,1], [1,0,1]],
        'O': [[1,1,1], [1,0,1], [1,0,1], [1,0,1], [1,1,1]],
        'N': [[1,0,1], [1,1,1], [1,1,1], [1,0,1], [1,0,1]],
        '0': [[1,1,1], [1,0,1], [1,0,1], [1,0,1], [1,1,1]],
        '1': [[0,1,0], [1,1,0], [0,1,0], [0,1,0], [1,1,1]],
        '2': [[1,1,1], [0,0,1], [1,1,1], [1,0,0], [1,1,1]],
        '3': [[1,1,1], [0,0,1], [1,1,1], [0,0,1], [1,1,1]],
        '4': [[1,0,1], [1,0,1], [1,1,1], [0,0,1], [0,0,1]],
        '5': [[1,1,1], [1,0,0], [1,1,1], [0,0,1], [1,1,1]],
        '6': [[1,1,1], [1,0,0], [1,1,1], [1,0,1], [1,1,1]],
        '7': [[1,1,1], [0,0,1], [0,0,1], [0,0,1], [0,0,1]],
        '8': [[1,1,1], [1,0,1], [1,1,1], [1,0,1], [1,1,1]],
        '9': [[1,1,1], [1,0,1], [1,1,1], [0,0,1], [1,1,1]],
        '.': [[0,0,0], [0,0,0], [0,0,0], [0,0,0], [0,1,0]],
        ':': [[0,0,0], [0,1,0], [0,0,0], [0,1,0], [0,0,0]],
        ' ': [[0,0,0], [0,0,0], [0,0,0], [0,0,0], [0,0,0]],
        '-': [[0,0,0], [0,0,0], [1,1,1], [0,0,0], [0,0,0]],
        '@': [[1,1,1], [1,0,1], [1,1,1], [1,1,1], [1,1,1]],
        '[': [[0,1,1], [0,1,0], [0,1,0], [0,1,0], [0,1,1]],
        ']': [[1,1,0], [0,1,0], [0,1,0], [0,1,0], [1,1,0]],

        
    }
    
    pattern = patterns.get(char.upper(), patterns[' '])
    for row in range(5):
        for col in range(3):
            if pattern[row][col]:
                oled_set_pixel(x + col, y + row, 1)

def draw_text(text, x, y):
    """Draw text string using mini font."""
    x_offset = 0
    for char in text:
        draw_char(char, x + x_offset, y)
        x_offset += 4  # 3 pixels + 1 space

def draw_status_bar():
    """Draw status bar at bottom with current settings."""
    y_start = PLOT_HEIGHT
    
    # Clear status area
    for y in range(y_start, HEIGHT):
        for x in range(WIDTH):
            oled_set_pixel(x, y, 0)
    
    # Draw separator line
    for x in range(WIDTH):
        oled_set_pixel(x, y_start, 1)
    
    text_y = y_start + 2
    
    # Show mode indicators with highlighting
    # Format: [T] V L M A  val  1.65V @11
    modes = ["T", "V", "L", "M", "A"]
    for i, mode_char in enumerate(modes):
        x_pos = i * 11  # Tighter spacing for 5 modes
        
        # Highlight current mode with brackets
        if i == state.current_mode:
            draw_char('[', x_pos, text_y)
            draw_char(mode_char, x_pos + 4, text_y)
            draw_char(']', x_pos + 8, text_y)
        else:
            draw_char(mode_char, x_pos + 4, text_y)
    
    # Show current value for active mode
    value_text = ""
    if state.current_mode == 0:  # Timebase
        if state.timebase_ms >= 1:
            value_text = f"{int(state.timebase_ms)}m"
        else:
            value_text = f"{int(state.timebase_ms * 1000)}u"
    elif state.current_mode == 1:  # V/div
        auto_indicator = "A" if state.auto_range else ""
        value_text = f"{state.v_per_div:.1f}{auto_indicator}"
    elif state.current_mode == 2:  # Trigger level
        value_text = f"{state.trigger_level:.1f}"
    elif state.current_mode == 3:  # Trigger mode
        value_text = state.trigger_names[state.trigger_mode][:2]
    elif state.current_mode == 4:  # Auto-range
        value_text = "ON" if state.auto_range else "OF"
    
    # Draw value next to modes
    draw_text(value_text, 68, text_y)
    
    # Show voltage reading
    v_text = f"{state.avg_v:.1f}V"
    draw_text(v_text, 92, text_y)
    
    # Show node number
    node_text = f"@{state.connected_node}"
    draw_text(node_text, 112, text_y)

def apply_autoranging():
    """Automatically adjust v_per_div to fit signal on screen."""
    if not state.auto_range:
        return
    
    # Calculate signal range (with 10% margin)
    signal_range = (state.max_v - state.min_v) * 1.1
    
    # Avoid division by zero
    if signal_range < 0.01:
        signal_range = 0.5
    
    # Calculate ideal v_per_div (8 divisions vertically)
    ideal_v_per_div = signal_range / 8
    
    # Snap to standard values
    v_steps = [0.1, 0.2, 0.5, 1.0, 2.0, 5.0]
    for v in v_steps:
        if ideal_v_per_div <= v:
            state.v_per_div = v
            return
    
    # Default to largest
    state.v_per_div = 5.0

def capture_waveform():
    """Capture waveform data with triggering support (optimized)."""
    delay_us = int(state.timebase_ms * 1000 / PLOT_WIDTH)
    
    # Fast trigger detection if not in free-run mode
    if state.trigger_mode != 0:
        start_time = time.ticks_ms()
        last_voltage = adc_get(0)
        trig_level = state.trigger_level
        
        while time.ticks_diff(time.ticks_ms(), start_time) < state.trigger_timeout_ms:
            voltage = adc_get(0)
            
            # Optimized trigger check (avoid branching)
            if state.trigger_mode == 1:  # Rising edge
                if last_voltage < trig_level <= voltage:
                    break
            else:  # Falling edge (mode 2)
                if last_voltage > trig_level >= voltage:
                    break
            
            last_voltage = voltage
            time.sleep_us(50)  # Reduced from 100us for faster trigger response
    
    # Fast sample capture with statistics tracking
    min_v = 3.3
    max_v = 0.0
    sum_v = 0.0
    samples = state.samples  # Local reference for speed
    
    # Optimized sampling loop
    if delay_us > 10:  # Only delay if necessary
        for i in range(PLOT_WIDTH):
            voltage = adc_get(0)
            samples[i] = voltage
            
            # Track statistics (branchless min/max)
            if voltage < min_v:
                min_v = voltage
            if voltage > max_v:
                max_v = voltage
            sum_v += voltage
            
            time.sleep_us(delay_us)
    else:
        # No delay - maximum speed sampling
        for i in range(PLOT_WIDTH):
            voltage = adc_get(0)
            samples[i] = voltage
            if voltage < min_v:
                min_v = voltage
            if voltage > max_v:
                max_v = voltage
            sum_v += voltage
    
    # Update statistics
    state.min_v = min_v
    state.max_v = max_v
    state.avg_v = sum_v / PLOT_WIDTH
    
    # Apply autoranging after capture
    apply_autoranging()

def draw_waveform():
    """Draw the captured waveform on the display with proper clipping."""
    for x in range(PLOT_WIDTH - 1):
        # Convert voltages to pixel coordinates
        y1 = voltage_to_pixel(state.samples[x])
        y2 = voltage_to_pixel(state.samples[x + 1])
        
        # Skip if both points are off-screen (clipping optimization)
        if y1 == -1 and y2 == -1:
            continue
        
        # If one point is off-screen, clamp it
        if y1 == -1:
            y1 = 0 if state.samples[x] < state.min_v else PLOT_HEIGHT - 1
        if y2 == -1:
            y2 = 0 if state.samples[x + 1] < state.min_v else PLOT_HEIGHT - 1
        
        # Draw vertical line segment between points (faster than Bresenham for vertical lines)
        if y1 == y2:
            # Horizontal segment - single pixel
            oled_set_pixel(x, y1, 1)
        else:
            # Vertical segment
            y_min = min(y1, y2)
            y_max = max(y1, y2)
            for y in range(y_min, y_max + 1):
                oled_set_pixel(x, y, 1)

def draw_trigger_indicator():
    """Draw trigger level indicator on display."""
    if state.trigger_mode != 0:  # Not free-run
        y = voltage_to_pixel(state.trigger_level)
        if 0 <= y < PLOT_HEIGHT:
            # Draw trigger marker on right edge (dashed line)
            for x in range(PLOT_WIDTH - 5, PLOT_WIDTH):
                if x % 2 == 0:
                    oled_set_pixel(x, y, 1)
            
            # Draw arrow pointing to trigger level on right edge
            # Arrow: >
            oled_set_pixel(PLOT_WIDTH - 2, y, 1)
            if y > 0:
                oled_set_pixel(PLOT_WIDTH - 3, y - 1, 1)
            if y < PLOT_HEIGHT - 1:
                oled_set_pixel(PLOT_WIDTH - 3, y + 1, 1)
    
    # If adjusting trigger level, draw full-width line for reference
    if state.current_mode == 2 and state.show_overlay:
        y = voltage_to_pixel(state.trigger_level)
        if 0 <= y < PLOT_HEIGHT:
            for x in range(0, PLOT_WIDTH, 2):  # Dashed line across screen
                oled_set_pixel(x, y, 1)


def print_statistics():
    """Periodically print signal statistics to terminal."""
    now = time.ticks_ms()
    if time.ticks_diff(now, state.last_stats_print) >= state.stats_interval_ms:
        state.last_stats_print = now
        
        # Calculate signal characteristics
        peak_to_peak = state.max_v - state.min_v
        
        
        # Print stats with nice formatting
        print("\r                                                                                       \r", end='')
        print(f"Signal Stats @Row{state.connected_node}: ", end='')
        print(f"Avg={state.avg_v:.3f}V  Min={state.min_v:.3f}V  Max={state.max_v:.3f}V  ", end='')
        print(f"Vpp={peak_to_peak:.3f}V  ", end='')
        


def update_display():
    """Complete display update - grid, waveform, status."""
    # Note: oled_clear(False) = don't call show() after clear (prevents flashing)
    oled_clear(False)
    draw_grid()
    draw_trigger_indicator()
    draw_waveform()
    draw_status_bar()
    
    # Show adjustment overlay if active and within duration
    if state.show_overlay:
        elapsed = time.ticks_diff(time.ticks_ms(), state.overlay_timer)
        if elapsed < state.overlay_duration_ms:
            draw_adjustment_overlay()
        else:
            state.show_overlay = False  # Hide overlay after timeout
    
    oled_show()  # Single update for efficiency

# ============================================================================
# CONTROL HANDLERS
# ============================================================================

def handle_probe_touch():
    """Handle non-blocking probe touch to reconnect ADC."""
    # Note: Use positional args - MicroPython C modules don't support keyword args
    pad = probe_read(False)  # Non-blocking read (False = blocking=False)
    
    if pad != NO_PAD:
        # Check if it's a valid breadboard node (1-60)
        pad_num = int(pad)
        if 1 <= pad_num <= 60:
            # Disconnect from old node and connect to new
            if state.last_node is not None:
                disconnect(ADC0, state.last_node)
                print(f"\nProbe touched: Row {pad_num} (disconnected from row {state.last_node})")
            else:
                print(f"\nProbe touched: Row {pad_num}")
            
            state.connected_node = pad_num
            connect(ADC0, state.connected_node)
            state.last_node = state.connected_node
            
            print(f"ADC0 -> Row {state.connected_node} (measuring voltage)")
            time.sleep(0.01)  # Brief delay to show connection

def handle_clickwheel():
    """Handle clickwheel rotation and button for settings adjustment."""
    # Check for mode change (button click)
    button = clickwheel_get_button()
    if button == CLICKWHEEL_PRESSED:
        # Cycle through modes (5 modes now: Time, V/div, Level, Trig, Auto)
        state.current_mode = (state.current_mode + 1) % 5
        
        # Print mode change with description and current value
        mode_descriptions = {
            0: "TIME - Adjust timebase (sweep speed)",
            1: "V/DIV - Adjust volts per division (vertical scale)",
            2: "LEVEL - Adjust trigger level threshold",
            3: "TRIG - Select trigger mode (free/rise/fall)",
            4: "AUTO - Toggle automatic voltage ranging"
        }
        print(f"\nMode: {state.mode_names[state.current_mode]} - {mode_descriptions[state.current_mode]}")
        
        # Print current value for new mode
        if state.current_mode == 0:  # Timebase
            if state.timebase_ms >= 1:
                print(f"   Current: {state.timebase_ms:.0f}ms/screen")
            else:
                print(f"   Current: {state.timebase_ms*1000:.0f}us/screen")
        elif state.current_mode == 1:  # V/div
            auto_status = " (Auto-range: ON)" if state.auto_range else ""
            print(f"   Current: {state.v_per_div:.1f}V/div{auto_status}")
        elif state.current_mode == 2:  # Trigger level
            print(f"   Current: {state.trigger_level:.2f}V")
        elif state.current_mode == 3:  # Trigger mode
            mode_names = ["Free-run (no trigger)", "Rising edge trigger", "Falling edge trigger"]
            print(f"   Current: {mode_names[state.trigger_mode]}")
        elif state.current_mode == 4:  # Auto-range
            status = "ENABLED" if state.auto_range else "DISABLED"
            print(f"   Current: {status}")
        
        # Show overlay when mode changes
        state.show_overlay = True
        state.overlay_timer = time.ticks_ms()
        
        time.sleep(0.01)  # Debounce
    
    # Check for value adjustment (rotation)
    # Note: Use positional args (True = consume=True for one-shot detection)
    direction = clickwheel_get_direction(True)
    
    if direction == CLICKWHEEL_UP:
        adjust_setting(1)
    elif direction == CLICKWHEEL_DOWN:
        adjust_setting(-1)

def draw_adjustment_overlay():
    """Draw a large overlay showing current adjustment."""
    # Clear center area for overlay
    for y in range(8, 20):
        for x in range(20, 108):
            oled_set_pixel(x, y, 0)
    
    # Draw border
    for x in range(20, 108):
        oled_set_pixel(x, 8, 1)
        oled_set_pixel(x, 19, 1)
    for y in range(8, 20):
        oled_set_pixel(20, y, 1)
        oled_set_pixel(107, y, 1)
    
    # Build overlay text based on mode
    if state.current_mode == 0:  # Timebase
        if state.timebase_ms >= 1:
            text = f"TIME: {int(state.timebase_ms)}ms"
        else:
            text = f"TIME: {int(state.timebase_ms * 1000)}us"
    elif state.current_mode == 1:  # V/div
        text = f"V/DIV: {state.v_per_div:.1f}V"
    elif state.current_mode == 2:  # Trigger level
        text = f"LEVEL: {state.trigger_level:.2f}V"
    elif state.current_mode == 3:  # Trigger mode
        text = f"TRIG: {state.trigger_names[state.trigger_mode]}"
    elif state.current_mode == 4:  # Auto-range
        text = f"AUTO: {'ON' if state.auto_range else 'OFF'}"
    
    # Draw text centered in overlay (starting at x=24, y=11)
    draw_text(text, 24, 11)

def adjust_setting(delta):
    """Adjust current setting based on active mode."""
    if state.current_mode == 0:  # Timebase
        # Adjust timebase (exponential steps)
        time_steps = [0.05, 0.1, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000]
        try:
            current_idx = time_steps.index(state.timebase_ms)
        except ValueError:
            current_idx = 4  # Default to 2ms
        
        old_value = state.timebase_ms
        current_idx = max(0, min(len(time_steps) - 1, current_idx + delta))
        state.timebase_ms = time_steps[current_idx]
        
        # Print to terminal with clear update
        print("\r" + " "*80 + "\r", end='')  # Clear line
        if state.timebase_ms >= 1:
            print(f"Timebase: {state.timebase_ms:.0f}ms/screen", end='')
        else:
            print(f"Timebase: {state.timebase_ms*1000:.0f}us/screen", end='')
        
        # Show direction indicator
        if state.timebase_ms > old_value:
            print("  [SLOWER]", end='')
        elif state.timebase_ms < old_value:
            print("  [FASTER]", end='')
        
    elif state.current_mode == 1:  # V/div
        # Disable auto-range when manually adjusting
        was_auto = state.auto_range
        state.auto_range = False
        
        # Adjust voltage per division
        v_steps = [0.1, 0.2, 0.5, 1.0, 2.0, 5.0]
        try:
            current_idx = v_steps.index(state.v_per_div)
        except ValueError:
            current_idx = 3  # Default to 1.0V
        
        old_value = state.v_per_div
        current_idx = max(0, min(len(v_steps) - 1, current_idx + delta))
        state.v_per_div = v_steps[current_idx]
        
        # Print to terminal with clear update
        print("\r" + " "*80 + "\r", end='')  # Clear line
        print(f"V/div: {state.v_per_div:.1f}V/div", end='')
        
        # Show direction and auto-range status
        if state.v_per_div > old_value:
            print("  [ZOOM OUT]", end='')
        elif state.v_per_div < old_value:
            print("  [ZOOM IN]", end='')
        
        if was_auto:
            print("  (auto-range disabled)", end='')
        
    elif state.current_mode == 2:  # Trigger level
        # Adjust trigger level in 0.1V steps
        old_value = state.trigger_level
        state.trigger_level += delta * 0.1
        state.trigger_level = max(0.0, min(3.3, state.trigger_level))
        
        # Print to terminal with clear update
        print("\r" + " "*80 + "\r", end='')  # Clear line
        print(f"Trigger Level: {state.trigger_level:.2f}V", end='')
        
        # Show direction and percentage
        percentage = (state.trigger_level / 3.3) * 100
        print(f"  ({percentage:.0f}% of 3.3V)", end='')
        
        if state.trigger_level > old_value:
            print("  [UP]", end='')
        elif state.trigger_level < old_value:
            print("  [DOWN]", end='')
        
    elif state.current_mode == 3:  # Trigger mode
        # Cycle through trigger modes
        old_mode = state.trigger_mode
        state.trigger_mode = (state.trigger_mode + delta) % 3
        
        # Print to terminal with descriptive names and clear update
        print("\r" + " "*80 + "\r", end='')  # Clear line
        mode_descriptions = ["Free-run (no trigger)", "Rising edge trigger", "Falling edge trigger"]
        mode_symbols = ["FREE", "RISE ↑", "FALL ↓"]
        print(f"Trigger Mode: {mode_descriptions[state.trigger_mode]}", end='')
        
        if old_mode != state.trigger_mode:
            print(f"  [{mode_symbols[state.trigger_mode]}]", end='')
        
    elif state.current_mode == 4:  # Auto-range
        # Toggle auto-range on/off
        state.auto_range = not state.auto_range
        
        # Print to terminal with clear update
        print("\r" + " "*80 + "\r", end='')  # Clear line
        status = "ENABLED" if state.auto_range else "DISABLED"
        print(f"Auto-range: {status}", end='')
        
        # Show what it does
        if state.auto_range:
            print("  (V/div will auto-adjust to fit signal)", end='')
        else:
            print(f"  (V/div locked at {state.v_per_div:.1f}V/div)", end='')
    
    # Mark that we need to show the overlay
    state.show_overlay = True
    state.overlay_timer = time.ticks_ms()

def handle_probe_button():
    """Handle probe button for reset and exit."""
    # Note: Use positional args (False=non-blocking, True=consume)
    button = probe_button(False, True)
    
    if button == CONNECT_BUTTON:
        # Reset to defaults
        print("\n" + "="*50)
        print("RESET TO DEFAULTS")
        print("="*50)
        state.timebase_ms = 20.0
        state.v_per_div = 1.0
        state.trigger_level = 1.65
        state.trigger_mode = 0
        state.auto_range = True
        state.current_mode = 0
        print("   Timebase: 20ms/screen")
        print("   V/div: 1.0V/div")
        print("   Trigger Level: 1.65V")
        print("   Trigger Mode: Free-run")
        print("   Auto-range: ENABLED")
        print("   Mode: TIME")
        time.sleep(0.3)
        return False
    
    elif button == REMOVE_BUTTON:
        # Exit oscilloscope
        print("\n" + "="*50)
        print("Exiting oscilloscope")
        print("="*50)
        return True
    
    return False

# ============================================================================
# MAIN OSCILLOSCOPE LOOP
# ============================================================================

def main():
    """Main oscilloscope loop."""
    print("\n" + "="*60)
    print("JUMPERLESS OSCILLOSCOPE - Full API Demo")
    print("="*60)
    
    print("\nCONTROLS:")
    print("  Touch probe -> Connect ADC0 to breadboard row (1-60)")
    print("  Rotate wheel -> Adjust current setting")
    print("  Click wheel -> Cycle modes (TIME->V/DIV->LEVEL->TRIG->AUTO)")
    print("  Front button -> Reset to defaults")
    print("  Rear button -> Exit")
    
    print("\nMODES:")
    print("  TIME  - Timebase (50us to 1000ms per screen)")
    print("  V/DIV - Volts per division (0.1V to 5V)")
    print("  LEVEL - Trigger level (0V to 3.3V)")
    print("  TRIG  - Trigger mode (FREE/RISE/FALL)")
    print("  AUTO  - Auto-ranging toggle (ON/OFF)")
    
    print("\nCURRENT SETTINGS:")
    print(f"  Timebase: {state.timebase_ms}ms/screen")
    print(f"  V/div: {state.v_per_div}V/div")
    print(f"  Trigger: {state.trigger_names[state.trigger_mode]} @ {state.trigger_level}V")
    print(f"  Auto-range: {'ENABLED' if state.auto_range else 'DISABLED'}")
    print(f"  Connected: ADC0 -> Row {state.connected_node}")
    
    print("\nStarting oscilloscope...")
    print("-"*60 + "\n")
    
    # Initialize
    oled_connect()
    init_connections()
    state.last_node = state.connected_node
    state.last_stats_print = time.ticks_ms()  # Initialize stats timer
    
    # Main loop
    try:
        while True:
            # Handle user inputs (non-blocking)
            handle_probe_touch()      # Check for probe touch
            handle_clickwheel()        # Check for clickwheel input
            if handle_probe_button():  # Check for exit
                break
            
            # Capture and display waveform
            capture_waveform()         # Sample ADC with triggering
            update_display()           # Draw to OLED efficiently
            
            # Print statistics periodically
            print_statistics()         # Show signal measurements in terminal
            
            # Small delay to prevent overwhelming the system
            time.sleep_us(10)
    
    finally:
        # Cleanup 
        oled_clear()
        oled_print("Oscilloscope\nExited", 1)
        print("\n\nCleanup complete")

# ============================================================================
# RUN
# ============================================================================

if __name__ == "__main__":
    main()