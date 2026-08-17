#include "FilesystemStuff.h"
#include "USBAudio.h"   // USB_AUDIO_ENABLE (example provisioning is V5-only)
#include "Adafruit_USBD_CDC.h"
#include "FatFS.h"
#include "FileParsing.h"
#include "Graphics.h"
#include "Jerial.h"    // TermControl is now part of Jerial
#include "JumperlOS.h" // For ContextManager
#include "Menus.h"
#include "Python_Proper.h"
#include "RotaryEncoder.h"
#include "SharedBuffer.h" // For zero-copy transfer debugging
#include "USBfs.h"      // usbMountedByHost - refuse firmware writes while a host has the disk mounted
#include "FileCache.h"  // fileCacheInvalidate + cache-aware wrappers
#include "config.h"
#include "externVars.h" // For fs_mutex filesystem synchronization
#include "AsyncPassthrough.h" // For suspendUARTRxIRQ/resumeUARTRxIRQ during flash ops
#include "micropythonExamples.h"
#include "oled.h"
#include <cstring>
#include <memory>
#include <time.h>
#include <stdint.h>

// External references
extern class oled oled;
extern volatile int showLEDsCore2;

// eKilo editor integration
#include "EkiloEditor.h"

// Bitmap editor integration
#include "BitmapEditor.h"

// States integration for slot file preview
#include "States.h"

// Global flag to signal return to main menu after editing
static bool returnToMainMenu = false;

// Global file manager instance for output area access
static FileManager* globalFileManager = nullptr;

// Message queue for filesystem messages that won't get overwritten
struct FilesystemMessage {
    String message;
    int color;
    unsigned long timestamp;
};

static const int MAX_FS_MESSAGES = 10;
static FilesystemMessage fsMessages[ MAX_FS_MESSAGES ];
static int fsMessageCount = 0;
static unsigned long lastMessageDisplayTime = 0;

//==============================================================================
// MicroPython example defaults - hashing and metadata
//==============================================================================


// Compute FNV-1a hash of a file on the FatFS filesystem (uses safe file access)
static bool fnv1a32_file( const char* path, uint32_t& outHash ) {
    File f = safeFileOpen( path, "r", 2000 );
    if ( !f ) {
        return false;
    }

    const size_t bufSize = 256;
    uint8_t buffer[ bufSize ];
    const uint32_t fnv_offset_basis = 0x811C9DC5u;
    const uint32_t fnv_prime = 0x01000193u;

    uint32_t h = fnv_offset_basis;
    bool ok = true;

    while ( true ) {
        int bytesRead = f.read( buffer, bufSize );
        if ( bytesRead < 0 ) {
            ok = false;
            break;
        }
        if ( bytesRead == 0 ) {
            break;
        }
        for ( int i = 0; i < bytesRead; ++i ) {
            h ^= buffer[ i ];
            h *= fnv_prime;
        }
    }

    safeFileClose( f, false );
    if ( !ok ) return false;
    outHash = h;
    return true;
}


// Build path for an \"original\" variant of a given example.
// variantIndex == 0 -> \"_original\", 1 -> \"_original1\", etc.
static String buildOriginalVariantPath( const char* canonicalPath, int variantIndex ) {
    String fullPath = String( canonicalPath );
    int lastSlash = fullPath.lastIndexOf( '/' );
    if ( lastSlash < 0 ) {
        lastSlash = -1;
    }

    String dir = fullPath.substring( 0, lastSlash + 1 );
    String filename = fullPath.substring( lastSlash + 1 );

    int dotIndex = filename.lastIndexOf( '.' );
    String namePart;
    String extPart;

    if ( dotIndex >= 0 ) {
        namePart = filename.substring( 0, dotIndex );
        extPart = filename.substring( dotIndex ); // includes '.'
    } else {
        namePart = filename;
        extPart = "";
    }

    String suffix;
    if ( variantIndex == 0 ) {
        suffix = "_original";
    } else {
        suffix = "_original" + String( variantIndex );
    }

    return dir + namePart + suffix + extPart;
}


void clearFilesystemMessages( ) {
    fsMessageCount = 0;
}

void displayFilesystemMessages( ) {
    // Only show messages for 10 seconds
    if ( millis( ) - lastMessageDisplayTime > 10000 ) {
        clearFilesystemMessages( );
        return;
    }

    if ( fsMessageCount > 0 ) {
        // Display messages at the bottom of screen (line 25+)
        for ( int i = 0; i < fsMessageCount && i < 3; i++ ) { // Show max 3 messages
            Serial.print( "\033[" );                          // Move cursor
            Serial.print( DEFAULT_DISPLAY_LINES + i );
            Serial.print( ";1H" );

            changeTerminalColor( fsMessages[ fsMessageCount - 1 - i ].color, false );
            Serial.print( fsMessages[ fsMessageCount - 1 - i ].message );

            // Clear rest of line
            Serial.print( "\033[K" );
            changeTerminalColor( -1, false );
        }
        Serial.flush( );
    }
}


void addFilesystemMessage( const String& message, int color = 248 ) {
    // Safety check - don't add messages during early startup
    if ( !Serial ) {
        return;
    }

    // If we have a global file manager instance, use its persistent message area
    if ( globalFileManager != nullptr ) {
        globalFileManager->addPersistentMessage( message, color );
        return;
    }

    // Add to traditional message queue for compatibility (fallback)
    if ( fsMessageCount < MAX_FS_MESSAGES ) {
        fsMessages[ fsMessageCount ].message = message;
        fsMessages[ fsMessageCount ].color = color;
        fsMessages[ fsMessageCount ].timestamp = millis( );
        fsMessageCount++;
    } else {
        // Shift messages up and add new one at end
        for ( int i = 0; i < MAX_FS_MESSAGES - 1; i++ ) {
            fsMessages[ i ] = fsMessages[ i + 1 ];
        }
        fsMessages[ MAX_FS_MESSAGES - 1 ].message = message;
        fsMessages[ MAX_FS_MESSAGES - 1 ].color = color;
        fsMessages[ MAX_FS_MESSAGES - 1 ].timestamp = millis( );
    }
    changeTerminalColor( color, false );
    Serial.println( message );
    Serial.flush();
    changeTerminalColor( -1, false );
    // displayFilesystemMessages();
    
    lastMessageDisplayTime = millis( );
}

FileManager::FileManager( ) {
    currentPath = "/";
    maxFiles = 100;

    // Check available memory before allocating file list
    size_t freeHeap = rp2040.getFreeHeap( );
    if ( freeHeap < sizeof( FileEntry ) * maxFiles + 10240 ) { // Need array + 10KB overhead
        // Reduce maxFiles if memory is limited. Clamp the subtraction: with
        // freeHeap < 10240 the unsigned math underflowed to ~4GB and kept
        // maxFiles at 50 on exactly the boards that couldn't afford it.
        size_t availForList = ( freeHeap > 10240 ) ? freeHeap - 10240 : 0;
        maxFiles = min( 50, (int)( availForList / sizeof( FileEntry ) ) );
        if ( maxFiles < 10 )
            maxFiles = 10; // Minimum usable size
    }

    fileList = new FileEntry[ maxFiles ];
    if ( !fileList ) {
        // Critical error - fallback to minimal allocation
        maxFiles = 10;
        fileList = new FileEntry[ maxFiles ];
    }
    fileCount = 0;
    listingTruncated = false;
    selectedIndex = 0;
    displayOffset = 0;
    maxDisplayLines = ::DEFAULT_DISPLAY_LINES; // Use configurable display lines
    textAreaLines = maxDisplayLines - 7;       // Reserve lines for headers, footers, and output area

    // Initialize OLED batching
    lastInputTime = 0;
    oledUpdatePending = false;

    // Initialize encoder state tracking
    lastEncoderDirectionState = NONE;
    lastEncoderButtonState = IDLE;

    // Initialize OLED scrolling
    oledHorizontalOffset = 0;
    oledCursorPosition = 0;

    // Initialize REPL mode
    replMode = false;
    shouldExitForREPL = false;
    originalCursorRow = 0;
    originalCursorCol = 0;
    startRow = 0;
    linesUsed = 0;
    lastOpenedFileContent = "";

    // Initialize filesystem
    initializeFilesystem( );

    // Initialize output area (positioned after help lines and persistent messages)
    int fileListStartRow = 6;
    int fileListEndRow = fileListStartRow + textAreaLines - 1;
    int helpStartRow = fileListEndRow + 2; // Leave 1 line gap after file list

    // Initialize persistent message area (above output area)
    persistentMessageHeight = 3;
    persistentMessageStartRow = helpStartRow + 4; // Start after help lines (2 lines) + border (1 line) + gap (1 line)
    persistentMessageCount = 0;

    // Position output area after persistent messages
    outputAreaStartRow = persistentMessageStartRow + persistentMessageHeight + 2; // Add separator and gap
    outputAreaHeight = 3;                                                         // Reduced to fit persistent messages
    outputAreaCurrentRow = 0;

    // Set global pointer for message routing
    globalFileManager = this;

    // Initialize input blocking
    lastDisplayUpdate = 0;
}

FileManager::~FileManager( ) {
    delete[] fileList;

    // Clear global pointer
    if ( globalFileManager == this ) {
        globalFileManager = nullptr;
    }
}

void FileManager::initializeFilesystem( ) {
    // Serial.println("[FS] Initializing filesystem...");

    // Try to initialize FatFS (it may already be initialized by main firmware)
    bool fs_ok = true;

    // Test if we can access the filesystem by checking basic operations
    if ( !FatFS.exists( "/" ) ) {
        // Serial.println("[FS] Root directory not accessible, trying to initialize FatFS...");

        // Try to begin/mount the filesystem
        if ( !FatFS.begin( ) ) {
            // Serial.println("[FS] Failed to mount FatFS filesystem");
            fs_ok = false;
        } else {
            // Serial.println("[FS] FatFS mounted successfully");
        }
    } else {
        // Serial.println("[FS] Root directory accessible");
    }

    if ( fs_ok ) {
        // Don't create fake directories - only work with what actually exists
        // Serial.println("[FS] Using existing filesystem structure");

        // Test root directory access one more time using correct Dir API
        Dir testRoot = FatFS.openDir( "/" );
        bool root_accessible = false;
        int test_count = 0;
        while ( testRoot.next( ) && test_count < 3 ) {
            test_count++;
            root_accessible = true;
        }

        if ( root_accessible ) {
            // Serial.println("[FS] Filesystem initialization successful - root directory accessible");

            // Create required Python directories if they don't exist
            const char* pythonDirs[] = {
                "/python_scripts",
                "/python_scripts/lib",
                "/python_scripts/modules",
                "/python_scripts/examples" };

            for ( const char* dir : pythonDirs ) {
                safeMkdir( dir );
            }
        } else {
            // Serial.println("[FS] Warning: Root directory not accessible via Dir API");
            // Serial.println("[FS] Will show filesystem unavailable message");
            currentPath = "[NO_FS]";
        }
    } else {
        // Serial.println("[FS] Filesystem not available - file operations will be limited");
        currentPath = "[NO_FS]";
    }
}

FileType FileManager::getFileType( const String& filename ) {
    String lower = filename;
    lower.toLowerCase( );

    if ( lower.endsWith( ".py" ) || lower.endsWith( ".pyw" ) || lower.endsWith( ".pyi" ) ) {
        return FILE_TYPE_PYTHON;

    } else if ( lower.endsWith( ".json" ) || lower.endsWith( ".yaml" ) || lower.endsWith( ".yml" ) ) {
        return FILE_TYPE_JSON; // YAML (e.g. slot*.yaml) is JSON-ish structured config
    } else if ( lower.endsWith( ".cfg" ) || lower.endsWith( ".conf" ) || lower.startsWith( "config" ) || filename == "config.txt" ) {
        return FILE_TYPE_CONFIG;
    } else if ( lower.startsWith( "nodefileslot" ) && lower.endsWith( ".txt" ) ) {
        return FILE_TYPE_NODEFILES;
    } else if ( lower.startsWith( "netcolorsslot" ) && lower.endsWith( ".txt" ) ) {
        return FILE_TYPE_COLORS;
    } else if ( lower.endsWith( ".txt" ) || lower.endsWith( ".md" ) || lower.endsWith( ".readme" ) ) {
        return FILE_TYPE_TEXT;
    } else if ( lower.endsWith( ".bmp" ) || lower.endsWith( ".bin" ) ) {
        return FILE_TYPE_BITMAP;
    } else if ( lower.endsWith( ".png" ) || lower.endsWith( ".jpg" ) || lower.endsWith( ".jpeg" ) || lower.endsWith( ".gif" ) || lower.endsWith( ".tiff" ) || lower.endsWith( ".ico" ) || lower.endsWith( ".webp" ) ) {
        return FILE_TYPE_IMAGE;
    } else if ( lower.endsWith( ".bb" ) || lower.endsWith( ".led" ) ) {
        return FILE_TYPE_LED;
    } else if ( lower.endsWith( ".mp3" ) || lower.endsWith( ".wav" ) || lower.endsWith( ".ogg" ) || lower.endsWith( ".flac" ) || lower.endsWith( ".aac" ) || lower.endsWith( ".m4a" ) || lower.endsWith( ".m4b" ) || lower.endsWith( ".m4p" ) || lower.endsWith( ".m4v" ) || lower.endsWith( ".m4b" ) || lower.endsWith( ".m4p" ) || lower.endsWith( ".m4v" ) || lower.endsWith( ".m4b" ) || lower.endsWith( ".m4p" ) || lower.endsWith( ".m4v" ) ) {
        return FILE_TYPE_AUDIO;
    } else if ( lower.endsWith( ".mp4" ) || lower.endsWith( ".mov" ) || lower.endsWith( ".avi" ) || lower.endsWith( ".mkv" ) || lower.endsWith( ".webm" ) || lower.endsWith( ".wmv" ) || lower.endsWith( ".flv" ) || lower.endsWith( ".mpeg" ) || lower.endsWith( ".mpg" ) || lower.endsWith( ".m4v" ) || lower.endsWith( ".m4b" ) || lower.endsWith( ".m4p" ) || lower.endsWith( ".m4v" ) || lower.endsWith( ".m4b" ) || lower.endsWith( ".m4p" ) || lower.endsWith( ".m4v" ) ) {
        return FILE_TYPE_VIDEO;
    } else if ( lower.endsWith( ".pdf" ) || lower.endsWith( ".doc" ) || lower.endsWith( ".docx" ) || lower.endsWith( ".xls" ) || lower.endsWith( ".xlsx" ) || lower.endsWith( ".ppt" ) || lower.endsWith( ".pptx" ) ) {
        return FILE_TYPE_DOCUMENT;
    } else if ( lower.endsWith( ".zip" ) || lower.endsWith( ".rar" ) || lower.endsWith( ".7z" ) || lower.endsWith( ".tar" ) || lower.endsWith( ".gz" ) || lower.endsWith( ".bz2" ) || lower.endsWith( ".xz" ) || lower.endsWith( ".iso" ) || lower.endsWith( ".dmg" ) || lower.endsWith( ".pkg" ) || lower.endsWith( ".deb" ) || lower.endsWith( ".rpm" ) || lower.endsWith( ".msi" ) || lower.endsWith( ".exe" ) || lower.endsWith( ".app" ) || lower.endsWith( ".dmg" ) || lower.endsWith( ".pkg" ) || lower.endsWith( ".deb" ) || lower.endsWith( ".rpm" ) || lower.endsWith( ".msi" ) || lower.endsWith( ".exe" ) || lower.endsWith( ".app" ) ) {
        return FILE_TYPE_ARCHIVE;
    }
    return FILE_TYPE_UNKNOWN;
}

String FileManager::getFileIcon( FileType type ) {
    switch ( type ) {
    case FILE_TYPE_DIRECTORY:
        return "⌘";
    case FILE_TYPE_PYTHON:
        return "𓆚";
    case FILE_TYPE_TEXT:
        return "⍺";
    case FILE_TYPE_CONFIG:
        return "⚙";
    case FILE_TYPE_JSON:
        return "⟐";
    case FILE_TYPE_NODEFILES:
        return "☊";
    case FILE_TYPE_COLORS:
        return "⎃";
    case FILE_TYPE_BITMAP:
        return "🁝";
    case FILE_TYPE_IMAGE:
        return "🀥";
    case FILE_TYPE_LED:
        return "☄︎";
    case FILE_TYPE_AUDIO:
        return "♒︎";
    case FILE_TYPE_VIDEO:
        return "🀕";
    case FILE_TYPE_DOCUMENT:
        return "🀆";
    case FILE_TYPE_ARCHIVE:
        return "🁣";
    default:
        return "⍺";
    }
}

String FileManager::formatFileSize( size_t size ) {
    if ( size < 1024 ) {
        return String( size ) + " B";
    } else if ( size < 1024 * 1024 ) {
        return String( size / 1024 ) + " KB";
    } else {
        return String( size / ( 1024 * 1024 ) ) + " MB";
    }
}

String FileManager::formatDateTime( time_t timestamp ) {
    if ( timestamp == 0 ) {
        return "Unknown";
    }

    // Convert to local time structure
    struct tm* timeinfo = localtime( &timestamp );
    if ( !timeinfo ) {
        return String( (long)timestamp );
    }

    // Format as MM/DD/YY HH:MM
    char buffer[ 32 ];
    strftime( buffer, sizeof( buffer ), "%m/%d/%y %H:%M", timeinfo );
    return String( buffer );
}

void FileManager::updateOLEDStatus( ) {
    // Get current selection info
    const char* selectedFileName = ( fileCount > 0 && selectedIndex < fileCount )
                                       ? fileList[ selectedIndex ].name.c_str( )
                                       : nullptr;

    // Breadboard mode (from click menu): show on breadboard LEDs via b.print()
    // Top row = directory name (7 chars), Bottom row = selected file name (7 chars)
    // NOTE: this must run BEFORE the oledConnected early-return - the
    // breadboard text is the primary display when no OLED is attached.
    if ( fromClickMenu ) {
        String topLine;
        if ( currentPath == "/" ) {
            topLine = "root";
        } else {
            int lastSlash = currentPath.lastIndexOf( '/' );
            topLine = ( lastSlash >= 0 ) ? currentPath.substring( lastSlash + 1 ) : currentPath;
        }
        if ( topLine.length( ) > 7 )
            topLine = topLine.substring( topLine.length( ) - 7 );
        String bottomLine = selectedFileName ? String( selectedFileName ) : "";
        // Strip common extensions so more useful chars fit in 7-char display
        if ( bottomLine.endsWith( ".py" ) || bottomLine.endsWith( ".PY" ) ) {
            bottomLine = bottomLine.substring( 0, bottomLine.length( ) - 3 );
        }
        if ( bottomLine.length( ) > 7 )
            bottomLine = bottomLine.substring( 0, 7 );

        b.clear( );
        b.print( topLine.c_str( ), 0x001008, 0xFFFFFF, 0, 0, 1 );   // top row
        b.print( bottomLine.c_str( ), 0x100810, 0xFFFFFF, 0, 1, 1 ); // bottom row
        showLEDsCore2 = 2;
        // Fall through to still update the OLED normally
    }

    if ( !oled.oledConnected )
        return;

    if ( !selectedFileName ) {
        // No file selected, just show path
        oled.showFileStatus( currentPath.c_str( ), fileCount, nullptr );
        return;
    }

    // Build full display text and calculate cursor position
    String pathPart = currentPath;
    String fullText;

    // Don't add extra "/" if currentPath is already root "/"
    // Add newline after path separator for better display formatting (except root)
    if ( currentPath == "/" ) {
        fullText = pathPart + String( selectedFileName );
    } else {
        fullText = pathPart + "/\n" + String( selectedFileName );
    }

    // Cursor position is at the end of the selected filename
    oledCursorPosition = fullText.length( );

    // Show the full text with cursor indicator (no horizontal scrolling needed)
    oled.showFileStatusScrolled( fullText.c_str( ), fileCount, oledCursorPosition );
}

