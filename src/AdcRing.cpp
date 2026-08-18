// SPDX-License-Identifier: MIT
#include "AdcRing.h"
#include <Arduino.h>
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/structs/dma.h"
#include "hardware/structs/adc.h"

// See AdcRing.h for the design. V5 (RP2350B, eight ADC inputs) only: the OG's
// RP2040 keeps the START_ONCE path (no board to verify, T2.3's precedent).
#if !defined(OG_JUMPERLESS)
#define ADC_RING_BUILD 1
#else
#define ADC_RING_BUILD 0
#endif

// The ring: 4096 halfwords = 8 KB, aligned to its own size for the DMA write
// ring wrap. Plain SRAM (.bss), never PSRAM.
#define RING_BYTES  (ADC_RING_HALFWORDS * 2u)
#define RING_BITS   13u
static_assert((1u << RING_BITS) == RING_BYTES, "ring bits vs ring size");
static uint16_t __attribute__((aligned(8192))) s_ring[ADC_RING_HALFWORDS];

// One block = 1 ms of conversions = 48 sweeps of 8. The DMA channel runs in
// TRIGGER_SELF mode with this count: it re-triggers itself at the end of every
// block, resumes at its current write address (which the ring wrap keeps
// inside s_ring), and raises the completion IRQ each time - the RP2350
// datasheet's own "streaming through SRAM ring buffers" case (12.6.2.2.1).
#define BLOCK_HALFWORDS 384u
static_assert(BLOCK_HALFWORDS % ADC_RING_CHANNELS == 0, "a block is whole sweeps");   // blocks need not tile the ring: the write wrap lands mid-block, harmlessly
// clk_adc = 48 MHz; a conversion starts every (1 + DIV.INT) clocks: 125 clocks
// = 384 ksps = exactly 48 kHz per channel over 8.
#define RING_CLKDIV     124.0f
#define RING_RR_MASK    0xFFu

static int      s_ch = -1;
static int      s_lockId = -1;
static volatile bool     s_active = false;
static int      s_fail = ADC_RING_BUILD ? 0 : 4;
static volatile uint32_t s_generation = 0;
static volatile uint32_t s_total = 0;      // halfwords written (monotonic modulo laps between updates)
static volatile uint32_t s_lastPos = 0;    // ring position at the last s_total update
static volatile bool     s_needResync = false;
// stats
static volatile uint32_t s_blockIrqs = 0, s_overruns = 0, s_resyncs = 0, s_stalls = 0, s_reads = 0, s_maxWaitUs = 0;

#if ADC_RING_BUILD

static inline spin_lock_t* rlock(void) { return spin_lock_instance((uint)s_lockId); }

// Where the DMA is writing right now, in halfwords from the ring start.
static inline uint32_t __not_in_flash_func(livePos)(void) {
    uint32_t wa = dma_hw->ch[s_ch].write_addr;
    return ((wa - (uint32_t)(uintptr_t)s_ring) >> 1) & (ADC_RING_HALFWORDS - 1u);
}

// Bring s_total up to the live pointer. Under the lock: readers on both cores
// and the block IRQ all do this. Loses whole laps only if nobody calls it for
// > 10.7 ms (a flash write parks both cores) - readers only ever use
// differences of a few hundred sweeps, and the ring content is consistent
// with the position either way.
static uint32_t __not_in_flash_func(syncTotal)(void) {
    if (!s_active) return s_total;
    uint32_t save = spin_lock_blocking(rlock());
    uint32_t pos = livePos();
    uint32_t delta = (pos - s_lastPos) & (ADC_RING_HALFWORDS - 1u);
    s_total += delta;
    s_lastPos = pos;
    uint32_t t = s_total;
    spin_unlock(rlock(), save);
    return t;
}

// The audio consumer's hook (USBAudio.cpp): called from the block IRQ with the
// current halfword total; weak so a build without USB audio links.
extern "C" void __attribute__((weak)) usbAudioOnRingBlock(uint32_t totalHalfwords) { (void)totalHalfwords; }

