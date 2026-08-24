// SPDX-License-Identifier: MIT
#include "MpRemoteService.h"
#include "Debugs.h"
#include "ArduinoStuff.h"  // For USBSer2
#include "Python_Proper.h" // For MicroPython execution
#include "externVars.h"
#include <cstring>
#include "Probing.h"
#include "Commands.h"
// USB is pumped through yield() (mutex-guarded), never raw tud_task().

extern "C" {
#include "py/gc.h"
#include "py/mpstate.h"
#include "py/runtime.h"
#include <micropython_embed.h>

#if USE_NATIVE_PYEXEC_RAW_REPL
// MicroPython's native raw REPL implementation
#include "shared/runtime/pyexec.h"
#endif

// Forward declaration for soft reboot function
void jl_soft_reboot( void );
}

// MicroPython stdout/stderr routing (defined in Python_Proper.cpp)
extern "C" void* global_mp_stream_ptr;
extern Stream* global_mp_stream;
extern Stream* mp_interrupt_check_stream;
extern bool mp_interrupt_requested;
extern bool mp_soft_reset_requested;

// Global flag to indicate if we're in raw REPL mode
// Used by jl_after_python_exec_hook to prevent closing file handles between commands
// CRITICAL: In raw REPL, file handles must persist across multiple commands
bool jl_in_raw_repl_mode = false;

// Callback function pointers for script execution notifications
// Called from jl_before/after_python_exec_hook when scripts begin/complete executing
extern "C" {
    typedef void (*script_callback_t)(void);
    script_callback_t jl_on_script_begin_callback = nullptr;
    script_callback_t jl_on_script_complete_callback = nullptr;
    
    // C-compatible wrappers to call the MpRemoteService callbacks
    static void jl_mp_remote_script_begin_wrapper() {
        // Serial.println("[DEBUG] begin_wrapper called");
        MpRemoteService::getInstance().onScriptExecutionBegin();
    }
    
    static void jl_mp_remote_script_complete_wrapper() {
        MpRemoteService::getInstance().onScriptExecutionComplete();
    }
}

// Singleton instance
MpRemoteService* MpRemoteService::instance = nullptr;

// Global reference
MpRemoteService& mpRemoteService = MpRemoteService::getInstance( );

// Static output buffer pointers for capture callback
static MpRemoteService* s_active_capture = nullptr;

MpRemoteService::MpRemoteService( ) {
    // NO BUFFERS ALLOCATED HERE ANY MORE. This constructor used to `new` four
    // of them - 8 KB code + 2x 4 KB output + 512 B line = 16,896 bytes - memset
    // them, and then never touch them again: nothing in src/, modules/ or the
    // MicroPython port ever read or wrote any of the four, nor the four _len
    // fields beside them. The raw-REPL path streams straight through
    // mp_embed_exec_str and the capture callback; these were left behind by an
    // earlier design.
    //
    // They cost more than their size suggests, because this object is a global
    // (`mpRemoteService` below) and so they were taken by a STATIC INITIALIZER,
    // before setup() - claiming the largest contiguous blocks on a completely
    // empty heap and never giving them back. The knock-on: MicroPython's GC
    // heap allocator could not fit its configured 64 KB afterwards and silently
    // dropped to a 48 KB rung, which is a real user-facing loss (it is the heap
    // every companion script compiles in). Deleting dead code gave the
    // interpreter 16 KB back.
    //
    // The OG build shrinking these to 1536/1024 was a symptom being treated:
    // the comment in the header records the code buffer as "the first
    // allocation to abort() boot" there. It was never needed at any size.
    heapMark("MpRemoteService ctor");

    // Register callbacks for script execution begin/complete notifications
    jl_on_script_begin_callback = jl_mp_remote_script_begin_wrapper;
    jl_on_script_complete_callback = jl_mp_remote_script_complete_wrapper;
}

MpRemoteService& MpRemoteService::getInstance( ) {
    if ( !instance ) {
        instance = new MpRemoteService( );
    }
    return *instance;
}

// Non-zero while the VM is executing code via mp_embed_exec_str
// (lib/micropython/port/micropython_embed.c). See guard below.
extern "C" volatile int jl_vm_exec_depth;

