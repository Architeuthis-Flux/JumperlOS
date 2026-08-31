#ifndef AsyncPassthrough_h
#define AsyncPassthrough_h

#include "Arduino.h"

#define ASYNC_PASSTHROUGH_ENABLED 1

extern bool asyncPassthroughEnabled;
extern bool asyncPassthroughTagParsingEnabled;
extern unsigned long microsPerByteSerial1;
extern unsigned long serial1baud;
extern volatile bool s_line_coding_override;
extern bool async_begun;
#if ASYNC_PASSTHROUGH_ENABLED == 1
namespace AsyncPassthrough {
    // Initialize the CDC1 <-> UART passthrough (pico-sdk uart with HW FIFO)
    void begin(unsigned long baud = 115200);

    // Call frequently from loop() to move data in both directions and
    // apply any pending line-coding changes safely outside ISRs
    void task();

    // Expose UART received ring for other modules.
    // DMA-filled ring (zero-CPU RX); see AsyncPassthrough.cpp. Size must match
    // the definition there (1 << UART_RX_RING_BITS == 8192).
    extern uint8_t uartReceived[8192];
    extern volatile uint16_t uartReceivedHead;
    extern volatile uint16_t uartReceivedTail;


    // Disable tag parsing with smart re-enable based on upload completion detection
    // Re-enables when EITHER condition is met:
    //   1. absolute_timeout_ms elapsed since disable (safety fallback)
    //   2. inactivity_timeout_ms elapsed since last USB->UART data (upload finished)
    // Example: disableTagParsingWithInactivityTimeout(5000, 500)
    //   - Re-enables after 500ms of no data (upload done) OR 5 seconds max (safety)
    void disableTagParsingWithInactivityTimeout(uint32_t absolute_timeout_ms, uint32_t inactivity_timeout_ms);
    
    
    // Startup protection - call when system initialization is complete
    // Tag parsing is disabled during startup to prevent crashes from early Arduino commands
    void signalStartupComplete();
    
    // DTR pulse detection and Arduino reset handling
    // Call this frequently to monitor DTR state changes on CDC interface
    void checkDTRState(Adafruit_USBD_CDC& cdc);
    
    // Returns true if a DTR pulse was detected since last check
    bool wasDTRPulseDetected();
    
    // Clear the DTR pulse flag
    void clearDTRPulse();

    // Set DTR lockout — suppresses DTR detection for duration_ms
    // Called after flash completes to prevent port-close DTR from re-triggering
    void setDTRLockout(uint32_t duration_ms);
    
    // Trigger Arduino reset via GPIO pin
    void resetArduino(int resetPin);

    // ============================================================================
    // Flash Completion Detection (STK500 protocol watching + inactivity fallback)
    // ============================================================================

    // Enter flash mode: starts STK500 protocol watching and activity tracking
    void enterFlashMode();

    // Exit flash mode: clears all flash detection state
    void exitFlashMode();

    // Poll to detect flash completion. Returns true when ANY of:
    //   1. STK_LEAVE_PROGMODE (0x51 0x20) seen + 500ms grace + no new data
    //   2. 1500ms of USB→UART inactivity (after data was seen)
    //   3. 60s hard safety timeout
    bool checkFlashDone();

    // Reset only the STK500 byte scanner (for DTR re-resets during flash).
    // Unlike enterFlashMode(), does NOT reset timestamps or data-seen flag.
    void resetFlashSTKDetection();

    // Returns true if any USB→UART data has been seen since enterFlashMode()
    bool hasFlashDataBeenSeen();

    // ============================================================================
    // UART Response Functions - Route command responses back to Arduino
    // ============================================================================
    
    // Queue a response to be sent back over UART
    // Use this when a command was received from UART and the response should go back
    bool queueUARTResponse(const char* data, size_t len);
    bool queueUARTResponse(const String& data);
    
    // Send any pending UART responses (called from task())
    void sendPendingUARTResponses();
    
    // Check if the current command came from UART (for response routing)
    bool wasCommandFromUART();
    
    // Clear the command-from-UART flag after processing
    void clearCommandFromUARTFlag();
    
    // ============================================================================
    // UART IRQ Control - For safe command processing
    // ============================================================================
    
    /**
     * Suspend UART RX interrupt processing.
     * Call this when a complete command has been detected and is being extracted.
     * This prevents the IRQ from pushing more bytes during critical state changes.
     * IMPORTANT: Keep suspension time minimal to avoid data loss!
     */
    void suspendUARTRxIRQ();
    
    /**
     * Resume UART RX interrupt processing.
     * Call this after command has been copied to CommandBuffer.
     */
    void resumeUARTRxIRQ();
    
    /**
     * Check if UART RX IRQ is currently suspended
     */
    bool isUARTRxIRQSuspended();
    
    /**
     * Clear all tag parser state and command buffers.
     * Call this on DTR pulse to ensure clean state for Arduino flashing.
     */
    void clearTagParserState();

