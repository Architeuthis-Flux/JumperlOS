// SPDX-License-Identifier: MIT

/*
Kevin Santo Cappuccio
Architeuthis Flux

KevinC@ppucc.io

5/28/2024

*/

#include "FatFS.h"
#include "FatFS_LazyPersist.h"
#include "Jerial.h"

#include "pico.h"
#define PICO_RP2350A 0
// #include <pico/stdlib.h>

#include <Arduino.h>

#ifdef USE_TINYUSB
#include "tusb.h" // TinyUSB (the pump is TinyUSB_Device_Task()/yield(), never raw tud_task())
#include <Adafruit_TinyUSB.h>
#endif

#include "ArduinoStuff.h"
#include "CH446Q.h"
#include "Commands.h"
#include <EEPROM.h>
#include <JeoPixel.h>
#include <SPI.h>
#include <Wire.h>

#include "Apps.h"
#include "ArduinoStuff.h"
#include "AsyncPassthrough.h"
#include "boards/board.h"  // board::currentBoard().caps - runtime hardware gating
#include "CommandBuffer.h" // New simplified command buffer system
#include "Debugs.h"
#include "FakeGpio.h"
#include "NetVoltageScan.h"
#include "FileParsing.h"
#include "FilesystemStuff.h"
#include "GraphicOverlays.h"
#include "Graphics.h"
#include "HelpDocs.h"
#include "DisplayService.h"
#include "Highlighting.h"
#include "MpBackground.h"
#include "PartLabels.h"
#include "StepViewer.h"
#include "JumperlOS.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "MenuTransitions.h"
#include "Menus.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "USBAudio.h"
#include "CrashLog.h"
#include "IrqSlots.h"
#include "FlashPark.h"
#include "PersistentStuff.h"
#include "Probing.h"
#include "routing/InfraPaths.h" // infraProbePowerSource (boot feed verification)
#include "SelfTest.h"
#include "Python_Proper.h"
#include "RotaryEncoder.h"
#include "States.h" // New state management system
// TermControl is now part of Jerial.h

#include "USBfs.h"
#include "configManager.h"
#include "externVars.h"
#include "oled.h"
#include <hardware/adc.h>
#include <hardware/structs/sio.h> // stale doorbell guard below

#include "MeasureMode.h"
#include "MpRemoteService.h"    // mpremote/ViperIDE raw REPL service
#include "PsramArena.h"         // App-side PSRAM allocator (Phase 1)
#include "FileCache.h"          // Write-back PSRAM file cache (Phase 2)
#include "Undo.h"               // Delta-based undo log (Phase 4.1)
#include "SingleCharCommands.h" // Single-character command system
#include "KickGap.h"            // watchdog measure-only stage: would-be kick stamps (T1.6)
#include "XbarLatency.h"        // tap->crossbar->LEDs latency probe (T2.2 gate)
#include "CoreMailbox.h"        // core 0 -> core 1 request mailbox (T2.2b)
#include "WaveGen.h"            // New async wavegen
#include "AdcRing.h"            // the always-on ADC ring (T2.1)
#include "externVars.h"

bread b;

// Debug flags
bool debugWaitLoopTiming = false;
bool debugUSB = false; // USB mass storage debug output



// Global async waveform generator
WaveGen wavegen;

// Jerial global instance is defined in Jerial.cpp

int supplySwitchPosition = 0;

// void lastNetConfirm(int forceLastNet = 0);
void rotaryEncoderStuff( void );
void initRotaryEncoder( void );
void printDirectoryContents( const char* dirname, int level );

void core2stuff( void );

// Core-1 LED-frame aborts caused by the core-1 frame hold (X prints it). Was ~20 Hz
// from the current-sense poll's pause toggle until 2026-08-16.
volatile uint32_t ledFrameAbortsPause = 0;

volatile int loadingFile = 0;

unsigned long lastNetConfirmTimer = 0;
// int machineMode = 0;

// https://wokwi.com/projects/367384677537829889

volatile bool core2initFinished = false;

volatile bool configLoaded = false;

volatile int startupAnimationFinished = 0;

unsigned long startupTimers[ 16 ];

// Deferred startup complete: record when we're ready, fire after delay
unsigned long startupCompleteRequestTime = 0;
bool startupCompletePending = false;
#define STARTUP_COMPLETE_DELAY_MS 4000

volatile int dumpLED = 0;
unsigned long dumpLEDTimer = 0;
unsigned long dumpLEDrate = 250;
// LED-dump mode (T1.10): core 1 only raises this after a frame is shown; the
// terminal dump itself - USB CDC writes - runs on core 0 (PortHousekeeping).
volatile uint32_t ledFramesShown = 0;      // T2.2c diag: leds.show() from the LED branch
volatile uint32_t ledIdleFramesShown = 0;  // ... of which swirl-only (no request) renders
// T2.2c diag: the last 16 LED requests core 1 took - what was taken, what it
// did (X prints them). bits: taken bits; rails; menu = inClickMenu|inPadMenu<<1;
// shown = leds.show() ran; t = ms.
volatile LedTakeLog ledTakeLog[ 32 ];
volatile uint8_t ledTakeLogIdx = 0;
volatile bool ledDumpFrameReady = false;

#include "FirmwareVersion.generated.h"

const char firmwareVersion[] = FIRMWARE_VERSION;

bool newConfigOptions = true; //! set to true with new config options //!

// ── Core stack layout ──
// By default arduino-pico puts Core 0's stack in SCRATCH_Y (0x20081000-
// 0x20082000, 4KB physical) and Core 1's stack in SCRATCH_X directly below
// it, topping out at exactly 0x20081000. Deep Core 0 call chains (the
// click-menu → doMenuAction → runApp → File Manager path with all its
// String/heap locals) overflow SCRATCH_Y straight into Core 1's stack
// frames. Caught live via SWD: Core 0 malloc-lock frames written below
// 0x20081000, Core 1 popping a Core-0 stack remnant (0x00000010) into PC →
// INVSTATE hard fault. This was also the true mechanism behind the earlier
// "Core 1 executed XIP garbage during a flash program" crashes (handoff doc
// open issue 3) - FM flash ops happen at maximum stack depth, so the smash
// correlated with flash writes and masqueraded as an XIP/idleOtherCore hole.
//
// Setting this arduino-pico weak flag moves Core 1's stack to an 8KB heap
// block, which (a) takes Core 1 out of the blast zone entirely and (b) lets
// Core 0 grow through both scratch banks (8KB) before reaching anything
// that matters.
bool core1_separate_stack = true;

#ifdef PICO_RP2350
// ── Stale doorbell guard ──
// arduino-pico parks the other core for every flash erase/program by ringing
// an SIO doorbell (rp2040.idleOtherCore()); core 1's doorbell IRQ handler then
// spins with interrupts off until resumeOtherCore() clears __otherCoreIdled.
// RP2350's SIO doorbell bits are NOT cleared by a SYSRESETREQ-style reset
// (debugger reset, and any reset that isn't a full power cycle), so a reset
// that lands while a doorbell is rung leaves that bit set. On the next boot
// core 1 enables its doorbell IRQ inside main1(), sees the stale bell, parks
// itself for a resume that never comes, and core 0 hangs in setup() waiting
// for core2initFinished. Caught live over SWD (2026-08-15): DOORBELL_IN=0x80
// on core 1 immediately after `reset halt`, both cores wedged at boot, no
// firmware bug involved. This constructor runs before main() launches core 1,
// so it is the one place that can clear both cores' bells before anyone
// listens for them. DOORBELL_OUT_CLR clears the bells this core posted to the
// other core; DOORBELL_IN_CLR clears our own.
namespace {
struct ClearStaleDoorbells {
    ClearStaleDoorbells( ) {
        sio_hw->doorbell_out_clr = 0xFFu;
        sio_hw->doorbell_in_clr = 0xFFu;
    }
} s_clearStaleDoorbells;
} // namespace

// Cortex-M33 hardware stack-limit guard: a future overflow becomes an
// immediate, debuggable STKOF UsageFault at the faulting push instead of
// silent cross-core memory corruption. Limits sit a redzone above the
// absolute floor of each core's stack region.
static inline void armStackLimit( uint32_t floorAddr ) {
    // CCR.STKOFHFNMIGN: ignore the stack-limit check for the exception entry
    // stacking of HardFault/NMI. Without it an MSPLIM violation is unrecoverable
    // rather than debuggable: the M33 pins SP at the limit and raises STKOF,
    // USGFAULTENA is not set so it escalates to HardFault, and that entry's own
    // stacking is checked against the same MSPLIM - a derived fault at priority
    // -1, which is architectural LOCKUP. No record, no reboot, nothing to read.
    // With this bit the handler is entered and CrashLog captures it (CFSR bit
    // 20, UFSR.STKOF). CCR is banked per core, so this must run on both.
    // CCR is at 0xE000ED14 (M33_CCR_OFFSET); STKOFHFNMIGN is bit 10.
    *(volatile uint32_t*)0xE000ED14u |= (1u << 10);
    __asm volatile( "dsb" ::: "memory" );
    __asm volatile( "msr MSPLIM, %0" ::"r"( floorAddr ) : );
    __asm volatile( "isb" ::: "memory" );
}
#endif

