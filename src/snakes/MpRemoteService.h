// SPDX-License-Identifier: MIT
#ifndef MPREMOTE_SERVICE_H
#define MPREMOTE_SERVICE_H

#include "JumperlOS.h"
#include <Arduino.h>

/**
 * @brief Raw REPL Implementation Strategy
 * 
 * USE_NATIVE_PYEXEC_RAW_REPL:
 *   1 = Use MicroPython's event-driven pyexec_event_repl_process_char() (recommended)
 *       - Non-blocking, processes one character at a time
 *       - Battle-tested implementation from official ports
 *       - Handles all protocol details (raw-paste mode, flow control, interrupts)
 *       - Allows concurrent service of other Jumperless subsystems
 *       - Simpler, more reliable
 * 
 *   0 = Use custom MpRemoteService state machine (legacy)
 *       - Custom implementation with manual state management
 *       - More complex, harder to debug
 *       - Useful for understanding protocol details
 */
#define USE_NATIVE_PYEXEC_RAW_REPL 1

/**
 * @brief MicroPython Raw REPL Service for mpremote and ViperIDE support
 * 
 * This service monitors USBSer2 and implements the MicroPython raw REPL protocol,
 * allowing tools like mpremote and ViperIDE to communicate with the device.
 * 
 * Protocol Summary:
 * - Ctrl-A (0x01): Enter raw REPL mode
 * - Ctrl-B (0x02): Exit raw REPL, return to friendly REPL
 * - Ctrl-C (0x03): Keyboard interrupt
 * - Ctrl-D (0x04): Soft reset (in raw REPL) or execute code
 * - Ctrl-E (0x05): Check for raw-paste mode (followed by 'A' + 0x01)
 * 
 * Response format after code execution:
 *   OK<output>\x04<error_output>\x04
 */
class MpRemoteService : public Service {
public:
    static MpRemoteService& getInstance();
    MpRemoteService(const MpRemoteService&) = delete;
    MpRemoteService& operator=(const MpRemoteService&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "MpRemote"; }
    ServicePriority getPriority() const override { return ServicePriority::CRITICAL; }
    
    /**
     * @brief Callback triggered when a Python script begins execution in raw REPL mode
     * 
     * This virtual method is called automatically when a Python script is about to
     * start executing in raw REPL mode (used by ViperIDE/mpremote). It's invoked from
     * jl_before_python_exec_hook before script compilation and execution begins.
     * 
     * Timing: Called BEFORE:
     * - Script compilation starts
     * - Any Python code executes
     * - Output is generated
     * 
     * Use cases:
     * - Starting execution timers
     * - Logging script start events
     * - Setting up monitoring or profiling
     * - Updating UI status to "executing"
     * - Initializing per-script state
     * 
     * Example usage (create derived class):
     * @code
     * class CustomMpRemoteService : public MpRemoteService {
     *     void onScriptExecutionBegin() override {
     *         execution_start_time = millis();
     *         Serial.println("Script starting...");
     *     }
     * };
     * @endcode
     * 
     * @note This is NOT called for:
     * - Interactive (friendly) REPL commands
     * - Scripts run outside the raw REPL
     * - Each individual line in multiline mode
     * 
     * @note Pair this with onScriptExecutionComplete() for timing measurements
     */
    virtual void onScriptExecutionBegin();
    
    /**
     * @brief Callback triggered when a Python script completes execution in raw REPL mode
     * 
     * This virtual method is called automatically after each Python script finishes
     * executing in raw REPL mode (used by ViperIDE/mpremote). It's invoked from
     * jl_after_python_exec_hook after garbage collection but before returning to
     * the REPL prompt.
     * 
     * Timing: Called AFTER:
     * - Script execution completes
     * - Garbage collection runs
     * - File handles are preserved (not closed in raw REPL mode)
     * 
     * Use cases:
     * - Logging script execution events
     * - Updating UI or status indicators
     * - Performing cleanup that doesn't interfere with file handles
     * - Collecting statistics or metrics
     * 
     * Example usage (create derived class):
     * @code
     * class CustomMpRemoteService : public MpRemoteService {
     *     void onScriptExecutionComplete() override {
     *         Serial.println("Script finished!");
     *         // Your custom logic here
     *     }
     * };
     * @endcode
     * 
     * @note This is NOT called for:
     * - Interactive (friendly) REPL commands
     * - Scripts run outside the raw REPL
     * - Each individual line in multiline mode
     * 
     * @warning Do NOT close file handles here - raw REPL mode needs them to persist!
     */
    virtual void onScriptExecutionComplete();
    
