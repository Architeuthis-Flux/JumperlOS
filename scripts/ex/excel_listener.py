"""
Excel GUI Listener Script (V1.0.2)
"""

import sys
import select
import time
import jfs

## Command string field indices
CMD_IDX_GPIO            = 1
CMD_IDX_PWM_FREQ        = 2
CMD_IDX_PWM_DUTY        = 3
CMD_IDX_VOLTAGES        = 4
CMD_IDX_SAMPLE_INTERVAL = 5
# CMD_IDX_TBD = 6
# CMD_IDX_TBD = 7
# CMD_IDX_TBD = 8
CMD_IDX_CONNECTIONS     = 9  ## All indices >= this are connection commands

MIN_CMD_LENGTH          = 12
MIN_CMD_COUNT           = 10
DEFAULT_SAMPLE_MS       = 10000
ADC_NODES = {"ADC0": 0, "ADC1": 1, "ADC2": 2, "ADC3": 3} ## ADC4 is not supported (because it is often flakey)
CURRENT_NODES = {"I_P": 0, "I_N": 1}

MESSAGE_LEVEL = 0  # 0 = off, 1 = basic, 2 = verbose
def debug_msg(msg, level=1):
    if MESSAGE_LEVEL >= level:
        print(msg)

def reset_breadboard():
    nodes_clear()
    for channel in range(4):
        set_dac(channel, 0)
    for pin in range(1, 9):
        gpio_set_dir(pin, False) ## False --> INPUT
        gpio_set_pull(pin, 0) ## -1, 0, 1 --> down, none, up

## Reset all breadboard settings since Excel will be setting them
reset_breadboard()
debug_msg("General Purpose Testing Script")

def is_setting_in_config(setting_text="ignore_dtr = 1;"):
    f = jfs.open('config.txt', 'r')
    content = f.read()
    f.close()
    if setting_text in content:
        return True
    else:
        return False

## TODO: check & set the "lines" setting instead of "wires"
# if is_setting_in_config("lines_wires = 1;") or is_setting_in_config("lines_wires = wires;"):
#     f = jfs.open('config.txt', 'w+')
#     # "lines_wires = lines;"
#     f.close()

## TODO: check the lock OLED setting and make sure won't interfere
# "lock_connection = 0;"

def parse_command(full_command_string):
    """Returns a dict of structured fields from a raw USB command string, or None if invalid."""
    debug_msg(f"\nstring recieved: {full_command_string}")
    cmd_list = full_command_string.split(",")
    if len(full_command_string) < MIN_CMD_LENGTH or len(cmd_list) < MIN_CMD_COUNT:
        return None
    return {
        "gpio_chars":       cmd_list[CMD_IDX_GPIO],
        "pwm_frequencies":  [float(x) for x in cmd_list[CMD_IDX_PWM_FREQ].split(";")],
        "pwm_duty_cycles":  [float(x) for x in cmd_list[CMD_IDX_PWM_DUTY].split(";")],
        "voltages":         [float(x) for x in cmd_list[CMD_IDX_VOLTAGES].split(";")],
        "sample_interval":  int(cmd_list[CMD_IDX_SAMPLE_INTERVAL]),
        "connections":      [c.split(";", 1) for c in cmd_list[CMD_IDX_CONNECTIONS:]], ## connections is a list of lists
    }

def apply_gpio(gpio_chars, freq_list, duty_list, status_list):
    for pin, c in enumerate(gpio_chars, start=1):
        if 1 <= pin <= 8:
            set_gpio_from_char(pin, c, freq_list[pin - 1], duty_list[pin - 1])
        else:
            status_list.append(f"ENCODING ({c}) FOR UNKNOWN PIN ({pin})")

def apply_voltages(voltage_list):
    for channel, voltage in enumerate(voltage_list):
        set_dac(channel, float(voltage))

def apply_connections(connection_list, net_name_list):
    ''' Attempts to create each connection, adds new nets to list, and updates & returns sensor_state'''
    sensor_state = {
        "adc":     [False, False, False, False], ## ADC0–ADC3
        "current": [False, False],               ## [I+ present, I- present] (Used for power too)
    }
    ## Only attempt to form connections if a list of connections was supplied
    if len(connection_list[0]) > 1:
        for net_name, nodes_str in connection_list:
            node_1, node_2 = nodes_str.split('-', 1)
            connect(node_1, node_2, 0)
            ## Query to confirm the connection was formed
            if is_connected(node_1,node_2):
                debug_msg(f"Connected {node_1} — {node_2}", level=2)
                ## Add the net_name to the list if it's new
                if net_name not in net_name_list:
                    net_name_list.append(net_name)
                    set_net_name(len(net_name_list) - 1, net_name)
                ## Check both endpoints against sensor node lists
                for node in (node_1, node_2):
                    if node in ADC_NODES:
                        sensor_state["adc"][ADC_NODES[node]] = True
                        debug_msg(f"ADC{ADC_NODES[node]} enabled for sampling", level=2)
                    if node in CURRENT_NODES:
                        sensor_state["current"][CURRENT_NODES[node]] = True
                        debug_msg(f"Current node {node} detected", level=2)
            else:
                status_message_list.append(f"WARNING: Failed to connect {node_1} — {node_2}")
                debug_msg(status_message_list[-1])
    return sensor_state

