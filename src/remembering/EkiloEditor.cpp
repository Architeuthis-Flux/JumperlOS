/*
 * EkiloEditor.cpp - Arduino-compatible eKilo text editor for Jumperless
 * Based on the original eKilo editor by Antonio Foti
 * Adapted for Arduino/embedded systems by removing Unix dependencies
 */

#include "EkiloEditor.h"
#include "Graphics.h"
#include "Jerial.h"
#include "oled.h"
#include "RotaryEncoder.h"
#include "JumperlessDefines.h"
#include "States.h"  // For SlotManager service calls
#include "externVars.h"  // For fs_mutex filesystem synchronization
#include "FilesystemStuff.h"  // For safe file operations
#include "JumperlOS.h"  // For ContextManager
#include "SharedBuffer.h"  // For zero-copy transfer to Python REPL
#include <time.h>

// External references
extern class oled oled;

// Global editor state
static EditorConfig E;

// Track currently open file for external monitoring (e.g., SlotManager preview mode)
static char* g_currently_editing_file = nullptr;

// Key definitions
#define KEY_NULL 0
#define CTRL_C 3
#define CTRL_D 4
#define CTRL_F 6
#define CTRL_H 8
#define TAB 9
#define CTRL_L 12
#define ENTER 13
#define CTRL_P 16
#define CTRL_Q 17
#define CTRL_S 19
#define CTRL_U 21
#define ESC 27
#define BACKSPACE 127

// Virtual key codes for special keys
#define ARROW_LEFT 1000
#define ARROW_RIGHT 1001
#define ARROW_UP 1002
#define ARROW_DOWN 1003
#define DEL_KEY 1004
#define HOME_KEY 1005
#define END_KEY 1006
#define PAGE_UP 1007
#define PAGE_DOWN 1008

// Syntax highlighting
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2
#define HL_MLCOMMENT 3
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8
#define HL_JUMPERLESS_FUNC 9
#define HL_JUMPERLESS_TYPE 10
#define HL_JFS_FUNC 11

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

// Python syntax highlighting
const char* python_extensions[] = {".py", ".pyw", ".pyi", nullptr};
const char* python_keywords[] = {
    "and", "as", "assert", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "exec", "finally", "for", "from", "global",
    "if", "import", "in", "is", "lambda", "not", "or", "pass", "print",
    "raise", "return", "try", "while", "with", "yield", "async", "await",
    "nonlocal", "True", "False", "None",
    
    // Python Built-ins (marked with |)
    "abs|", "all|", "any|", "bin|", "bool|", "bytes|", "callable|",
    "chr|", "dict|", "dir|", "enumerate|", "eval|", "filter|", "float|",
    "format|", "getattr|", "globals|", "hasattr|", "hash|", "help|", "hex|",
    "id|", "input|", "int|", "isinstance|", "iter|", "len|", "list|",
    "locals|", "map|", "max|", "min|", "next|", "object|", "oct|", "open|",
    "ord|", "pow|", "print|", "range|", "repr|", "reversed|", "round|",
    "set|", "setattr|", "slice|", "sorted|", "str|", "sum|", "super|",
    "tuple|", "type|", "vars|", "zip|", "self|", "cls|",
    
    // Jumperless Functions (marked with ||)
    "dac_set||", "dac_get||", "set_dac||", "get_dac||", "adc_get||", "get_adc||",
    "ina_get_current||", "ina_get_voltage||", "ina_get_bus_voltage||", "ina_get_power||",
    "get_current||", "get_voltage||", "get_bus_voltage||", "get_power||",
    "gpio_set||", "gpio_get||", "gpio_set_dir||", "gpio_get_dir||", "gpio_set_pull||", "gpio_get_pull||",
    "set_gpio||", "get_gpio||", "set_gpio_dir||", "get_gpio_dir||", "set_gpio_pull||", "get_gpio_pull||",
    "connect||", "disconnect||", "is_connected||", "nodes_clear||", "node||",
    "oled_print||", "oled_clear||", "oled_connect||", "oled_disconnect||",
    "clickwheel_up||", "clickwheel_down||", "clickwheel_press||",
    "print_bridges||", "print_paths||", "print_crossbars||", "print_nets||", "print_chip_status||",
    "probe_read||", "read_probe||", "probe_read_blocking||", "probe_read_nonblocking||",
    "get_button||", "probe_button||", "probe_button_blocking||", "probe_button_nonblocking||",
    "probe_wait||", "wait_probe||", "probe_touch||", "wait_touch||", "button_read||", "read_button||",
    "check_button||", "button_check||", "arduino_reset||", "probe_tap||", "run_app||", "format_output||",
    "help_nodes||", "pause_core2||", "send_raw||", "pwm||", "pwm_set_duty_cycle||", "pwm_set_frequency||", "pwm_stop||", "nodes_save||",
    
    // Wavegen Functions (marked with ||)
    "wavegen_set_output||", "set_wavegen_output||",
    "wavegen_set_freq||", "set_wavegen_freq||",
    "wavegen_set_wave||", "set_wavegen_wave||",
    "wavegen_set_sweep||", "set_wavegen_sweep||",
    "wavegen_set_amplitude||", "set_wavegen_amplitude||",
    "wavegen_set_offset||", "set_wavegen_offset||",
    "wavegen_start||", "start_wavegen||",
    "wavegen_stop||", "stop_wavegen||",
    "wavegen_get_output||", "get_wavegen_output||",
    "wavegen_get_freq||", "get_wavegen_freq||",
    "wavegen_get_wave||", "get_wavegen_wave||",
    "wavegen_get_amplitude||", "get_wavegen_amplitude||",
    "wavegen_get_offset||", "get_wavegen_offset||",
    "wavegen_is_running||",
    
    // Jumperless Types/Constants (marked with |||)
    "TOP_RAIL|||", "BOTTOM_RAIL|||", "GND|||", "DAC0|||", "DAC1|||", "ADC0|||", "ADC1|||", "ADC2|||", "ADC3|||", "ADC4|||",
    "PROBE|||", "ISENSE_PLUS|||", "ISENSE_MINUS|||", "UART_TX|||", "UART_RX|||", "BUFFER_IN|||", "BUFFER_OUT|||",
    "GPIO_1|||", "GPIO_2|||", "GPIO_3|||", "GPIO_4|||", "GPIO_5|||", "GPIO_6|||", "GPIO_7|||", "GPIO_8|||",
    "D0|||", "D1|||", "D2|||", "D3|||", "D4|||", "D5|||", "D6|||", "D7|||", "D8|||", "D9|||", "D10|||", "D11|||", "D12|||", "D13|||",
    "A0|||", "A1|||", "A2|||", "A3|||", "A4|||", "A5|||", "A6|||", "A7|||",
    "D13_PAD|||", "TOP_RAIL_PAD|||", "BOTTOM_RAIL_PAD|||", "LOGO_PAD_TOP|||", "LOGO_PAD_BOTTOM|||",
    "CONNECT_BUTTON|||", "REMOVE_BUTTON|||", "BUTTON_NONE|||", "CONNECT|||", "REMOVE|||", "NONE|||",
    
    // Wavegen constants (marked with |||)
    "SINE|||", "TRIANGLE|||", "SAWTOOTH|||", "SQUARE|||", "RAMP|||", "ARBITRARY|||",
    
    // JFS Functions (marked with ||||)
    "open||||", "read||||", "write||||", "close||||", "seek||||", "tell||||", "size||||", "available||||",
    "exists||||", "listdir||||", "mkdir||||", "rmdir||||", "remove||||", "rename||||", "stat||||", "info||||",
    "SEEK_SET||||", "SEEK_CUR||||", "SEEK_END||||", "nodes_save||||", 
    
    // Basic filesystem functions (marked with ||||)
    "fs_exists||||", "fs_listdir||||", "fs_read||||", "fs_write||||", "fs_cwd||||",
    
    nullptr
};


SyntaxDefinition syntax_db[] = {
    {
        python_extensions,
        python_keywords,
        "#", "", "",  // Single line comment, no multi-line for Python
        HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
    }
};

// Editor row structure
typedef struct EditorRow {
    int idx;
    int size;
    int rsize;
    char* chars;           // Line content - may point into SharedBuffer or be heap-allocated
    char* render;          // Rendered version (with tabs expanded)
    unsigned char* hl;     // Syntax highlighting
    int hl_oc;
    bool owns_chars;       // True if chars was malloc'd, false if pointing into SharedBuffer
} EditorRow;

EditorConfig::EditorConfig() {
    cx = cy = 0;
    rowoff = coloff = 0;
    screenrows = DEFAULT_EDITOR_ROWS;  // Use configurable default
    screencols = DEFAULT_EDITOR_COLS;
    numrows = 0;
    row_capacity = 0;  // Will grow exponentially to reduce fragmentation
    row = nullptr;
    dirty = 0;
    filename = nullptr;
    syntax = nullptr;
    should_quit = 0;
    strcpy(statusmsg, "");
    statusmsg_time = 0;
    
    // Initialize OLED horizontal scrolling
    oled_horizontal_offset = 0;
    
    // Initialize OLED batching
    oled_last_input_time = 0;
    oled_update_pending = false;
    
    // Initialize clickwheel character selection
    char_selection_mode = false;
    selected_char_index = 0;
    char_selection_timer = 0;
    
    // Initialize encoder position tracking
    last_encoder_position = 0;
    last_encoder_update = 0;
    
    // Initialize button state tracking
    last_button_state = true; // Button is active low
    button_debounce_time = 0;
    
    // Initialize menu navigation
    in_menu_mode = false;
    menu_selection = 0;
    
    // Initialize Ctrl+P functionality
    should_launch_repl = false;
    
    // Initialize screen refresh optimization
    screen_dirty = true; // Start with dirty screen to force initial draw
    
    // Initialize status message history
    status_history_head = 0;
    status_history_count = 0;
    for (int i = 0; i < STATUS_HISTORY_SIZE; i++) {
        status_history[i][0] = '\0';
    }
    
    // Initialize read-only and low memory modes
    read_only = false;
    low_memory_mode = false;
}

EditorConfig::~EditorConfig() {
    // Cleanup will be handled by the editor
}

// True only while every row still points into the SharedBuffer image of the
// file that was loaded (zero-copy load, no row added or removed since). It is
// the precondition ekilo_save's fast path needs and used to be assumed.
static bool s_bufferMirrorsRows = false;

// Initialize the editor
void ekilo_init() {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row_capacity = 0;
    E.row = nullptr;
    E.dirty = 0;
    E.filename = nullptr;
    E.syntax = nullptr;
    E.should_quit = 0;
    E.should_launch_repl = false;  // CRITICAL: Reset between sessions to prevent stale Ctrl+P state
    // Per-session modes: E is constructed once at boot, so a wheel Cancel
    // that quit from the menu left in_menu_mode set and the NEXT session
    // opened in menu mode; one fragmented launch set low_memory_mode forever.
    E.in_menu_mode = false;
    E.menu_selection = 0;
    E.char_selection_mode = false;
    E.selected_char_index = 0;
    E.char_selection_timer = 0;
    E.low_memory_mode = false;   // ekilo_run recomputes it from the heap test
    E.read_only = false;         // ekilo_open sets it per file
    E.oled_update_pending = false;
    s_bufferMirrorsRows = false;
    strcpy(E.statusmsg, "");
    E.statusmsg_time = 0;
    setTerminalLineBuffering(true); // editor needs raw keystrokes
    
    // Clear any pending input that might be CPR garbage from TUI or other sources
    // Do this FIRST before anything else
   // delay(50);  // Wait for any in-flight CPR responses to arrive
    // while (Jerial.available()) {
    //     Jerial.read();
    // }
   // delay(10);  // Brief extra wait
    while (Jerial.available()) {
        Jerial.read();
    }
    
    // Use conservative defaults - terminal size probing disabled due to 
    // CPR response timing issues causing garbage output and crashes
    E.screenrows = DEFAULT_EDITOR_ROWS - 1; // Account for help header
    E.screencols = DEFAULT_EDITOR_COLS;
    
    // Mark screen as dirty for initial draw
    E.screen_dirty = true;
}

// Memory management for dynamic buffer with capacity tracking
// Uses exponential growth to reduce realloc fragmentation
struct Buffer {
    char* b;
    int len;
    int capacity;  // Track capacity to avoid realloc on every append
};

// Dynamic memory allocation - use what's available
#define MIN_FREE_HEAP (1536)      // Keep only 1.5KB reserved for system
#define CRITICAL_FREE_HEAP (512)  // Emergency threshold

bool check_memory_available(size_t needed) {
    size_t freeHeap = rp2040.getFreeHeap();
    // Allow allocation if we have enough for what's needed plus a small buffer
    return (freeHeap > needed + MIN_FREE_HEAP);
}

