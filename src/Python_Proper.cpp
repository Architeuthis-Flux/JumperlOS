#include "Python_Proper.h"
#include "Adafruit_USBD_CDC.h"
#include "ArduinoStuff.h"
#include <Arduino.h>
#include <FatFS.h>
#include "config.h"
#include "configManager.h"
#include "FilesystemStuff.h"
#include "EkiloEditor.h"
#include "FileParsing.h"
#include "CH446Q.h"
#include "Commands.h"
#include "AsyncPassthrough.h"
#include "Jerial.h" // TermControl is now part of Jerial
#include "LEDs.h"
#include "SyntaxHighlighting.h"
#include "CommandBuffer.h"  // For UART response capture
#include "JumperlOS.h"
#include "MpRemoteService.h"
#include "SharedBuffer.h"  // For zero-copy transfer from Ekilo editor
#include "externVars.h"  // For pauseCore2 synchronization
#include "micropythonExamples.h"  // For embedded Python scripts including ViperIDE reinit
#include "RotaryEncoder.h"  // For clickwheel interrupt during script execution
#include "Highlighting.h"   // For handing the display back at script exit
#include "MeasureMode.h"    // For releasing measure mode at script exit
#include "ReadingDisplay.h" // clearLiveSerialLine() at script exit
#include "pyexec.h"
extern "C" {
#include "py/gc.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/mpstate.h"
#include "py/repl.h"
#include "py/mpthread.h"
#include <micropython_embed.h>
}

// Forward declaration for JFS file cleanup - must be called after script execution
// to close any files opened by MicroPython that weren't explicitly closed
extern "C" void jl_close_all_jfs_files(void);
// Mount the Jumperless VFS at "/" for MicroPython
extern "C" void jl_vfs_mount_root(void);

// Embedded rp2.py content for PIO support (@asm_pio decorator).
// Provisioned to /python_scripts/lib/rp2.py at MicroPython init, so
// `import rp2` resolves through the normal VFS sys.path lookup.
#include "rp2_py_content.h"

// writeStringToFile() lives in FilesystemStuff.cpp but isn't declared in
// FilesystemStuff.h yet — forward-declare it here so we can provision rp2.py.
extern bool writeStringToFile(const char *filename, const char *content);

// Global state for proper MicroPython integration
// Heap is malloc'd so only the needed amount of SRAM is consumed:
// 96KB when PSRAM installed (split heap adds ~8MB via gc_add), 128KB without.
unsigned char *mp_heap = nullptr;
size_t mp_heap_size = 0;
static bool mp_initialized = false;
static bool mp_repl_active = false;
static bool jumperless_globals_loaded = false;
bool mp_interrupt_requested = false; // Flag for keyboard interrupt
bool mp_soft_reset_requested = false; // Flag for Ctrl+D soft reset requests
volatile bool clickWheelPythonInterrupt = false; // When true, holding clickwheel interrupts Python
static unsigned long clickwheel_interrupt_ignore_until = 0; // Grace period after script start (ms)

// Global state for REPL initial file loading
static String repl_initial_filepath = "";
static bool repl_has_initial_file = false;

// Keyboard interrupt character storage
static int keyboard_interrupt_char = 3; // Default to Ctrl+C (MicroPython default)

// Command execution state
static char mp_command_buffer[512];
static bool mp_command_ready = false;
static char mp_response_buffer[1024];

// Terminal colors for different REPL states
/// 0 = menu (cyan) 1 = prompt (light blue) 2 = output (chartreuse) 3 = input
/// (orange-yellow) 4 = error (orange-red) 5 = purple 6 = dark purple 7 = light
/// cyan 8 = magenta 9 = pink 10 = green 11 = grey 12 = dark grey 13 = light grey
/// 14 = history prompt (magenta)
static int replColors[15] = {
     38,  // menu (cyan)
     69,  // prompt (light blue)
    155,  // output (chartreuse)
    221,  // input (orange-yellow)
    202,  // error (orange-red)
     92,  // purple
     56,  // dark purple
     51,  // light cyan
    199,  // magenta
    207,  // pink
     40,  // green
      8,  // grey
    235,  // dark grey
    248,  // light grey
    199,  // history prompt (magenta)
};

Stream *global_mp_stream = &Serial;

// Separate stream for interrupt checking.
// Keep this aligned to the active REPL/input stream so interrupt polling
// does not consume bytes from an unrelated stream.
Stream *mp_interrupt_check_stream = &Serial;

// C-compatible pointer for HAL functions
extern "C" {
    void *global_mp_stream_ptr = (void *)&Serial;
    // When non-NULL, mp_hal_stdio_poll() and mp_hal_stdin_rx_chr() use this
    // instead of global_mp_stream_ptr for stdin operations.  This prevents
    // MpRemoteService::service() (called during time.sleep via serviceCritical)
    // from switching the stdin stream away from Serial to USBSer2, which causes
    // select.poll() to check the wrong CDC port and drop characters.
    void *mp_stdin_locked_stream_ptr = nullptr;
}

// Forward declaration for color function (from Graphics.cpp)
void changeTerminalColor(int termColor, bool flush, Stream *stream);

// Forward declarations
extern "C" {
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len);
int mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_delay_ms(mp_uint_t ms);
mp_uint_t mp_hal_ticks_ms(void);
// Full VM soft reboot (JumperlessMicroPythonAPI.cpp) - used to honor Ctrl-D
// soft-reset requests from the friendly REPL.
void jl_soft_reboot(void);

// Forward declaration for interrupt checking  
// (Note: Actual function is C++ linkage for use by other modules)
// setupFilesystemAndPaths is now declared in Python_Proper.h

// Arduino wrapper functions for HAL
int arduino_serial_available(Stream *stream = global_mp_stream);
int arduino_serial_read(Stream *stream = global_mp_stream);
void arduino_serial_write(const char *str, int len, void *stream);
void arduino_delay_ms(unsigned int ms);
unsigned int arduino_millis(void);

// Export global_mp_stream for C code
extern void *global_mp_stream_ptr;

// =============================================================================
// PSRAM Support Wrapper for MicroPython
// =============================================================================
// Wrapper function to access Arduino-Pico PSRAM API from C code
// Used by micropython_embed.c to detect PSRAM and add it to GC heap
//
// IMPORTANT: This function checks jumperlessConfig.hardware.psram_installed first.
// If psram_installed is 0, we skip PSRAM detection entirely to avoid crashes
// on boards where PSRAM isn't installed or the framework crashes on detection.
size_t jl_get_psram_size(void) {
    // Check config first - if psram_installed is 0, skip detection entirely
    // This prevents crashes on boards without PSRAM hardware
    if (jumperlessConfig.hardware.psram_installed == 0) {
        return 0;
    }
    
    // rp2040.getPSRAMSize() returns the PSRAM chip size in bytes
    // Returns 0 if no PSRAM is detected
    return rp2040.getPSRAMSize();
}

// Phase 1: report only the MicroPython sub-region of PSRAM. The app arena
// (file cache, undo log, scratch buffers) sits at the start; MicroPython
// gets the remainder. If the arena failed to initialize, fall back to
// reporting the full chip so MicroPython still gets PSRAM.
extern uintptr_t psram_mp_base(void);
extern size_t psram_mp_size(void);
extern bool psram_available(void);

uintptr_t jl_get_psram_mp_base(void) {
    if (psram_available() && psram_mp_size() > 0) return psram_mp_base();
    return 0x11000000;  // PSRAM_BASE - fall back to whole chip
}

size_t jl_get_psram_mp_size(void) {
    if (psram_available() && psram_mp_size() > 0) return psram_mp_size();
    return jl_get_psram_size();
}
}

/**
 * Safe garbage collection with Core 2 synchronization
 * 
 * CRITICAL: GC finalisers run during gc_collect() and may call filesystem
 * operations (like file flush/close). Core 2 must be paused during this
 * to prevent concurrent filesystem access which can cause crashes.
 * 
 * Memory barriers (__dmb) ensure pauseCore2 changes are visible to Core 2
 * before proceeding with garbage collection.
 */
static inline void gc_collect_safe(void) {
    // Pause Core 2 before GC to prevent concurrent filesystem access
    bool was_paused = pauseCore2ForFlash(100);
    
    // Run garbage collection (this may call finalisers that access files)
    gc_collect();
    
    // Restore previous pauseCore2 state
    unpauseCore2ForFlash(was_paused);
}

// Arduino timing functions for MicroPython
extern "C" void mp_hal_delay_ms(mp_uint_t ms) { 
  // Check for interrupt during delays and run essential services
  extern void jl_service_python(void);  // From JumperlessMicroPythonAPI.cpp
  
  unsigned int start_time = millis();
  unsigned int last_service_time = start_time;
  unsigned int last_interrupt_check = 0;  // Track last interrupt check independently
  
  while (millis() - start_time < ms) {
    unsigned int current = millis();
    
    // CRITICAL: Service USB EVERY iteration to ensure Ctrl+C bytes reach the CDC buffer
    // Without this, USB characters get stuck in hardware buffers and interrupts don't work
    TinyUSB_Device_Task(); // mutex-guarded pump (never raw tud_task(): the USB IRQ pumps too)
    
    // Check for interrupts at least every 1ms (but don't throttle too aggressively)
    if (current != last_interrupt_check) {
      // Check for interrupt on all streams
      mp_hal_check_interrupt();
      last_interrupt_check = current;
      
      // Raise any pending exception (e.g. KeyboardInterrupt) immediately.
      // mp_hal_check_interrupt() schedules the exception via mp_sched_keyboard_interrupt()
      // but also clears mp_interrupt_requested internally, so checking that flag here
      // is unreliable. mp_handle_pending(true) checks mp_pending_exception directly
      // and nlr_raise()s it, which is the standard MicroPython pattern used by
      // mp_hal_stdin_rx_chr and MICROPY_EVENT_POLL_HOOK in other ports.
      // MicroPython >= 1.28: the bool arg became mp_handle_pending_behaviour_t;
      // AND_EXCEPTIONS == the old `true` (run callbacks and raise the exception).
      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);

      // Fast-path check for flags that mp_handle_pending doesn't cover.
      // Deliberately do NOT clear mp_soft_reset_requested here: the flag must
      // survive until the friendly-REPL outer loop (or MpRemoteService) can
      // perform the actual jl_soft_reboot(). Clearing it here silently turned
      // Ctrl-D into a no-op "soft reset" that left Pin IRQs and Timer alarms
      // armed against a VM everyone believed had been reset.
      if (mp_soft_reset_requested) {
          return;
      }
    }
    
    // Run essential services every 50ms during Python delays
    // This keeps current sense measurements and marching ants animation running
    if (current - last_service_time >= 50) {
      jOS.serviceCritical();
      last_service_time = current;
    }
    
    delayMicroseconds(100); // Very small delay to prevent overwhelming the system
  }
}

extern "C" mp_uint_t mp_hal_ticks_ms(void) { 
  // // Check for interrupt during timing calls (called frequently)
  // mp_hal_check_interrupt();
  return millis(); 
}

// Arduino wrapper functions for the HAL layer
extern "C" int arduino_serial_available(Stream *stream) {
  // Check for interrupt request before checking availability
  // mp_hal_check_interrupt();
  Stream *s = stream ? stream : global_mp_stream;
  if (!s) {
    return 0;
  }
  return s->available();
}

extern "C" int arduino_serial_read(Stream *stream) {
  // Check for interrupt request before reading
 // mp_hal_check_interrupt();
  Stream *s = stream ? stream : global_mp_stream;
  if (!s) {
    return -1;
  }
  int c = s->read();
// Serial.write(c);
// Serial.flush();
  return c;
}

// Wrapper callable from C code (mphalport.c) to service USB.
// tud_task() itself is available in C++ but not directly linkable from
// the micropython library's C compilation units.
extern "C" void service_usb_task(void) {
  TinyUSB_Device_Task(); // mutex-guarded pump
}

// USB-CDC back-pressure guard. Adafruit_USBD_CDC::write() spins forever
// (yield loop) when the port is "connected" (host holds DTR) but the host has
// stopped reading and the TX FIFO is full — the classic "browser/IDE holding
// the port wedges the board on any print()". Wait for room while pumping USB;
// on timeout the caller must DROP its output instead of writing.
bool jl_cdc_wait_writable(Stream *stream, size_t need, unsigned long timeoutMs) {
  // Only USB-CDC ports have the blocking-write hazard. Other streams (Jerial,
  // OLEDOut, ...) may not override availableForWrite() (Print's default
  // returns 0), so gating them here would wrongly drop all their output.
  bool is_cdc = (stream == (Stream *)&Serial) || (stream == (Stream *)&USBSer1) ||
                (stream == (Stream *)&USBSer2) || (stream == (Stream *)&USBSer3);
  if (!stream || !is_cdc) {
    return true;
  }
  if ((size_t)stream->availableForWrite() >= need) {
    return true;
  }
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    yield(); // pump USB + flush CDC: give the host a chance to drain the TX FIFO
    if ((size_t)stream->availableForWrite() >= need) {
      return true;
    }
    delayMicroseconds(100);
  }
  return false;
}

extern "C" void arduino_serial_write(const char *str, int len, void *stream) {
  Stream *s = (Stream *)stream;
  if (s) {
    // When writing to USBSer2 (raw REPL transport), emit bytes verbatim to avoid
    // surprising host decoders that expect exact raw REPL framing.
    bool is_raw_repl_stream = (s == &USBSer2);

    // Convert \n to \r\n for proper terminal display
    for (int i = 0; i < len; i++) {
      // Back-pressure: drop the rest of the output if the host holds DTR but
      // has stopped draining (a blocking write here hangs the whole board).
      if (!jl_cdc_wait_writable(s, 2)) {
        return;
      }
      if (!is_raw_repl_stream && str[i] == '\n') {
        // Friendly REPL / main Serial: normalize to CRLF
        s->write('\r');
        s->write('\n');
      } else {
        // Raw REPL stream: send bytes verbatim (no CR injection)
        s->write(str[i]);
        extern bool oled_copy_print_enabled; // Flag for OLED print copy feature
        extern uint8_t pyexec_repl_active; // Flag to check if REPL is active (from pyexec.cpp)
        if (oled_copy_print_enabled && oled.isConnected() && pyexec_repl_active) {
            OLEDOut.print((char)str[i]);
            // OLEDOut.write((const uint8_t*)&str[i], 1);
        }
      }
    }
    s->flush();
  }
}

extern "C" void arduino_delay_ms(unsigned int ms) { 
  // Check for interrupt during delays
  unsigned long start_time = millis();
  while (millis() - start_time < ms) {
    mp_hal_check_interrupt();
    delayMicroseconds(100);
    if (mp_interrupt_requested || mp_soft_reset_requested) {
        return;
    }
  }
}

extern "C" unsigned int arduino_millis() { return millis(); }

// HAL function to set the keyboard interrupt character
extern "C" mp_uint_t mp_hal_set_interrupt_char(int c) {
    // Track previous interrupt character to avoid spamming debug output
    static int prev_interrupt_char = -1;
    
    keyboard_interrupt_char = c;
    
    // CRITICAL: When disabling interrupts (c == -1), also clear any stale
    // interrupt request flag. check_stream_for_interrupt() sets BOTH
    // mp_interrupt_requested AND mp_pending_exception simultaneously.
    // nlr_jump (from the raised KeyboardInterrupt) skips the code that
    // clears mp_interrupt_requested, leaving it stale. If not cleared here,
    // mp_hal_check_interrupt() would re-schedule the interrupt AFTER the
    // NLR context is popped, causing nlr_jump_fail → core 0 crash.
    if (c < 0) {
        mp_interrupt_requested = false;
    }
    
    // Only print debug message if:
    // - The interrupt character actually changed between two ACTIVE values
    // - We're not on USBSer2 (would interfere with mpremote/ViperIDE protocol)
    // - Not being set to -1 (MicroPython uses -1 to temporarily disable interrupts)
    // - Not restoring from -1 (happens after every script execution when
    //   mp_embed_exec_str's exception handler disables interrupts then the
    //   post-exec cleanup re-enables them — this is normal, not worth logging)
    const bool is_mpremote_active = (global_mp_stream == &USBSer2);
    const bool char_changed = (c != prev_interrupt_char);
    const bool is_real_char = (c >= 0); // -1 means "disable interrupts" - don't spam output
    const bool is_restoration = (prev_interrupt_char < 0); // Coming back from disabled state

    if (global_mp_stream && !is_mpremote_active && char_changed && is_real_char && !is_restoration) {
        char char_name = (c >= 1 && c <= 26) ? (char)(c + 64) : '?';
        // global_mp_stream->printf("[MP] Keyboard interrupt character set to Ctrl+%c (ASCII %d)\n\r", char_name, c);
    }
    
    prev_interrupt_char = c;
    return 0;
}

// Helper function to get current interrupt character
extern "C" int getCurrentInterruptChar(void) {
    return keyboard_interrupt_char;
}

void setGlobalStream(Stream *stream) {
  global_mp_stream = stream;
  global_mp_stream_ptr = (void *)stream;
  mp_interrupt_check_stream = stream;
}

// Set global stream AND correct interrupt character based on stream type
// This ensures the keyboard interrupt is always appropriate for the controlling interface
void setGlobalStreamWithInterrupt(Stream *stream) {
  // First set the stream
  global_mp_stream = stream;
  global_mp_stream_ptr = (void *)stream;
  mp_interrupt_check_stream = stream;
  
  // Determine which interrupt character to use based on stream type
  int interrupt_char;
  if (stream == &USBSer2) {
    // USBSer2 is used by mpremote/ViperIDE - uses Ctrl+C (3)
    interrupt_char = MP_INTERRUPT_CHAR_USBSER2;
  } else {
    // Serial, Jerial, or other streams use Ctrl+Q (17)
    interrupt_char = MP_INTERRUPT_CHAR_SERIAL;
  }
  
  // Set the interrupt character in MicroPython
  // Use the low-level HAL function to avoid circular calls and debug spam
  mp_hal_set_interrupt_char(interrupt_char);
}

// Terminal color control function is now in Graphics.cpp

unsigned long lastInterruptCheckTime = 0;
unsigned long interruptCheckInterval = 1;  // 1ms throttle for responsive interrupts

// Debug flag for mpremote/ViperIDE - when enabled, echo Python output to main Serial
// This helps debug issues with raw REPL output
extern bool mpremote_debug_python_output;
bool mpremote_debug_python_output = false;

// Global flag for OLED print copy feature
bool oled_copy_print_enabled = false;

// MicroPython HAL stdout function with Jumperless-specific functionality
extern "C" void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    // Check for interrupt before outputting (this is called frequently)
    
    // DEBUG: If mpremote debug is enabled AND we're outputting to USBSer2, also echo to Serial
    if (mpremote_debug_python_output && global_mp_stream == &USBSer2 && len > 0) {
        Serial.print("[MpRemote-PY] ");
        for (size_t i = 0; i < len && i < 200; i++) {
            char c = str[i];
            if (c == '\n') Serial.print("\\n");
            else if (c == '\r') Serial.print("\\r");
            else if (c >= 0x20 && c < 0x7F) Serial.print(c);
            else Serial.printf("\\x%02X", (uint8_t)c);
        }
        if (len > 200) Serial.print("...");
        Serial.println();
    }
    
    // If OLED copy is enabled, send to OLEDOut stream
    if (oled_copy_print_enabled && oled.isConnected()) {
        OLEDOut.write((const uint8_t*)str, len);
    }
    
    // Basic output to global stream (regular MicroPython output)
    if (global_mp_stream) {
        // DEFENSE-IN-DEPTH: When a Serial REPL script is executing (lock is set),
        // use the locked stream for stdout too. This prevents MpRemoteService from
        // redirecting print() output to USBSer2 even if save/restore is bypassed.
        Stream* out_stream = global_mp_stream;
        if (mp_stdin_locked_stream_ptr && (Stream*)mp_stdin_locked_stream_ptr != &USBSer2) {
            out_stream = (Stream*)mp_stdin_locked_stream_ptr;
        }

        bool is_raw_repl_stream = (out_stream == &USBSer2);

    for (size_t i = 0; i < len; i++) {
      // CRITICAL: Service USB every 32 characters to prevent CDC buffer deadlock
      // Without this, Serial.write() can block if CDC TX buffer is full,
      // and USB never gets serviced, causing a permanent freeze
      if (i % 50 == 0) {
        yield();  // pump USB + flush CDC to drain the TX buffer
        mp_hal_check_interrupt();
      }

      // Back-pressure: if the host holds DTR but stopped draining, drop the
      // rest of this output instead of letting CDC write() spin forever.
      if (!jl_cdc_wait_writable(out_stream, 2)) {
        return;
      }

      if (!is_raw_repl_stream && str[i] == '\n') {
                // Friendly REPL / main Serial: normalize to CRLF
                out_stream->write('\r');
                out_stream->write('\n');

            } else {
                // Raw REPL stream: send bytes verbatim (no CR injection)
                out_stream->write(str[i]);
                if (oled_copy_print_enabled && oled.isConnected()) {
                    OLEDOut.write((const uint8_t*)&str[i], 1);
                }
            }
        }
        // Service USB after output to ensure data starts transmitting
        out_stream->flush();
    }
    
    // UART Response Capture: If this command came from UART, also queue output there
    if (CommandBuffer::getInstance().shouldRespondToUART() && len > 0) {
        cmdBuffer.queueForUART((const uint8_t*)str, len);
        // Add carriage return for UART if the output ends with newline
        // if (str[len-1] == '\n') {
        //     cmdBuffer.queueForUART((const uint8_t*)"\r", 1);
        // }
    }
}

// HAL functions are now implemented in lib/micropython/port/mphalport.c

// Static helper function to check a stream for interrupts
// CRITICAL: Pass expected_interrupt_char for the stream being checked, not global keyboard_interrupt_char
// This allows Serial (Ctrl+Q=17) and USBSer2 (Ctrl+C=3) to have different interrupt characters
static inline bool check_stream_for_interrupt(Stream* stream, uint32_t current_time, 
                                               uint32_t& last_interrupt_time, 
                                               bool in_raw_repl,
                                               int expected_interrupt_char) {
  if (!stream) {
    // Serial.println("[MP] Interrupt check: No stream available");

     return false;
  }
  
  int available = stream->available();
  if (available == 0) return false;
  
  // CRITICAL: Do not consume non-control bytes here.
  // stdin/readline is the sole consumer for normal input characters.
  int c = stream->peek();

  // Soft reset request (Ctrl+D) - works on all streams
  if (c == 0x04) {
    stream->read(); // consume control byte
    mp_soft_reset_requested = true;
    // Schedule a SystemExit to unwind the VM immediately; the flag is then
    // handled (jl_soft_reboot) by the friendly-REPL outer loop in
    // enterMicroPythonREPLWithFile, or by MpRemoteService for raw REPL.
    mp_sched_exception(MP_OBJ_FROM_PTR(&mp_type_SystemExit));
    last_interrupt_time = current_time;
    return true;
  }

  // Keyboard interrupt character - use the stream-specific expected character
  if (c == MP_INTERRUPT_CHAR_SERIAL || c == MP_INTERRUPT_CHAR_USBSER2) {
    stream->read(); // consume control byte
    if (current_time - last_interrupt_time > 20) { // Debounce (20ms = responsive but not too sensitive)
      // CRITICAL: Set the flag FIRST to break out of mp_hal_delay_ms immediately
      mp_interrupt_requested = true;
      
      // THEN schedule the interrupt in MicroPython's VM
      mp_sched_keyboard_interrupt();
      last_interrupt_time = current_time;
      
      // Echo the interrupt character for user feedback (but not in raw REPL mode)
      if (global_mp_stream && !in_raw_repl) {
        if (expected_interrupt_char == MP_INTERRUPT_CHAR_SERIAL) {
          global_mp_stream->print("^Q"); // Ctrl+Q for Serial/Jerial
        } else if (expected_interrupt_char == MP_INTERRUPT_CHAR_USBSER2) {
          global_mp_stream->print("^C"); // Ctrl+C for USBSer2
        }
      }
    }
    return true;
  }
  
  return false;
}

