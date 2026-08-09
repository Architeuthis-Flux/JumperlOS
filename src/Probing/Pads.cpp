// SPDX-License-Identifier: MIT
//
// Pads.cpp - probe engine v2 (see Pads.h).
//
// GENERATED-ASSEMBLED FILE LAYOUT: the session pipeline, v2 decode readers,
// and dispatchers below are v2-authored; the encoder-cursor navigation and
// the SF-menu/chooser section further down are copied from the legacy
// engine (Probing.cpp) with the class renamed, so their behavior - and any
// fix applied there - should be kept in sync manually until the legacy
// engine is retired.

#include "Pads.h"

#include "../CH446Q.h"
#include "../Commands.h"
#include "../FileParsing.h"
#include "../Graphics.h"
#include "../JumperlessDefines.h"
#include "../LEDs.h"
#include "../MatrixState.h"
#include "../Menus.h"
#include "../NetManager.h"
#include "../NetsToChipConnections.h"
#include "../Peripherals.h"
#include "../PersistentStuff.h"
#include "../RotaryEncoder.h"
#include "../States.h"
#include "../Highlighting.h"
#include "../oled.h"
#include "../configManager.h"
#include "PadDecode.h"
#include "ProbeManager.h"

// ============================================================================
// Engine-internal state. The legacy engine has file-scope twins of most of
// these in Probing.cpp; they are engine-private (nothing outside the probing
// TU reads them), so the v2 engine keeps its own copies with internal
// linkage. State that IS shared across the firmware (switchPosition,
// connectOrClearProbe, probeRowMap, showProbeLEDs, ...) comes in through
// the reference aliases in Probing.h and is NOT duplicated here.
// ============================================================================

static int lastReadRaw = 0;

static unsigned long probeTimeout = 0;

static int connectedRowsIndex = 0;
static int connectedRows[ 32 ] = { -1 };

static int nodesToConnect[ 2 ] = { -1, -1 };

static unsigned long probeButtonTimer = 0;

static int voltageSelection = TOP_RAIL;

static int rainbowList[ 13 ][ 3 ] = {
    { 40, 50, 80 }, { 88, 33, 70 }, { 30, 15, 45 }, { 8, 27, 45 }, { 45, 18, 19 }, { 35, 42, 5 }, { 02, 45, 35 }, { 18, 25, 45 }, { 40, 12, 45 }, { 10, 32, 45 }, { 18, 5, 43 }, { 45, 28, 13 }, { 8, 12, 8 } };

static int checkingPads = 0;

// Special function option colors - used in encoder cursor and probe menus
static uint32_t sfOptionColors[ 12 ] = {
    0x09000a,
    0x0d0500,
    0x000809,
    0x040f00,
    0x000f03,
    0x00030d,
    0x080a00,
    0x030013,
    0x000a03,
    0x00030a,
    0x040010,
    0x070006,
};

static uint32_t deleteFade[ 13 ] = { 0x371f16, 0x28160b, 0x191307, 0x141005, 0x0f0901,
                                     0x090300, 0x050200, 0x030100, 0x030000, 0x020000,
                                     0x010000, 0x000000, 0x000000 };

static int fadeIndex = 0;

static unsigned long idleTime = millis( );
static unsigned long idleSaveTime = 3000;

static unsigned long doubleTimeout = 0;

// checkPads() state
static unsigned long padTimeout = 0;
static int lastPadTouched = 0;
static unsigned long padNoTouch = 0;
static int lastPadTouchedTime = 0;
static int samePadCount = 0;

// justReadProbe() confirmation state
static int lastProbeRead = 0;
static int lastRowProbed = -1;
static unsigned long lastDuplicateTime = 0;

// probe-mode session persistents (survive across sessions)
static int persistentEncoderCursorNode = -1;
static int persistentCursorZone = 0; // ZONE_BREADBOARD
static int persistentSubIndex = 0;
static bool firstProbeEntry = true;

// external shared hooks (same externs the legacy engine uses)
extern unsigned long startupTimers[];
extern int probeToggle( int buttonState );   // Peripherals.cpp
int probeGpioPowerClaimIdx( void );          // Probing.cpp (GPIO buffer-power claim)

#define SPACE_FROM_LEFT 6 // node-name column indent (matches the legacy engine)

// ============================================================================
// Logo pad assignment helpers (same logic as the legacy engine)
// ============================================================================

static int resolveLogoPadAssignment( int configValue, int defaultNode ) {
    switch ( configValue ) {
    case 0:
        return RP_UART_TX;
    case 1:
        return RP_UART_RX;
    case 2:
        return RP_GPIO_18;
    case 3:
        return RP_GPIO_19;
    default:
        if ( configValue > 0 ) {
            return configValue;
        }
        return defaultNode;
    }
}

static int nodeToLogoPadConfig( int node, int fallbackConfig ) {
    switch ( node ) {
    case RP_UART_TX:
        return 0;
    case RP_UART_RX:
        return 1;
    case RP_GPIO_18:
        return 2;
    case RP_GPIO_19:
        return 3;
    default:
        if ( node > 0 ) {
            return node;
        }
        return fallbackConfig;
    }
}

// Wait for core 2 to finish a blocking display write before OLED/LED text
// swaps (same contract as the legacy helper).
static bool waitForBlockingDisplay( uint32_t timeoutMs = 100 ) {
    unsigned long start = millis( );
    while ( core2busy == true ) {
        if ( millis( ) - start > timeoutMs ) {
            return false;
        }
        tight_loop_contents( );
    }
    return true;
}

// ============================================================================
// Singleton + dispatch plumbing
// ============================================================================

Pads* Pads::instance = nullptr;

Pads& Pads::getInstance( ) {
    if ( instance == nullptr ) {
        instance = new Pads( );
    }
    return *instance;
}

Pads& pads = Pads::getInstance( );

bool probeEngineV2Active( ) {
#if defined(OG_JUMPERLESS)
    // OG has no ADC7 / measure buffer and a different sense frontend; it
    // stays on the legacy engine unconditionally.
    return false;
#else
    return jumperlessConfig.debug.probe_engine_v2;
#endif
}

int padsProbeMode( int setOrClear, int firstConnection, bool fromClickMenu ) {
    pads.beginSession( setOrClear, firstConnection, fromClickMenu );
    return 1;
}

int padsJustReadProbe( bool allowDuplicates, int rawPad ) {
    return pads.justReadProbe( allowDuplicates, rawPad );
}

int padsReadProbeRaw( int readNothingTouched, bool allowDuplicates ) {
    return pads.readProbeRaw( readNothingTouched, allowDuplicates );
}

int padsReadProbe( ) {
    return pads.readProbe( );
}

void padsCheckPads( ) {
    pads.checkPads( );
}

int padsGetNothingTouched( int samples ) {
    return pads.getNothingTouched( samples );
}

int padsLastProbeReading( ) {
    return pads.getLastProbeReading( );
}

// ---- jOS dispatcher services ------------------------------------------------

ProbeEngineService* ProbeEngineService::instance = nullptr;
ProbeEngineService& ProbeEngineService::getInstance( ) {
    if ( instance == nullptr )
        instance = new ProbeEngineService( );
    return *instance;
}
ProbeEngineService& probeEngineService = ProbeEngineService::getInstance( );

ServiceStatus ProbeEngineService::service( ) {
    if ( probeEngineV2Active( ) ) {
        return pads.service( );
    }
    return Probing::getInstance( ).service( );
}

ProbeSwitchGate* ProbeSwitchGate::instance = nullptr;
ProbeSwitchGate& ProbeSwitchGate::getInstance( ) {
    if ( instance == nullptr )
        instance = new ProbeSwitchGate( );
    return *instance;
}
ProbeSwitchGate& probeSwitchGate = ProbeSwitchGate::getInstance( );

ServiceStatus ProbeSwitchGate::service( ) {
    // Parity gate: the legacy blocking probeMode implicitly suspended the
    // switch check for the whole session (only serviceCritical ran), and
    // checkSwitchPosition's DAC-swap fallback can glitch the tip mid-read.
    if ( probeActive ) {
        return ServiceStatus::IDLE;
    }
    return probeSwitch.service( );
}

ProbePadsGate* ProbePadsGate::instance = nullptr;
ProbePadsGate& ProbePadsGate::getInstance( ) {
    if ( instance == nullptr )
        instance = new ProbePadsGate( );
    return *instance;
}
ProbePadsGate& probePadsGate = ProbePadsGate::getInstance( );

ServiceStatus ProbePadsGate::service( ) {
    if ( probeActive ) {
        return ServiceStatus::IDLE; // session owns the probe (parity gate)
    }
    if ( !probeEngineV2Active( ) ) {
        return probePads.service( );
    }
    // v2: same 20Hz rate limit as the legacy ProbePads service
    unsigned long now = millis( );
    if ( now - lastCheckTime < 50 ) {
        return ServiceStatus::IDLE;
    }
    lastCheckTime = now;
    pads.checkPads( );
    return ServiceStatus::BUSY;
}

// ============================================================================
// Pads service: idle probe read cache + button handling + session advance
// ============================================================================

ServiceStatus Pads::service( ) {
    // Probe sub-menu open (SF menu / pad menu): those flows own the probe.
    if ( sfProbeMenu != 0 || inPadMenu != 0 ) {
        return ServiceStatus::BLOCKING;
    }

    if ( session.active ) {
        sessionPass( );
        return ServiceStatus::BUSY;
    }

    ServiceStatus status = ServiceStatus::IDLE;

    // Idle-mode probe read at 100Hz: feeds Highlighting / MeasureMode via
    // getLastProbeReading(), same contract as the legacy service.
    static unsigned long lastProbeCheckTime = 0;
    unsigned long now = millis( );
    if ( now - lastProbeCheckTime >= 10 ) {
        lastProbeCheckTime = now;
        int probeReading = justReadProbe( true );
        lastProbeReading = probeReading;
        if ( probeReading > 0 ) {
            status = ServiceStatus::BUSY;
        }
        // Auto-trim persistence: only marks the config dirty once the
        // probe has been quiet for a while, so the flash write can never
        // land mid-probing.
        PadDecode::persistTrimsIfQuiet( );
    }

    handleProbeButtonActions( );

    return status;
}

// v2 twin of the legacy handleProbeButtonActions: identical toggle /
// warning logic, but presses START a session instead of calling into a
// blocking loop. Highlight cleanup happens in endSession.
void Pads::handleProbeButtonActions( ) {
    if ( blockProbeButton > 0 && ( millis( ) - blockProbeButtonTimer < blockProbeButton ) ) {
        return; // blocked: don't consume events
    }

    int buttonPress = probeButton.getButtonPress( );

    if ( brightenedNet > 0 ) {
        int probeToggleResult = probeToggle( buttonPress );
        if ( probeToggleResult >= 0 && brightenedNet > 0 ) {
            blockProbeButton = gpioToggleFrequency;
            blockProbeButtonTimer = millis( );
            return;
        } else if ( probeToggleResult == -4 ) {
            highlighting.clearHighlighting( );
            blockProbeButton = 800;
            blockProbeButtonTimer = millis( );
            return;
        } else if ( probeToggleResult == -5 ) {
            if ( brightenedNode > 0 ) {
                if ( warningNet == brightenedNet && warningTimeout > 0 ) {
                    // Second press: clear the warned net via a preloaded
                    // single-shot clear session.
                    warningTimeout = 0;
                    connectOrClearProbe = 0;
                    showProbeLEDs = 2;
                    probingTimer = millis( );
                    startupTimers[ 0 ] = millis( );
                    beginSession( 0, brightenedNode );
                    return;
                } else {
                    highlighting.warnNet( brightenedNode );
                    warningTimeout = 1500;
                    warningTimer = millis( );
                    blockProbeButton = 400;
                    blockProbeButtonTimer = millis( );
                    return;
                }
            }
            blockProbeButton = 800;
            blockProbeButtonTimer = millis( );
        } else if ( probeToggleResult == -3 || probeToggleResult == -2 ) {
            blockProbeButton = 800;
            blockProbeButtonTimer = millis( );
        }
    } else {
        firstConnection = -1; // Highlighting's hovered-node latch
    }

    if ( buttonPress != 0 ) {
        if ( buttonPress == 2 ) {
            connectOrClearProbe = 1;
            showProbeLEDs = 1;
            probingTimer = millis( );
            brightenedNet = 0;
            blockProbeButtonTimer = millis( );
            blockProbeButton = 1000;
            beginSession( 1, firstConnection );
        } else if ( buttonPress == 1 ) {
            startupTimers[ 0 ] = millis( );
            connectOrClearProbe = 0;
            showProbeLEDs = 2;
            probingTimer = millis( );
            brightenedNet = 0;
            blockProbeButtonTimer = millis( );
            blockProbeButton = 1000;
            beginSession( 0, firstConnection );
        }
    }
}

// ============================================================================
// Probe-mode session
// ============================================================================

void Pads::emitBanner( ) {
    if ( session.bannerEmitted )
        return;
    session.bannerEmitted = true;
    if ( session.setOrClear == 1 ) {
        changeTerminalColor( 45 );
        Serial.println( "\n\r\t connect nodes\n\r" );
        Serial.flush( );
        changeTerminalColor( -1 );
    } else {
        changeTerminalColor( 202 );
        Serial.println( "\n\r\t clear nodes\n\r" );
        Serial.flush( );
        changeTerminalColor( -1 );
    }
}

void Pads::beginSession( int setOrClear, int firstConnection, bool fromClickMenu ) {
    if ( session.active ) {
        // Shouldn't happen (button events are blocked during sessions),
        // but never stack sessions - tear the old one down first.
        endSession( SessionEnd::External );
    }

    g_probeDoubleTapBail = false;

    blockProbeButton = 3000;
    blockProbeButtonTimer = millis( );
    probeButton.clearButtonState( );

    session = ProbeSession( );
    session.active = true;
    session.setOrClear = setOrClear;
    session.firstConnection = firstConnection;
    session.fromClickMenu = fromClickMenu;
    session.outerEntryTime = millis( );
    session.wasHeld = oled.oledIsHeld( );
    for ( int i = 0; i < 20; i++ )
        session.deleteMisses[ i ] = -1;

    // encoder cursor: restore persistent position (or defaults on the very
    // first session since boot)
    session.encoderCursorNode = firstProbeEntry ? 14 : persistentEncoderCursorNode;
    session.cursorZone = firstProbeEntry ? 0 : persistentCursorZone;
    session.subIndex = firstProbeEntry ? 0 : persistentSubIndex;
    firstProbeEntry = false;
    session.lastEncoderPosition = encoderPosition;
    session.lastEncoderMovement = millis( );
    session.encoderAccel = EncoderAccelerator::Slow( );
    session.savedRotaryDivider = rotaryDivider;
    rotaryDivider = 3;

    routableBufferPower( 1, 1 );

    sessionRestart( );
}

// The restartProbing / restartProbingNoPrint entry rendering: run on session
// begin and again on connect<->clear mode switches (replaces the gotos).
void Pads::sessionRestart( ) {
    probeActive = 1;
    // A NetVoltageScan sense tap checks probeActive before starting but can
    // be mid-flight right now - wait it out so probe raw crosspoint sends
    // never overlap a tap's PIO handshake from the other core.
    waitCore2( );
    brightenNet( -1 );

    rawOtherColors[ 1 ] = ( session.setOrClear == 1 ) ? 0x4500e8 : 0x6644A8;

    // First-entry banner is deferred until the double-tap window has passed
    // (see sessionPass); mode-switch restarts print immediately.
    if ( !session.firstEntry ) {
        emitBanner( );
    }

    if ( session.setOrClear == 1 && session.firstConnection == -1 ) {
        oled.clearPrintShow( "connect", 2, true, true, true );
    } else if ( session.setOrClear == 0 && session.firstConnection == -1 ) {
        oled.clearPrintShow( "clear", 2, true, true, true );
    }

    clearColorOverrides( 1, 1, 0 );

    probeHighlight = -1;
    session.numberOfLocalChanges = 0;

    connectOrClearProbe = session.setOrClear;

    session.row0 = -2;
    session.row1 = -2;

    probeTimeout = millis( );

    if ( session.setOrClear == 0 ) {
        probeButtonTimer = millis( );
    }

    showProbeLEDs = ( session.setOrClear == 1 ) ? 1 : 2;

    showLEDsCore2 = 1;
    session.doubleSelectTimeout = millis( );
    session.doubleSelectCountdown = 0;
    session.lastProbedRows[ 0 ] = 0;
    session.lastProbedRows[ 1 ] = 0;
    session.lastProbedRows[ 2 ] = 0;
    session.lastProbedRows[ 3 ] = 0;
    session.fadeTimer = millis( );
    session.fadeClear = -1;

    blockProbeButton = 1000;
    blockProbeButtonTimer = millis( );

    session.probeModeStartTime = millis( );
}

void Pads::endSession( SessionEnd reason ) {
    if ( !session.active )
        return;

    node1or2 = 0;
    nodesToConnect[ 0 ] = -1;
    nodesToConnect[ 1 ] = -1;
    connectedRowsIndex = 0;
    connectedRows[ 0 ] = -1;
    probeActive = false;
    probeHighlight = -1;
    brightenNet( -1 );

    if ( session.connectionsThisSession == 0 && session.bannerEmitted ) {
        // Only rewind 3 lines if the banner was actually printed - if we
        // bailed on a double-tap before the banner fired, those lines
        // belong to whatever was on screen before the session.
        Serial.print( "\x1b[3A\x1b[0J" );
    }
    Serial.flush( );

    // Dirty state is picked up by the auto-save scheduler within ~1s.
    session.numberOfLocalChanges = 0;

    oled.showJogo32h( );

    rotaryDivider = session.savedRotaryDivider;

    // Clear any visible cursor LED based on zone (0 breadboard, 1 nano,
    // >=2 special function zones)
    if ( session.cursorZone == 0 && session.encoderCursorNode >= 0 ) {
        b.printRawRow( 0b00000100, session.encoderCursorNode, 0x000000, 0x000000 );
    } else if ( session.cursorZone == 1 && session.encoderCursorNode >= 0 ) {
        int pixel = getNanoHeaderPixel( session.encoderCursorNode );
        if ( pixel >= 0 )
            leds.setPixelColor( pixel, 0x000000 );
    } else if ( session.cursorZone >= 2 ) {
        clearLEDsExceptRails( );
    }

    globalEncoderCursorNode = -1;
    globalEncoderCursorInHeader = 0;
    Highlighting::getInstance( ).clearHighlighting( );

    inPadMenu = 0;

    // Next session starts at the default cursor position
    firstProbeEntry = true;

    showLEDsCore2 = 2;

    probeButton.clearButtonState( );
    blockProbeButton = 1000;
    blockProbeButtonTimer = millis( );
    clearColorOverrides( 1, 1, 0 );

    if ( switchPosition == 1 ) {
        showProbeLEDs = 4;
    } else {
        showProbeLEDs = 3;
    }

    bool openMenu = session.exitToClickMenu;
    session.active = false;
    (void)reason;

    if ( openMenu ) {
        // The wheel click that ended probing now opens the click menu -
        // after full exit cleanup so the menu starts from idle state. The
        // real click event is long gone, so re-arm a synthetic one.
        synthesizeEncoderClick( );
        Menus::getInstance( ).clickMenu( );
    }
}

