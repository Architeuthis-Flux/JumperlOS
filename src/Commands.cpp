#include "Commands.h"
#include "AsyncPassthrough.h"
#include <hardware/sync.h>  // For __dmb() memory barrier
#include "CH446Q.h"
#include "FileParsing.h"
#include "Graphics.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "States.h"
#include "Menus.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "RotaryEncoder.h"
#include "RouteSafety.h"

#include "USBfs.h"
#include "externVars.h"
#include "config.h"
#include "configManager.h"

volatile int sendAllPathsCore2 =
    0; // this signals the core 2 to send all the paths to the CH446Q

// showLEDsCore2 — cross-core "refresh the LEDs" request, decoded in core2stuff()
// (main.cpp). The integer is a packed command:
//   base mode (after stripping the modifiers below):
//     0 = idle / nothing requested
//     1 = show the netlist (showNets + measurements + row animations + overlays)
//     2 = menu/graphics text buffer only (flush b.print output; skip showNets)
//     3 = staged graphics: keep whatever is already in the buffer, skip showNets
//   modifiers:
//     negative  -> clear the breadboard (clearLEDsExceptRails) before composing,
//                  then process abs(value)  (e.g. -1 = clear + show nets)
//     +10       -> use a blocking show (showBBBlocking) instead of async DMA
//                  (e.g. 12 = blocking menu-buffer flush; see waitForBlockingDisplay)
// (Modes 4/5/6 were never produced by any writer and have been removed.)
//
// FOLLOW-UP (deep refactor, after the encoder/flush pass is verified on hardware):
// replace this packed magic-int with a typed request, e.g.
//   struct LedRefreshRequest { enum Mode { OFF, NETS, MENU, STAGED } mode;
//                              bool clear_bb; bool blocking; };
// and funnel all writers through one Core-2 compose function with a single show()
// call site, retiring the negative/+10 encoding. See also the encoder-source and
// dirty-flag follow-up notes in RotaryEncoder.cpp / LEDs.cpp.
volatile int showLEDsCore2 = 0; // this signals the core 2 to show the LEDs
volatile int showProbeLEDs =
    0; // this signals the core 2 to show the probe LEDs

volatile int core1request = 0; // this signals core 1 wants to do something

unsigned long waitCore2() {

  // delayMicroseconds(60);
  unsigned long timeout = micros();
  core1request = 1;
  __dmb();  // Memory barrier after setting core1request
  
  while (core2busy || (sendAllPathsCore2 != 0)) {
    __dmb();  // Memory barrier to ensure we see latest values from Core 2
    
    if (micros() - timeout > 25000) {  // 25ms timeout
      // ponytail: timeout-and-proceed only. core2busy is core 2's status and
      // sendAllPathsCore2 is a pending crossbar path send - force-clearing
      // them here falsified the flag for every other waiter and cancelled
      // path sends core 2 hadn't gotten to yet.
      break;
    }
    
    // CRITICAL: Service USB during wait to prevent disconnect
    #ifdef USE_TINYUSB
    extern void tud_task(void);
    tud_task();
    #endif
    
    // Small yield to prevent tight loop
    tight_loop_contents();
  }

  __dmb();  // Final barrier before continuing
  core1request = 0;
  return micros() - timeout;
}

int lastSlot = netSlot;

void refresh(int flashOrLocal, int ledShowOption, int fillUnused, int clean) {

  if (flashOrLocal == 1) {
    //if (ledShowOption == 0){
      //refreshBlind(1, fillUnused, clean);
    //} else {
      refreshLocalConnections(ledShowOption, fillUnused, clean);
    //}
  
  } else {
    refreshConnections(ledShowOption, fillUnused, clean);
  }
}

//#define DEBUG_REFRESH 1

// Guard against overlapping refresh operations
// Note: Not static because these are exported via Commands.h for auto-save deadlock prevention
volatile bool refreshInProgress = false;
static volatile bool refreshPending = false;
static volatile uint32_t lastRefreshTime = 0;