void FileManager::scheduleOLEDUpdate( ) {
    unsigned long currentTime = millis( );

    // If no recent input, update immediately for responsiveness
    if ( !oledUpdatePending || ( currentTime - lastInputTime ) > 200 ) {
        updateOLEDStatus( );
        lastInputTime = currentTime;
        oledUpdatePending = false;
    } else {
        // Batch subsequent updates
        lastInputTime = currentTime;
        oledUpdatePending = true;
    }
}

void FileManager::processOLEDUpdate( ) {
    // Only update OLED if enough time has passed since last input
    if ( oledUpdatePending && ( millis( ) - lastInputTime ) >= 50 ) { // 50ms delay
        updateOLEDStatus( );
        oledUpdatePending = false;
    }
}

void FileManager::calculateHorizontalScrolling( const String& fullText, int cursorPos ) {
    if ( !oled.isConnected( ) )
        return;

    // Better estimate: small font can fit ~25-26 characters for 128px width
    const int maxVisibleChars = 25;

    // Calculate visible cursor position relative to current offset
    int visibleCursorPos = cursorPos - oledHorizontalOffset;

    // Scroll right if cursor is too close to right edge
    if ( visibleCursorPos >= maxVisibleChars - OLED_SCROLL_MARGIN ) {
        oledHorizontalOffset = cursorPos - maxVisibleChars + OLED_SCROLL_MARGIN + 1;
    }
    // Scroll left if cursor is too close to left edge
    else if ( visibleCursorPos < OLED_SCROLL_MARGIN ) {
        oledHorizontalOffset = cursorPos - OLED_SCROLL_MARGIN;
    }

    // Keep offset within bounds
    if ( oledHorizontalOffset < 0 ) {
        oledHorizontalOffset = 0;
    }
    if ( oledHorizontalOffset > fullText.length( ) - maxVisibleChars ) {
        oledHorizontalOffset = max( 0, (int)fullText.length( ) - maxVisibleChars );
    }
}

// Calculate directory depth for visual indentation
int FileManager::calculatePathDepth( const String& path ) {
    if ( path == "/" )
        return 0;

    int depth = 0;
    for ( int i = 0; i < path.length( ); i++ ) {
        if ( path[ i ] == '/' )
            depth++;
    }
    return depth - 1; // Subtract 1 because root "/" counts as depth 0
}

bool FileManager::changeDirectory( const String& path ) {
    String newPath = path;

    // Handle relative paths
    if ( !newPath.startsWith( "/" ) ) {
        if ( currentPath.endsWith( "/" ) ) {
            newPath = currentPath + newPath;
        } else {
            newPath = currentPath + "/" + newPath;
        }
    }

    // Clean up path (remove double slashes, etc.)
    newPath.replace( "//", "/" );

    // Check if directory exists - handle root directory specially.
    // Park Core 1 around the FatFS ops: the SPIFTL journal can append a flash
    // page on any f_close, and Core 1 executing XIP code during a flash
    // program hard-faults (garbage instruction fetch). Same contract as
    // safeFileOpen/safeFileClose.
    bool was_paused = pauseCore2ForFlash( 100 );
    bool dirExists = false;
    if ( newPath == "/" ) {
        // For root directory, use openDir instead of exists
        Dir testRoot = FatFS.openDir( "/" );
        int test_count = 0;
        while ( testRoot.next( ) && test_count < 3 ) {
            test_count++;
            dirExists = true;
            break;
        }
    } else {
        dirExists = FatFS.exists( newPath.c_str( ) );
    }
    unpauseCore2ForFlash( was_paused );

    if ( dirExists ) {
        currentPath = newPath;
        selectedIndex = 0;
        displayOffset = 0;
        refreshListing( );
        scheduleOLEDUpdate( );
        return true;
    }

    changeTerminalColor( FileColors::ERROR, false );
    Serial.println( "Directory not found: " + newPath );
    changeTerminalColor( -1, false ); // Reset colors
    return false;
}

bool FileManager::goUp( ) {
    if ( currentPath == "/" )
        return false;

    int lastSlash = currentPath.lastIndexOf( '/' );
    if ( lastSlash == 0 ) {
        return changeDirectory( "/" );
    } else {
        return changeDirectory( currentPath.substring( 0, lastSlash ) );
    }
}

bool FileManager::goHome( ) {
    return changeDirectory( "/" );
}

void FileManager::refreshListing( ) {
    static int recursion_depth = 0;

    // Prevent infinite recursion
    if ( recursion_depth > 3 ) {
        Serial.println( "[FS] Too many recursion attempts, marking filesystem as unavailable" );
        currentPath = "[NO_FS]";
        recursion_depth = 0;
        // Fall through to handle [NO_FS] case
    }

    fileCount = 0;
    listingTruncated = false;

    // Handle special case where filesystem is not available
    if ( currentPath == "[NO_FS]" ) {
        recursion_depth = 0; // Reset counter
        // Create virtual entries to show filesystem unavailable message
        if ( maxFiles > 0 ) {
            fileList[ 0 ].name = "** FILESYSTEM NOT AVAILABLE **";
            fileList[ 0 ].path = "[ERROR]";
            fileList[ 0 ].isDirectory = false;
            fileList[ 0 ].size = 0;
            fileList[ 0 ].type = FILE_TYPE_UNKNOWN;
            fileCount = 1;

            if ( maxFiles > 1 ) {
                fileList[ 1 ].name = "Directory access failed after mount";
                fileList[ 1 ].path = "[INFO]";
                fileList[ 1 ].isDirectory = false;
                fileList[ 1 ].size = 0;
                fileList[ 1 ].type = FILE_TYPE_UNKNOWN;
                fileCount = 2;

                if ( maxFiles > 2 ) {
                    fileList[ 2 ].name = "Press 'q' to quit file manager";
                    fileList[ 2 ].path = "[HELP]";
                    fileList[ 2 ].isDirectory = false;
                    fileList[ 2 ].size = 0;
                    fileList[ 2 ].type = FILE_TYPE_UNKNOWN;
                    fileCount = 3;
                }
            }
        }
        return;
    }

    // Park Core 1 around the FatFS directory walk: the SPIFTL journal can
    // append a flash page on any f_close, and Core 1 executing XIP code during
    // a flash program hard-faults on a garbage instruction fetch. This is the
    // same contract safeFileOpen/safeFileClose follow; the nested recursive
    // calls below are safe via the was_paused save/restore idiom.
    bool was_paused = pauseCore2ForFlash( 100 );

    // Use FatFS directory API instead of file API
    Dir dir = FatFS.openDir( currentPath );

    // Check if directory exists by testing if we can read from it
    bool dir_accessible = false;
    int test_entries = 0;

    // Test directory accessibility by trying to read a few entries
    // BUT: Empty directories are still valid and accessible!
    while ( dir.next( ) && test_entries < 5 ) {
        test_entries++;
        dir_accessible = true;
    }

    // If we couldn't read any entries, the directory might be empty OR inaccessible
    // Try to distinguish between empty and actually broken directories
    if ( !dir_accessible ) {
        // For empty directories, we should still consider them accessible
        // Check if the directory path exists as a directory
        if ( FatFS.exists( currentPath.c_str( ) ) ) {
            // Directory exists, so it's accessible even if empty
            dir_accessible = true;
        } else {
            // Directory actually failed to open - this is a real error
            changeTerminalColor( FileColors::ERROR, false );
            Serial.println( "Failed to open directory: " + currentPath );
            changeTerminalColor( -1, false ); // Reset colors

            recursion_depth++;

            // Try to recover by going to root or alternative directory (only if not too deep)
            if ( recursion_depth <= 2 && currentPath != "/" ) {
                Serial.println( "[FS] Attempting to return to root directory..." );
                currentPath = "/";
                // Try root directory - it should exist
                if ( FatFS.exists( "/" ) ) {
                    refreshListing( ); // Recursive call to try root
                    recursion_depth--; // Decrement on successful path
                    unpauseCore2ForFlash( was_paused );
                    return;
                }

                // No alternative directories to try - filesystem issues
            }

            // If all fails, mark filesystem as unavailable
            Serial.println( "[FS] All recovery attempts failed, filesystem unavailable" );
            currentPath = "[NO_FS]";
            recursion_depth = 0; // Reset for next attempt
            unpauseCore2ForFlash( was_paused );
            refreshListing( );   // Show error message
            return;
        }
    }

    // Successfully opened directory, reset recursion counter
    recursion_depth = 0;

    // Add ".." entry if not at root
    if ( currentPath != "/" ) {
        FileEntry& parentEntry = fileList[ fileCount ];
        parentEntry.name = "..";
        parentEntry.path = "[UP]"; // Special marker for parent directory
        parentEntry.isDirectory = true;
        parentEntry.size = 0;
        parentEntry.lastModified = 0;
        parentEntry.type = FILE_TYPE_DIRECTORY;
        fileCount++;
    }

    // Re-open directory to start from beginning for actual listing
    dir = FatFS.openDir( currentPath );

    while ( dir.next( ) ) { // (Core 1 still parked here)
        if ( fileCount >= maxFiles ) {
            // Remember entries were dropped so the UI can say so.
            // ponytail: a skipped hidden file can also trip this - fine for an indicator
            listingTruncated = true;
            break;
        }
        // Service USB periodically during long directory reads to prevent disconnect
        if ( ( fileCount & 0x03 ) == 0 ) TinyUSB_Device_Task( ); // mutex-guarded pump

        String fileName = dir.fileName( );

        // Skip hidden files (files starting with '.') except for ".." navigation
        if ( fileName.startsWith( "." ) && fileName != ".." ) {
            continue;
        }

        FileEntry& entry = fileList[ fileCount ];
        entry.name = fileName;
        entry.path = getFullPath( currentPath, entry.name );
        entry.isDirectory = dir.isDirectory( );
        entry.size = dir.isDirectory( ) ? 0 : dir.fileSize( );
        entry.lastModified = dir.fileTime( ); // FAT modified time ("Modified:" label; the wrapper's fileCreationTime() is just an alias of it)

        if ( entry.isDirectory ) {
            entry.type = FILE_TYPE_DIRECTORY;
        } else {
            entry.type = getFileType( entry.name );
        }

        fileCount++;
    }

    // Directory walk done — let Core 1 run again. The sort below is pure RAM.
    unpauseCore2ForFlash( was_paused );

    // Sort directories first, then files alphabetically
    // But keep ".." at the very top if it exists
    int startSort = 0;
    if ( fileCount > 0 && fileList[ 0 ].name == ".." ) {
        startSort = 1; // Skip the ".." entry in sorting
    }

    for ( int i = startSort; i < fileCount - 1; i++ ) {
        // Service USB during sort to prevent disconnect on large directories
        if ( ( i & 0x03 ) == 0 ) TinyUSB_Device_Task( ); // mutex-guarded pump

        for ( int j = i + 1; j < fileCount; j++ ) {
            bool shouldSwap = false;

            if ( fileList[ i ].isDirectory && !fileList[ j ].isDirectory ) {
                shouldSwap = false; // Keep directory first
            } else if ( !fileList[ i ].isDirectory && fileList[ j ].isDirectory ) {
                shouldSwap = true; // Move directory up
            } else {
                // Same type, sort alphabetically
                shouldSwap = fileList[ i ].name > fileList[ j ].name;
            }

            if ( shouldSwap ) {
                FileEntry temp = fileList[ i ];
                fileList[ i ] = fileList[ j ];
                fileList[ j ] = temp;
            }
        }
    }
}

void FileManager::showCurrentListing( bool showHeader ) {
    if ( showHeader ) {
        changeTerminalColor( FileColors::HEADER, true );
        Serial.println( "\n╭───────────────────────────────────────────────────────────────────────────╮" );
        Serial.println( "│                            JUMPERLESS FILE MANAGER                        │" );
        Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );

        changeTerminalColor( FileColors::STATUS, false );
        Serial.print( "⌘ Current Path: " );
        printColoredPath( currentPath );
        Serial.println( );

        changeTerminalColor( FileColors::STATUS, false );
        Serial.println( "Files: " + String( fileCount ) + "  |  Use ↑↓ arrows or rotary encoder to navigate" );
        Serial.println( "Enter/Click: Open/Edit  |  Space: File operations  |  h: Help  |  q: Quit" );
        Serial.println( "╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌" );
    }

    // Calculate display range
    int startIdx = displayOffset;
    int endIdx = min( displayOffset + maxDisplayLines, fileCount );

    for ( int i = startIdx; i < endIdx; i++ ) {
        bool isSelected = ( i == selectedIndex );
        FileEntry& entry = fileList[ i ];

        // Position cursor for this file entry
        moveCursor( 6 + ( i - startIdx ), 3 );
        clearCurrentLine( );

        // Calculate indentation based on directory depth
        int currentDepth = calculatePathDepth( currentPath );
        String indentPrefix = "";
        for ( int d = 0; d < currentDepth; d++ ) {
            indentPrefix += "  "; // 2 spaces per level
        }

        // Selection indicator
        if ( isSelected ) {
            changeTerminalColor( 226, false ); // Bright yellow background
            Serial.print( "► " );
        } else {
            Serial.print( "  " );
        }

        // Add visual indentation
        Serial.print( indentPrefix );

        // File type color and icon
        int color = FileColors::UNKNOWN;
        switch ( entry.type ) {
        case FILE_TYPE_DIRECTORY:
            color = FileColors::DIRECTORY;
            break;
        case FILE_TYPE_PYTHON:
            color = FileColors::PYTHON;
            break;
        case FILE_TYPE_TEXT:
            color = FileColors::TEXT;
            break;
        case FILE_TYPE_CONFIG:
            color = FileColors::CONFIG;
            break;
        case FILE_TYPE_JSON:
            color = FileColors::JSON;
            break;
        case FILE_TYPE_NODEFILES:
            color = FileColors::NODEFILES;
            break;
        case FILE_TYPE_COLORS:
            color = FileColors::COLORS;
            break;
        case FILE_TYPE_BITMAP:
            color = FileColors::BITMAP;
            break;
        case FILE_TYPE_IMAGE:
            color = FileColors::IMAGE;
            break;
        case FILE_TYPE_LED:
            color = FileColors::LED;
            break;
        case FILE_TYPE_AUDIO:
            color = FileColors::AUDIO;
            break;
        case FILE_TYPE_VIDEO:
            color = FileColors::VIDEO;
            break;
        case FILE_TYPE_DOCUMENT:
            color = FileColors::DOCUMENT;
            break;
        case FILE_TYPE_ARCHIVE:
            color = FileColors::ARCHIVE;
            break;
        case FILE_TYPE_UNKNOWN:
            color = FileColors::TEXT;
            break;
        }

        changeTerminalColor( color, false );

        // Special handling for ".." entry
        if ( entry.name == ".." && entry.path == "[UP]" ) {
            Serial.print( "⌘ " );
            Serial.print( ".." );

            // Add padding to align with size column (adjusted for indentation)
            int usedSpace = 2 + indentPrefix.length( ) + 2 + 2; // selector + indent + icon + ".."
            int padding = 50 - usedSpace;
            for ( int p = 0; p < padding && p >= 0; p++ )
                Serial.print( " " );
        } else {
            Serial.print( getFileIcon( entry.type ) + " " );

            // Filename
            String displayName = entry.name;
            int maxNameLength = 45 - ( currentDepth * 2 ); // Adjust for indentation
            if ( displayName.length( ) > maxNameLength ) {
                displayName = displayName.substring( 0, maxNameLength - 3 ) + "...";
            }
            Serial.print( displayName );

            // Padding for size column (adjusted for indentation)
            int usedSpace = 2 + indentPrefix.length( ) + 2 + displayName.length( ); // selector + indent + icon + name
            int padding = 50 - usedSpace;
            for ( int p = 0; p < padding && p >= 0; p++ )
                Serial.print( " " );
        }

        changeTerminalColor( 248, false ); // Light grey for size
        if ( entry.name == ".." && entry.path == "[UP]" ) {
            Serial.print( "     <UP>" );
        } else if ( entry.isDirectory ) {
            Serial.print( "    <DIR>" );
        } else {
            String sizeStr = formatFileSize( entry.size );
            int sizeWidth = 10 - sizeStr.length( );
            for ( int s = 0; s < sizeWidth && s >= 0; s++ )
                Serial.print( " " );
            Serial.print( sizeStr );
        }

        Serial.println( );
    }

    // Show scroll indicator if needed
    if ( fileCount > maxDisplayLines ) {
        changeTerminalColor( FileColors::STATUS, true );
        Serial.println( "╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌" );
        Serial.println( "Showing " + String( startIdx + 1 ) + "-" + String( endIdx ) + " of " + String( fileCount ) + " files" );
        if ( listingTruncated ) {
            Serial.println( "(listing truncated at " + String( maxFiles ) + " entries)" );
        }
    }

    changeTerminalColor( -1, false ); // Reset colors
}

void FileManager::showFileInfo( const FileEntry& file ) {
    changeTerminalColor( FileColors::HEADER, true );
    Serial.println( "\n╭───────────────────────────────────────────────────────────────────────────╮" );
    Serial.println( "│                                FILE INFO                                  │" );
    Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );

    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "Name: " + file.name );
    Serial.println( "Path: " + file.path );
    Serial.print( "Type: " );
    Serial.println( file.isDirectory ? "Directory" : "File" );
    if ( !file.isDirectory ) {
        Serial.println( "Size: " + formatFileSize( file.size ) );
    }
    if ( file.lastModified != 0 ) {
        Serial.println( "Modified: " + formatDateTime( file.lastModified ) );
    }
    Serial.println( "╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌" );

    changeTerminalColor( -1, false ); // Reset colors
    Serial.println( "\nPress any key to continue..." );
}

void FileManager::showHelp( ) {
    changeTerminalColor( FileColors::HEADER, true );
    Serial.println( "\n╭───────────────────────────────────────────────────────────────────────────╮" );
    Serial.println( "│                          FILE MANAGER HELP                                │" );
    Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );

    changeTerminalColor( FileColors::STATUS, true );
    Serial.println( "\n⌘ NAVIGATION:" );
    changeTerminalColor( 221, false ); // Yellow
    Serial.print( "  ↑/↓ or encoder  " );
    changeTerminalColor( 248, false );
    Serial.println( "- Move selection up/down" );
    changeTerminalColor( 221, false );
    Serial.print( "  Enter           " );
    changeTerminalColor( 248, false );
    Serial.println( "- Open directory or edit file" );
    changeTerminalColor( 221, false );
    Serial.print( "  ..              " );
    changeTerminalColor( 248, false );
    Serial.println( "- Go up one directory" );
    changeTerminalColor( 221, false );
    Serial.print( "  /               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Go to root directory" );

    changeTerminalColor( FileColors::STATUS, true );
    Serial.println( "\n⍺ FILE OPERATIONS:" );
    changeTerminalColor( 155, false ); // Green
    Serial.print( "  v               " );
    changeTerminalColor( 248, false );
    Serial.println( "- View file contents" );
    changeTerminalColor( 155, false );
    Serial.print( "  e               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Edit with eKilo editor" );
    changeTerminalColor( 155, false );
    Serial.print( "  n               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Create new file" );
    changeTerminalColor( 155, false );
    Serial.print( "  d               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Create new directory" );
    changeTerminalColor( 155, false );
    Serial.print( "  r               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Rename file/directory" );
    changeTerminalColor( 155, false );
    Serial.print( "  x               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Delete file/directory" );

    changeTerminalColor( FileColors::STATUS, true );
    Serial.println( "\n⟐ OTHER:" );
    changeTerminalColor( 207, false ); // Magenta
    Serial.print( "  i               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Show file info" );
    changeTerminalColor( 207, false );
    Serial.print( "  h               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Show this help" );
    changeTerminalColor( 207, false );
    Serial.print( "  q               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Quit file manager" );
    changeTerminalColor( 207, false );
    Serial.print( "  t               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Format/indent Python files in directory" );

    changeTerminalColor( FileColors::ERROR, true );
    Serial.println( "\n○ FILE TYPE COLORS:" );
    changeTerminalColor( FileColors::DIRECTORY, false );
    Serial.print( "⌘ Directories  " );
    changeTerminalColor( FileColors::PYTHON, false );
    Serial.print( "𓆚 Python  " );
    changeTerminalColor( FileColors::TEXT, false );
    Serial.print( "⍺ Text  " );
    changeTerminalColor( FileColors::CONFIG, false );
    Serial.print( "⚙ Config  " );
    changeTerminalColor( FileColors::NODEFILES, false );
    Serial.print( "☊ NodeFiles  " );
    changeTerminalColor( FileColors::COLORS, false );
    Serial.println( "⎃ Colors" );

    changeTerminalColor( -1, false ); // Reset colors
    Serial.println( "\nPress any key to continue..." );
}