// One pipeline pass, called from service() while a session is active.
// This is the legacy probeMode loop body: poll input -> resolve gestures ->
// pair/commit nodes -> render, with the blocking-loop plumbing (manual
// service pumping, OLED hold polling for other services, goto restarts)
// removed - the main loop's serviceAll() runs everything else between
// passes, and probeActive gates the services that must not react to probe
// input while a session owns it.
void Pads::sessionPass( ) {
    // The terminal owns input: any typed character ends the session (the
    // main loop then processes it normally, same visible behavior as the
    // legacy exit-on-Serial.available()).
    if ( Serial.available( ) != 0 ) {
        endSession( SessionEnd::SerialInput );
        return;
    }

    // Hold-end repaint: when an undo-toast hold releases, put the session
    // banner back on the OLED (OLEDService is probeActive-gated, so the
    // session still owns this repaint).
    bool isHeld = oled.oledIsHeld( );
    if ( session.wasHeld && !isHeld && session.firstConnection == -1 ) {
        if ( session.setOrClear == 1 ) {
            oled.clearPrintShow( "connect", 2, true, true, true );
        } else {
            oled.clearPrintShow( "clear", 2, true, true, true );
        }
    }
    session.wasHeld = isHeld;

    // Double-tap fast-return / stay-alive cutoff. service() (ProbeButton,
    // CRITICAL) sets g_probeDoubleTapBail when a double-tap fires undo/redo.
    if ( g_probeDoubleTapBail ) {
        g_probeDoubleTapBail = false;
        // Cancel any in-flight pending press - the double-tap means click 1
        // was the first half of an undo gesture, not a real action.
        session.pendingInProbeButton = 0;
        if ( ( millis( ) - session.outerEntryTime ) < ProbingDoubleTap::kWindowMs ) {
            blockProbeButton = ProbingDoubleTap::kWindowMs;
            blockProbeButtonTimer = millis( );
            endSession( SessionEnd::DoubleTapBail );
            return;
        }
        // else: past the entry window - the undo already fired, stay alive.
    }

    bool pendingCommitting = false;

    // First-entry banner: emit once the double-tap entry window has passed
    // without a fast-return.
    if ( session.firstEntry && !session.bannerEmitted &&
         ( millis( ) - session.outerEntryTime ) >= ProbingDoubleTap::kWindowMs ) {
        emitBanner( );
    }

    connectedRowsIndex = 0;

    // ======= encoder-based node selection with special function zones ====
    int rowShim[ 16 ] = { 0 };
    rowShim[ 0 ] = session.row0;
    rowShim[ 1 ] = session.row1;
    handleEncoderCursorNavigation(
        session.setOrClear,
        node1or2,
        nodesToConnect,
        connectOrClearProbe,
        session.probeModeStartTime,
        session.lastEncoderPosition,
        session.encoderAccumulator,
        session.encoderCursorNode,
        session.cursorZone,
        session.subIndex,
        session.lastEncoderCursorNode,
        session.lastCursorZone,
        session.lastSubIndex,
        session.lastEncoderMovement,
        session.encoderCursorVisible,
        persistentEncoderCursorNode,
        persistentCursorZone,
        persistentSubIndex,
        rowShim,
        connectedRows,
        connectedRowsIndex,
        session.encoderAccel,
        5000,
        !session.fromClickMenu );
    session.row0 = rowShim[ 0 ];
    session.row1 = rowShim[ 1 ];

    // Encoder button HELD exits probing (visual cleanup already done by the
    // navigation function; clear the event here so the next consumer starts
    // fresh).
    if ( encoderButtonState == HELD ) {
        encoderButtonState = IDLE;
        lastButtonEncoderState = IDLE;
        endSession( SessionEnd::EncoderHeld );
        return;
    }

    // Short wheel click with no encoder cursor showing, in a session the
    // probe button started: the user wants the menu, not probing.
    if ( !session.fromClickMenu && !session.encoderCursorVisible &&
         encoderButtonState == RELEASED ) {
        encoderButtonState = IDLE;
        lastButtonEncoderState = IDLE;
        session.exitToClickMenu = true;
        endSession( SessionEnd::ExitToClickMenu );
        return;
    }

    // ======= physical probe / preloaded first connection ==================
    if ( session.firstConnection > 0 ) {
        session.row0 = session.firstConnection;
        connectedRows[ 0 ] = session.row0;
        connectedRowsIndex = 1;
        session.firstConnection = ( session.setOrClear == 0 ) ? -2 : -3;
    } else if ( session.row0 == -1 ) {
        session.row0 = readProbe( );
    }

    // Encoder events from readProbe are handled by the cursor logic above.
    if ( session.row0 == -19 || session.row0 == -17 || session.row0 == -10 ) {
        encoderDirectionState = NONE;
        session.row0 = -1;
        return;
    }

    // Resolve a deferred in-probe button press whose double-tap window has
    // elapsed without a cancel (only when no fresher input claimed row0).
    if ( session.row0 == -1 &&
         session.pendingInProbeButton != 0 &&
         ( millis( ) - session.pendingInProbeButtonTime ) >= ProbingDoubleTap::kWindowMs ) {
        session.row0 = session.pendingInProbeButton;
        session.pendingInProbeButton = 0;
        pendingCommitting = true;
    }

    if ( session.row0 != -1 ) {
        idleTime = millis( );
    }

    sessionIdleSave( );
    sessionFadeAnimation( );

    if ( session.row0 == -18 || session.row0 == -16 ) {
        if ( sessionHandleButton( pendingCommitting ) ) {
            return; // mode switch restarted rendering, or session ended
        }
    }

    if ( isConnectable( session.row0 ) == false && session.row0 != -1 ) {
        session.row0 = -1;
    }

    sessionHandleRow( );

    // Double-select window bookkeeping (allows re-selecting the same row
    // again after 700ms of no contact).
    if ( millis( ) - session.doubleSelectTimeout > 700 ) {
        session.row1 = -2;
        lastReadRaw = 0;
        session.lastProbedRows[ 0 ] = 0;
        session.lastProbedRows[ 1 ] = 0;
        session.doubleSelectTimeout = millis( );
        session.doubleSelectCountdown = 700;
    }

    if ( session.doubleSelectCountdown <= 0 ) {
        session.doubleSelectCountdown = 0;
    } else {
        session.doubleSelectCountdown =
            session.doubleSelectCountdown - ( millis( ) - session.doubleSelectTimeout );
        session.doubleSelectTimeout = millis( );
    }

    probeTimeout = millis( );

    // Single-shot clear (preloaded node) finishes after its first pass.
    if ( session.firstConnection == -2 ) {
        session.firstConnection = -1;
        endSession( SessionEnd::SingleShotDone );
        return;
    }

    // row0 carries into the next pass on purpose (the encoder-navigation
    // stage resets it to -1 whenever no encoder selection is active - the
    // same mechanism the legacy loop used).
}

// Save local node file if idle for 3 seconds (parity with the legacy loop;
// SlotManager's own service is probeActive-gated, so the session drives it).
void Pads::sessionIdleSave( ) {
    if ( millis( ) - idleTime > idleSaveTime ) {
        idleTime = millis( );
        if ( globalState.isDirty( ) && session.numberOfLocalChanges > 0 ) {
            SlotManager::getInstance( ).service( );
            if ( !globalState.isDirty( ) ) {
                session.numberOfLocalChanges = 0;
            }
        }
    }
}

// Remove-mode fade animation for recently-cleared rows (verbatim behavior).
void Pads::sessionFadeAnimation( ) {
    if ( session.setOrClear == 1 ) {
        session.deleteMissesIndex = 0;
        if ( millis( ) - session.fadeTimer > 500 ) {
            session.fadeTimer = millis( );
        }
    } else {
        if ( millis( ) - session.fadeTimer > 12 ) {
            session.fadeTimer = millis( );

            if ( fadeIndex < 12 ) {
                fadeIndex++;
                if ( removeFade > 0 ) {
                    removeFade--;
                    showProbeLEDs = 2;
                }
            } else {
                session.deleteMissesIndex = 0;
                for ( int i = 0; i < 20; i++ ) {
                    session.deleteMisses[ i ] = -1;
                }
            }

            int fadeFloor = fadeIndex;
            if ( fadeFloor < 0 ) {
                fadeFloor = 0;
            }

            for ( int i = session.deleteMissesIndex - 1; i >= 0; i-- ) {
                int fadeOffset = map( i, 0, session.deleteMissesIndex, 0, 12 ) + fadeFloor;
                if ( fadeOffset > 12 ) {
                    fadeOffset = 12;
                }
                b.printRawRow( 0b00000100, session.deleteMisses[ i ] - 1, deleteFade[ fadeOffset ],
                               0xfffffe );
            }

            if ( session.deleteMissesIndex == 0 && session.fadeClear == 0 ) {
                session.fadeClear = 1;
            }
        }
    }
}

// Probe button press (-18 clear / -16 connect) handling: mode switches,
// selection resets, and the double-tap deferral. Returns true when this
// pass is fully consumed (mode restart or session end).
bool Pads::sessionHandleButton( bool pendingCommitting ) {
    // Defer fresh in-probe presses by kWindowMs so a double-tap can cancel
    // them BEFORE they take effect. pendingCommitting is true only when the
    // resolver injected this press after the window elapsed.
    if ( !pendingCommitting ) {
        if ( session.pendingInProbeButton != 0 && session.pendingInProbeButton != session.row0 ) {
            // Cross-button case: commit the old pending now, defer the new.
            int newPress = session.row0;
            session.row0 = session.pendingInProbeButton;
            session.pendingInProbeButton = newPress;
            session.pendingInProbeButtonTime = millis( );
            // fall through with row0 = old pending
        } else {
            session.pendingInProbeButton = session.row0;
            session.pendingInProbeButtonTime = millis( );
            session.row0 = -1;
            return true; // wait out the window
        }
    }

    if ( session.row0 == -18 ) { // clear button
        if ( session.setOrClear == 0 ) {
            // Already in clear mode: reset the selection state, then fall
            // through to the commit/exit tail below - a same-mode press is
            // the "done probing" gesture (legacy falls through to break).
            nodesToConnect[ 0 ] = -1;
            nodesToConnect[ 1 ] = -1;
            node1or2 = 0;
            blockProbeButton = 8000;
            blockProbeButtonTimer = millis( );
            probeHighlight = -1;
            showLEDsCore2 = -1;
            Serial.print( "\x1b[2K\r" );
            Serial.flush( );
        } else {
            // Switch to clear mode (was goto restartProbing).
            session.setOrClear = 0;
            probingTimer = millis( );
            blockProbeButton = 8000;
            blockProbeButtonTimer = millis( );
            probeButtonTimer = millis( );
            sfProbeMenu = 0;
            connectedRowsIndex = 0;
            connectedRows[ 0 ] = -1;
            connectedRows[ 1 ] = -1;
            nodesToConnect[ 0 ] = -1;
            nodesToConnect[ 1 ] = -1;
            showLEDsCore2 = 1;
            node1or2 = 0;
            if ( session.connectionsThisSession == 0 && session.bannerEmitted ) {
                Serial.print( "\x1b[3A\x1b[0J" ); // rewind the stale banner
                Serial.flush( );
            }
            session.firstEntry = false;
            session.bannerEmitted = false; // re-print banner for the new mode
            session.connectionsThisSession = 0;
            sessionRestart( );
            session.row0 = -1;
            return true;
        }
    }

    if ( session.row0 == -16 ) { // connect button
        if ( session.setOrClear == 1 ) {
            if ( node1or2 == 1 ) {
                // First node was picked: a connect press cancels it.
                connectedRowsIndex = 0;
                nodesToConnect[ 0 ] = -1;
                nodesToConnect[ 1 ] = -1;
                node1or2 = 0;
                blockProbeButton = 8000;
                blockProbeButtonTimer = millis( );
                probeHighlight = -1;
                showLEDsCore2 = -2;
                Serial.print( "\x1b[2K\r" );
                Serial.flush( );
                // was goto restartProbingNoPrint: re-run entry rendering
                // without a fresh banner
                sessionRestart( );
                session.row0 = -1;
                return true;
            }
            // No node picked: same-mode press falls through to commit/exit.
        } else {
            // Switch to connect mode (was goto restartProbing).
            session.setOrClear = 1;
            probingTimer = millis( );
            blockProbeButton = 8000;
            blockProbeButtonTimer = millis( );
            probeButtonTimer = millis( );
            sfProbeMenu = 0;
            connectedRowsIndex = 0;
            connectedRows[ 0 ] = -1;
            connectedRows[ 1 ] = -1;
            nodesToConnect[ 0 ] = -1;
            nodesToConnect[ 1 ] = -1;
            node1or2 = 0;
            if ( session.connectionsThisSession == 0 && session.bannerEmitted ) {
                Serial.print( "\x1b[3A\x1b[0J" );
                Serial.flush( );
            } else {
                Serial.print( "\x1b[2K\r" );
                Serial.flush( );
            }
            session.connectionsThisSession = 0;
            session.firstEntry = false;
            session.bannerEmitted = false;
            sessionRestart( );
            session.row0 = -1;
            return true;
        }
    }

    // Same-mode press with nothing pending: commit and exit the session.
    session.row1 = -2;
    probingTimer = millis( );
    connectedRowsIndex = 0;
    node1or2 = 0;
    nodesToConnect[ 0 ] = -1;
    nodesToConnect[ 1 ] = -1;
    probeHighlight = -1;
    endSession( SessionEnd::ButtonCommit );
    return true;
}

// Node pairing and connect/clear commits (the heart of the legacy loop).
void Pads::sessionHandleRow( ) {
    if ( session.row0 == -1 || session.row0 == session.row1 ) {
        return;
    }

    session.lastProbedRows[ 1 ] = session.lastProbedRows[ 0 ];
    session.lastProbedRows[ 0 ] = session.row0;

    if ( connectedRowsIndex == 1 ) {
        nodesToConnect[ node1or2 ] = connectedRows[ 0 ];

        char node1Name[ 12 ];
        strcpy( node1Name, definesToChar( nodesToConnect[ 0 ] ) );
        char node2Name[ 12 ];
        strcpy( node2Name, "   " );

        char bothNames[ 25 ];
        strcpy( bothNames, node1Name );
        strcat( bothNames, " - " );
        strcat( bothNames, node2Name );

        Serial.print( "\r                   \r" );
        int numChars = strlen( node1Name );
        for ( int i = 0; i < SPACE_FROM_LEFT - numChars; i++ ) {
            Serial.print( " " );
        }
        Serial.print( node1Name );
        if ( session.setOrClear == 1 ) {
            Serial.print( "  -  " );
        }
        Serial.flush( );

        probeHighlight = nodesToConnect[ node1or2 ];
        if ( session.setOrClear == 1 ) {
            brightenNet( probeHighlight, 5 );
            oled.clearPrintShow( bothNames, 2, true, true, true );
        }

        if ( nodesToConnect[ node1or2 ] > 0 &&
             nodesToConnect[ node1or2 ] <= NANO_RESET_1 && session.setOrClear == 1 ) {
            // First-node confirmation flash
            showProbeLEDs = 11;
            b.printRawRow( 0b0010001, nodesToConnect[ node1or2 ] - 1, 0x000121e, 0xfffffe );
            showLEDsCore2 = 2;
            delay( 40 );
            b.printRawRow( 0b00001010, nodesToConnect[ node1or2 ] - 1, 0x0f0498, 0xfffffe );
            showLEDsCore2 = 2;
            delay( 40 );
            b.printRawRow( 0b00000100, nodesToConnect[ node1or2 ] - 1, 0x4000e8, 0xfffffe );
            showLEDsCore2 = 2;
            delay( 60 );
            showLEDsCore2 = 2;
        }

        node1or2++;
        probingTimer = millis( );
        session.doubleSelectTimeout = millis( );
        session.doubleSelectCountdown = 200;
    }

    if ( node1or2 >= 2 || ( session.setOrClear == 0 && node1or2 >= 1 ) ) {
        probeHighlight = -1;

        if ( session.setOrClear == 1 && ( nodesToConnect[ 0 ] != nodesToConnect[ 1 ] ) &&
             nodesToConnect[ 0 ] > 0 && nodesToConnect[ 1 ] > 0 ) {
            Serial.print( "\r              \r" );
            Serial.flush( );
            char node1Name[ 12 ];
            strcpy( node1Name, definesToChar( nodesToConnect[ 0 ] ) );
            char node2Name[ 12 ];
            strcpy( node2Name, definesToChar( nodesToConnect[ 1 ] ) );
            char bothNames[ 25 ];
            strcpy( bothNames, node1Name );
            strcat( bothNames, " - " );
            strcat( bothNames, node2Name );

            if ( connectionAllowed( nodesToConnect[ 0 ], nodesToConnect[ 1 ] ) == false ) {
                Serial.print( " can't connect " );
                Serial.print( node1Name );
                Serial.print( " to " );
                Serial.println( node2Name );
                Serial.flush( );
                node1or2 = 0;
                nodesToConnect[ 0 ] = -1;
                nodesToConnect[ 1 ] = -1;
                return;
            }

            int numChars = strlen( node1Name );
            for ( int i = 0; i < SPACE_FROM_LEFT - numChars; i++ ) {
                Serial.print( " " );
            }
            Serial.print( node1Name );
            Serial.print( "  -  " );
            numChars = Serial.print( node2Name );

            // GPIO direction note for the second node
            if ( nodesToConnect[ 1 ] >= RP_GPIO_1 && nodesToConnect[ 1 ] <= RP_GPIO_8 ) {
                int gpioIndex = -1;
                for ( int i = 0; i < 10; i++ ) {
                    if ( gpioDef[ i ][ 1 ] == nodesToConnect[ 1 ] ) {
                        gpioIndex = i;
                        break;
                    }
                }
                if ( gpioIndex != -1 ) {
                    if ( globalState.config.gpioDirection[ gpioIndex ] == 0 ) {
                        Serial.print( " (output)  connected" );
                    } else {
                        Serial.print( " (input)  connected" );
                    }
                }
                Serial.flush( );
            } else {
                Serial.print( "     \tconnected\n\r" );
                Serial.flush( );
            }

            if ( session.firstConnection == -3 ) {
                // Single-shot connect: commit and end.
                addBridgeToState( nodesToConnect[ 0 ], nodesToConnect[ 1 ], -1, true );
                session.numberOfLocalChanges++;
                session.connectionsThisSession++;
                endSession( SessionEnd::SingleShotDone );
                return;
            }

            addBridgeToState( nodesToConnect[ 0 ], nodesToConnect[ 1 ], -1, true );
            showProbeLEDs = 1;
            session.numberOfLocalChanges++;
            session.connectionsThisSession++;
            brightenNet( -1 );

            oled.clearPrintShow( bothNames, 2, true, true, true );
            session.fadeTimer = millis( );

            session.row1 = -1;
            for ( int i = 0; i < 12; i++ ) {
                session.deleteMisses[ i ] = -1;
            }
            session.doubleSelectTimeout = millis( );
            session.doubleSelectCountdown = 400;

        } else if ( session.setOrClear == 0 ) {
            char node1Name[ 28 ]; // node name + " cleared" suffix
            node1Name[ 0 ] = '\0';

            for ( int i = 12; i > 0; i-- ) {
                session.deleteMisses[ i ] = session.deleteMisses[ i - 1 ];
            }
            session.deleteMisses[ 0 ] = nodesToConnect[ 0 ];
            if ( session.deleteMissesIndex < 12 ) {
                session.deleteMissesIndex++;
            }
            fadeIndex = -3;

            for ( int i = session.deleteMissesIndex - 1; i >= 0; i-- ) {
                b.printRawRow( 0b00000100, session.deleteMisses[ i ] - 1,
                               deleteFade[ map( i, 0, session.deleteMissesIndex, 0, 12 ) ],
                               0xfffffe );
            }
            clearHighlighting( 0 );

            bool removed = removeBridgeFromState( nodesToConnect[ 0 ], -1, true );
            int rowsRemoved = removed ? lastRemovedNodesIndex : 0;
            if ( removed ) {
                session.numberOfLocalChanges += rowsRemoved;
            }

            if ( rowsRemoved > 0 ) {
                session.connectionsThisSession++;
                removeFade = 10;

                Serial.print( ", " );
                int charCount = 0;
                for ( int i = 0; i < lastRemovedNodesIndex; i++ ) {
                    charCount += printNodeOrName( disconnectedNode( ), 1 );
                    if ( i < lastRemovedNodesIndex - 1 ) {
                        Serial.print( ", " );
                        charCount += 2;
                    }
                }
                for ( int i = 0; i < 8 - charCount; i++ ) {
                    Serial.print( " " );
                }
                Serial.print( " cleared" );
                if ( lastRemovedNodesIndex > 0 ) {
                    Serial.print( " " );
                    Serial.print( lastRemovedNodesIndex + 1 );
                    Serial.print( " nodes" );
                }
                Serial.println( );
                Serial.flush( );

                sprintf( node1Name, "%s cleared", definesToChar( nodesToConnect[ 0 ] ) );
                oled.clearPrintShow( node1Name, 2, true, true, true );

                session.fadeClear = 0;
                session.fadeTimer = 0;
            } else {
                snprintf( node1Name, sizeof( node1Name ), "%s",
                          definesToChar( nodesToConnect[ 0 ] ) );
                oled.clearPrintShow( node1Name, 2, true, true, true );
            }
        }

        if ( session.firstConnection == -3 ) {
            endSession( SessionEnd::SingleShotDone );
            return;
        }

        node1or2 = 0;
        nodesToConnect[ 0 ] = -1;
        nodesToConnect[ 1 ] = -1;
        session.doubleSelectTimeout = millis( );
    }

    session.row1 = session.row0;
}

