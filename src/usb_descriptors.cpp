/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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
 *
 */

#include "tusb.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "JumperlessDefines.h"

// Override compile-time config with runtime dynamic config
#define OVERRIDE_USB_CONFIG 1
#include "usb_interface_config.h"
#include "config.h"
#include "configManager.h"

// Global flag for DTR ignore mode - can be updated at runtime from config
volatile bool g_usb_ignore_dtr = false;

// Add missing interface name constants
#define USB_HID_NAME     "JL HID Interface"
#define USB_MIDI_NAME    "JL MIDI Interface"  
#define USB_VENDOR_NAME  "JL Vendor Interface"

extern "C" {

// Declare the real functions for linker wrapping
extern uint8_t const * __real_tud_descriptor_device_cb(void);
extern uint8_t const * __real_tud_descriptor_configuration_cb(uint8_t index);
extern uint16_t const * __real_tud_descriptor_string_cb(uint8_t index, uint16_t langid);

// Declare the real CDC connected function for linker wrapping
extern bool __real_tud_cdc_n_connected(uint8_t itf);

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // Use Interface Association Descriptor (IAD) for composite device
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x1D50,
    .idProduct          = 0xACAB,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

#if USB_AUDIO_ENABLE
// Identical to desc_device except for bcdDevice. Windows caches composite
// driver bindings keyed on VID/PID/bcdDevice, so presenting two different
// interface layouts under one revision leaves it replaying a stale stack for
// whichever layout it saw first. Bumping the revision makes it re-run composite
// enumeration. VID/PID stay put so existing udev rules and INFs keep matching,
// and the serial string is untouched - that's what makes the CDC ports come
// back as the SAME /dev/cu.usbmodemJLV5portN nodes after a re-enumeration.
tusb_desc_device_t const desc_device_audio =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x1D50,
    .idProduct          = 0xACAB,
    .bcdDevice          = 0x0101,   // <- the only difference

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};
#endif

// Wrapped function that intercepts the library's descriptor callback
__attribute__((used))
uint8_t const * __wrap_tud_descriptor_device_cb(void)
{
  // Debug: Flash the LED to show our descriptor is being called
  static bool first_call = true;
  if (first_call) {
    first_call = false;
    Serial.println("◆ USB Device descriptor requested");
    Serial.printf("◆ Device class: 0x%02X, subclass: 0x%02X, protocol: 0x%02X\n",
                 desc_device.bDeviceClass, desc_device.bDeviceSubClass, desc_device.bDeviceProtocol);
  }
#if USB_AUDIO_ENABLE
  if (usb_audio_device_enabled()) return (uint8_t const *) &desc_device_audio;
#endif
  return (uint8_t const *) &desc_device;
}

// HID Report Descriptor
#if USB_HID_ENABLE_COUNT > 0
uint8_t const desc_hid_report[] =
{
  TUD_HID_REPORT_DESC_GENERIC_INOUT(16)
};
#endif

//--------------------------------------------------------------------+
// Interface numbers are defined in usb_interface_config.h
//--------------------------------------------------------------------+

// Calculate total descriptor length
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + \
                          (USB_CDC_ENABLE_COUNT * TUD_CDC_DESC_LEN) + \
                          (USB_MSC_ENABLE * TUD_MSC_DESC_LEN) + \
                          (USB_HID_ENABLE_COUNT * TUD_HID_DESC_LEN) + \
                          (USB_MIDI_ENABLE * TUD_MIDI_DESC_LEN) + \
                          (USB_VENDOR_ENABLE * TUD_VENDOR_DESC_LEN))

//--------------------------------------------------------------------+
// Dynamic Endpoint Assignment
//--------------------------------------------------------------------+

// CDC endpoints - use sequential numbering to avoid conflicts
#if USB_CDC_ENABLE_COUNT >= 1
#define EPNUM_CDC_0_NOTIF   0x81
#define EPNUM_CDC_0_OUT     0x02
#define EPNUM_CDC_0_IN      0x82
#endif

