#ifndef PYTHON_PROPER_H
#define PYTHON_PROPER_H

#include <Arduino.h>
#include "py/mpconfig.h"

extern Stream *global_mp_stream;

// Separate stream for interrupt checking - always points to main Serial
// This prevents mp_hal_check_interrupt from consuming data when global_mp_stream is USBSer2
extern Stream *mp_interrupt_check_stream;

// MicroPython heap — malloc'd so unused SRAM stays free
// 96KB with PSRAM (split heap adds PSRAM via gc_add), 128KB without
extern unsigned char *mp_heap;
extern size_t mp_heap_size;

// Reinitialize MicroPython with heap size matching current PSRAM config
void reinitMicroPythonForPsramChange(void);

// Python connection context modes
enum PythonConnectionContext {
    PYTHON_CONTEXT_GLOBAL = 0,  // Changes persist to global state
    PYTHON_CONTEXT_ISOLATED = 1 // Changes isolated to Python session
};

extern PythonConnectionContext connectionContext;

// Forward declaration from existing Python.cpp
extern int parseAndExecutePythonCommand(char* command, char* response);

// Forward declarations from JumperlessMicroPythonAPI.cpp
extern "C" void jl_init_micropython_local_copy(void);
extern "C" void jl_exit_micropython_restore_entry_state(void);
extern "C" void jl_restore_micropython_entry_state(void);
extern "C" int jl_has_unsaved_changes(void);
extern "C" void jl_toggle_connection_context(void);  // Toggle between global and isolated mode
extern "C" const char* jl_get_connection_context_name(void);  // Get human-readable context name

// Command history and filesystem support with ring buffer and write batching
class ScriptHistory {
private:
  // Ring buffer configuration
  static const int MAX_HISTORY = 20;          // Reduced from 50 for efficiency
  static const int MAX_PARENT_VERSIONS = 5;   // Keep 5 versions per parent script
  static const int MULTILINE_THRESHOLD = 15;  // Lines to trigger parent tracking
  
  // History entry with metadata
  struct HistoryEntry {
    String content;
    String parent_id;       // Hash for multiline scripts (empty for simple commands)
    uint32_t timestamp;     // millis() when added
    bool is_multiline;      // Flag for scripts with >15 lines
  };
  
  HistoryEntry history[MAX_HISTORY];
  int head = 0;              // Ring buffer head (next write position)
  int count = 0;             // Number of valid entries (max MAX_HISTORY)
  int current_history_index = -1;
  
  // Write batching state
  bool dirty = false;                           // RAM differs from disk
  uint32_t last_flush_time = 0;                 // For periodic flush
  static const uint32_t FLUSH_INTERVAL_MS = 30000; // 30 second idle flush
  volatile bool flush_in_progress = false;      // Prevent concurrent flushes
  
  // File paths and saved scripts tracking
  String scripts_dir = "/python_scripts";
  String last_saved_script = "";     // Track most recent saved script
  String saved_scripts[10];          // Track up to 10 saved scripts
  int saved_scripts_count = 0;
  int next_script_number = 1;        // For sequential script naming
  String numbered_scripts[20];       // Map numbers to script names for easy loading
  int numbered_scripts_count = 0;
  
  // Helper to resolve [FILE:...] references in history entries
  String resolveHistoryContent(const String& content);

public:
  void initFilesystem();
  void addToHistory(const String &script, const String &sourceFile = "");
  String getPreviousCommand();
  String getNextCommand();
  String getCurrentHistoryCommand();
  void resetHistoryNavigation();
  String getLastExecutedCommand();
  String getLastSavedScript();
  int getNextScriptNumber();
  int getNumberedScriptsCount();
  String getNumberedScript(int index);
  bool saveScript(const String &script, const String &filename = "");
  String loadScript(const String &filename);
  bool deleteScript(const String &filename);
  void listScripts();
  void clearHistory();
  
