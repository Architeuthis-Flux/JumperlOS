#include "SyntaxHighlighting.h"
#include "SingleCharCommands.h"
#include <string.h>

// Map editor highlight class to 256-color index
static int map_hl_to_color(int hl) {
  switch (hl) {
    case HL_COMMENT: return 34;      // green
    case HL_MLCOMMENT: return 244;   // gray
    case HL_KEYWORD1: return 214;    // orange
    case HL_KEYWORD2: return 79;     // green (builtins)
    case HL_STRING: return 39;       // cyan
    case HL_NUMBER: return 199;      // red
    case HL_MATCH: return 27;        // blue
    case HL_JUMPERLESS_FUNC: return 207; // magenta
    case HL_JUMPERLESS_TYPE: return 105; // purple
    case HL_JFS_FUNC: return 45;     // cyan-blue
    case HL_TERMINAL_FUNC: return 197; // magenta
    case HL_TERMINAL_NUMBER: return 199; // red
    case HL_TERMINAL_NODE1: return 78; // green
    case HL_TERMINAL_NODE2: return 79; // green
    case HL_TERMINAL_STRING: return 39; // cyan
    default: return 255;             // white
  }
}

// Python keyword arrays - consolidated from EkiloEditor.cpp
static const char* python_keywords[] = {
    "and", "as", "assert", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "exec", "finally", "for", "from", "global",
    "if", "import", "in", "is", "lambda", "not", "or", "pass", "print",
    "raise", "return", "try", "while", "with", "yield", "async", "await",
    "nonlocal", "True", "False", "None",
    nullptr
};

static const char* python_builtins[] = {
    "abs", "all", "any", "bin", "bool", "bytes", "callable",
    "chr", "dict", "dir", "enumerate", "eval", "filter", "float",
    "format", "getattr", "globals", "hasattr", "hash", "help", "hex",
    "id", "input", "int", "isinstance", "iter", "len", "list",
    "locals", "map", "max", "min", "next", "object", "oct", "open",
    "ord", "pow", "print", "range", "repr", "reversed", "round",
    "set", "setattr", "slice", "sorted", "str", "sum", "super",
    "tuple", "type", "vars", "zip", "self", "cls",
    nullptr
};