void refreshConnections(int ledShowOption, int fillUnused, int clean) {

  // CRITICAL: This should ONLY be called from Core 0
  if (rp2040.cpuid() != 0) {
    Serial.println("ERROR: refreshConnections() called from Core 2! This should only run on Core 0.");
    return;
  }

  // OPTIMIZATION: Prevent overlapping refreshes
  // If already refreshing, mark as pending and return immediately
  // This will re-run after current refresh completes
  if (refreshInProgress) {
    refreshPending = true;
    return;
  }
  
  // NO BATCHING: Process commands immediately as they arrive
  // The hardware is fast enough to handle rapid updates
  refreshInProgress = true;
  refreshPending = false;
  lastRefreshTime = millis();

  // Timing instrumentation
  unsigned long tStart = millis();
  unsigned long t[10];
  int ti = 0;

  // CRITICAL: Wait for core 2 to finish any LED rendering before modifying shared data
  // Core 2 reads from globalState.connections.nets[] in assignNetColors()
  // while we're about to modify it in getNodesToConnect()
  waitCore2();
  t[ti++] = millis(); // t[0] = after waitCore2
  pauseCore2 = true;
  unsigned long start = millis();
  core1busy = true;
  clearAllNTCC();
  //core1busy = true;
  // return;
  
  // NEW: Load bridges from globalState instead of node files
  loadBridgesFromState();
  t[ti++] = millis(); // t[1] = after loadBridgesFromState

  getNodesToConnect();
  t[ti++] = millis(); // t[2] = after getNodesToConnect
  
  // Reconcile DisplayState custom names/colors after nets are rebuilt
  // Uses firstNode to find where nets moved to
  globalState.display.reconcileAfterRebuild();
  
  rebuildChangedNetColorsFromBridges();  // Recompute net colors from bridges after net regeneration
  t[ti++] = millis(); // t[3] = after rebuildChangedNetColorsFromBridges

  bridgesToPaths();
  t[ti++] = millis(); // t[4] = after bridgesToPaths
  
  checkChangedNetColors(-1);
  chooseShownReadings();
  t[ti++] = millis(); // t[5] = after checkChangedNetColors + chooseShownReadings
  
    
  // extern void updateAllFakeGPIOAfterConnectionChange(void);
  // updateAllFakeGPIOAfterConnectionChange();

  
  pauseCore2 = false;
  core1busy = false;

  // Signal Core 2 to send paths (Core 2 handles this in loop1 -> core2stuff)
  // If any chip's bookkeeping is suspect after a PIO timeout, force a clean
  // refresh so hardware and lastChipXY reconverge.
  if (anyChipXYSuspect()) {
    clean = 1;
  }
  if (clean == 1) {
    sendAllPathsCore2 = -1;  // -1 means clean/reset paths first
  } else {
    sendAllPathsCore2 = 1;   // 1 means send paths without cleaning
  }
  __dmb();  // Ensure Core 2 sees the signal

  // CRITICAL: Wait for core 2 to actually process the sendAllPathsCore2 signal
  // IMPORTANT: Must call tud_task() during wait to prevent USB disconnect!
  unsigned long pathsTimeout = millis();
  while (sendAllPathsCore2 != 0 && (millis() - pathsTimeout < 1000)) {
    __dmb();  // Memory barrier to see Core 2's update
    delayMicroseconds(100);
    // CRITICAL: Service USB during wait to prevent disconnect
    #ifdef USE_TINYUSB
    extern void tud_task(void);
    tud_task();
    #endif
  }
  t[ti++] = millis(); // t[6] = after sendAllPathsCore2 wait
  
  if (sendAllPathsCore2 != 0) {
    Serial.println("WARNING: Core 2 did not process sendAllPathsCore2 in time!");
    sendAllPathsCore2 = 0;
    __dmb();  // Ensure the clear is visible
  }

  if (ledShowOption != 0) {
    showLEDsCore2 = ledShowOption;
    waitCore2();  // Wait for core 2 to finish rendering (which calls assignNetColors)
  }
  t[ti++] = millis(); // t[7] = after showLEDs wait
  
  // Now that core 2 has computed netColors[], we can safely read them for terminal colors
  assignTermColor();
  t[ti++] = millis(); // t[8] = after assignTermColor
  
  // Print timing breakdown for slow refreshes
  unsigned long totalTime = t[ti-1] - tStart;
  if (totalTime > 100) {
    Serial.printf("⏱️ refresh: waitC2=%lu load=%lu nodes=%lu colors=%lu paths=%lu misc=%lu sendPaths=%lu LEDs=%lu term=%lu TOTAL=%lums\n",
                  t[0]-tStart, t[1]-t[0], t[2]-t[1], t[3]-t[2], t[4]-t[3], t[5]-t[4], t[6]-t[5], t[7]-t[6], t[8]-t[7], totalTime);
  }
  
  refreshInProgress = false;
  
  // If another refresh was requested while we were busy, do it now
  if (refreshPending) {
    refreshPending = false;
    refreshConnections(ledShowOption, fillUnused, clean);
  }

  // sendPaths();
}

// ============================================================================
// Locked Connections Management
// ============================================================================
/**
 * @brief Check and restore all locked connections from config
 * 
 * This function ensures that connections marked as "locked" in jumperlessConfig
 * are always present in the active state, even after clear/load operations.
 * 
 * Locked connections:
 * - serial_1 (UART0): RP_UART_TX(116)<->NANO_D0(70), RP_UART_RX(117)<->NANO_D1(71)
 * - serial_2 (Routable Serial): Implementation pending - needs node mapping
 * - top_oled (I2C): gpio_sda<->sda_row, gpio_scl<->scl_row
 * 
 * @return Number of locked connections added (0 if all were already present)
 */
