/*
 * Jumperless MicroPython API Wrapper Functions
 *
 * These functions provide a C-compatible interface for MicroPython
 * to call Jumperless functionality directly without string parsing.
 */

#include <errno.h> // For EEXIST, ENOTDIR, EIO errno constants
#include <cstdarg> // For va_list, va_start, va_end
#include <cmath>   // For fabs() float comparison

#include "ArduinoStuff.h"
#include "CH446Q.h"
#include "Commands.h"
#include "FileParsing.h"
#include "FakeGpio.h"

#include "Graphics.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "USBAudio.h"
#include "RotaryEncoder.h"
#include "oled.h"
#include "OledGui.h"  // retained OLED screen system (jl_oled_screen_* bridge)

#include "JumperlessDefines.h"
#include "routing/InfraPaths.h" // infraDacParkEpochBump (dac_set save=False)
#include "SafeString.h"
#include "hardware/gpio.h"

#include "FilesystemStuff.h" // For safe file operations
#include "AsyncPassthrough.h" // For UART IRQ suspension during flash writes
#include "States.h"
#include "WaveGen.h"
#include "externVars.h" // For fs_mutex filesystem synchronization

extern WaveGen wavegen;             // defined in main.cpp

// External declarations
extern SafeString nodeFileString;
// extern void refreshConnections( int ledShowOption = -1, int fillUnused = 1, int clean = 0 );
// lastChipXY is now declared in CH446Q.h as chipXYBitfield[12]

// =============================================================================
// Native-codegen exec commit hook (MP_PLAT_COMMIT_EXEC)
// =============================================================================
// Makes MicroPython runtime-emitted native code (@micropython.native /
// .viper / .asm_thumb) safe to execute when MICROPY_EMIT_THUMB is enabled.
//
// The native emitter writes machine code into a GC-heap buffer via the data
// path, then executes it. With the optional 8MB PSRAM mod the GC heap is a
// split heap (internal SRAM + PSRAM via gc_add), and py/gc.c gc_alloc() can
// place a code block in the XIP-mapped PSRAM window (0x11000000) — there is no
// flag to pin native code to SRAM. Code freshly written into the XIP window can
// sit dirty in the RP2350 XIP cache; before the instruction side fetches it we
// must clean (write back to PSRAM) + invalidate (force re-fetch) the covered
// lines, then DSB/ISB. Internal SRAM has no such cache on the Cortex-M33, so it
// only needs the barrier (run unconditionally — required for self-modifying
// code on Cortex-M regardless).
//
// MicroPython calls this from mp_asm_base_get_code() once per emitted function,
// right before the code pointer is first used (see lib/micropython/port/
// mpconfigport.h section 6, which maps MP_PLAT_COMMIT_EXEC here). We keep the
// default GC-heap MP_PLAT_ALLOC_EXEC so the GC still reclaims code buffers; a
// custom non-GC allocator would leak every redefined native function because
// the emitter calls mp_asm_base_deinit(..., free_code=false) and nothing else
// calls MP_PLAT_FREE_EXEC.
#if defined(__has_include)
#  if __has_include("hardware/xip_cache.h")
#    include "hardware/xip_cache.h"
#    define JL_HAVE_XIP_CACHE 1
#  endif
#endif

extern "C" void *jl_mp_commit_exec( void *buf, size_t len ) {
    if ( buf == nullptr || len == 0 ) {
        return buf;
    }

#if JL_HAVE_XIP_CACHE
    // XIP_BASE / XIP_END / XIP_CACHE_LINE_SIZE come from the SDK headers pulled
    // in by hardware/xip_cache.h. The window covers flash (CS0, 0x10000000) and
    // PSRAM (CS1, 0x11000000); only addresses inside it need cache maintenance.
    const uintptr_t addr = (uintptr_t)buf;
    if ( addr >= XIP_BASE && addr < XIP_END ) {
        // Offsets passed to the cache API are relative to XIP_BASE and must be
        // cache-line aligned; round start down and end up.
        const uintptr_t line = (uintptr_t)XIP_CACHE_LINE_SIZE;
        uintptr_t start = ( addr - XIP_BASE ) & ~( line - 1 );
        uintptr_t end = ( ( addr - XIP_BASE ) + len + line - 1 ) & ~( line - 1 );
        uintptr_t span = end - start;
        // Clean first (commit dirty write data to PSRAM), then invalidate so the
        // instruction fetch re-reads the freshly written bytes from the backing
        // store. Order matters: invalidate-before-clean could discard our writes.
        xip_cache_clean_range( start, span );
        xip_cache_invalidate_range( start, span );
    }
#endif

    // Always: drain the store buffer (DSB) and flush the instruction pipeline
    // (ISB) so subsequent fetches see the new code.
    __asm volatile( "dsb 0xf" ::: "memory" );
    __asm volatile( "isb 0xf" ::: "memory" );

    return buf;
}

#include "Apps.h"
#include "CH446Q.h"
#include "FatFS.h"
#include "NetManager.h"
#include "NetVoltageScan.h"
#include "Probing.h"
#include "Python_Proper.h"
#include "config.h"

#include "MpRemoteService.h"
#include "Jerial.h"  // For OLEDOut stream
#include "JsonState.h"
#include "GraphicOverlays.h"
#include "WokwiParser.h"  // for parseWokwiDiagram()


// MicroPython includes for soft reset
extern "C" {
#include "micropython_embed.h" // For mp_embed_exec_str()
#include "py/cstack.h"         // For mp_cstack_init_with_top()
#include "py/gc.h"             // For gc_collect() and gc_sweep_all()
#include "py/mpstate.h"        // For MP_STATE_VM, MP_STATE_THREAD
#include "py/obj.h"            // For MP_OBJ_NEW_QSTR, MP_OBJ_FROM_PTR
#include "py/runtime.h"        // For mp_init(), mp_deinit(), and dict functions
}

// Forward declarations
int justReadProbe( bool allowDuplicates );
extern "C" void jl_vfs_mount_root( void );   // VFS mounting
extern void setupFilesystemAndPaths( void ); // Filesystem setup
// Pin.irq() lifecycle hooks (machine_pin_jl.c) - must be callable from session
// teardown so no GPIO IRQ stays armed after Python exits.
extern "C" void machine_pin_irq_deinit( void );
// jl_close_all_jfs_files is defined later in the extern "C" block

// Include JumperlOS for service management
#include "JumperlOS.h"

// USB is pumped through yield()/TinyUSB_Device_Task() (mutex-guarded), never raw tud_task().

/**
 * @brief Run essential services during MicroPython execution
 *
 * This is called from mp_hal_delay_ms() to keep the system responsive
 * while Python scripts are running. It runs:
 * - Peripherals service (current sense measurements for marching ants)
 * - TinyUSB task (keep USB alive)
 */
extern "C" void jl_service_python( void ) {
    jOS.serviceAll( );
}

// External stream pointers for MicroPython I/O routing
extern Stream* global_mp_stream;
extern void* global_mp_stream_ptr;
extern Stream* mp_interrupt_check_stream;

// Python connection context - controls whether Python changes persist or are isolated
#define PYTHON_SLOT_NUMBER 99 // Special slot for Python isolated context

PythonConnectionContext connectionContext = PYTHON_CONTEXT_GLOBAL; // Default to global mode
static int pythonEntrySlot = -1;                                   // Track which slot was active when entering Python

