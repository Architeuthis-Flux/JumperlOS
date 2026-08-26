/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2023 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "py/mphal.h"

#include "py/runtime.h"
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include <string.h>

// Forward declaration for filesystem bridge functions
extern char* jl_fs_read_file(const char* path);
extern void* jl_fs_open_file(const char* path, const char* mode);
extern int jl_fs_stat_isdir(const char* path);
extern int jl_fs_exists(const char* path);

// Import the global stream from our main Arduino code
extern void* global_mp_stream_ptr;
extern void* mp_stdin_locked_stream_ptr;  // When non-NULL, stdin is locked to this stream
extern void arduino_serial_write(const char *str, int len, void *stream);
extern int arduino_serial_read(void *stream);
extern const mp_obj_type_t mp_type_jfs_file;

// Local copy of the JFS file object layout (defined in modules/jumperless/modjumperless.c)
typedef struct _mp_obj_jfs_file_t {
    mp_obj_base_t base;
    void* file_handle;
    uint8_t is_open;
    uint8_t is_binary;
} mp_obj_jfs_file_t;

// Send string of given length to stdout, converting \n to \r\n.
// Note: Implementation moved to Python.cpp for Jumperless-specific functionality
// void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
//     if (global_mp_stream_ptr) {
//         arduino_serial_write(str, len, global_mp_stream_ptr);
//     }
// }

// Send string of given length to stdout (raw).
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    // DEFENSE-IN-DEPTH: When Serial REPL script is executing (lock set),
    // use the locked stream for stdout to prevent MpRemoteService redirection.
    void *out_ptr = (mp_stdin_locked_stream_ptr && mp_stdin_locked_stream_ptr != global_mp_stream_ptr)
                    ? mp_stdin_locked_stream_ptr
                    : global_mp_stream_ptr;
    // CRITICAL: Check for NULL and validate pointer before writing
    // During soft reset, global_mp_stream_ptr might be temporarily invalid
    if (out_ptr && (uintptr_t)out_ptr > 0x20000000) {
        arduino_serial_write(str, len, out_ptr);
    }
    return len;
}

// Send string to stdout (raw).
void mp_hal_stdout_tx_str(const char *str) {
    mp_hal_stdout_tx_strn(str, strlen(str));
}

// Note: mp_hal_set_interrupt_char is implemented in Python_Proper.cpp
// to integrate with our custom interrupt handling system

// Forward declarations for interrupt checking and Arduino timing
extern void mp_hal_check_interrupt(void);
extern void delayMicroseconds(unsigned int us);
extern int getCurrentInterruptChar(void);

// Receive single character from stdin.
// CRITICAL: When no data is available, we must service USB and yield to prevent
// starvation. MicroPython's readline() calls this in a tight for(;;) loop during
// input(). Without USB servicing here, the USB CDC layer desyncs and causes
// hard faults when the script tries to write output after input() returns.
int mp_hal_stdin_rx_chr(void) {
    // Use locked stream if set (prevents MpRemoteService from redirecting stdin
    // to USBSer2 during time.sleep via serviceCritical), else fall back to global.
    void *stdin_stream = mp_stdin_locked_stream_ptr ? mp_stdin_locked_stream_ptr : global_mp_stream_ptr;
    if (stdin_stream) {
        for (;;) {
            // Service USB before reading to ensure any pending USB packets
            // are moved into the CDC FIFO. Prevents character loss when
            // reading in quick succession (e.g., select.poll + read loop).
            extern void service_usb_task(void);
            service_usb_task();
            int c = arduino_serial_read(stdin_stream);
            if (c != -1) {
                // RACE CONDITION FIX: arduino_serial_read() can consume the
                // interrupt character (Ctrl+Q=0x11 / Ctrl+C=0x03) before
                // check_stream_for_interrupt() gets to peek at it. tud_task()
                // inside mp_hal_check_interrupt() delivers USB data to the
                // buffer, but the throttled check_stream_for_interrupt() may
                // not run before the next loop iteration's read consumes it.
                //
                // Without this check, the interrupt byte passes through to
                // MicroPython's readline as a regular character instead of
                // raising KeyboardInterrupt. This causes input() to silently
                // swallow Ctrl+Q/Ctrl+C, requiring multiple presses to interrupt.
                int int_char = getCurrentInterruptChar();
                // Accept BOTH interrupt chars (Ctrl+Q=17, Ctrl+C=3) regardless of
                // which is currently "active". Only gate on int_char >= 0 to respect
                // MicroPython disabling interrupts during exception handling.
                if (int_char >= 0 && (c == 17 || c == 3)) {
                    // Consume the interrupt char - don't return it to readline.
                    // Schedule + raise KeyboardInterrupt properly.
                    mp_sched_keyboard_interrupt();
                    mp_handle_pending(true);  // nlr_raise -> mp_embed_exec_str catch
                    continue;  // Fallback if mp_handle_pending didn't raise
                }
                // Normalize \n (LF, 0x0A) to \r (CR, 0x0D) for MicroPython.
                // MicroPython's readline only accepts \r as Enter/line-end.
                // Some hosts (e.g., Jumperless App interactive mode) send \n
                // for Enter instead of \r. Without this, input() ignores Enter
                // and the user can never submit a line.
                if (c == '\n') {
                    c = '\r';
                }
                return c;
            }
            // No data available - check for interrupts (which also services USB
            // via tud_task internally) and yield to prevent CPU spin
            mp_hal_check_interrupt();

            // CRITICAL: Process any pending exceptions (e.g., KeyboardInterrupt).
            // Without this, mp_sched_keyboard_interrupt() only SETS a flag but
            // the exception is never RAISED because the VM's backwards-jump check
            // doesn't run while we're blocked in C code. This mirrors what upstream
            // MicroPython ports do via MICROPY_EVENT_POLL_HOOK.
            // mp_handle_pending(true) will nlr_jump if an exception is pending,
            // which unwinds through mp_embed_exec_str()'s nlr_push catch frame.
            mp_handle_pending(true);

            // The active input stream can change at runtime (e.g., friendly REPL
            // to raw REPL transport). Re-sync to current stream pointer.
            // But NOT when stdin is locked during script execution — the lock
            // exists precisely to prevent MpRemoteService from redirecting stdin.
            if (!mp_stdin_locked_stream_ptr && stdin_stream != global_mp_stream_ptr) {
                stdin_stream = global_mp_stream_ptr;
                if (!stdin_stream) {
                    return -1;
                }
            }

            delayMicroseconds(100); // Small yield to prevent CPU spin
        }
    }
    // For embedded use, we don't support stdin input
    return -1; // No character available
}



