/**
 * AsyncPassthrough - Bidirectional UART↔USB bridge with command relay
 * 
 * Architecture (complete flow):
 * 
 * 1. UART→USB (Arduino sends commands):
 *    Arduino Serial1 → UART RX (ISR) → ring buffer
 *    → bridge_uart_to_usb() reads ring buffer
 *    → Parses <j>command</j> tags
 *    → Relays command chars to Jerial.relay_buffer
 *    → Raw data (with tags) → USBSer1 (CDC 1) for debugging
 *    → Filtered data (tags removed) → Serial (CDC 0) for display
 * 
 * 2. Jerial.relay_buffer → Command Processing:
 *    RelayBufferStream wraps relay_buffer (automatically strips tags on read)
 *    → MultiSourceStream multiplexes RelayBufferStream + Serial
 *    → TermControl reads from MultiSourceStream (line buffering)
 *    → Main loop calls Jerial.service() → TermControl::service()
 *    → Completed lines → SingleCharCommands → executeCommand()
 * 
 * 3. USB→UART (Upload/send to Arduino):
 *    Serial (CDC 0) → bridge_usb_to_uart() reads
 *    → Raw data → USBSer1 (CDC 1) for debugging
 *    → Filtered/forwarded → UART TX → Arduino Serial1
 * 
 * Benefits:
 *   - Non-blocking command relay using composable Stream layers
 *   - No 8-command queue limit (uses 512-byte relay buffer)
 *   - Complete debug visibility via USBSer1 (CDC 1)
 *   - Clean separation: AsyncPassthrough relays, Jerial processes
 */

// Debug flag for command relay tracing
// Set to 1 to see each character being relayed from <j> tags
#include "ArduinoStuff.h"
#include "Commands.h"
#include "FileParsing.h"
#include "hardware/structs/io_bank0.h"
#define DEBUG_RELAYED_COMMANDS 0

#include "AsyncPassthrough.h"
#include "class/cdc/cdc_device.h"
#include "CommandBuffer.h"  // New simplified command buffer system
#if ASYNC_PASSTHROUGH_ENABLED == 1
// pico-sdk
#include "config.h"
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/regs/uart.h>
#include <hardware/structs/uart.h>
#include <hardware/dma.h>
#include <pico/stdlib.h>
// TinyUSB
#include <tusb.h>

// Arduino core
#include <Arduino.h>
#include "Jerial.h" // Unified serial interface
#include "Python_Proper.h" // For executeSinglePythonCommand and ScriptHistory
#include "Peripherals.h" // For gpio_get_function in floating detection
#include "externVars.h" // For gpioReadingColors and showLEDsCore2
// External Jerial instance
extern JerialClass Jerial;

// External USBSer1 (declared in ArduinoStuff.cpp) - file scope, before namespace
extern Adafruit_USBD_CDC USBSer1;

// External Arduino connection functions (from ArduinoStuff.cpp)
extern int checkIfArduinoIsConnected(void);
extern void connectArduino(int flashOrLocal, int refreshConnections);

// External filesystem mutex (from externVars.h / main.cpp)
// Used to check if filesystem is busy before accepting commands
extern bool fs_mutex_try_acquire(void);
extern void fs_mutex_release(void);

bool asyncPassthroughEnabled = jumperlessConfig.serial_1.async_passthrough;
bool asyncPassthroughTagParsingEnabled = false; // DISABLED during startup - enabled after boot complete

// Startup protection - system must be fully initialized before accepting tag commands
// This prevents crashes if Arduino sends <j> commands before Jumperless is ready
static volatile bool s_startup_complete = false;
static const uint32_t STARTUP_TAG_PARSING_DELAY_MS = 5000;  // Minimum time before tag parsing allowed
static uint32_t s_arduino_release_time = 0;

// Track when tag parsing was re-enabled after flashing
// Used for post-flash cooldown to ensure system stability
static uint32_t s_tag_parsing_reenable_time = 0;
static const uint32_t POST_FLASH_COOLDOWN_MS = 100;  // 100ms cooldown after re-enable  // When Arduino was released from reset
static const uint32_t ARDUINO_BOOT_DELAY_MS = 700;  // Wait this long after Arduino reset release before enabling tag parsing

// Tag parsing timeout support (for Arduino flashing, etc.)
static uint32_t s_tag_parsing_timeout_ms = 0;  // 0 = no timeout
static uint32_t s_tag_parsing_disabled_time = 0;  // When tag parsing was disabled

// Smart re-enable: Track last USB->UART activity to detect end of upload
static uint32_t s_last_usb_to_uart_data_time = 0;  // Last time USB->UART data was sent
static uint32_t s_tag_parsing_inactivity_timeout_ms = 0;  // Re-enable after this many ms of no data (0 = disabled)


bool async_begun = false;
// ============================================================================
// Flash Completion Detection (STK500 protocol watching)
// ============================================================================
// STK500 protocol constants
#define STK_LEAVE_PROGMODE  0x51   // 'Q' - last command avrdude sends
#define CRC_EOP             0x20   // ' ' - end of packet marker

static volatile bool     s_flash_mode_active   = false;  // True while in flash detection mode
static volatile bool     s_stk_leave_seen      = false;  // STK_LEAVE_PROGMODE sequence detected
static volatile uint32_t s_stk_leave_time      = 0;      // When STK_LEAVE was detected
static volatile uint32_t s_flash_start_time    = 0;      // When flash mode was entered
static volatile bool     s_any_flash_data      = false;   // Has any USB→UART data been seen?
static uint8_t           s_stk_prev_byte       = 0;      // Previous byte for 2-byte sequence detection

// Flash detection timeouts
static const uint32_t FLASH_STK_GRACE_MS     = 500;    // Grace period after STK_LEAVE_PROGMODE
static const uint32_t FLASH_INACTIVITY_MS    = 1500;   // Inactivity fallback
static const uint32_t FLASH_NO_DATA_TIMEOUT_MS = 4000; // Abort if avrdude never sent any data
static const uint32_t FLASH_HARD_TIMEOUT_MS  = 15000;  // 15s safety cap

// ============================================================================
// Jumperless Command Tag Detection
// ============================================================================

// State machine for detecting command tags
enum TagState {
    TAG_SEARCHING,           // Looking for '<'
    TAG_DETECTING,           // Reading tag name (both opening and closing)
    TAG_IN_COMMAND           // Inside a command tag, accumulating command
};

// Tag parser state structure
struct TagParserState {
    TagState state;
    char tag_buffer[32];           // Buffer for tag name
    uint8_t tag_buffer_idx;
    char current_tag[32];          // Currently open tag name
    bool needs_python_prefix;      // For <p> tags: true if we need to relay '>'
    bool seen_first_char;          // For <p> tags: true after first non-whitespace char
    
    // Command accumulation buffer - collect entire command before relay
    // This eliminates the need for per-character delays that caused blocking
    char command_buffer[256];      // Buffer for accumulating command content
    uint16_t command_buffer_idx;   // Current position in command buffer
    
    // Timeout: characters processed since entering a non-SEARCHING state
    // Prevents the parser from getting stuck in TAG_IN_COMMAND or TAG_DETECTING
    // if a closing tag never arrives (e.g., due to framing error or truncation)
    uint16_t chars_in_state;       // Characters processed in current tag/command state
};

// Separate state machines for each direction
// Initialize with zeroed command buffers
static TagParserState usb_to_uart_parser = { TAG_SEARCHING, {0}, 0, {0}, false, false, {0}, 0, 0 };
static TagParserState uart_to_usb_parser = { TAG_SEARCHING, {0}, 0, {0}, false, false, {0}, 0, 0 };

// Command relay tracking (no rate limiting - process all commands immediately)
static uint32_t s_last_relayed_command_time = 0;
static uint32_t s_relayed_commands = 0;

// ============================================================================
// UART Response Queue - Routes command responses back to UART
// ============================================================================

// Queue for responses that need to be sent back to UART
#define UART_RESPONSE_QUEUE_SIZE 8
#define UART_RESPONSE_MAX_LEN 256

struct UARTResponseEntry {
    char data[UART_RESPONSE_MAX_LEN];
    uint16_t length;
    uint16_t read_offset;  // Current read position (avoids O(n²) memmove)
    bool pending;
};

static UARTResponseEntry s_uart_response_queue[UART_RESPONSE_QUEUE_SIZE];
static volatile uint8_t s_uart_response_head = 0;
static volatile uint8_t s_uart_response_tail = 0;
static volatile uint8_t s_uart_response_count = 0;

// Track if current command came from UART (for response routing)
static volatile bool s_command_from_uart = false;

// ARCHITECTURAL NOTE: Character-by-character relay instead of line buffering
// ============================================================================
// Previous approach: Accumulate complete commands in 512-byte buffer, relay as lines
// Problem: 8-command queue filled while bridge_uart_to_usb() blocked (900-7000ms)
// 
// Current approach: Relay characters one-by-one as they arrive via Jerial.relayInput()
// Benefits:
//   - Uses larger relay_buffer (no 8-command queue limit)
//   - Commands flow through normal Jerial.read() path
//   - Main loop's line buffering handles command completion naturally
//   - No blocking accumulation - chars available immediately for processing
// ============================================================================

// Supported command tags
static const char* COMMAND_TAGS[] = {
    "jumperlessCommand",
    "j",
    "jumperless",
    "p"  // Python tag - works like 'j' but relays '>' prefix for Python commands
};
static const uint8_t NUM_COMMAND_TAGS = 4;

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------

// Interop mode between AsyncPassthrough and MicroPython UART driver
// 0 = integrate (both use shared IRQ handler)
// 1 = preempt (AsyncPassthrough suspends while MicroPython owns UART0)
#ifndef JL_UART0_INTEROP_MODE
#define JL_UART0_INTEROP_MODE 0
#endif

#ifndef ASYNC_PASSTHROUGH_CDC_ITF
// Bridge TinyUSB CDC instance 1 (USBSer1) <-> Serial1
#define ASYNC_PASSTHROUGH_CDC_ITF 1
#endif

#ifndef ASYNC_PASSTHROUGH_UART
#define ASYNC_PASSTHROUGH_UART uart0
#define ASYNC_PASSTHROUGH_UART_IRQ UART0_IRQ

#define ASYNC_PASSTHROUGH_UART_TX_PIN 0
#define ASYNC_PASSTHROUGH_UART_RX_PIN 1
#endif

#ifndef ASYNC_PASSTHROUGH_UART_DEFAULT_BAUD
#define ASYNC_PASSTHROUGH_UART_DEFAULT_BAUD 115200u
#endif

unsigned long usPerByteSerial1 = ( 1000000 / 115200 + 1 ) * ( 8 + 0 + 1 );
unsigned long serial1baud = 115200;

// ----------------------------------------------------------------------------
// Helpers / State
// ----------------------------------------------------------------------------

static inline void set_micros_per_byte( cdc_line_coding_t const* line_coding ) {

    // Compute an approximate microseconds-per-byte based on line coding.
    // Bits per character = 1 start + data_bits + parity(0|1) + stop_bits(1|1.5|2)
    const uint32_t us_per_bit = ( 1000000u + line_coding->bit_rate - 1 ) / line_coding->bit_rate; // ceil

    const uint32_t parity_bits = ( line_coding->parity == 0 ? 0u : 1u );

    // CDC stop_bits: 0=1 stop, 1=1.5 stop, 2=2 stop
    uint32_t us_for_stops = us_per_bit; // default 1 stop
    if ( line_coding->stop_bits == 2 ) {
        us_for_stops = 2u * us_per_bit;
    } else if ( line_coding->stop_bits == 1 ) {
        // 1.5 stop bits -> round up
        us_for_stops = us_per_bit + ( us_per_bit >> 1 );
    }

    const uint32_t us_without_stops = ( 1u + (uint32_t)line_coding->data_bits + parity_bits ) * us_per_bit;
    usPerByteSerial1 = us_without_stops + us_for_stops;
	serial1baud = line_coding->bit_rate;
}

static inline uint16_t make_serial_config_from_line_coding( uint8_t data_bits, uint8_t parity, uint8_t stop_bits ) {
    // Map TinyUSB CDC line coding to Arduino's SERIAL_* bitfield
    unsigned long data = 0x400ul; // default 8 bits
    unsigned long par = 0x3ul;    // default NONE
    unsigned long stop = 0x10ul;  // default 1 stop bit

    switch ( data_bits ) {
    case 5:
        data = 0x100ul;
        break; // SERIAL_DATA_5
    case 6:
        data = 0x200ul;
        break; // SERIAL_DATA_6
    case 7:
        data = 0x300ul;
        break; // SERIAL_DATA_7
    default:
        data = 0x400ul;
        break; // SERIAL_DATA_8
    }

    // Per CDC spec: 0=None,1=Odd,2=Even,3=Mark,4=Space
    switch ( parity ) {
    case 2:
        par = 0x1ul;
        break; // SERIAL_PARITY_EVEN
    case 1:
        par = 0x2ul;
        break; // SERIAL_PARITY_ODD
    case 3:
        par = 0x4ul;
        break; // SERIAL_PARITY_MARK
    case 4:
        par = 0x5ul;
        break; // SERIAL_PARITY_SPACE
    default:
        par = 0x3ul;
        break; // SERIAL_PARITY_NONE
    }

    // Per CDC spec: 0=1 stop, 1=1.5 stop, 2=2 stop
    switch ( stop_bits ) {
    case 2:
        stop = 0x30ul;
        break; // SERIAL_STOP_BIT_2
    case 1:
        stop = 0x20ul;
        break; // SERIAL_STOP_BIT_1_5
    default:
        stop = 0x10ul;
        break; // SERIAL_STOP_BIT_1
    }

    return (uint16_t)( data | par | stop );
}

// ----------------------------------------------------------------------------
// UART RX IRQ support: push to TinyUSB or fallback ring buffer
// ----------------------------------------------------------------------------

static volatile bool s_uart_flush_pending = false;
// Track suspend state to avoid concurrent access when MicroPython owns UART0
static volatile bool s_uart_suspended_by_mpy = false;
// Track whether UART IRQ has been enabled (deferred from begin() to signalStartupComplete())
static volatile bool s_uart_irq_enabled = false;
// Deferred resync flag - set by ISR, handled in task() to avoid busy_wait in ISR context
static volatile bool s_resync_requested = false;
// =============================================================================
// Zero-CPU DMA UART bridge
// =============================================================================
// RX: a single DMA channel continuously drains the UART RX FIFO into the
// `uartReceived` ring (DREQ_UART0_RX). The PL011 asserts a single-transfer DMA
// request for every byte (FIFO non-empty), so even tiny replies - like the
// 2-byte STK500 bootloader sync - land in RAM immediately with no CPU, no
// FIFO-threshold stranding, and effectively no overrun. The old per-byte ISR
// (timing/garbage heuristics + forced resync) is gone; the PL011 naturally
// re-aligns on the next start bit after any idle gap.
//
// Exposed ring: uartReceived. Must be aligned to its own size for the DMA
// ring-wrap feature. `uartReceivedHead` now tracks the DMA write pointer
// (producer) and `uartReceivedTail` is the consume index (consumer, in task()).
// DMA RX ring for the Arduino UART passthrough. The buffer is aligned to its
// own power-of-2 size (RP2040 DMA ring requirement) and indexed via
// UART_RECEIVED_MASK, both derived from this one define - so shrinking it stays
// consistent. On the RP2040 (OG) the 8 KB ring is precious static .bss/heap, so
// use 2 KB there (still ample for the passthrough at typical baud rates).
#if defined(OG_JUMPERLESS)
#define UART_RX_RING_BITS 11                 // 2^11 = 2048 bytes
#else
// 2^12 = 4096 bytes (was 13 / 8 KB). At 115200 baud the UART delivers
// ~11.5 KB/s and task() drains >=256 B per call, so 4 KB is ~350 ms of
// consumer slack. uartReceivedOverflowCount (X panel / printUARTStats) now
// actually counts laps, so a regression here is visible, not silent.
#define UART_RX_RING_BITS 12
#endif
uint8_t uartReceived[ 1u << UART_RX_RING_BITS ]
    __attribute__( ( aligned( 1u << UART_RX_RING_BITS ) ) );