static void __not_in_flash_func(adcRingIrq)(void) {
    if (s_ch < 0 || !(dma_hw->ints1 & (1u << s_ch))) return;
    dma_hw->ints1 = (1u << s_ch);   // W1C
    s_blockIrqs++;
    uint32_t total = syncTotal();
    if (adc_hw->fcs & ADC_FCS_OVER_BITS) {
        adc_hw->fcs |= ADC_FCS_OVER_BITS;   // W1C
        s_overruns++;
        s_needResync = true;                // a lost conversion rotates the channel phase: restart
    }
    if (!s_needResync) usbAudioOnRingBlock(total);
}

static void ringHardwareStop(void) {
    adc_run(false);
    hw_clear_bits(&adc_hw->cs, ADC_CS_START_MANY_BITS);
    if (s_ch >= 0) {
        dma_channel_set_irq1_enabled((uint)s_ch, false);
        // the datasheet's abort sequence: EN + chain off, abort, poll (bounded)
        hw_write_masked(&dma_hw->ch[s_ch].al1_ctrl,
                        ((uint)s_ch << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) | (0u << DMA_CH0_CTRL_TRIG_EN_LSB),
                        DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS | DMA_CH0_CTRL_TRIG_EN_BITS);
        dma_hw->abort = 1u << s_ch;
        uint32_t t0 = time_us_32();
        while ((dma_hw->abort & (1u << s_ch)) && (time_us_32() - t0) < 2000) { tight_loop_contents(); }
        t0 = time_us_32();
        while ((dma_hw->ch[s_ch].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) && (time_us_32() - t0) < 2000) { tight_loop_contents(); }
        dma_hw->ints1 = (1u << s_ch);
    }
    adc_fifo_drain();
    adc_set_round_robin(0);
    adc_hw->fcs = 0;
}