static const char* jumperless_functions[] = {
    // DAC/ADC
    "dac_set", "dac_get", "set_dac", "get_dac", "adc_get", "get_adc",
    // INA current sensing
    "ina_get_current", "ina_get_voltage", "ina_get_bus_voltage", "ina_get_power",
    "get_current", "get_voltage", "get_bus_voltage", "get_power",
    "get_ina_current", "get_ina_voltage", "get_ina_bus_voltage", "get_ina_power",
    // GPIO
    "gpio_set", "gpio_get", "gpio_set_dir", "gpio_get_dir", "gpio_set_pull", "gpio_get_pull",
    "set_gpio", "get_gpio", "set_gpio_dir", "get_gpio_dir", "set_gpio_pull", "get_gpio_pull",
    "gpio_set_read_floating", "gpio_get_read_floating",
    "set_gpio_read_floating", "get_gpio_read_floating",
    // GPIO pin ownership
    "gpio_claim_pin", "gpio_release_pin", "gpio_release_all_pins",
    // Node connections
    "connect", "disconnect", "fast_connect", "fast_disconnect", "is_connected", "nodes_clear", "node",
    "nodes_save", "nodes_discard", "nodes_has_changes", "switch_slot",
    // Net information API
    "get_net_name", "set_net_name", "get_net_color", "get_net_color_name", "set_net_color", "set_net_color_hsv",
    "get_num_nets", "get_num_bridges", "get_net_nodes", "get_bridge", "get_net_info", "get_all_nets",
    "net_name", "net_info",
    // Path query functions
    "get_num_paths", "get_path_info", "get_all_paths", "get_path_between",
    // State functions
    "get_state", "set_state",
    // Context
    "context_toggle", "context_get",
    // OLED
    "oled_print", "oled_clear", "oled_show", "oled_connect", "oled_disconnect",
    "oled_set_text_size", "oled_get_text_size", "oled_copy_print",
    "oled_get_fonts", "oled_set_font", "oled_get_current_font",
    "oled_load_bitmap", "oled_display_bitmap", "oled_show_bitmap_file",
    "oled_get_framebuffer", "oled_set_framebuffer", "oled_get_framebuffer_size",
    "oled_set_pixel", "oled_get_pixel",
    // Clickwheel
    "clickwheel_up", "clickwheel_down", "clickwheel_press",
    "clickwheel_get_position", "clickwheel_reset_position", "clickwheel_get_direction",
    "clickwheel_get_button", "clickwheel_is_initialized",
    // Status/debug
    "print_bridges", "print_paths", "print_crossbars", "print_nets", "print_chip_status",
    // Probe
    "probe_read", "read_probe", "probe_read_blocking", "probe_read_nonblocking",
    "get_button", "probe_button", "probe_button_blocking", "probe_button_nonblocking",
    "probe_wait", "wait_probe", "probe_touch", "wait_touch", "button_read", "read_button",
    "check_button", "button_check",
    // Probe switch
    "get_switch_position", "set_switch_position", "check_switch_position", "probe_tap",
    "probe_autoconnect",
    // Misc
    "arduino_reset", "run_app", "pause_core2", "send_raw",
    "help", "nodes_help",
    "change_terminal_color", "cycle_term_color",
    // Service management
    "force_service", "force_service_by_index", "get_service_index",
    // PWM
    "pwm", "pwm_set_duty_cycle", "pwm_set_frequency", "pwm_stop",
    "set_pwm", "set_pwm_duty_cycle", "set_pwm_frequency", "stop_pwm",
    // WaveGen setters
    "wavegen_set_output", "set_wavegen_output",
    "wavegen_set_freq", "set_wavegen_freq",
    "wavegen_set_wave", "set_wavegen_wave",
    "wavegen_set_sweep", "set_wavegen_sweep",
    "wavegen_set_amplitude", "set_wavegen_amplitude",
    "wavegen_set_offset", "set_wavegen_offset",
    "wavegen_start", "start_wavegen",
    "wavegen_stop", "stop_wavegen",
    // WaveGen getters
    "wavegen_get_output", "get_wavegen_output",
    "wavegen_get_freq", "get_wavegen_freq",
    "wavegen_get_wave", "get_wavegen_wave",
    "wavegen_get_amplitude", "get_wavegen_amplitude",
    "wavegen_get_offset", "get_wavegen_offset",
    "wavegen_is_running",
    // Graphic Overlays
    "overlay_set", "overlay_clear", "overlay_clear_all", "overlay_set_pixel",
    "overlay_count", "overlay_shift", "overlay_place", "overlay_serialize",
    // FakeGPIO classes
    "FakeGpioPin", "FakeGpioDisconnect",
    // Legacy filesystem
    "fs_exists", "fs_listdir", "fs_read", "fs_write", "fs_cwd",
    nullptr
};

