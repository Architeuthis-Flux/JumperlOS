/*
 * MicroPython port configuration for Jumperless embedding
 * Based on the embed port with built-in modules enabled
 *
 * ORGANIZATION
 * ------------
 * This file is grouped into the same logical sections used by
 * CodeDocs/mpExplain.md so the two can be read side by side:
 *
 *   0.  Port includes & typedefs
 *   1.  Feature profile level
 *   2.  Core runtime
 *   3.  RAM / GC / PSRAM heap
 *   4.  Flash & filesystem (VFS)
 *   5.  Compiler, execution speed & runtime optimizations
 *   6.  Native code generation (emitters / assemblers)
 *   7.  Core data types & language representation
 *   8.  REPL, shell interaction & debugging diagnostics
 *   9.  Multicore threads & concurrency
 *   10. Built-in standard Python modules (MICROPY_PY_*)
 *   11. Hardware peripheral & driver integration (machine.*)
 *   12. sys / os module configuration
 *   13. Board identity & version banner
 *   14. Port module table, state & VM hooks
 *
 * CONVENTION FOR COMMENTED FLAGS
 * ------------------------------
 * Lines marked `// [avail]` are flags listed in CodeDocs/mpExplain.md that
 * we do NOT currently override. They are left commented so the full menu of
 * tunables is visible in one place. After each is the default source it
 * inherits from when left unset (mpconfig.h global default, ROM-level group,
 * or "doc-only" when the doc name does not exist in this MicroPython tree).
 * Uncomment + set a value to override. Do not assume a `// [avail]` line is
 * active — it is documentation, not configuration.
 */

// =============================================================================
// 0. Port includes & typedefs
// =============================================================================
#include <stdint.h>
#include <alloca.h>
#include <stddef.h>
#include <limits.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX 0x7fffffff
#endif

// Basic type definitions
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef float mp_float_t;
typedef long mp_off_t;

// Hardware abstraction layer (HAL) types for machine module
typedef uint32_t mp_hal_pin_obj_t;

// Remove conflicting typedefs - let MicroPython define these
// typedef struct _mp_obj_base_t mp_obj_base_t;
// typedef struct _mp_obj_t *mp_obj_t;

#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#define MICROPY_ALLOC_PATH_MAX      (256)

// =============================================================================
// 1. Feature profile level
// =============================================================================
// Sets the baseline feature group. FULL_FEATURES unlocks most of the standard
// library surface; bump to MICROPY_CONFIG_ROM_LEVEL_EVERYTHING to also pull in
// the EXTRA/EVERYTHING-gated builtins flagged below.
#define MICROPY_CONFIG_ROM_LEVEL  MICROPY_CONFIG_ROM_LEVEL_FULL_FEATURES

// =============================================================================
// 2. Core runtime
// =============================================================================
#define MICROPY_ENABLE_GC           (1)

// Enable compiler and event-driven REPL for pyexec_event_repl_process_char()
#define MICROPY_ENABLE_COMPILER     (1)
#define MICROPY_REPL_EVENT_DRIVEN   (1)

// CRITICAL: Enable finalizers for proper cleanup of file handles and other resources
// This allows __del__ methods to be called during garbage collection
#define MICROPY_ENABLE_FINALISER    (1)
// NOTE: historical mis-spelled alias kept for any local code that references it.
#define MICROPY_ENABLE_FINALIZER    (1)

#define MICROPY_KBD_EXCEPTION      (1)
#define MICROPY_ENABLE_VM_ABORT (1)

// Pre-allocate a small static buffer used to raise exceptions when the GC heap
// is exhausted or from a scheduled/soft-IRQ context. Without it, a Ctrl-C
// (KeyboardInterrupt via MICROPY_VM_HOOK_LOOP below) or any exception that
// arrives while the heap is full can fail to allocate and wedge the VM. ~256B
// of static RAM buys reliable interrupt/OOM recovery on an interactive board.
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE   (256)

// GC register scanning configuration for ARM Cortex-M33 (RP2350)
// Use setjmp-based fallback for safer, more portable GC register scanning.
// This captures all callee-saved registers reliably via setjmp() instead of
// relying on architecture-specific inline assembly which can be fragile.
#define MICROPY_GCREGS_SETJMP       (1)

