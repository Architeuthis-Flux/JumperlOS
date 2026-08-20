"""
Type stub for Jumperless MicroPython Module
============================================

This file provides type hints and autocomplete support for the jumperless module.
All functions and constants are available globally without needing to import.
"""

from typing import Union, Tuple, Dict, List, Optional

# ============================================================================
# Custom Types
# ============================================================================

class GPIOState:
    """GPIO state: HIGH, LOW, or FLOATING"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...

class GPIODirection:
    """GPIO direction: INPUT or OUTPUT"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...

class GPIOPull:
    """GPIO pull configuration: PULLUP, PULLDOWN, or NONE"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...

class ConnectionState:
    """Connection state: CONNECTED or DISCONNECTED"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...

class ProbeButton:
    """Probe button state: CONNECT, REMOVE, or NONE"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...

class ProbePad:
    """Probe pad: numbered pads (1-60) or special pads"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...
    def __eq__(self, other) -> bool: ...
    def __ne__(self, other) -> bool: ...
    def __add__(self, other) -> str: ...

class Node:
    """Node object for hardware connections"""
    def __bool__(self) -> bool: ...
    def __int__(self) -> int: ...
    def __float__(self) -> float: ...
    def __str__(self) -> str: ...
    def __eq__(self, other) -> bool: ...
    def __ne__(self, other) -> bool: ...

# Type aliases for function parameters
NodeRef = Union[int, str, Node]
DACChannel = Union[int, NodeRef]
ADCChannel = int
INASensor = int
GPIOPin = Union[int, NodeRef]
PWMPin = int

# ============================================================================
# DAC Functions
# ============================================================================

def dac_set(channel: DACChannel, voltage: float, save: bool = True) -> None:
    """Set DAC output voltage (-8.0 to 8.0V)
    
    Args:
        channel: 0-3, or DAC0, DAC1, TOP_RAIL, BOTTOM_RAIL
        voltage: Output voltage in volts (-8.0 to 8.0)
        save: Save to config file (default: True)
    """
    ...

def dac_get(channel: DACChannel) -> float:
    """Get DAC output voltage
    
    Args:
        channel: 0-3, or DAC0, DAC1, TOP_RAIL, BOTTOM_RAIL
        
    Returns:
        Current voltage setting in volts
    """
    ...

def set_dac(channel: DACChannel, voltage: float, save: bool = True) -> None:
    """Alias for dac_set()"""
    ...

def get_dac(channel: DACChannel) -> float:
    """Alias for dac_get()"""
    ...

# ============================================================================
# ADC Functions
# ============================================================================

def adc_get(channel: ADCChannel) -> float:
    """Read ADC input voltage
    
    Args:
        channel: 0-4 (0-3: 8V range, 4: 5V range)
        
    Returns:
        Measured voltage in volts
    """
    ...

def get_adc(channel: ADCChannel) -> float:
    """Alias for adc_get()"""
    ...

# ============================================================================
# INA Current/Power Monitor Functions
# ============================================================================

def ina_get_current(sensor: INASensor) -> float:
    """Read current in amperes"""
    ...

def ina_get_voltage(sensor: INASensor) -> float:
    """Read shunt voltage"""
    ...

def ina_get_bus_voltage(sensor: INASensor) -> float:
    """Read bus voltage"""
    ...

def ina_get_power(sensor: INASensor) -> float:
    """Read power in watts"""
    ...

def get_ina_current(sensor: INASensor) -> float:
    """Alias for ina_get_current()"""
    ...

def get_ina_voltage(sensor: INASensor) -> float:
    """Alias for ina_get_voltage()"""
    ...

def get_ina_bus_voltage(sensor: INASensor) -> float:
    """Alias for ina_get_bus_voltage()"""
    ...

def get_ina_power(sensor: INASensor) -> float:
    """Alias for ina_get_power()"""
    ...

def get_current(sensor: INASensor) -> float:
    """Alias for ina_get_current()"""
    ...

def get_voltage(sensor: INASensor) -> float:
    """Alias for ina_get_voltage()"""
    ...

def get_bus_voltage(sensor: INASensor) -> float:
    """Alias for ina_get_bus_voltage()"""
    ...

def get_power(sensor: INASensor) -> float:
    """Alias for ina_get_power()"""
    ...

# ============================================================================
# GPIO Functions
# ============================================================================

def gpio_set(pin: GPIOPin, value: Union[bool, int, GPIOState]) -> None:
    """Set GPIO pin state (HIGH/LOW)"""
    ...

def gpio_get(pin: GPIOPin) -> GPIOState:
    """Read GPIO pin state (returns HIGH/LOW/FLOATING)"""
    ...

def gpio_set_dir(pin: GPIOPin, direction: Union[bool, int, str, GPIODirection]) -> None:
    """Set GPIO pin direction (True/OUTPUT or False/INPUT)"""
    ...

def gpio_get_dir(pin: GPIOPin) -> GPIODirection:
    """Get GPIO pin direction (returns INPUT or OUTPUT)"""
    ...

def gpio_set_pull(pin: GPIOPin, pull: Union[int, str, GPIOPull]) -> None:
    """Set GPIO pull resistor (1/PULLUP, -1/PULLDOWN, 0/NONE)"""
    ...

def gpio_get_pull(pin: GPIOPin) -> GPIOPull:
    """Get GPIO pull resistor (returns PULLUP, PULLDOWN, or NONE)"""
    ...

# GPIO Aliases
def set_gpio(pin: GPIOPin, value: Union[bool, int, GPIOState]) -> None:
    """Alias for gpio_set()"""
    ...

def get_gpio(pin: GPIOPin) -> GPIOState:
    """Alias for gpio_get()"""
    ...

def set_gpio_dir(pin: GPIOPin, direction: Union[bool, int, str, GPIODirection]) -> None:
    """Alias for gpio_set_dir()"""
    ...

def get_gpio_dir(pin: GPIOPin) -> GPIODirection:
    """Alias for gpio_get_dir()"""
    ...

def set_gpio_pull(pin: GPIOPin, pull: Union[int, str, GPIOPull]) -> None:
    """Alias for gpio_set_pull()"""
    ...

def get_gpio_pull(pin: GPIOPin) -> GPIOPull:
    """Alias for gpio_get_pull()"""
    ...

