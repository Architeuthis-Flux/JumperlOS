// SPDX-License-Identifier: MIT
#ifndef JERIAL_H
#define JERIAL_H

#include <Arduino.h>
#include "hardware/uart.h"
#include "oled.h"
#include "SyntaxHighlighting.h"

// Forward declarations
class ScriptHistory;
class TermControl;
class InjectionBufferStream;
class MultiSourceStream;

// Global variable for interactive mode tracking
extern int termInInteractiveMode;

// ---------------------------------------------------------------------------
// Line-buffering / interactive-mode control (single source of truth)
//
// Line buffering ON  == app interactive (raw passthrough) mode ON: the firmware
//                       owns the line editor and the app forwards keystrokes raw.
// Line buffering OFF == app processes single chars; the app owns line input.
//
// These three functions are the ONLY places that should emit the SO/SI sync
// bytes (0x0E / 0x0F) or mutate termInInteractiveMode. Routing every change
// through them keeps the app in sync and avoids redundantly forcing it on/off.
// ---------------------------------------------------------------------------

// Notify the app of the current config state, but only if it changed since we
// last told it (idempotent reconciliation; safe to call every loop).
void pushLineBufferingToApp();

// Firmware-initiated change: set the config and push it to the app. Returns the
// PREVIOUS enabled state so callers that change the mode temporarily (e.g. the
// OLED terminal) can restore it with another call.
bool setTerminalLineBuffering(bool enabled);

// App-initiated change: the app already moved to this state (it sent us SO/SI),
// so adopt it silently — update tracking WITHOUT echoing the control char back.
void acknowledgeAppLineBuffering(bool enabled);

/**
 * Jerial - Unified Serial Communications System
 * 
 * A drop-in replacement for Serial with advanced routing, terminal control,
 * and intelligent endpoint management.
 * 
 * Features:
 * - Route output to multiple streams simultaneously (broadcast mode)
 * - Automatic tag stripping (<j> and </j> tags)
 * - Terminal control with line editing and history (for USB Serial endpoints)
 * - Intelligent OLED filtering (strips consecutive newlines and ANSI codes)
 * - Support for USB CDC, UART, and OLED endpoints
 * - Full Stream interface compatibility
 * 
 * Usage:
 *   Jerial.addOutputStream(&Serial);      // Add Serial as output
 *   Jerial.addOutputStream(&OLEDOut);     // Add OLED as output
 *   Jerial.setInputStream(&Serial);       // Set Serial as input with terminal control
 *   Jerial.println("Hello!");             // Broadcasts to all outputs with appropriate filtering
 */

#define JERIAL_MAX_OUTPUTS 10
#define JERIAL_MAX_LINE_LENGTH 1024
#define JERIAL_NEWLINE_OUT "\r\n"

enum class JerialEndpoint {
    NONE = 0,
    USB_SERIAL,      // Main Serial (USB CDC 0) - supports terminal control
    USB_SER1,        // USBSer1 (USB CDC 1) - supports terminal control
    USB_SER2,        // USBSer2 (USB CDC 2) - supports terminal control
    USB_SER3,        // USBSer3 (USB CDC 3) - supports terminal control
    SERIAL1,         // Arduino Serial1 (UART) - raw passthrough
    UART0,           // Direct hardware UART0 - raw passthrough
    UART1,           // Direct hardware UART1 - raw passthrough
    OLED,            // OLED display output - filtered for display
    CUSTOM           // Custom stream pointer
};

// ============================================================================
// InjectionBufferStream - Stream wrapper for injection buffer
// ============================================================================
/**
 * Clean Stream Architecture for Command Injection
 * 
 * Problem: AsyncPassthrough needs to inject commands (from Arduino-sent <j> tags)
 *          into the terminal input flow for processing by the main loop.
 * 
 * Solution: Use composable Stream classes:
 * 
 * 1. InjectionBufferStream - Wraps JerialClass::injection_buffer as a Stream
 * 2. MultiSourceStream - Multiplexes injection + real input with priority
 * 3. TermControl reads from MultiSourceStream naturally via stream->read()
 * 
 * Flow:
 *   AsyncPassthrough → Jerial.injectInput() → injection_buffer
 *   Main loop → Jerial.service() → TermControl::service()
 *   TermControl → MultiSourceStream::read() → InjectionBufferStream (priority)
 *   TermControl → MultiSourceStream::read() → Serial (fallback)
 *   TermControl → line buffering → completed_line
 *   Main loop → Jerial.getCompletedLine() → SingleCharCommands
 * 
 * Benefits:
 *   - No special-case injection processing logic
 *   - Natural Stream interface throughout
 *   - Composable architecture (can add more sources easily)
 *   - Injected commands flow through same line buffering as user input
 */