// C call-stack bounds checking — raises RuntimeError instead of a hard crash.
#define MICROPY_STACK_CHECK (1)
#define MICROPY_STACK_CHECK_MARGIN (1024)  // 1KB margin for embedded systems

// Cooperative scheduler (micropython.schedule() + soft IRQ callbacks).
#define MICROPY_ENABLE_SCHEDULER    (1)
#define MICROPY_SCHEDULER_DEPTH     (8)

#define MICROPY_HELPER_LEXER_UNIX   (0)
// [avail] MICROPY_STACKLESS                  (0)  // default mpconfig.h: heap-allocated Py->Py calls
// [avail] MICROPY_STACKLESS_STRICT           (0)  // default mpconfig.h: only with MICROPY_STACKLESS

// =============================================================================
// 3. RAM / GC / PSRAM heap
// =============================================================================
// Jumperless v5 can optionally have 8MB PSRAM installed on GPIO 19 (QSPI CS1).
// PSRAM detection is done at runtime - the same firmware works with or without PSRAM.
//
// CRITICAL: MICROPY_GC_SPLIT_HEAP must ALWAYS be enabled so gc_add() is available
// for adding PSRAM to the heap at runtime when detected.
#define MICROPY_GC_SPLIT_HEAP (1)
// Use 32-bit stack entries for larger heap addresses (PSRAM is at 0x11000000)
#define MICROPY_GC_STACK_ENTRY_TYPE uint32_t
// Larger GC stack to avoid slowdowns during full sweeps of PSRAM-backed heap
#define MICROPY_ALLOC_GC_STACK_SIZE (1024)

// Raw memory diagnostics — backs micropython.mem_info() / mem_total().
#define MICROPY_MEM_STATS           (1)
// Pass sizes straight to free/realloc (matches our allocator).
#define MICROPY_MALLOC_USES_ALLOCATED_SIZE (1)

// [avail] MICROPY_GC_SPLIT_HEAP_AUTO         (0)  // default mpconfig.h: auto-grow split heap (esp32 uses 1)
// [avail] MICROPY_TRACK_ALLOCATED_BYTES           // doc-only name; gc.mem_alloc() is always available here

// =============================================================================
// 4. Flash & filesystem (VFS)
// =============================================================================
// Enable standard MicroPython VFS so tools (e.g. mpremote/ViperIDE) behave normally
#define MICROPY_VFS                 (1)
#define MICROPY_VFS_FAT             (0)  // Use custom JFS VFS driver, not FatFs blockdev
#define MICROPY_VFS_LFS2            (0)
#define MICROPY_VFS_POSIX           (0)
// Allow the lexer/reader to pull files via VFS (required for imports/open)
#define MICROPY_READER_VFS          (1)
// Disable legacy hand-written os bridge now that VFS+standard os are available
#define MICROPY_JL_CUSTOM_OS_BRIDGE (0)

// External import (sys.path lookups) + open()/io for file objects.
#define MICROPY_ENABLE_EXTERNAL_IMPORT (1)
#define MICROPY_PY_IO_FILEIO        (1)
#define MICROPY_PY_IO               (1)

// NOTE on filesystem usage reporting (JumperIDE shows used/free on FAT-backed
// boards but not here): we ship a custom JFS VFS whose statvfs() lives in
// modules/jumperless/modjumperless.c. os.statvfs() must return non-zero
// f_bsize / f_frsize for the standard `s[1]*s[2]` / `s[0]*s[3]` usage recipe
// to produce a real number — see that file's jl_vfs_statvfs_method().

// =============================================================================
// 5. Compiler, execution speed & runtime optimizations
// =============================================================================
#define MICROPY_OPT_COMPUTED_GOTO   (1)
#define MICROPY_MODULE_WEAK_LINKS   (1)

// Parser allocation tuning. The parse tree is built from the GC heap, which on
// a PSRAM board may be PSRAM-backed (slower realloc). Larger initial chunks
// mean fewer grow-and-copy cycles when compiling big pasted scripts. Matches
// the sizing the Temporal badge uses.
#define MICROPY_ALLOC_PARSE_RULE_INIT       (128)
#define MICROPY_ALLOC_PARSE_RULE_INC        (32)
#define MICROPY_ALLOC_PARSE_RESULT_INIT     (64)
#define MICROPY_ALLOC_PARSE_RESULT_INC      (32)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT      (256)