def gpio_set_read_floating(pin: GPIOPin, enable: bool) -> None:
    """Set whether GPIO reads as floating when disconnected"""
    ...

def gpio_get_read_floating(pin: GPIOPin) -> bool:
    """Get whether GPIO reads as floating when disconnected"""
    ...

def set_gpio_read_floating(pin: GPIOPin, enable: bool) -> None:
    """Alias for gpio_set_read_floating()"""
    ...

def get_gpio_read_floating(pin: GPIOPin) -> bool:
    """Alias for gpio_get_read_floating()"""
    ...

def gpio_claim_pin(pin: GPIOPin) -> None:
    """Claim pin for timing-critical use (e.g. NeoPixels); release when done"""
    ...

def gpio_release_pin(pin: GPIOPin) -> None:
    """Release a previously claimed pin"""
    ...

def gpio_release_all_pins() -> None:
    """Release all claimed GPIO pins"""
    ...

# ============================================================================
# PWM Functions
# ============================================================================

def pwm(pin: PWMPin, frequency: float = 1000.0, duty_cycle: float = 0.5) -> None:
    """Setup PWM on GPIO pin (pins 1-8 only)
    
    Args:
        pin: GPIO pin 1-8
        frequency: 0.001Hz to 62.5MHz (default 1000Hz)
        duty_cycle: 0.0 to 1.0 (default 0.5 = 50%)
    """
    ...

def pwm_set_duty_cycle(pin: PWMPin, duty_cycle: float) -> None:
    """Set PWM duty cycle (0.0 to 1.0)"""
    ...

def pwm_set_frequency(pin: PWMPin, frequency: float) -> None:
    """Set PWM frequency (0.001Hz to 62.5MHz)"""
    ...

def pwm_stop(pin: PWMPin) -> None:
    """Stop PWM on pin"""
    ...

# PWM Aliases
def set_pwm(pin: PWMPin, frequency: float = 1000.0, duty_cycle: float = 0.5) -> None:
    """Alias for pwm()"""
    ...

def set_pwm_duty_cycle(pin: PWMPin, duty_cycle: float) -> None:
    """Alias for pwm_set_duty_cycle()"""
    ...

def set_pwm_frequency(pin: PWMPin, frequency: float) -> None:
    """Alias for pwm_set_frequency()"""
    ...

def stop_pwm(pin: PWMPin) -> None:
    """Alias for pwm_stop()"""
    ...

# ============================================================================
# Waveform Generator Functions
# ============================================================================

def wavegen_set_output(channel: DACChannel) -> None:
    """Set wavegen output: DAC0, DAC1, TOP_RAIL, or BOTTOM_RAIL"""
    ...

def wavegen_set_freq(hz: float) -> None:
    """Set frequency (0.0001 to 10000 Hz)"""
    ...

def wavegen_set_wave(shape: Union[int, str]) -> None:
    """Set waveform shape: SINE, TRIANGLE, SAWTOOTH, SQUARE"""
    ...

def wavegen_set_sweep(start_hz: float, end_hz: float, seconds: float) -> None:
    """Configure linear frequency sweep"""
    ...

def wavegen_set_amplitude(vpp: float) -> None:
    """Set amplitude (0.0 to 16.0 Vpp)"""
    ...

def wavegen_set_offset(v: float) -> None:
    """Set DC offset (-8.0 to +8.0 V)"""
    ...

def wavegen_start(run: bool = True) -> None:
    """Start waveform generation"""
    ...

def wavegen_stop() -> None:
    """Stop waveform generation"""
    ...

def wavegen_get_output() -> int:
    """Get current output channel"""
    ...

def wavegen_get_freq() -> float:
    """Get current frequency"""
    ...

def wavegen_get_wave() -> int:
    """Get current waveform"""
    ...

def wavegen_get_amplitude() -> float:
    """Get current amplitude"""
    ...

def wavegen_get_offset() -> float:
    """Get current offset"""
    ...

def wavegen_is_running() -> bool:
    """Check if wavegen is active"""
    ...

# Wavegen Aliases
def set_wavegen_output(channel: DACChannel) -> None:
    """Alias for wavegen_set_output()"""
    ...

def set_wavegen_freq(hz: float) -> None:
    """Alias for wavegen_set_freq()"""
    ...

def set_wavegen_wave(shape: Union[int, str]) -> None:
    """Alias for wavegen_set_wave()"""
    ...

def set_wavegen_sweep(start_hz: float, end_hz: float, seconds: float) -> None:
    """Alias for wavegen_set_sweep()"""
    ...

def set_wavegen_amplitude(vpp: float) -> None:
    """Alias for wavegen_set_amplitude()"""
    ...

def set_wavegen_offset(v: float) -> None:
    """Alias for wavegen_set_offset()"""
    ...

def start_wavegen(run: bool = True) -> None:
    """Alias for wavegen_start()"""
    ...

def stop_wavegen() -> None:
    """Alias for wavegen_stop()"""
    ...

def get_wavegen_output() -> int:
    """Alias for wavegen_get_output()"""
    ...

def get_wavegen_freq() -> float:
    """Alias for wavegen_get_freq()"""
    ...

def get_wavegen_wave() -> int:
    """Alias for wavegen_get_wave()"""
    ...

def get_wavegen_amplitude() -> float:
    """Alias for wavegen_get_amplitude()"""
    ...

def get_wavegen_offset() -> float:
    """Alias for wavegen_get_offset()"""
    ...

# ============================================================================
# Node Connection Functions
# ============================================================================

def node(name_or_id: Union[str, int, Node]) -> Node:
    """Create a node object from string name or integer ID"""
    ...

def connect(node1: NodeRef, node2: NodeRef, duplicates: int = -1) -> None:
    """Connect two nodes
    
    Args:
        node1, node2: Nodes to connect (int, string, or Node constant)
        duplicates: Duplicate connection behavior (default: -1)
            -1: Just add the connection (standard behavior, no duplicate management)
            0: Force exactly 0 duplicates (remove any existing duplicates)
            1+: Force exactly N duplicates (add/remove connections to reach count)
    
    Example:
        connect(1, 5)              # Add connection without managing duplicates
        connect(1, 5, duplicates=0)  # Ensure no duplicate paths exist
        connect(1, 5, duplicates=2)  # Force exactly 2 duplicate paths
    """
    ...