static const char* jumperless_types[] = {
    // GPIO states
    "HIGH", "LOW", "FLOATING",
    // GPIO directions
    "INPUT", "OUTPUT",
    // Power rails
    "TOP_RAIL", "T_RAIL", "BOTTOM_RAIL", "BOT_RAIL", "B_RAIL", "GND",
    // DAC/ADC
    "DAC0", "DAC1", "DAC_0", "DAC_1",
    "ADC0", "ADC1", "ADC2", "ADC3", "ADC4", "ADC7",
    // Current sense
    "ISENSE_PLUS", "ISENSE_MINUS", "ISENSE_P", "ISENSE_N", "I_P", "I_N",
    "CURRENT_SENSE_PLUS", "CURRENT_SENSE_MINUS", "CURRENT_SENSE_P", "CURRENT_SENSE_N",
    // UART/Buffer
    "UART_TX", "UART_RX", "TX", "RX", "BUFFER_IN", "BUFFER_OUT", "BUF_IN", "BUF_OUT",
    // GPIO pins
    "GPIO_1", "GPIO_2", "GPIO_3", "GPIO_4", "GPIO_5", "GPIO_6", "GPIO_7", "GPIO_8",
    "GP1", "GP2", "GP3", "GP4", "GP5", "GP6", "GP7", "GP8",
    "GPIO_20", "GPIO_21", "GPIO_22", "GPIO_23", "GPIO_24", "GPIO_25", "GPIO_26", "GPIO_27",
    // Arduino digital pins
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11", "D12", "D13",
    "NANO_D0", "NANO_D1", "NANO_D2", "NANO_D3", "NANO_D4", "NANO_D5", "NANO_D6", "NANO_D7",
    "NANO_D8", "NANO_D9", "NANO_D10", "NANO_D11", "NANO_D12", "NANO_D13",
    // Arduino analog pins
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
    "NANO_A0", "NANO_A1", "NANO_A2", "NANO_A3", "NANO_A4", "NANO_A5", "NANO_A6", "NANO_A7",
    // Probe pads - digital
    "D0_PAD", "D1_PAD", "D2_PAD", "D3_PAD", "D4_PAD", "D5_PAD", "D6_PAD", "D7_PAD",
    "D8_PAD", "D9_PAD", "D10_PAD", "D11_PAD", "D12_PAD", "D13_PAD",
    // Probe pads - analog
    "A0_PAD", "A1_PAD", "A2_PAD", "A3_PAD", "A4_PAD", "A5_PAD", "A6_PAD", "A7_PAD",
    // Probe pads - rails/special
    "TOP_RAIL_PAD", "BOTTOM_RAIL_PAD", "BOT_RAIL_PAD",
    "TOP_RAIL_GND", "TOP_GND_PAD", "BOTTOM_RAIL_GND", "BOT_RAIL_GND", "BOTTOM_GND_PAD", "BOT_GND_PAD",
    "LOGO_PAD_TOP", "LOGO_PAD_BOTTOM", "BUILDING_PAD_TOP", "BUILDING_PAD_BOTTOM",
    "GPIO_PAD", "DAC_PAD", "ADC_PAD",
    "RESET_PAD", "AREF_PAD", "NO_PAD",
    // Probe pads - nano power/control
    "NANO_VIN", "VIN_PAD", "NANO_RESET_0", "RESET_0_PAD", "NANO_RESET_1", "RESET_1_PAD",
    "NANO_GND_0", "GND_0_PAD", "NANO_GND_1", "GND_1_PAD",
    "NANO_3V3", "3V3_PAD", "NANO_5V", "5V_PAD",
    // Probe buttons
    "BUTTON_NONE", "BUTTON_CONNECT", "BUTTON_REMOVE",
    "CONNECT_BUTTON", "REMOVE_BUTTON",
    // Probe switch states
    "SWITCH_MEASURE", "SWITCH_SELECT", "SWITCH_UNKNOWN",
    // Clickwheel states
    "CLICKWHEEL_NONE", "CLICKWHEEL_UP", "CLICKWHEEL_DOWN",
    "CLICKWHEEL_IDLE", "CLICKWHEEL_PRESSED", "CLICKWHEEL_HELD",
    "CLICKWHEEL_RELEASED", "CLICKWHEEL_DOUBLECLICKED",
    // Waveforms
    "SINE", "TRIANGLE", "SAWTOOTH", "SQUARE", "RAMP", "ARBITRARY",
    // FakeGPIO modes
    "FAKE_GPIO_INPUT", "FAKE_GPIO_OUTPUT",
    // Slot
    "CURRENT_SLOT",
    nullptr
};

