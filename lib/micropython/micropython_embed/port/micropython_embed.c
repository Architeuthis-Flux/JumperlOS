/*
 * MicroPython embed API bridge for Jumperless
 * This provides a bridge between Arduino C++ code and the MicroPython runtime
 */

#include <stdio.h>
#include <string.h>
// CRITICAL: Include MicroPython core headers FIRST to define all types
#include "py/compile.h"     // Defines mp_parse_input_kind_t and related types
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/cstack.h"  // Use newer cstack API instead of deprecated stackctrl
#include "py/nlr.h"
#include "py/builtin.h"
#include "py/mphal.h"
#include "shared/runtime/gchelper.h"

#include "micropython_embed.h"

// =============================================================================
// PSRAM Support for Extended MicroPython Heap
// =============================================================================
// When PSRAM is available, we add it as an additional GC heap region using
// MICROPY_GC_SPLIT_HEAP. This gives MicroPython access to ~8MB additional memory.
//
// The Arduino-Pico framework provides PSRAM detection and initialization.
// PSRAM is memory-mapped at address 0x11000000 (PSRAM_BASE on RP2350).
// Detection is done at runtime - the same firmware works with or without PSRAM.
//
// The wrapper function jl_get_psram_size() is implemented in the Arduino C++ code
// and calls rp2040.getPSRAMSize() to detect PSRAM.
#define PSRAM_BASE 0x11000000
// C wrapper for Arduino rp2040.getPSRAMSize() - implemented in Python_Proper.cpp
extern size_t jl_get_psram_size(void);
// Partitioned MicroPython sub-region of PSRAM (everything past the app-side
// PsramArena that holds the undo log / file cache / scratch buffers). Falls
// back to the whole chip when the arena is unavailable. Implemented in
// Python_Proper.cpp.
extern uintptr_t jl_get_psram_mp_base(void);
extern size_t jl_get_psram_mp_size(void);
// Track PSRAM state for reporting
static size_t psram_heap_size = 0;

// Forward declaration of Arduino delay function
void delay(unsigned long ms);

// Forward declarations for Jumperless functions
extern bool jl_in_raw_repl_mode;
void jl_close_all_jfs_files(void);

// Callback function pointers for script execution notifications
// Set by MpRemoteService to receive notifications when scripts start/finish executing
typedef void (*script_callback_t)(void);
extern script_callback_t jl_on_script_begin_callback;
extern script_callback_t jl_on_script_complete_callback;

