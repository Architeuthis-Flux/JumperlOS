// USB Audio Class 2.0 microphone - see include/USBAudio.h for the design.
//
// Three pieces: the descriptor toggle (usb_descriptors.cpp picks the variant on
// g_usb_audio_enabled), the class control plane (the tud_audio_* callbacks at
// the bottom), and the ADC->DMA->ring->TinyUSB capture pump in the middle.
//
// THE ONE RULE FOR THE CAPTURE PATH: the DMA must stay memory-safe with NO help
// from a CPU. Core 1 owns the completion IRQ, and core 1 is parked with
// interrupts masked (rp2040.idleOtherCore(), inside the doorbell IRQ) for every
// flash erase/program - tens of ms per sector. On RP2350 a chain-triggered
// channel reloads TRANS_COUNT but keeps its current WRITE_ADDR, so two channels
// chained to each other keep ping-ponging without the IRQ, and if that IRQ was
// the only thing resetting the write pointer they walk it straight out of the
// buffer. That is exactly what happened: SWD caught the pointers 443 KB past
// g_half[], through .bss and into core 0's stack, one jfs write after streaming
// started (see CodeDocs/USB_AUDIO_HANDOFF.md). The fix is hardware: each half is
// a power-of-two, aligned region and the channel's write address is ring-wrapped
// to it, so a late IRQ costs a glitch, never memory.

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
#include "externVars.h"   // pauseCore2

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

// Sweep EVERY ADC channel, not just the two being streamed. The audio pair is
// whatever the user picked; the rest ride along and are demuxed straight into
// adcReadings[], which is what the OLED, probe sensing, supply sense and
// adc_get() all read. Covering all 8 removes an entire bug class - otherwise
// any channel outside the sweep silently reads 0 for the whole recording, and
// which channels those are would shift with the user's choice of audio pair.
//
// Cost is only ADC bandwidth: 8 channels at the default 16 kHz is 128 ksps
// against the converter's ~500 ksps ceiling, i.e. ~7.8 us per conversion, which
// is comfortably above its settling requirement. Note that at the maximum
// 48 kHz the sweep hits 384 ksps and per-channel settling gets tight, so the
// non-audio readings are less accurate there - another reason 16 kHz is the
// default.
#define JL_AUDIO_HOUSEKEEP_MASK 0xFFu

static uint8_t  g_rrMask   = 0;    // every channel in the round-robin
static uint8_t  g_nChan    = 0;    // popcount(g_rrMask)
static uint8_t  g_slotOf[8];       // ADC channel -> position within one sweep
static uint16_t g_perMs    = 0;    // conversions per millisecond (all channels)
static uint16_t g_framesMs = 0;    // stereo audio frames per millisecond

// One USB full-speed frame is 1 ms = 48 stereo frames = 96 conversions.
// Sizing the DMA half-buffers to exactly that keeps the capture cadence and the
// isochronous cadence in lockstep, so each IRQ produces exactly one packet.
// Worst case: 48 kHz x 8 channels = 384 conversions per millisecond.
#define JL_AUDIO_MAX_PER_MS     384
#define JL_AUDIO_FRAMES_PER_MS  48
#define JL_AUDIO_SAMPLES_PER_MS (JL_AUDIO_FRAMES_PER_MS * 2)

// clk_adc is the SDK default 48 MHz and the ADC period is (1 + DIV.INT) clocks,
// so 499 - not 500 - gives exactly 96.000 ksps round-robin = 48.000 kHz per
// channel. 500 would give 48e6/501 = 95808 sps, a -0.2% error that shows up as
// an audible artefact every few seconds.
#define JL_AUDIO_ADC_CLKDIV     499.0f