static const char* jfs_functions[] = {
    // JFS file operations
    "open", "read", "write", "close", "seek", "tell", "position", "size", "available",
    "flush", "print", "name",
    // JFS directory/path operations
    "exists", "listdir", "mkdir", "rmdir", "remove", "rename", "stat", "info",
    // JFS seek constants
    "SEEK_SET", "SEEK_CUR", "SEEK_END",
    // Legacy fs_ prefixed functions
    "fs_exists", "fs_listdir", "fs_read", "fs_write", "fs_cwd",
    nullptr
};

SyntaxHighlighting::SyntaxHighlighting(Stream* stream) {
  syntaxHighlightingStream = stream;
}

SyntaxHighlighting::~SyntaxHighlighting() {
}

void SyntaxHighlighting::setStream(Stream* stream) {
  syntaxHighlightingStream = stream;
}

void SyntaxHighlighting::syntaxHighlighting() {
}

void SyntaxHighlighting::syntaxHighlightingToColor(int hl) {
  if (!syntaxHighlightingStream) return;
  int color = map_hl_to_color(hl);
  syntaxHighlightingStream->print("\x1b[38;5;");
  syntaxHighlightingStream->print(color);
  syntaxHighlightingStream->print("m");
}

// Keyword classification methods
bool SyntaxHighlighting::isPythonKeyword(const String& word) {
  for (int i = 0; python_keywords[i] != nullptr; i++) {
    if (word == python_keywords[i]) {
      return true;
    }
  }
  return false;
}

bool SyntaxHighlighting::isPythonBuiltin(const String& word) {
  for (int i = 0; python_builtins[i] != nullptr; i++) {
    if (word == python_builtins[i]) {
      return true;
    }
  }
  return false;
}

bool SyntaxHighlighting::isJumperlessFunction(const String& word) {
  for (int i = 0; jumperless_functions[i] != nullptr; i++) {
    if (word == jumperless_functions[i]) {
      return true;
    }
  }
  return false;
}

bool SyntaxHighlighting::isJumperlessType(const String& word) {
  for (int i = 0; jumperless_types[i] != nullptr; i++) {
    if (word == jumperless_types[i]) {
      return true;
    }
  }
  return false;
}

bool SyntaxHighlighting::isJFSFunction(const String& word) {
  for (int i = 0; jfs_functions[i] != nullptr; i++) {
    if (word == jfs_functions[i]) {
      return true;
    }
  }
  return false;
}

// Classify a Python identifier
PythonKeywordType SyntaxHighlighting::classifyPythonKeyword(const String& word) {
  if (isPythonKeyword(word)) return KW_PYTHON;
  if (isPythonBuiltin(word)) return KW_BUILTIN;
  if (isJumperlessFunction(word)) return KW_JUMPERLESS;
  if (isJumperlessType(word)) return KW_JUMPERLESS_TYPE;
  if (isJFSFunction(word)) return KW_JFS;
  return KW_NONE;
}

// Terminal command set derived from main.cpp switch cases
static bool is_terminal_command_char(char c) {
  switch (c) {
    case 'z': case '|': case 'w': case 'X': case 'G': case 'S': case 'j': case 'U':
    case 'u': case '/': case 'C': case 'E': case 'k': case 'R': case '>': case 'P':
    case 'p': case '.': case 'c': case '_': case 'g': case '&': case '\'': case 'x':
    case '+': case '-': case '~': case '`': case '^': case '?': case '@': case '$':
    case 'r': case 'A': case 'a': case 'F': case '=': case 'i': case '#': case 'e':
    case 's': case 'v': case '}': case '{': case 'n': case 'b': case 'm': case '!':
    case 'o': case '<': case 'y': case 'f': case 't': case 'T': case 'l': case 'd':
    case ':':
      return true;
    default:
      break;
  }
  if (c >= '0' && c <= '3') return true; // menu choices in Z
  return false;
}