void setup( ) {
#ifdef PICO_RP2350
    // Core 0 stack: SCRATCH_Y top (0x20082000) growing down; with Core 1 on
    // its separate heap stack, both scratch banks belong to Core 0. Floor =
    // SCRATCH_X base (0x20080000) + 64-byte redzone.
    extern uint32_t __scratch_x_start__;
    armStackLimit( (uint32_t)&__scratch_x_start__ + 160 );  // > extended FP exception frame
#endif
    heapMark("setup() entry");
    flashParkRegisterCore( ); // our side of the flash-write park (see FlashPark.h)
    // Consume any pending crash record NOW, while we still know it belongs to
    // this firmware: the scratch registers survive a reflash, and a record that
    // never reached a terminal would otherwise be reported against a binary
    // that no longer exists. Printed later, once a terminal shows up.
    crashlogLatchAtBoot( );
    pinMode( RESETPIN, OUTPUT_12MA );

    digitalWrite( RESETPIN, HIGH );


    // Keep wear leveling on (default useFTL=true). The earlier
    // setUseFTL(false) experiment was only here to measure the
    // FatFS-without-FTL baseline (~127 ms per save) - production
    // mode uses the FTL plus lazy-persist mode below to get
    // <50 ms per save while preserving wear leveling.

    // CRITICAL: Hold Arduino in reset during JumperlOS boot
    // This prevents the Arduino from sending commands before the system is ready
    // The reset will be released in AsyncPassthrough::signalStartupComplete()

    // Note: there is NO per-boot FS-erase path here. To wipe the FatFS
    // partition you flash [env:jumperless_v5_erase] instead, which uses
    // scripts/erase_fs_partition.py to picotool-erase the FS region at
    // upload time (one-shot, single flash). On the next boot, FatFS sees
    // a blank partition and the existing _autoFormat=true path below
    // creates a fresh empty volume.

    if ( !FatFS.begin( ) ) {
        Serial.println( "Failed to initialize FatFS" );
    } else {
        // Serial.println( "FatFS initialized successfully" );
        // SPIFTL now runs in delta-journal mode (enabled at construction via
        // FATFS_SPIFTL_JOURNAL). persist() on every disk_ioctl(CTRL_SYNC) /
        // f_close appends ONE already-erased flash page with just the changed
        // L2P/peCount entries (~sub-millisecond) instead of the old ~750 ms
        // full-snapshot rewrite. So every save is now both fast AND immediately
        // power-loss durable - we no longer need lazy deferral. Keep lazy OFF.
        // FileCache's fatFsForceSync() calls become cheap no-ops (the CTRL_SYNC
        // append already persisted the metadata). See FatFS_LazyPersist.h.
        fatFsSetJournal( true );
        fatFsSetLazyPersist( false );
        
        // Serial.printf( "SPIFTL delta-journal: %s\n",
                    //    fatFsIsJournal() ? "ON (fast durable saves)" : "OFF (full-snapshot persist)" );
    }

    heapMark("FatFS.begin");

    // Initialize multicore synchronization primitives BEFORE Core 2 starts
    // This provides proper mutex-based protection for shared resources
    core_sync_init( );
    heapMark("boot: before Serial");
    Serial.begin( 115200 );

    // Configure Jerial for both input and output
    // Jerial automatically enables terminal control (line editing, history) for USB Serial endpoints
    // RelayBufferStream is automatically prioritized via MultiSourceStream layer
    Jerial.setInputStream( JerialEndpoint::USB_SERIAL );  // Input with terminal control
    Jerial.addOutputStream( JerialEndpoint::USB_SERIAL ); // Output to USB

    startupTimers[ 0 ] = millis( );

    // Load hardware revision from EEPROM first (survives config resets)
    // This reads directly from EEPROM and initializes it if needed
    loadHardwareFromEEPROM( );

    loadConfig( );
    heapMark("loadConfig");
    //delay(2000);

    // Reconcile the durable EEPROM store with the just-loaded config. EEPROM
    // wins for the kept identity+calibration fields (so they survive an FS
    // wipe); if no valid store exists yet it's seeded from config here and
    // committed later via the deferred flush path.
    eepromReconcileAfterConfig( );

    // Auto-detect PSRAM hardware and fix config if it disagrees
    size_t detectedPsram = 0;
    {
        detectedPsram = rp2040.getPSRAMSize( );
        int shouldBeInstalled = ( detectedPsram > 0 ) ? 1 : 0;
        if ( jumperlessConfig.hardware.psram_installed != shouldBeInstalled ) {
            Serial.printf( "[PSRAM] Auto-detected %s — updating psram_installed %d -> %d\n",
                detectedPsram > 0 ? "8MB PSRAM" : "no PSRAM",
                jumperlessConfig.hardware.psram_installed, shouldBeInstalled );
            jumperlessConfig.hardware.psram_installed = shouldBeInstalled;
            applyPsramModeChange( shouldBeInstalled );
            saveConfig( );
        }
    }

    // Initialize the app-side PSRAM arena BEFORE MicroPython starts so its
    // gc_add() call only sees the unused tail. Safe to call with no PSRAM.
    if ( jumperlessConfig.hardware.psram_installed && detectedPsram > 0 ) {
        bool ok = psram_arena_init( detectedPsram, jumperlessConfig.hardware.psram_app_size_kb );
        if ( ok ) {
            Serial.printf( "[PSRAM] App arena ready: %u KB free, MP region %u KB\n",
                (unsigned)( psram_app_free( ) / 1024 ),
                (unsigned)( psram_mp_size( ) / 1024 ) );
        } else {
            Serial.println( "[PSRAM] App arena init failed - continuing without app cache" );
        }
    }

    // Initialize the file cache (relies on the arena - no-op if unavailable).
    fileCacheInit( );
    heapMark("fileCacheInit");

    // Initialize the undo log. Must come before any state mutation hooks fire,
    // so nets/probing routines see a valid log from the first edit.
    undoInit( );
    heapMark("undoInit");

    // Check for firmware updates and provision new files if needed
    checkAndHandleFirmwareUpdate( );

    // Initialize MicroPython examples at boot so they're ready for USBSer2 REPL access
    initializeMicroPythonExamples( );
    heapMark("examples provisioning");

    // Same, for the built-in /projects/<dir>/ trees the Guides launcher lists
    initializeProjects( );
    heapMark("projects provisioning");

    configLoaded = 1;
    startupTimers[ 1 ] = millis( );
    delayMicroseconds( 200 );

    initNets( );
    heapMark("initNets");
    backpowered = 0;

    // delay(1000);

    if ( jumperlessConfig.serial_1.function >= 5 &&
         jumperlessConfig.serial_1.function <= 6 ) {
        dumpLED = 1;
    }
    if ( jumperlessConfig.serial_2.function >= 5 &&
         jumperlessConfig.serial_2.function <= 6 ) {
        dumpLED = 1;
    }

    // A UART in OLED mode gets the mirror via Jerial's endpoint fan-out, so
    // just turn the mirror ON (port_1 = the Jerial path). The old 2/3 values
    // predate show_in_terminal being a real port selector.
    if ( jumperlessConfig.serial_1.function == 4 ||
         jumperlessConfig.serial_1.function == 6 ||
         jumperlessConfig.serial_2.function == 4 ||
         jumperlessConfig.serial_2.function == 6 ) {
        jumperlessConfig.top_oled.show_in_terminal = 1;
    }

    // Jerial.addOutputStream(JerialEndpoint::USB_SER2);  // Output to Serial1
    // Jerial.addOutputStream(JerialEndpoint::OLED);     // Optional: also show on OLED
    // Jerial.addOutputStream(JerialEndpoint::SERIAL1);  // Optional: UART to Arduino
    Jerial.addInputSource( Jerial.getRelayStream( ) ); // Add relay stream as high-priority input source
    Jerial.addInputSource( JerialEndpoint::USB_SER3 );     // Add Port 4 as an input source

    // Enable automatic tag stripping for input
    // This removes <j> and </j> tags from incoming USB commands to prevent weird behavior
    // Jerial.setAutoStripTags(true);
    digitalWrite( RESETPIN, LOW );
    initDAC( );
    heapMark("initDAC");
    // Serial.println("DAC initialized");
    // Serial.flush();

    #if !defined(OG_JUMPERLESS)
    pinMode( PROBE_PIN, OUTPUT_8MA );
    // Park whichever side of the shared TRRS net is NOT the WS2812 data pin.
    // When probe_led_on_button_pin is set, GPIO9 belongs to probeLEDs (claimed
    // in initLEDs() on core1, possibly concurrently with this code) - touching
    // it here would clobber its PIO funcsel.
    if ( jumperlessConfig.probe.led_on_button_pin ) {
        pinMode( PROBE_LED_PIN, INPUT_PULLDOWN );
    } else {
        pinMode( BUTTON_PIN, INPUT_PULLDOWN );
    }
    digitalWrite( PROBE_PIN, HIGH );
    #endif

    // digitalWrite(BUTTON_PIN, HIGH);
    startupTimers[ 2 ] = millis( );

    // initINA219( );

    // Serial.println("INA219 initialized");
    // Serial.flush();
    SetArduinoResetLine( LOW, 1 ); // Hold both Arduinos in reset
    delayMicroseconds( 100 );

    digitalWrite( RESETPIN, LOW );

    while ( core2initFinished == 0 ) {
        tight_loop_contents( );
        // delayMicroseconds(1);
    }
    startupTimers[ 3 ] = millis( );
    // Both cores are up and registered: from here every flash write parks the
    // other core through FlashPark instead of arduino-pico's racy doorbell
    // handshake (the "board drops off USB during a config save" bug).
    flashParkTakeover( );
    // Serial.println("Core2 initialized");
    // Serial.flush();

    // probing.routableBufferPower( 1, 0 );

    // Serial.println("Routable buffer power initialized");
    // Serial.flush();

    startupTimers[ 4 ] = millis( );
#if defined(OG_JUMPERLESS)
    // The OG runs its own short rainbow-swirl boot animation (ported from the OG
    // reference firmware) instead of the V5 image animation, which renders
    // imagery meaningless on the OG's 111-LED strip. core0 owns the strip here
    // (it waited on core2initFinished above, so initLEDs() is done), so this is
    // safe to drive directly.
    ogStartupAnimation( );
#else
    // V5 image boot animation. (drawAnimatedImage stays bounds-safe via the
    // LEDs.cpp guards regardless of board.)
    if ( board::currentBoard( ).caps.hasStartupAnimation ) {
        drawAnimatedImage( 0 );
    }
#endif
    startupAnimationFinished = 1;
    // Serial.println("Startup animation finished");
    // Serial.flush();
    clearAllNTCC( );
    initINA219( );
    heapMark("initINA219");

    // Auto-detect OLED on the internal I2C0 bus and bring the config in
    // line with what's actually wired up. Must run AFTER initDAC() /
    // initINA219() (Wire is up) and BEFORE firstLoop==2 (which decides
    // whether to call oled.init() based on top_oled.connect_on_boot).
    // Full policy + rationale lives next to the implementation in oled.cpp.
    autoDetectAndConfigureOled( );
    heapMark("OLED autodetect");

    // Serial.println("currentReadingOffset0_mA = " + String(currentReadingOffset0_mA));
    // Serial.println("currentReadingOffset1_mA = " + String(currentReadingOffset1_mA));
    // Serial.flush();
    // delay(50);

    startupTimers[ 5 ] = millis( );
    // Serial.println("NTCC initialized");
    // Serial.flush();
    delayMicroseconds( 100 );

    // Serial.println("Arduino initialized");
    // Serial.flush();
    // delay(100);
    initMenu( );
    heapMark("initMenu");
    startupTimers[ 6 ] = millis( );
    initADC( );
    heapMark("initADC");
    startupTimers[ 7 ] = millis( );
    // Serial.println("ADC initialized");
    // Serial.flush();

#if !defined(OG_JUMPERLESS)
    // OG has no resistive probe pads (V5-only); probing.getNothingTouched() reads ADC
    // channel 5 which doesn't exist on the RP2040, causing "All nothing touched
    // samples rejected". Scanning probe is Phase 2 work.
    probing.getNothingTouched( );
#endif

    startupTimers[ 8 ] = millis( );
    // createSlots( -1, 0 );
    //  Serial.println("Slots created");
    //  Serial.flush();
    //  Note: YAML slot files are now created on-demand in States.cpp when first accessed

    initializeNetColorTracking( );   // Initialize net color tracking after slots are
                                     // created
    initializeValidationTracking( ); // Initialize validation tracking
    startupTimers[ 9 ] = millis( );
    // Serial.println("Net color tracking initialized");
    // Serial.flush();

    // tuiGlue.setSerial( &USBSer3 );
    //  Defer TuiGlue activation to first loop() call to avoid DTR wait and terminal probing delays
    //  tuiGlue.openOnDemand();
    //  Serial.println("TuiGlue initialized");
    //  Serial.flush();

    // Initialize and register services with jOSmanager
    // Serial.println("Registering services with jOSmanager...");

    // oledService.setOledDisplay( &oled );

    // Register the services. The comment on each line is the priority its
    // getPriority() actually returns (the header is authoritative - these used
    // to disagree). Order within a priority is registration order. "inner set"
    // = Service::inInnerSet(): what jOS.serviceInner() keeps alive inside the
    // modal loops (probe mode, click/pad menus, apps, MicroPython delays) and
    // while a BLOCKING service holds the loop - CRITICAL by default, plus
    // AsyncPassthrough. Periods (periodUs(), 0 = every pass) live in the headers.
    //
    // NOT registered any more (each verified a no-op, see
    // CodeDocs/SCHEDULER_AND_HARDWARE_OFFLOAD.md B2): TermSerialService (body
    // commented out), RelayedCommandService (disabled - CommandBuffer in loop()
    // does the job), SingleCharCommands (commands run synchronously from loop()),
    // USBPeriodicService (usbPeriodic() is a debug print), and
    // FileCacheFlushService unless USE_FILE_CACHE is compiled in.

    jOS.registerService( &tinyUSBService );          // CRITICAL - USB pump every pass (TinyUSB_Device_Task); inner set
    jOS.registerService( &asyncPassthroughService ); // HIGH - USB CDC1<->UART0 bridging (prevent data loss); inner set (inInnerSet() override - the bridge keeps running inside probe mode / menus)
    jOS.registerService( &menus );                   // HIGH - click-wheel menu; BLOCKING while a menu is open
    jOS.registerService( &slotManager );             // HIGH - states auto-save (idle-gated)
    jOS.registerService( &stepViewer );              // HIGH - guide-step browser (wheel owns steps while armed; registered BEFORE the probe stack so it sees turns ahead of Highlighting)


    // Probe stack is gated on the board having resistive probe pads (V5). The OG
    // has no probe pads (its scanning probe is Phase 2 work), so registering
    // these would poll nonexistent ADC channels and spam measure mode. Runtime
    // cap instead of #ifdef so the contract - not a board macro - drives it.
    if ( board::currentBoard( ).caps.hasProbePads ) {
        jOS.registerService( &probeButton );      // CRITICAL - button state machine (PIO IRQ does the sampling); inner set
        jOS.registerService( &probing );          // HIGH - probe reading + probing.probeMode() entry; BLOCKING while a pad menu is open
        jOS.registerService( &highlighting );     // HIGH - encoder net highlight / voltage adjuster (BLOCKING while it owns the wheel)
        jOS.registerService( &measureModeService ); // HIGH - measure-position readings
        jOS.registerService( &probeSwitch );      // NORMAL - switch position (500 ms self-gated) + infraServiceTick()
        jOS.registerService( &probePads );        // LOW - expensive ADC pad reading (50 ms self-gated)
    }

    jOS.registerService( &mpRemoteService ); // CRITICAL - mpremote/ViperIDE raw REPL on USBSer2; inner set
    jOS.registerService( &peripherals );     // CRITICAL - current-sense poll (10 ms); inner set

    jOS.registerService( &oledGuiService );      // NORMAL - retained OLED screen render + live bindings (inert until a screen is active)
    jOS.registerService( &partLabels );          // NORMAL, 20 ms - ambient part labels (_PARTS_ overlay, auto-hide), tap-to-inspect, pin-class warnings; dormant on OG (ledsPerRow gate)
    jOS.registerService( &mpBackgroundService ); // NORMAL, 5 ms - background MicroPython callback (bg_start); NOT inner set on purpose - pauses in probe mode/menus/foreground scripts
    jOS.registerService( &displayService );      // NORMAL, 2 ms, inner set - breadboard display driving (beacon/init/animate, one bus chunk per tick; the animation surviving menus IS the point)
    jOS.registerService( &portHousekeepingService ); // NORMAL, 10 ms - Arduino DTR/flash detect + UART auto-connect, ENQ port-info reply, net-scan debug (was a 10 ms block in loop(); B6)
    jOS.registerService( &ledDumpService );          // NORMAL, 10 ms, inner set - the terminal LED picture (R! / serial function 5-6), drawn on core 0 now (was loop1 on core 1; T1.10)

    jOS.registerService( &oledService );         // LOW - OLED connection maintenance (OLED preserved on OG: a user can wire a panel to GP18/19)
    jOS.registerService( &liveCrossbarService ); // LOW - live crossbar terminal display
    jOS.registerService( &configSaveService );   // LOW - background config save (non-blocking)
#if USE_FILE_CACHE
    // Write-back file cache lives in PSRAM; only boards with PSRAM run its flush.
    if ( board::currentBoard( ).caps.hasPsram ) {
        jOS.registerService( &fileCacheFlushService ); // LOW - write-back PSRAM cache flush
    }
#endif

    // Initialize context stack with MAIN_MENU as the root context
    // This provides proper navigation tracking for all child contexts
    ContextEntry mainMenuCtx( ContextType::MAIN_MENU );
    mainMenuCtx.onEnter = nullptr; // No special setup needed
    mainMenuCtx.onExit = nullptr;  // Main menu never exits normally
    mainMenuCtx.onSuspend = nullptr;
    mainMenuCtx.onResume = []( void* ) {
        // When returning to main menu from any child context,
        // ensure any cleanup is done
        extern void closeAllFiles( );
        closeAllFiles( );
    };
    ContextManager::getInstance( ).pushContext( mainMenuCtx );
    // Clear any non-scrolling region that may persist from a previous session
    // This resets terminal state in case LED dump or crossbar display was active before reboot
    clearNonScrollingRegion( );

    // T2.1: the always-on ADC ring - from here every readAdc() on either core
    // is a memory read / a short wait on the ring, and the pad poll no longer
    // owns core 0 (AdcRing.h). Started here, on core 0, after initADC() and
    // the config (its DMA_IRQ_1 handler runs on this core). If it declines,
    // the START_ONCE path stays and X says why.
    adcRingStart( );
    heapMark("adcRingStart");
#if USB_AUDIO_ENABLE
    // Last thing in setup(), once USB and the config are both up: if the saved
    // config wants the USB mic, re-enumerate so the host actually sees it. USB
    // came up before the config was read, so the host already has the base
    // descriptor by now. No-op unless audio is enabled.
    usb_audio_boot_enumerate( );
    heapMark("usb audio enumerate");
    // Printed here and not per-stage: most of the stages above run before USB
    // enumerates, so a live print would have gone nowhere. Also reachable from
    // the memory menu, which is where you want it after the board has been
    // used for a while.
    // heapLedgerPrint();
#endif
    // Serial.println("Service registration complete");
    // Serial.flush();
}