// C-compatible wrapper functions for MicroPython
extern "C" {
#include "py/mpthread.h"
// WaveGen C wrappers (C linkage)
void jl_wavegen_set_output( int channel );
void jl_wavegen_set_freq( float hz );
void jl_wavegen_set_wave( int wave );
void jl_wavegen_set_amplitude( float vpp );
void jl_wavegen_set_offset( float v );
void jl_wavegen_set_sweep( float start_hz, float end_hz, float seconds );
void jl_wavegen_start( int start );
void jl_wavegen_stop( void );

// WaveGen getters
int jl_wavegen_get_output( void );
float jl_wavegen_get_freq( void );
int jl_wavegen_get_wave( void );
float jl_wavegen_get_amplitude( void );
float jl_wavegen_get_offset( void );
int jl_wavegen_is_running( void );
void jl_wavegen_get_sweep( float* start_hz, float* end_hz, float* seconds );

void jl_pause_core2( bool pause ) {
    pauseCore2 = pause;
}

void jl_change_terminal_color( int color, bool flush ) {
    changeTerminalColor( color, flush );
}

void jl_cycle_term_color( bool reset, float step, bool flush ) {
    cycleTermColor( reset, step, flush );
}

void jl_print_terminal_colors( void ) {
    printSpectrumOrderedColorCube( );
}
// WaveGen implementation
static float s_wg_sweep_start_hz = 0.0f;
static float s_wg_sweep_end_hz = 0.0f;
static float s_wg_sweep_time_s = 0.0f;
static bool s_wg_user_set_output = false;
static bool s_wg_user_set_freq = false;
static bool s_wg_user_set_wave = false;
static bool s_wg_user_set_amp = false;
static bool s_wg_user_set_offset = false;
void jl_wavegen_set_output( int channel ) {
    // Map 0..3 to WAVEGEN_DAC0..3; also accept rails via same mapping the module uses
    if ( channel < 0 )
        channel = 0;
    if ( channel > 3 )
        channel = 3;
    wavegen.setChannel( (waveGen_channel_t)channel );
    s_wg_user_set_output = true;
}

void jl_wavegen_set_freq( float hz ) {
    if ( hz <= 0.0f )
        hz = 0.0001f;
    wavegen.setFrequency( hz );
    s_wg_user_set_freq = true;
}

void jl_wavegen_set_wave( int wave ) {
    if ( wave < 0 )
        wave = 0;
    if ( wave > 3 )
        wave = 3;
    wavegen.setWaveform( (waveGen_waveform_t)wave );
    s_wg_user_set_wave = true;
}

void jl_wavegen_set_amplitude( float vpp ) {
    // Public API specifies Vpp. Internally we use amplitude as peak value.
    // So convert Vpp to peak amplitude: A = Vpp / 2
    if ( vpp < 0.0f )
        vpp = 0.0f;
    float peak = vpp * 0.5f;
    wavegen.setAmplitude( peak );
    s_wg_user_set_amp = true;
}

void jl_wavegen_set_offset( float v ) {
    wavegen.setOffset( v );
    s_wg_user_set_offset = true;
}

void jl_wavegen_set_sweep( float start_hz, float end_hz, float seconds ) {
    if ( start_hz <= 0.0f )
        start_hz = 0.0001f;
    if ( end_hz <= 0.0f )
        end_hz = 0.0001f;
    if ( seconds < 0.0f )
        seconds = 0.0f;
    s_wg_sweep_start_hz = start_hz;
    s_wg_sweep_end_hz = end_hz;
    s_wg_sweep_time_s = seconds;
}

void jl_wavegen_start( int start ) {
    if ( start ) {
        // Ensure initialized and safe mode
        wavegen.begin( );
        wavegen.setFallbackMode( true );
        // Apply defaults if user didn't set anything yet
        if ( !s_wg_user_set_output ) {
            jl_wavegen_set_output( 1 ); // default DAC1
        }
        if ( !s_wg_user_set_freq ) {
            jl_wavegen_set_freq( 100.0f ); // default 100 Hz
        }
        if ( !s_wg_user_set_wave ) {
            jl_wavegen_set_wave( 0 ); // SINE
        }
        if ( !s_wg_user_set_amp ) {
            jl_wavegen_set_amplitude( 3.3f ); // Vpp
        }
        if ( !s_wg_user_set_offset ) {
            jl_wavegen_set_offset( 1.65f ); // center 0-3.3V
        }
        wavegen.start( );
    } else {
        if ( wavegen.isRunning( ) ) {
            wavegen.stop( );
        }
    }
}

void jl_wavegen_stop( void ) {
    wavegen.stop( );
}

int jl_wavegen_get_output( void ) {
    return (int)wavegen.getChannel( );
}

float jl_wavegen_get_freq( void ) {
    return wavegen.getFrequency( );
}

int jl_wavegen_get_wave( void ) {
    return (int)wavegen.getWaveform( );
}

float jl_wavegen_get_amplitude( void ) {
    // Convert from internal peak to Vpp for external callers
    return wavegen.getAmplitude( ) * 2.0f;
}

float jl_wavegen_get_offset( void ) {
    return wavegen.getOffset( );
}

int jl_wavegen_is_running( void ) {
    return wavegen.isRunning( ) ? 1 : 0;
}

void jl_wavegen_get_sweep( float* start_hz, float* end_hz, float* seconds ) {
    if ( start_hz )
        *start_hz = s_wg_sweep_start_hz;
    if ( end_hz )
        *end_hz = s_wg_sweep_end_hz;
    if ( seconds )
        *seconds = s_wg_sweep_time_s;
}

// DAC Functions
void jl_dac_set( int channel, float voltage, int save ) {
    // if (channel == 0) {
    //     channel = 2;
    // } else if (channel == 1) {
    //     channel = 3;
    // } else if (channel == 2) {
    //     channel = 0;
    // } else if (channel == 3) {
    //     channel = 1;
    // }
    // Any user write that parks the probe-feed DAC outside its window claims
    // it and relocates the feed immediately - persisted or not (dac_set()
    // defaults to save=False, and a script's DAC voltage silently reverting to
    // the probe-power park on the next rebuild is exactly the surprise this
    // avoids). Viability is judged from the hardware voltage the write just
    // recorded, so no epoch games are needed here.
    setDacByNumber( channel, voltage, save, 0, true );
}

float jl_dac_get( int channel ) {
    float voltage = 0.0f;

    // DAC 0/1 report the voltage actually on the pin, including save=False
    // writes; the rails only ever move through the state.
    if ( channel == 0 ) {
        voltage = getDacHardwareVoltage( 0 );
    } else if ( channel == 1 ) {
        voltage = getDacHardwareVoltage( 1 );
    } else if ( channel == 2 ) {
        voltage = globalState.power.topRail;
    } else if ( channel == 3 ) {
        voltage = globalState.power.bottomRail;
    }

    return voltage;
}

// ADC Functions
float jl_adc_get( int channel ) {
    return readAdcVoltage( channel, 16 );
}

// USB Audio (UAC2 microphone). See include/USBAudio.h for the two-layer design:
// these control VISIBILITY and configuration; the host starts and stops the
// actual capture by opening or closing the input device.
#if USB_AUDIO_ENABLE
int jl_usb_audio_enable( void )  { return usb_audio_set_device_enabled( true )  ? 1 : 0; }
int jl_usb_audio_disable( void ) { return usb_audio_set_device_enabled( false ) ? 1 : 0; }
int jl_usb_audio_is_enabled( void ) { return usb_audio_device_enabled( ) ? 1 : 0; }
int jl_usb_audio_is_streaming( void ) { return usb_audio_is_streaming( ) ? 1 : 0; }

int jl_usb_audio_set_channels( int left, int right ) {
    return usb_audio_set_channels( left, right ) ? 1 : 0;
}
void jl_usb_audio_save( void ) { usb_audio_save_config( ); }
int jl_usb_audio_set_rate( int hz ) {
    return usb_audio_set_rate( (uint32_t) hz ) ? 1 : 0;
}
int jl_usb_audio_set_full_scale( float volts ) {
    return usb_audio_set_full_scale( volts ) ? 1 : 0;
}
void jl_usb_audio_set_dc_block( int on ) { usb_audio_set_dc_block( on != 0 ); }

// Flat out-params so the MicroPython side can build the status dict without
// this C++ translation unit ever touching an mp_obj_t.
void jl_usb_audio_status( int *enabled, int *streaming, int *host_open, int *left, int *right,
                          float *full_scale, int *dc_block, int *sample_rate, int *pending_rate,
                          int *frames_sent, int *fifo_overflow, int *adc_overrun,
                          int *late_irq, int *resyncs, int *probe_pauses, int *claim_fail,
                          int *init_fail ) {
    usb_audio_status_t s;
    usb_audio_get_status( &s );
    if ( enabled )       *enabled       = s.enabled ? 1 : 0;
    if ( streaming )     *streaming     = s.streaming ? 1 : 0;
    if ( host_open )     *host_open     = s.host_open ? 1 : 0;
    if ( left )          *left          = s.left_ch;
    if ( right )         *right         = s.right_ch;
    if ( full_scale )    *full_scale    = s.full_scale;
    if ( dc_block )      *dc_block      = s.dc_block ? 1 : 0;
    if ( sample_rate )   *sample_rate   = (int) s.sample_rate;
    if ( pending_rate )  *pending_rate  = (int) s.pending_rate;
    if ( frames_sent )   *frames_sent   = (int) s.frames_sent;
    if ( fifo_overflow ) *fifo_overflow = (int) s.fifo_overflow;
    if ( adc_overrun )   *adc_overrun   = (int) s.adc_overrun;
    if ( late_irq )      *late_irq      = (int) s.late_irq;
    if ( resyncs )       *resyncs       = (int) s.resyncs;
    if ( probe_pauses )  *probe_pauses  = (int) s.probe_pauses;
    if ( claim_fail )    *claim_fail    = (int) s.claim_fail;
    if ( init_fail )     *init_fail     = (int) s.init_fail;
}
#else
// OG / RP2040 has no USB audio (4 ADC channels, no SRAM headroom for a DMA ring
// plus an ISO endpoint). The MicroPython wrappers in modjumperless.c are built
// for every board, so provide stubs rather than leaving them unresolved at
// link time - calling usb_audio_setup() on OG returns False instead of blowing
// up with a NameError, which keeps the scripting surface identical.
int  jl_usb_audio_enable( void )        { return 0; }
int  jl_usb_audio_disable( void )       { return 0; }
int  jl_usb_audio_is_enabled( void )    { return 0; }
int  jl_usb_audio_is_streaming( void )  { return 0; }
// These three report SUCCESS on OG even though there is no audio hardware.
// usb_audio_setup() calls them before jl_usb_audio_enable(), and each raises a
// ValueError from MicroPython on a false return - so returning 0 here made
// usb_audio_setup() throw "channels must be distinct" on a board whose only
// real answer is "not supported". Enable is the one that reports the truth, so
// usb_audio_setup() cleanly returns False.
int  jl_usb_audio_set_channels( int left, int right ) { (void)left; (void)right; return 1; }
int  jl_usb_audio_set_rate( int hz )    { (void)hz; return 1; }
int  jl_usb_audio_set_full_scale( float volts ) { (void)volts; return 1; }
void jl_usb_audio_set_dc_block( int on ) { (void)on; }
void jl_usb_audio_save( void )          { }
void jl_usb_audio_status( int *enabled, int *streaming, int *host_open, int *left, int *right,
                          float *full_scale, int *dc_block, int *sample_rate, int *pending_rate,
                          int *frames_sent, int *fifo_overflow, int *adc_overrun,
                          int *late_irq, int *resyncs, int *probe_pauses, int *claim_fail,
                          int *init_fail ) {
    if ( enabled )       *enabled       = 0;
    if ( streaming )     *streaming     = 0;
    if ( host_open )     *host_open     = 0;
    if ( left )          *left          = 0;
    if ( right )         *right         = 1;
    if ( full_scale )    *full_scale    = 0.0f;
    if ( dc_block )      *dc_block      = 0;
    if ( sample_rate )   *sample_rate   = 0;
    if ( pending_rate )  *pending_rate  = 0;
    if ( frames_sent )   *frames_sent   = 0;
    if ( fifo_overflow ) *fifo_overflow = 0;
    if ( adc_overrun )   *adc_overrun   = 0;
    if ( late_irq )      *late_irq      = 0;
    if ( resyncs )       *resyncs       = 0;
    if ( probe_pauses )  *probe_pauses  = 0;
    if ( claim_fail )    *claim_fail    = 0;
    if ( init_fail )     *init_fail     = 0;
}
#endif

// INA Functions
// NOTE: INA219 uses I2C which may conflict with Core 2 operations (OLED, etc.)
// We temporarily pause Core 2 during I2C operations to prevent bus conflicts
// and potential crashes from concurrent I2C access.

float jl_ina_get_current( int sensor ) {
    bool was_paused = pauseCore2;
    pauseCore2 = true;       // Prevent Core 2 I2C conflicts
    delayMicroseconds( 50 ); // Allow Core 2 to finish any in-progress I2C

    float result = 0.0f;
    if ( sensor == 0 ) {
        result = INA0.getCurrent( );
    } else if ( sensor == 1 ) {
#if defined(OG_JUMPERLESS)
        result = 0.0f;
#else
        result = INA1.getCurrent( );
#endif
    }

    pauseCore2 = was_paused;
    return result;
}

float jl_ina_get_voltage( int sensor ) {
    bool was_paused = pauseCore2;
    pauseCore2 = true;
    delayMicroseconds( 50 );

    float result = 0.0f;
    if ( sensor == 0 ) {
        result = INA0.getBusVoltage( );
    } else if ( sensor == 1 ) {
#if defined(OG_JUMPERLESS)
        result = 0.0f;
#else
        result = INA1.getBusVoltage( );
#endif
    }

    pauseCore2 = was_paused;
    return result;
}

float jl_ina_get_bus_voltage( int sensor ) {
    bool was_paused = pauseCore2;
    pauseCore2 = true;
    delayMicroseconds( 50 );

    float result = 0.0f;
    if ( sensor == 0 ) {
        result = INA0.getBusVoltage( );
    } else if ( sensor == 1 ) {
#if defined(OG_JUMPERLESS)
        result = 0.0f;
#else
        result = INA1.getBusVoltage( );
#endif
    }

    pauseCore2 = was_paused;
    return result;
}

float jl_ina_get_power( int sensor ) {
    bool was_paused = pauseCore2;
    pauseCore2 = true;
    delayMicroseconds( 50 );

    float result = 0.0f;
    if ( sensor == 0 ) {
        result = INA0.getPower( );
    } else if ( sensor == 1 ) {
#if defined(OG_JUMPERLESS)
        result = 0.0f;
#else
        result = INA1.getPower( );
#endif
    }

    pauseCore2 = was_paused;
    return result;
}

// Net voltage scan queries (NetVoltageScan.cpp). Plain array reads written
// on core 2 - non-blocking and safe at REPL speed. All return 1 on success,
// 0 when the scan has no fresh data (scan off, node unrouted, floating).
int jl_scan_node_voltage( int node, float* voltage ) {
    if ( !nodeVoltageValid( node ) ) {
        return 0;
    }
    *voltage = nodeVoltage[ node ];
    return 1;
}

int jl_scan_net_current( int net, float* current_mA, float* voltage,
                         int* fromNode, int* toNode ) {
#if defined(OG_JUMPERLESS)
    return 0; // scanner is V5-only; netCurrentInfo is a single dummy slot
#else
    if ( net <= 0 || net >= MAX_NETS ) {
        return 0;
    }
    const NetCurrentInfo& info = netCurrentInfo[ net ];
    if ( !info.valid ) {
        return 0;
    }
    *current_mA = info.current_mA;
    *voltage = info.voltage;
    *fromNode = info.fromNode;
    *toNode = info.toNode;
    return 1;
#endif
}

int jl_scan_path_current( int pathIndex, float* current_mA ) {
    if ( !pathCurrentKnown( pathIndex ) ) {
        return 0;
    }
    *current_mA = pathCurrentSigned_mA( pathIndex );
    return 1;
}

// GPIO Functions
void jl_gpio_set( int pin, int value ) {
    if ( pin >= 1 && pin <= 10 ) {
        gpioState[ pin - 1 ] = value ? 1 : 0;
        digitalWrite( gpioDef[ pin - 1 ][ 0 ], value );
    } else if ( pin >= 20 && pin <= 27 ) {
        gpioState[ pin - 20 ] = value ? 1 : 0;
        digitalWrite( pin, value );
    }
}

int jl_gpio_get( int pin ) {
    if ( pin >= 1 && pin <= 10 ) {
        // No pre-wait on readingGPIO here: gpioReadWithFloating() acquires the
        // lock itself (with a 100ms takeover timeout). The old timeout-less
        // `while (readingGPIO)` spin could hang this core forever if the
        // holder crashed or was parked during a flash write.
        int reading = gpioReadWithFloating( gpioDef[ pin - 1 ][ 0 ], 50 );

        return reading;

    } else if ( pin >= 20 && pin <= 27 ) {
        return gpio_get( pin );
    }
    return 0;
}



int jl_gpio_get_dir( int pin ) {
    if ( pin >= 1 && pin <= 10 ) {
        return !gpio_get_dir( gpioDef[ pin - 1 ][ 0 ] );
    } else if ( pin >= 20 && pin <= 27 ) {
        return !gpio_get_dir( pin );
    }
    return 0;
}

void jl_gpio_set_dir( int pin, int direction ) {
    if ( pin >= 1 && pin <= 10 ) {
        int config_index = pin - 1;
        int physical_pin = gpioDef[ config_index ][ 0 ];
        // gpio_set_dir expects true == OUTPUT
        gpio_set_dir( physical_pin, (direction == 0) );

        
        // If setting to input (numeric 1), ensure input buffer is enabled
        if ( direction == 1 ) {  // 1 = input
            gpio_set_input_enabled( physical_pin, true );
        } else {
            gpio_set_input_enabled( physical_pin, false );
        }
        
        // Update state using proper state management (0=OUTPUT,1=INPUT)
        globalState.setGpioDirection( config_index, direction );
    } else if ( pin >= 20 && pin <= 27 ) {
        int config_index = pin - 20;
        gpio_set_dir( pin, (direction == 0) );
        
        // If setting to input (numeric 1), ensure input buffer is enabled
        if ( direction == 1 ) {  // 1 = input
            gpio_set_input_enabled( pin, true );
        } else {
            gpio_set_input_enabled( pin, false );
        }
        
        // Update state using proper state management (0=OUTPUT,1=INPUT)
        globalState.setGpioDirection( config_index, direction );
    }
}

int jl_gpio_get_pull( int pin ) {

    if ( pin >= 1 && pin <= 10 ) {
        pin = gpioDef[ pin - 1 ][ 0 ];
        bool pull_up = gpio_is_pulled_up( pin );
        bool pull_down = gpio_is_pulled_down( pin );
        if ( pull_up && pull_down ) {
            return 2; // bus keeper
        } else if ( pull_up ) {
            return 1; // pullup
        } else if ( pull_down ) {
            return -1; // pulldown
        } else {
            return 0; // no pull
        }
    } else if ( pin >= 20 && pin <= 27 ) {
        bool pull_up = gpio_is_pulled_up( pin );
        bool pull_down = gpio_is_pulled_down( pin );
        if ( pull_up && pull_down ) {
            return 2; // bus keeper
        } else if ( pull_up ) {
            return 1; // pullup
        } else if ( pull_down ) {
            return -1; // pulldown
        } else {
            return 0; // no pull
        }
    }
    return 0;
}

void jl_gpio_set_pull( int pin, int pull ) {

    bool pull_up = false;
    bool pull_down = false;

    int config_pull = 0;
    if ( pull == 0 ) {
        pull_up = false;
        pull_down = false;
        config_pull = 2; // no pull
    } else if ( pull == 1 ) {
        pull_up = true;
        pull_down = false;
        config_pull = 1; // pullup
    } else if ( pull == -1 ) {
        pull_up = false;
        pull_down = true;
        config_pull = 0; // pulldown
    } else if ( pull == 2 ) {
        pull_up = true;
        pull_down = true; // bus keeper mode
        config_pull = 3;  // bus keeper
    }

    if ( pin >= 1 && pin <= 10 ) {
        int config_index = pin - 1;  // Save the config array index BEFORE converting pin
        int physical_pin = gpioDef[ config_index ][ 0 ];  // Get physical GPIO number

        // First disable all pulls to clear previous state
        gpio_disable_pulls( physical_pin );
        
        // Apply RP2350 errata fix - toggle input enable to ensure proper state
        gpio_set_input_enabled( physical_pin, false );
        delayMicroseconds( 5 );
        gpio_set_input_enabled( physical_pin, true );
        
        // Give time for pin to discharge/stabilize after disabling pulls
        delayMicroseconds( 50 );
        
        // Now apply the new pull configuration
        gpio_set_pulls( physical_pin, pull_up, pull_down );
        
        // Give pull resistors time to settle (especially important for pulldowns)
        delayMicroseconds( 100 );

        // Update state using proper state management
        globalState.setGpioPull( config_index, config_pull );
    } else if ( pin >= 20 && pin <= 27 ) {
        int config_index = pin - 20;
        
        // First disable all pulls to clear previous state
        gpio_disable_pulls( pin );
        
        // Apply RP2350 errata fix - toggle input enable to ensure proper state
        gpio_set_input_enabled( pin, false );
        delayMicroseconds( 5 );
        gpio_set_input_enabled( pin, true );
        
        // Give time for pin to discharge/stabilize after disabling pulls
        delayMicroseconds( 50 );
        
        // Now apply the new pull configuration
        gpio_set_pulls( pin, pull_up, pull_down );
        
        // Give pull resistors time to settle (especially important for pulldowns)
        delayMicroseconds( 100 );
        
        // Update state using proper state management
        globalState.setGpioPull( config_index, config_pull );
    }
}

void jl_gpio_set_floating_read( int pin, int floating ) {
    if ( pin >= 1 && pin <= 10 ) {
        int index = pin - 1;
        gpioReadFloating[ index ] = floating;
        globalState.config.gpioReadFloating[ index ] = (uint8_t)floating;
        globalState.markDirty();
    } else if ( pin >= 20 && pin <= 27 ) {
        int index = pin - 20;
        gpioReadFloating[ index ] = 0;
        globalState.config.gpioReadFloating[ index ] = 0;
        globalState.markDirty();
    }
}

int jl_gpio_get_floating_read( int pin ) {
    if ( pin >= 1 && pin <= 10 ) {
        int index = pin - 1;
        return gpioReadFloating[ index ] ? 1 : 0;
    } else if ( pin >= 20 && pin <= 27 ) {
        int index = pin - 20;
        return gpioReadFloating[ index ] ? 1 : 0;
    }
    return 0;
}


} // temporarily close extern "C" for C++ declarations

