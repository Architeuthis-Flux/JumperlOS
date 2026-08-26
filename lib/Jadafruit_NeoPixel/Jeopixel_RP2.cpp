#if defined(ARDUINO_ARCH_RP2040)// RP2040 specific driver

#include "JeoPixel.h"
#include <malloc.h> // For memalign - cache-aligned buffer allocation

bool JeoPixel::rp2040claimPIO(void) {
  // Find a PIO with enough available space in its instruction memory
  pio = NULL;

  // A preferred block first (setPreferredPIO): only if it has a free SM, room
  // for the program and - RP2350B, where a block reaches 32 of the 48 pins -
  // a GPIOBASE that covers this pin. Otherwise the SDK's search below.
  bool claimed = false;
  if (preferred_pio != NULL) {
    uint base = pio_get_gpio_base(preferred_pio);
    bool reaches = ((uint)pin >= base) && ((uint)pin < base + 32u);
    if (reaches && pio_can_add_program(preferred_pio, &ws2812_program)) {
      int s = -1;
      if (preferred_sm >= 0 && preferred_sm <= 3 && !pio_sm_is_claimed(preferred_pio, (uint)preferred_sm)) {
        pio_sm_claim(preferred_pio, (uint)preferred_sm);
        s = preferred_sm;
      } else {
        s = pio_claim_unused_sm(preferred_pio, false);
      }
      if (s >= 0) {
        pio = preferred_pio;
        pio_sm = (uint)s;
        pio_program_offset = pio_add_program(pio, &ws2812_program);
        claimed = true;
      }
    }
  }

  if (!claimed &&
      ! pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_program, 
                                                         &pio, &pio_sm, &pio_program_offset, 
                                                         pin, 1, true)) {
    pio = NULL;
    pio_sm = -1;
    pio_program_offset = 0;
    return false; // No PIO available
  }

  // yay ok!
  
  if (is800KHz) {
    // 800kHz, 8 bit transfers
    ws2812_program_init(pio, pio_sm, pio_program_offset, pin, 800000, 8);
  } else {
    // 400kHz, 8 bit transfers
    ws2812_program_init(pio, pio_sm, pio_program_offset, pin, 400000, 8);
  }

  // Log the placement in the app's PIO registry (defined in src/; always
  // linked with this lib in this project).
  {
    extern void pioRegistryLog(PIO pio, int sm, uint offset, uint length, const char* name);
    pioRegistryLog(pio, (int)pio_sm, pio_program_offset, ws2812_program.length,
                   pio_owner_name ? pio_owner_name : "ws2812");
  }

  // OPTIMIZATION: Try to claim a DMA channel for non-blocking LED updates
  dma_channel = dma_claim_unused_channel(false); // false = don't panic if unavailable
  use_dma = (dma_channel >= 0);
  
  if (use_dma) {
    // Configure DMA channel for PIO transfers
    dma_channel_config c = dma_channel_get_default_config(dma_channel);
    
    // Transfer 32-bit words (each LED byte is shifted to top 8 bits)
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    
    // Use PIO TX FIFO DREQ to pace transfers (won't overrun FIFO)
    channel_config_set_dreq(&c, pio_get_dreq(pio, pio_sm, true));
    
    // Read from incrementing memory address
    channel_config_set_read_increment(&c, true);
    
    // Write to same PIO FIFO address
    channel_config_set_write_increment(&c, false);
    
    // CRITICAL FIX FOR RP2350: Set DMA to HIGH priority to prevent FIFO underruns
    // RP2350 runs at 150MHz with different bus arbitration than RP2040
    // Low-priority DMA can be starved by CPU cache fills, causing PIO FIFO underruns
    // which corrupt WS2812 timing and cause "LED shifting" glitches
    channel_config_set_high_priority(&c, true);
    
    // Enable bus snooping for cache coherency (RP2350B specific)
    // This ensures DMA sees freshly written pixel data even if in cache
    channel_config_set_sniff_enable(&c, false);  // Sniff not needed for reads
    
    // Configure the channel (but don't set source/count/start yet - done in show())
    dma_channel_configure(
      dma_channel,          // Channel to configure
      &c,                   // Configuration
      &pio->txf[pio_sm],    // Destination: PIO TX FIFO
      NULL,                 // Source: set in show()
      0,                    // Transfer count: set in show()
      false                 // Don't start yet
    );
    
    // Allocate DMA buffer (will be populated in show())
    // Buffer size matches the number of LED bytes needed
    // Note: numBytes not available here, will be allocated in updateLength() or first show()
  }

  return true;
}