// ============================================================================
// v2 readers: burst read + phantom rejection + PadDecode physics decode
// ============================================================================

// Median of n probe burst averages (n <= 16). Rejects single glitch bursts
// better than a mean when the outer read count is raised.
static int medianProbeBursts( const int* v, int n ) {
    int tmp[ 16 ];
    for ( int i = 0; i < n; i++ )
        tmp[ i ] = v[ i ];
    for ( int i = 1; i < n; i++ ) {
        int key = tmp[ i ];
        int j = i - 1;
        while ( j >= 0 && tmp[ j ] > key ) {
            tmp[ j + 1 ] = tmp[ j ];
            j--;
        }
        tmp[ j + 1 ] = key;
    }
    if ( n & 1 )
        return tmp[ n / 2 ];
    return ( tmp[ n / 2 - 1 ] + tmp[ n / 2 ] + 1 ) / 2;
}

// Phantom-touch gate: blink the tip feed off for one short burst before
// accepting a changed reading - a real probe contact collapses to the dark
// floor, an injected reading (finger bridging a powered row onto a pad)
// persists and gets rejected. Same logic as the legacy engine; thresholds
// come from the floor tracker instead of probe_min.
static bool probeReadingIsPhantom( int average ) {
    int pin = -1;
#if defined(OG_JUMPERLESS)
    pin = PROBE_PIN;
#else
    if ( switchPosition != 0 ) {
        pin = PROBE_PIN; // select: tip fed directly from PROBE_PIN
    } else if ( probeGpioPowerClaimIdx( ) >= 0 ) {
        pin = gpioDef[ probeGpioPowerClaimIdx( ) ][ 0 ]; // measure: GPIO-powered buffer
    }
#endif
    if ( pin < 0 ) {
        return false; // DAC-powered measure buffer: nothing we can blink
    }

    // ponytail: a steady contact re-blinks at most every 250ms (a >5 count
    // move re-checks immediately), so the tip feed isn't chopped on every
    // accepted read. Ceiling: a phantom landing within 5 counts of a
    // just-verified reading rides the stale verdict for up to 250ms.
    static int lastCheckedValue = -1000;
    static unsigned long lastCheckedMs = 0;
    if ( millis( ) - lastCheckedMs < 250 && abs( average - lastCheckedValue ) <= 5 ) {
        return false;
    }

    gpio_put( pin, false );
    delayMicroseconds( 25 ); // sense node RC ~1us, buffer slew a few us
    int dark = readAdc( 5, 8 );
    gpio_put( pin, true );
    delayMicroseconds( 25 );

    if ( dark > PadDecode::floorCounts( ) + 10 ) {
        lastCheckedMs = 0; // never cache a rejection
        if ( debugProbing == 1 ) {
            Serial.printf( "phantom reading rejected: dark %d > %d, lit %d\n\r",
                           dark, PadDecode::floorCounts( ) + 10, average );
        }
        return true;
    }
    lastCheckedValue = average;
    lastCheckedMs = millis( );
    return false;
}

int Pads::readProbeRaw( int readNothingTouched, bool allowDuplicates ) {
    (void)readNothingTouched;

    int numberOfReads = 8;
    int lowReads = 0;
    int touchFloor = PadDecode::touchThresholdCounts( );

    int measurements[ 16 ] = { 0 };

    // Deeper bursts for the row-highlight path in measure position (its tip
    // feed comes through the crossbar: droopier and noisier than select's
    // direct PROBE_PIN feed).
    int burstSamples = 16;
    if ( connectOrClearProbe != 1 && checkingPads != 1 && switchPosition == 0 ) {
        burstSamples = 24;
    }

    for ( int i = 0; i < numberOfReads; i++ ) {
        measurements[ i ] = readAdc( 5, burstSamples );
        // Low-but-real readings (SF pad region) get a longer burst so the
        // median has more to work with.
        if ( measurements[ i ] < 300 && measurements[ i ] > ( touchFloor + 10 ) && i < 4 ) {
            lowReads++;
        }
        if ( lowReads > 2 ) {
            numberOfReads = 16;
        }
        delayMicroseconds( 5 );
    }

    int maxVariance = 0;
    for ( int i = 0; i < numberOfReads - 1; i++ ) {
        int variance = abs( measurements[ i ] - measurements[ i + 1 ] );
        if ( variance > maxVariance ) {
            maxVariance = variance;
        }
    }
    int average = medianProbeBursts( measurements, numberOfReads );

    // Feed the dark-floor tracker with every stable reading (it ignores
    // anything above the touch threshold internally).
    if ( maxVariance <= 6 ) {
        PadDecode::trackFloor( average );
    }

    if ( maxVariance <= 6 && ( ( abs( average - lastReadRaw ) > 5 ) || checkingPads == 1 ) &&
         ( average >= touchFloor ) ) {
        if ( probeReadingIsPhantom( average ) ) {
            return -1;
        }
        lastReadRaw = average;
        return average;
    }

    if ( allowDuplicates && ( average >= touchFloor ) && maxVariance <= 6 &&
         abs( average - lastReadRaw ) <= 10 ) {
        if ( probeReadingIsPhantom( average ) ) {
            return -1;
        }
        lastReadRaw = average;
        return average;
    }

    if ( ( abs( average - lastReadRaw ) < 2 ) && allowDuplicates && ( average >= touchFloor ) ) {
        if ( probeReadingIsPhantom( average ) ) {
            return -1;
        }
        return average;
    }

    return -1;
}

int Pads::smoothProbeReading( int probeRead, bool reset ) {
    if ( reset ) {
        smoothedProbeRead = -1;
    }

    if ( probeRead <= 0 ) {
        return probeRead;
    }

    // Damp ADC jitter WITHOUT chasing a contact-settle ramp: blend small
    // steps only and snap to anything larger (see the legacy banner).
    const int resetThreshold = 8;

    if ( smoothedProbeRead < 0 || abs( probeRead - smoothedProbeRead ) > resetThreshold ) {
        smoothedProbeRead = probeRead;
    } else {
        smoothedProbeRead = ( ( smoothedProbeRead * 5 ) + probeRead + 3 ) / 6;
    }

    return smoothedProbeRead;
}

int Pads::justReadProbe( bool allowDuplicates, int rawPad ) {
    // Call sequence number: the row-change confirmation below requires
    // STRICTLY consecutive reads (any intervening no-read call advances the
    // sequence and voids a pending row change).
    static uint32_t callSeq = 0;
    callSeq++;

    if ( blockProbing > 0 && ( millis( ) - blockProbingTimer < blockProbing ) ) {
        return -1;
    }
    if ( blockProbing > 0 ) {
        blockProbing = 0;
    }

    int probeRead = readProbeRaw( 0, allowDuplicates );
    if ( probeRead <= 0 ) {
        return -1;
    }

    int stableProbeRead = smoothProbeReading( probeRead );
    PadDecodeResult d = PadDecode::decode( stableProbeRead );
    int rowProbed = d.index;

    if ( rowProbed <= 0 || rowProbed > 101 ) {
        if ( debugProbing == 1 ) {
            Serial.printf( "pad decode rejected raw %d\n\r", stableProbeRead );
        }
        return -1;
    }

    if ( allowDuplicates ) {
        if ( probeRowMapByPad[ rowProbed ] == lastRowProbed ) {
            // Same row: rate-limit duplicates to one per 500ms.
            if ( millis( ) - lastDuplicateTime < 500 ) {
                if ( debugProbing == 1 ) {
                    Serial.printf( "Rejected duplicate row: %d\n\r", probeRowMap[ rowProbed ] );
                }
                return -1;
            }
            lastDuplicateTime = millis( );
            lastProbeRead = probeRead;
            PadDecode::noteConfidentDecode( d );
            return ( rawPad == 1 ) ? probeRowMapByPad[ rowProbed ] : probeRowMap[ rowProbed ];
        }
        // A DIFFERENT row must be seen on two CONSECUTIVE reads before it's
        // reported (rejects single noisy decodes into a neighboring band).
        static int pendingNewRow = -1;
        static uint32_t pendingNewRowSeq = 0;
        if ( probeRowMapByPad[ rowProbed ] != pendingNewRow ||
             callSeq != pendingNewRowSeq + 1 ) {
            pendingNewRow = probeRowMapByPad[ rowProbed ];
            pendingNewRowSeq = callSeq;
            return -1;
        }
        pendingNewRow = -1;
        lastDuplicateTime = millis( );
        lastProbeRead = probeRead;
        lastRowProbed = probeRowMapByPad[ rowProbed ];
        PadDecode::noteConfidentDecode( d );
        return ( rawPad == 1 ) ? probeRowMapByPad[ rowProbed ] : probeRowMap[ rowProbed ];
    }

    lastProbeRead = probeRead;
    lastRowProbed = probeRowMapByPad[ rowProbed ];
    PadDecode::noteConfidentDecode( d );
    return ( rawPad == 1 ) ? probeRowMapByPad[ rowProbed ] : probeRowMap[ rowProbed ];
}

/// Single poll of the probe + its button + the encoder (one pass; callers
/// loop, or the session service provides the cadence).
/// @return row probed, or -16 connect / -18 remove buttons, -19/-17 encoder
///         up/down, -10 encoder pressed, -1 nothing
int Pads::readProbe( ) {
    if ( blockProbing > 0 && ( millis( ) - blockProbingTimer < blockProbing ) ) {
        return -1;
    }
    if ( blockProbing > 0 ) {
        blockProbing = 0;
    }

    // Encoder state (owned by core 1; consumed by the caller's cursor logic)
    if ( encoderDirectionState != NONE ) {
        if ( encoderDirectionState == UP ) {
            return -19;
        } else if ( encoderDirectionState == DOWN ) {
            return -17;
        }
    } else if ( encoderButtonState != IDLE ) {
        return -10;
    }

    // Service the button here: blocking sub-flows (SF choosers) call this
    // in their own loops where the main loop isn't pumping services. The
    // service has its own 4ms rate limit, so this is cheap.
    probeButton.service( );

    int buttonState = checkProbeButton( );
    if ( buttonState == 1 ) {
        return -18;
    } else if ( buttonState == 2 ) {
        return -16;
    }

    if ( millis( ) - doubleTimeout > 1000 ) {
        doubleTimeout = millis( );
        lastReadRaw = 0;
    }

    int probeRead = readProbeRaw( );
    if ( probeRead <= 0 ) {
        return -1;
    }
    doubleTimeout = millis( );

    PadDecodeResult d = PadDecode::decode( probeRead );

    // SF/logo pad region: average several reads before trusting the decode
    // (the big ladder resistances make single bursts noisier down there).
    if ( d.index >= 95 ) {
        int probeReadings[ 8 ] = { 0 };
        int sum = 0, good = 0;
        for ( int i = 0; i < 8; i++ ) {
            probeReadings[ i ] = readProbeRaw( 0, 1 );
            if ( probeReadings[ i ] > 0 ) {
                sum += probeReadings[ i ];
                good++;
            }
        }
        if ( good == 0 ) {
            return -1;
        }
        d = PadDecode::decode( sum / good );
    }

    if ( d.index <= 0 || d.index > 101 ) {
        if ( debugProbing == 1 ) {
            Serial.printf( "pad decode out of range (raw %d)\n\r", probeRead );
        }
        return -1;
    }
    if ( debugProbing == 1 ) {
        Serial.printf( "probeRowMap[%d]: %d (raw %d expected %d)\n\r", d.index,
                       probeRowMap[ d.index ], d.raw, d.expected );
    }

    PadDecode::noteConfidentDecode( d );

    int rowProbed = selectSFprobeMenu( probeRowMap[ d.index ] );
    if ( debugProbing == 1 ) {
        Serial.printf( "rowProbed: %d\n\r", rowProbed );
    }
    connectedRows[ 0 ] = rowProbed;
    connectedRowsIndex = 1;

    return rowProbed;
}

