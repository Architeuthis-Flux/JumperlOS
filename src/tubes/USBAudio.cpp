// USB Audio Class 2.0 microphone - see include/USBAudio.h for the design.
//
// Three pieces: the descriptor toggle (usb_descriptors.cpp picks the variant on
// g_usb_audio_enabled), the class control plane (the tud_audio_* callbacks at
// the bottom), and the capture pump in the middle - which, since T2.1, is a
// CONSUMER of the always-on ADC ring (src/AdcRing.cpp): the ring engine owns
// the ADC and the DMA (memory-safe by hardware ring wrap, no core in the loop),
// and this file only converts sweeps into PCM. The history of why the DMA must
// never depend on a CPU (SWD caught the old ping-pong pointers 443 KB past
// their buffer after a flash-write park) is in CodeDocs/USB_AUDIO_HANDOFF.md;
// the ring engine keeps that rule.

#include "USBAudio.h"

#if USB_AUDIO_ENABLE

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <pico/mutex.h>
#include <pico/multicore.h>
#include <hardware/adc.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/structs/dma.h>

#include "ArduinoStuff.h"
#include "Peripherals.h"
#include "config.h"
#include "configManager.h"
#include "externVars.h"   // core-1 frame hold
#include "AdcRing.h"      // the always-on ADC ring (T2.1): audio is one of its consumers

// The big global USB mutex from the core's TinyUSB port. tud_task() is pumped
// from the USB soft-IRQ as well as from thread context, so cycling the bus
// without holding this races the ISR. (USBfs.cpp:803 does exactly that cycle
// without the mutex - a latent wedge, worth fixing separately.) It is a plain
// namespace-scope global in a C++ TU, so it links unmangled.
extern mutex_t __usb_mutex;

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

// Selects the config descriptor variant. Read from USB IRQ context by
// tud_descriptor_configuration_cb, written only from thread context.
static volatile bool g_usb_audio_enabled = false;

// Set once the stream owns the ADC; read by readAdc()/updateLazyAdcReadings().
volatile bool usbAudioOwnsAdc = false;

// The class callbacks run in USB IRQ context, where spinning on readingADC for
// up to 100 ms would be catastrophic. They only raise these; serviceUSBAudio()
// on core 1 does the real work.
static volatile bool g_startRequested = false;
static volatile bool g_stopRequested  = false;
static volatile bool g_streaming      = false;

// What the host currently has selected on the AudioStreaming interface: 1 while
// it has the microphone open, 0 otherwise. Every "resume capture" decision
// (after a probe pause, after a config save, after a self test or calibration
// hands the ADC back) checks this rather than assuming the host is still listening -
// restarting capture for a host that closed the mic would claim the ADC and
// never release it.
static volatile uint8_t g_hostAlt = 0;

// Streaming rate. 16 kHz by default rather than 48: the ADC is a single
// mux'd converter shared with the OLED voltage cache, probe sensing and supply
// sense, and running it flat out for audio starved all of them. 16 kHz is still
// well past what a breadboard signal needs and leaves the converter ~3x more
// headroom. Changing this changes what the clock entity reports, so the host
// resamples; it does not need a descriptor rebuild.
#define JL_AUDIO_DEFAULT_RATE 16000u
static uint32_t g_rateHz = JL_AUDIO_DEFAULT_RATE;
// Set when a rate change arrives while the host has the mic open; applied by
// the pump at the next stop. 0 = nothing pending. See usb_audio_set_rate().
static volatile uint32_t g_pendingRateHz = 0;

// (The round-robin mask, per-ms burst sizing and the ADC clock divider all
// moved to src/AdcRing.cpp with T2.1: the ring engine sweeps every channel at
// 48 kHz whether or not audio is streaming; audio decimates to its rate.)