// NOTE on "already on (FULL >= EXTRA)": MICROPY_CONFIG_ROM_LEVEL is
// FULL_FEATURES (40), and FULL >= EXTRA_FEATURES (30) >= CORE_FEATURES (10),
// so every flag tagged "already on" below is ALREADY ACTIVE by inheritance
// from the ROM level — it does not need an override here. The tag documents
// where the value comes from; it is not a TODO.
// [avail] MICROPY_OPT_LOAD_ATTR_FAST_PATH         // already on (FULL >= EXTRA)
// [avail] MICROPY_OPT_MAP_LOOKUP_CACHE            // already on (FULL >= EXTRA): per-bytecode attr cache
// [avail] MICROPY_COMP_CONST                      // already on (FULL >= CORE)
// [avail] MICROPY_COMP_CONST_FOLDING              // already on (FULL >= CORE)
// [avail] MICROPY_COMP_MODULE_CONST               // already on (FULL >= EXTRA): cross-module const folding
// [avail] MICROPY_COMP_DOUBLE_TUPLE_ASSIGN        // already on (FULL >= CORE)
// [avail] MICROPY_COMP_TRIPLE_TUPLE_ASSIGN        // already on (FULL >= EXTRA)
// [avail] MICROPY_COMP_RETURN_IF_EXPR             // already on (FULL >= EXTRA)
// [avail] MICROPY_PERSISTENT_CODE_LOAD       (0)  // default mpconfig.h: load .mpy files
// [avail] MICROPY_PERSISTENT_CODE_SAVE       (0)  // default mpconfig.h: save .mpy files
// [avail] MICROPY_ENABLE_DYNRUNTIME          (0)  // default mpconfig.h: native dynamic .mpy modules
// doc-only names (not present in this MicroPython tree):
//   MICROPY_OPT_CACHE_MAP_LOOKUP_IN_BYTECODE, MICROPY_OPT_STORE_ATTR_FAST_PATH,
//   MICROPY_PREV_ALLOC_TYPES, MICROPY_FREE_UNUSED_PARENTS_BEFORE_REALLOC,
//   MICROPY_REUSE_AS_OBJ_IDS

// =============================================================================
// 6. Native code generation (emitters / assemblers)
// =============================================================================
// Enables @micropython.native / @micropython.viper (MICROPY_EMIT_THUMB) and
// @micropython.asm_thumb (MICROPY_EMIT_INLINE_THUMB). The emitter sources
// (emitnative.c / asmthumb.c / emitinlinethumb.c) are compiled in, and the
// RP2350 is a Cortex-M33 that runs the ARMv7-M Thumb-2 these emitters produce.
//
// THE PSRAM HAZARD AND WHY IT IS NOT A "FLAG":
// There is NO MicroPython flag that pins native code to internal SRAM.
// (MICROPY_ALLOC_NATIVE_CHUNK_INIT does not exist; gc_add() ordering only
// creates a *preference*, not a guarantee.) Native code is allocated by
// MP_PLAT_ALLOC_EXEC, whose default is `m_new` (the GC heap). With the optional
// PSRAM mod the heap is a split heap, and py/gc.c gc_alloc() first-fits across
// areas starting from a rolling cursor (MP_STATE_MEM(gc_last_free_area)) — so a
// code block CAN land in the XIP-mapped PSRAM window (0x11000000). Code freshly
// written there via the data path may sit dirty in the XIP cache; executing it
// without cache maintenance can fetch stale bytes and hard-fault (the slowness
// the internet warns about is secondary — the real issue is correctness).
//
// THE FIX (see jl_mp_commit_exec in src/JumperlessMicroPythonAPI.cpp): keep the
// default GC-heap allocator and
// add an MP_PLAT_COMMIT_EXEC hook. mp_asm_base_get_code() calls it once per
// emitted function, right before the code pointer is used (asmbase.h). For
// PSRAM-resident code it cleans+invalidates the XIP cache and issues DSB/ISB;
// for SRAM it is just the barrier. We deliberately do NOT use a custom
// SRAM-pinned MP_PLAT_ALLOC_EXEC: the emitter calls mp_asm_base_deinit(...,
// free_code=false) and nothing else calls MP_PLAT_FREE_EXEC, so only the GC
// reclaims code buffers — a non-GC allocator would leak every redefined
// native function. The commit hook is correct AND leak-free.
//
// Flip JL_ENABLE_NATIVE_CODEGEN to 0 (e.g. -DJL_ENABLE_NATIVE_CODEGEN=0) to
// disable everything here. NOTE: changing this requires re-running
// scripts/build_micropython.sh (it regenerates micropython_embed/ from this
// file and the emitter TUs are config-gated). HARDWARE-TEST the PSRAM path.
#ifndef JL_ENABLE_NATIVE_CODEGEN
#define JL_ENABLE_NATIVE_CODEGEN (1)
#endif