int handleLockedConnections() {
  int connectionsAdded = 0;
  SlotManager& mgr = SlotManager::getInstance();
  JumperlessState& state = mgr.getActiveState();
  
  // OLED Lock Connection (I2C)
  // Check both jumperlessConfig.top_oled.lock_connection and globalState.config.oledLockConnection
  if ((jumperlessConfig.top_oled.lock_connection == 1 || globalState.config.oledLockConnection == 1) && oledUsingHardwiredPins == false) {
    int sda_gpio = jumperlessConfig.top_oled.gpio_sda;
    int sda_row = jumperlessConfig.top_oled.sda_row;
    int scl_gpio = jumperlessConfig.top_oled.gpio_scl;
    int scl_row = jumperlessConfig.top_oled.scl_row;
    
    // Check if SDA connection exists
    if (!globalState.hasConnection(sda_row, sda_gpio)) {
      if (state.connections.numBridges < MAX_BRIDGES) {
        state.connections.bridges[state.connections.numBridges][0] = sda_row;
        state.connections.bridges[state.connections.numBridges][1] = sda_gpio;
        state.connections.bridges[state.connections.numBridges][2] = -1;  // duplicates
        state.connections.numBridges++;
        connectionsAdded++;
        
        if (debugFP) {
          Serial.print("🔒 Restored locked OLED SDA: ");
          Serial.print(sda_row);
          Serial.print("-");
          Serial.println(sda_gpio);
        }
      }
    }
    
    // Check if SCL connection exists
    if (!globalState.hasConnection(scl_row, scl_gpio)) {
      if (state.connections.numBridges < MAX_BRIDGES) {
        state.connections.bridges[state.connections.numBridges][0] = scl_row;
        state.connections.bridges[state.connections.numBridges][1] = scl_gpio;
        state.connections.bridges[state.connections.numBridges][2] = -1;  // duplicates
        state.connections.numBridges++;
        connectionsAdded++;
        
        if (debugFP) {
          Serial.print("🔒 Restored locked OLED SCL: ");
          Serial.print(scl_row);
          Serial.print("-");
          Serial.println(scl_gpio);
        }
      }
    }
  }
  
  // Serial 1 Lock Connection (UART0)
  if (jumperlessConfig.serial_1.lock_connection == 1) {
    // TX: RP_UART_TX (116) <-> NANO_D0 (70)
    if (!globalState.hasConnection(RP_UART_TX, NANO_D0)) {
      if (state.connections.numBridges < MAX_BRIDGES) {
        state.connections.bridges[state.connections.numBridges][0] = RP_UART_TX;
        state.connections.bridges[state.connections.numBridges][1] = NANO_D0;
        state.connections.bridges[state.connections.numBridges][2] = -1;  // duplicates
        state.connections.numBridges++;
        connectionsAdded++;
        
        if (debugFP) {
          Serial.println("🔒 Restored locked Serial1 TX: 116-70");
        }
      }
    }
    
    // RX: RP_UART_RX (117) <-> NANO_D1 (71)
    if (!globalState.hasConnection(RP_UART_RX, NANO_D1)) {
      if (state.connections.numBridges < MAX_BRIDGES) {
        state.connections.bridges[state.connections.numBridges][0] = RP_UART_RX;
        state.connections.bridges[state.connections.numBridges][1] = NANO_D1;
        state.connections.bridges[state.connections.numBridges][2] = -1;  // duplicates
        state.connections.numBridges++;
        connectionsAdded++;
        
        if (debugFP) {
          Serial.println("🔒 Restored locked Serial1 RX: 117-71");
        }
      }
    }
  }
  
  // Serial 2 Lock Connection (Routable Serial)
  if (jumperlessConfig.serial_2.lock_connection == 1) {
    // TODO: Determine node mappings for serial_2
    // This is left as a placeholder since the exact node numbers aren't clear
    // from the codebase. When determined, follow the same pattern as above:
    //
    // if (!globalState.hasConnection(SERIAL2_TX_GPIO, SERIAL2_TX_ROW)) {
    //   // Add bridge
    // }
    // if (!globalState.hasConnection(SERIAL2_RX_GPIO, SERIAL2_RX_ROW)) {
    //   // Add bridge
    // }
    
    if (debugFP && connectionsAdded == 0) {
      Serial.println("⚠️  Serial2 lock enabled but node mapping not yet implemented");
    }
  }
  
  // Mark dirty if we added connections
  if (connectionsAdded > 0) {
    state.markDirty();
    
    if (debugFP) {
      Serial.print("🔒 Total locked connections restored: ");
      Serial.println(connectionsAdded);
    }
  }
  
  return connectionsAdded;
}

//#define DEBUG_REFRESH 0

// Separate guard for local refresh operations
// Note: Not static because these are exported via Commands.h for auto-save deadlock prevention
volatile bool refreshLocalInProgress = false;
static volatile bool refreshLocalPending = false;
static volatile uint32_t lastRefreshLocalTime = 0;