// Function to check for interrupt and raise KeyboardInterrupt if requested
// NOTE: Uses mp_interrupt_check_stream instead of global_mp_stream to avoid
// consuming data from USBSer2 when it's being used as the output stream (e.g., for mpremote)
extern "C" void mp_hal_check_interrupt(void) {
  static uint32_t last_interrupt_time = 0;
  static uint32_t last_check_time = 0;
  static uint32_t last_gc_check_time = 0;

  // CRITICAL: Respect MicroPython's interrupt-disable convention FIRST, before
  // anything else. parse_compile_execute() calls mp_hal_set_interrupt_char(-1)
  // during exception handling. If mp_interrupt_requested was set (by the same
  // Ctrl+C that triggered the exception) but NOT yet cleared (because nlr_jump
  // skipped the clearing code), we MUST NOT re-schedule the interrupt here.
  // Re-scheduling would set mp_pending_exception AFTER the NLR context is gone,
  // causing nlr_jump_fail → infinite loop → USB disconnect on next
  // mp_handle_pending(true) call.
  if (keyboard_interrupt_char < 0) {
    // Silently discard any stale interrupt request from before the exception
    mp_interrupt_requested = false;
    return;
  }

  // Check interrupt flag before any throttling or expensive operations
  // This flag can be set from ISR or from stream detection and needs immediate response
  if (mp_interrupt_requested) {
    // Schedule the interrupt in MicroPython's VM
    mp_sched_keyboard_interrupt();
    // IMMEDIATELY clear the flag to prevent duplicate scheduling
    // Note: mp_hal_delay_ms checks the flag BEFORE calling this function,
    // so it will see the flag and break out before we clear it here
    mp_interrupt_requested = false;
    last_interrupt_time = millis();
    return;
  }

  uint32_t current_time = millis();
  
  // CRITICAL: Service USB FIRST, BEFORE any throttling checks
  // This ensures Ctrl+C bytes move from USB hardware → CDC buffer even when throttled
  // Without this, interrupts during time.sleep() won't work!
  TinyUSB_Device_Task(); // MUST be called every time, regardless of throttling
  
  // PROACTIVE MEMORY MANAGEMENT: Check for low memory and trigger GC if needed
  // This prevents MemoryError during long-running scripts by freeing memory before it's critically low
  // Only check every 500ms to avoid overhead - GC is expensive (~50-100ms on embedded systems)
  if (current_time - last_gc_check_time >= 500) {
    last_gc_check_time = current_time;
    
    #if MICROPY_ENABLE_GC
    // Get free memory without triggering GC
    gc_info_t info;
    gc_info(&info);
    
    // Trigger GC if less than 5KB free (critical threshold for script execution)
    // This ensures we always have breathing room for allocations
    const size_t CRITICAL_FREE_THRESHOLD = 5 * 1024;  // 5KB
    if (info.free < CRITICAL_FREE_THRESHOLD) {
      // Memory is getting critically low - run GC to free up space
      // This prevents MemoryError from occurring mid-script
      // CRITICAL: Use gc_collect_safe() to pause Core 2 first - GC finalisers
      // may access the filesystem, which can corrupt if Core 2 is also accessing it
      gc_collect_safe();
    }
    #endif
  }
  
  // Throttle stream polling to avoid excessive overhead
  // NOTE: USB servicing (tud_task) is ABOVE this check so it always runs
  if (current_time - last_check_time < interruptCheckInterval) {
    return;
  }
  last_check_time = current_time;

  // Cache in_raw_repl check once instead of calling getInstance() multiple times
  const bool in_raw_repl = MpRemoteService::getInstance().isInRawRepl();

  // ALWAYS check BOTH Serial and USBSer2 for interrupts.
  // check_stream_for_interrupt only peeks/consumes control bytes (Ctrl+C, Ctrl+Q, Ctrl+D)
  // so checking both is safe — no normal data bytes are stolen.
  // This ensures that either Ctrl+C or Ctrl+Q works on either port, regardless of
  // which stream is the "active" one. Previously, only one stream was checked,
  // meaning interrupts typed on the wrong port were silently dropped.
  if (check_stream_for_interrupt(&Serial, current_time, last_interrupt_time,
                                 in_raw_repl, MP_INTERRUPT_CHAR_SERIAL)) {
    return;
  }
  if (check_stream_for_interrupt(&USBSer2, current_time, last_interrupt_time,
                                 in_raw_repl, MP_INTERRUPT_CHAR_USBSER2)) {
    return;
  }

  // Click-menu path: MEDUIM_HELD (3-second hold) always acts as KeyboardInterrupt.
  // The encoder state machine drives a logo LED animation from white→red during
  // the hold, then fires MEDIUM_HELD at 3 seconds.  We always honour it regardless
  // of clickWheelPythonInterrupt because a 3-second deliberate hold is unambiguous.
  if (encoderButtonState == MEDIUM_HELD &&
      millis() >= clickwheel_interrupt_ignore_until) {
    mp_interrupt_requested = true;
    mp_sched_keyboard_interrupt();
    encoderButtonState = IDLE;  // Consume the button event
    if (global_mp_stream) {
      global_mp_stream->println("\r\nKeyboardInterrupt (clickwheel)");
    }
  }
}




// Execute a user boot script (boot.py) once at startup, like standard MicroPython boards.
static void runBootPyIfPresent(void) {
  if (!mp_initialized) {
    return;
  }

  static bool boot_py_executed = false;
  if (boot_py_executed) {
    return;
  }
  boot_py_executed = true;

  changeTerminalColor(replColors[13], true, global_mp_stream);
  mp_embed_exec_str(
    "import sys\n"
    "candidates = [\n"
    "    'boot.py',\n"
    "    '/python_scripts/boot.py',\n"
    "    '/python_scripts/lib/boot.py',\n"
    "    '/python_scripts/modules/boot.py',\n"
    "    '/python_scripts/examples/boot.py',\n"
    "]\n"
    "for _path in candidates:\n"
    "    try:\n"
    "        f = open(_path)\n"
    "    except OSError:\n"
    "        continue\n"
    "    try:\n"
    "        __file__ = _path\n"
    "        exec(f.read(), globals())\n"
    "    finally:\n"
    "        f.close()\n"
    "    break\n"
  );
}

bool initMicroPythonProper(Stream *stream, bool preserve_interrupt_char) {
  // global_mp_stream = stream;
 
  if (mp_initialized) {
    return true;
  }

  // global_mp_stream->println(
  //     "[MP] Initializing MicroPython...");

  // Get proper stack pointer
  char stack_dummy;
  char *stack_top = &stack_dummy;

  // Allocate heap if needed (first boot or after PSRAM config change freed it)
  if (!mp_heap) {
    mp_heap_size = jumperlessConfig.hardware.psram_installed
        ? MICROPY_HEAP_SIZE_PSRAM : MICROPY_HEAP_SIZE;
    mp_heap = (unsigned char *)malloc(mp_heap_size);
    if (!mp_heap) {
      global_mp_stream->printf("[MP] FATAL: failed to malloc %d KB heap\n",
          (int)(mp_heap_size / 1024));
      return false;
    }
  }

  changeTerminalColor(replColors[11], true, global_mp_stream);
  mp_embed_init(mp_heap, mp_heap_size, stack_top);

  // Mount Jumperless filesystem into MicroPython's VFS so tools can use standard os/open
  jl_vfs_mount_root();

  // Configure keyboard interrupt character
  // For raw REPL (mpremote/ViperIDE) we must preserve the default Ctrl+C
  if (!preserve_interrupt_char) {
    // Set Ctrl+Q (ASCII 17) as the keyboard interrupt character instead of Ctrl+C (ASCII 3)
    // This enables proper KeyboardInterrupt exceptions that can be caught by try/except
    // and will automatically interrupt running loops/scripts when Ctrl+Q is pressed
    mp_embed_exec_str("import micropython; micropython.kbd_intr(17)");
  } else {
    keyboard_interrupt_char = 3; // Keep MicroPython default when host manages it
  }

  // Simple initialization - don't load complex modules during startup
  // mp_embed_exec_str("print('MicroPython ready for Jumperless')");
  changeTerminalColor(replColors[11], true, global_mp_stream);
  // Set up filesystem and module import paths

  setupFilesystemAndPaths();

  // Provision rp2.py on the filesystem if it doesn't exist.
  // This embeds the full PIO assembler (@asm_pio decorator) so that
  // 'import rp2' works out of the box without uploading any files.
  // It lives in /python_scripts/lib/ alongside jumperless.py — that path
  // is already on sys.path (see setupFilesystemAndPaths above), so the
  // import resolves cleanly via VFS without any Python eval gymnastics.
  // Writing on every init means accidental deletion self-heals next boot.
#if !defined(OG_JUMPERLESS)
  {
    const char *rp2_path = "/python_scripts/lib/rp2.py";
    if (!safeFileExists(rp2_path, 500)) {
      // Make sure the parent directories exist (idempotent — safeMkdir
      // returns true when the directory is already there).
      safeMkdir("/python_scripts", 2000);
      safeMkdir("/python_scripts/lib", 2000);
      writeStringToFile(rp2_path, rp2_py_content);
    }
  }
#else
  // OG (RP2040): skip rp2.py provisioning. The ~7.7 KB writeStringToFile spikes
  // ~10 KB of C heap, which collides with the larger MicroPython GC heap on the
  // tight 264 KB SRAM. The PIO @asm_pio helper isn't needed for the minimal OG
  // goal (LLM + basic MicroPython control); `import rp2` simply won't resolve.
#endif

    
  mp_initialized = true;
  mp_repl_active = false;

  addJumperlessPythonFunctions();

  changeTerminalColor(replColors[11], true, global_mp_stream);
  addMicroPythonModules();

  // Execute user boot script if present (boot.py), like standard MicroPython boards
  runBootPyIfPresent();

  changeTerminalColor(replColors[11], true, global_mp_stream);
  //global_mp_stream->println("[MP] MicroPython initialized successfully");

  changeTerminalColor(replColors[11], true, global_mp_stream);
  //global_mp_stream->println("[MP] interrupt char: " + String(keyboard_interrupt_char));
  
  // Execute ViperIDE reinit script when connecting to USBSer2 (mpremote/ViperIDE)
  // This sends filesystem information that ViperIDE/mpremote tools expect on connection
  // #ifdef INCLUDE_VIPERIDE_REINIT
  // if (global_mp_stream == &USBSer2) {
  //   mp_embed_exec_str(VIPERIDE_REINIT_PY);
  // }
  // #endif
  
  return true;
}

void deinitMicroPythonProper(void) {
  if (mp_initialized) {
    global_mp_stream->println("[MP] Deinitializing MicroPython...");
    
    // Close any open files before deinitializing MicroPython
    closeAllOpenFiles();

    // Tear down any transient OLED GUI screens a script left active so the
    // background render service doesn't keep driving (or dangling on) freed
    // state. A screen the script deliberately marked persistent (the idle/logo
    // replacement) is kept alive and keeps rendering after Python goes away.
    extern void oledGuiShutdownTransient(void);
    oledGuiShutdownTransient();
    
    mp_embed_deinit();
    mp_initialized = false;
    mp_repl_active = false;
    jumperless_globals_loaded = false;  // Reset globals flag
    mp_interrupt_requested = false;  // Clear any pending interrupt

    pauseCore2 = false;
  }
}

void reinitMicroPythonForPsramChange(void) {
  size_t new_size = jumperlessConfig.hardware.psram_installed
      ? MICROPY_HEAP_SIZE_PSRAM : MICROPY_HEAP_SIZE;

  if (new_size == mp_heap_size && mp_initialized) {
    return;
  }
  changeTerminalColor(replColors[11], true, global_mp_stream);

  Serial.printf("\n[MP] PSRAM config changed (%d KB -> %d KB) — saving config and rebooting...\n",
      (int)(mp_heap_size / 1024), (int)(new_size / 1024));
  Serial.flush();
  saveConfig();
  Serial.flush();
  delay(1000);
  rp2040.reboot();
}

void startMicroPythonREPL(void) {
  if (!mp_initialized) {
    changeTerminalColor(replColors[4], true, global_mp_stream);
    global_mp_stream->println("[MP] Error: MicroPython not initialized");
    return;
  }

  if (mp_repl_active) {
    changeTerminalColor(replColors[4], true, global_mp_stream);
    global_mp_stream->println("[MP] REPL already active");
    return;
  }

  // Clear any pending interrupt flag when starting REPL
  mp_interrupt_requested = false;

  // Print Python prompt with color
  changeTerminalColor(replColors[1], true, global_mp_stream);
  global_mp_stream->print(
      ">>> "); // Simple prompt - the processMicroPythonInput handles everything
  global_mp_stream->flush();

  mp_repl_active = true;
}

void stopMicroPythonREPL(void) {
  if (mp_repl_active) {
    changeTerminalColor(0, false, global_mp_stream);
    global_mp_stream->println("\n\r[MP] Exiting REPL...");
    
    // Restore to entry state (discard Python changes by default)
    jl_exit_micropython_restore_entry_state();
    
    // Close any open files before exiting REPL
    closeAllOpenFiles();
    
    // Give filesystem time to fully sync
    delay(50);
    yield();
    
    s_line_coding_override = false;
    
    mp_repl_active = false;
    mp_interrupt_requested = false; // Clear any pending interrupt
    pauseCore2 = false;
  }
}

bool isMicroPythonREPLActive(void) { return mp_repl_active; }

// Helper function to set initial file for REPL
void setREPLInitialFile(const String& filepath) {
  repl_initial_filepath = filepath;
  repl_has_initial_file = (filepath.length() > 0);
}



void enterMicroPythonREPL(Stream *stream) {
  enterMicroPythonREPLWithFile(stream, "");
}

void enterMicroPythonREPLWithFile(Stream *stream, const String& filepath) {
  // Save the original stream for this REPL session
  // CRITICAL: Don't use global_mp_stream in the loop because it can be changed
  // by MpRemoteService when ViperIDE/mpremote connects on USBSer2
  Stream* repl_stream = stream;
  
  // Push PYTHON_REPL context onto the stack for proper navigation
  ContextEntry ctx(ContextType::PYTHON_REPL);
  ctx.onExit = [](void*) {
    // Cleanup callback - close any open files
    extern void closeAllFiles();
    closeAllFiles();
    // Clear transfer path to prevent stale file paths from persisting
    ContextManager::getInstance().clearTransferPath();
    // Clear shared buffer if it wasn't consumed
    SharedBuffer::getInstance().clear();
  };
  
  // PRIORITY 1: Check SharedBuffer first (fastest - no file I/O needed)
  // This is the preferred path from Ekilo editor for instant transfers
  SharedBuffer& sharedBuf = SharedBuffer::getInstance();
  
  String fileToLoad = filepath;
  bool contentFromSharedBuffer = false;
  
  // Helper lambda to check if file is a Python file
  auto isPythonFile = [](const String& path) -> bool {
    return path.endsWith(".py") || path.endsWith(".PY");
  };
  
  if (sharedBuf.isReady() && sharedBuf.hasContent()) {
    // Content is already in memory from Ekilo - we'll load it in processMicroPythonInput
    // Just get the filename if available
    if (sharedBuf.hasFilename()) {
      String bufferFilename = sharedBuf.getFilename();
      // Only use if it's a Python file
      if (isPythonFile(bufferFilename)) {
        fileToLoad = bufferFilename;
        contentFromSharedBuffer = true;
      } else {
        // Not a Python file - clear the shared buffer and ignore
        sharedBuf.clear();
        changeTerminalColor(replColors[5], true, repl_stream);
        repl_stream->print("[!] Ignoring non-.py file: ");
        repl_stream->println(bufferFilename);
        changeTerminalColor(replColors[0], false, repl_stream);
      }
    } else {
      // No filename but has content - assume it's Python code
      contentFromSharedBuffer = true;
    }
    // DON'T clear shared buffer here if valid - it will be consumed in processMicroPythonInput
  }
  // PRIORITY 2: Check transfer path (file path was set, need to load from file)
  else if (fileToLoad.length() == 0 && ContextManager::getInstance().hasTransferPath()) {
    String transferPath = ContextManager::getInstance().getTransferPath();
    // IMPORTANT: Clear transfer path after consuming it (whether valid or not)
    // This prevents the original caller from re-loading the file after nested REPL exits
    ContextManager::getInstance().clearTransferPath();
    
    // Only use if it's a Python file
    if (isPythonFile(transferPath)) {
      fileToLoad = transferPath;
    } else {
      changeTerminalColor(replColors[5], true, repl_stream);
      repl_stream->print("[!] Ignoring non-.py file: ");
      repl_stream->println(transferPath);
      changeTerminalColor(replColors[0], false, repl_stream);
    }
  }
  
  // Final check on fileToLoad from filepath parameter
  if (fileToLoad.length() > 0 && !isPythonFile(fileToLoad) && !contentFromSharedBuffer) {
    changeTerminalColor(replColors[5], true, repl_stream);
    repl_stream->print("[!] Ignoring non-.py file: ");
    repl_stream->println(fileToLoad);
    changeTerminalColor(replColors[0], false, repl_stream);
    fileToLoad = "";  // Clear it - not a valid Python file
  }
  
  ContextManager::getInstance().pushContext(ctx);

  // Colorful initialization like original implementation
  changeTerminalColor(replColors[6], true, repl_stream);

  // Initialize MicroPython if not already done
  if (!mp_initialized) {
    if (!initMicroPythonProper()) {
      changeTerminalColor(replColors[4], true, repl_stream); // error color
      repl_stream->println("Failed to initialize MicroPython!");
      ContextManager::getInstance().popContext();  // Pop on error
      return;
    }
  }
  
  // CRITICAL: Ensure stream and interrupt character are set correctly for THIS REPL session
  // This must happen BEFORE any MicroPython code execution to ensure proper interrupt handling
  setGlobalStreamWithInterrupt(repl_stream);
  
  // Always add jumperless functions to global namespace when entering REPL
  // This makes all functions available without the jumperless. prefix
  addJumperlessPythonFunctions();
  // Initialize local copy of current nodefile for faster operations
  jl_init_micropython_local_copy();

  // Automatically create MicroPython examples if needed
  initializeMicroPythonExamples();

  // Check if REPL is already active
  if (mp_repl_active) {
    changeTerminalColor(replColors[4], true, repl_stream);
    repl_stream->println("[MP] REPL already active");
    return;
  }

  // Set initial file if provided (uses transfer path if available)
  if (fileToLoad.length() > 0) {
    setREPLInitialFile(fileToLoad);
  }

  // Show colorful welcome messages
  // changeTerminalColor(replColors[7], true,repl_stream);
  // repl_stream->println("MicroPython REPL with embedded Jumperless
  // hardware control!"); repl_stream->println("Type normal Python code,
  // then press Enter to execute"); repl_stream->println("Use TAB for
  // indentation (or exactly 4 spaces)"); repl_stream->println("Use ↑/↓
  // arrows for command history, ←/→ arrows for cursor movement");
  // repl_stream->println("Navigate multiline code with ← to beginning of
  // lines"); repl_stream->println("Type help_jumperless() for hardware
  // control commands");

  changeTerminalColor(replColors[0], true, repl_stream);
  repl_stream->println("MicroPython initialized successfully");
  repl_stream->flush();
  //delay(200);

  showREPLreference();

  //! This is to break the Python App's input loop
  repl_stream->println();

  bool wasInteractive = false;
  if (repl_stream == &Serial || repl_stream == &Jerial) {
    wasInteractive = ( termInInteractiveMode == 1 );
    setTerminalLineBuffering( true ); // turn on interactive mode for the REPL
  }

  Serial.write("\x1b[0 q");

  // The "press enter to start" prompt/wait only exists to break the app's cooked
  // line-input loop. If the app is already interactive (forwarding raw keys),
  // start the REPL immediately instead of waiting for (and consuming) an Enter.
  if (!wasInteractive) {
    changeTerminalColor(replColors[4], true, repl_stream);
    repl_stream->print("\n\rPress enter to start REPL");
    repl_stream->println();
    repl_stream->flush();

    extern void jl_service_python(void);
    unsigned long lastWaitServiceTime = millis();
    while (repl_stream->available() == 0) {
      // Run services while waiting for user input (including MpRemoteService)
      if (millis() - lastWaitServiceTime >= 50) {
        jl_service_python();
        lastWaitServiceTime = millis();
      }
      delayMicroseconds(1);
    }

    repl_stream->read(); // consume the enter keypress
  }

  // Start the REPL with colors
  changeTerminalColor(replColors[1], true, repl_stream);
  startMicroPythonREPL();

  // Blocking loop - stay in REPL until user exits
  extern void jl_service_python(void);  // From JumperlessMicroPythonAPI.cpp
  unsigned long lastServiceTime = millis();
  
  while (mp_repl_active) {
    // SAFETY NET: Wrap the REPL iteration in an NLR handler to catch any
    // stray exceptions that escape mp_embed_exec_str()'s catch frame.
    // After script execution, nlr_top == NULL. If ANY subsequent MicroPython
    // API call (gc_collect finaliser, vstr_add_byte in event REPL, etc.)
    // triggers nlr_raise, it would hit nlr_jump_fail -> device crash.
    // This wrapper ensures such exceptions are caught and the REPL continues.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
      // CRITICAL: Use repl_stream (the original stream), not global_mp_stream
      // because global_mp_stream can be changed by MpRemoteService when ViperIDE connects
      processMicroPythonInput(repl_stream);

      // Honor a Ctrl-D soft-reset request now that the script/input has fully
      // unwound (no Python frames on the stack). Previously the flag was
      // silently swallowed in mp_hal_delay_ms, so a friendly-REPL "soft reset"
      // never ran machine_pin_irq_deinit / VM reinit — Pin IRQs and Timer
      // alarms stayed armed against a VM the user believed was reset.
      if (mp_soft_reset_requested) {
        mp_soft_reset_requested = false;
        mp_interrupt_requested = false;
        changeTerminalColor(replColors[4], true, repl_stream);
        repl_stream->println("\r\nsoft reboot");
        jl_soft_reboot();
        setGlobalStreamWithInterrupt(repl_stream);
        changeTerminalColor(replColors[1], true, repl_stream);
        repl_stream->print(">>> ");
        repl_stream->flush();
      }

      // Run essential services every 50ms to keep measurements and animations running
      // This includes MpRemoteService which handles ViperIDE/mpremote on USBSer2
      if (millis() - lastServiceTime >= 50) {
        jl_service_python();
        lastServiceTime = millis();
      }
      nlr_pop();
    } else {
      // Caught an exception that escaped all inner handlers.
      // This prevents nlr_jump_fail -> device crash / USB disconnect.
      mp_hal_set_interrupt_char(-1);
      // MicroPython >= 1.28: CLEAR_EXCEPTIONS == the old `false`.
      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_CLEAR_EXCEPTIONS);
      mp_interrupt_requested = false;
      MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
      mp_stdin_locked_stream_ptr = nullptr;  // Clear stdin lock on exception
      
      // Print diagnostic info using Serial directly (bypasses MicroPython I/O).
      // NOTE: Do NOT call mp_obj_print_exception here — it's a C function and
      // C++ name mangling causes linker errors. Serial.printf is safe and sufficient
      // since the primary exception handler in mp_embed_exec_str already printed
      // the traceback before we got here.
      Serial.printf("\r\n[REPL] Caught stray exception (safety net) val=%p\r\n", nlr.ret_val);
      
      // CRITICAL: If the exception escaped while MpRemoteService had control
      // (global_mp_stream was USBSer2), ViperIDE may be stuck waiting for the
      // raw REPL completion markers. Send \x04\x04> to unblock it.
      // This is safe even if markers were already sent — extra \x04s just
      // produce an empty response that ViperIDE handles gracefully.
      if (MpRemoteService::getInstance().isInRawRepl() && USBSer2 &&
          jl_cdc_wait_writable(&USBSer2, 3, 500)) {
        USBSer2.write('\x04');
        USBSer2.write('\x04');
        USBSer2.write('>');
        USBSer2.flush();
        yield();
      }
      
      // Restore stream state for next REPL iteration
      setGlobalStreamWithInterrupt(repl_stream);
    }
    
    delayMicroseconds(1); // Small delay to prevent overwhelming
  }

  pauseCore2 = false;

  // Cleanup with colors
  changeTerminalColor(replColors[0], true, repl_stream);

  repl_stream->println("\n\rExiting REPL...");
  repl_stream->flush();
  delay(10);

  if (repl_stream == &Serial) {
    // repl_stream->write(0x0F); // turn off interactive mode
    // repl_stream->flush();
    //delay(100); // Give system time to switch modes
    // repl_stream->write(0x0E); // turn interactive mode back on for main menu
    // repl_stream->flush();
  }
  repl_stream->print("\033[0m");
  // stream->println("Returned to Arduino mode");
  
  // Clear screen before returning to parent context (file manager, etc.)
  repl_stream->print("\x1b[2J\x1b[H");
  repl_stream->flush();
  
  // Pop PYTHON_REPL context - cleanup callback will be called
  ContextManager::getInstance().popContext();
}