volatile uint16_t uartReceivedHead = 0;
volatile uint16_t uartReceivedTail = 0;
static volatile uint32_t uartReceivedOverflowCount = 0;
static volatile uint32_t s_uart_overrun_count = 0;

// DMA channels (claimed in begin()). -1 = unclaimed.
static int s_rx_dma_chan = -1;
static int s_tx_dma_chan = -1;
static volatile uint32_t s_tx_dma_inflight = 0;  // bytes handed to TX DMA, not yet retired

// When to release the AVR RST pulse (asserted in checkDTRState(), released by
// the task() timer). Declared here so task() - which appears earlier in the
// file than the DTR section - can see it. 0 = no pulse pending.
static volatile uint32_t s_reset_release_at_ms = 0;

// While true, UART->USB forwarding is suppressed and the RX ring is discarded.
// Set during the Arduino reset pulse so the sketch's dying bytes and the
// reset-glitch bytes never reach avrdude. Cleared (along with a flush of BOTH
// the RX ring and the CDC1 TX FIFO) when the pulse is released, so avrdude sees
// a clean stream that starts at the bootloader.
static volatile bool s_reset_rx_suppress = false;

// Forward declarations for DMA setup (used by begin()/enableUARTReceiver()).
static void setupRxDma( void );
static void setupTxDma( void );

// TX staging ring — main thread pushes, TX DMA drains to the UART HW FIFO.
static uint8_t uartToSend[ 1024 ];
static volatile uint16_t uartToSendHead = 0;   // Main thread writes
static volatile uint16_t uartToSendTail = 0;   // TX DMA reads
#define UART_TOSEND_MASK 0x03FF
static volatile uint32_t uartToSendOverflowCount = 0;

static volatile uint32_t s_uart_framing_error_count = 0;
static volatile uint32_t s_uart_framing_error_total = 0;
static volatile uint32_t s_uart_resync_count = 0;

// Garbage detection - non-ASCII/non-printable bytes indicate misalignment
static volatile uint32_t s_uart_garbage_count = 0;        // Consecutive garbage bytes
static volatile uint32_t s_uart_garbage_total = 0;        // Total garbage bytes seen
static volatile uint32_t s_uart_garbage_resyncs = 0;      // Resyncs triggered by garbage

// Framing error threshold - after this many consecutive FE errors, force resync
#define FRAMING_ERROR_RESYNC_THRESHOLD 3

// Garbage detection threshold - after this many consecutive non-ASCII bytes, resync
// Non-ASCII = bytes outside printable range (0x20-0x7E) plus common control chars
#define GARBAGE_RESYNC_THRESHOLD 4

// Check if a byte is "garbage" (likely from misaligned framing)
// Valid bytes: printable ASCII (0x20-0x7E), newline, carriage return, tab
static inline bool is_garbage_byte(uint8_t c) {
    // Printable ASCII range
    if (c >= 0x00 && c <= 0x7E) return false;
    // Common control characters we expect
    if (c == '\n' || c == '\r' || c == '\t') return false;
    // Null can appear in binary protocols
    if (c == 0x00) return false;
    // Everything else is suspicious - especially high-bit-set bytes (0x80-0xFF)
    return true;
}

// ============================================================================
// Idle Line Detection and Timing Validation
// ============================================================================

// Track timing for idle detection and sync validation
static volatile uint32_t s_last_rx_byte_time_us = 0;
static volatile uint32_t s_last_idle_detected_time = 0;
static volatile uint32_t s_bytes_since_last_idle = 0;
static volatile uint32_t s_idle_periods_detected = 0;
static volatile bool s_line_was_idle = true;  // Start assuming idle (fresh start = synced)

// Timing anomaly detection
static volatile uint32_t s_timing_anomaly_count = 0;
static volatile uint32_t s_last_inter_byte_time_us = 0;

// Idle threshold: line is "idle" if no data for this many microseconds
// At 115200 baud, one byte takes ~87µs, so 200µs means ~2+ byte times of silence
#define UART_IDLE_THRESHOLD_US 200

// Timing validation tolerance: bytes should arrive at ≥ usPerByteSerial1
// Allow up to 20% faster than theoretical minimum (accounts for measurement jitter)
#define TIMING_TOLERANCE_PERCENT 80

#define UART_RECEIVED_MASK ( (uint16_t)( sizeof( uartReceived ) - 1 ) )

// Snapshot buffers used by the UI to show "last data" on the OLED.
// Keep these small and cheap to update from ISR (RX) and task (USB→UART).
#define LAST_DATA_SNAPSHOT_SIZE 64
static volatile char s_last_uart_rx_buf[LAST_DATA_SNAPSHOT_SIZE];
static volatile uint8_t s_last_uart_rx_head = 0;   // next write index (ISR-writable)
static volatile uint8_t s_last_uart_rx_len = 0;    // number of valid bytes (<= SIZE)

static char s_last_usb_to_uart_buf[LAST_DATA_SNAPSHOT_SIZE];
static uint8_t s_last_usb_to_uart_head = 0; // next write index (task context)
static uint8_t s_last_usb_to_uart_len = 0;  // number of valid bytes

// write_addr snapshot for the overflow witness in rx_dma_sync_head().
// NOT trans_count: on the RP2350, arming with 0xFFFFFFFF sets
// TRANS_COUNT.MODE = 0xF = ENDLESS (the SDK's own
// dma_encode_endless_transfer_count()), where the count never decrements -
// so a trans_count delta is identically zero and the witness could never
// fire (review finding). The write pointer advances per byte on both the
// RP2350 and the RP2040, so its masked delta is the arrival count modulo
// the ring size. An exact whole-lap alias (N*4096 bytes between two syncs
// landing on the same masked address) reads as its remainder - the witness
// can undercount extreme laps, but it can no longer miss every one.
static volatile uint32_t s_rx_dma_last_wa = 0;

// Refresh uartReceivedHead from the live RX-DMA write pointer. The DMA writes
// the ring with zero CPU; this is how the consumer "sees" newly arrived bytes.
static inline void rx_dma_sync_head( void ) {
    if ( s_rx_dma_chan < 0 ) return;
    if ( !dma_channel_is_busy( s_rx_dma_chan ) ) {
        // Transfer count exhausted (only after ~GBs of data) - re-arm cleanly.
        setupRxDma();
        return;
    }
    // Overflow witness: if more bytes arrived since the last sync than the
    // ring had free, the DMA overwrote unconsumed data. Count it (once per
    // detection, not per byte) so the stats dumps report real laps.
    uint32_t wa = dma_hw->ch[ s_rx_dma_chan ].write_addr;
    uint16_t arrived = (uint16_t)( ( wa - s_rx_dma_last_wa ) & UART_RECEIVED_MASK );
    s_rx_dma_last_wa = wa;
    uint16_t freeBytes = (uint16_t)( ( uartReceivedTail - uartReceivedHead - 1 ) & UART_RECEIVED_MASK );
    if ( arrived > freeBytes ) uartReceivedOverflowCount++;

    uartReceivedHead =
        (uint16_t)( ( wa - (uint32_t)(uintptr_t)uartReceived ) & UART_RECEIVED_MASK );
}

static inline bool ring_pop_byte( uint8_t* out ) {
    if ( uartReceivedTail == uartReceivedHead ) return false;
    *out = uartReceived[ uartReceivedTail ];
    uartReceivedTail = (uint16_t)( ( uartReceivedTail + 1 ) & UART_RECEIVED_MASK );
    return true;
}

static inline uint16_t ring_available( ) {
    rx_dma_sync_head();
    return (uint16_t)( ( uartReceivedHead - uartReceivedTail ) & UART_RECEIVED_MASK );
}

static inline uint8_t ring_peek_at( uint16_t offset ) {
    uint16_t idx = (uint16_t)( ( uartReceivedTail + offset ) & UART_RECEIVED_MASK );
    return uartReceived[ idx ];
}

// "Clear" the RX ring - discard everything currently buffered by snapping the
// consume index up to the live DMA write pointer (the DMA keeps running).
static inline void ring_clear( void ) {
    rx_dma_sync_head();
    uartReceivedTail = uartReceivedHead;
}

// TX ring buffer helper functions (main context pushes, ISR pops)
static inline bool tx_ring_push_byte( uint8_t b ) {
    uint16_t next_head = (uint16_t)( ( uartToSendHead + 1 ) & UART_TOSEND_MASK );
    if ( next_head == uartToSendTail ) {
        uartToSendOverflowCount++;
        return false;
    }
    uartToSend[ uartToSendHead ] = b;
    uartToSendHead = next_head;
    return true;
}

static inline bool tx_ring_pop_byte( uint8_t *out ) {
    if ( uartToSendHead == uartToSendTail ) return false;
    *out = uartToSend[ uartToSendTail ];
    uartToSendTail = (uint16_t)( ( uartToSendTail + 1 ) & UART_TOSEND_MASK );
    return true;
}

static inline uint16_t tx_ring_available( void ) {
    return (uint16_t)( ( uartToSendHead - uartToSendTail ) & UART_TOSEND_MASK );
}

static inline void tx_ring_clear( void ) {
    if ( s_tx_dma_chan >= 0 && dma_channel_is_busy( s_tx_dma_chan ) ) {
        dma_channel_abort( s_tx_dma_chan );
    }
    s_tx_dma_inflight = 0;
    uartToSendHead = 0;
    uartToSendTail = 0;
}

// Drive the TX DMA: retire a finished transfer, then start the next contiguous
// span from the staging ring. Zero-CPU after kick; non-blocking. Safe to call
// often (no-op while a transfer is in flight or the ring is empty).
static inline void tx_dma_pump( void ) {
    if ( s_tx_dma_chan < 0 ) return;
    if ( dma_channel_is_busy( s_tx_dma_chan ) ) return;

    // A previous transfer has completed - retire those bytes from the ring.
    if ( s_tx_dma_inflight ) {
        uartToSendTail = (uint16_t)( ( uartToSendTail + s_tx_dma_inflight ) & UART_TOSEND_MASK );
        s_tx_dma_inflight = 0;
    }

    uint16_t avail = tx_ring_available();
    if ( avail == 0 ) return;

    // DMA can't wrap a plain transfer, so send up to the end of the buffer; the
    // remainder goes on the next pump.
    uint16_t to_end = (uint16_t)( ( UART_TOSEND_MASK + 1 ) - uartToSendTail );
    uint16_t span = ( avail < to_end ) ? avail : to_end;

    s_tx_dma_inflight = span;
    dma_channel_set_read_addr( s_tx_dma_chan, &uartToSend[ uartToSendTail ], false );
    dma_channel_set_trans_count( s_tx_dma_chan, span, true );  // trigger
}

// Arm the continuous RX DMA: UART RX FIFO -> uartReceived ring, paced by the
// UART RX DREQ, wrapping within the (size-aligned) ring. Runs forever.
static void setupRxDma( void ) {
    if ( s_rx_dma_chan < 0 ) s_rx_dma_chan = dma_claim_unused_channel( true );

    dma_channel_config c = dma_channel_get_default_config( s_rx_dma_chan );
    channel_config_set_transfer_data_size( &c, DMA_SIZE_8 );
    channel_config_set_read_increment( &c, false );          // UART data reg is fixed
    channel_config_set_write_increment( &c, true );           // walk the ring
    channel_config_set_ring( &c, true, UART_RX_RING_BITS );   // wrap the write address
    channel_config_set_dreq( &c, uart_get_dreq( ASYNC_PASSTHROUGH_UART, false ) );
    channel_config_set_high_priority( &c, true );

    uartReceivedTail = 0;
    // reset the overflow witness's baseline to where the DMA starts writing
    s_rx_dma_last_wa = (uint32_t)(uintptr_t)uartReceived;
    dma_channel_configure(
        s_rx_dma_chan, &c,
        uartReceived,                                          // write: ring (aligned)
        &uart_get_hw( ASYNC_PASSTHROUGH_UART )->dr,            // read: UART data register
        0xFFFFFFFFu,                                           // ~infinite transfer
        true );                                                // start now
}

// Configure the TX DMA channel (started per-span by tx_dma_pump()). Read address
// and transfer count are set on each pump; the write address (UART data reg) and
// DREQ are fixed here.
static void setupTxDma( void ) {
    if ( s_tx_dma_chan < 0 ) s_tx_dma_chan = dma_claim_unused_channel( true );

    dma_channel_config c = dma_channel_get_default_config( s_tx_dma_chan );
    channel_config_set_transfer_data_size( &c, DMA_SIZE_8 );
    channel_config_set_read_increment( &c, true );            // walk the staging ring
    channel_config_set_write_increment( &c, false );          // UART data reg is fixed
    channel_config_set_dreq( &c, uart_get_dreq( ASYNC_PASSTHROUGH_UART, true ) );

    s_tx_dma_inflight = 0;
    uartToSendHead = 0;
    uartToSendTail = 0;
    dma_channel_configure(
        s_tx_dma_chan, &c,
        &uart_get_hw( ASYNC_PASSTHROUGH_UART )->dr,            // write: UART data register
        uartToSend,                                            // read: staging ring (set per pump)
        0,                                                     // count set per pump
        false );                                               // don't start
}

// ---------------------------------------------------------------------------
// UI snapshot accessors (lightweight copies of last-data buffers)
// ---------------------------------------------------------------------------
size_t AsyncPassthrough::getLastUsbToUartSnapshot(char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    size_t copyLen = (size_t)s_last_usb_to_uart_len;
    if ( copyLen > outSize - 1 ) copyLen = outSize - 1;

    // Copy in chronological order: head points to next write index
    uint8_t start = (uint8_t)( ( s_last_usb_to_uart_head + LAST_DATA_SNAPSHOT_SIZE - s_last_usb_to_uart_len ) % LAST_DATA_SNAPSHOT_SIZE );
    for ( size_t i = 0; i < copyLen; i++ ) {
        out[i] = s_last_usb_to_uart_buf[(start + i) % LAST_DATA_SNAPSHOT_SIZE];
    }
    out[copyLen] = '\0';
    return copyLen;
}

size_t AsyncPassthrough::getLastUartRxSnapshot(char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    // Disable UART IRQ briefly to get a consistent snapshot
    irq_set_enabled(ASYNC_PASSTHROUGH_UART_IRQ, false);

    size_t copyLen = (size_t)s_last_uart_rx_len;
    if ( copyLen > outSize - 1 ) copyLen = outSize - 1;
    // oldest index is head - len (mod size)
    uint8_t start = (uint8_t)( ( s_last_uart_rx_head + LAST_DATA_SNAPSHOT_SIZE - s_last_uart_rx_len ) % LAST_DATA_SNAPSHOT_SIZE );
    for ( size_t i = 0; i < copyLen; i++ ) {
        out[i] = (char)s_last_uart_rx_buf[(start + i) % LAST_DATA_SNAPSHOT_SIZE];
    }
    out[copyLen] = '\0';

    // Re-enable IRQ (assume normal runtime)
    irq_set_enabled(ASYNC_PASSTHROUGH_UART_IRQ, true);
    return copyLen;
}