    /**
     * @brief Enable or disable the service
     */
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    
    /**
     * @brief Check if currently in raw REPL mode
     */
    bool isInRawRepl() const { return m_in_raw_repl; }
    
    /**
     * @brief Enable debug output to main Serial
     */
    void setDebug(bool enabled) { m_debug = enabled; }
    bool isDebugEnabled() const { return m_debug; }

    int printReceivedPython = 0;
    void setPrintReceivedPython(int value) { printReceivedPython = value; }
    int getPrintReceivedPython() const { return printReceivedPython; }
    
private:
    MpRemoteService();
    ~MpRemoteService() = default;
    static MpRemoteService* instance;
    
    // State machine for raw REPL
    enum class ReplState {
        IDLE,           // Friendly REPL mode - accepting line-based input
        ENTERING_RAW,   // Just received Ctrl-A
        RAW_REPL,       // In raw REPL mode - waiting for code or Ctrl-B
        RECEIVING_CODE, // Receiving code bytes until Ctrl-D
        EXECUTING,      // Executing received code
        RAW_PASTE_CHECK // Checking for raw-paste mode (0x05 + 'A' + 0x01)
    };
    
    ReplState m_state = ReplState::IDLE;
    bool m_enabled = true;
    bool m_in_raw_repl = false;
    bool m_soft_reset_pending = false;
    bool m_debug = false;  // Enable debug by default for troubleshooting
    bool m_code_executing = false;  // Track if code is currently executing in raw REPL
    
    // Code buffer for receiving code.
    // These four buffers (~16.5 KB total) are new[]'d in the constructor, which
    // runs at STATIC-INIT time via the global `mpRemoteService` reference. On the
    // RP2040 (OG) the heap is only ~25 KB after static allocation, so the 8 KB
    // code buffer was the first allocation to abort() boot. Shrink them on OG;
    // mpremote/ViperIDE scripts there are small.
#if defined(OG_JUMPERLESS)
    static const size_t CODE_BUFFER_SIZE = 1536;
#else
    static const size_t CODE_BUFFER_SIZE = 8192;  // 8KB buffer for code
#endif
    char* m_code_buffer = nullptr;
    size_t m_code_len = 0;
    
    // Friendly REPL line buffer (separate from raw REPL code buffer)
    static const size_t LINE_BUFFER_SIZE = 512;  // 512B for single line
    char* m_line_buffer = nullptr;
    size_t m_line_len = 0;
    
    // Output capture buffers
#if defined(OG_JUMPERLESS)
    static const size_t OUTPUT_BUFFER_SIZE = 1024;
#else
    static const size_t OUTPUT_BUFFER_SIZE = 4096;
#endif
    char* m_stdout_buffer = nullptr;
    char* m_stderr_buffer = nullptr;
    size_t m_stdout_len = 0;
    size_t m_stderr_len = 0;
    
    // Raw paste mode state
    bool m_raw_paste_supported = true;  // We support raw-paste mode
    uint8_t m_raw_paste_check_state = 0; // 0=waiting A, 1=waiting 0x01
    
    // Previous stream (for restoring after execution)
    Stream* m_previous_stream = nullptr;
    
    // Protocol handlers

    

    void sendRawReplPrompt();
    
    // Output capture
    void startOutputCapture();
    void stopOutputCapture();
    void captureStdout(const char* str, size_t len);
    void captureStderr(const char* str, size_t len);
    
    // Helper to write to USBSer2
    void writeResponse(const char* str);
    void writeResponse(const char* str, size_t len);
    void writeByte(uint8_t b);
    
    // MicroPython initialization
    bool ensureMicroPythonInitialized();
};

// Global reference for clean syntax
extern MpRemoteService& mpRemoteService;

// Global flag indicating whether we're in raw REPL mode (for stream routing)
extern bool jl_in_raw_repl_mode;

#endif // MPREMOTE_SERVICE_H