def disconnect(node1: NodeRef, node2: NodeRef) -> None:
    """Disconnect two nodes (set node2 to -1 to disconnect all from node1)"""
    ...

def fast_connect(node1: NodeRef, node2: NodeRef, duplicates: int = -1) -> None:
    """Connect two nodes, skipping LED computation
    
    This function adds connections without updating LED state. Useful when making
    many connections at once - you can defer LED updates until all connections are done.
    
    Note: This is not much faster overall, it just skips LED updates for bulk operations.
    
    Args:
        node1, node2: Nodes to connect (int, string, or Node constant)
        duplicates: Same behavior as connect() (default: -1)
    
    Example:
        # Make multiple connections without LED updates
        for i in range(10):
            fast_connect(i, i+10)
        # LEDs update automatically after loop completes
    """
    ...

def fast_disconnect(node1: NodeRef, node2: NodeRef) -> None:
    """Disconnect two nodes, skipping LED computation
    
    Same LED-skipping behavior as fast_connect(). Useful for bulk disconnections.
    
    Args:
        node1, node2: Nodes to disconnect
    """
    ...

def nodes_clear() -> None:
    """Clear all connections"""
    ...

def is_connected(node1: NodeRef, node2: NodeRef) -> ConnectionState:
    """Check if nodes are connected (returns CONNECTED or DISCONNECTED)"""
    ...

def nodes_save(slot: int = -1) -> int:
    """Save connections to slot (default: current slot)
    
    Returns:
        Slot number that was saved to
    """
    ...



def nodes_has_changes() -> bool:
    """Check if there are unsaved changes"""
    ...

# ============================================================================
# Net Information Functions
# ============================================================================

def get_net_name(netNum: int) -> Optional[str]:
    """Get the name of a net"""
    ...

def set_net_name(netNum: int, name: Optional[str]) -> None:
    """Set a custom net name (pass None or empty string to reset)"""
    ...

def get_net_color(netNum: int) -> int:
    """Get net color as 0xRRGGBB hex value"""
    ...

def get_net_color_name(netNum: int) -> str:
    """Get net color as human-readable string"""
    ...

def set_net_color(netNum: int, color: Union[str, int], r: int = None, g: int = None, b: int = None) -> bool:
    """Set net color by name, hex string, or RGB values
    
    Args:
        netNum: Net number
        color: Color name ("red", "blue"), hex string ("#FF0000"), or RGB int
        r, g, b: Optional RGB values (0-255)
        
    Returns:
        True on success, False on failure
    """
    ...

def get_num_nets() -> int:
    """Get number of active nets"""
    ...

def get_num_bridges() -> int:
    """Get number of bridges"""
    ...

def get_net_nodes(netNum: int) -> str:
    """Get comma-separated list of nodes in a net"""
    ...

def get_bridge(bridgeIdx: int) -> Tuple[int, int, int]:
    """Get bridge info as tuple (node1, node2, duplicates)"""
    ...

def get_net_info(netNum: int) -> Dict[str, Union[str, int]]:
    """Get full net info as dict with keys: name, number, color, color_name, nodes"""
    ...

# Net Info Aliases
def net_name(netNum: int) -> Optional[str]:
    """Alias for get_net_name()"""
    ...

def net_color(netNum: int) -> int:
    """Alias for get_net_color()"""
    ...

def net_info(netNum: int) -> Dict[str, Union[str, int]]:
    """Alias for get_net_info()"""
    ...

def get_all_nets() -> List[Dict[str, Union[str, int]]]:
    """Get list of all nets with full info"""
    ...

def set_net_color_hsv(netNum: int, h: float, s: float = -1, v: float = -1) -> bool:
    """Set net color using HSV color space (auto-detects 0.0-1.0 vs 0-255 range)
    
    Args:
        netNum: The net number
        h: Hue value (0.0-1.0 or 0-255, auto-detected based on value)
        s: Saturation (0.0-1.0 or 0-255, default: max saturation if not provided)
        v: Value/brightness (0.0-1.0 or 0-255, default: 32 for reasonable brightness)
    
    Returns:
        True on success, False on failure
    
    Example:
        set_net_color_hsv(0, 0.0)           # Red at default brightness
        set_net_color_hsv(1, 0.33)          # Green  
        set_net_color_hsv(2, 0.66, 1.0, 1.0)  # Blue at max brightness
    """
    ...

# ============================================================================
# Path Query Functions
# ============================================================================

def get_num_paths(include_duplicates: bool = True) -> int:
    """Get the number of routing paths
    
    Args:
        include_duplicates: If True, count all paths. If False, count only primary paths.
    
    Returns:
        Number of paths
    
    Example:
        total = get_num_paths()           # All paths including duplicates
        primary = get_num_paths(False)    # Only primary paths
    """
    ...

def get_path_info(path_idx: int) -> Optional[Dict]:
    """Get detailed routing path information
    
    Args:
        path_idx: Path index (0 to get_num_paths()-1)
    
    Returns:
        Dict with keys: node1, node2, net, chips, x, y, duplicate
        Returns None if index is invalid
    
    Example:
        path = get_path_info(0)
        if path:
            print(f"Path from {path['node1']} to {path['node2']}")
    """
    ...

def get_all_paths() -> List[Dict]:
    """Get all routing paths as a list of dicts
    
    Returns:
        List of path dicts, same format as get_path_info()
    
    Example:
        for path in get_all_paths():
            print(f"{path['node1']} -> {path['node2']}")
    """
    ...

def get_path_between(node1: int, node2: int) -> Optional[Dict]:
    """Query the routing path between two specific nodes
    
    Args:
        node1, node2: Nodes to query path between
    
    Returns:
        Path dict if found, None otherwise
    
    Example:
        path = get_path_between(1, 5)
        if path:
            print(f"Route uses chips: {path['chips']}")
    """
    ...

# ============================================================================
# Net Current Scan (background voltage/current sensing)
# ============================================================================

def get_node_voltage(node: Union[int, str, "Node"]) -> Optional[float]:
    """Get the voltage of a routed node measured by the background net
    voltage scan (enabled by [display] net_currents / the 'i' command)

    Args:
        node: Node number, name, or node object (e.g. 15, "D2", TOP_RAIL)

    Returns:
        Voltage in volts, or None when the scan has no fresh sample
        (scan disabled, node not routed, or the node is floating)

    Example:
        v = get_node_voltage(15)
        if v is not None:
            print(f"Row 15 sits at {v:.2f} V")
    """
    ...