// Forward declaration of getGPIOIndexFromPin (C++ function defined in Peripherals.cpp)
extern int getGPIOIndexFromPin(int pin);

// Forward declaration of gpio_function_map (defined in Peripherals.cpp)
extern gpio_function_t gpio_function_map[10];

extern "C" { // reopen extern "C"

// Debug flag for pin ownership - can be toggled via debugger or serial command
bool debugGpioPinOwnership = false;  // Default to false for production use

// Debug printf helper that works in embedded context
// Uses Serial.printf which actually outputs to console
void jl_debug_printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.print(buffer);
}

// Pin ownership functions for MicroPython timing-critical operations
void jl_gpio_claim_pin( int pin ) {
    // Use the system's existing pin-to-index mapping
    int index = getGPIOIndexFromPin(pin);
    if (index >= 0 && index < 10) {
        // Mark the pin as MicroPython-owned so core 2's readGPIO() skips it.
        // Without this, gpioReadWithFloating() leaves the pad's input buffer
        // DISABLED between scans (RP2350-E9 workaround), which makes SIO reads
        // and Pin.irq() see a dead pin plus phantom pull-twiddle edges.
        // Released by jl_gpio_release_all_pins() on Python exit / soft reboot.
        globalState.config.gpioPythonOwned[index] = true;
        
        // CRITICAL: Update the gpio_function_map to indicate this pin is in SIO mode
        // This prevents other parts of the system from changing the function
        gpio_function_map[index] = GPIO_FUNC_SIO;
        
        // CRITICAL: Memory barrier to ensure Core 2 sees the change
        // This ensures the volatile write is visible to Core 2 immediately
        __dmb();  // Data Memory Barrier
        
        if (debugGpioPinOwnership) {
            Serial.printf("\n*** [MicroPython] Pin %d (index %d) CLAIMED - readGPIO will skip ***\n", pin, index);
            Serial.printf("    GPIO function map[%d] = %d (SIO=%d)\n", index, gpio_function_map[index], GPIO_FUNC_SIO);
        }
    } else {
        Serial.printf("\n*** [MicroPython] WARNING: Pin %d not found in gpioDef ***\n", pin);
    }
}

void jl_gpio_release_pin( int pin ) {
    // Use the system's existing pin-to-index mapping
    int index = getGPIOIndexFromPin(pin);
    if (index >= 0 && index < 10) {
        globalState.config.gpioPythonOwned[index] = false;
        
        // CRITICAL: Memory barrier to ensure Core 2 sees the change
        __dmb();  // Data Memory Barrier
        
        if (debugGpioPinOwnership) {
            Serial.printf("\n*** [MicroPython] Pin %d (index %d) RELEASED ***\n", pin, index);
        }
    }
}

void jl_gpio_release_all_pins( void ) {
    for ( int i = 0; i < 10; i++ ) {
        globalState.config.gpioPythonOwned[ i ] = false;
    }
    
    // CRITICAL: Memory barrier to ensure Core 2 sees all changes
    __dmb();  // Data Memory Barrier
}

} // temporarily close extern "C" for C++ declarations

// Note: setCustomNetName() and hasCustomNetName() are declared in States.h with C++ linkage

// Forward declarations for color parsing (C++ functions that return String)
uint32_t parseColorValue( const String& colorStr, bool& success );
String colorValueToName( uint32_t color );

// Helper to get color name into a C buffer (wraps C++ function)
static void getColorNameIntoBuffer( uint32_t color, char* buffer, size_t bufSize ) {
    String name = colorValueToName( color );
    strncpy( buffer, name.c_str( ), bufSize - 1 );
    buffer[ bufSize - 1 ] = '\0';
}

extern "C" { // reopen extern "C"

// ============================================================================
// Net Information API
// ============================================================================

// Get the name of a specific net
// Returns the net name string, or nullptr if net doesn't exist
const char* jl_get_net_name( int netNum ) {
    if ( netNum < 0 || netNum >= MAX_NETS )
        return nullptr;

    // Check DisplayState for custom name first
    const char* customName = globalState.display.getNetName( netNum );
    if ( customName != nullptr ) {
        return customName;
    }

    // Fall back to default name from net struct
    return globalState.connections.nets[ netNum ].name;
}

// Set a custom name for a net
// Pass empty string or nullptr to reset to default name
void jl_set_net_name( int netNum, const char* name ) {
    if ( netNum < 0 || netNum >= MAX_NETS )
        return;

    setCustomNetName( netNum, name );
    globalState.markDirty( );
}

// Get the color of a net as a 32-bit RGB value (0xRRGGBB)
uint32_t jl_get_net_color( int netNum ) {
    if ( netNum < 0 || netNum >= MAX_NETS )
        return 0;

    // Check for custom color first
    rgbColor color;
    uint32_t rawColor;
    char colorName[ 32 ];

    if ( globalState.display.getNetColor( netNum, color, rawColor, colorName ) ) {
        return rawColor;
    }

    // Return the computed color from the net struct
    rgbColor netColor = globalState.connections.nets[ netNum ].color;
    return ( netColor.r << 16 ) | ( netColor.g << 8 ) | netColor.b;
}

// Get the color name of a net (returns static buffer)
const char* jl_get_net_color_name( int netNum ) {
    static char colorNameBuffer[ 32 ];

    if ( netNum < 0 || netNum >= MAX_NETS ) {
        strcpy( colorNameBuffer, "unknown" );
        return colorNameBuffer;
    }

    // Check for custom color first
    rgbColor color;
    uint32_t rawColor;

    if ( globalState.display.getNetColor( netNum, color, rawColor, colorNameBuffer ) ) {
        return colorNameBuffer;
    }

    // Generate color name from computed color
    rgbColor netColor = globalState.connections.nets[ netNum ].color;
    uint32_t packed = ( netColor.r << 16 ) | ( netColor.g << 8 ) | netColor.b;
    getColorNameIntoBuffer( packed, colorNameBuffer, sizeof( colorNameBuffer ) );
    return colorNameBuffer;
}

// Set the color of a net by name (e.g., "red", "blue", "pink") or hex string (e.g., "#FF0000")
int jl_set_net_color( int netNum, const char* colorStr ) {
    if ( netNum < 0 || netNum >= MAX_NETS || !colorStr )
        return 0;

    String colorString( colorStr );
    bool parseSuccess;
    uint32_t rawColor = parseColorValue( colorString, parseSuccess );

    if ( !parseSuccess ) {
        return 0; // Invalid color
    }

    rgbColor color;
    color.r = ( rawColor >> 16 ) & 0xFF;
    color.g = ( rawColor >> 8 ) & 0xFF;
    color.b = rawColor & 0xFF;

    // Store as custom color
    globalState.display.setNetColor( netNum, color, rawColor, colorStr );
    globalState.markDirty( );

    return 1; // Success
}



// Set the color of a net by RGB values
int jl_set_net_color_rgb( int netNum, int r, int g, int b ) {
    if ( netNum < 0 || netNum >= MAX_NETS )
        return 0;

    rgbColor color;
    color.r = r & 0xFF;
    color.g = g & 0xFF;
    color.b = b & 0xFF;

    uint32_t rawColor = ( color.r << 16 ) | ( color.g << 8 ) | color.b;
    char colorNameBuf[ 32 ];
    getColorNameIntoBuffer( rawColor, colorNameBuf, sizeof( colorNameBuf ) );

    globalState.display.setNetColor( netNum, color, rawColor, colorNameBuf );
    globalState.markDirty( );

    return 1;
}

// Set the color of a net by HSV values (auto-detects 0.0-1.0 vs 0-255 range)
// saturation defaults to max (255) if not provided or < 0
// value defaults to 32 (reasonable LED brightness) if not provided or < 0
int jl_set_net_color_hsv( int netNum, float h, float s, float v ) {
    if ( netNum < 0 || netNum >= MAX_NETS )
        return 0;

    hsvColor hsv;

    // Auto-detect range: if h <= 1.0, assume 0.0-1.0 range, else assume 0-255
    bool normalized = ( h >= 0.0f && h <= 1.0f );

    if ( normalized ) {
        // Convert 0.0-1.0 range to 0-255
        hsv.h = (unsigned char)( h * 255.0f );

        // S defaults to max if not provided or negative
        if ( s < 0.0f ) {
            hsv.s = 255;
        } else {
            hsv.s = (unsigned char)( s * 255.0f );
        }

        // V defaults to 32 (reasonable brightness) if not provided or negative
        if ( v < 0.0f ) {
            hsv.v = 32;
        } else {
            hsv.v = (unsigned char)( v * 255.0f );
        }
    } else {
        // Use 0-255 range directly
        hsv.h = (unsigned char)( (int)h & 0xFF );

        // S defaults to max if not provided or negative
        if ( s < 0.0f ) {
            hsv.s = 255;
        } else {
            hsv.s = (unsigned char)( (int)s & 0xFF );
        }

        // V defaults to 32 (reasonable brightness) if not provided or negative
        if ( v < 0.0f ) {
            hsv.v = 32;
        } else {
            hsv.v = (unsigned char)( (int)v & 0xFF );
        }
    }

    // Convert HSV to RGB
    rgbColor color = HsvToRgb( hsv );
    uint32_t rawColor = ( color.r << 16 ) | ( color.g << 8 ) | color.b;

    char colorNameBuf[ 32 ];
    snprintf( colorNameBuf, sizeof( colorNameBuf ), "hsv(%d,%d,%d)", hsv.h, hsv.s, hsv.v );

    globalState.display.setNetColor( netNum, color, rawColor, colorNameBuf );
    globalState.markDirty( );

    return 1;
}

// Get the number of active nets
int jl_get_num_nets( void ) {
    return numberOfNets;
}




// Get the number of bridges
int jl_get_num_bridges( void ) {
    return globalState.connections.numBridges;
}

// Get nodes in a net as a comma-separated string (returns static buffer)
const char* jl_get_net_nodes( int netNum ) {
    static char nodesBuffer[ 256 ];
    nodesBuffer[ 0 ] = '\0';

    if ( netNum < 0 || netNum >= MAX_NETS )
        return nodesBuffer;

    int pos = 0;
    bool first = true;

    for ( int j = 0; j < MAX_NODES && globalState.connections.nets[ netNum ].nodes[ j ] != 0; j++ ) {
        if ( !first && pos < 250 ) {
            nodesBuffer[ pos++ ] = ',';
        }
        first = false;

        // Get short name for node
        const char* nodeName = definesToChar( globalState.connections.nets[ netNum ].nodes[ j ], 0 );
        if ( nodeName && strlen( nodeName ) > 0 && pos < 250 ) {
            int len = strlen( nodeName );
            if ( pos + len < 255 ) {
                strcpy( &nodesBuffer[ pos ], nodeName );
                pos += len;
            }
        }
    }
    nodesBuffer[ pos ] = '\0';

    return nodesBuffer;
}

// Get bridge info: node1, node2, duplicates for a specific bridge index
int jl_get_bridge( int bridgeIdx, int* node1, int* node2, int* duplicates ) {
    if ( bridgeIdx < 0 || bridgeIdx >= globalState.connections.numBridges )
        return 0;

    if ( node1 )
        *node1 = globalState.connections.bridges[ bridgeIdx ][ 0 ];
    if ( node2 )
        *node2 = globalState.connections.bridges[ bridgeIdx ][ 1 ];
    if ( duplicates )
        *duplicates = globalState.connections.bridges[ bridgeIdx ][ 2 ];

    return 1;
}

} // temporarily close extern "C" for C++ fake GPIO integration

// Note: Fake GPIO implementation has been moved to FakeGpio.cpp
// This file now contains only thin extern "C" wrappers for MicroPython