unsigned long startupCore2timers[ 10 ];

void setupCore2stuff( ) {
    // delay(2000);
    startupCore2timers[ 0 ] = millis( );
    initCH446Q( );
    startupCore2timers[ 1 ] = millis( );
    // delay(1);

    while ( configLoaded == 0 ) {
        delayMicroseconds( 1 );
    }

    initLEDs( );

    startupCore2timers[ 2 ] = millis( );
    initRowAnimations( );
    startupCore2timers[ 3 ] = millis( );
    setupSwirlColors( );
    startupCore2timers[ 4 ] = millis( );

    startupCore2timers[ 5 ] = millis( );
    // Only initialize the quadrature encoder on boards that have one. On the OG
    // this matters beyond a no-op: initRotaryEncoder() claims a PIO state machine
    // and loads the quadrature program, and the RP2040's 2 PIO blocks are already
    // oversubscribed (probe button + WS2812 strips). Loading a dead encoder
    // program there starved the LED program and the core1 poll of the unloaded SM
    // caused the documented periodic reset. The cap keeps that PIO slot free.
    if ( board::currentBoard( ).caps.hasRotaryEncoder ) {
        initRotaryEncoder( );
    }
    startupCore2timers[ 6 ] = millis( );
    initSecondSerial( );
    core2initFinished = 1;
    // delay(4);
}

void setup1( ) {
    // flash_safe_execute_core_init();

#ifdef PICO_RP2350
    // Core 1 stack: 8KB heap block (core1_separate_stack above). Floor =
    // base of that allocation + 64-byte redzone.
    if ( core1_separate_stack_address != nullptr ) {
        armStackLimit( (uint32_t)core1_separate_stack_address + 160 );  // > extended FP exception frame
    }
#endif
    flashParkRegisterCore( ); // core 1's side of the flash-write park (see FlashPark.h)

    setupCore2stuff( );
    startupCore2timers[ 7 ] = millis( );
    while ( startupAnimationFinished == 0 ) {
        // delayMicroseconds(1);
        // if (Serial.available() > 0) {
        //   char c = Serial.read();
        //  // Serial.print(c);
        //   //Serial.flush();
        //   }
    }

    startupCore2timers[ 8 ] = millis( );
}

#define TEST_PSRAM 0

#if TEST_PSRAM == 1

int buff[ 4 * 1024 ] PSRAM; // 4MB array

void initBuff( ) {
    // bzero(buff, sizeof(buff));
    for ( int i = 0; i < 4 * 1024; i += 1 ) {
        buff[ i ] = i;
    }
}

void printBuff( ) {
    for ( int i = 0; i < 4 * 1024; i += 1 ) {
        Serial.print( buff[ i ] );
        Serial.print( " " );
        Serial.flush( );
    }
    Serial.println( );
    Serial.flush( );
}
#endif

char connectFromArduino = '\0';

int input = '\0';

int serSource = 0;
int readInNodesArduino = 0;

int firstLoop = 1;

volatile int probeActive = 0;

int showExtraMenu = 0;

int lastHighlightedNet = -1;
int lastBrightenedNet = -1;
int lastWarningNet = -1;

int dontShowMenu = 0;

unsigned long timer = 0;
int lastProbeButton = 0;
unsigned long waitTimer = 0;
unsigned long switchTimer = 0;

int attract = 0;

unsigned long switchPositionCheckTimer = 0;

unsigned long mscModeRefreshTimer = 0;
unsigned long mscModeRefreshInterval = 2000;

volatile int core1passthrough = 1;
int switchPosCount = 0;

#include <pico/stdlib.h>

#include <hardware/gpio.h>

unsigned long core1Timeout = millis( );

#define debug_startup_timers 0
#define debug_busy_timers 0

// Debug flag to enable verbose checkpoint output for crash debugging
// Set to 1 to enable checkpoint markers (H, I, W, X, Y, a-f, etc.)
// WARNING: This adds significant USB traffic which can affect stability!
#define DEBUG_MAIN_LOOP_CHECKPOINTS 0