class InjectionBufferStream : public Stream {
public:
    InjectionBufferStream(uint8_t* buffer, size_t buffer_size, uint16_t* read_pos, uint16_t* write_pos)
        : buffer(buffer), buffer_size(buffer_size), read_pos(read_pos), write_pos(write_pos),
          tag_buffer_pos(0), skip_next_tag_char(false) {
        memset(tag_buffer, 0, sizeof(tag_buffer));
    }
    
    virtual int available() override {
        // Note: This doesn't account for filtered tags, but that's fine for availability checks
        if (*read_pos != *write_pos) {
            int count = *write_pos - *read_pos;
            if (count < 0) {
                count += buffer_size;
            }
            // Debug
            // Serial.printf("InjectionStream::available() = %d (r=%d w=%d)\n", count, *read_pos, *write_pos);
            // Serial.flush();
            return count;
        }
        return 0;
    }
    
    virtual int read() override {
        // Read with automatic <j> and </j> tag filtering
        while (*read_pos != *write_pos) {
            uint8_t c = buffer[*read_pos];
            *read_pos = (*read_pos + 1) % buffer_size;
            
            // Serial.printf("InjectionStream::read(): got '%c' (%d) from buffer[%d]\n", 
            //               (char)c, c, (*read_pos - 1 + buffer_size) % buffer_size);
            // Serial.flush();
            
            // Apply tag filtering
            if (!shouldSkipChar((char)c)) {
                // Serial.printf("  -> returning '%c' (not a tag)\n", (char)c);
                // Serial.flush();
                return c;  // Return non-tag character
            }
            // Serial.printf("  -> skipped (part of tag)\n");
            // Serial.flush();
            // Character was part of a tag, continue to next
        }
        return -1;
    }
    
    virtual int peek() override {
        // Simple peek without tag filtering (not used in critical path)
        if (*read_pos != *write_pos) {
            return buffer[*read_pos];
        }
        return -1;
    }
    
    virtual void flush() override {}
    virtual size_t write(uint8_t) override { return 0; }  // Injection buffer is read-only
    
private:
    uint8_t* buffer;
    size_t buffer_size;
    uint16_t* read_pos;
    uint16_t* write_pos;
    
    // Tag filtering state (for <j> and </j> tags)
    char tag_buffer[4];
    uint8_t tag_buffer_pos;
    bool skip_next_tag_char;
    
    // Tag filtering logic (detects and skips <j> and </j> tags)
    bool shouldSkipChar(char c) {
        if (tag_buffer_pos == 0) {
            if (c == '<') {
                tag_buffer[0] = '<';
                tag_buffer_pos = 1;
                return true;  // Skip the '<'
            }
            return false;  // Normal character
        }
        
        tag_buffer[tag_buffer_pos++] = c;
        
        // Check for complete opening tag "<j>"
        if (tag_buffer_pos == 3 && 
            tag_buffer[0] == '<' && tag_buffer[1] == 'j' && tag_buffer[2] == '>') {
            tag_buffer_pos = 0;
            return true;  // Skip entire tag
        }
        
        // Check for complete closing tag "</j>"
        if (tag_buffer_pos == 4 && 
            tag_buffer[0] == '<' && tag_buffer[1] == '/' && 
            tag_buffer[2] == 'j' && tag_buffer[3] == '>') {
            tag_buffer_pos = 0;
            return true;  // Skip entire tag
        }
        
        // Check if this can't possibly be a tag anymore
        bool not_a_tag = false;
        if (tag_buffer_pos == 2 && tag_buffer[1] != 'j' && tag_buffer[1] != '/') {
            not_a_tag = true;
        } else if (tag_buffer_pos == 3 && tag_buffer[1] == 'j' && tag_buffer[2] != '>') {
            not_a_tag = true;
        } else if (tag_buffer_pos == 3 && tag_buffer[1] == '/' && tag_buffer[2] != 'j') {
            not_a_tag = true;
        } else if (tag_buffer_pos >= 4) {
            not_a_tag = true;
        }
        
        if (not_a_tag) {
            // This isn't a tag, we need to emit the buffered characters
            // But we can't easily do that in this architecture, so we'll just
            // reset and treat the current character as non-tag
            // NOTE: This is a limitation - partial tags like "<x" won't be emitted correctly
            // In practice this doesn't matter for our use case (injected commands are clean)
            tag_buffer_pos = 0;
            return false;  // Don't skip current character
        }
        
        return true;  // Still accumulating potential tag
    }
};