void refreshLocalConnections(int ledShowOption, int fillUnused, int clean) {

  // CRITICAL: This should ONLY be called from Core 0
  if (rp2040.cpuid() != 0) {
    Serial.println("ERROR: refreshLocalConnections() called from Core 2! This should only run on Core 0.");
    return;
  }

  // OPTIMIZATION: Prevent overlapping refreshes
  // Queue up another refresh if one is already running
  if (refreshLocalInProgress) {
    refreshLocalPending = true;
    return;
  }
  
  refreshLocalInProgress = true;
  refreshLocalPending = false;
  lastRefreshLocalTime = millis();

  // OPTIMIZATION: Wait for Core 2 to finish previous operation before starting new one
  // This is necessary to prevent race conditions in the path arrays
  // But it allows overlap: Core 0 can route while Core 2 sends previous paths
  // With the optimizations, Core 2 only runs when there's work, so this wait should be minimal
  unsigned long core2_wait_start = micros();
  while (core2busy) {
    __dmb();  // Memory barrier
    tight_loop_contents();
    
    // Timeout safety: If Core 2 is taking too long, force proceed
    // This can happen if Core 2 is stuck waiting for mutex or other resources
    // 200ms timeout allows complex CH446Q operations to complete
    // (was 20ms which caused race conditions during rapid connect/disconnect)
    if (micros() - core2_wait_start > 200000) {  // 200ms timeout
      Serial.println("WARNING: Core 2 timeout (200ms)! Forcing proceed.");
      // ponytail: timeout-and-proceed only; core2busy is core 2's to clear
      break;
    }
    // Service USB periodically during the wait to prevent port disconnect
    if ((micros() - core2_wait_start) % 5000 < 10) {
      tud_task();
    }
  }
  
  //pauseCore2 = true;
unsigned long start2 = millis();
  clearAllNTCC();
  core1busy = true;
  
  // NEW: Load bridges from globalState instead of local files
  loadBridgesFromState();

  getNodesToConnect();
#if DEBUG_REFRESH
  Serial.print("getNodesToConnect = ");
  Serial.println(millis() - start2);
#endif
  
  // Reconcile DisplayState custom names/colors after nets are rebuilt
  globalState.display.reconcileAfterRebuild();
  
  rebuildChangedNetColorsFromBridges();  // Recompute net colors from bridges after net regeneration
#if DEBUG_REFRESH
  Serial.print("rebuildChangedNetColorsFromBridges = ");
  Serial.println(millis() - start2);
#endif
  bridgesToPaths();
#if DEBUG_REFRESH
  Serial.print("bridgesToPaths = ");
  Serial.println(millis() - start2);
#endif
  checkChangedNetColors(-1);
#if DEBUG_REFRESH
  Serial.print("checkChangedNetColors = ");
  Serial.println(millis() - start2);
#endif
  chooseShownReadings();
#if DEBUG_REFRESH
  Serial.print("chooseShownReadings = ");
  Serial.println(millis() - start2);
#endif
  // Restore GPIO configurations from jumperlessConfig after net processing
  setGPIO();
#if DEBUG_REFRESH
  Serial.print("refreshLocalConnections time = ");
  Serial.println(millis() - start2);
#endif

// extern void updateAllFakeGPIOAfterConnectionChange(void);
// updateAllFakeGPIOAfterConnectionChange();


  core1busy = false;
  //pauseCore2 = false;

  // OPTIMIZATION: Use Core 2 bypass for parallel execution (like fastRefresh)
  // This allows Core 0 to return immediately while Core 2 sends paths asynchronously
  // Result: ~13ms saved per refresh by eliminating synchronous wait
  sendAllPathsCore2 = 3;  // 3 = bypass flag for immediate parallel execution
  __dmb();  // Memory barrier so Core 2 sees the update
  
  // NOTE: We do NOT wait for Core 2 to finish here (unlike old synchronous approach)
  // The next refresh will check core2busy flag before starting, ensuring proper sequencing
  // This enables overlapping: Core 0 can start next operation while Core 2 sends previous paths

  // LED display can happen in parallel too
  if (ledShowOption != 0) {
    showLEDsCore2 = ledShowOption;
    // Don't wait - let LEDs update asynchronously
  }
  
  // OPTIMIZATION: Only compute terminal colors if actually needed
  // Terminal colors are only used for debug/display output
  #ifdef TERM_COLOR_NETS
  // Only compute if in debug mode or if terminal colors are actively being used
  // This saves ~1ms when not needed
  assignTermColor();
  #endif
#if DEBUG_REFRESH
  Serial.print("refreshLocalConnections assignTermColor = ");
  Serial.println(millis() - start2);
#endif
#if DEBUG_REFRESH
  Serial.print("refreshLocalConnections after waitCore2 time = ");
  Serial.println(millis() - start2);
#endif

  refreshLocalInProgress = false;
  // Serial.print("refreshLocalConnections time = ");
  // Serial.print(millis() - lastRefreshLocalTime);
  // Serial.println(" ms");
  
  // If another refresh was requested while we were busy, do it now
  // This is critical for MicroPython scripts that make multiple connections quickly
  if (refreshLocalPending) {
    refreshLocalPending = false;
   // refreshLocalConnections(ledShowOption, fillUnused, clean);
  }


  //!
  // Serial.print("Free heap = ");
  // Serial.println(rp2040.getFreeHeap());


  // sendPaths();
  
  //waitCore2();
}