void Pads::checkPads( void ) {
    checkingPads = 1;

    int probeReadings[ 12 ] = { 0 };
    for ( int i = 0; i < 12; i++ ) {
        probeReadings[ i ] = readProbeRaw( 0, 1 );
    }

    int probeReading = 0;
    int numberOfGoodReadings = 0;
    for ( int i = 0; i < 12; i++ ) {
        if ( probeReadings[ i ] > 0 ) {
            probeReading += probeReadings[ i ];
            numberOfGoodReadings++;
        }
    }
    if ( numberOfGoodReadings == 0 ) {
        checkingPads = 0;
        return;
    }
    probeReading = probeReading / numberOfGoodReadings;

    PadDecodeResult d = PadDecode::decode( probeReading );
    if ( d.index <= 0 ) {
        checkingPads = 0;
        return;
    }

    padNoTouch = 0;
    int padNode = probeRowMap[ d.index ];

    if ( padNode != lastPadTouched ) {
        lastPadTouchedTime = millis( );
        samePadCount = 0;
    } else {
        samePadCount++;
    }
    lastPadTouched = padNode;
    padTimeout = millis( );

    Serial.print( "\r                                 \r" );

    switch ( padNode ) {
    case LOGO_PAD_TOP:
        Serial.print( "Top guy" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( LOGO_TOP, -2 );
        break;
    case LOGO_PAD_BOTTOM:
        Serial.print( "Bottom guy" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( LOGO_BOTTOM, -2 );
        break;
    case ADC_PAD:
        Serial.print( "ADC pad" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( ADC_0, -2 );
        setLogoOverride( ADC_1, -2 );
        break;
    case DAC_PAD:
        Serial.print( "DAC pad" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( DAC_0, -2 );
        setLogoOverride( DAC_1, -2 );
        break;
    case GPIO_PAD:
        Serial.print( "GPIO pad" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( GPIO_0, -2 );
        setLogoOverride( GPIO_1, -2 );
        break;
    case BUILDING_PAD_TOP:
        Serial.print( "Building top" );
        clearColorOverrides( 1, 1, 0 );
        break;
    case BUILDING_PAD_BOTTOM:
        Serial.print( "Building bottom" );
        clearColorOverrides( 1, 1, 0 );
        break;
    default:
        break;
    }

    Serial.flush( );
    checkingPads = 0;
}

// Boot-time floor seed. Replaces the legacy retry/reject dance with the
// continuous floor tracker; kept for main.cpp and app callers.
int Pads::getNothingTouched( int samples ) {
    if ( samples < 1 )
        samples = 1;
    if ( samples > 16 )
        samples = 16;
    int vals[ 16 ];
    int good = 0;
    for ( int i = 0; i < samples; i++ ) {
        int v = readAdc( 5, 8 );
        if ( v < 200 ) { // anything higher is a touch or a fault, not a floor
            vals[ good++ ] = v;
        }
    }
    if ( good == 0 ) {
        Serial.println( "All nothing touched samples rejected, check sense pad "
                        "connections\n\r" );
        return 0;
    }
    int floorEst = medianProbeBursts( vals, good );
    PadDecode::seedFloor( floorEst );
    return floorEst;
}

int Pads::checkProbeButton( void ) {
    // Event-based: consume the cached press posted by the ProbeButton
    // CRITICAL service (shared hardware driver).
    return probeButton.getButtonPress( true );
}

int Pads::checkProbeButtonState( void ) {
    return probeButton.getButtonState( );
}

// ============================================================================
// Encoder cursor navigation (breadboard / nano / SF zones)
// (copied from the legacy engine, class renamed; keep in sync manually)
// ============================================================================

void Pads::handleEncoderCursorNavigation(
    int setOrClear,
    int node1or2,
    const int* nodesToConnect,
    int connectOrClearProbe,
    unsigned long probeModeStartTime,
    long& lastEncoderPosition,
    float& encoderAccumulator,
    int& encoderCursorNode,
    int& cursorZone,
    int& subIndex,
    int& lastEncoderCursorNode,
    int& lastCursorZone,
    int& lastSubIndex,
    unsigned long& lastEncoderMovement,
    bool& encoderCursorVisible,
    int& persistentEncoderCursorNode,
    int& persistentCursorZone,
    int& persistentSubIndex,
    int* row,
    int* connectedRows,
    int& connectedRowsIndex,
    EncoderAccelerator& encoderAccel,
    unsigned long encoderHideTimeout,
    bool clickExitsToMenu ) {
    // Navigation zones enum (local to match probeMode context)
    enum CursorZone { ZONE_BREADBOARD = 0,
                      ZONE_NANO = 1,
                      ZONE_RAILS = 2,
                      ZONE_DAC = 3,
                      ZONE_ADC = 4,
                      ZONE_GPIO = 5,
                      ZONE_UART = 6,
                      ZONE_CURRENT = 7 };

    // ======= ENCODER-BASED NODE SELECTION WITH SPECIAL FUNCTION ZONES =======
    // Check for encoder movement
    long currentEncoderPosition = encoderPosition;
    long encoderDelta = -( currentEncoderPosition - lastEncoderPosition );

    if ( millis( ) - probeModeStartTime < 100 ) {
        lastEncoderPosition = encoderPosition;
    }

    // Don't clear color overrides here - they need to persist while cursor is visible in zones
    // Overrides are cleared when changing zones or on timeout
    if ( encoderDelta != 0 && ( millis( ) - probeModeStartTime > 100 ) ) {
        idleTime = millis( );

        // Wake hysteresis - CONNECT mode only: encoder probing is an
        // unusual way to make connections, so a grazed/settling wheel
        // shouldn't paint the cursor over the user's board. While the
        // cursor is hidden, swallow motion until 5 counts accumulate
        // inside a rolling 1-second window; only then wake the cursor
        // (which starts moving from this final count). Once visible it
        // moves per count as before. Clear mode (and everything else)
        // keeps the immediate wake-on-first-count behavior.
        if ( !encoderCursorVisible && setOrClear == 1 ) {
            // ponytail: statics, not more ref params - probeMode is one
            // blocking session at a time, and the 1s staleness reset makes
            // cross-session carryover moot.
            constexpr int kCursorWakeCounts = 5;
            static int wakeAccumulated = 0;
            static unsigned long lastWakeNudge = 0;
            if ( millis( ) - lastWakeNudge > 1000 ) {
                wakeAccumulated = 0;
            }
            lastWakeNudge = millis( );
            wakeAccumulated += (int)( encoderDelta > 0 ? encoderDelta : -encoderDelta );
            if ( wakeAccumulated < kCursorWakeCounts ) {
                // Not deliberate yet - swallow this nudge entirely.
                lastEncoderPosition = currentEncoderPosition;
                lastEncoderMovement = millis( );
                probeTimeout = millis( ); // still counts as activity
                row[ 0 ] = -1;            // caller's "no encoder selection" invariant
                return;
            }
            wakeAccumulated = 0;
        }

        // Encoder moved - show cursor and reset BOTH timeouts
        lastEncoderMovement = millis( );
        probeTimeout = millis( ); // Reset probe mode timeout to keep it active during use
        encoderCursorVisible = true;

        // Get accelerated delta
        float accelDelta = encoderAccel.getAcceleratedDelta( encoderDelta );
        encoderAccumulator += accelDelta;

        // Convert accumulated movement to integer steps
        int steps = (int)encoderAccumulator;
        if ( steps != 0 ) {
            encoderAccumulator -= steps; // Keep fractional part

            // Save last position for clearing (before we move)
            lastEncoderCursorNode = encoderCursorNode;
            lastCursorZone = cursorZone;
            lastSubIndex = subIndex;

            // Navigate through zones
            if ( steps < 0 ) {
                // Moving down (counter-clockwise)
                if ( cursorZone == ZONE_BREADBOARD ) {
                    encoderCursorNode += steps;
                    if ( encoderCursorNode < 0 ) {
                        // Wrap to last special functions zone (UART)
                        cursorZone = ZONE_CURRENT;
                        subIndex = 1; // UART RX
                        encoderCursorNode = -1;
                    }
                } else if ( cursorZone == ZONE_NANO ) {
                    encoderCursorNode += steps;
                    if ( encoderCursorNode < NANO_D0 ) {
                        // Go to breadboard
                        cursorZone = ZONE_BREADBOARD;
                        encoderCursorNode = 59;
                    }
                } else {
                    // Navigate within special function zones
                    subIndex += steps;
                    if ( subIndex < 0 ) {
                        // Move to previous zone
                        cursorZone--;
                        if ( cursorZone < ZONE_RAILS ) {
                            cursorZone = ZONE_NANO;
                            encoderCursorNode = NANO_A7;
                        } else {
                            // Set subIndex to max for new zone
                            switch ( cursorZone ) {
                            case ZONE_RAILS:
                                subIndex = 2;
                                break; // 3 rails (TOP, BOTTOM, GND)
                            case ZONE_DAC:
                                subIndex = 1;
                                break; // 2 DACs
                            case ZONE_ADC:
                                subIndex = 5;
                                break; // 6 ADCs (0-4, 7)
                            case ZONE_GPIO:
                                subIndex = 7;
                                break; // 8 GPIOs
                            case ZONE_UART:
                                subIndex = 1;
                                break; // 2 UART (TX, RX)
                            case ZONE_CURRENT:
                                subIndex = 1;
                                break; // Current +/-
                            }
                        }
                    }
                }
            } else {
                // Moving up (clockwise)
                if ( cursorZone == ZONE_BREADBOARD ) {
                    encoderCursorNode += steps;
                    if ( encoderCursorNode > 59 ) {
                        // Go to nano header
                        cursorZone = ZONE_NANO;
                        encoderCursorNode = NANO_D0;
                    }
                } else if ( cursorZone == ZONE_NANO ) {
                    encoderCursorNode += steps;
                    if ( encoderCursorNode > NANO_A7 ) {
                        // Go to special functions (Rails)
                        cursorZone = ZONE_RAILS;
                        subIndex = 0;
                        encoderCursorNode = -1;
                    }
                } else {
                    // Navigate within special function zones
                    int maxSubIndex = 0;
                    switch ( cursorZone ) {
                    case ZONE_RAILS:
                        maxSubIndex = 2;
                        break;
                    case ZONE_DAC:
                        maxSubIndex = 1;
                        break;
                    case ZONE_ADC:
                        maxSubIndex = 5;
                        break;
                    case ZONE_GPIO:
                        maxSubIndex = 7;
                        break;
                    case ZONE_UART:
                        maxSubIndex = 1;
                        break;
                    case ZONE_CURRENT:
                        maxSubIndex = 1;
                        break;
                    }

                    subIndex += steps;
                    if ( subIndex > maxSubIndex ) {
                        // Move to next zone
                        cursorZone++;
                        if ( cursorZone > ZONE_CURRENT ) {
                            // Wrap back to breadboard
                            cursorZone = ZONE_BREADBOARD;
                            encoderCursorNode = 0;
                        } else {
                            subIndex = 0;
                        }
                    }
                }
            }

            // ========== ATOMIC LED UPDATE - PAUSE CORE2 WHILE MAKING CHANGES ==========
            // pauseCore2 = 1; // Pause Core 2 LED updates

            // 1. Clear previous highlighting if zone or subIndex changed
            bool zoneChanged = ( lastCursorZone != cursorZone );
            bool subIndexChanged = ( cursorZone >= ZONE_RAILS && lastSubIndex != subIndex );

            if ( zoneChanged || subIndexChanged ) {
                Highlighting::getInstance( ).clearHighlighting( );
                clearLEDsExceptRails( );
                b.clear( );
                showLEDsCore2 = 2;
                clearColorOverrides( 1, 1, 0 );

                // Clear inPadMenu when leaving special function zones
                if ( lastCursorZone >= ZONE_RAILS && cursorZone < ZONE_RAILS ) {
                    inPadMenu = 0;
                }
            }

            // 2. Clear previous cursor position only if in same zone
            if ( !zoneChanged ) {
                if ( cursorZone == ZONE_BREADBOARD && lastEncoderCursorNode >= 0 && lastEncoderCursorNode != encoderCursorNode ) {
                    b.printRawRow( 0b00000100, lastEncoderCursorNode, 0x000000, 0x000000 );
                } else if ( cursorZone == ZONE_NANO && lastEncoderCursorNode >= 0 && lastEncoderCursorNode != encoderCursorNode ) {
                    int pixel = getNanoHeaderPixel( lastEncoderCursorNode );
                    if ( pixel >= 0 )
                        leds.setPixelColor( pixel, 0x000000 );
                }
            }

            // 3. Set cursor color based on mode
            uint32_t cursorColor = setOrClear == 1 ? 0x250035 : 0x362404;
            uint32_t dimColor = setOrClear == 1 ? 0x080205 : 0x080500;

            ///[active/dim][setOrClear][index]
            uint32_t cursorColors[ 2 ][ 2 ][ 8 ] = {
                {
                    // active

                    // clear
                    { 0x391912, 0x3a1810, 0x3b1608, 0x3d1411, 0x3f1214, 0x411015, 0x450e16, 0x480c14 },

                    // set
                    { 0x191229, 0x18102a, 0x16082b, 0x14112d, 0x12142f, 0x101531, 0x0e1635, 0x0c1438 },

                },
                {
                    // dim
                    // clear
                    { 0x040302, 0x040302, 0x040202, 0x050302, 0x050401, 0x050301, 0x040302, 0x040302 },

                    // set
                    { 0x050304, 0x050204, 0x040204, 0x030205, 0x040105, 0x050104, 0x040203, 0x030305 },

                } };

            globalEncoderCursorColor = cursorColor;

            // 4. Get actual node and display based on zone
            int actualNode = -1;
            char displayName[ 32 ] = "";

            if ( cursorZone == ZONE_BREADBOARD ) {
                actualNode = encoderCursorNode + 1;
                strcpy( displayName, definesToChar( actualNode, 0 ) );
                b.printRawRow( 0b00000100, encoderCursorNode, 0x121215, cursorColor );
                globalEncoderCursorNode = encoderCursorNode;
                globalEncoderCursorInHeader = 0;
            } else if ( cursorZone == ZONE_NANO ) {
                actualNode = encoderCursorNode;
                strcpy( displayName, definesToChar( actualNode, 0 ) );
                int pixel = getNanoHeaderPixel( encoderCursorNode );
                if ( pixel >= 0 )
                    leds.setPixelColor( pixel, cursorColor );
                globalEncoderCursorNode = encoderCursorNode;
                globalEncoderCursorInHeader = 1;
            } else if ( cursorZone == ZONE_RAILS ) {
                // Display rails: TOP_RAIL, BOTTOM_RAIL, GND - one at a time
                const char* railNames[ 3 ] = { "  Top    Rail", " Bottom  Rail", "         GND" };
                const char* railDisplayNames[ 3 ] = { "Top Rail", "Bottom Rail", "GND" };
                int railBrightened[ 3 ] = { 0, 2, 1 };
                uint32_t railColors[ 3 ] = { 0x200010, 0x150015, 0x003005 };
                int railNodes[ 3 ] = { TOP_RAIL, BOTTOM_RAIL, GND };
                actualNode = railNodes[ subIndex ];
                strcpy( displayName, railDisplayNames[ subIndex ] );

                // DON'T set inPadMenu for rails - we want the rail LEDs to update
                // Rails don't have breadboard positions so no text conflict
                inPadMenu = 1;

                // Highlight the rail LEDs
                Highlighting::getInstance( ).brightenedRail = railBrightened[ subIndex ];

                // Show only the currently selected rail in the center
                b.print( railNames[ subIndex ], scaleBrightness( railColors[ subIndex ], 0 ), 0xFFFFFF, 0, -1, -2 );
            } else if ( cursorZone == ZONE_DAC ) {
                // Display DAC 0 and 1
                const int dacNodes[ 2 ] = { DAC0, DAC1 };
                actualNode = dacNodes[ subIndex ];
                snprintf( displayName, sizeof( displayName ), "DAC %d", subIndex );

                // Prevent net LEDs from overwriting our text display
                inPadMenu = 1;

                clearLEDsExceptRails( );
                b.print( "DAC", scaleDownBrightness( rawOtherColors[ 9 ], 4, 22 ), 0xFFFFFF, 1, 0, 3 );

                // Set DAC colorOverrides using helper functions
                if ( subIndex == 0 ) {
                    setLogoOverride( DAC_0, -2 );

                    b.print( "0", cursorColors[ 1 ][ setOrClear ][ 0 ], 0xFFFFFF, 0, 1, 3 );
                    b.print( "1", cursorColors[ 0 ][ setOrClear ][ 4 ], 0xFFFFFF, 5, 1, 0 );
                } else {

                    setLogoOverride( DAC_1, -2 );
                    b.print( "0", cursorColors[ 0 ][ setOrClear ][ 0 ], 0xFFFFFF, 0, 1, 3 );
                    b.print( "1", cursorColors[ 1 ][ setOrClear ][ 4 ], 0xFFFFFF, 5, 1, 0 );
                }
            } else if ( cursorZone == ZONE_ADC ) {
                // Display ADC 0-4, 7 (ADC 5,6 don't exist)
                const int adcMap[ 6 ] = { ADC0, ADC1, ADC2, ADC3, ADC4, ADC7 };
                const char* adcLabels[ 6 ] = { "0", "1", "2", "3", "4", "P" }; // P for probe
                actualNode = adcMap[ subIndex ];
                snprintf( displayName, sizeof( displayName ), "ADC %s", adcLabels[ subIndex ] );

                // Prevent net LEDs from overwriting our text display
                inPadMenu = 1;

                clearLEDsExceptRails( );
                b.print( " ADC", scaleDownBrightness( rawOtherColors[ 8 ], 4, 22 ), 0xFFFFFF, 0, 0, 3 );

                for ( int i = 0; i < 6; i++ ) {
                    uint32_t color = ( i == subIndex ) ? cursorColors[ 0 ][ setOrClear ][ i ] : cursorColors[ 1 ][ setOrClear ][ i ];
                    b.print( adcLabels[ i ], color, 0xFFFFFF, i, 1, ( i == 0 ? -1 : i - 1 ) );
                }

                if ( subIndex % 2 == 0 ) {
                    setLogoOverride( ADC_0, -2 );

                } else {

                    setLogoOverride( ADC_1, -2 );
                }

            } else if ( cursorZone == ZONE_GPIO ) {
                // Display GPIO 1-8
                actualNode = RP_GPIO_1 + subIndex;
                snprintf( displayName, sizeof( displayName ), "GPIO %d", subIndex + 1 );

                // Prevent net LEDs from overwriting our text display
                inPadMenu = 1;

                clearLEDsExceptRails( );
                uint32_t inColor = ( connectOrClearProbe == 0 ) ? 0x000000 : 0x000606;
                uint32_t outColor = ( connectOrClearProbe == 0 ) ? 0x000000 : 0x060100;

                // Display GPIO 1-4 on top row using original positioning
                const int positions[ 4 ] = { 0, 2, 4, 6 };
                const int nudges[ 4 ] = { 1, 0, -1, -2 };

                for ( int i = 0; i < 4; i++ ) {
                    uint32_t numColor = ( i == subIndex ) ? cursorColors[ 0 ][ setOrClear ][ i ] : cursorColors[ 1 ][ setOrClear ][ i ];
                    char numStr[ 2 ] = { (char)( '1' + i ), '\0' };
                    b.print( numStr, numColor, 0xFFFFFF, positions[ i ], 0, nudges[ i ] );

                    // Show input/output indicators for top row
                    int rowBase = 2 + i * 7;
                    b.printRawRow( 0b00011000, rowBase, ( i == subIndex ) ? inColor : 0x000000, 0xFFFFFF );
                    b.printRawRow( 0b00011000, rowBase + 4, ( i == subIndex ) ? outColor : 0x000000, 0xFFFFFF );
                }

                // Display GPIO 5-8 on bottom row using original positioning
                for ( int i = 4; i < 8; i++ ) {
                    uint32_t numColor = ( i == subIndex ) ? cursorColors[ 0 ][ setOrClear ][ i ] : cursorColors[ 1 ][ setOrClear ][ i ];
                    char numStr[ 2 ] = { (char)( '1' + i ), '\0' };
                    b.print( numStr, numColor, 0xFFFFFF, positions[ i - 4 ], 1, nudges[ i - 4 ] );

                    // Show input/output indicators for bottom row
                    int rowBase = 32 + ( i - 4 ) * 7;
                    b.printRawRow( 0b00000011, rowBase, ( i == subIndex ) ? inColor : 0x000000, 0xFFFFFF );
                    b.printRawRow( 0b00000011, rowBase + 4, ( i == subIndex ) ? outColor : 0x000000, 0xFFFFFF );
                }

                // Clear all logo overrides first, then set only GPIO
                clearColorOverrides( true, true, false );

                // Set GPIO colorOverrides using helper functions
                if ( subIndex % 2 == 0 ) {
                    setLogoOverride( GPIO_0, -2 );

                } else {

                    setLogoOverride( GPIO_1, -2 ); //-2 sets  the override to the default highlighed color
                }
            } else if ( cursorZone == ZONE_UART ) {
                // Display UART TX and RX - one at a time to prevent overlap
                const char* uartNames[ 2 ] = { "TX", "RX" };
                int uartNodes[ 2 ] = { RP_UART_TX, RP_UART_RX };
                actualNode = uartNodes[ subIndex ];
                snprintf( displayName, sizeof( displayName ), "UART %s", uartNames[ subIndex ] );

                // Prevent net LEDs from overwriting our text display
                inPadMenu = 1;

                // Show UART label and only the selected TX or RX
                b.print( " UART", sfOptionColors[ 3 ], 0xFFFFFF, 0, 0, 2 );
                b.print( uartNames[ 0 ], subIndex == 0 ? cursorColors[ 0 ][ setOrClear ][ 0 ] : cursorColors[ 1 ][ setOrClear ][ 0 ], 0xFFFFFF, 1, 1, -2 );
                b.print( uartNames[ 1 ], subIndex == 1 ? cursorColors[ 0 ][ setOrClear ][ 1 ] : cursorColors[ 1 ][ setOrClear ][ 1 ], 0xFFFFFF, 4, 1, 2 );

                // Clear all logo overrides first, then set only UART
                // clearColorOverrides(true, true, false);

                // Set logo colorOverrides for UART using helper functions
                if ( subIndex == 0 ) {
                    setLogoOverride( LOGO_TOP, -2 );

                } else {

                    setLogoOverride( LOGO_BOTTOM, -2 );
                }
            } else if ( cursorZone == ZONE_CURRENT ) {
                const char* currentNames[ 2 ] = { "I+", "I-" };
                int currentNodes[ 2 ] = { ISENSE_PLUS, ISENSE_MINUS };
                actualNode = currentNodes[ subIndex ];
                snprintf( displayName, sizeof( displayName ), "Current %s", subIndex == 0 ? "+" : "-" );

                // Color definitions for I+ (red) and I- (green)
                const uint32_t plusBrightColor = 0x2A0002;  // Bright red for selected I+
                const uint32_t plusDimColor = 0x0a0000;     // Dim red for unselected I+
                const uint32_t minusBrightColor = 0x002A02; // Bright green for selected I-
                const uint32_t minusDimColor = 0x000A00;    // Dim green for unselected I-

                inPadMenu = 1;
                clearLEDsExceptRails( );

                b.print( "Current", sfOptionColors[ 6 ], 0xFFFFFF, 0, 0, 1 );
                b.print( currentNames[ 0 ], subIndex == 0 ? plusBrightColor : plusDimColor, 0xFFFFFF, 1, 1, -2 );
                b.print( currentNames[ 1 ], subIndex == 1 ? minusBrightColor : minusDimColor, 0xFFFFFF, 4, 1, 2 );

                clearColorOverrides( true, true, false );
            }

            // 5. Try highlighting nets if we're on a regular node
            int netOnNode = 0;
            if ( actualNode > 0 && ( cursorZone == ZONE_BREADBOARD || cursorZone == ZONE_NANO ) ) {
                // -1 = "resolve from the node like a probe tap" (0 used to
                // be misread as an encoder-driven highlight by UART display)
                netOnNode = Highlighting::getInstance( ).highlightNets( actualNode, -1, 1 );
            }

            // 6. Display name on Serial and OLED
            if ( netOnNode <= 0 || cursorZone >= ZONE_RAILS ) {
                if ( brightenedNode > 0 ) {
                    Highlighting::getInstance( ).clearHighlighting( );
                }

                Serial.print( "\r                                               \r" );
                Serial.print( ">>>> " );
                Serial.print( displayName );
                Serial.flush( );

                oled.clearPrintShow( displayName, 2, true, true );
            }

            // 7. Save persistent cursor position
            persistentEncoderCursorNode = encoderCursorNode;
            persistentCursorZone = cursorZone;
            persistentSubIndex = subIndex;

            // 8. NOW update LEDs atomically - unpause and trigger update
            // pauseCore2 = 0;    // Unpause Core 2
            // showLEDsCore2 = 2; // Trigger single atomic update

            // ========== END ATOMIC UPDATE ==========

            // If we have first node and selecting second, show preview
            if ( node1or2 == 1 && nodesToConnect[ 0 ] > 0 && setOrClear == 1 && cursorZone <= ZONE_NANO ) {
                int previewNode = ( cursorZone == ZONE_NANO ) ? encoderCursorNode : ( encoderCursorNode + 1 );

                // Visual preview: highlight both nodes without modifying state
                if ( previewNode > 0 && previewNode != nodesToConnect[ 0 ] &&
                     previewNode >= 1 && previewNode <= 60 ) {
                    // Show first node
                    b.printRawRow( 0b00000100, nodesToConnect[ 0 ] - 1, 0x121215, 0x4500e8 );
                    // Show second node being previewed
                    b.printRawRow( 0b00000100, previewNode - 1, 0x121215, 0x00e845 );
                }
            }

            // showLEDsCore2 = 2;
        }

        lastEncoderPosition = currentEncoderPosition;
    }

    // Check for encoder cursor timeout (auto-hide after 5 seconds)
    if ( encoderCursorVisible && ( millis( ) - lastEncoderMovement ) > encoderHideTimeout ) {
        // Clear the cursor position before hiding
        if ( cursorZone == ZONE_BREADBOARD && encoderCursorNode >= 0 ) {
            b.printRawRow( 0b00000100, encoderCursorNode, 0x000000, 0x000000 );
        } else if ( cursorZone == ZONE_NANO && encoderCursorNode >= 0 ) {
            int pixel = getNanoHeaderPixel( encoderCursorNode );
            if ( pixel >= 0 )
                leds.setPixelColor( pixel, 0x000000 );
        } else if ( cursorZone >= ZONE_RAILS ) {
            // Clear special function zone display
            clearLEDsExceptRails( );
            // Clear inPadMenu flag
            inPadMenu = 0;
        }

        // Clear net highlighting and all color overrides
        Highlighting::getInstance( ).clearHighlighting( 0 );
        clearColorOverrides( true, true, false );

        encoderCursorVisible = false;
        lastEncoderCursorNode = -1;
        lastCursorZone = -1;
        globalEncoderCursorNode = -1; // Clear global
        globalEncoderCursorInHeader = 0;
        showLEDsCore2 = 2;
    }
    rotaryEncoderButtonStuff();

    // Check if button pressed while cursor is hidden
    if ( !encoderCursorVisible && ( encoderButtonState == RELEASED ) ) {
        if ( clickExitsToMenu ) {
            // probeMode was entered from the probe button, not the click
            // menu: a bare click with no cursor showing means "get out of
            // my way and give me the menu." Leave the event UNCONSUMED -
            // probeMode's loop sees RELEASED right after we return and
            // turns it into an exit-to-menu.
        } else {
            // Menu-launched session: re-show cursor and consume the press
            encoderCursorVisible = true;
            lastEncoderMovement = millis( ); // Reset cursor timeout
            probeTimeout = millis( );        // Reset probe mode timeout to keep it active
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE; // Set to IDLE to prevent menu trigger on release
                                           // Trigger cursor redraw on next iteration
            showLEDsCore2 = 2;
        }
    }

    // Check for encoder button press to select node
    // Only process encoder button if cursor is visible (otherwise it might interfere with normal operation)

    if ( encoderCursorVisible && ( ( encoderButtonState == RELEASED ) || ( ProbeButton::getInstance( ).getButtonState( ) == ( setOrClear == 1 ? 2 : 1 ) ) && ( millis( ) - probeModeStartTime > 500 ) ) ) {
        // IMMEDIATELY reset button state to prevent it from triggering click menu
        // Serial.println("Encoder button press to select node - processing");
        // Serial.flush();
        encoderButtonState = IDLE;
        lastButtonEncoderState = IDLE; // Set to IDLE (not PRESSED) to prevent menu trigger on release
        blockProbeButton = 8000;
        blockProbeButtonTimer = millis( );
        probeTimeout = millis( ); // Reset probe mode timeout when user selects node
        ProbeButton::getInstance( ).clearButtonState( );

        // Serial.println("Encoder button press to select node - processing");
        // Serial.flush();

        // Get the actual node number based on current zone
        int selectedNode = -1;

        if ( cursorZone == ZONE_BREADBOARD ) {
            selectedNode = encoderCursorNode + 1;
        } else if ( cursorZone == ZONE_NANO ) {
            selectedNode = encoderCursorNode;
        } else if ( cursorZone == ZONE_RAILS ) {
            const int railNodes[ 3 ] = { TOP_RAIL, BOTTOM_RAIL, GND };
            selectedNode = railNodes[ subIndex ];
        } else if ( cursorZone == ZONE_DAC ) {
            const int dacNodes[ 2 ] = { DAC0, DAC1 };
            selectedNode = dacNodes[ subIndex ];

            // For DAC, launch voltage adjuster
            VoltageAdjustConfig config;
            config.minVoltage = -8.0;
            config.maxVoltage = 8.0;
            config.enableSnap = false;
            config.liveUpdateInRange = true;
            config.liveUpdateMin = 0.0;
            config.liveUpdateMax = 5.0;

            if ( subIndex == 0 ) {
                // DAC 0
                config.initialValue = globalState.power.dac0;
                config.label = "DAC 0";
                config.callback = []( float newValue, bool isLive, void* context ) {
                    setDac0voltage( newValue, 1, 0, false );
                    globalState.power.dac0 = newValue;
                };
            } else {
                // DAC 1
                config.initialValue = globalState.power.dac1;
                config.label = "DAC 1";
                config.callback = []( float newValue, bool isLive, void* context ) {
                    setDac1voltage( newValue, 1, 0, false );
                    globalState.power.dac1 = newValue;
                };
            }

            AdjustResult result = VoltageAdjuster::adjust( config );
            if ( result == AdjustResult::CONFIRMED ) {
                saveVoltages( globalState.power.topRail, globalState.power.bottomRail,
                              globalState.power.dac0, globalState.power.dac1 );
            }

            // Clear and continue without selecting node
            encoderCursorVisible = false;
            lastEncoderCursorNode = -1;
            globalEncoderCursorNode = -1;
            setLogoOverride( DAC_0, -2 );
            setLogoOverride( DAC_1, -2 );
            // clearLEDsExceptRails( );
            showLEDsCore2 = -1;
            return; // Early return for DAC adjustment
        } else if ( cursorZone == ZONE_ADC ) {
            const int adcMap[ 6 ] = { ADC0, ADC1, ADC2, ADC3, ADC4, ADC7 };
            selectedNode = adcMap[ subIndex ];
        } else if ( cursorZone == ZONE_GPIO ) {
            selectedNode = RP_GPIO_1 + subIndex;

            // For GPIO, prompt for input/output selection if connecting
            if ( connectOrClearProbe == 1 ) {
                int gpioIndex = subIndex + 1; // GPIO 1-8

                // Clear the special function display first
                // clearLEDsExceptRails( );
                b.clear( );

                // Show input/output selection menu
                int ioSelection = chooseGPIOinputOutput( gpioIndex );

                // If user cancelled, don't select the node
                if ( ioSelection == -1 ) {
                    // User cancelled - clear and don't select node
                    selectedNode = -1;
                }
            }
        } else if ( cursorZone == ZONE_UART ) {
            const int uartNodes[ 2 ] = { RP_UART_TX, RP_UART_RX };
            selectedNode = uartNodes[ subIndex ];
        } else if ( cursorZone == ZONE_CURRENT ) {
            const int currentNodes[ 2 ] = { ISENSE_PLUS, ISENSE_MINUS };
            selectedNode = currentNodes[ subIndex ];
        }

        // Treat encoder selection like a probe touch
        if ( selectedNode > 0 ) {
            row[ 0 ] = selectedNode;
            connectedRows[ 0 ] = selectedNode;
            connectedRowsIndex = 1;
        }

        // Clear net highlighting and color overrides
        Highlighting::getInstance( ).clearHighlighting( 0 );

        // Clear all colorOverrides using helper function
        clearColorOverrides( true, true, false );

        // Clear inPadMenu flag
        inPadMenu = 0;

        // Reset encoder cursor for next selection
        encoderCursorVisible = false;
        lastEncoderCursorNode = -1;
        lastCursorZone = -1;
        globalEncoderCursorNode = -1; // Clear global (hides cursor)
        globalEncoderCursorInHeader = 0;
        lastEncoderMovement = millis( ); // Reset timeout

        if (cursorZone != ZONE_BREADBOARD) {
            probeButton.clearButtonState( );
            blockProbeButton = 100;
            blockProbeButtonTimer = millis( );
            showLEDsCore2 = -1;
        }

        // If we selected from special function zone, reset to row 15 breadboard for next selection
        if ( cursorZone >= ZONE_RAILS ) {
            persistentEncoderCursorNode = 14; // Row 15 (0-indexed)
            persistentCursorZone = ZONE_BREADBOARD;
            persistentSubIndex = 0;
            // Also reset the local working variables immediately
            encoderCursorNode = 14;
            cursorZone = ZONE_BREADBOARD;
            subIndex = 0;
        } else {
            // Normal breadboard/nano selection - persist position
            persistentEncoderCursorNode = encoderCursorNode;
            persistentCursorZone = cursorZone;
            persistentSubIndex = subIndex;
        }

        // Clear breadboard display and continue to normal probe processing
        // clearLEDsExceptRails( );

        //showLEDsCore2 = -1;

        // Continue to normal probe processing below
    } else {
        row[ 0 ] = -1; // No encoder selection this iteration - allow probe to work
    }

    // Check for encoder button HELD to exit probe mode. We do the
    // visual cleanup here (LED, highlighting, etc.) but DELIBERATELY
    // leave encoderButtonState == HELD so the caller's break check
    // (immediately after this function returns) actually sees it. The
    // caller is responsible for clearing the state after breaking.
    //
    // (Previously this branch cleared encoderButtonState = IDLE before
    // returning, which silently broke the caller's HELD detection and
    // made "hold encoder to exit probeMode" a no-op.)
    if ( encoderButtonState == HELD ) {
        // Clear cursor LED before exiting based on zone
        if ( cursorZone == ZONE_BREADBOARD && encoderCursorNode >= 0 ) {
            b.printRawRow( 0b00000100, encoderCursorNode, 0x000000, 0x000000 );
        } else if ( cursorZone == ZONE_NANO && encoderCursorNode >= 0 ) {
            int pixel = getNanoHeaderPixel( encoderCursorNode );
            if ( pixel >= 0 )
                leds.setPixelColor( pixel, 0x000000 );
        } else if ( cursorZone >= ZONE_RAILS ) {
            clearLEDsExceptRails( );
        }

        globalEncoderCursorNode = -1; // Clear globals on exit
        globalEncoderCursorInHeader = 0;

        // Clear all highlighting and color overrides
        Highlighting::getInstance( ).clearHighlighting( 0 );
        clearColorOverrides( true, true, false );

        // Clear inPadMenu flag
        inPadMenu = 0;

        showLEDsCore2 = -2; // Update LEDs to show cleared state
    }

    // ======= END ENCODER SELECTION =======
}

// ============================================================================
// SF probe menu + pad settings + DAC/ADC/GPIO/Isense/voltage choosers
// (copied from the legacy engine, class renamed; keep in sync manually)
// ============================================================================

int Pads::selectSFprobeMenu( int function ) {
    // Serial.println("selectSFprobeMenu");
    // Serial.flush();

    if ( checkingPads == 1 ) {
        inPadMenu = 0;

        // Serial.println("inPadMenu = 0");
        // Serial.flush();
        return function;
    }

    // bool selectFunction = false;
    inPadMenu = 1;
    switch ( function ) {

    case ADC_PAD: {
        inPadMenu = 1;

        // Serial.println("ADC_PAD");
        // Serial.flush();

        function = chooseADC( );
        blockProbing = 800;
        blockProbingTimer = millis( );
        // delay(10);
        inPadMenu = 0;
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( ADC_0, -2 );
        setLogoOverride( ADC_1, -2 );
        break;
    }
    case DAC_PAD: {
        inPadMenu = 1;
        // Serial.println("DAC_PAD");
        //  Serial.flush();
        function = chooseDAC( );
        blockProbing = 800;
        blockProbingTimer = millis( );
        // delay(10);
        inPadMenu = 0;
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( DAC_0, -2 );
        setLogoOverride( DAC_1, -2 );
        break;
    }
    case GPIO_PAD: {
        inPadMenu = 1;

        // Serial.println("GPIO_PAD");
        // Serial.flush();
        function = chooseGPIO( );
        blockProbing = 800;
        blockProbingTimer = millis( );
        // delay(10);
        inPadMenu = 0;
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( GPIO_0, -2 );
        setLogoOverride( GPIO_1, -2 );
        break;
    }
    case BUILDING_PAD_TOP:
    case BUILDING_PAD_BOTTOM:
    case LOGO_PAD_TOP:
    case LOGO_PAD_BOTTOM: {

        // b.clear();
        //     function = -1;
        // break;
        // b.clear();
        clearLEDsExceptRails( );
        switch ( function ) {
        case LOGO_PAD_TOP: {
            inPadMenu = 1;
            b.print( "UART", sfOptionColors[ 3 ], 0xFFFFFF, 0, 0, 0 );
            b.print( "Tx", sfOptionColors[ 7 ], 0xFFFFFF, 0, 1, 0 );
            b.printRawRow( 0b00000001, 23, 0x400014, 0xffffff );
            b.printRawRow( 0b00000011, 24, 0x400014, 0xffffff );
            b.printRawRow( 0b00011111, 25, 0x400014, 0xffffff );
            b.printRawRow( 0b00011011, 26, 0x400014, 0xffffff );
            b.printRawRow( 0b00000001, 27, 0x400014, 0xffffff );

            b.printRawRow( 0b00011100, 53, 0x400014, 0xffffff );
            b.printRawRow( 0b00011000, 54, 0x400014, 0xffffff );
            b.printRawRow( 0b00010000, 55, 0x400014, 0xffffff );

            // CRITICAL: Signal Core 2 to display menu with blocking PIO transfer
            // Prevents deadlock where async DMA drops frame and menu isn't visible
            showLEDsCore2 = 2; // 12 = blocking mode, value 2 (normal display)
            // waitForBlockingDisplay();  // Wait for Core 2 to finish displaying

            function = resolveLogoPadAssignment( jumperlessConfig.logo_pads.top_guy, RP_UART_TX );
            clearColorOverrides( 1, 1, 0 );
            setLogoOverride( LOGO_TOP, -2 );

            break;
        }
        case LOGO_PAD_BOTTOM: {
            inPadMenu = 1;
            b.print( "UART", sfOptionColors[ 3 ], 0xFFFFFF, 0, 0, -1 );
            b.print( "Rx", sfOptionColors[ 5 ], 0xFFFFFF, 0, 1, -1 );

            b.printRawRow( 0b00000000, 25, 0x280032, 0xffffff );
            b.printRawRow( 0b00000001, 26, 0x280032, 0xffffff );
            b.printRawRow( 0b00000011, 27, 0x280032, 0xffffff );

            b.printRawRow( 0b00001110, 53, 0x280032, 0xffffff );
            b.printRawRow( 0b00011110, 54, 0x280032, 0xffffff );
            b.printRawRow( 0b00010000, 55, 0x280032, 0xffffff );
            b.printRawRow( 0b00011111, 56, 0x280032, 0xffffff );
            b.printRawRow( 0b00011111, 57, 0x280032, 0xffffff );
            b.printRawRow( 0b00000011, 58, 0x280032, 0xffffff );

            b.printRawRow( 0b00000001, 52, 0x050500, 0xfffffe );
            b.printRawRow( 0b00000001, 53, 0x050500, 0xfffffe );
            b.printRawRow( 0b00000001, 54, 0x050500, 0xfffffe );
            b.printRawRow( 0b00000001, 55, 0x050500, 0xfffffe );
            b.printRawRow( 0b00000001, 59, 0x050500, 0xfffffe );

            // CRITICAL: Signal Core 2 to display menu with blocking PIO transfer
            // Prevents deadlock where async DMA drops frame and menu isn't visible
            showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display)
            waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

            function = resolveLogoPadAssignment( jumperlessConfig.logo_pads.bottom_guy, RP_UART_RX );
            clearColorOverrides( 1, 1, 0 );
            setLogoOverride( LOGO_BOTTOM, -2 );

            break;
        }
            // }
        case BUILDING_PAD_TOP:
        case BUILDING_PAD_BOTTOM: {
            // Both building pads now use the same chooser function
            function = chooseIsense( );
            break;
        }
        }

        // showLEDsCore2 = 2;
        // delayWithButton( 900 );

        // b.clear();
        clearLEDsExceptRails( );

        // lastReadRaw = 0;
        // b.print("Attach", sfOptionColors[0], 0xFFFFFF, 0, 0, -1);
        // b.print("to Pad", sfOptionColors[2], 0xFFFFFF, 0, 1, -1);
        // showLEDsCore2 = 2;

        // delayWithButton(800);

        // delay(800);

        // function = attachPadsToSettings(function);

        // if ( node1or2 == 0 ) {
        //     node1or2 = 1;
        //     nodesToConnect[ 0 ] = function;
        //     nodesToConnect[ 1 ] = -1;
        //     connectedRowsIndex = 1;
        // } else {
        //     nodesToConnect[ 1 ] = function;

        //     // connectedRowsIndex = 0;
        // }
        // Serial.print("sf connectedRowsIndex: ");
        // Serial.print(connectedRowsIndex);
        // Serial.print(" nodesToConnect[0]: ");
        // Serial.print(nodesToConnect[0]);
        // Serial.print(" nodesToConnect[1]: ");
        // Serial.println(nodesToConnect[1]);

        // Serial.print("function!!!!!: ");
        // printNodeOrName(function, 1);
        showLEDsCore2 = 1;
        lightUpRail( );
        // delay( 200 );
        inPadMenu = 0;
        sfProbeMenu = 0;
        // return function;

        // delay( 100 );

        break;
    }

    case 0: {
        // Serial.print( "function: " );
        // printNodeOrName( function, 1 );
        // Serial.print( function );
        // Serial.println( );
        function = -1;
        break;
    }
    case TOP_RAIL_GND:
    case BOTTOM_RAIL_GND: {
        function = 100;
        break;
    }
    default: {
        connectedRows[ 0 ] = function;
        connectedRowsIndex = 1;
        // lightUpRail( );
        // delay(500);
        // showLEDsCore2 = -1;
        // delayWithButton(900);
        sfProbeMenu = 0;
        inPadMenu = 0;

        return function;

        // inPadMenu = 0;
    }
    }

    // Serial.println("\n\n\n\\n\n\nn\nn\n\n\button state: ");
    // Serial.println(ProbeButton::getInstance().getButtonState( ));
    // Serial.println("\n\n\n\\n\n\nn\nn\n\n ");
    // Serial.flush();

    // this should only happen if it was a special function pad
    ProbeButton::getInstance( ).clearButtonState( );
    blockProbeButton = 1800;
    blockProbeButtonTimer = millis( );
    blockProbing = 800;
    blockProbingTimer = millis( );

    connectedRows[ 0 ] = function;
    connectedRowsIndex = 1;
    // lightUpRail( );
    // delay(500);
    showLEDsCore2 = -1;
    // delayWithButton(900);
    sfProbeMenu = 0;
    inPadMenu = 0;

    return function;
}

int Pads::attachPadsToSettings( int pad ) {
    int function = -1;
    int functionSetting = -1; // 0 = DAC, 1 = ADC, 2 = GPIO
    int settingOption =
        -1; // 0 = toggle, 1 = up/down, 2 = pwm, 3 = set voltage, 4 = input
    int dacChosen = -1;
    int adcChosen = -1;
    int gpioChosen = -1;
    connectedRowsIndex = 0;
    connectedRows[ 0 ] = -1;
    node1or2 = 0;
    unsigned long skipTimer = millis( );
    inPadMenu = 1;
    b.clear( );
    clearLEDsExceptRails( );
    // showLEDsCore2 = 2;
    //   lastReadRaw = 0;
    b.print( "DAC", sfOptionColors[ 0 ], 0xFFFFFF, 0, 0, -1 );
    b.print( "ADC", sfOptionColors[ 1 ], 0xFFFFFF, 4, 0, 0 );
    b.print( "GPIO", sfOptionColors[ 2 ], 0xFFFFFF, 8, 1, 1 );

    // CRITICAL: Signal Core 2 to display menu with blocking PIO transfer BEFORE blocking loop
    // Prevents deadlock where async DMA drops frame and user can't see menu options
    showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display)
    waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

    int selected = -1;

    while ( selected == -1 && longShortPress( 500 ) != 1 &&
            longShortPress( 500 ) != 2 ) {
        int reading = justReadProbe( );
        if ( reading != -1 ) {
            switch ( reading ) {
            case 1 ... 13: {
                selected = 0;
                functionSetting = 0;
                dacChosen = chooseDAC( 1 );
                Serial.print( "dacChosen: " );
                Serial.println( dacChosen );
                // b.clear();
                settingOption = dacChosen - DAC0;
                clearLEDsExceptRails( );
                // showLEDsCore2 = 1;

                break;
            }
            case 18 ... 30: {
                selected = 1;
                functionSetting = 1;
                adcChosen = chooseADC( );
                Serial.print( "adcChosen: " );
                Serial.println( adcChosen );
                settingOption = adcChosen - ADC0;

                // b.clear();
                clearLEDsExceptRails( );
                delayWithButton( 400 );
                // showLEDsCore2 = 1;

                break;
            }
            case 37 ... 53: {
                selected = 2;
                functionSetting = 2;
                // b.clear();
                clearLEDsExceptRails( );
                // showLEDsCore2 = 2;

                gpioChosen = chooseGPIO( 1 );
                // b.clear();
                clearLEDsExceptRails( );
                // showLEDsCore2 = 2;
                // if (gpioChosen >= 122 && gpioChosen <= 125) {
                //   gpioChosen = gpioChosen - 122 + 5;
                //   } else if (gpioChosen >= 135 && gpioChosen <= 138) {
                //     gpioChosen = gpioChosen - 134;
                //     }
                if ( gpioChosen >= RP_GPIO_1 && gpioChosen <= RP_GPIO_8 ) {
                    gpioChosen = gpioChosen - RP_GPIO_1 + 1;
                }

                // Serial.print( "gpioChosen: " );
                // Serial.println( gpioChosen );
                // Serial.print( "gpioState[gpioChosen]: " );
                // Serial.println( gpioState[ gpioChosen - 1 ] );
                if ( gpioState[ gpioChosen - 1 ] != 0 ) {
                    clearLEDsExceptRails( );
                    // showLEDsCore2 = 2;
                    Serial.print( "Set GP" );
                    Serial.print( gpioChosen );
                    Serial.println( " to Output" );
                    char gpString[ 4 ];
                    itoa( gpioChosen, gpString, 10 );

                    b.print( "GPIO", sfOptionColors[ ( gpioChosen + 1 ) % 7 ], 0xFFFFFF, 0, 0,
                             0 );
                    b.print( gpString, sfOptionColors[ gpioChosen - 1 ], 0xFFFFFF, 4, 0, 3 );
                    // b.print(" ", sfOptionColors[0], 0xFFFFFF, 0, 1, -2);
                    b.print( "Output", sfOptionColors[ ( gpioChosen + 3 ) % 7 ], 0xFFFFFF, 1,
                             1, 1 );
                    b.printRawRow( 0b00000100, 31, 0x200010, 0xffffff );
                    b.printRawRow( 0b00000100, 32, 0x200010, 0xffffff );
                    b.printRawRow( 0b00010101, 33, 0x200010, 0xffffff );
                    b.printRawRow( 0b00001110, 34, 0x200010, 0xffffff );
                    b.printRawRow( 0b00000100, 35, 0x200010, 0xffffff );
                    // showLEDsCore2 = 2;
                    delayWithButton( 400 );

                } else {
                }
                Serial.print( "gpioChosen - 1: " );
                Serial.println( gpioChosen - 1 );
                Serial.flush( );
                gpioState[ gpioChosen - 1 ] = 0;
                settingOption = gpioChosen - 1;
                setGPIO( );
                clearLEDsExceptRails( );

                // showLEDsCore2 = 2;
                b.print( "Tap to", sfOptionColors[ ( gpioChosen + 1 ) % 7 ], 0xFFFFFF, 0, 0,
                         1 );
                b.print( "toggle", sfOptionColors[ ( gpioChosen + 2 ) % 7 ], 0xFFFFFF, 0, 1,
                         1 );
                delayWithButton( 500 );
                clearLEDsExceptRails( );
                // showLEDsCore2 = 1;
                // inPadMenu = 0;

                break;
            }
            }
        }
    }
    // inPadMenu = 0;
    // Serial.print( "pad: " );
    // Serial.println( pad );
    // Serial.print( "functionSetting: " );
    // Serial.println( functionSetting );
    // Serial.print( "settingOption: " );
    // Serial.println( settingOption );
    switch ( functionSetting ) {
    case 2: {
        switch ( gpioChosen ) {
        case 1: {
            function = RP_GPIO_1;
            break;
        }
        case 2: {
            function = RP_GPIO_2;
            break;
        }
        case 3: {
            function = RP_GPIO_3;
            break;
        }
        case 4: {
            function = RP_GPIO_4;
            break;
        }
        case 5: {
            function = RP_GPIO_5;
            break;
        }
        case 6: {
            function = RP_GPIO_6;
            break;
        }
        case 7: {
            function = RP_GPIO_7;
            break;
        }
        case 8: {
            function = RP_GPIO_8;
            break;
        }
        }
        break;
    }
    case 1: {
        function = adcChosen;
        break;
    }
    case 0: {
        function = dacChosen;
        break;
    }
    }

    switch ( pad ) {
    case LOGO_PAD_TOP: {
        jumperlessConfig.logo_pads.top_guy = nodeToLogoPadConfig( function, jumperlessConfig.logo_pads.top_guy );
        // jumperlessConfig.logo_pads.top_guy = settingOption;

        break;
    }
    case LOGO_PAD_BOTTOM: {
        jumperlessConfig.logo_pads.bottom_guy = nodeToLogoPadConfig( function, jumperlessConfig.logo_pads.bottom_guy );
        // jumperlessConfig.logo_pads.bottom_guy = settingOption;
        break;
    }
    case BUILDING_PAD_TOP: {
        jumperlessConfig.logo_pads.building_pad_top = nodeToLogoPadConfig( function, jumperlessConfig.logo_pads.building_pad_top );
        // jumperlessConfig.logo_pads.building_pad_top= settingOption;
        break;
    }
    case BUILDING_PAD_BOTTOM: {
        jumperlessConfig.logo_pads.building_pad_bottom = nodeToLogoPadConfig( function, jumperlessConfig.logo_pads.building_pad_bottom );
        // jumperlessConfig.logo_pads.building_pad_bottom_setting = settingOption;
        break;
    }
    }
    saveLogoBindings( );
    delay( 3 );
    inPadMenu = 0;
    showLEDsCore2 = 1;
    return function;
}

int Pads::delayWithButton( int delayTime ) {
    // Rewritten to use state-based API (doesn't consume button events)
    // This allows button presses to propagate to outer probe mode logic
    unsigned long skipTimer = millis( );
    int lastSeenState = 0;

    while ( millis( ) - skipTimer < delayTime ) {
        // Check CURRENT state without consuming events
        int currentState = probeButton.getButtonState( );

        // Detect button press (transition from 0 to pressed)
        if ( currentState != 0 && lastSeenState == 0 ) {
            // Button just pressed - return which button it was
            // Serial.print("delayWithButton detected press: ");
            // Serial.println(currentState);
            return currentState;
        }

        lastSeenState = currentState;
        delayMicroseconds( 100 );
    }

    // Timeout - no button pressed
    return 0;
}

int Pads::chooseDAC( int justPickOne ) {
    // Serial.println("chooseDAC");
    // Serial.flush();
    // Set to true to skip menu and go directly to DAC 1
    // Set to false to show the DAC 0/1 selection menu
    static const bool skipToDAC1 = false;

    int function = -1;
    sfProbeMenu = 2;

    // CRITICAL FIX: Clear LEDs and buffer BEFORE setting display flag
    clearLEDsExceptRails( );
    b.clear( );

    // Serial.println("clearLEDsExceptRails");
    // Serial.flush();

    if ( connectOrClearProbe == 0 ) {
        justPickOne = 1;
    }

    if ( skipToDAC1 && justPickOne == 0 ) {
        // Direct to DAC 1 mode - skip menu
        function = 107;
        // lastReadRaw = 0;
        b.print( "DAC 1", scaleDownBrightness( rawOtherColors[ 9 ], 4, 22 ), 0xFFFFFF, 1, 0,
                 3 );

        // Serial.println("b.print");
        // Serial.flush();
        // b.print( "1", sfOptionColors[ 2 ], 0xFFFFFF, 5, 1, 0 );
        // b.print("8v", sfOptionColors[2], 0xFFFFFF, 5, 0, 1);
        // b.printRawRow(0b00011000, 58, sfOptionColors[4], 0xffffff);
        // b.printRawRow(0b00000100, 57, sfOptionColors[4], 0xffffff);
        // b.printRawRow(0b00000100, 56, sfOptionColors[4], 0xffffff);
        // b.printRawRow(0b00010101, 55, sfOptionColors[4], 0xffffff);
        // b.printRawRow(0b00001110, 54, sfOptionColors[4], 0xffffff);
        // b.printRawRow(0b00000100, 53, sfOptionColors[4], 0xffffff);

        if ( justPickOne == 1 ) {
            return function;
        }

        // Use new unified voltage adjuster with probe support
        VoltageAdjustConfig config;
        config.minVoltage = -8.0;
        config.maxVoltage = 8.0;
        config.initialValue = globalState.power.dac1;
        config.label = "DAC 1";
        config.enableSnap = false;
        config.liveUpdateInRange = true;
        config.liveUpdateMin = 0.0;
        config.liveUpdateMax = 5.0;
        config.callback = []( float newValue, bool isLive, void* context ) {
            setDac1voltage( newValue, 1, 0, false );
            globalState.power.dac1 = newValue;
        };

        probeButton.clearButtonState( );
        AdjustResult result = VoltageAdjuster::adjust( config );
        if ( result == AdjustResult::CONFIRMED ) {
            // Save to persistent storage
            saveVoltages( globalState.power.topRail, globalState.power.bottomRail,
                          globalState.power.dac0, globalState.power.dac1 );
        }

        blockProbeButton = 2000;
        blockProbeButtonTimer = millis( );
        showLEDsCore2 = -1;
        probeButton.clearButtonState( );
        delay( 100 );

    } else {
        // Original menu mode - show DAC 0/1 selection
        // CRITICAL FIX: Write to buffer FIRST, then signal Core 2
        b.print( "DAC", scaleDownBrightness( rawOtherColors[ 9 ], 4, 22 ), 0xFFFFFF, 1, 0, 3 );
        b.print( "0", sfOptionColors[ 0 ], 0xFFFFFF, 0, 1, 3 );
        b.print( "1", sfOptionColors[ 2 ], 0xFFFFFF, 5, 1, 0 );

        // CRITICAL FIX: Signal Core 2 to display menu with blocking PIO transfer
        // Core 1 must NEVER call leds.showBlocking() - use flag to signal Core 2
        // Without this, async DMA may drop the frame and menu won't display,
        // causing a deadlock where code waits for input on invisible menu
        showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display) - NO CLEAR
        waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

        // Serial.println("waitForBlockingDisplay");
        // Serial.flush();

        int selected = -1;
        function = 0;
        while ( selected == -1 ) {

            jOS.serviceCritical( );

            if ( ProbeButton::getInstance( ).getButtonState( ) != 0 ) {
                // selected = DAC1;
                // function = DAC1;
                break;
            }
            // Serial.println("justReadProbe");
            // Serial.flush();
            delayMicroseconds( 100 );

            int reading = justReadProbe( );
            if ( reading != -1 ) {
                switch ( reading ) {
                case 31 ... 43: {
                    selected = DAC0;
                    function = DAC0;
                    if ( justPickOne == 1 ) {
                        return function;
                    }

                    // Use new unified voltage adjuster with probe support
                    VoltageAdjustConfig config;
                    config.minVoltage = -8.0;
                    config.maxVoltage = 8.0;
                    config.initialValue = globalState.power.dac0;
                    config.label = "DAC 0";
                    config.enableSnap = false;
                    config.liveUpdateInRange = false;
                    config.liveUpdateMin = 3.3;
                    config.liveUpdateMax = 3.31;
                    config.callback = []( float newValue, bool isLive, void* context ) {
                        setDac0voltage( newValue, 1, 0, true );
                        globalState.power.dac0 = newValue;
                    };

                    probeButton.clearButtonState( );
                    blockProbing = 300;
                    blockProbingTimer = millis( );

                    AdjustResult result = VoltageAdjuster::adjust( config );
                    if ( result == AdjustResult::CONFIRMED ) {
                        // Save to persistent storage
                        saveVoltages( globalState.power.topRail, globalState.power.bottomRail,
                                      globalState.power.dac0, globalState.power.dac1 );
                    }
                    probeButton.clearButtonState( );
                    blockProbeButton = 2000;
                    blockProbeButtonTimer = millis( );
                    showLEDsCore2 = -1;

                    delay( 100 );
                    break;
                }
                case 48 ... 60: {
                    selected = 107;
                    function = 107;
                    if ( justPickOne == 1 ) {
                        return function;
                    }

                    // Use new unified voltage adjuster with probe support
                    VoltageAdjustConfig config;
                    config.minVoltage = -8.0;
                    config.maxVoltage = 8.0;
                    config.initialValue = globalState.power.dac1;
                    config.label = "DAC 1";
                    config.enableSnap = false;
                    config.liveUpdateInRange = true;
                    config.liveUpdateMin = 0.0;
                    config.liveUpdateMax = 5.0;
                    config.callback = []( float newValue, bool isLive, void* context ) {
                        setDac1voltage( newValue, 1, 0, true );
                        globalState.power.dac1 = newValue;
                    };

                    probeButton.clearButtonState( );
                    AdjustResult result = VoltageAdjuster::adjust( config );
                    if ( result == AdjustResult::CONFIRMED ) {
                        // Save to persistent storage
                        saveVoltages( globalState.power.topRail, globalState.power.bottomRail,
                                      globalState.power.dac0, globalState.power.dac1 );
                    }
                    probeButton.clearButtonState( );
                    blockProbeButton = 2000;
                    blockProbeButtonTimer = millis( );
                    showLEDsCore2 = -1;

                    delay( 100 );
                    break;
                }
                }
            }
        }
    }
    // Serial.println("return function");
    // Serial.flush();

    return function;
}

int Pads::chooseIsense( void ) {
    int function = -1;
    int selectedOption = 0; // 0 = ISENSE_PLUS (default), 1 = ISENSE_MINUS

    // Prevent net LEDs from overwriting our menu
    inPadMenu = 1;

    // Clear LEDs before showing menu
    clearLEDsExceptRails( );
    b.clear( );
    showLEDsCore2 = 2;

    // Track encoder position for selection with accumulator
    long lastEncPos = encoderPosition;
    int lastSelectedOption = -1;  // Track if we need to redraw
    int encoderAccumulator = 0;   // Accumulate encoder clicks
    const int clicksToSwitch = 5; // Require 8 clicks to switch

    // Serial.println( "Choose Current Sense" );
    // Serial.println( "  I+ (ISENSE_PLUS) or I- (ISENSE_MINUS)" );
    // Serial.println( "  Encoder: rotate to select, press to confirm" );

    // Color definitions for I+ (red) and I- (green)
    const uint32_t plusBrightColor = 0x2A0002;  // Bright red for selected I+
    const uint32_t plusDimColor = 0x0a0000;     // Dim red for unselected I+
    const uint32_t minusBrightColor = 0x002A02; // Bright green for selected I-
    const uint32_t minusDimColor = 0x000A00;    // Dim green for unselected I-

    // Initial display
    b.clear( );
    clearLEDsExceptRails( );
    uint32_t plusColor = ( selectedOption == 0 ) ? plusBrightColor : plusDimColor;
    uint32_t minusColor = ( selectedOption == 1 ) ? minusBrightColor : minusDimColor;
    b.print( "Current", sfOptionColors[ 6 ], 0xFFFFFF, 0, 0, 1 );
    b.print( "I+", plusColor, 0xFFFFFF, 1, 1, -2 );
    b.print( "I-", minusColor, 0xFFFFFF, 4, 1, 2 );
    oled.clearPrintShow( "Current\n I +     I -", 2, 100 );

    lastSelectedOption = selectedOption;

    // CRITICAL FIX: Signal Core 2 to display menu with blocking PIO transfer
    // Core 1 must NEVER call leds.showBlocking() - use flag to signal Core 2
    // Without this, async DMA may drop the frame and menu won't display,
    // causing a deadlock where code waits for input on invisible menu
    showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display)
    waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

    int selected = -1;
    while ( selected == -1 ) {
        // Keep critical services running
        jOS.serviceCritical( );

        // Check for encoder movement and accumulate
        long currentEncPos = encoderPosition;
        long encDelta = currentEncPos - lastEncPos;
        if ( encDelta != 0 ) {
            encoderAccumulator += encDelta;
            lastEncPos = currentEncPos;

            // Switch option when accumulator reaches threshold
            if ( encoderAccumulator >= clicksToSwitch ) {
                selectedOption = 0; // ISENSE_MINUS
                encoderAccumulator = 0;
            } else if ( encoderAccumulator <= -clicksToSwitch ) {
                selectedOption = 1; // ISENSE_PLUS
                encoderAccumulator = 0;
            }
        }

        // Only update display if selection changed
        if ( selectedOption != lastSelectedOption ) {
            b.clear( );
            clearLEDsExceptRails( );
            if ( selectedOption == 0 ) {
                oled.clearPrintShow( "I Sense +", 2, 100 );
            } else {
                oled.clearPrintShow( "I Sense -", 2, 100 );
                // oled.clearPrintShow( "I+", 2, 100 );
            }

            plusColor = ( selectedOption == 0 ) ? plusBrightColor : plusDimColor;
            minusColor = ( selectedOption == 1 ) ? minusBrightColor : minusDimColor;

            b.print( "Current", sfOptionColors[ 6 ], 0xFFFFFF, 0, 0, 1 );
            b.print( "I+", plusColor, 0xFFFFFF, 1, 1, -2 );
            b.print( "I-", minusColor, 0xFFFFFF, 4, 1, 2 );
            showLEDsCore2 = 2;

            lastSelectedOption = selectedOption;
        }

        // Check for encoder button press
        if ( encoderButtonState == PRESSED && lastButtonEncoderState == IDLE ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            selected = selectedOption;
            break;
        }

        // Also check for probe touch for backward compatibility
        int reading = justReadProbe( );
        if ( reading != -1 ) {
            switch ( reading ) {
            case 31 ... 43: {
                selected = 0; // ISENSE_PLUS
                break;
            }
            case 47 ... 60: {
                selected = 1; // ISENSE_MINUS
                break;
            }
            }
        }

        // Check for button press to exit
        if ( probeButton.getButtonState( ) == 1 ) {
            probeButton.clearButtonState( );
            blockProbeButton = 1000;
            blockProbeButtonTimer = millis( );
            // selected = -1;
            break;
        } else if ( probeButton.getButtonState( ) == 2 ) {
            probeButton.clearButtonState( );
            blockProbeButton = 1000;
            blockProbeButtonTimer = millis( );
            //   selected = -1;
            break;
        }

        delayMicroseconds( 200 );
    }

    // Map selection to function
    if ( selected == 0 ) {
        function = ISENSE_PLUS;
    } else if ( selected == 1 ) {
        function = ISENSE_MINUS;
    } else {
        function = -1;
    }

    delay( 100 );

    // connectedRowsIndex ++;
    // Serial.print( "Current Sense selected: " );
    // Serial.println( function == ISENSE_PLUS ? "ISENSE_PLUS (+)" : function == ISENSE_MINUS ? "ISENSE_MINUS (-)" : "None" );
    // Serial.flush( );

    clearLEDsExceptRails( );
    b.clear( );
    showLEDsCore2 = -1;

    // Clear inPadMenu flag
    inPadMenu = 0;

    return function;
}

int Pads::chooseADC( void ) {
    int function = -1;

    // CRITICAL FIX: Clear LEDs and buffer FIRST
    clearLEDsExceptRails( );
    b.clear( );

    // Write menu to buffer BEFORE signaling Core 2
    b.print( " ADC", scaleDownBrightness( rawOtherColors[ 8 ], 4, 22 ), 0xFFFFFF, 0, 0,
             3 );
    b.print( "0", sfOptionColors[ 0 ], 0xFFFFFF, 0, 1, -1 );
    b.print( "1", sfOptionColors[ 1 ], 0xFFFFFF, 1, 1, 0 );
    b.print( "2", sfOptionColors[ 2 ], 0xFFFFFF, 2, 1, 1 );
    b.print( "3", sfOptionColors[ 3 ], 0xFFFFFF, 3, 1, 2 );
    b.print( "4", sfOptionColors[ 4 ], 0xFFFFFF, 4, 1, 3 );
    b.print( "P", sfOptionColors[ 5 ], 0xFFFFFF, 5, 1, 4 );

    // CRITICAL FIX: Signal Core 2 to display menu with blocking PIO transfer
    // Core 1 must NEVER call leds.showBlocking() - use flag to signal Core 2
    // Without this, async DMA may drop the frame and menu won't display,
    // causing a deadlock where code waits for input on invisible menu
    showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display)
    waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

    // Serial.print("inPadMenu: ");
    // Serial.println(inPadMenu);
    // Serial.print("sfProbeMenu: ");
    // Serial.println(sfProbeMenu);
    // Serial.print("probeActive: ");
    // Serial.println(probeActive);
    // while (true);
    int selected = -1;
    while ( selected == -1 && ProbeButton::getInstance( ).getButtonState( ) == 0 ) {
        jOS.serviceCritical( );
        int reading = justReadProbe( );
        // Serial.print("reading: ");
        // Serial.println(reading);
        if ( reading != -1 ) {
            //       Serial.print("reading: ");
            // Serial.println(reading);
            switch ( reading ) {
            case 31 ... 35: {
                selected = ADC0;
                function = ADC0;

                break;
            }
            case 36 ... 40: {
                selected = ADC1;
                function = ADC1;
                // while (justReadProbe() == reading) {
                //   // Serial.print("reading: ");
                //   // Serial.println(justReadProbe());
                //   delay(10);
                // }
                break;
            }
            case 41 ... 45: {
                selected = ADC2;
                function = ADC2;
                // while (justReadProbe() == reading) {
                //   // Serial.print("reading: ");
                //   // Serial.println(justReadProbe());
                //   delay(10);
                // }
                break;
            }
            case 46 ... 50: {
                selected = ADC3;
                function = ADC3;
                // while (justReadProbe() == reading) {
                //   // Serial.print("reading: ");
                //   // Serial.println(justReadProbe());
                //   delay(10);
                // }
                break;
            }
            case 51 ... 55: {
                selected = ADC4;
                function = ADC4;
                // while (justReadProbe() == reading) {
                //   // Serial.print("reading: ");
                //   // Serial.println(justReadProbe());
                //   delay(10);
                // }
                break;
            }
            case 56 ... 60: {
                selected = ADC7;
                function = ADC7;
                // while (justReadProbe() == reading) {
                //   // Serial.print("reading: ");
                //   // Serial.println(justReadProbe());
                //   delay(10);
                // }
                break;
            }
            }
            // while (justReadProbe() == reading) {
            //   Serial.print("reading: ");
            //   Serial.println(justReadProbe());
            //   delay(100);
            // }
        }
    }

    clearLEDsExceptRails( );
    // showNets();
    showLEDsCore2 = 1;
    return function;
}