// ============================================================================
// MultiSourceStream - Multiplexes multiple input streams with priority
// ============================================================================
class MultiSourceStream : public Stream {
public:
    MultiSourceStream(Stream* priority, Stream* fallback)
        : priority_stream(priority), fallback_stream(fallback) {}
    
    virtual int available() override {
        int n = priority_stream ? priority_stream->available() : 0;
        if (n > 0) return n;
        return fallback_stream ? fallback_stream->available() : 0;
    }
    
    virtual int read() override {
        if (priority_stream && priority_stream->available() > 0) {
            int c = priority_stream->read();
            // Debug
            // Serial.printf("MuxRead: injection char '%c' (%d)\n", (char)c, c);
            // Serial.flush();
            return c;
        }
        return fallback_stream ? fallback_stream->read() : -1;
    }
    
    virtual int peek() override {
        if (priority_stream && priority_stream->available() > 0) {
            return priority_stream->peek();
        }
        return fallback_stream ? fallback_stream->peek() : -1;
    }
    
    virtual void flush() override {
        if (fallback_stream) fallback_stream->flush();
    }
    
    virtual size_t write(uint8_t byte) override {
        return fallback_stream ? fallback_stream->write(byte) : 0;
    }
    
private:
    Stream* priority_stream;
    Stream* fallback_stream;
};

class JerialClass : public Stream {
public:
    JerialClass();
    ~JerialClass();

    // ============================================================================
    // Stream Interface - Full compatibility with Serial
    // ============================================================================
    
    virtual int available() override;
    virtual int read() override;
    virtual int peek() override;
    virtual void flush() override;
    virtual size_t write(uint8_t byte) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual int availableForWrite() override;

    
    
    // ============================================================================
    // Terminal Control - Line Editing and History (for USB Serial endpoints)
    // ============================================================================
    
    /**
     * Process input and handle terminal control
     * Call this in main loop - returns true when a completed line is ready
     * Only works when input stream supports terminal control (USB Serial endpoints)
     */
    bool service();
    
    /**
     * Check if a completed line is ready for processing
     */
    bool hasCompletedLine() const;
    
    /**
     * Get the completed line (consumes it)
     */
    String getCompletedLine();
    
    /**
     * Get the response target stream for the current command
     * Returns nullptr if no specific target (use default output)
     */
    Stream* getResponseTarget();
    
    /**
     * Peek at the completed line without consuming it
     */
    String peekCompletedLine() const;
    
    /**
     * Clear completed line without consuming
     */
    void clearCompletedLine();
    
    /**
     * Inject a completed line directly (for programmatic commands)
     * @param line The command line to inject
     * @param response_target Optional stream to send the response to (nullptr = use current output)
     */
    void injectCompletedLine(const char* line, Stream* response_target = nullptr);
    
    /**
     * Set the response target for the NEXT completed line assembled from character injection
     * This allows character-by-character injection while tracking response routing
     * Use this when injecting characters via injectInput() - the target will be attached
     * to the line when it's completed (on newline)
     */
    void setPendingResponseTarget(Stream* target);
    