void processMicroPythonInput(Stream *stream) {
  
  if (!mp_initialized) {
    return;
  }

  // CRITICAL: ALWAYS set global_mp_stream to this REPL's stream at the start.
  // MpRemoteService::service() switches global_mp_stream to USBSer2 every ~50ms
  // in the outer loop. Without unconditionally restoring here, ALL output from
  // the built-in REPL (prompts, script output, color changes) can go to USBSer2,
  // corrupting ViperIDE's raw REPL protocol and losing output on Serial.
  if (stream) {
    setGlobalStreamWithInterrupt(stream);
  }

  // Handle REPL input with proper text editor functionality and history
  if (mp_repl_active) {
    static REPLEditor editor;
    static ScriptHistory history;  // Each REPL instance gets its own history
    static bool history_initialized = false;

    // PRIORITY 1: Check SharedBuffer first (zero-copy from Ekilo editor)
    // This is the fastest path - content is already in memory, no file I/O needed
    SharedBuffer& sharedBuf = SharedBuffer::getInstance();
    
    // Only print debug when there's actually content to avoid spam
    if (sharedBuf.isReady() && sharedBuf.hasContent()) {
      // Reset the editor to ensure we can load the content
      editor.reset();
      
      // Initialize history if needed
      if (!history_initialized) {
        history.initFilesystem();
        history_initialized = true;
      }
      
      // Get buffer info
      size_t bufLen = sharedBuf.length();
      const char* bufData = sharedBuf.data();
      String filename = sharedBuf.hasFilename() ? sharedBuf.getFilename() : "buffer";
      
      // Create String from SharedBuffer - must copy before clearing!
      String fileContent;
      if (bufLen > 0 && bufData) {
        fileContent.concat(bufData, bufLen);
      }
      
      // Store the source filename for history tracking
      editor.source_filename = filename;
      
      // Clear the buffer now that we've fully copied content
      sharedBuf.clear();
      
      // Show the loaded content using existing method
      editor.loadScriptContent(fileContent, "Script loaded from buffer: " + filename);
      
      // Clear any pending file (shared buffer takes priority)
      repl_initial_filepath = "";
      repl_has_initial_file = false;
      editor.first_run = false;
      
      return;
    }
    
    // PRIORITY 2: Check for pending initial file to load from filesystem
    if (repl_has_initial_file && repl_initial_filepath.length() > 0) {
      // Reset the editor to ensure we can load the file
      editor.reset();
      
      // Initialize history if needed
      if (!history_initialized) {
        history.initFilesystem();
        history_initialized = true;
      }

      // Load the file content into the editor using safe file functions
      File file = safeFileOpen(repl_initial_filepath.c_str(), "r", 2000);
      if (file) {
        String fileContent = file.readString();
        safeFileClose(file, false);  // Read-only, no flush

        // Show the loaded content
        editor.loadScriptContent(fileContent, "Script loaded from file: " + repl_initial_filepath);
      } else {
        changeTerminalColor(replColors[4], true, global_mp_stream);
        global_mp_stream->println("Failed to load file: " + repl_initial_filepath);
        changeTerminalColor(replColors[1], true, global_mp_stream);
      }
      
      // Clear the pending file
      repl_initial_filepath = "";
      repl_has_initial_file = false;
      editor.first_run = false;
      return;
    }

    if (editor.first_run) {
      // Initialize history first, before any input processing
      if (!history_initialized) {
        history.initFilesystem();
        history_initialized = true;
      }

      // Start with a fresh prompt
      changeTerminalColor(replColors[1], true, global_mp_stream);
      global_mp_stream->flush();
      editor.first_run = false;
    }

    // Check for standalone ESC timeout (if ESC was pressed but no '[' followed within 10ms)
    if (editor.escape_state == 1 && (millis() - editor.escape_start_time) > 10) {
      // Standalone ESC detected - interrupt and quit REPL
      editor.escape_state = 0; // Reset escape state
      
      global_mp_stream->println("^["); // Show ESC was pressed
      
      // Set interrupt flag for script execution
      mp_interrupt_requested = true;
      
      // Exit REPL
      changeTerminalColor(replColors[4], true, global_mp_stream);
      global_mp_stream->println("KeyboardInterrupt (ESC)");
      global_mp_stream->println("Exiting REPL...");
      changeTerminalColor(replColors[1], true, global_mp_stream);
      history.forceFlush();  // Save history before exit
      stopMicroPythonREPL();
      
      editor.reset();
      return;
    }

    // Periodic history flush check during idle time
    history.checkPeriodicFlush();

    // Input batching state for responsive arrow key handling
    static uint32_t last_arrow_time = 0;
    static int arrow_repeat_count = 0;
    static int last_arrow_direction = 0;  // 65=up, 66=down, 67=right, 68=left
    
    // Aggressive timeout check for stale queued arrow keys (key released)
    if (last_arrow_direction != 0) {
      uint32_t gap = millis() - last_arrow_time;
      if (gap > 35) {  // 25ms gap = key released, be aggressive
        // Clear ALL pending arrow keys from buffer (any direction)
        int cleared = 0;
        while (global_mp_stream->available() >= 3 && cleared < 50) {
          // Peek for ESC [ arrow pattern
          int b0 = global_mp_stream->peek();
          if (b0 != 27) break;  // Not an escape sequence
          global_mp_stream->read();  // consume ESC
          if (!global_mp_stream->available()) break;
          int b1 = global_mp_stream->read();
          if (b1 != 91) break;  // Not [
          if (!global_mp_stream->available()) break;
          int b2 = global_mp_stream->peek();
          if (b2 >= 65 && b2 <= 68) {
            global_mp_stream->read();  // Discard ANY arrow key
            cleared++;
          } else {
            break;  // Not an arrow, stop
          }
        }
        last_arrow_direction = 0;
        arrow_repeat_count = 0;
      }
    }

    // Check for available input
    if (global_mp_stream->available()) {
      int c = global_mp_stream->read();

      // Character processing for escape sequences

      // Handle escape sequences for arrow keys
      if (editor.escape_state == 0 && c == 27) { // ESC
        editor.escape_state = 1;
        editor.escape_start_time = millis(); // Record when ESC was pressed
        return;
      } else if (editor.escape_state == 1 && c == 91) { // [
        editor.escape_state = 2;
        return;
      } else if (editor.escape_state == 2) {
        editor.escape_state = 0; // Reset escape state

        switch (c) {
        case 65: //TODO: Up arrow - history previous
        {
          // Track arrow for batching with aggressive timeout
          uint32_t now = millis();
          int moves = 1;
          uint32_t since_last = now - last_arrow_time;
          if (last_arrow_direction == 65 && since_last < 40) {
            arrow_repeat_count++;
            if (arrow_repeat_count > 4 && since_last < 30) moves = 2;  // Conservative acceleration
          } else {
            arrow_repeat_count = 1;  // Reset on direction change or timeout
          }
          last_arrow_direction = 65;
          last_arrow_time = now;
          
          // Only allow history navigation if:
          // 1. Current input is empty (blank prompt), OR
          // 2. We just loaded from history and no other keys were pressed
          bool allow_history = (editor.current_input.length() == 0) || 
                               (editor.just_loaded_from_history);
          
          if (allow_history) {
            String prev_cmd = history.getPreviousCommand();
            if (prev_cmd.length() > 0) {
              editor.loadFromHistory(global_mp_stream, prev_cmd);
              global_mp_stream->flush();
            }
          } else if (editor.in_multiline_mode) {
            // In multiline mode, move cursor up (with batched moves)
            for (int i = 0; i < moves; i++) {
              editor.moveCursorUp();
            }
            editor.repositionCursorOnly(global_mp_stream);
          }
          // If neither history nor multiline, do nothing
        }
          return;

        case 66: // Down arrow - history next
        {
          // Track arrow for batching with aggressive timeout
          uint32_t now = millis();
          int moves = 1;
          uint32_t since_last = now - last_arrow_time;
          if (last_arrow_direction == 66 && since_last < 40) {
            arrow_repeat_count++;
            if (arrow_repeat_count > 4 && since_last < 30) moves = 2;  // Conservative acceleration
          } else {
            arrow_repeat_count = 1;  // Reset on direction change or timeout
          }
          last_arrow_direction = 66;
          last_arrow_time = now;
          
          // Only allow history navigation if:
          // 1. We're currently in history mode, OR
          // 2. Current input is empty (blank prompt), OR  
          // 3. We just loaded from history and no other keys were pressed
          bool allow_history = (editor.in_history_mode) ||
                               (editor.current_input.length() == 0) || 
                               (editor.just_loaded_from_history);
          
          if (allow_history) {
            String next_cmd = history.getNextCommand();
            if (next_cmd.length() > 0) {
              editor.loadFromHistory(global_mp_stream, next_cmd);
              global_mp_stream->flush();
            } else {
              // Return to original input
              editor.exitHistoryMode(global_mp_stream);
              global_mp_stream->flush();
            }
          } else if (editor.in_multiline_mode) {
            // In multiline mode, move cursor down (with batched moves)
            for (int i = 0; i < moves; i++) {
              editor.moveCursorDown();
            }
            editor.repositionCursorOnly(global_mp_stream);
          }
          // If neither history nor multiline, do nothing
        }
          return;

        case 67: // Right arrow
        {
          // Track arrow for batching with aggressive timeout
          uint32_t now = millis();
          int moves = 1;
          uint32_t since_last = now - last_arrow_time;
          if (last_arrow_direction == 67 && since_last < 40) {
            arrow_repeat_count++;
            if (arrow_repeat_count > 4 && since_last < 30) moves = 2;  // Conservative acceleration
          } else {
            arrow_repeat_count = 1;  // Reset on direction change or timeout
          }
          last_arrow_direction = 67;
          last_arrow_time = now;
          
          // Exit history mode when user starts navigating
          if (editor.in_history_mode) {
            editor.in_history_mode = false;
            editor.just_loaded_from_history = false;
            history.resetHistoryNavigation();
          }
          
          for (int i = 0; i < moves && editor.cursor_pos < editor.current_input.length(); i++) {
            editor.moveCursorRight();
          }
          editor.repositionCursorOnly(global_mp_stream);
        }
          return;

        case 68: // Left arrow
        {
          // Track arrow for batching with aggressive timeout
          uint32_t now = millis();
          int moves = 1;
          uint32_t since_last = now - last_arrow_time;
          if (last_arrow_direction == 68 && since_last < 40) {
            arrow_repeat_count++;
            if (arrow_repeat_count > 4 && since_last < 30) moves = 2;  // Conservative acceleration
          } else {
            arrow_repeat_count = 1;  // Reset on direction change or timeout
          }
          last_arrow_direction = 68;
          last_arrow_time = now;
          
          // Exit history mode when user starts navigating
          if (editor.in_history_mode) {
            editor.in_history_mode = false;
            editor.just_loaded_from_history = false;
            history.resetHistoryNavigation();
          }
          
          for (int i = 0; i < moves && editor.cursor_pos > 0; i++) {
            editor.moveCursorLeft();
          }
          editor.repositionCursorOnly(global_mp_stream);
        }
          return;

        default:
          // Unknown escape sequence - just ignore it
          return;
        }
      } else if (editor.escape_state > 0) {
        // We're in the middle of an escape sequence but got an unexpected character
        editor.escape_state = 0; // Reset escape state
        // Don't process this character as regular input
        return;
      }

      // Handle configured interrupt character - force quit REPL or interrupt script
      if (c == keyboard_interrupt_char) {
        char char_display = (keyboard_interrupt_char >= 1 && keyboard_interrupt_char <= 26) ? 
                           (char)(keyboard_interrupt_char + 64) : '?';
        global_mp_stream->printf("^%c\n\r", char_display);
        
        // Set interrupt flag for script execution
        mp_interrupt_requested = true;
        
        // Always exit REPL immediately when interrupt char is pressed during input
        // This covers the case where user is typing but not executing a script
        changeTerminalColor(replColors[4], true, global_mp_stream);
        global_mp_stream->printf("KeyboardInterrupt (Ctrl+%c)\n\r", char_display);
        global_mp_stream->println("Force quit - exiting REPL...");
        changeTerminalColor(replColors[1], true, global_mp_stream);
        history.forceFlush();  // Save history before exit
        stopMicroPythonREPL();
        
        editor.reset();
        return;
      }

      // Handle Ctrl+S - instant save to history (crash-safe)
      if (c == 19) { // Ctrl+S = ASCII 19
        global_mp_stream->print("^S");
        if (editor.current_input.length() > 0) {
          // Save current input immediately to disk
          history.addToHistory(editor.current_input);
          history.forceFlush();
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("\r\n[Saved to history]");
          changeTerminalColor(replColors[1], true, global_mp_stream);
        } else {
          // No input - save last executed command as a script file
          String last = history.getLastExecutedCommand();
          if (last.length() > 0) {
            history.saveScript(last, "");  // Auto-generates filename
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("\r\n[Last script saved]");
            changeTerminalColor(replColors[1], true, global_mp_stream);
          } else {
            changeTerminalColor(replColors[4], true, global_mp_stream);
            global_mp_stream->println("\r\n[No input to save]");
            changeTerminalColor(replColors[1], true, global_mp_stream);
          }
        }
        editor.drawPrompt(global_mp_stream, 0);
        global_mp_stream->flush();
        return;
      }

      // Handle Enter key - check for multiline or execute
      if (c == '\r' || c == '\n') {
        
        global_mp_stream->println(); // Echo newline

        // Clear history flags at the start of enter processing
        editor.just_loaded_from_history = false;
        
        // Check for special commands first
        String trimmed_input = editor.current_input;
        trimmed_input.trim();

        //! Exit commands
        if (trimmed_input == "exit()" || trimmed_input == "quit()" ||
            trimmed_input == "exit" || trimmed_input == "quit") {
          history.forceFlush();  // Save history before exit
          stopMicroPythonREPL();
          editor.reset();
          return;
        }

        //! History commands
        if (trimmed_input == "history" || trimmed_input == "history()") {
          history.listScripts();
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Connection context toggle
        if (trimmed_input == "context" || trimmed_input == "context()") {
          jl_toggle_connection_context();
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->print("Connection context switched to: ");
          changeTerminalColor(replColors[2], true, global_mp_stream);
          global_mp_stream->println(jl_get_connection_context_name());
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          if (connectionContext == PYTHON_CONTEXT_GLOBAL) {
            global_mp_stream->println("   Changes will persist to global state");
            global_mp_stream->println("   Connections remain after exiting Python");
          } else {
            global_mp_stream->println("   Changes are isolated to Python session");
            global_mp_stream->println("   Connections cleared on exit (saved to slots/slotPython.yaml)");
          }
          
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Multiline mode commands
        if (trimmed_input == "multiline" || trimmed_input == "multiline()") {
          global_mp_stream->println("Multiline mode status:");
          if (editor.multiline_forced_on) {
            global_mp_stream->println("  Currently: FORCED ON");
          } else if (editor.multiline_forced_off) {
            global_mp_stream->println("  Currently: FORCED OFF");
          } else {
            global_mp_stream->println("  Currently: AUTO (default)");
          }
          global_mp_stream->println("Commands:");
          global_mp_stream->println("  multiline on   - Force multiline mode "
                                    "ON (use 'run' to execute)");
          global_mp_stream->println(
              "  multiline off  - Force multiline mode OFF");
          global_mp_stream->println(
              "  multiline auto - Return to automatic detection");
                  global_mp_stream->println(
            "  multiline edit - Use main eKilo editor for multiline input");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline on") {
          editor.multiline_forced_on = true;
          editor.multiline_forced_off = false;
          editor.multiline_override = true;
          global_mp_stream->println("Multiline mode: FORCED ON");
          changeTerminalColor(replColors[7], false, global_mp_stream);
          global_mp_stream->println("Enter will add new lines. Type 'run' to "
                                    "execute accumulated script.");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline off") {
          editor.multiline_forced_on = false;
          editor.multiline_forced_off = true;
          editor.multiline_override = true;
          global_mp_stream->println("Multiline mode: FORCED OFF");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline auto") {
          editor.multiline_forced_on = false;
          editor.multiline_forced_off = false;
          editor.multiline_override = false;
          global_mp_stream->println("Multiline mode: AUTO (default)");
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        if (trimmed_input == "multiline edit") {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Opening eKilo editor...");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          // CRITICAL: Pause Core2 during flash write operations
          // Create python_scripts directory if it doesn't exist
          safeMkdir("/python_scripts", 2000);
          
          // Save last executed command to temporary file using safe functions
          String tempFile = "/python_scripts/_temp_repl_edit.py";
          String lastCommand = history.getLastExecutedCommand();
          const char* contentToWrite;
          size_t contentLen;
          if (lastCommand.length() > 0) {
            contentToWrite = lastCommand.c_str();
            contentLen = lastCommand.length();
          } else {
            // Start with a helpful comment if no history
            contentToWrite = "# Edit your Python script here\n# Press Ctrl+S to save and return to REPL\n# Press Ctrl+P to save and execute immediately\n";
            contentLen = strlen(contentToWrite);
          }
          safeFileWriteAll(tempFile.c_str(), contentToWrite, contentLen, 2000);
          
          // Launch main eKilo editor with temporary file
          String savedContent = launchEkiloREPL(tempFile.c_str());
          
          // Restore interactive mode after returning from eKilo (raw input).
          if (global_mp_stream == &Serial) {
            setTerminalLineBuffering( true );
          }
          
          // Handle the return from eKilo
          if (savedContent.length() > 0) {
            // Check if this was a Ctrl+P (save and execute) request
            if (savedContent.startsWith("[LAUNCH_REPL]")) {
              // Remove the marker and execute the content
              String contentToExecute = savedContent.substring(13); // Remove "[LAUNCH_REPL]"
              if (contentToExecute.length() > 0) {
                changeTerminalColor(replColors[2], true, global_mp_stream);
                global_mp_stream->println("Executing script from eKilo:");
                
                // Add to history before execution
                history.addToHistory(contentToExecute);
                
                // Execute the script
                mp_stdin_locked_stream_ptr = (void*)global_mp_stream;  // Lock stdin
                mp_embed_exec_str(contentToExecute.c_str());
                mp_stdin_locked_stream_ptr = nullptr;  // Unlock stdin
                // Clear stale interrupt/exception state after script execution
                mp_interrupt_requested = false;
                MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
                yield(); // pump USB + flush the script's output
                // Force GC after script to close file handles and free memory
                // Use safe version with Core 2 synchronization
                gc_collect_safe();
                // CRITICAL: Close any files that weren't explicitly closed by the script
                // This prevents file handle leaks and filesystem conflicts
                jl_close_all_jfs_files();
                // Restore global_mp_stream to REPL's own stream
                setGlobalStreamWithInterrupt(stream);
              }
            } else {
              // Regular save - load content into REPL editor
              editor.current_input = savedContent;
              editor.cursor_pos = savedContent.length();
              editor.in_multiline_mode = (savedContent.indexOf('\n') >= 0);
              changeTerminalColor(replColors[5], true, global_mp_stream);
              global_mp_stream->println("Script loaded into REPL editor");
              // Don't reset - show the loaded content
              editor.drawFromCurrentLine(global_mp_stream);
              return;
            }
          } else {
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Returned from eKilo editor");
          }
          
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }



        //! New command - create new script with eKilo editor
        if (trimmed_input == "new" || trimmed_input == "new()") {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Opening eKilo editor...");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          // Launch eKilo editor in REPL mode
          String savedContent = launchEkiloREPL(nullptr);
          
          // Restore interactive mode after returning from eKilo (raw input).
          if (global_mp_stream == &Serial) {
            setTerminalLineBuffering( true );
          }
          
          // If content was saved, load it into the editor
          if (savedContent.length() > 0) {
            editor.loadScriptContent(savedContent, "Script content loaded into REPL");
            return;
          } else {
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Returned from eKilo editor");
            
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            editor.drawPrompt(global_mp_stream, 0);
            global_mp_stream->flush();
            return;
          }
        }

        //! Edit command - launch main eKilo editor for multiline editing
        if (trimmed_input == "edit" || trimmed_input == "edit()") {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Opening eKilo editor...");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          // Create python_scripts directory if it doesn't exist
          safeMkdir("/python_scripts", 2000);
          
          // Save last executed command to temporary file using safe functions
          String tempFile = "/python_scripts/_temp_repl_edit.py";
          String lastCommand = history.getLastExecutedCommand();
          const char* editContent;
          size_t editLen;
          if (lastCommand.length() > 0) {
            editContent = lastCommand.c_str();
            editLen = lastCommand.length();
          } else {
            // Start with a helpful comment if no history
            editContent = "# Edit your Python script here\n# Press Ctrl+S to save and return to REPL\n# Press Ctrl+P to save and execute immediately\n";
            editLen = strlen(editContent);
          }
          safeFileWriteAll(tempFile.c_str(), editContent, editLen, 2000);
          
          // Launch main eKilo editor with temporary file
          String savedContent = launchEkiloREPL(tempFile.c_str());
          
          // Restore interactive mode after returning from eKilo (raw input).
          if (global_mp_stream == &Serial) {
            setTerminalLineBuffering( true );
          }
          
          // Handle the return from eKilo
          if (savedContent.length() > 0) {
            // Check if this was a Ctrl+P (save and execute) request

              // Regular save - load content into REPL editor
              showREPLreference();
              editor.loadScriptContent(savedContent, "Script content loaded into REPL");
              // editor.current_input = savedContent;
              // editor.cursor_pos = savedContent.length();
              // editor.in_multiline_mode = (savedContent.indexOf('\n') >= 0);
              // changeTerminalColor(replColors[5], true, global_mp_stream);
              // global_mp_stream->println("Script loaded into REPL editor");
              // // Don't reset - show the loaded content
              // editor.drawFromCurrentLine(global_mp_stream);
              return;
            
          } else {
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Returned from eKilo editor");
          }
          
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Help commands
        if (trimmed_input == "helpl" || trimmed_input == "helpl()") {
          // Show REPL help
          changeTerminalColor(replColors[7], true, global_mp_stream);
          global_mp_stream->println("\n   MicroPython REPL Help");
          
          showREPLreference();

          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Save command - save the last executed script from history
        if (trimmed_input.startsWith("save ") || trimmed_input == "save" ||
            trimmed_input == "save()") {
          // Get the most recent executed script from history
          String last_script = history.getLastExecutedCommand();
          if (last_script.length() > 0) {
            String filename = "";
            if (trimmed_input.startsWith("save ")) {
              filename = trimmed_input.substring(5);
              filename.trim();
            }
            if (history.saveScript(last_script, filename)) {
              global_mp_stream->println("Script saved to filesystem");
            } else {
              global_mp_stream->println("Failed to save script");
            }
          } else {
            global_mp_stream->println("No previous script to save");
          }
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Load command - load a script from filesystem
        if (trimmed_input.startsWith("load ") || trimmed_input == "load") {
          if (trimmed_input == "load") {
            // Show available scripts when no filename provided
            global_mp_stream->println("Available scripts:");
            history.listScripts();
            String recent_script = history.getLastSavedScript();
            if (recent_script.length() > 0) {
              global_mp_stream->println("Most recent: " + recent_script);
              global_mp_stream->println("Try: load " + recent_script);
            }
            global_mp_stream->println(
                "Usage: load <number> or load <filename>");
          } else {
            String arg = trimmed_input.substring(5);
            arg.trim();
            if (arg.length() > 0) {
              String filename = "";

              // Check if argument is a number
              bool is_number = true;
              for (int i = 0; i < arg.length(); i++) {
                if (!isdigit(arg.charAt(i))) {
                  is_number = false;
                  break;
                }
              }

              if (is_number) {
                // Handle numeric input
                int script_number = arg.toInt();
                if (script_number >= 1 &&
                    script_number <= history.getNumberedScriptsCount()) {
                  filename = history.getNumberedScript(
                      script_number - 1); // Convert 1-based to 0-based
                  global_mp_stream->println("Loading script " +
                                            String(script_number) + ": " +
                                            filename);
                } else {
                  global_mp_stream->println(
                      "Invalid script number. Use 'history' to see available "
                      "scripts.");
                  editor.reset();
                  changeTerminalColor(replColors[1], true, global_mp_stream);
                  editor.drawPrompt(global_mp_stream, 0);
                  global_mp_stream->flush();
                  return;
                }
              } else {
                // Handle filename input
                filename = arg;
              }

              if (filename.length() > 0) {
                String loaded_script = history.loadScript(filename);
                if (loaded_script.length() > 0) {
                  // Load the script into the editor
                  showREPLreference();
                  editor.loadScriptContent(loaded_script, "Script loaded into REPL editor");
                  // editor.current_input = loaded_script;
                  // editor.cursor_pos = loaded_script.length();
                  // editor.in_multiline_mode = (loaded_script.indexOf('\n') >= 0);
                  // editor.redrawAndPosition(global_mp_stream);
                  return; // Stay in editing mode
                }
              }
            } else {
              global_mp_stream->println(
                  "Usage: load <number> or load <filename>");
            }
          }
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Delete command - delete a script from filesystem
        if (trimmed_input.startsWith("delete ") ||
            trimmed_input.startsWith("del ")) {
          int start_pos = trimmed_input.startsWith("delete ") ? 7 : 4;
          String filename = trimmed_input.substring(start_pos);
          filename.trim();
          if (filename.length() > 0) {
            history.deleteScript(filename);
          } else {
            global_mp_stream->println("Usage: delete filename");
          }
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
          return;
        }

        //! Files command - launch file manager in python_scripts directory
        if (trimmed_input == "files" || trimmed_input == "files()" ||
            trimmed_input == "filemanager" || trimmed_input == "filemanager()") {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Opening file manager...");
          changeTerminalColor(replColors[0], false, global_mp_stream);
          
          // Clear any existing transfer path and shared buffer before launching file manager
          ContextManager::getInstance().clearTransferPath();
          SharedBuffer::getInstance().clear();
          
          // Launch file manager in REPL mode
          String savedContent = filesystemAppPythonScriptsREPL();
          
          // Restore interactive mode after returning from file manager (raw input).
          if (global_mp_stream == &Serial) {
            setTerminalLineBuffering( true );
          }

          // PRIORITY 1: Check SharedBuffer first (fastest - already in memory)
          SharedBuffer& fileBuf = SharedBuffer::getInstance();
          if (fileBuf.isReady() && fileBuf.hasContent()) {
            showREPLreference();
            String content(fileBuf.data(), fileBuf.length());
            String filename = fileBuf.hasFilename() ? fileBuf.getFilename() : "buffer";
            fileBuf.clear();
            editor.loadScriptContent(content, String("Loaded from buffer: ") + filename);
            return;
          }
          
          // PRIORITY 2: Check if a file path was set via transfer mechanism
          bool hasContent = savedContent.length() > 0;
          bool hasTransferPath = ContextManager::getInstance().hasTransferPath();
          
          if (hasTransferPath) {
            // Load from file path
            const char* filePath = ContextManager::getInstance().getTransferPath();
            showREPLreference();
            
            // Read file content for editor using safe file functions
            File f = safeFileOpen(filePath, "r", 2000);
            if (f) {
              String content = f.readString();
              safeFileClose(f, false);  // Read-only, no flush
              // Store source filename for history tracking
              editor.source_filename = String(filePath);
              editor.loadScriptContent(content, String("Loaded: ") + filePath);
            } else {
              global_mp_stream->println("Failed to load file from transfer path");
              editor.reset();
            }
            ContextManager::getInstance().clearTransferPath();
            return;
          } else if (hasContent) {
            // Fallback: use returned content (backward compatibility)
            showREPLreference();
            editor.loadScriptContent(savedContent, "File content loaded into REPL");
            return;
          } else {
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Returned from file manager");
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            editor.drawPrompt(global_mp_stream, 0);
            global_mp_stream->flush();
            return;
          }
        }

        //! Special handling for forced multiline mode
        if (editor.multiline_forced_on) {
          // Check if the user typed 'run' (as the only content or last line)
          if (trimmed_input == "run") {
            // Only "run" was typed - no script to execute
            global_mp_stream->println("No script to execute.");
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            editor.drawPrompt(global_mp_stream, 0);
            global_mp_stream->flush();
            return;
          } else if (trimmed_input.endsWith("\nrun") &&
                     trimmed_input.length() > 4) {
            // Script followed by 'run' command
            String script_to_execute =
                trimmed_input.substring(0, trimmed_input.length() - 4);
            script_to_execute.trim();

            if (script_to_execute.length() > 0) {
              changeTerminalColor(replColors[2], true, global_mp_stream);
              global_mp_stream->println("Executing accumulated script:");

              // Execute the user's current input (edited or original)
              // No longer override with history command - user edits should be respected

              // Add to history before execution
              history.addToHistory(script_to_execute);

              // Reset history navigation now that we're executing
              history.resetHistoryNavigation();

              // Reset local nodefile copy for multiline scripts (start fresh each time)
              jl_init_micropython_local_copy();

              // Execute the complete script
              mp_stdin_locked_stream_ptr = (void*)global_mp_stream;  // Lock stdin
              mp_embed_exec_str(script_to_execute.c_str());
              mp_stdin_locked_stream_ptr = nullptr;  // Unlock stdin
              // Clear stale interrupt/exception state after script execution
              mp_interrupt_requested = false;
              MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
              yield(); // pump USB + flush the script's output
              // Force GC after script to close file handles and free memory
              // Use safe version with Core 2 synchronization
              gc_collect_safe();
              // CRITICAL: Close any files that weren't explicitly closed by the script
              jl_close_all_jfs_files();
              // Restore global_mp_stream to REPL's own stream
              setGlobalStreamWithInterrupt(stream);
            }

            // Reset and show new prompt
            editor.reset();
            changeTerminalColor(replColors[1], true, global_mp_stream);
            editor.drawPrompt(global_mp_stream, 0);
            global_mp_stream->flush();
            return;
          }
          // If not 'run', force multiline continuation (never execute
          // individual lines)
        }

        // Check if this is an empty line in multiline mode (escape mechanism)
        bool force_execution = false;
        bool force_multiline = false;
        
        // Check if there's no non-whitespace after the current cursor position
        String remaining_input = editor.current_input.substring(editor.cursor_pos);
        remaining_input.trim();
        bool no_content_after_cursor = (remaining_input.length() == 0);
        
        // Check if the previous line is a newline (empty line)
        int line_start = editor.current_input.lastIndexOf('\n', editor.cursor_pos - 1);
        line_start = (line_start >= 0) ? line_start + 1 : 0;
        String current_line = editor.current_input.substring(line_start, editor.cursor_pos);
        current_line.trim();
        bool previous_line_empty = (current_line.length() == 0);
        
        // Don't force multiline mode when loading from history - let normal detection work
        if (editor.in_multiline_mode && !editor.multiline_forced_on) {
          // Only allow empty line escape in AUTO mode, not when forced ON
          // AND only if there's no content after cursor AND previous line is empty
          if (previous_line_empty && no_content_after_cursor) {
            force_execution = true; // Empty line in multiline mode with no trailing content
          }
        }

        // Check if MicroPython needs more input (multiline detection)
        // Use current input WITHOUT adding newline first
        bool needs_more_input = false;
        if (force_multiline) {
          // Force multiline mode (e.g., when loading from history with content)
          needs_more_input = true;
        } else if (editor.multiline_forced_on) {
          // In forced ON mode, ALWAYS continue - never execute until 'run' is
          // typed
          needs_more_input = true;
        } else if (editor.current_input.length() > 0 && !force_execution) {
          if (editor.multiline_forced_off) {
            // In forced OFF mode, NEVER continue (always execute on Enter)
            needs_more_input = false;
          } else if (no_content_after_cursor) {
            // If there's no content after cursor, use automatic detection
            // Use reference - no need to copy just for .c_str()
            needs_more_input =
                mp_repl_continue_with_input(editor.current_input.c_str());
          } else {
            // If there IS content after cursor, always continue (insert newline)
            needs_more_input = true;
          }
        }

        if (needs_more_input && !force_execution) {
          editor.in_multiline_mode = true;

          // Add newline at cursor position since we need more input
          editor.current_input =
              editor.current_input.substring(0, editor.cursor_pos) + "\n" +
              editor.current_input.substring(editor.cursor_pos);
          editor.cursor_pos++;

          // Smart auto-indent: maintain or increase indentation level
          // Get the line we just finished (before the newline we just added)
          // Use reference to avoid copying large strings
          const String& lines = editor.current_input;
          int last_newline = lines.lastIndexOf(
              '\n',
              editor.cursor_pos - 2); // -2 to skip the newline we just added
          String last_line = "";
          if (last_newline >= 0) {
            last_line = lines.substring(
                last_newline + 1,
                editor.cursor_pos - 1); // exclude the newline we just added
          } else {
            last_line = lines.substring(
                0, editor.cursor_pos - 1); // first line, exclude newline
          }

          // Calculate current indentation level of the previous line
          int current_indent = 0;
          for (int i = 0; i < last_line.length(); i++) {
            if (last_line.charAt(i) == ' ') {
              current_indent++;
            } else {
              break;
            }
          }

          String trimmed_last_line = last_line;
          trimmed_last_line.trim();

          String indent_spaces = "";
          if (trimmed_last_line.endsWith(":")) {
            // Increase indentation level by 4 spaces
            for (int i = 0; i < current_indent + 4; i++) {
              indent_spaces += " ";
            }
          } else if (current_indent > 0) {
            // Maintain current indentation level
            for (int i = 0; i < current_indent; i++) {
              indent_spaces += " ";
            }
          }

          // Insert the indentation at cursor position
          editor.current_input =
              editor.current_input.substring(0, editor.cursor_pos) +
              indent_spaces + editor.current_input.substring(editor.cursor_pos);
          editor.cursor_pos += indent_spaces.length();

          // Redraw the entire input to show current position after newline
          // This handles prompts, indentation, and cursor positioning
          editor.redrawAndPosition(global_mp_stream);
        } else {
          // Execute the complete statement (or force execution)
          if (editor.current_input.length() > 0) {
            changeTerminalColor(replColors[2], true, global_mp_stream);

            // Clean up the input (remove trailing newlines)
            // Find where trailing newlines start
            const String& input_ref = editor.current_input;
            int clean_end = input_ref.length();
            while (clean_end > 0 && input_ref.charAt(clean_end - 1) == '\n') {
              clean_end--;
            }

            if (clean_end > 0) {
              // Execute the user's current input (edited or original)
              // No longer override with history command - user edits should be respected
              
              // For execution, we need to pass a null-terminated string
              // Temporarily modify the buffer in place to avoid large string copies
              char* input_buffer = const_cast<char*>(input_ref.c_str());
              char saved_char = input_buffer[clean_end];
              input_buffer[clean_end] = '\0';  // Temporarily null-terminate

              // Add to history before execution (pass source filename for large scripts)
              history.addToHistory(input_buffer, editor.source_filename);

              // Reset history navigation now that we're executing
              history.resetHistoryNavigation();

              // Let MicroPython handle the complete statement
              mp_stdin_locked_stream_ptr = (void*)global_mp_stream;  // Lock stdin to current REPL stream
              mp_embed_exec_str(input_buffer);
              mp_stdin_locked_stream_ptr = nullptr;  // Unlock stdin
              
              // CRITICAL FIX: Clear stale interrupt/exception state after script execution.
              // When input() is interrupted (Ctrl+C) or script terminates:
              // 1. check_stream_for_interrupt() may have set mp_interrupt_requested = true
              // 2. mp_sched_keyboard_interrupt() may have set mp_pending_exception
              // 3. mp_embed_exec_str() catches the exception for traceback, but does NOT
              //    clear these flags on the exception path
              // Without clearing, post-execution output (gc_collect, color changes, prompt)
              // can see stale interrupt state and malfunction, causing hangs.
              mp_interrupt_requested = false;
              MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
              
              // Service USB to ensure any traceback output is fully transmitted
              // before proceeding with gc_collect and prompt drawing
              yield();
              
              // Restore the original character
              input_buffer[clean_end] = saved_char;
              
              // Clear source filename after execution
              editor.source_filename = "";
              
              // Force garbage collection after script execution to clean up
              // temporary objects and close orphaned file handles (via finalizers)
              // Use safe version with Core 2 synchronization to prevent crashes
              gc_collect_safe();
              
              // CRITICAL: Close any files that weren't explicitly closed by the script
              // This ensures files are available for other operations (eKilo, etc.)
              jl_close_all_jfs_files();

              // CRITICAL: Restore global_mp_stream to the REPL's own stream.
              // During gc_collect_safe() and jl_close_all_jfs_files() above,
              // the main loop may have called jl_service_python() which runs
              // MpRemoteService::service(), switching global_mp_stream to USBSer2.
              // Without restoring here, the prompt/color output below goes to
              // USBSer2 (corrupting ViperIDE's raw REPL protocol) instead of
              // the Serial terminal where the built-in REPL is running.
              setGlobalStreamWithInterrupt(stream);
            }

            changeTerminalColor(replColors[1], true, global_mp_stream);
          }

          // Reset and show new prompt
          editor.reset();
          changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          global_mp_stream->flush();
        }

      } else if (c == '\b' || c == 127) { // Backspace
        // Exit history mode when user starts editing
        if (editor.in_history_mode) {
          editor.in_history_mode = false;
          editor.just_loaded_from_history = false; // Clear the flag when user starts editing
          history.resetHistoryNavigation();
        }
        
        if (editor.cursor_pos > 0) {
          char char_to_delete =
              editor.current_input.charAt(editor.cursor_pos - 1);

          if (char_to_delete == '\n') {
            // Use the proper backspace over newline function
            editor.backspaceOverNewline(global_mp_stream);
          } else {
            // Check if we're backspacing over a complete tab (4 spaces)
            bool is_tab_backspace = false;
            if (editor.cursor_pos >= 4) {
              String potential_tab = editor.current_input.substring(
                  editor.cursor_pos - 4, editor.cursor_pos);
              if (potential_tab == "    ") {
                // Check if these 4 spaces are at the start of a line or after
                // other whitespace
                int line_start = editor.current_input.lastIndexOf(
                    '\n', editor.cursor_pos - 1);
                line_start = (line_start >= 0) ? line_start + 1 : 0;
                String line_before_cursor = editor.current_input.substring(
                    line_start, editor.cursor_pos - 4);

                // If line before cursor is all whitespace, treat as tab
                bool all_whitespace = true;
                for (int i = 0; i < line_before_cursor.length(); i++) {
                  if (line_before_cursor.charAt(i) != ' ') {
                    all_whitespace = false;
                    break;
                  }
                }

                if (all_whitespace) {
                  is_tab_backspace = true;
                  // Remove 4 spaces at once
                  editor.current_input.remove(editor.cursor_pos - 4, 4);
                  editor.cursor_pos -= 4;
                  editor.redrawAndPosition(global_mp_stream);
                }
              }
            }

            if (!is_tab_backspace) {
              // Normal single character backspace
              editor.current_input.remove(editor.cursor_pos - 1, 1);
              editor.cursor_pos--;
              editor.redrawAndPosition(global_mp_stream);
            }
          }
        }
      } else if (c == 5) { // Ctrl+E - Edit current input in main eKilo editor
        changeTerminalColor(replColors[5], true, global_mp_stream);
        global_mp_stream->println("\n[Opening eKilo editor...]");
        changeTerminalColor(replColors[0], false, global_mp_stream);
        
        // Create python_scripts directory if it doesn't exist (safe function handles Core2 pause)
        safeMkdir("/python_scripts", 2000);
        
        // Save current input to temporary file
        String tempFile = "/python_scripts/_temp_repl_edit.py";
        const char* contentToWrite;
        const char* defaultContent = "# Edit your Python script here\n# Press Ctrl+S to save and return to REPL\n# Press Ctrl+P to save and execute immediately\n";
        
        if (editor.current_input.length() > 0) {
          contentToWrite = editor.current_input.c_str();
        } else {
          contentToWrite = defaultContent;
        }
        
        // Use safe file write (handles Core2 pause and mutex internally)
        safeFileWriteAll(tempFile.c_str(), contentToWrite, 0, 2000);
        
        // Launch main eKilo editor with temporary file
        String savedContent = launchEkiloREPL(tempFile.c_str());
        
        // Restore interactive mode after returning from eKilo (raw input).
        if (global_mp_stream == &Serial) {
          setTerminalLineBuffering( true );
        }
        
        // Handle the return from eKilo
        if (savedContent.length() > 0) {
          // Check if this was a Ctrl+P (save and execute) request
          if (savedContent.startsWith("[LAUNCH_REPL]")) {
            // Remove the marker and execute the content
            String contentToExecute = savedContent.substring(13); // Remove "[LAUNCH_REPL]"
            if (contentToExecute.length() > 0) {
              changeTerminalColor(replColors[2], true, global_mp_stream);
              global_mp_stream->println("Executing script from eKilo:");
              
              // Add to history before execution
              history.addToHistory(contentToExecute);
              
              // Execute the script
              mp_stdin_locked_stream_ptr = (void*)global_mp_stream;  // Lock stdin
              mp_embed_exec_str(contentToExecute.c_str());
              mp_stdin_locked_stream_ptr = nullptr;  // Unlock stdin
              // Clear stale interrupt/exception state after script execution
              mp_interrupt_requested = false;
              MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
              yield(); // pump USB + flush the script's output
              // Force GC after script to close file handles and free memory
              // Use safe version with Core 2 synchronization
              gc_collect_safe();
              // CRITICAL: Close any files that weren't explicitly closed by the script
              jl_close_all_jfs_files();
              // Restore global_mp_stream to REPL's own stream
              setGlobalStreamWithInterrupt(stream);
            }
          } else {
            // Regular save - load content into REPL editor
            editor.current_input = savedContent;
            editor.cursor_pos = savedContent.length();
            editor.in_multiline_mode = (savedContent.indexOf('\n') >= 0);
            changeTerminalColor(replColors[5], true, global_mp_stream);
            global_mp_stream->println("Script loaded into REPL editor");
            // Don't reset - show the loaded content
            editor.drawFromCurrentLine(global_mp_stream);
            return;
          }
        } else {
          changeTerminalColor(replColors[5], true, global_mp_stream);
          global_mp_stream->println("Returned from eKilo editor");
        }
        
        editor.reset();
        changeTerminalColor(replColors[1], true, global_mp_stream);
          editor.drawPrompt(global_mp_stream, 0);
          
        return;
        
      } else if (c == '\t') { // TAB character
        // Exit history mode when user starts editing
        if (editor.in_history_mode) {
          editor.in_history_mode = false;
          editor.just_loaded_from_history = false; // Clear the flag when user starts editing
          history.resetHistoryNavigation();
        }
        
        // Convert TAB to 4 spaces at cursor position
        String spaces = "    ";
        editor.current_input =
            editor.current_input.substring(0, editor.cursor_pos) + spaces +
            editor.current_input.substring(editor.cursor_pos);
        editor.cursor_pos += 4;
        
        // Always redraw the entire input buffer to keep everything synchronized
        editor.redrawAndPosition(global_mp_stream);
      } else if (c >= 32 && c <= 126) { // Printable characters
        // Exit history mode when user starts editing
        if (editor.in_history_mode) {
          editor.in_history_mode = false;
          editor.just_loaded_from_history = false; // Clear the flag when user starts editing
          history.resetHistoryNavigation();
        }

        // Insert character at cursor position
        editor.current_input =
            editor.current_input.substring(0, editor.cursor_pos) + (char)c +
            editor.current_input.substring(editor.cursor_pos);
        editor.cursor_pos++;

        // Always redraw the entire input buffer to keep everything synchronized
        editor.redrawAndPosition(global_mp_stream);
      } else {
        mp_repl_continue_with_input(editor.current_input.c_str());
      }
      //Serial.println("looping");
      // All other characters are ignored
    }
  }
}

// Helper function to add complete Jumperless hardware module
void addNodeConstantsToGlobalNamespace(void) {
  if (!mp_initialized) {
    return;
  }
  
  // This function is now redundant since addJumperlessPythonFunctions() 
  // does 'from jumperless import *' which imports everything.
  // Keeping this for backward compatibility, but just calls the main function.
  addJumperlessPythonFunctions();
}

void testGlobalImports(void) {
  if (!mp_initialized) {
    return;
  }
  
  mp_embed_exec_str(
      "print('Testing global imports...')\n"
      "print('oled_connect available:', 'oled_connect' in globals())\n"
      "print('connect available:', 'connect' in globals())\n"
      "print('TOP_RAIL available:', 'TOP_RAIL' in globals())\n"
      "print('D13 available:', 'D13' in globals())\n"
      "#print('jumperless module available:', 'jumperless' in globals())\n");
}

void addJumperlessPythonFunctions(void) {
  if (!mp_initialized) {
    return;
  }
  
  // Only load once to avoid redundant imports
  if (jumperless_globals_loaded) {
    if (global_mp_stream) {
      //global_mp_stream->println("[DEBUG] Jumperless globals already loaded, skipping");
    }
    return;
  }
  
  // Debug: print that this function is being called
  if (global_mp_stream) {
    //global_mp_stream->println("[DEBUG] Loading jumperless globals for first time");
  }

  // Import jumperless module and add ALL functions and constants to global namespace
  mp_embed_exec_str(
      "try:\n"
      "    import jumperless\n"
      "    #print('Native jumperless module available')\n"
      "    funcs = [attr for attr in dir(jumperless) if not attr.startswith('_')]\n"
      "    #print('Available functions: ' + str(funcs))\n"
      "    \n"
      "    # Import all jumperless functions into global namespace\n"
      "    # This eliminates the need for jumperless. prefix\n"
      "    #print('Importing all functions globally...')\n"
      "    from jumperless import *\n"
      "    \n"
      "    # Also keep jumperless module available for explicit access if needed\n"
      "    globals()['jumperless'] = jumperless\n"
      "    \n"
      "    # Add a helper function for checking interrupts in tight loops\n"
      "    def check_interrupt():\n"
      "        '''Call this function in tight loops to allow keyboard interrupt.'''\n"
      "        import time\n"
      "        time.sleep_ms(1)  # Minimal delay that triggers interrupt checking\n"
      "    \n"
      "    # Make it available globally\n"
      "    globals()['check_interrupt'] = check_interrupt\n"
      "    \n"
      "    # Test that functions are actually available\n"
      "    #available_functions = [name for name in globals() if not name.startswith('_') and callable(globals()[name])]\n"
      "    #print(' Available global functions: ' + str(len(available_functions)))\n"
      "    #if 'oled_connect' in globals():\n"
      "    #    print(' oled_connect() is available globally')\n"
      "    #else:\n"
      "    #    print(' oled_connect() not found in globals')\n"
      "    \n"
      "    #print('All jumperless functions and constants available globally')\n"
      "    #print('You can now use: connect(), dac_set(), TOP_RAIL, D13, etc.')\n"
      "    #print('For tight loops, use check_interrupt() to allow interrupts')\n"
      "    \n"
      "except ImportError as e:\n"
      "    print('△ Native jumperless module not available: ' + str(e))\n"
      "except Exception as e:\n"
      "    print('△ Error setting up globals: ' + str(e))\n"
      "    import traceback\n"
      "    traceback.print_exc()\n");

  // Define walk() globally so a recursive, machine-readable filesystem
  // listing is available even before a host IDE connects. Mirrors the
  // body of scripts/ex/viperide_reinit.py. Emits one line per entry:
  //   f|<path>|<size>   (file)    d|<path>|<size>   (directory, then recurses)
  // os is imported here so walk() works regardless of addMicroPythonModules
  // ordering.
  mp_embed_exec_str(
      "import os\n"
      "def walk(p):\n"
      "    for n in os.listdir(p if p else '/'):\n"
      "        fn=p+'/'+n\n"
      "        try: s=os.stat(fn)\n"
      "        except: s=(0,)*7\n"
      "        try:\n"
      "            if s[0] & 0x4000 == 0:\n"
      "                print('f|'+fn+'|'+str(s[6]))\n"
      "            elif n not in ('.','..'):\n"
      "                print('d|'+fn+'|'+str(s[6]))\n"
      "                walk(fn)\n"
      "        except:\n"
      "            print('f|'+p+'/???|'+str(s[6]))\n"
      "globals()['walk'] = walk\n");

  // Mark as successfully loaded
  jumperless_globals_loaded = true;
}

// Run the global walk() (defined in addJumperlessPythonFunctions) and route
// its output to `out` (e.g. the USBSer3 backchannel). Brings MicroPython up
// (full init, so os + VFS are mounted) if it isn't already, then restores the
// previous MP stream. Emits f|path|size / d|path|size lines, framed.
void runFilesystemWalk(Stream* out) {
  if (out == nullptr) {
    out = global_mp_stream ? global_mp_stream : &Serial;
  }
  Stream* prev = global_mp_stream;

  if (!mp_initialized) {
    initMicroPythonProper(out);
  }
  if (!mp_initialized) {
    out->print("{\"error\":\"mp_init_failed\"}\r\n");
    return;
  }

  // Make sure os is imported and walk() is defined (both no-op if already done).
  addMicroPythonModules();
  addJumperlessPythonFunctions();

  setGlobalStream(out);
  out->print("fs_start\r\n");
  mp_embed_exec_str(
      "try:\n"
      "    walk('')\n"
      "except Exception as e:\n"
      "    print('fs_error|'+str(e))\n");
  out->print("fs_end\r\n");
  out->flush();

  setGlobalStream(prev);
}

void addMicroPythonModules(bool time, bool machine, bool os, bool math, bool gc) {
  if (!mp_initialized) {
    return;
  }
  
  if (time) {
    mp_embed_exec_str("import time\n");
   // mp_embed_exec_str("print('Time module imported successfully')\n");
  }
  // if (machine) {
  //   mp_embed_exec_str("import machine\n");
  //   mp_embed_exec_str("print('Machine module imported successfully')\n");
  // }
  if (os) {
    mp_embed_exec_str("import os\n");
  //  mp_embed_exec_str("print('OS module imported successfully')\n");
  }
  if (math) {
    mp_embed_exec_str("import math\n");
   // mp_embed_exec_str("print('Math module imported successfully')\n");
  }
  if (gc) {
    mp_embed_exec_str("import gc\n");
   // mp_embed_exec_str("print('GC module imported successfully')\n");
  }
}


int padTextColumn(int textColumnEnd, int textColumnLength) {
  int padding = textColumnEnd - textColumnLength;
  for (int i = 0; i < padding; i++) {
    global_mp_stream->print(" ");
  }
  return padding;
}

void showREPLreference(int verbose) {

  int textColumnEnd = 12;
  int textColumnLength = 0;


  changeTerminalColor(replColors[0], true, global_mp_stream);

  global_mp_stream->println();

  changeTerminalColor(replColors[2], true, global_mp_stream);

  global_mp_stream->println("    MicroPython REPL");

  // Show commands menu
  changeTerminalColor(replColors[5], true, global_mp_stream);

  global_mp_stream->println("\n Commands:");

  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  quit");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Exit REPL");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->printf("  Ctrl+%c ", (char)(keyboard_interrupt_char + 64)); // Convert ASCII to Ctrl+ notation
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   quit REPL or interrupt running script");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  helpl");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Show REPLhelp");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  history");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Show command history");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  save");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Save last script");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  load");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Load saved script");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  files ");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Open file manager (python_scripts)");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  new ");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Create new script with eKilo editor");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  edit  ");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->println("-   Edit current input in main eKilo editor");
  changeTerminalColor(replColors[3], false, global_mp_stream);

  textColumnLength = global_mp_stream->print("  context  ");
  padTextColumn(textColumnEnd, textColumnLength);
  changeTerminalColor(replColors[0], false, global_mp_stream);
  global_mp_stream->print("-   Toggle connection context - currently: ");
  changeTerminalColor(replColors[2], false, global_mp_stream);
  global_mp_stream->println(jl_get_connection_context_name());
  changeTerminalColor(replColors[3], false, global_mp_stream);

  // textColumnLength = global_mp_stream->print("  help() ");
  // padTextColumn(textColumnEnd, textColumnLength);
    // global_mp_stream->print("  help() ");
  // changeTerminalColor(replColors[0], false, global_mp_stream);
  // global_mp_stream->println("     -   Show hardware commands");

  changeTerminalColor(replColors[5], true, global_mp_stream);
  global_mp_stream->println("\nNavigation:");
  changeTerminalColor(replColors[8], false, global_mp_stream);
  global_mp_stream->println("  ↑/↓ arrows - Browse command history");
  global_mp_stream->println("  ←/→ arrows - Move cursor, edit text");
  global_mp_stream->println("  TAB        - Add 4-space indentation");
  global_mp_stream->println(
      "  Enter      - Execute (on an empty line)");
  // global_mp_stream->println("  Ctrl+Q     - Force quit REPL or interrupt running script");
  // global_mp_stream->println("  files      - Browse and manage Python scripts");
  // global_mp_stream->println("  new        - Create new scripts with eKilo editor");
  // global_mp_stream->println("  run        - Execute accumulated script "
  //                           "(multiline forced ON)");

  changeTerminalColor(replColors[5], true, global_mp_stream);
  global_mp_stream->println("\nHardware:");
  changeTerminalColor(replColors[7], false, global_mp_stream);
  global_mp_stream->println(
      "  help()       - Show Jumperless hardware commands");

  char int_char = (keyboard_interrupt_char >= 1 && keyboard_interrupt_char <= 26) ? 
                  (char)(keyboard_interrupt_char + 64) : '?';
  // global_mp_stream->printf(
  //     "  check_interrupt() - Call in tight loops to allow Ctrl+%c\n", int_char);
  global_mp_stream->println();
  //     "  ");
}

