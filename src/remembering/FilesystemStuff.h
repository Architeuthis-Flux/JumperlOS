#ifndef FILESYSTEMSTUFF_H
#define FILESYSTEMSTUFF_H

#include <Arduino.h>
#include <FatFS.h>
#include "oled.h"

// Forward declarations
class FileManager;

// File types for icons and handling
enum FileType {
    FILE_TYPE_UNKNOWN = 0,
    FILE_TYPE_PYTHON = 1,
    FILE_TYPE_TEXT = 2,
    FILE_TYPE_CONFIG = 3,
    FILE_TYPE_JSON = 4,
    FILE_TYPE_DIRECTORY = 5,
    FILE_TYPE_NODEFILES = 6,
    FILE_TYPE_COLORS = 7,
    FILE_TYPE_BITMAP = 8,
    FILE_TYPE_IMAGE = 9,
    FILE_TYPE_LED = 10,
    FILE_TYPE_AUDIO = 11,
    FILE_TYPE_VIDEO = 12,
    FILE_TYPE_DOCUMENT = 13,
    FILE_TYPE_ARCHIVE = 14
};

// File entry structure
struct FileEntry {
    String name;
    String path;
    FileType type;
    size_t size;
    bool isDirectory;
    time_t lastModified;
};

// Display configuration constants
static const int DEFAULT_DISPLAY_LINES = 25; // Increased from ~15 to 25 lines

// Color scheme for filesystem display
struct FileColors {
    static const int DIRECTORY = 51;     // Cyan
    static const int PYTHON = 155;       // Green 
    static const int TEXT = 221;         // Yellow
    static const int CONFIG = 207;       // Magenta
    static const int JSON = 69;          // Blue
    static const int NODEFILES = 202;    // Orange
    static const int COLORS = 199;       // Pink
    static const int UNKNOWN = 248;      // Light grey
    static const int HEADER = 38;        // Bright green
    static const int STATUS = 226;       // Bright yellow
    static const int ERROR = 196;        // Red
    static const int BITMAP = 199;       // Pink
    static const int IMAGE = 57;         // purple
    static const int LED = 206;          // light blue
    static const int AUDIO = 123;        // light blue
    static const int VIDEO = 214;        // light green
    static const int DOCUMENT = 117;     // light yellow
    static const int ARCHIVE = 117;      // light yellow
};

class FileManager {
private:
    String currentPath;
    FileEntry* fileList;
    int fileCount;
    int maxFiles;
    int selectedIndex;
    int displayOffset;
    int maxDisplayLines;
    bool listingTruncated; // refreshListing() dropped entries beyond maxFiles
    
    // OLED update batching
    unsigned long lastInputTime;
    bool oledUpdatePending;
    static const unsigned long OLED_UPDATE_DELAY = 50000; // 50 milliseconds for responsive updates
    
    // Rotary encoder state tracking
    int lastEncoderDirectionState;
    int lastEncoderButtonState;
    
    // OLED horizontal scrolling
    int oledHorizontalOffset;
    int oledCursorPosition;
    static const int OLED_SCROLL_MARGIN = 3; // Start scrolling when within 3 chars of edge
    
    // REPL mode and positioning
    bool replMode = false;
    bool shouldExitForREPL = false;  // Flag to exit file manager when content is ready for REPL
    // Click-menu mode: show 7+7 breadboard OLED, run .py / load slot / open image on select
    bool fromClickMenu = false;
    
    // Deferred script execution: when a .py file is selected in click-menu mode,
    // store the path and exit the file manager so the script runs with a clean terminal
    String pendingScriptPath;
    bool shouldExitForScript = false;
    int originalCursorRow;
    int originalCursorCol;
    int startRow;
    int linesUsed;
    
    // File content storage for REPL mode
    String lastOpenedFileContent;
    
    // Output area for prompts and messages
    int outputAreaStartRow;
    int outputAreaHeight;
    int outputAreaCurrentRow;
    