// T2.1: the ADC is the always-on ring's (src/AdcRing.cpp) - free-running
// round-robin over all eight inputs at 48 kHz per channel into an 8 KB SRAM
// ring by DMA, one block IRQ per millisecond. Audio is one CONSUMER of those
// sweeps: the ring's block IRQ (core 0) calls usbAudioOnRingBlock() with the
// halfword total, and this side walks the sweeps it has not seen yet, takes
// its two channels (mean over `g_decim` sweeps per frame - the stream rate
// must divide 48 000: 48/24/16/12/9.6/8 k), converts, DC-blocks and pushes
// PCM into the SPSC ring below. No ADC or DMA ownership, no probe pause, no
// sentinel channels, no resync dance: the probe and every other reader read
// the same ring at the same time. (Kevin, 2026-08-18: audio is a niche
// feature; the pads take precedence - if this ever fights the probe, it is
// audio that gives.)
static volatile uint32_t g_ringTail16 = 0;   // halfword total consumed so far (sweep-aligned)
static uint32_t g_decim = 3;                 // sweeps per frame = 48000 / rate

// Channels 5 and 7 are the probe's pad-sense and tip inputs; keep them out of
// the audio pair (a finger on the pads is not a signal to record).
static inline bool usbAudioChannelStreamable(int ch) {
    return ch >= 0 && ch <= 7 && ch != 5 && ch != 7;
}

// Channel selection and the derived fixed-point conversion terms, latched at
// stream start so a mid-stream recalibration can't tear them.
static uint8_t g_chL = 0, g_chR = 1;
static int32_t g_zeroCounts[2] = { 0, 0 };
static int32_t g_gainQ16[2]    = { 0, 0 };
static bool    g_dcBlock = true;
static float   g_fullScaleVolts = 8.0f;

// One-pole DC blocker state, Q15. Hosts dislike a standing DC offset and the
// +-8V channels sit at ~1792 counts for 0V, so this is on by default.
static int32_t g_hpX1[2], g_hpY1[2];

// Lock-free SPSC ring carrying converted PCM from the capture side to the USB
// side: producer = the ring's block IRQ (core 0), consumer =
// tud_audio_tx_done_isr (core 0, USB context). Calling tud_audio_write()
// straight from a DMA IRQ took the whole board off the bus once (the FIFO
// mutex - the same cross-core deadlock custom_tusb_config.h documents for
// CDC), hence the indirection. Power-of-two so the wrap is a mask.
#define JL_AUDIO_RING_SAMPLES 1024u   // ~10 ms of stereo headroom at 48 k, 32 ms at the 16 k default (SRAM is the constraint - audio is niche)
#define JL_AUDIO_RING_MASK    (JL_AUDIO_RING_SAMPLES - 1u)
static int16_t g_ring[JL_AUDIO_RING_SAMPLES];
static volatile uint32_t g_ringHead = 0;   // producer index
static volatile uint32_t g_ringTail = 0;   // consumer index

// Rolling means of the two streamed channels (status / legacy readers).
static volatile int32_t g_lastMean[2] = { 0, 0 };

// Runtime health counters surfaced through usb_audio_status(). These are the
// things that tell you whether a recording is clean without listening to it.
static volatile uint32_t g_statFramesSent  = 0;   // stereo frames handed to TinyUSB
static volatile uint32_t g_statFifoFull    = 0;   // ring full: host stalled
static volatile uint32_t g_statAdcOver     = 0;   // (ring engine's overruns, mirrored)
static volatile uint32_t g_statLateIrq     = 0;   // producer fell more than the ADC ring behind: skipped ahead
static volatile uint32_t g_statResyncs     = 0;   // stream (re)starts
static volatile uint32_t g_statProbePauses = 0;   // (none since T2.1)
static volatile uint32_t g_statClaimFail   = 0;   // start declined: the ring engine is not running
static volatile uint8_t  g_initFail        = 0;   // 0 ok (the ring engine reports its own)

extern "C" bool usb_audio_device_enabled(void) { return g_usb_audio_enabled; }
extern "C" bool usb_audio_is_streaming(void)   { return g_streaming; }

//--------------------------------------------------------------------+
// Sample conversion
//--------------------------------------------------------------------+
// readAdcVoltage() computes volts = raw*(adcSpread[ch]/4095) - adcZero[ch],
// skipping the offset for channels 4 and 5. Inverting that gives the raw code
// sitting at 0 V, which for the +-8V channels is ~1792 - NOT mid-scale 2048.
// adcSpread[]/adcZero[] are reloaded from config at runtime, so these are
// cached per stream rather than hardcoded.
static void usbAudioCacheCal(int ch, int slot) {
    const float spread = adcSpread[ch];
    const float zeroV  = (ch == 4 || ch == 5) ? 0.0f : adcZero[ch];
    g_zeroCounts[slot] = (int32_t) lroundf(zeroV * 4095.0f / spread);
    g_gainQ16[slot]    = (int32_t) lroundf(32767.0f * spread * 65536.0f
                                           / (4095.0f * g_fullScaleVolts));
}

