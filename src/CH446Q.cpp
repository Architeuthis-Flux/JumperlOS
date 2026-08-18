// SPDX-License-Identifier: MIT

#include <cstdint>  // For uint16_t
#include "XbarLatency.h" // tap->crossbar->LEDs latency probe (T2.2 gate)
#include "CoreMailbox.h"  // core1req::complete() at the end of a list send (T2.2b/T2.3)
#include "hardware/dma.h" // the DMA-fed list send (T2.3)
#include "CH446Q.h"
#include "Colors.h"       // For changeTerminalColor
#include "JumperlessDefines.h"
#include "JumperlOS.h"    // For LiveCrossbarService
#include "LEDs.h"
#include "MatrixState.h"
#include "NetManager.h"   // For assignTermColor
#include "States.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "RotaryEncoder.h" // encoderServiceYield() in the PIO handshake wait
#include "RouteSafety.h"
#include "externVars.h"


#include "hardware/pio.h"
#include <hardware/sync.h>  // For __dmb() memory barrier

#include "ch446.pio.h"
#include "FileParsing.h"

//#include "SerialWrapper.h"


// #include "pio_spi.h"

#define MYNAMEISERIC                                                           \
  0 // on the board I sent to eric, the data and clock lines are bodged to GPIO
    // 18 and 19. To allow for using hardware SPI

// int chipToPinArray[12] = {CS_A, CS_B, CS_C, CS_D, CS_E, CS_F, CS_G, CS_H,
// CS_I, CS_J, CS_K, CS_L};
PIO pio = pio0;

uint sm = pio_claim_unused_sm(pio, true);

uint offset = 0;

volatile int chipSelect = -1;  // -1 = no single-crosspoint send in flight (the token; see sendXYrawUnchecked)
volatile uint32_t irq_flags = 0;

// struct justXY {
//   bool connected[16][8]; // 16 X values, 8 Y values, stores whether a connection exists
//   };

chipXYBitfield lastChipXY[12];

// OPTIMIZATION: Track which chips have connections to avoid scanning empty chips
static bool chipHadConnections[12] = {false};

// RAM-resident: this ISR fires once per crosspoint (shared PIO0_IRQ_1, core 1's
// NVIC) and the SM stalls until it runs - an XIP miss here is a stall on the
// crossbar send, and during a core-0 flash write it would fault. Same for the
// per-crosspoint path below (sendPath / sendXYrawUnchecked / sendXYraw) and
// setCSex in Peripherals.cpp: sendPaths/sendAllPaths were already
// __not_in_flash_func but everything they called per crosspoint ran from flash
// (X showed the irq 16 handler at 0x1005xxxx). See
// CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md C2-0.
// ---------------------------------------------------------------------------
// T2.3 (C2a): the DMA-fed list send.
//
// A list send (sendAllPaths, from sendPaths) used to push one word per
// crosspoint into the PIO TX FIFO and spin until the ISR had strobed that
// chip's select - core 1 blocked for the whole send (~30 us per crosspoint,
// ~1 ms typical, tens of ms for a clean rebuild), and NO LED frame during it.
// Now, on core 1 (V5): the loops that used to send COLLECT instead -
// sendXYrawUnchecked() appends the PIO word to dmaWords[] and the chip to
// dmaCs[] (bookkeeping and the chip-K safety clears exactly as before, so the
// order on the wire is the order that would have been sent) - and one DMA
// channel (DREQ = this SM's TX FIFO) feeds the words while the ISR strobes
// dmaCs[dmaIdx++] per PIO IRQ. The existing "irq nowait 1 / wait 0 irq 1 rel"
// handshake in ch446.pio.h throttles the DMA for free: the SM will not pull
// the next word until the ISR has cleared the flag. Completion = the ISR
// strobing the last chip: it completes the mailbox request the caller passed
// in (core1req::complete - the spinlock is safe from IRQ context: IRQs are off
// while it is held, so a same-core holder cannot be preempted by this ISR, and
// a cross-core holder releases within a few instructions) and stamps the
// latency probe. sendPaths() returns as soon as the DMA is kicked; core 1
// goes on rendering. The words/chips are a SNAPSHOT (statics), never the live
// paths[] tables: a refresh that gave up its 25 ms wait may rebuild them
// while the DMA is still strobing - harmless, the old state finishes and the
// new request re-sends. Not done here (deliberately): the single-crosspoint
// CPU path (sendXYrawUnchecked outside a list send - taps, FakeGpio, the
// short check) stays; it waits for an in-flight DMA first, and the DMA kick
// waits for an in-flight CPU send, because the ISR counts IRQs and one foreign
// pio_sm_put mid-DMA would shift every later chip select by one. Core 0
// callers of sendAllPaths (refreshPaths from commands / file loads) keep the
// CPU path byte-for-byte (the ISR is core 1's). The OG keeps the CPU path
// (no OG board attached; C2b sets the precedent). No PIO program edit, no
// DMA IRQ (DMA_IRQ_1 is USBAudio's; completion is the PIO ISR's idx == n).
// A stall (no PIO IRQ for 200 ms of core-1 time - a park is not a stall)
// aborts the DMA, marks the unsent chips suspect, counts a timeout, restarts
// the SM and completes the request anyway, so no waiter can hang on it.
// ---------------------------------------------------------------------------
#if !defined(OG_JUMPERLESS)
#define CH446Q_DMA_SEND 1
#else
#define CH446Q_DMA_SEND 0
#endif

// ---------------------------------------------------------------------------
// T3.2 (C2b): the chip-select strobe done by a SECOND state machine, in PIO2.
//
// The shifter (PIO0 SM, DAT/CK on GPIO 14/15, GPIOBASE 0) cannot reach the
// twelve chip selects on GPIO 28..39 (they need GPIOBASE 16), so until now a
// CPU ISR strobed the select after every word - one interrupt per crosspoint,
// on core 1, and the SM stalled until it ran. Now (V5 only - the OG's RP2040
// has no PIO2 and no cross-block IRQs): PIO2 is set to GPIOBASE 16 first thing
// on core 1 (before the LED strips claim their SMs, so nobody has loaded a
// program there yet), and one SM there runs ch446_cs_strobe (ch446_pio2cs.pio):
// it pulls a word, waits for the shifter's "word shifted" flag, pulses the
// selected chip's STB for ~160 ns, and hands the shifter its "done" flag - the
// two blocks are neighbours through the RP2350's PREV/NEXT IRQ wrap (PIO0's
// prev is PIO2). No CPU in the loop, no ISR per crosspoint.
//
// ONE word describes a crosspoint for BOTH machines: bits 31..24 the CH446Q
// address byte (the shifter shifts left with an 8-bit autopull threshold, so
// it consumes the top byte and discards the rest), bits 11..0 the one-hot
// chip-select mask (bit c = chip c = GPIO 28+c; the strobe SM shifts right and
// consumes 13 bits), bit 12 = LAST. A list send runs TWO DMA channels over
// the SAME dmaWords[] array (one into each TX FIFO), so the two streams can
// never disagree in length or order; a single send puts the same word into
// both FIFOs and polls PIO2 flag 0 (which the strobe SM raises after a LAST
// word) - no ISR at all for singles. A list's last word carries LAST, so the
// strobe SM raises flag 0 exactly once per list; that flag is routed to
// PIO2_IRQ_1 (an EXCLUSIVE handler on core 1 - the shared chain is 6/6 full,
// and in strobe mode the legacy PIO0_IRQ_1 handler is not registered, which
// gives that chain slot back) only between kick and completion, and the ISR
// completes the mailbox request. Everything else - collection, arbitration
// (spinlock OS1, chipSelect as the single-send token), the stall watchdog,
// suspect marking on abort - is the T2.3 machinery with the ISR taken out.
//
// Fallback: if PIO2 cannot be re-based (someone loaded a program first), has
// no room, no free SM, or no DMA channel is left, the legacy path (this
// file's isrFromPio + the SIO chip selects) is used exactly as before, and X
// says so ("cs strobe: fallback").
//
// PIO layout this relies on (see LEDs.cpp / RotaryEncoder.cpp): PIO0 = this
// shifter + the probe LED/button SM (steered there: its button program needs
// 15 instructions and only PIO0 has the room), PIO1 = the top LED strip + the
// encoder (24 instructions at origin 0), PIO2 (base 16) = this strobe + the
// breadboard LED strip.
// ---------------------------------------------------------------------------
#if !defined(OG_JUMPERLESS) && defined(PICO_RP2350)
#define CH446Q_PIO2_CS 1
#include "ch446_pio2cs.pio.h"
#else
#define CH446Q_PIO2_CS 0
#endif

#define CH446Q_CS_LAST     (1u << 12)         // word bit 12: last word of a list send
#define CH446Q_CS_FIRST_GPIO 28
#define CH446Q_CS_PIN_MASK (0xFFFull << CH446Q_CS_FIRST_GPIO)

#define CH446Q_DMA_MAX_WORDS 1024   // 128 paths x 4 hops = 512 + the chip-K safety clears; max seen 73 per send. (Was 2048: 5 KB of SRAM given back for the heap, 2026-08-18 - a full collect flushes and continues anyway.)
static uint32_t dmaWords[CH446Q_DMA_MAX_WORDS];  // PIO TX words (address byte << 24 | cs mask | LAST)
static uint8_t  dmaCs[CH446Q_DMA_MAX_WORDS];     // chip per word (the legacy ISR's strobe list)
static volatile uint32_t dmaCount = 0;   // words in the send in flight
static volatile uint32_t dmaIdx = 0;     // words strobed by the legacy ISR so far
static volatile bool dmaActive = false;  // a DMA send is in flight (ISR strobes dmaCs[])
static volatile bool dmaCollect = false; // sendXYrawUnchecked() appends instead of sending
static int dmaChan = -1;                 // -1: no channel (CPU path)
static volatile int  reqSlotPending = -1;    // mailbox slot to complete when the send is done (-1: none)
static volatile uint32_t reqGenPending = 0;
static uint32_t ch446q_dma_sends = 0;    // stats for X
static uint32_t ch446q_dma_words = 0;
static uint32_t ch446q_dma_stalls = 0;
static uint32_t ch446q_dma_maxWords = 0;