    /**
     * Get the pending response target (used internally by TermControl)
     */
    Stream* getPendingResponseTarget();
    
    /**
     * Clear the pending response target (used internally by TermControl)
     */
    void clearPendingResponseTarget();
    
    /**
     * Set the current response target for command execution (used by InjectedCommandService)
     * This bypasses the queue system and directly sets the response target
     * that will be returned by getResponseTarget()
     */
    void setCurrentResponseTarget(Stream* target);
    
    /**
     * Clear the current response target after command execution
     */
    void clearCurrentResponseTarget();
    
    /**
     * Get current line being edited (read-only)
     */
    const char* getCurrentLineBuffer();

    /**
     * Where the input-line cursor sits, in terminal columns: visible prompt
     * width plus characters typed so far. 0 when the line editor is inactive.
     * Used by the pinned live-reading line to release its pin before a long
     * input line wraps and breaks its "two rows up" cursor math.
     */
    int getInputLineColumns();

    /**
     * Repaint the prompt + current input line where the cursor is now, leaving
     * the cursor at the user's edit position. Used by ReadingDisplay after it
     * scrolls rows in for a pinned reading, so the input line follows its text
     * instead of being erased by the pin's cursor math.
     */
    void redrawInputLine();
    
    /**
     * Set terminal prompt
     */
    void setPrompt(const char* prompt);
    
    /**
     * Set colored terminal prompt
     */
    void setColoredPrompt(const char* prompt, int color_code = 79);
    
    /**
     * Enable/disable character echoing for terminal control
     */
    void enableEcho(bool enabled);
    
    /**
     * Get access to history manager (may be nullptr)
     */
    ScriptHistory* getHistory();
    
    // ============================================================================
    // Output Routing - Can broadcast to multiple streams
    // ============================================================================
    
    /**
     * Add an output stream for broadcasting
     * All write/print operations will be sent to all registered output streams
     */
    bool addOutputStream(Stream* stream);
    
    /**
     * Add an output stream by endpoint type
     */
    bool addOutputStream(JerialEndpoint endpoint);
    
    /**
     * Remove a specific output stream
     */
    bool removeOutputStream(Stream* stream);
    
    /**
     * Remove output stream by endpoint type
     */
    bool removeOutputStream(JerialEndpoint endpoint);
    
    /**
     * Clear all output streams
     */
    void clearOutputStreams();
    
    /**
     * Get number of registered output streams
     */
    int getOutputCount() const { return output_count; }
    
    /**
     * Set single output stream (clears existing and adds one)
     */
    void setOutputStream(Stream* stream);
    void setOutputStream(JerialEndpoint endpoint);
    
    // ============================================================================
    // Input Routing - Read from a specific source
    // ============================================================================
    
    /**
     * Set the input stream for read operations
     * If the stream supports terminal control (USB Serial), TermControl will be enabled
     */
    void setInputStream(Stream* stream);
    
    /**
     * Set input stream by endpoint type
     */
    void setInputStream(JerialEndpoint endpoint);
    
    /**
     * Get current input stream
     */
    Stream* getInputStream() const { return input_stream; }
    
    /**
     * Add an input source to monitor (for multi-source input)
     * When using serviceInputs(), will check all registered input sources
     */
    bool addInputSource(Stream* stream);
    bool addInputSource(JerialEndpoint endpoint);
    
    /**
     * Check all registered input sources and automatically switch to the one with data
     * Returns true if any source has data available
     * Call this in your main loop before checking available()
     */
    bool serviceInputs();
    
    /**
     * Inject data into the input stream (makes it appear as if it was received)
     * Useful for programmatically sending commands that should be processed normally
     * @param strip_tags If true, removes <j> and </j> tags from input
     */
    bool injectInput(const char* data, bool strip_tags = false);
    bool injectInput(const uint8_t* data, size_t size, bool strip_tags = false);
    
    /**
     * Clear the injection buffer
     */
    void clearInjectedInput();
    
    /**
     * Get the injection stream (for adding as high-priority input source)
     * This stream wraps the injection buffer and automatically strips tags
     */
    Stream* getInjectionStream() { return injection_stream; }
    