ServiceStatus MpRemoteService::service( ) {
    if ( !m_enabled ) {
        jl_in_raw_repl_mode = false; // Ensure flag is cleared when service is disabled
        return ServiceStatus::IDLE;
    }

    // REENTRANCY GUARD. service() is reached from inside running scripts:
    // time.sleep() -> mp_hal_delay_ms -> jOS.serviceInner() -> here (and
    // the jOS CRITICAL-priority loop can reach us the same way). Feeding
    // USBSer2 bytes into pyexec_event_repl_process_char while a script is
    // already on the C stack is nested VM execution (stack blowup / GC / NLR
    // corruption), and a nested jl_soft_reboot() frees the GC heap out from
    // under the outer script's live frames. Two conditions cover both shapes:
    //   - s_in_service: we are already inside service() (raw-REPL script
    //     executing inside pyexec_event_repl_process_char below).
    //   - jl_vm_exec_depth: a script entered via mp_embed_exec_str (friendly
    //     Serial REPL, apps, boot.py) is still running.
    // Ctrl-C on USBSer2 still works while deferred: mp_hal_check_interrupt()
    // peeks the stream directly and is called throughout script execution.
    static bool s_in_service = false;
    if ( s_in_service || jl_vm_exec_depth > 0 ) {
        return m_in_raw_repl ? ServiceStatus::BUSY : ServiceStatus::IDLE;
    }
    s_in_service = true;

    static bool repl_initialized = false;

    // CRITICAL: Save current stream state BEFORE any setGlobalStreamWithInterrupt calls.
    // Without save/restore, setGlobalStreamWithInterrupt(&USBSer2) permanently
    // redirects stdout to USBSer2, making print() output from Serial REPL scripts
    // invisible — characters appear "dropped" on the Serial terminal.
    Stream* saved_stream = global_mp_stream;

    // One-time event REPL setup on first service pass
    if ( !repl_initialized) {
        if ( m_debug ) {
            Serial.println( "[MpRemote] Initializing event REPL" );
        }
        // Ensure MicroPython is initialized - and HONOR the result. This return
        // value was ignored until 2026-08-18: on a no-PSRAM board whose SRAM
        // heap couldn't fit the MicroPython GC heap, initMicroPythonProper()
        // failed, pyexec_event_repl_init() below ran on a VM with NO heap,
        // every allocation raised MemoryError through the emergency exception
        // buffer with no nlr catcher on this path, and nlr_jump_fail's
        // reinit-retry recursed until core 0 died by stack overflow (STKOF)
        // 28.5 s after boot - a hard boot loop. SWD-verified: 28 nested
        // mp_init->raise->nlr_jump_fail rounds on the dead stack, mp_heap NULL.
        // Everything below this block (RX processing, the Ctrl-D soft-reboot
        // pyexec_event_repl_init) is unreachable until this passes, so this is
        // the single choke point for "MicroPython actually has a VM".
        if ( !ensureMicroPythonInitialized( ) ) {
            static bool s_unavailableWarned = false;
            if ( !s_unavailableWarned ) {
                s_unavailableWarned = true;
                Serial.println( "[MpRemote] MicroPython unavailable (GC heap allocation failed) - raw REPL on USBSer2 disabled" );
            }
            s_in_service = false;
            return ServiceStatus::IDLE;
        }

        // CRITICAL: Redirect MicroPython I/O to USBSer2 AND set correct interrupt (Ctrl+C)
        // This MUST happen before initializing REPL so banner goes to correct stream
        // and interrupt handling is correct for mpremote/ViperIDE
        setGlobalStreamWithInterrupt(&USBSer2);

        // Initialize event-driven REPL (will print banner to USBSer2)
        pyexec_event_repl_init( );
        repl_initialized = true;
        // m_in_raw_repl = false;
    }

    // Check if USBSer2 has data available
    if ( !USBSer2 || USBSer2.available( ) == 0 ) {
        // Restore stream if init block switched it (first call only)
        if (saved_stream && global_mp_stream != saved_stream) {
            setGlobalStreamWithInterrupt(saved_stream);
        }
        s_in_service = false;
        return m_in_raw_repl ? ServiceStatus::BUSY : ServiceStatus::IDLE;
    }

    // CRITICAL: Before processing USBSer2 input, ensure stream and interrupt are set correctly
    // This must happen per-input-batch, not just at initialization, because multiple
    // REPLs (Serial and USBSer2) can be active simultaneously
    setGlobalStreamWithInterrupt(&USBSer2);

    // Process characters one at a time using event-driven REPL
    // This is non-blocking and allows us to service other things
    int processed_count = 0;
    while ( USBSer2.available( ) && processed_count < 8192 ) { // Process max 8192 chars per service call
        int c = USBSer2.read( );
        if ( c < 0 )
            break;

        if ( m_debug || printReceivedPython ) {
            if (c < 0x20) {
                Serial.printf( "[MpRemote] Rx: \\x%02X\r\n", c );
            } else {
                Serial.write( c );
            }
            Serial.flush();
        }

        // Drop stray control bytes that the FRIENDLY REPL can't use. These leak
        // in from the app/terminal's interactive-mode handshake (line-buffering
        // SO 0x0E / SI 0x0F / legacy DC3 0x13, see Jerial.cpp) and from flow
        // control. The native friendly REPL has no SingleCharCommands
        // handling, so any such byte lands in the line buffer and Enter
        // parses it as code -> "SyntaxError: invalid syntax" on an apparently
        // empty line. Allow only the control codes the friendly REPL actually
        // acts on; pass everything >= 0x20 (printable + UTF-8) untouched.
        // Raw REPL (ViperIDE/mpremote) transfers arbitrary bytes verbatim, so
        // this filtering is gated to friendly mode only.
        if ( !m_in_raw_repl && c < 0x20 ) {
            switch ( c ) {
                case 0x01: // Ctrl-A: enter raw REPL
                case 0x02: // Ctrl-B: exit to friendly REPL
                case 0x03: // Ctrl-C: interrupt
                case 0x04: // Ctrl-D: EOF / soft reset
                case 0x05: // Ctrl-E: paste mode
                case 0x08: // Backspace
                case 0x09: // Tab
                case 0x0A: // LF
                case 0x0D: // CR (Enter)
                case 0x1B: // ESC (arrow-key / escape sequences)
                    break; // legitimate: fall through to feed the REPL
                default:
                    continue; // stray control byte: drop it                  //!!!!!!!!!!!!
            }
        }

        // Feed character to event-driven REPL
        // Returns 0 normally, PYEXEC_FORCED_EXIT if soft reset requested
        //
        // SAFETY NET: Wrap in NLR to catch stray exceptions (e.g., MemoryError
        // from vstr_add_byte, or stale KeyboardInterrupt). Without this, any
        // nlr_raise inside pyexec with nlr_top == NULL crashes the device.
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            int result = pyexec_event_repl_process_char( c );
            nlr_pop();

            // Check current REPL mode
            extern pyexec_mode_kind_t pyexec_mode_kind;
            m_in_raw_repl = ( pyexec_mode_kind == PYEXEC_MODE_RAW_REPL );
            
            // Update global flag for jl_after_python_exec_hook
            jl_in_raw_repl_mode = m_in_raw_repl;

            if ( result & PYEXEC_FORCED_EXIT ) {
                if ( m_debug ) {
                    Serial.println( "[MpRemote] Soft reset requested via event REPL" );
                }
                mp_soft_reset_requested = true;
            }
        } else {
            // Caught exception from event REPL processing.
            // This prevents nlr_jump_fail -> device crash / USB disconnect.
            mp_hal_set_interrupt_char(-1);
            // MicroPython >= 1.28 takes mp_handle_pending_behaviour_t; the
            // CLEAR_EXCEPTIONS variant == the old bool `false` (run pending
            // callbacks, clear/discard any pending exception).
            mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_CLEAR_EXCEPTIONS);
            mp_interrupt_requested = false;
            MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
            Serial.printf("[MpRemote] Caught stray exception val=%p\r\n", nlr.ret_val);

            // CRITICAL: Send raw REPL completion markers so ViperIDE doesn't hang.
            // The raw REPL protocol expects: OK<stdout>\x04<stderr>\x04>
            // If the exception interrupted parse_compile_execute mid-flight,
            // some or all of these markers may not have been sent. We can't know
            // exactly which were already sent, but sending \x04\x04> is safe:
            // - If ViperIDE already got both \x04s, the extra ones start a new
            //   (empty) response which it will handle gracefully.
            // - If it was waiting for markers, this unblocks it.
            if (m_in_raw_repl && USBSer2 && jl_cdc_wait_writable(&USBSer2, 3, 500)) {
                USBSer2.write('\x04');
                USBSer2.write('\x04');
                USBSer2.write('>');
                USBSer2.flush();
                yield(); // mutex-guarded pump + CDC flush
            }

            // Restore USBSer2 stream state and stop processing this batch
            setGlobalStreamWithInterrupt(&USBSer2);
            break;
        }

        processed_count++;
    }

    // CRITICAL: Flush USBSer2 after processing to ensure EOF markers (\x04) and
    // the raw REPL prompt (>) actually reach the client (ViperIDE/mpremote).
    // parse_compile_execute() writes these via mp_hal_stdout_tx_strn() which does
    // NO flush — bytes can sit in the CDC TX buffer indefinitely without this.
    if (processed_count > 0) {
        USBSer2.flush();
        yield();  // mutex-guarded pump + CDC flush: ensure the transfer actually happens
    }

    // CRITICAL: Handle soft reset requests from the native REPL
    if ( mp_soft_reset_requested ) {
        mp_soft_reset_requested = false;
        mp_interrupt_requested = false;

        if ( m_debug ) {
            Serial.println( "[MpRemote] Executing soft reset via jl_soft_reboot()" );
        }

        // Announce the reset first (mpremote/ViperIDE expect "soft reboot"
        // before the new banner/prompt).
        if ( m_in_raw_repl ) {
            writeResponse( "soft reboot\r\n" );
        }

        // Call our custom soft reset that doesn't corrupt pointers
        jl_soft_reboot( );

        // CRITICAL: Re-initialize the event REPL. jl_soft_reboot() wiped
        // MP_STATE_VM(repl_line) with the rest of the VM state; feeding the
        // next USBSer2 character into pyexec_event_repl_process_char without
        // this dereferences the NULL repl_line and crashes the board (the
        // "dies right after Ctrl-D / mpremote reset" signature).
        // pyexec_event_repl_init() re-enters the mode recorded in
        // pyexec_mode_kind (which survives the VM reset), printing the raw
        // REPL prompt or friendly banner itself.
        setGlobalStreamWithInterrupt(&USBSer2);
        pyexec_event_repl_init( );
        USBSer2.flush( );
    }

    // CRITICAL: Restore the stream that was active before we switched to USBSer2.
    // This ensures Serial REPL script I/O isn't permanently redirected.
    if (saved_stream && saved_stream != &USBSer2) {
        setGlobalStreamWithInterrupt(saved_stream);
    }

    s_in_service = false;
    return m_in_raw_repl ? ServiceStatus::BUSY : ServiceStatus::IDLE;
}