const char *test_code = R"""(
try:
    import jumperless
            #print("☺ Native jumperless module imported successfully")
    
    # Test that functions exist
    if hasattr(jumperless, 'dac_set') and hasattr(jumperless, 'adc_get'):
        print("☺ Core DAC/ADC functions found")
    else:
        print("☹ Core DAC/ADC functions missing")
        
    if hasattr(jumperless, 'nodes_connect') and hasattr(jumperless, 'gpio_set'):
        print("☺ Node and GPIO functions found")
    else:
        print("☹ Node and GPIO functions missing")
        
    if hasattr(jumperless, 'oled_print') and hasattr(jumperless, 'ina_get_current'):
                print("☺ OLED and INA functions found")
    else:
        print("☹ OLED and INA functions missing")
    
    print("☺ Native Jumperless module test completed successfully")
    
except ImportError as e:
    print("☹ Failed to import native jumperless module:", str(e))
except Exception as e:
    print("☹ Error testing native jumperless module:", str(e))
)""";

// Status functions
bool isMicroPythonInitialized(void) { return mp_initialized; }

// Force garbage collection to free memory before launching editor
// This helps prevent crashes when memory is fragmented from Python operations
void forceGarbageCollection(void) {
    if (!mp_initialized) {
        return;  // Nothing to collect if Python isn't running
    }
    
    // Use safe garbage collection with Core2 synchronization
    gc_collect_safe();
    
    // Also close any orphaned file handles
    jl_close_all_jfs_files();
}