// Clear the saved USB->UART snapshot (UI consumed it)
void AsyncPassthrough::clearLastUsbToUartSnapshot() {
    s_last_usb_to_uart_head = 0;
    s_last_usb_to_uart_len = 0;
}

// Clear the saved UART->USB (RX) snapshot. IRQ is briefly disabled to be safe.
void AsyncPassthrough::clearLastUartRxSnapshot() {
    irq_set_enabled(ASYNC_PASSTHROUGH_UART_IRQ, false);
    s_last_uart_rx_head = 0;
    s_last_uart_rx_len = 0;
    irq_set_enabled(ASYNC_PASSTHROUGH_UART_IRQ, true);
}

// Flush TinyUSB CDC buffers for a specific interface
// Used during Arduino reset to ensure clean state
static inline void flush_cdc_buffers( uint8_t itf ) {
    if ( !tud_inited() ) return;
    
    // Flush TX (anything we queued to send to host)
    if ( tud_cdc_n_connected( itf ) ) {
        tud_cdc_n_write_flush( itf );
    }
    
    // Drain RX (discard anything host sent that we haven't processed)
    uint8_t discard[64];
    while ( tud_cdc_n_available( itf ) > 0 ) {
        tud_cdc_n_read( itf, discard, sizeof( discard ) );
    }
}

// Forward declaration for clearTagParserState used in checkDTRState
void clearTagParserState( void );

// "Resync" in the DMA model. There is no shift-register/FIFO state to fight and
// no software ring to clear byte-by-byte: the DMA streams every byte into RAM,
// and the PL011 itself re-aligns on the next valid start bit after any idle gap.
// So a forced resync just means "discard whatever is currently buffered and
// resume cleanly" - i.e. snap the consume index up to the live DMA head.
static inline void uart_force_receiver_resync( void ) {
    rx_dma_sync_head();
    uartReceivedTail = uartReceivedHead;
    s_uart_resync_count++;
    s_uart_framing_error_count = 0;
    s_uart_garbage_count = 0;
}

#if 0  // RX is now DMA-driven; the per-byte ISR (and its resync heuristics) is gone.
static void async_uart_irq_handler( void ) {




    uart_hw_t* hw = uart_get_hw( ASYNC_PASSTHROUGH_UART );
    int i = 0;
    bool had_framing_error = false;
    // return;
    
    // Get current time for timing validation
    uint32_t now_us = time_us_32();
    
    // Check if we were idle before this IRQ (natural sync point)
    uint32_t time_since_last = now_us - s_last_rx_byte_time_us;
    if ( time_since_last > UART_IDLE_THRESHOLD_US && s_last_rx_byte_time_us != 0 ) {
        // Line was idle - this is a natural sync point!
        // UART automatically resynced on the start bit of this new data
        s_line_was_idle = true;
        s_idle_periods_detected++;
        s_last_idle_detected_time = now_us;
        s_bytes_since_last_idle = 0;
        
        // After an idle period, we can trust the framing is good
        // Reset consecutive error counter since we've naturally resynced
        s_uart_framing_error_count = 0;
    } else {
        s_line_was_idle = false;
    }

    int max_bytes_to_process = 16;  // Prevent spending too long in one IRQ if data is flooding in
    int bytes_processed = 0;
    // TX: Drain TX ring buffer into UART HW FIFO (non-blocking)
    // This runs before RX processing to minimize TX latency
    while ( tx_ring_available() > 0 && uart_is_writable( ASYNC_PASSTHROUGH_UART ) && bytes_processed < max_bytes_to_process ) {
        uint8_t b;
        if ( tx_ring_pop_byte( &b ) ) {
            hw->dr = b;
            bytes_processed++;
        }
    }
    // Disable TXIM when TX ring is empty to prevent continuous spurious interrupts.
    // bridge_usb_to_uart() re-enables TXIM when it pushes new data.
    if ( tx_ring_available() == 0 ) {
        hw_clear_bits( &hw->imsc, UART_UARTIMSC_TXIM_BITS );
    }

    bytes_processed = 0;
    while ( uart_is_readable( ASYNC_PASSTHROUGH_UART )  && bytes_processed < max_bytes_to_process ) {
        uint32_t byte_time_us = time_us_32();
        
        // Check for errors BEFORE reading the byte (errors are per-byte in FIFO)
        uint32_t dr = hw->dr;  // Read data register (includes error flags in upper bits)
        uint8_t c = (uint8_t)( dr & 0xFF );
        
        // Timing validation: check if inter-byte time is reasonable
        // Bytes can't arrive faster than one per usPerByteSerial1
        if ( i > 0 && s_last_rx_byte_time_us != 0 ) {
            uint32_t inter_byte_us = byte_time_us - s_last_rx_byte_time_us;
            s_last_inter_byte_time_us = inter_byte_us;
            
            // If bytes are arriving impossibly fast, something is wrong
            uint32_t min_time_us = (usPerByteSerial1 * TIMING_TOLERANCE_PERCENT) / 100;
            if ( inter_byte_us > 0 && inter_byte_us < min_time_us ) {
                // Bytes arriving faster than physically possible - timing anomaly
                // This could indicate we're misaligned and reading partial bytes
                s_timing_anomaly_count++;
            }
        }
        
        s_last_rx_byte_time_us = byte_time_us;
        s_bytes_since_last_idle++;
        
        // Check per-byte error flags from data register
        // DR bits: [11]=OE, [10]=BE, [9]=PE, [8]=FE
        bool has_hw_error = false;
        if ( dr & ( UART_UARTDR_OE_BITS | UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS ) ) {
            if ( dr & UART_UARTDR_FE_BITS ) {
                s_uart_framing_error_count++;
                s_uart_framing_error_total++;
                had_framing_error = true;
                has_hw_error = true;
            }
            if ( dr & UART_UARTDR_OE_BITS ) {
                s_uart_overrun_count++;
            }
            // Don't push bytes with framing errors - they're likely garbage
            if ( dr & UART_UARTDR_FE_BITS ) {
                s_uart_garbage_count++;  // Count as garbage too
                s_uart_garbage_total++;
                continue;  // Skip this corrupted byte
            }
        }
        
        // GARBAGE DETECTION: Disabled - was too aggressive and blocked valid data
        // The original intent was to filter framing errors, but it prevented
        // valid high-bit characters and ViperIDE responses from passing through.
        // Framing error detection via hardware flags (above) is sufficient.
        // 
        // if ( asyncPassthroughTagParsingEnabled && is_garbage_byte(c) ) {
        //     s_uart_garbage_count++;
        //     s_uart_garbage_total++;
        //     // Don't push garbage bytes to the ring buffer
        //     continue;
        // }
        
        // Good byte received - reset consecutive error/garbage counters
        if ( !has_hw_error ) {
            s_uart_framing_error_count = 0;
        }
        s_uart_garbage_count = 0;  // Reset garbage streak on valid byte
        
        ring_push_byte( c );
        i++;
        if (i > max_bytes_to_process) {
            // This shouldn't happen in normal operation
            break;
        }
    }
    
    // Check if we've accumulated too many consecutive framing errors
    // This indicates persistent misalignment that needs a resync
    // NOTE: Garbage byte resync disabled - was too aggressive
    bool needs_resync = false;
    if ( s_uart_framing_error_count >= FRAMING_ERROR_RESYNC_THRESHOLD ) {
        needs_resync = true;
    }
    // Garbage resync disabled - was preventing valid high-bit characters
    // if ( asyncPassthroughTagParsingEnabled && s_uart_garbage_count >= GARBAGE_RESYNC_THRESHOLD ) {
    //     s_uart_garbage_resyncs++;
    //     needs_resync = true;
    // }
    if ( needs_resync ) {
        // Defer resync to task() — calling uart_force_receiver_resync() from the ISR
        // does busy_wait_us(100) at priority 1, which blocks ALL lower-priority
        // interrupts including USB. With continuous garbage data this loops
        // repeatedly and starves the USB stack, causing crashes.
        s_resync_requested = true;
    }
    
    // Clear any sticky error flags in RSR (Receive Status Register)
    uint32_t rsr = hw->rsr;
    if ( rsr & ( UART_UARTRSR_OE_BITS | UART_UARTRSR_FE_BITS | UART_UARTRSR_PE_BITS | UART_UARTRSR_BE_BITS ) ) {
        hw->rsr = 0xFFFFFFFFu;  // Write to RSR (alias of ECR) clears errors
    }

    // CRITICAL: Clear pending interrupt flags via ICR (Interrupt Clear Register).
    // PL011 error interrupts (OE, FE, PE, BE) are STICKY — they are NOT cleared
    // by reading DR or writing RSR. They MUST be cleared via ICR. Without this,
    // a single overrun or framing error causes the ISR to fire in an infinite
    // loop at priority 1, starving USB and all lower-priority code → crash.
    // RT (receive timeout) is also cleared here for completeness.
    hw->icr = UART_UARTICR_OEIC_BITS   // Overrun error
            | UART_UARTICR_FEIC_BITS   // Framing error
            | UART_UARTICR_PEIC_BITS   // Parity error
            | UART_UARTICR_BEIC_BITS   // Break error
            | UART_UARTICR_RTIC_BITS;  // Receive timeout
    
    s_uart_flush_pending = true;

   
}
#endif  // disabled async_uart_irq_handler


// ----------------------------------------------------------------------------
// Runtime command parsing - Parse "tag parsing = on/off" from UART
// ----------------------------------------------------------------------------
#define RUNTIME_CMD_RING_SIZE 32
static char s_runtime_cmd_ring[RUNTIME_CMD_RING_SIZE];
static uint8_t s_runtime_cmd_write_idx = 0;

// Parse runtime control commands from UART (e.g., "tag parsing = on")
// Uses a ring buffer that keeps only the last 32 characters
// Searches for "tag parsing =" anywhere in the buffer, allowing comment prefixes:
//   # tag parsing = off      (Python)
//   // tag parsing = on       (C/C++)
//   ; tag parsing = off       (Assembly/INI)
//   print("tag parsing = on") (anywhere in text)
// Returns: false always (never consumes, just monitors passively)
static inline bool process_runtime_command(uint8_t c) {
    // Add character to ring buffer (always, even if not a command)
    s_runtime_cmd_ring[s_runtime_cmd_write_idx] = c;
    s_runtime_cmd_write_idx = (s_runtime_cmd_write_idx + 1) % RUNTIME_CMD_RING_SIZE;
    
    // Check for command on newline
    if (c == '\n' || c == '\r') {
        // Build a linear string from ring buffer (last RUNTIME_CMD_RING_SIZE chars)
        char temp_buffer[RUNTIME_CMD_RING_SIZE + 1];
        for (int i = 0; i < RUNTIME_CMD_RING_SIZE; i++) {
            temp_buffer[i] = s_runtime_cmd_ring[(s_runtime_cmd_write_idx + i) % RUNTIME_CMD_RING_SIZE];
        }
        temp_buffer[RUNTIME_CMD_RING_SIZE] = '\0';
        
        // Look for "tag parsing" pattern in the buffer (case insensitive)
        // Convert to lowercase for matching
        char lower[RUNTIME_CMD_RING_SIZE + 1];
        for (int i = 0; i < RUNTIME_CMD_RING_SIZE; i++) {
            lower[i] = tolower(temp_buffer[i]);
        }
        lower[RUNTIME_CMD_RING_SIZE] = '\0';
        
        // Search for various forms anywhere in buffer: "tag parsing =", "tag_parsing =", "tagparsing ="
        // This allows comment prefixes like: # tag parsing = off
        char* cmd_pos = nullptr;
        if ((cmd_pos = strstr(lower, "tag parsing =")) != nullptr ||
            (cmd_pos = strstr(lower, "tag_parsing =")) != nullptr ||
            (cmd_pos = strstr(lower, "tagparsing =")) != nullptr) {
            
            // Found command pattern! Extract the value after '='
            char* equals = strchr(cmd_pos, '=');
            if (equals != nullptr && (equals - lower) < RUNTIME_CMD_RING_SIZE - 1) {
                char* value = equals + 1;
                
                // Trim leading whitespace
                while (*value == ' ' || *value == '\t') value++;
                
                // Check value (already lowercase)
                bool new_state = false;
                bool valid_command = false;
                
                if (strncmp(value, "on", 2) == 0 || strncmp(value, "1", 1) == 0 ||
                    strncmp(value, "enable", 6) == 0 || strncmp(value, "true", 4) == 0) {
                    new_state = true;
                    valid_command = true;
                } else if (strncmp(value, "off", 3) == 0 || strncmp(value, "0", 1) == 0 ||
                          strncmp(value, "disable", 7) == 0 || strncmp(value, "false", 5) == 0) {
                    new_state = false;
                    valid_command = true;
                }
                
                if (valid_command) {
                    // SAFETY: Don't allow enabling tag parsing until startup is complete
                    // This prevents crashes from early commands before system is ready
                    if (new_state && !s_startup_complete) {
                        // Silently ignore enable requests during startup
                        // Tag parsing will be enabled automatically after startup completes
                        return false;
                    }
                    
                    // Update RUNTIME state only (not config file)
                    asyncPassthroughTagParsingEnabled = new_state;
                    
                    // Send confirmation back over UART via the TX DMA staging
                    // ring (non-blocking, no FIFO interleave with the DMA).
                    char response[64];
                    int rlen = snprintf(response, sizeof(response), "\r\nTag parsing %s\r\n", 
                            new_state ? "enabled" : "disabled");
                    for (int ri = 0; ri < rlen && ri < (int)sizeof(response); ri++) {
                        tx_ring_push_byte( (uint8_t)response[ri] );
                    }
                    tx_dma_pump();
                }
            }
        }
    }
    
    // Never consume - always forward all characters
    return false;
}

// ----------------------------------------------------------------------------
// Command prefix forwarding to main Serial
// ----------------------------------------------------------------------------
static const char* s_forward_prefixes[ 8 ];
static size_t s_forward_prefix_count = 0;
static size_t s_forward_max_len = 0;
static const char* s_forward_end_tokens[ 8 ];
static size_t s_forward_end_count = 0;
static bool s_forward_end_on_newline = true;
static bool s_forward_active = false;
static unsigned long s_forward_last_byte_us = 0;

static inline bool ring_starts_with( const char* prefix ) {
    const size_t len = strlen( prefix );
    if ( ring_available( ) < len ) return false;
    for ( size_t i = 0; i < len; ++i ) {
        if ( ring_peek_at( (uint16_t)i ) != (uint8_t)prefix[ i ] ) return false;
    }
    return true;
}

static inline void ring_discard_n( size_t n ) {
    for ( size_t i = 0; i < n; ++i ) {
        uint8_t tmp;
        if ( !ring_pop_byte( &tmp ) ) break;
    }
}

static void process_uart_forward_prefixes( ) {
    if ( s_forward_prefix_count == 0 ) return;
    // Try each prefix; on first match, discard prefix and forward until newline or buffer empty
    for ( size_t i = 0; i < s_forward_prefix_count; ++i ) {
        const char* pfx = s_forward_prefixes[ i ];
        if ( !pfx ) continue;
        if ( ring_starts_with( pfx ) ) {
            const size_t plen = strlen( pfx );
            ring_discard_n( plen );
            s_forward_active = true;
            return; // Only process one prefix per task iteration
        }
    }
}