def get_net_current(netNum: int) -> Optional[Dict]:
    """Get the current flowing in a net's dominant path

    Args:
        netNum: Net number (1 to get_num_nets())

    Returns:
        Dict with keys: current_mA (magnitude), voltage (midpoint),
        from_node, to_node (conventional current flows from -> to).
        None when the scan has no data for this net.

    Example:
        info = get_net_current(4)
        if info:
            print(f"{info['current_mA']:.2f} mA from {info['from_node']} to {info['to_node']}")
    """
    ...

def get_path_current(path_idx: int) -> Optional[float]:
    """Get the signed current through one routing path

    Args:
        path_idx: Path index (same index space as get_path_info())

    Returns:
        Current in mA; positive means conventional current flows
        node1 -> node2 (matching get_path_info() fields).
        None when the scan has no data for this path.

    Example:
        i = get_path_current(0)
        if i is not None:
            print(f"Path 0 carries {abs(i):.2f} mA")
    """
    ...

# ============================================================================
# Slot Management
# ============================================================================

def switch_slot(slot: int) -> int:
    """Switch to a different slot (0-7)
    
    Returns:
        Previous slot number
    """
    ...

CURRENT_SLOT: int
"""Current active slot number"""

# ============================================================================
# Projects and Parts (guided placement)
# ============================================================================

def load_project(name_or_path: str) -> bool:
    """Load a project's wiring into the live state

    A project wiring.yaml IS a slot YAML, so this takes the same loader the
    Files browser uses: bridges, nets, power and any `parts:` section are
    applied and routed immediately.

    Args:
        name_or_path: "555" -> /projects/555/wiring.yaml.
                      Anything containing '/' is used as a literal path.

    Returns:
        True if the file loaded, False otherwise (the reason is printed)

    Example:
        load_project("555")
        load_project("/slots/slot3.yaml")
    """
    ...

def place_part(name: str, row: int, pins_json: str, footprint: str = "",
               type: str = "", value: str = "") -> int:
    """Place a part and expand its pins into bridges

    Args:
        name: Part name, 1-15 characters, unique (remove_part first to replace)
        row: Breadboard row of pin 1 (1-60)
        pins_json: Pins map, e.g.
            '{"A": {"pin": 1, "connect": "GND"}, "B": {"pin": 2, "connect": 7}}'
            pin:     1-based physical pin, placed by the footprint geometry
            offset:  same-side offset from row (wins over pin: when >= 0)
            connect: row 1-60 or a node name (GND, TOP_RAIL, DAC0, ...);
                     omit it to let the leg occupy the hole with no bridge
            class:   "signal" (default), "power", "gnd" or "nc"
        footprint: "dip8" / "sip2" - default "" infers a SIP strip sized from
                   the highest pin/offset listed
        type: Optional part type (resistor|capacitor|diode|led|bjt|fet|ic)
        value: Optional value string ("10k", "NE555")

    Returns:
        0 on success, -1 on failure (the reason is printed)

    Example:
        place_part("U9", 5, '{"A": {"pin": 1, "connect": "GND"}, '
                            '"B": {"pin": 2, "connect": 7}}')
        place_part("U1", 5, '{"GND": {"pin": 1, "connect": "GND"}}',
                   "dip8", "ic", "NE555")
    """
    ...

def remove_part(name: str) -> int:
    """Remove a part: its expansion bridges, its {NAME}_{PIN} net names and its entry

    Returns:
        0 on success, -1 when there is no part with that name
    """
    ...

def list_parts() -> List[Dict]:
    """List the parts in the loaded state

    Returns:
        A list of dicts:
        {'name', 'type', 'value', 'row', 'footprint', 'placed',
         'pins': {PIN: {'node', 'connect', 'class'}}}
        'node' is the resolved breadboard node for the leg (-1 if off-board),
        'connect' is the node it bridges to (-1 for none).

    Example:
        for p in list_parts():
            print(p['name'], p['footprint'], 'placed' if p['placed'] else 'pending')
    """
    ...

def guide_progress() -> int:
    """Guided-placement progress of the loaded state

    Returns:
        The saved guide step, or -1 when no guide source is loaded
    """
    ...

# ============================================================================
# Context Control
# ============================================================================

def context_toggle() -> None:
    """Toggle connection context between 'global' and 'python' modes"""
    ...

def context_get() -> str:
    """Get current context name ('global' or 'python')"""
    ...

# ============================================================================
# OLED Display Functions
# ============================================================================

def oled_print(text: Union[str, int, float, Node, GPIOState, ConnectionState], size: int = 2) -> None:
    """Display text on OLED (size: 1 or 2)"""
    ...

def oled_clear() -> None:
    """Clear OLED display"""
    ...

def oled_show() -> None:
    """Refresh OLED display"""
    ...

def oled_connect() -> None:
    """Connect I2C lines to OLED"""
    ...

def oled_disconnect() -> None:
    """Disconnect I2C lines from OLED"""
    ...

def oled_set_text_size(size: int) -> bool:
    """Set the default text size for oled_print()
    
    Args:
        size: Text size (0=small multiline scrolling, 1=normal, 2=large centered)
        
    Returns:
        True if successful, False if invalid size
    """
    ...

def oled_get_text_size() -> int:
    """Get the current default text size
    
    Returns:
        Current default text size (0-2)
    """
    ...

def oled_copy_print(enable: bool) -> None:
    """Enable/disable copying MicroPython print() output to OLED
    
    When enabled, all print() statements will also appear on the OLED
    in small scrolling text mode.
    
    Args:
        enable: True to enable copy mode, False to disable
    """
    ...

def oled_get_fonts() -> list[str]:
    """Get list of available font families
    
    Returns:
        List of font family names that can be used with oled_set_font()
    """
    ...

def oled_set_font(font_name: str) -> bool:
    """Set the current font family
    
    Args:
        font_name: Name of font family (e.g., "Eurostile", "Jokerman", "Comic Sans")
        
    Returns:
        True if font was set successfully, False if font not found
    """
    ...

def oled_get_current_font() -> str:
    """Get the name of the currently active font
    
    Returns:
        Current font family name
    """
    ...