void printMicroPythonStatus(void) {
  global_mp_stream->println("\n=== MicroPython Status ===");
  global_mp_stream->printf("Initialized: %s\n", mp_initialized ? "Yes" : "No");
  global_mp_stream->printf("REPL Active: %s\n", mp_repl_active ? "Yes" : "No");
  global_mp_stream->printf("Heap Size: %d bytes (max %d, PSRAM %s)\n",
      (int)mp_heap_size, MICROPY_HEAP_SIZE,
      jumperlessConfig.hardware.psram_installed ? "yes" : "no");

  if (mp_initialized) {
    // Get memory info
    mp_embed_exec_str(
        "import gc; print(f'Free: {gc.mem_free()}, Used: {gc.mem_alloc()}')");
  }
  global_mp_stream->println("=========================\n");
}

// Test function to verify the native Jumperless module is working
void testJumperlessNativeModule(void) {
  if (!mp_initialized) {
    global_mp_stream->println(
        "[MP] Error: MicroPython not initialized for module test");
    return;
  }

  global_mp_stream->println("[MP] Testing native Jumperless module...");

  // Simple test to verify the module can be imported and functions are
  // accessible

  global_mp_stream->println("[MP] Executing native module test...");
  mp_embed_exec_str(test_code);
  global_mp_stream->println("[MP] Native module test complete");
}

// Test function to verify stream redirection is working
void testStreamRedirection(Stream *newStream) {
  if (!mp_initialized) {
    global_mp_stream->println(
        "[MP] Error: MicroPython not initialized for stream test");
    return;
  }

  Stream *oldStream = global_mp_stream;

  // Test output to original stream
  global_mp_stream->println("[MP] Testing stream redirection...");
  global_mp_stream->println("[MP] This should appear on the original stream");
  mp_embed_exec_str("print('Python output to original stream')");

  // Change to new stream using proper setter
  setGlobalStream(newStream);
  newStream->println(
      "[MP] Stream changed - this should appear on the new stream");
  mp_embed_exec_str("print('Python output to new stream')");

  // Change back to original stream using proper setter
  setGlobalStream(oldStream);
  oldStream->println("[MP] Stream changed back - this should appear on the "
                     "original stream again");
  mp_embed_exec_str("print('Python output back to original stream')");

  global_mp_stream->println("[MP] Stream redirection test complete");
}

// ScriptHistory method implementations
void ScriptHistory::initFilesystem() {
  // Note: FatFS should already be initialized by main application
  // Do not call FatFS.begin() here as it can interfere with config loading
  
  // Create scripts directory if it doesn't exist using safe function
  if (!safeMkdir(scripts_dir.c_str(), 2000)) {
    global_mp_stream->println("Failed to create scripts directory");
    return;
  }

  // Load existing history from file (has its own mutex handling)
  loadHistoryFromFile();

  // Find the next available script number (has its own mutex handling)
  findNextScriptNumber();
}

void ScriptHistory::addToHistory(const String &script, const String &sourceFile) {
  if (script.length() == 0)
    return;
    
  // For large scripts, store just a reference marker with the filename if available
  // This prevents copying 6KB+ scripts into RAM history
  static const size_t MAX_INLINE_SIZE = 1024;  // Store content inline only if < 1KB
  
  String content_to_store;
  if (script.length() > MAX_INLINE_SIZE) {
    // Check if we have a source filename to reference
    if (sourceFile.length() > 0) {
      // Store a reference marker instead of content
      content_to_store = "[FILE:" + sourceFile + "]";
      Serial.printf("[History] Storing file reference '%s' instead of %d byte script\n", 
                    sourceFile.c_str(), script.length());
    } else {
      // No filename available, skip storing this large script
      Serial.printf("[History] Script too large (%d bytes) and no file reference, skipping\n", script.length());
      return;
    }
  } else {
    content_to_store = script;
  }

  // Check if this command already exists in history (ring buffer search)
  for (int i = 0; i < count; i++) {
    int idx = (head - count + i + MAX_HISTORY) % MAX_HISTORY;
    if (history[idx].content == content_to_store) {
      // Move this command to the head by updating its timestamp
      history[idx].timestamp = millis();
      current_history_index = -1;
      dirty = true;
      return;
    }
  }

  // Count lines for multiline detection (use original script for accurate count)
  int line_count = countLines(script);
  bool is_multiline = (line_count > MULTILINE_THRESHOLD);
  String parent_id = "";
  
  // For multiline scripts, compute parent hash and prune old versions
  if (is_multiline) {
    parent_id = computeParentHash(script);
    pruneParentVersions(parent_id, MAX_PARENT_VERSIONS - 1); // Keep space for new one
  }

  // Ring buffer insert at head position
  int insert_idx = head % MAX_HISTORY;
  history[insert_idx].content = content_to_store;  // Store reference or small content
  history[insert_idx].parent_id = parent_id;
  history[insert_idx].timestamp = millis();
  history[insert_idx].is_multiline = is_multiline;
  
  head++;
  if (count < MAX_HISTORY) count++;
  
  current_history_index = -1; // Reset navigation
  dirty = true;  // Mark for batched write (don't write immediately)
}

// Helper to resolve file references in history entries
String ScriptHistory::resolveHistoryContent(const String& content) {
  // Check if this is a file reference marker
  if (content.startsWith("[FILE:") && content.endsWith("]")) {
    String filename = content.substring(6, content.length() - 1);
    // Load file content
    File file = safeFileOpen(filename.c_str(), "r", 2000);
    if (file) {
      String fileContent = file.readString();
      safeFileClose(file, false);
      return fileContent;
    }
    // File not found, return the reference marker
    return content;
  }
  return content;
}

String ScriptHistory::getPreviousCommand() {
  if (count == 0)
    return "";

  if (current_history_index == -1) {
    current_history_index = count - 1;
  } else if (current_history_index > 0) {
    current_history_index--;
  }
  // If already at the oldest command, stay there

  // Convert navigation index to ring buffer index
  int ring_idx = (head - count + current_history_index + MAX_HISTORY) % MAX_HISTORY;
  return resolveHistoryContent(history[ring_idx].content);
}

String ScriptHistory::getNextCommand() {
  if (count == 0 || current_history_index == -1)
    return "";

  if (current_history_index < count - 1) {
    current_history_index++;
    int ring_idx = (head - count + current_history_index + MAX_HISTORY) % MAX_HISTORY;
    return resolveHistoryContent(history[ring_idx].content);
  } else {
    // Moving forward past the newest command returns to original input
    current_history_index = -1;
    return ""; // Return to current input
  }
}

String ScriptHistory::getCurrentHistoryCommand() {
  if (current_history_index >= 0 && current_history_index < count) {
    int ring_idx = (head - count + current_history_index + MAX_HISTORY) % MAX_HISTORY;
    return resolveHistoryContent(history[ring_idx].content);
  }
  return "";
}

void ScriptHistory::resetHistoryNavigation() { 
  current_history_index = -1; 
}

void ScriptHistory::clearHistory() {
  count = 0;
  head = 0;
  current_history_index = -1;
  dirty = true;  // Mark for batched write
}

String ScriptHistory::getLastExecutedCommand() {
  if (count == 0)
    return "";
  // Most recent is at (head - 1) in ring buffer
  int ring_idx = (head - 1 + MAX_HISTORY) % MAX_HISTORY;
  return history[ring_idx].content;
}

String ScriptHistory::getLastSavedScript() { 
  return last_saved_script; 
}

int ScriptHistory::getNextScriptNumber() { 
  return next_script_number; 
}

int ScriptHistory::getNumberedScriptsCount() { 
  return numbered_scripts_count; 
}

String ScriptHistory::getNumberedScript(int index) {
  if (index >= 0 && index < numbered_scripts_count) {
    return numbered_scripts[index];
  }
  return "";
}

bool ScriptHistory::saveScript(const String &script, const String &filename) {
  String fname = filename;
  if (fname.length() == 0) {
    // Generate sequential filename
    fname = "script_" + String(next_script_number);

    // Make sure this filename doesn't already exist, increment if needed
    String fullPath = scripts_dir + "/" + fname + ".py";
    while (safeFileExists(fullPath.c_str(), 500)) {
      next_script_number++;
      fname = "script_" + String(next_script_number);
      fullPath = scripts_dir + "/" + fname + ".py";
    }
    next_script_number++; // Increment for next time
  }
  if (!fname.endsWith(".py")) {
    fname += ".py";
  }

  String fullPath = scripts_dir + "/" + fname;
  
  // Use safe file write function (handles Core2 pause, mutex, and flush)
  if (!safeFileWriteAll(fullPath.c_str(), script.c_str(), script.length(), 2000)) {
    global_mp_stream->println("Failed to create script file: " + fullPath);
    return false;
  }

  last_saved_script = fname; // Store for easy reference

  // Add to saved scripts list (avoid duplicates)
  bool already_exists = false;
  for (int i = 0; i < saved_scripts_count; i++) {
    if (saved_scripts[i] == fname) {
      already_exists = true;
      break;
    }
  }

  if (!already_exists && saved_scripts_count < 10) {
    saved_scripts[saved_scripts_count++] = fname;
  }

  global_mp_stream->println("Script saved as: " + fullPath);
  addToHistory(script); // Also add to memory history
  return true;
}

String ScriptHistory::loadScript(const String &filename) {
  String fullPath = scripts_dir + "/" + filename;
  if (!filename.endsWith(".py")) {
    fullPath += ".py";
  }

  // Check if file exists using safe function
  if (!safeFileExists(fullPath.c_str(), 1000)) {
    global_mp_stream->println("Script not found: " + fullPath);
    return "";
  }

  // Open and read file using safe functions
  File file = safeFileOpen(fullPath.c_str(), "r", 2000);
  if (!file) {
    global_mp_stream->println("Failed to open script file: " + fullPath);
    return "";
  }

  String content = file.readString();
  safeFileClose(file, false);  // Read-only, no flush

  global_mp_stream->println("Script loaded: " + fullPath);
  return content;
}

bool ScriptHistory::deleteScript(const String &filename) {
  String fullPath = scripts_dir + "/" + filename;

  if (filename.startsWith("history")) {
    fullPath = scripts_dir + "/history.txt";
    global_mp_stream->println("Deleting history file: " + fullPath);
    clearHistory();
    return true;
  }


  if (!filename.endsWith(".py")) {
    //fullPath += ".py";
  }

  // Check if file exists using safe function
  if (!safeFileExists(fullPath.c_str(), 1000)) {
    global_mp_stream->println("Script not found: " + fullPath);
    return false;
  }

  // Delete file using safe function
  bool removed = safeFileDelete(fullPath.c_str(), 2000);
  
  if (removed) {
    // Remove from saved scripts tracking
    for (int i = 0; i < saved_scripts_count; i++) {
      String saved_name = saved_scripts[i];
      if (!saved_name.endsWith(".py")) {
        //saved_name += ".py";
      }
      String check_name = filename;
      if (!check_name.endsWith(".py")) {
       // check_name += ".py";
      }

      if (saved_name == check_name) {
        // Shift remaining scripts down
        for (int j = i; j < saved_scripts_count - 1; j++) {
          saved_scripts[j] = saved_scripts[j + 1];
        }
        saved_scripts_count--;
        break;
      }
    }

    global_mp_stream->println("Script deleted: " + fullPath);
    return true;
  } else {
    global_mp_stream->println("Failed to delete script: " + fullPath);
    return false;
  }
}