    /**
     * Check if injection buffer has a complete line (ending in \n)
     * This is a fast, thread-safe check without consuming data
     * @return true if there's at least one complete line in the buffer
     */
    bool hasInjectedCompleteLine() const;
    
    /**
     * Extract a complete line directly from injection buffer (fast path)
     * This bypasses TermControl and reads directly from injection_buffer
     * Thread-safe: uses atomic positions
     * @return The completed line (without newline), or empty string if none
     */
    String getInjectedCompleteLine();

    volatile int hasInjectedCommand = 0;
    // Note: Tag stripping is now handled automatically by InjectionBufferStream
    // for injected commands. User-typed input doesn't have tags.
    
    // ============================================================================
    // Utility Functions
    // ============================================================================
    
    /**
     * Print a string with line ending normalization.
     * Converts bare \n (not preceded by \r) to \r\n.
     * Use this for multiline output (YAML, JSON, etc.) when terminal_line_buffering
     * is enabled, so terminals like Tabby render line breaks correctly.
     */
    size_t printNormalized(const String& str);
    size_t printNormalized(const char* str);
    
    /**
     * Check if a specific stream is registered as output
     */
    bool hasOutputStream(Stream* stream) const;
    
    /**
     * Enable/disable broadcast mode
     * When disabled, only writes to first output stream
     */
    void setBroadcastMode(bool enabled) { broadcast_enabled = enabled; }
    bool isBroadcastMode() const { return broadcast_enabled; }
    
    /**
     * Get endpoint type for a stream pointer
     */
    static JerialEndpoint getEndpointType(Stream* stream);
    
    /**
     * Get stream pointer for an endpoint type
     */
    static Stream* getStreamForEndpoint(JerialEndpoint endpoint);
    
    /**
     * Helper to convert endpoint to string name
     */
    static const char* endpointToString(JerialEndpoint endpoint);
    
    /**
     * Check if an endpoint supports terminal control features
     */
    static bool supportsTerminalControl(JerialEndpoint endpoint);
    
    // ============================================================================
    // Direct Print Methods - Override normal routing to print to specific port
    // ============================================================================
    
    /**
     * Print directly to a specific endpoint, bypassing normal output routing
     * Useful for sending data to a port that isn't registered as an output
     * @param endpoint The endpoint to write to
     * @param data The data to write
     * @return Number of bytes written
     */
    size_t printTo(JerialEndpoint endpoint, const char* data);
    size_t printTo(JerialEndpoint endpoint, const String& data);
    size_t printTo(JerialEndpoint endpoint, uint8_t byte);
    size_t printTo(JerialEndpoint endpoint, const uint8_t* buffer, size_t size);
    
    /**
     * Print directly to a specific stream, bypassing normal output routing
     */
    size_t printTo(Stream* stream, const char* data);
    size_t printTo(Stream* stream, const String& data);
    size_t printTo(Stream* stream, uint8_t byte);
    size_t printTo(Stream* stream, const uint8_t* buffer, size_t size);
    
    /**
     * Print line directly to a specific endpoint
     */
    size_t printlnTo(JerialEndpoint endpoint, const char* data = "");
    size_t printlnTo(JerialEndpoint endpoint, const String& data);
    
    /**
     * Print line directly to a specific stream
     */
    size_t printlnTo(Stream* stream, const char* data = "");
    size_t printlnTo(Stream* stream, const String& data);
    
private:
    // Output streams (for broadcasting)
    Stream* output_streams[JERIAL_MAX_OUTPUTS];
    int output_count;
    bool broadcast_enabled;
    
    // Input stream (single source)
    Stream* input_stream;
    
    // Input sources for auto-switching
    Stream* input_sources[JERIAL_MAX_OUTPUTS];
    int input_source_count;
    
    // Terminal control instance (created when needed for USB Serial endpoints)
    TermControl* term_control;
    bool term_control_active;
    
    // Pending response target for next completed line (from character-by-character injection)
    // Stored at JerialClass level since AsyncPassthrough (interrupt context) needs to set it
    Stream* pending_response_target;
    
    // Current response target for command execution (set by InjectedCommandService)
    // This bypasses the queue system for fast-path command execution
    Stream* current_response_target;
    