// The multiply MUST be 64-bit: (raw - zero) reaches +-2303 and gainQ16 is
// ~1.2e6 for the default 8 V full scale, so the product hits 2.76e9 and would
// wrap an int32. Cortex-M33 does this in one SMULL, and narrowing it to Q15
// would overflow outright for a small user-supplied full_scale.
static inline int16_t __not_in_flash_func(usbAudioConvert)(uint16_t raw12, int slot) {
    int32_t d = (int32_t)(raw12 & 0x0FFF) - g_zeroCounts[slot];
    int32_t s = (int32_t)(((int64_t) d * g_gainQ16[slot]) >> 16);
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t) s;
}

// y[n] = x[n] - x[n-1] + a*y[n-1] with a = 1 - 2^-9, i.e. fc ~= 15 Hz at 48 kHz.
static inline int16_t __not_in_flash_func(usbAudioDcBlock)(int16_t s, int slot) {
    int32_t xn = (int32_t) s << 15;
    int32_t yn = xn - g_hpX1[slot] + g_hpY1[slot] - (g_hpY1[slot] >> 9);
    g_hpX1[slot] = xn;
    g_hpY1[slot] = yn;
    int32_t o = yn >> 15;
    if (o >  32767) o =  32767;
    if (o < -32768) o = -32768;
    return (int16_t) o;
}

// The ring's block IRQ (core 0, once per ms) hands us the halfword total.
// Walk complete sweeps from our tail, one frame per g_decim sweeps. Bounded
// per call; if we fell more than the ADC ring holds behind (a flash write
// parked the IRQ), skip to the newest whole ring rather than read overwritten
// history as audio.
extern "C" void __not_in_flash_func(usbAudioOnRingBlock)(uint32_t total) {
    if (!g_streaming) return;
    const volatile uint16_t *ring = adcRingData();
    uint32_t tail = g_ringTail16;
    uint32_t behind = total - tail;
    if (behind > (ADC_RING_HALFWORDS - 512u)) {
        // too far behind: restart just behind the head, on a sweep boundary
        tail = (total - 512u) & ~7u;
        g_statLateIrq++;
    }
    const uint32_t frameHw = 8u * g_decim;             // halfwords per output frame
    int16_t out[2 * 64];
    int nOut = 0;
    int32_t sumL = 0, sumR = 0; int nSum = 0;
    // at most ~4 ms of frames per call, so a catch-up cannot hog the IRQ
    int budget = 4 * 48;
    while ((total - tail) >= frameHw && budget-- > 0) {
        uint32_t l = 0, r = 0;
        for (uint32_t k = 0; k < g_decim; k++) {
            uint32_t base = (tail + 8u * k) & (ADC_RING_HALFWORDS - 1u);
            l += ring[(base + g_chL) & (ADC_RING_HALFWORDS - 1u)] & 0x0FFFu;
            r += ring[(base + g_chR) & (ADC_RING_HALFWORDS - 1u)] & 0x0FFFu;
        }
        l /= g_decim; r /= g_decim;
        sumL += (int32_t)l; sumR += (int32_t)r; nSum++;
        int16_t a = usbAudioConvert((uint16_t)l, 0);
        int16_t b = usbAudioConvert((uint16_t)r, 1);
        if (g_dcBlock) { a = usbAudioDcBlock(a, 0); b = usbAudioDcBlock(b, 1); }
        out[2 * nOut] = a; out[2 * nOut + 1] = b; nOut++;
        tail += frameHw;
        if (nOut == 64) {
            const uint32_t head = g_ringHead, t = g_ringTail;
            const uint32_t n = (uint32_t)(nOut * 2);
            const uint32_t free_n = JL_AUDIO_RING_SAMPLES - 1u - ((head - t) & JL_AUDIO_RING_MASK);
            if (free_n >= n) {
                for (uint32_t i = 0; i < n; i++) g_ring[(head + i) & JL_AUDIO_RING_MASK] = out[i];
                __sync_synchronize();
                g_ringHead = (head + n) & JL_AUDIO_RING_MASK;
            } else {
                g_statFifoFull++;
            }
            nOut = 0;
        }
    }
    if (nOut > 0) {
        const uint32_t head = g_ringHead, t = g_ringTail;
        const uint32_t n = (uint32_t)(nOut * 2);
        const uint32_t free_n = JL_AUDIO_RING_SAMPLES - 1u - ((head - t) & JL_AUDIO_RING_MASK);
        if (free_n >= n) {
            for (uint32_t i = 0; i < n; i++) g_ring[(head + i) & JL_AUDIO_RING_MASK] = out[i];
            __sync_synchronize();
            g_ringHead = (head + n) & JL_AUDIO_RING_MASK;
        } else {
            g_statFifoFull++;
        }
    }
    if (nSum > 0) { g_lastMean[0] = sumL / nSum; g_lastMean[1] = sumR / nSum; }
    g_ringTail16 = tail;
}