static bool ringHardwareStart(void) {
    // ADC: free-running round-robin over all eight, FIFO on with DREQ, one
    // sample per DREQ (threshold 1), sticky flags cleared.
    adc_run(false);
    hw_clear_bits(&adc_hw->cs, ADC_CS_START_MANY_BITS);
    adc_fifo_drain();
    adc_set_clkdiv(RING_CLKDIV);
    adc_select_input(0);
    adc_set_round_robin(RING_RR_MASK);
    adc_hw->fcs = ADC_FCS_EN_BITS | ADC_FCS_DREQ_EN_BITS
                | (1u << ADC_FCS_THRESH_LSB)
                | ADC_FCS_OVER_BITS | ADC_FCS_UNDER_BITS;   // W1C the sticky flags

    // DMA: 16-bit, fixed read (the FIFO), incrementing write with the ring
    // wrap over the whole buffer, DREQ_ADC, TRIGGER_SELF with a 1 ms count.
    dma_channel_config c = dma_channel_get_default_config((uint)s_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    channel_config_set_ring(&c, true, RING_BITS);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure((uint)s_ch, &c, s_ring, &adc_hw->fifo, BLOCK_HALFWORDS, false);
    // TRIGGER_SELF: the count register's top nibble is the mode on RP2350.
    dma_hw->ch[s_ch].transfer_count = (DMA_CH0_TRANS_COUNT_MODE_VALUE_TRIGGER_SELF << DMA_CH0_TRANS_COUNT_MODE_LSB) | BLOCK_HALFWORDS;
    dma_hw->ints1 = (1u << s_ch);
    dma_channel_set_irq1_enabled((uint)s_ch, true);

    // A (re)start writes from ring[0] with AINSEL 0: s_total keeps counting
    // (readers only use differences) but must sit on a ring boundary so its
    // position - and its sweep phase - match the hardware again.
    s_lastPos = 0;
    s_total = (s_total + (ADC_RING_HALFWORDS - 1u)) & ~(ADC_RING_HALFWORDS - 1u);
    __dmb();
    dma_channel_start((uint)s_ch);
    adc_hw->cs = ADC_CS_EN_BITS
               | (0u << ADC_CS_AINSEL_LSB)
               | ((uint32_t)RING_RR_MASK << ADC_CS_RROBIN_LSB)
               | ADC_CS_START_MANY_BITS;
    return true;
}
#endif // ADC_RING_BUILD

bool adcRingStart(void) {
#if ADC_RING_BUILD
    if (s_active) return true;
    if (s_ch < 0) {
        int ch = dma_claim_unused_channel(false);
        if (ch < 0) { s_fail = 1; return false; }
        s_ch = ch;
    }
    if (s_lockId < 0) {
        int id = spin_lock_claim_unused(false);
        if (id < 0) { s_fail = 3; return false; }
        spin_lock_init((uint)id);
        s_lockId = id;
    }
    // DMA_IRQ_1, exclusive (MicroPython's rp2.DMA masks IRQ 0 while it
    // reconfigures channels). Look before we leap: irq_set_exclusive_handler
    // hard-asserts if the line already has a handler.
    const irq_handler_t cur = irq_get_exclusive_handler(DMA_IRQ_1);
    if ((cur != NULL && cur != adcRingIrq) || irq_has_shared_handler(DMA_IRQ_1)) { s_fail = 2; return false; }
    if (cur != adcRingIrq) irq_set_exclusive_handler(DMA_IRQ_1, adcRingIrq);
    irq_set_enabled(DMA_IRQ_1, true);
    s_fail = 0;
    ringHardwareStart();
    s_generation++;
    s_needResync = false;
    __dmb();
    s_active = true;
    return true;
#else
    return false;
#endif
}

void adcRingStop(void) {
#if ADC_RING_BUILD
    if (!s_active) return;
    s_active = false;
    __dmb();
    ringHardwareStop();
    // Put the peripheral back the way Peripherals.cpp's initADC() leaves it:
    // clkdiv 1, input 0, no round-robin, FIFO off, not running.
    adc_fifo_setup(false, false, 0, false, false);
    adc_fifo_drain();
    adc_set_clkdiv(1.0f);
    adc_select_input(0);
    adc_set_round_robin(0);
    adc_run(false);
    s_generation++;
#endif
}

bool adcRingActive(void) { return s_active; }
int  adcRingFailReason(void) { return s_fail; }
uint32_t adcRingGeneration(void) { return s_generation; }

uint32_t __not_in_flash_func(adcRingSweeps)(void) {
#if ADC_RING_BUILD
    return syncTotal() >> 3;
#else
    return 0;
#endif
}

#if ADC_RING_BUILD
// Sum of n samples of channel ch in the sweeps [endSweep - n, endSweep).
static inline int __not_in_flash_func(sumWindow)(int ch, uint32_t endSweep, int n) {
    uint32_t sum = 0;
    uint32_t s = endSweep - (uint32_t)n;
    for (int i = 0; i < n; i++, s++) {
        sum += s_ring[((s << 3) + (uint32_t)ch) & (ADC_RING_HALFWORDS - 1u)] & 0x0FFFu;
    }
    return (int)sum;
}
// The newest sweep index that has channel ch's sample written (exclusive end
// for windows: sweeps < end are complete for ch).
static inline uint32_t __not_in_flash_func(newestEndFor)(int ch, uint32_t total) {
    uint32_t S = total >> 3;               // sweeps 0..S-1 fully written
    if ((int)(total & 7u) > ch) S++;       // this sweep already holds ch
    return S;
}
#endif

int __not_in_flash_func(adcRingMeanNewest)(int ch, int n) {
#if ADC_RING_BUILD
    if (!s_active || ch < 0 || ch > 7) return 0;
    if (n < 1) n = 1;
    if (n > (int)ADC_RING_SWEEPS - 64) n = (int)ADC_RING_SWEEPS - 64;
    s_reads++;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t t0 = syncTotal();
        uint32_t end = newestEndFor(ch, t0);
        int sum = sumWindow(ch, end, n);
        uint32_t t1 = syncTotal();
        // torn if the DMA lapped into the window while we read it
        if ((t1 - t0) < (uint32_t)(ADC_RING_SWEEPS - (uint32_t)n) * 8u) return sum / n;
    }
    return 0;
#else
    (void)ch; (void)n; return 0;
#endif
}