extern "C" { // reopen extern "C" for MicroPython API wrappers

// Forward declaration for jl_close_all_jfs_files (defined later in this block)
void jl_close_all_jfs_files( void );

// Debug helper functions for C code to print to Serial
void arduino_serial_print(const char* str) {
    if (str) Serial.print(str);
}

void arduino_serial_print_int(int value) {
    Serial.print(value);
}

void arduino_serial_print_ptr(void* ptr) {
    Serial.print("0x");
    Serial.print((unsigned long)ptr, HEX);
}

// ============================================================================
// Fake GPIO C API Wrappers - Forward calls to FakeGpio.cpp
// ============================================================================

// Configure INPUT mode fake GPIO
int jl_fake_gpio_config_input(int node, float threshold_high, float threshold_low) {
    return fakeGpioConfigInput(node, threshold_high, threshold_low);
}

// Configure OUTPUT mode fake GPIO (voltage-based, legacy)
int jl_fake_gpio_config_output(int node, float v_high, float v_low, float threshold_high, float threshold_low) {
    return fakeGpioConfigOutput(node, v_high, v_low, threshold_high, threshold_low);
}

// Configure OUTPUT mode fake GPIO (node-based, preferred)
int jl_fake_gpio_config_output_nodes(int node, int high_node, int low_node, float threshold_high, float threshold_low) {
    return fakeGpioConfigOutputNodes(node, high_node, low_node, threshold_high, threshold_low);
}

// Unified config function (auto-detects mode)
int jl_fake_gpio_config(int node, float v_high, float v_low, float threshold_high, float threshold_low, int mode = -1) {
    return fakeGpioConfig(node, v_high, v_low, threshold_high, threshold_low, mode);
}

// Read fake GPIO pin
int jl_fake_gpio_read(int node) {
    return fakeGpioRead(node);
}

// Write fake GPIO pin
int jl_fake_gpio_write(int node, int state) {
    return fakeGpioWrite(node, state);
}

// Set mode (for MicroPython Pin class compatibility)
void jl_fake_gpio_set_mode(int node, int mode) {
    // This is called from MicroPython Pin.init() to set INPUT/OUTPUT mode
    // The actual configuration should have been done via jl_fake_gpio_config_*
    // This is just for compatibility - the mode is already set during config
    // We could add validation here if needed
    (void)node;  // Unused - mode is set during config
    (void)mode;  // Unused - mode is set during config
}

// Fast toggle functions
int jl_fake_gpio_disconnect(int node1, int node2) {
    return fakeGpioDisconnect(node1, node2);
}

int jl_fake_gpio_reconnect(int node1, int node2) {
    return fakeGpioReconnect(node1, node2);
}


int jl_get_num_paths( int include_duplicates ) {
    // In the current routing pipeline, globalState.connections.numPaths is synchronized to the
    // "primary" path count (non-duplicates). Duplicate paths may exist in paths[] beyond numPaths.
    //
    // Desired behavior:
    // - include_duplicates == 0: return numPaths (exclude duplicates)
    // - include_duplicates != 0: count all valid entries in paths[] (include duplicates)
    if ( !include_duplicates ) {
        return globalState.connections.numPaths;
    }

    // Count all populated paths. Unused entries are cleared to node1=node2=0.
    int count = 0;
    return numberOfPaths;
    // for ( int i = 0; i < MAX_BRIDGES; i++ ) {
    //     if ( globalState.connections.paths[ i ].node1 == 0 || globalState.connections.paths[ i ].node2 == 0|| globalState.connections.paths[ i ].x[0] < 0 && globalState.connections.paths[ i ].y[0] < 0) {
    //         break;
    //     }
    //     count++;
    // }
    // return count;
}


// Get path info for a specific bridge/path index
// Returns formatted string: "node1,node2,net,chip0,chip1,chip2,chip3,x0,x1,x2,x3,x4,x5,y0,y1,y2,y3,y4,y5,duplicate"
const char* jl_get_path_info( int pathIdx ) {
    static char pathBuffer[ 512 ];
    pathBuffer[ 0 ] = '\0';

    // Note: Paths should already be computed by refreshLocalConnections()
    // We don't recompute here to avoid unnecessary overhead
    // If you need to force recomputation, call refreshLocalConnections() first
    
    // Validate index and ensure paths are computed
    if ( pathIdx < 0 || pathIdx >= globalState.connections.numPaths ) {
        // Serial.print( "jl_get_path_info: Invalid index " );
        // Serial.print( pathIdx );
        // Serial.print( ", numPaths=" );
        // Serial.println( globalState.connections.numPaths );
        return pathBuffer;
    }

    const pathStruct& path = globalState.connections.paths[ pathIdx ];

    // Format: node1,node2,net,chips[4],x[6],y[6],duplicate
    snprintf( pathBuffer, sizeof( pathBuffer ),
              "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
              path.node1, path.node2, path.net,
              path.chip[ 0 ], path.chip[ 1 ], path.chip[ 2 ], path.chip[ 3 ],
              path.x[ 0 ], path.x[ 1 ], path.x[ 2 ], path.x[ 3 ], path.x[ 4 ], path.x[ 5 ],
              path.y[ 0 ], path.y[ 1 ], path.y[ 2 ], path.y[ 3 ], path.y[ 4 ], path.y[ 5 ],
              path.duplicate );

    return pathBuffer;
}

// Get all active paths as a formatted string
// Returns count, followed by each path on a new line
const char* jl_get_all_path_info( void ) {
#if defined(OG_JUMPERLESS)
    static char allPathsBuffer[ 1024 ]; // RP2040: scarce SRAM, fewer paths fit
#else
    static char allPathsBuffer[ 4096 ]; // Large buffer for multiple paths
#endif
    allPathsBuffer[ 0 ] = '\0';

    // Note: Paths should already be computed by refreshLocalConnections()
    // We don't recompute here to avoid unnecessary overhead
    
    int numPaths = globalState.connections.numPaths;
    int numBridges = globalState.connections.numBridges;
    int pos = 0;
    
    Serial.print( "jl_get_all_path_info: numBridges=" );
    Serial.print( numBridges );
    Serial.print( ", numPaths=" );
    Serial.println( numPaths );

    // First line: number of paths
    pos += snprintf( allPathsBuffer + pos, sizeof( allPathsBuffer ) - pos, "%d\n", numPaths );

    // Each subsequent line: path info
    for ( int i = 0; i < numPaths && pos < sizeof( allPathsBuffer ) - 256; i++ ) {
        const pathStruct& path = globalState.connections.paths[ i ];
        pos += snprintf( allPathsBuffer + pos, sizeof( allPathsBuffer ) - pos,
                         "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                         path.node1, path.node2, path.net,
                         path.chip[ 0 ], path.chip[ 1 ], path.chip[ 2 ], path.chip[ 3 ],
                         path.x[ 0 ], path.x[ 1 ], path.x[ 2 ], path.x[ 3 ], path.x[ 4 ], path.x[ 5 ],
                         path.y[ 0 ], path.y[ 1 ], path.y[ 2 ], path.y[ 3 ], path.y[ 4 ], path.y[ 5 ],
                         path.duplicate );
    }

    return allPathsBuffer;
}

// Get path info for a connection between two specific nodes
// Returns same format as jl_get_path_info, or empty string if not found
const char* jl_get_path_between( int node1, int node2 ) {
    static char pathBuffer[ 512 ];
    pathBuffer[ 0 ] = '\0';

    // Note: Paths should already be computed by refreshLocalConnections()
    // We don't recompute here to avoid unnecessary overhead
    
    // Serial.print( "jl_get_path_between: Looking for " );
    // Serial.print( node1 );
    // Serial.print( " <-> " );
    // Serial.print( node2 );
    // Serial.print( ", numPaths=" );
    // Serial.println( globalState.connections.numPaths );

    // Search for path matching these nodes (in either order)
    for ( int i = 0; i < globalState.connections.numPaths; i++ ) {
        const pathStruct& path = globalState.connections.paths[ i ];
        if ( ( path.node1 == node1 && path.node2 == node2 ) ||
             ( path.node1 == node2 && path.node2 == node1 ) ) {
            // Found it - format and return
            snprintf( pathBuffer, sizeof( pathBuffer ),
                      "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                      path.node1, path.node2, path.net,
                      path.chip[ 0 ], path.chip[ 1 ], path.chip[ 2 ], path.chip[ 3 ],
                      path.x[ 0 ], path.x[ 1 ], path.x[ 2 ], path.x[ 3 ], path.x[ 4 ], path.x[ 5 ],
                      path.y[ 0 ], path.y[ 1 ], path.y[ 2 ], path.y[ 3 ], path.y[ 4 ], path.y[ 5 ],
                      path.duplicate );
            break;
        }
    }

    return pathBuffer;
}

// Node Functions
int jl_nodes_connect( int node1, int node2, int save, int duplicates ) {

    // Add to RAM state
    // duplicates: -1 = allow, 0 = no duplicates, 1+ = allow N duplicates
    addBridgeToState( node1, node2, duplicates, true );

    // Update shown readings to detect current sense connections
    // This enables the marching ants animation when ISENSE_PLUS/MINUS are connected
    //chooseShownReadings( );

    return 1;
}

int jl_nodes_disconnect( int node1, int node2 ) {
    // Remove from RAM state
    removeBridgeFromState( node1, node2, true );

    // Update shown readings to detect current sense disconnections
    //chooseShownReadings( );

    return 1;
}

int jl_nodes_fast_connect( int node1, int node2, int duplicates ) {
    // OPTIMIZATION: Fast connection with immediate refresh
    // Uses fastRefresh() instead of full refresh for minimal latency
    
    // Add to RAM state
    // duplicates: -1 = allow, 0 = no duplicates, 1+ = allow N duplicates
    addBridgeToState( node1, node2, duplicates, false );

    // Update shown readings to detect current sense connections
    // chooseShownReadings( );
    
    // Fast refresh with immediate hardware update (bypasses Core 2 scheduler)
    fastRefresh( 1 );  // 1 = show LEDs after refresh

    return 1;
}

int jl_nodes_fast_disconnect( int node1, int node2 ) {
    // OPTIMIZATION: Fast disconnection with immediate refresh
    // Uses fastRefresh() instead of full refresh for minimal latency
    
    // Remove from RAM state
    removeBridgeFromState( node1, node2, false );

    // Update shown readings to detect current sense disconnections
    // chooseShownReadings( );
    
    // Fast refresh with immediate hardware update (bypasses Core 2 scheduler)
    fastRefresh( 1 );  // 1 = show LEDs after refresh

    return 1;
}

int jl_nodes_clear( void ) {
    // Pause Core 2 BEFORE modifying state to prevent race conditions
    // Core 2 handles LEDs and may be reading state while we modify it
    bool was_paused = pauseCore2;
    pauseCore2 = true;
    delayMicroseconds( 50 ); // Allow Core 2 to finish any in-progress operations

    // Clear the entire state (safe now that Core 2 is paused)
    globalState.clearAllConnections( );
    // Save the cleared state
    // saveStateToSlot();

    // Unpause Core 2 BEFORE refreshConnections since it internally calls waitCore2
    // and needs Core 2 to be running to process sendAllPathsCore2/showLEDsCore2
    pauseCore2 = was_paused;

    refreshConnections( -1, 1, 0 );
    // waitCore2 is called internally by refreshConnections

    return 1;
}

int jl_nodes_is_connected( int node1, int node2 ) {
    // Check in globalState instead of file
    bool connected = globalState.hasConnection( node1, node2 );
    return connected ? 1 : 0;
}

int jl_nodes_save( int slot ) {
    int target_slot = ( slot == -1 ) ? netSlot : slot; // Use current slot if -1

    // Pause Core 2 while saving to prevent race conditions
    bool was_paused = pauseCore2;
    pauseCore2 = true;
    delayMicroseconds( 50 );

    // Save globalState to YAML
    saveStateToSlot( target_slot );

    // Unpause Core 2 BEFORE refreshConnections since it internally calls waitCore2
    pauseCore2 = was_paused;

    // Refresh connections to make sure everything is in sync
    refreshConnections( );

    return target_slot; // Return the slot that was saved to
}

void jl_init_micropython_local_copy( void ) {
    // Store which slot was active when entering Python
    pythonEntrySlot = netSlot;

    if ( connectionContext == PYTHON_CONTEXT_ISOLATED ) {
        // ISOLATED MODE: Save current state to backup and switch to Python slot
        storeStateBackup( );

        // Load Python slot (or create empty if doesn't exist)
        SlotManager& mgr = SlotManager::getInstance( );
        String errorMsg;

        // Try to load existing Python slot
        if ( !mgr.slotExists( PYTHON_SLOT_NUMBER ) ) {
            // Create empty Python slot
            mgr.getActiveState( ).clear( );
            mgr.saveSlot( PYTHON_SLOT_NUMBER, errorMsg );
        }

        // Load Python slot into active state
        if ( mgr.loadSlot( PYTHON_SLOT_NUMBER, errorMsg ) ) {
            netSlot = PYTHON_SLOT_NUMBER;
            mgr.setActiveSlot( PYTHON_SLOT_NUMBER );
        } else {
            Serial.println( "Warning: Failed to load Python slot: " + errorMsg );
        }
    } else {
        // GLOBAL MODE: Just store a backup for potential restore
        // but continue working with the current slot
        storeStateBackup( );
    }
}

extern "C" void jl_soft_reboot( void ) {
    // Perform a complete VM reinit to ensure clean state
    // This is heavier than a soft reset but prevents memory corruption

    // CRITICAL: Flush and clear all stream buffers before touching VM
    if ( global_mp_stream ) {
        global_mp_stream->flush( );
    }

#ifdef USE_TINYUSB
    if ( USBSer2 ) {
        USBSer2.flush( );
        // Clear any stale data in CDC RX buffer
        while ( USBSer2.available( ) ) {
            USBSer2.read( );
        }
    }
    // Service USB to ensure buffers are fully drained
    for ( int i = 0; i < 10; i++ ) {
        yield( ); // mutex-guarded pump + CDC flush
        delay( 1 );
    }
#endif

    // Save current stream (don't touch interrupt char - it's managed elsewhere)
    Stream* saved_stream = global_mp_stream;
    void* saved_stream_ptr = global_mp_stream_ptr;
    Stream* saved_interrupt_stream = mp_interrupt_check_stream;
    bool was_in_raw_repl = MpRemoteService::getInstance( ).isInRawRepl( );

    // CRITICAL: Close all open file handles before deinitializing VM
    // This ensures files are properly flushed and closed before Python objects are freed
    jl_close_all_jfs_files( );

    // Deinitialize VM completely - this frees all Python objects
    mp_embed_deinit( );

    // Get stack pointer for reinit. NOTE: mp_embed_init persists the
    // shallowest stack_top it has ever seen and ignores deeper values like
    // this one — passing a deep-call-site address used to make later GC
    // stack scans (stack_top - &regs) underflow and scan ~4GB.
    char stack_dummy;
    char* stack_top = &stack_dummy;

    // Reinitialize VM with clean heap - declared in Python_Proper.h
    mp_embed_init( mp_heap, mp_heap_size, stack_top );

    // Restore streams (pointers are still valid, just VM state was reset)
    global_mp_stream = saved_stream;
    global_mp_stream_ptr = saved_stream_ptr;
    mp_interrupt_check_stream = saved_interrupt_stream;

    // Re-mount filesystem
    jl_vfs_mount_root( );

    // Set up filesystem paths again
    setupFilesystemAndPaths( );

    // Restore keyboard interrupt setting based on REPL mode
    // Raw REPL (mpremote/ViperIDE on USBSer2) uses Ctrl+C
    // Friendly REPL (built-in on Serial/Jerial) uses Ctrl+Q
    if ( was_in_raw_repl ) {
        // Raw REPL uses Ctrl+C (MP_INTERRUPT_CHAR_USBSER2)
        mp_embed_exec_str( "import micropython; micropython.kbd_intr(3)" );
    } else {
        // Friendly REPL uses Ctrl+Q (MP_INTERRUPT_CHAR_SERIAL)
        mp_embed_exec_str( "import micropython; micropython.kbd_intr(17)" );
    }

    // Restore Jumperless functions
    addJumperlessPythonFunctions( );
    addMicroPythonModules( );

    // Import jumperless convenience functions
    mp_embed_exec_str( "try:\n    from jumperless import *\nexcept: pass\n" );

    // Re-define walk() in the fresh VM (addJumperlessPythonFunctions early-returns
    // after first load, so its walk()/check_interrupt defs are lost on reinit).
    // Mirrors the definition baked at init so :fs keeps working after a soft reset.
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
        "globals()['walk'] = walk\n" );

#ifdef USE_TINYUSB
    // Final USB service to ensure clean state
    for ( int i = 0; i < 5; i++ ) {
        yield( ); // mutex-guarded pump + CDC flush
        delay( 1 );
    }
#endif
}