#if CH446Q_PIO2_CS
static PIO      csPio = pio2;
static int      csSm = -1;              // -1: strobe SM not available -> legacy ISR path
static uint     csOffset = 0;
static int      dmaChanCs = -1;         // second DMA channel: dmaWords[] -> PIO2 TX FIFO
static uint8_t  csFallbackReason = 0;   // 0 = strobe active; see ch446qCsStrobeInfo()
static uint32_t cs_list_irqs = 0;       // completion IRQs (one per list send)
static uint32_t cs_single_sends = 0;    // single-crosspoint sends through the strobe SM
static uint32_t cs_single_timeouts = 0; // ... that timed out (recovery ran)
static inline bool csStrobeActive(void) { return csSm >= 0; }
#endif

// The chip-select bits of a word (bit c = chip c). Out-of-range chips strobe
// nothing - the legacy setCSex() ignored them the same way.
static inline uint32_t ch446qCsBits(int chip) {
  return (chip >= 0 && chip < 12) ? (1u << chip) : 0u;
}

// The send-arbitration lock: a CPU single-crosspoint send (chipSelect != -1)
// and a DMA send (dmaActive) must never overlap - see the banner. Fixed SIO
// spinlock OS1 (the OG's atomic helper uses it, but the OG has no DMA send).
static inline spin_lock_t* sendLock(void) { return spin_lock_instance(PICO_SPINLOCK_ID_OS1); }

bool ch446qSendInFlight(void) { __dmb(); return dmaActive; }

void ch446qDmaStats(uint32_t* sends, uint32_t* words, uint32_t* stalls, uint32_t* maxWords, bool* enabled) {
  if (sends) *sends = ch446q_dma_sends;
  if (words) *words = ch446q_dma_words;
  if (stalls) *stalls = ch446q_dma_stalls;
  if (maxWords) *maxWords = ch446q_dma_maxWords;
  if (enabled) *enabled = (dmaChan >= 0);
}

// The end of a list send, whichever path did it: complete the mailbox request
// the caller registered (if any) and stamp the latency probe. Called from the
// ISR (DMA path) or from sendPaths() (CPU path / nothing to send).
static void __not_in_flash_func(ch446qSendComplete)(void) {
  int slot = reqSlotPending;
  if (slot >= 0) {
    reqSlotPending = -1;
    core1req::complete((core1req::Slot)slot, reqGenPending);
  }
  __dmb();
  xbarLatSendDone();  // latency probe: crossbar matches the netlist (XbarLatency.h)
}

void __not_in_flash_func(isrFromPio)(void) {

  if (dmaActive) {
    // DMA-fed list send: this IRQ is word dmaIdx of the snapshot.
    uint32_t i = dmaIdx;
    int cs = (i < dmaCount) ? dmaCs[i] : -1;
    if (cs >= 0) {
      setCSex(cs, 1);
      setCSex(cs, 0);
    }
    dmaIdx = i + 1;
    // Let the SM pull the next word (it is parked at "wait 0 irq 1 rel").
    // Flag 1 ONLY: the probe LED/button SM lives on PIO0 too (T3.2 layout)
    // and its samples arrive on flag 0 - the old blanket clear of every PIO0
    // flag would eat them.
    pio_interrupt_clear(pio, 1);
    if (dmaIdx >= dmaCount) {
      dmaActive = false;
      __dmb();
      ch446qSendComplete();
    }
    return;
  }

  // delayMicroseconds(500);
  setCSex(chipSelect, 1);
  //  Serial.println("interrupt from pio  ");
  // Serial.print(chipSelect);
  // Serial.print(" \n\r");

  setCSex(chipSelect, 0);

  chipSelect = -1;

  // Clear the state machine interrupt (not PIO0_IRQ_0)
  // The PIO program uses "irq 1" and "wait 0 irq 1 rel", so we need to clear interrupt 1 for this state machine
  pio_interrupt_clear(pio, 1);  // Clear interrupt 1 (absolute) - and only that one (see above)
  }

#if CH446Q_PIO2_CS
// The strobe path's one interrupt: PIO2 flag 0, raised by the strobe SM after
// a LAST word. Enabled as an IRQ source only between a list kick and here (a
// single send polls the same flag with the source disabled). Exclusive
// handler on PIO2_IRQ_1, core 1's NVIC (registered from initCH446Q on core 1).
static void __not_in_flash_func(ch446qCsStrobeIsr)(void) {
  if (!pio_interrupt_get(csPio, 0)) return;
  pio_interrupt_clear(csPio, 0);
  pio_set_irq1_source_enabled(csPio, pis_interrupt0, false);
  cs_list_irqs++;
  if (dmaActive) {
    dmaActive = false;
    __dmb();
    ch446qSendComplete();
  }
}

// Put both machines back at their entry points with empty FIFOs, no handshake
// flag pending, every chip select LOW and the shifter's bit counters reset
// (a restart mid-word would otherwise carry a partial X into the next word).
// Used by the single-send timeout and the DMA abort.
static void __not_in_flash_func(ch446qStrobeReset)(void) {
  pio_sm_set_enabled(pio, sm, false);
  pio_sm_set_enabled(csPio, csSm, false);
  pio_sm_clear_fifos(pio, sm);
  pio_sm_clear_fifos(csPio, csSm);
  pio_sm_restart(pio, sm);
  pio_sm_restart(csPio, csSm);
  pio_set_irq1_source_enabled(csPio, pis_interrupt0, false);
  pio_interrupt_clear(pio, 5);
  pio_interrupt_clear(pio, 1);
  pio_interrupt_clear(csPio, 4);
  pio_interrupt_clear(csPio, 0);
  pio_sm_set_pins_with_mask64(csPio, csSm, 0, CH446Q_CS_PIN_MASK);
  pio_sm_exec(pio, sm, pio_encode_set(pio_x, 8 - 2));
  pio_sm_exec(pio, sm, pio_encode_set(pio_y, 8 - 2));
  pio_sm_exec(pio, sm, pio_encode_jmp(offset + spi_ch446_pio2cs_offset_entry_point));
  pio_sm_exec(csPio, csSm, pio_encode_jmp(csOffset + ch446_cs_strobe_offset_entry_point));
  pio_sm_set_enabled(csPio, csSm, true);
  pio_sm_set_enabled(pio, sm, true);
}
#endif

// Kick the collected words. Called at the end of a collecting sendAllPaths()
// (dmaCollect already false). Waits for any CPU single-crosspoint send to
// finish first (the arbitration described in the banner). If nothing was
// collected, completes right away.
static void __not_in_flash_func(ch446qDmaKick)(void) {
  uint32_t n = dmaCount;
  if (n == 0 || dmaChan < 0) {
    ch446qSendComplete();
    return;
  }
  // Wait out a CPU single-crosspoint send in flight (chipSelect != -1 until
  // its ISR strobe), then claim the wire for the DMA under the lock.
  unsigned long t0 = micros();
  for (;;) {
    uint32_t save = spin_lock_blocking(sendLock());
    if (chipSelect == -1) {
      dmaIdx = 0;
      dmaActive = true;
      spin_unlock(sendLock(), save);
      break;
    }
    spin_unlock(sendLock(), save);
    if (micros() - t0 > 100000) {
      // The CPU send's own 100 ms timeout will have fired by now; take the wire.
      uint32_t s2 = spin_lock_blocking(sendLock());
      chipSelect = -1;
      dmaIdx = 0;
      dmaActive = true;
      spin_unlock(sendLock(), s2);
      break;
    }
    tight_loop_contents();
  }
  ch446q_dma_sends++;
  ch446q_dma_words += n;
  if (n > ch446q_dma_maxWords) ch446q_dma_maxWords = n;
#if CH446Q_PIO2_CS
  if (csStrobeActive()) {
    // Strobe path (T3.2): mark the last word, arm the one completion IRQ,
    // and start both channels over the same array - the strobe SM's channel
    // first so a chip select is always waiting when the shifter finishes a
    // word (either order works; this one keeps the shifter from stalling on
    // the first handshake).
    dmaWords[n - 1] |= CH446Q_CS_LAST;
    pio_interrupt_clear(csPio, 0);
    pio_set_irq1_source_enabled(csPio, pis_interrupt0, true);
    dma_channel_config cc = dma_channel_get_default_config(dmaChanCs);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, true);
    channel_config_set_write_increment(&cc, false);
    channel_config_set_dreq(&cc, pio_get_dreq(csPio, (uint)csSm, true));
    dma_channel_config c = dma_channel_get_default_config(dmaChan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    __dmb();
    dma_channel_configure(dmaChanCs, &cc, &csPio->txf[csSm], dmaWords, n, true);
    dma_channel_configure(dmaChan, &c, &pio->txf[sm], dmaWords, n, true);
    return;
  }
#endif
  dma_channel_config c = dma_channel_get_default_config(dmaChan);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
  __dmb();
  dma_channel_configure(dmaChan, &c, &pio->txf[sm], dmaWords, n, true);
}

