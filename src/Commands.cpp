#include "Commands.h"
#include "XbarLatency.h" // tap->crossbar->LEDs latency probe (T2.2 gate)
#include "CoreMailbox.h"  // core 0 -> core 1 request mailbox (REQ_SEND / REQ_BYPASS; T2.2b)
#include "AsyncPassthrough.h"
#include <hardware/sync.h>  // For __dmb() memory barrier
#include "CH446Q.h"
#include "FileParsing.h"
#include "Graphics.h"
#include "InfraPaths.h"
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
#include "WaveGen.h"
#include "externVars.h"
#include "config.h"
#include "configManager.h"

// (sendAllPathsCore2 - the 0/1/-1/3 "send all the paths to the CH446Q" flag -
// is gone: T2.2b, CoreMailbox.h - core1req::REQ_SEND / REQ_BYPASS.)

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
// T2.2c: the packed int above is now core1req::REQ_SHOW_LEDS (CoreMailbox.h) -
// exactly the typed request the FOLLOW-UP note asked for, and it landed with
// the vocabulary intact: requestLedShow(1/2/3/-1/12/...) is the one writer.
// The mode REPLACES a pending one (postMode: last write wins, as the int did),
// the clear-first / blocking flags coalesce, core 1 takes the request in
// core2stuff() and completes its generation after the show, and staged
// graphics' "3 keeps control" is a state bit here (ledGraphicsOwned()) since
// a taken request cannot stick.
#include "CoreMailbox.h"
static volatile bool s_ledGfxOwned = false;

uint32_t requestLedShow( int legacyValue ) {
  uint32_t flags = 0;
  int v = legacyValue;
  if ( v < 0 ) { flags |= core1req::LED_CLEAR; v = -v; }
  if ( v >= 10 ) { flags |= core1req::LED_BLOCKING; v -= 10; }
  uint32_t mode = 0;
  switch ( v ) {
    case 1: mode = core1req::LED_NETS; break;
    case 2: mode = core1req::LED_MENU; break;
    case 3: mode = core1req::LED_GFX; break;
    default: mode = 0; break;   // 0: cancel whatever mode is pending
  }
  // Note: a mode-2 request drops s_ledGfxOwned even when the keep-rule above
  // preserves the pending GFX bits - the two agree on the end state (the
  // graphics no longer own the strip once someone asks for a flush/render),
  // this is just where that ownership flag turns off. (T3.4 walks this code.)
  s_ledGfxOwned = ( mode == core1req::LED_GFX );
  __dmb( );
  if ( mode == 0 && flags == 0 ) {
    return core1req::postMode( core1req::REQ_SHOW_LEDS, core1req::LED_MODE_MASK | core1req::LED_CLEAR | core1req::LED_BLOCKING, 0, 0, 0 );
  }
  // A menu FLUSH (mode 2) must NOT downgrade a full render (nets/graphics)
  // that is already pending - it only coalesces its flags. Without this the
  // netlist-and-clear a menu/probe exit posts (requestLedShow(-1)) is replaced
  // by the press/hold animation's mode-2 flush that lands a microsecond later:
  // core 1 then clears the board and flushes an empty buffer, leaving the
  // breadboard (and the current ants, which draw inside showNets) blank until
  // the next full render. NETS and GFX always replace (a fresh full render
  // supersedes a stale flush; graphics take the strip). The keep-rule is
  // applied INSIDE postMode under the slot lock - core 1's own mode-2 post
  // (the menu-transition pacer in core2stuff) races core 0's NETS otherwise.
  uint32_t keep = ( mode == core1req::LED_MENU ) ? ( core1req::LED_NETS | core1req::LED_GFX ) : 0u;
  return core1req::postMode( core1req::REQ_SHOW_LEDS, core1req::LED_MODE_MASK, mode, flags, keep );
}

bool ledShowIdle( void ) {
  return core1req::idle( core1req::REQ_SHOW_LEDS ) && !s_ledGfxOwned;
}

bool ledGraphicsOwned( void ) { return s_ledGfxOwned; }

uint32_t ledShowPendingBits( void ) {
  uint32_t bits = 0;
  core1req::snapshot( core1req::REQ_SHOW_LEDS, &bits, nullptr, nullptr );
  return bits;
}

volatile int showProbeLEDs =
    0; // this signals the core 2 to show the probe LEDs