void jl_exit_micropython_restore_entry_state( void ) {
    // CRITICAL: Disarm all Pin.irq() interrupts BEFORE releasing pin ownership.
    // Once gpioPythonOwned[] clears, Core 2's readGPIO()/gpioReadWithFloating()
    // resumes pull/input-enable twiddling on these pins, which manufactures
    // edges. A still-armed hard=True handler would then run Python bytecode in
    // ISR context at arbitrary points in Arduino-side code (TinyUSB reentrancy,
    // readingGPIO lock spins) — the delayed post-session crash signature.
    machine_pin_irq_deinit( );

    // Release all GPIO pins claimed by MicroPython
    jl_gpio_release_all_pins( );

    // Pause Core 2 during state modifications to prevent race conditions
    bool was_paused = pauseCore2;
    pauseCore2 = true;
    delayMicroseconds( 50 );

    if ( connectionContext == PYTHON_CONTEXT_ISOLATED ) {
        // ISOLATED MODE: Save Python slot and restore entry state
        SlotManager& mgr = SlotManager::getInstance( );
        String errorMsg;

        // Save current Python state to Python slot
        mgr.saveSlot( PYTHON_SLOT_NUMBER, errorMsg );

        // Restore the entry state (discards Python changes from global state)
        restoreAndSaveStateBackup( );

        // Restore the original slot number
        if ( pythonEntrySlot >= 0 && pythonEntrySlot < NUM_SLOTS ) {
            netSlot = pythonEntrySlot;
            mgr.setActiveSlot( pythonEntrySlot );
        }
    } else {
        // GLOBAL MODE: Changes persist, just clear the backup
        clearStateBackup( );
    }

    // Unpause Core 2 BEFORE refreshConnections since it internally calls waitCore2
    pauseCore2 = was_paused;

    // Refresh connections to match the current state
    refreshConnections( -1, 1, 0 );
}

void jl_restore_micropython_entry_state( void ) {
    // Use the new state-based backup system to restore entry state
    restoreAndSaveStateBackup( );

    // Refresh connections to match the restored state
    refreshLocalConnections( );
}

int jl_has_unsaved_changes( void ) {
    // Use the new state-based backup system to check for changes
    return hasStateChanges( ) ? 1 : 0;
}

void jl_toggle_connection_context( void ) {
    // Toggle between global and isolated modes
    if ( connectionContext == PYTHON_CONTEXT_GLOBAL ) {
        connectionContext = PYTHON_CONTEXT_ISOLATED;
    } else {
        connectionContext = PYTHON_CONTEXT_GLOBAL;
    }
}

const char* jl_get_connection_context_name( void ) {
    return ( connectionContext == PYTHON_CONTEXT_GLOBAL ) ? "global" : "python";
}

// Helper function to convert chip identifier to chip number
int parseChipIdentifier( const char* chip_str ) {
    if ( strlen( chip_str ) == 1 ) {
        char c = chip_str[ 0 ];
        if ( c >= 'A' && c <= 'L' ) {
            return c - 'A'; // A=0, B=1, ..., L=11
        } else if ( c >= 'a' && c <= 'l' ) {
            return c - 'a'; // a=0, b=1, ..., l=11
        }
    }
    // If not a letter, try to parse as number
    int chip_num = atoi( chip_str );
    if ( chip_num >= 0 && chip_num <= 11 ) {
        return chip_num;
    }
    return -1; // Invalid chip identifier
}

void jl_send_raw( int chip, int x, int y, int setOrClear ) {
    // Validate chip number (0-11)
    if ( chip < 0 || chip > 11 ) {
        Serial.print( "jl_send_raw: Invalid chip number: " );
        Serial.println( chip );
        return; // Invalid chip number
    }

    // Validate x,y coordinates (assuming 0-15 range based on typical crossbar chips)
    if ( x < 0 || x > 15 || y < 0 || y > 15 ) {
        Serial.print( "jl_send_raw: Invalid coordinates: " );
        Serial.print( x );
        Serial.print( "," );
        Serial.println( y );
        return; // Invalid coordinates
    }

    // Update lastChipXY bitfield and send to hardware
    if (setOrClear) {
        lastChipXY[chip].connected[y] |= (1 << x);   // Set bit
    } else {
        lastChipXY[chip].connected[y] &= ~(1 << x);  // Clear bit
    }
    sendXYraw(chip, x, y, setOrClear);
}

void jl_send_raw_str( const char* chip_str, int x, int y, int setOrClear ) {
    int chip = parseChipIdentifier( chip_str );
    if ( chip >= 0 ) {
        // Serial.print("jl_send_raw_str: chip = ");
        // Serial.println(chip);
        // Serial.print("jl_send_raw_str: x = ");
        // Serial.println(x);
        // Serial.print("jl_send_raw_str: y = ");
        // Serial.println(y);
        // Serial.print("jl_send_raw_str: setOrClear = ");
        jl_send_raw( chip, x, y, setOrClear );
    }
}

int jl_switch_slot( int slot ) {
    // Validate slot number
    if ( slot < 0 || slot >= NUM_SLOTS ) {
        return -1; // Invalid slot number
    }

    // Save current slot if different
    if ( netSlot != slot ) {
        // Pause Core 2 briefly while changing slot number
        bool was_paused = pauseCore2;
        pauseCore2 = true;
        delayMicroseconds( 50 );

        int old_slot = netSlot;
        netSlot = slot;

        // Unpause Core 2 BEFORE refreshConnections since it internally calls waitCore2
        pauseCore2 = was_paused;

        // Refresh connections for the new slot
        refreshConnections( -1 );

        return old_slot; // Return the previous slot number
    }

    return slot; // Already in this slot
}

// OLED Functions
static int default_oled_text_size = 2; // Default to size 2

int jl_oled_print( const char* text, int size ) {
    // If size is -1, use default
    if ( size == -1 ) {
        size = default_oled_text_size;
    }
    
    if ( oled.isConnected( ) ) {
        if ( size == 0 ) {
            // Small multiline scrolling text
            oled.showMultiLineSmallText( text, false, true );
        } else {
            // Regular centered text
            oled.clearPrintShow( text, size, true, true, true );
        }
        return 1;
    } else {
        return 0;
    }
}

int jl_oled_clear( int show ) {
    if ( oled.isConnected( ) ) {
        oled.clear( 1000 );
        if ( show ) {
            oled.show( 1000 );
        }
        return 1;
    } else {
        return 0;
    }
}

int jl_oled_show( void ) {
    if ( oled.isConnected( ) ) {
        oled.show( 1000 );
        return 1;
    } else {
        return 0;
    }
}

int jl_oled_connect( void ) {
    return oled.init( );
}

int jl_oled_disconnect( void ) {
    oled.disconnect( );
    return 1;
}

// Text Size Control
int jl_oled_set_text_size( int size ) {
    if ( size < 0 || size > 2 ) return 0;
    default_oled_text_size = size;
    return 1;
}

int jl_oled_get_text_size( void ) {
    return default_oled_text_size;
}

// Put the script-settable display preferences back to their defaults at the
// end of a Python session. Both live in this TU (oled_copy_print_enabled is
// defined in Python_Proper.cpp but only ever set from here), and neither had
// any clearer before - a script that enabled print-to-OLED kept it enabled
// until reboot.
void jl_reset_python_display_prefs( void ) {
    extern bool oled_copy_print_enabled;
    default_oled_text_size = 2;
    oled_copy_print_enabled = false;
}

// Print Redirection - declared in Python_Proper.cpp
extern bool oled_copy_print_enabled;

int jl_oled_copy_print( int enable ) {
    oled_copy_print_enabled = ( enable != 0 );
    if ( enable && oled.isConnected( ) ) {
        // Clear OLEDOut buffer and prepare for small text scrolling
        // This properly resets the showMultiLineSmallText static buffer
        OLEDOut.clear( );
    }
    return 1;
}

// Font System
const char* jl_oled_get_fonts( int* count ) {
    // Returns comma-separated font family names
    static const char* fontNames = "Eurostile,Jokerman,Comic Sans,Courier New,"
                                   "New Science,New Science Ext,Andale Mono,"
                                   "Free Mono,Iosevka Regular,Berkeley Mono,Pragmatism";
    *count = 11;
    return fontNames;
}

int jl_oled_set_font( const char* fontName ) {
    // if ( !oled.isConnected( ) ) return 0;
    if ( fontName == NULL ) return 0;

    String requestedFont( fontName );
    requestedFont.trim( );
    if ( requestedFont.length( ) == 0 ) return 0;

    int fontIndex = oled.setFont( requestedFont, 0 );
    // Return 1 for success (fontIndex >= 0), 0 for failure (fontIndex == -1)
    return ( fontIndex >= 0 ) ? 1 : 0;
}

const char* jl_oled_get_current_font( void ) {
    // Get current font family name
    if ( !oled.isConnected( ) ) return "";
    String fontName = oled.getFontName( oled.currentFontFamily );
    static char fontNameBuffer[ 64 ];
    strncpy( fontNameBuffer, fontName.c_str( ), sizeof( fontNameBuffer ) - 1 );
    fontNameBuffer[ sizeof( fontNameBuffer ) - 1 ] = '\0';
    return fontNameBuffer;
}

// Bitmap Functions
int jl_oled_load_bitmap( const char* filepath ) {
    return loadBitmapFromFile( filepath ) ? 1 : 0;
}

int jl_oled_display_bitmap( int x, int y, int width, int height, 
                            const uint8_t* data, size_t data_len ) {
    if ( !oled.isConnected( ) ) return 0;
    
    if ( data != NULL && data_len > 0 ) {
        // Display provided bitmap data
        oled.displayBitmap( x, y, data, width, height );
    } else if ( customBitmapLoaded ) {
        // Display previously loaded bitmap
        oled.displayBitmap( x, y, customBitmapBuffer, 
                           customBitmapWidth, customBitmapHeight );
    } else {
        return 0; // No bitmap available
    }
    
    oled.show( );
    return 1;
}

int jl_oled_show_bitmap_file( const char* filepath, int x, int y ) {
    if ( jl_oled_load_bitmap( filepath ) ) {
        return jl_oled_display_bitmap( x, y, 0, 0, NULL, 0 );
    }
    return 0;
}

// Framebuffer Access
const uint8_t* jl_oled_get_framebuffer( int* width, int* height, int* buffer_size ) {
    if ( !oled.isConnected( ) ) return NULL;
    
    Adafruit_SSD1306& display = getDisplay( );
    *width = display.width( );
    *height = display.height( );
    *buffer_size = ( *width * *height ) / 8;
    
    return display.getBuffer( );
}

int jl_oled_set_framebuffer( const uint8_t* data, size_t len ) {
    if ( !oled.isConnected( ) ) return 0;
    
    Adafruit_SSD1306& display = getDisplay( );
    int expected_size = ( display.width( ) * display.height( ) ) / 8;
    
    if ( (int)len != expected_size ) return 0;
    
    uint8_t* buffer = display.getBuffer( );
    memcpy( buffer, data, len );
    display.display( );
    
    return 1;
}

void jl_oled_get_framebuffer_size( int* width, int* height, int* buffer_bytes ) {
    if ( oled.isConnected( ) ) {
        Adafruit_SSD1306& display = getDisplay( );
        *width = display.width( );
        *height = display.height( );
        *buffer_bytes = ( *width * *height ) / 8;
    } else {
        *width = 0;
        *height = 0;
        *buffer_bytes = 0;
    }
}

int jl_oled_set_pixel( int x, int y, int color ) {
    if ( !oled.isConnected( ) ) return 0;
    getDisplay( ).drawPixel( x, y, color );
    return 1;
}

int jl_oled_get_pixel( int x, int y ) {
    if ( !oled.isConnected( ) ) return -1;
    return getDisplay( ).getPixel( x, y );
}