unsigned long busyPrintTime = 0;
unsigned long busyPrintInterval = 3000;
unsigned long busyTimers[ 10 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// Jerial is now used for all serial communications (input and output)

// Global storage for current command line (for backwards compatibility with parsers)
String currentCommandLine = "";

unsigned long loopStart = millis( );

// "[command]?" help applies to every command char except whitespace and the
// commands whose handlers take '?' as their OWN sub-command: A?/a? (Arduino
// connect/disconnect status query - cmd_connectArduino / cmd_disconnectArduino),
// i? (RouteSafety self-check + suspect audit - cmd_netCurrents; the HIL suite
// parses it), M? (USB-audio status - cmd_usbAudio). Same list in both
// terminal modes (before B6, char mode showed the help page for i?/M? and
// line mode ran the handler for x?/m?/... - see the note in loop()).
static bool helpQuestionApplies( int c ) {
    switch ( c ) {
    case '\n':
    case '\r':
    case ' ':
    case '\0':
    case 'A':
    case 'a':
    case 'i':
    case 'M':
        return false;
    default:
        return true;
    }
}

void loop( ) {
    // Declare variables at function scope to avoid goto scope issues
    bool useLineBuffering = false;
    bool hasRelayedData = false;
    static const unsigned int HELP_WAIT_MS = 100;
    // Char-mode "[command]?" / "help" look-ahead (B6): instead of spinning
    // 100 ms with nothing serviced after every command char, the char is
    // parked here and the busy loop keeps running services until the next
    // char arrives or the deadline passes. Statics: the busy loop is entered
    // through a label.
    static bool helpArmed = false;
    static int helpArmedChar = '\0';
    static unsigned long helpDeadlineMs = 0;
    static bool helpDeadlineSpent = false; // the one wait per command char has happened
    bool gotCompletedLine = false;         // this pass's input came in as a whole line (line mode / relayed)

menu:

    // Serial.print("firstLoop = ");
    // Serial.println(firstLoop);
    // Serial.flush();
    if ( firstLoop == 1 ) {

        if ( firstStart == true || autoCalibrationNeeded == true ) {
            if ( autoCalibrationNeeded == true ) {
                Serial.println( "New calibration options detected in config.txt. "
                                "Running automatic calibration..." );
                delay( 1000 );
            }
            calibrateDacs( );
            // calibrateProbeSwitchThresholds( );
            // probeCalibApp();
            firstStart = false;
        }

        // Migration sentinel: probe.droop_ohms == 0 means the probe switch /
        // droop calibration has never run on this board (it predates the
        // pad/switch calibration apps). Run it once after the update - it's
        // quick and self-guided; skipping it leaves the switch classifier on
        // the empirical fallback constants.
        if ( probeCalibrationNeeded == true && firstStart == false ) {
            probeCalibrationNeeded = false;
            changeTerminalColor( 213, true );
            Serial.println( "\n\rThis firmware update added probe switch/droop "
                            "calibration that this board has never run." );
            Serial.println( "Starting Switch Calib now (also reachable later: "
                            "clickwheel menu > Calibration > Switch Thresh)." );
            changeTerminalColor( -1, true );
            delay( 1500 );
            calibrateProbeSwitchThresholds( );
        }
        firstLoop = 2;

        // Decide which context to come up in BEFORE the first loadfile: pass.
        // Config is loaded by now (setup() waits on configLoaded), and this
        // only seeds netSlot / activeSlotPath - loadfile: below does the
        // actual load, so numbered and file contexts share one load path.
        seedBootContext( );

        goto loadfile;
    }

    if ( firstLoop == 2 ) {
        // Serial.println("initializing oled");
        // Serial.flush();
        // Serial.println("millis = " + String(millis()));

#if !defined(OG_JUMPERLESS)
        // OG has no OLED; skip init (it also kicks refreshConnections + debug
        // printfs that abort on the OG's tight heap).
        if ( jumperlessConfig.top_oled.connect_on_boot == 1 ) {
            // Serial.println("Initializing OLED");
            oled.init( );
        }
#endif
        probing.checkProbeCurrentZero( );

        // Ensure the probe-sense DAC is at the calibrated measure_mode_output_voltage
        // and ROUTABLE_BUFFER_IN<->DACn is wired before the first probe tap.
        // Without this, initDAC()/applyStateToHardware() leave the probe DAC at
        // whatever the slot's saved power.dac0/1 was (often the 3.33V default),
        // and pad detection in measure mode misreads until probing.probeMode() runs.
        // (The auto_connect_probe gate lives in infra's enProbePower(), so this
        // is a no-op when probe auto-connect is disabled.)
        probing.routableBufferPower( 1, 0 );
        // Boot determinism: the nudge above refreshes immediately UNLESS a
        // rebuild happened to be in flight (then only a sticky retry flag is
        // left for the next ~500ms service tick). Verify the feed actually
        // routed and force one synchronous rebuild if not - the probe must be
        // powered and parked before the user's first tap, every boot.
        if ( jumperlessConfig.probe.auto_connect > 0 &&
             infraProbePowerSource( ) < 0 ) {
            refreshLocalConnections( 0, 1, 0 );
        }

        // If the first-start self test ran before the restart that got us
        // here, repaint its pass/fail LED overlay from the one-shot marker
        // (deleted inside, so the overlay clears on the next reset).
        selfTestShowSavedResultIfPending( );

        printColorJogoSmall( );
        // If the previous run ended in a HardFault, say so right here - once -
        // so a crash leaves a trail instead of a mystery reboot.
        crashlogReportOnce( Serial );
        // Say so if any shared-IRQ registration had to be refused - that feature
        // is running without its interrupt (silent, but not a dead core).
        jlIrqSlotsReport( Serial );
        // The boot heap ledger is RECORDED here on every boot (heapMark calls
        // through setup) but printed only from the Memory Usage screen - 18
        // lines on every boot is noise, and the recording is what matters:
        // every stage it measures runs before USB enumerates, so the numbers
        // have to be captured long before anyone can ask for them.
#if TEST_PSRAM == 1
        while ( 1 ) {
            initBuff( );
            printBuff( );
            delay( 1000 );
        }
#endif
        firstLoop = 0;

// Defer startup complete by STARTUP_COMPLETE_DELAY_MS to avoid crashing
// if an Arduino is already sending UART data at boot
#if ASYNC_PASSTHROUGH_ENABLED == 1
        startupCompleteRequestTime = millis( );
        startupCompletePending = true;
#endif

    }

    // if ( Jerial.available( ) >
    //      20 ) { // this is so if you dump a lot of data into the serial buffer, it
    //             // will consume it and not keep looping
    //     while ( Jerial.available( ) > 0 ) {
    //         char c = Jerial.read( );
    //         // Jerial.print(c);
    //         // Jerial.flush();
    //     }
    // }

    // (The lastProbePowerDAC change detector is gone: probe-power source
    // moves are handled by InfraPaths' rebuild-head evaluation + nudges.)

    // Jerial.print("clearing highlighting");
    // Jerial.flush();

    clearHighlighting( );

    // Jerial.print("clearHighlighting");
    // Jerial.flush();

    // Serial.print("termInInteractiveMode = ");
    // Serial.println(termInInteractiveMode);
    // Serial.flush();

    if ( dontShowMenu == 0 ) {
    forceprintmenu:
        // Use the new SingleCharCommands menu system
        singleCharCommands.printMenu( showExtraMenu );
        // USBSer2.print( "printMenu" );
        // USBSer2.flush( );

#if debug_startup_timers == 1
        for ( int i = 1; i < 16; i++ ) {
            Serial.print( "startupTimer[" );
            Serial.print( i - 1 );
            Serial.print( " - " );
            Serial.print( i );
            Serial.print( "] = " );
            Serial.println( startupTimers[ i ] - startupTimers[ i - 1 ] );
            Serial.flush( );
        }

        for ( int i = 1; i < 12; i++ ) {
            Serial.print( "startupCore2Timer[" );
            Serial.print( i - 1 );
            Serial.print( " - " );
            Serial.print( i );
            Serial.print( "] = " );
            Serial.println( startupCore2timers[ i ] - startupCore2timers[ i - 1 ] );
            Serial.flush( );
        }
#endif
    }

dontshowmenu:
#if DEBUG_MAIN_LOOP_CHECKPOINTS
    Serial.write( 'H' ); // Reached dontshowmenu
    yield( );
#endif

    // Config saving is now handled by ConfigSaveService which monitors configChanged flag
    // This allows saves from anywhere in the UI, not just when main menu is shown
    connectFromArduino = '\0';
    firstConnection = -1;

#if DEBUG_MAIN_LOOP_CHECKPOINTS
    Serial.write( 'I' ); // About to enter busy loop
    yield( );
#endif

#if debug_busy_timers == 1
    Serial.println( "Starting main loop: " + String( millis( ) ) + " ms" );
    yield( );
#endif
    busyPrintTime = millis( );

    //! This is the main busy wait loop waiting for input
    // CRITICAL: Use Jerial.available() to check relay buffer + Serial
    // CRITICAL: Force line buffering when relay buffer has data (commands need full lines!)

    // Calculate whether to use line buffering (variables declared at function scope)
    hasRelayedData = ( Jerial.getRelayStream( ) && Jerial.getRelayStream( )->available( ) > 0 );
    useLineBuffering = ( jumperlessConfig.terminal.line_buffering == 1 ) || hasRelayedData;

    // DEBUG DISABLED: Heartbeat markers removed to minimize USB pressure
    static uint32_t heartbeatCounter = 0;
    static uint32_t lastHeartbeatPrint = 0;

    loopStart = millis( );
    while ( !Jerial.hasCompletedLine( ) &&
            ( useLineBuffering || Jerial.available( ) == 0 ) &&
            !( helpArmed && (int32_t)( millis( ) - helpDeadlineMs ) >= 0 ) &&
            connectFromArduino == '\0' && slotChanged == 0 ) {

        // Heartbeat disabled for production
        heartbeatCounter++;
        bool printHeartbeat = false; // Was: (heartbeatCounter - lastHeartbeatPrint >= 10000)

        // Recalculate useLineBuffering each iteration
        hasRelayedData = ( Jerial.getRelayStream( ) && Jerial.getRelayStream( )->available( ) > 0 );
        useLineBuffering = ( jumperlessConfig.terminal.line_buffering == 1 ) || hasRelayedData;

        unsigned long loopStart = micros( );

        busyTimers[ 0 ] = micros( );

#if DEBUG_MAIN_LOOP_CHECKPOINTS
        // DEBUG: Checkpoint throughout busy loop to find freeze location
        static uint32_t loopCount = 0;
        loopCount++;
        bool printLoop = ( loopCount % 5000 == 0 );

        if ( printLoop ) {
            Serial.write( 'L' );
            yield( );
        } // Loop start
#endif

        // Would-be watchdog kick, core 0 (measure-only stage - see KickGap.h)
        kickGapStamp( 0, KICK_LOOP0 );

        // Service all registered subsystems via jOSmanager
        // This now includes: Jerial, tud_task, usbPeriodic, oledPeriodic, and all other services
        jOS.serviceAll( );

        // DEBUG: Marker after serviceAll - if we see '<{...}' but not '>' then freeze is between
        // serviceAll() and the marker below
        if ( printHeartbeat ) {
            Serial.write( '>' ); // After serviceAll marker
            yield( );
            lastHeartbeatPrint = heartbeatCounter; // Update here so we get consistent '<{...}>'
        }

#if DEBUG_MAIN_LOOP_CHECKPOINTS
        if ( printLoop ) {
            Serial.write( 'S' );
            yield( );
        } // After serviceAll

        // DEBUG: Check Core 2 state
        if ( printLoop ) {
            extern volatile bool core2busy;
            uint32_t sendBits = 0;
            core1req::snapshot( core1req::REQ_SEND, &sendBits, nullptr, nullptr );
            Serial.write( 'C' );
            Serial.write( '2' );
            Serial.write( '[' );
            Serial.print( (int)core2busy );
            Serial.write( ',' );
            Serial.print( (int)sendBits );
            Serial.write( ',' );
            Serial.print( (int)ledShowPendingBits( ) );
            Serial.write( ']' );
            yield( );
        }
#endif

#if DEBUG_MAIN_LOOP_CHECKPOINTS
        if ( printLoop ) {
            Serial.write( '1' );
            yield( );
        } // Checkpoint 1
#endif

        // (The 10 ms secondSerialHandler() / replyWithSerialInfo() /
        // serviceNetVoltageScanDebug() block that sat here is
        // PortHousekeepingService now - a 10 ms service inside serviceAll().
        // Everything in it does USB-CDC I/O and stays on core 0; B6, T1.7.)
        busyTimers[ 2 ] = micros( );

        // Check for menu activation (goto loadfile)
        // Note: clickMenu() is called within menus.service(), but we need to detect result
        // This will be refactored when we remove gotos entirely
        if ( menus.inClickMenu != 0 ) {

            goto loadfile;
        }
        busyTimers[ 3 ] = micros( );

        // Check if terminal has completed line (includes relayed commands - works regardless of buffering mode)
        if ( Jerial.hasCompletedLine( ) ) {
            break; // Line is ready for processing (could be user input or relayed command)
        }

        busyTimers[ 4 ] = micros( );

        if ( debugWaitLoopTiming ) {
            unsigned long loopEnd = micros( );
            if ( ( loopEnd - loopStart ) > 100000 ) { // More than 100ms (adjust threshold as needed)
                Serial.printf( "DEBUG: *** FULL LOOP took %lu us (%.2f ms) ***\n",
                               loopEnd - loopStart, ( loopEnd - loopStart ) / 1000.0 );
            }
        }
#if debug_busy_timers == 1
        if ( millis( ) - busyPrintTime > busyPrintInterval ) {
            busyPrintTime = millis( );
            for ( int i = 1; i < 10; i++ ) {
                Serial.print( "busyTimer " );
                Serial.print( i );
                Serial.print( ": " );
                Serial.print( busyTimers[ i ] - busyTimers[ i - 1 ] );
                Serial.println( " us" );
            }
            Serial.print( "total: " );
            Serial.print( busyTimers[ 9 ] - busyTimers[ 0 ] );
            Serial.print( " us\t\ttotal system time: " );
            Serial.print( millis( ) );
            Serial.println( " ms" );
            Serial.println( "\n\r" );
            yield( ); // non-blocking USB pump + CDC flush
        }
#endif
        // Service Jerial to process line buffering and poll Port 4 (USBSer3) TUI commands
        Jerial.service( );

        // NEW: Check for pending commands from CommandBuffer (from UART tags)
        // This is the synchronous, simplified path that replaces RelayedCommandService
        if ( CommandBuffer::getInstance( ).hasPendingCommand( ) ) {
            break; // Exit busy loop to process the command
        }
    }

    // =========================================================================
    // NEW: Handle pending commands from CommandBuffer (UART relayed commands)
    // This runs BEFORE checking Jerial, ensuring UART commands are processed promptly
    // =========================================================================
    if ( CommandBuffer::getInstance( ).hasPendingCommand( ) ) {
        // SAFETY: Never execute commands during early startup
        // This prevents crashes if commands somehow get queued before system is ready
        if ( firstLoop > 0 ) {
            // Discard the command - system not ready
            CommandBuffer::getInstance( ).consumePendingCommand( ); // Consume and discard
            // Serial.println( "Warning: Command ignored during startup" );
        } else {
            // Use zero-copy consume to avoid heap-allocating a String for every command
            // String heap fragmentation was causing crashes after minutes of continuous commands
            const char* cmdPtr = CommandBuffer::getInstance( ).consumePendingCommandPtr( );

            if ( cmdPtr != nullptr ) {
                // Trim leading whitespace manually (avoid extra String heap ops)
                while ( *cmdPtr == ' ' || *cmdPtr == '\t' || *cmdPtr == '\r' || *cmdPtr == '\n' )
                    cmdPtr++;

                if ( *cmdPtr != '\0' ) {
                    // SAFETY: Validate command has printable content before processing
                    // This prevents garbled/corrupted serial data from reaching command handlers
                    bool hasValidContent = false;
                    size_t cmdLen = strlen( cmdPtr );
                    if ( cmdLen > 0 && cmdLen < 1024 ) {
                        for ( size_t ci = 0; ci < cmdLen && ci < 8; ci++ ) {
                            if ( cmdPtr[ ci ] >= ' ' && cmdPtr[ ci ] < 127 ) {
                                hasValidContent = true;
                                break;
                            }
                        }
                    }

                    if ( hasValidContent ) {
                        // Single String creation for compatibility with executeCommand API
                        currentCommandLine = cmdPtr;
                        currentCommandLine.trim( );
                        input = cmdPtr[ 0 ];
                        g_commandInputIsLine = true; // a relayed command is a whole line

                        // Service USB to prevent port disconnect during command execution
                        TinyUSB_Device_Task( ); // mutex-guarded pump

                        // Execute the command
                        inMainMenu = true;
                        CommandResult cmdResult = singleCharCommands.executeCommand( (char)input, currentCommandLine );
                        inMainMenu = false;

                        // Queue response to UART if command came from there
                        if ( CommandBuffer::getInstance( ).shouldRespondToUART( ) ) {
                            // Response already sent to Serial - copy to UART buffer
                            // (The command handler writes to Serial, which we can capture)
                            // For now, the command handlers need to check shouldRespondToUART()
                            // and queue responses via CommandBuffer::getInstance().queueForUART()
                            CommandBuffer::getInstance( ).setRespondToUART( false ); // Reset flag
                        }

                        // Clear the command-from-UART flag now that we've processed it
                        AsyncPassthrough::clearCommandFromUARTFlag( );

                        CommandBuffer::getInstance( ).incrementCommandsProcessed( );

                        // Handle special command results
                        switch ( cmdResult ) {
                        case CMD_LOAD_FILE:
                            goto loadfile;
                        case CMD_SHOW_MENU:
                            // Refresh display
                            break;
                        default:
                            break;
                        }

                        goto dontshowmenu; // Skip menu display
                    } // end if (hasValidContent)
                } // end if (*cmdPtr != '\0')
            } // end if (cmdPtr != nullptr)
        } // end else (firstLoop == 0)
    } // end if (hasPendingCommand)

    // Check for completed lines first (includes both relayed and buffered input)
    // This works regardless of line buffering mode - relayed commands always work
    // CRITICAL: Use line buffering when relay buffer has data
    static unsigned long lastCommandProcessedTime = 0;
    gotCompletedLine = false;
    if ( helpArmed ) {
        // Char mode: a command char was read on the previous pass and we went
        // back to the busy loop (services running) to see whether a '?' - or
        // "elp..." after an 'h' - follows within HELP_WAIT_MS. Either a char
        // arrived or the deadline passed; resume with the armed char. Anything
        // that arrived stays in the stream for the peek below / the next pass.
        helpArmed = false;
        input = helpArmedChar;
        currentCommandLine = String( (char)input );
        g_commandInputIsLine = false; // char mode: args may still be arriving
    } else if ( Jerial.hasCompletedLine( ) ) {
        gotCompletedLine = true;
        g_commandInputIsLine = true;
        // Track command processing latency

        unsigned long timeSinceLastCommand = millis( ) - lastCommandProcessedTime;
        if ( lastCommandProcessedTime > 0 && timeSinceLastCommand > 500 ) {
#if debugJerial
            Serial.print( "⏱️  Main loop gap: " );
            Serial.print( timeSinceLastCommand );
            Serial.println( " ms between commands" );
            Serial.flush( );
#endif
        }
        lastCommandProcessedTime = millis( );

        String cmdLine = Jerial.getCompletedLine( ); // Get and consume the line
        cmdLine.trim( );
        currentCommandLine = cmdLine; // Store for backwards compatibility with parsers

        if ( cmdLine.length( ) > 0 ) {
            input = cmdLine[ 0 ];
        } else {
            input = '\n';
        }
        noteUserInput( );
    } else if ( !useLineBuffering ) {
        // Only read single character if NOT in line buffering mode
        // (line buffering mode already handled by Jerial.service() above)
        // NOTE: Jerial.read() now handles relay buffer with tag filtering automatically
        if ( Jerial.available( ) > 0 ) {
            input = Jerial.read( );
            g_commandInputIsLine = false; // char mode: args may still be arriving
            noteUserInput( );
#if debugJerial
            Serial.printf( "Main: Read char '%c' (%d) from Jerial\n", (char)input, input );
            Serial.flush( );
#endif
            // Set currentCommandLine with just the single character for backwards compatibility
            // CRITICAL: Cast to char first! String(int) creates decimal string "87", not "W"
            currentCommandLine = String( (char)input );
        }
    }

    // Service incoming serial and use our line buffer instead of direct Serial.read

    // timer = millis( );
    // // Serial.print("input = ");
    // Serial.println(input);
    // Serial.flush();

    // -------- Help: "help", "help <category>", and "[command]?" --------
    // Line mode: the whole line is already in currentCommandLine, so this is
    // a look at the string - "help", "help <category>", "x?". (Before B6 the
    // line path waited 100 ms on an empty stream and then ran the bare
    // command: "help" printed the menu and "x?" CLEARED THE BOARD.)
    // Char mode: the '?' (or "elp...") has not arrived yet when the command
    // char is read. Instead of a 100 ms spin with nothing serviced, arm a
    // deadline and go back to the busy loop; it returns here on the next
    // char or when the deadline passes (see helpArmed above).
    // Commands that take '?' as their own sub-command are left alone in both
    // modes (they used to get the help page in char mode and their own
    // handler in line mode - now always their handler).
    if ( gotCompletedLine ) {
        if ( currentCommandLine == "help" ) {
            showGeneralHelp( );
            goto dontshowmenu;
        }
        if ( currentCommandLine.startsWith( "help " ) ) {
            String category = currentCommandLine.substring( 5 );
            category.trim( );
            showCategoryHelp( category.c_str( ) );
            goto dontshowmenu;
        }
        if ( currentCommandLine.length( ) == 2 && currentCommandLine[ 1 ] == '?' &&
             helpQuestionApplies( input ) ) {
            showCommandHelp( input );
            goto dontshowmenu;
        }
    } else if ( input == 'h' || helpQuestionApplies( input ) ) {
        if ( Jerial.available( ) == 0 && !helpDeadlineSpent ) {
            // Nothing after the char yet: arm the deadline and keep servicing.
            helpArmed = true;
            helpArmedChar = input;
            helpDeadlineMs = millis( ) + HELP_WAIT_MS;
            helpDeadlineSpent = true; // one wait per command char
            goto dontshowmenu;
        }
        helpDeadlineSpent = false;
        if ( input == 'h' && Jerial.available( ) > 0 ) {
            String helpString = "h";
            while ( Jerial.available( ) > 0 && helpString.length( ) < 50 ) {
                char c = Jerial.read( );
                if ( c == '\n' || c == '\r' )
                    break;
                helpString += c;
            }
            if ( helpString == "help" ) {
                showGeneralHelp( );
                goto dontshowmenu;
            }
            if ( helpString.startsWith( "help " ) ) {
                String category = helpString.substring( 5 );
                category.trim( );
                showCategoryHelp( category.c_str( ) );
                goto dontshowmenu;
            }
            // Just 'h' alone (or 'h' + something else): fall through
        } else if ( Jerial.available( ) > 0 && Jerial.peek( ) == '?' ) {
            Jerial.read( ); // consume '?'
            showCommandHelp( input );
            goto dontshowmenu;
        }
    }

    if ( input == ' ' || input == '\n' || input == '\r' ) {
        // Serial.print(input);
        // Serial.flush();
        goto dontshowmenu;
    }
skipinput:

    // ========================================================================
    // Execute command using SingleCharCommands service
    // ========================================================================
    {
        inMainMenu = true;
        CommandResult cmdResult = singleCharCommands.executeCommand( (char)input, currentCommandLine );
        inMainMenu = false;

        // Handle command result
        switch ( cmdResult ) {
        case CMD_LOAD_FILE:
            goto loadfile;
        case CMD_DONT_SHOW_MENU:
            goto dontshowmenu;
        case CMD_SHOW_MENU:
        default:
            // (The "clean up serial buffer" drain that sat here - a 1 ms
            // delay, then eat raw Serial down to 5 bytes - is gone (B6): it
            // ate the tail of any multi-line paste after the first command,
            // and Jerial.service()'s line buffering is what owns the input.
            // printMenu() still discards a >20-byte backlog on its own.)
            goto menu;
        }
    }

    // ========================================================================
    // OBSOLETE CODE BELOW - KEPT FOR REFERENCE ONLY
    // ========================================================================
    // The giant switch statement has been refactored into SingleCharCommands
    // All command handlers are now in SingleCharCommands.cpp with proper OOP design
    // This code below should NEVER execute - it's kept temporarily for reference
    // TODO: Delete lines 710-2238 (the old switch statement) after testing
    // ========================================================================

    goto menu; // Safety: skip old code and go back to menu

loadfile:
    loadingFile = 1;
    // Suppress undo recording for the entire slot-load region. Loading
    // a slot's YAML into globalState issues addConnection() /
    // setRailVoltage() / setDacVoltage() calls in bulk - these are
    // bringing globalState into sync with what's already on disk, NOT
    // user actions, and must not enter the destination slot's history.
    // Without this, switching to slot 1 would fill its undo log with
    // phantom "connect X-Y" entries (and any user undo on slot 1 would
    // start undoing the file load itself, which is nonsensical).
    undoBeginIngest();
    startupTimers[ 10 ] = millis( );
    // Just clear preview mode flag - don't restore original slot
    // Let the normal load below handle loading the selected slot
    SlotManager& mgr = SlotManager::getInstance( );
    if ( mgr.isPreviewMode( ) ) {
        // Serial.println("Clearing preview mode");
        // Serial.flush();
        //  Clear preview flag without loading anything
        mgr.clearPreviewMode( );
    }
    startupTimers[ 11 ] = millis( );
    // Save current state if dirty before reloading to prevent data loss
    // BUT skip this on the very first load (firstLoop == 1) to avoid overwriting
    // the saved slot with an empty/uninitialized state
    // Serial.println("globalState.isDirty( ) = " + String(globalState.isDirty( )));
    // Serial.println("firstLoop = " + String(firstLoop));
    // Serial.flush();
    if ( globalState.isDirty( ) && firstLoop == 0 ) {
        // Serial.println( "Saving dirty state before reload" );
        // Serial.flush();
        String saveError;
        if ( mgr.saveActiveSlot( saveError ) ) {
            if ( debugFP ) {
                Serial.println( "✓ Auto-saved dirty state before reload" );
            }
        } else if ( debugFP ) {
            Serial.println( "Warning: Failed to auto-save: " + saveError );
        }
    }
    startupTimers[ 12 ] = millis( );
    // Load YAML state from the active context into globalState. Branch on the
    // sentinel EXACTLY - never on `< 0`: netSlot == -1 and netSlot == NUM_SLOTS
    // are live defcon sentinels set by Graphics::attractMode, and they must
    // keep failing through to clearActiveSlot() the way they always have.
    //
    // On the very first pass this is what executes the boot seed: seedBootContext()
    // has already put either a number or (-2 + activeSlotPath) in place.
    String loadError;
    bool loadOk;
    if ( netSlot == SLOT_FILE_CONTEXT ) {
        String ctxPath = String( mgr.getActiveSlotPath( ) );
        loadOk = mgr.loadSlotFromPath( ctxPath, loadError );
        if ( !loadOk ) {
            // The boot context's file vanished or won't open. Fall back to
            // slot 0 AND rewrite last_active.txt, so the failure doesn't
            // recur on every boot from here on.
            Serial.println( "Active slot file unavailable (" + ctxPath + "): " + loadError );
            Serial.println( "Falling back to slot 0" );
            netSlot = 0;
            loadOk = mgr.loadSlot( 0, loadError );  // rewrites last_active via updateLastActive
        }
    } else {
        loadOk = mgr.loadSlot( netSlot, loadError );
    }
    if ( !loadOk ) {
        if ( debugFP ) {
            Serial.print( "Warning: Failed to load slot " );
            Serial.print( netSlot );
            Serial.print( ": " );
            Serial.println( loadError );
            Serial.println( "Starting with empty slot" );
        }
        // Empty slot is OK - just start fresh
        mgr.clearActiveSlot( );
    }
    startupTimers[ 13 ] = millis( );
    if ( slotChanged == 1 ) {
        // clearChangedNetColors(0);
        // loadChangedNetColorsFromFile( netSlot, 0 );
    }

    // Initialize fake GPIO pins from loaded state (before refreshing connections)
    // This restores FakeGpioOutput/Input entries from FAKE_GPIO bridges in the state
    initializeFakeGpioFromLoadedState( );

    // (Stale power-claim bridges in old slot files are dropped by
    // infraScrubLoadedBridges() inside the state load sanitizer, and the
    // refresh below re-adds the live infra bridges via infraEvaluate().)

    slotChanged = 0;
    loadingFile = 0;
    if ( firstLoop == 2 ) {
        refreshConnections( -1, 1, 1 );
    } else {
        refreshConnections( -1, 1, 0 );
    }

    // Phase 2: now that paths are routed, finalize FakeGPIO (extract chipKY,
    // register TDM channels, disconnect input paths for TDM isolation)
    finalizeFakeGpioAfterRouting( );

    startupTimers[ 14 ] = millis( );
    // refreshConnections( -1, 1, 0 );
    undoEndIngest();
}

unsigned long lastSwirlTime = 0;

int swirlCount = 42;
int spread = 13;

unsigned long schedulerTimer = 0;
unsigned long schedulerUpdateTime = 8000;

int swirled = 0;
int countsss = 0;

int probeCycle = 0;
int netUpdateRefreshCount = 0;

int clearBeforeSend = 0;

int passthroughStatus = 0;

unsigned long serialInfoTimer = 0;

bool debugWaitLoopTimingCore2 = false; // Enable via 'core2timing' command
unsigned long lastCore2LoopStart = 5000000;
unsigned long t[ 22 ];

// Core 2 timing stats - smart accumulation
unsigned long core2LoopIterations = 0;
unsigned long lastTimingPrint = 0;
unsigned long timingPrintInterval = 1000; // Print summary every 1000ms

// Track LED show() calls
unsigned long ledShowCallCount = 0;
unsigned long ledShowTotalTime = 0;
unsigned long ledShowMinTime = 999999;
unsigned long ledShowMaxTime = 0;

#define POWER_SUPPLY_SENSE_ENABLED 1

bool printPowerSupplySense = false;
unsigned long powerSupplySenseTimer = 0;
unsigned long powerSupplySenseRate = 1000;
float supplySense = 9.10F;

void loop1( ) {
    // Would-be watchdog kick, core 1 (measure-only stage - see KickGap.h).
    // Stamped BEFORE the pause wait so a long frame hold / FlashPark park /
    // WaveGen capture shows up as a gap.
    kickGapStamp( 1, KICK_LOOP1 );

    while ( core1FramesHeld( ) ) {
        // Check for an immediate bypass request even while paused (not while a
        // DMA-fed send is still strobing - its ISR completes that one first)
        if ( core1req::pending( core1req::REQ_BYPASS ) && !ch446qSendInFlight( ) ) {
            uint32_t g = 0;
            if ( core1req::take( core1req::REQ_BYPASS, &g ) ) {
                core2busy = true;
                sendPaths( 0, core1req::REQ_BYPASS, g ); // Send paths without cleaning; CH446Q completes g
                __dmb( ); // Memory barrier so Core 0 sees the update
                core2busy = false;
            }
        }
        tight_loop_contents( );
        // replyWithSerialInfo( );
    }

    // if (micros() - lastCore2LoopStart > 5000000 &&  (micros( ) - schedulerTimer > schedulerUpdateTime)) {
    //     debugWaitLoopTimingCore2 = true;
    //     lastCore2LoopStart = micros( );
    // }
    //     else {
    //         debugWaitLoopTimingCore2 = false;
    //     }

    for ( int i = 0; i < 22; i++ ) {
        t[ i ] = 0;
    }

    // Core 2 timing instrumentation (only when debug enabled)
    static unsigned long core2LoopStart = 0;
    if ( debugWaitLoopTimingCore2 ) {
        core2LoopStart = micros( );
    }

    // Priority order:
    // 1) High: path/LED refresh triggered by core1 (handled in core2stuff)
    // 2) Medium: wavegen_service (function generator streaming)
    // 3) Medium-low: rotary encoder
    // 4) Low: logo swirls/animations

    if ( core1FramesHeld( ) )
        return; // Exit early to allow flash operations

#if POWER_SUPPLY_SENSE_ENABLED == 1

    if ( millis( ) - powerSupplySenseTimer > powerSupplySenseRate ) {

        supplySense = readAdcVoltage( 6, 4 );

        // NO Serial FROM CORE 1 (sweep finding): USB CDC writes from this
        // core are the codebase's own documented board-wedge class. The
        // reading lives in the supplySense global; a core-0 debug path can
        // print it.
        // if ( printPowerSupplySense == 1 ) { ... }

        powerSupplySenseTimer = millis( );
    }
#endif
    // OPTIMIZATION: Only service wavegen when it's actually running
    // wavegen.service() contains a blocking while() loop for I2C streaming!
    t[ 2 ] = micros( );
    if ( wavegen.isRunning( ) ) {
        // Serial.println("CORE2: wavegen.service() is being called");
        wavegen.service( );
    }
    if ( debugWaitLoopTimingCore2 ) {
        t[ 3 ] = micros( );
        // if ( ( t[3] - t[2] ) > 1000 ) {
        //    // Serial.printf( "CORE2: wavegen.service() took %lu us\n", t[3] - t[2] );
        // }
    }

    if ( doomOn == 1 ) {
        playDoom( );
        doomOn = 0;
    } else if ( !core1FramesHeld( ) ) {
        // Always call core2stuff() for logo swirls and animations
        t[ 4 ] = micros( );
        core2stuff( );
        t[ 5 ] = micros( );
#if 0 // Enable for debugging
        if ( ( t[5] - t[4] ) > 1000 ) { // Report if core2stuff takes > 1ms
            Serial.printf( "CORE2: core2stuff() took %lu us\n", t[5] - t[4] );
        }
#endif
    }

    // REMOVED: AsyncPassthrough::task() and secondSerialHandler() moved to Core 0
    // They were causing refreshLocalConnections() to be called from Core 2
    // when handling Arduino flashing (DTR pulse detection)

    // Check the frame hold before serial operations
    if ( core1FramesHeld( ) )
        return;

    // replyWithSerialInfo() was MOVED to Core 0's loop() (next to
    // secondSerialHandler()). It does USB-CDC I/O (USBSerX.available()/peek()/
    // print()), and on RP2040 arduino-pico those pump TinyUSB_Device_Task() via
    // yield() ON THE CALLING CORE. TinyUSB must only be serviced from Core 0:
    // running it here raced Core 0's USB use (REPL/commands) and wedged the
    // board - SWD-confirmed core1 hang in
    //   loop1 -> replyWithSerialInfo -> USBSer2.available -> yield
    //         -> TinyUSB_Device_Task -> mutex_try_enter -> spin_unlock (0xd0000154)
    // surfacing as "Core 2 ... sendAllPathsCore2 ... timeout" + USB disconnect.
    // This matches the earlier move of AsyncPassthrough/secondSerialHandler off
    // Core 2 (see note above). Applies to BOTH boards.

    // (The LED-dump block that sat here - dumpLEDs() every dumpLEDrate ms from
    // THIS core - is gone (T1.10). dumpLEDs() writes USB CDC, and USB is core
    // 0's: the same wedge family as replyWithSerialInfo() above. Core 1 now
    // only raises ledDumpFrameReady after a frame is shown (core2stuff);
    // LedDumpService on core 0 does the dump.)

    // Core 2 loop timing disabled by default to prevent deadlocks
    // Enable by uncommenting and setting threshold higher (e.g., > 50000 for 50ms+)
    // unsigned long core2LoopEnd = micros( );
    // if ( core2LoopEnd - core2LoopStart > 50000 ) {
    //     Serial.printf( "CORE2 LOOP: %lu us\n", core2LoopEnd - core2LoopStart );
    // }
}

#define FORCE_SHOW_INTERVAL 1000 // 1 second
unsigned long lastForcedShow = 0;

// DEBUG: Set to 1 to disable Core 2 processing for crash debugging
#define DEBUG_DISABLE_CORE2_PROCESSING 0 // TEMP: Testing if crash is in Core 2

// Set by ledClass::show() when the strip DMA was still busy and the composed
// frame was not pushed to the wire (LEDs.cpp).
extern volatile bool ledShowFrameDropped;

// Give back a request that was taken but not rendered/shown. A raw post() ORs
// the mode back in on top of whatever core 0 posted meanwhile (LED_NETS|LED_MENU
// pending together), and the decode below serves MENU first - the full render
// gets downgraded to a menu flush. postMode() re-applies the mode exclusively
// and carries the same keep-rule requestLedShow() uses (Commands.cpp).
// Re-post BEFORE completing the taken generation: in between, the slot would
// read idle (bits 0, doneGen caught up) for a frame that is neither shown nor
// pending, and core 0 polls exactly that (ledShowIdle - Probing.cpp's
// wait-for-shown spins on it). doneGen is monotonic, so completing the older
// generation after the new post is safe.
static void repostLedShow( uint32_t bits, uint32_t gen ) {
    const uint32_t mode = bits & core1req::LED_MODE_MASK;
    core1req::postMode( core1req::REQ_SHOW_LEDS, core1req::LED_MODE_MASK, mode,
                        bits & ~core1req::LED_MODE_MASK,
                        mode == core1req::LED_MENU ? ( core1req::LED_NETS | core1req::LED_GFX ) : 0 );
    core1req::complete( core1req::REQ_SHOW_LEDS, gen );
}

void core2stuff( ) // core 2 handles the LEDs and the CH446Q8
{
    core2busy = false;

#if DEBUG_DISABLE_CORE2_PROCESSING
    // Skip all Core 2 processing for crash debugging
    // If crash stops when this is enabled, the bug is in Core 2 code
    requestLedShow( 0 );
    return;
#endif

    // Encoder poll: Core 1 is the SOLE owner of encoder state (see
    // RotaryEncoder.cpp single-owner banner; Core 0 calls are no-ops).
    // Polled unconditionally here - not just in the branch-specific sites
    // below - because probe mode runs with showLEDsCore2 != 0 and
    // inClickMenu == 0, a combination none of the in-branch poll sites
    // cover. Internally throttled to 2kHz, so calling every pass is cheap.
    rotaryEncoderStuff( );

    // T2.3: the DMA-fed list send's stall watchdog (a no-op unless a send is
    // in flight). A send in flight also means: do not start another one - its
    // ISR completes the request it serves; the next request waits its turn.
    ch446qDmaService( );
    const bool sendInFlight = ch446qSendInFlight( );

    // OPTIMIZATION: Check bypass flag BEFORE trying to acquire mutex
    // This prevents deadlock when Core 0 is waiting for Core 2 but Core 2 can't get mutex
    // The bypass flag (3) is specifically designed for fast, non-blocking operation
    if ( !sendInFlight && core1req::pending( core1req::REQ_BYPASS ) ) {
        // For bypass mode, try to acquire mutex with very short timeout
        // If we can't get it quickly, just skip this frame - the request stays
        // posted (a peek is not a take) and we retry next pass
        if ( core_sync_acquire_timeout_ms( 1 ) ) {
            uint32_t g = 0;
            if ( core1req::take( core1req::REQ_BYPASS, &g ) ) {
                core2busy = true;
                sendPaths( 0, core1req::REQ_BYPASS, g ); // no clean; CH446Q completes g (from the ISR on the DMA path)
                __dmb( ); // Memory barrier so Core 0 sees the update
                core2busy = false;
            }
            core_sync_release( );
        } else {
            __dmb( );
        }
        return; // Exit immediately after handling bypass
    }

    // THREAD SAFETY: Try to acquire the core sync mutex with a VERY short timeout
    // OPTIMIZATION: Use 100us timeout instead of 5ms to reduce blocking
    // If Core 1 is holding the lock, we skip this frame and try again next iteration
    // This allows Core 2 to continue with logo swirls and other non-mutex tasks
    if ( !core_sync_acquire_timeout_ms( 0 ) ) { // 0 = try without blocking
        // Could not acquire mutex immediately - Core 1 is busy with shared resources
        // Skip this frame to prevent blocking Core 2 loop
        // Logo swirls and animations will still run on next iteration
        return;
    }

    // From here on, we hold the mutex and can safely access shared resources
    // Make sure to release it before returning!

    // CRITICAL FIX: Check the frame hold immediately after acquiring mutex
    // If Core 0 took a frame hold while we were waiting for the mutex, we need to
    // release and return immediately to avoid flash XIP crashes during file writes
    if ( core1FramesHeld( ) ) {
        ledFrameAbortsPause++;
        core_sync_release( );
        return;
    }

    // Pace menu frame transitions: when the engine says the next blend frame
    // is due, request a menu flush. This MUST live up here, checked every
    // iteration — the LED branch below only executes when a show is
    // already requested or a logo swirl ticked, so a re-trigger placed inside it
    // got paced by the 51ms swirl timer instead of MT_FRAME_MS (visibly
    // choppy transitions).
    if ( inClickMenu == 1 && ledShowIdle( ) && menuTransitionFrameDue( ) ) {
        requestLedShow( 2 );
    }

    // T2.2c: the LED-show request lives in the mailbox (REQ_SHOW_LEDS). PEEK
    // it here (a peek is not a take): the mode decides whether this pass runs
    // now (menu flush / staged graphics: immediately, no scheduler tick) or on
    // the tick (nets); the flags carry clear-first and blocking exactly as the
    // old int's sign and +10 did. Staged graphics' "3 keeps control of the
    // LEDs" is the ledGraphicsOwned() state: while it holds and nothing new is
    // requested, this branch does nothing and the logo does not swirl - what
    // the sticky 3 did.
    uint32_t ledBits = 0;
    core1req::snapshot( core1req::REQ_SHOW_LEDS, &ledBits, nullptr, nullptr );
    const bool ledPending  = ( ledBits & core1req::LED_MODE_MASK ) != 0;
    const bool ledImmediate = ( ledBits & ( core1req::LED_MENU | core1req::LED_GFX ) ) != 0;
    bool useBlockingMode = false;

    // Run the LED block immediately (don't wait for the 8ms scheduler tick) for the
    // interactive modes 2 (menu text flush) and 3 (staged graphics). Modes 4/5/6
    // were never written by any code and have been removed.
    if ( micros( ) - schedulerTimer > schedulerUpdateTime || ledImmediate ) {

        // (a pending path send goes first: the LED branch waits for the SEND
        // slots to be idle, exactly as it waited for sendAllPathsCore2 == 0)
        if ( ( ( ledPending && ( loadingFile == 0 || ( ledBits & core1req::LED_GFX ) ) ) ||
               ( swirled == 1 && !ledGraphicsOwned( ) ) ) &&
             core1req::allIdle( ) ) {

            // Take the request now (its bits are cleared; anything posted while
            // we render stays pending for the next pass - the old
            // compare-and-swap clear, done by the mailbox). No request = a
            // swirl-only pass, which renders exactly as the old rails == 0 did.
            uint32_t ledGen = 0;
            uint32_t taken = ledPending ? core1req::take( core1req::REQ_SHOW_LEDS, &ledGen ) : 0;
            uint32_t repostBits = taken;   // what a re-post below still owes; a flag is dropped as it is consumed
            int rails = 0;   // 3 doesn't show nets and keeps control of the LEDs
            if ( taken & core1req::LED_GFX )       rails = 3;
            else if ( taken & core1req::LED_MENU ) rails = 2;
            else if ( taken & core1req::LED_NETS ) rails = 1;
            if ( taken & core1req::LED_CLEAR )    clearBeforeSend = 1;
            if ( taken & core1req::LED_BLOCKING ) useBlockingMode = true;

            if ( rails != 3 ) {
                core2busy = true;
                lightUpRail( -1, -1, 1 );
                logoSwirl( swirlCount, spread, probeActive );
                core2busy = false;
            }

            if ( rails == 3 ) {
                core2busy = true;

                logoSwirl( swirlCount, spread, probeActive );
                core2busy = false;
            }

            // Special case for menu mode (rails == 2): call leds.show() to display menu text
            // that was written to buffer by b.print() in menu code
            bool needsLedShow = false;

            // Allow showing nets if not in menu OR if in preview mode OR
            // if the History scrub menu is live (its job is to show the
            // reverted bridge state on the breadboard, not menu text), or if a
            // GPIO/BCD UI is open (same deal: it lives on the OLED so the
            // breadboard can keep showing the circuit being configured).
            if ( rails != 2 && rails != 3 &&
                 ( inClickMenu == 0 || SlotManager::getInstance( ).isPreviewMode( ) ||
                   g_historyScrubActive || g_gpioUiShowsCircuit ) &&
                 inPadMenu == 0 ) {
                needsLedShow = true;

                // Skip defcon display when previewing slots - always show nets
                if ( defconDisplay >= 0 && probeActive == 0 && !SlotManager::getInstance( ).isPreviewMode( ) ) {

                    // core2busy = true;
                    // defcon( swirlCount, spread, defconDisplay );
                    // core2busy = false;
                } else {

                    // while ( core1busy == true ) {
                    //     // core2busy = false;
                    // }
                    core2busy = true;

                    if ( clearBeforeSend == 1 ) {
                        clearLEDsExceptRails( );
                        // Serial.println("clearing");
                        clearBeforeSend = 0;
                        repostBits &= ~core1req::LED_CLEAR;
                    }

                    // Check the frame hold before long-running showNets() to allow quick exit for flash ops
                    if ( core1FramesHeld( ) ) {
                        ledFrameAbortsPause++;
                        // The request was already TAKEN: completing it keeps
                        // the ledShowIdle gate honest, and re-posting means
                        // the dropped frame renders once the hold releases
                        // (sweep finding - taken-but-never-completed wedged
                        // the whole gated pipeline).
                        if ( taken ) {
                            repostLedShow( repostBits, ledGen );
                        }
                        core2busy = false;
                        core_sync_release( );
                        return;
                    }

                    t[ 6 ] = micros( );
                    showNets( );
                    if ( debugWaitLoopTimingCore2 ) {
                        t[ 7 ] = micros( );
                        if ( ( t[ 7 ] - t[ 6 ] ) > 5000 ) {
                            // Serial.printf( "CORE2:   showNets() took %lu us\n", t[7] - t[6] );
                        }
                    }

                    // showNets() can take several ms - service the encoder
                    // mid-pass so clickwheel latency doesn't stack up with
                    // the rest of this block (self-throttled, owner core).
                    rotaryEncoderStuff( );

                    t[ 8 ] = micros( );
                    readGPIO( );     // if want, I can make this update the LEDs like 10 times
                                     // faster by putting outside this loop,
                    readFakeGPIO( ); // Background reading for fake GPIO inputs with visual updates
                    t[ 9 ] = micros( );

                    // CRITICAL: Update ADC/GPIO mappings before reading measurements
                    // This ensures showADCreadings[] is populated from current paths
                    // Prevents race condition where Core 0/1 updates paths but Core 2 reads stale ADC mappings
                    // chooseShownReadings( );

                    showLEDmeasurements( );

                    t[ 10 ] = micros( );
                    showAllRowAnimations( );

                    // Render graphic overlays on top of all other LED visualizations
                    renderGraphicOverlays( );

                    t[ 11 ] = micros( );

                    core2busy = false;
                    // needsLedShow = true;
                    netUpdateRefreshCount = 0;
                }
            } else if ( rails == 2 && ( inClickMenu == 1 || inPadMenu == 1 ) ) {
                // Menu mode - display menu text buffer written by b.print()
                // Supports both click menus (inClickMenu) and probe menus (inPadMenu)
                // The transition engine paints the prev->target frame blend into
                // the buffer first; it returns false while Core 0 is mid-repaint
                // so we never show a half-drawn menu frame.
                //
                // CRITICAL: re-check the frame hold and raise core2busy around the
                // render. menuTransitionRender() is flash-resident code, and
                // pauseCore2ForFlash() waits on core2busy before letting a
                // flash op proceed — without this bracket it saw Core 2 as
                // "idle" while we were mid-render and started erasing flash
                // under our XIP fetches (the File-Manager-from-menu crash:
                // FM runs with inClickMenu==1, so this branch stays hot
                // during its flash writes; serial entry has inClickMenu==0
                // and never hit it).
                if ( core1FramesHeld( ) ) {
                    ledFrameAbortsPause++;
                    if ( taken ) {   // see the showNets() abort above
                        repostLedShow( repostBits, ledGen );
                    }
                    core_sync_release( );
                    return;
                }
                core2busy = true;
                needsLedShow = menuTransitionRender( );
                core2busy = false;
            } else if ( rails == 2 ) {
                // holdAnimationStuff() uses showLEDsCore2=2 for press/hold/reboot
                // sweeps on the header + logo. That mode is shared with menu
                // text flushes, but outside menus there is no buffer paint — still
                // need leds.show() or the animation never reaches the strip.
                needsLedShow = true;
            }

            // Call leds.show() if either showNets() was called OR if we're in menu mode.
            // Menu frames re-check the transition draw gate at the last moment:
            // Core 0 may have STARTED a repaint since menuTransitionRender()
            // returned, and showing now would stream a half-painted buffer
            // (one frame of garbled/misaligned text).
            if ( rails == 2 && inClickMenu == 1 && !menuTransitionCanShow( ) ) {
                needsLedShow = false;
            }
            if ( needsLedShow ) {
                core2busy = true;

                // Check the frame hold before long-running leds.show() to allow quick exit for flash ops
                // if ( core1FramesHeld( ) ) {
                //     core2busy = false;
                //     core_sync_release( );
                //     return;
                // }

                t[ 12 ] = micros( );

                // Use blocking or async show based on flag
                // Blocking mode (showLEDsCore2 >= 10) forces atomic display updates
                // Used for voltage adjuster and other UIs that need complete frames
                if ( useBlockingMode ) {
                    leds.showBBBlocking( );
                } else {
                    leds.show( );
                }
                lastForcedShow = millis( );
                ledFramesShown++;         // X: strip frames shown by this branch (idle renders + requests)
                if ( rails == 0 ) ledIdleFramesShown++;
                ledDumpFrameReady = true; // LED-dump mode: core 0 may dump this frame
                xbarLatShow( );           // latency probe: first show after a send (XbarLatency.h)

                t[ 13 ] = micros( );

                // Update probe LEDs to reflect current state

                core2busy = false;
            }

            // The request we took is done (a show posted while we rendered is
            // still pending in the slot and runs next pass). A staged-graphics
            // request is done too - its ownership persists as state, not as a
            // pending request. Unless leds.show() found the strip DMA busy and
            // skipped the transfer - then the composed frame never reached the
            // wire, so hand the request back the same way the aborts above do.
            if ( taken ) {
                if ( needsLedShow && ledShowFrameDropped ) {
                    repostLedShow( repostBits, ledGen );
                } else {
                    core1req::complete( core1req::REQ_SHOW_LEDS, ledGen );
                }
                uint8_t menuBits = (uint8_t)( ( inClickMenu ? 1 : 0 ) | ( inPadMenu ? 2 : 0 ) );
                uint8_t prev = (uint8_t)( ( ledTakeLogIdx + 31 ) & 31 );
                if ( ledTakeLog[ prev ].t != 0 && ledTakeLog[ prev ].bits == (uint8_t)taken && ledTakeLog[ prev ].rails == (uint8_t)rails &&
                     ledTakeLog[ prev ].menu == menuBits && ledTakeLog[ prev ].shown == ( needsLedShow ? 1 : 0 ) ) {
                    ledTakeLog[ prev ].repeats++;
                } else {
                    uint8_t k = ledTakeLogIdx;
                    ledTakeLog[ k ].t = millis( ); ledTakeLog[ k ].bits = (uint8_t)taken; ledTakeLog[ k ].rails = (uint8_t)rails;
                    ledTakeLog[ k ].menu = menuBits; ledTakeLog[ k ].shown = needsLedShow ? 1 : 0; ledTakeLog[ k ].repeats = 0;
                    ledTakeLogIdx = (uint8_t)( ( k + 1 ) & 31 );
                }
            }

            // (Menu transition frames are re-triggered by the paced check at
            // the top of core2stuff — see menuTransitionFrameDue.)

            swirled = 0;
            if ( inClickMenu == 1 ) {
                t[ 16 ] = micros( );
                rotaryEncoderStuff( );
                t[ 17 ] = micros( );
            }
            core2busy = false;

        } else if ( !sendInFlight && core1req::pending( core1req::REQ_SEND ) ) {
            t[ 18 ] = micros( );
            uint32_t g = 0;
            uint32_t bits = core1req::take( core1req::REQ_SEND, &g );
            if ( bits ) {
                // A sticky clean bit wins over any plain send it coalesced with.
                // CH446Q completes g when the send is done (ISR on the DMA path).
                sendPaths( ( bits & core1req::SEND_CLEAN ) ? 1 : 0, core1req::REQ_SEND, g );
            }
            t[ 19 ] = micros( );
        } else if ( millis( ) - lastSwirlTime > 51 && loadingFile == 0 &&
                    ledShowIdle( ) && core1busy == false ) {

            lastSwirlTime = millis( );

            if ( swirlCount >= LOGO_COLOR_LENGTH - 1 ) {
                swirlCount = 0;
            } else {
                swirlCount++;
            }

            if ( swirlCount % 20 == 0 ) {
                countsss++;
            }

            if ( ledShowIdle( ) && !wavegen.isRunning( ) ) {
                swirled = 1; // only swirl when wavegen not streaming
            }



        } else {
            t[ 20 ] = micros( );
            rotaryEncoderStuff( );
            t[ 21 ] = micros( );
        }

        // if ( millis( ) - lastForcedShow > FORCE_SHOW_INTERVAL ) {
        //     lastForcedShow = millis( );
        //     leds.show();
        // }

        schedulerTimer = micros( );
        core2busy = false;
    } else if ( checkingButton == 0 && ProbeButton::getInstance( ).getButtonState( ) == 0 ) {
        t[ 14 ] = micros( );
        probing.probeLEDhandler( );
        t[ 15 ] = micros( );
        // lastProbeLEDs = showProbeLEDs;
    }

    // THREAD SAFETY: Release the core sync mutex
    // This MUST be called before returning to allow Core 1 to access shared resources
    core_sync_release( );

    // Keep the full adcReadings[] cache fresh in the background (for the OLED
    // GUI / cached {adc:N} tokens). Self-throttled and gated on the core-1 frame hold.
    // Done AFTER releasing core_sync so a background ADC read (which uses its
    // own atomic ADC lock) can never extend the core_sync hold and stall core0.
    // Compiled out when LAZY_ADC_READINGS == 0.
    updateLazyAdcReadings( );

#if USB_AUDIO_ENABLE
    // Start/stop the USB audio capture the host asked for. The class callbacks
    // run in USB IRQ context and only raise a flag, because claiming the ADC
    // there could spin on readingADC for 100 ms inside an interrupt.
    //
    // Lives on core1 next to the lazy ADC refresh rather than in loop(): this
    // is the path that provably runs every iteration, whereas core0's loop()
    // can sit inside the menu/REPL for long stretches. It also keeps the DMA
    // completion IRQ on core1, away from core0's LED/USB frame work.
    serviceUSBAudio( );
#endif

    // Board-wide node voltage scan for per-connection currents. Runs AFTER
    // core_sync_release like the lazy ADC reads, so a multi-ms sense tap can
    // never extend the LED-frame core_sync/core2busy hold that Core 0's
    // waitCore2() blocks on - stacking it inside the frame was the "clickwheel
    // super laggy" regression. Still core 1 only (taps stay serialized with
    // sendPaths); internally gated on the frame hold/sendAllPathsCore2, preempted
    // by recent user input, and raises core2busy around its hardware work.
    serviceNetVoltageScan( );

    // TIMING DEBUG OUTPUT - Smart accumulation and periodic printing
    if ( debugWaitLoopTimingCore2 ) {
        core2LoopIterations++;

        // Calculate LED show time for this iteration
        unsigned long ledsShowTime = ( t[ 13 ] > t[ 12 ] ) ? ( t[ 13 ] - t[ 12 ] ) : 0;

        // Accumulate LED show() statistics
        if ( ledsShowTime > 0 ) {
            ledShowCallCount++;
            ledShowTotalTime += ledsShowTime;
            if ( ledsShowTime < ledShowMinTime )
                ledShowMinTime = ledsShowTime;
            if ( ledsShowTime > ledShowMaxTime )
                ledShowMaxTime = ledsShowTime;
        }

        // Print summary once per second
        unsigned long now = millis( );
        if ( now - lastTimingPrint >= timingPrintInterval ) {
            lastTimingPrint = now;

            if ( ledShowCallCount > 0 ) {
                Serial.println( "\n==== LED TIMING SUMMARY (1 sec) ====" );
                Serial.printf( "  leds.show() calls:  %lu\n", ledShowCallCount );
                Serial.printf( "  Average time:       %lu us\n", ledShowTotalTime / ledShowCallCount );
                Serial.printf( "  Min time:           %lu us\n", ledShowMinTime );
                Serial.printf( "  Max time:           %lu us\n", ledShowMaxTime );
                Serial.printf( "  Total LED time:     %.2f ms\n", ledShowTotalTime / 1000.0f );
                Serial.printf( "  Core2 iterations:   %lu\n", core2LoopIterations );
                Serial.printf( "  LED refresh rate:   %.1f Hz\n",
                               ledShowCallCount * 1000.0f / timingPrintInterval );
                Serial.println( "=================================\n" );
                Serial.flush( );

                // Reset counters
                ledShowCallCount = 0;
                ledShowTotalTime = 0;
                ledShowMinTime = 999999;
                ledShowMaxTime = 0;
                core2LoopIterations = 0;
            }
        }
    }
}// force rebuild
