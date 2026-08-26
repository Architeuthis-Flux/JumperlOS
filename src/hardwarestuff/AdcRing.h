// SPDX-License-Identifier: MIT
#ifndef ADCRING_H
#define ADCRING_H
// ---------------------------------------------------------------------------
// The always-on ADC ring (T2.1 / C1 in CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md).
//
// The one ADC free-runs in round-robin over all eight inputs at 48 kHz per
// channel (384 ksps) into an 8 KB SRAM ring by DMA - two channels chained to
// each other, each writing one 1 ms block (384 halfwords) with a hardware
// write-ring wrap on the whole buffer, so nothing depends on any core: a
// parked core costs a late block count, never a wild pointer or a lost
// sample. Sweep s (the s-th pass over the eight inputs) sits at halfwords
// 8s..8s+7, channel c at 8s+c; the DMA starts at ring[0] with AINSEL 0, so a
// halfword's channel is its index & 7 for as long as no conversion is ever
// lost (ADC FIFO overrun -> resync, counted).
//
// Readers never touch the converter. readAdc()/readAdcHeld() keep their
// meaning ("a fresh burst starting now") by WAITING for n new sweeps
// (~21 us each) - so every drive-then-read site (the probe blink, the dark
// pad read, NetVoltageScan's taps) is right without a per-site audit - and
// the hot paths read history: readProbeRaw() decodes the newest 128 samples
// of the pad channel as 8 windows of 16 in one memory read (the pad poll
// that cost core 0 ~37 ms per pass becomes microseconds), the OLED cache
// takes means. USB audio is one more consumer of the same sweeps (rates
// that divide 48 k). The legacy START_ONCE path stays behind adcRingActive()
// - the D-menu toggle switches the two live, for an A/B on the same board.
//
// Cores: readers on either core (the sample counter is under a claimed SIO
// spinlock); the 1 ms block IRQ (DMA_IRQ_1, exclusive) on core 0.
// ---------------------------------------------------------------------------
#include <stdint.h>
#include <stdbool.h>

#define ADC_RING_CHANNELS   8u
#define ADC_RING_SWEEPS     512u                    // ring depth in sweeps (~10.7 ms)
#define ADC_RING_HALFWORDS  (ADC_RING_SWEEPS * ADC_RING_CHANNELS)   // 4096
#define ADC_RING_SWEEP_HZ   48000u                  // per-channel sample rate
#define ADC_RING_SWEEP_US   (1000000.0f / (float)ADC_RING_SWEEP_HZ)   // 20.83 us

// Start / stop the engine (core 0; start after initADC()). start() = the ADC
// is the ring's from now on; stop() puts the peripheral back to the
// START_ONCE state initADC() leaves (the legacy path takes over).
bool     adcRingStart(void);
void     adcRingStop(void);
bool     adcRingActive(void);
// The engine's fallback reason when start() declined: 0 ok, 1 no DMA
// channels, 2 DMA_IRQ_1 taken, 3 no spinlock, 4 not built (OG).
int      adcRingFailReason(void);

// Sweeps completed so far (monotonic modulo the ring's laps between block
// IRQs; readers only ever use differences of a few hundred). Sweep index S
// means "sweeps 0..S-1 are complete".
uint32_t adcRingSweeps(void);
uint32_t adcRingGeneration(void);   // bumps on every (re)start / resync - a reader that spans one should retry

// Mean of the newest n samples of channel ch (memory read, no wait). n is
// clamped to ADC_RING_SWEEPS - 64. Returns 0 if the engine is not active.
int      adcRingMeanNewest(int ch, int n);
// Mean over the window of n sweeps ENDING at sweep endSweep (exclusive):
// sweeps endSweep-n .. endSweep-1, channel ch. For callers slicing history
// into ordered windows. Sweeps older than the ring depth read as garbage -
// callers keep windows within the last ~440 sweeps.
int      adcRingMeanWindow(int ch, uint32_t endSweep, int n);
// Wait until n sweeps NEWER than sweep index `after` exist (bounded by
// timeoutUs; ~ (n+1) x 21 us normally), then return their mean. This is a
// fresh burst starting "now" when `after` = adcRingSweeps() at the call.
// On timeout returns the newest n (and counts a stall).
int      adcRingMeanAfter(int ch, uint32_t after, int n, uint32_t timeoutUs);
// Same, but the caller learns whether the mean really is that fresh burst.
// *fresh comes back false on timeout, engine stop, a generation bump (a
// resync restarted the sweep phase mid-read), or the DMA lapping the window
// while it was summed - all cases where the returned mean is best-available
// history, not the n sweeps after `after`. Callers that feed downstream
// decisions (the net scan's taps) fail the read instead of trusting it;
// adcRingMeanAfter() is this with fresh ignored.
int      adcRingMeanAfterStrict(int ch, uint32_t after, int n, uint32_t timeoutUs, bool* fresh);

// Housekeeping: resync after an ADC FIFO overrun (called from a service on
// either core; single-entry).
void     adcRingService(void);

// The raw ring, for consumers that walk sweeps themselves (USB audio):
// halfword i holds channel (i & 7) of sweep (i >> 3), 12-bit right-aligned.
const volatile uint16_t* adcRingData(void);
// Halfword total right now (sweep-aligned readers use adcRingSweeps()).
uint32_t adcRingHalfwords(void);

// Diagnostics for X.
struct AdcRingStats {
    bool     active;
    int      failReason;
    uint32_t generation;
    uint32_t sweeps;         // adcRingSweeps() now
    uint32_t blockIrqs;      // 1 ms block completions seen
    uint32_t overruns;       // ADC FIFO overruns (each forces a resync)
    uint32_t resyncs;
    uint32_t stalls;         // adcRingMeanAfter() timeouts
    uint32_t reads;          // adcRingMean* calls
    uint32_t maxWaitUs;      // longest adcRingMeanAfter() wait
    uint32_t dmaA, dmaB;     // channels
    float    sweepHz;
};
void     adcRingGetStats(AdcRingStats* out);

#endif