    // Injection buffer for programmatic input
    char injection_buffer[512];
    uint16_t injection_read_pos;
    uint16_t injection_write_pos;
    
    // Stream wrappers for clean architecture
    InjectionBufferStream* injection_stream;
    MultiSourceStream* mux_stream;
    
    // Legacy tag buffer members (kept for compatibility but unused)
    // Tag filtering now happens in InjectionBufferStream
    char tag_buffer[4];
    uint8_t tag_buffer_pos;
    bool in_tag;
    
    // Internal helpers
    size_t writeToOutputs(uint8_t byte);
    size_t writeToOutputs(const uint8_t *buffer, size_t size);
    void createTermControlIfNeeded(Stream* stream); // Create TermControl for USB Serial endpoints
    void destroyTermControl();    // Clean up TermControl instance
};

// Global Jerial instance - use this like Serial
extern JerialClass Jerial;



/**
 * TermControl - Terminal line editing and history
 * 
 * Internal class used by Jerial for USB Serial endpoints.
 * Provides line buffering, ANSI escape sequence handling, and command history.
 * 
 * NOT intended for direct use - use Jerial instead.
 */
class TermControl : public Stream {
public:
    TermControl(Stream* underlying_stream, bool create_own_history = true);
    ~TermControl();

    // Standard Stream interface
    virtual int available() override;
    virtual int read() override;
    virtual int peek() override;
    virtual void flush() override;
    virtual size_t write(uint8_t byte) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;

    // Terminal-specific functionality
    bool service();                     // Call this in main loop to handle input
    bool hasCompletedLine() const;      // True when a complete line is ready
    String getCompletedLine();          // Get the completed line (consumes it)
    String peekCompletedLine() const;   // Get the completed line without consuming it
    void clearCompletedLine();          // Clear completed line without consuming
    void injectCompletedLine(const char* line, Stream* response_target = nullptr); // Inject a completed line directly
    Stream* getResponseTarget();        // Get response target for current command
    const char* getCurrentLineBuffer(); // Get current line being edited (read-only)
    int getLineLength() const { return line_length; }             // typed-so-far, in columns
    int getPromptVisibleWidth() const { return prompt_visible_cols; } // prompt width WITHOUT its SGR codes
    void setPrompt(const char* prompt); // Set prompt string
    void enableEcho(bool enabled);      // Enable/disable character echoing
    ScriptHistory* getHistory() { return history; } // Get access to history instance
    void setColoredPrompt(const char* prompt, int color_code = 79); // Set colored prompt
    bool hasInput() const { return hasCompletedLine(); } // Alias for compatibility

private:
    Stream* stream;                     // Underlying stream (usually Serial)
    ScriptHistory* history;             // History manager
    bool owns_history;                  // True if we created the history instance
    bool history_initialized;           // True if history filesystem has been initialized
    
    // Current line being edited
    char current_line[JERIAL_MAX_LINE_LENGTH];      // Clean input buffer (no ANSI codes)
    char display_buffer[JERIAL_MAX_LINE_LENGTH * 4]; // Display buffer with ANSI codes
    int display_length;
    int line_length;
    int cursor_position;
    
    // Completed line ready for external parsing (single slot - legacy)
    String completed_line;
    bool line_ready;
    
    // Command queue for injected commands (prevents blocking in AsyncPassthrough)
    static const int COMMAND_QUEUE_SIZE = 8;
    String command_queue[COMMAND_QUEUE_SIZE];
    Stream* response_targets[COMMAND_QUEUE_SIZE];  // Target stream for each command's response
    volatile uint8_t queue_head;  // Next slot to write
    volatile uint8_t queue_tail;  // Next slot to read
    volatile uint8_t queue_count; // Number of items in queue
    
    // Last retrieved response target (captured when getCompletedLine() is called)
    Stream* last_response_target;
    
    // Settings
    String prompt_text;
    int prompt_visible_cols = 0;        // columns the prompt occupies on screen (no ANSI)
    bool echo_enabled;
    SyntaxHighlighting syntax_highlighter;
    