#if USB_CDC_ENABLE_COUNT >= 2
#define EPNUM_CDC_1_NOTIF   0x83
#define EPNUM_CDC_1_OUT     0x04
#define EPNUM_CDC_1_IN      0x84
#endif

#if USB_CDC_ENABLE_COUNT >= 3
#define EPNUM_CDC_2_NOTIF   0x85
#define EPNUM_CDC_2_OUT     0x06
#define EPNUM_CDC_2_IN      0x86
#endif

#if USB_CDC_ENABLE_COUNT >= 4
#define EPNUM_CDC_3_NOTIF   0x87
#define EPNUM_CDC_3_OUT     0x08
#define EPNUM_CDC_3_IN      0x88
#endif

#if USB_CDC_ENABLE_COUNT >= 5
#define EPNUM_CDC_4_NOTIF   0x89
#define EPNUM_CDC_4_OUT     0x0A
#define EPNUM_CDC_4_IN      0x8A
#endif

// MSC endpoints - place after all CDC endpoints to avoid conflicts
#if USB_MSC_ENABLE
#define EPNUM_MSC_OUT       0x0B    // EP11 OUT (after CDC endpoints)
#define EPNUM_MSC_IN        0x8B    // EP11 IN (after CDC endpoints)
#endif

// HID endpoints (if enabled in future)
#if USB_HID_ENABLE_COUNT >= 1
#define EPNUM_HID_0         (EPNUM_MSC_IN + 1)
#endif

#if USB_HID_ENABLE_COUNT >= 2
#define EPNUM_HID_1         (EPNUM_HID_0 + 1)
#endif

// MIDI endpoints (if enabled in future)
#if USB_MIDI_ENABLE
#define EPNUM_MIDI_OUT      (EPNUM_MSC_OUT + 1)
#define EPNUM_MIDI_IN       (EPNUM_MSC_IN + 1)
#endif

// Vendor endpoints (if enabled in future)
#if USB_VENDOR_ENABLE
#define EPNUM_VENDOR_OUT    (EPNUM_MSC_OUT + 1)
#define EPNUM_VENDOR_IN     (EPNUM_MSC_IN + 1)
#endif

// Audio isochronous IN. Deliberately NOT (EPNUM_MSC_IN + 1): 0x89/0x8A belong
// to the CDC_4 macros above and would collide the moment anyone bumps
// USB_CDC_ENABLE_COUNT to 5, and 0x8B is MSC. EP12 is clear either way.
#if USB_AUDIO_ENABLE
#define EPNUM_AUDIO_IN      0x8C
#endif

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

// Same as TUD_CDC_DESCRIPTOR but also sets iFunction in the IAD so that both
// the IAD string and the interface string carry the CDC name.  The stock macro
// already puts _stridx in the interface descriptor's iInterface field, but
// Chrome/macOS doesn't always surface it.  Adding it to the IAD too helps
// Windows Device Manager show the correct name.
#define TUD_CDC_DESCRIPTOR_NAMED(_itfnum, _stridx, _ep_notif, _ep_notif_size, _epout, _epin, _epsize) \
  /* Interface Association — iFunction = _stridx (stock has 0) */\
  8, TUSB_DESC_INTERFACE_ASSOCIATION, _itfnum, 2, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE, _stridx,\
  /* CDC Control Interface — iInterface = _stridx (same as stock) */\
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE, _stridx,\
  /* CDC Header */\
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0120),\
  /* CDC Call Management */\
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0, (uint8_t)((_itfnum) + 1),\
  /* CDC ACM: line coding + send break (bmCapabilities = 0x06) */\
  4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 6,\
  /* CDC Union */\
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, _itfnum, (uint8_t)((_itfnum) + 1),\
  /* Endpoint Notification */\
  7, TUSB_DESC_ENDPOINT, _ep_notif, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(_ep_notif_size), 1,\
  /* CDC Data Interface */\
  9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum)+1), 0, 2, TUSB_CLASS_CDC_DATA, 0, 0, 0,\
  /* Endpoint Out */\
  7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,\
  /* Endpoint In */\
  7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