  // Thread-safe write batching methods
  void checkPeriodicFlush();          // Called from REPL idle loop
  void forceFlush();                  // Immediate flush (Ctrl+S, exit)

private:
  void findNextScriptNumber();
  void saveHistoryToFile();           // Now uses atomic write pattern
  void loadHistoryFromFile();
  void flushToDisk();                 // Thread-safe flush with mutex
  
  // Parent tracking helpers
  int countLines(const String &script);
  String computeParentHash(const String &script);
  void pruneParentVersions(const String &parent_id, int keep_count);
};

// Text editor helper functions for REPL
struct REPLEditor {
  String current_input = "";
  int cursor_pos = 0;
  bool in_multiline_mode = false;
  bool first_run = true;
  int escape_state = 0;       // 0=normal, 1=ESC, 2=ESC[
  String original_input = ""; // Store original input before history navigation
  bool in_history_mode = false;
  String source_filename = ""; // Filename if content was loaded from a file (for history)
  bool multiline_override = false;   // Manual multiline mode override
  bool multiline_forced_on = false;  // Force multiline mode on
  bool multiline_forced_off = false; // Force multiline mode off
  int last_displayed_lines = 0;      // Track how many lines we last displayed
  bool just_loaded_from_history = false; // Flag to track when we just loaded from history
  String last_displayed_content = "";    // Track the last content we displayed
  unsigned long escape_start_time = 0;   // Timestamp when ESC was pressed

  // Centralized cursor position management
  struct CursorPosition {
    int line = 0;           // Current line number (0-based)
    int column = 0;         // Current column within line (0-based)
    int total_lines = 0;    // Total number of lines in input
    bool is_valid = false;  // Whether position calculations are current
  } cursor_position;

  // Logical cursor management (updates internal cursor position)
  void updateCursorPosition();                     // Calculate line/column from cursor_pos
  void setCursorFromLineColumn(int line, int col); // Set cursor_pos from line/column
  void moveCursorUp();                             // Move logical cursor up one line
  void moveCursorDown();                           // Move logical cursor down one line
  void moveCursorLeft();                           // Move logical cursor left one character
  void moveCursorRight();                          // Move logical cursor right one character
  void moveCursorToLineStart();                    // Move logical cursor to start of current line
  void moveCursorToLineEnd();                      // Move logical cursor to end of current line
  void moveCursorToEnd();                          // Move logical cursor to end of all content
  
  // Content loading and display
  void loadScriptContent(const String &script, const String &message);
  
  // Terminal control (sends ANSI escape codes)
  void clearToEndOfLine(Stream *stream);
  void clearBelow(Stream *stream);
  void moveCursorToColumn(Stream *stream, int column);
  
  // Display management
  void redrawAndPosition(Stream *stream);          // Redraw content and position cursor
  void repositionCursorOnly(Stream *stream);       // Move cursor without redrawing content
  void resetCursorTracking();                      // Reset cursor position tracking
  void invalidateCursorTracking();                 // Mark cursor position as unknown
  void drawFromCurrentLine(Stream *stream);        // Simple drawing from current line (for history)
  void getCurrentLine(String &line, int &line_start, int &cursor_in_line);
  void backspaceOverNewline(Stream *stream);
  void loadFromHistory(Stream *stream, const String &historical_input);
  void exitHistoryMode(Stream *stream);
  void reset();
  void fullReset(); // Complete reset including multiline mode settings
  void drawPrompt(Stream *stream, int level = 0);
};

// Keyboard interrupt character constants
#define MP_INTERRUPT_CHAR_SERIAL   17  // Ctrl+Q for built-in REPL (Serial/Jerial)
#define MP_INTERRUPT_CHAR_USBSER2   3  // Ctrl+C for mpremote/ViperIDE (USBSer2)

// Core initialization and cleanup
void setGlobalStream(Stream *stream);
void setGlobalStreamWithInterrupt(Stream *stream);  // Sets stream AND correct interrupt char
bool initMicroPythonProper(Stream *stream = global_mp_stream, bool preserve_interrupt_char = false);
void deinitMicroPythonProper(void);