static inline bool is_end_token_seen( uint8_t last_byte ) {
    // Newline handling
    if ( s_forward_end_on_newline && ( last_byte == '\n' || last_byte == '\r' ) ) return true;
    // Token-based
    if ( s_forward_end_count == 0 ) return false;
    for ( size_t i = 0; i < s_forward_end_count; ++i ) {
        const char* tok = s_forward_end_tokens[ i ];
        if ( !tok ) continue;
        const size_t len = strlen( tok );
        if ( len == 0 ) continue;
        if ( ring_available( ) < len ) continue;
        bool match = true;
        for ( size_t j = 0; j < len; ++j ) {
            if ( ring_peek_at( (uint16_t)( ring_available( ) - len + j ) ) != (uint8_t)tok[ j ] ) {
                match = false; break;
            }
        }
        if ( match ) {
         Serial.println("End token seen!");   
        return true;
        }

    }
    return false;
}

// Check if a tag name matches a command tag
static inline bool is_command_tag( const char* tag ) {
    for ( uint8_t i = 0; i < NUM_COMMAND_TAGS; i++ ) {
        if ( strcmp( tag, COMMAND_TAGS[i] ) == 0 ) {
            return true;
        }
    }
    return false;
}

// Process a single byte through the command tag state machine
// Returns true if byte should be forwarded, false if consumed by tag processing
// NOTE: In mode 1 (passthrough + parsing), this ALWAYS returns true to forward all chars
//       while still processing tags for commands. Mode 2 (future) will consume tag chars.
static inline bool process_command_tag_byte( uint8_t c, TagParserState* parser, const char* direction ) {
    bool should_strip_tags = (jumperlessConfig.serial_1.tag_parsing == 2);  // Mode 2 = strip tags
    
    if (flashingArduino) {
        // If we're currently flashing the Arduino, we should ignore all tag parsing to prevent crashes
        // Just forward all bytes without processing tags
        return true;
    }
    // TIMEOUT: If we've been in a non-SEARCHING state for too many characters,
    // the tag is malformed or we lost sync. Reset to prevent getting permanently stuck.
    // Max command is 256 bytes + tag overhead (~40 chars) = ~300 chars max reasonable.
    // Use 512 as a generous upper limit.
    if ( parser->state != TAG_SEARCHING ) {
        parser->chars_in_state++;
        if ( parser->chars_in_state > 512 ) {
            // Tag parser stuck - reset to searching state
            parser->state = TAG_SEARCHING;
            parser->tag_buffer_idx = 0;
            parser->command_buffer_idx = 0;
            parser->current_tag[0] = '\0';
            parser->needs_python_prefix = false;
            parser->seen_first_char = false;
            parser->chars_in_state = 0;
            return true;  // Forward the byte
        }
    }
    
    switch ( parser->state ) {
        case TAG_SEARCHING:
            if ( c == '<' ) {
                parser->state = TAG_DETECTING;
                parser->tag_buffer_idx = 0;
                parser->chars_in_state = 0;  // Reset timeout counter
                memset( parser->tag_buffer, 0, sizeof(parser->tag_buffer) );
                return !should_strip_tags; // Mode 1: forward, Mode 2: consume
            }
            return true; // Forward normal byte
            
        case TAG_DETECTING:
            if ( c == '>' ) {
                // End of opening tag
                parser->tag_buffer[parser->tag_buffer_idx] = '\0';
                
                // Check if this is a closing tag (starts with '/')
                if ( parser->tag_buffer[0] == '/' ) {
                    // Closing tag
                    if ( strcmp( &parser->tag_buffer[1], parser->current_tag ) == 0 ) {
                        // Valid closing tag for current command
                        // Safety: ensure buffer index is within bounds
                        if (parser->command_buffer_idx >= sizeof(parser->command_buffer)) {
                            parser->command_buffer_idx = sizeof(parser->command_buffer) - 1;
                        }
                        // Null-terminate the accumulated command
                        parser->command_buffer[parser->command_buffer_idx] = '\0';
                        
                        s_command_from_uart = true;
                        
                        // NEW: Use CommandBuffer for synchronous command handling
                        // This replaces the complex Jerial relay buffer system
                        if (parser->command_buffer_idx > 0) {
                            // =========================================================
                            // SAFETY CHECK: Startup, flashing, and filesystem protection
                            // =========================================================
                            // 1. Don't accept commands during startup
                            // 2. Don't accept commands during Arduino flashing
                            // 3. Check if filesystem is busy (might cause crash)
                            // =========================================================
                            
                            if ( !s_startup_complete ) {
                                // System not ready - silently ignore command
                                // This prevents crashes from early Arduino commands
                                #if DEBUG_RELAYED_COMMANDS
                                Serial.println("Tag command ignored (startup not complete)");
                                #endif
                                parser->state = TAG_SEARCHING;
                                parser->command_buffer_idx = 0;
                                return !should_strip_tags;
                            }
                            
                            // Check if Arduino is being flashed - don't process commands during this
                            // extern declared in ArduinoStuff.h
                            extern volatile bool flashingArduino;
                            if ( flashingArduino ) {
                                // Flashing in progress - ignore command to prevent crashes
                                #if DEBUG_RELAYED_COMMANDS
                                Serial.println("Tag command ignored (flashing in progress)");
                                #endif
                                parser->state = TAG_SEARCHING;
                                parser->command_buffer_idx = 0;
                                return !should_strip_tags;
                            }
                            
                            // Check post-flash cooldown - ensure system is stable after re-enable
                            // This prevents race conditions when Arduino sends commands immediately after boot
                            if ( s_tag_parsing_reenable_time > 0 && 
                                 (millis() - s_tag_parsing_reenable_time) < POST_FLASH_COOLDOWN_MS ) {
                                // Wait for Core 2 to be idle before accepting commands
                                extern volatile bool core2busy;
                                if ( core2busy ) {
                                    // Core 2 still busy - defer command
                                    #if DEBUG_RELAYED_COMMANDS
                                    Serial.println("Tag command deferred (post-flash cooldown, Core2 busy)");
                                    #endif
                                    parser->state = TAG_SEARCHING;
                                    parser->command_buffer_idx = 0;
                                    return !should_strip_tags;
                                }
                            }
                            
                            // Check if filesystem is busy (non-blocking check)
                            // If busy, defer command - it will be lost but prevents crash
                            bool fs_available = fs_mutex_try_acquire();
                            if ( fs_available ) {
                                // Got mutex - release immediately, we just needed to check
                                fs_mutex_release();
                            } else {
                                // Filesystem is busy - can't safely process command
                                // Better to lose the command than crash
                                #if DEBUG_RELAYED_COMMANDS
                                Serial.println("Tag command deferred (filesystem busy)");
                                #endif
                                parser->state = TAG_SEARCHING;
                                parser->command_buffer_idx = 0;
                                return !should_strip_tags;
                            }
                            
                            s_relayed_commands++;
                            
                            // Determine if this is a <p> (Python) or <j> (raw) command
                            bool isPythonTag = (strcmp(parser->current_tag, "p") == 0);
                            bool accepted = false;
                            
                            if (isPythonTag) {
                                accepted = CommandBuffer::getInstance().setPendingPCommand(parser->command_buffer);
                            } else {
                                accepted = CommandBuffer::getInstance().setPendingJCommand(parser->command_buffer);
                            }
                            
                            if (!accepted) {
                                // Previous command still pending - can't accept new one
                                // Reset state but don't lose the command
                                // In practice this shouldn't happen if main loop processes fast enough
                                parser->state = TAG_SEARCHING;
                                parser->command_buffer_idx = 0;
                                return !should_strip_tags;
                            }
                        }
                        
                        s_last_relayed_command_time = millis();
                        
                        // Reset state - just reset indices, no need to memset large buffers
                        parser->state = TAG_SEARCHING;
                        parser->current_tag[0] = '\0';  // Just null-terminate, don't memset
                        parser->command_buffer_idx = 0;  // Just reset index
                        parser->needs_python_prefix = false;
                        parser->seen_first_char = false;
                    } else {
                        // Mismatched closing tag, treat as normal text
                        parser->state = TAG_SEARCHING;
                        parser->command_buffer_idx = 0;  // Just reset index
                    }
                    return !should_strip_tags; // Mode 1: forward, Mode 2: consume
                    
                } else if ( is_command_tag( parser->tag_buffer ) ) {
                    // Valid opening command tag
                    strcpy( parser->current_tag, parser->tag_buffer );
                    parser->state = TAG_IN_COMMAND;
                    
                    // Reset command buffer for new command
                    parser->command_buffer_idx = 0;
                    parser->chars_in_state = 0;  // Reset timeout counter for command accumulation
                    
                    // For Python tags, prepare to relay '>' prefix if needed
                    if ( strcmp( parser->current_tag, "p" ) == 0 ) {
                        parser->needs_python_prefix = true;
                        parser->seen_first_char = false;
                    }
                    
                    return !should_strip_tags; // Mode 1: forward, Mode 2: consume
                    
                } else {
                    // Not a command tag, forward the whole thing
                    parser->state = TAG_SEARCHING;
                    return true; // Let this '>' through
                }
                
            } else if ( parser->tag_buffer_idx < sizeof(parser->tag_buffer) - 1 ) {
                parser->tag_buffer[parser->tag_buffer_idx++] = c;
                return !should_strip_tags; // Mode 1: forward, Mode 2: consume tag name
                
            } else {
                // Tag buffer overflow, treat as normal text
                parser->state = TAG_SEARCHING;
                return true;
            }
            break;
            
        case TAG_IN_COMMAND:
            if ( c == '<' ) {
                // Possible closing tag
                parser->state = TAG_DETECTING;
                parser->tag_buffer_idx = 0;
                memset( parser->tag_buffer, 0, sizeof(parser->tag_buffer) );
                return !should_strip_tags; // Mode 1: forward, Mode 2: consume
                
            } else {
                // For <p> tags, add '>' prefix only if not already present
                if ( parser->needs_python_prefix && !parser->seen_first_char ) {
                    // Skip whitespace when checking for existing '>'
                    if ( c != ' ' && c != '\t' && c != '\n' && c != '\r' ) {
                        parser->seen_first_char = true;
                        
                        // Only add '>' prefix if the command doesn't already start with it
                        if ( c != '>' && parser->command_buffer_idx < sizeof(parser->command_buffer) - 1 ) {
                            parser->command_buffer[parser->command_buffer_idx++] = '>';
                        }
                        
                        parser->needs_python_prefix = false;
                    }
                }
                
                // NON-BLOCKING: Accumulate character into command buffer
                // The entire command will be relayed at once when closing tag is detected
                // This eliminates the need for blocking delayMicroseconds(350) per character
                if ( parser->command_buffer_idx < sizeof(parser->command_buffer) - 1 ) {
                    parser->command_buffer[parser->command_buffer_idx++] = c;
                }
                // If buffer is full, just drop characters (command too long)
                
                return !should_strip_tags; // Mode 1: forward, Mode 2: consume
            }
            break;
    }
    
    return true; // Default: forward
}


static inline void bridge_usb_to_uart( uint8_t itf ) {
    // Drain CDC RX and forward to UART (pico-sdk, HW FIFO)
    // Process through command tag filter
    uint8_t buf[ 64 ];  // Smaller buffer to process in chunks (less blocking)
    uint8_t forward_buf[ 64 ];
    
    // Only process one small chunk per call to avoid hogging CPU.
    // BACKPRESSURE (sweep finding, high): read at most what the TX staging
    // ring can actually hold. tx_ring_push_byte drops on a full ring, so a
    // host writing faster than the UART drains used to lose bytes SILENTLY
    // mid-upload; leaving them in the CDC FIFO instead makes USB NAK and the
    // host wait, which is what every other serial bridge does.
    if ( tud_cdc_n_available( itf ) ) {
        uint16_t ringFree = (uint16_t)( ( uartToSendTail - uartToSendHead - 1 ) & UART_TOSEND_MASK );
        if ( ringFree == 0 ) return;          // drain first, read next pass
        size_t room = sizeof( buf );
        if ( ringFree < room ) room = ringFree;
        size_t rd = tud_cdc_n_read( itf, buf, room );
        if ( rd > 0 ) {
            
           
            // Track USB->UART data activity for inactivity timeout
            // CRITICAL: Only track when tag parsing is DISABLED (during flashing)
            // This lets us detect when the Arduino upload finishes (500ms of no data)
            if ( !asyncPassthroughTagParsingEnabled && s_tag_parsing_inactivity_timeout_ms > 0 ) {
                // Tag parsing disabled - track data to detect upload completion
                s_last_usb_to_uart_data_time = millis();
            }

            // Track activity and watch for STK_LEAVE_PROGMODE during flash mode
            if ( s_flash_mode_active ) {
                uint32_t now = millis();
                s_last_usb_to_uart_data_time = now;
                s_any_flash_data = true;

                // Scan for STK_LEAVE_PROGMODE (0x51) followed by CRC_EOP (0x20)
                if ( !s_stk_leave_seen ) {
                    for ( size_t i = 0; i < rd; i++ ) {
                        if ( s_stk_prev_byte == STK_LEAVE_PROGMODE && buf[i] == CRC_EOP ) {
                            s_stk_leave_seen = true;
                            s_stk_leave_time = now;
                            break;
                        }
                        s_stk_prev_byte = buf[i];
                    }
                }
            }
            
            size_t forward_idx = 0;
            
            // // Copy raw data to USBSer1 (CDC 1) for debugging BEFORE tag processing
            // if ( tud_cdc_n_connected( 1 ) ) {
            //     for ( size_t i = 0; i < rd; i++ ) {
            //         tud_cdc_n_write_char( 1, buf[i] );
            //     }
            //     tud_cdc_n_write_flush( 1 );
            // }
            // Process each byte through tag detector (USB→UART)
            for ( size_t i = 0; i < rd; i++ ) {
                // Monitor for runtime control commands (e.g., "tag parsing = on")
                // Skip during flashing - binary STK500 data would corrupt the 32-char
                // ring buffer and could false-trigger "tag parsing = on/off" matches
                if ( !flashingArduino ) {
                    process_runtime_command(buf[i]);
                }
                
                if ( asyncPassthroughTagParsingEnabled ) {
                    // Tag parsing enabled - check for command tags
                    if ( process_command_tag_byte( buf[i], &usb_to_uart_parser, "USB→UART" ) ) {
                        // Byte should be forwarded to UART
                        forward_buf[forward_idx++] = buf[i];
                    }
                } else {
                    // Tag parsing disabled - pass through all bytes directly (binary upload)
                    forward_buf[forward_idx++] = buf[i];
                }
            }
            
            // Forward to UART via the zero-CPU TX DMA. Identical path during
            // flashing and normal passthrough: push into the staging ring and
            // kick the DMA. STK500 request/response timing is fine because the
            // DMA moves bytes into the UART FIFO immediately, and it never
            // blocks Core 0 (the old flashing path busy-waited on the FIFO).
            if ( forward_idx > 0 ) {
                // Update USB→UART "last data" snapshot (keep last LAST_DATA_SNAPSHOT_SIZE bytes)
                for ( size_t fi = 0; fi < forward_idx; fi++ ) {
                    s_last_usb_to_uart_buf[s_last_usb_to_uart_head] = (char)forward_buf[fi];
                    s_last_usb_to_uart_head = (uint8_t)( ( s_last_usb_to_uart_head + 1 ) % LAST_DATA_SNAPSHOT_SIZE );
                    if ( s_last_usb_to_uart_len < LAST_DATA_SNAPSHOT_SIZE ) s_last_usb_to_uart_len++;
                }

                for ( size_t fi = 0; fi < forward_idx; fi++ ) {
                    tx_ring_push_byte( forward_buf[fi] );
                }
                tx_dma_pump();
            }
           
        } else {


            
        }
    }
}