// More aggressive check for critical operations
bool check_memory_critical(size_t needed) {
    size_t freeHeap = rp2040.getFreeHeap();
    return (freeHeap > needed + CRITICAL_FREE_HEAP);
}

// Optimized buffer append with exponential growth
// This dramatically reduces heap fragmentation during screen refresh
void buffer_append(Buffer* ab, const char* s, int len) {
    if (!ab || !s || len <= 0) return;
    
    // Check if we need to grow
    if (ab->len + len > ab->capacity) {
        // Exponential growth: start at 4KB, then double
        // 4KB is enough for a typical 35x80 screen with escape codes
        int new_capacity = (ab->capacity == 0) ? 4096 : ab->capacity * 2;
        
        // Ensure we have enough for this append
        while (new_capacity < ab->len + len) {
            new_capacity *= 2;
        }
        
        // Cap at reasonable max to avoid over-allocation
        if (new_capacity > 32768) new_capacity = ab->len + len + 1024;
        
        char* new_buf = (char*)realloc(ab->b, new_capacity);
        if (new_buf == nullptr) {
            // Try minimal growth as fallback
            new_capacity = ab->len + len + 64;
            new_buf = (char*)realloc(ab->b, new_capacity);
            if (new_buf == nullptr) {
                return;  // Failed silently
            }
        }
        ab->b = new_buf;
        ab->capacity = new_capacity;
    }
    
    memcpy(ab->b + ab->len, s, len);
    ab->len += len;
}

void buffer_free(Buffer* ab) {
    if (!ab) return;
    free(ab->b);
    ab->b = nullptr;
    ab->len = 0;
    ab->capacity = 0;
}

// ============================================================================
// Input Handler Implementation - Batches keys and provides acceleration
// ============================================================================

InputHandler g_input_handler;

void InputHandler::init() {
    last_key = 0;
    last_category = KEY_CAT_NONE;
    last_key_time = 0;
    repeat_start_time = 0;
    repeat_count = 0;
}

KeyCategory InputHandler::categorize(int key) {
    switch (key) {
        case ARROW_UP:    return KEY_CAT_UP;
        case ARROW_DOWN:  return KEY_CAT_DOWN;
        case ARROW_LEFT:  return KEY_CAT_LEFT;
        case ARROW_RIGHT: return KEY_CAT_RIGHT;
        case PAGE_UP:     return KEY_CAT_PAGEUP;
        case PAGE_DOWN:   return KEY_CAT_PAGEDOWN;
        default:          return KEY_CAT_OTHER;
    }
}

bool InputHandler::shouldAccelerate() {
    if (repeat_count < 3) return false;  // Need at least 3 repeats
    uint32_t now = millis();
    uint32_t held_time = now - repeat_start_time;
    uint32_t since_last = now - last_key_time;
    
    // Reset acceleration if gap too long (key was released and re-pressed)
    if (since_last > ACCEL_TIMEOUT_MS) {
        return false;
    }
    
    return held_time > ACCEL_START_MS;
}

int InputHandler::processInput(Stream* stream, int& moves) {
    moves = 1;  // Default: single move
    uint32_t now = millis();
    
    // Check if we should reset state (key released)
    if (last_category != KEY_CAT_NONE && last_category != KEY_CAT_OTHER) {
        uint32_t gap = now - last_key_time;
        if (gap > REPEAT_THRESHOLD_MS) {
            // Key was released - aggressively clear queue and reset state
            clearRepeats(stream);
            last_category = KEY_CAT_NONE;
            repeat_count = 0;
            repeat_start_time = 0;
        }
    }
    
    if (!stream->available()) {
        return 0;
    }
    
    // Read the first key
    int first_key = ekilo_read_key();
    if (first_key == 0) return 0;
    
    KeyCategory cat = categorize(first_key);
    
    // Check if this is a continuation of same direction within timeout
    if (cat != KEY_CAT_OTHER && cat == last_category) {
        uint32_t since_last = now - last_key_time;
        if (since_last < ACCEL_TIMEOUT_MS) {
            repeat_count++;
        } else {
            // Gap too long - treat as new press, reset acceleration
            repeat_start_time = now;
            repeat_count = 1;
        }
    } else {
        // New direction or non-repeatable key
        repeat_start_time = now;
        repeat_count = 1;
    }
    
    last_key = first_key;
    last_category = cat;
    last_key_time = now;
    
    // For non-directional keys, just return immediately
    if (cat == KEY_CAT_OTHER) {
        return first_key;
    }
    
    // For directional keys, batch only a few pending same-direction keys
    int batch_count = 1;
    while (stream->available() && batch_count < MAX_BATCH_SIZE) {
        int peek_key = ekilo_read_key();
        if (peek_key == 0) break;
        
        KeyCategory peek_cat = categorize(peek_key);
        if (peek_cat == cat) {
            // Same direction - batch it
            batch_count++;
            repeat_count++;
        } else {
            // Different key - stop batching
            break;
        }
    }
    
    // Apply conservative acceleration if key is genuinely held
    if (shouldAccelerate()) {
        moves = batch_count * ACCEL_MULTIPLIER;
        if (moves > 8) moves = 8;  // Strict cap
    } else {
        moves = batch_count;
        if (moves > 3) moves = 3;  // Limit even without acceleration
    }
    
    return first_key;
}

void InputHandler::clearRepeats(Stream* stream) {
    // Aggressively clear ALL pending directional keys from the buffer
    // This prevents "runaway" cursor when key is released
    if (last_category == KEY_CAT_NONE || last_category == KEY_CAT_OTHER) return;
    
    int cleared = 0;
    while (stream->available() && cleared < 100) {  // Clear up to 100 pending keys
        int peek = ekilo_read_key();
        if (peek == 0) break;
        
        KeyCategory peek_cat = categorize(peek);
        if (peek_cat != KEY_CAT_OTHER) {
            // Any directional key - discard it (not just same direction)
            cleared++;
        } else {
            // Non-directional key - stop clearing, this one is important
            break;
        }
    }
}

// Memory monitoring and cleanup functions
void ekilo_show_memory_status() {
    size_t freeHeap = rp2040.getFreeHeap();
    size_t usedMemory = 0;
    
    // Calculate memory used by editor content
    for (int i = 0; i < E.numrows; i++) {
        if (E.row[i].chars) usedMemory += E.row[i].size;
        if (E.row[i].render) usedMemory += E.row[i].rsize;
        if (E.row[i].hl) usedMemory += E.row[i].rsize;
    }
    
    ekilo_set_status_message("Memory: %dKB free, %dKB used by editor", 
                           freeHeap / 1024, usedMemory / 1024);
}

// Emergency cleanup function
void ekilo_emergency_cleanup() {
    // Free all allocated memory to prevent further crashes
    for (int i = 0; i < E.numrows; i++) {
        ekilo_free_row(&E.row[i]);
    }
    free(E.row);
    E.row = nullptr;
    E.numrows = 0;
    E.row_capacity = 0;
    
    free(E.filename);
    E.filename = nullptr;
    
    ekilo_set_status_message("Emergency cleanup completed - editor reset");
}

// External monitoring - get currently editing file
const char* ekilo_get_currently_editing_file() {
    return g_currently_editing_file;
}

// External monitoring - get current buffer content for live preview
String ekilo_get_current_buffer_content() {
    if (E.numrows == 0) {
        return String("");
    }
    
    // Build string from all rows
    String content;
    content.reserve(E.numrows * 80);  // Pre-allocate reasonable size
    
    for (int i = 0; i < E.numrows; i++) {
        if (E.row[i].chars) {
            content += String(E.row[i].chars);
        }
        if (i < E.numrows - 1) {
            content += '\n';  // Add newline between rows
        }
    }
    
    return content;
}

// Track if we recently sent a CPR query (to filter late responses)
uint32_t last_cpr_query_time = 0;  // Non-static so probe function can update it
static const uint32_t CPR_RESPONSE_WINDOW = 1000;  // 1 second window for late responses

// Check if input looks like a stray CPR response remnant (digits;digitsR)
// Only call this when we have reason to believe CPR garbage might be present
static bool consume_stray_cpr() {
    // Only check if we're within the CPR response window
    if (millis() - last_cpr_query_time > CPR_RESPONSE_WINDOW) {
        return false;
    }
    
    // Look for pattern: digit(s) ; digit(s) R
    if (!Jerial.available()) return false;
    
    char first = Jerial.peek();
    if (first < '0' || first > '9') return false;
    
    // Looks like it might be CPR remnant - consume it
    char buf[20];
    int n = 0;
    
    while (Jerial.available() && n < 15) {
        char c = Jerial.read();
        buf[n++] = c;
        if (c == 'R') {
            // Definitely was a CPR - we consumed it
            return true;
        }
        if (c != ';' && (c < '0' || c > '9')) {
            // Not CPR pattern - stop, but we've already consumed some chars
            break;
        }
    }
    
    return n > 0;  // We consumed something
}

// Arduino-compatible key reading
int ekilo_read_key() {
    // First, check for and consume any stray CPR responses
    while (consume_stray_cpr()) {
        // Keep consuming until buffer is clean
    }
    
    if (!Jerial.available()) return 0;
    
    char c = Jerial.read();
    
    // Handle escape sequences (arrow keys, etc.)
    if (c == ESC) {
        // Wait (bounded) for the rest of a sequence: a split arrow key used to
        // read as a bare ESC, which quit the editor.
        unsigned long start = millis();
        while (millis() - start < 8 && !Jerial.available()) {
            delayMicroseconds(200);
        }
        
        if (!Jerial.available()) return ESC;
        
        char seq[3];
        seq[0] = Jerial.read();
        if (seq[0] != '[' && seq[0] != 'O') {
            return 0;   // Alt+key / unknown - ignore, never "quit"
        }
        while (millis() - start < 8 && !Jerial.available()) {
            delayMicroseconds(200);
        }
        if (!Jerial.available()) return 0;
        seq[1] = Jerial.read();
        
        if (seq[0] == '[') {
            // Check if this is a CPR response (ESC [ digits ; digits R)
            if (seq[1] >= '0' && seq[1] <= '9') {
                // Could be CPR or function key - read more
                char buf[20];
                int n = 0;
                buf[n++] = seq[1];
                
                while (Jerial.available() && n < 15) {
                    char next = Jerial.read();
                    buf[n++] = next;
                    if (next == 'R') {
                        // This is a CPR response - discard it
                        return 0;
                    }
                    if (next == '~') {
                        // This is a function key sequence
                        switch (seq[1]) {
                            case '1': return HOME_KEY;
                            case '3': return DEL_KEY;
                            case '4': return END_KEY;
                            case '5': return PAGE_UP;
                            case '6': return PAGE_DOWN;
                            case '7': return HOME_KEY;
                            case '8': return END_KEY;
                        }
                        return ESC;
                    }
                    if (next != ';' && (next < '0' || next > '9')) {
                        break;  // Unknown sequence (final byte consumed)
                    }
                }
                return 0;   // undecoded CSI (Ctrl/Shift+arrow, F-keys): ignore
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'A': return ARROW_UP;     // SS3 arrows (application cursor mode)
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return 0;   // any other decoded-but-unknown sequence: ignore
    } else {
        return c;
    }
}

// Check if character is a separator for syntax highlighting
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != nullptr;
}

// Update syntax highlighting for a row
void ekilo_update_syntax(EditorRow* row) {
    if (!row || row->rsize <= 0) return;
    
    unsigned char* new_hl = (unsigned char*)realloc(row->hl, row->rsize);
    if (!new_hl) {
        // If realloc fails, we can still function without syntax highlighting
        free(row->hl);
        row->hl = nullptr;
        return;
    }
    row->hl = new_hl;
    memset(row->hl, HL_NORMAL, row->rsize);
    
    if (E.syntax == nullptr) return;
    
    const char** keywords = E.syntax->keywords;
    const char* scs = E.syntax->singleline_comment_start;
    const char* mcs = E.syntax->multiline_comment_start;
    const char* mce = E.syntax->multiline_comment_end;
    
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;
    
    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_oc);
    
    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
        
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }
        
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }
        
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int highlight_type = HL_KEYWORD1;
                
                // Check for different keyword types based on suffix
                if (klen >= 4 && !strncmp(&keywords[j][klen - 4], "||||", 4)) {
                    highlight_type = HL_JFS_FUNC;
                    klen -= 4;
                } else if (klen >= 3 && !strncmp(&keywords[j][klen - 3], "|||", 3)) {
                    highlight_type = HL_JUMPERLESS_TYPE;
                    klen -= 3;
                } else if (klen >= 2 && !strncmp(&keywords[j][klen - 2], "||", 2)) {
                    highlight_type = HL_JUMPERLESS_FUNC;
                    klen -= 2;
                } else if (klen >= 1 && keywords[j][klen - 1] == '|') {
                    highlight_type = HL_KEYWORD2;
                    klen--;
                }
                
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], highlight_type, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != nullptr) {
                prev_sep = 0;
                continue;
            }
        }
        
        prev_sep = is_separator(c);
        i++;
    }
    
    int changed = (row->hl_oc != in_comment);
    row->hl_oc = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        ekilo_update_syntax(&E.row[row->idx + 1]);
}