void refreshBlind(
    int disconnectFirst,
    int fillUnused,
    int clean) { // this doesn't actually touch the flash so we don't
  // need to wait for core 2
  waitCore2();
  //core1busy = true;
  // fillUnused = 0;
  clearAllNTCC();
  //openNodeFile(netSlot, 1);
  //core1busy = true;
  getNodesToConnect();
  rebuildChangedNetColorsFromBridges();  // Recompute net colors from bridges after net regeneration
  bridgesToPaths();
  checkChangedNetColors(-1);
  assignNetColors();
  assignTermColor();
  // printPathsCompact();
  //core1busy = false;
  //   if (lastSlot != netSlot) {
  //   createLocalNodeFile(netSlot);
  //   lastSlot = netSlot;
  // }
  // if (disconnectFirst == 1) {
  //   sendAllPathsCore2 = 1;
  // } else if (disconnectFirst == 0) {
  //   sendAllPathsCore2 = 1;
  // } else {
  //   sendAllPathsCore2 = 1; // disconnectFirst;
  // }
  if (clean == 1) {
    sendAllPathsCore2 = -1;
    if (rp2040.cpuid() == 1) {
      sendPaths(sendAllPathsCore2);
     // sendAllPathsCore2 = 0;
    } 
    } else {
      sendAllPathsCore2 = 1;
      if (rp2040.cpuid() == 1) {
        sendPaths(sendAllPathsCore2);
        //sendAllPathsCore2 = 0;
      }
    }


  chooseShownReadings();
  
  // Restore GPIO configurations from jumperlessConfig after net processing
  setGPIO();
  
  // sendPaths();
  //core1busy = false;
  waitCore2();
}

