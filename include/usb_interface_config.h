#ifndef USB_INTERFACE_CONFIG_H
#define USB_INTERFACE_CONFIG_H
#include <stdint.h>
#include <stdbool.h>  // For bool type in C
// Note: Don't include config.h here - it causes preprocessor issues
// We'll handle dynamic naming in the USB descriptor callback

// =============================================================================
// SIMPLE STATIC USB Interface Configuration
// =============================================================================
// Fixed configuration: 4 CDC + 1 MSC (+ the runtime UAC2 microphone below)

// CDC Serial Interfaces (Communication Device Class)
// Overridable per board via -DUSB_CDC_ENABLE_COUNT=N (see platformio.ini).
// V5 exposes 4: CDC0 commands, USBSer1 Arduino passthrough, USBSer2 mpremote,
// USBSer3 LLM JSON backchannel. OG keeps the same layout; if the RP2040
// full-speed endpoint budget is tight, drop USBSer1 first (set this to 3).
#ifndef USB_CDC_ENABLE_COUNT
#define USB_CDC_ENABLE_COUNT 4
#endif

// MSC (Mass Storage Class). Overridable per board (OG may disable mass storage
// if USBfs is filtered out of the build).
#ifndef USB_MSC_ENABLE
#define USB_MSC_ENABLE 1
#endif

// All other interfaces disabled for simplicity
#define USB_MIDI_ENABLE 0
#define USB_HID_ENABLE 0
#define USB_HID_ENABLE_COUNT 0
#define USB_VENDOR_ENABLE 0

// USB Audio Class 2.0 microphone (2ch / 48kHz / 16-bit from two ADC channels).
// V5 only: OG's RP2040 has 4 ADC channels, 128-byte CDC FIFOs and no SRAM to
// spare for a DMA ring plus an ISO endpoint.
//
// This one is DIFFERENT from every other interface above: enabling it here only
// compiles the code in. The UAC2 function is NOT in the default config
// descriptor - it is appended only while the runtime flag is set, so the board
// does not park a phantom microphone in everybody's sound settings. See
// usb_audio_device_enabled() below and the two descriptor variants in
// src/usb_descriptors.cpp.
#if defined(OG_JUMPERLESS)
#define USB_AUDIO_ENABLE 0
#else
#define USB_AUDIO_ENABLE 1
#endif

// =============================================================================
// TinyUSB Configuration Mapping
// =============================================================================

#define CFG_TUD_CDC USB_CDC_ENABLE_COUNT
#define CFG_TUD_MSC USB_MSC_ENABLE
#define CFG_TUD_MIDI USB_MIDI_ENABLE
#define CFG_TUD_HID USB_HID_ENABLE

// =============================================================================
// Simple Static Interface Names
// =============================================================================

static const char* USB_CDC_NAMES[] = {
    "Jumperless Main",       // CDC 0 - Main serial
    "JL UART Passthrough",   // CDC 1 - Arduino/Serial1
    "JL Micropython REPL",     // CDC 2 - Micropython REPL
    "JL TUI",                // CDC 3 - TUI
    "JL Serial 3"            // CDC 4 - Serial 3
};

#define USB_MSC_NAME     "JL Mass Storage"
#define USB_AUDIO_NAME   "JL Audio In"

// String indices are POSITIONAL in the #if-compacted string_desc_arr[] over in
// usb_descriptors.cpp, so they shift whenever an interface is toggled. Compute
// them; never write a literal.
//
// (The existing TUD_MSC_DESCRIPTOR in usb_descriptors.cpp hardcodes 9 and is
// therefore off by one - with 4 CDC the array only runs 0..8, MSC's own name
// sitting at 8 - so the MSC interface currently has no name. Don't copy that.)
#define STRIDX_CDC_BASE  4
#define STRIDX_MSC       (STRIDX_CDC_BASE + USB_CDC_ENABLE_COUNT)
#define STRIDX_AUDIO     (STRIDX_MSC + USB_MSC_ENABLE)