static inline void bridge_uart_to_usb( uint8_t itf ) {
    // Flush any ring-buffered bytes from the RX DMA to CDC
    if ( !tud_inited( ) ) return;

    // During an Arduino reset pulse, do NOT forward UART->USB - just discard
    // whatever the DMA captured (the sketch's dying bytes and reset glitch).
    // This keeps avrdude's stream clean so its first real bytes are from the
    // freshly-reset bootloader, not leftover sketch output.
    if ( s_reset_rx_suppress ) {
        rx_dma_sync_head();
        uartReceivedTail = uartReceivedHead;
        return;
    }

    // If CDC is not connected, drain ring buffer to prevent overflow.
    // Without this, the DMA keeps filling the ring while nobody pops it,
    // and the ring fills up — dropping all subsequent UART data until the
    // host reconnects. During flashing, this would lose bootloader responses
    // if avrdude hasn't opened the port yet.
    if ( !tud_cdc_n_connected( itf ) ) {
        // Just advance tail to head — discard all buffered data
        uartReceivedTail = uartReceivedHead;
        return;
    }
    uint32_t wrote = 0;
    uint32_t avail = tud_cdc_n_write_available( itf );
    uint8_t c;
    




    // Limit per-call processing to keep other services responsive.
    // 256 bytes ≈ 256µs of tag parsing — large enough to keep up with
    // 115200 baud (~11520 bytes/sec) at reasonable task() call rates,
    // small enough to not block the main loop.
    const uint32_t MAX_BYTES_PER_CALL = 256;
    uint32_t processed = 0;
    
    // If in forwarding mode, stream all bytes to main Serial until end token or timeout
    if ( s_forward_active ) {
        unsigned long forwardStart = micros();
        bool ended = false;
        
        // CRITICAL: Reduce chunk size during forwarding (Serial.write is VERY slow!)
        // Even 128 bytes can take 800-3400ms when writing to Serial!
        const uint32_t MAX_FORWARD_BYTES = 64;  // Smaller chunk for Serial output
        uint32_t forwardCount = 0;
        
        while ( ring_pop_byte( &c ) && forwardCount < MAX_FORWARD_BYTES ) {
            // Serial.write( c );
            // gpioReadingColors[9] = 0x200400;
            s_forward_last_byte_us = micros();
            wrote++;
            processed++;
            forwardCount++;
            // Check for end on newline or token match
            if ( is_end_token_seen( c ) ) { ended = true; break; }
            // Timeout: if idle > 8*usPerByteSerial1, end session
            if ( micros() - s_forward_last_byte_us > ( 8 * usPerByteSerial1 ) ) { ended = true; break; }
        }
        if ( ended ) {
            // Serial.flush();  // REMOVED: Too slow! Data will flush naturally
            s_forward_active = false;
        }
        
        unsigned long forwardTime = micros() - forwardStart;
        #if DEBUG_RELAYED_COMMANDS
        if (forwardTime > 50000) {
            Serial.printf("⏱️  forwarding took %lu ms (%u bytes)\n", forwardTime / 1000, forwardCount);
        }
        #endif
    }
    
    // Process UART->USB data through tag parser (for Arduino-sent commands)
    // Limit to MAX_BYTES_PER_CALL to prevent blocking
    // CRITICAL: Check CDC write space BEFORE popping from ring buffer!
    // Previous code popped unconditionally, losing data when CDC was full.
    bool cdc1_connected = tud_cdc_n_connected( 1 );


    while ( ring_available() > 0 && processed < MAX_BYTES_PER_CALL ) {
        // Check if CDC 1 has space BEFORE popping - don't pop if we'd just drop the byte
        if ( cdc1_connected && tud_cdc_n_write_available( 1 ) == 0 ) {
            // CDC 1 buffer full - stop processing to avoid data loss
            // Data stays in ring buffer and will be sent next call
            break;
        }
        
        if ( !ring_pop_byte( &c ) ) break;

        // Update the RX "last data" snapshot for the OLED. This used to live in
        // the per-byte ISR (ring_push_byte); with DMA filling the ring, the
        // consumer maintains it instead.
        s_last_uart_rx_buf[s_last_uart_rx_head] = (char)c;
        s_last_uart_rx_head = (uint8_t)( ( s_last_uart_rx_head + 1 ) % LAST_DATA_SNAPSHOT_SIZE );
        if ( s_last_uart_rx_len < LAST_DATA_SNAPSHOT_SIZE ) s_last_uart_rx_len++;

        // Monitor for runtime control commands (e.g., "tag parsing = on")
        // Skip during flashing - binary bootloader responses would corrupt the
        // 32-char ring buffer and could false-trigger command matches
        if ( !flashingArduino ) {
            process_runtime_command(c);
        }
        
        // Passthrough all data to CDC 1 (UART->USB Arduino output)
        if ( cdc1_connected && tud_cdc_n_write_available( 1 ) > 0 ) {
            tud_cdc_n_write_char( 1, c );
            wrote++;
        }
        
        // Process command tags if enabled
        if ( asyncPassthroughTagParsingEnabled ) {
            // Tag parsing enabled - check for command tags
            if ( process_command_tag_byte( c, &uart_to_usb_parser, "UART→USB" ) ) {
                // Byte should be forwarded to CDC 0 (main serial - for ViperIDE, etc.)
                // In mode 1 (passthrough + parsing), this returns true for ALL chars
                // In mode 2 (strip tags), this returns false for tag-related chars
                // Check availability per-write to avoid blocking
                // if ( tud_cdc_n_connected( 1 ) && tud_cdc_n_write_available( 1 ) > 0 ) {
                //     tud_cdc_n_write_char( 1, c );
                // }
            }
        } else {
            // Tag parsing disabled - pass through all bytes directly to CDC 0
            // Check availability per-write to avoid blocking
            // if ( tud_cdc_n_connected( 1 ) && tud_cdc_n_write_available( 1) > 0 ) {
            //     tud_cdc_n_write_char( 1, c );
            // }
        }
        processed++;
    }
    
    // Flush USBSer1 (CDC 1) if we wrote to it
    if ( processed > 0 && tud_cdc_n_connected( 1 ) ) {
        tud_cdc_n_write_flush( 1 );
    }
    
    if ( wrote > 0 || s_uart_flush_pending ) {
        unsigned long flushStart = micros();
        tud_cdc_n_write_flush( itf );
        unsigned long flushTime = micros() - flushStart;
        if (flushTime > 50000) {
            Serial.printf("⏱️  tud_cdc_n_write_flush took %lu ms\n", flushTime / 1000);
        }
        
        unsigned long tudStart = micros();
        TinyUSB_Device_Task(); // mutex-guarded pump: service USB to reduce latency under load
        unsigned long tudTime = micros() - tudStart;
        if (tudTime > 50000) {
            Serial.printf("⏱️  tud_task (in bridge) took %lu ms\n", tudTime / 1000);
        }
    }
    s_uart_flush_pending = ( uartReceivedTail != uartReceivedHead );
}

// ISR-safe flags updated by TinyUSB callbacks, processed in main context
static volatile bool s_usb_rx_pending = false;
static volatile bool s_apply_line_coding_pending = false;
volatile bool s_line_coding_override = false;

static cdc_line_coding_t s_line_coding = { .bit_rate = ASYNC_PASSTHROUGH_UART_DEFAULT_BAUD, .stop_bits = 1, .parity = 0, .data_bits = 8 };

// ----------------------------------------------------------------------------
// TinyUSB CDC Callbacks (C linkage)
// ----------------------------------------------------------------------------

extern "C" {

void tud_cdc_rx_cb( uint8_t itf ) {
    // Mark pending work; do actual bridging in main context to avoid ISR reentrancy
    if ( itf == ASYNC_PASSTHROUGH_CDC_ITF ) {
        s_usb_rx_pending = true;
    }
}

// Cache new line coding and apply it in main context
void tud_cdc_line_coding_cb( uint8_t itf, cdc_line_coding_t const* p ) {
    if ( itf != ASYNC_PASSTHROUGH_CDC_ITF )
        return;
    s_line_coding = *p;
    s_apply_line_coding_pending = true;

    AsyncPassthrough::processPendingLineCoding();

}
}

// ----------------------------------------------------------------------------
// External functions / state from ArduinoStuff.cpp
// ----------------------------------------------------------------------------
extern void SetArduinoResetLine(bool state, int topBottomBoth);
extern void ESPReset(void);
extern volatile bool flashingArduino;

// ----------------------------------------------------------------------------
// Foreground task runner
// ----------------------------------------------------------------------------