// (probePowerDAC is no longer snapshot/restored here: it is a compat view
// owned by infraEvaluate(), which pins it - nothing a script does can
// legitimately change "which DAC is the probe's". The old file-scope
// initializer also read the cross-TU reference before its dynamic init.)
volatile int switchPositionOnMpRemoteService = switchPosition;
unsigned long lastShowLEDmeasurementsintervalinMpRemoteService = 0;
unsigned long lastReadGPIOIntervalinMpRemoteService = 0;
extern unsigned long showLEDmeasurementsInterval;
extern unsigned long readGPIOInterval;
extern volatile bool switchPositionScriptDirty;  // JumperlessMicroPythonAPI.cpp


void MpRemoteService::onScriptExecutionBegin() {
    // Default implementation - can be overridden for custom behavior
    // This is called before each Python script begins execution in raw REPL mode
    
    if ( m_debug ) {
        Serial.println( "[MpRemote] Script execution beginning" );
    }
    switchPositionOnMpRemoteService = switchPosition;
    switchPositionScriptDirty = false;  // re-arm: raw REPL snapshots per exec
    lastShowLEDmeasurementsintervalinMpRemoteService = showLEDmeasurementsInterval;
    showLEDmeasurementsInterval = 55000;
    lastReadGPIOIntervalinMpRemoteService = readGPIOInterval;
    readGPIOInterval = 5000;
    // probing.routableBufferPower( 0, 1, 1 );
    // refreshConnections( 0, 1, 0 );
    // Example: You could add pre-execution setup here:
    // - Start execution timer
    // - Log script start event
    // - Set up monitoring/profiling
    // - Update UI status
}