// Convert syntax highlighting to ANSI color codes (256-color mode)
int ekilo_syntax_to_color(int hl) {
    switch (hl) {
        case HL_COMMENT: return 34; // Green
        case HL_MLCOMMENT: return 244; // Light gray (much more subtle than bright cyan)
        case HL_KEYWORD1: return 214;  // Orange (vibrant and distinct)
        case HL_KEYWORD2: return 79;   // Forest green (rich green for built-ins)
        case HL_STRING: return 39;    
        case HL_NUMBER: return 199;    
        case HL_MATCH: return 27;      
        case HL_JUMPERLESS_FUNC: return 207;  
        case HL_JUMPERLESS_TYPE: return 105; 
        case HL_JFS_FUNC: return 45;   // Cyan-blue (distinct for filesystem functions)
        default: return 255;           // Bright white (default text)
    }
}

// Select syntax highlighting based on filename
void ekilo_select_syntax_highlight(const char* filename) {
    E.syntax = nullptr;
    if (filename == nullptr) return;
    
    const char* ext = strrchr(filename, '.');
    if (ext == nullptr) return;
    
    for (unsigned int j = 0; j < sizeof(syntax_db) / sizeof(syntax_db[0]); j++) {
        SyntaxDefinition* s = &syntax_db[j];
        for (int i = 0; s->filematch[i]; i++) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(filename, s->filematch[i]))) {
                E.syntax = s;
                
                // Re-highlight all rows
                for (int filerow = 0; filerow < E.numrows; filerow++) {
                    ekilo_update_syntax(&E.row[filerow]);
                }
                return;
            }
        }
    }
}

// Convert tabs to spaces and calculate render string
void ekilo_update_row(EditorRow* row) {
    if (!row) return;
    
    // In low memory mode, skip render buffer entirely - use chars directly
    if (E.low_memory_mode) {
        free(row->render);
        row->render = nullptr;
        row->rsize = row->size;  // Display will use chars directly
        free(row->hl);
        row->hl = nullptr;
        return;
    }
    
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }
    
    size_t needed_size = row->size + tabs * 7 + 1;
    
    free(row->render);
    row->render = (char*)malloc(needed_size);
    if (!row->render) {
        // Silently fail - we can still edit, just no fancy rendering.
        // Drop the old hl too: it was sized for the previous render and the
        // refresh loop indexes it against the (possibly longer) chars.
        row->rsize = 0;
        free(row->hl);
        row->hl = nullptr;
        return;
    }
    
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
    
    ekilo_update_syntax(row);
}