namespace AsyncPassthrough {

// Force the PL011 receiver to re-align to a clean byte boundary.
//
// Why this is needed: a UART receiver only self-aligns across a genuine *idle*
// gap (line high) - the next high->low is then a true start bit. When the
// Jumperless reboots while the Arduino is already mid-transmission, the DMA
// receiver can come up mid-byte. With continuous-ish data it can stay misframed,
// which is the "out of sync until I flash" symptom (flashing resets the Arduino,
// whose setup() delay produces a long clean idle, so it re-aligns there).
//
// This waits for a real idle gap (no new DMA bytes for IDLE_US, capped), then
// disables/re-enables RXE during that idle so the receiver is guaranteed to
// start the next byte aligned, and re-arms the RX DMA from the ring base.
static void uart_rx_clean_resync( uint32_t max_wait_us ) {
    uart_hw_t* hw = uart_get_hw( ASYNC_PASSTHROUGH_UART );
    const uint32_t IDLE_US = 2000;   // > any inter-byte gap at >=115200; sketch gaps are ms

    // 1. Wait for a genuine idle gap (DMA write pointer stops advancing).
    //    (:: - these ring indices are file-scope globals, not namespace members.)
    uint32_t start = time_us_32();
    rx_dma_sync_head();
    uint16_t last_head = ::uartReceivedHead;
    uint32_t last_change = start;
    while ( time_us_32() - start < max_wait_us ) {
        rx_dma_sync_head();
        if ( ::uartReceivedHead != last_head ) {
            last_head = ::uartReceivedHead;
            last_change = time_us_32();
        } else if ( time_us_32() - last_change > IDLE_US ) {
            break;  // line has been idle long enough
        }
        TinyUSB_Device_Task();  // keep USB alive during the (boot-time) wait (mutex-guarded pump)
    }

    // 2. Re-align the receiver during the idle. Abort the DMA first so we can
    //    drain the HW FIFO without racing it, cycle RXE, then re-arm.
    if ( s_rx_dma_chan >= 0 && dma_channel_is_busy( s_rx_dma_chan ) ) {
        dma_channel_abort( s_rx_dma_chan );
    }
    hw_clear_bits( &hw->cr, UART_UARTCR_RXE_BITS );
    busy_wait_us( 50 );
    while ( !( hw->fr & UART_UARTFR_RXFE_BITS ) ) { (void)hw->dr; }  // drain residue
    hw->rsr = 0xFFFFFFFFu;                                            // clear sticky errors
    hw_set_bits( &hw->cr, UART_UARTCR_RXE_BITS );

    // 3. Re-arm the RX DMA from the ring base (resets head/tail to 0).
    setupRxDma();
}

// "Go live" point for RX, called when boot is complete. The RX DMA was armed in
// begin(); here we force a clean byte-aligned resync (handles the case where the
// Arduino was already streaming when the Jumperless rebooted), discard anything
// captured during boot, and mark the receiver live.
static void enableUARTReceiver() {
    if ( s_uart_irq_enabled ) return;
    if ( s_rx_dma_chan < 0 ) setupRxDma();
    uart_rx_clean_resync( 60000 );  // up to 60ms (one-time, at boot) to find an idle gap
    ring_clear();                   // discard boot-time bytes (snap tail to DMA head)
    tx_ring_clear();
    s_uart_irq_enabled = true;
}

void begin( unsigned long baud ) {
    // Configure UART pins and UART with HW FIFO enabled
    gpio_set_function( ASYNC_PASSTHROUGH_UART_TX_PIN, GPIO_FUNC_UART );
    gpio_set_function( ASYNC_PASSTHROUGH_UART_RX_PIN, GPIO_FUNC_UART );
    gpio_function_map[8] = GPIO_FUNC_UART;
    gpio_function_map[9] = GPIO_FUNC_UART;

    uart_init( ASYNC_PASSTHROUGH_UART, baud );
    uart_set_format( ASYNC_PASSTHROUGH_UART, 8, 1, UART_PARITY_NONE );
    uart_set_fifo_enabled( ASYNC_PASSTHROUGH_UART, true );

    // Enable the UART's DMA request lines (uart_init already does this, but be
    // explicit). The PL011 then asserts a single-transfer DREQ per RX byte and
    // a TX DREQ whenever the TX FIFO has space.
    uart_get_hw( ASYNC_PASSTHROUGH_UART )->dmacr =
        UART_UARTDMACR_TXDMAE_BITS | UART_UARTDMACR_RXDMAE_BITS;

    setDTRLockout(3000);

    // Reset legacy stats (still surfaced by the diagnostics API; mostly 0 now).
    s_uart_framing_error_count = 0;
    s_uart_framing_error_total = 0;
    s_uart_overrun_count = 0;
    s_uart_resync_count = 0;
    s_uart_garbage_count = 0;
    s_uart_garbage_total = 0;
    s_uart_garbage_resyncs = 0;
    s_timing_anomaly_count = 0;
    s_idle_periods_detected = 0;
    s_bytes_since_last_idle = 0;
    s_last_rx_byte_time_us = 0;
    s_line_was_idle = true;

    // Set up the zero-CPU DMA bridge: RX ring (continuous) + TX (per-span).
    setupRxDma();
    setupTxDma();

    // Track initial micros per byte based on default config
    s_line_coding.bit_rate = (uint32_t)baud;
    s_line_coding.data_bits = 8;
    s_line_coding.parity = 0;
    s_line_coding.stop_bits = 0; // 1 stop
    set_micros_per_byte( &s_line_coding );

    // Tag parsing stays disabled until signalStartupComplete().
    asyncPassthroughTagParsingEnabled = false;

    async_begun = true;
}
// #define DEBUG_RELAYED_COMMANDS 1
void processPendingLineCoding() {
 if ( s_apply_line_coding_pending) {
        // Apply to pico-sdk UART
        uint data_bits = 8;
        switch ( s_line_coding.data_bits ) {
        case 5: data_bits = 5; break;
        case 6: data_bits = 6; break;
        case 7: data_bits = 7; break;
        default: data_bits = 8; break;
        }

        uart_parity_t parity = UART_PARITY_NONE;
        if ( s_line_coding.parity == 1 ) {
            parity = UART_PARITY_ODD;
        } else if ( s_line_coding.parity == 2 ) {
            parity = UART_PARITY_EVEN;
        } else {
            parity = UART_PARITY_NONE;
        }

        uint stop_bits = 1;
        if ( s_line_coding.stop_bits == 2 ) {
            stop_bits = 2;
        } else if ( s_line_coding.stop_bits == 1 ) {
            // 1.5 not supported; approximate with 2
            stop_bits = 2;
        } else {
            stop_bits = 1;
        }
       
        uart_set_baudrate( ASYNC_PASSTHROUGH_UART, s_line_coding.bit_rate );
        uart_set_format( ASYNC_PASSTHROUGH_UART, data_bits, stop_bits, parity );
        
        serial1baud = s_line_coding.bit_rate;
        set_micros_per_byte( &s_line_coding );
        s_apply_line_coding_pending = false;

        // tud_cdc_n_write_str(0, "Applied new line coding:\n");
        // tud_cdc_n_write_str(0, "  Baud: ");
        // tud_cdc_n_write_str(0, String(s_line_coding.bit_rate).c_str());
        // tud_cdc_n_write_str(0, "\n");
        // tud_cdc_n_write_str(0, "  Data bits: ");
        // tud_cdc_n_write_str(0, String(data_bits).c_str());
        // tud_cdc_n_write_str(0, "\n");
        // tud_cdc_n_write_str(0, "  Stop bits: ");
        // tud_cdc_n_write_str(0, String(stop_bits).c_str());
        // tud_cdc_n_write_str(0, "\n");
        // tud_cdc_n_write_str(0, "  Parity: ");
        // if (parity == UART_PARITY_NONE) {
        //     tud_cdc_n_write_str(0, "None\n");
        // } else if (parity == UART_PARITY_ODD) {
        //     tud_cdc_n_write_str(0, "Odd\n");
        // } else if (parity == UART_PARITY_EVEN) {
        //     tud_cdc_n_write_str(0, "Even\n");
        // }
        // tud_cdc_n_write_flush(0);
        
        
        
        // #if DEBUG_RELAYED_COMMANDS
        // if ((t1 - t0) > 50000) {
        //     Serial.printf("⏱️  line_coding took %lu ms\n", (t1 - t0) / 1000);
        // }
        // #endif
    }
}
// Poll the host's CDC1 line coding and apply it to the UART whenever it diverges
// from what's currently set. This makes the UART baud/format ALWAYS track the
// host's serial monitor, not just when the host happens to change it while we're
// listening. Without this, a baud the host set before the passthrough went live
// (e.g. the monitor opened at 500000 before/across a Jumperless reboot) never
// reaches the UART - it stayed at the 115200 default until something read the
// host baud (printMicrosPerByte -> checkForConfigChangesUSBSer1). The event
// callback (tud_cdc_line_coding_cb) only fires on a *change*, so it misses the
// already-open case; this poll closes that gap.
static void syncUartToHostLineCoding() {
    if ( !tud_inited() ) return;
    if ( !tud_cdc_n_connected( ASYNC_PASSTHROUGH_CDC_ITF ) ) return;

    cdc_line_coding_t hc;
    tud_cdc_n_get_line_coding( ASYNC_PASSTHROUGH_CDC_ITF, &hc );
    if ( hc.bit_rate == 0 ) return;  // host hasn't set a real baud yet

    // Apply only on an actual difference (cheap compare; avoids reconfiguring
    // the UART every iteration).
    if ( hc.bit_rate != s_line_coding.bit_rate ||
         hc.data_bits != s_line_coding.data_bits ||
         hc.parity   != s_line_coding.parity ||
         hc.stop_bits != s_line_coding.stop_bits ) {
        s_line_coding = hc;
        s_apply_line_coding_pending = true;
        processPendingLineCoding();
    }
}

bool enableResync = true;  // Set to true to enable resync sequence on demand (e.g. via runtime command)

unsigned long last_usb_uart = 0;
unsigned long last_uart_usb = 0;

void task( ) {
    // DEBUG DISABLED: All checkpoint output removed to minimize USB pressure
    // The A12345678 markers were contributing to freeze by adding USB load
    static uint32_t taskCount = 0;
    taskCount++;
    
    // Checkpoints disabled for production
    bool printCheckpoints = false;  // Was: (taskCount % 1000 == 0)
    
    unsigned long taskStart = micros();
    unsigned long t0, t1;
    
    // Re-entrancy guard: task() is called from flashArduino() tight loop AND registered
    // as a CRITICAL service. Prevent re-entrant calls (e.g. via tud_task() callbacks).
    static volatile bool s_in_task = false;
    if (s_in_task) return;
    s_in_task = true;

    // Cap-emulation reset release: if checkDTRState() asserted the AVR RST pulse,
    // release it once the pulse width elapses. Runs unconditionally (even while
    // flashing) and is latency-tolerant - holding RST a little longer is harmless.
    if ( s_reset_release_at_ms && millis() >= s_reset_release_at_ms ) {
        SetArduinoResetLine( HIGH, 1 );
        s_reset_release_at_ms = 0;
        // Flush EVERY stale buffer so avrdude's stream starts at the bootloader:
        //  - the RX ring (sketch backlog + reset-glitch bytes), and
        //  - the CDC1 TX FIFO (bytes we'd already queued toward avrdude before
        //    the reset). Without the CDC flush, avrdude reads leftover sketch
        //    output ("<p>disconn...") as the sync response -> "not in sync".
        ring_clear();
        if ( tud_inited() ) tud_cdc_n_write_clear( ASYNC_PASSTHROUGH_CDC_ITF );
        s_reset_rx_suppress = false;   // resume forwarding (now clean)
    }

    // CRITICAL: Check DTR state FIRST, before any data processing.
    // checkDTRState() fires the cap-emulation reset on a DTR *rising* edge.
    // It MUST run even while flashing: the host opening the port often asserts
    // DTR once (an early rising edge), and then avrdude does its real reset by
    // deasserting and RE-asserting DTR ~50ms before it sends sync. We have to
    // catch that second rising edge - exactly like a hardware DTR cap pulses
    // RESET on every edge - or we reset too early and miss the bootloader
    // window. avrdude holds DTR asserted for the whole rest of the session
    // (only a falling edge on close, which we ignore), so this never re-resets
    // mid-flash.
    t0 = micros();
    
    if (printCheckpoints) { Serial.write('1'); yield(); }
    
    checkDTRState( USBSer1 );
    
    if (printCheckpoints) { Serial.write('2'); yield(); }
    t1 = micros();
    #if DEBUG_RELAYED_COMMANDS
    if ((t1 - t0) > 50000) {  // > 50ms
        Serial.printf("⏱️  checkDTRState took %lu ms\n", (t1 - t0) / 1000);
        Serial.flush();
    }
    #endif
    
    // =========================================================================
    // STARTUP PROTECTION: Auto-enable tag parsing after system is ready
    // =========================================================================
    // Tag parsing is DISABLED during startup to prevent crashes from Arduino
    // commands arriving before the system is fully initialized.
    // 
    // Tag parsing is enabled when BOTH conditions are met:
    //   1. s_startup_complete flag is set (signaled by main loop)
    //   2. At least STARTUP_TAG_PARSING_DELAY_MS has elapsed (safety margin)
    // =========================================================================
    if ( !asyncPassthroughTagParsingEnabled && !s_startup_complete ) {
        // Check if enough time has passed AND startup is signaled complete
        // Note: s_startup_complete should be set by main loop when initialization is done
        // But we also have a time-based fallback in case the signal is never sent
        if ( millis() > STARTUP_TAG_PARSING_DELAY_MS + 1000 ) {
            // Fallback: Enable tag parsing after 4 seconds even if not signaled
            // This ensures tag parsing eventually works even if something goes wrong
            s_startup_complete = true;
            // Also enable UART IRQ if it wasn't enabled by signalStartupComplete()
            enableUARTReceiver();
            // #if DEBUG_RELAYED_COMMANDS
            Serial.println("Tag parsing enabled (fallback timeout)");
            // #endif
        }
    }
    
    // If startup is complete and tag parsing should be enabled (based on config)
    // CRITICAL: Wait for ARDUINO_BOOT_DELAY_MS after Arduino was released from reset
    // This gives the Arduino time to boot before we start parsing its output as commands
    if ( s_startup_complete && !asyncPassthroughTagParsingEnabled && 
         jumperlessConfig.serial_1.tag_parsing > 0 &&
         s_tag_parsing_timeout_ms == 0 &&  // Not in upload/flashing mode
         s_tag_parsing_inactivity_timeout_ms == 0 &&
         s_arduino_release_time > 0 &&
         (millis() - s_arduino_release_time) >= ARDUINO_BOOT_DELAY_MS ) {
        
            if (enableResync) {
                // Perform resync on startup if enabled - helps with noisy lines and ensures clean start
                
        // CRITICAL: Disable UART RX interrupt during cleanup to prevent race conditions
        irq_set_enabled( ASYNC_PASSTHROUGH_UART_IRQ, false );
        
        // Drain UART hardware FIFO first
        uart_hw_t* hw = uart_get_hw( ASYNC_PASSTHROUGH_UART );
        while ( uart_is_readable( ASYNC_PASSTHROUGH_UART ) ) {
            (void)hw->dr;  // Discard data
        }
        
        // Clear ALL software buffers
        clearTagParserState();
        ring_clear();
        CommandBuffer::getInstance().clearPendingCommand();

        // Clear pending UART interrupt flags before re-enabling IRQ
        uart_get_hw( ASYNC_PASSTHROUGH_UART )->icr =
            UART_UARTICR_OEIC_BITS | UART_UARTICR_FEIC_BITS
          | UART_UARTICR_PEIC_BITS | UART_UARTICR_BEIC_BITS
          | UART_UARTICR_RTIC_BITS;
        
        // Re-enable UART RX interrupt
        irq_set_enabled( ASYNC_PASSTHROUGH_UART_IRQ, true );
    }
        asyncPassthroughTagParsingEnabled = true;
        #if DEBUG_RELAYED_COMMANDS
        Serial.println("Tag parsing enabled (startup complete + boot delay)");
        #endif
    }
    
    // Check if tag parsing should be re-enabled (after flashing, etc.)
    // CRITICAL: Skip while flash mode is active — checkFlashDone() handles flash exit.
    // Without this guard, the 8s absolute timeout fires mid-flash, calling
    // uart_force_receiver_resync() and ring_clear() which destroys the UART
    // state while avrdude is still communicating → garbage / desync.
    if ( !asyncPassthroughTagParsingEnabled && s_startup_complete && !s_flash_mode_active ) {
        bool should_reenable = false;
        const char* reenable_reason = nullptr;
        
        // CRITICAL: Always check Arduino boot delay before re-enabling
        // After flashing, the Arduino needs time to boot before we parse its output
        bool boot_delay_ok = (s_arduino_release_time == 0) ||  // No reset tracked (shouldn't happen)
                            (millis() - s_arduino_release_time >= ARDUINO_BOOT_DELAY_MS);
        
        if ( !boot_delay_ok ) {
            // Still waiting for Arduino to boot - don't re-enable yet
            // Keep checking on next iteration
        } else {
            // Check absolute timeout (e.g., 10 seconds after disable) - safety fallback
            if ( s_tag_parsing_timeout_ms > 0 ) {
                uint32_t elapsed = millis() - s_tag_parsing_disabled_time;
                if ( elapsed >= s_tag_parsing_timeout_ms ) {
                    should_reenable = true;
                    reenable_reason = "absolute timeout (10s safety)";
                }
            }
            
            // Check inactivity timeout (e.g., 3000ms after last USB->UART data)
            // This detects when Arduino upload has finished
            // IMPORTANT: Only check if we've seen at least SOME data first
            // This prevents premature re-enable if upload hasn't started yet
            if ( !should_reenable && s_tag_parsing_inactivity_timeout_ms > 0 && s_last_usb_to_uart_data_time > 0 ) {
                uint32_t inactivity = millis() - s_last_usb_to_uart_data_time;
                
                // Extra safety: Require minimum elapsed time since disable (1 second)
                // This ensures we don't re-enable during the initial upload handshake
                uint32_t elapsed_since_disable = millis() - s_tag_parsing_disabled_time;
                
                if ( inactivity >= s_tag_parsing_inactivity_timeout_ms && elapsed_since_disable >= 1000 ) {
                    should_reenable = true;
                    reenable_reason = "upload complete (3s idle)";
                }
            }
        }
        
        if ( should_reenable && enableResync ) {
            // CRITICAL: Perform full UART resync to handle baud rate transitions
            // After flashing, the bootloader may have used a different baud rate,
            // causing garbage when the sketch starts. This resyncs the receiver.
            uart_force_receiver_resync();
            
            // CRITICAL: Disable UART RX interrupt during cleanup to prevent race conditions
            // Without this, the IRQ could fill the ring buffer between our clear operations
            irq_set_enabled( ASYNC_PASSTHROUGH_UART_IRQ, false );
            
            // Drain UART hardware FIFO again after resync
            uart_hw_t* hw = uart_get_hw( ASYNC_PASSTHROUGH_UART );
            while ( uart_is_readable( ASYNC_PASSTHROUGH_UART ) ) {
                (void)hw->dr;  // Discard data
            }
            
            // Now clear all software buffers
            clearTagParserState();
            ring_clear();  // Clear UART RX ring buffer
            CommandBuffer::getInstance().clearPendingCommand();  // Clear any pending commands

            // Clear pending UART interrupt flags before re-enabling IRQ
            hw->icr = UART_UARTICR_OEIC_BITS | UART_UARTICR_FEIC_BITS
                    | UART_UARTICR_PEIC_BITS | UART_UARTICR_BEIC_BITS
                    | UART_UARTICR_RTIC_BITS;
            
            // Re-enable UART RX interrupt
            irq_set_enabled( ASYNC_PASSTHROUGH_UART_IRQ, true );
            
            asyncPassthroughTagParsingEnabled = true;
            s_tag_parsing_timeout_ms = 0;  // Clear timeouts
            s_tag_parsing_inactivity_timeout_ms = 0;
            s_last_usb_to_uart_data_time = 0;
            s_tag_parsing_reenable_time = millis();  // Track re-enable time for cooldown
            
            // Single clean message when re-enabling (no flush - too slow!)
            #if DEBUG_RELAYED_COMMANDS
            Serial.printf("✓ Tag parsing re-enabled: %s\n", reenable_reason);
            #endif
        }
    }
    
    if (printCheckpoints) { Serial.write('3'); yield(); }

    // Handle deferred UART receiver resync (requested by ISR via flag).
    // This runs in main context where busy_wait_us(100) is harmless,
    // unlike doing it from the priority-1 ISR where it starves USB.
    if ( s_resync_requested ) {
        s_resync_requested = false;
        uart_force_receiver_resync();
    }

    // If suspended by MicroPython, avoid touching UART hardware (and don't
    // fire the cap-emulation reset - MP owns the UART, no flash is happening).
    if ( s_uart_suspended_by_mpy ) {
        TinyUSB_Device_Task(); // mutex-guarded pump
        return;
    }
    
    if (printCheckpoints) { Serial.write('4'); yield(); }
    
    // Apply pending line coding from host, and keep the UART baud/format in sync
    // with the host's CDC line coding (handles the "monitor already open at a
    // non-default baud" case the event callback misses). Skip during flashing -
    // avrdude's line coding is handled via the callback and we don't want to
    // reconfigure the UART mid-session.
    t0 = micros();
    processPendingLineCoding();
    if ( !flashingArduino ) {
        syncUartToHostLineCoding();
    }

    if (printCheckpoints) { Serial.write('5'); yield(); }
    
    // USB -> UART when either pending flag set or data available
    t0 = micros();
    if ( s_usb_rx_pending || ( tud_inited( ) && tud_cdc_n_available( ASYNC_PASSTHROUGH_CDC_ITF ) ) ) {
        if (gpioReadingColors[8] != 0x1b0700) {
         gpioReadingColors[8] = 0x1b0700;
         requestLedShow( 2 );
        }
         last_usb_uart = millis();
        bridge_usb_to_uart( ASYNC_PASSTHROUGH_CDC_ITF );
        s_usb_rx_pending = false;
        
    } else if (millis() - last_usb_uart > 60) {
        // No USB->UART activity for a while - reset color
        if (gpioReadingColors[8] != 0x001b07) {
        gpioReadingColors[8] = 0x001b07;
        requestLedShow( 2 );
        }
    }
    
    if (printCheckpoints) { Serial.write('6'); yield(); }
    t1 = micros();
    #if DEBUG_RELAYED_COMMANDS
    if ((t1 - t0) > 50000) {
        Serial.printf("⏱️  bridge_usb_to_uart took %lu ms\n", (t1 - t0) / 1000);
    }
    #endif
    
    // UART -> USB (where tag parsing and command relay happen)
    t0 = micros();
    if ( ring_available() > 0 ) {
            if (gpioReadingColors[9] != 0x1b0700) {
         gpioReadingColors[9] = 0x1b0700;
         
         requestLedShow( 2 );
            }
last_uart_usb = millis();
    bridge_uart_to_usb( ASYNC_PASSTHROUGH_CDC_ITF );
    } else if (millis() - last_uart_usb > 60) {
        // No UART->USB activity for a while - reset color
        if (gpioReadingColors[9] != 0x001b07) {
        gpioReadingColors[9] = 0x001b07;
        requestLedShow( 2 );
        }
    }
    t1 = micros();
    
    if (printCheckpoints) { Serial.write('7'); yield(); }  // After UART->USB bridge
    
    #if DEBUG_RELAYED_COMMANDS
    if ((t1 - t0) > 50000) {
        Serial.printf("⏱️  bridge_uart_to_usb took %lu ms\n", (t1 - t0) / 1000);
    }
    #endif

    // Check if uartReceived starts with any forward prefix and route to main Serial
    t0 = micros();
    process_uart_forward_prefixes();
    t1 = micros();
    #if DEBUG_RELAYED_COMMANDS
    if ((t1 - t0) > 50000) {
        Serial.printf("⏱️  process_uart_forward_prefixes took %lu ms\n", (t1 - t0) / 1000);
    }
    #endif

    // Service USB stack regardless to minimize latency and prevent CDC TX stalling
    // (mutex-guarded: the Adafruit port also pumps tud_task() from its USB IRQ, so a
    // raw call here could re-enter the stack. yield() also flushes every CDC port.)
    t0 = micros();
    yield();
    t1 = micros();
    #if DEBUG_RELAYED_COMMANDS
    if ((t1 - t0) > 50000) {
        Serial.printf("⏱️  tud_task took %lu ms\n", (t1 - t0) / 1000);
    }
    #endif
    
    // Send any pending UART responses (responses to commands from Arduino)
    sendPendingUARTResponses();

    // CRITICAL: pump the TX DMA every iteration, unconditionally. The pump is
    // event-driven elsewhere (only on new USB data / queued responses), but a
    // single avrdude command (e.g. a 128-byte flash page) is read in 64-byte
    // chunks: the first chunk kicks the DMA and the rest queue behind it. Since
    // avrdude then WAITS for the bootloader's OK before sending more, no new
    // event arrives to drain that remainder - the bootloader gets a partial
    // command, never replies, and avrdude reports "programmer is not
    // responding" mid-write. Draining here guarantees the staging ring always
    // empties to the UART. Cheap: returns immediately if the DMA is busy or the
    // ring is empty.
    tx_dma_pump();

    if (printCheckpoints) { Serial.write('8'); Serial.write('\n'); }  // Task completed
    
    s_in_task = false;  // Release re-entrancy guard
    
    unsigned long taskEnd = micros();
    #if DEBUG_RELAYED_COMMANDS
    if ((taskEnd - taskStart) > 100000) {  // > 100ms total
        Serial.printf("⏱️  AsyncPassthrough::task() TOTAL: %lu ms\n", (taskEnd - taskStart) / 1000);
    }
    #endif
}
void disableTagParsingWithInactivityTimeout( uint32_t absolute_timeout_ms, uint32_t inactivity_timeout_ms ) {
    // If already disabled with an inactivity timeout active, REFRESH the timeouts
    // Previous code returned early here, preserving stale timers from the first DTR pulse.
    // This caused tag parsing to re-enable mid-flash (absolute timeout expired based on
    // first pulse time), which triggered uart_force_receiver_resync() + ring_clear(),
    // destroying pending bootloader responses and causing avrdude to see 0xff.
    if ( !asyncPassthroughTagParsingEnabled && s_tag_parsing_inactivity_timeout_ms > 0 ) {
        // Already disabled - just refresh timers so they restart from NOW
        s_tag_parsing_disabled_time = millis();
        s_tag_parsing_timeout_ms = absolute_timeout_ms;
        s_tag_parsing_inactivity_timeout_ms = inactivity_timeout_ms;
        s_last_usb_to_uart_data_time = millis();  // Reset inactivity tracker too
        return;
    }
    
    asyncPassthroughTagParsingEnabled = false;
    s_tag_parsing_disabled_time = millis();
    s_tag_parsing_timeout_ms = absolute_timeout_ms;
    s_tag_parsing_inactivity_timeout_ms = inactivity_timeout_ms;
    s_last_usb_to_uart_data_time = 0;  // Will be set when first data arrives
    
    // Clear parser state when disabling tag parsing
    // This prevents garbage from flashing/binary data from being processed as commands
    // when tag parsing is re-enabled
    // NOTE: Do NOT call ring_clear() here - the ring buffer may contain bootloader
    // responses that bridge_uart_to_usb() needs to forward to avrdude.
    clearTagParserState();
    // ring_clear();  // DO NOT CLEAR - bootloader responses live here
    CommandBuffer::getInstance().clearPendingCommand();
}

bool getTagParsingEnabled() {
    return asyncPassthroughTagParsingEnabled;
}

void signalStartupComplete() {
    // Called by main loop when system initialization is complete
    // This enables tag parsing if configured to do so
    if ( !s_startup_complete ) {
        s_startup_complete = true;

            if ( jumperlessConfig.serial_1.async_passthrough == true ) {
        AsyncPassthrough::begin( 115200 );
    }
    initArduino( );

        // CRITICAL: Enable UART IRQ now that boot is complete and task() is draining.
        // This was deferred from begin() to prevent the priority-1 ISR from
        // starving USB interrupts and main code during boot initialization.
        // enableUARTReceiver() drains the HW FIFO, clears errors/buffers,
        // then enables the interrupt mask and NVIC IRQ.
        enableUARTReceiver();

        // CRITICAL: Release Arduino from reset now that system is ready
        // Arduino was held in reset during boot (see main.cpp setup())
        // This prevents crashes from early Arduino commands
        SetArduinoResetLine(HIGH, 2);  // Release both Arduinos from reset
        s_arduino_release_time = millis();  // Track when we released Arduino

        // NOTE: Don't enable tag parsing here - let it happen via the normal
        // task() path after ARDUINO_BOOT_DELAY_MS. This gives the Arduino time to boot.
        // During this delay, UART data is just passed through (not parsed for tags).

        #if DEBUG_RELAYED_COMMANDS
        Serial.println("Startup complete - UART IRQ enabled, Arduino released from reset");
        #endif
    }
}

bool isStartupComplete() {
    return s_startup_complete;
}

// ============================================================================
// DTR Pulse Detection and Arduino Reset
// ============================================================================

// DTR state tracking. The reset is cap-emulation: a DTR rising edge pulses the
// Arduino RESET line, exactly like a real Arduino's DTR-through-a-capacitor
// auto-reset circuit. This now happens IMMEDIATELY in checkDTRState() (which
// runs every task() iteration as part of the CRITICAL passthrough service),
// instead of being deferred to a blocking flashArduino() servicing loop.
static bool s_dtr_state[3] = { false, false, false };
static bool s_rts_prev = false;
static bool s_dtr_pulse_detected = false;
static uint32_t s_last_dtr_reset_time = 0;

// Set to 1 to log DTR/RTS line transitions and reset triggers on Serial (CDC0 /
// Main). Use this to confirm whether the host's avrdude actually drives a clean
// reset edge to the device (e.g. avrdude 8.0 on Windows). Off in normal builds.
#define DEBUG_DTR_RESET 0
// Debounce between accepted rising edges. Small enough that avrdude's
// open-assert and its later real reset-assert (≈250ms apart on avrdude 6.3,
// less on newer versions) both get through, but large enough to swallow
// contact-bounce-style double edges.
#define DTR_MIN_RESET_INTERVAL_MS 40

// Post-flash lockout: suppress reset detection briefly after a flash completes
// so the host's port-close DTR toggle can't immediately re-trigger a reset.
static uint32_t s_dtr_lockout_until = 0;

// (s_reset_release_at_ms is declared near the DMA globals so task() can see it.)

// Enter "dumb pipe" flash mode and fire the cap-emulation reset. Lightweight:
// flips flags, clears the parser, and (for AVR) asserts RST now + schedules the
// release in task(). The heavy crossbar auto-connect is done once by
// secondSerialHandler()/flashArduino() via the s_dtr_pulse_detected flag.
static void triggerArduinoFlashReset() {
    // Stop the running sketch's <j>/<p> command flood from hogging Core 0 and
    // turn the bridge into a transparent pipe for avrdude RIGHT NOW.
    flashingArduino = true;
    asyncPassthroughTagParsingEnabled = false;
    disableTagParsingWithInactivityTimeout( 8000, 2000 );
    enterFlashMode();
    s_dtr_pulse_detected = true;   // secondSerialHandler() does the one-time UART auto-connect

    int rt = jumperlessConfig.serial_1.flash_reset_type;
    if ( rt == 1 ) {
        // AVR: assert RST on the real RST net (GPIO 18 / selector 1 - works with
        // PSRAM installed). Released ~12ms later by the task() timer. Suppress
        // UART->USB forwarding for the whole pulse and drop the current RX
        // backlog AND the already-queued CDC1 TX bytes now, so the sketch's
        // last bytes can't leak to avrdude (a second flush happens at release).
        s_reset_rx_suppress = true;
        ring_clear();
        if ( tud_inited() ) tud_cdc_n_write_clear( ASYNC_PASSTHROUGH_CDC_ITF );
        SetArduinoResetLine( LOW, 1 );
        s_reset_release_at_ms = millis() + 12;
    }
    // rt == 2 (ESP32): the blocking B1+RST dance is done by flashArduino() on the
    //   main loop (it can't be a simple GPIO pulse here).
    // rt == 0: no reset, just a transparent pipe.
}

void checkDTRState(Adafruit_USBD_CDC& cdc) {
    bool current_dtr = cdc.dtr();
    // Adafruit_USBD_CDC exposes no rts() accessor, so read the raw CDC line-state
    // bit (bit 1 = RTS). checkDTRState is only ever called with USBSer1, the
    // passthrough CDC interface, so the fixed itf is correct here.
    bool current_rts = ( tud_cdc_n_get_line_state( ASYNC_PASSTHROUGH_CDC_ITF ) & 0x02 ) != 0;
    bool prev_dtr = s_dtr_state[2];
    bool prev_rts = s_rts_prev;

    // Maintain the small history (used by some diagnostics) and detect the edge.
    s_dtr_state[0] = s_dtr_state[1];
    s_dtr_state[1] = s_dtr_state[2];
    s_dtr_state[2] = current_dtr;
    s_rts_prev = current_rts;

#if DEBUG_DTR_RESET == 1
    if ( current_dtr != prev_dtr || current_rts != prev_rts ) {
        Serial.printf( "[DTR] dtr %d->%d  rts %d->%d  @%lu\n",
                       prev_dtr, current_dtr, prev_rts, current_rts, millis() );
        Serial.flush();
    }
#endif

    // Cap emulation: a rising edge on DTR (or RTS) resets - like the series
    // capacitor passing the falling edge of DTR#/RTS# into the RESET pin. We
    // watch BOTH because avrdude's "arduino" programmer toggles DTR *and* RTS to
    // reset, and on some host/driver/avrdude combos (notably avrdude 8.0 on
    // Windows) only one of them survives as a clean USB control-line edge that we
    // can poll. Either rising edge triggers the reset; the debounce below absorbs
    // the near-simultaneous second line.
    bool rising = ( prev_dtr == false && current_dtr == true ) ||
                  ( prev_rts == false && current_rts == true );
    if ( !rising ) return;

    uint32_t now = millis();
    if ( now < 4000 ) return;                                        // ignore boot-time toggles
    if ( s_dtr_lockout_until && now < s_dtr_lockout_until ) return;  // post-flash lockout
    if ( now - s_last_dtr_reset_time < DTR_MIN_RESET_INTERVAL_MS ) return;
    s_last_dtr_reset_time = now;

#if DEBUG_DTR_RESET == 1
    Serial.printf( "[DTR] -> cap-emulation reset @%lu\n", now );
    Serial.flush();
#endif

    triggerArduinoFlashReset();
}

// Set DTR lockout — called after flash completes to suppress port-close DTR
void setDTRLockout(uint32_t duration_ms) {
    s_dtr_lockout_until = millis() + duration_ms;
}

bool wasDTRPulseDetected() {
    return s_dtr_pulse_detected;
}

void clearDTRPulse() {
    s_dtr_pulse_detected = false;
}

// ============================================================================
// Flash Completion Detection (STK500 protocol watching + inactivity fallback)
// ============================================================================

void enterFlashMode() {
    s_flash_mode_active      = true;
    s_stk_leave_seen         = false;
    s_stk_leave_time         = 0;
    s_flash_start_time       = millis();
    s_any_flash_data         = false;
    s_stk_prev_byte          = 0;
    s_last_usb_to_uart_data_time = millis();
}

void exitFlashMode() {
    s_flash_mode_active  = false;
    s_stk_leave_seen     = false;
    s_any_flash_data     = false;
    s_stk_prev_byte      = 0;
}

void resetFlashSTKDetection() {
    // Reset only the STK500 byte scanner for a new bootloader session.
    // Does NOT reset timestamps or s_any_flash_data — those must survive
    // across DTR re-resets so inactivity/hard timeout still work correctly.
    s_stk_leave_seen = false;
    s_stk_prev_byte  = 0;
}

bool hasFlashDataBeenSeen() {
    return s_any_flash_data;
}

bool checkFlashDone() {
    if ( !s_flash_mode_active ) return false;

    uint32_t now = millis();

    // Priority 1: STK_LEAVE_PROGMODE was detected
    if ( s_stk_leave_seen ) {
        // If new USB→UART data arrived AFTER detection, it was a false positive
        // (binary flash data that happened to contain 0x51 0x20)
        if ( s_last_usb_to_uart_data_time > s_stk_leave_time ) {
            // Reset detection and keep looking
            s_stk_leave_seen = false;
            s_stk_prev_byte  = 0;
        } else if ( now - s_stk_leave_time >= FLASH_STK_GRACE_MS ) {
            // Grace period elapsed, no new data — flash is truly done
            return true;
        }
    }

    // Priority 2: Inactivity fallback (only if some data was transferred)
    if ( s_any_flash_data && ( now - s_last_usb_to_uart_data_time >= FLASH_INACTIVITY_MS ) ) {
        return true;
    }

    // Priority 3: No-data abort. If avrdude never actually sent anything after
    // we entered flash mode, the DTR pulse was spurious (port-open toggle, an
    // App port-accessibility probe, a serial monitor connecting, etc.). Bail
    // quickly instead of holding the main loop hostage for the full hard
    // timeout — this is the "hang when the flash doesn't finish" case.
    if ( !s_any_flash_data && ( now - s_flash_start_time >= FLASH_NO_DATA_TIMEOUT_MS ) ) {
        return true;
    }

    // Priority 4: Hard safety timeout
    if ( now - s_flash_start_time >= FLASH_HARD_TIMEOUT_MS ) {
        return true;
    }

    return false;
}

void resetArduino(int resetPin) {
    // Pull reset line low for 3ms, then high
    pinMode(resetPin, OUTPUT);
    digitalWrite(resetPin, LOW);
    delay(5);
    digitalWrite(resetPin, HIGH);
    pinMode(resetPin, INPUT);
}

// ============================================================================
// UART IRQ Control and Tag Parser State Management
// ============================================================================

// IRQ suspension state
static volatile bool s_uart_rx_irq_suspended = false;

// Command processing state (prevents new commands during execution)
static volatile bool s_command_processing_active = false;

// In the DMA model these are effectively no-ops. The old IRQ-based RX needed to
// be suspended during flash-erase / filesystem critical sections (the priority-1
// ISR could starve USB, or race on shared state). DMA RX has neither problem: it
// fills a RAM ring with zero CPU and keeps working even during XIP stalls - which
// is strictly BETTER (it's exactly the data loss the IRQ model suffered). Kept
// for ABI: many callers (FileCache, FilesystemStuff, configManager, the MP API)
// wrap operations in suspend/resume pairs.
void suspendUARTRxIRQ() {
    s_uart_rx_irq_suspended = true;
}

void resumeUARTRxIRQ() {
    s_uart_rx_irq_suspended = false;
}

bool isUARTRxIRQSuspended() {
    return s_uart_rx_irq_suspended;
}

void drainUARTRxBuffers() {
    // Discard everything currently buffered. With DMA there's no IRQ to disable
    // and no HW-FIFO/ring to walk byte-by-byte: the DMA continuously empties the
    // RX FIFO into the ring, so we just snap the consume index up to the live DMA
    // write pointer (ring_clear) and reset the TX staging ring. Used after an
    // Arduino reset to drop the phantom bytes the tri-stated TX pin produces.
    ring_clear();
    tx_ring_clear();
    s_uart_rx_irq_suspended = false;
}

void clearTagParserState() {
    // Reset BOTH parser state machines to clean state
    // CRITICAL: Called on DTR pulse to ensure no partial tags corrupt flashing
    
    // Reset USB->UART parser
    usb_to_uart_parser.state = TAG_SEARCHING;
    usb_to_uart_parser.tag_buffer_idx = 0;
    usb_to_uart_parser.current_tag[0] = '\0';
    usb_to_uart_parser.command_buffer_idx = 0;
    usb_to_uart_parser.needs_python_prefix = false;
    usb_to_uart_parser.seen_first_char = false;
    usb_to_uart_parser.chars_in_state = 0;
    
    // Reset UART->USB parser
    uart_to_usb_parser.state = TAG_SEARCHING;
    uart_to_usb_parser.tag_buffer_idx = 0;
    uart_to_usb_parser.current_tag[0] = '\0';
    uart_to_usb_parser.command_buffer_idx = 0;
    uart_to_usb_parser.needs_python_prefix = false;
    uart_to_usb_parser.seen_first_char = false;
    uart_to_usb_parser.chars_in_state = 0;
    
    // Clear the runtime command ring buffer too
    memset(s_runtime_cmd_ring, 0, sizeof(s_runtime_cmd_ring));
    s_runtime_cmd_write_idx = 0;
}

void setCommandProcessingActive(bool active) {
    s_command_processing_active = active;
}

bool isCommandProcessingActive() {
    return s_command_processing_active;
}

// ============================================================================
// UART Response Functions - Send command responses back to Arduino
// ============================================================================

bool queueUARTResponse(const char* data, size_t len) {
    if (!data || len == 0) return false;
    if (len > UART_RESPONSE_MAX_LEN - 1) len = UART_RESPONSE_MAX_LEN - 1;
    
    if (s_uart_response_count >= UART_RESPONSE_QUEUE_SIZE) {
        // Queue full - drop oldest
        s_uart_response_tail = (s_uart_response_tail + 1) % UART_RESPONSE_QUEUE_SIZE;
        s_uart_response_count--;
    }
    
    UARTResponseEntry* entry = &s_uart_response_queue[s_uart_response_head];
    memcpy(entry->data, data, len);
    entry->data[len] = '\0';
    entry->length = len;
    entry->read_offset = 0;
    entry->pending = true;
    
    s_uart_response_head = (s_uart_response_head + 1) % UART_RESPONSE_QUEUE_SIZE;
    s_uart_response_count++;
    
    return true;
}

bool queueUARTResponse(const String& data) {
    return queueUARTResponse(data.c_str(), data.length());
}

void sendPendingUARTResponses() {
    // Push command responses into the TX DMA staging ring (zero-CPU drain).
    // Bounded per call so a large response can't monopolise the staging ring;
    // the remainder goes out on subsequent calls. tx_ring_push_byte drops if
    // the ring is momentarily full (rare for these small responses).
    const uint32_t MAX_BYTES_PER_CALL = 64;
    uint32_t bytesSent = 0;
    bool pushed = false;

    // First, drain the legacy queue (for backwards compatibility)
    while (s_uart_response_count > 0 && bytesSent < MAX_BYTES_PER_CALL) {
        UARTResponseEntry* entry = &s_uart_response_queue[s_uart_response_tail];
        if (entry->pending && entry->read_offset < entry->length) {
            while (entry->read_offset < entry->length && bytesSent < MAX_BYTES_PER_CALL) {
                if (!tx_ring_push_byte(entry->data[entry->read_offset])) {
                    // Staging ring momentarily full - resume next call.
                    if (pushed) tx_dma_pump();
                    return;
                }
                entry->read_offset++;
                bytesSent++;
                pushed = true;
            }
            if (entry->read_offset >= entry->length) {
                entry->pending = false;
            }
        }
        if (!entry->pending) {
            s_uart_response_tail = (s_uart_response_tail + 1) % UART_RESPONSE_QUEUE_SIZE;
            s_uart_response_count--;
        }
    }

    // Second, drain the CommandBuffer outgoing buffer (new system)
    CommandBuffer& cb = CommandBuffer::getInstance();
    while (cb.hasOutgoingData() && bytesSent < MAX_BYTES_PER_CALL) {
        int byte = cb.peekOutgoingByte();
        if (byte < 0) break;
        if (!tx_ring_push_byte((uint8_t)byte)) {
            // Staging ring full - leave the byte queued, resume next call.
            break;
        }
        cb.getOutgoingByte();  // consume the peeked byte
        bytesSent++;
        pushed = true;
    }

    if (pushed) tx_dma_pump();
}

bool wasCommandFromUART() {
    return s_command_from_uart;
}

void clearCommandFromUARTFlag() {
    s_command_from_uart = false;
}

// ============================================================================
// UART Framing Error Detection and Resync - Public API
// ============================================================================

void getUARTErrorStats(uint32_t* framing_errors, uint32_t* overruns, uint32_t* resyncs) {
    if (framing_errors) *framing_errors = s_uart_framing_error_total;
    if (overruns) *overruns = s_uart_overrun_count;
    if (resyncs) *resyncs = s_uart_resync_count;
}

void resetUARTErrorStats() {
    s_uart_framing_error_total = 0;
    s_uart_framing_error_count = 0;
    s_uart_overrun_count = 0;
    s_uart_resync_count = 0;
    s_uart_garbage_count = 0;
    s_uart_garbage_total = 0;
    s_uart_garbage_resyncs = 0;
}

void forceUARTResync() {
    // Call the internal resync function
    //if (flashingArduino) return;  // Don't resync during flashing - it will cause upload failures
    uart_force_receiver_resync();
}

void printUARTErrorStats() {
    Serial.printf("UART Error Stats:\n");
    Serial.printf("  Framing errors: %lu (consecutive: %lu)\n", 
                  s_uart_framing_error_total, s_uart_framing_error_count);
    Serial.printf("  Overrun errors: %lu\n", s_uart_overrun_count);
    Serial.printf("  Receiver resyncs: %lu\n", s_uart_resync_count);
    Serial.printf("  Ring buffer overflow: %lu\n", uartReceivedOverflowCount);
}

void sendBreakToRemote(uint32_t break_duration_us) {
    uart_hw_t* hw = uart_get_hw(ASYNC_PASSTHROUGH_UART);
    
    // Wait for TX FIFO to drain before sending break
    // This ensures we don't corrupt any pending data
    while (!(hw->fr & UART_UARTFR_TXFE_BITS)) {
        // TX FIFO not empty, wait
        tight_loop_contents();
    }
    
    // Set BRK bit in Line Control Register to force TX LOW
    // This holds the TX line in the "space" (LOW) state
    hw_set_bits(&hw->lcr_h, UART_UARTLCR_H_BRK_BITS);
    
    // Hold break for specified duration
    // At 115200 baud, one bit time is ~8.7µs
    // A frame is ~10-11 bits, so ~87-96µs minimum for a valid break
    // We default to 100µs which gives a bit of margin
    busy_wait_us(break_duration_us);
    
    // Clear BRK bit to return TX to normal (idle HIGH)
    hw_clear_bits(&hw->lcr_h, UART_UARTLCR_H_BRK_BITS);
    
    // Small delay to let the remote receiver stabilize
    busy_wait_us(10);
}

void fullBidirectionalResync() {
    // Step 1: Resync our local receiver
    forceUARTResync();
    
    // Step 2: Send break to force remote receiver to resync
    // Use longer break (200µs) for better reliability
    sendBreakToRemote(200);
    
    // Step 3: Brief pause to let both sides stabilize
    busy_wait_us(50);
    
    Serial.println("Full bidirectional UART resync completed");
}

// ============================================================================
// Idle Line Detection and Timing Validation - Public API
// ============================================================================

bool isLineIdle() {
    if (s_last_rx_byte_time_us == 0) return true;  // Never received = idle
    uint32_t now = time_us_32();
    uint32_t elapsed = now - s_last_rx_byte_time_us;
    return elapsed > UART_IDLE_THRESHOLD_US;
}

uint32_t getTimeSinceLastRxUs() {
    if (s_last_rx_byte_time_us == 0) return UINT32_MAX;
    return time_us_32() - s_last_rx_byte_time_us;
}

void getTimingStats(uint32_t* idle_periods, uint32_t* bytes_since_idle, 
                    uint32_t* timing_anomalies, uint32_t* last_inter_byte_us) {
    if (idle_periods) *idle_periods = s_idle_periods_detected;
    if (bytes_since_idle) *bytes_since_idle = s_bytes_since_last_idle;
    if (timing_anomalies) *timing_anomalies = s_timing_anomaly_count;
    if (last_inter_byte_us) *last_inter_byte_us = s_last_inter_byte_time_us;
}

void resetTimingStats() {
    s_idle_periods_detected = 0;
    s_bytes_since_last_idle = 0;
    s_timing_anomaly_count = 0;
    s_last_inter_byte_time_us = 0;
    s_last_rx_byte_time_us = 0;
    s_line_was_idle = true;
}

void printFullDiagnostics() {
    Serial.println("=== UART Full Diagnostics ===");
    
    // Error stats
    Serial.println("Error Statistics:");
    Serial.printf("  Framing errors: %lu (consecutive: %lu)\n", 
                  s_uart_framing_error_total, s_uart_framing_error_count);
    Serial.printf("  Garbage bytes: %lu (consecutive: %lu)\n",
                  s_uart_garbage_total, s_uart_garbage_count);
    Serial.printf("  Overrun errors: %lu\n", s_uart_overrun_count);
    Serial.printf("  Receiver resyncs: %lu (garbage-triggered: %lu)\n", 
                  s_uart_resync_count, s_uart_garbage_resyncs);
    Serial.printf("  Ring buffer overflow: %lu\n", uartReceivedOverflowCount);
    
    // Timing stats
    Serial.println("Timing Statistics:");
    Serial.printf("  Idle periods detected: %lu\n", s_idle_periods_detected);
    Serial.printf("  Bytes since last idle: %lu\n", s_bytes_since_last_idle);
    Serial.printf("  Timing anomalies: %lu\n", s_timing_anomaly_count);
    Serial.printf("  Last inter-byte time: %lu us\n", s_last_inter_byte_time_us);
    Serial.printf("  Expected byte time: %lu us\n", usPerByteSerial1);
    
    // Current state
    Serial.println("Current State:");
    Serial.printf("  Line idle: %s\n", isLineIdle() ? "YES" : "NO");
    Serial.printf("  Time since last RX: %lu us\n", getTimeSinceLastRxUs());
    Serial.printf("  Baud rate: %lu\n", serial1baud);
    Serial.printf("  Ring buffer: %u bytes pending\n", ring_available());
    
    Serial.println("=============================");
}

bool waitForLineIdle(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        if (isLineIdle()) {
            return true;
        }
        delayMicroseconds(10);
    }
    return false;
}

} // namespace AsyncPassthrough

