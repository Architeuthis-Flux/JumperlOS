"""
Type stub for Jumperless MicroPython Module
============================================

This file provides type hints and autocomplete support for the jumperless module.
All functions and constants are available globally without needing to import.
"""

from typing import Union, Tuple, Dict, List, Optional, Any

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

def connect(node1: NodeRef, node2: NodeRef, save: bool = False) -> None:
    """Connect two nodes
    
    Args:
        node1, node2: Nodes to connect (int, string, or Node constant)
        save: Save to file (default: False, uses local copy)
    """
    ...

def disconnect(node1: NodeRef, node2: NodeRef) -> None:
    """Disconnect two nodes (set node2 to -1 to disconnect all from node1)"""
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

def nodes_discard() -> None:
    """Discard unsaved changes and restore last saved state"""
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

# ============================================================================
# Slot Management
# ============================================================================

def switch_slot(slot: int) -> int:
    """Switch to a different slot (0-7)
    
    Returns:
        Previous slot number
    """
    ...

def get_state() -> str:
    """Return the complete board state as a JSON string."""
    ...


def set_state(json: str, clear_first: bool = True, from_wokwi: bool = False) -> int:
    """Apply board state from JSON string or (if ``from_wokwi``) Wokwi diagram.

    Args:
        json: State JSON or filename/Wokwi JSON.
        clear_first: Clear existing state before applying (default True).
        from_wokwi: Interpret ``json`` as Wokwi diagram when True.

    Returns:
        0 on success, -1 on failure.
    """
    ...

CURRENT_SLOT: int
"""Current active slot number"""

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

# Additional constants added in firmware
SWITCH_MEASURE: int
SWITCH_SELECT: int
SWITCH_UNKNOWN: int

CLICKWHEEL_NONE: int
CLICKWHEEL_UP: int
CLICKWHEEL_DOWN: int
CLICKWHEEL_IDLE: int
CLICKWHEEL_PRESSED: int
CLICKWHEEL_HELD: int
CLICKWHEEL_RELEASED: int
CLICKWHEEL_DOUBLECLICKED: int

FAKE_GPIO_INPUT: int
FAKE_GPIO_OUTPUT: int

# Extended GPIO utility functions
def gpio_set_read_floating(pin: GPIOPin, enabled: bool) -> None: ...
def gpio_get_read_floating(pin: GPIOPin) -> bool: ...
def set_gpio_read_floating(pin: GPIOPin, enabled: bool) -> None: ...
def get_gpio_read_floating(pin: GPIOPin) -> bool: ...
def gpio_claim_pin(pin: GPIOPin) -> None: ...
def gpio_release_pin(pin: GPIOPin) -> None: ...
def gpio_release_all_pins() -> None: ...

# Fast connection helpers
def fast_connect(node1: NodeRef, node2: NodeRef, duplicates: int = -1) -> None: ...
def fast_disconnect(node1: NodeRef, node2: NodeRef, duplicates: int = -1) -> None: ...

# Net/path utilities
def set_net_color_hsv(netNum: int, h: float, s: float, v: float) -> bool: ...
def get_num_paths(include_duplicates: bool = True) -> int: ...
def get_path_info(pathIdx: int) -> str: ...
def get_all_paths() -> str: ...
def get_path_between(node1: NodeRef, node2: NodeRef) -> str: ...
def get_all_nets() -> int: ...

# Fake GPIO classes
class FakeGpioDisconnect: ...
class FakeGpioPin: ...

# OLED advanced helpers
def oled_set_text_size(size: int) -> None: ...
def oled_get_text_size() -> int: ...
def oled_copy_print(enable: bool) -> None: ...
def oled_get_fonts() -> List[str]: ...
def oled_set_font(fontName: str) -> None: ...
def oled_get_current_font() -> str: ...
def oled_load_bitmap(filepath: str) -> int: ...
def oled_display_bitmap(x: int, y: int, width: int, height: int, data) -> int: ...
def oled_show_bitmap_file(filepath: str, x: int, y: int) -> int: ...
def oled_get_framebuffer() -> Tuple[int, int, bytes]: ...
def oled_set_framebuffer(data, length: int) -> int: ...
def oled_get_framebuffer_size() -> Tuple[int, int, int]: ...
def oled_set_pixel(x: int, y: int, color: int) -> int: ...
def oled_get_pixel(x: int, y: int) -> int: ...

# OLED GUI (retained screens) - flat handle-based API
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

# OLED GUI (retained screens) - object-oriented wrapper
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

# Graphic overlays
def overlay_set(name: str, startRow: int, startCol: int, width: int, height: int, colors) -> int: ...
def overlay_clear(name: str) -> int: ...
def overlay_clear_all() -> None: ...
def overlay_set_pixel(row: int, col: int, color: int) -> None: ...
def overlay_count() -> int: ...
def overlay_shift(name: str, dRow: int, dCol: int) -> int: ...
def overlay_place(name: str, row: int, col: int) -> int: ...
def overlay_serialize() -> str: ...

# Service & switch utilities
def force_service(service_name: str) -> int: ...
def force_service_by_index(index: int) -> int: ...
def get_service_index(service_name: str) -> int: ...
def get_switch_position() -> int: ...
def set_switch_position(position: int) -> None: ...
def check_switch_position() -> int: ...

def clickwheel_get_position() -> int: ...
def clickwheel_reset_position() -> None: ...
def clickwheel_get_direction(consume: bool = True) -> int: ...
def clickwheel_get_button() -> int: ...
def clickwheel_is_initialized() -> bool: ...

# ============================================================================
# Python Helper Functions
# ============================================================================

def quick_connect(*nodes: NodeRef) -> None:
    """Connect multiple nodes in sequence
    
    Usage:
        quick_connect(1, 2, 3, 4)  # Connects 1-2, 2-3, 3-4
        quick_connect(D13, TOP_RAIL, A0)
    """
    ...

def disconnect_all(*nodes: NodeRef) -> None:
    """Disconnect the specified nodes from everything.

    Args:
        *nodes: Node identifiers to disconnect.
    """
    ...


def voltage_divider(vin_node: NodeRef, vout_node: NodeRef, gnd_node: NodeRef = GND) -> None:
    """Helper to configure a simple voltage divider circuit.

    Args:
        vin_node: Input nodal connection.
        vout_node: Output node (typically ADC).
        gnd_node: Ground reference (default GND).
    """
    ...