void ScriptHistory::listScripts() {
  changeTerminalColor(replColors[9], false, global_mp_stream);

  // Reset numbered scripts mapping
  numbered_scripts_count = 0;

  // Show recent command history (without numbers) - last 5 from ring buffer
  changeTerminalColor(replColors[6], false, global_mp_stream);
  global_mp_stream->println("\n\rRecent Commands:");
  changeTerminalColor(replColors[9], false, global_mp_stream);
  
  int show_count = (count < 5) ? count : 5;
  for (int i = 0; i < show_count; i++) {
    // Access from newest to oldest
    int idx = (head - 1 - i + MAX_HISTORY) % MAX_HISTORY;
    String history_line = history[idx].content.substring(0, 60);
    history_line.replace("\n", "\n\r");
    if (history_line.length() > 60)
      history_line += "...";
    global_mp_stream->printf("   %s\n\r", history_line.c_str());
    if (history[idx].content.length() > 60)
      global_mp_stream->println("...");
  }
  if (count == 0) {
    global_mp_stream->println("   No commands in history");
  }

  // Show saved script files with numbers
  changeTerminalColor(replColors[6], false, global_mp_stream);
  global_mp_stream->println("\n\rSaved Scripts:");
  changeTerminalColor(replColors[8], false, global_mp_stream);
  if (!safeFileExists(scripts_dir.c_str(), 500)) {
    global_mp_stream->println("   No scripts directory");
    return;
  }

  int script_count = 0;

  // First, show scripts we know we saved in this session
  for (int i = 0; i < saved_scripts_count; i++) {
    String fullPath = scripts_dir + "/" + saved_scripts[i];
    if (!saved_scripts[i].endsWith(".py")) {
      fullPath += ".py";
    }

    int32_t fileSize = safeFileSize(fullPath.c_str(), 500);
    if (fileSize >= 0) {
      if (numbered_scripts_count < 20) {
        numbered_scripts[numbered_scripts_count] = saved_scripts[i];
        String display_name = saved_scripts[i];
        if (!display_name.endsWith(".py")) {
          display_name += ".py";
        }
        global_mp_stream->printf("   %d. %s (%d bytes) [recent]\n\r",
                                 numbered_scripts_count + 1,
                                 display_name.c_str(), fileSize);
        numbered_scripts_count++;
        script_count++;
      }
    }
  }

  // Check for sequential numbered scripts that aren't tracked in memory
  for (int i = 1; i <= 50; i++) { // Check script_1.py through script_50.py
    String script_name = "script_" + String(i);
    String test_script = scripts_dir + "/" + script_name + ".py";

    int32_t testFileSize = safeFileSize(test_script.c_str(), 500);
    if (testFileSize >= 0) {
      // Check if we already listed this one
      bool already_listed = false;
      for (int j = 0; j < saved_scripts_count; j++) {
        if (saved_scripts[j] == script_name ||
            (saved_scripts[j] + ".py") == (script_name + ".py")) {
          already_listed = true;
          break;
        }
      }

      if (!already_listed && numbered_scripts_count < 20) {
        numbered_scripts[numbered_scripts_count] = script_name;
        global_mp_stream->printf("   %d. %s.py (%d bytes)\n\r",
                                 numbered_scripts_count + 1,
                                 script_name.c_str(), testFileSize);
        numbered_scripts_count++;
        script_count++;
      }
    }
  }

  // Also check for some common named scripts that might exist
  String common_names[] = {"test",  "demo", "main",
                           "setup", "loop", "example"};
  int num_common = sizeof(common_names) / sizeof(common_names[0]);

  for (int i = 0; i < num_common; i++) {
    String test_script = scripts_dir + "/" + common_names[i] + ".py";
    int32_t commonFileSize = safeFileSize(test_script.c_str(), 500);
    if (commonFileSize >= 0) {
      // Check if we already listed this one
      bool already_listed = false;
      for (int j = 0; j < saved_scripts_count; j++) {
        if (saved_scripts[j] == common_names[i] ||
            (saved_scripts[j] + ".py") == (common_names[i] + ".py")) {
          already_listed = true;
          break;
        }
      }

      if (!already_listed && numbered_scripts_count < 20) {
        numbered_scripts[numbered_scripts_count] = common_names[i];
        global_mp_stream->printf("   %d. %s.py (%d bytes)\n\r",
                                 numbered_scripts_count + 1,
                                 common_names[i].c_str(), commonFileSize);
        numbered_scripts_count++;
        script_count++;
      }
    }
  }

  if (script_count == 0) {
    global_mp_stream->println("   No saved scripts found");
    global_mp_stream->println(
        "   Use 'save' or 'save scriptname' to save scripts");
  } else {
    global_mp_stream->printf(
        "\n   Type 'load <number>' or 'load <name>' to load a script\n\r");
  }

  global_mp_stream->println();
}

void ScriptHistory::findNextScriptNumber() {
  // Scan for existing script_X.py files to find the next available number
  next_script_number = 1;
  for (int i = 1; i <= 100; i++) { // Check up to script_100.py
    String test_script = scripts_dir + "/script_" + String(i) + ".py";
    if (safeFileExists(test_script.c_str(), 300)) {
      next_script_number = i + 1; // Set to next available number
    } else {
      break; // Found first gap, use it
    }
  }
}

// Thread-safe atomic write to history file
void ScriptHistory::saveHistoryToFile() {
  // This is now a wrapper that calls flushToDisk
  flushToDisk();
}

// Core flush implementation with thread safety and atomic write
void ScriptHistory::flushToDisk() {
  if (!dirty || flush_in_progress) return;
  // ponytail: non-atomic test-and-set; fine while history is only touched from core 0
  flush_in_progress = true;
  
  // Capture current state before file operations
  int save_count = count;
  int save_head = head;
  
  // Atomic write: write to temp file first, then rename
  String tempPath = scripts_dir + "/history.tmp";
  String finalPath = scripts_dir + "/history.txt";

  // Use safe file functions for all operations
  File file = safeFileOpen(tempPath.c_str(), "w", 500);
  if (file) {
    bool ok = true;
    // Write entries in chronological order (oldest first)
    for (int i = 0; ok && i < save_count; i++) {
      int idx = (save_head - save_count + i + MAX_HISTORY) % MAX_HISTORY;
      
      // Skip empty entries (from pruning)
      if (history[idx].content.length() == 0) continue;
      
      // New format with metadata: ===ENTRY:timestamp:parent_id===
      String header = "===ENTRY:" + String(history[idx].timestamp) + ":" + history[idx].parent_id + "===\n";
      ok = file.write((const uint8_t*)header.c_str(), header.length()) == header.length() &&
           file.write((const uint8_t*)history[idx].content.c_str(), history[idx].content.length()) == history[idx].content.length() &&
           file.write((const uint8_t*)"\n===END===\n", 11) == 11;
    }
    file.flush();
    safeFileClose(file, true);  // Write mode, needs flush
    
    if (ok) {
      // Replace only now that the temp is fully written and flushed
      // (FatFS rename won't overwrite, so delete first)
      safeFileDelete(finalPath.c_str());
      if (safeFileRename(tempPath.c_str(), finalPath.c_str())) {
        dirty = false;
        last_flush_time = millis();
      }
      // rename failed: keep dirty so a later flush retries
    } else {
      // Write failed (disk full / I/O error): drop the temp, keep the old
      // history.txt on disk, and leave dirty set so a later flush retries.
      safeFileDelete(tempPath.c_str());
    }
  }

  flush_in_progress = false;
}

void ScriptHistory::loadHistoryFromFile() {
  String historyPath = scripts_dir + "/history.txt";
  
  // Check if history file exists
  if (!safeFileExists(historyPath.c_str(), 500)) {
    return; // No history file exists yet
  }

  // Open and read file using safe functions
  File file = safeFileOpen(historyPath.c_str(), "r", 1000);
  if (!file) {
    return; // Fail silently
  }

  // Bounded read (same 64KB cap as States.cpp readSlotFile) - don't let a
  // huge/corrupt history file exhaust the heap
  String content;
  size_t bytesRead = 0;
  const size_t maxBytes = 65536;  // 64KB safety limit
  while (file.available() && bytesRead < maxBytes) {
    int c = file.read();
    if (c < 0) break;
    content += (char)c;
    bytesRead++;
  }
  safeFileClose(file, false);  // Read-only, no flush

  // Parse saved history - supports both old and new formats
  int start = 0;
  while (start < (int)content.length() && count < MAX_HISTORY) {
    // Try new format first: ===ENTRY:timestamp:parent_id===
    int entry_start = content.indexOf("===ENTRY:", start);
    int old_start = content.indexOf("===SCRIPT_START===", start);
    
    if (entry_start != -1 && (old_start == -1 || entry_start < old_start)) {
      // New format
      int meta_end = content.indexOf("===", entry_start + 9);
      if (meta_end == -1) break;
      
      // Parse metadata
      String meta = content.substring(entry_start + 9, meta_end);
      int colon_pos = meta.indexOf(':');
      uint32_t timestamp = 0;
      String parent_id = "";
      if (colon_pos != -1) {
        timestamp = meta.substring(0, colon_pos).toInt();
        parent_id = meta.substring(colon_pos + 1);
      }
      
      int content_start = meta_end + 3;
      if (content.charAt(content_start) == '\n') content_start++;
      
      int content_end = content.indexOf("===END===", content_start);
      if (content_end == -1) break;
      
      String script = content.substring(content_start, content_end);
      script.trim();
      
      if (script.length() > 0) {
        int idx = head % MAX_HISTORY;
        history[idx].content = script;
        history[idx].parent_id = parent_id;
        history[idx].timestamp = timestamp;
        history[idx].is_multiline = (countLines(script) > MULTILINE_THRESHOLD);
        head++;
        count++;
      }
      
      start = content_end + 9;
    } else if (old_start != -1) {
      // Old format (backward compatibility)
      int script_end = content.indexOf("===SCRIPT_END===", old_start);
      if (script_end == -1) break;

      int script_start = old_start + 18;
      if (content.charAt(script_start) == '\n') script_start++;

      String script = content.substring(script_start, script_end);
      script.trim();

      if (script.length() > 0) {
        int idx = head % MAX_HISTORY;
        history[idx].content = script;
        history[idx].parent_id = "";
        history[idx].timestamp = millis();  // Assign current time
        history[idx].is_multiline = (countLines(script) > MULTILINE_THRESHOLD);
        head++;
        count++;
      }

      start = script_end + 16;
    } else {
      break;  // No more entries
    }
  }
  
  dirty = false;  // Just loaded, so in sync with disk
}

// Periodic flush check - call from REPL idle loop
void ScriptHistory::checkPeriodicFlush() {
  if (!dirty) return;
  if (flush_in_progress) return;
  if (millis() - last_flush_time < FLUSH_INTERVAL_MS) return;
  
  flushToDisk();
}

// Force immediate flush (Ctrl+S or REPL exit)
void ScriptHistory::forceFlush() {
  if (!dirty) return;
  flushToDisk();
}

// Helper: count newlines in script
int ScriptHistory::countLines(const String &script) {
  int count = 1;  // At least one line
  for (unsigned int i = 0; i < script.length(); i++) {
    if (script.charAt(i) == '\n') count++;
  }
  return count;
}

// Helper: compute simple hash of first 3 lines as parent identifier
String ScriptHistory::computeParentHash(const String &script) {
  // Use first 3 lines (or whole script if shorter) to create identifier
  String hash_input = "";
  int line_count = 0;
  int start = 0;
  
  for (unsigned int i = 0; i <= script.length() && line_count < 3; i++) {
    if (i == script.length() || script.charAt(i) == '\n') {
      hash_input += script.substring(start, i);
      start = i + 1;
      line_count++;
    }
  }
  
  // Simple hash: sum of character codes modulo a prime
  uint32_t hash = 0;
  for (unsigned int i = 0; i < hash_input.length(); i++) {
    hash = (hash * 31 + hash_input.charAt(i)) % 999983;
  }
  
  return "p" + String(hash);
}

// Helper: prune old versions of a parent, keeping only 'keep_count' newest
void ScriptHistory::pruneParentVersions(const String &parent_id, int keep_count) {
  if (parent_id.length() == 0) return;
  
  // Find all entries with this parent, sorted by timestamp
  int matching_indices[MAX_HISTORY];
  uint32_t matching_times[MAX_HISTORY];
  int match_count = 0;
  
  for (int i = 0; i < count; i++) {
    int idx = (head - count + i + MAX_HISTORY) % MAX_HISTORY;
    if (history[idx].parent_id == parent_id) {
      matching_indices[match_count] = idx;
      matching_times[match_count] = history[idx].timestamp;
      match_count++;
    }
  }
  
  // If we have more than keep_count, remove oldest
  while (match_count > keep_count) {
    // Find oldest
    int oldest_i = 0;
    for (int i = 1; i < match_count; i++) {
      if (matching_times[i] < matching_times[oldest_i]) {
        oldest_i = i;
      }
    }
    
    // Clear this entry (mark as empty)
    int remove_idx = matching_indices[oldest_i];
    history[remove_idx].content = "";
    history[remove_idx].parent_id = "";
    
    // Compact: shift entries in ring buffer (expensive but rare)
    // For simplicity, just mark as removed - they'll be skipped on save
    
    // Remove from matching arrays
    for (int i = oldest_i; i < match_count - 1; i++) {
      matching_indices[i] = matching_indices[i + 1];
      matching_times[i] = matching_times[i + 1];
    }
    match_count--;
  }
  
  dirty = true;
}

// REPLEditor method implementations
// Static variables for cursor tracking (shared between functions)
static bool cursor_position_known = false;
static int last_terminal_line = 0;
static int last_terminal_column = 0;

void REPLEditor::getCurrentLine(String &line, int &line_start, int &cursor_in_line) {
  // Find the newline before the cursor position (start of current line)
  int line_start_pos = 0;
  for (int i = cursor_pos - 1; i >= 0; i--) {
    if (current_input.charAt(i) == '\n') {
      line_start_pos = i + 1;
      break;
    }
  }
  
  // Find the newline after the cursor position (end of current line)
  int line_end_pos = current_input.length();
  for (int i = cursor_pos; i < current_input.length(); i++) {
    if (current_input.charAt(i) == '\n') {
      line_end_pos = i;
      break;
    }
  }
  
  line_start = line_start_pos;
  line = current_input.substring(line_start_pos, line_end_pos);
  cursor_in_line = cursor_pos - line_start_pos;
}

void REPLEditor::moveCursorToColumn(Stream *stream, int column) {
  stream->print("\033[");
  stream->print(column + 1); // Terminal columns are 1-based
  stream->print("G");
  // Removed flush - let caller batch multiple operations and flush at end
}

void REPLEditor::clearToEndOfLine(Stream *stream) {
  stream->print("\033[K"); // CSI K - Erase to Right
  // Removed flush - let caller batch multiple operations and flush at end
}

void REPLEditor::clearBelow(Stream *stream) {
  stream->print("\033[J"); // CSI J - Erase Below
  // Removed flush - let caller batch multiple operations and flush at end
}

void REPLEditor::backspaceOverNewline(Stream *stream) {
  if (cursor_pos > 0 && current_input.charAt(cursor_pos - 1) == '\n') {
    // Remove the newline
    current_input.remove(cursor_pos - 1, 1);
    cursor_pos--;

    // Check if we're leaving multiline mode
    if (current_input.indexOf('\n') == -1) {
      in_multiline_mode = false;
    }

    // Redraw after removing newline
    redrawAndPosition(stream);
  }
}

void REPLEditor::drawPrompt(Stream *stream, int level) {
  // Use history prompt color when in history mode, normal prompt color otherwise
  int prompt_color = in_history_mode ? replColors[14] : replColors[1];
  // Don't flush here - let the caller handle flushing after all drawing is complete
  changeTerminalColor(prompt_color, false, stream);
  if (level == 0) {
   stream->print(">>> ");
  } else if (level == 1) {
   stream->print("... ");
  } else if (level == 2) {
   stream->print("└─>     ");
  }
  // Removed redundant flush - caller should batch operations and flush once at the end
}



// Maximum number of content characters to render per REPL line (excluding prompt)
// Keep this conservative (<= 76) so prompt (4 chars) + content (76) fits in 80-col terminals
static const int REPL_MAX_CONTENT_COLUMNS = 76;

// Track previous target display row (0-based) so next redraw can move up correctly
static int REPL_prev_target_display_row_index = 0;

// Compute 0-based display row index from logical line/column with wrapping
static int REPL_compute_display_row_index(const String &content, int target_line, int column) {
  if (target_line < 0) return 0;
  int width = REPL_MAX_CONTENT_COLUMNS;
  int row_index = 0;
  int current_line = 0;
  int line_start = 0;
  for (int i = 0; i <= content.length(); i++) {
    if (i == content.length() || content.charAt(i) == '\n') {
      int line_len = i - line_start;
      if (current_line < target_line) {
        int chunks = (line_len + width - 1) / width;
        if (chunks < 1) chunks = 1;
        row_index += chunks;
      } else if (current_line == target_line) {
        row_index += (column / width);
        break;
      }
      line_start = i + 1;
      current_line++;
    }
  }
  return row_index;
}

void REPLEditor::loadFromHistory(Stream *stream, const String &historical_input) {
  if (!in_history_mode) {
    original_input = current_input; // Save current input
    in_history_mode = true;
  }

  current_input = historical_input;
  cursor_pos = current_input.length();
  in_multiline_mode = (current_input.indexOf('\n') >= 0);
  escape_state = 0; // Reset escape state when loading new input
  
  // Flag that we just loaded from history - first Enter should add newline
  just_loaded_from_history = true;

  // For history, just print simply from current line - no complex positioning
  drawFromCurrentLine(stream);

  // Small delay to prevent input processing issues
  delayMicroseconds(100);
}

// Simple drawing function for history - clears previous display and shows new content
void REPLEditor::drawFromCurrentLine(Stream *stream) {
  // Clear the previous history display if we have one
  if (last_displayed_lines > 0) {
    // Move to beginning of current line
    stream->print("\r");
    
    // Move up to the start of the previous display
    for (int i = 0; i < last_displayed_lines; i++) {
      stream->print("\033[A"); // Move up one line
    }
    
    // Move to beginning of line and clear everything below
    stream->print("\r");
    clearBelow(stream);
  } else {
    // Just clear the current line and below
    stream->print("\r");
    clearBelow(stream);
  }
  
  // If we have no input, just show prompt
  if (current_input.length() == 0) {
    drawPrompt(stream, 0);
    stream->flush();
    last_displayed_lines = 0;
    return;
  }

  // Split input into lines and display each one with soft-wrapping
  // Use reference to avoid copying large strings (Arduino String copy can silently fail!)
  const String& lines = current_input;
  int line_start = 0;
  int current_line_num = 0;
  int lines_displayed = 0;

  for (int i = 0; i <= lines.length(); i++) {
    if (i == lines.length() || lines.charAt(i) == '\n') {
      String line = lines.substring(line_start, i);

      // Soft-wrap the line into chunks of REPL_MAX_CONTENT_COLUMNS
      // Detect if this is a full comment line (after leading spaces the first char is '#')
      bool is_comment_line = false;
      {
        int k = 0;
        while (k < (int)line.length() && isspace((int)line.charAt(k))) k++;
        if (k < (int)line.length() && line.charAt(k) == '#') {
          is_comment_line = true;
        }
      }
      int remaining = line.length();
      int offset = 0;
      int chunk_index = 0;
      while (true) {
        // Determine the length of this chunk
        int this_len = min(remaining, REPL_MAX_CONTENT_COLUMNS);
        String chunk = line.substring(offset, offset + this_len);

        // Prompt: first chunk uses level 0/1, subsequent chunks use continuation prompt (level 2)
        // Don't flush here - we flush once at the end of the function for better performance
        changeTerminalColor(replColors[1], false, stream);
        if (chunk_index == 0) {
          drawPrompt(stream, (current_line_num == 0) ? 0 : 1);
        } else {
          drawPrompt(stream, 2);
        }

        // Render the chunk, keeping comment highlight across wrapped rows
        if (is_comment_line) {
          stream->print("\x1b[38;5;34m");
          for (int cj = 0; cj < chunk.length(); cj++) {
            stream->write(chunk.charAt(cj));
          }
        } else {
          displayStringWithSyntaxHighlighting(chunk, stream);
        }

        remaining -= this_len;
        offset += this_len;

        // If more to render for this logical line, newline and continue another wrapped row
        if (remaining > 0) {
          stream->println();
          lines_displayed++;
          chunk_index++;
        } else {
          if (is_comment_line) {
            stream->print("\x1b[0m");
          }
          break;
        }
      }

      // Add newline if not the last logical line
      if (i < lines.length()) {
        stream->println();
        lines_displayed++;
      }

      line_start = i + 1;
      current_line_num++;
    }
  }

  // Update tracking for next time
  last_displayed_lines = lines_displayed;

  stream->flush();
}

void REPLEditor::exitHistoryMode(Stream *stream) {
  if (in_history_mode) {
    current_input = original_input;
    cursor_pos = current_input.length();
    in_multiline_mode = (current_input.indexOf('\n') >= 0);
    in_history_mode = false;
    just_loaded_from_history = false; // Clear the flag when exiting history mode
    escape_state = 0; // Reset escape state
    drawFromCurrentLine(stream);
  }
}


// ============================================================================
// CENTRALIZED CURSOR POSITION MANAGEMENT SYSTEM
// ============================================================================

// Update cursor position calculations from current cursor_pos
void REPLEditor::updateCursorPosition() {
  cursor_position.line = 0;
  cursor_position.column = 0;
  cursor_position.total_lines = 1; // At least one line
  cursor_position.is_valid = true;
  
  if (current_input.length() == 0) {
    return; // Already initialized to 0,0
  }
  
  // Count total lines
  for (int i = 0; i < current_input.length(); i++) {
    if (current_input.charAt(i) == '\n') {
      cursor_position.total_lines++;
    }
  }
  
  // Find current line and column
  int line_start = 0;
  for (int i = 0; i <= current_input.length() && i <= cursor_pos; i++) {
    if (i == cursor_pos) {
      cursor_position.column = cursor_pos - line_start;
      break;
    }
    
    if (i < current_input.length() && current_input.charAt(i) == '\n') {
      cursor_position.line++;
      line_start = i + 1;
    }
  }
}

// Set cursor_pos from line/column coordinates
void REPLEditor::setCursorFromLineColumn(int line, int col) {
  if (line < 0 || col < 0) return;
  
  // Ensure we have current position data
  if (!cursor_position.is_valid) {
    updateCursorPosition();
  }
  
  // Clamp line to valid range
  line = min(line, cursor_position.total_lines - 1);
  
  // Find the start of the target line
  int target_line_start = 0;
  int current_line = 0;
  
  for (int i = 0; i <= current_input.length(); i++) {
    if (current_line == line) {
      target_line_start = i;
      break;
    }
    
    if (i < current_input.length() && current_input.charAt(i) == '\n') {
      current_line++;
      target_line_start = i + 1;
    }
  }
  
  // Find the end of the target line
  int target_line_end = current_input.length();
  for (int i = target_line_start; i < current_input.length(); i++) {
    if (current_input.charAt(i) == '\n') {
      target_line_end = i;
      break;
    }
  }
  
  // Calculate line length and clamp column
  int line_length = target_line_end - target_line_start;
  col = min(col, line_length);
  
  // Set cursor position
  cursor_pos = target_line_start + col;
  
  // Update position cache
  cursor_position.line = line;
  cursor_position.column = col;
  cursor_position.is_valid = true;
}