def oled_load_bitmap(filepath: str) -> bool:
    """Load a bitmap file into the internal bitmap buffer
    
    Supports raw bitmap files and custom format with header.
    Common sizes: 128x32 (512 bytes), 128x64 (1024 bytes)
    
    Args:
        filepath: Path to bitmap file
        
    Returns:
        True if loaded successfully, False on error
    """
    ...

def oled_display_bitmap(x: int, y: int, width: int, height: int, data: Optional[bytes] = None) -> bool:
    """Display a bitmap on the OLED
    
    If data is provided, displays that bitmap directly.
    If data is None, displays the previously loaded bitmap from oled_load_bitmap().
    
    Args:
        x: X position on display
        y: Y position on display
        width: Bitmap width in pixels (ignored if using loaded bitmap)
        height: Bitmap height in pixels (ignored if using loaded bitmap)
        data: Optional bitmap data as bytes (1 bit per pixel, packed)
        
    Returns:
        True if displayed successfully, False on error
    """
    ...

def oled_show_bitmap_file(filepath: str, x: int, y: int) -> bool:
    """Load and display a bitmap file in one call
    
    Convenience function that combines oled_load_bitmap() and oled_display_bitmap().
    
    Args:
        filepath: Path to bitmap file
        x: X position on display
        y: Y position on display
        
    Returns:
        True if successful, False on error
    """
    ...

def oled_get_framebuffer() -> bytes:
    """Get the current OLED framebuffer as bytes
    
    Returns a copy of the framebuffer that can be modified and
    written back with oled_set_framebuffer().
    
    Returns:
        Framebuffer data as bytes (1 bit per pixel, packed)
        Size depends on display: 128x32 = 512 bytes, 128x64 = 1024 bytes
    """
    ...

def oled_set_framebuffer(data: Union[bytes, bytearray]) -> bool:
    """Set the entire OLED framebuffer from bytes
    
    Allows direct manipulation of the display buffer.
    Data must be the correct size for the display.
    
    Args:
        data: Framebuffer data (1 bit per pixel, packed)
              Must be 512 bytes for 128x32 or 1024 bytes for 128x64
        
    Returns:
        True if successful, False if wrong size
    """
    ...

def oled_get_framebuffer_size() -> tuple[int, int, int]:
    """Get the framebuffer dimensions
    
    Returns:
        Tuple of (width, height, buffer_size_in_bytes)
    """
    ...

def oled_set_pixel(x: int, y: int, color: int) -> bool:
    """Set a single pixel on the OLED
    
    Note: Call oled_show() to make the change visible.
    
    Args:
        x: X coordinate (0 to width-1)
        y: Y coordinate (0 to height-1)
        color: Pixel color (0=black/off, 1=white/on)
        
    Returns:
        True if successful, False if OLED not connected
    """
    ...

def oled_get_pixel(x: int, y: int) -> int:
    """Get the value of a single pixel
    
    Args:
        x: X coordinate (0 to width-1)
        y: Y coordinate (0 to height-1)
        
    Returns:
        Pixel color (0=black/off, 1=white/on, -1=error)
    """
    ...

# ============================================================================
# OLED GUI (retained screens)
# ============================================================================

def oled_screen() -> int:
    """Create a new retained screen; returns a screen handle (>=1) or 0."""
    ...
def oled_screen_free(screen: int) -> None: ...
def oled_screen_clear(screen: int) -> None: ...
def oled_screen_show(screen: int, persist: bool = False) -> bool:
    """Make the screen the active display (starts live rendering).

    persist=True registers it as the idle display (takes the boot logo's place,
    steps aside for other content, and survives the script). persist=False
    (default) is a one-shot foreground show."""
    ...
def oled_screen_hide() -> None: ...
def oled_screen_reset() -> None:
    """Free every retained screen and blank the panel (call at script start to discard prior screens)."""
    ...
def oled_add_text(screen: int, text: str, x: int = 0, y: int = 0, font: str = "Pragmatism",
                  size: int = 8, halign: int = -1, valign: int = -1, z: int = 0) -> int:
    """Add a text element. text may contain {token} templates. Returns an element handle."""
    ...
def oled_add_shape(screen: int, kind: int = 1, x: int = 0, y: int = 0, w: int = 0, h: int = 0,
                   filled: int = 0, z: int = 0) -> int:
    """Add a shape (0=line, 1=rect outline, 2=filled rect). Returns an element handle."""
    ...
def oled_set(elem: int, prop: str, value: Union[int, str]) -> bool:
    """Set an element property: text/font (str) or x/y/w/h/z/size/visible/anchor/halign/valign/shape/filled (int)."""
    ...
def oled_set_var(name: str, value: Union[int, float, str]) -> None:
    """Push a live value into the variable registry, referenced as {name} in text templates."""
    ...
def oled_screen_save(screen: int, name: str) -> bool:
    """Save the screen to /screens/<name>.json."""
    ...
def oled_screen_load(name: str) -> int:
    """Load /screens/<name>.json into a new screen; returns its handle or 0."""
    ...

ALIGN_LEFT: int
ALIGN_CENTER: int
ALIGN_RIGHT: int
ALIGN_TOP: int
ALIGN_MIDDLE: int
ALIGN_BOTTOM: int
SHAPE_LINE: int
SHAPE_RECT: int
SHAPE_FILLED_RECT: int

# NOTE: The object-oriented OLED layout API (Screen, Text, Shape, Line, Rect,
# load_screen) is NOT part of the native `jumperless` module - it is pure-Python
# and lives in `oledgui.py`, built on top of the flat oled_screen()/oled_add_*()
# functions above. Import it explicitly:  from oledgui import Screen, Text, ...
# (Declaring those classes here shadowed the real oledgui ones with a less
# precise signature, so `scr.add(Text(...))` was typed as `Text | Shape`.)

# ============================================================================
# Probe Functions
# ============================================================================

def probe_read(blocking: bool = True) -> ProbePad:
    """Read probe pad (default: blocking until touched)"""
    ...

def read_probe(blocking: bool = True) -> ProbePad:
    """Alias for probe_read()"""
    ...

def probe_read_blocking() -> ProbePad:
    """Wait for probe touch (explicit blocking)"""
    ...

def probe_read_nonblocking() -> ProbePad:
    """Check probe immediately (returns NO_PAD if not touched)"""
    ...