// ----------------------------------------------------------------------------
// Interop hooks for MicroPython ownership of UART0
// ----------------------------------------------------------------------------

// Track suspend state to avoid redundant reconfiguration
// (defined above to gate task() too)

extern "C" void jl_asyncpassthrough_suspend_uart0( void ) {
    if ( s_uart_suspended_by_mpy ) return;
    // Hand UART0 to MicroPython: stop the DMA bridge and drop the UART's DMA
    // request lines so MP's own driver isn't disturbed. task() early-returns
    // while s_uart_suspended_by_mpy is set, so the consumer won't touch the ring.
    if ( s_rx_dma_chan >= 0 && dma_channel_is_busy( s_rx_dma_chan ) ) dma_channel_abort( s_rx_dma_chan );
    if ( s_tx_dma_chan >= 0 && dma_channel_is_busy( s_tx_dma_chan ) ) dma_channel_abort( s_tx_dma_chan );
    s_tx_dma_inflight = 0;
    uart_get_hw( ASYNC_PASSTHROUGH_UART )->dmacr = 0;
    s_uart_suspended_by_mpy = true;
}

extern "C" void jl_asyncpassthrough_resume_uart0( void ) {
    if ( !s_uart_suspended_by_mpy ) return;
    // Re-take UART0 for the DMA bridge after MicroPython is done with it.
    gpio_set_function( ASYNC_PASSTHROUGH_UART_TX_PIN, GPIO_FUNC_UART );
    gpio_set_function( ASYNC_PASSTHROUGH_UART_RX_PIN, GPIO_FUNC_UART );
    uart_set_format( ASYNC_PASSTHROUGH_UART, 8, 1, UART_PARITY_NONE );
    uart_set_fifo_enabled( ASYNC_PASSTHROUGH_UART, true );
    uart_get_hw( ASYNC_PASSTHROUGH_UART )->dmacr =
        UART_UARTDMACR_TXDMAE_BITS | UART_UARTDMACR_RXDMAE_BITS;
    setupRxDma();   // re-arm continuous RX ring
    setupTxDma();   // re-arm TX channel
    s_uart_suspended_by_mpy = false;
}