#if USB_AUDIO_ENABLE
//--------------------------------------------------------------------+
// UAC2 microphone descriptor
//--------------------------------------------------------------------+
// TinyUSB 0.20 deleted BOTH the TUD_AUDIO_MIC_* composites AND the
// TUD_AUDIO_DESC_* building blocks - audio.h is types and enums only now - so
// this is spelled out byte by byte. Topology:
//
//     [Clock 0x04] -> [Input Terminal 0x01 (mic, 2ch)]
//                  -> [Output Terminal 0x02 (USB streaming)] -> ISO IN
//
// The AudioStreaming interface has alt-0 (zero bandwidth, no endpoint) and
// alt-1 (the ISO IN endpoint), as UAC2 requires. audiod only opens the endpoint
// on SET_INTERFACE alt=1, which is what lets the host - not us - decide when
// capture actually starts.
// Entity IDs (JL_AUDIO_ID_*) and the descriptor geometry (JL_AUDIO_CS_AC_LEN =
// 46 for the class-specific AudioControl block, JL_AUDIO_DESC_LEN = 118 for the
// whole function, JL_AUDIO_EP_SZ = 196) all come from usb_interface_config.h,
// because custom_tusb_config.h needs the length too. The static_asserts below
// check the bytes we actually emit against them.