// Single command execution for main.cpp
bool initMicroPythonQuiet(bool preserve_interrupt_char = false);
bool executeSinglePythonCommand(const char* command, char* result_buffer = nullptr, size_t buffer_size = 0);
bool executePythonFileContent(const char* src);

// Hand the UI back to the firmware after Python ran (see Python_Proper.cpp).
// Called from BOTH script paths: executePythonFileContent() and the raw-REPL
// completion hook in MpRemoteService.
// fullHandback: true tears the display state down too (file runs). The raw
// REPL passes false for ordinary console lines - see the body.
void onPythonSessionEnd(bool fullHandback = true);

// Reset the display preferences only a Python script can set
// (oled_set_text_size / oled_copy_print). Defined in JumperlessMicroPythonAPI.cpp,
// inside its extern "C" API block.
extern "C" void jl_reset_python_display_prefs(void);

// Recursive, machine-readable filesystem listing via the global Python walk().
// Routes output to `out` (defaults to the current MP stream). Brings up
// MicroPython if needed. Emits f|path|size / d|path|size lines.
void runFilesystemWalk(Stream* out = nullptr);
bool executeSinglePythonCommandFormatted(const char* command, char* result_buffer, size_t buffer_size);
bool executeSinglePythonCommandFloat(const char* command, float* result);
float quickPythonCommand(const char* command);
String parseCommandWithPrefix(const char* command);
bool isJumperlessFunction(const char* function_name);

// Syntax highlighting helper
#include "SyntaxHighlighting.h"

// REPL control
void startMicroPythonREPL(void);
void stopMicroPythonREPL(void);
bool isMicroPythonREPLActive(void);
void processMicroPythonInput(Stream *stream = global_mp_stream);

// REPL helper functions
void loadScriptIntoREPL(const String &script, const String &message);

// Simple blocking REPL function - call from main.cpp
void enterMicroPythonREPL(Stream *stream = global_mp_stream);
void enterMicroPythonREPLWithFile(Stream *stream, const String& filepath);

// Helper functions
void addJumperlessPythonFunctions(void);
void addNodeConstantsToGlobalNamespace(void);
void testGlobalImports(void);
void addMicroPythonModules(bool time = true, bool machine = false, bool os = true, bool math = true, bool gc = true);
void testJumperlessNativeModule(void);
void setupFilesystemAndPaths(void);

// VFS functions (implemented in JumperlessMicroPythonAPI.cpp)
extern "C" void jl_vfs_mount_root(void);
void testStreamRedirection(Stream *newStream);
void testSingleCommandExecution(void);
void testFormattedOutput(void);
void showREPLreference(int verbose = 0);

// Status functions
bool isMicroPythonInitialized(void);
void printMicroPythonStatus(void);

// Memory management - run garbage collection to free memory before editor
void forceGarbageCollection(void);

// USB-CDC back-pressure guard: wait (pumping USB) until `stream` can accept
// `need` bytes. Returns false on timeout — the caller must DROP its output
// instead of handing it to a CDC write() that would spin forever when the
// host holds DTR but has stopped draining (browser/IDE holding the port).
// Non-CDC streams return true immediately.
bool jl_cdc_wait_writable(Stream *stream, size_t need = 1,
                          unsigned long timeoutMs = 100);

// Interrupt handling
extern bool mp_interrupt_requested; // Global interrupt flag for Ctrl+Q
extern bool mp_soft_reset_requested; // Soft reset request flag
extern volatile bool clickWheelPythonInterrupt; // Clickwheel button interrupts Python when true
extern "C" mp_uint_t mp_hal_set_interrupt_char(int c);
extern "C" int getCurrentInterruptChar(void);

// Terminal color control
void changeTerminalColor(Stream *stream = global_mp_stream, int color = 69, bool bold = true);

#endif // PYTHON_PROPER_H 