def probe_wait() -> ProbePad:
    """Alias for probe_read_blocking()"""
    ...

def wait_probe() -> ProbePad:
    """Alias for probe_read_blocking()"""
    ...

def probe_touch() -> ProbePad:
    """Alias for probe_read_blocking()"""
    ...

def wait_touch() -> ProbePad:
    """Alias for probe_read_blocking()"""
    ...

def probe_tap(node: NodeRef) -> None:
    """Simulate tapping probe on a node"""
    ...

# ============================================================================
# Probe Button Functions
# ============================================================================

def get_button(blocking: bool = True) -> ProbeButton:
    """Get probe button state (default: blocking until pressed)"""
    ...

def probe_button(blocking: bool = True) -> ProbeButton:
    """Alias for get_button()"""
    ...

def probe_button_blocking() -> ProbeButton:
    """Wait for button press (explicit blocking)"""
    ...

def probe_button_nonblocking() -> ProbeButton:
    """Check button immediately"""
    ...

def button_read(blocking: bool = True) -> ProbeButton:
    """Alias for get_button()"""
    ...

def read_button(blocking: bool = True) -> ProbeButton:
    """Alias for get_button()"""
    ...

def check_button() -> ProbeButton:
    """Alias for probe_button_nonblocking()"""
    ...

def button_check() -> ProbeButton:
    """Alias for probe_button_nonblocking()"""
    ...

# ============================================================================
# Probe Switch Functions  
# ============================================================================

def get_switch_position() -> int:
    """Get current probe switch position
    
    Returns:
        0 (SWITCH_MEASURE): Probe in measure mode
        1 (SWITCH_SELECT): Probe in select mode
        -1 (SWITCH_UNKNOWN): Position unknown
    
    Example:
        if get_switch_position() == SWITCH_MEASURE:
            voltage = measureMode()
    """
    ...

def set_switch_position(position: int) -> None:
    """Manually set probe switch position
    
    Args:
        position: 0 (SWITCH_MEASURE), 1 (SWITCH_SELECT), or -1 (SWITCH_UNKNOWN)
    """
    ...

def check_switch_position() -> int:
    """Check probe switch position via current sensing and update state
    
    Uses hysteresis thresholds to prevent oscillation between modes.
    
    Returns:
        Updated switch position (0, 1, or -1)
    
    Example:
        position = check_switch_position()
        if position == SWITCH_SELECT:
            pad = probe_read(blocking=False)
    """
    ...

# ============================================================================
# Clickwheel Functions
# ============================================================================

def clickwheel_up(clicks: int = 1) -> None:
    """Scroll clickwheel up"""
    ...

def clickwheel_down(clicks: int = 1) -> None:
    """Scroll clickwheel down"""
    ...

def clickwheel_press() -> None:
    """Press clickwheel button"""
    ...

def clickwheel_get_position() -> int:
    """Get raw clickwheel position counter
    
    Returns:
        Current position value (accumulates as you turn)
    
    Example:
        pos = clickwheel_get_position()
        print(f"Position: {pos}")
    """
    ...

def clickwheel_reset_position() -> None:
    """Reset clickwheel position counter to 0"""
    ...

def clickwheel_get_direction(consume: bool = True) -> int:
    """Get clickwheel direction event
    
    Args:
        consume: If True (default), clears direction after reading (one-shot).
                 If False, direction persists until consumed.
    
    Returns:
        0 (CLICKWHEEL_NONE), 1 (CLICKWHEEL_UP), or 2 (CLICKWHEEL_DOWN)
    
    Note: Direction persists until consumed, so you won't miss turn events
    
    Example:
        direction = clickwheel_get_direction()
        if direction == CLICKWHEEL_UP:
            value += 1
    """
    ...

def clickwheel_get_button() -> int:
    """Get clickwheel button state
    
    Returns:
        0 (CLICKWHEEL_IDLE), 1 (CLICKWHEEL_PRESSED), 2 (CLICKWHEEL_HELD),
        3 (CLICKWHEEL_RELEASED), or 4 (CLICKWHEEL_DOUBLECLICKED)
    """
    ...

def clickwheel_is_initialized() -> bool:
    """Check if clickwheel hardware is initialized and ready"""
    ...

# ============================================================================
# Service Management Functions
# ============================================================================

def force_service(name: str) -> bool:
    """Force immediate execution of a specific system service
    
    Args:
        name: Service name (e.g., "ProbeButton", "Peripherals")
    
    Returns:
        True if service found and executed, False otherwise
    
    Example:
        while True:
            force_service("ProbeButton")  # Ensure button updates
            button = check_button()
            time.sleep(0.001)
    """
    ...

def force_service_by_index(index: int) -> bool:
    """Force service execution by index (faster than name lookup)
    
    Args:
        index: Service index from get_service_index()
    
    Returns:
        True if successful, False otherwise
    
    Example:
        btn_idx = get_service_index("ProbeButton")
        while True:
            force_service_by_index(btn_idx)  # Fastest
            button = check_button()
    """
    ...

def get_service_index(name: str) -> int:
    """Get service index by name for use with force_service_by_index()
    
    Args:
        name: Service name
    
    Returns:
        Service index (0+), or -1 if not found
    
    Example:
        idx = get_service_index("ProbeButton")
        if idx >= 0:
            force_service_by_index(idx)
    """
    ...

# ============================================================================
# Terminal Color Functions
# ============================================================================

def change_terminal_color(color: int = -1, flush: bool = True) -> None:
    """Set terminal color by 256-color palette index
    
    Args:
        color: Color index (0-255), or -1 for default
        flush: Flush output immediately (default: True)
    
    Example:
        change_terminal_color(196)  # Bright red
        print("This is in red")
        change_terminal_color(-1)   # Reset to default
    """
    ...

def cycle_term_color(reset: bool = False, step: float = 100.0, flush: bool = True) -> None:
    """Cycle through terminal colors
    
    Args:
        reset: If True, reset to start of color sequence
        step: Color increment step (default: 100.0)
        flush: Flush output immediately (default: True)
    
    Example:
        cycle_term_color(reset=True)   # Start fresh
        for i in range(10):
            cycle_term_color()         # Next color
            print(f"Line {i}")
    """
    ...

# ============================================================================
# Filesystem Functions
# ============================================================================