#ifdef __cplusplus
extern "C" {
#endif

#if MICROPY_PY_MACHINE && MICROPY_PY_MACHINE_UART
// Ensure the UART type from the port backend is retained by the linker
extern const mp_obj_type_t machine_uart_type;
const mp_obj_type_t *jl_retain_machine_uart_type = &machine_uart_type;
#endif

// Pin.irq() lifecycle hooks (machine_pin_jl.c)
extern void machine_pin_irq_init(void);
extern void machine_pin_irq_deinit(void);
// machine.Timer lifecycle hook (machine_timer_jl.c): cancels all live hardware
// alarms so none can fire mp_sched_schedule into a freed/reinitialized heap.
extern void machine_timer_deinit_all(void);

// Minimal stubs for modmachine low-level hooks to satisfy linker
void mp_machine_idle(void) {}
void mp_machine_set_freq(size_t n_args, const mp_obj_t *args) { (void)n_args; (void)args; }
mp_obj_t mp_machine_get_freq(void) { return MP_OBJ_NEW_SMALL_INT(150000000); }
mp_obj_t mp_machine_unique_id(void) { return mp_const_none; }
void mp_machine_lightsleep(size_t n_args, const mp_obj_t *args) { (void)n_args; (void)args; }
NORETURN void mp_machine_deepsleep(size_t n_args, const mp_obj_t *args) { (void)n_args; (void)args; for (;;) {} }
NORETURN void mp_machine_reset(void) { for (;;) {} }
mp_int_t mp_machine_reset_cause(void) { return 0; }

// NOTE: Heap is allocated in Python_Proper.cpp (mp_heap) and passed to mp_embed_init()
// This file does NOT allocate its own heap - that would waste 128KB of RAM!
// The heap variable below is REMOVED - caller provides heap buffer

// Note: HAL functions (arduino_serial_write, arduino_serial_read) are implemented
// in the Arduino C++ code (Python_Proper.cpp) as extern "C" functions

// Initialize MicroPython runtime with optional PSRAM support
int mp_embed_init(void *heap, size_t heap_size, void *stack_top) {
    // Persist the shallowest (highest-address, i.e. boot-time) stack_top ever
    // seen across VM reinits. jl_soft_reboot() re-captured a local's address
    // at its own (deep) call site; if Python later ran at a SHALLOWER depth,
    // gc_helper_collect_regs_and_stack computed (stack_top - &regs) as a
    // negative number cast to size_t and the GC scanned ~4GB of address space
    // -> hard fault. The stack grows down, so taking the max is always the
    // real top.
    static void *jl_boot_stack_top = NULL;
    if (jl_boot_stack_top == NULL || (uintptr_t)stack_top > (uintptr_t)jl_boot_stack_top) {
        jl_boot_stack_top = stack_top;
    }
    stack_top = jl_boot_stack_top;

    // Use the newer cstack API with proper stack limit initialization.
    //
    // Core 0's stack is the two 4KB scratch banks (8KB total: Core 1 is moved
    // to a separate heap stack via core1_separate_stack in src/main.cpp, and
    // MSPLIM guards the floor). The previous 32KB/16KB values here were
    // fiction: MICROPY_STACK_CHECK could never fire before the hardware STKOF
    // fault, so deep Python recursion hard-faulted instead of raising
    // RecursionError. 7KB (minus the 1KB MICROPY_STACK_CHECK_MARGIN) allows
    // ~6KB of checked descent from the real stack top, leaving ~2KB of C/ISR
    // headroom above the MSPLIM floor.
    const size_t stack_size = 7 * 1024;
    mp_cstack_init_with_top(stack_top, stack_size);
    
    // Initialize primary GC heap (SRAM)
    gc_init(heap, (char*)heap + heap_size);
    
    // =============================================================================
    // PSRAM Heap Extension (Runtime Detection)
    // =============================================================================
    // Check if PSRAM is present and add it as additional GC heap.
    // This uses MICROPY_GC_SPLIT_HEAP feature to maintain multiple heap regions.
    // The PSRAM is memory-mapped by the Arduino-Pico framework at PSRAM_BASE (0x11000000).
    // Detection is done at runtime - same firmware works with or without PSRAM.
    #if MICROPY_GC_SPLIT_HEAP
    // Only claim the MicroPython sub-region of PSRAM, NOT the whole chip.
    // The app-side PsramArena (undo log, file cache, scratch buffers) lives at
    // PSRAM_BASE; gc_add() plops its area struct + alloc tables at the start
    // of whatever range it's given and memsets them, so adding the full chip
    // here silently wiped the undo rings (restored history reverted as no-ops)
    // and let GC bookkeeping keep scribbling over the arena at runtime.
    // jl_get_psram_mp_base/size return the partition computed by
    // psram_arena_init(), or the whole chip when no arena was reserved.
    size_t mp_psram_size = jl_get_psram_mp_size();
    uintptr_t mp_psram_base = jl_get_psram_mp_base();
    if (mp_psram_size > 0 && mp_psram_base != 0) {
        // Note: gc_add() handles the region as a separate heap segment
        gc_add((void *)mp_psram_base, (void *)(mp_psram_base + mp_psram_size));
        psram_heap_size = mp_psram_size;
    }
    #endif
    
    mp_init();

    // Zero pin IRQ root pointers + install the shared GPIO IRQ handler.
    // Must run after mp_init() so MP_STATE_PORT is the fresh VM state.
    machine_pin_irq_init();
    return 0;
}

// Get PSRAM heap size (returns 0 if PSRAM not present)
size_t mp_embed_get_psram_size(void) {
    return psram_heap_size;
}

// Deinitialize MicroPython runtime
void mp_embed_deinit(void) {
    // Disable all Pin.irq() interrupts and cancel all machine.Timer alarms
    // BEFORE tearing down the VM, so no ISR/alarm can call into freed heap
    // objects. Timer cancel must run while the heap is still valid (the
    // registry points at heap objects).
    machine_pin_irq_deinit();
    machine_timer_deinit_all();
    mp_deinit();
}

// Non-zero while the VM is executing code via mp_embed_exec_str. Read by
// MpRemoteService::service() so it never feeds USBSer2 characters into
// pyexec (nested VM execution) or soft-reboots the VM out from under a
// script that is still on the C stack (both were live crash paths: service()
// is reached from inside time.sleep via mp_hal_delay_ms -> serviceCritical).
volatile int jl_vm_exec_depth = 0;

// Execute a string of Python code
int mp_embed_exec_str(const char *str) {
    if (!str) return -1;
    
    nlr_buf_t nlr;
    jl_vm_exec_depth++;
    if (nlr_push(&nlr) == 0) {
        // Compile and execute the string
        qstr source_name = qstr_from_str("<stdin>");
        mp_parse_input_kind_t input_kind = MP_PARSE_FILE_INPUT;
        mp_lexer_t *lex = mp_lexer_new_from_str_len(source_name, str, strlen(str), 0);
        if (lex) {
            mp_parse_tree_t parse_tree = mp_parse(lex, input_kind);
            // Compile as non-REPL file input so expression statements do NOT echo results
            mp_obj_t module_fun = mp_compile(&parse_tree, source_name, false);
            if (module_fun != MP_OBJ_NULL) {
                mp_call_function_0(module_fun);
                
                // CRITICAL: Disable interrupts BEFORE mp_handle_pending().
                // If mp_handle_pending(true) raises a pending exception (e.g. KeyboardInterrupt
                // set during the last VM instruction), the nlr_jump lands in the else branch
                // below. With interrupt char set to -1, the exception handler's traceback output
                // won't re-trigger interrupts via mp_hal_check_interrupt() in
                // mp_hal_stdout_tx_strn_cooked(). Matches parse_compile_execute() behavior.
                mp_hal_set_interrupt_char(-1);
                mp_handle_pending(true); 
            }
        }
        nlr_pop();
        jl_vm_exec_depth--;
        return 0;
    } else {
        // Uncaught exception (e.g. KeyboardInterrupt from input()/sleep())
        //
        // CRITICAL: Disable interrupts and clear pending state BEFORE printing
        // the traceback. Without this, the traceback output path calls
        // mp_hal_check_interrupt() (via mp_hal_stdout_tx_strn_cooked every 50 chars)
        // which can see stale mp_interrupt_requested=true left by nlr_jump skipping
        // the normal cleanup. That re-calls mp_sched_keyboard_interrupt(), setting
        // mp_pending_exception with NO active NLR frame -- causing nlr_jump_fail
        // (device crash) or garbled prompt output (1-3 '>' chars instead of '>>>').
        //
        // This matches upstream parse_compile_execute() in pyexec.c.
        mp_hal_set_interrupt_char(-1);  // Prevent re-triggering during traceback output
        mp_handle_pending(false);       // Discard any stale pending exception/callbacks
        
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        jl_vm_exec_depth--;
        return -1;
    }
}

// REPL functionality - basic implementation
void mp_embed_repl(void) {
    // This is a simple REPL implementation
    // In practice, the Jumperless firmware handles REPL state management
    // in Python_Proper.cpp, so this is mainly a stub for API completeness
    printf(">>> ");
    
    // The actual REPL loop is handled by the Arduino code
    // This function exists primarily for API compatibility
}

// CRITICAL: Hook called before Python script execution begins
// This function is called by MicroPython's parse_compile_execute() before each
// script starts executing. It's defined as MICROPY_BOARD_BEFORE_PYTHON_EXEC in mpconfigport.h
//
// Purpose:
// 1. Notify MpRemoteService that a script is about to execute
// 2. Allow pre-execution setup or logging
// 3. Track script execution timing
//
// Parameters:
//   parse_input_kind - Type of input (MP_PARSE_FILE_INPUT=1, MP_PARSE_SINGLE_INPUT=2, etc.)
//   exec_flags - Execution flags (EXEC_FLAG_PRINT_EOF, etc.)
//
// Note: Using int/unsigned int in declaration (mpconfigport.h) to avoid header ordering issues
void jl_before_python_exec_hook(int parse_input_kind, unsigned int exec_flags) {
    (void)exec_flags;  // Unused for now
    
    // Only notify for file input (complete scripts), not for REPL single-line inputs
    // MP_PARSE_FILE_INPUT = 1, MP_PARSE_SINGLE_INPUT = 2
    if (parse_input_kind == 1 /* MP_PARSE_FILE_INPUT */) {
        // Notify MpRemoteService that script execution is beginning (if in raw REPL)
        if (jl_in_raw_repl_mode && jl_on_script_begin_callback) {
            jl_on_script_begin_callback();
        }
    }
}

// CRITICAL: Hook called after every Python script execution
// This function is called by MicroPython's parse_compile_execute() after each
// script completes (successfully or with exception). It's defined as
// MICROPY_BOARD_AFTER_PYTHON_EXEC in mpconfigport.h
//
// Purpose:
// 1. Trigger garbage collection to free memory for next script execution
// 2. Close any leaked file handles from scripts that didn't clean up properly
// 3. Provide stable memory state for ViperIDE and other tools
//
// Parameters:
//   parse_input_kind - Type of input (MP_PARSE_FILE_INPUT=1, MP_PARSE_SINGLE_INPUT=2, etc.)
//   exec_flags - Execution flags (EXEC_FLAG_PRINT_EOF, etc.)
//   exception - Pointer to exception object if one occurred (nlr.ret_val), NULL if success
//   result - Pointer to result code from execution
//
// Note: Using int/unsigned int in declaration (mpconfigport.h) to avoid header ordering issues,
// but treating them as mp_parse_input_kind_t and mp_uint_t internally
void jl_after_python_exec_hook(int parse_input_kind, unsigned int exec_flags, void *exception, int *result) {
    (void)exec_flags;        // Unused for now
    (void)exception;         // Unused for now
    (void)result;            // Unused for now
    
    // CRITICAL: Always run garbage collection after script execution
    // This is especially important for raw REPL mode (ViperIDE/mpremote) where
    // multiple scripts run sequentially. Without GC, memory fragments and causes
    // MemoryError on subsequent runs even when total memory is sufficient.
    //
    // Only run GC for file input (complete scripts), not for REPL single-line inputs
    // to avoid performance issues during interactive use
    // MP_PARSE_FILE_INPUT = 1, MP_PARSE_SINGLE_INPUT = 2
    if (parse_input_kind == 1 /* MP_PARSE_FILE_INPUT */) {
        #if MICROPY_ENABLE_GC
        gc_collect();
        #endif
        
        // CRITICAL: Do NOT close files when in raw REPL mode!
        // Raw REPL (used by ViperIDE/mpremote) sends multiple commands that need
        // file handles to persist across commands. For example:
        //   Command 1: f=open('file.txt','w')
        //   Command 2: f.write('data')
        //   Command 3: f.close()
        // If we close files after Command 1, Command 2 will fail with EIO.
        //
        // Only close leaked files after:
        // - Interactive REPL script execution (handled in Python_Proper.cpp)
        // - Soft reset (handled in jl_soft_reboot)
        // - Python exit (handled by jl_exit_python)
        if (!jl_in_raw_repl_mode) {
            jl_close_all_jfs_files();
        }
        
        // Notify MpRemoteService that script execution completed (if in raw REPL)
        // This allows the service to perform post-execution tasks like logging or cleanup
        if (jl_in_raw_repl_mode && jl_on_script_complete_callback) {
            jl_on_script_complete_callback();
        }
    }
}

// HAL implementations that are required by MicroPython

// Import stat function - removed because VFS provides inline implementation

// Garbage collection function
// CRITICAL: Must properly scan CPU registers AND the entire call stack
// to find all live Python objects. The previous implementation only scanned
// mp_state_ctx which caused live objects to be freed, resulting in crashes
// after running scripts multiple times.
void gc_collect(void) {
    gc_collect_start();
    // gc_helper_collect_regs_and_stack() captures callee-saved registers
    // and scans from current stack position to stack_top for GC roots
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

// Non-local return (exception handling) failure function
void nlr_jump_fail(void *val) {
    // REENTRY GUARD. The recovery attempt below (mp_deinit()/mp_init()) can
    // itself raise - e.g. when the GC heap was never allocated, EVERY
    // allocation raises MemoryError through the emergency exception buffer -
    // and that lands right back here. Without this guard each round nested
    // another ~0x100 bytes of stack every ~2 s (the two delay(1000)s), and
    // core 0 died by STKOF stack overflow 28.5 s after boot: a hard boot
    // loop, SWD-verified on a no-PSRAM V5 (2026-08-18, 28 nested rounds on
    // the dead stack). On reentry: no second recovery, no more printing
    // through MicroPython paths - just park this (unrecoverable) call chain.
    // The flag is set at ENTRY, before the recovery calls, on purpose.
    static volatile bool nlr_fail_active = false;
    if (nlr_fail_active) {
        mp_hal_stdout_tx_strn_cooked("\r\nFATAL: nlr_jump_fail re-entered (recovery raised) - MicroPython parked\r\n", 74);
        for (;;) { delay(1000); }
    }
    nlr_fail_active = true;

    // CRITICAL: Disable ALL interrupt mechanisms FIRST to prevent re-entrant crashes.
    // Without this, mp_hal_stdout_tx_strn_cooked -> mp_hal_check_interrupt could
    // re-trigger the same exception path that got us here.
    // Use mp_hal_set_interrupt_char(-1) which sets keyboard_interrupt_char=-1 AND
    // clears mp_interrupt_requested (keyboard_interrupt_char is static in C++ code).
    extern bool mp_interrupt_requested;
    mp_hal_set_interrupt_char(-1);
    mp_interrupt_requested = false;
    MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;
    
    // Print diagnostic header
    mp_hal_stdout_tx_strn_cooked("\r\nFATAL: nlr_jump_fail - uncaught exception\r\n", 46);
    
    // Try to print the exception details with NLR protection.
    // mp_obj_print_exception can itself raise (e.g., OOM), so we catch that.
    if (val) {
        char buf[80];
        int n = snprintf(buf, sizeof(buf), "  exception val=%p\r\n", val);
        mp_hal_stdout_tx_strn_cooked(buf, n);
        
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(val));
            nlr_pop();
        } else {
            mp_hal_stdout_tx_strn_cooked("  (could not print exception - double fault)\r\n", 47);
        }
    }
    
    mp_hal_stdout_tx_strn_cooked("MicroPython has encountered a fatal error.\r\n", 45);
    mp_hal_stdout_tx_strn_cooked("Attempting recovery via reinit...\r\n", 35);
    
    // CRITICAL: Send raw REPL completion markers to unblock ViperIDE/mpremote.
    // If this crash happened during raw REPL execution, the client is waiting
    // for \x04\x04> to complete the transaction. Without this, ViperIDE hangs
    // forever. Use raw tx (not cooked) so \x04 bytes aren't CRLF-mangled.
    mp_hal_stdout_tx_strn("\x04\x04>", 3);
    
    // CRITICAL: This function is marked NORETURN - must NOT return!
    // An infinite loop prevents stack corruption from falling through.
    // Reinit MicroPython to allow potential recovery by the REPL loop,
    // then spin forever since the caller's stack frame is invalidated.
    machine_pin_irq_deinit();
    machine_timer_deinit_all();
    mp_deinit();
    delay(1000);
    mp_init();
    machine_pin_irq_init();
    delay(1000);
    // Must never return from NORETURN function - spin forever
    for(;;) { delay(1000); }
}

#ifdef __cplusplus
}
#endif 