// Expose an override for MicroPython to update UART0 line coding
extern "C" void jl_asyncpassthrough_override_line_coding( uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits ) {
    // Map to pico-sdk settings and apply immediately

    // Serial.println("jl_asyncpassthrough_override_line_coding");
    // Serial.println(baud);
    // Serial.println(data_bits);
    // Serial.println(parity);
    // Serial.println(stop_bits);

    uint d = ( data_bits < 5 ? 5 : ( data_bits > 8 ? 8 : data_bits ) );
    uart_parity_t p = UART_PARITY_NONE;
    if ( parity == 1 ) p = UART_PARITY_ODD; else if ( parity == 2 ) p = UART_PARITY_EVEN;
    uint s = ( stop_bits >= 2 ? 2 : 1 );

    if ( baud > 0 ) {
        uart_set_baudrate( ASYNC_PASSTHROUGH_UART, baud );
    }
    uart_set_format( ASYNC_PASSTHROUGH_UART, d, s, p );

    // Keep tracking vars in sync (CDC semantics: stop_bits 0=1,1=1.5,2=2)
    if ( baud > 0 ) {
        s_line_coding.bit_rate = baud;
        serial1baud = baud;
    }
    s_line_coding.data_bits = d;
    s_line_coding.parity = ( parity == 1 ? 1 : ( parity == 2 ? 2 : 0 ) );
    s_line_coding.stop_bits = ( s == 1 ? 0 : 2 );
    set_micros_per_byte( &s_line_coding );
    s_line_coding_override = true;
}

#endif // ASYNC_PASSTHROUGH_ENABLED