    /**
     * Drain all UART RX buffers (HW FIFO + ring buffer).
     * Disables UART IRQ during the operation to prevent race conditions.
     * Call after Arduino reset to discard phantom 0xFF bytes caused by
     * the ATmega TX pin going tri-state during reset.
     */
    void drainUARTRxBuffers();
    
    /**
     * Mark that command processing is in progress.
     * When set, tag parsing will not accept new commands (they'll be buffered).
     */
    void setCommandProcessingActive(bool active);
    
    /**
     * Check if command processing is currently active
     */
    bool isCommandProcessingActive();
    
    // ============================================================================
    // UART Framing Error Detection and Resync
    // ============================================================================
    
    /**
     * Get UART error statistics for debugging
     * @param framing_errors - total framing errors detected
     * @param overruns - total overrun errors detected  
     * @param resyncs - number of times receiver was resynced due to framing errors
     */
    void getUARTErrorStats(uint32_t* framing_errors, uint32_t* overruns, uint32_t* resyncs);
    
    /**
     * Reset UART error counters
     */
    void resetUARTErrorStats();
    
    /**
     * Manually force a UART receiver resync
     * This disables/re-enables the RX to clear the shift register and wait for a new start bit
     * Use when you suspect framing alignment issues
     */
    void forceUARTResync();
    
    /**
     * Print UART error statistics to Serial (for debugging)
     */
    void printUARTErrorStats();
    
    /**
     * Send a UART break condition to force the remote receiver (Arduino) to resync
     * A break condition holds the TX line LOW for longer than a frame time,
     * which forces any UART receiver to discard its current byte and wait for
     * a new start bit.
     * 
     * @param break_duration_us - how long to hold the line LOW (default ~100us = ~11 bit times at 115200)
     */
    void sendBreakToRemote(uint32_t break_duration_us = 500);
    
    /**
     * Full bidirectional resync: resyncs both local receiver AND sends break to remote
     * Use this when you suspect both sides might be out of sync
     */
    void fullBidirectionalResync();

    // ---------------------------------------------------------------------------
    // Last-data snapshots for UI (lightweight, display-only)
    // - getLastUsbToUartSnapshot(): most-recent USB→UART bytes (host → remote)
    // - getLastUartRxSnapshot(): most-recent UART→USB bytes (remote → host)
    // These are non-blocking, inexpensive copies of an internal ring and are
    // intended for small OLED display use only.
    // ---------------------------------------------------------------------------
    size_t getLastUsbToUartSnapshot(char* out, size_t outSize);
    size_t getLastUartRxSnapshot(char* out, size_t outSize);

    // Clear the saved snapshots after they've been consumed by the UI
    void clearLastUsbToUartSnapshot();
    void clearLastUartRxSnapshot();

    // ============================================================================
    // Idle Line Detection and Timing Validation
    // ============================================================================
    
    /**
     * Check if the UART line is currently idle (no data for > idle threshold)
     * After an idle period, UART framing is guaranteed to be correct
     */
    bool isLineIdle();

    /**
     * Has any REAL byte crossed the Arduino UART passthrough since boot, in
     * either direction? Sticky - latches true and never clears. This is the
     * routable-GPIO availability gate's signal for the UART Tx/Rx pins: a
     * DTR-toggle phantom connect moves no bytes and does NOT latch it.
     * Tradeoff (deliberate, permissive): latched at the consume points, so
     * bytes drained while CDC is disconnected are missed - an untouched UART
     * pin stays assignable.
     */
    bool uartTrafficSinceBoot();
    
    /**
     * Get the time in microseconds since the last byte was received
     * @return microseconds since last RX, or UINT32_MAX if never received
     */
    uint32_t getTimeSinceLastRxUs();
    
    /**
     * Get timing statistics for debugging
     * @param idle_periods - number of idle periods detected
     * @param bytes_since_idle - bytes received since last idle period
     * @param timing_anomalies - count of impossible timing detected
     * @param last_inter_byte_us - time between last two bytes
     */
    void getTimingStats(uint32_t* idle_periods, uint32_t* bytes_since_idle, 
                        uint32_t* timing_anomalies, uint32_t* last_inter_byte_us);
    
    /**
     * Reset timing statistics
     */
    void resetTimingStats();
    
    /**
     * Print comprehensive UART diagnostics including timing info
     */
    void printFullDiagnostics();
    
    /**
     * Wait for line to become idle (blocking)
     * @param timeout_ms - maximum time to wait
     * @return true if line became idle, false if timeout
     */
    bool waitForLineIdle(uint32_t timeout_ms);


    void processPendingLineCoding(void);
}

#endif

// Interop C hooks to coordinate UART0 ownership with MicroPython
// Only effective when JL_UART0_INTEROP_MODE == 1
#ifdef __cplusplus
extern "C" {
#endif
void jl_asyncpassthrough_suspend_uart0( void );
void jl_asyncpassthrough_resume_uart0( void );

// Allow MicroPython to update UART0 line coding while integrating with passthrough
void jl_asyncpassthrough_override_line_coding( uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits );
#ifdef __cplusplus
}
#endif

#endif