// Move cursor up one line (data only, no terminal output)
void REPLEditor::moveCursorUp() {
  updateCursorPosition();
  
  if (cursor_position.line > 0) {
    setCursorFromLineColumn(cursor_position.line - 1, cursor_position.column);
  }
}

// Move cursor down one line (data only, no terminal output)
void REPLEditor::moveCursorDown() {
  updateCursorPosition();
  
  if (cursor_position.line < cursor_position.total_lines - 1) {
    setCursorFromLineColumn(cursor_position.line + 1, cursor_position.column);
  }
}

// Move cursor left one character (data only, no terminal output)
void REPLEditor::moveCursorLeft() {
  if (cursor_pos > 0) {
    cursor_pos--;
    cursor_position.is_valid = false; // Mark for recalculation
  }
}

// Move cursor right one character (data only, no terminal output)
void REPLEditor::moveCursorRight() {
  if (cursor_pos < current_input.length()) {
    cursor_pos++;
    cursor_position.is_valid = false; // Mark for recalculation
  }
}

// Move cursor to start of current line (data only, no terminal output)
void REPLEditor::moveCursorToLineStart() {
  updateCursorPosition();
  setCursorFromLineColumn(cursor_position.line, 0);
}

// Move cursor to end of current line (data only, no terminal output)
void REPLEditor::moveCursorToLineEnd() {
  updateCursorPosition();
  
  // Find the current line length
  int line_start = cursor_pos - cursor_position.column;
  int line_end = current_input.length();
  
  for (int i = line_start; i < current_input.length(); i++) {
    if (current_input.charAt(i) == '\n') {
      line_end = i;
      break;
    }
  }
  
  int line_length = line_end - line_start;
  setCursorFromLineColumn(cursor_position.line, line_length);
}


// Move cursor to the very end of all content (data only, no terminal output)
void REPLEditor::moveCursorToEnd() {
  cursor_pos = current_input.length();
  cursor_position.is_valid = false; // Mark for recalculation
}

// Load script content into editor and display it properly
void REPLEditor::loadScriptContent(const String &script, const String &message) {
  // Load the script content into the editor
  current_input = script;
  
  // Don't auto-set multiline mode - let MicroPython's continuation detection
  // decide if more input is needed when the user presses Enter.
  // This allows loaded scripts to execute immediately on Enter.
  in_multiline_mode = false;
  
  // Move cursor to end BEFORE redraw so positioning is correct
  moveCursorToEnd();
  
  // Display success message
  changeTerminalColor(replColors[5], true, global_mp_stream);
  global_mp_stream->println(message);
  
  // Redraw the content - cursor will be positioned at the end
  redrawAndPosition(global_mp_stream);
}

// Redraw content and position cursor - fixed positioning approach
void REPLEditor::redrawAndPosition(Stream *stream) {
  updateCursorPosition();
  
  // The key insight: we need to always position our display at the same location
  // To do this, we'll track how many lines our display occupies and always
  // clear exactly that many lines, then redraw from the same starting position
  
  // Step 1: Calculate how many display rows we need (with soft-wrapping)
  int total_display_lines = 0; // number of display rows across all logical lines
  int target_display_row_index = 0; // the display row where the cursor should be
  if (current_input.length() > 0) {
    // Use reference to avoid copying large strings (Arduino String copy can silently fail!)
    const String& lines_for_count = current_input;
    int line_start_idx = 0;
    int count_line_num = 0;
    for (int idx = 0; idx <= lines_for_count.length(); idx++) {
      if (idx == lines_for_count.length() || lines_for_count.charAt(idx) == '\n') {
        int line_len = idx - line_start_idx;
        int chunks = max(1, (line_len + REPL_MAX_CONTENT_COLUMNS - 1) / REPL_MAX_CONTENT_COLUMNS);
        total_display_lines += chunks;
        if (count_line_num < cursor_position.line) {
          target_display_row_index += chunks;
        } else if (count_line_num == cursor_position.line) {
          target_display_row_index += (cursor_position.column / REPL_MAX_CONTENT_COLUMNS);
        }
        line_start_idx = idx + 1;
        count_line_num++;
      }
    }
  }
  
  // Step 2: Clear our previous display area
  // Move to beginning of current line
  stream->print("\r");
  
  // Calculate how far to move up based on where the cursor currently is
  // The terminal cursor should be on the display row corresponding to the target display row
  // We need to move up to the first line of our display
  int lines_to_move_up = target_display_row_index;
  
  // Also account for any extra lines if the previous display was larger
  int extra_lines_to_clear = 0;
  if (last_displayed_lines > 0) {
    extra_lines_to_clear = max(0, last_displayed_lines - total_display_lines);
  }
  lines_to_move_up += extra_lines_to_clear;
  
  if (lines_to_move_up > 0) {
    stream->print("\033[");
    stream->print(lines_to_move_up);
    stream->print("A"); // Move up to first line of display
  }
  
  // Clear everything below this position
  clearBelow(stream);
  
  // Step 3: If we have no input, just show prompt
  if (current_input.length() == 0) {
    drawPrompt(stream, 0);
    stream->flush();
    last_displayed_lines = 0;
    return;
  }

  // Step 4: Display all lines with soft-wrapping
  // IMPORTANT: Use reference to avoid copying large strings (Arduino String copy can silently fail!)
  const String& lines = current_input;
  int line_start = 0;
  int current_line_num = 0;
  int lines_with_newlines = 0;

  for (int i = 0; i <= lines.length(); i++) {
    if (i == lines.length() || lines.charAt(i) == '\n') {
      String line = lines.substring(line_start, i);

      // Emit this logical line in wrapped chunks
      // Detect if this is a full comment line (after leading spaces the first char is '#')
      bool is_comment_line = false;
      {
        int k = 0;
        while (k < (int)line.length() && isspace((int)line.charAt(k))) k++;
        if (k < (int)line.length() && line.charAt(k) == '#') {
          is_comment_line = true;
        }
      }
      int remaining = line.length();
      int offset = 0;
      int chunk_index = 0;
      while (true) {
        int this_len = min(remaining, REPL_MAX_CONTENT_COLUMNS);
        String chunk = line.substring(offset, offset + this_len);

        if (chunk_index == 0) {
          drawPrompt(stream, (current_line_num == 0) ? 0 : 1);
        } else {
          drawPrompt(stream, 2);
        }

        if (is_comment_line) {
          stream->print("\x1b[38;5;34m");
          for (int cj = 0; cj < chunk.length(); cj++) {
            stream->write(chunk.charAt(cj));
          }
        } else {
          displayStringWithSyntaxHighlighting(chunk, stream);
        }

        remaining -= this_len;
        offset += this_len;

        if (remaining > 0) {
          stream->println();
          lines_with_newlines++;
          chunk_index++;
        } else {
          if (is_comment_line) {
            stream->print("\x1b[0m");
          }
          break;
        }
      }

      // Newline between logical lines (except after the last one)
      if (i < lines.length()) {
        stream->println();
        lines_with_newlines++;
      }

      line_start = i + 1;
      current_line_num++;
    }
  }

  // Step 5: Update tracking
  last_displayed_lines = max(1, total_display_lines);
  REPL_prev_target_display_row_index = target_display_row_index;

  // Step 6: Position cursor at the target location
  // We're currently at the end of the last line (after displaying all content)
  // Need to move cursor to the correct row based on cursor_position
  stream->print("\r");
  
  // Calculate how many rows from end of content to target cursor position
  int rows_from_end = total_display_lines - 1 - target_display_row_index;
  
  if (rows_from_end > 0) {
    // Move up from end of content to target row
    stream->print("\033[");
    stream->print(rows_from_end);
    stream->print("A");
  }
  // If rows_from_end == 0, cursor is already at the right row (end of content)
  
  // Position horizontally
  stream->print("\r");
  int cursor_wrap_index = (cursor_position.column / REPL_MAX_CONTENT_COLUMNS);
  int prompt_length;
  if (cursor_wrap_index == 0) {
    prompt_length = 4; // '>>> ' or '... '
  } else {
    prompt_length = 8; // '└─>     '
  }
  int target_column = prompt_length + (cursor_position.column % REPL_MAX_CONTENT_COLUMNS);
  moveCursorToColumn(stream, target_column);
  
  stream->flush();
  
  // Reset cursor position tracking for repositionCursorOnly
  resetCursorTracking();
}

// Reset the cursor position tracking (call after redrawAndPosition)
void REPLEditor::resetCursorTracking() {
  updateCursorPosition();
  cursor_position_known = true;
  last_terminal_line = cursor_position.line;
  last_terminal_column = cursor_position.column;
}

// Mark cursor position as unknown (forces next movement to redraw)
void REPLEditor::invalidateCursorTracking() {
  cursor_position_known = false;
}

// Move cursor to correct position without redrawing content
void REPLEditor::repositionCursorOnly(Stream *stream) {
  updateCursorPosition();
  
  // If we don't know where the terminal cursor is, do a full reposition
  if (!cursor_position_known) {
    redrawAndPosition(stream);
    return;
  }
  
  // Calculate relative movement needed, accounting for wraps
  int current_display_row = REPL_compute_display_row_index(current_input, cursor_position.line, cursor_position.column);
  int last_display_row = REPL_compute_display_row_index(current_input, last_terminal_line, last_terminal_column);
  int line_diff = current_display_row - last_display_row;
  
  // Move vertically if needed
  if (line_diff != 0) {
    if (line_diff > 0) {
      // Need to move down - but when at bottom of screen, cursor can't move down
      // Instead, add newlines to create space and push content up
      for (int i = 0; i < line_diff; i++) {
        stream->println(); // Add newline to push content up
      }
    } else {
      // Move up, but don't go above line 0
      int lines_to_move_up = min(-line_diff, last_terminal_line);
      if (lines_to_move_up > 0) {
        stream->print("\033[");
        stream->print(lines_to_move_up);
        stream->print("A");
      }
    }
  }
  
  // Always recalculate horizontal position to account for prompts and wrapping
  stream->print("\r");
  int cursor_wrap_index = (cursor_position.column / REPL_MAX_CONTENT_COLUMNS);
  int prompt_length = (cursor_wrap_index == 0) ? 4 : 8;
  int target_column = prompt_length + (cursor_position.column % REPL_MAX_CONTENT_COLUMNS);
  moveCursorToColumn(stream, target_column);
  
  // Update tracking
  last_terminal_line = cursor_position.line;
  last_terminal_column = cursor_position.column;
  
  stream->flush();
}

void REPLEditor::reset() {
  current_input = "";
  cursor_pos = 0;
  in_multiline_mode = false;
  first_run = true;
  escape_state = 0;
  original_input = "";
  in_history_mode = false;
  just_loaded_from_history = false; // Clear the flag on reset
  source_filename = ""; // Clear source filename on reset
  // Don't reset multiline mode settings - preserve user's choice
  // multiline_override, multiline_forced_on, multiline_forced_off should persist
  last_displayed_lines = 0;
  last_displayed_content = ""; // Clear the last displayed content
  mp_interrupt_requested = false; // Clear any pending interrupt
  
  // Reset cursor position tracking
  cursor_position.line = 0;
  cursor_position.column = 0;
  cursor_position.total_lines = 0;
  cursor_position.is_valid = false;
  
  // Reset terminal cursor tracking
  cursor_position_known = false;
}

void REPLEditor::fullReset() {
  current_input = "";
  cursor_pos = 0;
  in_multiline_mode = false;
  first_run = true;
  escape_state = 0;
  original_input = "";
  in_history_mode = false;
  just_loaded_from_history = false; // Clear the flag on full reset
  multiline_override = false;
  multiline_forced_on = false;
  multiline_forced_off = false;
  last_displayed_lines = 0;
  last_displayed_content = ""; // Clear the last displayed content
  
  // Reset cursor position tracking
  cursor_position.line = 0;
  cursor_position.column = 0;
  cursor_position.total_lines = 0;
  cursor_position.is_valid = false;
  
  // Reset terminal cursor tracking
  cursor_position_known = false;
}

// ============================================================================
// CURSOR MOVEMENT FUNCTIONS - CLEANED UP
// ============================================================================
// 
// This REPLEditor now has a clean, simple cursor movement system:
//
// LOGICAL CURSOR FUNCTIONS (update internal position only):
//   - moveCursorUp(), moveCursorDown() - move cursor between lines
//   - moveCursorLeft(), moveCursorRight() - move cursor within line
//   - moveCursorToLineStart(), moveCursorToLineEnd() - move to line boundaries
//   - updateCursorPosition(), setCursorFromLineColumn() - position management
//
// TERMINAL CONTROL FUNCTIONS (send ANSI escape codes):
//   - clearBelow() - clear everything below cursor
//   - moveCursorToColumn() - position terminal cursor at column
//
// DISPLAY FUNCTIONS:
//   - redrawAndPosition() - redraws content and positions cursor correctly
//   - repositionCursorOnly() - moves cursor without redrawing (for arrow keys)
//   - getCurrentLine() - gets current line information
//   - backspaceOverNewline() - handles backspace over newlines
//   - loadFromHistory(), exitHistoryMode() - history navigation
//
// All deprecated functions have been removed to eliminate confusion.
// ============================================================================

// Note: enterPasteMode function removed - replaced with "new" command that opens eKilo editor
// for creating new scripts. This provides a better user experience than paste mode.

// New functions for single command execution from main.cpp




/**
 * Initialize MicroPython quietly without any output
 * Returns true if successful, false if failed
 */
bool initMicroPythonQuiet(bool preserve_interrupt_char) {
  if (mp_initialized) {
    return true;
  }

  // Store original stream and redirect to null
  Stream *original_stream = global_mp_stream;
  global_mp_stream = nullptr;
  global_mp_stream_ptr = nullptr;

  // Get proper stack pointer
  char stack_dummy;
  char *stack_top = &stack_dummy;

  // Allocate heap if needed
  if (!mp_heap) {
    mp_heap_size = jumperlessConfig.hardware.psram_installed
        ? MICROPY_HEAP_SIZE_PSRAM : MICROPY_HEAP_SIZE;
    mp_heap = (unsigned char *)malloc(mp_heap_size);
    if (!mp_heap) {
      global_mp_stream = original_stream;
      global_mp_stream_ptr = (void *)original_stream;
      return false;
    }
  }

  // Initialize MicroPython silently
  mp_embed_init(mp_heap, mp_heap_size, stack_top);
  
  // Configure keyboard interrupt character: always keep default Ctrl+C
  // mp_embed_exec_str("import micropython; micropython.kbd_intr(3)");
  // keyboard_interrupt_char = 3;
  
  mp_initialized = true;
  mp_repl_active = false;

  // Restore original stream
  global_mp_stream = original_stream;
  global_mp_stream_ptr = (void *)original_stream;
  
  // Import all jumperless functions and constants globally (silently)
  // This ensures everything is available for single commands without prefix
  mp_embed_exec_str(
      "try:\n"
      "    import jumperless\n"
      "    from jumperless import *\n"
      "    globals()['jumperless'] = jumperless\n"
      "    \n"
      "    # Add interrupt checking helper for tight loops\n"
      "    def check_interrupt():\n"
      "        import time\n"
      "        time.sleep_ms(1)\n"
      "    globals()['check_interrupt'] = check_interrupt\n"
      "except: pass\n");
  Serial.write("\x1b[0 q");
  return true;
}

// Function output type enumeration
enum FunctionOutputType {
  OUTPUT_NONE,           // No output formatting
  OUTPUT_VOLTAGE,        // Format as voltage with V unit
  OUTPUT_CURRENT,        // Format as current with mA unit  
  OUTPUT_POWER,          // Format as power with mW unit
  OUTPUT_GPIO_STATE,     // Format as HIGH/LOW
  OUTPUT_GPIO_DIR,       // Format as INPUT/OUTPUT
  OUTPUT_GPIO_PULL,      // Format as PULLUP/NONE/PULLDOWN
  OUTPUT_BOOL_CONNECTED, // Format as CONNECTED/DISCONNECTED
  OUTPUT_BOOL_YESNO,     // Format as YES/NO
  OUTPUT_COUNT,          // Format as simple number
  OUTPUT_FLOAT           // Format as float with precision
};

// Function type mapping structure
struct FunctionTypeMap {
  const char* function_name;
  FunctionOutputType output_type;
};

/**
 * Global mapping of function names to their output types for formatted printing
 */
static const FunctionTypeMap function_type_map[] = {
  // DAC functions
  {"dac_set", OUTPUT_NONE},
  {"dac_get", OUTPUT_VOLTAGE},
  {"set_dac", OUTPUT_NONE},           // Alias
  {"get_dac", OUTPUT_VOLTAGE},        // Alias
  
  // ADC functions  
  {"adc_get", OUTPUT_VOLTAGE},
  {"get_adc", OUTPUT_VOLTAGE},        // Alias
  
  // INA functions
  {"ina_get_current", OUTPUT_CURRENT},
  {"ina_get_voltage", OUTPUT_VOLTAGE},
  {"ina_get_bus_voltage", OUTPUT_VOLTAGE},
  {"ina_get_power", OUTPUT_POWER},
  {"get_ina_current", OUTPUT_CURRENT},      // Alias
  {"get_ina_voltage", OUTPUT_VOLTAGE},      // Alias
  {"get_ina_bus_voltage", OUTPUT_VOLTAGE},  // Alias
  {"get_ina_power", OUTPUT_POWER},          // Alias
  {"get_current", OUTPUT_CURRENT},          // Alias
  {"get_voltage", OUTPUT_VOLTAGE},          // Alias
  {"get_bus_voltage", OUTPUT_VOLTAGE},      // Alias
  {"get_power", OUTPUT_POWER},              // Alias
  
  // PWM functions
  {"pwm", OUTPUT_NONE},
  {"pwm_set_frequency", OUTPUT_NONE},
  {"pwm_set_duty_cycle", OUTPUT_NONE},
  {"pwm_stop", OUTPUT_NONE},
  {"set_pwm", OUTPUT_NONE},                    // Alias
  {"set_pwm_frequency", OUTPUT_NONE},          // Alias
  {"set_pwm_duty_cycle", OUTPUT_NONE},         // Alias
  {"stop_pwm", OUTPUT_NONE},                   // Alias
  
  // GPIO functions
  {"gpio_set", OUTPUT_NONE},
  {"gpio_get", OUTPUT_GPIO_STATE},
  {"gpio_set_dir", OUTPUT_NONE},
  {"gpio_get_dir", OUTPUT_GPIO_DIR},
  {"gpio_set_pull", OUTPUT_NONE},
  {"gpio_get_pull", OUTPUT_GPIO_PULL},
  {"set_gpio", OUTPUT_NONE},               // Alias
  {"get_gpio", OUTPUT_GPIO_STATE},         // Alias
  {"set_gpio_dir", OUTPUT_NONE},           // Alias
  {"get_gpio_dir", OUTPUT_GPIO_DIR},       // Alias
  {"set_gpio_pull", OUTPUT_NONE},          // Alias
  {"get_gpio_pull", OUTPUT_GPIO_PULL},     // Alias
  {"set_gpio_direction", OUTPUT_NONE},     // Alias
  {"get_gpio_direction", OUTPUT_GPIO_DIR}, // Alias
  
  // Node functions
  {"connect", OUTPUT_BOOL_CONNECTED},
  {"disconnect", OUTPUT_NONE},
  {"fast_connect", OUTPUT_BOOL_CONNECTED},      // Fast refresh version
  {"fast_disconnect", OUTPUT_NONE},             // Fast refresh version
  {"nodes_clear", OUTPUT_NONE},
  {"is_connected", OUTPUT_BOOL_CONNECTED},
  {"connect_nodes", OUTPUT_BOOL_CONNECTED},     // Alias
  {"disconnect_nodes", OUTPUT_NONE},            // Alias
  {"clear_nodes", OUTPUT_NONE},                 // Alias
  {"clear_connections", OUTPUT_NONE},           // Alias
  {"nodes_connected", OUTPUT_BOOL_CONNECTED},   // Alias
  {"connected", OUTPUT_BOOL_CONNECTED},         // Alias
  
  // OLED functions
  {"oled_print", OUTPUT_NONE},
  {"oled_clear", OUTPUT_NONE},
  {"oled_show", OUTPUT_NONE},
  {"oled_connect", OUTPUT_BOOL_YESNO},
  {"oled_disconnect", OUTPUT_NONE},
  {"print_oled", OUTPUT_NONE},          // Alias
  {"clear_oled", OUTPUT_NONE},          // Alias
  {"show_oled", OUTPUT_NONE},           // Alias
  {"connect_oled", OUTPUT_BOOL_YESNO},  // Alias
  {"disconnect_oled", OUTPUT_NONE},     // Alias
  {"display_print", OUTPUT_NONE},       // Alias
  {"display_clear", OUTPUT_NONE},       // Alias
  {"display_show", OUTPUT_NONE},        // Alias
  
  // Status functions
  {"print_bridges", OUTPUT_NONE},
  {"print_paths", OUTPUT_NONE},
  {"print_crossbars", OUTPUT_NONE},
  {"print_nets", OUTPUT_NONE},
  {"print_chip_status", OUTPUT_NONE},
  {"show_bridges", OUTPUT_NONE},        // Alias
  {"show_paths", OUTPUT_NONE},          // Alias
  {"show_crossbars", OUTPUT_NONE},      // Alias
  {"show_nets", OUTPUT_NONE},           // Alias
  {"show_chip_status", OUTPUT_NONE},    // Alias
  {"bridges", OUTPUT_NONE},             // Alias
  {"paths", OUTPUT_NONE},               // Alias
  {"crossbars", OUTPUT_NONE},           // Alias
  {"nets", OUTPUT_NONE},                // Alias
  {"chip_status", OUTPUT_NONE},         // Alias
  
  // Other functions
  {"arduino_reset", OUTPUT_NONE},
  {"probe_tap", OUTPUT_NONE},
  {"clickwheel_up", OUTPUT_NONE},
  {"clickwheel_down", OUTPUT_NONE},
  {"clickwheel_press", OUTPUT_NONE},
  {"run_app", OUTPUT_NONE},
  {"help", OUTPUT_NONE},
  {"reset_arduino", OUTPUT_NONE},       // Alias
  {"reset", OUTPUT_NONE},               // Alias
  {"app_run", OUTPUT_NONE},             // Alias
  {"tap_probe", OUTPUT_NONE},           // Alias
  {"tap", OUTPUT_NONE},                 // Alias
  {"wheel_up", OUTPUT_NONE},            // Alias
  {"wheel_down", OUTPUT_NONE},          // Alias
  {"wheel_press", OUTPUT_NONE},         // Alias
  {"click_up", OUTPUT_NONE},            // Alias
  {"click_down", OUTPUT_NONE},          // Alias
  {"click_press", OUTPUT_NONE},         // Alias
  {"scroll_up", OUTPUT_NONE},           // Alias
  {"scroll_down", OUTPUT_NONE},         // Alias
  {"press", OUTPUT_NONE},               // Alias
  {"send_raw", OUTPUT_NONE},            
  {"pause_core2", OUTPUT_NONE},
  
  {nullptr, OUTPUT_NONE} // End marker
};

/**
 * List of jumperless module function names for automatic prefix detection
 */