//--------------------------------------------------------------------+
// ADC + DMA lifecycle
//--------------------------------------------------------------------+

// Stream start: no ADC or DMA to set up any more - only our own state. The
// ring engine must be running (it is, from setup(); the D-menu A/B toggle can
// stop it, in which case audio declines and counts it).
static bool usbAudioStreamStart(void) {
    if (!adcRingActive()) { g_statClaimFail++; return false; }
    if (g_rateHz < 8000u || g_rateHz > 48000u || (ADC_RING_SWEEP_HZ % g_rateHz) != 0) return false;
    g_decim = ADC_RING_SWEEP_HZ / g_rateHz;
    usbAudioCacheCal(g_chL, 0);
    usbAudioCacheCal(g_chR, 1);
    g_hpX1[0] = g_hpY1[0] = g_hpX1[1] = g_hpY1[1] = 0;
    g_ringHead = g_ringTail = 0;
    g_ringTail16 = adcRingSweeps() << 3;   // start from now
    __sync_synchronize();
    g_streaming = true;
    g_statResyncs++;
    return true;
}

// Nothing to claim any more: kept for the callers (usb_audio_set_device_enabled).
extern "C" bool usbAudioInit(void) { return true; }

static void usbAudioStreamStop(void) {
    g_streaming = false;
    __sync_synchronize();
}

// (was: hand the ADC back to the rest of the board) - the stream just stops;
// a pending rate change is adopted here, the one safe moment.
static void usbAudioReleaseAdc(void) {
    usbAudioStreamStop();
    if (g_pendingRateHz != 0) {
        g_rateHz = g_pendingRateHz;
        g_pendingRateHz = 0;
        usb_audio_sync_config();
    }
    __sync_synchronize();
    usbAudioOwnsAdc = false;
}


//--------------------------------------------------------------------+
// Persistence
//--------------------------------------------------------------------+

// Restore the saved audio setup at BOOT. This is the answer to "can I have the
// mic without dropping my serial ports?": USB gives no way to add an interface
// after enumeration, but if the flag is already set when the host first
// enumerates us, there is nothing to drop. Must run BEFORE the USB stack comes
// up so the very first GET_DESCRIPTOR already reports the audio function.
extern "C" void usb_audio_apply_config(void) {
    g_chL            = (uint8_t) jumperlessConfig.usb_audio.left;
    g_chR            = (uint8_t) jumperlessConfig.usb_audio.right;
    g_rateHz         = (uint32_t) jumperlessConfig.usb_audio.rate;
    g_fullScaleVolts = jumperlessConfig.usb_audio.full_scale;
    g_dcBlock        = jumperlessConfig.usb_audio.dc_block;

    // config.txt is a hand-editable text file, so every field here has to be
    // treated as untrusted. full_scale especially: it is a DIVISOR in
    // usbAudioCacheCal(), so a stray "full_scale = 0;" would divide by zero and
    // feed lroundf(inf) - undefined behaviour, and the gain it produces then
    // corrupts every sample. Mirror the same limits usb_audio_set_full_scale()
    // enforces, rather than trusting the file.
    if (g_chL == g_chR || !usbAudioChannelStreamable(g_chL) ||
        !usbAudioChannelStreamable(g_chR)) { g_chL = 0; g_chR = 1; }
    if (g_rateHz < 8000u || g_rateHz > 48000u || (g_rateHz % 1000u)) g_rateHz = JL_AUDIO_DEFAULT_RATE;
    if (!(g_fullScaleVolts >= 0.05f) || g_fullScaleVolts > 20.0f) g_fullScaleVolts = 8.0f;

    // Record the desired state. Do NOT cycle the bus from here: this runs deep
    // inside config loading and is not a safe place to be disconnecting USB.
    g_usb_audio_enabled = jumperlessConfig.usb_audio.enabled;
    // Write the clamped values back, so `~config` and config.txt report what the
    // hardware will actually do rather than the rejected value from the file.
    usb_audio_sync_config();
}