#define TUD_JL_AUDIO_MIC_DESCRIPTOR(_itfnum, _stridx, _epin, _epsize) \
  /* Interface Association: 2 interfaces, AUDIO / undefined subclass / IAD v2 */\
  8, TUSB_DESC_INTERFACE_ASSOCIATION, (uint8_t)(_itfnum), 2,\
     TUSB_CLASS_AUDIO, 0x00, AUDIO_FUNC_PROTOCOL_CODE_V2, _stridx,\
  /* Standard AC Interface. bNumEndpoints MUST be 0 - audiod_open() asserts it\
     is <= 1, and anything above 0 demands CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP. */\
  9, TUSB_DESC_INTERFACE, (uint8_t)(_itfnum), 0, 0,\
     TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, AUDIO_INT_PROTOCOL_CODE_V2, _stridx,\
  /* CS AC Header: bcdADC 2.00, category MICROPHONE, wTotalLength, bmControls 0 */\
  9, TUSB_DESC_CS_INTERFACE, AUDIO20_CS_AC_INTERFACE_HEADER,\
     U16_TO_U8S_LE(0x0200), AUDIO20_FUNC_MICROPHONE,\
     U16_TO_U8S_LE(JL_AUDIO_CS_AC_LEN), 0x00,\
  /* Clock Source: internal programmable. bmControls 0x07 = frequency\
     host-programmable (0b11 at pos 0) | validity read-only (0b01 at pos 2).\
     We advertise programmable and ACCEPT SET_CUR of 48000 rather than\
     advertising read-only: advertise-then-stall is a known Windows stream-init\
     breaker, and accept-the-one-legal-value is what the stock uac2 example does. */\
  8, TUSB_DESC_CS_INTERFACE, AUDIO20_CS_AC_INTERFACE_CLOCK_SOURCE,\
     JL_AUDIO_ID_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_PRO_CLK, 0x07, 0x00, 0x00,\
  /* Input Terminal: generic microphone, 2ch (front L | front R), clocked by 0x04 */\
  17, TUSB_DESC_CS_INTERFACE, AUDIO20_CS_AC_INTERFACE_INPUT_TERMINAL,\
     JL_AUDIO_ID_IN_TERM, U16_TO_U8S_LE(AUDIO_TERM_TYPE_IN_GENERIC_MIC), 0x00,\
     JL_AUDIO_ID_CLOCK, 2, U32_TO_U8S_LE(0x00000003), 0x00,\
     U16_TO_U8S_LE(0x0000), 0x00,\
  /* Output Terminal: USB streaming, sourced from the input terminal */\
  12, TUSB_DESC_CS_INTERFACE, AUDIO20_CS_AC_INTERFACE_OUTPUT_TERMINAL,\
     JL_AUDIO_ID_OUT_TERM, U16_TO_U8S_LE(AUDIO_TERM_TYPE_USB_STREAMING), 0x00,\
     JL_AUDIO_ID_IN_TERM, JL_AUDIO_ID_CLOCK, U16_TO_U8S_LE(0x0000), 0x00,\
  /* Standard AS Interface alt 0 - zero bandwidth, host parks here when idle */\
  9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum)+1), 0, 0,\
     TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, AUDIO_INT_PROTOCOL_CODE_V2, 0x00,\
  /* Standard AS Interface alt 1 - one ISO IN endpoint, host selects to record */\
  9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum)+1), 1, 1,\
     TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, AUDIO_INT_PROTOCOL_CODE_V2, 0x00,\
  /* CS AS General: linked to the output terminal, PCM, 2ch front L|R */\
  16, TUSB_DESC_CS_INTERFACE, AUDIO20_CS_AS_INTERFACE_AS_GENERAL,\
     JL_AUDIO_ID_OUT_TERM, 0x00, AUDIO20_FORMAT_TYPE_I,\
     U32_TO_U8S_LE(AUDIO20_DATA_FORMAT_TYPE_I_PCM), 2,\
     U32_TO_U8S_LE(0x00000003), 0x00,\
  /* Type I Format: 2-byte subslot, 16-bit resolution */\
  6, TUSB_DESC_CS_INTERFACE, AUDIO20_CS_AS_INTERFACE_FORMAT_TYPE,\
     AUDIO20_FORMAT_TYPE_I, 2, 16,\
  /* Standard AS ISO Data Endpoint. Asynchronous because our ADC runs off the\
     RP2350 crystal, not the host frame clock - the host resamples. The usage\
     bits must stay 0 (data): audiod tests them to tell data from feedback. */\
  7, TUSB_DESC_ENDPOINT, _epin,\
     (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA),\
     U16_TO_U8S_LE(_epsize), 1,\
  /* CS AS ISO Data Endpoint: no pitch/overrun/underrun controls, no lock delay */\
  8, TUSB_DESC_CS_ENDPOINT, AUDIO20_CS_EP_SUBTYPE_GENERAL, 0x00, 0x00, 0x00,\
     U16_TO_U8S_LE(0x0000)
#endif // USB_AUDIO_ENABLE