void fastRefresh(int ledShowOption) {
  // OPTIMIZATION: Fast refresh with minimal overhead
  // Skips unnecessary validation and uses Core 2 bypass for immediate updates
  
  // CRITICAL: This should ONLY be called from Core 0
  if (rp2040.cpuid() != 0) {
    Serial.println("ERROR: fastRefresh() called from Core 2! This should only run on Core 0.");
    return;
  }

  // Prevent overlapping refreshes
  if (refreshLocalInProgress) {
    refreshLocalPending = true;
    return;
  }
  
  refreshLocalInProgress = true;
  refreshLocalPending = false;
  
  // Performance profiling (set PROFILE_FAST_REFRESH = 1 to enable)
  #define PROFILE_FAST_REFRESH 0
  unsigned long startTime = micros();
  unsigned long stepTime = startTime;
  
  // PARALLELISM: Wait for Core 2 to finish previous operation before starting new one
  // This is necessary to prevent race conditions in the path arrays
  // But it allows overlap: Core 0 can route while Core 2 sends previous paths
  unsigned long core2_wait_start = micros();
  while (core2busy) {
    __dmb();  // Memory barrier
    tight_loop_contents();
    
    // Timeout safety (should never happen in normal operation)
    if (micros() - core2_wait_start > 10000) {  // 10ms timeout
      Serial.println("WARNING: Timed out waiting for Core 2!");
      break;
    }
  }
  #if PROFILE_FAST_REFRESH
  if (micros() - core2_wait_start > 10) {  // Only print if we actually waited
    Serial.print("wait for Core 2: "); Serial.print(micros() - core2_wait_start); Serial.println(" us");
    stepTime = micros();
  }
  #endif
  
  core1busy = true;
  
  // FAST PATH: Streamlined full refresh (incremental approach was fundamentally broken)
  // The key optimizations:
  // 1. Skip fillUnused (don't route duplicate paths)
  // 2. Bypass Core 2 scheduler for immediate hardware update
  // 3. Minimal validation and error checking
  
  clearAllNTCC();                         // Clear routing state
  #if PROFILE_FAST_REFRESH
  Serial.print("clearAllNTCC: "); Serial.print(micros() - stepTime); Serial.println(" us");
  stepTime = micros();
  #endif
  
  loadBridgesFromState();                 // Load all bridges from state
  #if PROFILE_FAST_REFRESH
  Serial.print("loadBridgesFromState: "); Serial.print(micros() - stepTime); Serial.println(" us");
  stepTime = micros();
  #endif
  
  getNodesToConnect();                    // Process all bridges into nets
  #if PROFILE_FAST_REFRESH
  Serial.print("getNodesToConnect: "); Serial.print(micros() - stepTime); Serial.println(" us");
  stepTime = micros();
  #endif
  
  // OPTIMIZATION: Skip display reconciliation - not needed for simple connect/disconnect
  // globalState.display.reconcileAfterRebuild();  
  
  // OPTIMIZATION: Skip color rebuilding in fast refresh (saves ~90us)
  // Colors are only for display/debugging, not required for functionality
  // Full refresh (from file/Wokwi) will rebuild colors properly
  // rebuildChangedNetColorsFromBridges();
  // #if PROFILE_FAST_REFRESH
  // Serial.print("rebuildChangedNetColorsFromBridges: "); Serial.print(micros() - stepTime); Serial.println(" us");
  // stepTime = micros();
  // #endif
  
  // CORE OPTIMIZATION: Full routing but without fillUnused (skip duplicate paths)
  // Note: bridgesToPaths() includes sortPathsByNet() which rebuilds paths from nets
  // We MUST use startIndex=0 because sortPathsByNet() rebuilds the entire array
  bridgesToPaths(0, 0, 0);  // fillUnused=0, allowStacking=0, startIndex=0
  #if PROFILE_FAST_REFRESH
  Serial.print("bridgesToPaths: "); Serial.print(micros() - stepTime); Serial.println(" us");
  stepTime = micros();
  #endif
  
  // OPTIMIZATION: Skip color checking in fast refresh (saves ~30us)
  // Colors are only for display/debugging, not required for functionality
  // checkChangedNetColors(-1);
  // #if PROFILE_FAST_REFRESH
  // Serial.print("checkChangedNetColors: "); Serial.print(micros() - stepTime); Serial.println(" us");
  // stepTime = micros();
  // #endif
  
  // OPTIMIZATION: Skip reading updates in fast refresh (saves ~40us)
  // Current sense readings are not critical for basic routing
  // chooseShownReadings();
  // #if PROFILE_FAST_REFRESH
  // Serial.print("chooseShownReadings: "); Serial.print(micros() - stepTime); Serial.println(" us");
  // stepTime = micros();
  // #endif
  
  setGPIO();                              // Configure hardware
  #if PROFILE_FAST_REFRESH
  Serial.print("setGPIO: "); Serial.print(micros() - stepTime); Serial.println(" us");
  stepTime = micros();
  #endif
  
  core1busy = false;
  
  // CRITICAL OPTIMIZATION: Bypass Core 2 scheduler for immediate path sending
  // We set the flag and return immediately - Core 2 will process asynchronously
  // This enables parallelism: Core 0 can start next operation while Core 2 sends paths
  sendAllPathsCore2 = 3;  // Special value for immediate bypass
  __dmb();                // Memory barrier so Core 2 sees the update
  
  // OPTIMIZATION: Skip terminal color assignment in fast refresh (saves ~380us)
  // This is only for display/debugging, not required for functionality
  // assignTermColor();
  // #if PROFILE_FAST_REFRESH
  // Serial.print("assignTermColor: "); Serial.print(micros() - stepTime); Serial.println(" us");
  // #endif
  
  refreshLocalInProgress = false;
  
  // PARALLELISM: We do NOT wait for Core 2 to finish!
  // Core 2 will process sendAllPathsCore2 asynchronously
  // The next operation will check core2busy before starting
  // This allows overlapping: Core 0 routes next operation while Core 2 sends previous paths
  
  #if PROFILE_FAST_REFRESH
  unsigned long elapsed = micros() - startTime;
  Serial.print("fastRefresh TOTAL: ");
  Serial.print(elapsed);
  Serial.println(" us");
  Serial.println();
  #endif
  
  // Handle pending refresh if one was requested
  if (refreshLocalPending) {
    refreshLocalPending = false;
  }
}

struct rowLEDs getRowLEDdata(int row) {

  struct rowLEDs rowLEDs = {0, 0, 0, 0, 0};
  // uint8_t *pixelPointer = leds.getPixels();
  if (row < 1) {
    return rowLEDs;
  } else if (row >= 70 && row < 125) {
    // row = row - 1;
    for (int i = 0; i < 35; i++) { // stored in GRB order
      if (bbPixelToNodesMapV5[i][0] == row) {
        rowLEDs.color[0] = leds.getPixelColor(bbPixelToNodesMapV5[i][1]);
        return rowLEDs;
      }
    }

    // Serial.print(row);
    // Serial.print(" ");
    // Serial.println(rowLEDs.color[0]);

    return rowLEDs;
  }
  row = row - 1;
  for (int i = 0; i < 5; i++) { // stored in GRB order
  rowLEDs.color[i] = 0x000000;
    rowLEDs.color[i] = leds.getPixelColor(row * 5 + i);
    // rowLEDs.color[i] = packRgb(pixelPointer[15 * row + (3 * i) + 1],
    //                            pixelPointer[15 * row + (3 * i)],
    //                            pixelPointer[15 * row + (3 * i) + 2]);
    // Serial.print(row * 5 + i);
    // Serial.print(" ");
    // Serial.println(leds.getPixelColor(row * 5 + i));
  }