int Pads::chooseGPIOinputOutput( int gpioChosen ) {
    int settingOption = -1;
    int selectedOption = 0; // 0 = input (default), 1 = output

    // Prevent net LEDs from overwriting our menu
    inPadMenu = 1;

    // Clear LEDs before showing menu
    clearLEDsExceptRails( );
    b.clear( );
    showLEDsCore2 = 2;

    // Show initial display
    char gpioNumStr[ 3 ];
    snprintf( gpioNumStr, sizeof( gpioNumStr ), "%d", gpioChosen );

    // Track encoder position for selection with accumulator
    long lastEncPos = encoderPosition;
    int lastSelectedOption = -1;  // Track if we need to redraw
    int encoderAccumulator = 0;   // Accumulate encoder clicks
    const int clicksToSwitch = 8; // Require 8 clicks to switch

    // Serial.print( "GPIO " );
    // Serial.print( gpioChosen );
    // Serial.print( " - Select Input or Output" );
    // Serial.println( "  Encoder: rotate to select, press to confirm" );

    // Initial display
    b.clear( );
    clearLEDsExceptRails( );
    uint32_t inputColor = ( selectedOption == 0 ) ? 0x4500e8 : 0x150050;
    uint32_t outputColor = ( selectedOption == 1 ) ? 0x4500e8 : 0x150050;
    b.print( "Input", inputColor, 0xFFFFFF, 1, 0, 3 );
    b.print( gpioNumStr, sfOptionColors[ gpioChosen - 1 ], 0xFFFFFF, 0, 0, -2 );
    b.print( "Output", outputColor, 0xFFFFFF, 0, 1, 3 );
    lastSelectedOption = selectedOption;

    // CRITICAL FIX: Signal Core 2 to display menu with blocking PIO transfer
    // Core 1 must NEVER call leds.showBlocking() - use flag to signal Core 2
    // Without this, async DMA may drop the frame and menu won't display,
    // causing a deadlock where code waits for input on invisible menu
    showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display)
    waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

    while ( settingOption == -1 && ProbeButton::getInstance( ).getButtonState( ) == 0 ) {
        // Keep critical services running
        jOS.serviceCritical( );

        // Check for encoder movement and accumulate
        long currentEncPos = encoderPosition;
        long encDelta = currentEncPos - lastEncPos;
        if ( encDelta != 0 ) {
            encoderAccumulator += encDelta;
            lastEncPos = currentEncPos;

            // Switch option when accumulator reaches threshold
            if ( encoderAccumulator >= clicksToSwitch ) {
                selectedOption = 1; // Output
                encoderAccumulator = 0;
            } else if ( encoderAccumulator <= -clicksToSwitch ) {
                selectedOption = 0; // Input
                encoderAccumulator = 0;
            }
        }

        // Only update display if selection changed
        if ( selectedOption != lastSelectedOption ) {
            b.clear( );
            clearLEDsExceptRails( );

            inputColor = ( selectedOption == 0 ) ? 0x4500e8 : 0x150050;
            outputColor = ( selectedOption == 1 ) ? 0x4500e8 : 0x150050;

            b.print( "Input", inputColor, 0xFFFFFF, 1, 0, 3 );
            b.print( gpioNumStr, sfOptionColors[ gpioChosen - 1 ], 0xFFFFFF, 0, 0, -2 );
            b.print( "Output", outputColor, 0xFFFFFF, 0, 1, 3 );
            showLEDsCore2 = 2;

            lastSelectedOption = selectedOption;
        }

        // Check for encoder button press
        if ( encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            settingOption = selectedOption;
            break;
        }

        // Also check for probe touch for backward compatibility
        int reading = justReadProbe( );
        if ( reading != -1 ) {
            switch ( reading ) {
            case 9 ... 29: {
                settingOption = 0; // Input
                break;
            }
            case 35 ... 59: {
                settingOption = 1; // Output
                break;
            }
            }
            blockProbing = 1000;
            blockProbingTimer = millis( );
        }

        // // Check for button press to exit
        // if ( longShortPress( 500 ) == 1 ) {
        //     blockProbing = 1000;
        //     blockProbingTimer = millis( );
        //     break;
        // }

        delayMicroseconds( 200 );
    }

    // Apply the selection
    if ( settingOption == 0 ) {
        // Input selected
        gpioState[ gpioChosen - 1 ] = 4;
        if ( globalState.config.gpioDirection[ gpioChosen - 1 ] == 0 ) {
            globalState.config.gpioDirection[ gpioChosen - 1 ] = 1;
            globalState.markDirty( );
            configChanged = true;
        }
    } else if ( settingOption == 1 ) {
        // Output selected
        gpioState[ gpioChosen - 1 ] = 0;
        if ( globalState.config.gpioDirection[ gpioChosen - 1 ] == 1 ) {
            globalState.config.gpioDirection[ gpioChosen - 1 ] = 0;
            globalState.markDirty( );
            configChanged = true;
        }
    }

    // Serial.print( "gpioChosen (chooseGPIOinputOutput): " );
    // Serial.print( gpioChosen );
    // Serial.print( " -> " );
    // Serial.println( settingOption == 0 ? "Input" : "Output" );
    // Serial.flush( );

    clearLEDsExceptRails( );
    b.clear( );
    showLEDsCore2 = -1;

    // Clear inPadMenu flag
    inPadMenu = 0;

    return settingOption;
}