// The CDC + MSC body, factored out so the plain and audio-mode configuration
// descriptors can never drift apart. Everything here is byte-identical in both
// variants; only the config header and the trailing audio block differ.
//
// Each piece carries its own trailing comma and compiles to nothing when its
// interface is disabled, so the USB_CDC_ENABLE_COUNT / USB_MSC_ENABLE
// configurability of the original array is preserved exactly.
#if USB_CDC_ENABLE_COUNT >= 1
#define JL_CDC0_DESC TUD_CDC_DESCRIPTOR_NAMED(ITF_NUM_CDC_0, STRIDX_CDC_BASE + 0, EPNUM_CDC_0_NOTIF, 8, EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),
#else
#define JL_CDC0_DESC
#endif
#if USB_CDC_ENABLE_COUNT >= 2
#define JL_CDC1_DESC TUD_CDC_DESCRIPTOR_NAMED(ITF_NUM_CDC_1, STRIDX_CDC_BASE + 1, EPNUM_CDC_1_NOTIF, 8, EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),
#else
#define JL_CDC1_DESC
#endif
#if USB_CDC_ENABLE_COUNT >= 3
#define JL_CDC2_DESC TUD_CDC_DESCRIPTOR_NAMED(ITF_NUM_CDC_2, STRIDX_CDC_BASE + 2, EPNUM_CDC_2_NOTIF, 8, EPNUM_CDC_2_OUT, EPNUM_CDC_2_IN, 64),
#else
#define JL_CDC2_DESC
#endif
#if USB_CDC_ENABLE_COUNT >= 4
#define JL_CDC3_DESC TUD_CDC_DESCRIPTOR_NAMED(ITF_NUM_CDC_3, STRIDX_CDC_BASE + 3, EPNUM_CDC_3_NOTIF, 8, EPNUM_CDC_3_OUT, EPNUM_CDC_3_IN, 64),
#else
#define JL_CDC3_DESC
#endif
#if USB_CDC_ENABLE_COUNT >= 5
#define JL_CDC4_DESC TUD_CDC_DESCRIPTOR_NAMED(ITF_NUM_CDC_4, STRIDX_CDC_BASE + 4, EPNUM_CDC_4_NOTIF, 8, EPNUM_CDC_4_OUT, EPNUM_CDC_4_IN, 64),
#else
#define JL_CDC4_DESC
#endif
#if USB_MSC_ENABLE
// STRIDX_MSC, not the literal 9 the original used. With 4 CDC the string array
// only runs 0..8 (MSC's own name sits at 8), so 9 fell off the end and
// __wrap_tud_descriptor_string_cb returned NULL - the MSC interface has been
// nameless this whole time. Fixed in passing; do not reintroduce a literal.
#define JL_MSC_DESC  TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRIDX_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
#else
#define JL_MSC_DESC
#endif

#define JL_BASE_INTERFACES \
  JL_CDC0_DESC JL_CDC1_DESC JL_CDC2_DESC JL_CDC3_DESC JL_CDC4_DESC JL_MSC_DESC

uint8_t const desc_fs_configuration[] =
{
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 500),

  // CDC interfaces (uses the _NAMED variant so WebUSB can read interfaceName)
  // and MSC, shared verbatim with the audio-mode descriptor below.
  JL_BASE_INTERFACES

  // HID interfaces
#if USB_HID_ENABLE_COUNT >= 1
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_0, 9, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID_0, 16, 10),
#if USB_HID_ENABLE_COUNT >= 2
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_1, 9, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID_1, 16, 10),
#endif
#endif

  // MIDI interface
#if USB_MIDI_ENABLE
  TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 10, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
#endif

  // Vendor interface
#if USB_VENDOR_ENABLE
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 11, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),
#endif
};

static_assert(sizeof(desc_fs_configuration) == CONFIG_TOTAL_LEN,
              "base configuration descriptor length mismatch");

#if USB_AUDIO_ENABLE
//--------------------------------------------------------------------+
// Configuration Descriptor, audio variant
//--------------------------------------------------------------------+
// Identical to desc_fs_configuration with the UAC2 function appended, so no
// existing interface or endpoint number moves. Two const arrays in flash rather
// than one buffer rebuilt at runtime, deliberately: audiod_open() stashes a raw
// pointer INTO the configuration descriptor (_audiod_fct[i].p_desc) and keeps
// dereferencing it for the whole session, so a buffer that could be rewritten
// or moved underneath it is a use-after-free waiting to happen.
#define CONFIG_TOTAL_LEN_AUDIO (CONFIG_TOTAL_LEN + JL_AUDIO_DESC_LEN)

uint8_t const desc_fs_configuration_audio[] =
{
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_AUDIO, 0, CONFIG_TOTAL_LEN_AUDIO, 0x80, 500),

  JL_BASE_INTERFACES

  TUD_JL_AUDIO_MIC_DESCRIPTOR(ITF_NUM_AUDIO_CTRL, STRIDX_AUDIO,
                              EPNUM_AUDIO_IN, JL_AUDIO_EP_SZ),
};