unsigned long waitCore2() {

  // delayMicroseconds(60);
  unsigned long timeout = micros();

  // "Core 2 idle": no render in progress (core2busy) and no path send pending
  // or in flight (the mailbox's done generation has caught up with the
  // request generation in every slot - CoreMailbox.h). Same 25 ms
  // timeout-and-proceed as always; nothing is force-cleared.
  // (T2.2c: this deliberately does NOT wait on the LED slot - a probe session
  // or a menu keeps a show posted almost continuously, and waiting for it here
  // put ~25 ms in front of every refreshConnections(); Kevin felt the switch
  // classifier go sluggish within a minute of the first build that did. A show
  // is async, as the old int was; a caller that needs "shown" waits on the
  // generation requestLedShow() returns, or on ledShowIdle().)
  while (core2busy || !core1req::allIdle()) {
    __dmb();  // Memory barrier to ensure we see latest values from Core 2
    
    if (micros() - timeout > 25000) {  // 25ms timeout
      // ponytail: timeout-and-proceed only. core2busy is core 2's status and
      // a pending send belongs to core 2 - clearing either here falsified the
      // state for every other waiter and cancelled path sends core 2 hadn't
      // gotten to yet.
      break;
    }
    
    // CRITICAL: Service USB during wait to prevent disconnect
    TinyUSB_Device_Task(); // mutex-guarded pump (the USB IRQ pumps too - never raw tud_task())

    // Small yield to prevent tight loop
    tight_loop_contents();
  }

  __dmb();  // Final barrier before continuing
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
  holdCore1Frames( ); // core-1 frame hold (T3.4) - core 1 stops rendering while we rebuild
  unsigned long start = millis();
  core1busy = true;
  // Converge system-owned connections (probe power, lock bridges) BEFORE
  // loading bridges into the router. Lives here - at the head of each
  // HARDWARE-APPLYING rebuild - and deliberately NOT inside
  // loadBridgesFromState(): slot preview calls that without applying to
  // hardware, and evaluation fires DAC/GPIO hardware callbacks.
  infraEvaluate();
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

  
  releaseCore1Frames( );
  core1busy = false;

  // Signal Core 2 to send paths (Core 2 handles this in loop1 -> core2stuff)
  // If any chip's bookkeeping is suspect after a PIO timeout, force a clean
  // refresh so hardware and lastChipXY reconverge.
  if (anyChipXYSuspect()) {
    clean = 1;
  }
  // Ask core 2 to send the paths (mailbox: the request coalesces with any
  // still pending, a clean is sticky, and completion is a generation - not
  // "the flag reads 0" - so a request landing mid-send is never lost).
  uint32_t sendGen = core1req::post(core1req::REQ_SEND,
                                    core1req::SEND_PATHS |
                                    (clean == 1 ? core1req::SEND_CLEAN : 0u));
  xbarLatRequest();  // latency probe: request stamped (XbarLatency.h)

  // CRITICAL: Wait for core 2 to actually process the send request
  // IMPORTANT: Must call tud_task() during wait to prevent USB disconnect!
  // Only the LEGACY wavegen path captures core 2 (its blocking per-sample
  // loop): there, don't burn the full second on every refresh - leave the
  // send pending and it goes out the moment streaming stops. On the DMA
  // path (T3.3) a stream leaves core 2 free and the send goes through like
  // any other - the crossbar no longer diverges from the netlist while a
  // waveform plays.
  extern WaveGen wavegen;
  unsigned long pathsTimeout = millis();
  while (!core1req::isDone(core1req::REQ_SEND, sendGen) &&
         (millis() - pathsTimeout < 1000) && !wavegen.isCoreLoopStreaming()) {
    delayMicroseconds(100);
    // CRITICAL: Service USB during wait to prevent disconnect
    TinyUSB_Device_Task(); // mutex-guarded pump
  }
  t[ti++] = millis(); // t[6] = after the send-request wait

  if (!core1req::isDone(core1req::REQ_SEND, sendGen)) {
    // Nothing is force-cleared (the old flag used to be zeroed here, which
    // cancelled a path send core 2 hadn't gotten to yet - waitCore2()'s own
    // comment calls this out - and it fired every time a refresh landed while
    // wavegen was streaming or a menu held the LED branch, leaving the crossbar
    // silently diverged from the netlist). The request stays posted: core 2
    // serves it on its next free pass, and waitCore2() callers already
    // tolerate a pending send (25ms timeout-and-proceed).
    Serial.println("WARNING: Core 2 has not processed the path send yet "
                   "(send still pending)");
  }

  if (ledShowOption != 0) {
    requestLedShow( ledShowOption );
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
// handleLockedConnections is GONE: the OLED / serial_1 lock bridges are
// infra functions now (routing/InfraPaths.cpp), re-added by
// infraEvaluate() at the head of every hardware-applying rebuild.


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
      TinyUSB_Device_Task(); // mutex-guarded pump
    }
  }
  
  //holdCore1Frames( );
unsigned long start2 = millis();
  clearAllNTCC();
  core1busy = true;
  // See refreshConnections: infra convergence runs at every hardware-applying
  // rebuild head, never inside loadBridgesFromState (slot preview uses it).
  infraEvaluate();

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
  //releaseCore1Frames( );

  // OPTIMIZATION: Use Core 2 bypass for parallel execution (like fastRefresh)
  // This allows Core 0 to return immediately while Core 2 sends paths asynchronously
  // Result: ~13ms saved per refresh by eliminating synchronous wait
  core1req::post(core1req::REQ_BYPASS, 1u);  // the old "3": send now, no clean, no wait
  xbarLatRequest();  // latency probe: request stamped (XbarLatency.h)
  
  // NOTE: We do NOT wait for Core 2 to finish here (unlike old synchronous approach)
  // The next refresh will check core2busy flag before starting, ensuring proper sequencing
  // This enables overlapping: Core 0 can start next operation while Core 2 sends previous paths

  // LED display can happen in parallel too
  if (ledShowOption != 0) {
    requestLedShow( ledShowOption );
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
  {
    // (no callers today; kept in step with the mailbox)
    core1req::post(core1req::REQ_SEND,
                   core1req::SEND_PATHS | (clean == 1 ? core1req::SEND_CLEAN : 0u));
    xbarLatRequest();  // latency probe: request stamped (XbarLatency.h)
    if (rp2040.cpuid() == 1) {
      uint32_t g = 0;
      uint32_t bits = core1req::take(core1req::REQ_SEND, &g);
      if (bits) {
        sendPaths((bits & core1req::SEND_CLEAN) ? 1 : 0, core1req::REQ_SEND, g);  // CH446Q completes g
      }
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

  // See refreshConnections: infra convergence at every hardware-applying
  // rebuild head.
  infraEvaluate();

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
  core1req::post(core1req::REQ_BYPASS, 1u);  // the old "3": send now, no clean, no wait
  xbarLatRequest();  // latency probe: request stamped (XbarLatency.h)
  
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