// Make the host's view match the restored config.
//
// The arduino-pico core brings USB up BEFORE setup() runs, so by the time the
// config file has been read the host has already fetched the base configuration
// descriptor - setting the flag in usb_audio_apply_config() alone is too late
// and the mic silently never appears (the give-away is bcdDevice reading 0x0100
// on the host while the firmware believes audio is enabled). Cycle the bus once
// here instead. At boot nothing is attached yet, so this costs nothing visible,
// unlike toggling the mic later in a session.
extern "C" void usb_audio_boot_enumerate(void) {
    if (!g_usb_audio_enabled) return;
    g_usb_audio_enabled = false;              // so the call below does real work
    usb_audio_set_device_enabled(true);
}

// Persist the current setup so the next boot comes up this way with no
// re-enumeration at all.
// Mirror the live g_* state into jumperlessConfig so the struct, the terminal's
// `~config` dump and config.txt all agree with what the hardware is doing.
// Without this the live setters moved g_* only, usb_audio_save_config() copied
// g_* over the struct, and a value set through the config surface was silently
// reverted on disk by the next Ms.
extern "C" void usb_audio_sync_config(void) {
    jumperlessConfig.usb_audio.enabled    = g_usb_audio_enabled;
    jumperlessConfig.usb_audio.left       = g_chL;
    jumperlessConfig.usb_audio.right      = g_chR;
    jumperlessConfig.usb_audio.rate       = (int) (g_pendingRateHz ? g_pendingRateHz : g_rateHz);
    jumperlessConfig.usb_audio.full_scale = g_fullScaleVolts;
    jumperlessConfig.usb_audio.dc_block   = g_dcBlock;
}

extern "C" void usb_audio_save_config(void) {
    // Stop capture across the write. The DMA is memory-safe through a flash
    // write now (ring-wrapped halves), but the recording would still be a torn
    // mess across the masked window and the resync afterwards; a clean pause
    // and restart is a shorter, quieter gap.
    const bool wasStreaming = g_streaming;
    if (wasStreaming) usb_audio_yield_adc("saving config to flash");

    usb_audio_sync_config();
    saveConfig();
    // Let the pump pick capture back up if the host still has the mic open.
    if (wasStreaming) usb_audio_resume_adc();
}

//--------------------------------------------------------------------+
// Enumeration control
//--------------------------------------------------------------------+