// =============================================================================
// OLED GUI (retained screens) - flat handle-based bridge for MicroPython
// =============================================================================
// Screen handles are 1-based ints. Element handles encode the owning screen
// and the element index as (screenHandle << 8) | elementIndex, so a single
// integer fully identifies an element. The Python wrapper (oledgui.py) builds
// Screen/Text/Shape classes on top of these.
extern "C" {

static inline int jl_oled_pack_elem( int screen, int idx ) {
    if ( screen < 1 || idx < 0 ) return -1;
    return ( screen << 8 ) | ( idx & 0xFF );
}
static inline int jl_oled_elem_screen( int elem ) { return ( elem >> 8 ) & 0xFF; }
static inline int jl_oled_elem_index( int elem )  { return elem & 0xFF; }

int jl_oled_screen_new( void ) {
    return oledGuiCreateScreen( );
}

void jl_oled_screen_free( int screen ) {
    oledGuiDestroyScreen( screen );
}

void jl_oled_screen_clear( int screen ) {
    OledScreen* s = oledGuiGetScreen( screen );
    if ( s ) s->clearElements( );
}

int jl_oled_screen_show( int screen, int persist ) {
    OledScreen* s = oledGuiGetScreen( screen );
    if ( !s ) return 0;
    // persist != 0 registers this as the idle screen: it takes the place of the
    // boot logo (drawn by showJogo32h when the UI returns to idle) and survives
    // the script/REPL that created it. persist == 0 is a foreground show that is
    // torn down when the script ends.
    OledGui::getInstance( ).activate( s, persist != 0 );
    // Render immediately on this (core0/Python) thread - the background
    // render service is NORMAL priority and does NOT run while a Python
    // script blocks in time.sleep (that path only runs CRITICAL services),
    // so a script that shows a screen and then loops would otherwise never
    // see it drawn. This uses the same safe flush path as oled_print.
    OledGui::getInstance( ).renderNow( true );
    return 1;
}

void jl_oled_screen_hide( void ) {
    // Stop rendering AND blank the panel so the screen is visually removed.
    // Also forget any persistent idle registration - an explicit hide() means
    // "take it down", so it must not reappear at the next showJogo32h().
    OledGui::getInstance( ).clearIdle( );
    OledGui::getInstance( ).hideAndClear( );
}

void jl_oled_screen_reset( void ) {
    // Free EVERY screen handle and blank the panel. Call at the start of a
    // script to cleanly discard any screens a previous run left behind
    // (otherwise re-running leaks handles until the pool is exhausted).
    oledGuiShutdownAll( );
}

int jl_oled_add_text( int screen, const char* text, int x, int y,
                      const char* font, int size, int halign, int valign, int z ) {
    OledScreen* s = oledGuiGetScreen( screen );
    if ( !s ) return -1;
    int idx = s->addText( text, (int16_t)x, (int16_t)y, font, (uint8_t)( size > 0 ? size : 8 ) );
    if ( idx < 0 ) return -1;
    if ( z != 0 ) s->setZ( idx, (int8_t)z );
    // halign/valign < 0 means "free placement" (use x/y); >=0 enables anchoring.
    if ( halign >= 0 && valign >= 0 ) {
        s->setAnchor( idx, (OledHAlign)( halign & 3 ), (OledVAlign)( valign & 3 ) );
    }
    return jl_oled_pack_elem( screen, idx );
}

int jl_oled_add_shape( int screen, int kind, int x, int y, int w, int h,
                       int filled, int z ) {
    OledScreen* s = oledGuiGetScreen( screen );
    if ( !s ) return -1;
    int idx = s->addShape( (OledShapeKind)( kind & 3 ), (int16_t)x, (int16_t)y,
                           (int16_t)w, (int16_t)h, filled != 0 );
    if ( idx < 0 ) return -1;
    if ( z != 0 ) s->setZ( idx, (int8_t)z );
    return jl_oled_pack_elem( screen, idx );
}

int jl_oled_elem_set_str( int elem, const char* prop, const char* value ) {
    OledScreen* s = oledGuiGetScreen( jl_oled_elem_screen( elem ) );
    if ( !s ) return 0;
    bool ok = s->setStrProp( jl_oled_elem_index( elem ), prop, value );
    if ( ok && OledGui::getInstance( ).active( ) == s ) {
        OledGui::getInstance( ).renderNow( false );
    }
    return ok ? 1 : 0;
}

int jl_oled_elem_set_int( int elem, const char* prop, int value ) {
    OledScreen* s = oledGuiGetScreen( jl_oled_elem_screen( elem ) );
    if ( !s ) return 0;
    bool ok = s->setIntProp( jl_oled_elem_index( elem ), prop, value );
    if ( ok && OledGui::getInstance( ).active( ) == s ) {
        OledGui::getInstance( ).renderNow( false );
    }
    return ok ? 1 : 0;
}

int jl_oled_set_var( const char* name, const char* value ) {
    OledVars::setStr( name, value );
    // Re-render so any {name} token on the active screen updates now.
    OledGui::getInstance( ).renderNow( false );
    return 1;
}

int jl_oled_set_var_num( const char* name, float value ) {
    OledVars::setNum( name, value );
    OledGui::getInstance( ).renderNow( false );
    return 1;
}

int jl_oled_screen_save( int screen, const char* name ) {
    OledScreen* s = oledGuiGetScreen( screen );
    if ( !s ) return 0;
    return oledGuiSaveScreen( s, name ) ? 1 : 0;
}

int jl_oled_screen_load( const char* name ) {
    return oledGuiLoadScreen( name );
}

} // extern "C"

// Arduino Functions
void jl_arduino_reset( void ) {
    resetArduino( );
}

// Status Functions
int jl_nodes_print_bridges( void ) {
    printPathsCompact( );
    return 1;
}

int jl_nodes_print_paths( void ) {
    printPathsCompact( );
    return 1;
}

int jl_nodes_print_crossbars( void ) {
    printChipStateArray( );
    return 1;
}

int jl_nodes_print_nets( void ) {
    listNets( 0 );
    return 1;
}

int jl_nodes_print_chip_status( void ) {
    printChipStatus( );
    return 1;
}

int jl_run_app( char* appName ) {
    runApp( -1, appName );
    return 1;
}

// Probe Functions
void jl_probe_tap( int node ) {
    // Simulated tap: hold the cached probe reading on `node` for ~1.2s - long
    // enough for MeasureMode's stability window and the probe highlighter to
    // latch, the same way a real held tip does. The hold survives the end of
    // the raw-REPL exec (it expires on wall time), so a script can tap and
    // exit and the reading display reacts in the normal service loop.
    // node <= 0 cancels an active hold. See Probing::simulateProbeTap().
    Probing::getInstance( ).simulateProbeTap( node, 1200 );
}

int jl_probe_read_blocking( void ) {
    int pad = -1;
    static int call_count = 0;
    call_count++;

    while ( pad == -1 ) {
        mp_hal_check_interrupt( );

        // Check if interrupt was requested and return special value
        if ( mp_interrupt_requested ) {
            mp_interrupt_requested = false; // Clear the flag
            Serial.print( "DEBUG: Interrupt detected in jl_probe_read_blocking, call #" );
            Serial.println( call_count );
            return -999; // Special return value indicating interrupt
        }

        pad = justReadProbe( false, 1 );
       // delay( 1 ); // Small delay to prevent busy waiting
    }
    return pad;
}

int jl_probe_read_nonblocking( void ) {
    return justReadProbe( true, 1 );
}

// highlightNets function moved to Highlighting.cpp

// Double click detector state machine

// C wrapper functions for MicroPython module
// Note: probeButton is declared in Probing.h

extern "C" int jl_probe_button_nonblocking( int consume ) {
    // Get button state from the ProbeButton service
    // consume: if false (default), returns current held state; if true, consumes press event
    // Returns: 0=NONE, 1=CONNECT(front), 2=REMOVE(rear)
    int button_state;

    if ( consume ) {
        // One-shot mode: consume the button press event
        button_state = probeButton.getButtonPress( true );
    } else {
        // Continuous mode: read current button state (persists while held)
        button_state = probeButton.getButtonState( );
    }

    // Handle probe revision differences (button mapping)
    if ( jumperlessConfig.hardware.probe_revision > 3 ) {
        if ( button_state == 1 ) {
            button_state = 2;
        } else if ( button_state == 2 ) {
            button_state = 1;
        }
    }
    return button_state;
}

extern "C" int jl_probe_button_blocking( int consume ) {
    // Loop until any button is pressed
    // consume: if false (default), returns current held state; if true, consumes press event
    // Returns: 1=CONNECT(front), 2=REMOVE(rear) (never returns 0)
    int button_state = 0;

    if ( consume ) {
        // One-shot mode: wait for a button press EVENT and consume it
        while ( button_state == 0 ) {
            mp_hal_check_interrupt( );

            // Check if interrupt was requested and return special value
            if ( mp_interrupt_requested ) {
                mp_interrupt_requested = false; // Clear the flag
                return -999;                    // Special return value indicating interrupt
            }

            button_state = probeButton.getButtonPress( true ); // Consume on read
            // delay( 1 ); // Small delay to prevent busy-waiting
        }
    } else {
        // Continuous mode: wait until button STATE becomes non-zero (held)
        while ( button_state == 0 ) {
            mp_hal_check_interrupt( );

            // Check if interrupt was requested and return special value
            if ( mp_interrupt_requested ) {
                mp_interrupt_requested = false; // Clear the flag
                return -999;                    // Special return value indicating interrupt
            }

            button_state = probeButton.getButtonState( ); // Read current state, don't consume
            // delay( 1 ); // Small delay to prevent busy-waiting
        }
    }

    // Handle probe revision differences (button mapping)
    if ( jumperlessConfig.hardware.probe_revision > 3 ) {
        if ( button_state == 1 ) {
            button_state = 2;
        } else if ( button_state == 2 ) {
            button_state = 1;
        }
    }

    return button_state;
}

// Clickwheel Functions
void jl_clickwheel_up( int clicks ) {
    encoderOverride = 10;
    lastDirectionState = NONE;
    encoderDirectionState = UP;
}

void jl_clickwheel_down( int clicks ) {
    encoderOverride = 10;
    lastDirectionState = NONE;
    encoderDirectionState = DOWN;
}

void jl_clickwheel_press( void ) {
    encoderOverride = 10;
    lastButtonEncoderState = PRESSED;
    encoderButtonState = RELEASED;
}

// PWM Functions
extern "C" int jl_pwm_setup( int gpio_pin, float frequency, float duty_cycle ) {
    return setupPWM( gpio_pin, frequency, duty_cycle );
}

extern "C" int jl_pwm_set_duty_cycle( int gpio_pin, float duty_cycle ) {
    return setPWMDutyCycle( gpio_pin, duty_cycle );
}

extern "C" int jl_pwm_set_frequency( int gpio_pin, float frequency ) {
    return setPWMFrequency( gpio_pin, frequency );
}

extern "C" int jl_pwm_stop( int gpio_pin ) {
    return stopPWM( gpio_pin );
}

// Service Management Functions
extern "C" int jl_force_service( const char* service_name ) {
    if ( !service_name ) {
        return 0;
    }
    return jOS.forceServiceByName( service_name ) ? 1 : 0;
}

extern "C" int jl_force_service_by_index( int index ) {
    if ( index < 0 ) {
        return 0;
    }
    return jOS.forceServiceByIndex( static_cast<uint8_t>( index ) ) ? 1 : 0;
}

extern "C" int jl_get_service_index( const char* service_name ) {
    if ( !service_name ) {
        return -1;
    }
    return jOS.getServiceIndex( service_name );
}

// Probe Switch Functions
extern "C" int jl_get_switch_position( void ) {
    // int connected = bufferPowerConnected;
    // if (connected == false) {
    //     routableBufferPower( 1, 0, 1 );
    //     // delay( 10 );
    // }   
    int result = Probing::getInstance( ).switchPosition;
    // if (connected == false) {
    //         routableBufferPower( 0, 0, 1 );
    //     }
    return result;
}

// Set when a script writes switchPosition, so the raw-REPL exit hook knows
// whether its snapshot is worth restoring. Without this the hook blindly
// wrote back a value sampled at script start, undoing a genuine mid-script
// physical flip - and checkSwitchPosition() legitimately holds its last value
// when it can't sense, so the bad write would not self-heal.
volatile bool switchPositionScriptDirty = false;

extern "C" void jl_set_switch_position( int position ) {
    // Validate: -1 = unknown, 0 = measure, 1 = select
    if ( position >= -1 && position <= 1 ) {
        Probing::getInstance( ).switchPosition = position;
        switchPositionScriptDirty = true;
    }
}

extern "C" int jl_check_switch_position( void ) {
    // int connected = bufferPowerConnected;
    // if (connected == false) {
    //     routableBufferPower( 1, 0, 0 );
    //     delay( 10 );
    // }
    int result = Probing::getInstance( ).checkSwitchPosition( );
    // if (connected == false) {
    //     routableBufferPower( 0, 0, 0 );
       
    // }
    return result;
}

// Probe Autoconnect
// enable: 1 = on, 0 = off (temporary, until reboot), -1 = query current state
extern "C" int jl_probe_autoconnect( int enable ) {
    if ( enable == -1 ) {
        return jumperlessConfig.dacs.auto_connect_probe;
    }
    if ( enable ) {
        jumperlessConfig.dacs.auto_connect_probe = 1;
        routableBufferPower( 1, 0, 1 );
    } else {
        jumperlessConfig.dacs.auto_connect_probe = 0;
        routableBufferPower( 0, 0, 1 );
    }
    return jumperlessConfig.dacs.auto_connect_probe;
}

// Clickwheel (Rotary Encoder) Functions
extern "C" long jl_clickwheel_get_position( void ) {
    return encoderPosition;
}

extern "C" void jl_clickwheel_reset_position( void ) {
    resetEncoderPosition = true;
    encoderPosition = 0;
    encoderPositionOffset = 0;
}

extern "C" int jl_clickwheel_get_direction( int consume ) {
    // Returns: 0 = NONE, 1 = UP, 2 = DOWN
    int direction = static_cast<int>( encoderDirectionState );
    
    if ( consume && direction != 0 ) {
        // Mark as consumed and clear the direction
        encoderDirectionConsumed = true;
        encoderDirectionState = NONE;
    }
    
    return direction;
}

extern "C" int jl_clickwheel_get_button( void ) {
    // Returns: 0 = IDLE, 1 = PRESSED, 2 = HELD, 3 = RELEASED, 4 = DOUBLECLICKED
    return static_cast<int>( encoderButtonState );
}

extern "C" bool jl_clickwheel_is_initialized( void ) {
    return isRotaryEncoderInitialized( );
}

// Filesystem Functions - all require mutex for thread safety
int jl_fs_exists( const char* path ) {
    if ( !path )
        return 0;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem
    int result = FatFS.exists( path ) ? 1 : 0;
    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem

    return result;
}

