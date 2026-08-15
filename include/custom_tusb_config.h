/*
    The MIT License (MIT)

    Copyright (c) 2018, hathach for Adafruit

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/



#ifndef _TUSB_CONFIG_RP2040_H_
#define _TUSB_CONFIG_RP2040_H_

#include "usb_interface_config.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------
// CRITICAL: Enable high-speed mode for RP2350B
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)

// Enable device stack
#define CFG_TUD_ENABLED 1

// Enable host stack with pio-usb if Pico-PIO-USB library is available
#if __has_include("pio_usb.h")
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif
#define CFG_TUSB_OS OPT_OS_PICO

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 1
#endif

// For selectively disable device log (when > CFG_TUSB_DEBUG)
// #define CFG_TUD_LOG_LEVEL 3
// #define CFG_TUH_LOG_LEVEL 3

// #define CFG_TUSB_MEM_SECTION
// #define CFG_TUSB_MEM_ALIGN TU_ATTR_ALIGNED(16)  // Increased from 4 for better DMA performance

// // STREAMING OPTIMIZATIONS: Enable double packet buffering for better throughput
// #define CFG_TUD_BULK_DOUBLE_PACKET_BUFFERING 0

//--------------------------------------------------------------------
// Device Configuration
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE 64

// CRITICAL: Disable auto-generated descriptors FIRST
#define CFG_TUD_DESC_AUTO 0

#define CFG_TUD_CDC USB_CDC_ENABLE_COUNT

// CRITICAL: Fix a core-0 live-lock (SWD-confirmed backtrace, Jul 2026).
// With this Adafruit default ON (1), every host DTR toggle makes
// cdcd_control_xfer_cb (running in TinyUSB's soft-IRQ context via
// usb_task_irq) call tu_fifo_set_overwritable(), which takes the CDC TX
// FIFO's write mutex with OSAL_TIMEOUT_WAIT_FOREVER. If the IRQ lands while
// thread-context code is inside tud_cdc_n_write/ff_push_n HOLDING that same
// mutex (i.e. any print racing a host connect/disconnect), the ISR blocks
// forever on a mutex owned by the thread it preempted -> core 0 spins
// inside the IRQ, USB dies, board wedges until power cycle. This was the
// "board dies when the browser/IDE opens or drops the port mid-print" bug.
// Setting 0 makes the overwritable flag constant (init false, set false), so
// tu_fifo_set_overwritable early-returns and never locks in ISR context.
// Cost: TX FIFO no longer overwrites stale data while disconnected — writes
// to a disconnected port just fill the FIFO and drop, which is fine here.
#define CFG_TUD_CDC_TX_OVERWRITABLE_IF_NOT_CONNECTED 0

// Enable multiple CDC descriptors - OPTIMIZED FOR STREAMING
#define CFG_TUD_CDC_EP_BUFSIZE 64   // Match USB 2.0 bulk packet size for efficiency

#define CFG_TUD_MSC USB_MSC_ENABLE
#define CFG_TUD_MIDI USB_MIDI_ENABLE
#define CFG_TUD_VENDOR USB_VENDOR_ENABLE

// -------- USB Audio Class 2.0 microphone (2ch / 48 kHz / 16-bit) -------------
// Compiling the class in costs static RAM unconditionally (~1 KB: _audiod_fct,
// the control buffer and the EP IN software FIFO) even while the audio function
// is hidden from the descriptor. That's the price of not rebuilding TinyUSB
// state at runtime, and it's noise against the ~160 KB of headroom this build
// has (.bss is 220 KB of the RP2350's 520 KB).
#define CFG_TUD_AUDIO USB_AUDIO_ENABLE

#if USB_AUDIO_ENABLE
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 0
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 0
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 0
#define CFG_TUD_AUDIO_CTRL_BUF_SZ 64

// 196, NOT 192. With flow control on, audiod_calc_tx_packet_sz() computes
// packet_sz_tx_max = (48+1) frames * 2ch * 2B = 196 and then asserts
// packet_sz_tx_max <= ep_in_sz, where ep_in_sz is read straight out of the
// descriptor's wMaxPacketSize. Put 192 here (or there) and the microphone
// enumerates perfectly but SET_INTERFACE alt-1 stalls: it never streams.
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 196
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ 1024

// The ADC samples off the RP2350's crystal; the host's SOF runs off its own
// clock. Those drift, so a fixed 48 samples/frame would eventually over- or
// under-run the software FIFO and click. Flow control lets the driver send
// 188/192/196-byte packets to track the host's actual consumption instead.
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 1

// TWO TinyUSB trees are in this build and both see this config file.
//
// The audio class driver that actually links is Adafruit's TinyUSB 0.20
// (libraries/Adafruit_TinyUSB_Arduino) - it is the only audio_device.c
// compiled. But the arduino-pico core also builds cores/rp2040/sdkoverride/
// {hid,midi,msc,ncm}_device.c against the pico-sdk's bundled TinyUSB *0.18*
// headers, and tusb.h there pulls in 0.18's audio_device.h, which #errors
// unless these three older macros exist. 0.20 dropped all three (audiod_open()
// parses the descriptor at runtime instead), so they are inert for the driver
// we actually run - they exist purely to get the core's own files to compile.
//
// FUNC_1_DESC_LEN must still be right: keep it equal to JL_AUDIO_DESC_LEN,
// which usb_descriptors.cpp static_asserts against the emitted bytes.
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN     JL_AUDIO_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT     1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ  64
#endif

// CDC FIFO size of TX and RX - OPTIMIZED FOR HIGH-THROUGHPUT STREAMING.
// Each enabled CDC interface allocates RX+TX FIFOs in TinyUSB's static state
// (_cdcd_itf ~= 8.5 KB for 4 CDC at 1024/1024). On the RP2040 (OG) that static
// RAM is precious and it's a full-speed (64-byte packet) device anyway, so use
// small FIFOs there.
#if defined(OG_JUMPERLESS)
// RP2040 OG: 4 CDC interfaces each carry RX+TX FIFOs in TinyUSB's static state,
// so every 128 bytes here costs 4 x 2 x 128 = 1 KB of scarce SRAM. At 128 B
// (2x the 64-byte full-speed bulk packet) the REPL/passthrough still stream;
// bursts just refill a packet sooner. Drops _cdcd_itf ~1 KB vs 256.
#define CFG_TUD_CDC_RX_BUFSIZE 128
#define CFG_TUD_CDC_TX_BUFSIZE 128
#else
#define CFG_TUD_CDC_RX_BUFSIZE 1024   // Keep RX buffer reasonable
#define CFG_TUD_CDC_TX_BUFSIZE 1024  // Larger TX buffer for streaming - double the previous size
#endif

// STREAMING PERFORMANCE OPTIMIZATIONS
#define CFG_TUSB_OPT_HIGH_SPEED 1
#define CFG_TUSB_OPT_PACKET_SIZE 512  // USB 2.0 bulk packet size

// Additional streaming optimizations
#define CFG_TUD_CDC_TX_FLUSH_THRESHOLD 512  // Flush when we have a full packet
#define CFG_TUD_CDC_STREAM_MODE 1            // Enable streaming mode optimizations

// MSC Buffer size of Device Mass storage
#define CFG_TUD_MSC_EP_BUFSIZE 64

// HID buffer size Should be sufficient to hold ID (if any) + Data
#define CFG_TUD_HID_EP_BUFSIZE 64

// MIDI FIFO size of TX and RX
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 128

// Vendor FIFO size of TX and RX
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

//--------------------------------------------------------------------
// Host Configuration
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Number of hub devices
#define CFG_TUH_HUB 1

// max device support (excluding hub device): 1 hub typically has 4 ports
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)

// Enable tuh_edpt_xfer() API
// #define CFG_TUH_API_EDPT_XFER       1

// Number of mass storage
#define CFG_TUH_MSC 1

// Number of HIDs
// typical keyboard + mouse device can have 3,4 HID interfaces
#define CFG_TUH_HID (3 * CFG_TUH_DEVICE_MAX)

// Number of CDC interfaces
// FTDI and CP210x are not part of CDC class, only to re-use CDC driver API
#define CFG_TUH_CDC 3
#define CFG_TUH_CDC_FTDI 1
#define CFG_TUH_CDC_CP210X 1

// RX & TX fifo size
#define CFG_TUH_CDC_RX_BUFSIZE 128
#define CFG_TUH_CDC_TX_BUFSIZE 128

// Set Line Control state on enumeration/mounted:
// DTR ( bit 0), RTS (bit 1)
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM 0x03

// Set Line Coding on enumeration/mounted, value for cdc_line_coding_t
// bit rate = 115200, 1 stop bit, no parity, 8 bit data width
// This need Pico-PIO-USB at least 0.5.1
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM                                        \
  { 115200, CDC_LINE_CONDING_STOP_BITS_0, CDC_LINE_CODING_PARITY_NONE, 8 }

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_RP2040_H_ */