// Returns static buffer containing input string with ANSI sequences inserted
char* SyntaxHighlighting::highlightString(const char* string, enum SyntaxHighlightingType type) {
  static char out[1024];  // Increased to handle longer lines with ANSI codes
  if (!string) {
    out[0] = '\0';
    return out;
  }

  size_t len = strlen(string);
  if (len == 0) {
    out[0] = '\0';
    return out;
  }

  // Always keep result bounded
  const size_t max_out = sizeof(out) - 1;
  size_t pos = 0;

  auto push = [&](char ch) {
    if (pos + 1 < max_out) out[pos++] = ch;
  };
  auto push_str = [&](const char* s) {
    while (*s && pos + 1 < max_out) out[pos++] = *s++;
  };
  auto push_color = [&](int color) {
    char seq[24];
    snprintf(seq, sizeof(seq), "\x1b[38;5;%dm", color);
    push_str(seq);
  };
  auto push_reset = [&]() {
    push_str("\x1b[0m");
  };

  if (type == SYNTAX_TERMINAL) {
    // Per-character classification; typically called with single-char input
    for (size_t i = 0; i < len; i++) {
      char c = string[i];
      if (is_terminal_command_char(c)) {
        push_color(map_hl_to_color(HL_JUMPERLESS_FUNC));
        push(c);
        push_reset();
      } else if ((c >= '0' && c <= '9')) {
        push_color(map_hl_to_color(HL_NUMBER));
        push(c);
        push_reset();
      } else if (c == '\'' || c == '"') {
        push_color(map_hl_to_color(HL_STRING));
        push(c);
        push_reset();
      } else if (c == '#') {
        push_color(map_hl_to_color(HL_COMMENT));
        push(c);
        push_reset();
      } else {
        push_color(map_hl_to_color(HL_NORMAL));
        push(c);
        push_reset();
      }
    }
  } else if (type == SYNTAX_JUMPERLESS_CONNECTIONS) {
    // Smart connection highlighting for Jumperless node connections
    String input_str = String(string);
    String highlighted = highlightJumperlessConnections(input_str);
    
    // Copy the highlighted string to output buffer
    const char* highlighted_cstr = highlighted.c_str();
    size_t highlighted_len = highlighted.length();
    for (size_t i = 0; i < highlighted_len && pos + 1 < max_out; i++) {
      out[pos++] = highlighted_cstr[i];
    }
  } else {
    // Default: no transformation for now
    for (size_t i = 0; i < len; i++) push(string[i]);
  }

  out[pos] = '\0';
  return out;
}

// Check if character is a registered terminal command
bool SyntaxHighlighting::isTerminalCommand(char c) {
  return singleCharCommands.getCommand( c ) != nullptr;
}