// =============================================================================
// Interface Number Definitions (for debugging and verification)
// =============================================================================

// Calculate interface numbers based on enabled interfaces
enum {
  // CDC interfaces (each CDC uses 2 interface numbers)
#if USB_CDC_ENABLE_COUNT >= 1
  ITF_NUM_CDC_0 = 0,
  ITF_NUM_CDC_0_DATA,
#endif
#if USB_CDC_ENABLE_COUNT >= 2
  ITF_NUM_CDC_1,
  ITF_NUM_CDC_1_DATA,
#endif
#if USB_CDC_ENABLE_COUNT >= 3
  ITF_NUM_CDC_2,
  ITF_NUM_CDC_2_DATA,
#endif
#if USB_CDC_ENABLE_COUNT >= 4
  ITF_NUM_CDC_3,
  ITF_NUM_CDC_3_DATA,
#endif
#if USB_CDC_ENABLE_COUNT >= 5
  ITF_NUM_CDC_4,
  ITF_NUM_CDC_4_DATA,
#endif

  // MSC interface
#if USB_MSC_ENABLE
  ITF_NUM_MSC,
#endif

  ITF_NUM_TOTAL
};

// Audio interface numbers are appended AFTER ITF_NUM_TOTAL rather than being
// members of the enum above, because the audio function comes and goes at
// runtime. Keeping them outside guarantees every CDC and MSC interface number
// is byte-identical in both descriptor variants - the host only ever sees
// interfaces appear or disappear off the end, never renumber.
#if USB_AUDIO_ENABLE
#define ITF_NUM_AUDIO_CTRL   (ITF_NUM_TOTAL)      // AudioControl
#define ITF_NUM_AUDIO_AS     (ITF_NUM_TOTAL + 1)  // AudioStreaming
#define ITF_NUM_TOTAL_AUDIO  (ITF_NUM_TOTAL + 2)

// UAC2 entity IDs. Shared: usb_descriptors.cpp builds the descriptor from them,
// USBAudio.cpp matches incoming class requests against them.
#define JL_AUDIO_ID_CLOCK     0x04
#define JL_AUDIO_ID_IN_TERM   0x01
#define JL_AUDIO_ID_OUT_TERM  0x02

// Descriptor geometry. Lives here rather than in usb_descriptors.cpp because
// custom_tusb_config.h needs the total length too - see the two-TinyUSB-trees
// note there. usb_descriptors.cpp static_asserts the real array against it, so
// these can't silently drift from the bytes actually emitted.
#define JL_AUDIO_CS_AC_LEN    (9 + 8 + 17 + 12)                              // 46
#define JL_AUDIO_DESC_LEN     (8 + 9 + JL_AUDIO_CS_AC_LEN + 9 + 9 + 16 + 6 + 7 + 8)  // 118
#define JL_AUDIO_EP_SZ        196
#endif

// =============================================================================
// USB CDC Flow Control Configuration
// =============================================================================
// These functions allow ignoring DTR/RTS flow control signals from the host.
// This is useful for hosts that don't set DTR (industrial software, custom apps).

#ifdef __cplusplus
extern "C" {
#endif

// Global flag indicating whether to ignore DTR for connection status
extern volatile bool g_usb_ignore_dtr;

// Apply CDC configuration from jumperlessConfig - call after config is loaded
void usb_cdc_apply_config(void);

// Get current DTR ignore state
bool usb_cdc_get_ignore_dtr(void);

// Set DTR ignore mode at runtime (also updates config)
void usb_cdc_set_ignore_dtr(bool ignore);

#if USB_AUDIO_ENABLE
// Selects which config descriptor variant tud_descriptor_configuration_cb
// hands back. Lives in USBAudio.cpp; declared here so usb_descriptors.cpp can
// call it without pulling in the whole audio header.
bool usb_audio_device_enabled(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // USB_INTERFACE_CONFIG_H 