static_assert(sizeof(desc_fs_configuration_audio) == CONFIG_TOTAL_LEN_AUDIO,
              "UAC2 configuration descriptor length mismatch - check JL_AUDIO_DESC_LEN");
static_assert(sizeof(desc_fs_configuration_audio) - sizeof(desc_fs_configuration)
                  == JL_AUDIO_DESC_LEN,
              "audio block is not exactly JL_AUDIO_DESC_LEN bytes");
#endif // USB_AUDIO_ENABLE

// Wrapped function that intercepts the library's configuration descriptor callback
__attribute__((used))
uint8_t const * __wrap_tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index; // for multiple configurations
  
  static bool first_call = true;
  if (first_call) {
    first_call = false;
    Serial.println("◆ USB Configuration descriptor requested");
    Serial.printf("◆ Total interfaces: %d\n", ITF_NUM_TOTAL);
    Serial.printf("◆ CDC interfaces: %d\n", USB_CDC_ENABLE_COUNT);
    Serial.printf("◆ MSC enabled: %d\n", USB_MSC_ENABLE);
    Serial.printf("◆ Config length: %d bytes\n", CONFIG_TOTAL_LEN);
    
    // Debug interface assignments
    Serial.println("◆ Interface assignments:");
#if USB_CDC_ENABLE_COUNT >= 1
    Serial.printf("◆   CDC 0: interfaces %d, %d\n", ITF_NUM_CDC_0, ITF_NUM_CDC_0_DATA);
#endif
#if USB_CDC_ENABLE_COUNT >= 2
    Serial.printf("◆   CDC 1: interfaces %d, %d\n", ITF_NUM_CDC_1, ITF_NUM_CDC_1_DATA);
#endif
#if USB_CDC_ENABLE_COUNT >= 3
    Serial.printf("◆   CDC 2: interfaces %d, %d\n", ITF_NUM_CDC_2, ITF_NUM_CDC_2_DATA);
#endif
#if USB_CDC_ENABLE_COUNT >= 4
    Serial.printf("◆   CDC 3: interfaces %d, %d\n", ITF_NUM_CDC_3, ITF_NUM_CDC_3_DATA);
#endif
#if USB_CDC_ENABLE_COUNT >= 5
    Serial.printf("◆   CDC 4: interfaces %d, %d\n", ITF_NUM_CDC_4, ITF_NUM_CDC_4_DATA);
#endif
#if USB_MSC_ENABLE
    Serial.printf("◆   MSC: interface %d\n", ITF_NUM_MSC);
    Serial.printf("◆   MSC endpoints: OUT=0x%02X, IN=0x%02X\n", EPNUM_MSC_OUT, EPNUM_MSC_IN);
#endif
  }

#if USB_AUDIO_ENABLE
  // The audio function is present only while the runtime flag is set, so the
  // board doesn't advertise a microphone nobody asked for. usb_audio_enable()
  // flips this and re-enumerates. No logging here: this runs in TinyUSB task
  // context (USB soft-IRQ) and hosts fetch it several times per enumeration.
  if (usb_audio_device_enabled()) return desc_fs_configuration_audio;
#endif

  return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// String Descriptor Index
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC_0,
  STRID_CDC_1,
  STRID_CDC_2,
  STRID_CDC_3,
  STRID_CDC_4,
  STRID_MSC,
  STRID_HID,
  STRID_MIDI,
  STRID_VENDOR
};

// Dynamic string descriptor array
static const char* string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
  "Architeuthis Flux",           // 1: Manufacturer
#if defined(OG_JUMPERLESS)
  "Jumperless OG",               // 2: Product
  "JLOGport",                    // 3: Serial base -> enumerates as JLOGportX
#else
  "Jumperless V5",               // 2: Product
  "JLV5port",                    // 3: Serial base -> enumerates as JLV5portX
#endif
  // CDC interface names (only include if enabled)
#if USB_CDC_ENABLE_COUNT >= 1
  USB_CDC_NAMES[0],              // 4: CDC Interface 0