// Get friendly name for a node (number -> name mapping)
String SyntaxHighlighting::getNodeFriendlyName(const String& input) {
  String trimmed = input;
  trimmed.trim();
  
  // Handle numeric inputs - map ONLY the actual Arduino pin node numbers
  int nodeNum = trimmed.toInt();
  if (nodeNum > 0 && trimmed == String(nodeNum)) {
    switch (nodeNum) {
      // Arduino digital pins (70-83 range) - DON'T map small numbers!
      case 70: return "D0";   case 71: return "D1";   case 72: return "D2";
      case 73: return "D3";   case 74: return "D4";   case 75: return "D5";
      case 76: return "D6";   case 77: return "D7";   case 78: return "D8";
      case 79: return "D9";   case 80: return "D10";  case 81: return "D11";
      case 82: return "D12";  case 83: return "D13";
      // Analog pins (86-93 range)
      case 86: return "A0";   case 87: return "A1";   case 88: return "A2";
      case 89: return "A3";   case 90: return "A4";   case 91: return "A5";
      case 92: return "A6";   case 93: return "A7";
      // Power and special nodes (100+ range)
      case 100: return "GND"; case 101: return "TOP_RAIL"; case 102: return "BOTTOM_RAIL";
      case 103: return "3V3"; case 105: return "5V";
      case 106: return "DAC0"; case 107: return "DAC1";
      case 108: return "ISENSE_PLUS"; case 109: return "ISENSE_MINUS";
      case 110: return "ADC0"; case 111: return "ADC1"; case 112: return "ADC2"; case 113: return "ADC3";
      default: return trimmed;  // Return as-is (breadboard rows 1-30 stay as numbers)
    }
  }
  
  // Handle common name variants
  if (trimmed == "GND" || trimmed == "GROUND") return "GND";
  if (trimmed == "5V" || trimmed == "+5V") return "5V";
  if (trimmed == "3V3" || trimmed == "3.3V") return "3V3";
  if (trimmed.startsWith("D") && trimmed.length() <= 3) return trimmed;
  if (trimmed.startsWith("A") && trimmed.length() <= 2) return trimmed;
  if (trimmed == "TOP_RAIL" || trimmed == "TOPRAIL") return "TOP_RAIL";
  if (trimmed == "BOTTOM_RAIL" || trimmed == "BOTTOMRAIL") return "BOTTOM_RAIL";
  
  return trimmed;  // Return input if no specific mapping found
}

// Check if a string is a valid node name/number
bool SyntaxHighlighting::isValidNodeName(const String& name) {
  String trimmed = name;
  trimmed.trim();
  
  // Check if it's a number in valid range
  int nodeNum = trimmed.toInt();
  if (nodeNum > 0 && trimmed == String(nodeNum)) {
    return (nodeNum >= 1 && nodeNum <= 150);  // Valid node number range
  }
  
  // Check common node name patterns
  if (trimmed.startsWith("D") || trimmed.startsWith("A") || 
      trimmed == "GND" || trimmed == "5V" || trimmed == "3V3" ||
      trimmed.startsWith("DAC") || trimmed.startsWith("ADC") ||
      trimmed.startsWith("GPIO") || trimmed.indexOf("RAIL") != -1) {
    return true;
  }
  
  return false;
}

// Highlight a single connection (node1-node2)
String SyntaxHighlighting::highlightSingleConnection(const String& connection) {
  String upper_connection = connection;
  upper_connection.toUpperCase();
  upper_connection.trim();
  
  // Look for dash in the connection
  int dash_pos = upper_connection.indexOf('-');
  if (dash_pos <= 0 || dash_pos >= upper_connection.length() - 1) {
    return connection;  // No valid dash found, return as-is
  }
  
  String before_dash = upper_connection.substring(0, dash_pos);
  String after_dash = upper_connection.substring(dash_pos + 1);
  before_dash.trim();
  after_dash.trim();
  
  // Map to friendly names and validate
  String node1_name = getNodeFriendlyName(before_dash);
  String node2_name = getNodeFriendlyName(after_dash);
  
  // Only highlight if both nodes are recognized
  if (isValidNodeName(before_dash) && isValidNodeName(after_dash)) {
    int node1_color = map_hl_to_color(HL_TERMINAL_NODE1);
    int node2_color = map_hl_to_color(HL_TERMINAL_NODE2);
    return "\x1b[38;5;" + String(node1_color) + "m" + node1_name + "\x1b[0m-\x1b[38;5;" + String(node2_color) + "m" + node2_name + "\x1b[0m";
  }
  
  return connection;  // Return original if not recognized
}