// Recovery for a wedged DMA send: abort the transfer, mark what did not go out
// suspect, restart the SM, complete the request so nobody waits on it forever.
static void __not_in_flash_func(ch446qDmaAbort)(void) {
#if CH446Q_PIO2_CS
  if (csStrobeActive()) {
    // How far did it get? The strobe channel's remaining count says how many
    // words were pushed; up to 8 of those may still sit in its (joined) FIFO
    // plus one in the OSR, so everything from pushed - 9 on is "maybe not
    // strobed" and its chip is marked suspect. Then both channels stop, both
    // machines restart, and the request is completed so nobody waits on it.
    uint32_t n = dmaCount;
    uint32_t remaining = dma_channel_hw_addr(dmaChanCs)->transfer_count & 0x0FFFFFFFu;  // [31:28] = MODE on RP2350
    uint32_t pushed = (remaining <= n) ? (n - remaining) : 0;
    uint32_t from = (pushed > 9) ? (pushed - 9) : 0;
    dma_channel_abort(dmaChanCs);
    dma_channel_abort(dmaChan);
    uint32_t save = spin_lock_blocking(sendLock());
    dmaActive = false;
    spin_unlock(sendLock(), save);
    ch446q_timeout_count++;
    ch446q_dma_stalls++;
    bool marked[12] = {false};
    for (uint32_t i = from; i < n && i < CH446Q_DMA_MAX_WORDS; i++) {
      uint32_t bits = dmaWords[i] & 0xFFFu;
      if (bits == 0) continue;
      int cs = __builtin_ctz(bits);
      if (cs >= 0 && cs < 12 && !marked[cs]) { marked[cs] = true; markChipXYSuspect(cs); }
    }
    ch446qStrobeReset();
    chipSelect = -1;
    ch446qSendComplete();
    return;
  }
#endif
  uint32_t idx = dmaIdx;
  dma_channel_abort(dmaChan);
  uint32_t save = spin_lock_blocking(sendLock());
  dmaActive = false;
  spin_unlock(sendLock(), save);
  ch446q_timeout_count++;
  ch446q_dma_stalls++;
  bool marked[12] = {false};
  for (uint32_t i = idx; i < dmaCount && i < CH446Q_DMA_MAX_WORDS; i++) {
    int cs = dmaCs[i];
    if (cs >= 0 && cs < 12 && !marked[cs]) { marked[cs] = true; markChipXYSuspect(cs); }
  }
  pio_sm_set_enabled(pio, sm, false);
  pio_sm_clear_fifos(pio, sm);
  pio_sm_restart(pio, sm);
  pio_sm_exec(pio, sm, pio_encode_jmp(offset + spi_ch446_multi_cs_offset_entry_point));
  pio_interrupt_clear(pio, 1);  // flag 1 only (the button SM's flag 0 lives on PIO0 too)
  pio_sm_set_enabled(pio, sm, true);
  chipSelect = -1;
  ch446qSendComplete();
}

// Stall watchdog for the DMA path. Called every core2stuff() pass on core 1.
// Progress = the ISR advancing dmaIdx (legacy path) or the strobe channel's
// remaining transfer count falling (strobe path - once it reaches 0 the only
// progress left is the completion IRQ, which lands within microseconds unless
// something is wedged); "stall time" accumulates in bounded per-pass
// increments (<= 5 ms each), so a FlashPark park or a pauseCore2 stretch -
// during which the ISR cannot run and neither can this - adds at most one
// increment when we come back, and never trips the recovery.
static inline uint32_t ch446qDmaProgress(void) {
#if CH446Q_PIO2_CS
  if (csStrobeActive()) return dmaCount - (dma_channel_hw_addr(dmaChanCs)->transfer_count & 0x0FFFFFFFu);  // [31:28] = MODE on RP2350
#endif
  return dmaIdx;
}

void __not_in_flash_func(ch446qDmaService)(void) {
  static uint32_t lastSeenIdx = 0;
  static uint32_t lastPassUs = 0;
  static uint32_t stallUs = 0;
  if (!dmaActive) {
    stallUs = 0;
    lastPassUs = time_us_32();
    lastSeenIdx = ch446qDmaProgress();
    return;
  }
  uint32_t now = time_us_32();
  uint32_t idx = ch446qDmaProgress();
  if (idx != lastSeenIdx) {
    lastSeenIdx = idx;
    stallUs = 0;
  } else {
    uint32_t d = now - lastPassUs;
    if (d > 5000) d = 5000;
    stallUs += d;
  }
  lastPassUs = now;
  if (stallUs < 200000) return;
  stallUs = 0;
  ch446qDmaAbort();
}

int changedPaths[MAX_BRIDGES];
int changedPathsCount = 0;

// Index array for chip-ordered processing while keeping main path array in net order
int chipOrderedIndex[MAX_BRIDGES];
bool chipOrderValid = false;

// Timeout counter for PIO debugging
int ch446q_timeout_count = 0;