#if JL_ENABLE_NATIVE_CODEGEN
#define MICROPY_EMIT_THUMB              (1)  // @micropython.native / .viper
#define MICROPY_EMIT_INLINE_THUMB       (1)  // @micropython.asm_thumb
// MICROPY_EMIT_THUMB_ARMV7M (1) and MICROPY_EMIT_INLINE_THUMB_FLOAT (1) inherit
// their mpconfig.h defaults, which suit the M33 (ARMv7-M Thumb-2 + FPU).
//
// We do runtime JIT codegen only (@micropython.native compiles in RAM); we do
// NOT load precompiled native .mpy files from the filesystem. Since v1.28,
// MICROPY_PERSISTENT_CODE_LOAD_NATIVE defaults to MICROPY_EMIT_MACHINE_CODE,
// which would turn on host-architecture detection in py/persistentcode.h. That
// host arch table has no entry for Apple Silicon (__aarch64__), so the host-side
// QSTR/preprocess pass fails to build with "Unsupported native architecture".
// Disabling it keeps native codegen working while fixing the host build.
#define MICROPY_PERSISTENT_CODE_LOAD_NATIVE (0)
#ifdef __cplusplus
extern "C" {
#endif
// Implemented in src/JumperlessMicroPythonAPI.cpp. Returns buf unchanged after making
// freshly-emitted code at [buf, buf+len) safe to execute (XIP cache maintenance
// for PSRAM-resident code + DSB/ISB barrier).
void *jl_mp_commit_exec(void *buf, size_t len);
#ifdef __cplusplus
}
#endif
#define MP_PLAT_COMMIT_EXEC(buf, len, opt) jl_mp_commit_exec((buf), (len))
#endif // JL_ENABLE_NATIVE_CODEGEN

// =============================================================================
// 7. Core data types & language representation
// =============================================================================
// Float support - enable single-precision floating point
#define MICROPY_FLOAT_IMPL          (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_BUILTINS_FLOAT   (1)

// Enable arbitrary-precision long integers (fixes small int overflow for e.g., 1 << 30 in rp2.py)
#define MICROPY_LONGINT_IMPL        (MICROPY_LONGINT_IMPL_MPZ)

// [avail] MICROPY_OBJ_REPR              (MICROPY_OBJ_REPR_A)  // default mpconfig.h: 32-bit packed repr
// [avail] MICROPY_STREAMS_NON_BLOCK                           // already on (FULL >= EXTRA)
// [avail] MICROPY_MODULE_BUILTIN_INIT                         // already on (FULL >= EXTRA)

// =============================================================================
// 8. REPL, shell interaction & debugging diagnostics
// =============================================================================
#define MICROPY_HELPER_REPL         (1)
#define MICROPY_REPL_AUTO_INDENT    (1)
#define MICROPY_REPL_EMACS_KEYS     (1)

// Enable error reporting features
#define MICROPY_ERROR_REPORTING     (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_ENABLE_SOURCE_LINE  (1)

// [avail] MICROPY_WARNINGS                    // already on (FULL >= EXTRA): sys.warnoptions
// [avail] MICROPY_DEBUG_PRINTERS         (0)  // default mpconfig.h: verbose internal dump printers
// doc-only debug names (not in this tree): MICROPY_DEBUG_VM_EXEC,
//   MICROPY_DEBUG_PARSE, MICROPY_DEBUG_COMPILER, MICROPY_DEBUG_GC

// =============================================================================
// 9. Multicore threads & concurrency
// =============================================================================
// Threading is OFF: the embed port shares the host (Arduino) core and has no
// mpthreadport backend. Asyncio (cooperative, single-core) covers concurrency.
// [avail] MICROPY_PY_THREAD              (0)  // default mpconfig.h: _thread module
// [avail] MICROPY_PY_THREAD_GIL          (0)  // default mpconfig.h: GIL (requires PY_THREAD)