// Process comma-separated connections
String SyntaxHighlighting::processConnections(const String& connections) {
  String result = "";
  String working_input = connections;
  working_input.trim();
  
  // Process comma-separated connections
  int comma_pos = 0;
  while ((comma_pos = working_input.indexOf(',')) != -1 || working_input.length() > 0) {
    String connection_part;
    if (comma_pos != -1) {
      connection_part = working_input.substring(0, comma_pos);
      working_input = working_input.substring(comma_pos + 1);
    } else {
      connection_part = working_input;
      working_input = "";
    }
    
    connection_part.trim();
    String highlighted_part = highlightSingleConnection(connection_part);
    
    if (result.length() > 0) {
      result += ", ";
    }
    result += highlighted_part;
    
    if (working_input.length() == 0) break;
  }
  
  return result;
}


// Python syntax highlighting with full Jumperless support
String SyntaxHighlighting::highlightPythonCode(const String& code) {
  String result = "";
  int i = 0;
  
  while (i < code.length()) {
    char c = code.charAt(i);
    
    // Handle comments first (highest priority)
    if (c == '#') {
      int comment_color = map_hl_to_color(HL_COMMENT);
      result += "\x1b[38;5;" + String(comment_color) + "m";
      // Consume rest of line as comment
      while (i < code.length() && code.charAt(i) != '\n') {
        result += code.charAt(i);
        i++;
      }
      result += "\x1b[0m"; // Reset color
      continue;
    }
    
    // Handle strings (second priority)
    if (c == '"' || c == '\'') {
      int string_color = map_hl_to_color(HL_STRING);
      result += "\x1b[38;5;" + String(string_color) + "m";
      char quote = c;
      result += c;
      i++;
      // Consume entire string including the closing quote
      while (i < code.length() && code.charAt(i) != quote) {
        if (code.charAt(i) == '\\' && i + 1 < code.length()) {
          result += code.charAt(i);  // Backslash
          i++;
          if (i < code.length()) {
            result += code.charAt(i);  // Escaped char
            i++;
          }
        } else {
          result += code.charAt(i);
          i++;
        }
      }
      if (i < code.length()) {
        result += code.charAt(i); // Closing quote
        i++;
      }
      result += "\x1b[0m"; // Reset color
      continue;
    }
    
    // Handle numbers
    if (isdigit(c) || (c == '.' && i + 1 < code.length() && isdigit(code.charAt(i + 1)))) {
      int number_color = map_hl_to_color(HL_NUMBER);
      result += "\x1b[38;5;" + String(number_color) + "m";
      while (i < code.length() && (isdigit(code.charAt(i)) || code.charAt(i) == '.')) {
        result += code.charAt(i);
        i++;
      }
      result += "\x1b[0m"; // Reset color
      continue;
    }
    
    // Handle keywords and identifiers
    if (isalpha(c) || c == '_') {
      int start = i;
      while (i < code.length() && (isalnum(code.charAt(i)) || code.charAt(i) == '_')) {
        i++;
      }
      
      String word = code.substring(start, i);
      
      // Classify the word and choose appropriate color
      PythonKeywordType kw_type = classifyPythonKeyword(word);
      int color = 255; // Default white
      
      switch (kw_type) {
        case KW_PYTHON:
          color = map_hl_to_color(HL_KEYWORD1); // Orange for Python keywords
          break;
        case KW_BUILTIN:
          color = map_hl_to_color(HL_KEYWORD2); // Green for Python builtins
          break;
        case KW_JUMPERLESS:
          color = map_hl_to_color(HL_JUMPERLESS_FUNC); // Magenta for Jumperless functions
          break;
        case KW_JUMPERLESS_TYPE:
          color = map_hl_to_color(HL_JUMPERLESS_TYPE); // Purple for Jumperless constants
          break;
        case KW_JFS:
          color = map_hl_to_color(HL_JFS_FUNC); // Cyan-blue for JFS functions
          break;
        case KW_NONE:
        default:
          color = 255; // White for unknown identifiers
          break;
      }
      
      if (color != 255) {
        result += "\x1b[38;5;" + String(color) + "m" + word + "\x1b[0m";
      } else {
        result += word;  // Don't color unknown words
      }
      continue;
    }
    
    // Default character (operators, whitespace, punctuation)
    result += c;
    i++;
  }
  
  return result;
}