void MpRemoteService::onScriptExecutionComplete() {
    // Default implementation - can be overridden for custom behavior
    // This is called after each Python script completes execution in raw REPL mode
    
    // Serial.println( "[MpRemote] Script execution completed" );
    // Only undo a switch position the SCRIPT wrote. Restoring unconditionally
    // clobbered a genuine mid-script physical flip, and probing.checkSwitchPosition()
    // holds its last value when it can't sense, so that write didn't self-heal.
    const bool scriptMovedSwitch = switchPositionScriptDirty;
    if ( switchPositionScriptDirty ) {
        switchPosition = switchPositionOnMpRemoteService;
        switchPositionScriptDirty = false;
    }
    showLEDmeasurementsInterval = lastShowLEDmeasurementsintervalinMpRemoteService;
    readGPIOInterval = lastReadGPIOIntervalinMpRemoteService;

    // Restores happen FIRST so the handback below sees the final switchPosition.
    // (This also releases the core-1 frame hold, which used to be set here directly.)
    //
    // Full display teardown ONLY when the script moved the switch itself - i.e.
    // the way a script latches measure mode. This runs after every raw-REPL
    // exec, so an unconditional teardown wiped the user's parked reading and
    // churned the crossbar once per typed ViperIDE line.
    onPythonSessionEnd( scriptMovedSwitch );

    // probing.routableBufferPower( 1, 1, 0 );
    // refreshConnections( 0, 1, 0 );
    // Example: You could add cleanup, logging, or state management here
    // This fires after GC has run but before returning to the REPL prompt
}