  return rowLEDs;
}

void setRowLEDdata(int row, struct rowLEDs rowLEDcolors) {

  // uint8_t *pixelPointer = leds.getPixels();
  if (row < 1 || row > 125) {
    return;
  } else if (row >= 70 && row < 125) {
    // row = row - 1;
    rgbColor colorrgb = unpackRgb(rowLEDcolors.color[0]);
    for (int i = 0; i < 35; i++) { // stored in GRB order
      if (bbPixelToNodesMapV5[i][0] == row) {
        leds.setPixelColor(bbPixelToNodesMapV5[i][1], colorrgb.r, colorrgb.g,
                           colorrgb.b);
        return;
      }
    }
    return;
  }
  row = row - 1;
  for (int i = 0; i < 5; i++) { // stored in GRB order

    leds.setPixelColor(row * 5 + i, rowLEDcolors.color[i]);
    // rgbColor colorrgb = unpackRgb(rowLEDcolors.color[i]);
    // pixelPointer[15 * row + (3 * i) + 1] = colorrgb.r;
    // pixelPointer[15 * row + (3 * i)] = colorrgb.g;
    // pixelPointer[15 * row + (3 * i) + 2] = colorrgb.b;
  }
  return;
}

void connectNodes(int node1, int node2) {

  if (node1 == node2 || node1 < 1 || node2 < 1) {
    return;
  }
  if ((node1 > 60 && node1 < 70) || (node2 > 60 && node2 < 70)) {
    return;
  }

  addBridgeToState(node1, node2);
  saveStateToSlot();  // Save immediately

  refreshConnections();
  waitCore2();
  // createLocalNodeFile(netSlot);
}

void disconnectNodes(int node1, int node2) {
  removeBridgeFromState(node1, node2);
  saveStateToSlot();  // Save immediately
  refreshConnections();
  waitCore2();
}

float measureVoltage(int adcNumber, int node, bool checkForFloating) {
  int adcDefine = 0;

  switch (adcNumber) {
  case 0:
    adcDefine = ADC0;
    break;
  case 1:
    adcDefine = ADC1;
    break;
  case 2:
    adcDefine = ADC2;
    break;
  case 3:
    adcDefine = ADC3;
    break;
  case 4:
    adcDefine = ADC4;
    break;
  case 5:
    // adcDefine = ADC5;
    break;
  case 6:
    // adcDefine = ADC6;
    break;
  case 7:
    adcDefine = ADC7;
    break;
  default:
    return 0.0;
  }

  // Temporary measurement connections - no need to save
  // Remove any existing ADC connections, then add the new one
  removeBridgeFromState(adcDefine, -1);  // Remove all connections to this ADC
  addBridgeToState(node, adcDefine);
  refreshLocalConnections(1 , 0, 0);
  waitCore2();
  //refreshBlind(-1);
  //         printPathsCompact();
  // printChipStatus();

  // Serial.println(readAdc(adcNumber, 32) * (5.0 / 4095));
  // core1busy = true;
  float voltage = readAdcVoltage(adcNumber, 8);

  // Serial.print("voltage = ");
  // Serial.println(voltage);

  int floating = 0;
  if (checkForFloating == true) {
    if (voltage < 0.3 && voltage > -0.3) {

      if (checkFloating(node) == true) {
        floating = 1;
      }
    }
    waitCore2();
  }
  // Clean up temporary measurement connection
  removeBridgeFromState(node, adcDefine);
  refreshLocalConnections(0, 0, 0);
  //refreshBlind();
  waitCore2();

  if (floating == 1) {
    return (float)0xFFFFFFFF;
  }

  return voltage;
}

