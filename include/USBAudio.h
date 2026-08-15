#ifndef USB_AUDIO_H
#define USB_AUDIO_H

// USB Audio Class 2.0 microphone: two ADC channels streamed to the host as
// 2ch / 16 kHz (8-48 configurable) / 16-bit audio, so any recorder - QuickTime, Audacity, a DAW,
// ffmpeg - can listen to a circuit with no custom host software.
//
// Two independent layers, which is what makes "only show up when asked for"
// work without fighting the USB spec:
//
//   visibility  usb_audio_enable()/disable() add or remove the UAC2 function
//               from the config descriptor and re-enumerate the device. While
//               disabled the host sees the plain CDC+MSC composite it always
//               has, with no phantom microphone in its sound settings.
//
//   streaming   the HOST starts and stops capture by selecting AudioStreaming
//               alt-setting 1 or 0 (i.e. by opening or closing the input
//               device). That drives the ADC DMA. MicroPython never pumps
//               samples - it could not keep up with the sample clock anyway.
//
// V5 only; USB_AUDIO_ENABLE is 0 on the OG/RP2040 build.

#include "usb_interface_config.h"

#if USB_AUDIO_ENABLE

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- device visibility (re-enumerates; drops every CDC port for ~2 s) ------

// Add or remove the UAC2 function from the config descriptor and cycle the USB
// bus so the host picks up the new layout. Returns false if it refused (MSC is
// mounted, or already in the requested state and nothing to do).
//
// NOTE: this tears down the CDC port the caller is talking on. The serial
// number string is deliberately left untouched so the OS recreates the SAME
// /dev/cu.usbmodemJLV5portN nodes and terminals reconnect on their own.
bool usb_audio_set_device_enabled(bool on);

// Which descriptor variant tud_descriptor_configuration_cb should hand back.
// Also declared in usb_interface_config.h so usb_descriptors.cpp can call it
// without pulling in this header.
bool usb_audio_device_enabled(void);

// Restore the saved setup at boot, before the USB stack starts. If the mic was
// saved as enabled it is present from the very first enumeration, so no port is
// ever dropped - this is how you run with the mic on permanently.
void usb_audio_apply_config(void);

// Call once late in setup(), after USB and the config are both up: re-enumerates
// if the saved config wants the mic, since USB came up before the config was
// read. No-op when audio is not enabled.
void usb_audio_boot_enumerate(void);

// Persist the current setup (enabled state, channels, rate, scaling).
void usb_audio_save_config(void);

// ---- streaming ------------------------------------------------------------

// True while the ADC DMA is running, i.e. the host has the microphone open.
bool usb_audio_is_streaming(void);

// True from the moment the stream claims the ADC until it releases it. Read by
// readAdc()/updateLazyAdcReadings() to short-circuit instead of stalling on the
// 100 ms readingADC timeout.
extern volatile bool usbAudioOwnsAdc;

// Give the ADC back immediately, stopping any active stream. Called by things
// that must have the real ADC - logic analyzer arming, self test, calibration -
// so they never spin on a lock the audio path is holding. Does NOT
// re-enumerate: the device stays visible, the host just hears silence. Safe
// from either core (on core 1 it runs the pump inline; on core 0 it waits,
// bounded, for core 1 to carry it out). No Serial output - may run on core 1.
//
// RETURNS true if the ADC is free when it returns. It can fail: the pump runs
// only from core2stuff(), which loop1() skips while pauseCore2 is set or a
// logic-analyzer capture is running. ANYTHING that calibrates or measures must
// check this - reading anyway yields sweep means and a hard 0 on the probe
// channels, which silently poisons whatever is solved from them.
bool usb_audio_yield_adc(const char *why);

// The counterpart: after a yield, ask the pump to restart capture - but ONLY
// if the host still has the microphone open. Cheap and idempotent; call it
// when the logic analyzer / self test / calibration is done with the ADC.
void usb_audio_resume_adc(void);

// "The probe is in use right now." Stamped by Probing/MeasureMode when they
// have genuinely decoded a touch or are measuring; capture yields the ADC to
// the probe while these keep arriving and resumes ~300 ms after they stop.
// One volatile store; safe from any context.
void usb_audio_probe_activity(void);