# def build_response(net_name_list, net_colors_list, status_list):
#     ''' This function builds the response string based on the supplied lists of information '''
#     adc_placeholders = "ADC0,ADC1,ADC2,ADC3,ADC4,Current,LED_DETAIL"
#     return f"{'|'.join(net_name_list)},{'|'.join(net_colors_list)},{adc_placeholders},{'|'.join(status_list)}"

def build_response(net_name_list, net_colors_list, measurements, status_list):
    """Builds the response string. measurements is the dict from sample_measurements()."""
    placeholder_values = "TBD0,TBD1,TBD2,TBD3,TBD4"
    adc_fields = "|".join(
        f"{v:.4f}" if v is not None else "N/C"
        for v in measurements["adc"]
    )
    power_field = f"{measurements['power']:.6f}" if measurements["power"] is not None else "N/C"
    current_field = f"{measurements['current']:.6f}" if measurements["current"] is not None else "N/C"
    return f"{'|'.join(net_name_list)},{'|'.join(net_colors_list)},{adc_fields}|{power_field}|{current_field},{placeholder_values},LED_DETAIL,{'|'.join(status_list)}"

def set_gpio_from_char(pin_id, setting_char, frequency_setting, duty_cycle_setting):
    pwm_stop(pin_id)
    if setting_char == 'H':
        gpio_set_dir(pin_id, True) ## true --> OUTPUT
        gpio_set(pin_id, True) ## True --> OUTPUT HIGH
        debug_msg(f"setting {pin_id} to {setting_char}")
    elif setting_char == 'L':
        gpio_set_dir(pin_id, True) ## true --> OUTPUT
        gpio_set(pin_id, False) ## False --> OUTPUT LOW
        debug_msg(f"setting {pin_id} to {setting_char}")
    elif setting_char == 'P':
        gpio_set_dir(pin_id, True) ## true --> OUTPUT
        pwm(pin_id, frequency_setting, duty_cycle_setting)
        debug_msg(f"setting {pin_id} to {setting_char} with a frequency of {frequency_setting} and duty cycle of {duty_cycle_setting}")
    else:
        try:
            setting_int = int(setting_char)
            setting_int -= 1
        except (ValueError, TypeError) as e:
            status_message_list.append(f"WARNING: Invalid GPIO setting ({setting_char}) detected on pin {pin_id} defaulting to INPUT")
            debug_msg(status_message_list[-1])
            status_message_list.append(f"Error message: {e}")
            debug_msg(status_message_list[-1])
            setting_int = 0
        gpio_set_dir(pin_id, False) ## False --> INPUT
        gpio_set_pull(pin_id, setting_int) ## -1, 0, 1 --> down, none, up
        debug_msg(f"setting {pin_id} to {setting_int}")

def sample_measurements(sensor_state):
    ''' Returns a dictionary of "adc" : [readings] and "current" : reading '''
    adc_readings = []
    for i, enabled in enumerate(sensor_state["adc"]):
        if enabled:
            adc_readings.append(adc_get(i))
            debug_msg(f"ADC{i}: {adc_readings[-1]:.3f}V", level=2)
        else:
            adc_readings.append(None)

    if all(sensor_state["current"]):
        power_reading = get_power(0)
        debug_msg(f"Power: {power_reading*1000:.2f}mW", level=2)
        current_reading = ina_get_current(0)
        debug_msg(f"Current: {current_reading*1000:.2f}mA", level=2)
    else:
        power_reading = None
        current_reading = None

    return {"adc": adc_readings, "power": power_reading, "current": current_reading}

def try_connecting_oled():
    try:
        oled_connect() ## Connect OLED
        oled_clear()
        oled_enabled = True
    except (ValueError, TypeError) as e:
        print("WARNING: Unable to connect OLED. OLED features disabled.")
        print(f"Error message: {e}")
        oled_enabled = False
    return oled_enabled

## Connect the OLED and display Excel
if try_connecting_oled():
    oled_show_bitmap_file("/images/excelGUI.bin", 0, 0) ## when new firmware is available...
    # oled_show_bitmap_file("/images/excel_gui.bin", 0, 0)
    oled_disconnect()

## Debug code: initial net info
# debug_msg("Initial Net Info:")
# for net_num in range(0, get_num_nets()):
#     debug_msg(f"Net {get_net_name(net_num)} is colored {get_net_color(net_num):06x}")