void FileManager::moveSelection( int direction ) {
    if ( fileCount == 0 )
        return;

    selectedIndex += direction;

    // Wrap around
    if ( selectedIndex < 0 ) {
        selectedIndex = fileCount - 1;
    } else if ( selectedIndex >= fileCount ) {
        selectedIndex = 0;
    }

    // Reset horizontal scrolling when selection changes
    oledHorizontalOffset = 0;
    oledCursorPosition = 0;

    // Adjust display offset if needed (use configurable visible lines)
    if ( selectedIndex < displayOffset ) {
        displayOffset = selectedIndex;
    } else if ( selectedIndex >= displayOffset + textAreaLines ) {
        displayOffset = selectedIndex - textAreaLines + 1;
    }

    // Update display in place
    updateStatusLine( );
    updateFileListDisplay( );
    scheduleOLEDUpdate( );
}

FileEntry* FileManager::getCurrentFile( ) {
    if ( selectedIndex >= 0 && selectedIndex < fileCount ) {
        return &fileList[ selectedIndex ];
    }
    return nullptr;
}

// Run a Python script file directly via mp_embed_exec_str (no interactive REPL).
// Returns true on success.
static bool runPythonScript( const String& fullPath ) {
    File f = safeFileOpen( fullPath.c_str( ), "r" );
    if ( !f ) {
        Serial.println( "\r\nFailed to open " + fullPath );
        return false;
    }
    // Cap: the whole script is slurped into one heap String before exec, so
    // refuse anything implausibly large (matches writeStringToFile's 64KB cap)
    // instead of letting a big binary exhaust the heap.
    size_t scriptSize = f.size( );
    if ( scriptSize > 64 * 1024 ) {
        safeFileClose( f, false );
        Serial.println( "\r\nScript too large to run (" + String( scriptSize / 1024 ) + "KB, max 64KB): " + fullPath );
        return false;
    }
    String content = f.readString( );
    safeFileClose( f, false );  // Release fs_mutex before running the script
    if ( content.length( ) != scriptSize ) {
        // Short read = heap allocation failed mid-readString; don't exec a truncated script
        Serial.println( "\r\nFailed to read " + fullPath + " (out of memory?)" );
        return false;
    }
    if ( content.length( ) == 0 ) {
        Serial.println( "\r\nScript is empty: " + fullPath );
        return false;
    }
    Serial.println( "\r\nRunning " + fullPath + " ..." );
    bool ok = executePythonFileContent( content.c_str( ) );
    Serial.println( "\r\n--- script finished ---" );
    return ok;
}

// Resolve a bare script name (e.g. "adc_basics.py") to a full filesystem path
// by searching the standard Python directories.
String resolvePythonScriptPath( const String& filename ) {
    String name = filename;
    name.trim( );
    if ( name.length( ) == 0 )
        return "";

    // Strip leading '/' for the search (we'll add it per-directory)
    if ( name.startsWith( "/" ) )
        name = name.substring( 1 );

    // Ensure it has a .py or .pyw extension
    String lower = name;
    lower.toLowerCase( );
    if ( !lower.endsWith( ".py" ) && !lower.endsWith( ".pyw" ) )
        name += ".py";

    // If the original filename was an absolute path, check it directly first
    if ( filename.startsWith( "/" ) ) {
        if ( safeFileExists( filename.c_str( ), 500 ) )
            return filename;
    }

    // Search directories in priority order
    const char* searchDirs[] = {
        "/python_scripts",
        "/python_scripts/examples",
        "/python_scripts/lib",
        "/python_scripts/modules",
        "/"  // root
    };

    for ( const char* dir : searchDirs ) {
        String candidate = String( dir );
        if ( !candidate.endsWith( "/" ) )
            candidate += "/";
        candidate += name;
        if ( safeFileExists( candidate.c_str( ), 500 ) )
            return candidate;
    }

    return "";  // not found
}

// Public wrapper: run a Python script given its full path.
bool runPythonScriptByPath( const String& fullPath ) {
    File f = safeFileOpen( fullPath.c_str( ), "r" );
    if ( !f ) {
        Serial.println( "\r\nFailed to open " + fullPath );
        return false;
    }
    // Same 64KB heap-safety cap as runPythonScript above.
    size_t scriptSize = f.size( );
    if ( scriptSize > 64 * 1024 ) {
        safeFileClose( f, false );
        Serial.println( "\r\nScript too large to run (" + String( scriptSize / 1024 ) + "KB, max 64KB): " + fullPath );
        return false;
    }
    String content = f.readString( );
    safeFileClose( f, false );
    if ( content.length( ) != scriptSize ) {
        Serial.println( "\r\nFailed to read " + fullPath + " (out of memory?)" );
        return false;
    }
    if ( content.length( ) == 0 ) {
        Serial.println( "\r\nScript is empty: " + fullPath );
        return false;
    }
    Serial.println( "\r\nRunning " + fullPath + " ..." );
    setTerminalLineBuffering( true ); // script needs raw/interactive input

    bool ok = executePythonFileContent( content.c_str( ) );

    pushLineBufferingToApp( ); // resync app to the user's config afterward
    Serial.println( "\r\n--- script finished ---" );
    Serial.flush();
    return ok;
}

void FileManager::selectCurrentFile( ) {
    FileEntry* file = getCurrentFile( );
    if ( !file )
        return;

    // Handle special ".." entry
    if ( file->name == ".." && file->path == "[UP]" ) {
        if ( goUp( ) ) {
            // Successfully went up - redraw the interface
            drawInterface( );
        }
        return;
    }

    if ( file->isDirectory ) {
        if ( changeDirectory( file->path ) ) {
            // Successfully changed directory - redraw the interface
            drawInterface( );
        }
    } else {
        String fullPath = getFullPath( currentPath, file->name );
        String lower = file->name;
        lower.toLowerCase( );

        // Type-based dispatch: run .py from click menu, load slot YAML, open image in BitmapEditor, else eKilo
        if ( fromClickMenu && ( lower.endsWith( ".py" ) || lower.endsWith( ".pyw" ) ) ) {
            // Defer script execution: store path and exit file manager
            // so the script runs with a clean terminal and proper I/O
            pendingScriptPath = fullPath;
            shouldExitForScript = true;
            return;  // run() loop will detect this and exit
        } else if ( lower.endsWith( ".yaml" ) && lower.startsWith( "slot" ) ) {
            String err;
            if ( SlotManager::getInstance( ).loadSlotFromPath( fullPath, err ) ) {
                outputToArea( "Loaded slot: " + file->name, FileColors::STATUS );
                changeTerminalColor( FileColors::STATUS, false );
                Serial.println( "\n\rLoaded " + file->name );
                changeTerminalColor( -1, false );
            } else {
                outputToArea( "Load failed: " + err, FileColors::ERROR );
            }
            scheduleOLEDUpdate( );
        } else if ( lower.endsWith( ".bin" ) || lower.endsWith( ".bmp" ) ) {
            changeTerminalColor( FileColors::STATUS, false );
            Serial.println( "\n\n\rOpening " + file->name + " in bitmap editor..." );
            changeTerminalColor( -1, false );
            if ( launchBitmapEditor( fullPath ) ) {
                refreshListing( );
                drawInterface( );
            }
        } else {
            editFile( fullPath );
            refreshListing( ); // Refresh in case file was modified
            drawInterface( );  // Redraw entire interface after editing
        }
    }
}

bool FileManager::createFile( const String& filename ) {
    String fullPath = getFullPath( currentPath, filename );

    if ( safeFileExists( fullPath.c_str( ) ) ) {
        outputToArea( "File already exists: " + filename, FileColors::ERROR );
        return false;
    }

    File file = safeFileOpen( fullPath.c_str( ), "w" );
    if ( file ) {
        safeFileClose( file, true );
        outputToArea( "Created file: " + filename, FileColors::STATUS );
        refreshListing( );
        return true;
    } else {
        outputToArea( "Failed to create file: " + filename, FileColors::ERROR );
        return false;
    }
}

bool FileManager::deleteFile( const String& filename ) {
    String fullPath = getFullPath( currentPath, filename );

    bool success = safeFileDelete( fullPath.c_str( ) );

    if ( success ) {
        outputToArea( "Deleted: " + filename, FileColors::STATUS );
        refreshListing( );
        return true;
    } else {
        outputToArea( "Failed to delete: " + filename, FileColors::ERROR );
        return false;
    }
}

bool FileManager::editFile( const String& filename ) {
    // Check if this is a bitmap file (.bin or .bmp)
    String lower = filename;
    lower.toLowerCase( );

    if ( lower.endsWith( ".bin" ) || lower.endsWith( ".bmp" ) ) {
        // Launch bitmap editor
        changeTerminalColor( FileColors::STATUS, false );
        Serial.println( "\n\n\rOpening " + filename + " in bitmap editor..." );
        changeTerminalColor( -1, false ); // Reset colors

        return launchBitmapEditor( filename );
    }

    // Regular text file - use eKilo
    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "\n\n\rOpening " + filename + " in text editor..." );
    changeTerminalColor( -1, false ); // Reset colors

    return editFileWithEkilo( filename );
}

bool FileManager::editFileWithEkilo( const String& filename ) {
    // Check available memory before opening file
    size_t freeHeap = rp2040.getFreeHeap( );
    if ( freeHeap < 2048 ) { // Require at least 2KB free for editor
        outputToArea( "ERROR: Not enough memory to open editor (" + String( freeHeap / 1024 ) + "KB free)", FileColors::ERROR );
        return false;
    }

    if ( replMode || true ) {
        // In REPL mode - use launchEkiloREPL and store returned content
        // Serial.println("REPLMODE");
        // delay(1000);
        String content = launchEkiloREPL( filename.c_str( ) );

        // Serial.print("return\n\nn\n\n\n\n\n\n\n\n\n\n\n\n\r");

        // Check for special marker indicating REPL was already launched
        if ( content == "[REPL_LAUNCHED]" ) {
            // REPL was launched with Ctrl+P - file already executed
            // Clear content and exit file manager
            lastOpenedFileContent = "";
            shouldExitForREPL = true;
        }
        // Check for zero-copy transfer path (new preferred mechanism)
        else if ( ContextManager::getInstance( ).hasTransferPath( ) ) {
            // Transfer path is set - store empty and let caller use transfer path
            // The caller (Python REPL) will read directly from the file
            lastOpenedFileContent = "";
            shouldExitForREPL = true;
            // Note: Transfer path is NOT cleared here - caller will clear after reading
        }
        // Legacy: content was passed directly (backward compatibility)
        else if ( content.length( ) > 0 ) {
            // Match the SharedBuffer cap (24KB) - the historical 8KB limit
            // here was tighter than the underlying buffer for no good reason.
            if ( content.length( ) > SHARED_BUFFER_SIZE ) {
                outputToArea( "WARNING: File content too large for REPL mode, truncating", FileColors::ERROR );
                content = content.substring( 0, SHARED_BUFFER_SIZE );
            }
            lastOpenedFileContent = content;
            shouldExitForREPL = true;
        }
        // No content returned and no transfer path - editor was closed without saving
        else {
            // Check if file exists on disk (user may have previously saved it)
            File file = safeFileOpen( filename.c_str( ), "r" );
            if ( file ) {
                size_t fileSize = file.size( );
                if ( fileSize > SHARED_BUFFER_SIZE ) {
                    safeFileClose( file, false );
                    outputToArea( "WARNING: File too large for REPL mode (" + String( fileSize / 1024 ) + "KB)", FileColors::ERROR );
                    return false;
                }
                String existingContent = file.readString( );
                safeFileClose( file, false );
                if ( existingContent.length( ) > 0 ) {
                    lastOpenedFileContent = existingContent;
                }
            }
        }
    } else {
        // Serial.println("NORMALMODE");
        // delay(1000);
        // Normal mode - use regular launchEkilo
        launchEkiloStandalone( filename.c_str( ) );
    }
    return true;
}

// Bounded line read for file previews. Reads to '\n' or maxLen chars,
// whichever comes first, so a large binary with no newlines can't slurp
// the whole file into one heap String (readStringUntil is unbounded).
// ponytail: a line longer than maxLen just continues on the next preview
// line instead of skipping to EOL - fine for a truncated preview.
static String readLineBounded( File& f, size_t maxLen ) {
    String line;
    while ( f.available( ) && line.length( ) < maxLen ) {
        char ch = (char)f.read( );
        if ( ch == '\n' )
            break;
        line += ch;
    }
    return line;
}

bool FileManager::viewFile( const String& filename ) {
    File file = safeFileOpen( filename.c_str( ), "r" );
    if ( !file ) {
        changeTerminalColor( FileColors::ERROR, false );
        Serial.println( "Failed to open file: " + filename );
        changeTerminalColor( -1, false ); // Reset colors
        return false;
    }

    changeTerminalColor( FileColors::HEADER, true );
    Serial.println( "\n╭───────────────────────────────────────────────────────────────────────────╮" );
    Serial.println( "│                              FILE VIEWER                                  │" );
    Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );
    Serial.println( "File: " + filename );
    Serial.println( "╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌" );

    changeTerminalColor( 248, true ); // Light grey for content

    int lineCount = 0;
    while ( file.available( ) && lineCount < 50 ) { // Limit to 50 lines for viewing
        String line = readLineBounded( file, 200 );
        Serial.println( line );
        lineCount++;
    }

    if ( file.available( ) ) {
        changeTerminalColor( FileColors::STATUS, false );
        Serial.println( "\n... (file continues - use 'e' to edit full file)" );
    }

    safeFileClose( file, false );
    changeTerminalColor( -1, false ); // Reset colors
    Serial.println( "\nPress any key to continue..." );

    while ( Serial.available( ) == 0 ) {
        delayMicroseconds( 100 ); // Use normal delay for blocking wait
    }
    while ( Serial.available( ) > 0 ) {
        Serial.read( );
    }

    return true;
}