// HAL timing functions - basic implementations for embedded use
// Note: mp_hal_delay_ms and mp_hal_ticks_ms are defined in Python_Proper.cpp
#include <Arduino.h>
#include "hardware/clocks.h"

void mp_hal_delay_us(mp_uint_t us) {
    delayMicroseconds(us);
}

mp_uint_t mp_hal_ticks_us(void) {
    return micros();
}

mp_uint_t mp_hal_ticks_cpu(void) {
    // For RP2040/RP2350, we can use the same as ticks_us
    // In a real implementation, this might use a higher resolution counter
    return micros();
}

// time.time() support — returns seconds since Epoch as mp_obj_t
// Called by extmod/modtime.c when MICROPY_PY_TIME_TIME_TIME_NS is enabled.
// Uses the RP2350's always-on timer (same hardware as machine.RTC).
#include "pico/aon_timer.h"

mp_obj_t mp_time_time_get(void) {
    struct timespec ts;
    if (aon_timer_is_running()) {
        aon_timer_get_time(&ts);
    } else {
        // AON timer not started — return millis-based uptime in seconds
        ts.tv_sec = (time_t)(millis() / 1000);
        ts.tv_nsec = 0;
    }
    return mp_obj_new_int_from_ull((uint64_t)ts.tv_sec);
}

// time.time_ns() support — returns nanoseconds since Epoch
uint64_t mp_hal_time_ns(void) {
    struct timespec ts;
    if (aon_timer_is_running()) {
        aon_timer_get_time(&ts);
    } else {
        ts.tv_sec = (time_t)(millis() / 1000);
        ts.tv_nsec = (long)((millis() % 1000) * 1000000ULL);
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

// Get CPU frequency in Hz - required by machine.bitstream for precise timing
uint32_t mp_hal_get_cpu_freq(void) {
    return clock_get_hz(clk_sys);
}

// Forward declaration for checking serial data availability
extern int arduino_serial_available(void *stream);

// ---- HAL functions required by shared/runtime/sys_stdio_mphal.c ----
// sys_stdio_mphal.c provides the canonical sys.stdin/stdout/stderr objects.
// It needs these three HAL functions from the port:
//   mp_hal_stdin_rx_chr()  — already implemented above
//   mp_hal_stdout_tx_strn_cooked()  — Python_Proper.cpp (the Jumperless
//     implementation: UART response capture, mid-print interrupt checks,
//     OLED copy). Do NOT re-add a stub here: both definitions are strong
//     and -Wl,--allow-multiple-definition hands the win to whichever
//     object links first — a stub here silently shadowed the real one
//     for months (pending finding #17).
//   mp_hal_stdio_poll()  — implemented here

#include "py/stream.h"

// Poll stdin/stdout for readability/writability.
// Called by sys_stdio_mphal.c's ioctl handler for select.poll() support.
mp_uint_t mp_hal_stdio_poll(mp_uint_t poll_flags) {
    // CRITICAL: Service USB BEFORE checking availability.
    // Without this, characters sitting in USB hardware buffers won't be
    // moved into the CDC software FIFO, causing select.poll() to return
    // empty even though data has arrived. This causes dropped characters
    // in tight poll+read loops (e.g., TermRead example from MicroPython
    // discussion #11448).
    extern void service_usb_task(void);
    service_usb_task();

    // Use locked stream if set (prevents MpRemoteService from redirecting
    // stdin to USBSer2 during time.sleep), else fall back to global.
    void *poll_stream = mp_stdin_locked_stream_ptr ? mp_stdin_locked_stream_ptr : global_mp_stream_ptr;

    mp_uint_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) &&
        poll_stream && arduino_serial_available(poll_stream)) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        // USB CDC is always writable
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