int Pads::chooseGPIO( int skipInputOutput ) {
    int function = -1;
    sfProbeMenu = 3;

    // CRITICAL FIX: Clear LEDs and buffer FIRST, BEFORE signaling Core 2
    b.clear( );
    clearLEDsExceptRails( );

    uint32_t inColor = 0x000606;
    uint32_t outColor = 0x060100;
    if ( connectOrClearProbe == 0 ) {
        inColor = 0x000000;
        outColor = 0x000000;
    }
    if ( node1or2 == 0 ) {
        Serial.println( "           Choose GPIO" );
    } else {
        Serial.println( "Choose GPIO" );
    }
    Serial.println( "  tap pads near numbers to choose" );
    Serial.println( "       ⁱ = input, ⁰ = output\n\r" );
    Serial.println( "      ┌─┬─┐ ┌─┬─┐ ┌─┬─┐ ┌─┬─┐" );
    Serial.println( "     ╭───────────────────────╮" );
    Serial.println( "     │ ⁱ1⁰   ⁱ2⁰   ⁱ3⁰   ⁱ4⁰ │" );
    Serial.println( "     ├───────────────────────┤" );
    Serial.println( "     │ ᵢ5₀   ᵢ6₀   ᵢ7₀   ᵢ8₀ │" );
    Serial.println( "     ╰───────────────────────╯" );
    Serial.println( "      └─┴─┘ └─┴─┘ └─┴─┘ └─┴─┘" );

    // Write all menu content to buffer BEFORE signaling Core 2
    b.printRawRow( 0b00011000, 2, inColor, 0xFFFFFF );
    b.print( "1", sfOptionColors[ 0 ], 0xFFFFFF, 0, 0, 1 );
    b.printRawRow( 0b00011000, 6, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 7, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 9, inColor, 0xFFFFFF );
    b.print( "2", sfOptionColors[ 1 ], 0xFFFFFF, 2, 0, 0 );
    b.printRawRow( 0b00011000, 13, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 14, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 16, inColor, 0xFFFFFF );
    b.print( "3", sfOptionColors[ 2 ], 0xFFFFFF, 4, 0, -1 );
    b.printRawRow( 0b00011000, 20, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 21, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 23, inColor, 0xFFFFFF );
    b.print( "4", sfOptionColors[ 3 ], 0xFFFFFF, 6, 0, -2 );
    b.printRawRow( 0b00011000, 27, outColor, 0xFFFFFF );
    b.printRawRow( 0b00011000, 28, outColor, 0xFFFFFF );

    b.printRawRow( 0b00000011, 32, inColor, 0xFFFFFF );
    b.print( "5", sfOptionColors[ 4 ], 0xFFFFFF, 0, 1, 1 );
    b.printRawRow( 0b00000011, 36, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 37, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 39, inColor, 0xFFFFFF );
    b.print( "6", sfOptionColors[ 5 ], 0xFFFFFF, 2, 1, 0 );
    b.printRawRow( 0b00000011, 43, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 44, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 46, inColor, 0xFFFFFF );
    b.print( "7", sfOptionColors[ 6 ], 0xFFFFFF, 4, 1, -1 );
    b.printRawRow( 0b00000011, 50, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 51, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 53, inColor, 0xFFFFFF );

    b.print( "8", sfOptionColors[ 7 ], 0xFFFFFF, 6, 1, -2 );
    b.printRawRow( 0b00000011, 57, outColor, 0xFFFFFF );
    b.printRawRow( 0b00000011, 58, outColor, 0xFFFFFF );

    // CRITICAL FIX: Signal Core 2 to display menu with blocking PIO transfer
    // Core 1 must NEVER call leds.showBlocking() - use flag to signal Core 2
    // Without this, async DMA may drop the frame and menu won't display,
    // causing a deadlock where code waits for input on invisible menu
    showLEDsCore2 = 12;        // 12 = blocking mode, value 2 (normal display)
    waitForBlockingDisplay( ); // Wait for Core 2 to finish displaying

    int selected = -1;
    // delayWithButton(300);
    //  return 0;
    int outIn = 2;
    // Loop until GPIO selected or button pressed to exit
    // Use state-based check - doesn't consume events
    //
    // Idle-timeout backstop: if neither a button nor a valid in-range pad
    // press lands within MENU_IDLE_TIMEOUT_MS, bail out as if the user
    // had pressed the clear button. Without this, the menu could spin
    // forever if every probe tap landed in a GAP between switch cases
    // (e.g. row 9 is in neither 3..8 nor 10..15), or if probe ADC was
    // delivering noise without crossing a valid range. Symptom of that
    // bug was "GPIO menu unresponsive to probing but clear button still
    // exits" - exactly what the user reported.
    constexpr uint32_t MENU_IDLE_TIMEOUT_MS = 15000;
    uint32_t menuStartMs = millis();
    uint32_t lastReadingMs = menuStartMs;
    int lastReadingValue = -1;
    while ( selected == -1 && checkProbeButtonState( ) == 0 ) {
        jOS.serviceCritical( );
        int reading = justReadProbe( );
        if ( reading != -1 ) {
            lastReadingMs = millis();
            lastReadingValue = reading;
            switch ( reading ) {
            case 3 ... 8: {

                selected = RP_GPIO_1;
                function = RP_GPIO_1;
                if ( reading >= 2 && reading <= 4 ) {
                    outIn = 1;
                } else if ( reading >= 6 && reading <= 8 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }

                break;
            }
            case 10 ... 15: {
                selected = RP_GPIO_2;
                function = RP_GPIO_2;
                if ( reading >= 10 && reading <= 12 ) {
                    outIn = 1;
                } else if ( reading >= 14 && reading <= 15 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            case 17 ... 22: {
                selected = RP_GPIO_3;
                function = RP_GPIO_3;
                if ( reading >= 17 && reading <= 19 ) {
                    outIn = 1;
                } else if ( reading >= 21 && reading <= 22 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            case 24 ... 29: {
                selected = RP_GPIO_4;
                function = RP_GPIO_4;
                if ( reading >= 24 && reading <= 26 ) {
                    outIn = 1;
                } else if ( reading >= 28 && reading <= 29 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            case 33 ... 38: {
                selected = RP_GPIO_5;
                function = RP_GPIO_5;
                if ( reading >= 33 && reading <= 35 ) {
                    outIn = 1;
                } else if ( reading >= 37 && reading <= 38 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            case 40 ... 45: {
                selected = RP_GPIO_6;
                function = RP_GPIO_6;
                if ( reading >= 40 && reading <= 42 ) {
                    outIn = 1;
                } else if ( reading >= 44 && reading <= 45 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            case 47 ... 52: {
                selected = RP_GPIO_7;
                function = RP_GPIO_7;
                if ( reading >= 47 && reading <= 49 ) {
                    outIn = 1;
                } else if ( reading >= 51 && reading <= 52 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            case 54 ... 59: {
                selected = RP_GPIO_8;
                function = RP_GPIO_8;
                if ( reading >= 54 && reading <= 56 ) {
                    outIn = 1;
                } else if ( reading >= 58 && reading <= 59 ) {
                    outIn = 0;
                } else {
                    outIn = 2;
                }
                break;
            }
            }
        }

        // Idle-timeout backstop. If the user hasn't picked anything in
        // MENU_IDLE_TIMEOUT_MS, give up so we don't trap them in an
        // unresponsive menu. We treat ANY valid in-range tap as activity
        // (via lastReadingMs above), so this only fires for genuine
        // inactivity OR for continuous bad-range taps that never select.
        if ( (uint32_t)(millis() - lastReadingMs) > MENU_IDLE_TIMEOUT_MS ) {
            Serial.print("chooseGPIO idle timeout - last reading was ");
            Serial.println(lastReadingValue);
            Serial.flush();
            break;
        }
        // Total-runtime cap as a second backstop in case lastReadingMs
        // somehow keeps getting bumped (e.g. ADC noise consistently lands
        // in a valid range but the value picks no case).
        if ( (uint32_t)(millis() - menuStartMs) > (MENU_IDLE_TIMEOUT_MS * 2) ) {
            Serial.println("chooseGPIO total-runtime cap reached");
            Serial.flush();
            break;
        }
    }

    if ( function == -1 ) {
        return function;
    }
    if ( selected == -1 ) {
        return function;
    }
    if ( skipInputOutput == 0 && connectOrClearProbe == 1 ) {

        int gpioChosen = -1;

        for ( int i = 0; i < 10; i++ ) {
            if ( gpioDef[ i ][ 1 ] == function ) {
                gpioChosen = gpioDef[ i ][ 2 ];
                break;
            }
        }
        // Serial.print("gpioChosen (chooseGPIO): ");
        // Serial.println(gpioChosen);
        // Serial.flush();
        // switch (function) {
        //   case RP_GPIO_1 ... RP_GPIO_8: {
        //   gpioChosen = function - RP_GPIO_1 + 1;
        //   break;
        //   }
        // case 122 ... 125: {
        // gpioChosen = function - 117;
        // break;
        // }
        //}

        if ( outIn == 2 ) {
            chooseGPIOinputOutput( gpioChosen );
        } else if ( outIn == 1 ) {
            gpioState[ gpioDef[ gpioChosen ][ 2 ] ] = 0;
            // if (globalState.config.gpioDirection[gpioChosen - 1] == 0) {
            globalState.config.gpioDirection[ gpioChosen ] = 1;
            updateStateFromGPIOConfig( gpioChosen );
            // gpioState[gpioChosen] = 4;
            // updateGPIOConfigFromState();
            // configChanged = true;
            // printGPIOState();
            //  }
        } else if ( outIn == 0 ) {
            gpioState[ gpioDef[ gpioChosen ][ 2 ] ] = 4;
            // if (globalState.config.gpioDirection[gpioChosen - 1] == 1) {
            globalState.config.gpioDirection[ gpioChosen ] = 0;
            updateStateFromGPIOConfig( gpioChosen );
            // gpioState[gpioChosen] = 0;
            // updateGPIOConfigFromState();
            // configChanged = true;
            // printGPIOState();
            //}
        }

        // Serial.print("gpioChosen (chooseGPIO): ");
        // Serial.print(gpioChosen);
        // Serial.print(" outIn: ");
        // Serial.println(outIn);
        // Serial.flush();
        clearLEDsExceptRails( );
        // printConfigSectionToSerial(7);
    }
    // clearLEDsExceptRails();
    //  showNets();
    ProbeButton::getInstance( ).clearButtonState( );
    blockProbeButton = 500;
    blockProbeButtonTimer = millis( );
    showLEDsCore2 = -1;
    // updateGPIOConfigFromState();

    return function;
}

float Pads::voltageSelect( int fiveOrEight ) {
    float voltageProbe = 0.0;
    uint32_t color = 0x000000;

    // fiveOrEight = 8; // they're both 8v now
    if ( fiveOrEight == 5 && false ) {

        b.clear( );
        clearLEDsExceptRails( );

        uint8_t step = 0b0000000;
        for ( int i = 31; i <= 60; i++ ) {
            if ( ( i - 1 ) % 6 == 0 ) {
                step = step << 1;
                step = step | 0b00000001;
            }

            b.printRawRow( step, i - 1, logoColors8vSelect[ ( i - 31 ) * 2 ], 0xffffff );
        }
        // b.print("Set", scaleDownBrightness(rawOtherColors[9], 4, 22),
        //         0xFFFFFF, 1, 0, 3);
        b.print( "Set", scaleDownBrightness( rawOtherColors[ 9 ], 4, 22 ), 0xFFFFFF, 1,
                 0, 3 );
        b.print( "0v", sfOptionColors[ 7 ], 0xFFFFFF, 0, 0, -2 );
        b.print( "5v", sfOptionColors[ 7 ], 0xFFFFFF, 5, 0, 1 );
        int vSelected = -1;
        int encoderReadingPos = 45;
        rotaryDivider = 8; // one 0.1V step per physical detent (8 counts)
        while ( vSelected == -1 ) {
            jOS.serviceCritical( );
            int reading = justReadProbe( );
            // rotaryEncoderStuff();
            int encodeEdit = 0;
            if ( encoderDirectionState == UP || reading == -19 ) {
                encoderDirectionState = NONE;
                voltageProbe = voltageProbe + 0.1;
                encodeEdit = 1;
                // Serial.println(reading);

            } else if ( encoderDirectionState == DOWN || reading == -17 ) {
                encoderDirectionState = NONE;
                voltageProbe = voltageProbe - 0.1;

                encodeEdit = 1;
                // Serial.println(voltageProbe);

            } else if ( encoderButtonState == RELEASED &&
                            lastButtonEncoderState == PRESSED ||
                        reading == -10 ) {
                encodeEdit = 1;
                encoderButtonState = IDLE;
                vSelected = 10;
            }
            if ( voltageProbe < 0.0 ) {
                voltageProbe = 0.0;
            } else if ( voltageProbe > 5.0 ) {
                voltageProbe = 5.0;
            }
            // Serial.println(reading);
            if ( reading > 0 && reading >= 31 && reading <= 60 || encodeEdit == 1 ) {
                //
                b.clear( 1 );

                char voltageString[ 7 ] = " 0.0 V";

                if ( voltageProbe < 0.0 ) {
                    voltageProbe = 0.0;
                } else if ( voltageProbe > 5.0 ) {
                    voltageProbe = 5.0;
                }

                if ( encodeEdit == 0 ) {
                    voltageProbe = ( reading - 31 ) * ( 5.0 / 29 );

                } else {
                    reading = 31 + ( voltageProbe + 8.0 ) * ( 29.0 / 16.0 );
                }
                // Serial.println(voltageProbe);
                color = logoColors8vSelect[ ( reading - 31 ) * 2 ];

                snprintf( voltageString, 7, "%0.1f v", voltageProbe );
                b.print( voltageString, color, 0xFFFFFF, 0, 1, 3 );
                showLEDsCore2 = -2;
                delay( 10 );
            }
            // Check button state to exit voltage selection (state-based, doesn't consume event)
            if ( checkProbeButtonState( ) > 0 || vSelected == 10 ) {
                // Serial.println("button\n\r");

                rawSpecialNetColors[ 4 ] = color;
                rgbColor rg = unpackRgb( color );
                specialNetColors[ 4 ].r = rg.r;
                specialNetColors[ 4 ].g = rg.g;
                specialNetColors[ 4 ].b = rg.b;
                b.clear( );
                // clearLEDsExceptRails();
                // showLEDsCore2 = 1;
                if ( vSelected != 10 ) {
                    vSelected = 1;
                } else {
                    vSelected = 10;
                    Serial.println( "encoder button\n\r" );
                    delay( 500 );
                }
                // if (checkProbeButtonState() == 2) {
                //   vSelected = 10;
                // }
                vSelected = 1;
                probeButton.clearButtonState( );

                return voltageProbe;
                showLEDsCore2 = -1;
                break;
            }
        }

    } else if ( fiveOrEight == 8 || true ) { // they're both 8v now
        b.clear( );
        clearLEDsExceptRails( );

        uint8_t step = 0b00011111;
        for ( int i = 31; i <= 60; i++ ) {
            if ( ( i - 1 ) % 3 == 0 && i < 46 && i > 32 ) {
                step = step >> 1;
                step = step & 0b01111111;

            } else if ( ( i ) % 3 == 1 && i > 46 ) {
                step = step << 1;
                step = step | 0b00000001;
            }

            b.printRawRow( step, i - 1, logoColors8vSelect[ ( i - 31 ) * 2 ], 0xffffff );
        }
        // b.print("Set", scaleDownBrightness(rawOtherColors[9], 4, 22),
        //         0xFFFFFF, 1, 0, 3);
        b.print( "-8v", sfOptionColors[ 0 ], 0xFFFFFF, 0, 0, -2 );
        b.print( "+8v", sfOptionColors[ 1 ], 0xFFFFFF, 4, 0, 1 );
        int vSelected = -1;
        int encoderReadingPos = 45;
        rotaryDivider = 8; // one 0.1V step per physical detent (8 counts)

        float lastVoltageProbe = -10.0;

        while ( vSelected == -1 ) {

            jOS.serviceCritical( );
            int reading = justReadProbe( );
            rotaryEncoderStuff( );

            int encodeEdit = 0;
            if ( encoderDirectionState == UP || reading == -19 ) {
                encoderDirectionState = NONE;
                voltageProbe = voltageProbe + 0.1;
                encodeEdit = 1;
                // Serial.println(reading);

            } else if ( encoderDirectionState == DOWN || reading == -17 ) {
                encoderDirectionState = NONE;
                voltageProbe = voltageProbe - 0.1;
                encodeEdit = 1;
                // Serial.println(voltageProbe);

            } else if ( encoderButtonState == RELEASED &&
                            lastButtonEncoderState == PRESSED ||
                        reading == -10 ) {
                encodeEdit = 1;
                encoderButtonState = IDLE;
                vSelected = 10;
            }
            // Serial.println(reading);
            if ( reading > 0 && reading >= 31 && reading <= 60 || encodeEdit == 1 ) {
                //
                b.clear( 1 );

                char voltageString[ 7 ] = " 0.0 V";

                if ( voltageProbe < -8.0 ) {
                    voltageProbe = -8.0;
                } else if ( voltageProbe > 8.0 ) {
                    voltageProbe = 8.0;
                }

                if ( encodeEdit == 0 ) {
                    voltageProbe = ( reading - 31 ) * ( 16.0 / 29 );
                    voltageProbe = voltageProbe - 8.0;
                    if ( voltageProbe < 0.4 && voltageProbe > -0.4 ) {
                        voltageProbe = 0.0;
                    }
                } else {
                    reading = 31 + ( voltageProbe + 8.0 ) * ( 29.0 / 16.0 );
                }
                //

                color = logoColors8vSelect[ ( reading - 31 ) * 2 ];

                snprintf( voltageString, 7, "%0.1f v", voltageProbe );
                b.print( voltageString, color, 0xFFFFFF, 0, 1, 3 );
                showLEDsCore2 = 2;
                Serial.print( "\r                                           \r" );
                Serial.print( "DAC " );
                Serial.print( fiveOrEight ? "1:  " : "0:  " );
                Serial.print( voltageProbe, 1 );
                Serial.print( " V" );
                // delay(10);
            }

            // Check button state to exit voltage selection (state-based, doesn't consume event)
            if ( checkProbeButtonState( ) > 0 || vSelected == 10 ) {
                Serial.println( " " );

                rawSpecialNetColors[ 4 ] = color;
                rgbColor rg = unpackRgb( color );
                specialNetColors[ 4 ].r = rg.r;
                specialNetColors[ 4 ].g = rg.g;
                specialNetColors[ 4 ].b = rg.b;
                b.clear( );
                // clearLEDsExceptRails();
                // showLEDsCore2 = 1;
                if ( vSelected != 10 ) {
                    vSelected = 1;
                } else {
                    vSelected = 10;
                    // Serial.println("encoder button\n\r");
                    // delay(500);
                }
                vSelected = 1;
                showLEDsCore2 = -1;
                probeButton.clearButtonState( );
                blockProbeButton = 2000;
                blockProbeButtonTimer = millis( );
                return voltageProbe;
                break;
            }
        }
    }

    blockProbeButton = 2000;
    blockProbeButtonTimer = millis( );
    // Serial.println(" ");
    return 0.0;
}

// Track when LED was last updated to allow current to stabilize
// Must be declared before checkSwitchPosition() which uses it

// ============================================================================
// longShortPress
// (copied from the legacy engine, class renamed; keep in sync manually)
// ============================================================================

int Pads::longShortPress( int pressLength ) {
    // Rewritten to use state-based API (doesn't consume button events)
    // Returns: -1 = no press, 1 = short remove press, 2 = short connect press,
    //          3 = long remove press, 4 = long connect press
    return -1;
    unsigned long clickTimer = millis( );

    // Wait for initial button press (check state, don't consume event)
    int initialState = probeButton.getButtonState( );
    if ( initialState == 0 ) {
        return -1; // No button currently pressed
    }

    // Button is pressed - track which button it was
    int whichButton = initialState; // 1=remove, 2=connect

    // Wait to see if it's held for pressLength duration
    while ( millis( ) - clickTimer < pressLength ) {
        int currentState = probeButton.getButtonState( );

        // If button released before timeout, it's a short press
        if ( currentState == 0 ) {
            // Serial.print("Short press detected: ");
            // Serial.println(whichButton);
            return whichButton; // Return 1 or 2 for short press
        }

        delay( 5 );
    }

    // Button held for full duration - it's a long press
    // Return 3 for long remove, 4 for long connect
    int longPressCode = ( whichButton == 1 ) ? 3 : 4;
    // Serial.print("Long press detected: ");
    // Serial.println(longPressCode);

    // Wait for button release to avoid repeated detection
    while ( probeButton.getButtonState( ) != 0 ) {
        delay( 5 );
    }

    return longPressCode;
}