def fs_exists(path: str) -> bool:
    """Check if file/directory exists"""
    ...

def fs_listdir(path: str) -> List[str]:
    """List directory contents"""
    ...

def fs_read(path: str) -> str:
    """Read file contents"""
    ...

def fs_write(path: str, content: str) -> bool:
    """Write file contents"""
    ...

def fs_cwd() -> str:
    """Get current working directory"""
    ...

# ============================================================================
# Overlay Functions
# ============================================================================

def overlay_set(name: str, x: int, y: int, width: int, height: int, colors: list[int]) -> int:
    """Create/update a named overlay block at (x, y) from a list of packed 0xRRGGBB colors"""
    ...

def overlay_clear(name: str) -> None:
    """Clear a named overlay"""
    ...

def overlay_clear_all() -> None:
    """Clear all overlays"""
    ...

def overlay_set_pixel(x: int, y: int, color: int) -> None:
    """Set overlay pixel at (x, y) to a packed 0xRRGGBB color"""
    ...

def overlay_count() -> int:
    """Return number of active overlays"""
    ...

def overlay_shift(dx: int = 0, dy: int = 0) -> None:
    """Shift overlay position"""
    ...

def overlay_place(netNum: int, x: int, y: int) -> None:
    """Place overlay for net at (x, y)"""
    ...

def overlay_serialize() -> str:
    """Serialize overlays to YAML string"""
    ...

# ============================================================================
# Status/Debug Functions
# ============================================================================

def print_bridges() -> None:
    """Print all active bridges"""
    ...

def print_paths() -> None:
    """Print resolved paths"""
    ...

def print_crossbars() -> None:
    """Print raw crossbar state"""
    ...

def print_nets() -> None:
    """Print current nets"""
    ...

def print_chip_status() -> None:
    """Print CH446Q chip status"""
    ...

# ============================================================================
# Miscellaneous Functions
# ============================================================================

def arduino_reset() -> None:
    """Reset the Arduino Nano"""
    ...

def pause_core2(pause: bool) -> None:
    """Pause/resume core2 processing"""
    ...

def run_app(appName: str) -> None:
    """Launch a built-in app"""
    ...

def send_raw(chip: Union[int, str], x: int, y: int, setOrClear: int = 1) -> None:
    """Send raw data to crossbar chip"""
    ...

def change_terminal_color(color: int = -1, flush: bool = True) -> None:
    """Change terminal color"""
    ...

def cycle_term_color(reset: bool = False, step: float = 100.0, flush: bool = True) -> None:
    """Cycle through terminal colors"""
    ...

# ============================================================================
# Help Functions
# ============================================================================

def help(section: Optional[str] = None) -> None:
    """Display help for all functions or a specific section
    
    Sections: DAC, ADC, GPIO, PWM, WAVEGEN, INA, NODES, NETS, SLOTS, 
              OLED, PROBE, STATUS, FILESYSTEM, MISC, EXAMPLES
    """
    ...

def nodes_help() -> None:
    """Display detailed node reference"""
    ...

# ============================================================================
# GPIO State Constants
# ============================================================================

HIGH: GPIOState
LOW: GPIOState
FLOATING: GPIOState

# ============================================================================
# GPIO Direction Constants
# ============================================================================

INPUT: GPIODirection
OUTPUT: GPIODirection

# ============================================================================
# Node Constants - Power Rails
# ============================================================================

TOP_RAIL: Node
T_RAIL: Node
BOTTOM_RAIL: Node
BOT_RAIL: Node
B_RAIL: Node
GND: Node

# ============================================================================
# Node Constants - DACs
# ============================================================================

DAC0: Node
DAC_0: Node
DAC1: Node
DAC_1: Node

# ============================================================================
# Node Constants - ADCs
# ============================================================================

ADC0: Node
ADC1: Node
ADC2: Node
ADC3: Node
ADC4: Node
ADC7: Node

# ============================================================================
# Node Constants - Current Sense
# ============================================================================

ISENSE_PLUS: Node
ISENSE_P: Node
I_P: Node
CURRENT_SENSE_P: Node
CURRENT_SENSE_PLUS: Node
ISENSE_MINUS: Node
ISENSE_N: Node
I_N: Node
CURRENT_SENSE_N: Node
CURRENT_SENSE_MINUS: Node

# ============================================================================
# Node Constants - Buffer
# ============================================================================

BUFFER_IN: Node
BUF_IN: Node
BUFFER_OUT: Node
BUF_OUT: Node

# ============================================================================
# Node Constants - UART
# ============================================================================

UART_TX: Node
TX: Node
UART_RX: Node
RX: Node

# ============================================================================
# Node Constants - Arduino Nano Digital Pins
# ============================================================================

D0: Node
D1: Node
D2: Node
D3: Node
D4: Node
D5: Node
D6: Node
D7: Node
D8: Node
D9: Node
D10: Node
D11: Node
D12: Node
D13: Node

NANO_D0: Node
NANO_D1: Node
NANO_D2: Node
NANO_D3: Node
NANO_D4: Node
NANO_D5: Node
NANO_D6: Node
NANO_D7: Node
NANO_D8: Node
NANO_D9: Node
NANO_D10: Node
NANO_D11: Node
NANO_D12: Node
NANO_D13: Node

# ============================================================================
# Node Constants - Arduino Nano Analog Pins
# ============================================================================

A0: Node
A1: Node
A2: Node
A3: Node
A4: Node
A5: Node
A6: Node
A7: Node

NANO_A0: Node
NANO_A1: Node
NANO_A2: Node
NANO_A3: Node
NANO_A4: Node
NANO_A5: Node
NANO_A6: Node
NANO_A7: Node

# ============================================================================
# Node Constants - GPIO Pins
# ============================================================================

GPIO_1: Node
GPIO_2: Node
GPIO_3: Node
GPIO_4: Node
GPIO_5: Node
GPIO_6: Node
GPIO_7: Node
GPIO_8: Node

GP1: Node
GP2: Node
GP3: Node
GP4: Node
GP5: Node
GP6: Node
GP7: Node
GP8: Node

GPIO_20: Node
GPIO_21: Node
GPIO_22: Node
GPIO_23: Node
GPIO_24: Node
GPIO_25: Node
GPIO_26: Node
GPIO_27: Node

# ============================================================================
# Waveform Constants
# ============================================================================