void FileManager::run( ) {
    refreshListing( );
    bool running = true;

    // Click/hold classifier on the physical pin. The FM is a "slow UI":
    // selection fires on ENC_CLICK (release before the hold threshold), so
    // holding the wheel to back out NEVER opens the highlighted file first.
    EncoderClickTracker clickTracker;
    clickTracker.reset( );

    // Initialize interactive mode
    initInteractiveMode( );

    // Initial display
    drawInterface( );

    while ( running ) {
        // In REPL mode, check if we should exit after content is ready
        if ( replMode && shouldExitForREPL ) {
            // Close any open files before exiting
            closeAllFiles( );
            // Clear screen completely before exiting to avoid weird state
            clearScreen( );
            running = false;
            break;
        }
        
        // Check if a Python script was selected for deferred execution
        if ( shouldExitForScript ) {
            closeAllFiles( );
            clearScreen( );
            running = false;
            break;
        }

        // Call SlotManager service for file change detection
        // This ensures slot files update immediately when we return from editor
        SlotManager::getInstance( ).service( );

        // Process any pending OLED updates
        processOLEDUpdate( );

        // Display any legacy filesystem messages at bottom of screen (fallback only)
        displayFilesystemMessages( );

        // Note: Removed returnToMainMenu check - editor returns directly to file manager now

        // Handle rotary encoder input
        // rotaryEncoderStuff();

        // Check for encoder rotation. Act on the LEVEL, not on a transition:
        // the old `encoderDirectionState != lastEncoderDirectionState` gate
        // raced with Core 1's poller - if a new detent landed between our
        // NONE write and the re-read that fed lastEncoderDirectionState, we
        // recorded last==UP with the state still UP, and every further
        // same-direction detent compared equal: rotation permanently wedged
        // until the user turned the other way. getMenuSelection() consumes
        // by level for the same reason.
        if ( encoderDirectionState == UP ) {
            encoderDirectionState = NONE;
            moveSelection( -1 );
            scheduleOLEDUpdate( );
            lastInputTime = micros( ); // Record input time
        } else if ( encoderDirectionState == DOWN ) {
            encoderDirectionState = NONE;
            moveSelection( 1 );
            scheduleOLEDUpdate( );
            lastInputTime = micros( ); // Record input time
        }

        // Encoder button: classified on the physical pin (EncoderClickTracker)
        // instead of the shared cross-core state machine. CLICK = released
        // before the hold threshold (so a hold never opens the highlighted
        // file on its way to being a hold), HOLD = back one level,
        // LONG_HOLD = quit entirely (Ctrl-Q equivalent).
        switch ( clickTracker.poll( ) ) {
        case ENC_CLICK:
            // NOTE: deliberately do NOT clear inClickMenu here. Clearing
            // it on the first click knocked Core 1 out of the menu-render
            // branch for the rest of the session, so the breadboard file
            // text stopped showing and showLEDsCore2=-1 repainted the
            // wires over it. inClickMenu is owned by the entry/exit code
            // (pickPythonScriptFromClickMenu).
            selectCurrentFile( );
            scheduleOLEDUpdate( );
            lastInputTime = micros( ); // Record input time
            blockInputBriefly( );      // Block input briefly after interface changes
            // Note: selectCurrentFile() now handles interface redrawing internally
            break;

        case ENC_HOLD:
            // Hold-to-back, matching the menu idiom: one directory up. At the
            // top level there's nothing further up — keep holding and
            // ENC_LONG_HOLD below quits, or release and re-hold to exit.
            if ( currentPath == "/" ) {
                closeAllFiles( );
                // Swallow the rest of the hold so the main screen doesn't
                // see it as a disconnect/reboot gesture.
                unsigned long rootReleaseWait = millis( );
                while ( isEncoderButtonPhysicallyPressed( ) &&
                        millis( ) - rootReleaseWait < 3000 ) {
                    TinyUSB_Device_Task( ); // mutex-guarded pump
                    delayMicroseconds( 500 );
                }
                encoderButtonState = IDLE;
                lastButtonEncoderState = IDLE;
                running = false;
            } else {
                goUp( );
                drawInterface( );
                scheduleOLEDUpdate( );
                lastInputTime = micros( );
                blockInputBriefly( );
            }
            break;

        case ENC_LONG_HOLD: {
            // Universal quit (Ctrl-Q equivalent) from anywhere in the tree.
            closeAllFiles( );
            // Swallow the rest of the physical hold so the main screen
            // doesn't see it as a disconnect/reboot gesture. 3s bail-out
            // so a stuck/shorted button can't pin Core 0 here.
            unsigned long releaseWaitStart = millis( );
            while ( isEncoderButtonPhysicallyPressed( ) &&
                    millis( ) - releaseWaitStart < 3000 ) {
                TinyUSB_Device_Task( ); // mutex-guarded pump
                delayMicroseconds( 500 );
            }
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            running = false;
            break;
        }

        default:
            break;
        }
        if ( !running ) {
            break;
        }
        // Keep the shared state machine from leaking FM-era events to the
        // main screen: consume anything it latched while we own the wheel.
        if ( encoderButtonState == RELEASED || encoderButtonState == DOUBLECLICKED ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
        }

        // Keep the USB device stack serviced. The file manager blocks Core 0
        // for its whole session, and a navigation burst (each key = OLED I2C +
        // a multi-KB ANSI redraw + a directory walk) can starve TinyUSB long
        // enough that the host gives up and drops the CDC port entirely.
        yield( ); // mutex-guarded pump + CDC flush

        // Check if input is blocked (except Ctrl+Q and ESC)
        char input = 0;
        if ( Serial.available( ) ) {
            char c = Serial.peek( ); // Look at next character without reading it

            // Allow Ctrl+Q and ESC through even during input blocking
            if ( c == 17 || c == 27 || !isInputBlocked( ) ) { // Ctrl+Q, ESC, or not blocked
                input = Serial.read( );
                lastInputTime = micros( ); // Record input time for any serial input
                scheduleOLEDUpdate( );     // Schedule OLED update for any input
            } else {
                // Input is blocked, clear buffer except Ctrl+Q
                clearBufferedInput( true );
                delayMicroseconds( 100 );
                continue;
            }
        } else {
            delayMicroseconds( 100 );
            continue;
        }

        // Handle input
        switch ( input ) {
        case 'q':
        case 'Q':
        case 17: // Ctrl-Q
            // Close any open files before exiting
            closeAllFiles( );
            running = false;
            break;

        case 16: // Ctrl-P
            // In file manager, Ctrl-P doesn't make sense (no content to save)
            // Ignore silently to prevent crashes
            outputToArea( "Ctrl-P: No action in file manager", FileColors::STATUS );
            // Serial.println("Ctrl-P: No action in file manager");
            // Serial.flush();
            // delay(2000);
            // shouldExitForREPL = true;
            // delay(2000);
            break;

        case '\r':
        case '\n':
            selectCurrentFile( );
            blockInputBriefly( ); // Block input briefly after interface changes
            // Note: selectCurrentFile() now handles interface redrawing internally
            break;

        case 'h':
        case 'H':
            showInteractiveHelp( );
            break;

        case 'v':
        case 'V': {
            FileEntry* file = getCurrentFile( );
            if ( file && !file->isDirectory ) {
                showInteractiveFileView( file->path );
            }
            break;
        }

        case 'e':
        case 'E': {
            FileEntry* file = getCurrentFile( );
            if ( file && !file->isDirectory ) {
                editFile( file->path );
                refreshListing( );    // Refresh in case file was modified
                drawInterface( );     // Redraw entire interface
                blockInputBriefly( ); // Block input after interface refresh
            }
            break;
        }

        case 'i':
        case 'I': {
            FileEntry* file = getCurrentFile( );
            if ( file ) {
                showInteractiveFileInfo( *file );
            }
            break;
        }

        case 'n':
        case 'N': {
            String filename = promptForFilename( "Enter filename (with extension): " );
            if ( filename.length( ) > 0 && isValidFilename( filename ) ) {
                createFile( filename );
                refreshListing( );
                drawInterface( );
                blockInputBriefly( ); // Block input after interface refresh
            }
            break;
        }

        case 'd':
        case 'D': {
            String dirname = promptForFilename( "Enter directory name: " );
            if ( dirname.length( ) > 0 && isValidFilename( dirname ) ) {
                createDirectory( dirname );
                refreshListing( );
                drawInterface( );
                blockInputBriefly( ); // Block input after interface refresh
            }
            break;
        }

        case 'x':
        case 'X': {
            FileEntry* file = getCurrentFile( );
            if ( file && !( file->name == ".." && file->path == "[UP]" ) ) {
                String confirmPrompt;
                if ( file->isDirectory ) {
                    confirmPrompt = "Delete directory '" + file->name + "'? (y/N): ";
                } else {
                    confirmPrompt = "Delete file '" + file->name + "'? (y/N): ";
                }

                // Show prompt in output area
                if ( outputAreaCurrentRow == 0 ) {
                    showOutputAreaBorder( );
                }

                // Show the prompt WITHOUT automatic newline
                moveCursor( outputAreaStartRow + outputAreaCurrentRow, 1 );
                clearCurrentLine( );
                changeTerminalColor( FileColors::ERROR, false );
                Serial.print( confirmPrompt );
                changeTerminalColor( -1, false );

                // Show cursor for input
                Serial.print( "\033[?25h" );
                Serial.flush( );

                // Drain any already-buffered input so a queued byte (paste of
                // "xy", key auto-repeat, terminal burst) can't auto-confirm the delete
                while ( Serial.available( ) ) {
                    Serial.read( );
                }

                // Wait for confirmation and echo the character
                char confirm = 0;
                while ( confirm == 0 ) {
                    if ( Serial.available( ) ) {
                        confirm = Serial.read( );
                        // Echo the character immediately
                        Serial.print( confirm );
                        Serial.flush( );
                    } else {
                        delayMicroseconds( 100 );
                    }
                }

                Serial.print( "\n\r" );
                Serial.print( "\033[?25l" ); // Hide cursor
                Serial.flush( );

                outputAreaCurrentRow++; // Move to next line

                if ( confirm == 'y' || confirm == 'Y' ) {
                    // Capture before refreshListing() rebuilds fileList under `file`
                    bool wasDirectory = file->isDirectory;
                    outputToArea( "Deleting...", FileColors::STATUS );
                    bool deleted = deleteFile( file->name );
                    refreshListing( );
                    drawInterface( );
                    blockInputBriefly( ); // Block input after interface refresh
                    if ( !deleted && wasDirectory ) {
                        // FatFS.remove (f_unlink) deletes files and EMPTY dirs;
                        // a populated directory is the usual failure here.
                        // (Printed after drawInterface so the redraw's
                        // clearOutputArea doesn't immediately wipe it.)
                        outputToArea( "Delete failed: directory not empty - delete its contents first", FileColors::ERROR );
                    }
                } else {
                    outputToArea( "Delete cancelled", FileColors::STATUS );
                }
            }
            break;
        }

        case 27: { // ESC key or escape sequence (arrow keys)
            // Wait for the rest of an escape sequence: over USB CDC the '['
            // and final byte can land in a later packet, so poll up to ~25ms
            // instead of the old 2ms one-shot (which misparsed arrow keys as
            // bare ESC and bounced the user up a directory).
            unsigned long escWaitStart = millis( );
            while ( Serial.available( ) == 0 && millis( ) - escWaitStart < 25 ) {
                delayMicroseconds( 250 );
            }

            if ( Serial.available( ) ) {
                char seq1 = Serial.read( );
                bool isCSI = ( seq1 == '[' );
                if ( isCSI && Serial.available( ) == 0 ) {
                    // Same tolerant wait for the sequence's final byte
                    escWaitStart = millis( );
                    while ( Serial.available( ) == 0 && millis( ) - escWaitStart < 25 ) {
                        delayMicroseconds( 250 );
                    }
                }
                if ( isCSI && Serial.available( ) ) {
                    char seq2 = Serial.read( );
                    switch ( seq2 ) {
                    case 'A':
                        moveSelection( -1 );
                        break; // Up arrow
                    case 'B':
                        moveSelection( 1 );
                        break; // Down arrow
                    }
                } else {
                    // Not a recognized escape sequence, treat as standalone ESC
                    if ( currentPath == "/" ) {
                        // At root - exit file manager
                        closeAllFiles( );
                        running = false;
                    } else {
                        // Not at root - go up a directory
                        if ( goUp( ) ) {
                            drawInterface( );     // Redraw entire interface
                            blockInputBriefly( ); // Block input after interface refresh
                        }
                    }
                }
            } else {
                // No data following ESC - standalone ESC key press
                if ( currentPath == "/" ) {
                    // At root - exit file manager
                    closeAllFiles( );
                    running = false;
                } else {
                    // Not at root - go up a directory
                    if ( goUp( ) ) {
                        drawInterface( );     // Redraw entire interface
                        blockInputBriefly( ); // Block input after interface refresh
                    }
                }
            }
            break;
        }

        case '/':
            if ( goHome( ) ) {
                drawInterface( );
                blockInputBriefly( ); // Block input after interface refresh
            }
            break;

        case '.':
            if ( currentPath != "/" ) {
                if ( goUp( ) ) {
                    drawInterface( );
                    blockInputBriefly( ); // Block input after interface refresh
                }
            }
            break;

        case 'r':
        case 'R': {
            // Refresh directory listing
            refreshListing( );
            updateStatusLine( );
            updateFileListDisplay( );
            blockInputBriefly( ); // Block input after interface refresh
            break;
        }

        case 'u':
        case 'U': {
            // Show memory status
            size_t freeHeap = rp2040.getFreeHeap( );
            outputToArea( "Memory: " + String( freeHeap / 1024 ) + "KB free, " +
                              String( maxFiles ) + " max files",
                          FileColors::STATUS );
            break;
        }

        case 'm':
        case 'M': {
            // Manual MicroPython examples initialization
            clearFilesystemMessages( ); // Clear old messages first
            addFilesystemMessage( "Manual initialization requested...", 155 );
            initializeMicroPythonExamples( true ); // Force initialization
            refreshListing( );
            drawInterface( );
            blockInputBriefly( ); // Block input after interface refresh
            break;
        }

        default:
            // Handle unrecognized characters (including other control chars)
            if ( input >= 1 && input <= 31 && input != 17 && input != 16 && input != 27 ) {
                // Control character that we don't handle - show debug info
                outputToArea( "Unhandled control char: Ctrl+" + String( char( 'A' + input - 1 ) ), FileColors::STATUS );
            }
            // For regular printable characters, just ignore silently
            break;
        }
    }

    // Clean up interactive mode
    clearScreen( );
    showCursor( ); // Ensure cursor is visible when exiting

    // Restore normal font if we were using small fonts
    if ( oled.oledConnected ) {
        oled.restoreNormalFont( );
    }

    // Only exit interactive mode if we're NOT returning to main menu after REPL launch
    if ( !shouldExitForREPL ) {
        // Normal file manager exit: resync the app to the user's config.
        pushLineBufferingToApp( );
        delay( 10 ); // Give system time to switch modes
    }
    // If shouldExitForREPL is true, we're returning to main menu and want to keep interactive mode on

    // Close any open files before exiting file manager
    closeAllFiles( );

    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "Exiting File Manager..." );
    changeTerminalColor( -1, false ); // Reset colors

    // Print main menu when returning from editor
    // printMainMenu(0);
}

// Interactive mode implementation
void FileManager::initInteractiveMode( ) {
    // Enter Jumperless interactive mode (raw input for navigation keys)
    setTerminalLineBuffering( true );
    delay( 10 ); // Give system time to switch modes

    // Show cursor by default - only hide during drawing
    showCursor( );
}

void FileManager::clearScreen( ) {
    Serial.print( "\033[2J" ); // Clear entire screen
    Serial.print( "\033[H" );  // Move cursor to home (1,1)
    Serial.flush( );
}

void FileManager::moveCursor( int row, int col ) {
    Serial.print( "\033[" );
    Serial.print( row );
    Serial.print( ";" );
    Serial.print( col );
    Serial.print( "H" );
    Serial.flush( );
}

void FileManager::hideCursor( ) {
    Serial.print( "\033[?25l" );
    Serial.flush( );
}

void FileManager::showCursor( ) {
    Serial.print( "\033[?25h" );
    Serial.flush( );
}

void FileManager::clearCurrentLine( ) {
    Serial.print( "\033[2K" ); // Clear entire line
    Serial.print( "\r" );      // Return to beginning of line
    Serial.flush( );
}

void FileManager::drawInterface( bool fullScreen ) {
    // Always use full interface - no special REPL mode anymore
    // Full screen mode (original behavior)

    // Hide cursor during drawing for clean interface
    hideCursor( );

    // Clear output area when redrawing interface
    clearOutputArea( );

    // Position for header
    if ( fullScreen ) {
        moveCursor( 1, 1 );
    }
    // Draw header
    changeTerminalColor( FileColors::HEADER, false );
    Serial.println( "╭───────────────────────────────────────────────────────────────────────────╮" );
    Serial.println( "│                            JUMPERLESS FILE MANAGER                        │" );
    Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );
    changeTerminalColor( -1, false ); // Reset colors

    // Draw status line
    updateStatusLine( );

    // Draw file listing
    updateFileListDisplay( );

    // Draw comprehensive help lines at bottom (positioned after file list area)
    int fileListStartRow = 6;
    int fileListEndRow = fileListStartRow + textAreaLines - 1;
    int helpStartRow = fileListEndRow + 2; // Leave 1 line gap after file list
    if ( fullScreen ) {
        moveCursor( helpStartRow, 1 );
    }
    changeTerminalColor( 125, false );
    Serial.print( " [enter] = open   │ h = help │ v = quick view │ ↑↓/wheel = nav │ . = up dir   |" );
    if ( fullScreen ) {
        moveCursor( helpStartRow + 1, 1 );
    }
    changeTerminalColor( 89, false );
    Serial.print( " CTRL + q = quit  │ e = edit │ n = new file   │ d = new dir    │ u = memory   |" );

    changeTerminalColor( -1, false ); // Reset colors

    // Show output area border
    showOutputAreaBorder( );

    // Initialize and display persistent filesystem messages
    initializePersistentMessageArea( );
    displayPersistentMessages( );

    Serial.flush( );

    // Show cursor after drawing is complete
    showCursor( );
}

void FileManager::updateStatusLine( ) {
    hideCursor( ); // Hide cursor during status update
    moveCursor( 4, 1 );
    clearCurrentLine( );

    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( "⌘ Current Path: " );
    changeTerminalColor( FileColors::DIRECTORY, false );

    // Truncate path if too long
    String displayPath = currentPath;
    if ( displayPath.length( ) > 50 ) {
        displayPath = "..." + displayPath.substring( displayPath.length( ) - 47 );
    }
    Serial.print( displayPath );

    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( "  |  Files: " );
    Serial.print( fileCount );
    if ( fileCount > 0 ) {
        Serial.print( "  |  Selected: " );
        Serial.print( selectedIndex + 1 );
        Serial.print( "/" );
        Serial.print( fileCount );
    }

    Serial.print( "\x1b[0m" ); // Reset colors
    Serial.flush( );

    showCursor( ); // Show cursor after status update
}

void FileManager::updateFileListDisplay( ) {
    // Always use full interface - no special REPL mode anymore
    // Temporarily hide cursor during update for clean display
    hideCursor( );

    // Full screen mode (original behavior)
    // Clear file listing area (dynamically sized)
    int fileListStartRow = 6;
    int fileListEndRow = fileListStartRow + textAreaLines - 1;
    for ( int i = fileListStartRow; i <= fileListEndRow; i++ ) {
        moveCursor( i, 1 );
        clearCurrentLine( );
    }

    // Handle special case where filesystem is not available
    if ( currentPath == "[NO_FS]" ) {
        moveCursor( 8, 5 );
        changeTerminalColor( FileColors::ERROR, false );
        Serial.print( "** FILESYSTEM NOT AVAILABLE **" );

        moveCursor( 10, 5 );
        changeTerminalColor( FileColors::STATUS, false );
        Serial.print( "Directory access failed after mount" );

        moveCursor( 12, 5 );
        Serial.print( "Press 'q' to quit file manager" );

        changeTerminalColor( 0, false );
        Serial.flush( );
        delayMicroseconds( 100 ); // Short delay to show error
        return;
    }

    // Calculate display range
    int startIdx = displayOffset;
    int endIdx = min( displayOffset + maxDisplayLines, fileCount );
    endIdx = min( endIdx, startIdx + textAreaLines ); // Use configurable lines

    for ( int i = startIdx; i < endIdx; i++ ) {
        bool isSelected = ( i == selectedIndex );
        FileEntry& entry = fileList[ i ];

        // Position cursor for this file entry
        moveCursor( fileListStartRow + ( i - startIdx ), 3 );
        clearCurrentLine( );

        // Calculate indentation based on directory depth
        int currentDepth = calculatePathDepth( currentPath );
        String indentPrefix = "";
        for ( int d = 0; d < currentDepth; d++ ) {
            indentPrefix += "  "; // 2 spaces per level
        }

        // Selection indicator
        if ( isSelected ) {
            changeTerminalColor( 226, false ); // Bright yellow background
            Serial.print( "► " );
        } else {
            Serial.print( "  " );
        }

        // Add visual indentation
        Serial.print( indentPrefix );

        // File type color and icon
        int color = FileColors::UNKNOWN;
        switch ( entry.type ) {
        case FILE_TYPE_DIRECTORY:
            color = FileColors::DIRECTORY;
            break;
        case FILE_TYPE_PYTHON:
            color = FileColors::PYTHON;
            break;
        case FILE_TYPE_TEXT:
            color = FileColors::TEXT;
            break;
        case FILE_TYPE_CONFIG:
            color = FileColors::CONFIG;
            break;
        case FILE_TYPE_JSON:
            color = FileColors::JSON;
            break;
        case FILE_TYPE_NODEFILES:
            color = FileColors::NODEFILES;
            break;
        case FILE_TYPE_BITMAP:
            color = FileColors::BITMAP;
            break;
        case FILE_TYPE_IMAGE:
            color = FileColors::IMAGE;
            break;
        case FILE_TYPE_LED:
            color = FileColors::LED;
            break;
        case FILE_TYPE_AUDIO:
            color = FileColors::AUDIO;
            break;
        case FILE_TYPE_VIDEO:
            color = FileColors::VIDEO;
            break;
        case FILE_TYPE_DOCUMENT:
            color = FileColors::DOCUMENT;
            break;
        case FILE_TYPE_ARCHIVE:
            color = FileColors::ARCHIVE;
            break;
        default:
            color = FileColors::UNKNOWN;
            break;
        }
        changeTerminalColor( color, false );

        // Special handling for ".." entry
        if ( entry.name == ".." && entry.path == "[UP]" ) {
            Serial.print( "⌘ " );
            Serial.print( ".." );

            // Add padding to align with size column (adjusted for indentation)
            int usedSpace = 2 + indentPrefix.length( ) + 2 + 2; // selector + indent + icon + ".."
            int padding = 50 - usedSpace;
            for ( int p = 0; p < padding && p >= 0; p++ )
                Serial.print( " " );
        } else {
            Serial.print( getFileIcon( entry.type ) + " " );

            // Filename
            String displayName = entry.name;
            int maxNameLength = 45 - ( currentDepth * 2 ); // Adjust for indentation
            if ( displayName.length( ) > maxNameLength ) {
                displayName = displayName.substring( 0, maxNameLength - 3 ) + "...";
            }
            Serial.print( displayName );

            // Padding for size column (adjusted for indentation)
            int usedSpace = 2 + indentPrefix.length( ) + 2 + displayName.length( ); // selector + indent + icon + name
            int padding = 50 - usedSpace;
            for ( int p = 0; p < padding && p >= 0; p++ )
                Serial.print( " " );
        }

        changeTerminalColor( 248, false ); // Light grey for size
        if ( entry.name == ".." && entry.path == "[UP]" ) {
            Serial.print( "     <UP>" );
        } else if ( entry.isDirectory ) {
            Serial.print( "    <DIR>" );
        } else {
            String sizeStr = formatFileSize( entry.size );
            int sizeWidth = 10 - sizeStr.length( );
            for ( int s = 0; s < sizeWidth && s >= 0; s++ )
                Serial.print( " " );
            Serial.print( sizeStr );
        }

        changeTerminalColor( 0, false );
    }

    // Show scroll indicator if needed
    if ( fileCount > textAreaLines ) {
        moveCursor( fileListEndRow + 1, 3 );
        changeTerminalColor( FileColors::STATUS, false );
        Serial.print( "Showing " );
        Serial.print( startIdx + 1 );
        Serial.print( "-" );
        Serial.print( min( endIdx, fileCount ) );
        Serial.print( " of " );
        Serial.print( fileCount );
        Serial.print( " files" );
        if ( listingTruncated ) {
            Serial.print( " (listing truncated at " );
            Serial.print( maxFiles );
            Serial.print( " entries)" );
        }
        changeTerminalColor( 0, false );
    }

    Serial.flush( );

    // Show cursor after update is complete
    showCursor( );
}