// Stream output version of Python highlighting
void SyntaxHighlighting::displayPythonWithHighlighting(const String& text, Stream* stream) {
  if (!stream || text.length() == 0) return;
  
  String highlighted = highlightPythonCode(text);
  stream->print(highlighted);
}

// Smart highlighting for Jumperless connections
String SyntaxHighlighting::highlightJumperlessConnections(const String& input) {
  // DON'T trim - preserve exact spacing to maintain cursor alignment
  String working_input = input;
  
  // Check for single terminal commands first
  if (working_input.length() == 1) {
    char cmd = working_input.charAt(0);
    if (isTerminalCommand(cmd)) {
      int cmd_color = map_hl_to_color(HL_TERMINAL_FUNC);
      return "\x1b[38;5;" + String(cmd_color) + "m" + working_input + "\x1b[0m";
    }
  }
  
  // Handle commands followed by data
  if (working_input.length() > 1 && isTerminalCommand(working_input.charAt(0))) {
    String result = "";
    char cmd = working_input.charAt(0);
    int cmd_color = map_hl_to_color(HL_TERMINAL_FUNC);
    
    // Debug output
    // Serial.print("[DEBUG: Command detected: '");
    // Serial.print(cmd);
    // Serial.print("' with data: '");
    // Serial.print(working_input.substring(1));
    // Serial.println("']");
    
    // Highlight the command character
    result += "\x1b[38;5;" + String(cmd_color) + "m" + String(cmd) + "\x1b[0m";
    
    // Process the rest EXACTLY as typed (preserve all spacing)
    String command_part = working_input.substring(1);
    if (command_part.length() > 0) {
      if (cmd == '>') {
        // Python command - apply Python syntax highlighting
      //  Serial.println("[DEBUG: Processing Python command]");
        result += highlightPythonCode(command_part);
      } else {
        // Connection command - apply highlighting but preserve spacing
        //Serial.println("[DEBUG: Processing connection command]");
        result += highlightConnectionsPreserveSpaces(command_part);
      }
    }
    
    return result;
  }
  
  // Fall back to processing as pure connections with preserved spacing
  return highlightConnectionsPreserveSpaces(working_input);
}

// Highlight connections while preserving exact input spacing
String SyntaxHighlighting::highlightConnectionsPreserveSpaces(const String& input) {
  String result = "";
  int i = 0;
  
  while (i < input.length()) {
    char c = input.charAt(i);
    
    // Look for potential node patterns (numbers or names)
    if (isdigit(c) || isalpha(c)) {
      int start = i;
      // Collect the whole word/number
      while (i < input.length() && (isalnum(input.charAt(i)) || input.charAt(i) == '_')) {
        i++;
    }
    
      String token = input.substring(start, i);
      
      // Check if this is a valid node that should be highlighted
      if (isValidNodeName(token)) {
        String friendly_name = getNodeFriendlyName(token);
        int node_color = map_hl_to_color(HL_TERMINAL_NODE1);
        result += "\x1b[38;5;" + String(node_color) + "m" + friendly_name + "\x1b[0m";
      } else {
        result += token;  // Not recognized, keep as-is
      }
    } else {
      // Non-alphanumeric character - just copy as-is
      result += c;
      i++;
    }
  }
  
  return result;
}

// Helper function for Python syntax highlighting (used elsewhere)
// Now uses the centralized SyntaxHighlighting class with a static instance
// to avoid object creation overhead on every call
void displayStringWithSyntaxHighlighting(const String& text, Stream* stream) {
  if (text.length() == 0 || !stream) return;
  
  // Use a static highlighter instance to avoid repeated construction/destruction
  // The stream is set each time in case it changes between calls
  static SyntaxHighlighting highlighter(nullptr);
  highlighter.setStream(stream);
  highlighter.displayPythonWithHighlighting(text, stream);
}