#if CH446Q_PIO2_CS
// The strobe path's shifter init: the body of pio_spi_ch446_multi_cs_init
// (ch446.pio.h) for the spi_ch446_pio2cs program - same pins, shift, clock and
// X/Y preload, minus the flag-1 -> PIO0_IRQ_1 routing (this program never
// raises flag 1; its handshake is flags 4/5 with PIO2, never routed anywhere).
static void ch446qShifterInitPio2cs(uint prog_offs, uint n_bits, uint pin_sck, uint pin_mosi) {
  pio_sm_config c = spi_ch446_pio2cs_program_get_default_config(prog_offs);
  sm_config_set_out_pins(&c, pin_mosi, 1);
  sm_config_set_set_pins(&c, pin_sck, 1);
  sm_config_set_sideset_pins(&c, pin_sck);
  sm_config_set_out_shift(&c, false, true, n_bits);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_set_consecutive_pindirs(pio, sm, pin_mosi, 2, true);
  gpio_set_drive_strength(pin_sck, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(pin_mosi, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_slew_rate(pin_sck, GPIO_SLEW_RATE_FAST);
  gpio_set_slew_rate(pin_mosi, GPIO_SLEW_RATE_FAST);
  pio_gpio_init(pio, pin_mosi);
  pio_gpio_init(pio, pin_sck);
  pio_interrupt_clear(pio, 5);
  pio_interrupt_clear(pio, 1);
  pio_sm_init(pio, sm, prog_offs + spi_ch446_pio2cs_offset_entry_point, &c);
  pio_sm_exec(pio, sm, pio_encode_set(pio_x, n_bits - 2));
  pio_sm_exec(pio, sm, pio_encode_set(pio_y, n_bits - 2));
  pio_sm_set_enabled(pio, sm, true);
}

// Claim PIO2 (GPIOBASE 16) for the chip-select strobe SM. Returns false with
// csFallbackReason set if any step is refused; nothing is left half-claimed.
// Runs on core 1 in initCH446Q() - before the LED strips claim their SMs
// (initLEDs comes after this in setupCore2stuff), so PIO2 has no program yet
// and its base can still be changed. No USB I/O here (core 1); X reports.
static bool ch446qCsStrobeInit(void) {
  csFallbackReason = 0;
  if (pio_set_gpio_base(csPio, 16) != PICO_OK) { csFallbackReason = 1; return false; }
  if (!pio_can_add_program(csPio, &ch446_cs_strobe_program)) { csFallbackReason = 2; return false; }
  int s = pio_claim_unused_sm(csPio, false);
  if (s < 0) { csFallbackReason = 3; return false; }
  int chan = dma_claim_unused_channel(false);
  if (chan < 0) { pio_sm_unclaim(csPio, (uint)s); csFallbackReason = 4; return false; }
  csOffset = pio_add_program(csPio, &ch446_cs_strobe_program);
  pio_sm_config c = ch446_cs_strobe_program_get_default_config(csOffset);
  sm_config_set_out_pins(&c, CH446Q_CS_FIRST_GPIO, 12);
  sm_config_set_out_shift(&c, true /* right: bits 11..0 first */, false /* no autopull */, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);   // 8 deep; the RX FIFO is never used
  sm_config_set_clkdiv(&c, 1.0f);
  // Every chip select LOW, output, 12 mA (what pinMode(OUTPUT_12MA) gave the
  // SIO path), then handed to PIO2 - the level is set before the mux flips.
  pio_sm_set_pins_with_mask64(csPio, (uint)s, 0, CH446Q_CS_PIN_MASK);
  pio_sm_set_consecutive_pindirs(csPio, (uint)s, CH446Q_CS_FIRST_GPIO, 12, true);
  for (int i = 0; i < 12; i++) {
    gpio_set_drive_strength(CH446Q_CS_FIRST_GPIO + i, GPIO_DRIVE_STRENGTH_12MA);
    pio_gpio_init(csPio, CH446Q_CS_FIRST_GPIO + i);
  }
  pio_interrupt_clear(csPio, 0);
  pio_interrupt_clear(csPio, 4);
  if (pio_sm_init(csPio, (uint)s, csOffset + ch446_cs_strobe_offset_entry_point, &c) != PICO_OK) {
    // The pin range does not fit the block's base - cannot happen with base
    // 16 and pins 28..39, but never run with a half-configured SM.
    pio_remove_program(csPio, &ch446_cs_strobe_program, csOffset);
    pio_sm_unclaim(csPio, (uint)s);
    dma_channel_unclaim((uint)chan);
    csFallbackReason = 5;
    return false;
  }
  pio_sm_set_enabled(csPio, (uint)s, true);
  // The one interrupt: PIO2 flag 0 -> IRQ1 line, exclusive handler (no
  // shared-chain slot), core 1's NVIC. Source enabled per list send.
  pio_set_irq1_source_enabled(csPio, pis_interrupt0, false);
  irq_set_exclusive_handler(PIO2_IRQ_1, ch446qCsStrobeIsr);
  irq_set_enabled(PIO2_IRQ_1, true);
  dmaChanCs = chan;
  csSm = s;
  return true;
}

// For X: mode and counters of the strobe path.
void ch446qCsStrobeInfo(int* smOut, int* fallbackReason, uint32_t* listIrqs, uint32_t* singles, uint32_t* singleTimeouts) {
  if (smOut) *smOut = csSm;
  if (fallbackReason) *fallbackReason = csFallbackReason;
  if (listIrqs) *listIrqs = cs_list_irqs;
  if (singles) *singles = cs_single_sends;
  if (singleTimeouts) *singleTimeouts = cs_single_timeouts;
}
#else
void ch446qCsStrobeInfo(int* smOut, int* fallbackReason, uint32_t* listIrqs, uint32_t* singles, uint32_t* singleTimeouts) {
  if (smOut) *smOut = -1;
  if (fallbackReason) *fallbackReason = 6;  // not built for this board
  if (listIrqs) *listIrqs = 0;
  if (singles) *singles = 0;
  if (singleTimeouts) *singleTimeouts = 0;
}
#endif

void initCH446Q(void) {

  uint dat = 14;
  uint clk = 15;

  // uint cs = 7;

  // CRITICAL: Initialize GPIO pins for PIO use (required for RP2350B)
  pio_gpio_init(pio, dat);  // Initialize GPIO 14 for PIO0
  pio_gpio_init(pio, clk);  // Initialize GPIO 15 for PIO0

#if CH446Q_PIO2_CS
  // T3.2: the strobe SM on PIO2 first (its base must be set while PIO2 is
  // still empty). With it, the shifter runs the pio2cs program and no
  // PIO0_IRQ_1 handler is registered at all (a shared-chain slot freed);
  // without it, everything below is the legacy path.
  if (ch446qCsStrobeInit()) {
    offset = pio_add_program(pio, &spi_ch446_pio2cs_program);
    ch446qShifterInitPio2cs(offset, 8, clk, dat);
  } else
#endif
  {
    irq_add_shared_handler(PIO0_IRQ_1, isrFromPio,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(PIO0_IRQ_1, true);

    offset = pio_add_program(pio, &spi_ch446_multi_cs_program);
    // uint offsetCS = pio_add_program(pio, &spi_ch446_cs_handler_program);

    // Serial.print("offset: ");
    // Serial.println(offset);

    pio_spi_ch446_multi_cs_init(pio, sm, offset, 8, 1, 0, 1, clk, dat);
#if !defined(OG_JUMPERLESS)
    for (int i = 0; i < 12; i++) {
      pinMode(28 + i, OUTPUT_12MA);
      // digitalWrite(28+i, LOW);
    }
#endif
  }
#if !defined(OG_JUMPERLESS)
  // (V5 chip selects: PIO2's in strobe mode, SIO outputs on the legacy path -
  // both set up above.)
    #else
    // OG: crosspoint chip-selects are split across two GPIO banks - chips A-H on
    // GPIO 6-13 and chips I-L on GPIO 20-23 (matching CS_A..CS_L in the OG
    // reference firmware). The reference also drives every CS LOW (idle) right
    // after pinMode; do the same so the pins are guaranteed SIO outputs at a
    // known idle level before the first sendXYraw()/isrFromPio() CS pulse. The
    // missing LOW init was a divergence from the known-good reference.
    for (int i = 0; i < 8; i++) {
      pinMode(6 + i, OUTPUT_12MA);
      digitalWrite(6 + i, LOW);
      }
      for (int i = 0; i < 4; i++) {
        pinMode(20 + i, OUTPUT_12MA);
        digitalWrite(20 + i, LOW);
        }
    #endif
  // pio_spi_ch446_cs_handler_init(pio, smCS, offsetCS, 256, 1, 8, 20, 6);
  // pinMode(CS_A, OUTPUT);
  // digitalWrite(CS_A, HIGH);

  // Initialize lastChipXY array (all connections off)
  // OPTIMIZATION: Use memset with bitfield (much faster)
  memset(lastChipXY, 0, sizeof(lastChipXY));
  
  chipOrderValid = false; // Initialize chip order as invalid

#if CH446Q_DMA_SEND
  // The DMA-fed list send (T2.3). No panic if none is free: the CPU path
  // stays. USBAudio claims two lazily and AsyncPassthrough two more; the LED
  // strip has its own - plenty of the 16 left.
  dmaChan = dma_claim_unused_channel(false);
#endif
  }

// CRITICAL: Run from RAM to prevent XIP flash cache contention with Core 0
// When both cores execute code from flash simultaneously, they compete for XIP cache
// This causes unpredictable slowdowns. Running Core 2 from RAM eliminates this issue.
void __not_in_flash_func(sendPaths)(int clean, int reqSlot, uint32_t reqGen) {
  // Performance profiling (matches PROFILE_FAST_REFRESH in Commands.cpp)
  #define PROFILE_CORE2_SENDPATHS 0
  unsigned long core2_start = micros();
  unsigned long core2_step = core2_start;

  core2busy = true;
  xbarLatPickup();  // latency probe: the send starts (XbarLatency.h)
  // The mailbox request this send serves (if any) is completed by
  // ch446qSendComplete(): at the end of this function on the CPU path, from
  // the ISR when the last chip select has strobed on the DMA path (T2.3).
  if (reqSlot >= 0) {
    reqSlotPending = reqSlot;
    reqGenPending = reqGen;
  }

  // OPTIMIZATION: Only create chip-ordered index if invalid or doing clean refresh
  // For incremental updates (clean==0), we send paths in net order which is fine
  if (clean == 1 || !chipOrderValid) {
    createChipOrderedIndex();
    #if PROFILE_CORE2_SENDPATHS
    Serial.print("Core 2: createChipOrderedIndex: "); 
    Serial.print(micros() - core2_step); 
    Serial.println(" us");
    core2_step = micros();
    #endif
  }

  if (clean == 1) {
    digitalWrite(RESETPIN, HIGH);
    delayMicroseconds(1000);
    digitalWrite(RESETPIN, LOW);
    clearChipXYSuspect();
    #if PROFILE_CORE2_SENDPATHS
    Serial.print("Core 2: RESET pulse: "); 
    Serial.print(micros() - core2_step); 
    Serial.println(" us");
    core2_step = micros();
    #endif
  }

  routingGeneration++;
  
  sendAllPaths(clean);
  #if PROFILE_CORE2_SENDPATHS
  Serial.print("Core 2: sendAllPaths: "); 
  Serial.print(micros() - core2_step); 
  Serial.println(" us");
  core2_step = micros();
  #endif
  
  core2busy = false;
  // (sendAllPathsCore2 = 0 used to sit here - it erased any request that
  // landed during this send. Completion is core1req::complete() via
  // ch446qSendComplete() now - right here on the CPU path, from the ISR when
  // the DMA-fed send has strobed its last chip - and a request that arrived
  // meanwhile stays posted; T2.2b / T2.3.)
  __dmb();  // Memory barrier so Core 0 sees the update
  if (!dmaActive) {
    ch446qSendComplete();
  }
  
  #if PROFILE_CORE2_SENDPATHS
  unsigned long core2_total = micros() - core2_start;
  Serial.print("Core 2 TOTAL: "); 
  Serial.print(core2_total); 
  Serial.println(" us");
  Serial.println();
  #endif
}



void __not_in_flash_func(refreshPaths)(void) {
  for (int i = 0; i < MAX_BRIDGES; i++) {
    changedPaths[i] = -2;
    }
  chipOrderValid = false; // Invalidate chip order index
  sendAllPaths(1);
  }

// CRITICAL: Run from RAM to prevent XIP flash cache contention
void __not_in_flash_func(sendAllPaths)(int clean) {
  #define PROFILE_SENDALLPATHS 0
  unsigned long startTime = micros();
  unsigned long stepTime = startTime;

#if CH446Q_DMA_SEND
  // DMA-fed list send (T2.3): on core 1 with a channel, COLLECT the words the
  // loops below would have sent (sendXYrawUnchecked appends while dmaCollect
  // is set) and kick them at the end. Core 0 callers (refreshPaths from a
  // command / file load) keep the CPU path. A previous DMA send still in
  // flight is waited out first (its ISR is what completes it).
  bool collecting = (dmaChan >= 0) && ((sio_hw->cpuid & 1) == 1);
  if (collecting) {
    // (the take sites do not start a send while one is in flight, so this
    // wait is a formality; if it ever expires the DMA is wedged - recover)
    unsigned long t0 = micros();
    while (dmaActive && micros() - t0 < 300000) { tight_loop_contents(); }
    if (dmaActive) ch446qDmaAbort();
    dmaCount = 0;
    dmaCollect = true;
  }
#endif
  
  if (clean == 1) {
    // OPTIMIZATION: Use memset to clear lastChipXY (faster than nested loops)
    memset(lastChipXY, 0, sizeof(lastChipXY));
    memset(chipHadConnections, 0, sizeof(chipHadConnections));  // Clear tracking array
    #if PROFILE_SENDALLPATHS
    Serial.print("  clear lastChipXY: "); 
    Serial.print(micros() - stepTime); 
    Serial.println(" us");
    stepTime = micros();
    #endif

    // Send all paths in chip order for hardware efficiency
    int pathsSent = 0;
    for (int i = 0; i < numberOfPaths; i++) {
      int pathIdx = chipOrderValid ? chipOrderedIndex[i] : i;
      sendPath(pathIdx, 1, 0);
      pathsSent++;

      // Update lastChipXY and tracking array
      for (int j = 0; j < 4; j++) {
        if (globalState.connections.paths[pathIdx].chip[j] != -1 && 
            globalState.connections.paths[pathIdx].x[j] != -1 && 
            globalState.connections.paths[pathIdx].y[j] != -1) {
          int chip = globalState.connections.paths[pathIdx].chip[j];
          int x = globalState.connections.paths[pathIdx].x[j];
          int y = globalState.connections.paths[pathIdx].y[j];

          if (chip >= 0 && chip < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
            lastChipXY[chip].connected[y] |= (1 << x);  // Set bit in bitfield
            chipHadConnections[chip] = true;  // Track that this chip has connections
          }
        }
      }
    }
    #if PROFILE_SENDALLPATHS
    Serial.print("  send all paths ("); 
    Serial.print(pathsSent); 
    Serial.print("): "); 
    Serial.print(micros() - stepTime); 
    Serial.println(" us");
    #endif
    
    // Request live crossbar display update via service (waits for colors)
    liveCrossbarService.requestUpdate();
#if CH446Q_DMA_SEND
    if (collecting) { dmaCollect = false; ch446qDmaKick(); }
#endif
    return;
  } else {
    // INCREMENTAL: Only send changed paths
    findDifferentPaths();
    #if PROFILE_SENDALLPATHS
    Serial.print("  findDifferentPaths: "); 
    Serial.print(micros() - stepTime); 
    Serial.println(" us");
    stepTime = micros();
    #endif
    
    int changedCount = 0;
    for (int i = 0; i < numberOfPaths; i++) {
      if (changedPaths[i] == 1) {
        sendPath(i, 1, 0);
        changedCount++;
      }
    }
    #if PROFILE_SENDALLPATHS
    Serial.print("  send changed paths ("); 
    Serial.print(changedCount); 
    Serial.print("): "); 
    Serial.print(micros() - stepTime); 
    Serial.println(" us");
    #endif
    
    // Request live crossbar display update via service (waits for colors)
    liveCrossbarService.requestUpdate();
#if CH446Q_DMA_SEND
    if (collecting) { dmaCollect = false; ch446qDmaKick(); }
#endif
  }
}


void printChipStateArray(Stream *stream) {
  if (stream == nullptr) stream = &Jerial;

  stream->println("Analog Crossbar Array\n\r");


  // for (int i = 0; i < 12; i++) {
  //   stream->print("chip ");
  //   stream->print(i);
  //   stream->print(" ");
  //   for (int j = 0; j < 16; j++) {
  //     stream->print(xName(i, j));
  //     stream->print(" ");
  //     }
  //   stream->println();
  //   }
  // stream->println();
  // ... (rest of the function with stream-> instead of Serial.)

  // for (int i = 0; i < 12; i++) {
  //   for (int j = 0; j < 8; j++) {
  //     Serial.print(yName(i, j));
  //     Serial.print(" ");
  //     }
  //   Serial.println();
  //   }
  // Serial.println();
  int showX = 1;
  int showY = 1;



  for (int blockRow = 0; blockRow < 3; blockRow++) {
    int startChip = blockRow * 4;
    int endChip = startChip + 4;
    // Print chip headers
    stream->print("           ");
    for (int chip = startChip; chip < endChip; chip++) {
      stream->print("  chip ");
      stream->print(chipNumToChar(chip));
      stream->print(" ");
      if (chip < endChip - 1) {
        for (int s = 0; s < 25; s++) stream->print(" "); // spacing between blocks
        }
      }
    stream->println("");
    // Print Y headers for each chip block
    if (showY) {
    stream->print("     ");
    } else {
    stream->print("     ");
      }
    for (int j = 0; j < 4; j++)
      {
      for (int i = 0; i < 8; i++)
        {
        //stream->print(" ");
        if (showY)
          {
          stream->print(i);
          stream->print("  ");
          } else {
            stream->print("   ");
            }
        }
        if (showY && j < 3)
          {
          stream->print("          ");
          } else {
          stream->print("          ");
            }
      }

    stream->println();
    // Print each X row for all 4 chips in this block row
    for (int x = 0; x < 16; x++) {
      for (int chip = startChip; chip < endChip; chip++) {
        //stream->print("x");
        if (showX) {
        stream->print(" ");
        if (x < 10) stream->print(" ");
        stream->print(x);
        } else {
          stream->print("   ");
          }

        stream->print(" "); // space between chip blocks

        for (int y = 0; y < 8; y++) {
          int verticalLine = 0;
          int horizontalLine = 0;
          // Check if any x is connected at this y (vertical line)
          if (lastChipXY[chip].connected[y] != 0) {
            verticalLine = 1;
          }
          // Check if this x is connected at any y (horizontal line)
          for (int j = 0; j < 8; j++) {
            if (lastChipXY[chip].connected[j] & (1 << x)) {
              horizontalLine = 1;
              break;
            }
          }
          
          if (lastChipXY[chip].connected[y] & (1 << x)) {
            stream->print("─█─");
            stream->flush();
          } else {
            if (verticalLine && horizontalLine) {
              stream->print("─┼─");
              stream->flush();
            } else if (verticalLine) {
              stream->print(" │ ");
              stream->flush();
            } else if (horizontalLine) {
              stream->print("───");
              stream->flush();
            } else {
              stream->print(" . ");
              stream->flush();
            }
          }
        }

        stream->print(" "); // space between chip blocks
        stream->print(xName(chip, x));
        stream->print("  ");
        }

      stream->println();
      }
    for (int chip = startChip; chip < endChip; chip++) {
      stream->print("     ");
      //stream->print("y");
      for (int y = 0; y < 8; y++) {

        stream->print(yName(chip, y));
        //stream->print(" ");
        }
      stream->print("     "); // spacing between blocks
      }
    stream->println("\n\n\r"); // extra space between block rows
    }
    stream->flush();
  }

void printLastChipStateArray(void) {

}

/// @brief Print the crossbar array with colors showing which net owns each line
/// At crossings, horizontal segments show in the horizontal net's color,
/// vertical segments show in the vertical net's color
void printChipStateArrayColor(Stream *stream) {
  stream->println("Analog Crossbar Array (Colored by Net)\n\r");
  
  // Make sure terminal colors are assigned to nets
  assignTermColor();

  int showX = 1;
  int showY = 1;

  for (int blockRow = 0; blockRow < 3; blockRow++) {
    int startChip = blockRow * 4;
    int endChip = startChip + 4;
    
    // Print chip headers
    stream->print("           ");
    for (int chip = startChip; chip < endChip; chip++) {
      stream->print("  chip ");
      stream->print(chipNumToChar(chip));
      stream->print(" ");
      if (chip < endChip - 1) {
        for (int s = 0; s < 25; s++) stream->print(" ");
      }
    }
    stream->println("");
    
    // Print Y headers
    if (showY) {
      stream->print("     ");
    } else {
      stream->print("     ");
    }
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 8; i++) {
        if (showY) {
          stream->print(i);
          stream->print("  ");
        } else {
          stream->print("   ");
        }
      }
      if (showY && j < 3) {
        stream->print("          ");
      } else {
        stream->print("          ");
      }
    }
    stream->println();

    // Print each X row for all 4 chips in this block row
    for (int x = 0; x < 16; x++) {
      for (int chip = startChip; chip < endChip; chip++) {
        if (showX) {
          stream->print(" ");
          if (x < 10) stream->print(" ");
          stream->print(x);
        } else {
          stream->print("   ");
        }
        stream->print(" ");

        for (int y = 0; y < 8; y++) {
          int verticalLine = 0;
          int horizontalLine = 0;
          
          // Check if any x is connected at this y (vertical line exists)
          if (lastChipXY[chip].connected[y] != 0) {
            verticalLine = 1;
          }
          
          // Check if this x is connected at any y (horizontal line exists)
          for (int j = 0; j < 8; j++) {
            if (lastChipXY[chip].connected[j] & (1 << x)) {
              horizontalLine = 1;
              break;
            }
          }
          
          // Get the net colors for this x and y
          int xNet = globalState.connections.chipStates[chip].xStatus[x];
          int yNet = globalState.connections.chipStates[chip].yStatus[y];
          int xColor = (xNet > 0 && xNet < MAX_NETS) ? globalState.connections.nets[xNet].termColor : -1;
          int yColor = (yNet > 0 && yNet < MAX_NETS) ? globalState.connections.nets[yNet].termColor : -1;
          
          if (lastChipXY[chip].connected[y] & (1 << x)) {
            // Connection point - both lines belong to the same net (or at least meet here)
            // Use the net that "owns" this crosspoint - typically the X line's net
            int connNet = xNet > 0 ? xNet : yNet;
            int connColor = (connNet > 0 && connNet < MAX_NETS) ? globalState.connections.nets[connNet].termColor : -1;
            changeTerminalColor(connColor, false, stream, false);
            stream->print("─█─");
            changeTerminalColor(-1, false, stream, false);
          } else {
            if (verticalLine && horizontalLine) {
              // Crossing - horizontal in X color, center in Y color
              changeTerminalColor(xColor, false, stream, false);
              stream->print("─");
              changeTerminalColor(yColor, false, stream, false);
              stream->print("┼");
              changeTerminalColor(xColor, false, stream, false );
              stream->print("─");
              changeTerminalColor(-1, false, stream, false);
            } else if (verticalLine) {
              // Just vertical line
              changeTerminalColor(yColor, false, stream, false);
              stream->print(" │ ");
              changeTerminalColor(-1, false, stream, false);
            } else if (horizontalLine) {
              // Just horizontal line
              changeTerminalColor(xColor, false, stream, false);
              stream->print("───");
              changeTerminalColor(-1, false, stream, false);
            } else {
              // No line - just a dot
              changeTerminalColor( 238 , false, stream, false);
              stream->print(" . ");
              changeTerminalColor(-1, false, stream, false);
            }
          }
        }

        stream->print(" ");
        stream->print(xName(chip, x));
        stream->print("  ");
      }
      stream->println();
    }
    
    // Print Y names at bottom
    for (int chip = startChip; chip < endChip; chip++) {
      stream->print("     ");
      for (int y = 0; y < 8; y++) {
        stream->print(yName(chip, y));
      }
      stream->print("     ");
    }
    stream->println("\n\n\r");
  }
  stream->flush();
}

/// @brief Compact color-coded crossbar display - uses single characters per cell
/// Fits more information in less screen space while still showing net colors
/// @param chipsPerRow Number of chips to display per row (default 6)
void printChipStateArrayColorCompact(int chipsPerRow, char blankChar, Stream *stream) {
  stream->println("Crossbar (Compact)\n\r");
  
  // Make sure terminal colors are assigned to nets
  assignTermColor();

  // Clamp chipsPerRow to valid range
  if (chipsPerRow < 2) chipsPerRow = 2;
  if (chipsPerRow > 12) chipsPerRow = 12;

  int numRows = (12 + chipsPerRow - 1) / chipsPerRow;  // Ceiling division

  for (int blockRow = 0; blockRow < numRows; blockRow++) {
    int startChip = blockRow * chipsPerRow;
    int endChip = startChip + chipsPerRow;
    if (endChip > 12) endChip = 12;
    
    // Print chip headers (compact)
    stream->print("   ");
    for (int chip = startChip; chip < endChip; chip++) {
      stream->print(chipNumToChar(chip));
      stream->print("         ");  // 8 chars for 8 Y columns
    }
    stream->println();

    // Print each X row for all chips in this block row
    for (int x = 0; x < 16; x++) {
      // Row number (compact, no leading space for single digit)
      if (x < 10) stream->print(" ");
      stream->print(x);
      stream->print(" ");

      for (int chip = startChip; chip < endChip; chip++) {
        for (int y = 0; y < 8; y++) {
          int verticalLine = 0;
          int horizontalLine = 0;
          
          // Check if any x is connected at this y (vertical line exists)
          if (lastChipXY[chip].connected[y] != 0) {
            verticalLine = 1;
          }
          
          // Check if this x is connected at any y (horizontal line exists)
          for (int j = 0; j < 8; j++) {
            if (lastChipXY[chip].connected[j] & (1 << x)) {
              horizontalLine = 1;
              break;
            }
          }
          
          // Get the net colors for this x and y
          int xNet = globalState.connections.chipStates[chip].xStatus[x];
          int yNet = globalState.connections.chipStates[chip].yStatus[y];
          int xColor = (xNet > 0 && xNet < MAX_NETS) ? globalState.connections.nets[xNet].termColor : -1;
          int yColor = (yNet > 0 && yNet < MAX_NETS) ? globalState.connections.nets[yNet].termColor : -1;
          
          if (lastChipXY[chip].connected[y] & (1 << x)) {
            // Connection point - use the net color
            int connNet = xNet > 0 ? xNet : yNet;
            int connColor = (connNet > 0 && connNet < MAX_NETS) ? globalState.connections.nets[connNet].termColor : -1;
            changeTerminalColor(connColor, false, stream, false);
            stream->print("█");
            changeTerminalColor(-1, false, stream, false);
          } else if (verticalLine && horizontalLine) {
            // Crossing - show in vertical line's color (Y net)
            changeTerminalColor(yColor, false, stream, false);
            stream->print("┼");
            changeTerminalColor(-1, false, stream, false);
          } else if (verticalLine) {
            // Just vertical line
            changeTerminalColor(yColor, false, stream, false);
            stream->print("│");
            changeTerminalColor(-1, false, stream, false);
          } else if (horizontalLine) {
            // Just horizontal line
            changeTerminalColor(xColor, false, stream, false);
            stream->print("─");
            changeTerminalColor(-1, false, stream, false);
          } else {
            // No line
            changeTerminalColor( 238 , false, stream, false);
            stream->print(blankChar);
            changeTerminalColor(-1, false, stream, false);
          }
        }
        stream->print("  ");  // Single space between chips
      }
      stream->println();
      stream->flush();
    }
    stream->println();  // Single blank line between block rows
  }
  stream->flush();
}

// ============================================================================
// Live Crossbar Display - Updates at top of terminal on connection changes
// Uses DECSTBM (Set Top and Bottom Margins) to create a non-scrolling header
// ============================================================================

bool liveCrossbarEnabled = false;
static const int LIVE_CROSSBAR_HEIGHT = 18;  // Header + 16 rows + separator line

void setLiveCrossbarEnabled(bool enabled) {
  liveCrossbarEnabled = enabled;
  if (enabled) {
     Serial.flush();
    // Clear screen and move to home
    Serial.print("\033[2J\033[H");
     Serial.flush();
    // Draw the initial crossbar display
    updateLiveCrossbarDisplay();
    
    // Set scrolling region BELOW the crossbar area (DECSTBM)
    // This makes rows 1-LIVE_CROSSBAR_HEIGHT fixed (non-scrolling)
    // and rows LIVE_CROSSBAR_HEIGHT+1 to bottom scrollable
    Serial.printf("\033[%d;999r", LIVE_CROSSBAR_HEIGHT + 1);
     Serial.flush();
    
    // Move cursor to the scrolling region and print status
    Serial.printf("\033[%d;1H", LIVE_CROSSBAR_HEIGHT + 1);
    Serial.println("--- Live Crossbar Mode (c! to disable) ---\r");
    Serial.println("\r");
    Serial.flush();
  } else {
    // Reset scrolling region to full screen (DECSTBM with no params)
    Serial.print("\033[r");
    // Clear screen and home
    Serial.print("\033[2J\033[H");
    Serial.println("Live crossbar display disabled.\r");
    Serial.flush();
  }
}

/// @brief Update the live crossbar display at top of terminal
/// Uses DECSTBM scrolling region - crossbar is in non-scrolling area at top
/// Uses DECSC/DECRC (ESC 7 / ESC 8) to save/restore cursor position
/// All output is buffered into a single String and written in one shot
/// to prevent garbled display from fragmented escape sequences
void __not_in_flash_func(updateLiveCrossbarDisplay)(void) {
  if (!liveCrossbarEnabled) return;
  
  // Make sure terminal colors are assigned
  assignTermColor();
  
  // Build net lookup from paths array (more reliable than chipStates for multi-hop paths)
  // connectionNet[chip][x][y] = net number for that connection point
  static int8_t connectionNet[12][16][8];
  memset(connectionNet, 0, sizeof(connectionNet));
  
  // Scan all paths and map each (chip, x, y) to its net
  for (int i = 0; i < numberOfPaths; i++) {
    int net = globalState.connections.paths[i].net;
    if (net <= 0) continue;
    
    // Check all 4 possible hops in the path
    for (int hop = 0; hop < 4; hop++) {
      int chip = globalState.connections.paths[i].chip[hop];
      int x = globalState.connections.paths[i].x[hop];
      int y = globalState.connections.paths[i].y[hop];
      
      if (chip >= 0 && chip < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
        connectionNet[chip][x][y] = net;
      }
    }
  }
  
  // Buffer the entire frame to avoid garbled output from fragmented serial writes
  // ~4KB is enough for 12 chips × 16 rows × 8 cols with color escapes
  String buf;
  buf.reserve(4096);
  
  char tmp[32];  // Scratch buffer for sprintf formatting
  
  // Save cursor position with DECSC
  buf += "\0337";
  // Move to home position for drawing (top-left, in non-scrolling area)
  buf += "\033[H";
  
  // Print chip headers
  for (int chip = 0; chip < 12; chip++) {
    buf += chipNumToChar(chip);
    buf += "        ";
  }
  buf += "\033[K\r\n";  // Clear to EOL + CR + newline
  
  // Track last color to avoid redundant escape sequences
  int lastColor = -2;  // -2 = unset, -1 = reset, >= 0 = color
  
  // Print each X row (16 rows)
  for (int x = 0; x < 16; x++) {
    for (int chip = 0; chip < 12; chip++) {
      for (int y = 0; y < 8; y++) {
        // Check connection state
        bool isConnected = lastChipXY[chip].connected[y] & (1 << x);
        bool verticalLine = lastChipXY[chip].connected[y] != 0;
        bool horizontalLine = false;
        
        for (int j = 0; j < 8; j++) {
          if (lastChipXY[chip].connected[j] & (1 << x)) {
            horizontalLine = true;
            break;
          }
        }
        
        // Get net from our lookup (built from paths, not chipStates)
        int connNet = connectionNet[chip][x][y];
        int color = (connNet > 0 && connNet < MAX_NETS) ? globalState.connections.nets[connNet].termColor : -1;
        
        // For lines that pass through but don't connect here, find a net from the same X or Y line
        if (connNet == 0) {
          // Check if there's a net on this X line (horizontal)
          if (horizontalLine) {
            for (int j = 0; j < 8 && connNet == 0; j++) {
              connNet = connectionNet[chip][x][j];
            }
          }
          // Check if there's a net on this Y line (vertical)
          if (verticalLine && connNet == 0) {
            for (int j = 0; j < 16 && connNet == 0; j++) {
              if (connectionNet[chip][j][y] > 0) {
                connNet = connectionNet[chip][j][y];
              }
            }
          }
          color = (connNet > 0 && connNet < MAX_NETS) ? globalState.connections.nets[connNet].termColor : -1;
        }
        
        // Determine the character and color for this cell
        int cellColor;
        const char *cellChar;
        
        if (isConnected) {
          cellColor = color;
          cellChar = "█";
        } else if (verticalLine && horizontalLine) {
          cellColor = color;
          cellChar = "┼";
        } else if (verticalLine) {
          cellColor = color;
          cellChar = "│";
        } else if (horizontalLine) {
          cellColor = color;
          cellChar = "─";
        } else {
          cellColor = 238;  // Dim gray for empty cells
          cellChar = ".";
        }
        
        // Only emit color escape if it changed from previous cell
        if (cellColor != lastColor) {
          if (cellColor >= 0) {
            snprintf(tmp, sizeof(tmp), "\033[38;5;%dm", cellColor);
            buf += tmp;
          } else {
            buf += "\033[0m";
          }
          lastColor = cellColor;
        }
        buf += cellChar;
      }
      // Reset color between chips and add separator space
      if (lastColor != -1) {
        buf += "\033[0m";
        lastColor = -1;
      }
      buf += " ";
    }
    buf += "\033[K\r\n";  // Clear to EOL + CR + newline
  }
  
  // Separator line (row 18)
  buf += "                                                                                            \033[K";
  
  // Restore cursor position with DECRC (returns to where it was in scrolling region)
  buf += "\0338";
  
  // Write entire frame in one shot - prevents garbled escape sequences

  Serial.print(buf);
  Serial.flush();

  // Reset changeTerminalColor's internal tracking state since we bypassed it
  // force=true ensures it resets even though terminal was already reset by buffer content
  changeTerminalColor(-1, false, &Serial, true);
}

// New function to update the current chip state array based on paths
// CRITICAL: Run from RAM to prevent XIP flash cache contention
void __not_in_flash_func(updateChipStateArray)() {
  // Clear changed paths array (only clear what we need)
  memset(changedPaths, -1, numberOfPaths * sizeof(int));
  
  // OPTIMIZATION: Use bitfield instead of bool array (8x smaller, fits in cache)
  // 16 bytes per chip vs 128 bytes
  chipXYBitfield newChipXY[12];
  memset(newChipXY, 0, sizeof(newChipXY));
  bool newChipHasConnections[12] = {false};
  
  // Build new connection map and track which chips are used
  for (int i = 0; i < numberOfPaths; i++) {
    for (int j = 0; j < 4; j++) {
      int chip = globalState.connections.paths[i].chip[j];
      int x = globalState.connections.paths[i].x[j];
      int y = globalState.connections.paths[i].y[j];
      
      // Validate and mark connection using bitfield
      if (chip >= 0 && chip < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
        newChipXY[chip].connected[y] |= (1 << x);  // Set bit
        newChipHasConnections[chip] = true;
      }
    }
  }

  // Mark paths that have changed (new or modified connections)
  for (int i = 0; i < numberOfPaths; i++) {
    bool pathChanged = false;
    
    for (int j = 0; j < 4; j++) {
      int chip = globalState.connections.paths[i].chip[j];
      int x = globalState.connections.paths[i].x[j];
      int y = globalState.connections.paths[i].y[j];
      
      if (chip >= 0 && chip < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
        // Check if this crosspoint changed state using bitfield
        bool wasConnected = (lastChipXY[chip].connected[y] & (1 << x)) != 0;
        bool nowConnected = (newChipXY[chip].connected[y] & (1 << x)) != 0;
        if (wasConnected != nowConnected) {
          pathChanged = true;
          break;
        }
      }
    }
    
    if (pathChanged) {
      changedPaths[i] = 1;
    }
  }

  // CRITICAL: Handle disconnections
  // Only scan chips that HAD connections in the previous state (tracked in chipHadConnections)
  // OPTIMIZATION: Use bitwise operations for faster scanning
  for (int chip = 0; chip < 12; chip++) {
    if (!chipHadConnections[chip]) continue;  // Skip chips that never had connections
    
    // Scan this chip for disconnections using bitfield
    for (int y = 0; y < 8; y++) {
      // XOR to find differences: bits that changed from 1 to 0
      uint16_t removed = lastChipXY[chip].connected[y] & ~newChipXY[chip].connected[y];
      
      // Send disconnect for each removed connection
      if (removed) {
        for (int x = 0; x < 16; x++) {
          if (removed & (1 << x)) {
            sendXYrawUnchecked(chip, x, y, 0);
          }
        }
      }
    }
  }

  // Update tracking for next iteration
  memcpy(chipHadConnections, newChipHasConnections, sizeof(chipHadConnections));

  // OPTIMIZATION: Copy new state to lastChipXY efficiently using memcpy
  // Bitfield version is 8x smaller so this is very fast
  memcpy(lastChipXY, newChipXY, sizeof(lastChipXY));
}

// Updated findDifferentPaths to use the chip state approach
// CRITICAL: Run from RAM to prevent XIP flash cache contention
void __not_in_flash_func(findDifferentPaths)(void) {
  updateChipStateArray();
  }

void __not_in_flash_func(sendPath)(int i, int setOrClear, int newOrLast) {

  uint32_t chAddress = 0;

  int chipToConnect = 0;
  int chYdata = 0;
  int chXdata = 0;


    for (int chip = 0; chip < 4; chip++) {
      if (globalState.connections.paths[i].chip[chip] != -1) {
        // (chipSelect is claimed inside sendXYrawUnchecked - it is the
        // single-send token there; setting it here would hold the token
        // through a whole collected list and stall the kick.)
        chipToConnect = globalState.connections.paths[i].chip[chip];

        if (globalState.connections.paths[i].y[chip] == -1 || globalState.connections.paths[i].x[chip] == -1) {
          if (debugNTCC)
            Serial.print("!");

          continue;
          }

        // Bulk path send was vetted by validateAllPaths(); skip per-crosspoint check.
        sendXYrawUnchecked(chipToConnect, globalState.connections.paths[i].x[chip],
                           globalState.connections.paths[i].y[chip], setOrClear);
        }
      }
  
  }

void __not_in_flash_func(sendXYrawUnchecked)(int chip, int x, int y, int setOrClear, unsigned long timeoutUs) {
  uint32_t chAddress = 0;
#if !CH446Q_DMA_SEND
  // OG: the ISR strobes chipSelect. (V5 claims it under the arbitration lock
  // below - it is the single-send token there, and must not be clobbered here.)
  chipSelect = chip;
#endif

  if (chip >= 0 && chip < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
    if (setOrClear == 1) {
      lastChipXY[chip].connected[y] |= (1 << x);
    } else {
      lastChipXY[chip].connected[y] &= ~(1 << x);
    }
  }

#if !defined(OG_JUMPERLESS)
  // CRITICAL SAFETY: Chip K voltage source protection
  // Chip K X positions: 4=TOP_RAIL, 5=BOTTOM_RAIL, 6=DAC1, 7=DAC0, 15=GND
  // NEVER allow multiple voltage sources on the same Y (would short them together!)
#define CHIP_K_VOLTAGE_SOURCES 0x80F0 // Bits: 15,7,6,5,4

  if (chip == CHIP_K && setOrClear == 1 && x >= 0 && x < 16 && y >= 0 && y < 8) {
    if ((1 << x) & CHIP_K_VOLTAGE_SOURCES) {
      uint16_t otherVoltages = CHIP_K_VOLTAGE_SOURCES & ~(1 << x);
      uint16_t conflicting = lastChipXY[CHIP_K].connected[y] & otherVoltages;
      if (conflicting) {
        for (int conflictX = 0; conflictX < 16; conflictX++) {
          if (conflicting & (1 << conflictX)) {
            sendXYrawUnchecked(CHIP_K, conflictX, y, 0, timeoutUs);
          }
        }
      }
    }
  }
#endif

  int chYdata = y;
  int chXdata = x;

  chYdata = chYdata << 5;
  chYdata = chYdata & 0b11100000;

  chXdata = chXdata << 1;
  chXdata = chXdata & 0b00011110;

  chAddress = chYdata | chXdata;

  if (setOrClear == 1) {
    chAddress = chAddress | 0b00000001;
  }

  chAddress = chAddress << 24;
  // The word both machines read on the strobe path (T3.2): address byte on
  // top, chip-select bits below. The legacy shifter discards the low bits.
  const uint32_t word = chAddress | ch446qCsBits(chip);
  (void)word;  // (the OG's legacy path below sends chAddress alone)

#if CH446Q_DMA_SEND
  if (dmaCollect && ((sio_hw->cpuid & 1) == 1)) {
    // List send being collected (T2.3, core 1 only - the buffer is core 1's;
    // a core-0 caller landing here mid-collect takes the CPU path below and
    // waits for the DMA like everyone else): queue the word and its chip. A full
    // buffer flushes what is there and waits for it, so the wire order is
    // preserved. (chipSelect is not ours to touch here - it is the single-send
    // token, possibly held by a core-0 caller.)
    if (dmaCount >= CH446Q_DMA_MAX_WORDS) {
      dmaCollect = false;
      ch446qDmaKick();
      unsigned long t0 = micros();
      while (dmaActive && micros() - t0 < 300000) { tight_loop_contents(); }
      if (dmaActive) ch446qDmaAbort();
      dmaCount = 0;
      dmaCollect = true;
    }
    dmaWords[dmaCount] = word;
    dmaCs[dmaCount] = (uint8_t)chip;
    dmaCount = dmaCount + 1;
    return;
  }
  // A single-crosspoint CPU send while a DMA send is in flight would put a
  // foreign word into the FIFO and shift every later chip select by one -
  // wait it out (bounded by the same timeout as the handshake below), then
  // claim the wire under the arbitration lock. chipSelect == -1 is the token:
  // one single at a time across both cores (a core-0 refreshPaths and a
  // core-1 tap used to interleave freely - the strobe path carries the chip
  // in-band so the wire was never wrong, but two pollers on one done flag
  // would be), and it is released by the ISR / the poll below, never here.
  {
    unsigned long t0 = micros();
    for (;;) {
      uint32_t save = spin_lock_blocking(sendLock());
      if (!dmaActive && chipSelect == -1) {
        chipSelect = chip;
        spin_unlock(sendLock(), save);
        break;
      }
      spin_unlock(sendLock(), save);
      if (micros() - t0 > timeoutUs) {
        // The stall watchdog will clear a wedged DMA, the holder's own
        // timeout a wedged single; do not queue behind either.
        return;
      }
      tight_loop_contents();
    }
  }
#endif

#if CH446Q_PIO2_CS
  if (csStrobeActive()) {
    // Strobe path: the same word into both TX FIFOs (LAST set, so the strobe
    // SM raises flag 0 when this one crosspoint has been strobed), then poll
    // that flag - no ISR involved, ~1 us. A timeout means a wedged machine:
    // count it, mark the chip suspect, put both machines back at their entry
    // points (the same recovery the legacy path did with the SM restart).
    cs_single_sends++;
    pio_interrupt_clear(csPio, 0);
    pio_sm_put(csPio, (uint)csSm, word | CH446Q_CS_LAST);
    pio_sm_put(pio, sm, word);
    // ~0.6 us on the wire (86 PIO cycles at 150 MHz) - no encoderServiceYield()
    // in this loop: that yield was for the legacy ~30 us ISR handshake wait,
    // and it costs more than this whole wait.
    uint32_t wait_start = time_us_32();
    while (!pio_interrupt_get(csPio, 0)) {
      tight_loop_contents();
      if (time_us_32() - wait_start > timeoutUs) {
        ch446q_timeout_count++;
        cs_single_timeouts++;
        markChipXYSuspect(chip);
        // No Serial/USB I/O here (core 1 callers).
        ch446qStrobeReset();
        break;
      }
    }
    pio_interrupt_clear(csPio, 0);
    chipSelect = -1;
    return;
  }
#endif

  pio_sm_put(pio, sm, chAddress);

  unsigned long wait_start = micros();
  while (chipSelect != -1) {
    tight_loop_contents();
    encoderServiceYield();

    if (micros() - wait_start > timeoutUs) {
      ch446q_timeout_count++;
      markChipXYSuspect(chip);
      // CRITICAL: sendXYraw() runs on Core 1. Do NOT do Serial/USB I/O here.
      chipSelect = -1;
      pio_sm_set_enabled(pio, sm, false);
      delayMicroseconds(100);
      pio_sm_set_enabled(pio, sm, true);
      pio_interrupt_clear(pio, sm);
      isrFromPio();
      break;
    }
  }
}

int __not_in_flash_func(sendXYraw)(int chip, int x, int y, int setOrClear, unsigned long timeoutUs) {
#if !defined(OG_JUMPERLESS)
  if (setOrClear == 1 && sendXYrawCheckEnabled &&
      chip >= 0 && chip < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
    // Chip-K source switch: find conflicting sources on this Y, but only
    // SIMULATE their disconnect for the short check - hardware stays
    // untouched unless the connect is approved.
#define CHIP_K_VOLTAGE_SOURCES_CHK 0x80F0
    uint16_t conflicting = 0;
    if (chip == CHIP_K && ((1 << x) & CHIP_K_VOLTAGE_SOURCES_CHK)) {
      uint16_t otherVoltages = CHIP_K_VOLTAGE_SOURCES_CHK & ~(1 << x);
      conflicting = lastChipXY[CHIP_K].connected[y] & otherVoltages;
    }

    if (wouldShortCrosspointMasked(chip, x, y, conflicting ? CHIP_K : -1, y,
                                   conflicting)) {
      sendxy_blocked_count++;
      // No USB I/O on Core 1 — counter is readable from Core 0.
      return -1;
    }

    if (conflicting) {
      for (int conflictX = 0; conflictX < 16; conflictX++) {
        if (conflicting & (1 << conflictX)) {
          sendXYrawUnchecked(CHIP_K, conflictX, y, 0, timeoutUs);
        }
      }
    }
  }
#endif
  sendXYrawUnchecked(chip, x, y, setOrClear, timeoutUs);
  return 0;
}

void createXYarray(void) { }

// Creates an index array sorted by chip, x, y while keeping main path array in net order
void createChipOrderedIndex() {
  // Initialize index array
  for (int i = 0; i < numberOfPaths; i++) {
    chipOrderedIndex[i] = i;
    }
  
  // Sort the index array based on chip, x, y values of the paths they point to
  for (int i = 0; i < numberOfPaths - 1; i++) {
    for (int j = 0; j < numberOfPaths - i - 1; j++) {
      bool swap = false;
      for (int k = 0; k < 4; k++) {
        int idx1 = chipOrderedIndex[j];
        int idx2 = chipOrderedIndex[j + 1];
        
        if (globalState.connections.paths[idx1].chip[k] < globalState.connections.paths[idx2].chip[k]) break;
        if (globalState.connections.paths[idx1].chip[k] > globalState.connections.paths[idx2].chip[k]) { swap = true; break; }
        if (globalState.connections.paths[idx1].x[k] < globalState.connections.paths[idx2].x[k]) break;
        if (globalState.connections.paths[idx1].x[k] > globalState.connections.paths[idx2].x[k]) { swap = true; break; }
        if (globalState.connections.paths[idx1].y[k] < globalState.connections.paths[idx2].y[k]) break;
        if (globalState.connections.paths[idx1].y[k] > globalState.connections.paths[idx2].y[k]) { swap = true; break; }
        }
      if (swap) {
        int temp = chipOrderedIndex[j];
        chipOrderedIndex[j] = chipOrderedIndex[j + 1];
        chipOrderedIndex[j + 1] = temp;
        }
      }
    }
  chipOrderValid = true;
  }

// Legacy function name kept for compatibility, but now creates index instead of sorting
void sortPathsByChipXY() {
  createChipOrderedIndex();
  }

// Copy justXY bool array to bitfield (for conversion if needed)
void copyJustXYToBitfield(const struct justXY& src, chipXYBitfield& dst) {
  for (int y = 0; y < 8; y++) {
    dst.connected[y] = 0;
    for (int x = 0; x < 16; x++) {
      if (src.connected[x][y]) {
        dst.connected[y] |= (1 << x);
        }
      }
    }
  }

// Copy bitfield to justXY bool array (for conversion if needed)
void copyBitfieldToJustXY(const chipXYBitfield& src, struct justXY& dst) {
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 16; x++) {
      dst.connected[x][y] = (src.connected[y] & (1 << x)) != 0;
      }
    }
  }