int __not_in_flash_func(adcRingMeanWindow)(int ch, uint32_t endSweep, int n) {
#if ADC_RING_BUILD
    if (!s_active || ch < 0 || ch > 7 || n < 1) return 0;
    if (n > (int)ADC_RING_SWEEPS - 64) n = (int)ADC_RING_SWEEPS - 64;
    s_reads++;
    return sumWindow(ch, endSweep, n) / n;
#else
    (void)ch; (void)endSweep; (void)n; return 0;
#endif
}

int __not_in_flash_func(adcRingMeanAfter)(int ch, uint32_t after, int n, uint32_t timeoutUs) {
#if ADC_RING_BUILD
    if (!s_active || ch < 0 || ch > 7) return 0;
    if (n < 1) n = 1;
    if (n > (int)ADC_RING_SWEEPS - 64) n = (int)ADC_RING_SWEEPS - 64;
    // Fresh = sweeps after+1 .. after+n (sweep `after` may have been in
    // progress at the call). Wait until channel ch's sample of sweep after+n
    // is written.
    uint32_t need = after + (uint32_t)n + 1u;
    uint32_t t0 = time_us_32();
    uint32_t gen = s_generation;
    for (;;) {
        uint32_t total = syncTotal();
        if ((int32_t)(newestEndFor(ch, total) - need) >= 0) break;
        if ((time_us_32() - t0) > timeoutUs || s_generation != gen || !s_active) {
            s_stalls++;
            return adcRingMeanNewest(ch, n);   // best available
        }
        tight_loop_contents();
    }
    uint32_t dt = time_us_32() - t0;
    if (dt > s_maxWaitUs) s_maxWaitUs = dt;
    s_reads++;
    return sumWindow(ch, need, n) / n;
#else
    (void)ch; (void)after; (void)n; (void)timeoutUs; return 0;
#endif
}

void adcRingService(void) {
#if ADC_RING_BUILD
    if (!s_active || !s_needResync) return;
    static volatile bool busy = false;
    if (__atomic_test_and_set(&busy, __ATOMIC_ACQUIRE)) return;
    s_needResync = false;
    s_resyncs++;
    // Restart the engine: a lost conversion rotated the channel phase and only
    // a clean start (DMA at ring[0], AINSEL 0) re-establishes it.
    s_active = false;
    __dmb();
    ringHardwareStop();
    ringHardwareStart();
    s_generation++;
    __dmb();
    s_active = true;
    __atomic_clear(&busy, __ATOMIC_RELEASE);
#endif
}

const volatile uint16_t* adcRingData(void) { return s_ring; }
uint32_t adcRingHalfwords(void) {
#if ADC_RING_BUILD
    return syncTotal();
#else
    return 0;
#endif
}

void adcRingGetStats(AdcRingStats* out) {
    if (!out) return;
    out->active = s_active;
    out->failReason = s_fail;
    out->generation = s_generation;
    out->sweeps = adcRingSweeps();
    out->blockIrqs = s_blockIrqs;
    out->overruns = s_overruns;
    out->resyncs = s_resyncs;
    out->stalls = s_stalls;
    out->reads = s_reads;
    out->maxWaitUs = s_maxWaitUs;
    out->dmaA = (uint32_t)s_ch;
    out->dmaB = 0;
    out->sweepHz = (float)ADC_RING_SWEEP_HZ;
}
