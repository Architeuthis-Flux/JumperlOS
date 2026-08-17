// SPDX-License-Identifier: MIT

#include <cstdint>  // For uint16_t
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

volatile int chipSelect = 0;
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
void __not_in_flash_func(isrFromPio)(void) {

  // delayMicroseconds(500);
  setCSex(chipSelect, 1);
  //  Serial.println("interrupt from pio  ");
  // Serial.print(chipSelect);
  // Serial.print(" \n\r");

  setCSex(chipSelect, 0);

  chipSelect = -1;

  // Clear the state machine interrupt (not PIO0_IRQ_0)
  // The PIO program uses "irq 1" and "wait 0 irq 1 rel", so we need to clear interrupt 1 for this state machine
  pio_interrupt_clear(pio, 1);  // Clear interrupt 1 (absolute)
  
  // Also clear the PIO IRQ register for good measure
  irq_flags = pio0_hw->irq;
  hw_clear_bits(&pio0_hw->irq, irq_flags);
  

  }

int changedPaths[MAX_BRIDGES];
int changedPathsCount = 0;

// Index array for chip-ordered processing while keeping main path array in net order
int chipOrderedIndex[MAX_BRIDGES];
bool chipOrderValid = false;

// Timeout counter for PIO debugging
int ch446q_timeout_count = 0;

void initCH446Q(void) {

  uint dat = 14;
  uint clk = 15;

  // uint cs = 7;

  // CRITICAL: Initialize GPIO pins for PIO use (required for RP2350B)
  pio_gpio_init(pio, dat);  // Initialize GPIO 14 for PIO0
  pio_gpio_init(pio, clk);  // Initialize GPIO 15 for PIO0

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
  }

// CRITICAL: Run from RAM to prevent XIP flash cache contention with Core 0
// When both cores execute code from flash simultaneously, they compete for XIP cache
// This causes unpredictable slowdowns. Running Core 2 from RAM eliminates this issue.
void __not_in_flash_func(sendPaths)(int clean) {
  // Performance profiling (matches PROFILE_FAST_REFRESH in Commands.cpp)
  #define PROFILE_CORE2_SENDPATHS 0
  unsigned long core2_start = micros();
  unsigned long core2_step = core2_start;

  core2busy = true;

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
  sendAllPathsCore2 = 0;
  __dmb();  // Memory barrier so Core 0 sees the update
  
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
        chipSelect = globalState.connections.paths[i].chip[chip];

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
  chipSelect = chip;

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