void JeoPixel::rp2040releasePIO(void) {
  if (pio == NULL) 
    return;

  // Release DMA channel if we claimed one
  if (use_dma && dma_channel >= 0) {
    dma_channel_abort(dma_channel);  // Stop any ongoing transfer
    dma_channel_unclaim(dma_channel);
    dma_channel = -1;
    use_dma = false;
  }
  
  // Free DMA buffers if allocated
  if (dma_buffer) {
    free(dma_buffer);
    dma_buffer = NULL;
    dma_buffer_size = 0;
  }
  
  if (dma_buffer_backup) {
    free(dma_buffer_backup);
    dma_buffer_backup = NULL;
    dma_has_pending = false;
  }

  pio_remove_program_and_unclaim_sm(&ws2812_program, pio, pio_sm,  pio_program_offset);
}


// Private, called from show()
// CRITICAL: Run from RAM to avoid flash cache misses during DMA operations
void __not_in_flash_func(JeoPixel::rp2040Show)(uint8_t *pixels, uint32_t numBytes)
{
  // verify we have a valid PIO and state machine
  if (! pio || (pio_sm < 0)) {
    return;
  }

  // DMA PATH: Non-blocking transfer for faster LED updates
  if (use_dma && dma_channel >= 0) {
    // CRITICAL SECTION: Prevent interrupts during buffer state checks
    uint32_t interrupts = save_and_disable_interrupts();
    
    // Check if DMA is still busy from previous transfer
    bool dma_busy = dma_channel_is_busy(dma_channel);
    
    if (dma_busy) {
      // DMA is still running - use double-buffering strategy
      
      // Allocate backup buffer if needed
      if (!dma_buffer_backup || dma_buffer_size < numBytes) {
        // Release interrupts during allocation (can be slow)
        restore_interrupts(interrupts);
        
        if (dma_buffer_backup) {
          free(dma_buffer_backup);
        }
        
        size_t buffer_size = numBytes * sizeof(uint32_t);
        size_t aligned_size = (buffer_size + 15) & ~15;
        
        #ifdef __NEWLIB__
        dma_buffer_backup = (uint32_t *)memalign(16, aligned_size);
        #else
        dma_buffer_backup = (uint32_t *)malloc(aligned_size);
        #endif
        
        // Re-acquire lock after allocation
        interrupts = save_and_disable_interrupts();
      }
      
      if (dma_buffer_backup) {
        // OPTIMIZATION: Prepare backup buffer with new frame data
        // This happens while DMA is still sending previous frame
        for (uint32_t i = 0; i < numBytes; i++) {
          dma_buffer_backup[i] = ((uint32_t)pixels[i]) << 24;
        }
        __dmb(); // Ensure backup buffer is written before marking pending
        
        // Atomically mark that we have pending data
        dma_has_pending = true;
        dma_pending_bytes = numBytes;
        
        restore_interrupts(interrupts);
        
        // Return immediately - frame will be sent when current DMA completes
        return;
      }
      
      restore_interrupts(interrupts);
      // If backup allocation failed, fall through to wait
    } else {
      // DMA is idle - release lock before proceeding
      restore_interrupts(interrupts);
    }
    
    // CRITICAL SECTION: Check and swap pending buffer atomically
    interrupts = save_and_disable_interrupts();
    
    // Check if we have a pending frame that was queued while DMA was busy
    // We'll use this buffer, but we DON'T use buffer_already_prepared flag
    // because we'll refresh it from pixels[] to prevent stale data glitches
    bool use_pending_buffer = false;
    if (dma_has_pending && dma_buffer_backup) {
      // Verify DMA is truly idle before proceeding
      if (!dma_channel_is_busy(dma_channel)) {
        // We have a pending buffer - we'll use it but refresh from pixels[]
        use_pending_buffer = true;
        numBytes = dma_pending_bytes;
        dma_has_pending = false;
      }
      // If DMA became busy during check, leave pending flag set and retry next time
    }
    
    restore_interrupts(interrupts);
    
    // Wait for any previous DMA transfer to complete before starting new one
    // This ensures we never start a transfer while one is active
    dma_channel_wait_for_finish_blocking(dma_channel);
    
    // If using pending buffer, swap it to be the active buffer now
    if (use_pending_buffer) {
      uint32_t *temp = dma_buffer;
      dma_buffer = dma_buffer_backup;
      dma_buffer_backup = temp;
    }
    
    // Lazily allocate main DMA buffer if needed (or reallocate if size changed)
    if (!dma_buffer || dma_buffer_size < numBytes) {
      // Free old buffer if exists
      if (dma_buffer) {
        free(dma_buffer);
      }
      
      // CRITICAL: Allocate 16-byte aligned buffer for cache coherency
      // RP2350B cache lines are 16 bytes - alignment prevents cache line splitting
      // which can cause DMA to read stale data even with memory barriers
      size_t buffer_size = numBytes * sizeof(uint32_t);
      // Round up to next 16-byte boundary
      size_t aligned_size = (buffer_size + 15) & ~15;
      
      // Use memalign for cache-line aligned allocation (fallback to malloc if unavailable)
      #ifdef __NEWLIB__
      dma_buffer = (uint32_t *)memalign(16, aligned_size);
      #else
      // If memalign not available, use malloc and pray it's aligned
      dma_buffer = (uint32_t *)malloc(aligned_size);
      #endif
      
      dma_buffer_size = dma_buffer ? numBytes : 0;
      
      // CRITICAL: If allocation failed, disable DMA and fall back to blocking mode
      if (!dma_buffer) {
        use_dma = false;
        // Fall through to blocking mode below
      }
    }
    
    if (dma_buffer) {
      // CRITICAL SECTION: Atomically prepare and start DMA transfer
      uint32_t interrupts = save_and_disable_interrupts();
      
      // ALWAYS prepare fresh buffer from current pixels[] to prevent stale data glitches
      // This is the key fix for "shifting LEDs" - never use old buffered data
      // Copy from pixel buffer to DMA buffer in one atomic operation
      for (uint32_t i = 0; i < numBytes; i++) {
        dma_buffer[i] = ((uint32_t)pixels[i]) << 24;
      }
      
      __dmb();
      __dsb();

      // DMA the whole buffer from index 0. Do not prefill + offset: the
      // ws2812 program stalls with the pin LOW (extra reset — harmless),
      // but a fixed +4 skip while leftover FIFO words remain prefixes
      // those leftovers onto the new frame (one-pixel shift flicker).
      if (numBytes > 0) {
        dma_channel_transfer_from_buffer_now(dma_channel, dma_buffer, numBytes);
        __dsb();
        dma_awaiting_latch = true;
      }

      restore_interrupts(interrupts);
      
      // ASYNC MODE: Return immediately - DMA runs in background
      // Backup buffer allows us to queue the next frame if called while DMA busy
      return; // DMA path complete - transfer happening in background!
    }
    // If malloc failed, fall through to blocking mode
  }

  // FALLBACK PATH: Blocking PIO transfer (original method)
  while(numBytes--)
    // Bits for transmission must be shifted to top 8 bits
    pio_sm_put_blocking(pio, pio_sm, ((uint32_t)*pixels++)<< 24);
}
#endif

// C5 (Jumperless task #26): the probe-LED merged program needs to remove and
// re-add this TU's ws2812 program copy around mode flips. Only .length and
// the instructions matter to pio_remove_program/pio_add_program, so exposing
// this TU's static instance is sufficient.
const pio_program_t* probeLedWs2812Program(void) { return &ws2812_program; }