// Insert a row at the specified position
void ekilo_insert_row(int at, const char* s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    if (!s) return;
    s_bufferMirrorsRows = false;   // (ekilo_open re-asserts it after the load)
    
    // Just check if we have critical memory available - be aggressive about using what we have
    size_t freeHeap = rp2040.getFreeHeap();
    size_t needed = sizeof(EditorRow) + len + 64;  // Row struct + content + small buffer
    
    if (freeHeap < needed + CRITICAL_FREE_HEAP) {
        ekilo_set_status_message("ERROR: Low memory (%dKB free, need %d bytes)", freeHeap / 1024, needed);
        return;
    }
    
    // Use exponential growth strategy to reduce realloc fragmentation
    // Only realloc when we exceed capacity, doubling each time
    if (E.numrows >= E.row_capacity) {
        int new_capacity = (E.row_capacity == 0) ? 32 : E.row_capacity * 2;
        
        // Cap at reasonable maximum to avoid over-allocation
        if (new_capacity > 10000) new_capacity = E.numrows + 100;
        
        EditorRow* new_rows = (EditorRow*)realloc(E.row, sizeof(EditorRow) * new_capacity);
        if (!new_rows) {
            // Try smaller growth if doubling fails
            new_capacity = E.numrows + 16;
            new_rows = (EditorRow*)realloc(E.row, sizeof(EditorRow) * new_capacity);
            if (!new_rows) {
                ekilo_set_status_message("ERROR: Memory allocation failed for row array");
                return;
            }
        }
        E.row = new_rows;
        E.row_capacity = new_capacity;
    }
    
    memmove(&E.row[at + 1], &E.row[at], sizeof(EditorRow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;
    
    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = (char*)malloc(len + 1);
    if (!E.row[at].chars) {
        ekilo_set_status_message("ERROR: Memory allocation failed for row content");
        // Revert the row array changes
        memmove(&E.row[at], &E.row[at + 1], sizeof(EditorRow) * (E.numrows - at));
        for (int j = at; j < E.numrows; j++) E.row[j].idx--;
        return;
    }
    
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    
    E.row[at].rsize = 0;
    E.row[at].render = nullptr;
    E.row[at].hl = nullptr;
    E.row[at].hl_oc = 0;
    E.row[at].owns_chars = true;  // We malloc'd this memory
    ekilo_update_row(&E.row[at]);
    
    E.numrows++;
    E.dirty++;
}

// Insert a row that references SharedBuffer (zero-copy)
// chars points directly into SharedBuffer and should NOT be freed
void ekilo_insert_row_ref(int at, const char* chars, size_t len) {
    if (at < 0 || at > E.numrows) return;
    
    // Grow row array if needed (same as ekilo_insert_row)
    if (E.numrows >= E.row_capacity) {
        int new_capacity = (E.row_capacity == 0) ? 32 : E.row_capacity * 2;
        if (new_capacity > 10000) new_capacity = E.numrows + 100;
        
        EditorRow* new_rows = (EditorRow*)realloc(E.row, sizeof(EditorRow) * new_capacity);
        if (!new_rows) {
            new_capacity = E.numrows + 16;
            new_rows = (EditorRow*)realloc(E.row, sizeof(EditorRow) * new_capacity);
            if (!new_rows) return;
        }
        E.row = new_rows;
        E.row_capacity = new_capacity;
    }
    
    memmove(&E.row[at + 1], &E.row[at], sizeof(EditorRow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;
    
    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = (char*)chars;  // Point directly into SharedBuffer
    E.row[at].rsize = len;           // Set render size (used when render is null)
    E.row[at].render = nullptr;
    E.row[at].hl = nullptr;
    E.row[at].hl_oc = 0;
    E.row[at].owns_chars = false;  // DO NOT free - points into SharedBuffer
    
    E.numrows++;
    // Don't increment dirty - file just loaded
    
    // NOTE: We skip ekilo_update_row() here to avoid allocating render buffers
    // for all rows at once (causes memory fragmentation on reopen).
    // Render buffers will be created lazily when rows are displayed.
}

// Make a row mutable by copying its content to heap (copy-on-write)
void ekilo_row_make_mutable(EditorRow* row) {
    if (!row || row->owns_chars) return;  // Already mutable
    
    // Copy from SharedBuffer to heap
    char* new_chars = (char*)malloc(row->size + 1);
    if (!new_chars) {
        // Memory allocation failed - can't make mutable
        return;
    }
    
    if (row->chars && row->size > 0) {
        memcpy(new_chars, row->chars, row->size);
    }
    new_chars[row->size] = '\0';
    
    row->chars = new_chars;
    row->owns_chars = true;
}

// Free a row's memory
void ekilo_free_row(EditorRow* row) {
    free(row->render);
    // Only free chars if the row owns the memory (not pointing into SharedBuffer)
    if (row->owns_chars) {
        free(row->chars);
    }
    free(row->hl);
    row->chars = nullptr;
    row->render = nullptr;
    row->hl = nullptr;
    row->owns_chars = false;
}

// Delete a row
void ekilo_del_row(int at) {
    if (at < 0 || at >= E.numrows) return;
    s_bufferMirrorsRows = false;   // the buffer image no longer equals the rows
    
    ekilo_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(EditorRow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

// Insert character in a row
void ekilo_row_insert_char(EditorRow* row, int at, int c) {
    if (!row || at < 0 || at > row->size) {
        if (row && (at < 0 || at > row->size)) at = row->size;
        else return;
    }
    
    // Copy-on-write: if row points into SharedBuffer, copy to heap first
    ekilo_row_make_mutable(row);
    if (!row->owns_chars) {
        ekilo_set_status_message("ERROR: Cannot edit row (memory full)");
        return;
    }
    
    char* new_chars = (char*)realloc(row->chars, row->size + 2);
    if (!new_chars) {
        // Show error but don't spam
        static uint32_t last_error_time = 0;
        if (millis() - last_error_time > 2000) {
            ekilo_set_status_message("ERROR: Low memory - can't insert");
            last_error_time = millis();
        }
        return;
    }
    row->chars = new_chars;
    
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    ekilo_update_row(row);
    E.dirty++;
}

// Delete character from a row
void ekilo_row_del_char(EditorRow* row, int at) {
    if (!row || at < 0 || at >= row->size) return;
    
    // Copy-on-write: if row points into SharedBuffer, copy to heap first
    ekilo_row_make_mutable(row);
    if (!row->owns_chars) {
        ekilo_set_status_message("ERROR: Cannot edit row (memory full)");
        return;
    }
    
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    ekilo_update_row(row);
    E.dirty++;
}

// Append string to a row
void ekilo_row_append_string(EditorRow* row, char* s, size_t len) {
    if (!row || !s || len == 0) return;
    
    // Copy-on-write: if row points into SharedBuffer, copy to heap first
    ekilo_row_make_mutable(row);
    if (!row->owns_chars) {
        ekilo_set_status_message("ERROR: Cannot edit row (memory full)");
        return;
    }
    
    char* new_chars = (char*)realloc(row->chars, row->size + len + 1);
    if (!new_chars) {
        ekilo_set_status_message("ERROR: Low memory - can't append");
        return;
    }
    row->chars = new_chars;
    
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    ekilo_update_row(row);
    E.dirty++;
}

// Insert character at cursor
void ekilo_insert_char(int c) {
    if (E.cy == E.numrows) {
        ekilo_insert_row(E.numrows, "", 0);
    }
    ekilo_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
    
    // Schedule OLED update after character insertion
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Insert newline
void ekilo_insert_newline() {
    if (E.cx == 0) {
        ekilo_insert_row(E.cy, "", 0);
    } else {
        EditorRow* row = &E.row[E.cy];
        ekilo_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        ekilo_update_row(row);
        
        // When splitting a line, also update the new row to ensure proper syntax highlighting
        if (E.cy + 1 < E.numrows) {
            ekilo_update_row(&E.row[E.cy + 1]);
            
            // Update syntax highlighting for subsequent rows since line split can affect
            // multiline comments and other context-dependent highlighting
            for (int i = E.cy + 2; i < E.numrows; i++) {
                ekilo_update_row(&E.row[i]);
            }
        }
    }
    E.cy++;
    E.cx = 0;
    
    // Reset horizontal scrolling when moving to new line
    E.oled_horizontal_offset = 0;
    
    // Schedule OLED update after newline insertion
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Delete character
void ekilo_del_char() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;
    
    EditorRow* row = &E.row[E.cy];
    if (E.cx > 0) {
        ekilo_row_del_char(row, E.cx - 1);
        E.cx--;
    } else {
        // Joining lines - need to update syntax highlighting for subsequent lines
        E.cx = E.row[E.cy - 1].size;
        ekilo_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        ekilo_del_row(E.cy);
        E.cy--;
        // Reset horizontal scrolling when moving to previous line
        E.oled_horizontal_offset = 0;
        
        // When joining lines, update syntax highlighting for subsequent rows since 
        // line joining can affect multiline comments and other context-dependent highlighting
        for (int i = E.cy; i < E.numrows; i++) {
            ekilo_update_row(&E.row[i]);
        }
    }
    
    // Schedule OLED update after character deletion
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Set status message and add to history
void ekilo_set_status_message(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = millis();
    
    // Add to status history ring buffer (skip duplicates of last message)
    int prev_idx = (E.status_history_head - 1 + EditorConfig::STATUS_HISTORY_SIZE) % EditorConfig::STATUS_HISTORY_SIZE;
    if (E.status_history_count == 0 || strcmp(E.status_history[prev_idx], E.statusmsg) != 0) {
        strncpy(E.status_history[E.status_history_head], E.statusmsg, sizeof(E.status_history[0]) - 1);
        E.status_history[E.status_history_head][sizeof(E.status_history[0]) - 1] = '\0';
        E.status_history_head = (E.status_history_head + 1) % EditorConfig::STATUS_HISTORY_SIZE;
        if (E.status_history_count < EditorConfig::STATUS_HISTORY_SIZE) {
            E.status_history_count++;
        }
    }
    
    // Make error messages persist much longer (30 seconds vs 5 seconds)
    if (strstr(E.statusmsg, "ERROR:") || strstr(E.statusmsg, "WARNING:")) {
        E.statusmsg_time = millis() - 25000; // Show for 30 seconds instead of 5
    }
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Move cursor based on key
void ekilo_move_cursor(int key) {
    EditorRow* row = (E.cy >= E.numrows) ? nullptr : &E.row[E.cy];
    
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
                // Reset horizontal scrolling when changing lines
                E.oled_horizontal_offset = 0;
                
                // Auto-scroll if cursor moves above visible area
                if (E.cy < E.rowoff) {
                    E.rowoff = E.cy;
                }
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
                // Reset horizontal scrolling when changing lines
                E.oled_horizontal_offset = 0;
                
                // Auto-scroll if cursor moves below visible area
                int available_rows = E.screenrows - 4; // -4 for header+status+message+help
                if (E.cy >= E.rowoff + available_rows) {
                    E.rowoff = E.cy - available_rows + 1;
                }
            }
            break;
    }
    
    row = (E.cy >= E.numrows) ? nullptr : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
    
    // Schedule OLED update when cursor moves
    ekilo_schedule_oled_update();
    
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Open file - loads entire file into SharedBuffer, then parses into EditorRows
// Rows initially point into SharedBuffer (zero-copy), converted to heap on edit
int ekilo_open(const char* filename) {
    if (!filename) return -1;

    Serial.printf("[ekilo_open] Opening: %s\n", filename);
    Serial.printf("[ekilo_open] Free heap: %d\n", rp2040.getFreeHeap());

    // Brief delay to ensure any recent file operations have completed
    delay(2);
    yield();
    
    // Get file size first
    int32_t file_size = safeFileSize(filename, 2000);
    Serial.printf("[ekilo_open] safeFileSize returned: %d\n", file_size);
    
    if (file_size < 0) {
        // Retry after a brief delay
        delay(50);
        file_size = safeFileSize(filename, 2000);
        if (file_size < 0) {
            ekilo_set_status_message("ERROR: Cannot open file '%s'", filename);
            // -2 = EXISTS but could not be read (fs timeout/mutex): the caller
            // must not treat this as "new file" - its first save would
            // truncate the real file to whatever is in the empty buffer.
            return safeFileExists(filename, 1000) ? -2 : -1;
        }
    }
    
    // Hard limit: file must fit in SharedBuffer with room for edits
    if ((size_t)file_size >= SHARED_BUFFER_SIZE - 256) {
        ekilo_set_status_message("ERROR: File too large (max %dKB)", (SHARED_BUFFER_SIZE - 256) / 1024);
        return -2; // exists, refused - see above
    }
    
    // Check if this file should be read-only
    const char* ext = strrchr(filename, '.');
    const char* lastSlash = strrchr(filename, '/');
    const char* basename = lastSlash ? lastSlash + 1 : filename;
    
    E.read_only = (ext && strcmp(ext, ".log") == 0) ||
                  (strcmp(basename, "history.txt") == 0) ||
                  (strstr(filename, "/python_scripts/history") != nullptr);
    
    // Free existing filename and allocate new one
    free(E.filename);
    E.filename = strdup(filename);
    if (!E.filename) {
        ekilo_set_status_message("ERROR: Memory allocation failed");
        return -1;
    }
    
    // Update global tracking
    free(g_currently_editing_file);
    g_currently_editing_file = strdup(filename);
    
    // Set up syntax highlighting
    ekilo_select_syntax_highlight(filename);
    
    // ========== LOAD FILE INTO SHARED BUFFER ==========
    SharedBuffer& buf = SharedBuffer::getInstance();
    buf.clear();
    buf.setFilename(filename);
    
    File file = safeFileOpen(filename, "r", 2000);
    if (!file) {
        ekilo_set_status_message("ERROR: Cannot open file for reading");
        free(E.filename);
        E.filename = nullptr;
        return -1;
    }

    // Try to use the SharedBuffer for zero-copy loading. On units without
    // PSRAM the 24KB allocation can fail, in which case rawBuffer() is null -
    // we MUST NOT read into it (that reads/writes a null pointer and shows
    // raw flash/ROM garbage). Fall back to copying each line onto the heap.
    char* data = buf.rawBuffer();

    if (data != nullptr) {
        // ---------- ZERO-COPY PATH: read whole file into SharedBuffer ----------
        size_t bytes_read = file.read((uint8_t*)data, file_size);
        safeFileClose(file, false);

        buf.setLength(bytes_read);
        Serial.printf("[ekilo_open] Read %d bytes into SharedBuffer\n", (int)bytes_read);

        // Parse SharedBuffer into EditorRows that point directly into it
        // (zero-copy). They are copied to heap on first edit (copy-on-write).
        size_t pos = 0;
        size_t line_count = 0;

        while (pos < bytes_read) {
            size_t line_start = pos;

            // Find end of line
            while (pos < bytes_read && data[pos] != '\n' && data[pos] != '\r') {
                pos++;
            }

            size_t line_len = pos - line_start;

            // Create row pointing into SharedBuffer
            ekilo_insert_row_ref(E.numrows, &data[line_start], line_len);
            line_count++;

            // Skip newline characters
            if (pos < bytes_read && data[pos] == '\r') pos++;
            if (pos < bytes_read && data[pos] == '\n') pos++;

            // Yield periodically
            if (line_count % 50 == 0) {
                yield();
            }
        }

        E.dirty = 0;
        E.screen_dirty = true;
        s_bufferMirrorsRows = true;   // zero-copy rows over the buffer image

        Serial.printf("[ekilo_open] SUCCESS: Parsed %d lines from %d bytes\n", (int)line_count, (int)bytes_read);
        ekilo_set_status_message("Loaded %d lines (%d bytes)", line_count, bytes_read);
        return 0;
    }

    // ---------- FALLBACK PATH: SharedBuffer unavailable ----------
    // No 24KB transfer buffer (e.g. no PSRAM and SRAM heap couldn't satisfy
    // the request). Read the file line-by-line into heap-allocated rows, the
    // same way ekilo_insert_row() copies content. Slower and uses more small
    // allocations, but it always produces correct content.
    Serial.println("[ekilo_open] SharedBuffer unavailable - using per-row heap load");

    size_t line_count = 0;
    size_t bytes_read = 0;

    // Accumulate one line at a time on the heap, reading byte-by-byte so we
    // don't depend on a transfer buffer and handle both LF and CRLF endings.
    String line;
    line.reserve(96);

    while (true) {
        int c = file.read();
        if (c < 0) {
            // EOF - flush any pending partial line
            if (line.length() > 0) {
                ekilo_insert_row(E.numrows, line.c_str(), line.length());
                bytes_read += line.length() + 1;
                line_count++;
            }
            break;
        }

        if (c == '\n') {
            int line_len = line.length();
            if (line_len > 0 && line[line_len - 1] == '\r') {
                line.remove(line_len - 1);  // strip trailing CR on CRLF
            }
            ekilo_insert_row(E.numrows, line.c_str(), line.length());
            bytes_read += line.length() + 1;
            line_count++;
            line = "";

            if (line_count % 25 == 0) {
                yield();
            }
        } else {
            line += (char)c;
        }
    }
    safeFileClose(file, false);

    E.dirty = 0;
    E.screen_dirty = true;

    Serial.printf("[ekilo_open] FALLBACK: Loaded %d lines (~%d bytes) onto heap\n", (int)line_count, (int)bytes_read);
    ekilo_set_status_message("Loaded %d lines (low-mem mode)", line_count);
    return 0;
}

// Write editor rows straight to flash, one row at a time.
// Used when the SharedBuffer is unavailable (e.g. no PSRAM and the 24KB
// SRAM allocation failed) so saving still works without the transfer buffer.
static int ekilo_save_direct_to_flash() {
    Serial.println("[ekilo_save] SharedBuffer unavailable - writing rows directly to flash");

    bool was_paused = pauseCore2ForFlash(100);
    File file = safeFileOpen(E.filename, "w", 2000);
    if (!file) {
        unpauseCore2ForFlash(was_paused);
        ekilo_set_status_message("ERROR: Could not open file for writing");
        return 0;
    }

    size_t total = 0;
    for (int j = 0; j < E.numrows; j++) {
        if (E.row[j].chars && E.row[j].size > 0) {
            file.write((const uint8_t*)E.row[j].chars, E.row[j].size);
            total += E.row[j].size;
        }
        file.write((uint8_t)'\n');
        total++;
        if (j % 50 == 0) yield();
    }
    file.flush();
    safeFileClose(file, true);
    unpauseCore2ForFlash(was_paused);

    ContextManager::getInstance().setTransferPath(E.filename);
    E.dirty = 0;
    E.screen_dirty = true;
    ekilo_set_status_message("%d bytes written to flash (low-mem)", total);
    return total;
}

// Save file - serialize rows to SharedBuffer, then write to flash
// In REPL mode, skip flash write and just leave content in SharedBuffer
int ekilo_save() {
    if (E.filename == nullptr) {
        ekilo_set_status_message("Save aborted - no filename");
        return 0;
    }
    if (E.read_only) {
        // the keyboard paths check this; the wheel menu's Save did not
        ekilo_set_status_message("READ-ONLY file - cannot save");
        return 0;
    }
    
    SharedBuffer& buf = SharedBuffer::getInstance();

    // If the transfer buffer can't be allocated, bypass it entirely and
    // write rows straight to flash. (Ctrl+P transfer to the REPL relies on
    // the shared buffer and won't carry content in this mode, but the file
    // is still saved correctly.)
    if (buf.rawBuffer() == nullptr) {
        return ekilo_save_direct_to_flash();
    }
    
    // ========== OPTIMIZATION: CHECK IF WE CAN SKIP SERIALIZATION ==========
    // If NO rows have been edited (all still point into SharedBuffer), 
    // then SharedBuffer already has the correct content - just mark it ready!
    bool any_rows_edited = false;
    for (int j = 0; j < E.numrows; j++) {
        if (E.row[j].owns_chars) {
            any_rows_edited = true;
            break;
        }
    }
    
    if (!any_rows_edited && buf.hasContent() && E.numrows > 0 && s_bufferMirrorsRows) {
        // Fast path: SharedBuffer already has the file content, no copy needed
        // (numrows > 0 && s_bufferMirrorsRows: a new/empty file, or one whose
        // rows were added/removed, must NOT be written from the stale image)
        buf.setFilename(E.filename);
        buf.setContentType(SharedBufferContentType::PYTHON_SCRIPT);
        buf.setSourceContext((uint8_t)ContextType::EKILO_EDITOR);
        buf.setReady(true);
        size_t len = buf.length();
        
        Serial.printf("[ekilo_save] FAST PATH: No edits, SharedBuffer already has %d bytes\n", (int)len);
        
        // Write to flash
        bool was_paused = pauseCore2ForFlash(100);
        File file = safeFileOpen(E.filename, "w", 2000);
        if (!file) {
            // Must NOT clear dirty or report success here - that silently loses
            // the user's edits if they quit believing the save worked
            unpauseCore2ForFlash(was_paused);
            ekilo_set_status_message("ERROR: Could not open file for writing");
            return 0;
        }
        file.write((uint8_t*)buf.data(), len);
        file.flush();
        safeFileClose(file, true);
        unpauseCore2ForFlash(was_paused);
        
        ContextManager::getInstance().setTransferPath(E.filename);
        E.dirty = 0;
        E.screen_dirty = true;
        ekilo_set_status_message("%d bytes written to flash", len);
        return len;
    }
    
    // ========== SLOW PATH: FILE WAS EDITED, NEED TO REBUILD SHAREDBUFFER ==========
    Serial.printf("[ekilo_save] SLOW PATH: File was edited, rebuilding SharedBuffer\n");
    
    // Copy any rows still pointing to SharedBuffer before we clear it
    int rows_copied = 0;
    for (int j = 0; j < E.numrows; j++) {
        if (!E.row[j].owns_chars && E.row[j].chars != nullptr) {
            ekilo_row_make_mutable(&E.row[j]);
            if (!E.row[j].owns_chars) {
                ekilo_set_status_message("ERROR: Out of memory at line %d", j);
                return 0;
            }
            rows_copied++;
        }
    }
    
    // Now clear and rebuild SharedBuffer
    buf.clear();
    buf.setFilename(E.filename);
    buf.setContentType(SharedBufferContentType::PYTHON_SCRIPT);
    buf.setSourceContext((uint8_t)ContextType::EKILO_EDITOR);
    
    // Calculate total size needed
    size_t totalLen = 0;
    for (int j = 0; j < E.numrows; j++) {
        totalLen += E.row[j].size + 1;
    }
    
    if (totalLen >= SHARED_BUFFER_SIZE) {
        ekilo_set_status_message("ERROR: Content too large (%dKB)", totalLen / 1024);
        return 0;
    }
    
    // Write each row to SharedBuffer
    for (int j = 0; j < E.numrows; j++) {
        if (E.row[j].chars && E.row[j].size > 0) {
            if (!buf.appendLine(E.row[j].chars, E.row[j].size)) {
                ekilo_set_status_message("ERROR: Buffer overflow at line %d", j);
                buf.clear();
                return 0;
            }
        } else {
            if (!buf.appendLine("", 0)) {
                ekilo_set_status_message("ERROR: Buffer overflow at line %d", j);
                buf.clear();
                return 0;
            }
        }
    }
    
    buf.setReady(true);
    size_t len = buf.length();
    
    // ========== WRITE SHAREDBUFFER TO FLASH ==========
    // Pause Core2 during flash write
    bool was_paused = pauseCore2ForFlash(100);
    
    File file = safeFileOpen(E.filename, "w", 2000);
    if (!file) {
        unpauseCore2ForFlash(was_paused);
        ekilo_set_status_message("ERROR: Could not open file for writing");
        return 0;
    }
    
    // Write SharedBuffer content to file
    file.write((uint8_t*)buf.data(), len);
    file.flush();
    safeFileClose(file, true);
    
    unpauseCore2ForFlash(was_paused);
    
    // Set transfer path for zero-copy communication
    ContextManager::getInstance().setTransferPath(E.filename);
    
    E.dirty = 0;
    E.screen_dirty = true;
    ekilo_set_status_message("%d bytes written to flash", len);
    return len;
}

// ============================================================================
// SHARED BUFFER SAVE - Zero-copy transfer to Python REPL
// ============================================================================

/**
 * @brief Save editor content to SharedBuffer only (no flash write)
 * 
 * This is a wrapper that sets REPL mode temporarily to avoid flash writes.
 * Used for fast transfer to Python REPL without flash wear.
 * 
 * @return Number of bytes written, 0 on error
 */
int ekilo_save_to_shared_buffer() {
    // ekilo_save() now always saves to both flash AND SharedBuffer
    return ekilo_save();
}

/**
 * @brief Load content from SharedBuffer into editor
 * 
 * Counterpart to ekilo_save_to_shared_buffer() - loads content
 * that was placed in the buffer by another context.
 * 
 * @return Number of lines loaded, -1 on error
 */
int ekilo_load_from_shared_buffer() {
    SharedBuffer& buf = SharedBuffer::getInstance();
    
    if (!buf.isReady() || !buf.hasContent()) {
        ekilo_set_status_message("No content in shared buffer");
        return -1;
    }
    
    // Get the content
    const char* content = buf.data();
    size_t contentLen = buf.length();
    
    // Set filename if available
    if (buf.hasFilename()) {
        free(E.filename);
        E.filename = strdup(buf.getFilename());
        
        // Update global tracking
        free(g_currently_editing_file);
        g_currently_editing_file = strdup(buf.getFilename());
        
        // Select syntax highlighting based on filename
        ekilo_select_syntax_highlight(E.filename);
    }
    
    // Parse content line by line
    const char* lineStart = content;
    const char* end = content + contentLen;
    int lineCount = 0;
    
    while (lineStart < end) {
        // Find end of line
        const char* lineEnd = lineStart;
        while (lineEnd < end && *lineEnd != '\n') {
            lineEnd++;
        }
        
        size_t lineLen = lineEnd - lineStart;
        
        // Skip carriage returns at end of line
        while (lineLen > 0 && lineStart[lineLen - 1] == '\r') {
            lineLen--;
        }
        
        // Insert the row
        ekilo_insert_row(E.numrows, lineStart, lineLen);
        lineCount++;
        
        // Move to next line
        lineStart = lineEnd + 1;  // Skip the newline
        
        // Yield periodically
        if (lineCount % 50 == 0) {
            yield();
        }
    }
    
    // Clear the shared buffer after consuming
    buf.clear();
    
    E.dirty = 0;
    E.screen_dirty = true;
    
    ekilo_set_status_message("Loaded %d lines from buffer", lineCount);
    return lineCount;
}

// Write buffer in chunks for Windows compatibility
void ekilo_write_buffer_chunked(Buffer* ab) {
    if (!ab) return;
    
    if (!ab->b || ab->len == 0) {
        buffer_free(ab);
        return;
    }
    
    // Optimized for speed: larger chunks, minimal delay
    const int CHUNK_SIZE = 256; // Larger chunks for faster output
    const int DELAY_MICROS = 2;  // Minimal delay - just enough for USB buffering
    
    int bytes_written = 0;
    while (bytes_written < ab->len) {
        int chunk_size = min(CHUNK_SIZE, ab->len - bytes_written);
        
        // Validate chunk boundaries
        if (chunk_size <= 0 || bytes_written + chunk_size > ab->len) {
            break; // Prevent buffer overrun
        }
        
        // Write this chunk
        Jerial.write((uint8_t*)(ab->b + bytes_written), chunk_size);
        
        bytes_written += chunk_size;
        
        // Only flush and delay every few chunks for better throughput
        if (bytes_written % 1024 == 0 && bytes_written < ab->len) {
            Jerial.flush();
            delayMicroseconds(DELAY_MICROS);
        }
    }
    
    Jerial.flush(); // Final flush
    buffer_free(ab);
}

// Quick cursor position update without full redraw
static void ekilo_update_cursor_only() {
    char buf[32];
    // +2 for header row offset
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 2, (E.cx - E.coloff) + 1);
    Jerial.print(buf);
    Jerial.flush();
}

// Track previous scroll state for optimization
static int prev_rowoff = -1;
static int prev_coloff = -1;

// Refresh screen - Buffered approach with chunked output for Windows compatibility
void ekilo_refresh_screen() {
    // Safety checks
    if (E.screenrows <= 0) E.screenrows = 24;
    if (E.screencols <= 0) E.screencols = 80;
    if (E.cy < 0) E.cy = 0;
    if (E.cx < 0) E.cx = 0;
    if (E.numrows > 0 && E.cy >= E.numrows) E.cy = E.numrows - 1;
    
    // Track if scroll changed (requires full redraw)
    bool scroll_changed = false;
    
    // Scroll handling
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
        scroll_changed = true;
    }
    {
        // only screenrows-4 file rows are drawn (status/help lines take the
        // rest) - clamping against the full height parked the cursor on an
        // undrawn row over the status bar
        const int available_rows = E.screenrows - 4;
        if (available_rows > 0 && E.cy >= E.rowoff + available_rows) {
            E.rowoff = E.cy - available_rows + 1;
            scroll_changed = true;
        }
    }
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
        scroll_changed = true;
    }
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
        scroll_changed = true;
    }
    
    // Check if scroll position changed
    if (E.rowoff != prev_rowoff || E.coloff != prev_coloff) {
        scroll_changed = true;
        prev_rowoff = E.rowoff;
        prev_coloff = E.coloff;
    }
    
    // If screen isn't dirty and scroll didn't change, just update cursor
    if (!E.screen_dirty && !scroll_changed) {
        ekilo_update_cursor_only();
        return;
    }
    
    // Free render buffers for rows that scrolled out of view to reclaim memory
    // Keep a small buffer around visible area to avoid constant reallocation
    int visible_start = E.rowoff;
    int visible_end = E.rowoff + E.screenrows + 5;  // +5 buffer
    if (visible_end > E.numrows) visible_end = E.numrows;
    int visible_start_buffered = visible_start > 5 ? visible_start - 5 : 0;
    
    for (int i = 0; i < E.numrows; i++) {
        if (i < visible_start_buffered || i >= visible_end) {
            // This row is outside visible range - free its render buffer
            if (E.row[i].render != nullptr) {
                free(E.row[i].render);
                E.row[i].render = nullptr;
                E.row[i].rsize = E.row[i].size;  // Reset to use chars size
            }
            if (E.row[i].hl != nullptr) {
                free(E.row[i].hl);
                E.row[i].hl = nullptr;
            }
        }
    }
    
    Buffer ab = {nullptr, 0, 0};  // {buffer, length, capacity} - will grow exponentially
    
    // Clear screen and position cursor
    buffer_append(&ab, "\x1b[2J\x1b[H", 7);
    
    // Add persistent help header
    buffer_append(&ab, "\x1b[48;5;199m\x1b[38;5;236m", 27); // Pink background, dark text
    char help_header[128];
    snprintf(help_header, sizeof(help_header), 
                 "                    Jumperless Kilo Text Editor                      ");
    int help_len = strlen(help_header);
    if (help_len > E.screencols) help_len = E.screencols;
    buffer_append(&ab, help_header, help_len);
    
    // Pad help header to full width
    while (help_len < E.screencols) {
        buffer_append(&ab, " ", 1);
        help_len++;
    }
    buffer_append(&ab, "\x1b[0m\r\n", 6); // Reset formatting and newline
    
    // Draw rows (adjust available rows for header and help lines)
    int available_rows = E.screenrows - 4; // -4 for header+status+message+help
    for (int y = 0; y < available_rows; y++) {
        int filerow = E.rowoff + y;
        
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == available_rows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "eKilo editor -- version 1.0.0");
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    buffer_append(&ab, "~", 1);
                    padding--;
                }
                while (padding--) buffer_append(&ab, " ", 1);
                buffer_append(&ab, welcome, welcomelen);
            } else {
                buffer_append(&ab, "~", 1);
            }
        } else {
            // Lazy render buffer creation: only allocate for visible rows
            // This prevents memory fragmentation from allocating render buffers for entire file at load time
            if (!E.low_memory_mode && E.row[filerow].render == nullptr && E.row[filerow].chars != nullptr) {
                ekilo_update_row(&E.row[filerow]);
            }
            
            // In low memory mode, render may be null - use chars directly
            char* display_str = E.row[filerow].render ? E.row[filerow].render : E.row[filerow].chars;
            int display_size = E.row[filerow].render ? E.row[filerow].rsize : E.row[filerow].size;
            
            int len = display_size - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            
            // If we have syntax highlighting, use it
            if (display_str && E.row[filerow].render && E.row[filerow].hl && !E.low_memory_mode) {
                char* c = &display_str[E.coloff];
                unsigned char* hl = &E.row[filerow].hl[E.coloff];
                int current_color = -1;
                for (int j = 0; j < len; j++) {
                    if (iscntrl(c[j])) {
                        // Show control chars as inverted symbols
                        char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                        buffer_append(&ab, "\x1b[7m", 4);
                        buffer_append(&ab, &sym, 1);
                        buffer_append(&ab, "\x1b[0m", 4);
                        if (current_color != -1) {
                            char buf[16];
                            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                            buffer_append(&ab, buf, clen);
                        }
                    } else if (hl[j] == HL_NORMAL) {
                        if (current_color != -1) {
                            buffer_append(&ab, "\x1b[0m", 4);
                            current_color = -1;
                        }
                        buffer_append(&ab, &c[j], 1);
                    } else {
                        int color = ekilo_syntax_to_color(hl[j]);
                        if (color != current_color) {
                            current_color = color;
                            char buf[16];
                            int clen = snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", color);
                            buffer_append(&ab, buf, clen);
                        }
                        buffer_append(&ab, &c[j], 1);
                    }
                }
                if (current_color != -1) {
                    buffer_append(&ab, "\x1b[0m", 4);
                }
            } else if (display_str) {
                // No syntax highlighting - just output the text directly
                buffer_append(&ab, &display_str[E.coloff], len);
            }
        }
        
        // Clear to end of line and add newline
        buffer_append(&ab, "\x1b[K\r\n", 6);
    }
    
    // Status bar with reverse video
    buffer_append(&ab, "\x1b[7m", 4);
    char status[120], rstatus[80];
    
    // Calculate available space for filename
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    const char* suffix = E.read_only ? " [READ-ONLY]" : (E.dirty ? " (modified)" : "");
    const char* menu_suffix = E.in_menu_mode ? " [MENU MODE]" : (E.low_memory_mode ? " [LOW MEM]" : "");
    
    // Calculate space needed for " - XX lines" + suffix + menu + right status + padding
    int fixed_space = strlen(" - ") + 10 + strlen(" lines") + strlen(suffix) + strlen(menu_suffix) + rlen + 2;
    int available_for_filename = E.screencols - fixed_space;
    
    // Ensure we have at least some space for filename
    if (available_for_filename < 10) {
        available_for_filename = 10;
    }
    
    const char* display_filename = E.filename ? E.filename : "[No Name]";
    
    // Build status string with dynamic filename length
    int len = snprintf(status, sizeof(status), "%.*s - %d lines%s%s",
        available_for_filename, display_filename, E.numrows, suffix, menu_suffix);
    
    if (len > E.screencols) len = E.screencols;
    buffer_append(&ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            buffer_append(&ab, rstatus, rlen);
            break;
        } else {
            buffer_append(&ab, " ", 1);
            len++;
        }
    }
    buffer_append(&ab, "\x1b[0m", 4); // Reset formatting
    buffer_append(&ab, "\r\n", 2);
    
    // Message bar - show status history (most recent messages, scrolling)
    // Show up to 2 lines of status messages
    for (int line = 0; line < 2; line++) {
        int msg_idx = (E.status_history_head - 1 - line + EditorConfig::STATUS_HISTORY_SIZE) % EditorConfig::STATUS_HISTORY_SIZE;
        
        const char* msg = "";
        if (line == 0) {
            // First line: current message (with timeout)
            uint32_t timeout = (strstr(E.statusmsg, "ERROR:") || strstr(E.statusmsg, "WARNING:")) ? 30000 : 5000;
            if (strlen(E.statusmsg) > 0 && millis() - E.statusmsg_time < timeout) {
                msg = E.statusmsg;
            }
        } else if (line < E.status_history_count && E.status_history_count > 1) {
            // Second line: previous message from history (dimmed)
            msg = E.status_history[msg_idx];
            buffer_append(&ab, "\x1b[90m", 5); // Dim/gray color
        }
        
        int msglen = strlen(msg);
        if (msglen > E.screencols) msglen = E.screencols;
        buffer_append(&ab, msg, msglen);
        
        // Pad to full width
        while (msglen < E.screencols) {
            buffer_append(&ab, " ", 1);
            msglen++;
        }
        
        if (line == 1 && E.status_history_count > 1) {
            buffer_append(&ab, "\x1b[0m", 4); // Reset color
        }
        buffer_append(&ab, "\r\n", 2);
    }
    
    // Help bar (second line - comprehensive commands in magenta)
    buffer_append(&ab, "\x1b[35m", 5); // Magenta color
    char help_line1[120];
    char help_line2[120];
    snprintf(help_line1, sizeof(help_line1), 
             "Ctrl-S = Save │ Ctrl-Q = Quit │ ↑/↓ = Navigate | CTRL-P = Save and load in MicroPython");
    snprintf(help_line2, sizeof(help_line2),
             "Tab = Indent │ Backspace = Delete │ Wheel=Move/Type │ Ctrl-U = Memory Status");
    
    int help1_len = strlen(help_line1);
    if (help1_len > E.screencols) help1_len = E.screencols;
    buffer_append(&ab, help_line1, help1_len);
    // Pad first help line to full width
    while (help1_len < E.screencols) {
        buffer_append(&ab, " ", 1);
        help1_len++;
    }
    buffer_append(&ab, "\r\n", 2);
    
    int help2_len = strlen(help_line2);
    if (help2_len > E.screencols) help2_len = E.screencols;
    buffer_append(&ab, help_line2, help2_len);
    // Pad second help line to full width
    while (help2_len < E.screencols) {
        buffer_append(&ab, " ", 1);
        help2_len++;
    }
    buffer_append(&ab, "\x1b[0m", 4); // Reset color
    
    // Position cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 2, // +2 for header
                                                (E.cx - E.coloff) + 1);
    buffer_append(&ab, buf, strlen(buf));
    
    // Write buffer in chunks for Windows compatibility
    ekilo_write_buffer_chunked(&ab);
    
    // Clear dirty flag after full redraw
    E.screen_dirty = false;
    
    // Schedule OLED update with context around cursor
    ekilo_schedule_oled_update();
    
    // Note: Don't set screen_dirty here - we only call this when screen is already dirty
    // OLED update will be processed later when screen is clean and serial buffer is empty
}