    // Persistent filesystem messages that survive interface refreshes
    struct PersistentMessage {
        String message;
        int color;
        unsigned long timestamp;
    };
    static const int MAX_PERSISTENT_MESSAGES = 5;
    PersistentMessage persistentMessages[MAX_PERSISTENT_MESSAGES];
    int persistentMessageCount;
    int persistentMessageStartRow;
    int persistentMessageHeight;
    
    // Input blocking to prevent accidental double-presses
    unsigned long lastDisplayUpdate;
    static const unsigned long INPUT_BLOCK_TIME = 200; // 200ms block after display updates
    
    // Display configuration
    int textAreaLines; // Lines available for text (total - headers - footers)
    
    // Helper functions
    FileType getFileType(const String& filename);
    String getFileIcon(FileType type);
    String formatFileSize(size_t size);
    String formatDateTime(time_t timestamp);
    void updateOLEDStatus();
    void scheduleOLEDUpdate();
    void processOLEDUpdate();
    void calculateHorizontalScrolling(const String& fullText, int cursorPos);
    void showFileOperationMenu(const String& filename);
    bool confirmAction(const String& action, const String& target);
    void initializeFilesystem();
    int calculatePathDepth(const String& path);
    
public:
    FileManager();
    ~FileManager();
    
    // Navigation
    bool changeDirectory(const String& path);
    bool goUp();
    bool goHome();
    void refreshListing();
    
    // Display
    void showCurrentListing(bool showHeader = true);
    void showFileInfo(const FileEntry& file);
    void showHelp();
    
    // File operations
    bool createFile(const String& filename);
    bool createDirectory(const String& dirname);
    bool deleteFile(const String& filename);
    bool renameFile(const String& oldName, const String& newName);
    bool copyFile(const String& source, const String& dest);
    
    // File viewing/editing
    bool viewFile(const String& filename);
    bool editFile(const String& filename);
    bool editFileWithEkilo(const String& filename);
    
    // Navigation controls
    void moveSelection(int direction);
    void selectCurrentFile();
    
    // Main interface
    void run();
    
    // Interactive mode functions
    void initInteractiveMode();
    void drawInterface(bool fullScreen = true);
    void updateFileListDisplay();
    void updateStatusLine();
    void clearScreen();
    void moveCursor(int row, int col);
    void hideCursor();
    void showCursor();
    void clearCurrentLine();
    void showInteractiveHelp();
    void showInteractiveFileView(const String& filename);
    void showInteractiveFileInfo(const FileEntry& file);
    
    // File content tracking
    String getLastSavedFileContent();
    void setREPLMode(bool isREPLMode = false) { replMode = isREPLMode; }
    void setFromClickMenu(bool fromMenu = false) { fromClickMenu = fromMenu; }
    
    // Getters
    String getCurrentPath() const { return currentPath; }
    int getFileCount() const { return fileCount; }
    FileEntry* getCurrentFile();
    bool getShouldExitForREPL() const { return shouldExitForREPL; }
    bool getShouldExitForScript() const { return shouldExitForScript; }
    String getPendingScriptPath() const { return pendingScriptPath; }
    
    // Interactive input helpers
    String promptForFilename(const String& prompt);
    
    // Output area management
    void clearOutputArea();
    void outputToArea(const String& text, int color = 248);
    void showOutputAreaBorder();
    String promptInOutputArea(const String& prompt);
    
    // Persistent filesystem message management
    void addPersistentMessage(const String& message, int color = 248);
    void clearPersistentMessages();
    void displayPersistentMessages();
    void initializePersistentMessageArea();
    
    // Input management
    void blockInputBriefly();
    void clearBufferedInput(bool allowCtrlQ = true);
    bool isInputBlocked();
};

// Global functions
void filesystemApp(bool waitForEnter = true);
void filesystemAppPythonScripts(); // Start file manager in python_scripts directory
/** Pick a .py file from click menu; returns path or "" if user quit. Caller runs the file. */
String pickPythonScriptFromClickMenu();
String filesystemAppPythonScriptsREPL(); // REPL mode - returns content if file saved
void eKiloApp();
String launchEkilo(const char* filename, bool replMode); // Unified eKilo launcher
void launchEkiloStandalone(const char* filename = nullptr); // Legacy wrapper for standalone mode
String launchEkiloREPL(const char* filename = nullptr); // Legacy wrapper for REPL mode