## Check if the config.txt file has "ignore_dtr = 1;" in the [usb_cdc] section
connection_allowed = True ## an override value used for testing
if is_setting_in_config() and connection_allowed:
    # time.sleep(10) ## a delay to test connecting Excel
    debug_msg("Settings are compatible with Excel")    
    ## Init Variables
    echo_enabled = False
    print_measurements_enabled = False
    continue_looping = True
    last_print_time_ms = 0
    sample_interval_ms = 10000 ## Default of 10 seconds, but should be set by user input if ADCs or I sense is used
    
    ## Create a poll object
    poll_obj = select.poll()

    ## Register sys.stdin for polling on input events
    poll_obj.register(sys.stdin, select.POLLIN)
    
    ## Main Loop
    while continue_looping:
        current_time_ms = time.ticks_ms() ## because time.monotonic() is not implmented...
        events = poll_obj.poll(0) ## Poll with 0 timeout is nonblocking

        ## Check if data is available.
        ## The 'events' list contains tuples of (object, event_mask)
        if events:
            for obj, event in events:
                ## Create an empty list to hold incomming chars:
                char_list = []
                ## It occasionally drops the first chars if it doesn't poll at the right time
                ## So maybe send some extra characters at the beginning?
                while obj is sys.stdin and event & select.POLLIN:
                    ## Read one character at a time
                    char = sys.stdin.read(1)

                    if char is None:
                        break
                    elif char not in('\n', '\r', '|') and char is not None:
                        if echo_enabled:
                            print(f'{char}', end='')
                        ## append char to char_list (if not a ")
                        if char != '"':
                            char_list.append(char)
                    else:
                        ## Process the command because a terminator was recieved
                        debug_msg("\nTerminator detected, processing command...")
                        parsed = parse_command("".join(char_list))
                        if parsed:
                            ## Perform setup operations
                            nodes_clear()
                            status_message_list = []
                            ## Set the default net names to match Excel
                            net_name_list = ["Empty Net", "G", "T", "B", "0DAC", "1DAC"]
                            for i, net in enumerate(net_name_list):
                                 set_net_name(i, net)
                            ## Take a look at the default nets:
                            # for net_num in range(0, get_num_nets()):
                            #     debug_msg(f"Net {get_net_name(net_num)} is colored {get_net_color(net_num):06x}")
    
                            ## Ignore element 0 (the "EXCEL" lead text because it might be missing the first few characters)
                            
                            apply_gpio(parsed["gpio_chars"], parsed["pwm_frequencies"], parsed["pwm_duty_cycles"], status_message_list)
                            apply_voltages(parsed["voltages"])
                            sample_interval_ms = parsed["sample_interval"]
                            # state["sample_interval_ms"] = parsed["sample_interval"] ## Use this if state is converted to a dictionary
                            ## Sensor sampling state — reset and updated during apply_connections
                            sensor_state = apply_connections(parsed["connections"], net_name_list)
                            debug_msg(f"ADC any: {any(sensor_state["adc"])} Current all: {all(sensor_state["current"])}", 2)
                            if any(sensor_state["adc"]) or all(sensor_state["current"]):
                                print_measurements_enabled = True
                            else:
                                print_measurements_enabled = False
                            measurements = sample_measurements(sensor_state)
                        
                            ## After all commands have been issued, query the resulting colors for each net
                            net_colors_list = [f"{get_net_color(n):06x}" for n in range(get_num_nets())]
                            # for net_num in range(0, get_num_nets()):
                            #     debug_msg(f"Net {get_net_name(net_num)} is colored {get_net_color(net_num):06x}")
                            #     debug_msg(f"The nodes are: {get_net_nodes(net_num)}")

                            ## Print any status messages to the OLED
                            if len(status_message_list) > 0:
                                reset_breadboard()
                                if try_connecting_oled():
                                    oled_set_font("Berkeley Mono")
                                    oled_print("\n\n\n\n", 0)
                                    for message in status_message_list:
                                        oled_print(f"{message}\n", 0)
                                    oled_disconnect()
                            
                            ## Return the '|' delimited lists separated by ','s with placeholders for future data
                            print(build_response(net_name_list, net_colors_list, measurements, status_message_list))
                            last_print_time_ms = current_time_ms

                        ## Finally, regardless of whether the string was parsed or not, exit the loop
                        break
                        
        ## Used to print ADC and current measurements on a regular interval if used
        if print_measurements_enabled and current_time_ms - last_print_time_ms >= sample_interval_ms:
            ## Sample all ADCs and Current Sensor
            measurements = sample_measurements(sensor_state)
            print(build_response(net_name_list, net_colors_list, measurements, status_message_list))
            last_print_time_ms = current_time_ms

else:
    print('Unable to transmit to Excel, please set "ignore_dtr = 1;" in the [usb_cdc] section of config.txt')
    ## Connect OLED to print message about needed config setting(s)
    if try_connecting_oled():
        oled_set_font("Berkeley Mono")
        oled_print("\n\n\n\nPlease set\nignore_dtr = 1;\nin config.txt\nand reboot", 0)
        oled_disconnect()
    
## abort script by reaching the end
print("script will now exit")