// Process keypress with batching and acceleration
void ekilo_process_keypress() {
    static int quit_times = 3;
    static bool handler_initialized = false;
    
    // Initialize input handler once
    if (!handler_initialized) {
        g_input_handler.init();
        handler_initialized = true;
    }
    
    // Use input handler for batched/accelerated input
    int moves = 1;
    int c = g_input_handler.processInput(&Jerial, moves);
    if (c == 0) return; // No key available
    
    switch (c) {
        case '\n':
        
        case ENTER:
            if (E.read_only) {
                ekilo_set_status_message("READ-ONLY - press Ctrl+Q to exit");
                break;
            }
            ekilo_insert_newline();
            break;
            
        case ESC:
            // ESC exits menu mode if in menu, otherwise quits editor without saving
            if (E.in_menu_mode) {
                // Exit menu mode with ESC
                E.in_menu_mode = false;
                if (E.numrows > 0) {
                    E.cy = E.numrows - 1; // Go to last line
                    E.cx = E.row[E.cy].size; // Go to end of line
                }
                ekilo_set_status_message("Menu mode cancelled");
                ekilo_schedule_oled_update();
                E.screen_dirty = true;
            } else if (E.dirty) {
                // A bare ESC no longer throws edits away: say how to leave.
                ekilo_set_status_message("Unsaved changes: Ctrl-S saves, Ctrl-Q x%d quits without saving", quit_times);
                E.screen_dirty = true;
            } else {
                // Not in menu mode, nothing to lose - quit editor
                E.should_quit = 1;
            }
            break;
            
        case CTRL_Q:
            if (E.dirty && quit_times > 0) {
                ekilo_set_status_message("WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            E.should_quit = 1;
            break;
            
        case CTRL_S:
            if (E.read_only) {
                ekilo_set_status_message("READ-ONLY file - cannot save");
            } else {
                ekilo_save();
            }
            break;
            
        case CTRL_P:
            // Save and launch MicroPython REPL
            if (!E.read_only) {
                ekilo_save();
            }
            E.should_launch_repl = true;
            E.should_quit = 1;
            break;
            
        case CTRL_U:
            // Show detailed memory status including MicroPython usage
            {
                size_t freeHeap = rp2040.getFreeHeap();
                size_t usedByEditor = 0;
                
                // Calculate memory used by editor content
                for (int i = 0; i < E.numrows; i++) {
                    if (E.row[i].chars) usedByEditor += E.row[i].size;
                    if (E.row[i].render) usedByEditor += E.row[i].rsize;
                    if (E.row[i].hl) usedByEditor += E.row[i].rsize;
                }
                
                ekilo_set_status_message("Memory: %dKB free, Editor:%dKB, MP:%dKB, Need:%dKB reserve", 
                                       freeHeap / 1024, usedByEditor / 1024, MICROPY_HEAP_SIZE / 1024, MIN_FREE_HEAP / 1024);
            }
            E.screen_dirty = true;
            break;
            
        case HOME_KEY:
            E.cx = 0;
            E.oled_horizontal_offset = 0; // Reset scrolling when going to start of line
            ekilo_schedule_oled_update();
            E.screen_dirty = true;
            break;
            
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            // Don't reset horizontal scrolling for END - let it scroll to show end of line
            ekilo_schedule_oled_update();
            E.screen_dirty = true;
            break;
            
        case BACKSPACE:
        case CTRL_H:
        case DEL_KEY:
            if (E.read_only) {
                ekilo_set_status_message("READ-ONLY - press Ctrl+Q to exit");
                break;
            }
            if (c == DEL_KEY) ekilo_move_cursor(ARROW_RIGHT);
            ekilo_del_char();
            break;
            
        case PAGE_UP:
        case PAGE_DOWN: {
            // Apply batched page movement (moves pages at once if held)
            for (int m = 0; m < moves; m++) {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }
                
                int times = E.screenrows;
                while (times--)
                    ekilo_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            E.screen_dirty = true;
            break;
        }
        case TAB:
            if (E.read_only) {
                ekilo_set_status_message("READ-ONLY - press Ctrl+Q to exit");
                break;
            }
            ekilo_insert_char(' ');
            ekilo_insert_char(' ');
            ekilo_insert_char(' '); 
            ekilo_insert_char(' ');
            break;
        
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            // Apply batched/accelerated movement - always allowed
            for (int i = 0; i < moves; i++) {
                ekilo_move_cursor(c);
            }
            break;
            
        case CTRL_L:
            // Refresh screen
            E.screen_dirty = true;
            break;
            
        default:
            // Block character insertion in read-only mode
            if (E.read_only) {
                // Only show message occasionally to avoid spam
                static uint32_t last_ro_msg = 0;
                if (millis() - last_ro_msg > 1000) {
                    ekilo_set_status_message("READ-ONLY file - Ctrl+Q to exit");
                    last_ro_msg = millis();
                }
                break;
            }
            ekilo_insert_char(c);
            break;
    }
    
    quit_times = 1;
}

// OLED update batching functions
void ekilo_schedule_oled_update() {
    unsigned long currentTime = millis();
    
    // Always mark as pending - we'll process it when conditions are right
    E.oled_last_input_time = currentTime;
    E.oled_update_pending = true;
}

void ekilo_process_oled_update() {
    // Only update OLED if:
    // 1. There's a pending update
    // 2. Screen is clean (not dirty) for better responsiveness
    // 3. No serial input pending (to avoid interrupting input processing)
    // 4. Enough time has passed since last input
    if (E.oled_update_pending && 
        !E.screen_dirty && 
        !Jerial.available() && 
        (millis() - E.oled_last_input_time) >= 10) { // 50ms delay
        
        ekilo_update_oled_context();
        E.oled_update_pending = false;
    }
}

// Calculate horizontal scrolling for OLED display of current line
void ekilo_calculate_oled_scrolling() {
    if (!oled.isConnected() || E.numrows == 0) return;
    
    // Get current line text
    String currentLineText = "";
    if (E.cy < E.numrows && E.row[E.cy].chars) {
        currentLineText = String(E.row[E.cy].chars);
    }
    
    // Cursor position within the current line
    int cursorPos = E.cx;
    
    // Calculate characters that fit based on actual font metrics
    int charWidth = oled.getCharacterWidth();
    const int maxVisibleChars = charWidth > 0 ? (128 / charWidth) : 21;
    
    // Calculate visible cursor position relative to current offset
    int visibleCursorPos = cursorPos - E.oled_horizontal_offset;
    
    // Ensure cursor has 2 characters of padding from right edge for adding characters
    const int CURSOR_RIGHT_PADDING = 2;
    
    // Scroll right if cursor is too close to right edge (accounting for padding)
    if (visibleCursorPos >= maxVisibleChars - CURSOR_RIGHT_PADDING) {
        E.oled_horizontal_offset = cursorPos - maxVisibleChars + CURSOR_RIGHT_PADDING + 1;
    }
    // Scroll left if cursor is too close to left edge  
    else if (visibleCursorPos < E.OLED_SCROLL_MARGIN) {
        E.oled_horizontal_offset = cursorPos - E.OLED_SCROLL_MARGIN;
    }
    
    // Keep offset within bounds
    if (E.oled_horizontal_offset < 0) {
        E.oled_horizontal_offset = 0;
    }
    
    // Allow scrolling past end of line to provide space for adding characters
    // Don't restrict offset based on line length - let cursor have padding space
}

// Update OLED with 3 lines around cursor
// Now uses generalized oled.showSmallTextBuffer() for consistency
void ekilo_update_oled_context() {
    if (!oled.isConnected()) {
        return;
    }
    
    // Check memory before OLED update - skip if critically low
    size_t freeHeap = rp2040.getFreeHeap();
    if (freeHeap < 2048) {
        return;  // Skip OLED update when memory is critically low
    }
    
    // Calculate horizontal scrolling for current line
    if (E.numrows > 0) {
        ekilo_calculate_oled_scrolling();
    }
    
    // Handle menu mode separately (different UI)
    if (E.in_menu_mode) {
        // Clear display and show menu options
        oled.clearFramebuffer();
        oled.setSmallFont(SMALL_FONT_ANDALE_MONO);
        
        // Menu button setup
        const char* saveText = "Save";
        const char* cancelText = "Cancel";
        const int saveLen = 4;
        const int cancelLen = 6;
        
        // Calculate positions for side-by-side layout
        int charWidth = oled.getCharacterWidth();
        int saveWidth = saveLen * charWidth;
        int cancelWidth = cancelLen * charWidth;
        int buttonHeight = 12;
        int buttonPadding = 4;
        int gapBetweenButtons = 8;
        
        // Center buttons horizontally
        int totalWidth = (saveWidth + buttonPadding * 2) + gapBetweenButtons + (cancelWidth + buttonPadding * 2);
        int startX = (128 - totalWidth) / 2;
        int buttonY = 10;
        
        int saveButtonX = startX;
        int cancelButtonX = startX + (saveWidth + buttonPadding * 2) + gapBetweenButtons;
        
        // Draw Save button
        if (E.menu_selection == 0) {
            oled.fillRect(saveButtonX, buttonY, saveWidth + buttonPadding * 2, buttonHeight, SSD1306_WHITE);
            oled.setTextColor(SSD1306_BLACK);
            oled.drawText(saveButtonX + buttonPadding, buttonY + 8, saveText);
            oled.setTextColor(SSD1306_WHITE);
        } else {
            oled.drawText(saveButtonX + buttonPadding, buttonY + 8, saveText);
        }
        
        // Draw Cancel button
        if (E.menu_selection == 1) {
            oled.fillRect(cancelButtonX, buttonY, cancelWidth + buttonPadding * 2, buttonHeight, SSD1306_WHITE);
            oled.setTextColor(SSD1306_BLACK);
            oled.drawText(cancelButtonX + buttonPadding, buttonY + 8, cancelText);
            oled.setTextColor(SSD1306_WHITE);
        } else {
            oled.drawText(cancelButtonX + buttonPadding, buttonY + 8, cancelText);
        }
        
        oled.flushFramebuffer();
        return;
    }
    
    // Normal editing mode - build text buffer with 3 lines around cursor
    int currentRow = E.cy;
    int startRow = max(0, currentRow - 1);
    int endRow = min(E.numrows - 1, currentRow + 1);
    
    // Adjust for edges to always show 3 lines when possible
    if (startRow == 0 && E.numrows > 2) {
        endRow = min(2, E.numrows - 1);
    } else if (endRow >= E.numrows - 1 && E.numrows > 2) {
        endRow = E.numrows - 1;
        startRow = max(0, endRow - 2);
    }
    
    // Build text buffer with lines (use newline separators)
    char textBuffer[256];  // Stack buffer for text
    int bufPos = 0;
    int displayLineCount = 0;
    int cursorDisplayLine = -1;
    
    if (E.numrows == 0) {
        strcpy(textBuffer, "[Empty File]");
        cursorDisplayLine = 0;
        displayLineCount = 1;
    } else {
        for (int i = startRow; i <= endRow && i < E.numrows && bufPos < sizeof(textBuffer) - 2; i++) {
            if (i == currentRow) {
                cursorDisplayLine = displayLineCount;
            }
            
            // Copy line content
            if (E.row[i].chars && E.row[i].size > 0) {
                int copyLen = min(E.row[i].size, (int)(sizeof(textBuffer) - bufPos - 2));
                memcpy(textBuffer + bufPos, E.row[i].chars, copyLen);
                bufPos += copyLen;
            }
            
            // Add newline separator
            textBuffer[bufPos++] = '\n';
            displayLineCount++;
        }
        textBuffer[bufPos] = '\0';
    }
    
    // Configure display with generalized function
    oled::SmallTextDisplayConfig config = {};
    config.text = textBuffer;
    config.font = SMALL_FONT_ANDALE_MONO;
    config.clear_before = true;
    config.show_after = false;  // We'll flush after adding char selection indicator
    config.enable_cursor = true;
    config.cursor_line = cursorDisplayLine;
    config.cursor_col = E.cx;
    config.start_line = 0;
    config.max_lines = 3;
    config.horizontal_offset = E.oled_horizontal_offset;
    config.highlight_cursor_line = false;
    config.status_text = nullptr;
    
    // Display the text buffer
    oled.showSmallTextBuffer(config);
    
    // Show character selection mode if active
    if (E.char_selection_mode) {
        // Draw character selection indicator in top-right corner with larger text
        char selectionText[5];
        char selected = ekilo_get_character_from_index(E.selected_char_index);
        if (selected == '\n') {
            strcpy(selectionText, "\\n");
        } else if (selected == '\b') {
            strcpy(selectionText, "BS");
        } else if (selected == '\t') {
            strcpy(selectionText, "TAB");
        } else {
            snprintf(selectionText, sizeof(selectionText), "%c", selected);
        }
        
        // Use larger font for character indicator
        oled.setFontForSize(FONT_ANDALE_MONO, 2);
        int largeCharWidth = oled.getCharacterWidth();
        
        // Position in top-right corner
        int textWidth = strlen(selectionText) * largeCharWidth;
        int x = 128 - textWidth - 2;
        int y = 12;
        
        // Draw background box
        oled.fillRect(x - 1, y - 11, textWidth + 2, 14, SSD1306_WHITE);
        
        // Draw text in black on white background
        oled.setTextColor(SSD1306_BLACK);
        oled.drawText(x, y, selectionText);
        oled.setTextColor(SSD1306_WHITE);
    }
    
    // Flush to display
    oled.flushFramebuffer();
}

// Character selection array for clickwheel input
const char* character_set = " abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()[]{}|\\:;\"'<>,.?/+-=_~`\t\n\b";

// Get character from index in character set
char ekilo_get_character_from_index(int index) {
    int len = strlen(character_set);
    if (index < 0) index = len - 1;
    if (index >= len) index = 0;
    return character_set[index];
}

// Enter character selection mode
void ekilo_enter_char_selection() {
    E.char_selection_mode = true;
    E.selected_char_index = 1; // Start with 'A' instead of space
    E.char_selection_timer = millis();
    ekilo_set_status_message("CHAR SELECT: %c (Wheel=cycle, Click=confirm, Wait=exit)", 
                             ekilo_get_character_from_index(E.selected_char_index));
    
    // Schedule OLED update when entering character selection mode
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Exit character selection mode
void ekilo_exit_char_selection() {
    E.char_selection_mode = false;
    ekilo_set_status_message("Character selection cancelled");
    
    // Schedule OLED update when exiting character selection mode
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Cycle through characters
void ekilo_cycle_character(int direction) {
    if (!E.char_selection_mode) return;
    
    E.selected_char_index -= direction;
    int len = strlen(character_set);
    
    // Wrap around
    if (E.selected_char_index < 0) E.selected_char_index = len - 1;
    if (E.selected_char_index >= len) E.selected_char_index = 0;
    
    E.char_selection_timer = millis(); // Reset timeout
    
    char selected = ekilo_get_character_from_index(E.selected_char_index);
    if (selected == '\n') {
        ekilo_set_status_message("CHAR SELECT: \\n (Wheel=cycle, Click=confirm, Wait=exit)");
    } else if (selected == '\b') {
        ekilo_set_status_message("CHAR SELECT: BKSP (Wheel=cycle, Click=confirm, Wait=exit)");
    } else {
        ekilo_set_status_message("CHAR SELECT: %c (Wheel=cycle, Click=confirm, Wait=exit)", selected);
    }
    
    // Schedule OLED update in character selection mode for responsiveness
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Confirm character selection and insert it
void ekilo_confirm_character() {
    if (!E.char_selection_mode) return;
    if (E.read_only) {
        E.char_selection_mode = false;
        ekilo_set_status_message("READ-ONLY file - cannot edit");
        E.screen_dirty = true;
        return;
    }
    
    char selected = ekilo_get_character_from_index(E.selected_char_index);
    E.char_selection_mode = false;
    
    // If cursor is beyond the end of line, move to end first
    if (E.cy < E.numrows && E.cx > E.row[E.cy].size) {
        E.cx = E.row[E.cy].size;
    }
    
    if (selected == '\t') {
        ekilo_insert_char(' ');
        ekilo_insert_char(' ');
        ekilo_insert_char(' ');
        ekilo_insert_char(' ');
        ekilo_set_status_message("Inserted tab");
    } else if (selected == '\n') {
        ekilo_insert_newline();
        ekilo_set_status_message("Inserted newline");
    } else if (selected == '\b') {
        ekilo_del_char();
        ekilo_set_status_message("Backspace");
    } else {
        ekilo_insert_char(selected);
        ekilo_set_status_message("Inserted '%c'", selected);
    }
    
    // Schedule OLED update after character insertion
    ekilo_schedule_oled_update();
    
    // Mark screen as dirty for refresh
    E.screen_dirty = true;
}

// Click/hold classifier for the editor (reset on editor entry in ekilo_run)
static EncoderClickTracker g_ekilo_click;

// Process encoder input for cursor movement and character selection
void ekilo_process_encoder_input() {
    // Check for character selection timeout
    if (E.char_selection_mode && (millis() - E.char_selection_timer > E.CHAR_SELECTION_TIMEOUT)) {
        ekilo_exit_char_selection();
        return;
    }
    
    unsigned long currentTime = millis();
    
    // Read current encoder position
    long currentPosition = encoderPosition;
    
    // Calculate position delta
    long deltaPosition = (currentPosition - E.last_encoder_position)/4;
    deltaPosition = -deltaPosition;
    
    // Only process if there's a significant change and enough time has passed
    if (deltaPosition != 0 && (currentTime - E.last_encoder_update >= 20)) { // 20ms minimum between updates for responsiveness
        E.last_encoder_position = currentPosition;
        E.last_encoder_update = currentTime;
        
        if (E.char_selection_mode) {
            // In character selection mode: cycle through characters
            // Use smaller delta for precise character selection
            int steps = max(1, min(abs(deltaPosition), 3)); // 1-3 characters per rotation for faster cycling
            int direction = (deltaPosition > 0) ? -1 : 1;
            
            for (int i = 0; i < steps; i++) {
                ekilo_cycle_character(direction);
            }
        } else {
            // Normal mode: move cursor or navigate menu
            if (E.in_menu_mode) {
                // In menu mode: navigate between save/exit options or loop to file
                if (deltaPosition != 0) {
                    int direction = (deltaPosition > 0) ? 1 : -1;  // Match cursor movement direction
                    
                    if (direction > 0) {  // Right scroll in menu (clockwise)
                        // Scrolling right in menu: Save -> Cancel -> loop to beginning of file
                        E.menu_selection++;
                        if (E.menu_selection > 1) {
                            // Past exit option - loop to beginning of file
                            E.in_menu_mode = false;
                            E.cy = 0;
                            E.cx = 0;
                            ekilo_set_status_message("Looped to beginning of file");
                        } else {
                            // Update menu selection
                            if (E.menu_selection == 0) {
                                ekilo_set_status_message("Save selected - click to save file");
                            } else {
                                ekilo_set_status_message("Cancel selected - click to cancel without saving");
                            }
                        }
                    } else {  // Left scroll in menu (counter-clockwise)
                        // Scrolling left in menu: Cancel -> Save -> back to end of file
                        E.menu_selection--;
                        if (E.menu_selection < 0) {
                            // Before save option - exit menu to end of file
                            E.in_menu_mode = false;
                            if (E.numrows > 0) {
                                E.cy = E.numrows - 1;
                                E.cx = E.row[E.cy].size;
                            } else {
                                E.cy = 0;
                                E.cx = 0;
                            }
                            ekilo_set_status_message("Back to end of file");
                        } else {
                            // Update menu selection
                            if (E.menu_selection == 0) {
                                ekilo_set_status_message("Save selected - click to save file");
                            } else {
                                ekilo_set_status_message("Cancel selected - click to cancel without saving");
                            }
                        }
                    }
                    ekilo_schedule_oled_update();
                    E.screen_dirty = true;
                }
            } else {
                // Use direct delta for responsive cursor movement (horizontal only)
                int steps = max(1, min(abs(deltaPosition), 4)); // 1-4 characters per rotation
                int direction = (deltaPosition > 0) ? 1 : -1;  // Fixed: positive delta = right, negative = left
                
                for (int i = 0; i < steps; i++) {
                    if (direction > 0) {  // Right movement (clockwise scroll)
                        // Moving right - check if we should enter menu mode
                        if (E.cy >= E.numrows || (E.cy == E.numrows - 1 && E.cx >= E.row[E.cy].size)) {
                            // At or past end of file - enter menu mode
                            if (!E.in_menu_mode) {
                                E.in_menu_mode = true;
                                E.menu_selection = 0; // Start with save option
                                ekilo_set_status_message("Menu mode: Use wheel to select Save/Exit, click to confirm");
                                break; // Stop processing more steps
                            }
                        } else {
                            ekilo_move_cursor(ARROW_RIGHT);
                        }
                    } else {
                        // Moving left - check if we should loop to menu
                        if (E.cy == 0 && E.cx == 0) {
                            // At beginning of file - loop to menu
                            E.in_menu_mode = true;
                            E.menu_selection = 1; // Start with cancel (last option)
                            ekilo_set_status_message("Looped to menu from beginning");
                            break;
                        } else {
                            ekilo_move_cursor(ARROW_LEFT);
                        }
                    }
                }
                ekilo_schedule_oled_update();
                E.screen_dirty = true;
            }
        }
    }
    
    // Button: classified on the physical pin via EncoderClickTracker. The
    // editor is a "fast UI" - the primary action fires on ENC_PRESS for
    // instant feedback. A LONG_HOLD is the hardware Ctrl-Q: quit from
    // anywhere (saving first if there are unsaved changes, since a physical
    // gesture can't answer the unsaved-changes prompt).
    switch (g_ekilo_click.poll()) {
    case ENC_PRESS:
        if (E.char_selection_mode) {
            // In character selection mode: confirm character
            ekilo_confirm_character();
        } else if (E.in_menu_mode) {
            // In menu mode: handle menu selection
            if (E.menu_selection == 0) { // Save
                ekilo_save();
                E.in_menu_mode = false; // Exit menu after save
                ekilo_update_oled_context();
            } else if (E.menu_selection == 1) { // Cancel
                E.should_quit = 1;
            }
        } else {
            // Normal mode: enter character selection
            ekilo_enter_char_selection();
            // Initialize encoder position tracking when entering char selection
            E.last_encoder_position = encoderPosition;
        }
        break;

    case ENC_LONG_HOLD: {
        if (E.dirty && !E.read_only) {
            ekilo_save();
        }
        E.should_quit = 1;
        // Swallow the rest of the hold so the main screen / caller doesn't
        // interpret it as its own hold gesture (3s stuck-button bail-out).
        unsigned long releaseWaitStart = millis();
        while (isEncoderButtonPhysicallyPressed() &&
               millis() - releaseWaitStart < 3000) {
            delayMicroseconds(500);
        }
        encoderButtonState = IDLE;
        lastButtonEncoderState = IDLE;
        break;
    }

    default:
        break;
    }
}

// REMOVED: REPL mode functions - editor now always runs in normal mode

// ============================================================================
// UNIFIED EDITOR ENTRY POINT
// ============================================================================

// Static storage for result (accessed via ContextManager transfer data)
static EkiloResult g_ekilo_result;

/**
 * @brief Get the result of the last ekilo_run() call
 */
const EkiloResult* ekilo_get_result() {
    size_t len = 0;
    const void* data = ContextManager::getInstance().getTransferData(&len);
    if (data && len == sizeof(EkiloResult)) {
        return static_cast<const EkiloResult*>(data);
    }
    return nullptr;
}

/**
 * @brief Clear the stored ekilo result
 */
void ekilo_clear_result() {
    ContextManager::getInstance().clearTransferData();
}

/**
 * @brief Unified eKilo entry point - consolidates ekilo_main and ekilo_main_repl
 * 
 * This function:
 * 1. Initializes the editor
 * 2. Opens the file (or creates new)
 * 3. Runs the editor loop
 * 4. Stores result in ContextManager for caller
 * 5. Cleans up resources
 */
bool ekilo_run(const char* filename) {
    Serial.println("\n=== ekilo_run() ENTRY ===");
    Serial.printf("[ekilo_run] filename=%s\n\r", filename ? filename : "(null)");
    
    // Initialize result
    g_ekilo_result = EkiloResult();
    
    // Safety check: ensure we have minimum heap before starting
    size_t freeHeap = rp2040.getFreeHeap();
    Serial.printf("[ekilo_run] Free heap: %d bytes\n\r", (int)freeHeap);
    if (freeHeap < 4096) {
        Jerial.println("ERROR: Not enough memory to start editor");
        return false;
    }
    
    // Fragmentation detection: try to allocate a 2KB contiguous block
    // With lazy render allocation, we only need small allocations per visible row
    // So 2KB is sufficient - this avoids false positives from normal heap usage
    bool heap_fragmented = false;
    void* test_alloc = malloc(2048);
    if (test_alloc) {
        free(test_alloc);
       // Serial.println("[ekilo_run] Fragmentation test passed (2KB alloc OK)");
    } else {
        // Heap is fragmented - we have free memory but not contiguous
        heap_fragmented = true;
        Serial.println("[ekilo_run] Fragmentation test FAILED - using low-memory mode");
    }
    
    // Initialize editor
    ekilo_init();
    
    // Clear screen at startup for clean display
    Jerial.print("\x1b[2J\x1b[H");
    Jerial.flush();
    
    // Force low-memory mode if heap is fragmented
    if (heap_fragmented) {
        E.low_memory_mode = true;
    }
    
    // Extra safety: verify init succeeded
    if (E.screenrows <= 0 || E.screencols <= 0) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    
    // Open file or set up for new file
    if (filename != nullptr) {
        Serial.printf("[ekilo_run] Calling ekilo_open for: %s\n\r", filename);
        int open_result = ekilo_open(filename);
        Serial.printf("[ekilo_run] ekilo_open returned: %d, numrows=%d\n\r", open_result, E.numrows);
        if (open_result == -2) {
            // The file EXISTS but could not be loaded (too large / fs
            // failure). Entering new-file mode under the same name meant the
            // first Ctrl-S overwrote the real file with an empty buffer.
            Jerial.printf("\n\rekilo: %s\n\r", E.statusmsg);
            Jerial.flush();
            return false;
        }
        if (open_result != 0) {
            // File doesn't exist - set up for new file
            free(E.filename);
            E.filename = strdup(filename);
            if (!E.filename) {
                ekilo_set_status_message("ERROR: Memory allocation failed for filename");
                return false;
            }
            // The transfer buffer may still hold the PREVIOUS file: ekilo_save's
            // fast path would have written it into this new file.
            SharedBuffer::getInstance().clear();
            s_bufferMirrorsRows = false;
            
            // Ensure directory exists for new files using safe functions
            String file_path = String(filename);
            int last_slash = file_path.lastIndexOf('/');
            if (last_slash > 0) {
                String dir_path = file_path.substring(0, last_slash);
                if (!safeFileExists(dir_path.c_str(), 1000)) {
                    safeMkdir(dir_path.c_str(), 2000);
                }
            }
            
            // Set syntax highlighting for new files
            ekilo_select_syntax_highlight(filename);
            ekilo_set_status_message("Creating new file: %s", filename);
            E.screen_dirty = true;
            
            // Reset state for empty file
            for (int i = 0; i < E.numrows; i++) {
                ekilo_free_row(&E.row[i]);
            }
            free(E.row);
            E.row = nullptr;
            E.numrows = 0;
            E.row_capacity = 0;
            E.cy = 0;
            E.cx = 0;
        }
    }
    
    // Set status message
    ekilo_set_status_message("HELP: Ctrl-S = save | Ctrl-P = save & REPL | Ctrl-Q = quit | Clickwheel = move/type");
    
    // Initialize encoder position tracking
    E.last_encoder_position = encoderPosition;
    E.last_encoder_update = millis();
    
    // Initialize button state (tracker syncs to the pin so a click still
    // held from launching the editor isn't misread as a fresh press)
    g_ekilo_click.reset();
    E.last_button_state = digitalRead(BUTTON_ENC);
    E.button_debounce_time = millis();
    
    // Initial OLED update
    ekilo_schedule_oled_update();
    
    // Timing for service calls and memory checks
    unsigned long last_service_call = millis();
    unsigned long last_memory_check = millis();
    const unsigned long SERVICE_CALL_INTERVAL = 100;
    const unsigned long MEMORY_CHECK_INTERVAL = 5000;
    
    // ========== MAIN EDITOR LOOP ==========
    while (!E.should_quit) {
        yield();  // Feed watchdog
        
        unsigned long current_time = millis();
        
        // Service calls (SlotManager for file change detection)
        if (current_time - last_service_call > SERVICE_CALL_INTERVAL) {
            SlotManager::getInstance().service();
            last_service_call = current_time;
        }
        
        // Memory monitoring
        if (current_time - last_memory_check > MEMORY_CHECK_INTERVAL) {
            size_t currentFree = rp2040.getFreeHeap();
            if (currentFree < 2048) {
                ekilo_set_status_message("WARNING: Low memory (%dKB) - save your work!", currentFree / 1024);
                E.screen_dirty = true;
            }
            last_memory_check = current_time;
        }
        
        // Process keyboard input
        ekilo_process_keypress();
        
        // Process encoder input (FIXED: was missing in REPL mode)
        ekilo_process_encoder_input();
        
        // Process OLED updates
        ekilo_process_oled_update();
        
        // Refresh screen if needed
        if (E.screen_dirty) {
            ekilo_refresh_screen();
            E.screen_dirty = false;
        }
    }
    
    // ========== STORE RESULT ==========
    g_ekilo_result.launch_repl = E.should_launch_repl;
    g_ekilo_result.saved = (E.dirty == 0 && E.filename != nullptr);
    g_ekilo_result.cancelled = (E.dirty != 0);  // Quit with unsaved changes
    
    // Store saved file path for zero-copy transfer
    if (E.filename) {
        strncpy(g_ekilo_result.saved_path, E.filename, sizeof(g_ekilo_result.saved_path) - 1);
        g_ekilo_result.saved_path[sizeof(g_ekilo_result.saved_path) - 1] = '\0';
        
        // Also set transfer path in ContextManager
        ContextManager::getInstance().setTransferPath(E.filename);
    }
    
    // Store result in ContextManager transfer data
    ContextManager::getInstance().setTransferData(&g_ekilo_result, sizeof(g_ekilo_result));
    
    // ========== CLEANUP ==========
    oled.restoreNormalFont();
    
    for (int i = 0; i < E.numrows; i++) {
        ekilo_free_row(&E.row[i]);
    }
    free(E.row);
    free(E.filename);
    E.row = nullptr;
    E.numrows = 0;
    E.row_capacity = 0;
    E.filename = nullptr;
    
    // Clear global tracking
    free(g_currently_editing_file);
    g_currently_editing_file = nullptr;
    
    return true;
}

// ============================================================================
// LEGACY WRAPPERS (for backward compatibility)
// ============================================================================

// Legacy ekilo_main - wraps ekilo_run
int ekilo_main(const char* filename) {
    if (!ekilo_run(filename)) {
        return -1;
    }
    
    const EkiloResult* result = ekilo_get_result();
    if (result && result->launch_repl) {
        return 2;  // Signal to launch REPL
    }
    return 0;  // Normal exit
}

// ============================================================================
// REMOVED: ekilo_inline_edit (dead code - was never called)
// See git history if you need to recover this ~600 line function.
// ============================================================================


// ============================================================================
// Terminal Size Detection
// ============================================================================

// Probe terminal size using CSI 6n (Cursor Position Report)
// Returns true if successful, false if terminal doesn't respond
bool ekilo_probe_terminal_size(uint16_t& rows, uint16_t& cols) {
    // Mark that we're sending a CPR query (for stray response filtering)
    extern uint32_t last_cpr_query_time;
    last_cpr_query_time = millis();
    
    // Clear any pending input thoroughly
    delay(10);  // Brief delay for any in-flight data
    while (Jerial.available()) Jerial.read();
    
    // Save cursor position
    Jerial.print("\x1b[s");
    // Move to far bottom-right corner
    Jerial.print("\x1b[9999;9999H");
    // Query cursor position (CSI 6n)
    Jerial.print("\x1b[6n");
    Jerial.flush();
    
    // Wait a bit for response to arrive
    delay(50);
    
    // Read response with timeout: ESC [ rows ; cols R
    char buf[32];
    size_t n = 0;
    uint32_t start = millis();
    const uint32_t timeout = 300;  // 300ms timeout (response should be quick)
    
    while ((millis() - start) < timeout && n < sizeof(buf) - 1) {
        if (Jerial.available()) {
            char c = Jerial.read();
            // Skip until we see ESC
            if (n == 0 && c != '\x1b') continue;
            buf[n++] = c;
            // Response ends with 'R'
            if (c == 'R') break;
        }
        delayMicroseconds(500);
    }
    buf[n] = '\0';
    
    // Restore cursor position
    Jerial.print("\x1b[u");
    Jerial.flush();
    
    // Clear any remaining garbage from buffer (late responses, echoes)
    delay(20);
    while (Jerial.available()) Jerial.read();
    
    // Parse response: ESC [ rows ; cols R
    int r = 0, c = 0;
    if (sscanf(buf, "\x1b[%d;%dR", &r, &c) == 2) {
        rows = (uint16_t)r;
        cols = (uint16_t)c;
        return true;
    }
    
    return false;
}

// Re-probe terminal size and resize editor
void ekilo_resize_to_terminal() {
    uint16_t term_rows = 0, term_cols = 0;
    if (ekilo_probe_terminal_size(term_rows, term_cols)) {
        E.screenrows = constrain(term_rows - 4, 16, 60);
        E.screencols = constrain(term_cols, 40, 200);
        E.screen_dirty = true;
        ekilo_set_status_message("Resized to %dx%d", E.screencols, E.screenrows);
    } else {
        ekilo_set_status_message("Terminal size detection failed");
    }
}

// ============================================================================
// [REMOVED] Chunked File Loading - Now using SharedBuffer for all files
// Files > SHARED_BUFFER_SIZE (24KB) are rejected with clear error message
// ============================================================================ 