// Capture buffers. Must be plain SRAM (never PSRAM) and the conversion path
// must stay out of flash: the ISO endpoint is single-buffered on this DCD, so
// there is a hard 1 ms refill deadline and any XIP stall drops a frame.
//
// Each half is a 1 KB region, 1 KB-ALIGNED, and the DMA channel filling it has
// its write address RING-WRAPPED to that region (channel_config_set_ring). This
// is what makes a late or missing completion IRQ harmless: the chain keeps the
// channels ping-ponging, but every write lands inside its own half. Without the
// wrap the pointer marched through all of SRAM the first time core 1 was parked
// for a flash write (see the file header).
#define JL_AUDIO_HALF_BYTES     1024u
#define JL_AUDIO_HALF_RING_BITS 10u
static_assert((1u << JL_AUDIO_HALF_RING_BITS) == JL_AUDIO_HALF_BYTES, "ring bits vs half size");
static_assert(JL_AUDIO_MAX_PER_MS * 2u <= JL_AUDIO_HALF_BYTES, "one burst must fit in a half");
static uint16_t __attribute__((aligned(JL_AUDIO_HALF_BYTES))) g_half[2][JL_AUDIO_HALF_BYTES / 2u];
static int  g_dmaCh[2] = { -1, -1 };

// Channels 5 and 7 are the probe's pad-sense and tip inputs. Streaming them is
// self-defeating: ADC5 crossing the pad threshold stamps probe activity, which
// pauses the very capture that is reading it, every 300 ms forever. They are
// also the two channels served as a sentinel rather than a mean, so the audio
// would be silence anyway.
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
// side. This exists because the two live on DIFFERENT CORES and must never
// share a lock: the DMA completion IRQ runs on core1, while TinyUSB's software
// FIFO is owned by core0's USB IRQ. Calling tud_audio_write() straight from the
// DMA IRQ took the whole board off the bus - core1 blocked on the FIFO mutex
// while core0 held it, the same cross-core deadlock custom_tusb_config.h
// documents for CDC. Producer: usbAudioDmaIrq (core1). Consumer:
// tud_audio_tx_done_isr (core0, USB context). Power-of-two so the wrap is a mask.
#define JL_AUDIO_RING_SAMPLES 1024u   // ~5.3 ms of stereo headroom
#define JL_AUDIO_RING_MASK    (JL_AUDIO_RING_SAMPLES - 1u)
static int16_t g_ring[JL_AUDIO_RING_SAMPLES];
static volatile uint32_t g_ringHead = 0;   // producer index, core1 only
static volatile uint32_t g_ringTail = 0;   // consumer index, core0 only