// Capture current chipXY state into bitfield array
// This snapshots the complete crossbar state for all 12 chips
void captureCurrentChipXYState(chipXYBitfield snapshot[12]) {
  // OPTIMIZATION: Since lastChipXY is already a bitfield, just copy it directly!
  memcpy(snapshot, lastChipXY, sizeof(chipXYBitfield) * 12);
}

// Apply a complete chipXY state snapshot, sending only changed connections
// This preserves existing unchanged connections while switching ADC routing
// KEY OPTIMIZATION: Only sends changes, not entire state
void applyChipXYState(const chipXYBitfield targetState[12]) {
  for (int chip = 0; chip < 12; chip++) {
    for (int y = 0; y < 8; y++) {
      uint16_t currentRow = lastChipXY[chip].connected[y];  // Already bitfield!
      uint16_t targetRow = targetState[chip].connected[y];
      
      // Find differences using XOR
      uint16_t changes = currentRow ^ targetRow;
      if (changes) {
        // Send only changed connections
        for (int x = 0; x < 16; x++) {
          if (changes & (1 << x)) {
            bool newState = (targetRow & (1 << x)) != 0;
            sendXYrawUnchecked(chip, x, y, newState ? 1 : 0);
            // Update lastChipXY bitfield
            if (newState) {
              lastChipXY[chip].connected[y] |= (1 << x);   // Set bit
            } else {
              lastChipXY[chip].connected[y] &= ~(1 << x);  // Clear bit
            }
          }
        }
      }
    }
  }
}