String generateNextScriptName(); // Helper to generate next available script name

// Utility functions
String getFullPath(const String& basePath, const String& filename);
bool isValidFilename(const String& filename);

// File cleanup functions
void closeAllOpenFiles(void); // Comprehensive file cleanup across all systems

// Global utility functions for external use (e.g., USB filesystem)
FileType getFileTypeFromFilename(const String& filename);
String getFileIconFromType(FileType type);
String formatFileSizeForUSB(size_t size);
void printColoredPath(const String& path);

// Initialize MicroPython examples on boot
void initializeMicroPythonExamples(bool forceInitialization = false);

// Verify MicroPython examples exist
bool verifyMicroPythonExamples();

// Install the built-in projects into /projects/<dir>/ (the examples system's
// sibling; see CodeDocs/DESIGN_PROJECTS_SUBSYSTEM.md section 1). Unforced:
// create missing directories/files only. Forced (firmware update): 3-way
// FNV-1a check that updates untouched old defaults in place and parks the new
// default beside a user-edited file as *_original*. Never deletes anything,
// so hand-made projects on the board are left alone.
void initializeProjects(bool forceInitialization = false);

// Display configuration functions
int getConfiguredDisplayLines();
int getConfiguredEditorLines();

// Filesystem utility functions
bool deleteDirectoryContents(const String& path);

// =============================================================================
// SAFE FILE OPERATIONS
// =============================================================================
// Centralized file I/O functions with proper multicore synchronization.
// ALL file operations in the codebase should go through these functions
// to ensure thread safety and prevent Core2 crashes during flash writes.

/**
 * Safely open a file with proper mutex protection
 * @param path File path
 * @param mode File mode ("r", "w", "a", "r+", "w+", "a+")
 * @param timeout_ms Mutex timeout (0 = blocking, default 2000ms)
 * @return File handle or empty File if failed
 * @note Caller MUST call safeFileClose() when done
 * @note For write modes, Core2 is NOT paused until actual write/flush
 */
File safeFileOpen(const char* path, const char* mode, uint32_t timeout_ms = 2000);

/**
 * Safely close a file, flushing if it was opened for writing
 * @param file Reference to file handle (will be invalidated)
 * @param was_write_mode true if file was opened with write capabilities
 * @note Automatically handles Core2 pause for write mode flush
 */
void safeFileClose(File& file, bool was_write_mode = false);

/**
 * Safely read entire file contents into a buffer
 * @param path File path
 * @param buffer Output buffer (caller allocated)
 * @param buffer_size Size of buffer
 * @param bytes_read Output: actual bytes read
 * @param timeout_ms Mutex timeout (default 2000ms)
 * @param truncated Output (optional): set true when the file was larger than
 *        the buffer and the read was clamped to buffer_size-1. The return
 *        value stays true in that case, so callers doing deliberately bounded
 *        reads keep their existing behavior by passing nullptr.
 * @return true on success, false on error
 */
bool safeFileReadAll(const char* path, char* buffer, size_t buffer_size, 
                     size_t* bytes_read, uint32_t timeout_ms = 2000,
                     bool* truncated = nullptr);

/**
 * Safely write entire contents to a file (overwrites existing)
 * @param path File path
 * @param content Data to write
 * @param content_len Length of content (or 0 to use strlen)
 * @param timeout_ms Mutex timeout (default 2000ms)
 * @return true on success, false on error
 * @note Automatically pauses Core2, writes, flushes, and unpauses
 */
bool safeFileWriteAll(const char* path, const char* content, size_t content_len = 0,
                      uint32_t timeout_ms = 2000);

/**
 * Safely write to an open file handle
 * @param file File handle (must be open for writing)
 * @param data Data to write
 * @param len Length of data
 * @return Bytes written, or -1 on error
 * @note Caller should call safeFileFlush() or safeFileClose() after writes
 */