char* jl_fs_listdir( const char* path ) {
    if ( !path )
        return nullptr;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Distinguish "missing dir" (nullptr -> Python raises ENOENT) from
    // "empty dir" (empty buffer -> []). Root always exists.
    // ponytail: a path that names a plain FILE still lists as empty instead of
    // raising ENOTDIR; upgrade path is a jl_fs_stat_isdir() check here.
    if ( !( path[ 0 ] == '/' && path[ 1 ] == '\0' ) && !FatFS.exists( path ) ) {
        fs_mutex_release( );
        return nullptr;
    }

    // Use static buffer to avoid memory management issues
#if defined(OG_JUMPERLESS)
    static char listBuffer[ 768 ]; // RP2040: scarce SRAM
#else
    static char listBuffer[ 2048 ];
#endif
    listBuffer[ 0 ] = '\0';

    Dir dir = FatFS.openDir( path );

    bool first = true;
    while ( dir.next( ) ) {
        // Store filename once to avoid multiple .c_str() calls
        String fileNameStr = dir.fileName( );
        const char* fileName = fileNameStr.c_str( );

        // Skip hidden files/directories that start with '.'
        if ( fileName[ 0 ] == '.' ) {
            continue;
        }

        // Prevent buffer overflow: check BEFORE appending, against the actual
        // buffer size (768 on OG, 2048 on V5). The old post-append ">1900" check
        // could never fire before the 768-byte OG buffer was already smashed.
        // Entries past this point are silently omitted.
        if ( strlen( listBuffer ) + fileNameStr.length( ) + 2 >= sizeof( listBuffer ) ) {
            break;
        }

        if ( !first ) {
            // '\n' separator: illegal in FAT filenames, so it can never collide
            // with a name. The old ',' split "a,b.txt" into two phantom entries.
            strcat( listBuffer, "\n" );
        }
        strcat( listBuffer, fileName );
        if ( dir.isDirectory( ) ) {
            strcat( listBuffer, "/" );
        }
        first = false;
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return listBuffer;
}

// ponytail: this string-returning API inherently truncates - at the static
// buffer size (4KB / 1KB on OG) and, on the Python side, at the first NUL byte
// (callers strlen it). Not fixable without changing the char* contract, which
// is also declared in lib/micropython/port/mphalport.c (out of reach here).
// Binary-safe, unbounded reads go through jfs.open()+read() instead.
char* jl_fs_read_file( const char* path ) {
    if ( !path )
        return nullptr;

    // Use static buffer for file contents
#if defined(OG_JUMPERLESS)
    static char fileBuffer[ 1024 ]; // RP2040: scarce SRAM, smaller max read via this API
#else
    static char fileBuffer[ 4096 ];
#endif
    size_t bytesRead = 0;

    if ( !safeFileReadAll( path, fileBuffer, sizeof( fileBuffer ), &bytesRead, 2000 ) ) {
        return nullptr;
    }

    // Ensure null termination so text consumers (e.g. VFS readers) don't overrun
    if ( bytesRead < sizeof( fileBuffer ) ) {
        fileBuffer[ bytesRead ] = '\0';
    } else {
        fileBuffer[ sizeof( fileBuffer ) - 1 ] = '\0';
    }

    return fileBuffer;
}

int jl_fs_write_file( const char* path, const char* content, int len ) {
    if ( !path || !content || len < 0 )
        return 0;

    // len is the true byte count from the Python string/bytes object, so data
    // with embedded NULs is written intact (the old strlen-based path stopped
    // at the first NUL). len == 0 writes/truncates to an empty file.
    return safeFileWriteAll( path, content, (size_t)len, 2000 ) ? 1 : 0;
}

char* jl_fs_get_current_dir( void ) {
    static char currentDir[] = "/";
    return currentDir;
}

// Get file size (returns -1 if file doesn't exist)
int jl_fs_stat_size( const char* path ) {
    if ( !path )
        return -1;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    int result = -1;
    if ( FatFS.exists( path ) ) {
        // Try to open as a file; directories can't be opened as files (the
        // same probe jl_fs_stat_isdir uses). The old openDir()+next() check
        // actually inspected the dir's FIRST CHILD, so empty dirs and dirs
        // whose first entry is a file misreported as -1.
        File file = FatFS.open( path, "r" );
        if ( file ) {
            result = file.size( );
            file.close( );
        } else {
            result = 0; // exists but not openable as a file -> directory
        }
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return result;
}

// Check if path is a directory (returns 1 if directory, 0 otherwise)
int jl_fs_stat_isdir( const char* path ) {
    if ( !path )
        return 0;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    int result = 0;

    // Root is always a directory
    if ( path[ 0 ] == '/' && path[ 1 ] == '\0' ) {
        result = 1;
    } else if ( FatFS.exists( path ) ) {
        // Try to open as directory
        Dir dir = FatFS.openDir( path );
        // If we can iterate (has content) or it's a valid empty dir path, it's a directory
        // FatFS doesn't have a direct isDirectory for paths, so we check differently
        // First, check if it's a file by trying to open it
        File file = FatFS.open( path, "r" );
        if ( file ) {
            // Could open as file - check if it has directory-like behavior
            // In FatFS, directories can't be opened as regular files
            result = 0;
            file.close( );
        } else {
            // Couldn't open as file - might be a directory
            // Check by trying to list its contents
            Dir testDir = FatFS.openDir( path );
            result = 1; // Assume directory if we couldn't open as file but exists
        }
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return result;
}

// ============================================================
// JFS File Handle Tracking
// Keeps track of all open JFS file handles so they can be
// closed when exiting MicroPython (prevents file conflicts)
//
// The handle given to Python is NOT the raw File* - the heap can reuse a freed
// File's address for the next open, so pointer identity alone cannot detect a
// stale handle (ABA). Instead each slot carries a generation counter and the
// opaque handle encodes (generation << 4) | 0x8 | slot. A stale handle fails
// the generation check even after its slot is reused by a newer open.
// ============================================================
#define MAX_JFS_OPEN_FILES 8
static_assert( MAX_JFS_OPEN_FILES <= 8, "slot index must fit in the handle's 3 low bits" );
static void* jfs_open_files[ MAX_JFS_OPEN_FILES ] = { nullptr }; // File* per slot
// ponytail: generation is 28 bits (32-bit uintptr_t minus tag+slot); it wraps
// after ~268M opens of one slot, at which point a handle from exactly that many
// opens ago could false-positive. Upgrade path: widen the handle to 64 bits.
static uint32_t jfs_open_gens[ MAX_JFS_OPEN_FILES ] = { 0 };
int debug_fs = 0;

// Register a freshly opened File* and return the opaque handle for Python,
// or nullptr if the table is full.
static void* jfs_track_file( File* file ) {
    for ( int i = 0; i < MAX_JFS_OPEN_FILES; i++ ) {
        if ( jfs_open_files[ i ] == nullptr ) {
            jfs_open_files[ i ] = file;
            jfs_open_gens[ i ]++; // new generation: invalidates handles from prior occupants
            if ( debug_fs ) {
                Serial.println( "DEBUG: jfs_track_file: File is tracked" );
                Serial.flush( );
            }
            return (void*)( ( (uintptr_t)( jfs_open_gens[ i ] & 0x0FFFFFFFu ) << 4 ) | 0x8u | (uintptr_t)i );
        }
    }
    // No space. The open must FAIL in this case: an untracked handle could never
    // be closed (jl_fs_close_file treats "not tracked" as "already closed") and
    // would leak its heap File + FatFS FIL for the rest of the session.
    if ( debug_fs ) {
        Serial.println( "DEBUG: jfs_track_file: No space - rejecting open (max concurrent JFS files reached)" );
        Serial.flush( );
    }
    return nullptr;
}

// Decode + validate an opaque handle. Returns the live File*, or nullptr if the
// handle is stale: its slot was closed (by close/jl_close_all_jfs_files) or has
// since been reused by a newer open. This is the use-after-free guard for
// Python file objects that outlive their underlying File.
static File* jfs_resolve( void* handle ) {
    uintptr_t h = (uintptr_t)handle;
    if ( !( h & 0x8u ) )
        return nullptr; // not a handle we issued (includes NULL)
    int slot = (int)( h & 0x7u );
    if ( ( jfs_open_gens[ slot ] & 0x0FFFFFFFu ) != (uint32_t)( ( h >> 4 ) & 0x0FFFFFFFu ) ) {
        if ( debug_fs ) {
            Serial.println( "DEBUG: jfs_resolve: Stale handle (generation mismatch)" );
            Serial.flush( );
        }
        return nullptr; // slot was closed and reused by a newer open
    }
    return (File*)jfs_open_files[ slot ]; // nullptr if slot freed since issue
}

static void jfs_untrack_file( void* handle ) {
    if ( jfs_resolve( handle ) == nullptr ) {
        if ( debug_fs ) {
            Serial.println( "DEBUG: jfs_untrack_file: File is not found in tracked files" );
            Serial.flush( );
        }
        return;
    }
    jfs_open_files[ (uintptr_t)handle & 0x7u ] = nullptr;
    if ( debug_fs ) {
        Serial.println( "DEBUG: jfs_untrack_file: File is untracked" );
        Serial.flush( );
    }
}

// Close all open JFS files - called when exiting MicroPython
// CRITICAL: Must flush before close to ensure all buffered data is written to disk
// This prevents data loss and potential filesystem corruption on script exit
// THREAD SAFETY: Acquires fs_mutex to prevent concurrent filesystem access
void jl_close_all_jfs_files( void ) {
    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_close_all_jfs_files: Closing all open JFS files" );
        Serial.flush( );
    }
    AsyncPassthrough::suspendUARTRxIRQ( );
    // CRITICAL: Pause Core2 during flash operations (flush writes to flash)
    bool was_paused = pauseCore2ForFlash( 100 );

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    for ( int i = 0; i < MAX_JFS_OPEN_FILES; i++ ) {
        if ( jfs_open_files[ i ] != nullptr ) {
            File* file = (File*)jfs_open_files[ i ];
            if ( *file ) {
                // CRITICAL: Flush before close to ensure buffered writes are committed
                // Without this, data written but not flushed could be lost on close
                file->flush( );
                file->close( );
            }
            delete file;
            jfs_open_files[ i ] = nullptr;
        }
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_close_all_jfs_files: All open JFS files closed" );
        Serial.flush( );
    }
}

// Why the last jl_fs_open_file returned nullptr: ENOENT (open failed) or
// EMFILE (handle table full). Lets the Python layer raise the truthful errno.
// ponytail: single global, not per-call - fine because only the MicroPython
// core opens JFS files and it is single-threaded.
static int jfs_open_errno = ENOENT;
int jl_fs_open_errno( void ) {
    return jfs_open_errno;
}

// File operations
// THREAD SAFETY: All file operations acquire fs_mutex to prevent concurrent access
void* jl_fs_open_file( const char* path, const char* mode ) {
    if ( !path || !mode ) {
        jfs_open_errno = EINVAL;
        return nullptr;
    }
    if ( debug_fs ) {
        Serial.print( "DEBUG: jl_fs_open_file: Opening " );
        Serial.print( path );
        Serial.println( "..." );
        Serial.flush( );
    }

    // Normalize mode: FatFS does not care about binary flag. Strip 'b' to allow
    // "rb", "wb", "ab", "r+b", etc. to work like their text equivalents.
    char sanitizedMode[ 8 ] = { 0 };
    size_t mlen = strlen( mode );
    size_t si = 0;
    for ( size_t i = 0; i < mlen && si < sizeof( sanitizedMode ) - 1; i++ ) {
        char c = mode[ i ];
        if ( c == 'b' || c == 't' ) {
            continue; // ignore binary/text specifiers for FatFS
        }
        sanitizedMode[ si++ ] = c;
    }
    if ( si == 0 ) {
        sanitizedMode[ 0 ] = 'r';
        sanitizedMode[ 1 ] = '\0';
    }

    // USB MSC guard (same chokepoint rule as safeFileOpen): while a host has
    // the disk mounted it caches the FAT, so a firmware write behind its back
    // corrupts the host's view. Read-only opens are still allowed.
    extern bool usbMountedByHost; // USBfs.h
    if ( usbMountedByHost && strpbrk( sanitizedMode, "wa+" ) != nullptr ) {
        jfs_open_errno = EROFS; // read-only while the USB host holds the disk
        return nullptr;
    }

    // NOTE: File open is primarily flash READS (directory lookup, FAT scan)
    // Flash reads don't disable XIP, so Core2 pause may not be needed here.
    // Only flash WRITES disable XIP and require Core2 synchronization.
    // Testing: removed Core2 pause from open - only keep mutex for thread safety

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    File* file = new File( FatFS.open( path, sanitizedMode ) );
    if ( debug_fs ) {
        Serial.print( "DEBUG: jl_fs_open_file: File opened: " );
        Serial.print( path );
        Serial.print( " in mode: " );
        Serial.println( sanitizedMode );
        Serial.print( "DEBUG: jl_fs_open_file: File handle: " );
        // name() can return NULL for a failed open - don't deref it
        Serial.println( *file ? (String)file->name( ) : String( "(not open)" ) );
        Serial.flush( );
    }
    if ( !*file ) {
        if ( debug_fs ) {
            Serial.println( "DEBUG: jl_fs_open_file: File not open" );
            Serial.flush( );
        }
        delete file;
        // The File wrapper swallows the FRESULT, but we can still be truthful:
        // a file that EXISTS yet refuses to open is FR_LOCKED (FF_FS_LOCK
        // sharing rule - e.g. reader while a writer holds it) or a full lock
        // table -> EBUSY. A missing file is the plain ENOENT.
        // ponytail: write-mode disk-full also lands on EBUSY's exists-check
        // (file may have been created); plumbing the real FRESULT through
        // the wrapper is the upgrade path.
        jfs_open_errno = FatFS.exists( path ) ? EBUSY : ENOENT;
        fs_mutex_release( ); // THREAD SAFETY: Unlock before returning
        return nullptr;
    }

    // Track the file handle for cleanup on exit. If the tracking table is full
    // the open fails - see jfs_track_file for why an untracked handle is unsafe.
    void* handle = jfs_track_file( file );
    if ( !handle ) {
        file->close( );
        delete file;
        jfs_open_errno = EMFILE;
        fs_mutex_release( ); // THREAD SAFETY: Unlock before returning
        return nullptr;
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem

    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_fs_open_file: File opened" );
        Serial.flush( );
    }
    return handle;
}

// Returns 1 when the handle is closed (or was already closed/stale), 0 when
// fs_mutex could not be acquired - the file then stays OPEN and TRACKED so the
// caller can retry or jl_close_all_jfs_files() reclaims it at script exit.
int jl_fs_close_file( void* file_handle ) {
    if ( !file_handle )
        return 1; // nothing to close
    // fs_mutex is per-core RECURSIVE (depth-counted, see externVars.h), so the
    // GC-finaliser self-deadlock that the old non-blocking try_acquire guarded
    // against cannot happen: re-acquiring on the owning core just nests. A
    // bounded blocking acquire only ever waits out the OTHER core; on timeout
    // we report failure instead of silently leaking the slot.
    if ( !fs_mutex_acquire_timeout_ms( 250 ) ) {
        if ( debug_fs ) {
            Serial.println( "DEBUG: jl_fs_close_file: fs_mutex timeout - file left open and tracked" );
            Serial.flush( );
        }
        return 0;
    }

    // Stale-handle guard: resolves to nullptr when the slot was already closed
    // (explicit close, jl_close_all_jfs_files, or reuse by a newer open) - in
    // that case there is nothing left to do.
    File* file = jfs_resolve( file_handle );
    if ( !file ) {
        if ( debug_fs ) {
            Serial.println( "DEBUG: jl_fs_close_file: File not tracked - already closed" );
            Serial.flush( );
        }
        fs_mutex_release( );
        return 1;
    }

    // Untrack the file handle
    jfs_untrack_file( file_handle );

    // Only close if the file is actually open (prevents double-close crashes)
    if ( *file ) {
        AsyncPassthrough::suspendUARTRxIRQ( );
        // CRITICAL: Pause Core2 during flash operations (flush writes to flash)
        bool was_paused = pauseCore2ForFlash( 100 );

        // CRITICAL: Flush before close to ensure all buffered data is written
        // This is essential for GC finalizers where files may have pending writes
        // Without flush, close might lose unflushed buffer data
        file->flush( );
        file->close( );

        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
    }
    delete file;

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return 1;
}

int jl_fs_read_bytes( void* file_handle, char* buffer, int size ) {
    if ( !file_handle || !buffer || size <= 0 )
        return -1;

    // CRITICAL: Pause Core2 during flash read operations to prevent corruption!
    // Core2 concurrent flash access can corrupt read data, causing null bytes
    bool was_paused = pauseCore2ForFlash( 100 );
    
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Stale-handle guard: jl_close_all_jfs_files() deletes the File* after each
    // script/REPL execution; a Python object surviving that still holds the old
    // handle. jfs_resolve() rejects it (generation check), so no use-after-free.
    File* file = jfs_resolve( file_handle );
    if ( !file || !*file ) {
        fs_mutex_release( );
        unpauseCore2ForFlash( was_paused );
        return -1; // Handle stale/closed, or file not open
    }
    int result = file->readBytes( buffer, size );

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    
    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_fs_read_bytes: Read bytes" );
        Serial.flush( );
    }
    return result;
}

int jl_fs_write_bytes( void* file_handle, const char* data, int size ) {
    if ( !file_handle || !data || size <= 0 )
        return -1;

    AsyncPassthrough::suspendUARTRxIRQ( );
    // CRITICAL: Pause Core2 during flash write operations
    bool was_paused = pauseCore2ForFlash( 100 );

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Stale-handle guard (see jl_fs_read_bytes)
    File* file = jfs_resolve( file_handle );
    if ( !file || !*file ) {
        fs_mutex_release( );
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
        return -1; // Handle stale/closed, or file not open
    }
    int result = file->write( (const uint8_t*)data, size );

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_fs_write_bytes: Written bytes" );
        Serial.flush( );
    }

    return result;
}