// Pumped from core 1's loop1() (core2stuff). Does the actual ADC/DMA start and
// stop requested by the class callbacks, which run in USB IRQ context and must
// not spin on readingADC there. Owns the DMA completion IRQ on core 1.
void serviceUSBAudio(void);

// One-time DMA channel + IRQ handler acquisition. Call once from setup().
bool usbAudioInit(void);

// Serve a streamed channel from the last completed DMA half (a 48-sample mean).
// Returns false for channels we aren't capturing, and whenever the audio path
// doesn't own the ADC. Lets readAdc() keep returning real data for the two
// audio channels instead of stalling.
bool usbAudioSnapshotRaw(int channel, int *raw);

// Refresh only the streamed entries of the adcReadings[] cache, leaving the
// others at their last good value. Called by updateLazyAdcReadings().
bool usbAudioRefreshLazy(float *readings);

// ---- configuration --------------------------------------------------------

// Pick which ADC channels feed left and right. 0-3 are the +-8V crossbar-
// routable channels, so connect(ADC0, <row>) is what puts a breadboard node on
// a stereo channel. Restarts capture if it is already running (the L/R
// interleave has to be resynced). Returns false on an out-of-range channel.
bool usb_audio_set_channels(int left, int right);

// Input volts corresponding to full-scale PCM. Default 8.0 (the analog window).
// Takes effect immediately without a glitch; no re-enumeration.
bool usb_audio_set_full_scale(float volts);

// Streaming rate in Hz, 8000-48000 in 1 kHz steps. Default 16000: the ADC is
// shared with the OLED voltage cache, probe sensing and supply sense, and
// running it at 48 kHz starved them. Restarts capture if already running.
bool usb_audio_set_rate(uint32_t hz);

// One-pole ~15 Hz high-pass, on by default so hosts don't see a standing DC
// offset. Turn off for DC-coupled measurement.
void usb_audio_set_dc_block(bool on);

// Everything usb_audio_status() reports, in one shot. The counters are the
// health of a recording without listening to it: a clean stream has
// frames_sent climbing at sample_rate per second and everything below it flat
// (late_irq/resyncs tick once per flash write - expected, the DMA rides through
// those - probe_pauses once per probe use).
typedef struct {
    bool     enabled;        // UAC2 function present in the config descriptor
    bool     streaming;      // capture is running
    bool     host_open;      // host has selected the streaming alt setting
    int      left_ch;
    int      right_ch;
    float    full_scale;
    bool     dc_block;
    uint32_t sample_rate;
    uint32_t frames_sent;
    uint32_t fifo_overflow;  // host stalled / software FIFO full
    uint32_t adc_overrun;    // ADC FIFO overran; forced an interleave resync
    uint32_t late_irq;       // completion IRQ landed after the sibling chained back
    uint32_t resyncs;        // capture restarts (overrun / late IRQ / reconfigure)
    uint32_t probe_pauses;   // capture yielded to the probe
    uint32_t claim_fail;     // start declined: ADC held by someone else
    uint32_t init_fail;      // 0 ok, 1 no DMA channels, 2 DMA_IRQ_1 taken, 9 not tried
} usb_audio_status_t;

void usb_audio_get_status(usb_audio_status_t *out);

#ifdef __cplusplus
}

// Scoped "I need the real ADC" for the long-running C++ callers (self test,
// DAC calibration): yields the stream on entry, and on every exit path asks
// the pump to resume - which it only does if the host still has the mic open.
struct UsbAudioAdcYield {
    explicit UsbAudioAdcYield(const char *why) {
        if (usbAudioOwnsAdc) got_ = usb_audio_yield_adc(why);
    }
    ~UsbAudioAdcYield() { usb_audio_resume_adc(); }
    // False when the stream would not let go - the caller is NOT looking at the
    // real converter and must not persist anything derived from it.
    bool ok() const { return got_; }
private:
    bool got_ = true;
};
#endif

#else // !USB_AUDIO_ENABLE

// OG / RP2040 build: no audio. Keep the scoped helper so callers need no #if.
#ifdef __cplusplus
struct UsbAudioAdcYield {
    explicit UsbAudioAdcYield(const char *) {}
    bool ok() const { return true; }   // no audio on this board: always the real ADC
};
#endif

#endif // USB_AUDIO_ENABLE
#endif // USB_AUDIO_H