void FileManager::showInteractiveHelp( ) {
    // Save current screen
    clearScreen( );

    // Draw help screen
    moveCursor( 1, 1 );
    changeTerminalColor( FileColors::HEADER, false );
    Serial.println( "╭───────────────────────────────────────────────────────────────────────────╮" );
    Serial.println( "│                          FILE MANAGER HELP                                │" );
    Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );

    moveCursor( 4, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "⌘ NAVIGATION:" );
    moveCursor( 5, 5 );
    changeTerminalColor( 221, false ); // Yellow
    Serial.print( "↑/↓ or encoder  " );
    changeTerminalColor( 248, false );
    Serial.println( "- Move selection up/down" );
    moveCursor( 6, 5 );
    changeTerminalColor( 221, false );
    Serial.print( "Enter/Click     " );
    changeTerminalColor( 248, false );
    Serial.println( "- Open directory or edit file" );
    moveCursor( 7, 5 );
    changeTerminalColor( 221, false );
    Serial.print( "..              " );
    changeTerminalColor( 248, false );
    Serial.println( "- Go up one directory" );
    moveCursor( 8, 5 );
    changeTerminalColor( 221, false );
    Serial.print( "/               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Go to root directory" );

    moveCursor( 10, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "⍺ FILE OPERATIONS:" );
    moveCursor( 11, 5 );
    changeTerminalColor( 155, false ); // Green
    Serial.print( "v               " );
    changeTerminalColor( 248, false );
    Serial.println( "- View file contents" );
    moveCursor( 12, 5 );
    changeTerminalColor( 155, false );
    Serial.print( "e               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Edit with eKilo editor" );
    moveCursor( 13, 5 );
    changeTerminalColor( 155, false );
    Serial.print( "i               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Show file information" );

    moveCursor( 15, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "⚙ CREATE/DELETE:" );
    moveCursor( 16, 5 );
    changeTerminalColor( 155, false ); // Green
    Serial.print( "n               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Create new file" );
    moveCursor( 17, 5 );
    changeTerminalColor( 155, false );
    Serial.print( "d               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Create new directory" );
    moveCursor( 18, 5 );
    changeTerminalColor( 207, false ); // Magenta (red)
    Serial.print( "x               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Delete file/directory" );

    moveCursor( 20, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.println( "⟐ OTHER:" );
    moveCursor( 21, 5 );
    changeTerminalColor( 207, false ); // Magenta
    Serial.print( "r               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Refresh directory listing" );
    moveCursor( 22, 5 );
    changeTerminalColor( 207, false );
    Serial.print( "u               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Show memory status" );
    moveCursor( 23, 5 );
    changeTerminalColor( 207, false );
    Serial.print( "m               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Force initialize MicroPython examples" );
    moveCursor( 24, 5 );
    changeTerminalColor( 207, false );
    Serial.print( "h               " );
    changeTerminalColor( 248, false );
    Serial.println( "- Show this help" );
    moveCursor( 25, 5 );
    changeTerminalColor( 207, false );
    Serial.print( "CTRL+q          " );
    changeTerminalColor( 248, false );
    Serial.println( "- Quit file manager" );

    moveCursor( 26, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( "Press any key to return..." );
    changeTerminalColor( 0, false );
    Serial.flush( );

    // Wait for keypress
    while ( Serial.available( ) == 0 )
        delayMicroseconds( 100 );
    while ( Serial.available( ) > 0 )
        Serial.read( );

    // Redraw main interface
    drawInterface( );
}

void FileManager::showInteractiveFileView( const String& filename ) {
    // Save current screen
    clearScreen( );

    File file = safeFileOpen( filename.c_str( ), "r" );
    if ( !file ) {
        moveCursor( 5, 5 );
        changeTerminalColor( FileColors::ERROR, false );
        Serial.print( "Failed to open file: " + filename );
        changeTerminalColor( 0, false );
        Serial.flush( );
        delayMicroseconds( 2000 );
        drawInterface( );
        return;
    }

    // Draw header
    moveCursor( 1, 1 );
    changeTerminalColor( FileColors::HEADER, false );
    Serial.println( "╭───────────────────────────────────────────────────────────────────────────╮" );
    Serial.println( "│                              FILE VIEWER                                  │" );
    Serial.println( "╰───────────────────────────────────────────────────────────────────────────╯" );

    moveCursor( 4, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( "File: " );
    Serial.println( filename );

    // Show file content (first 15 lines)
    changeTerminalColor( 248, false ); // Light grey for content
    int lineCount = 0;
    int row = 6;
    while ( file.available( ) && lineCount < 15 && row < 21 ) {
        String line = readLineBounded( file, 200 );
        moveCursor( row, 3 );
        Serial.print( line.substring( 0, 75 ) ); // Truncate long lines
        row++;
        lineCount++;
    }

    if ( file.available( ) ) {
        moveCursor( 21, 3 );
        changeTerminalColor( FileColors::STATUS, false );
        Serial.print( "... (file continues - press 'e' to edit full file)" );
    }

    safeFileClose( file, false );

    moveCursor( 23, 3 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( "Press any key to return..." );
    changeTerminalColor( 0, false );
    Serial.flush( );

    // Wait for keypress
    while ( Serial.available( ) == 0 )
        delayMicroseconds( 100 );
    while ( Serial.available( ) > 0 )
        Serial.read( );

    // Redraw main interface
    drawInterface( );
}

void FileManager::showInteractiveFileInfo( const FileEntry& file ) {
    // Use a small info popup instead of full screen
    moveCursor( 18, 5 );
    clearCurrentLine( );
    changeTerminalColor( FileColors::HEADER, false );
    Serial.print( "Info: " );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( file.name );
    Serial.print( " | Size: " );
    if ( file.isDirectory ) {
        Serial.print( "<DIR>" );
    } else {
        Serial.print( formatFileSize( file.size ) );
    }
    if ( file.lastModified != 0 ) {
        Serial.print( " | Modified: " );
        Serial.print( formatDateTime( file.lastModified ) );
    }
    changeTerminalColor( 0, false );
    Serial.flush( );

    // Also show on OLED if connected
    if ( oled.oledConnected ) {
        String infoText = file.name;
        if ( file.isDirectory ) {
            infoText += " <DIR>";
        } else {
            infoText += " " + formatFileSize( file.size );
        }
        oled.showMultiLineSmallText( infoText.c_str( ), true );
        oled.restoreNormalFont( );
    }

    // Clear info after 3 seconds
    delay( 10 ); // Shortened from 3000ms
    moveCursor( 18, 5 );
    clearCurrentLine( );
    Serial.flush( );
}

// Utility functions
String getFullPath( const String& basePath, const String& filename ) {
    if ( basePath.endsWith( "/" ) ) {
        return basePath + filename;
    } else {
        return basePath + "/" + filename;
    }
}

bool isValidFilename( const String& filename ) {
    if ( filename.length( ) == 0 )
        return false;

    // "." and ".." are directory references, not creatable names
    if ( filename == "." || filename == ".." )
        return false;

    // Check for invalid characters. '/' and '\' are rejected so new-file/
    // rename can't create entries outside the visible directory.
    String invalidChars = "<>:\"|?*/\\";
    for ( int i = 0; i < invalidChars.length( ); i++ ) {
        if ( filename.indexOf( invalidChars[ i ] ) >= 0 ) {
            return false;
        }
    }

    return true;
}

void printColoredPath( const String& path ) {
    String segments = path;
    segments.replace( "/", " > " );

    changeTerminalColor( FileColors::DIRECTORY, false );
    Serial.print( segments );
    changeTerminalColor( 0, false );
}

// App entry points
void filesystemApp( bool waitForEnter ) {
    // Push context onto the stack for proper navigation
    ContextEntry ctx( ContextType::FILE_MANAGER );
    ctx.onExit = []( void* ) {
        // Cleanup callback - called when context is popped
        closeAllFiles( );
        // Clear transfer path to prevent stale file paths from being loaded by Python
        ContextManager::getInstance( ).clearTransferPath( );
        if ( oled.oledConnected ) {
            oled.restoreNormalFont( );
        }
    };
    ContextManager::getInstance( ).pushContext( ctx );

    bool showOledInTerminal = jumperlessConfig.top_oled.show_in_terminal;
    jumperlessConfig.top_oled.show_in_terminal = false;

    // Clear screen at startup for clean display
    // Serial.print("\x1b[2J\x1b[H");
    // Serial.flush();

    changeTerminalColor( FileColors::HEADER, true );
    Serial.println( "\n  Starting Jumperless File Manager..." );
    Serial.println( "   Navigate files and directories with colorful interface" );
    Serial.println( "   Create, edit, and manage files using eKilo editor\n\n\r" );
    changeTerminalColor( FileColors::STATUS, true );
    // The "press enter" handshake only exists to break the app's cooked line-input
    // loop; skip it (and the prompt) when the app is already forwarding raw keys.
    bool wasInteractive = ( termInInteractiveMode == 1 );
    if ( waitForEnter && !wasInteractive ) {
        Serial.println( "   Press Enter to launch File Manager..." );
    }
    changeTerminalColor( 8, true );
    // Heap-allocate: Core 0's stack is a small scratch bank and the menu
    // launch path is already deep; a stack-local FileManager (plus run()'s
    // call tree) overflowed into Core 1's stack. See core1_separate_stack
    // in main.cpp for the full post-mortem.
    std::unique_ptr<FileManager> managerPtr( new FileManager( ) );
    FileManager& manager = *managerPtr;
    manager.initInteractiveMode( );

    // Wait for user to press enter to break the input loop
    if ( waitForEnter && !wasInteractive ) {
        while ( Serial.available( ) == 0 && encoderButtonState == IDLE ) {
            delayMicroseconds( 100 );
        }

        // Clear the enter keypress
        while ( Serial.available( ) > 0 ) {
            Serial.read( );
        }
        encoderButtonState = IDLE;
    }

    // Save current screen state and switch to alternate screen buffer
    saveScreenState( );

    manager.clearScreen( );
    manager.hideCursor( );

    changeTerminalColor( FileColors::STATUS, true );
    Serial.println( "   Initializing filesystem..." );
    changeTerminalColor( 8, true );
    delayMicroseconds( 100 ); // Give time for initialization messages to be seen

    // Automatically create MicroPython examples if needed
    initializeMicroPythonExamples( );

    // Check if filesystem initialization was successful
    if ( manager.getCurrentPath( ) == "[NO_FS]" ) {
        changeTerminalColor( FileColors::ERROR, true );
        Serial.println( );
        Serial.println( "   FILESYSTEM INITIALIZATION FAILED" );
        changeTerminalColor( FileColors::STATUS, false );
        Serial.println( "   The file manager cannot access the FatFS filesystem." );
        Serial.println( "   This may be because:" );
        Serial.println( "   • FatFS is not properly initialized in the main firmware" );
        Serial.println( "   • No filesystem has been formatted on the flash storage" );
        Serial.println( "   • There is a hardware issue with the flash memory" );
        Serial.println( );
        Serial.println( "   You can still use the file manager interface, but" );
        Serial.println( "   file operations will not be available until the" );
        Serial.println( "   filesystem is properly set up." );
        Serial.println( );
        changeTerminalColor( FileColors::STATUS, true );
        Serial.print( "   Press 'q' to quit or any other key to continue..." );
        changeTerminalColor( 0, false );

        while ( Serial.available( ) == 0 ) {
            delayMicroseconds( 10 );
        }
        char c = Serial.read( );
        Serial.println( );

        if ( c == 'q' || c == 'Q' ) {
            Serial.println( "  File Manager cancelled." );
            // Pop context on early exit
            ContextManager::getInstance( ).popContext( );
            return;
        }
    }

    manager.run( );

    // Restore original screen state with all scrollback intact
    restoreScreenState( );

    jumperlessConfig.top_oled.show_in_terminal = showOledInTerminal;

    // Pop context - cleanup callback will be called automatically
    ContextManager::getInstance( ).popContext( );
}

// Returns selected .py path or "" if user quit. Exits file manager when done.
String pickPythonScriptFromClickMenu( ) {
    ContextEntry ctx( ContextType::FILE_MANAGER );
    ctx.onExit = []( void* ) {
        closeAllFiles( );
        ContextManager::getInstance( ).clearTransferPath( );
        if ( oled.oledConnected )
            oled.restoreNormalFont( );
    };

    inClickMenu = true;
    ContextManager::getInstance( ).pushContext( ctx );

    bool showOledInTerminal = jumperlessConfig.top_oled.show_in_terminal;
    jumperlessConfig.top_oled.show_in_terminal = false;

    if ( !safeFileExists( "/python_scripts" ) )
        safeMkdir( "/python_scripts" );
    initializeMicroPythonExamples( );

    saveScreenState( );
    // Heap-allocate: the click-menu launch path (clickMenu → getMenuSelection
    // → doMenuAction → runApp → here) is the deepest FM entry point and is
    // exactly the one that smashed Core 1's stack. Keep it off Core 0's stack.
    std::unique_ptr<FileManager> managerPtr( new FileManager( ) );
    FileManager& manager = *managerPtr;
    manager.setFromClickMenu( true );
    manager.initInteractiveMode( );
    manager.clearScreen( );
    manager.hideCursor( );

    if ( FatFS.exists( "/python_scripts" ) )
        manager.changeDirectory( "/python_scripts" );

    String selectedPath;
    if ( manager.getCurrentPath( ) != "[NO_FS]" ) {
        manager.run( );
        if ( manager.getShouldExitForScript( ) ) {
            selectedPath = manager.getPendingScriptPath( );
        }
    }

    restoreScreenState( );
    jumperlessConfig.top_oled.show_in_terminal = showOledInTerminal;
    ContextManager::getInstance( ).popContext( );
    // Session over: leave menu-render mode and put the wires back. (The FM
    // run loop no longer clears inClickMenu mid-session - it's owned here.)
    inClickMenu = false;
    b.clear( );
    showLEDsCore2 = -1;

    return selectedPath;
}

void eKiloApp( ) {
    changeTerminalColor( FileColors::HEADER, true );
    Serial.println( "\n  eKilo Text Editor" );
    Serial.println( "   Full-featured terminal text editor with syntax highlighting" );
    Serial.println( "   Perfect for editing MicroPython scripts and config files" );
    changeTerminalColor( FileColors::STATUS, true );
    Serial.println( "\n   Press Enter to launch eKilo Editor..." );
    changeTerminalColor( -1, true );

    // Wait for user to press enter to break the input loop
    while ( Serial.available( ) == 0 ) {
        delayMicroseconds( 100 );
    }
    // Clear the enter keypress
    while ( Serial.available( ) > 0 ) {
        Serial.read( );
    }

    // Save current screen state and switch to alternate screen buffer
    saveScreenState( );

    launchEkiloStandalone( nullptr );

    // Restore original screen state with all scrollback intact
    restoreScreenState( );

    // Windows-specific: Add extra delay to ensure proper display
    Serial.flush( );
    delay( 10 );
}

// Unified eKilo launcher that handles both standalone and REPL modes
// Uses ContextManager for proper navigation and zero-copy data transfer via file paths
String launchEkilo( const char* filename, bool replMode ) {
    // Store if we're called from file manager or standalone
    static bool calledFromFileManager = false;
    calledFromFileManager = ( filename != nullptr );

    String finalFilename = "";
    if ( filename ) {
        finalFilename = String( filename );
    } else if ( replMode ) {
        // Auto-generate filename using the same logic as ScriptHistory
        finalFilename = generateNextScriptName( );
    }

    // Push EKILO_EDITOR context onto the stack
    ContextEntry ctx( ContextType::EKILO_EDITOR );
    ctx.onExit = []( void* ) {
        // Cleanup callback - close any open files
        closeAllFiles( );
        // NOTE: Do NOT clear transfer path here - the caller (Python REPL) needs to read it
        // The caller is responsible for clearing the transfer path after reading
    };
    // Store filename in transfer path for child contexts (Python REPL)
    if ( finalFilename.length( ) > 0 ) {
        ContextManager::getInstance( ).setTransferPath( finalFilename.c_str( ) );
    }
    ContextManager::getInstance( ).pushContext( ctx );

    // Clear any pending serial data
    while ( Serial.available( ) > 0 ) {
        Serial.read( );
    }

    // Display header message
    changeTerminalColor( FileColors::HEADER, false );
    if ( finalFilename.length( ) > 0 ) {
        if ( replMode ) {
            Serial.println( "Opening " + finalFilename + " in eKilo editor..." );
        } else {
            Serial.println( "\n\n\r⍺ Opening " + finalFilename + " in eKilo editor..." );
        }
    } else {
        if ( replMode ) {
            Serial.println( "Creating new file in eKilo editor..." );
        } else {
            Serial.println( "\n\n\r⍺ Starting eKilo editor..." );
        }
    }
    Serial.print( "\x1b[0m" ); // Reset colors

    // Flush any pending serial data before launching editor
    Serial.flush( );

    // Brief pause to let user see the message
    delayMicroseconds( 100 );

    // Screen management based on mode
    if ( replMode ) {
        // Save current screen state and switch to alternate screen buffer
        saveScreenState( );
    } else {
        // Clear screen and prepare for full-screen editor
        Serial.print( "\x1b[2J\x1b[H" );
    }

    // ========== MEMORY CLEANUP BEFORE EDITOR ==========
    // Force garbage collection to free memory and reduce fragmentation
    // This is critical when launching editor from Python REPL context
    if ( isMicroPythonInitialized( ) ) {
        forceGarbageCollection( );
    }

    // Give system a moment to stabilize after GC
    yield( );
    delay( 5 );

    // ========== UNIFIED EDITOR CALL ==========
    // Use ekilo_run() - the single entry point for all editor operations
    const char* fileToOpen = finalFilename.length( ) > 0 ? finalFilename.c_str( ) : nullptr;

    bool success = ekilo_run( fileToOpen );

    // Get result from ContextManager (zero-copy)
    const EkiloResult* result = ekilo_get_result( );
    bool fileSaved = result && result->saved;

    // Store saved path for later use
    String savedFilePath = "";
    if ( result && result->saved_path[ 0 ] != '\0' ) {
        savedFilePath = String( result->saved_path );
    }

    // Only launch REPL if Ctrl+P was pressed AND file is a .py file
    bool shouldLaunchREPL = false;
    if ( result && result->launch_repl ) {
        // Check if file is a Python file
        bool isPythonFile = savedFilePath.endsWith( ".py" ) || savedFilePath.endsWith( ".PY" );
        if ( isPythonFile ) {
            shouldLaunchREPL = true;
        } else {
            // Not a Python file - warn user and don't launch REPL
            Serial.println( "\n\r[!] Ctrl+P only launches Python REPL for .py files" );
            Serial.println( "[!] Saved file, returning to previous screen..." );
            delay( 1500 ); // Give user time to read message
        }
    }

    // Screen restoration based on mode
    if ( replMode ) {
        // Restore original screen state with all scrollback intact
        restoreScreenState( );

        // Windows-specific: Add extra delay and clear to ensure proper display
        Serial.flush( );
        delay( 10 );
    }

    if ( shouldLaunchREPL ) {
        // Clear screen and launch MicroPython REPL
        Serial.print( "\x1b[2J\x1b[H" );

        // Launch MicroPython REPL with file path from result (zero-copy)
        if ( savedFilePath.length( ) > 0 ) {
            enterMicroPythonREPLWithFile( &Serial, savedFilePath );
        } else {
            enterMicroPythonREPL( &Serial );
        }

        // After REPL exits, pop context and return
        ContextManager::getInstance( ).popContext( );
        ekilo_clear_result( ); // Clean up result data

        // Clear screen before returning - Python REPL left garbage on screen
        Serial.print( "\x1b[2J\x1b[H" );
        Serial.flush( );

        // Return marker to signal that REPL was already launched
        // This prevents the caller from trying to load the file again
        return "[REPL_LAUNCHED]";
    }

    // Editor has exited normally - clean up and prepare for return
    // Clear screen immediately to prepare for next interface
    Serial.print( "\x1b[2J\x1b[H" );

    // Brief status message
    changeTerminalColor( FileColors::STATUS, false );
    if ( success && fileSaved ) {
        if ( savedFilePath.length( ) > 0 ) {
            Serial.println( "☺ File saved as " + savedFilePath );
        } else {
            Serial.println( "☺ Editor session completed successfully" );
        }
        if ( calledFromFileManager ) {
            Serial.println( "⌘ Returning to file manager..." );
        } else {
            Serial.println( "⌘ Returning to main menu..." );
        }
    } else {
        Serial.println( "☹ Editor session ended" );
        if ( calledFromFileManager ) {
            Serial.println( "⌘ Returning to file manager..." );
        } else {
            Serial.println( "⌘ Returning to main menu..." );
        }
    }
    changeTerminalColor( -1, false ); // Reset colors

    // Different handling based on how we were called
    if ( calledFromFileManager ) {
        // Called from file manager - just set flag, don't exit to main menu
        delayMicroseconds( 100 ); // Give user time to see the message
    } else {
        // Called standalone (from Apps menu) - wait for user input
        Serial.println( "Press any key to continue..." );
        while ( Serial.available( ) == 0 ) {
            delayMicroseconds( 100 );
        }
        while ( Serial.available( ) > 0 ) {
            Serial.read( );
        }
    }

    // Clear screen one more time to ensure clean return
    Serial.print( "\x1b[2J\x1b[H" );

    // Restore interactive mode after the editor (raw input).
    setTerminalLineBuffering( true );

    // Pop EKILO_EDITOR context - cleanup callback will be called
    ContextManager::getInstance( ).popContext( );
    ekilo_clear_result( ); // Clean up result data

    // Transfer path is set in ContextManager for zero-copy access
    // Return empty string so callers use the transfer path mechanism
    // Callers should check hasTransferPath() and read from getTransferPath()
    return "";
}

// Legacy wrapper for standalone mode (maintains backward compatibility)
void launchEkiloStandalone( const char* filename ) {
    launchEkilo( filename, false );
}

// Legacy wrapper for REPL mode (maintains backward compatibility)
String launchEkiloREPL( const char* filename ) {
    return launchEkilo( filename, true );
}

// Helper function to generate next script name
String generateNextScriptName( ) {
    String scripts_dir = "/python_scripts";

    // Create python_scripts directory if it doesn't exist
    safeMkdir( scripts_dir.c_str( ) );

    // Find next available script number
    int next_script_number = 1;
    for ( int i = 1; i <= 100; i++ ) {
        String test_script = scripts_dir + "/script_" + String( i ) + ".py";
        if ( safeFileExists( test_script.c_str( ) ) ) {
            next_script_number = i + 1;
        } else {
            break; // Found first gap, use it
        }
    }

    return scripts_dir + "/script_" + String( next_script_number ) + ".py";
}

bool FileManager::createDirectory( const String& dirname ) {
    String fullPath = getFullPath( currentPath, dirname );

    if ( safeFileExists( fullPath.c_str( ) ) ) {
        outputToArea( "Directory already exists: " + dirname, FileColors::ERROR );
        return false;
    }

    if ( safeMkdir( fullPath.c_str( ) ) ) {
        outputToArea( "Created directory: " + dirname, FileColors::STATUS );
        refreshListing( );
        return true;
    } else {
        outputToArea( "Failed to create directory: " + dirname, FileColors::ERROR );
        return false;
    }
}

String FileManager::promptForFilename( const String& prompt ) {
    return promptInOutputArea( prompt );
}

// REPL mode version - returns content if file was saved
// Uses context system for proper navigation and cleanup
String filesystemAppPythonScriptsREPL( ) {
    // Push FILE_MANAGER context onto the stack
    ContextEntry ctx( ContextType::FILE_MANAGER );
    ctx.onExit = []( void* ) {
        // Cleanup callback - close any open files
        closeAllFiles( );
        // NOTE: Do NOT clear transfer path or SharedBuffer here
        // The caller (Python REPL) needs to read them after we return
    };
    ContextManager::getInstance( ).pushContext( ctx );

    // Clear screen for file manager interface
    Serial.print( "\x1b[2J\x1b[H" );
    Serial.flush( );
    saveScreenState( );

    // Heap-allocate to keep the deep FM session off Core 0's small stack
    // (see core1_separate_stack in main.cpp).
    std::unique_ptr<FileManager> managerPtr( new FileManager( ) );
    FileManager& manager = *managerPtr;
    // Set REPL mode so the manager knows to use launchEkiloREPL
    manager.setREPLMode( true );

    // Use normal interactive mode instead of special REPL mode
    manager.initInteractiveMode( );

    // Create python_scripts directory if it doesn't exist
    safeMkdir( "/python_scripts" );

    // Automatically create MicroPython examples if needed
    initializeMicroPythonExamples( );

    // Navigate to python_scripts directory
    if ( safeFileExists( "/python_scripts" ) ) {
        manager.changeDirectory( "/python_scripts" );
    }

    manager.run( );

    // Close any open files before exiting REPL mode file manager
    closeAllFiles( );

    // Restore normal font if we were using small fonts
    if ( oled.oledConnected ) {
        oled.restoreNormalFont( );
    }

    // If we exited because REPL was launched, don't restore screen state
    // as the REPL has already modified the display
    if ( manager.getShouldExitForREPL( ) ) {
        // Clear screen completely for clean return to main menu
        Serial.print( "\x1b[2J\x1b[H" );
        Serial.flush( );
        // Resync the app to the user's line-buffering config for the main menu.
        pushLineBufferingToApp( );
    } else {
        // Normal exit - restore screen state
        restoreScreenState( );
    }
    manager.setREPLMode( false );

    // Pop FILE_MANAGER context - this preserves the transfer path for caller
    ContextManager::getInstance( ).popContext( );

    // Return empty string - caller should use hasTransferPath() to check for file
    // The transfer path (if set by eKilo) is preserved for the caller to read
    return "";
}

String FileManager::getLastSavedFileContent( ) {
    return lastOpenedFileContent;
}

//==============================================================================
// Global Utility Functions for External Use (e.g., USB filesystem)
//==============================================================================

FileType getFileTypeFromFilename( const String& filename ) {
    String lower = filename;
    lower.toLowerCase( );

    if ( lower.endsWith( ".py" ) || lower.endsWith( ".pyw" ) || lower.endsWith( ".pyi" ) ) {
        return FILE_TYPE_PYTHON;
    } else if ( lower.endsWith( ".json" ) || lower.endsWith( ".yaml" ) || lower.endsWith( ".yml" ) ) {
        return FILE_TYPE_JSON; // YAML (e.g. slot*.yaml) is JSON-ish structured config
    } else if ( lower.endsWith( ".cfg" ) || lower.endsWith( ".conf" ) || lower.startsWith( "config" ) || filename == "config.txt" ) {
        return FILE_TYPE_CONFIG;
    } else if ( lower.startsWith( "nodefileslot" ) && lower.endsWith( ".txt" ) ) {
        return FILE_TYPE_NODEFILES;
    } else if ( lower.startsWith( "netcolorsslot" ) && lower.endsWith( ".txt" ) ) {
        return FILE_TYPE_COLORS;
    } else if ( lower.endsWith( ".txt" ) || lower.endsWith( ".md" ) || lower.endsWith( ".readme" ) ) {
        return FILE_TYPE_TEXT;
    }
    return FILE_TYPE_UNKNOWN;
}

String getFileIconFromType( FileType type ) {
    switch ( type ) {
    case FILE_TYPE_DIRECTORY:
        return "⌘";
    case FILE_TYPE_PYTHON:
        return "𓆚";
    case FILE_TYPE_TEXT:
        return "⍺";
    case FILE_TYPE_CONFIG:
        return "⚙";
    case FILE_TYPE_JSON:
        return "⟐";
    case FILE_TYPE_NODEFILES:
        return "☊";
    case FILE_TYPE_COLORS:
        return "⎃";
    default:
        return "⍺";
    }
}

String formatFileSizeForUSB( size_t size ) {
    if ( size < 1024 ) {
        return String( size ) + " B";
    } else if ( size < 1024 * 1024 ) {
        return String( size / 1024 ) + " KB";
    } else {
        return String( size / ( 1024 * 1024 ) ) + " MB";
    }
}

//==============================================================================
// Display Configuration Functions
//==============================================================================

int getConfiguredDisplayLines( ) {
    return DEFAULT_DISPLAY_LINES;
}

int getConfiguredEditorLines( ) {
    // Editor gets slightly fewer lines due to headers and footers
    return DEFAULT_DISPLAY_LINES - 1;
}

//==============================================================================
// Filesystem Utility Functions
//==============================================================================

// Recursively delete all contents of a directory
// Note: fs_mutex is acquired by caller (provisionMicroPythonExamples)
bool deleteDirectoryContents( const String& path ) {
    Dir dir = FatFS.openDir( path );
    bool success = true;

    while ( dir.next( ) ) {
        String fileName = dir.fileName( );
        String fullPath;

        // Build full path
        if ( path == "/" ) {
            fullPath = "/" + fileName;
        } else {
            fullPath = path + "/" + fileName;
        }

        if ( dir.isDirectory( ) ) {
            // Recursively delete subdirectory contents first
            if ( !deleteDirectoryContents( fullPath ) ) {
                success = false;
                Serial.print( "Failed to delete contents of directory: " );
                Serial.println( fullPath );
                continue;
            }

            // Then delete the empty directory
            if ( !FatFS.rmdir( fullPath.c_str( ) ) ) { // TODO: create safeRmdir
                success = false;
                Serial.print( "Failed to remove directory: " );
                Serial.println( fullPath );
            } else {
                Serial.print( "Removed directory: " );
                Serial.println( fullPath );
            }
        } else {
            // Delete file
            if ( !safeFileDelete( fullPath.c_str( ) ) ) {
                success = false;
                Serial.print( "Failed to delete file: " );
                Serial.println( fullPath );
            } else {
                Serial.print( "Deleted file: " );
                Serial.println( fullPath );
            }
        }

        // Add small delay to prevent system overload
        delayMicroseconds( 100 );

        // Process any pending serial data to keep system responsive
        if ( Serial.available( ) ) {
            while ( Serial.available( ) ) {
                Serial.read( );
            }
        }
    }

    return success;
}

//==============================================================================
// Helper Functions for MicroPython Examples
//==============================================================================

// Simple version for startup - no message queue
bool writeStringToFileSimple( const char* filename, const char* content ) {
    if ( !filename || !content ) {
        return false;
    }

    size_t contentLength = strlen( content );
    if ( contentLength == 0 ) {
        return false;
    }

    // Pause Core2 for entire flash operation (FTL can trigger flash writes mid-write)
    bool was_paused = pauseCore2ForFlash( 100 );

    // Bounded acquire (same pattern as eepromCommitSafe): pauseCore2 is
    // already raised, so an unbounded blocking acquire here would park core 1
    // (LEDs, crossbar sends) for as long as the mutex holder takes - and the
    // holder can be a path that itself waits out core-1 flags. 5s is far past
    // any legitimate hold; give up rather than freeze the board.
    if ( !fs_mutex_acquire_timeout_ms( 5000 ) ) {
        unpauseCore2ForFlash( was_paused );
        return false;
    }

    File file = FatFS.open( filename, "w" );
    if ( !file ) {
        fs_mutex_release( );
        unpauseCore2ForFlash( was_paused );
        return false;
    }

    size_t totalBytesWritten = 0;
    const size_t chunkSize = 256; // FatFS FTL uses 512-byte sectors

    while ( totalBytesWritten < contentLength ) {
        size_t bytesToWrite = min( chunkSize, contentLength - totalBytesWritten );
        size_t bytesWritten = file.write( (const uint8_t*)( content + totalBytesWritten ), bytesToWrite );

        if ( bytesWritten != bytesToWrite ) {
            file.close( );
            fs_mutex_release( );
            unpauseCore2ForFlash( was_paused );
            return false;
        }

        totalBytesWritten += bytesWritten;
    }

    // close() internally calls f_sync() -- no need for explicit flush()
    file.close( );

    fs_mutex_release( );
    unpauseCore2ForFlash( was_paused );
    return true;
}

// Interactive version with message queue
bool writeStringToFile( const char* filename, const char* content ) {
    if ( !filename || !content ) {
        addFilesystemMessage( "ERROR: Invalid filename or content", 196 );
        return false;
    }

    size_t contentLength = strlen( content );
    if ( contentLength == 0 ) {
        addFilesystemMessage( "ERROR: Empty content for " + String( filename ), 196 );
        return false;
    }

    // This writer bypasses safeFileOpen, so enforce the USB MSC guard here too
    if ( usbMountedByHost ) {
        addFilesystemMessage( "Write to " + String( filename ) + " refused: USB host has the filesystem mounted; eject first", 196 );
        return false;
    }

    // Check memory availability and file size limits
    size_t freeHeap = rp2040.getFreeHeap( );
    if ( freeHeap < contentLength + 2048 ) { // Need content size + 2KB overhead
        addFilesystemMessage( "ERROR: Not enough memory to write file (" + String( contentLength / 1024 ) + "KB needed)", 196 );
        return false;
    }

    if ( contentLength > 64 * 1024 ) { // Limit individual files to 64KB
        addFilesystemMessage( "ERROR: File too large (" + String( contentLength / 1024 ) + "KB, max 64KB)", 196 );
        return false;
    }

    // Pause Core2 for entire flash operation (FTL can trigger flash writes mid-write)
    bool was_paused = pauseCore2ForFlash( 100 );

    // Bounded acquire with pauseCore2 raised - see writeStringToFileSimple.
    if ( !fs_mutex_acquire_timeout_ms( 5000 ) ) {
        unpauseCore2ForFlash( was_paused );
        addFilesystemMessage( "ERROR: filesystem busy, could not write " + String( filename ), 196 );
        return false;
    }

    File file = FatFS.open( filename, "w" );
    if ( !file ) {
        fs_mutex_release( );
        unpauseCore2ForFlash( was_paused );
        addFilesystemMessage( "ERROR: Cannot open " + String( filename ) + " for writing", 196 );
        return false;
    }

    size_t totalBytesWritten = 0;
    const size_t chunkSize = 256; // FatFS FTL uses 512-byte sectors

    while ( totalBytesWritten < contentLength ) {
        size_t bytesToWrite = min( chunkSize, contentLength - totalBytesWritten );
        size_t bytesWritten = file.write( (const uint8_t*)( content + totalBytesWritten ), bytesToWrite );

        if ( bytesWritten != bytesToWrite ) {
            addFilesystemMessage( "ERROR: Write failed at byte " + String( totalBytesWritten ), 196 );
            file.close( );
            fs_mutex_release( );
            unpauseCore2ForFlash( was_paused );
            return false;
        }

        totalBytesWritten += bytesWritten;
    }

    // close() internally calls f_sync() -- no need for explicit flush()
    file.close( );

    fs_mutex_release( );
    unpauseCore2ForFlash( was_paused );

    return true;
}

//==============================================================================
// Initialize MicroPython Examples Function - Conditional Compilation
//==============================================================================

// Single source of truth for the provisioned examples: the provisioner
// (initializeMicroPythonExamples) writes these and verifyMicroPythonExamples
// checks the same list. (The verifier used to keep its own copy, which had
// drifted out of sync.)
struct ExampleInfo {
    const char*     path;
    const char*     content;
    const char*     name;
    const uint32_t* knownHashes; // [0] = current firmware hash, [1..] = older versions
    int             hashCount;
};

static const ExampleInfo examples[] = {
#ifdef INCLUDE_JUMPERLESS_MODULE
        { "/python_scripts/lib/jumperless.py", JUMPERLESS_MODULE_PY, "jumperless.py", JUMPERLESS_MODULE_PY_HASHES, JUMPERLESS_MODULE_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_JUMPERLESS_STUB
        { "/python_scripts/lib/jumperless.pyi", JUMPERLESS_STUB_PYI, "jumperless.pyi", JUMPERLESS_STUB_PYI_HASHES, JUMPERLESS_STUB_PYI_HASH_COUNT },
#endif
#ifdef INCLUDE_VIPER_INIT
        { "/python_scripts/lib/viper_init.py", VIPER_INIT_PY, "viper_init.py", VIPER_INIT_PY_HASHES, VIPER_INIT_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_DAC_BASICS
        { "/python_scripts/examples/dac_basics.py", DAC_BASICS_PY, "dac_basics.py", DAC_BASICS_PY_HASHES, DAC_BASICS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_ADC_BASICS
        { "/python_scripts/examples/adc_basics.py", ADC_BASICS_PY, "adc_basics.py", ADC_BASICS_PY_HASHES, ADC_BASICS_PY_HASH_COUNT },
#endif
// V5 only: there is no USB audio hardware on OG, and the example's own loop
// waits forever for a re-enumeration that can never happen there.
// micropythonExamples.h is generated, so the board gate lives here.
#if defined(INCLUDE_USB_AUDIO_MIC) && USB_AUDIO_ENABLE
        { "/python_scripts/examples/usb_audio_mic.py", USB_AUDIO_MIC_PY, "usb_audio_mic.py", USB_AUDIO_MIC_PY_HASHES, USB_AUDIO_MIC_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_ASYNC_READ
        { "/python_scripts/examples/async_read.py", ASYNC_READ_PY, "async_read.py", ASYNC_READ_PY_HASHES, ASYNC_READ_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_GPIO_BASICS
        { "/python_scripts/examples/gpio_basics.py", GPIO_BASICS_PY, "gpio_basics.py", GPIO_BASICS_PY_HASHES, GPIO_BASICS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_MACHINE_PIN_BASICS
        { "/python_scripts/examples/machine_pin_basics.py", MACHINE_PIN_BASICS_PY, "machine_pin_basics.py", MACHINE_PIN_BASICS_PY_HASHES, MACHINE_PIN_BASICS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_PIN_IRQ_REACTION_GAME
        { "/python_scripts/examples/pin_irq_reaction_game.py", PIN_IRQ_REACTION_GAME_PY, "pin_irq_reaction_game.py", PIN_IRQ_REACTION_GAME_PY_HASHES, PIN_IRQ_REACTION_GAME_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_PIN_IRQ_FREQ_COUNTER
        { "/python_scripts/examples/pin_irq_freq_counter.py", PIN_IRQ_FREQ_COUNTER_PY, "pin_irq_freq_counter.py", PIN_IRQ_FREQ_COUNTER_PY_HASHES, PIN_IRQ_FREQ_COUNTER_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_PIN_IRQ_BASICS
        { "/python_scripts/examples/pin_irq_basics.py", PIN_IRQ_BASICS_PY, "pin_irq_basics.py", PIN_IRQ_BASICS_PY_HASHES, PIN_IRQ_BASICS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_FILE_IO_BASICS
        { "/python_scripts/examples/file_io_basics.py", FILE_IO_BASICS_PY, "file_io_basics.py", FILE_IO_BASICS_PY_HASHES, FILE_IO_BASICS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_NODE_CONNECTIONS
        { "/python_scripts/examples/node_connections.py", NODE_CONNECTIONS_PY, "node_connections.py", NODE_CONNECTIONS_PY_HASHES, NODE_CONNECTIONS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_README
        { "/python_scripts/examples/README.md", README_MD, "README.md", README_MD_HASHES, README_MD_HASH_COUNT },
#endif
#ifdef INCLUDE_TEST_RUNNER
        { "/python_scripts/examples/test_examples.py", TEST_RUNNER_PY, "test_examples.py", TEST_RUNNER_PY_HASHES, TEST_RUNNER_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_LED_BRIGHTNESS_CONTROL
        { "/python_scripts/examples/led_brightness_control.py", LED_BRIGHTNESS_CONTROL_PY, "led_brightness_control.py", LED_BRIGHTNESS_CONTROL_PY_HASHES, LED_BRIGHTNESS_CONTROL_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_VOLTAGE_MONITOR
        { "/python_scripts/examples/voltage_monitor.py", VOLTAGE_MONITOR_PY, "voltage_monitor.py", VOLTAGE_MONITOR_PY_HASHES, VOLTAGE_MONITOR_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_STYLOPHONE
        { "/python_scripts/examples/stylophone.py", STYLOPHONE_PY, "stylophone.py", STYLOPHONE_PY_HASHES, STYLOPHONE_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_UART_BASICS
        { "/python_scripts/examples/uart_basics.py", UART_BASICS_PY, "uart_basics.py", UART_BASICS_PY_HASHES, UART_BASICS_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_UART_LOOPBACK
        { "/python_scripts/examples/uart_loopback.py", UART_LOOPBACK_PY, "uart_loopback.py", UART_LOOPBACK_PY_HASHES, UART_LOOPBACK_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_INTERACTION_DEMO
        { "/python_scripts/examples/interaction_demo.py", INTERACTION_DEMO_PY, "interaction_demo.py", INTERACTION_DEMO_PY_HASHES, INTERACTION_DEMO_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_TEST_NEOPIXEL
        { "/python_scripts/examples/test_neopixel.py", TEST_NEOPIXEL_PY, "test_neopixel.py", TEST_NEOPIXEL_PY_HASHES, TEST_NEOPIXEL_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_FAKE_GPIO
        { "/python_scripts/examples/fake_gpio.py", FAKE_GPIO_PY, "fake_gpio.py", FAKE_GPIO_PY_HASHES, FAKE_GPIO_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_SIMPLE_FAKE_GPIO_TEST
        { "/python_scripts/examples/simple_fake_gpio_test.py", SIMPLE_FAKE_GPIO_TEST_PY, "simple_fake_gpio_test.py", SIMPLE_FAKE_GPIO_TEST_PY_HASHES, SIMPLE_FAKE_GPIO_TEST_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_OLEDGUI
        // oledgui is a library module - provision to /lib (which is ahead of
        // /examples on sys.path), so `import oledgui` from any script gets it.
        { "/python_scripts/lib/oledgui.py", OLEDGUI_PY, "oledgui.py", OLEDGUI_PY_HASHES, OLEDGUI_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_OLED_STATS_PAGE
        { "/python_scripts/examples/oled_stats_page.py", OLED_STATS_PAGE_PY, "oled_stats_page.py", OLED_STATS_PAGE_PY_HASHES, OLED_STATS_PAGE_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_OLED_LAYOUT_EDITOR
        { "/python_scripts/examples/oled_layout_editor.py", OLED_LAYOUT_EDITOR_PY, "oled_layout_editor.py", OLED_LAYOUT_EDITOR_PY_HASHES, OLED_LAYOUT_EDITOR_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_VIPERIDE_REINIT
        { "/python_scripts/examples/viperide_reinit.py", VIPERIDE_REINIT_PY, "viperide_reinit.py", VIPERIDE_REINIT_PY_HASHES, VIPERIDE_REINIT_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_OSCILLOSCOPE
        { "/python_scripts/examples/oscilloscope.py", OSCILLOSCOPE_PY, "oscilloscope.py", OSCILLOSCOPE_PY_HASHES, OSCILLOSCOPE_PY_HASH_COUNT },
#endif
#ifdef INCLUDE_TEST_OLED_FEATURES
        { "/python_scripts/examples/test_oled_features.py", TEST_OLED_FEATURES_PY, "test_oled_features.py", TEST_OLED_FEATURES_PY_HASHES, TEST_OLED_FEATURES_PY_HASH_COUNT },
#endif
// NOTE: was nested inside INCLUDE_TEST_OLED_FEATURES - disabling that would
// have silently stopped provisioning excel_listener too
#ifdef INCLUDE_EXCEL_LISTENER
        { "/python_scripts/examples/excel_listener.py", EXCEL_LISTENER_PY, "excel_listener.py", EXCEL_LISTENER_PY_HASHES, EXCEL_LISTENER_PY_HASH_COUNT },
#endif
    };

static const int totalExamples = sizeof( examples ) / sizeof( examples[ 0 ] );

void initializeMicroPythonExamples( bool forceInitialization ) {
    // Safety check - don't do anything if Serial is not available
    // if ( !Serial ) {
    //     return;
    // }

    // If no examples are enabled, exit early
    if ( totalExamples == 0 ) {
        if ( globalFileManager != nullptr ) {
            // globalFileManager->outputToArea("[INIT] No examples enabled at compile time", 155);
        } else {
            addFilesystemMessage( "[INIT] No examples enabled at compile time", 155 );
        }
        return;
    }

    // Quick check - if all enabled files exist and not forced, do nothing silently
    if ( !forceInitialization ) {
        bool allFilesExist = true;
        for ( int i = 0; i < totalExamples; i++ ) {
            if ( !FatFS.exists( examples[ i ].path ) ) {
                allFilesExist = false;
                break;
            }
        }

        // If all enabled files exist and not forced, exit silently
        if ( allFilesExist && FatFS.exists( "/python_scripts" ) && FatFS.exists( "/python_scripts/examples" ) && FatFS.exists( "/python_scripts/lib" ) ) {
            return;
        }
    }

    // Only provide feedback if we're actually doing work
    bool useOutputArea = ( globalFileManager != nullptr );

    String initAction = forceInitialization ? "[FORCE INIT]" : "[INIT]";
    String initMsg = initAction + " Initializing " + String( totalExamples ) + " MicroPython examples...";
    if ( useOutputArea ) {
        globalFileManager->outputToArea( initMsg, 155 );
    } else {
        addFilesystemMessage( initMsg, 155 );
    }

    // Clean up old location if it exists (migration from previous version)
    if ( safeFileExists( "/micropython_examples" ) ) {
        if ( useOutputArea ) {
            globalFileManager->outputToArea( "[MIGRATION] Cleaning up old location...", 155 );
        } else {
            addFilesystemMessage( "[MIGRATION] Cleaning up old location...", 155 );
        }

        // Try to remove files in the old directory first
        Dir oldDir = FatFS.openDir( "/micropython_examples" );
        while ( oldDir.next( ) ) {
            String fileName = oldDir.fileName( );
            String fullPath = "/micropython_examples/" + fileName;
            safeFileDelete( fullPath.c_str( ) );
        }

        // Then try to remove the directory itself
        FatFS.rmdir( "/micropython_examples" ); // TODO: create safeRmdir

        if ( useOutputArea ) {
            globalFileManager->outputToArea( "Old location cleaned up", 155 );
        } else {
            addFilesystemMessage( "Old location cleaned up", 155 );
        }
    }

    // First ensure python_scripts directory exists
    if ( !safeFileExists( "/python_scripts" ) ) {
        if ( useOutputArea ) {
            globalFileManager->outputToArea( "[CREATE] Creating /python_scripts directory...", 155 );
        } else {
            addFilesystemMessage( "[CREATE] Creating /python_scripts directory...", 155 );
        }

        if ( !safeMkdir( "/python_scripts" ) ) {
            if ( useOutputArea ) {
                globalFileManager->outputToArea( "ERROR: Failed to create /python_scripts", 196 );
            } else {
                addFilesystemMessage( "ERROR: Failed to create /python_scripts", 196 );
            }
            return;
        }

        if ( useOutputArea ) {
            globalFileManager->outputToArea( "Created /python_scripts directory", 155 );
        } else {
            addFilesystemMessage( "Created /python_scripts directory", 155 );
        }
    }

    // Ensure examples directory exists inside python_scripts
    if ( !safeFileExists( "/python_scripts/examples" ) ) {
        if ( useOutputArea ) {
            globalFileManager->outputToArea( "[CREATE] Creating examples directory...", 155 );
        } else {
            addFilesystemMessage( "[CREATE] Creating examples directory...", 155 );
        }

        // Create the examples directory
        if ( !safeMkdir( "/python_scripts/examples" ) ) {
            if ( useOutputArea ) {
                globalFileManager->outputToArea( "ERROR: Failed to create examples directory", 196 );
            } else {
                addFilesystemMessage( "ERROR: Failed to create examples directory", 196 );
            }
            return;
        }

        if ( useOutputArea ) {
            globalFileManager->outputToArea( "Created examples directory", 155 );
        } else {
            addFilesystemMessage( "Created examples directory", 155 );
        }
    }

    // Ensure lib directory exists inside python_scripts for the jumperless module
    if ( !safeFileExists( "/python_scripts/lib" ) ) {
        if ( useOutputArea ) {
            globalFileManager->outputToArea( "[CREATE] Creating lib directory...", 155 );
        } else {
            addFilesystemMessage( "[CREATE] Creating lib directory...", 155 );
        }

        // Create the lib directory
        if ( !safeMkdir( "/python_scripts/lib" ) ) {
            if ( useOutputArea ) {
                globalFileManager->outputToArea( "ERROR: Failed to create lib directory", 196 );
            } else {
                addFilesystemMessage( "ERROR: Failed to create lib directory", 196 );
            }
            return;
        }

        if ( useOutputArea ) {
            globalFileManager->outputToArea( "Created lib directory", 155 );
        } else {
            addFilesystemMessage( "Created lib directory", 155 );
        }
    }

    // Check available memory before creating or updating files
    size_t freeHeap = rp2040.getFreeHeap( );
    if ( freeHeap < 20000 ) { // Require at least 20KB free heap
        String errorMsg = "ERROR: Low memory (" + String( freeHeap ) + " bytes), skipping file creation";
        if ( useOutputArea ) {
            globalFileManager->outputToArea( errorMsg, 196 );
        } else {
            addFilesystemMessage( errorMsg, 196 );
        }
        return;
    }

    if ( useOutputArea ) {
        globalFileManager->outputToArea( "[FILES] Checking example files...", 155 );
    } else {
        addFilesystemMessage( "[FILES] Checking example files...", 155 );
    }

    int filesCreated = 0;
    int filesSkipped = 0;

    // Safety timeout - don't spend more than 30 seconds creating/updating files
    unsigned long startTime = millis( );
    const unsigned long maxTime = 30000;

    for ( int i = 0; i < totalExamples; i++ ) {
        if ( millis( ) - startTime > maxTime ) {
            String timeoutMsg = "TIMEOUT: File creation aborted after 30s";
            if ( useOutputArea ) {
                globalFileManager->outputToArea( timeoutMsg, 196 );
            } else {
                addFilesystemMessage( timeoutMsg, 196 );
            }
            break;
        }

        const char*     canonicalPath = examples[ i ].path;
        const char*     logicalName   = examples[ i ].name;
        const uint32_t* knownHashes   = examples[ i ].knownHashes;
        int             hashCount     = examples[ i ].hashCount;
        uint32_t        currentHash   = ( hashCount > 0 ) ? knownHashes[ 0 ] : 0;

        bool canonicalExists = safeFileExists( canonicalPath );

        if ( !canonicalExists ) {
            // File is missing - create it regardless of forced/non-forced
            String progressMsg = "Creating " + String( logicalName );
            if ( useOutputArea ) {
                globalFileManager->outputToArea( progressMsg, 155 );
            } else {
                addFilesystemMessage( progressMsg, 155 );
            }
            if ( writeStringToFile( canonicalPath, examples[ i ].content ) ) {
                filesCreated++;
            } else {
                String errorMsg = "✗ Failed to create " + String( logicalName );
                if ( useOutputArea ) {
                    globalFileManager->outputToArea( errorMsg, 196 );
                } else {
                    addFilesystemMessage( errorMsg, 196 );
                }
            }
            delay( 10 );
            continue;
        }

        if ( !forceInitialization ) {
            // Non-forced boot: file exists, nothing to do
            filesSkipped++;
            continue;
        }

        // Forced (firmware update): 3-way hash check
        //   currentHash   == knownHashes[0]: this build's default
        //   knownHashes[1..]: hashes from previous firmware versions
        uint32_t fileHash = 0;
        bool     hasFileHash = fnv1a32_file( canonicalPath, fileHash );

        if ( hasFileHash && fileHash == currentHash ) {
            // Canonical already matches this firmware's default - nothing to do
            filesSkipped++;
            continue;
        }

        // Check whether the canonical file is an old (unmodified) firmware version
        bool isOldFirmwareVersion = false;
        if ( hasFileHash ) {
            for ( int h = 1; h < hashCount && !isOldFirmwareVersion; ++h ) {
                if ( fileHash == knownHashes[ h ] ) {
                    isOldFirmwareVersion = true;
                }
            }
        }

        if ( isOldFirmwareVersion ) {
            // File is an older firmware default that the user never touched - overwrite in place
            String progressMsg = "Updating " + String( logicalName );
            if ( useOutputArea ) {
                globalFileManager->outputToArea( progressMsg, 155 );
            } else {
                addFilesystemMessage( progressMsg, 155 );
            }
            writeStringToFile( canonicalPath, examples[ i ].content );
            filesCreated++;
            delay( 10 );
            continue;
        }

        // Canonical has a hash that doesn't match any known firmware version - user edited it.
        // Files in a /lib/ folder are always overwritten in place; we never keep an
        // _original* copy for library modules (they must match the firmware).
        if ( strstr( canonicalPath, "/lib/" ) != nullptr ) {
            String progressMsg = "Updating " + String( logicalName );
            if ( useOutputArea ) {
                globalFileManager->outputToArea( progressMsg, 155 );
            } else {
                addFilesystemMessage( progressMsg, 155 );
            }
            writeStringToFile( canonicalPath, examples[ i ].content );
            filesCreated++;
            delay( 10 );
            continue;
        }

        // Check if any _original* variant already stores the current default before writing another.
        bool alreadyHaveDefault = false;
        const int maxVariants = 5;
        for ( int v = 0; v < maxVariants && !alreadyHaveDefault; ++v ) {
            String vPath = buildOriginalVariantPath( canonicalPath, v );
            uint32_t vHash = 0;
            if ( safeFileExists( vPath.c_str( ) ) &&
                 fnv1a32_file( vPath.c_str( ), vHash ) &&
                 vHash == currentHash ) {
                alreadyHaveDefault = true;
            }
        }

        if ( alreadyHaveDefault ) {
            filesSkipped++;
            continue;
        }

        // Write the current firmware default to the next free _original* slot
        String targetPath = buildOriginalVariantPath( canonicalPath, 0 );
        int variantIndex = 0;
        while ( safeFileExists( targetPath.c_str( ) ) && variantIndex < maxVariants ) {
            variantIndex++;
            targetPath = buildOriginalVariantPath( canonicalPath, variantIndex );
        }

        String progressMsg = "Creating original " + String( logicalName );
        if ( useOutputArea ) {
            globalFileManager->outputToArea( progressMsg, 155 );
        } else {
            addFilesystemMessage( progressMsg, 155 );
        }

        if ( writeStringToFile( targetPath.c_str( ), examples[ i ].content ) ) {
            filesCreated++;
        } else {
            String errorMsg = "✗ Failed to create original " + String( logicalName );
            if ( useOutputArea ) {
                globalFileManager->outputToArea( errorMsg, 196 );
            } else {
                addFilesystemMessage( errorMsg, 196 );
            }
        }

        delay( 10 );

        if ( Serial.available( ) ) {
            while ( Serial.available( ) ) { Serial.read( ); }
        }
    }

    // Summary message
    String summary;
    if ( filesCreated > 0 ) {
        summary = "✓ Created " + String( filesCreated ) + " MicroPython example(s), " + String( filesSkipped ) + " unchanged";
    } else {
        summary = "✓ " + String( filesSkipped ) + " examples unchanged";
    }

    // Always use addFilesystemMessage for consistency (it routes to persistent messages when file manager is active)
    addFilesystemMessage( summary, 155 );

    // Clear older messages to prevent memory buildup
    if ( fsMessageCount > 5 ) {
        fsMessageCount = 3; // Keep only the last 3 messages
    }
}

bool verifyMicroPythonExamples( ) {
    // Uses the file-scope examples[] list - single source of truth shared
    // with initializeMicroPythonExamples (this used to be a drifted copy).
    bool allOk = true;

    Serial.println( "Verifying MicroPython examples (hash-based)..." );

    for ( int i = 0; i < totalExamples; i++ ) {
        const char*     canonicalPath = examples[ i ].path;
        const char*     logicalName   = examples[ i ].name;
        const uint32_t* knownHashes   = examples[ i ].knownHashes;
        int             hashCount     = examples[ i ].hashCount;
        uint32_t        currentHash   = ( hashCount > 0 ) ? knownHashes[ 0 ] : 0;

        bool foundAnyCopy    = false;
        bool foundKnownCopy  = false; // matches any firmware version (current or old)
        bool foundCurrentCopy = false; // matches current firmware default
        uint32_t fileHash    = 0;

        auto matchesAnyKnown = [ & ]( uint32_t h ) -> bool {
            for ( int k = 0; k < hashCount; ++k ) {
                if ( h == knownHashes[ k ] ) { return true; }
            }
            return false;
        };

        // Check canonical file
        if ( safeFileExists( canonicalPath ) && fnv1a32_file( canonicalPath, fileHash ) ) {
            foundAnyCopy = true;
            if ( fileHash == currentHash ) { foundCurrentCopy = true; }
            if ( matchesAnyKnown( fileHash ) ) { foundKnownCopy = true; }
        }

        // Check _original* variants
        const int maxVariants = 5;
        for ( int variantIndex = 0; variantIndex < maxVariants && !foundCurrentCopy; ++variantIndex ) {
            String variantPath = buildOriginalVariantPath( canonicalPath, variantIndex );
            if ( safeFileExists( variantPath.c_str( ) ) && fnv1a32_file( variantPath.c_str( ), fileHash ) ) {
                foundAnyCopy = true;
                if ( fileHash == currentHash ) { foundCurrentCopy = true; break; }
                if ( matchesAnyKnown( fileHash ) ) { foundKnownCopy = true; }
            }
        }

        if ( !foundAnyCopy ) {
            Serial.print( "  MISSING: " );
            Serial.println( canonicalPath );
            allOk = false;
        } else if ( !foundCurrentCopy && !foundKnownCopy ) {
            // Only user-modified copies exist
            Serial.print( "  USER-MODIFIED: " );
            Serial.println( logicalName );
        } else if ( !foundCurrentCopy && foundKnownCopy ) {
            // File is an older (unmodified) firmware version
            Serial.print( "  OLD DEFAULT: " );
            Serial.println( logicalName );
        } else {
            Serial.print( "  OK: " );
            Serial.println( logicalName );
        }
    }

    if ( allOk ) {
        Serial.println( "All MicroPython example defaults verified successfully!" );
    } else {
        Serial.println( "Some MicroPython examples are missing defaults or only modified copies exist!" );
    }

    return allOk;
}

// =============================================================================
// Safe File Operations
// =============================================================================
// These functions provide thread-safe file I/O with proper Core2 synchronization.
// All file operations in the codebase should use these functions.

File safeFileOpen( const char* path, const char* mode, uint32_t timeout_ms ) {
    if ( !path || !mode )
        return File( );

    // Write-capable modes: "w", "a", and the '+' variants ("r+" writes too).
    bool write_mode = ( strpbrk( mode, "wa+" ) != nullptr );

    // USB MSC guard: while a host has the disk mounted it caches the FAT, so
    // a firmware write behind its back corrupts the host's view (and ours).
    // Read-only opens are still allowed. Failure signal matches the existing
    // contract: an invalid File() with the fs_mutex NOT held.
    if ( write_mode && usbMountedByHost ) {
        Serial.print( "[FS] write to " );
        Serial.print( path );
        Serial.println( " refused: USB host has the filesystem mounted; eject first" );
        return File( );
    }

    // Acquire filesystem mutex
    if ( timeout_ms == 0 ) {
        fs_mutex_acquire( );
    } else if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        return File( ); // Timeout - couldn't get mutex
    }

    File file = FatFS.open( path, mode );

    // Note: Mutex is NOT released here - caller must call safeFileClose()
    // This keeps the file exclusively locked while in use
    if ( !file ) {
        fs_mutex_release( ); // Release if open failed
    } else if ( write_mode ) {
        // Keep the cache honest: handle-based writes bypass the cache, and
        // "w" truncates at open even if nothing is written. FileCache.h says
        // direct writers must invalidate - this is the chokepoint for it.
        fileCacheInvalidate( path );
    }

    return file;
}

void safeFileClose( File& file, bool was_write_mode ) {
    if ( !file ) {
        return;
    }

    if ( was_write_mode ) {
        // Pause Core2 before close (which internally syncs)
        bool was_paused = pauseCore2ForFlash( 100 );
        // Skip explicit flush() - it does BOTH f_sync AND _fs->sync (full FS sync)
        // FatFS f_close() internally calls f_sync() which is sufficient for file data
        // The _fs->sync() in flush() syncs the entire filesystem which is overkill
        file.close( );
        unpauseCore2ForFlash( was_paused );
    } else {
        file.close( );
    }

    fs_mutex_release( );
}

// =============================================================================
// Raw backing-store implementations (always touch flash directly).
// FileCache calls these to actually flush; the cache-aware public versions
// below call into the cache first.
// =============================================================================

// Shared body for safeFileReadAllRaw / safeFileReadAll. `truncated` (optional)
// is set when the file was larger than the buffer and the read was clamped -
// the return value stays true so bounded-read callers keep their old behavior.
// ponytail: safeFileReadAllRaw keeps its historical 5-arg signature because
// FileCache.cpp and States.cpp re-declare it via extern; the truncation flag
// is only reachable through safeFileReadAll's new optional out-param.
static bool fileReadAllRawImpl( const char* path, char* buffer, size_t buffer_size,
                                size_t* bytes_read, uint32_t timeout_ms, bool* truncated ) {
    if ( !path || !buffer || buffer_size == 0 )
        return false;

    if ( bytes_read ) *bytes_read = 0;
    if ( truncated ) *truncated = false;

    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        return false;
    }

    File file = FatFS.open( path, "r" );
    if ( !file ) {
        fs_mutex_release( );
        return false;
    }

    size_t file_size = file.size( );
    size_t to_read = ( file_size < buffer_size - 1 ) ? file_size : buffer_size - 1;
    if ( truncated && file_size > to_read ) *truncated = true;

    size_t read_n = file.readBytes( buffer, to_read );
    buffer[ read_n ] = '\0';
    if ( bytes_read ) *bytes_read = read_n;

    file.close( );
    fs_mutex_release( );

    return true;
}

bool safeFileReadAllRaw( const char* path, char* buffer, size_t buffer_size,
                         size_t* bytes_read, uint32_t timeout_ms ) {
    return fileReadAllRawImpl( path, buffer, buffer_size, bytes_read, timeout_ms, nullptr );
}

// Trace macros: psram_debug acts as a master, fc_debug is the per-area
// toggle for filesystem layer traces. Either one enables FSDBG.
extern "C" volatile int psram_debug;
extern "C" volatile int fc_debug;
// Set non-zero (via fatFsSetTimingDebug) to log end-to-end direct-write timing.
extern "C" volatile int spiftl_timing_debug;
#include "hardware/structs/sio.h"
// No Serial.flush() - a per-line blocking flush in this hot FS path can stall
// USB CDC servicing and wedge an SRAM-only board under heavy trace output.
// Lines drain in the background; worst case we lose the last few on a crash.
#define FSDBG(fmt, ...) do { if (fc_debug || psram_debug) { Serial.printf("[%lu c%d] FS> " fmt "\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), ##__VA_ARGS__); } } while(0)

bool safeFileWriteAllRaw( const char* path, const char* content, size_t content_len,
                          uint32_t timeout_ms ) {
    if ( !path || !content )
        return false;

    if ( content_len == 0 )
        content_len = strlen( content );

    uint32_t _wt0 = spiftl_timing_debug ? micros() : 0;

    FSDBG("WriteAllRaw ENTER path=%s len=%u", path, (unsigned)content_len);

    AsyncPassthrough::suspendUARTRxIRQ();

    FSDBG("WriteAllRaw pauseCore2ForFlash");
    bool was_paused = pauseCore2ForFlash( 100 );

    extern volatile bool core2busy;
    if ( core2busy ) {
        FSDBG("WriteAllRaw waiting for core2busy");
        uint32_t extra = millis();
        while ( core2busy && (millis() - extra < 200) )
            delayMicroseconds(100);
        FSDBG("WriteAllRaw core2busy cleared in %ums", (unsigned)(millis()-extra));
    }

    FSDBG("WriteAllRaw fs_mutex_acquire");
    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        FSDBG("WriteAllRaw FAIL fs_mutex timeout");
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }

    FSDBG("WriteAllRaw FatFS.open");
    File file = FatFS.open( path, "w" );
    if ( !file ) {
        FSDBG("WriteAllRaw FAIL open");
        fs_mutex_release( );
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ();
        return false;
    }

    FSDBG("WriteAllRaw file.write");
    size_t written;
    {
        // Flash erase/program monopolizes the RP2350 QMI controller, so any
        // read from PSRAM (CS1) during the flash-write window stalls/faults.
        // FatFS hands full-sector transfers straight to the flash driver from
        // the caller's buffer, so a PSRAM source pointer (e.g. undo's
        // g_persistBuf, which undoAlloc() places in PSRAM) would be read mid
        // flash op and crash. When the source lives in PSRAM, stream it through
        // a small SRAM stack bounce buffer: the PSRAM read happens before the
        // flash op, and FatFS only ever sees an SRAM source. (The file cache's
        // flushEntryChunked solves the same hazard the same way.) The fs_mutex
        // is held here, so we don't share any static buffer across cores.
        uintptr_t srcAddr = reinterpret_cast<uintptr_t>( content );
        bool srcInPsram = ( srcAddr >= 0x11000000u && srcAddr < 0x12000000u );
        if ( srcInPsram ) {
            FSDBG("WriteAllRaw PSRAM source - bouncing through SRAM");
            uint8_t bounce[512];
            const uint8_t* p = (const uint8_t*)content;
            size_t remaining = content_len;
            written = 0;
            while ( remaining > 0 ) {
                size_t cn = remaining < sizeof( bounce ) ? remaining : sizeof( bounce );
                memcpy( bounce, p, cn );  // PSRAM read while flash is idle
                size_t w = file.write( bounce, cn );
                written += w;
                if ( w != cn ) break;     // short write - stop and report
                p += cn;
                remaining -= cn;
            }
        } else {
            written = file.write( (const uint8_t*)content, content_len );
        }
    }
    FSDBG("WriteAllRaw file.close");
    file.close( );

    fs_mutex_release( );
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ();

    FSDBG("WriteAllRaw EXIT written=%u/%u", (unsigned)written, (unsigned)content_len);
    if ( spiftl_timing_debug ) {
        // End-to-end direct write incl. f_close -> CTRL_SYNC -> FTL persist.
        Serial.printf("[FSWRITE] %s %u bytes in %lu us\n",
                      path, (unsigned)content_len, (unsigned long)(micros() - _wt0));
        Serial.flush();
    }
    return ( written == content_len );
}

// =============================================================================
// Cache-aware public wrappers
// =============================================================================
// (FileCache.h is included at the top of this file.)

bool safeFileReadAll( const char* path, char* buffer, size_t buffer_size,
                      size_t* bytes_read, uint32_t timeout_ms, bool* truncated ) {
    if ( !path || !buffer || buffer_size == 0 )
        return false;
    if ( truncated ) *truncated = false;

    // Cache hit? fileCacheReadInto copies under the cache mutex so a
    // concurrent fileCacheWrite from another core can't realloc the entry
    // body out from under us (which would have been a use-after-free if
    // we used fileCacheRead + memcpy here).
    size_t copied = 0;
    if ( fileCacheReadInto( path, (uint8_t*)buffer, buffer_size, &copied ) ) {
        if ( bytes_read ) *bytes_read = copied;
        if ( truncated ) *truncated = ( fileCacheSize( path ) > (int32_t)copied );
        return true;
    }
    return fileReadAllRawImpl( path, buffer, buffer_size, bytes_read, timeout_ms, truncated );
}

bool safeFileWriteAll( const char* path, const char* content, size_t content_len,
                       uint32_t timeout_ms ) {
    if ( !path || !content )
        return false;
    if ( content_len == 0 ) content_len = strlen( content );

    // USB MSC guard: see safeFileOpen - a firmware write while the host has
    // the disk mounted corrupts the host's cached FAT view.
    if ( usbMountedByHost ) {
        Serial.print( "[FS] write to " );
        Serial.print( path );
        Serial.println( " refused: USB host has the filesystem mounted; eject first" );
        return false;
    }

    // Try the cache first. On success the data is in PSRAM and will be
    // flushed in the background.
    if ( fileCacheWrite( path, (const uint8_t*)content, content_len ) ) {
        return true;
    }
    // No PSRAM (or cache full) - fall back to direct flash write.
    return safeFileWriteAllRaw( path, content, content_len, timeout_ms );
}

int safeFileWrite( File& file, const uint8_t* data, size_t len ) {
    if ( !file || !data || len == 0 )
        return -1;

    // Note: Assumes caller already holds fs_mutex from safeFileOpen()
    // Suspend UART IRQ to prevent priority-1 ISR from starving USB during flash ops
    AsyncPassthrough::suspendUARTRxIRQ();

    // Pause Core2 for the write operation
    bool was_paused = pauseCore2ForFlash( 100 );

    int written = file.write( data, len );

    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ();

    return written;
}

// Held-pause variant - caller already owns pauseCore2 + UART suspension
// + fs_mutex. Used by FileCache's chunked flusher so the pause/resume
// happens ONCE around the whole open+write+close+commit, not per chunk.
int safeFileWriteHeld( File& file, const uint8_t* data, size_t len ) {
    if ( !file || !data || len == 0 )
        return -1;
    return file.write( data, len );
}

// Held-pause variant of safeFileClose. Closes the file (which fsyncs
// in write mode) and releases fs_mutex but does NOT pause/unpause
// Core 1 - the caller already holds the outer pause.
void safeFileCloseHeld( File& file ) {
    if ( !file ) return;
    file.close();
    fs_mutex_release();
}

// Same as safeFileCloseHeld but does NOT release fs_mutex. Caller will
// chain another FS op (unlink/rename) and release the mutex once at the
// end. Keeping fs_mutex held across the chain prevents the
// filesystem-activity LED from blinking once per fs_mutex_release - the
// user sees ONE save indicator, not 3.
void safeFileCloseHeldNoRelease( File& file ) {
    if ( !file ) return;
    file.close();
}

bool safeFileFlush( File& file ) {
    if ( !file )
        return false;

    // Suspend UART IRQ to prevent priority-1 ISR from starving USB during flash ops
    AsyncPassthrough::suspendUARTRxIRQ();

    // Pause Core2 for flush operation (commits to flash)
    bool was_paused = pauseCore2ForFlash( 100 );

    file.flush( );

    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ();

    return true;
}

bool safeFileExistsRaw( const char* path, uint32_t timeout_ms ) {
    if ( !path )
        return false;

    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        return false;
    }

    bool exists = FatFS.exists( path );

    fs_mutex_release( );

    return exists;
}

bool safeFileExists( const char* path, uint32_t timeout_ms ) {
    // A path is "present" if either the cache has it OR flash has it.
    if ( fileCacheExists( path ) ) return true;
    return safeFileExistsRaw( path, timeout_ms );
}

int32_t safeFileSizeRaw( const char* path, uint32_t timeout_ms ) {
    if ( !path )
        return -1;

    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        return -1;
    }

    File file = FatFS.open( path, "r" );
    if ( !file ) {
        fs_mutex_release( );
        return -1;
    }

    int32_t size = file.size( );
    file.close( );

    fs_mutex_release( );

    return size;
}

int32_t safeFileSize( const char* path, uint32_t timeout_ms ) {
    int32_t cached = fileCacheSize( path );
    if ( cached >= 0 ) return cached;
    return safeFileSizeRaw( path, timeout_ms );
}

bool safeMkdir( const char* path, uint32_t timeout_ms ) {
    if ( !path )
        return false;

    AsyncPassthrough::suspendUARTRxIRQ( );

    // Pause Core2 for directory creation (flash write)
    bool was_paused = pauseCore2ForFlash( 100 );

    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
        return false;
    }

    // Check if already exists
    if ( FatFS.exists( path ) ) {
        fs_mutex_release( );
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
        return true; // Already exists is success
    }

    bool success = FatFS.mkdir( path );

    fs_mutex_release( );
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    return success;
}

bool safeFileDeleteRaw( const char* path, uint32_t timeout_ms ) {
    if ( !path )
        return false;

    AsyncPassthrough::suspendUARTRxIRQ( );

    // Pause Core2 for file deletion (flash write)
    bool was_paused = pauseCore2ForFlash( 100 );

    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
        return false;
    }

    bool success = FatFS.remove( path );

    fs_mutex_release( );
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    return success;
}

bool safeFileDelete( const char* path, uint32_t timeout_ms ) {
    // Drop any cached entry first so subsequent reads don't return stale data.
    fileCacheDelete( path );
    return safeFileDeleteRaw( path, timeout_ms );
}

bool safeFileRenameRaw( const char* pathFrom, const char* pathTo, uint32_t timeout_ms ) {
    if ( !pathFrom || !pathTo )
        return false;

    AsyncPassthrough::suspendUARTRxIRQ( );

    // Pause Core2 for file rename (flash write)
    bool was_paused = pauseCore2ForFlash( 100 );

    if ( !fs_mutex_acquire_timeout_ms( timeout_ms ) ) {
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
        return false;
    }

    bool success = FatFS.rename( pathFrom, pathTo );

    fs_mutex_release( );
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    return success;
}

bool safeFileRename( const char* pathFrom, const char* pathTo, uint32_t timeout_ms ) {
    fileCacheRename( pathFrom, pathTo );
    return safeFileRenameRaw( pathFrom, pathTo, timeout_ms );
}

//==============================================================================
// Output Area Management Functions
//==============================================================================

void FileManager::clearOutputArea( ) {
    // Clear the output area
    for ( int i = 0; i < outputAreaHeight; i++ ) {
        moveCursor( outputAreaStartRow + i, 1 );
        clearCurrentLine( );
    }
    outputAreaCurrentRow = 0;
}

void FileManager::outputToArea( const String& text, int color ) {
    if ( outputAreaCurrentRow >= outputAreaHeight ) {
        // Scroll up by shifting all lines up
        for ( int i = 0; i < outputAreaHeight - 1; i++ ) {
            moveCursor( outputAreaStartRow + i, 1 );
            clearCurrentLine( );
        }
        outputAreaCurrentRow = outputAreaHeight - 1;
    }

    moveCursor( outputAreaStartRow + outputAreaCurrentRow, 1 );
    clearCurrentLine( );

    changeTerminalColor( color, false );
    Serial.print( text );
    Serial.print( "\n\r" );           // Use proper line ending
    changeTerminalColor( -1, false ); // Reset colors

    outputAreaCurrentRow++;
}

void FileManager::showOutputAreaBorder( ) {
    moveCursor( outputAreaStartRow - 1, 1 );
    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( "╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌\n\r" );
    changeTerminalColor( -1, false ); // Reset colors
}

String FileManager::promptInOutputArea( const String& prompt ) {
    // Show the border if this is the first use
    if ( outputAreaCurrentRow == 0 ) {
        showOutputAreaBorder( );
    }

    // Show the prompt WITHOUT the automatic newline from outputToArea
    moveCursor( outputAreaStartRow + outputAreaCurrentRow, 1 );
    clearCurrentLine( );

    changeTerminalColor( FileColors::STATUS, false );
    Serial.print( prompt );
    changeTerminalColor( -1, false ); // Reset colors

    // Position cursor right after the prompt text for input
    int inputStartCol = prompt.length( ) + 1;

    // Show cursor for input
    Serial.print( "\033[?25h" );
    Serial.flush( );

    String input = "";
    while ( true ) {
        if ( Serial.available( ) ) {
            char c = Serial.read( );
            if ( c == '\r' || c == '\n' ) {
                Serial.print( "\n\r" );
                outputAreaCurrentRow++; // Move to next line after input
                break;
            } else if ( c == 8 || c == 127 ) { // Backspace
                if ( input.length( ) > 0 ) {
                    input.remove( input.length( ) - 1 );
                    // Move cursor back, print space to clear character, move cursor back again
                    Serial.print( "\b \b" );
                    Serial.flush( );
                }
            } else if ( c == 27 ) { // ESC - cancel
                Serial.print( "\n\r" );
                outputAreaCurrentRow++; // Move to next line
                moveCursor( outputAreaStartRow + outputAreaCurrentRow, 1 );
                clearCurrentLine( );
                changeTerminalColor( FileColors::ERROR, false );
                Serial.print( "[Cancelled]\n\r" );
                changeTerminalColor( -1, false );
                outputAreaCurrentRow++;
                Serial.print( "\033[?25l" ); // Hide cursor
                Serial.flush( );
                return "";
            } else if ( c >= 32 && c <= 126 ) { // Printable characters
                input += c;
                // Echo the character immediately
                Serial.print( c );
                Serial.flush( ); // Ensure immediate display
            }
            // Ignore other control characters
        }
        delayMicroseconds( 10 );
    }

    // Hide cursor
    Serial.print( "\033[?25l" );
    Serial.flush( );

    return input;
}

//==============================================================================
// Input Management Functions
//==============================================================================

void FileManager::blockInputBriefly( ) {
    lastDisplayUpdate = millis( );
}

void FileManager::clearBufferedInput( bool allowCtrlQ ) {
    char ctrlQFound = 0;
    char escFound = 0;
    while ( Serial.available( ) ) {
        char c = Serial.read( );
        // Save Ctrl+Q and ESC if we want to allow them
        if ( allowCtrlQ && c == 17 ) { // Ctrl+Q
            ctrlQFound = c;
        }
        if ( c == 27 ) { // ESC
            escFound = c;
        }
    }

    // Put Ctrl+Q and ESC back by sending them to Serial - this is a simple approach
    // In practice, the main loop will need to handle this specially
    if ( ctrlQFound != 0 || escFound != 0 ) {
        // We can't easily put it back, so we'll handle this in isInputBlocked()
        // by allowing Ctrl+Q and ESC to pass through even during blocking
    }
}

bool FileManager::isInputBlocked( ) {
    if ( lastDisplayUpdate == 0 )
        return false;

    unsigned long elapsed = millis( ) - lastDisplayUpdate;
    return elapsed < INPUT_BLOCK_TIME;
}

//==============================================================================
// Persistent Filesystem Message Management Functions
//==============================================================================

void FileManager::addPersistentMessage( const String& message, int color ) {
    if ( persistentMessageCount >= MAX_PERSISTENT_MESSAGES ) {
        // Shift messages up and add new one at end
        for ( int i = 0; i < MAX_PERSISTENT_MESSAGES - 1; i++ ) {
            persistentMessages[ i ] = persistentMessages[ i + 1 ];
        }
        persistentMessageCount = MAX_PERSISTENT_MESSAGES - 1;
    }

    persistentMessages[ persistentMessageCount ].message = message;
    persistentMessages[ persistentMessageCount ].color = color;
    persistentMessages[ persistentMessageCount ].timestamp = millis( );
    persistentMessageCount++;

    // Immediately display the updated messages
    displayPersistentMessages( );
}

void FileManager::clearPersistentMessages( ) {
    persistentMessageCount = 0;

    // Clear the persistent message area
    for ( int i = 0; i < persistentMessageHeight; i++ ) {
        moveCursor( persistentMessageStartRow + i, 1 );
        clearCurrentLine( );
    }
}

void FileManager::displayPersistentMessages( ) {
    // Auto-cleanup old messages (older than 30 seconds)
    unsigned long currentTime = millis( );
    int validMessages = 0;
    for ( int i = 0; i < persistentMessageCount; i++ ) {
        if ( currentTime - persistentMessages[ i ].timestamp < 30000 ) { // 30 seconds
            if ( validMessages != i ) {
                persistentMessages[ validMessages ] = persistentMessages[ i ];
            }
            validMessages++;
        }
    }
    persistentMessageCount = validMessages;

    // Clear the persistent message area first
    for ( int i = 0; i < persistentMessageHeight; i++ ) {
        moveCursor( persistentMessageStartRow + i, 1 );
        clearCurrentLine( );
    }

    // Display messages (newest first, up to available lines)
    int messagesToShow = min( persistentMessageCount, persistentMessageHeight );
    for ( int i = 0; i < messagesToShow; i++ ) {
        moveCursor( persistentMessageStartRow + i, 1 );

        // Show the most recent messages first
        int messageIndex = persistentMessageCount - messagesToShow + i;

        changeTerminalColor( persistentMessages[ messageIndex ].color, false );
        Serial.print( "⟨ " );
        Serial.print( persistentMessages[ messageIndex ].message );
        changeTerminalColor( -1, false ); // Reset colors
    }

    Serial.flush( );
}

void FileManager::initializePersistentMessageArea( ) {
    // Draw separator line above persistent messages
    moveCursor( persistentMessageStartRow - 1, 1 );
    changeTerminalColor( 240, false ); // Dark gray
    Serial.print( "⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯⋯\n\r" );
    changeTerminalColor( -1, false ); // Reset colors
}