int jl_fs_seek( void* file_handle, int position, int mode ) {
    if ( !file_handle )
        return 0;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Stale-handle guard (see jl_fs_read_bytes)
    File* file = jfs_resolve( file_handle );
    if ( !file || !*file ) {
        fs_mutex_release( );
        return 0; // Handle stale/closed, or file not open
    }
    SeekMode seekMode = SeekSet;
    if ( mode == 1 )
        seekMode = SeekCur;
    else if ( mode == 2 )
        seekMode = SeekEnd;
    int result = file->seek( position, seekMode ) ? 1 : 0;

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_fs_seek: Sought to position" );
        Serial.flush( );
    }
    return result;
}

int jl_fs_position( void* file_handle ) {
    if ( !file_handle )
        return -1;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Stale-handle guard (see jl_fs_read_bytes)
    File* file = jfs_resolve( file_handle );
    if ( !file || !*file ) {
        fs_mutex_release( );
        return -1; // Handle stale/closed, or file not open
    }
    int result = file->position( );

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    if ( debug_fs ) {
        Serial.println( "DEBUG: jl_fs_position: Position" );
        Serial.flush( );
    }
    return result;
}

int jl_fs_size( void* file_handle ) {
    if ( !file_handle )
        return -1;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Stale-handle guard (see jl_fs_read_bytes)
    File* file = jfs_resolve( file_handle );
    if ( !file || !*file ) {
        fs_mutex_release( );
        return -1; // Handle stale/closed, or file not open
    }
    int result = file->size( );

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return result;
}

int jl_fs_available( void* file_handle ) {
    if ( !file_handle )
        return 0;

    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    // Stale-handle guard (see jl_fs_read_bytes)
    File* file = jfs_resolve( file_handle );
    if ( !file || !*file ) {
        fs_mutex_release( );
        return 0; // Handle stale/closed, or file not open
    }
    int result = file->available( );

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return result;
}

// Flush file buffer to disk - CRITICAL for read-after-write operations
// THREAD SAFETY: Acquires fs_mutex to prevent concurrent filesystem access
void jl_fs_flush( void* file_handle ) {
    if ( file_handle ) {
        AsyncPassthrough::suspendUARTRxIRQ( );
        // CRITICAL: Pause Core2 during flash write operations
        bool was_paused = pauseCore2ForFlash( 100 );

        fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

        // Stale-handle guard (see jl_fs_read_bytes)
        File* file = jfs_resolve( file_handle );
        if ( file && *file ) { // Only flush if file is actually open
            file->flush( );
        }

        if ( debug_fs ) {
            Serial.println( "DEBUG: jl_fs_flush: Flushed file" );
            Serial.flush( );
        }
        fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
        unpauseCore2ForFlash( was_paused );
        AsyncPassthrough::resumeUARTRxIRQ( );
    }
}

// Directory operations - all require mutex for thread safety
// Returns 0 on success, negative errno on failure
int jl_fs_mkdir( const char* path ) {
    if ( !path )
        return -EIO; // Invalid argument

    // Check if directory already exists BEFORE attempting create
    // This prevents unnecessary flash write attempts
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem
    
    bool exists = FatFS.exists( path );
    if ( exists ) {
        // Check if it's a directory
        bool isdir = false;
        if ( path[ 0 ] == '/' && path[ 1 ] == '\0' ) {
            isdir = true; // Root is always a directory
        } else {
            File f = FatFS.open( path, "r" );
            if ( f ) {
                isdir = f.isDirectory( );
                f.close( );
            }
        }
        
        fs_mutex_release( ); // THREAD SAFETY: Unlock before returning
        
        if ( isdir ) {
            // Directory already exists - return EEXIST (errno 17)
            // This is expected behavior when creating directories recursively
            return -EEXIST;
        } else {
            // Path exists but is a file, not a directory
            return -ENOTDIR; // errno 20
        }
    }
    
    // Directory doesn't exist - try to create it
    // CRITICAL: Pause Core2 during flash write (directory creation modifies flash)
    fs_mutex_release( ); // Release before pausing Core2
    bool was_paused = pauseCore2ForFlash( 100 );
    fs_mutex_acquire( ); // Reacquire after pause
    
    bool result = FatFS.mkdir( path );
    
    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    
    if ( !result ) {
        // mkdir failed - could be various reasons:
        // - Parent directory doesn't exist (ENOENT)
        // - Disk full
        // - Permission issues
        // Return generic EIO since FatFS doesn't give detailed errors
        return -EIO;
    }

    return 0; // Success
}

int jl_fs_rmdir( const char* path ) {
    if ( !path )
        return 0;

    AsyncPassthrough::suspendUARTRxIRQ( );
    bool was_paused = pauseCore2ForFlash( 100 );
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem
    int result = FatFS.rmdir( path ) ? 1 : 0;
    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    return result;
}

int jl_fs_remove( const char* path ) {
    if ( !path )
        return 0;

    AsyncPassthrough::suspendUARTRxIRQ( );
    bool was_paused = pauseCore2ForFlash( 100 );
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem
    int result = FatFS.remove( path ) ? 1 : 0;
    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    return result;
}

int jl_fs_rename( const char* pathFrom, const char* pathTo ) {
    if ( !pathFrom || !pathTo )
        return 0;

    AsyncPassthrough::suspendUARTRxIRQ( );
    bool was_paused = pauseCore2ForFlash( 100 );
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem
    int result = FatFS.rename( pathFrom, pathTo ) ? 1 : 0;
    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    unpauseCore2ForFlash( was_paused );
    AsyncPassthrough::resumeUARTRxIRQ( );

    return result;
}

// Get filesystem info - requires mutex for consistent reads
int jl_fs_total_bytes( void ) {
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    FSInfo info;
    int result = -1;
    if ( FatFS.info( info ) ) {
        result = (int)( info.totalBytes & 0xFFFFFFFF ); // Return lower 32 bits
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return result;
}

int jl_fs_used_bytes( void ) {
    fs_mutex_acquire( ); // THREAD SAFETY: Lock filesystem

    FSInfo info;
    int result = -1;
    if ( FatFS.info( info ) ) {
        result = (int)( info.usedBytes & 0xFFFFFFFF ); // Return lower 32 bits
    }

    fs_mutex_release( ); // THREAD SAFETY: Unlock filesystem
    return result;
}


const char* jl_get_state() {
    static String jsonCache;
    jsonCache = JsonState::getJumperlessStateJSON();
    return jsonCache.c_str();
}

int jl_set_state(const char* jsonState, int clearFirst, int fromWokwi) {
    if (jsonState == nullptr) return -1;

    // When user requests Wokwi parsing we treat the first argument as either
    // a filename on the board or raw Wokwi JSON content.  If the string does
    // not begin with '{' and the file exists we load the file first.
    if (fromWokwi) {
        String json = String(jsonState);
        String errorMsg;

        if (clearFirst) {
            globalState.clear();
        }

        // Load file if it looks like a path
        if (json.length() > 0 && json.charAt(0) != '{' && safeFileExists(json.c_str(), 500)) {
            File f = safeFileOpen(json.c_str(), "r", 1000);
            if (!f) {
                Serial.print("set_state wokwi error: cannot open file ");
                Serial.println(json);
                return -1;
            }
            json = "";
            while (f.available()) {
                json += (char)f.read();
            }
            safeFileClose(f, false);
        }

        int activeSlot = SlotManager::getInstance().getActiveSlot();
        bool success = parseWokwiDiagram(json, globalState, activeSlot, errorMsg);

        if (!success) {
            Serial.print("set_state wokwi error: ");
            Serial.println(errorMsg);
            return -1;
        }

        // Apply hardware routing
        // Unpause Core 2 BEFORE refreshConnections since it internally waits
        bool was_paused = pauseCore2;
        pauseCore2 = false;
        refreshConnections(-1, 1, 1);
        pauseCore2 = was_paused;

        return 0;
    }

    // Default JSON-based state
    String json = String(jsonState);
    bool success = JsonStateParser::applyJSONState(json, clearFirst != 0);

    if (!success) {
        Serial.print("set_state error: ");
        Serial.println(JsonStateParser::getLastError());
        return -1;
    }

    return 0; // Success
}

// ============================================================================
// Graphic Overlay Functions (10x30 Coordinate System)
// ============================================================================
//
// The breadboard is addressed as a 10-row × 30-column grid:
//   Row 1-5 = Top half (E, D, C, B, A)
//   Row 6-10 = Bottom half (F, G, H, I, J)
//   Column 1-30 = Breadboard columns 1-30

/**
 * @brief Add a 2D graphic overlay on the breadboard LEDs
 * @param name Unique identifier for the overlay (max 31 chars)
 * @param startRow Starting row (1-10)
 * @param startCol Starting column (1-30)
 * @param width Width in columns
 * @param height Height in rows
 * @param colors Array of RGB colors in row-major order (0 = transparent)
 * @return Overlay index on success, -1 on failure
 */
int jl_overlay_set(const char* name, int startRow, int startCol,
                   int width, int height, const uint32_t* colors) {
    if (!name || !colors || width <= 0 || height <= 0) {
        return -1;
    }
    return graphicOverlayState.addOverlay(name, startRow, startCol, width, height, colors);
}

/**
 * @brief Clear a specific overlay by name
 * @param name Overlay identifier
 * @return 1 if found and removed, 0 otherwise
 */
int jl_overlay_clear(const char* name) {
    if (!name) return 0;
    return graphicOverlayState.removeOverlay(name) ? 1 : 0;
}

/**
 * @brief Clear all overlays
 */
void jl_overlay_clear_all(void) {
    graphicOverlayState.clearAll();
}

/**
 * @brief Set a single pixel on the breadboard (via direct overlay)
 * @param row Breadboard row (1-10)
 * @param col Column (1-30)
 * @param color RGB color (0 = transparent/off)
 */
void jl_overlay_set_pixel(int row, int col, uint32_t color) {
    graphicOverlayState.setPixel(row, col, color);
}

/**
 * @brief Get the number of active overlays
 * @return Number of active overlays
 */
int jl_overlay_count(void) {
    return graphicOverlayState.numOverlays;
}

/**
 * @brief Move an overlay by a relative offset
 * @param name Overlay identifier
 * @param dRow Row delta
 * @param dCol Column delta
 * @return 1 if found and moved, 0 otherwise
 */
int jl_overlay_shift(const char* name, int dRow, int dCol) {
    if (!name) return 0;
    return graphicOverlayState.shiftOverlay(name, dRow, dCol) ? 1 : 0;
}

/**
 * @brief Move an overlay to an absolute position
 * @param name Overlay identifier
 * @param row New row
 * @param col New column
 * @return 1 if found and moved, 0 otherwise
 */
int jl_overlay_place(const char* name, int row, int col) {
    if (!name) return 0;
    return graphicOverlayState.placeOverlay(name, row, col) ? 1 : 0;
}

/**
 * @brief Serialize all overlays to JSON string
 * @return Pointer to static string buffer containing JSON
 */
char* jl_overlay_serialize(void) {
#if defined(OG_JUMPERLESS)
    static char overlayBuffer[256]; // RP2040: graphic overlays are out on OG
#else
    static char overlayBuffer[4096];
#endif
    String json;
    serializeOverlaysToJSON(json);
    
    // Copy to static buffer
    strncpy(overlayBuffer, json.c_str(), sizeof(overlayBuffer) - 1);
    overlayBuffer[sizeof(overlayBuffer) - 1] = '\0';
    
    return overlayBuffer;
}

} // extern "C"