int safeFileWrite(File& file, const uint8_t* data, size_t len);

/**
 * @brief Held-pause variant of safeFileWrite for callers that already
 *        own a Core-1 pause window.
 *
 * Used by the FileCache flush path which wraps the entire write+commit
 * sequence in a single pauseCore2ForFlash + suspendUARTRxIRQ pair so
 * Core 1 freezes once (continuous) instead of N times (with flicker).
 *
 * Caller MUST currently hold:
 *   - pauseCore2ForFlash (i.e. a core-1 frame hold is up)
 *   - the fs_mutex (acquired via safeFileOpen)
 *   - AsyncPassthrough UART RX IRQ already suspended
 *
 * @param file Open file handle (caller still owns fs_mutex)
 * @param data Buffer to write (any heap is fine; must be CPU-readable)
 * @param len Byte count
 * @return Number of bytes written, or -1 on error
 */
int safeFileWriteHeld(File& file, const uint8_t* data, size_t len);

/**
 * @brief Held-pause variant of safeFileClose. Calls file.close() then
 *        releases fs_mutex but does NOT pause/unpause Core 1 (caller
 *        already holds that wrap). Pair with safeFileWriteHeld.
 */
void safeFileCloseHeld(File& file);

/**
 * @brief Like safeFileCloseHeld but does NOT release fs_mutex either.
 *        Used by atomic-write commits that chain close + unlink + rename
 *        under one fs_mutex acquisition so the filesystem-activity LED
 *        indicator only blinks once per logical save.
 */
void safeFileCloseHeldNoRelease(File& file);

/**
 * Safely flush a file with Core2 pause protection
 * @param file File handle (must be open for writing)
 * @return true on success
 */
bool safeFileFlush(File& file);

/**
 * Check if a file exists (thread-safe)
 * @param path File path
 * @param timeout_ms Mutex timeout (default 1000ms)
 * @return true if file exists
 */
bool safeFileExists(const char* path, uint32_t timeout_ms = 1000);

/**
 * Get file size (thread-safe)
 * @param path File path
 * @param timeout_ms Mutex timeout (default 1000ms)
 * @return File size in bytes, or -1 on error
 */
int32_t safeFileSize(const char* path, uint32_t timeout_ms = 1000);

/**
 * Safely create a directory (thread-safe with Core2 pause)
 * @param path Directory path
 * @param timeout_ms Mutex timeout (default 2000ms)
 * @return true on success or if directory already exists
 */
bool safeMkdir(const char* path, uint32_t timeout_ms = 2000);

/**
 * Safely delete a file (thread-safe with Core2 pause)
 * @param path File path
 * @param timeout_ms Mutex timeout (default 2000ms)
 * @return true on success
 */
bool safeFileDelete(const char* path, uint32_t timeout_ms = 2000);

/**
 * Safely rename a file (thread-safe with Core2 pause + UART IRQ suspension)
 * @param pathFrom Source file path
 * @param pathTo Destination file path
 * @param timeout_ms Mutex timeout (default 2000ms)
 * @return true on success
 */
bool safeFileRename(const char* pathFrom, const char* pathTo, uint32_t timeout_ms = 2000);

/**
 * Resolve a Python script filename to a full path by searching standard directories.
 * Searches: exact path, /python_scripts, /python_scripts/examples,
 *           /python_scripts/lib, /python_scripts/modules, and root /.
 * Appends .py if the filename doesn't already end with .py or .pyw.
 * @param filename The script name or path to resolve
 * @return Full path if found, or empty String if not found
 */
String resolvePythonScriptPath( const String& filename );

/**
 * Run a resolved Python script by path (public wrapper).
 * Reads the file, prints status, and calls executePythonFileContent.
 * @param fullPath Absolute path on the filesystem
 * @return true on success
 */
bool runPythonScriptByPath( const String& fullPath );

#endif // FILESYSTEMSTUFF_H 