// =============================================================================
// 10. Built-in standard Python modules (MICROPY_PY_*)
// =============================================================================
#define MICROPY_PY_ARRAY            (1)
#define MICROPY_PY_COLLECTIONS      (1)
#define MICROPY_PY_STRUCT           (1)
#define MICROPY_PY_MATH             (1)
#define MICROPY_PY_GC               (1)
#define MICROPY_PY_BINASCII         (1)
#define MICROPY_PY_ERRNO            (1)
#define MICROPY_PY_JSON             (1)
#define MICROPY_PY_RE               (1)
#define MICROPY_PY_HEAPQ            (1)
#define MICROPY_PY_HASHLIB          (1)
#define MICROPY_PY_RANDOM           (1)

// Builtins / language helpers
#define MICROPY_PY_BUILTINS_COMPILE (1)
#define MICROPY_PY_BUILTINS_EVAL_EXEC (1)
#define MICROPY_PY_BUILTINS_HELP    (1)
#define MICROPY_PY_BUILTINS_INPUT   (1)
#define MICROPY_PY_FSTRINGS         (1)

// Select module - required for asyncio and poll-based I/O
#define MICROPY_PY_SELECT           (1)
#define MICROPY_PY_SELECT_POSIX_OPTIMISATIONS (0)  // No POSIX poll on baremetal
#define MICROPY_PY_SELECT_SELECT    (1)            // Enable select.select() baremetal impl

// Asyncio module (C acceleration: _asyncio provides TaskQueue/Task types)
// Note: Full 'import asyncio' also requires the Python package from
// micropython/extmod/asyncio/ to be available on the filesystem
#define MICROPY_PY_ASYNCIO          (1)

// Framebuffer module for display/pixel manipulation
#define MICROPY_PY_FRAMEBUF         (1)

// 1-Wire bus bit-banging helper
#define MICROPY_PY_ONEWIRE          (1)

// micropython introspection module (mem_info / qstr_info)
#define MICROPY_PY_MICROPYTHON_MEM_INFO (1)

// CRC32 over binascii — cheap, and handy for zip/PNG/protocol checksums in
// Python. Off by default in mpconfig.h regardless of ROM level, so opt in.
#define MICROPY_PY_BINASCII_CRC32   (1)

// Additional module surface available in this tree (not currently overridden):
// [avail] MICROPY_PY_CMATH                    (0)  // default mpconfig.h: complex math
// [avail] MICROPY_PY_MATH_SPECIAL_FUNCTIONS        // already on (FULL >= EXTRA): erf/gamma/...
// [avail] MICROPY_PY_RE_SUB                        // already on (FULL >= EXTRA) (doc says URE_SUB)
// [avail] MICROPY_PY_HASHLIB_SHA256          (1)  // default mpconfig.h (doc says UHASHLIB_SHA256)
// [avail] MICROPY_PY_HASHLIB_SHA1            (0)  // default mpconfig.h
// [avail] MICROPY_PY_HASHLIB_MD5             (0)  // default mpconfig.h
// [avail] MICROPY_PY_CRYPTOLIB               (0)  // default mpconfig.h: AES (doc says UCRYPTOLIB)
// [avail] MICROPY_PY_DEFLATE                      // build advertises `deflate`. moddeflate.c
//         #includes lz77.c which #includes defl_static.c, so de/compression both
//         link; library.json excludes those two as standalone TUs precisely
//         because they are #included (avoids double-compile), NOT because they
//         are missing. Toggle MICROPY_PY_DEFLATE_COMPRESS for the compressor.
// [avail] MICROPY_PY_BUILTINS_SET                 // already on (FULL >= CORE)
// [avail] MICROPY_PY_BUILTINS_FROZENSET           // already on (FULL >= EXTRA)
// [avail] MICROPY_PY_BUILTINS_SLICE               // already on (FULL >= CORE)
// [avail] MICROPY_PY_BUILTINS_PROPERTY            // already on (FULL >= CORE)
// [avail] MICROPY_PY_BUILTINS_MIN_MAX             // already on (FULL >= CORE)
// [avail] MICROPY_PY_BUILTINS_STR_COUNT           // already on (FULL >= CORE)
// [avail] MICROPY_PY_BUILTINS_STR_OP_MODULO       // already on (FULL >= CORE)
// [avail] MICROPY_PY_BUILTINS_EXECFILE            // already on (FULL >= EXTRA)
// [avail] MICROPY_PY_DELATTR_SETATTR              // already on (FULL >= EXTRA)
// [avail] MICROPY_PY_BUILTINS_HELP_MODULES   (0)  // default mpconfig.h: help('modules')
// [avail] MICROPY_PY_SYS_MAXSIZE                  // already on (FULL >= EXTRA)