    // ANSI escape sequence state
    enum AnsiState {
        ANSI_NORMAL,
        ANSI_ESCAPE,
        ANSI_BRACKET,
        ANSI_MAIN_SERIAL_ENQ
    } ansi_state;

    int brace_depth; // Nesting level of curly braces

public:
    // Repaint prompt + current line at the cursor's present row, leaving the
    // cursor at the user's edit position. Public so ReadingDisplay can put the
    // input line back after scrolling rows in for a pinned reading.
    void renderCurrentLine();

private:
    // Internal methods
    void handleNormalChar(char c);
    void handleBackspace();
    void handleEnter();
    void handleArrowUp();
    void handleArrowDown();
    void handleArrowLeft();
    void handleArrowRight();
    void handleCtrlU();
    void handleTab();
    void handleMainSerialENQ();

    void clearCurrentLine();
    void moveCursorTo(int position);
    void moveCursorToPosition(int position);
    void insertCharAtCursor(char c);
    void deleteCharAtCursor();
    int calculateVisualLength(const String& text);
    bool currentLineIsMultiline() const; // True if the edit buffer contains an embedded newline
};



/**
 * OLEDStream - A Stream wrapper for the OLED display
 * 
 * Allows printing to the OLED using standard print/println/printf functions.
 * Automatically filters consecutive newlines and ANSI control sequences.
 * 
 * Features:
 * - Automatic line wrapping and scrolling
 * - Small text mode (5pt fonts) for better readability
 * - Filters consecutive newlines for compact display
 * - Strips ANSI color codes automatically
 * - Full Stream interface compatibility
 * 
 * Usage:
 *   OLEDStream oledOut;
 *   oledOut.println("Hello OLED!");
 *   oledOut.printf("Value: %d\n", 42);
 */

// OLEDSTREAM_MAX_LINES is now calculated dynamically based on display height
#define OLEDSTREAM_MAX_POSSIBLE_LINES 16  // Max for largest supported display (128x128)
#define OLEDSTREAM_LINE_LENGTH 32     // Max chars per line (conservative)
#define OLEDSTREAM_BUFFER_SIZE 512    // Total buffer size

class OLEDStream : public Stream {
public:
    OLEDStream();
    ~OLEDStream();
    
    // ============================================================================
    // Stream Interface
    // ============================================================================
    
    virtual int available() override;
    virtual int read() override;
    virtual int peek() override;
    virtual void flush() override;
    virtual size_t write(uint8_t byte) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    
    // ============================================================================
    // Print Methods - Convert values to ASCII (like Serial.print)
    // ============================================================================
    // Inherit base class print/println for strings, chars, Printable
    using Print::print;
    using Print::println;
    
    // Numeric type overrides - format entire value to a string,
    // then write atomically so the OLED updates once per print() call
    size_t print(int value, int base = DEC);
    size_t print(unsigned int value, int base = DEC);
    size_t print(long value, int base = DEC);
    size_t print(unsigned long value, int base = DEC);
    size_t print(long long value, int base = DEC);
    size_t print(unsigned long long value, int base = DEC);
    size_t print(double value, int decimals = 2);
    
    size_t println(int value, int base = DEC);
    size_t println(unsigned int value, int base = DEC);
    size_t println(long value, int base = DEC);
    size_t println(unsigned long value, int base = DEC);
    size_t println(long long value, int base = DEC);
    size_t println(unsigned long long value, int base = DEC);
    size_t println(double value, int decimals = 2);
    size_t println(void);
    
    // ============================================================================
    // OLED-specific Functions
    // ============================================================================
    
    /**
     * Set the small font to use for display
     */
    void setSmallFont(SmallFont font);
    
    /**
     * Get current small font
     */
    SmallFont getSmallFont() const { return current_font; }
    
    /**
     * Get maximum number of lines for current display height
     */
    int getMaxLines() const { return max_lines; }
    
    /**
     * Enable/disable automatic display updates
     * When disabled, call flush() manually to update display
     */
    void setAutoUpdate(bool enabled) { auto_update = enabled; }
    bool isAutoUpdate() const { return auto_update; }
    