#endif
#if USB_CDC_ENABLE_COUNT >= 2
  USB_CDC_NAMES[1],              // 5: CDC Interface 1
#endif
#if USB_CDC_ENABLE_COUNT >= 3
  USB_CDC_NAMES[2],              // 6: CDC Interface 2
#endif

#if USB_CDC_ENABLE_COUNT >= 4
  USB_CDC_NAMES[3],              // 7: CDC Interface 3
#endif

#if USB_CDC_ENABLE_COUNT >= 5
  USB_CDC_NAMES[4],              // 8: CDC Interface 4
#endif

  // Other interface names
#if USB_MSC_ENABLE
  USB_MSC_NAME,                  // 9: MSC Interface
#endif
#if USB_HID_ENABLE_COUNT > 0
  USB_HID_NAME,                  // 10: HID Interface
#endif
#if USB_MIDI_ENABLE
  USB_MIDI_NAME,                 // 11: MIDI Interface
#endif
#if USB_VENDOR_ENABLE
  USB_VENDOR_NAME,               // 12: Vendor Interface
#endif
#if USB_AUDIO_ENABLE
  USB_AUDIO_NAME,                // STRIDX_AUDIO - always present, since the
                                 // string table costs nothing when the audio
                                 // interface itself is hidden
#endif
};

#if USB_AUDIO_ENABLE
// Catches exactly the class of bug that left MSC nameless: a positional index
// that stopped matching its slot when some other interface was toggled.
static_assert(STRIDX_AUDIO == (sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) - 1,
              "STRIDX_AUDIO does not match its position in string_desc_arr");
#endif

static uint16_t _desc_str[32];

// Wrapped function that intercepts the library's string descriptor callback
__attribute__((used))
uint16_t const* __wrap_tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }
  else
  {
    // Use static string array only - no dynamic naming for now
    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) {
      return NULL;
    }
    
    const char* str = string_desc_arr[index];
    if (!str) return NULL;

    // Cap at max char
    chr_count = strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    // Convert ASCII string into UTF-16
    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

  return _desc_str;
}

//--------------------------------------------------------------------+
// CDC DTR Ignore Support
//--------------------------------------------------------------------+
// Wrapped function that intercepts tud_cdc_n_connected() calls
// When ignore_dtr is enabled, always returns true if USB device is ready
// This allows communication with hosts that don't set the DTR line
__attribute__((used))
bool __wrap_tud_cdc_n_connected(uint8_t itf)
{
  // If DTR ignore mode is enabled, just check if USB is ready
  // (not if DTR is set by the host)
  if (g_usb_ignore_dtr) {
    return tud_ready();
  }
  
  // Otherwise, use the original behavior which requires DTR to be set
  return __real_tud_cdc_n_connected(itf);
}

// Initialize CDC configuration based on jumperlessConfig
// Call this after config is loaded but before USB enumeration if possible
// Can also be called at runtime to change behavior
void usb_cdc_apply_config(void)
{
  // Update global flag from config
  // This flag is checked by __wrap_tud_cdc_n_connected() to bypass DTR checking
  g_usb_ignore_dtr = jumperlessConfig.usb_cdc.ignore_dtr;
  
  if (g_usb_ignore_dtr) {
    Serial.println("USB CDC: DTR ignore mode ENABLED");
    Serial.println("  - Connection will work without DTR signal from host");
  }
}

// Getter for the current DTR ignore state
bool usb_cdc_get_ignore_dtr(void)
{
  return g_usb_ignore_dtr;
}

// Setter for DTR ignore mode - allows runtime changes
void usb_cdc_set_ignore_dtr(bool ignore)
{
  g_usb_ignore_dtr = ignore;
  jumperlessConfig.usb_cdc.ignore_dtr = ignore;
  
  // Re-apply configuration
  usb_cdc_apply_config();
}

} // extern "C" 