void MpRemoteService::sendRawReplPrompt( ) {
    writeResponse( "raw REPL; CTRL-B to exit\r\n>" );
}

bool MpRemoteService::ensureMicroPythonInitialized( ) {
    if ( !isMicroPythonInitialized( ) ) {
        // Initialize MicroPython with output going to main Serial, not USBSer2
        // This prevents initialization messages from confusing tools like mpremote/ViperIDE
        Stream* saved_stream = global_mp_stream;
        bool result = initMicroPythonProper( &USBSer2, /*preserve_interrupt_char=*/true );
        global_mp_stream = saved_stream; // Restore
        return result;
    }
    return true;
}

void MpRemoteService::writeResponse( const char* str ) {
    // CRITICAL: Check for NULL or invalid pointer before dereferencing
    // Invalid pointers (like 0x00000000) point to boot ROM and cause garbage output
    if ( !str || (uintptr_t)str < 0x10000000 ) {
        Serial.printf( "[MpRemote] ERROR: Invalid string pointer: %p\r\n", str );
        return;
    }

    if ( m_debug ) {
        Serial.printf( "[MpRemote] Tx: " );
        // Print with escape sequences shown
        for ( const char* p = str; *p; p++ ) {
            if ( *p == '\r' )
                Serial.print( "\\r" );
            else if ( *p == '\n' )
                Serial.print( "\\n" );
            else if ( *p < 0x20 )
                Serial.printf( "\\x%02X", *p );
            else
                Serial.print( *p );
        }
        Serial.println( );
    }
    if ( USBSer2 ) {
        writeResponse( str, strlen( str ) );
        return;
    }
}

void MpRemoteService::writeResponse( const char* str, size_t len ) {
    // CRITICAL: Check for NULL or invalid pointer before dereferencing
    if ( !str || (uintptr_t)str < 0x10000000 ) {
        Serial.printf( "[MpRemote] ERROR: Invalid string pointer in writeResponse(len): %p\r\n", str );
        return;
    }

    if ( USBSer2 ) {
        // Chunked write with back-pressure: CDC write() spins forever when the
        // host holds DTR but stops reading. Longer timeout than plain prints
        // (raw REPL protocol markers matter), but still bounded — drop rather
        // than hang the board.
        size_t off = 0;
        while ( off < len ) {
            if ( !jl_cdc_wait_writable( &USBSer2, 1, 500 ) ) {
                Serial.printf( "[MpRemote] WARNING: USBSer2 TX stalled, dropped %u bytes\r\n",
                               (unsigned)( len - off ) );
                return;
            }
            size_t room = (size_t)USBSer2.availableForWrite( );
            size_t n = ( len - off < room ) ? ( len - off ) : room;
            USBSer2.write( (const uint8_t*)str + off, n );
            off += n;
        }
        USBSer2.flush( );
    }
}

void MpRemoteService::writeByte( uint8_t b ) {
    if ( m_debug ) {
        Serial.printf( "[MpRemote] TxByte: 0x%02X\r\n", b );
    }
    if ( USBSer2 ) {
        if ( !jl_cdc_wait_writable( &USBSer2, 1, 500 ) ) {
            return; // host stopped draining — drop instead of hanging
        }
        USBSer2.write( b );
        USBSer2.flush( );
    }
}