    /**
     * Clear the display and reset cursor
     */
    void clear();
    
    /**
     * Enable/disable scrolling (when disabled, wraps to line 0)
     */
    void setScrollEnabled(bool enabled) { scroll_enabled = enabled; }
    bool isScrollEnabled() const { return scroll_enabled; }
    
    /**
     * Check if OLED is connected
     */
    bool isConnected() const;
    
private:
    // Settings
    SmallFont current_font;
    bool auto_update;
    bool scroll_enabled;
    
    // ANSI escape filtering
    bool in_ansi_escape;
    
    // Newline filtering (prevent consecutive newlines)
    bool last_was_newline;
    
    // Internal helpers (stubs - showMultiLineSmallText handles buffering)
    void newline();
    void scrollUp();
    void updateDisplay();
    void printChar(char c);
    void recalculateMaxLines(); // Recalculate max_lines from oled.displayHeight
    bool isANSIEscape(char c); // Check if character is part of ANSI sequence
    
    // Display dimensions (kept for compatibility)
    int max_lines; // Dynamically calculated based on display height (displayHeight / 8)
};

// Global instance
extern OLEDStream OLEDOut;


#include <Arduino.h>
#include "hardware/uart.h"
#include "hardware/gpio.h"

/**
 * UARTStream - A Stream wrapper for Pico hardware UART
 * 
 * Provides a Stream-compatible interface for direct UART access.
 * Useful when you want to bypass Arduino Serial1 and work directly
 * with the hardware UART peripheral.
 * 
 * Features:
 * - Full Stream interface compatibility
 * - Direct hardware UART access (uart0 or uart1)
 * - Configurable baud rate, parity, stop bits
 * - Hardware FIFO support
 * - Works alongside Serial1 or independently
 * 
 * Usage:
 *   UARTStream uart0Stream(uart0, 0, 1);  // UART0, TX=GP0, RX=GP1
 *   uart0Stream.begin(115200);
 *   uart0Stream.println("Hello UART!");
 */

class UARTStream : public Stream {
public:
    /**
     * Constructor
     * @param uart_inst UART instance (uart0 or uart1)
     * @param tx_pin GPIO pin for TX
     * @param rx_pin GPIO pin for RX
     */
    UARTStream(uart_inst_t* uart_inst, uint tx_pin, uint rx_pin);
    ~UARTStream();
    
    // ============================================================================
    // Initialization
    // ============================================================================
    
    /**
     * Initialize UART with specified baud rate
     */
    void begin(uint32_t baud = 115200, 
               uint data_bits = 8, 
               uint stop_bits = 1,
               uart_parity_t parity = UART_PARITY_NONE);
    
    /**
     * Deinitialize UART
     */
    void end();
    
    /**
     * Check if UART is initialized
     */
    bool isInitialized() const { return initialized; }
    
    /**
     * Change baud rate
     */
    void setBaudRate(uint32_t baud);
    
    /**
     * Get current baud rate
     */
    uint32_t getBaudRate() const { return current_baud; }
    
    // ============================================================================
    // Stream Interface
    // ============================================================================
    
    virtual int available() override;
    virtual int read() override;
    virtual int peek() override;
    virtual void flush() override;
    virtual size_t write(uint8_t byte) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual int availableForWrite() override;
    
    // ============================================================================
    // UART-specific Functions
    // ============================================================================
    
    /**
     * Check if UART is writable (TX FIFO not full)
     */
    bool isWritable() const;
    
    /**
     * Check if UART is readable (RX FIFO has data)
     */
    bool isReadable() const;
    
    /**
     * Get UART instance
     */
    uart_inst_t* getUARTInstance() const { return uart; }
    
    /**
     * Enable/disable hardware flow control (RTS/CTS)
     */
    void setFlowControl(bool enable, uint cts_pin = 0, uint rts_pin = 0);
    
private:
    uart_inst_t* uart;
    uint tx_pin;
    uint rx_pin;
    uint32_t current_baud;
    bool initialized;
    
    // Peek buffer for peek() implementation
    bool has_peeked;
    uint8_t peek_byte;
};

#endif // JERIAL_H