extern "C" bool usb_audio_set_device_enabled(bool on) {
    if (on == g_usb_audio_enabled) return true;

    // Acquire the DMA channels and IRQ handler here, on core 0 in thread
    // context, the first time audio is switched on. NOT at boot (too early in
    // setup() to be touching the DMA allocator) and NOT from the core1 pump,
    // where dma_claim_unused_channel(true) panicking would silently stop the
    // core - which is exactly the failure this replaced.
    if (on && !usbAudioInit()) {
        Serial.printf("\n\r[usb_audio] cannot start: %s\n\r",
                      g_initFail == 1 ? "no free DMA channels"
                                      : "DMA_IRQ_1 already has a handler");
        return false;
    }

    if (g_streaming) usb_audio_yield_adc("re-enumerating");
    g_hostAlt = 0;   // the host's view of the interface is about to be torn down

    // Warn on every CDC, because we are about to drop whichever one this call
    // arrived on and the return value goes down with it.
    const char *msg = on
        ? "\n\r[usb_audio] re-enumerating WITH audio device - this port drops and returns in ~2s\n\r"
        : "\n\r[usb_audio] re-enumerating WITHOUT audio device - this port drops and returns in ~2s\n\r";
    Serial.print(msg);   USBSer1.print(msg);
    USBSer2.print(msg);  USBSer3.print(msg);
    Serial.flush();      USBSer1.flush();
    USBSer2.flush();     USBSer3.flush();

    // flush() only hands bytes to TinyUSB; the device task is what puts them on
    // the wire. delay() yields into the core's mutex-guarded TinyUSB pump, so
    // pump through that rather than a bare tud_task() (which would race the
    // USB soft-IRQ's own tud_task() on the same state).
    delay(50);

    g_usb_audio_enabled = on;   // takes effect on the next GET_DESCRIPTOR

    mutex_enter_blocking(&__usb_mutex);
    tud_disconnect();
    // Longer than the 500 ms the core itself uses: hosts want a clean "gone"
    // before a different interface layout shows up under the same VID/PID.
    // sleep_ms rather than delay so no Arduino yield() re-enters USB here.
    sleep_ms(600);
    tud_connect();
    mutex_exit(&__usb_mutex);

    // Let the host finish enumerating before we hand control back to a REPL
    // that is about to print a prompt into a port which does not exist yet.
    // tud_mounted() still reports the OLD configuration until the host's bus
    // reset lands, so wait for it to drop and then come back, bounded.
    bool sawReset = false;
    for (uint32_t t0 = millis(); millis() - t0 < 1500; ) {
        delay(2);
        if (!tud_mounted()) sawReset = true;
        else if (sawReset) break;
    }
    return true;
}

//--------------------------------------------------------------------+
// ADC ownership
//--------------------------------------------------------------------+

// Nothing to yield since T2.1: the ring is everyone's. Kept for the callers
// (self test, DAC calibration, config saves) that bracket their reads with it.
extern "C" bool usb_audio_yield_adc(const char *why) {
    (void) why;
    return true;
}

extern "C" void usb_audio_resume_adc(void) {
    if (g_usb_audio_enabled && g_hostAlt == 1 && !g_streaming) g_startRequested = true;
}

extern "C" void usb_audio_probe_activity(void) {
    // Nothing to do since T2.1: the probe and the stream read the same ring.
}

extern "C" void serviceUSBAudio(void) {
    // Single-entry, cheap: uncontended in the normal path.
    static volatile bool svcBusy = false;
    if (__atomic_test_and_set(&svcBusy, __ATOMIC_ACQUIRE)) return;
    struct SvcGuard {
        volatile bool *f;
        ~SvcGuard() { __atomic_clear(f, __ATOMIC_RELEASE); }
    } guard{ &svcBusy };

    if (g_stopRequested) {
        g_stopRequested = false;
        if (g_streaming) usbAudioReleaseAdc();
    }
    if (g_startRequested) {
        g_startRequested = false;
        if (!g_streaming && g_usb_audio_enabled) {
            if (!usbAudioStreamStart()) {
                // the ring engine is off (A/B toggle) or the rate does not
                // divide 48 k: the host hears silence, the counter says why
            }
        }
    }
    // The ring engine's overruns, mirrored into the audio status.
    AdcRingStats rs; adcRingGetStats(&rs);
    g_statAdcOver = rs.overruns;
}

//--------------------------------------------------------------------+
// Cooperation with the rest of the ADC users
//--------------------------------------------------------------------+

// Serve readAdc() from the sweep while we own the converter. For the streamed
// pair this is a 48-sample mean, so it is actually a better reading than
// readAdc(ch, 8); the housekeeping channels get their own per-sweep means.
//
// The two PROBE channels are the exception. ADC5 (pad-sense ladder) and ADC7
// (tip) are read by a drive->settle->sample protocol that a rolling mean can't
// serve - a smeared value fed into the row decoder selects the WRONG row. So
// they read as "nothing" here, and a pad that actually has the tip on it
// (sweep mean over the pad threshold) hands the converter to the probe within
// one core-1 pass; the probe's next poll, 10 ms later, gets the real ADC.
// Legacy readers (readAdc()'s sentinel path, the lazy cache) - never taken
// since T2.1: usbAudioOwnsAdc stays false, the ring serves everyone.
extern "C" bool usbAudioSnapshotRaw(int channel, int *raw) {
    (void) channel; (void) raw;
    return false;
}
extern "C" bool usbAudioRefreshLazy(float *readings) {
    (void) readings;
    return false;
}