// =============================================================================
// 11. Hardware peripheral & driver integration (machine.*)
// =============================================================================
#define MICROPY_PY_MACHINE                      (1)
#define MICROPY_PY_MACHINE_RESET                (1)  // Enable machine.reset()
#define MICROPY_PY_MACHINE_BARE_METAL_FUNCS     (1)  // Enable unique_id(), freq()
#define MICROPY_PY_MACHINE_BOOTLOADER           (1)  // Enable machine.bootloader()
#define MICROPY_PY_MACHINE_DISABLE_IRQ_ENABLE_IRQ (1)

// Use extmod/modmachine.c glue; provide only features we implement locally

// Peripherals (extmod glue + rp2 backend files)
#define MICROPY_PY_MACHINE_PWM                  (1)  // Enable PWM
#define MICROPY_PY_MACHINE_PWM_INCLUDEFILE      "../../lib/micropython/port/machine_pwm_jl.c"

#define MICROPY_PY_MACHINE_SPI                  (1)  // Enable hardware SPI
#define MICROPY_PY_MACHINE_SPI_MSB              (1)  // MSB first (standard)
#define MICROPY_PY_MACHINE_SPI_LSB              (0)  // LSB first
#define MICROPY_PY_MACHINE_SOFTSPI              (1)  // Enable software SPI

#define MICROPY_PY_MACHINE_I2C                  (1)  // Enable hardware I2C
#define MICROPY_PY_MACHINE_SOFTI2C              (1)  // Enable software I2C

#define MICROPY_PY_MACHINE_I2S                  (0)  // I2S not implemented yet

#define MICROPY_PY_MACHINE_UART                 (1)  // Already enabled

#define MICROPY_PY_MACHINE_ADC                  (1)  // Enable ADC
#define MICROPY_PY_MACHINE_ADC_INCLUDEFILE      "../../lib/micropython/port/machine_adc_jl.c"

#define MICROPY_PY_MACHINE_WDT                  (1)  // Enable watchdog timer
#define MICROPY_PY_MACHINE_WDT_INCLUDEFILE      "../../lib/micropython/port/machine_wdt_jl.c"

#define MICROPY_PY_MACHINE_BITSTREAM            (1)  // Already enabled
#define MICROPY_PY_MACHINE_PULSE                (0)

// Disable extras to avoid unresolved symbols
#define MICROPY_PY_MACHINE_MEMX                 (0)
#define MICROPY_PY_MACHINE_SIGNAL               (0)
#define MICROPY_PY_MACHINE_PIN_BASE             (0)

// Allow port to extend machine module (e.g., expose Pin, Timer, RTC, etc.)
#define MICROPY_PY_MACHINE_INCLUDEFILE          "../../lib/micropython/port/modmachine_jl.inc"

// =============================================================================
// 12. sys / os module configuration
// =============================================================================
#define MICROPY_PY___FILE__         (1)
#define MICROPY_PY_SYS_PLATFORM     "jumperless-rp2350"
#define MICROPY_PY_SYS_EXIT         (1)
#define MICROPY_PY_SYS_PATH         (1)
#define MICROPY_PY_SYS_PS1_PS2      (1)  // Enable for REPL
#define MICROPY_PY_SYS_STDIO_BUFFER (1)
#define MICROPY_PY_SYS_ATTR_DELEGATION (1)
// Expose sys.stdin, sys.stdout, sys.stderr as Python file objects
// These wrap mp_hal_stdin_rx_chr / mp_hal_stdout_tx_strn from mphalport.c
#define MICROPY_PY_SYS_STDFILES     (1)
// sys.getsizeof() — per-object RAM size. Cheap, and useful for profiling the
// PSRAM-backed heap. Default mpconfig.h only enables it at EVERYTHING (50),
// and we sit at FULL (40), so opt in explicitly.
#define MICROPY_PY_SYS_GETSIZEOF    (1)