// Rolling mean of the last completed half, so adc_get() keeps returning real
// data for the two streamed channels instead of the 0 that every other channel
// gets while the ADC is ours.
static volatile int32_t g_lastMean[2] = { 0, 0 };
// Per-channel means for the housekeeping channels, in raw ADC counts.
static volatile int32_t g_hkMean[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// Runtime health counters surfaced through usb_audio_status(). These are the
// things that tell you whether a recording is clean without listening to it.
static volatile uint32_t g_statFramesSent  = 0;   // stereo frames handed to TinyUSB
static volatile uint32_t g_statFifoFull    = 0;   // ring full: host stalled
static volatile uint32_t g_statAdcOver     = 0;   // ADC FIFO overran (forces a resync)
static volatile uint32_t g_statLateIrq     = 0;   // completion IRQ arrived after the sibling chained back
static volatile uint32_t g_statResyncs     = 0;   // capture restarts (overrun / late IRQ / reconfigure)
static volatile uint32_t g_statProbePauses = 0;   // times capture yielded to the probe
static volatile uint32_t g_statClaimFail   = 0;   // start declined: readingADC held by someone else
static volatile bool     g_needResync      = false;

// Probe arbitration. The probe drives a crosspoint, waits 5-60 us and reads -
// a protocol two orders of magnitude faster than our 1 ms capture average, so
// serving it from the sweep returns values that lag across many probe states
// and make readings jump. It needs the real converter. Rather than making the
// two mutually exclusive, capture yields while the probe is IN USE and resumes
// once it has been quiet for PROBE_HOLD. The host hears a gap while probing.
//
// "In use" is an explicit signal - usb_audio_probe_activity(), stamped by the
// probe/measure code when it has actually decoded a touch - NOT "any read of
// the probe ADC channels". The idle loop polls the pad-sense channel at 100 Hz
// whether or not anything is touching it, so inferring from reads paused
// capture forever and stop/started the DMA every 300 ms.
#define JL_AUDIO_PROBE_HOLD_US 300000u
static volatile uint32_t g_probeReqUs    = 0;    // 0 = no recent activity
static volatile bool     g_pausedForProbe = false;

static volatile uint8_t  g_initFail    = 9;   // 0 ok, 1 no DMA channels, 2 DMA_IRQ_1 taken, 9 not tried
static bool              g_dmaInitDone = false;

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

// no-tree-loop-distribute-patterns: keep GCC from turning the small zeroing
// loop below into a memset() call - memset lives in flash, and this handler
// must stay RAM-only.
static void __attribute__((optimize("no-tree-loop-distribute-patterns")))
__not_in_flash_func(usbAudioDmaIrq)(void) {
    for (int k = 0; k < 2; k++) {
        const int ch = g_dmaCh[k];
        if (ch < 0 || !(dma_hw->ints1 & (1u << ch))) continue;
        dma_hw->ints1 = (1u << ch);   // write-1-clear

        // If this channel is ALREADY running again, its sibling completed and
        // chained back before we got here: this IRQ is late (core 1 was parked
        // for a flash write, or something masked interrupts for >1 ms). The
        // ring wrap kept every write inside g_half[k], so memory is fine, but
        // the buffer holds a torn mix of two bursts and the ADC round-robin
        // phase relative to the buffer start is no longer known. Publish
        // nothing and ask the pump for a clean restart.
        if (dma_hw->ch[ch].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
            g_statLateIrq++;
            g_needResync = true;
        }
        // While a resync is pending the channel<->slot mapping of the buffer is
        // unknown, so neither the audio nor the housekeeping means may be
        // taken from it - a rotated interleave would hand readAdc() another
        // channel's voltage. Just keep the DMA armed and wait for the pump.
        if (g_needResync) {
            dma_channel_set_write_addr(ch, g_half[k], false);
            dma_channel_set_trans_count(ch, g_perMs, false);
            continue;
        }

        const uint16_t *src = g_half[k];
        const int frames = g_framesMs;
        const int nch    = g_nChan;
        int16_t out[JL_AUDIO_SAMPLES_PER_MS];
        int32_t sum0 = 0, sum1 = 0;

        // One "sweep" is one pass over every channel in the round-robin mask,
        // in ascending channel order. Audio takes its two slots; the rest are
        // averaged into adcReadings[] so the OLED and probe stay live.
        int32_t hkSum[8];
        for (int c = 0; c < 8; c++) hkSum[c] = 0;
        const int sl = g_slotOf[g_chL], sr = g_slotOf[g_chR];

        for (int f = 0; f < frames; f++) {
            const uint16_t *sweep = &src[f * nch];
            const uint16_t l = sweep[sl] & 0x0FFF;
            const uint16_t r = sweep[sr] & 0x0FFF;
            sum0 += l;
            sum1 += r;

            int16_t a = usbAudioConvert(l, 0);
            int16_t b = usbAudioConvert(r, 1);
            if (g_dcBlock) { a = usbAudioDcBlock(a, 0); b = usbAudioDcBlock(b, 1); }
            out[2 * f]     = a;
            out[2 * f + 1] = b;

            for (int c = 0; c < 8; c++) {
                if (JL_AUDIO_HOUSEKEEP_MASK & (1u << c)) hkSum[c] += sweep[g_slotOf[c]] & 0x0FFF;
            }
        }

        g_lastMean[0] = sum0 / frames;
        g_lastMean[1] = sum1 / frames;
        for (int c = 0; c < 8; c++) {
            if (JL_AUDIO_HOUSEKEEP_MASK & (1u << c)) g_hkMean[c] = hkSum[c] / frames;
        }

        // Publish into the ring; the USB side drains it. NOTHING TinyUSB
        // here - see the ring's comment for why. Once a resync is pending the
        // interleave may be rotated, so hold the audio until the restart.
        if (g_streaming && !g_needResync) {
            const uint32_t head = g_ringHead;
            const uint32_t tail = g_ringTail;
            const uint32_t n = (uint32_t)(frames * 2);
            const uint32_t free_n = JL_AUDIO_RING_SAMPLES - 1u - ((head - tail) & JL_AUDIO_RING_MASK);
            if (free_n >= n) {
                for (uint32_t i = 0; i < n; i++) {
                    g_ring[(head + i) & JL_AUDIO_RING_MASK] = out[i];
                }
                __sync_synchronize();
                g_ringHead = (head + n) & JL_AUDIO_RING_MASK;
            } else {
                g_statFifoFull++;
            }
        }

        // Re-arm this half for the sibling's chain-back. On RP2350 the chain
        // trigger reloads TRANS_COUNT by itself but resumes at the CURRENT
        // write address, so this reset is what keeps each burst starting at
        // the top of its half; if we are ever too late, the ring wrap above is
        // what keeps the pointer inside it.
        dma_channel_set_write_addr(ch, g_half[k], false);
        dma_channel_set_trans_count(ch, g_perMs, false);
    }

    // An ADC FIFO overrun permanently rotates the L/R interleave - one lost
    // conversion and every later sample lands in the wrong channel. Flag it for
    // core 0 to resync; never restart the ADC from inside the IRQ.
    if (adc_hw->fcs & ADC_FCS_OVER_BITS) {
        adc_hw->fcs |= ADC_FCS_OVER_BITS;   // W1C
        g_statAdcOver++;
        g_needResync = true;
    }
}

//--------------------------------------------------------------------+
// ADC + DMA lifecycle
//--------------------------------------------------------------------+

static bool usbAudioAdcStart(void) {
    // Acquire lazily here rather than trusting a caller to have done it.
    // usb_audio_apply_config() restores a saved-enabled mic at BOOT without
    // going through usb_audio_set_device_enabled(), so this used to run with
    // g_dmaCh[] still -1 and hand dma_channel_configure() an invalid channel,
    // which takes the board off the USB bus at every power-on. Idempotent.
    if (!usbAudioInit()) return false;

    // Round-robin over the two audio channels PLUS the housekeeping channels,
    // so the OLED cache, supply sense and probe tip keep getting real readings
    // instead of going blind for the whole recording.
    g_rrMask = (uint8_t)((1u << g_chL) | (1u << g_chR) | JL_AUDIO_HOUSEKEEP_MASK);
    g_nChan  = (uint8_t) __builtin_popcount(g_rrMask);

    // Round-robin always advances low channel index -> high, so a channel's
    // slot within one sweep is just how many masked channels precede it. This
    // replaces the old two-channel swapLR special case.
    uint8_t slot = 0;
    for (int c = 0; c < 8; c++) {
        g_slotOf[c] = slot;
        if (g_rrMask & (1u << c)) slot++;
    }

    g_framesMs = (uint16_t)(g_rateHz / 1000u);          // audio frames per ms
    g_perMs    = (uint16_t)(g_framesMs * g_nChan);      // conversions per ms
    if (g_perMs == 0 || g_perMs > JL_AUDIO_MAX_PER_MS) return false;

    usbAudioCacheCal(g_chL, 0);
    usbAudioCacheCal(g_chR, 1);
    g_hpX1[0] = g_hpY1[0] = g_hpX1[1] = g_hpY1[1] = 0;
    g_ringHead = g_ringTail = 0;

    const uint8_t first = (uint8_t) __builtin_ctz(g_rrMask);

    adc_run(false);
    adc_fifo_drain();
    // clk_adc is 48 MHz and the ADC period is (1 + DIV.INT) clocks, so the
    // divider is 48e6 / (rate * channels) - 1.
    adc_set_clkdiv((float)(48000000.0f / (float)(g_rateHz * g_nChan)) - 1.0f);
    adc_select_input(first);
    adc_set_round_robin(g_rrMask);
    adc_hw->fcs = ADC_FCS_EN_BITS | ADC_FCS_DREQ_EN_BITS
                | (1u << ADC_FCS_THRESH_LSB)
                | ADC_FCS_OVER_BITS | ADC_FCS_UNDER_BITS;   // W1C the sticky flags

    for (int k = 0; k < 2; k++) {
        dma_channel_config c = dma_channel_get_default_config(g_dmaCh[k]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_ADC);
        channel_config_set_chain_to(&c, g_dmaCh[1 - k]);
        // THE memory-safety line: wrap the write address inside this half.
        channel_config_set_ring(&c, true, JL_AUDIO_HALF_RING_BITS);
        dma_channel_configure(g_dmaCh[k], &c, g_half[k], &adc_hw->fifo,
                              g_perMs, false);
    }
    // Drop any completion left over from the previous stop (aborting a channel
    // completes it) so the handler doesn't fire on a stale half, then enable.
    dma_hw->ints1 = (1u << g_dmaCh[0]) | (1u << g_dmaCh[1]);
    irq_clear(DMA_IRQ_1);
    for (int k = 0; k < 2; k++) dma_channel_set_irq1_enabled(g_dmaCh[k], true);
    irq_set_enabled(DMA_IRQ_1, true);

    // NO tud_audio_write() prefill here. It is a TinyUSB FIFO write, and taking
    // that FIFO's mutex from core1 while core0's USB soft-IRQ may hold it is
    // exactly the deadlock family already documented for CDC in
    // custom_tusb_config.h. The flow controller settles within a few frames on
    // its own; a prefill is an optimisation, not a requirement.
    dma_channel_start(g_dmaCh[0]);
    adc_hw->cs = ADC_CS_EN_BITS
               | ((uint32_t) first    << ADC_CS_AINSEL_LSB)
               | ((uint32_t) g_rrMask << ADC_CS_RROBIN_LSB)
               | ADC_CS_START_MANY_BITS;
    return true;
}

// Claim DMA channels and install the completion handler ONCE, at boot, from
// core 0. Doing it inside the start path meant dma_claim_unused_channel(true)
// (panic-on-exhaustion) and irq_add_shared_handler (panics if installed twice)
// were reachable from core1 mid-session - a panic there kills core1 silently
// while core0 keeps answering, which is exactly how this first presented.
extern "C" bool usbAudioInit(void) {
    if (g_dmaInitDone) return g_dmaCh[0] >= 0;
    g_dmaInitDone = true;

    // NOTHING here may panic. The panic-on-failure forms of these APIs took the
    // whole board off the USB bus twice during bring-up (a panic just halts the
    // calling core - the symptom is the device silently vanishing, with no log
    // because the console went with it). A feature that cannot get its
    // resources must decline, not brick the board.
    //
    // DMA_IRQ_1, not 0: MicroPython's rp2.DMA owns IRQ 0 and masks it while
    // reconfiguring channels, which would swallow ours. Take IRQ 1 exclusively
    // - the shared-handler API was what took the board off the USB bus during
    // bring-up. irq_set_exclusive_handler() hard_asserts if anyone else already
    // owns the line, so look first with the non-panicking getters and decline.
    const irq_handler_t cur = irq_get_exclusive_handler(DMA_IRQ_1);
    if ((cur != NULL && cur != usbAudioDmaIrq) || irq_has_shared_handler(DMA_IRQ_1)) {
        g_initFail = 2;   // DMA_IRQ_1 belongs to someone else
        return false;
    }

    const int a = dma_claim_unused_channel(false);
    const int b = dma_claim_unused_channel(false);
    if (a < 0 || b < 0) {
        if (a >= 0) dma_channel_unclaim(a);
        if (b >= 0) dma_channel_unclaim(b);
        g_initFail = 1;   // no free DMA channels
        return false;
    }
    g_dmaCh[0] = a;
    g_dmaCh[1] = b;

    if (cur != usbAudioDmaIrq) irq_set_exclusive_handler(DMA_IRQ_1, usbAudioDmaIrq);
    g_initFail = 0;
    return true;
}

static void usbAudioAdcStop(void) {
    adc_run(false);
    adc_hw->cs &= ~ADC_CS_START_MANY_BITS;
    // RP2350-E5 (hardware/dma.h, dma_channel_abort): clear the ENABLE bit of
    // the aborted channel AND anything chained to it BEFORE the abort, or the
    // abort's completion can re-trigger the sibling and leave a channel
    // running behind our back. Both channels chain to each other, so disable
    // both first, then abort both, then drop the completions the aborts raise.
    for (int k = 0; k < 2; k++) {
        if (g_dmaCh[k] < 0) continue;
        dma_channel_set_irq1_enabled(g_dmaCh[k], false);
        hw_clear_bits(&dma_hw->ch[g_dmaCh[k]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    for (int k = 0; k < 2; k++) {
        if (g_dmaCh[k] < 0) continue;
        dma_channel_abort(g_dmaCh[k]);
    }
    for (int k = 0; k < 2; k++) {
        if (g_dmaCh[k] < 0) continue;
        dma_hw->ints1 = (1u << g_dmaCh[k]);   // W1C
    }
    irq_set_enabled(DMA_IRQ_1, false);
    irq_clear(DMA_IRQ_1);
    adc_fifo_drain();
    adc_set_round_robin(0);
    // Put the peripheral back exactly the way Peripherals.cpp's initADC() left
    // it, or every subsequent readAdc() gets garbage.
    initADC();
    // DMA channels stay claimed for the life of the process; nothing to unclaim.
}

// Hand the ADC back to the rest of the board. Only ever called from the pump
// (core 1) so there is exactly one releaser of the readingADC lock.
static void usbAudioReleaseAdc(void) {
    g_streaming = false;
    usbAudioAdcStop();
    // Capture is down: this is the one safe moment to adopt a rate the host was
    // not consulted about (see usb_audio_set_rate()).
    if (g_pendingRateHz != 0) {
        g_rateHz = g_pendingRateHz;
        g_pendingRateHz = 0;
        usb_audio_sync_config();
    }
    __sync_synchronize();
    usbAudioOwnsAdc = false;
    __atomic_clear(&readingADC, __ATOMIC_RELEASE);
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
    // treated as hostile. full_scale especially: it is a DIVISOR in
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
    // where dma_claim_unused_channel(true) panicking would silently kill the
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

extern "C" bool usb_audio_yield_adc(const char *why) {
    (void) why;   // no Serial from here: this can run on core 1 (see below)
    if (!g_streaming && !usbAudioOwnsAdc) return true;
    g_stopRequested = true;

    // Only the core-1 pump may execute the stop, so there is exactly one
    // releaser of readingADC: two cores both observing g_stopRequested and both
    // clearing the lock would hand the ADC to two owners at once. If we ARE
    // core 1 run the pump right here - the
    // regular pump call is downstream in this same loop1 pass and cannot help
    // us. Otherwise wait for it, bounded so a wedged core 1 cannot hang core 0.
    //
    // RETURNS whether the ADC is actually free now. It can legitimately fail:
    // the pump only runs from core2stuff(), which loop1() skips entirely while
    // pauseCore2 is set. A caller that
    // ignores this and reads anyway gets the audio sweep means - and a hard 0
    // on the probe channels - which is how DAC calibration could solve for, and
    // then PERSIST, constants derived from sentinel values.
    if (get_core_num() == 1) {
        serviceUSBAudio();
        return !usbAudioOwnsAdc;
    }
    const uint32_t t0 = micros();
    while (usbAudioOwnsAdc && (micros() - t0) < 250000u) {
        tight_loop_contents();
    }
    return !usbAudioOwnsAdc;
}

extern "C" void usb_audio_resume_adc(void) {
    if (g_usb_audio_enabled && g_hostAlt == 1 && !g_streaming) g_startRequested = true;
}

extern "C" void usb_audio_probe_activity(void) {
    g_probeReqUs = micros() | 1u;   // never the 0 sentinel
}

extern "C" void serviceUSBAudio(void) {
    // Single-entry, even though the only regular caller is core1's loop1().
    // Anything that reconfigures the ADC/DMA from another core while this is
    // mid-teardown would corrupt both. Cheap: uncontended in the normal path.
    static volatile bool svcBusy = false;
    if (__atomic_test_and_set(&svcBusy, __ATOMIC_ACQUIRE)) return;
    struct SvcGuard {
        volatile bool *f;
        ~SvcGuard() { __atomic_clear(f, __ATOMIC_RELEASE); }
    } guard{ &svcBusy };

    // STOP FIRST, before any quiesce gate. This is the ONLY releaser of
    // readingADC/usbAudioOwnsAdc, and a stop is precisely what a pauseCore2
    // window wants - gating it behind pauseCore2 meant usb_audio_yield_adc()
    // could return with the ADC still held, and its callers (self test, DAC
    // calibration) then proceeded as if they had the
    // real converter while every read was served from the audio sweep.
    if (g_stopRequested) {
        g_stopRequested = false;
        if (g_streaming || usbAudioOwnsAdc) usbAudioReleaseAdc();
    }

    // Core 0 wants core 1 quiesced (flash write, refresh). Same gate as our
    // neighbours updateLazyAdcReadings()/serviceNetVoltageScan(): STARTING or
    // restarting the ADC/DMA inside that window is the wrong moment. Releasing
    // it, handled above, never is.
    if (pauseCore2) return;

    if (g_startRequested) {
        g_startRequested = false;
        if (!g_streaming && g_usb_audio_enabled && !g_pausedForProbe) {
            // Claim the ADC for the whole stream. Bounded, because the caller
            // here is the main loop: if something else legitimately owns the
            // ADC we decline this start rather than stalling the board, and
            // the host simply hears silence. No Serial here - core 1.
            uint32_t t0 = micros();
            bool got = true;
            while (__atomic_test_and_set(&readingADC, __ATOMIC_ACQUIRE)) {
                if (micros() - t0 > 50000) { got = false; break; }
                tight_loop_contents();
            }
            if (!got) {
                g_statClaimFail++;
            } else {
                usbAudioOwnsAdc = true;
                __sync_synchronize();
                if (usbAudioAdcStart()) {
                    g_streaming = true;
                } else {
                    // Could not get DMA/IRQ - give the ADC straight back
                    // rather than holding it hostage for a stream that will
                    // never run.
                    usbAudioReleaseAdc();
                }
            }
        }
    }

    // Probe arbitration: give the ADC up while the probe is in use, take it
    // back once the probe has gone quiet. Deliberately hysteretic - probe
    // bursts are many reads over tens of ms and we must not thrash the DMA.
    if (g_probeReqUs != 0) {
        const uint32_t sinceProbe = micros() - g_probeReqUs;
        if (sinceProbe < JL_AUDIO_PROBE_HOLD_US) {
            if (g_streaming) {
                usbAudioReleaseAdc();
                g_pausedForProbe = true;
                g_statProbePauses++;
            }
        } else {
            g_probeReqUs = 0;
            if (g_pausedForProbe) {
                g_pausedForProbe = false;
                usb_audio_resume_adc();   // only if the host still has the mic open
            }
        }
    }

    // An ADC FIFO overrun (or a late completion IRQ) leaves the channel
    // interleave in an unknown phase, so the only safe fix is a full restart -
    // done here, never in the IRQ.
    if (g_needResync && g_streaming) {
        g_needResync = false;
        g_statResyncs++;
        usbAudioAdcStop();
        if (!usbAudioAdcStart()) usbAudioReleaseAdc();
    }
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
extern "C" bool usbAudioSnapshotRaw(int channel, int *raw) {
    if (!usbAudioOwnsAdc || raw == NULL || channel < 0 || channel > 7) return false;

    if (channel == 5) {
        if (g_hkMean[5] >= jumperlessConfig.calibration.minimum_probe_reading) {
            usb_audio_probe_activity();
        }
        *raw = 0;
        return true;
    }
    if (channel == 7) { *raw = 0; return true; }

    if (channel == (int) g_chL) { *raw = (int) g_lastMean[0]; return true; }
    if (channel == (int) g_chR) { *raw = (int) g_lastMean[1]; return true; }
    // Housekeeping channels ride the same sweep, so supply sense and the
    // routable ADCs still read real values while the host records.
    if (g_rrMask & (1u << channel)) { *raw = (int) g_hkMean[channel]; return true; }
    return false;
}

// Refresh only the streamed entries of the adcReadings[] cache. Everything else
// keeps its last good value rather than being overwritten with the 0 that a
// short-circuited readAdc() now returns.
extern "C" bool usbAudioRefreshLazy(float *readings) {
    if (!usbAudioOwnsAdc || readings == NULL) return false;
    // Every channel in the sweep refreshes, so the OLED voltage cache stays
    // live for the whole recording rather than freezing at whatever it held
    // when streaming began.
    for (int ch = 0; ch < 8; ch++) {
        if (!(g_rrMask & (1u << ch))) continue;
        int32_t raw;
        if (ch == (int) g_chL)      raw = g_lastMean[0];
        else if (ch == (int) g_chR) raw = g_lastMean[1];
        else                        raw = g_hkMean[ch];
        float v = (float) raw * (adcSpread[ch] / 4095.0f);
        if (ch != 4 && ch != 5) v -= adcZero[ch];
        readings[ch] = v;
    }
    return true;
}

// Drain the ring into TinyUSB's software FIFO. Runs in USB IRQ context on
// core 0 (audiod_xfer_cb -> here), which is the only safe place to touch that
// FIFO, and is called once per completed isochronous transfer - i.e. ~1 kHz
// while the host is recording.
extern "C" bool __not_in_flash_func(tud_audio_tx_done_isr)(uint8_t rhport, uint16_t n_bytes_sent,
                                                           uint8_t func_id, uint8_t ep_in,
                                                           uint8_t cur_alt_setting) {
    (void) rhport; (void) n_bytes_sent; (void) func_id; (void) ep_in; (void) cur_alt_setting;

    const uint32_t head = g_ringHead;
    uint32_t tail = g_ringTail;
    uint32_t avail = (head - tail) & JL_AUDIO_RING_MASK;
    if (avail > (uint32_t) JL_AUDIO_SAMPLES_PER_MS) avail = JL_AUDIO_SAMPLES_PER_MS;
    if (avail == 0) return true;   // nothing captured yet; driver sends silence

    int16_t buf[JL_AUDIO_SAMPLES_PER_MS];
    for (uint32_t n = 0; n < avail; n++) {
        buf[n] = g_ring[(tail + n) & JL_AUDIO_RING_MASK];
    }
    __sync_synchronize();
    g_ringTail = (tail + avail) & JL_AUDIO_RING_MASK;

    tud_audio_write(buf, (uint16_t)(avail * sizeof(int16_t)));
    g_statFramesSent += avail / 2u;
    return true;
}

//--------------------------------------------------------------------+
// Configuration
//--------------------------------------------------------------------+

// NOTE: these reconfigure via g_needResync rather than calling
// usbAudioAdcStop()/Start() inline. They are invoked from MicroPython or the
// console on core 0, while core1 owns the DMA IRQ and the service pump -
// tearing the DMA down from a second core would let the IRQ fire into a
// half-configured channel. Setting the flag lets core1 do the restart in the
// one place that is allowed to.
extern "C" bool usb_audio_set_channels(int left, int right) {
    if (!usbAudioChannelStreamable(left) || !usbAudioChannelStreamable(right) ||
        left == right) return false;
    g_chL = (uint8_t) left;
    g_chR = (uint8_t) right;
    if (g_streaming) g_needResync = true;
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