bool checkFloating(int node) {
  // delay(2);
  // Serial.print("node = ");
  // Serial.println(node);
  int gpioNumber = RP_GPIO_1;
  int gpioPin = GPIO_1_PIN;

  switch (node) {

  case 1 ... 93: {
    for (int i = 0; i < 4; i++) {
      if (globalState.connections.chipStates[11].xStatus[i + 4] == -1) {
        gpioNumber = RP_GPIO_1 + i;
        break;
      }
    }
    break;
  }

  // case 70 ... 120: {
  //   for (int i = 0; i < 12; i++) {
  //     if (globalState.connections.chipStates[8].xMap[i] == node) {
  //       gpioNumber = RP_UART_RX;
  //       break;
  //     }
  //     if (globalState.connections.chipStates[9].xMap[i] == node) {
  //       gpioNumber = RP_UART_TX;
  //       break;
  //     }
  //   }

  //   break;
  // }
  default:
  // Serial.print("cant find node = ");
  // Serial.println(node);
    return true;
  }
  // Serial.print("gpioNumber = ");
  // Serial.println(gpioNumber);

  switch (gpioNumber) {
  case RP_GPIO_1:
    gpioPin = GPIO_1_PIN;
    break;
  case RP_GPIO_2:

    gpioPin = GPIO_2_PIN;
    break;
  case RP_GPIO_3:

    gpioPin = GPIO_3_PIN;
    break;
  case RP_GPIO_4:
    gpioPin = GPIO_4_PIN;
    break;
  case RP_GPIO_5:
    gpioPin = GPIO_5_PIN;
    break;
  case RP_GPIO_6:
    gpioPin = GPIO_6_PIN;
    break;
  case RP_GPIO_7:
    gpioPin = GPIO_7_PIN;
    break;
  case RP_GPIO_8:
    gpioPin = GPIO_8_PIN;
    break;
    
    
  case RP_UART_RX:
    gpioPin = GPIO_RX_PIN;
    break;
  case RP_UART_TX:
    gpioPin = GPIO_TX_PIN;
    break;
  }
  // Serial.print("gpioPin = ");
  // Serial.println(gpioPin);

  // Temporary GPIO connection for floating check - no need to save
  addBridgeToState(node, gpioNumber);
  refreshLocalConnections(0, 0, 0); 
  //refreshBlind(0, 0, 0);
  waitCore2();
  //delay(100);

  int floating = gpioReadWithFloating(gpioPin, 100);
  // Serial.print("floating = ");
  // Serial.println(floating);

removeBridgeFromState(node, gpioNumber);
refreshLocalConnections(0, 0, 0);
waitCore2();

  if (floating == 2) {
    return true;
  } else {
    return false;
  }

  

  // return floating;
  // delayMicroseconds(30);
  // int reading = digitalRead(gpioPin);

  
  // if (reading == HIGH) {

  //   removeBridgeFromNodeFile(node, gpioNumber, netSlot, 1);

  //   return true;
  // } else {
  //   removeBridgeFromNodeFile(node, gpioNumber, netSlot, 1);
  //   return false;
  // }
}

float measureCurrent(int node1, int node2) { return 0; }

void setRailVoltage(int topBottom, float voltage) {
  switch (topBottom) {
  case 0:
    setTopRail(voltage, 1);
    break;
  case 1:
    setBotRail(voltage, 1);
    break;
  default:
    break;
  }

  return;
}

void connectGPIO(int gpioNumber, int node) {

  switch (gpioNumber) {
  case 1:
    gpioNumber = RP_GPIO_1;
    break;
  case 2:
    gpioNumber = RP_GPIO_2;
    break;
  case 3:
    gpioNumber = RP_GPIO_3;
    break;
  case 4:
    gpioNumber = RP_GPIO_4;
    break;
  case 5:
    gpioNumber = RP_GPIO_5;
    break;
  case 6:
    gpioNumber = RP_GPIO_6;
    break;
  case 7:
    gpioNumber = RP_GPIO_7;
    break;
  case 8:
    gpioNumber = RP_GPIO_8;
    break;
  }
  addBridgeToState(gpioNumber, node);
  saveStateToSlot();  // Save immediately
  refreshConnections();
}

// void printSlots(int fileNo, Stream *stream) {
//   if (stream == nullptr) stream = &Jerial;

//   if (fileNo == -1)

//     if (Serial.available() > 0) {
//       fileNo = Serial.read();
//       // break;
//     }

//   stream->print("\n\n\r");
//   if (fileNo == -1) {
//     stream->print("\tSlot Files");
//   } else {
//     stream->print("\tSlot File ");
//     stream->print(fileNo - '0');
//   }
//   stream->print("\n\n\r");
//   stream->print(
//       "\n\ryou can paste this text reload this circuit (enter 'o' first)");
//   stream->print("\n\r(or even just a single slot)\n\n\n\r");
//   if (fileNo == -1) {
//     for (int i = 0; i < NUM_SLOTS; i++) {
//       if (getSlotLength(i, 0) > 0) {  // Only print headers and content if slot has content
//         stream->print("\n\rSlot ");
//         stream->print(i);
//         if (i == netSlot) {
//           stream->print("        <--- current slot");
//         }

//         stream->print("\n\r/slots/slot");
//         stream->print(i);
//         stream->print(".yaml\n\r");
//         stream->print("\n\rf ");
//         printNodeFile(i, 0, 0, 0, false, stream);
//         stream->print("\n\n\r");
//       }
//     }
//   } else {

//     stream->print("\n\r/slots/slot");
//     stream->print(fileNo - '0');
//     stream->print(".yaml\n\r");

//     stream->print("\n\rf ");

//     printNodeFile(fileNo - '0', 0, 0, 0, true, stream); // Print empty slots when showing specific slot
//     stream->print("\n\r");
//   }
// }