// Time module configuration
#define MICROPY_PY_TIME             (1)
#define MICROPY_PY_TIME_TIME_TIME_NS (0)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)
#define MICROPY_PY_TIME_INCLUDEFILE "shared/timeutils/timeutils.h"

// OS module — backed by VFS; provides listdir/stat/statvfs/uname.
#define MICROPY_PY_OS               (1)
#define MICROPY_PY_OS_DUPTERM       (0)
#define MICROPY_PY_OS_DUPTERM_NOTIFY (0)
#define MICROPY_PY_OS_SYNC          (0)
#define MICROPY_PY_OS_UNAME         (1)  // Enable uname function
#define MICROPY_PY_OS_URANDOM       (0)

// Platform module for os.uname()
#define MICROPY_PY_PLATFORM         (1)

// =============================================================================
// 13. Board identity & version banner
// =============================================================================
// User C modules (Jumperless module will be added here)
#define MODULE_JUMPERLESS_ENABLED   (1)

// Board name for sys.platform
#define MICROPY_HW_BOARD_NAME "jumperless-v5"
#define MICROPY_HW_MCU_NAME   "rp2350b"

// Surface the Jumperless firmware version in the REPL banner's machine field
// (the segment after "; ") and in sys.implementation._machine, so external
// tooling like JumperIDE can parse "jumperless-v5 vX.Y.Z.W with rp2350b" from
// the greeting. Single source of truth is the project VERSION file, emitted as
// FIRMWARE_VERSION by scripts/version_from_file.py into
// include/FirmwareVersion.generated.h. That header is only on the include path
// during the PlatformIO firmware build (global -Iinclude/), so guard the include
// with __has_include: the host-side QSTR/embed build (scripts/build_micropython.sh)
// can't see it and falls back to "dev". MICROPY_BANNER_MACHINE is a runtime ROM
// string, not a QSTR, so the fallback never changes the generated headers — the
// real firmware always gets the version baked in.
#if defined(__has_include)
#  if __has_include("FirmwareVersion.generated.h")
#    include "FirmwareVersion.generated.h"
#  endif
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif
#define MICROPY_BANNER_MACHINE \
    MICROPY_HW_BOARD_NAME " v" FIRMWARE_VERSION " with " MICROPY_HW_MCU_NAME

// =============================================================================
// 14. Port module table, state & VM hooks
// =============================================================================
// Built-in modules - minimal set (most modules disabled to save memory)
// Only the jumperless module will be available via MP_REGISTER_MODULE

// Add built-in modules to the list - empty for maximum compatibility
#define MICROPY_PORT_BUILTIN_MODULES \

// Module weak links for compatibility - empty
#define MICROPY_PORT_BUILTIN_MODULE_WEAK_LINKS \

#define MP_STATE_PORT MP_STATE_VM

// Ensure the VM periodically polls for host-driven interrupts (Ctrl-C/Ctrl-D)
#ifdef __cplusplus
extern "C" {
#endif
void mp_hal_check_interrupt(void);
// Forward declare the hook functions - actual declarations are in py/compile.h context
// Using int instead of mp_parse_input_kind_t to avoid header ordering issues
void jl_before_python_exec_hook(int parse_input_kind, unsigned int exec_flags);
void jl_after_python_exec_hook(int parse_input_kind, unsigned int exec_flags, void *exception, int *result);
#ifdef __cplusplus
}
#endif
#define MP_HAL_CHECK_INTERRUPT_DECLARED 1
#define MICROPY_VM_HOOK_LOOP  mp_hal_check_interrupt();

// Hooks around pyexec's parse_compile_execute (raw REPL / USBSer2 event REPL
// only — mp_embed_exec_str does not run these). They fire the MpRemoteService
// onScriptExecutionBegin/Complete callbacks (core-1 GPIO-scan throttling
// during raw scripts), run GC after each raw-REPL script (ViperIDE memory
// stability), and close leaked file handles outside raw REPL mode.
//
// HISTORY: these were commented out in d01cf75 alongside the input() fix, but
// the actual root cause there was the dangling readline state, fixed by the
// readline_init() patch in pyexec.c (scripts/patch_micropython_sources.py).
// With the hooks off, all of the above plumbing was silently dead code.
#define MICROPY_BOARD_BEFORE_PYTHON_EXEC jl_before_python_exec_hook
#define MICROPY_BOARD_AFTER_PYTHON_EXEC  jl_after_python_exec_hook