//--------------------------------------------------------------------+
// Configuration
//--------------------------------------------------------------------+

// NOTE: these re-latch the stream in place (stop/start of our own state - no
// ADC or DMA involved since T2.1). They are invoked from MicroPython or the
// console on core 0, while core1 owns the DMA IRQ and the service pump -
// tearing the DMA down from a second core would let the IRQ fire into a
// half-configured channel. Setting the flag lets core1 do the restart in the
// one place that is allowed to.
extern "C" bool usb_audio_set_channels(int left, int right) {
    if (!usbAudioChannelStreamable(left) || !usbAudioChannelStreamable(right) ||
        left == right) return false;
    g_chL = (uint8_t) left;
    g_chR = (uint8_t) right;
    if (g_streaming) { usbAudioStreamStop(); usbAudioStreamStart(); }   // re-latch the pair and its calibration
    usb_audio_sync_config();
    return true;
}

// Changing the rate mid-stream restarts capture; the host is told the new
// value through the clock entity on its next query.
extern "C" bool usb_audio_set_rate(uint32_t hz) {
    if (hz < 8000u || hz > 48000u || (hz % 1000u) != 0u) return false;
    // DEFERRED while the host has the interface open. TinyUSB sizes every
    // isochronous IN packet from the rate the HOST negotiated via SET_CUR
    // SAM_FREQ, not from g_rateHz - so moving this mid-stream left capture at
    // the new rate and framing at the old one (16k->48k overruns the endpoint
    // FIFO, 48k->16k sends near-silence). Worse, tud_audio_set_req_entity_cb()
    // answers a SET_CUR with "want == g_rateHz", so a host that cached the
    // clock RANGE at attach gets STALLED on the very rate it negotiated.
    // Applying it at the next stop keeps device and host in agreement, and the
    // host re-reads the clock entity when it next opens the mic.
    if (g_streaming || g_hostAlt == 1) {
        // Asking for the rate already in use is not a change - don't leave a
        // pending value that status would report as a queued change.
        g_pendingRateHz = (hz == g_rateHz) ? 0u : hz;
        usb_audio_sync_config();
        return true;
    }
    g_rateHz = hz;
    g_pendingRateHz = 0;
    usb_audio_sync_config();
    return true;
}

extern "C" bool usb_audio_set_full_scale(float volts) {
    // >= not >, to match the clamp in usb_audio_apply_config() and both error
    // strings: exactly 0.05 was accepted from config.txt but rejected from a
    // script, which is the sort of inconsistency that wastes an afternoon.
    if (!(volts >= 0.05f) || volts > 20.0f) return false;
    g_fullScaleVolts = volts;
    // Recompute in place; the IRQ only ever reads these, and a torn update just
    // costs one sample at the old gain.
    usbAudioCacheCal(g_chL, 0);
    usbAudioCacheCal(g_chR, 1);
    usb_audio_sync_config();
    return true;
}

extern "C" void usb_audio_set_dc_block(bool on) {
    g_dcBlock = on;
    g_hpX1[0] = g_hpY1[0] = g_hpX1[1] = g_hpY1[1] = 0;
    g_ringHead = g_ringTail = 0;
    usb_audio_sync_config();
}

extern "C" void usb_audio_get_status(usb_audio_status_t *out) {
    if (out == NULL) return;
    out->enabled       = g_usb_audio_enabled;
    out->streaming     = g_streaming;
    out->host_open     = (g_hostAlt == 1);
    out->left_ch       = g_chL;
    out->right_ch      = g_chR;
    out->full_scale    = g_fullScaleVolts;
    out->dc_block      = g_dcBlock;
    out->sample_rate   = g_rateHz;
    out->pending_rate  = g_pendingRateHz;
    out->frames_sent   = g_statFramesSent;
    out->fifo_overflow = g_statFifoFull;
    out->adc_overrun   = g_statAdcOver;
    out->late_irq      = g_statLateIrq;
    out->resyncs       = g_statResyncs;
    out->probe_pauses  = g_statProbePauses;
    out->claim_fail    = g_statClaimFail;
    out->init_fail     = g_initFail;
}