static const char* jumperless_functions[] = {
  // DAC functions
  "dac_set", "dac_get", "set_dac", "get_dac",
  // ADC functions  
  "adc_get", "get_adc",
  // INA functions
  "ina_get_current", "ina_get_voltage", "ina_get_bus_voltage", "ina_get_power",
  "get_ina_current", "get_ina_voltage", "get_ina_bus_voltage", "get_ina_power",
  "get_current", "get_voltage", "get_bus_voltage", "get_power",
  // PWM functions
  "pwm", "pwm_set_frequency", "pwm_set_duty_cycle", "pwm_stop",
  "set_pwm", "set_pwm_frequency", "set_pwm_duty_cycle", "stop_pwm",
  // GPIO functions
  "gpio_set", "gpio_get", "gpio_set_dir", "gpio_get_dir", "gpio_set_pull", "gpio_get_pull",
  "set_gpio", "get_gpio", "set_gpio_dir", "get_gpio_dir", "set_gpio_pull", "get_gpio_pull",
  "set_gpio_direction", "get_gpio_direction",
  // Node functions
  "connect", "disconnect", "fast_connect", "fast_disconnect", "nodes_clear", "is_connected",
  "connect_nodes", "disconnect_nodes", "clear_nodes", "clear_connections", "nodes_connected", "connected",
  // OLED functions
  "oled_print", "oled_clear", "oled_show", "oled_connect", "oled_disconnect",
  "print_oled", "clear_oled", "show_oled", "connect_oled", "disconnect_oled",
  "display_print", "display_clear", "display_show",
  // Status functions
  "print_bridges", "print_paths", "print_crossbars", "print_nets", "print_chip_status",
  "show_bridges", "show_paths", "show_crossbars", "show_nets", "show_chip_status",
  "bridges", "paths", "crossbars", "nets", "chip_status",
  // Other functions
  "arduino_reset", "probe_tap", "clickwheel_up", "clickwheel_down", "clickwheel_press", "run_app", "help",
  "reset_arduino", "reset", "app_run", "tap_probe", "tap",
  "wheel_up", "wheel_down", "wheel_press", "click_up", "click_down", "click_press",
  "scroll_up", "scroll_down", "press", "send_raw", "pause_core2", "nodes_save",

  "scroll_up", "scroll_down", "press", "send_raw", "pause_core2",
  nullptr // End marker
};

/**
 * Check if a function name is a jumperless module function
 */
bool isJumperlessFunction(const char* function_name) {
  for (int i = 0; jumperless_functions[i] != nullptr; i++) {
    if (strcmp(function_name, jumperless_functions[i]) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * Get the output type for a function name
 */
FunctionOutputType getFunctionOutputType(const char* function_name) {
  for (int i = 0; function_type_map[i].function_name != nullptr; i++) {
    if (strcmp(function_type_map[i].function_name, function_name) == 0) {
      return function_type_map[i].output_type;
    }
  }
  return OUTPUT_NONE;
}

/**
 * Extract function name from a command string
 * e.g., "gpio_get(2)" -> "gpio_get"
 */
String extractFunctionName(const String& command) {
  int paren_pos = command.indexOf('(');
  if (paren_pos == -1) {
    return command; // No parentheses found
  }
  
  String func_name = command.substring(0, paren_pos);
  func_name.trim();
  
  // Since functions are now globally imported, no prefix handling needed
  return func_name;
}

/**
 * Format a result value based on the function output type
 */
String formatResult(float value, FunctionOutputType output_type) {
  switch (output_type) {
    case OUTPUT_VOLTAGE:
      return String(value, 3) + "V";
      
    case OUTPUT_CURRENT:
      if (value >= 1000.0f) {
        return String(value / 1000.0f, 3) + "A";
      } else {
        return String(value, 1) + "mA";
      }
      
    case OUTPUT_POWER:
      if (value >= 1000.0f) {
        return String(value / 1000.0f, 3) + "W";
      } else {
        return String(value, 1) + "mW";
      }
      
    case OUTPUT_GPIO_STATE:
      return (value != 0.0f) ? "HIGH" : "LOW";
      
    case OUTPUT_GPIO_DIR:
      return (value != 0.0f) ? "OUTPUT" : "INPUT";
      
    case OUTPUT_GPIO_PULL:
      if (value > 1.5f) return "KEEPER";
      else if (value > 0.5f) return "PULLUP";
      else if (value < -0.5f) return "PULLDOWN";
      else return "NONE";
      
    case OUTPUT_BOOL_CONNECTED:
      return (value != 0.0f) ? "CONNECTED" : "DISCONNECTED";
      
    case OUTPUT_BOOL_YESNO:
      return (value != 0.0f) ? "YES" : "NO";
      
    case OUTPUT_COUNT:
      return String((int)value);
      
    case OUTPUT_FLOAT:
      return String(value, 3);
      
    case OUTPUT_NONE:
    default:
      return "OK";
  }
}

/**
 * Parse a command and add jumperless. prefix if needed
 * Returns a new String with the parsed command
 */
String parseCommandWithPrefix(const char* command) {
  // Since all jumperless functions are now globally imported,
  // we no longer need to add prefixes - just return the command as-is
  String cmd = String(command);
  cmd.trim();
  return cmd;
}

/**
 * Execute a single MicroPython command with automatic initialization and prefix handling
 * This function can be called from main.cpp
 * 
 * @param command The command to execute (e.g., "gpio_get(2)" or "dac_set(0, 3.3)")
 * @param result_buffer Optional buffer to store string result (can be nullptr)
 * @param buffer_size Size of result buffer
 * @param response_target Optional stream to send output to (nullptr = use global_mp_stream)
 * @return true if command executed successfully, false otherwise
 */
bool executeSinglePythonCommand(const char* command, char* result_buffer, size_t buffer_size) {
  // SAFETY: Validate input before doing anything
  if (!command || command[0] == '\0') {
    return false;
  }
  
  // SAFETY: Check command length - reject obviously oversized commands
  size_t cmd_len = strlen(command);
  if (cmd_len > 1024) {
    Jerial.println("Error: Command too long (max 1024 chars)");
    return false;
  }
  
  // SAFETY: Check for at least one printable ASCII character
  bool has_printable = false;
  for (size_t i = 0; i < cmd_len; i++) {
    if (command[i] >= ' ' && command[i] < 127) {
      has_printable = true;
      break;
    }
  }
  if (!has_printable) {
    return false;  // Silently ignore garbage-only commands
  }

  // Initialize quietly if needed
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      if (result_buffer && buffer_size > 0) {
        strncpy(result_buffer, "ERROR: Failed to initialize MicroPython", buffer_size - 1);
        result_buffer[buffer_size - 1] = '\0';
      }
      return false;
    }
  }
  
  // Parse command and add prefix if needed
  String parsed_command = parseCommandWithPrefix(command);
  
  // Debug output - always enabled for now to track command execution
  #if DEBUG_INJECTED_COMMANDS
  Serial.printf("executeSinglePythonCommand: input=\"%s\", parsed=\"%s\", response_target=%p\n", 
                command, parsed_command.c_str(), response_target);
  Serial.flush();
  #endif

  // Temporarily switch to response target if specified
  Stream* original_stream = global_mp_stream;
    // if (response_target != nullptr) {
    //   global_mp_stream = response_target;
    // }
if (jumperlessConfig.display.terminal_line_buffering == 0) {
  SyntaxHighlighting highlighter(&Jerial);
  Jerial.print("\r                                                                \r");
  highlighter.displayPythonWithHighlighting(parsed_command, &Jerial);
  Jerial.println();
  Jerial.flush();
}
  
  // Service USB before execution to prevent port disconnect during long commands
  TinyUSB_Device_Task();
  
  // Clear result buffer
  if (result_buffer && buffer_size > 0) {
    memset(result_buffer, 0, buffer_size);
  }
  
  bool success = true;
  
  // Execute the command directly - MicroPython will handle errors internally
  // mp_embed_exec_str has nlr_push/nlr_pop to catch Python exceptions
  mp_embed_exec_str(parsed_command.c_str());

  // Service USB after execution (and flush the output)
  yield();
  
  // CRITICAL: Force garbage collection after each command to prevent heap exhaustion
  // Without this, repeated commands can fragment/exhaust the MicroPython heap
  mp_embed_exec_str("import gc; gc.collect()");
  
  // Close any files that weren't explicitly closed by the command
  jl_close_all_jfs_files();

  
  if (result_buffer && buffer_size > 0) {
    strncpy(result_buffer, "OK", buffer_size - 1);
    result_buffer[buffer_size - 1] = '\0';
 
  Jerial.println(result_buffer);

   }
  // Restore original stream
  // global_mp_stream = original_stream;
  
  // MINIMAL DEBUG: Removed verbose output to reduce USB buffer pressure
  return success;
}

/**
 * Hand the UI back to the firmware after Python has run.
 *
 * A script owns the probe, encoder and display while it runs - and services
 * keep ticking underneath it, because jOS.serviceAll() runs inside
 * MicroPython's delay hook. So measure mode and the highlight reader can both
 * latch onto whatever row the script last touched, then repaint over
 * everything forever: that is the "OLED stuck on one row's voltage until
 * reboot" bug.
 *
 * Only INTERACTION state is reset. Connections, DAC voltages, overlays and
 * whatever the script last drew on the OLED are its OUTPUT and must survive -
 * this stops the zombie repainters, it does not blank the screen.
 *
 * fullHandback=false does only the script-injected input state (encoder,
 * pauseCore2) and leaves the display alone - see the comment in the body.
 */
void onPythonSessionEnd( bool fullHandback ) {
    // The display teardown is NOT unconditional. The raw REPL calls this after
    // EVERY exec - every ViperIDE console line, tab completion and file listing
    // - and the obvious "did anything latch?" test is a trap: showReadingNet is
    // sticky by design (only resetReadingState() clears it), so after a single
    // probe tap it stays true forever and the gate fires in exactly the case it
    // was written to protect. Tearing down there wipes a reading the user parked
    // on the main terminal, re-routes the measure-mode ADC bridge (crossbar
    // churn, since MeasureMode re-latches immediately) and re-sends the LEDs,
    // once per typed line.
    //
    // So: the file-run path always hands back, and the raw-REPL path only does
    // when the SCRIPT itself moved the switch - the documented way a script
    // latches measure mode, already tracked for the switch restore.
    if ( fullHandback ) {
        Highlighting& hl = Highlighting::getInstance( );
        // Erase the pinned live reading rows BEFORE anything drops the anchor:
        // stopMeasurement() -> clearHighlighting() -> resetLastShown() forgets
        // where the rows are, and a value left frozen there reads as current.
        ReadingDisplay::clearLiveSerialLine( );
        measureModeService.stopMeasurement( );  // no-ops when inactive
        hl.clearHighlighting( 1 );
        hl.resetReadingState( );
    }

    // Encoder state a script can inject via clickwheel_up/down/press.
    encoderDirectionState = NONE;
    encoderButtonState = IDLE;
    lastButtonEncoderState = IDLE;
    encoderOverride = 0;

    pauseCore2 = 0;
}

/**
 * Execute raw Python source code (e.g. file contents) without syntax highlighting.
 * Initialises MicroPython if needed, runs mp_embed_exec_str(), then GC + file cleanup.
 * Unlike executeSinglePythonCommand() this does NOT echo the source to the terminal,
 * making it suitable for running entire .py files from the file manager.
 *
 * @param src  Null-terminated Python source string
 * @return true on success (MicroPython does its own error printing)
 */
bool executePythonFileContent( const char* src ) {
    if ( !src || !*src )
        return false;

    // Initialise quietly if needed
    if ( !mp_initialized ) {
        if ( !initMicroPythonQuiet( ) )
            return false;
    }

    // Enable clickwheel-as-interrupt so holding the button raises KeyboardInterrupt.
    // Grace period prevents selection click from instantly interrupting.
    clickwheel_interrupt_ignore_until = millis() + 500;
    clickWheelPythonInterrupt = true;

    // Execute the source directly
    mp_embed_exec_str( src );

    // Disable clickwheel interrupt now that script is done
    clickWheelPythonInterrupt = false;

    // GC + close any leaked file handles
    mp_embed_exec_str( "import gc; gc.collect()" );
    jl_close_all_jfs_files( );

    // Hand the UI back. Runs even when the script raised - mp_embed_exec_str()
    // catches Python exceptions internally and returns normally.
    onPythonSessionEnd( true );   // file run: the script owned the UI

    // A file run is a whole session, so the display prefs only a script can
    // set go back to defaults too. NOT done on the raw-REPL path, where each
    // console line is its own exec and prefs legitimately persist between them.
    jl_reset_python_display_prefs( );

    return true;
}

/**
 * Enhanced command execution with formatted output
 * This version captures the return value and formats it according to function type
 */
bool executeSinglePythonCommandFormatted(const char* command, char* result_buffer, size_t buffer_size) {
  // Initialize quietly if needed
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      if (result_buffer && buffer_size > 0) {
        strncpy(result_buffer, "ERROR: Failed to initialize MicroPython", buffer_size - 1);
        result_buffer[buffer_size - 1] = '\0';
      }
      return false;
    }
  }
  
  // Parse command and add prefix if needed
  String parsed_command = parseCommandWithPrefix(command);
  
  // Clear result buffer
  if (result_buffer && buffer_size > 0) {
    memset(result_buffer, 0, buffer_size);
  }
  
  // Note: Formatted output is now handled natively by the jumperless C module
  // Functions automatically return formatted strings like "HIGH", "3.300V", "123.4mA"
  
  // Simply execute the command - formatting is now handled by the native C module
  mp_embed_exec_str(parsed_command.c_str());
  
  // Close any files that weren't explicitly closed by the command
  jl_close_all_jfs_files();
  
  // if (result_buffer && buffer_size > 0) {
  //   strncpy(result_buffer, "Formatted by native module", buffer_size - 1);
  //   result_buffer[buffer_size - 1] = '\0';
  // }
  
  return true;
}

/**
 * Execute a single MicroPython command and return float result
 * Useful for functions that return numeric values like adc_get(), gpio_get(), etc.
 * 
 * @param command The command to execute (e.g., "gpio_get(2)")
 * @param result Pointer to store the numeric result
 * @return true if command executed successfully and result is valid, false otherwise
 */
bool executeSinglePythonCommandFloat(const char* command, float* result) {
  if (!result) return false;
  
  // Initialize quietly if needed
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      return false;
    }
  }
  
  // Parse command and add prefix if needed
  String parsed_command = parseCommandWithPrefix(command);
  
  bool success = true;
  *result = 0.0f;
  
  // For now, just execute the command directly
  // TODO: Implement proper result capture to get the actual return value
  mp_embed_exec_str(parsed_command.c_str());
  
  // Close any files that weren't explicitly closed by the command
  jl_close_all_jfs_files();
  
  return success;
}

/**
 * Simple convenience function for common commands
 * Returns the result as a float (useful for sensor readings)
 */
float quickPythonCommand(const char* command) {
  float result = 0.0f;
  executeSinglePythonCommandFloat(command, &result);
  return result;
}

/**
 * Test function to demonstrate single command execution
 * Can be called from main.cpp to test the functionality
 */
void testSingleCommandExecution(void) {
  if (!global_mp_stream) return;
  
  global_mp_stream->println("\n=== Testing Single Command Execution ===");
  
  // Ensure MicroPython is initialized with jumperless module
  if (!mp_initialized) {
    global_mp_stream->println("Initializing MicroPython quietly...");
    if (!initMicroPythonQuiet()) {
      global_mp_stream->println("ERROR: Failed to initialize MicroPython!");
      return;
    }
    global_mp_stream->println("MicroPython initialized successfully");
  }
  
  // Test 1: Simple command with automatic prefix
  global_mp_stream->println("Test 1: GPIO read with automatic prefix");
  char result_buffer[64];
  bool success = executeSinglePythonCommand("gpio_get(2)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("Command: gpio_get(2) -> %s (success: %s)\n", 
                           result_buffer, success ? "true" : "false");
  
  // Test 2: Command that already has prefix (should not add another)
  global_mp_stream->println("\nTest 2: Command with existing prefix");
  success = executeSinglePythonCommand("jumperless.dac_set(0, 2.5)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("Command: jumperless.dac_set(0, 2.5) -> %s (success: %s)\n", 
                           result_buffer, success ? "true" : "false");
  
  // Test 3: Python command that should not get prefix
  global_mp_stream->println("\nTest 3: Python command (no prefix)");
  success = executeSinglePythonCommand("print('Hello from Python!')", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("Command: print('Hello from Python!') -> %s (success: %s)\n", 
                           result_buffer, success ? "true" : "false");
  
  // Test 4: Float result function
  global_mp_stream->println("\nTest 4: Float result function");
  float float_result = 0.0f;
  success = executeSinglePythonCommandFloat("adc_get(0)", &float_result);
  global_mp_stream->printf("Command: adc_get(0) -> %.3f (success: %s)\n", 
                           float_result, success ? "true" : "false");
  
  // Test 5: Quick command function
  global_mp_stream->println("\nTest 5: Quick command function");
  float quick_result = quickPythonCommand("gpio_get(1)");
  global_mp_stream->printf("quickPythonCommand('gpio_get(1)') -> %.3f\n", quick_result);
  
  // Test 6: Command parsing demonstration
  global_mp_stream->println("\nTest 6: Command parsing examples");
  String parsed;
  
  parsed = parseCommandWithPrefix("gpio_get(2)");
  global_mp_stream->println("gpio_get(2) -> " + parsed);
  
  parsed = parseCommandWithPrefix("jumperless.dac_set(0, 3.3)");
  global_mp_stream->println("jumperless.dac_set(0, 3.3) -> " + parsed);
  
  parsed = parseCommandWithPrefix("print('test')");
  global_mp_stream->println("print('test') -> " + parsed);
  
  parsed = parseCommandWithPrefix("connect(1, 5)");
  global_mp_stream->println("connect(1, 5) -> " + parsed);
  
  global_mp_stream->println("\n=== Single Command Test Complete ===\n");
}

/**
 * Test function to demonstrate formatted output
 */
void testFormattedOutput(void) {
  if (!global_mp_stream) return;
  
  global_mp_stream->println("\n=== Testing Formatted Output ===");
  
  // Ensure MicroPython is initialized
  if (!mp_initialized) {
    if (!initMicroPythonQuiet()) {
      global_mp_stream->println("ERROR: Failed to initialize MicroPython!");
      return;
    }
  }
  
  char result_buffer[64];
  bool success;
  
  // Test GPIO state formatting
  global_mp_stream->println("\nGPIO State Formatting:");
  success = executeSinglePythonCommandFormatted("gpio_get(2)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  gpio_get(2) -> %s\n", result_buffer);
  
  // Test voltage formatting
  global_mp_stream->println("\nVoltage Formatting:");
  success = executeSinglePythonCommandFormatted("dac_get(0)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  dac_get(0) -> %s\n", result_buffer);
  
  success = executeSinglePythonCommandFormatted("adc_get(1)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  adc_get(1) -> %s\n", result_buffer);
  
  // Test current formatting
  global_mp_stream->println("\nCurrent/Power Formatting:");
  success = executeSinglePythonCommandFormatted("ina_get_current(0)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  ina_get_current(0) -> %s\n", result_buffer);
  
  success = executeSinglePythonCommandFormatted("ina_get_power(0)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  ina_get_power(0) -> %s\n", result_buffer);
  
  // Test GPIO direction formatting
  global_mp_stream->println("\nGPIO Direction Formatting:");
  success = executeSinglePythonCommandFormatted("gpio_get_dir(3)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  gpio_get_dir(3) -> %s\n", result_buffer);
  
  // Test GPIO pull formatting
  global_mp_stream->println("\nGPIO Pull Formatting:");
  success = executeSinglePythonCommandFormatted("gpio_get_pull(4)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  gpio_get_pull(4) -> %s\n", result_buffer);
  
  // Test connection formatting
  global_mp_stream->println("\nConnection Status Formatting:");
  success = executeSinglePythonCommandFormatted("is_connected(1, 5)", result_buffer, sizeof(result_buffer));
  global_mp_stream->printf("  is_connected(1, 5) -> %s\n", result_buffer);
  
  global_mp_stream->println("\n=== Formatted Output Test Complete ===\n");
}


//Very recent change here

// Filesystem setup and module path configuration
// This function automatically adds /python_scripts/lib and other directories to sys.path
// so you don't need to manually append paths for importing modules like neopixel
void setupFilesystemAndPaths(void) {
  // Silent setup - no output during filesystem initialization
  changeTerminalColor(replColors[13], true, global_mp_stream);
  
  // Set up sys.path for module imports using VFS
  // CRITICAL: This must be called AFTER jl_vfs_mount_root() so VFS is available
  mp_embed_exec_str(
    "import sys\n"
    "# Clear existing sys.path and set up Jumperless-specific paths\n"
    "sys.path.clear()\n"
    "sys.path.append('')  # Current directory (working directory)\n"
    "\n"
    "# Add Jumperless module directories\n"
    "# MicroPython will skip paths that don't exist during import, so it's safe to add them all\n"
    "sys.path.append('/python_scripts')\n"
    "sys.path.append('/python_scripts/lib')\n"
    "sys.path.append('/python_scripts/modules')\n"
    "sys.path.append('/python_scripts/examples')\n"
    "\n"
    "# Debug: Print sys.path to verify setup\n"
    "#print('Module search paths:')\n"
    "#for i, p in enumerate(sys.path):\n"
    "   # print('  ' + str(i) + ': ' + (p if p else '/ (cwd)'))\n"
  );
  
  // Test basic module availability
  // mp_embed_exec_str(
  //   "try:\n"
  //   "    # Test that we can import basic modules\n"
  //   "    import time\n"
  //   "    print('✓ time module available')\n"
  //   "except ImportError:\n"
  //   "    print('✗ time module not available')\n"
  //   "\n"
  //   "try:\n"
  //   "    import os\n"
  //   "    print('✓ os module available')\n"
  //   "except ImportError:\n"
  //   "    print('✗ os module not available')\n"
  //   "\n"
  //   "try:\n"
  //   "    import gc\n"
  //   "    print('✓ gc module available')\n"
  //   "except ImportError:\n"
  //   "    print('✗ gc module not available')\n"
  // );
}

/**
 * Comprehensive file cleanup function
 * Closes all potentially open files across the entire system
 * 
 * CRITICAL: Must pause Core 2 during file operations to prevent concurrent
 * filesystem access which can cause crashes. Files are flushed before closing
 * to ensure all buffered data is written to disk.
 */
void closeAllOpenFiles(void) {
  // Ensure Core 2 is paused during file operations
  bool was_paused = pauseCore2ForFlash(100);
  
  // 1. Close global file handles from FileParsing.cpp
  closeAllFiles();
  
  // 2. Close all JFS file handles opened from MicroPython
  // This is CRITICAL to prevent file conflicts with eKilo and other parts of the system
  // jl_close_all_jfs_files() now flushes files before closing
  jl_close_all_jfs_files();
  
  // 3. File manager cleanup is handled automatically when it goes out of scope
  
  // 4. Additional delay to ensure filesystem operations complete
  delay(10);
  yield();
  
  // Restore previous pauseCore2 state
  unpauseCore2ForFlash(was_paused);
}