SINE: int
TRIANGLE: int
SAWTOOTH: int
SQUARE: int
RAMP: int
ARBITRARY: int

# ============================================================================
# Probe Button Constants
# ============================================================================

BUTTON_NONE: ProbeButton
BUTTON_CONNECT: ProbeButton
BUTTON_REMOVE: ProbeButton
CONNECT_BUTTON: ProbeButton
REMOVE_BUTTON: ProbeButton

# ============================================================================
# Probe Switch Position Constants
# ============================================================================

SWITCH_MEASURE: int
"""Probe switch in measure mode (0)"""

SWITCH_SELECT: int
"""Probe switch in select mode (1)"""

SWITCH_UNKNOWN: int
"""Probe switch position unknown (-1)"""

# ============================================================================
# Clickwheel Constants
# ============================================================================

CLICKWHEEL_NONE: int
"""No clickwheel movement (0)"""

CLICKWHEEL_UP: int
"""Clickwheel turned clockwise (1)"""

CLICKWHEEL_DOWN: int
"""Clickwheel turned counter-clockwise (2)"""

CLICKWHEEL_IDLE: int
"""Clickwheel button idle (0)"""

CLICKWHEEL_PRESSED: int
"""Clickwheel button pressed (1)"""

CLICKWHEEL_HELD: int
"""Clickwheel button held down (2)"""

CLICKWHEEL_RELEASED: int
"""Clickwheel button released (3)"""

CLICKWHEEL_DOUBLECLICKED: int
"""Clickwheel button double-clicked (4)"""

# ============================================================================
# Probe Pad Constants
# ============================================================================

NO_PAD: ProbePad
LOGO_PAD_TOP: ProbePad
LOGO_PAD_BOTTOM: ProbePad
GPIO_PAD: ProbePad
DAC_PAD: ProbePad
ADC_PAD: ProbePad
BUILDING_PAD_TOP: ProbePad
BUILDING_PAD_BOTTOM: ProbePad

# Nano power/control pad constants
NANO_VIN: ProbePad
VIN_PAD: ProbePad
NANO_RESET_0: ProbePad
RESET_0_PAD: ProbePad
NANO_RESET_1: ProbePad
RESET_1_PAD: ProbePad
NANO_GND_1: ProbePad
GND_1_PAD: ProbePad
NANO_GND_0: ProbePad
GND_0_PAD: ProbePad
NANO_3V3: ProbePad
VIN_3V3_PAD: ProbePad
NANO_5V: ProbePad
VIN_5V_PAD: ProbePad

# Nano digital pin pad constants
D0_PAD: ProbePad
D1_PAD: ProbePad
D2_PAD: ProbePad
D3_PAD: ProbePad
D4_PAD: ProbePad
D5_PAD: ProbePad
D6_PAD: ProbePad
D7_PAD: ProbePad
D8_PAD: ProbePad
D9_PAD: ProbePad
D10_PAD: ProbePad
D11_PAD: ProbePad
D12_PAD: ProbePad
D13_PAD: ProbePad
RESET_PAD: ProbePad
AREF_PAD: ProbePad

# Nano analog pin pad constants
A0_PAD: ProbePad
A1_PAD: ProbePad
A2_PAD: ProbePad
A3_PAD: ProbePad
A4_PAD: ProbePad
A5_PAD: ProbePad
A6_PAD: ProbePad
A7_PAD: ProbePad

# Rail pad constants
TOP_RAIL_PAD: ProbePad
BOTTOM_RAIL_PAD: ProbePad
BOT_RAIL_PAD: ProbePad
TOP_RAIL_GND: ProbePad
TOP_GND_PAD: ProbePad
BOTTOM_RAIL_GND: ProbePad
BOT_RAIL_GND: ProbePad
BOTTOM_GND_PAD: ProbePad
BOT_GND_PAD: ProbePad

# ============================================================================
# JFS Filesystem Module
# ============================================================================

class JFSFile:
    """JFS file object"""
    def read(self, size: int = -1) -> Union[str, bytes]: ...
    def write(self, data: Union[str, bytes]) -> int: ...
    def print(self, *args) -> int: ...
    def seek(self, position: int, whence: int = 0) -> bool: ...
    def tell(self) -> int: ...
    def position(self) -> int: ...
    def size(self) -> int: ...
    def available(self) -> int: ...
    def name(self) -> str: ...
    def close(self) -> None: ...
    def flush(self) -> None: ...
    def __enter__(self) -> 'JFSFile': ...
    def __exit__(self, exc_type, exc_val, exc_tb) -> bool: ...

class JFSModule:
    """Jumperless Filesystem Module"""
    
    SEEK_SET: int
    SEEK_CUR: int
    SEEK_END: int
    
    def open(self, path: str, mode: str = 'r') -> JFSFile:
        """Open a file (modes: 'r', 'w', 'a', 'rb', 'wb', 'ab')"""
        ...
    
    def read(self, file: JFSFile, size: int = 1024) -> str:
        """Read from file"""
        ...
    
    def write(self, file: JFSFile, data: str) -> None:
        """Write to file"""
        ...
    
    def close(self, file: JFSFile) -> None:
        """Close file"""
        ...
    
    def seek(self, file: JFSFile, position: int, whence: int = 0) -> bool:
        """Seek in file"""
        ...
    
    def tell(self, file: JFSFile) -> int:
        """Get file position"""
        ...
    
    def size(self, file: JFSFile) -> int:
        """Get file size"""
        ...
    
    def available(self, file: JFSFile) -> int:
        """Get bytes available"""
        ...
    
    def exists(self, path: str) -> bool:
        """Check if path exists"""
        ...
    
    def listdir(self, path: str) -> List[str]:
        """List directory contents"""
        ...
    
    def mkdir(self, path: str) -> None:
        """Create directory"""
        ...
    
    def rmdir(self, path: str) -> None:
        """Remove directory"""
        ...
    
    def remove(self, path: str) -> None:
        """Remove file"""
        ...
    
    def rename(self, from_path: str, to_path: str) -> None:
        """Rename/move file"""
        ...
    
    def stat(self, path: str) -> Tuple[int, ...]:
        """Get file/directory status"""
        ...
    
    def info(self) -> Tuple[int, int, int]:
        """Get filesystem info (total, used, free)"""
        ...

jfs: JFSModule