// Capture chipXY state, EXCLUDING entire chip K
// Used by INPUT FakeGPIO pins to avoid capturing OUTPUT pin voltage switching state
// We exclude ALL of chip K because OUTPUT pins use chip K for voltage source switching,
// and any interference (even on different Y rows) can cause timing/state issues.
void captureCurrentChipXYStateExcludeChipK(chipXYBitfield snapshot[12]) {
  for (int chip = 0; chip < 12; chip++) {
    if (chip == CHIP_K) {
      // Clear chip K in snapshot - don't capture its state at all
      for (int y = 0; y < 8; y++) {
        snapshot[chip].connected[y] = 0;
      }
    } else {
      // Copy current state for all other chips
      snapshot[chip] = lastChipXY[chip];
    }
  }
}

// Apply chipXY state, EXCLUDING entire chip K
// Used by INPUT FakeGPIO pins to route to ADC without disturbing OUTPUT pin voltage switching
// We skip ALL of chip K to avoid any interference with OUTPUT pin operations.
void applyChipXYStateExcludeChipK(const chipXYBitfield targetState[12]) {
  for (int chip = 0; chip < 12; chip++) {
    // CRITICAL: Skip chip K entirely to preserve OUTPUT pin voltage switching state
    if (chip == CHIP_K) continue;
    
    for (int y = 0; y < 8; y++) {
      uint16_t currentRow = lastChipXY[chip].connected[y];
      uint16_t targetRow = targetState[chip].connected[y];
      
      // Find differences using XOR
      uint16_t changes = currentRow ^ targetRow;
      if (changes) {
        // Send only changed connections
        for (int x = 0; x < 16; x++) {
          if (changes & (1 << x)) {
            bool newState = (targetRow & (1 << x)) != 0;
            sendXYrawUnchecked(chip, x, y, newState ? 1 : 0);
            // Update lastChipXY bitfield
            if (newState) {
              lastChipXY[chip].connected[y] |= (1 << x);   // Set bit
            } else {
              lastChipXY[chip].connected[y] &= ~(1 << x);  // Clear bit
            }
          }
        }
      }
    }
  }
}