//--------------------------------------------------------------------+
// TinyUSB audio class callbacks
//--------------------------------------------------------------------+
// All of these are TU_ATTR_WEAK in audio_device.c, so these strong definitions
// win at link time. No --wrap needed: unlike MSC, the Adafruit library ships no
// competing Adafruit_USBD_Audio.cpp.

// Host selected an AudioStreaming alt setting. alt 1 = start capture, 0 = stop.
// This is the ONLY thing that starts the ADC - opening the input device in
// QuickTime or Audacity is what turns the microphone on.
extern "C" bool tud_audio_set_itf_cb(uint8_t rhport,
                                     tusb_control_request_t const *p_request) {
    (void) rhport;
    const uint8_t alt = tu_u16_low(p_request->wValue);
    g_hostAlt = alt;
    if (alt == 1) { g_startRequested = true; g_stopRequested = false; }
    else          { g_stopRequested  = true; g_startRequested = false; }
    return true;
}

extern "C" bool tud_audio_set_itf_close_ep_cb(uint8_t rhport,
                                              tusb_control_request_t const *p_request) {
    (void) rhport; (void) p_request;
    g_hostAlt = 0;
    g_stopRequested = true;
    g_startRequested = false;
    return true;
}

// Bus went away (unplug, host suspend, or a bus reset). TinyUSB's audiod_reset()
// does NOT call tud_audio_set_itf_close_ep_cb, so without these the stop request
// is never raised: g_streaming and usbAudioOwnsAdc stay set and the global
// readingADC lock is held FOREVER. Every ADC consumer on the board - probing,
// measure mode, self test, the OLED cache - would stay degraded until a reboot,
// just from unplugging the board while recording.
extern "C" void tud_umount_cb(void) {
    g_hostAlt = 0;
    g_stopRequested = true;
    g_startRequested = false;
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    g_hostAlt = 0;
    g_stopRequested = true;
    g_startRequested = false;
}

// Clock entity queries. The host will not open the device without these.
extern "C" bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                            tusb_control_request_t const *p_request) {
    (void) rhport;
    audio20_control_request_t const *req = (audio20_control_request_t const *) p_request;
    if (req->bEntityID != JL_AUDIO_ID_CLOCK) return false;

    if (req->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO20_CS_REQ_CUR) {
            audio20_control_cur_4_t cur = { .bCur = (int32_t) tu_htole32(g_rateHz) };
            return tud_control_xfer(rhport, p_request, &cur, sizeof(cur));
        }
        if (req->bRequest == AUDIO20_CS_REQ_RANGE) {
            audio20_control_range_4_n_t(1) rng = {
                .wNumSubRanges = tu_htole16(1),
                .subrange = { { .bMin = (int32_t) tu_htole32(g_rateHz),
                                .bMax = (int32_t) tu_htole32(g_rateHz),
                                .bRes = 0 } }
            };
            return tud_control_xfer(rhport, p_request, &rng, sizeof(rng));
        }
    }

    if (req->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID &&
        req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t cur = { .bCur = 1 };
        return tud_control_xfer(rhport, p_request, &cur, sizeof(cur));
    }

    return false;   // stall anything else
}

// We advertise the sample frequency as host-programmable (bmControls 0x07 in
// the descriptor) because advertising read-only and then stalling SET_CUR is a
// known Windows stream-init breaker. There is only one legal value, so accept
// it and ignore anything else.
extern "C" bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                            tusb_control_request_t const *p_request,
                                            uint8_t *pBuff) {
    (void) rhport;
    audio20_control_request_t const *req = (audio20_control_request_t const *) p_request;
    if (req->bEntityID == JL_AUDIO_ID_CLOCK &&
        req->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ &&
        req->bRequest == AUDIO20_CS_REQ_CUR) {
        const uint32_t want = tu_le32toh(*(uint32_t const *) pBuff);
        return want == g_rateHz;
    }
    return false;
}

#endif // USB_AUDIO_ENABLE
