#include "Probing.h"
#include "CH446Q.h"
#include "FileParsing.h"
#include "InfraPaths.h"
#include "JumperlOS.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "States.h"
// #include "AdcUsb.h"
#include "Commands.h"
#include "Graphics.h"
#include "PersistentStuff.h"
#include "RotaryEncoder.h"
#include "ReadingDisplay.h"
// gpio_coproc.h is RP2350-only (the single-cycle GPIO coprocessor). It
// hard-#errors on RP2040, so include it only on RP2350.
#if defined(PICO_RP2350)
#include "hardware/gpio_coproc.h"
#endif
#include "hardware/pio.h"

// Portable output-enable toggles. V5 uses the RP2350 coprocessor's
// single-cycle gpioc_bit_oe_set/clr; RP2040 (OG) uses the normal direction
// API (the pin is already switched to SIO before these are called).
static inline void probeOeClr( uint pin ) {
#if defined(PICO_RP2350)
    gpioc_bit_oe_clr( pin );
#else
    gpio_set_dir( pin, false );
#endif
}
static inline void probeOeSet( uint pin ) {
#if defined(PICO_RP2350)
    gpioc_bit_oe_set( pin );
#else
    gpio_set_dir( pin, true );
#endif
}
#include "hardware/pwm.h"
#include "pico.h"
#include "pico/stdlib.h"

#include <EEPROM.h>
// #include <FastLED.h>
#include "ArduinoStuff.h"
#include <algorithm>

#include "Highlighting.h"
#include "Menus.h" // clickMenu() - short-click exit from probeMode reopens the menu
#include "PersistentStuff.h"
#include "Python_Proper.h"
#include "Undo.h"
#include "WaveGen.h" // wavegen.isRunning() - shared I2C0 bus arbitration
extern WaveGen wavegen; // defined in main.cpp
#include "config.h"
#include "externVars.h"
#include "oled.h"
#include "USBAudio.h" // usb_audio_probe_activity()

// Button timing constants
#define BUTTON_SETTLE_US 8
#define BUTTON_SETTLE_SHORT_US 4

// ============================================================================
// ProbeButton Implementation (class declared in Probing.h)
// High-frequency button checking service for instant response
// ============================================================================

// Set by ProbeButton::service when a double-tap fires undo/redo. Read +
// cleared at the top of probeMode and inside its toggle branch to
// short-circuit before any "connect"/"clear" repaint clobbers the toast.
volatile bool g_probeDoubleTapBail = false;

// Static member initialization
ProbeButton* ProbeButton::instance = nullptr;

ProbeButton& ProbeButton::getInstance( ) {
    if ( instance == nullptr ) {
        instance = new ProbeButton( );
    }
    return *instance;
}

ProbeButton::ProbeButton( ) {
    // Initialize button state
}



/**
 * @brief High-frequency service method - checks button hardware with new blocking behavior
 *
 * New behavior:
 * 1. Checks hardware frequently (respects rate limiting)
 * 2. When press detected: blocks subsequent presses for blockDurationMs (default 1s)
 * 3. Block clears when: button released OR block timer expires
 * 4. Quick successive clicks register (each release clears block)
 * 5. Holding button registers once (block prevents re-trigger)
 * 6. Tracks continuous hold time and sets CONNECT_HELD/REMOVE_HELD flags
 */
// Enum declared up front so service() can name PIOProbeState::READY.
// The actual state variable definitions live with the rest of the PIO
// probe-button code lower in the file.
enum class PIOProbeState : uint8_t {
    UNTRIED     = 0,  // never tried to init
    READY       = 1,  // init succeeded, can use PIO path
    UNAVAILABLE = 2,  // init failed (no PIO mem / probeLEDs not ready), never retry
};
extern volatile PIOProbeState g_pioProbeState;
extern volatile bool          g_pendingUndo;
extern volatile bool          g_pendingRedo;
extern volatile const char*   g_pendingUndoLabel;
extern volatile const char*   g_pendingRedoLabel;
extern volatile int           g_pendingUndoSplit;
extern volatile int           g_pendingRedoSplit;

ServiceStatus ProbeButton::service( ) {
    lastStatus = ServiceStatus::IDLE;
    extern struct config jumperlessConfig;

    // -------------------------------------------------------------------
    // 0. Handle a runtime PIO/CPU mode toggle from the debug menu.
    //    The continuous-polling SM is always-on once we init - if the
    //    user flips use_pio_probe_button to false, we need to PAUSE
    //    polling (and disable the IRQ) so the CPU bit-bang in
    //    checkProbeButtonHardware() doesn't fight the PIO for the line.
    //    Flipping it back resumes polling.
    // -------------------------------------------------------------------
    static bool s_lastPIOFlag = false;
    bool currentFlag = jumperlessConfig.hardware.use_pio_probe_button;
    if ( currentFlag != s_lastPIOFlag ) {
        if ( g_pioProbeState == PIOProbeState::READY ) {
            extern void probeButtonPausePolling( void );
            extern void probeButtonResumePolling( void );
            if ( currentFlag ) probeButtonResumePolling( );
            else               probeButtonPausePolling( );
        }
        s_lastPIOFlag = currentFlag;
    }

    // -------------------------------------------------------------------
    // 1. Drain deferred undo/redo work posted by the PIO IRQ handler.
    //    The handler can't safely run these itself - they touch global
    //    state, hardware, and may print to Serial - so it stashes the
    //    intent here and we execute it in main-loop context.
    // -------------------------------------------------------------------
    if ( g_pendingUndo ) {
        g_pendingUndo = false;
        const char* lbl = (const char*)g_pendingUndoLabel;
        int lblSplit = g_pendingUndoSplit;
        g_pendingUndoLabel = nullptr;
        g_pendingUndoSplit = -1;
        if ( undoCanUndo( ) && undoUndo( ) ) {
            undoToast( false, lbl, lblSplit );
            // Signal probeMode (if it just started) to fast-return so
            // we don't paint "clear nodes" / "connect nodes" on top of
            // the undo toast. probeMode's bail-check is gated on its
            // own entry timestamp so this only triggers a return when
            // the double-tap landed inside the entry window. Past that
            // window, probeMode clears the flag and keeps running.
            g_probeDoubleTapBail = true;
        }
        lastStatus = ServiceStatus::BUSY;
    }
    if ( g_pendingRedo ) {
        g_pendingRedo = false;
        const char* lbl = (const char*)g_pendingRedoLabel;
        int lblSplit = g_pendingRedoSplit;
        g_pendingRedoLabel = nullptr;
        g_pendingRedoSplit = -1;
        if ( undoCanRedo( ) && undoRedo( ) ) {
            undoToast( true, lbl, lblSplit );
            g_probeDoubleTapBail = true;
        }
        lastStatus = ServiceStatus::BUSY;
    }

    // -------------------------------------------------------------------
    // 2. If PIO continuous polling is active, the IRQ handler has been
    //    calling processSample() for every PIO sample at ~1 ms cadence.
    //    All press/release/double-tap state is already up to date.
    //    Nothing for us to do here except the deferred drain above.
    // -------------------------------------------------------------------
    if ( jumperlessConfig.hardware.use_pio_probe_button &&
         g_pioProbeState == PIOProbeState::READY ) {
        return lastStatus;
    }

    // -------------------------------------------------------------------
    // 3. CPU fallback path (or PIO init still pending). Rate-limited
    //    hardware read, then run the shared state machine on the
    //    resulting sample.
    // -------------------------------------------------------------------
    unsigned long now = millis( );
    if ( now - lastCheckTime < ( switchPosition == 0 ? checkIntervalMsMeasure : checkIntervalMsSelect ) ) {
        return lastStatus;
    }
    lastCheckTime = now;

    int newState = checkProbeButtonHardware( );
    processSample( (uint8_t)newState );
    return lastStatus;
}

// =====================================================================
// processSample - shared state-machine body used by both service()
// (CPU path) and the PIO IRQ handler. Marked __not_in_flash_func so
// invocation from IRQ context can't trigger a flash cache miss.
//
// IRQ-SAFE: only updates ProbeButton fields and the pending-undo/redo
// flags. Does NOT call undoUndo/undoRedo/undoToast/Serial.printf.
// =====================================================================
void __not_in_flash_func( ProbeButton::processSample )( uint8_t newStateRaw ) {
    int newState = (int)newStateRaw;
    unsigned long now = millis( );

    // ========================================================================
    // BUTTON RELEASED - Clear state with debounce protection
    // ========================================================================
    if ( newState == 0 ) {
        // Track the *first* release sample so the bounce filter measures
        // sustained-released time, not time-since-press. The old code
        // gated block-clear on (now - blockStartTime), where blockStartTime
        // was set when the press was registered. That meant a fast click
        // (press shorter than minimumBlockMs) couldn't ever clear the
        // block - the second tap of a quick double-tap then landed while
        // isBlocked was still true and the rising edge was silently
        // dropped, which is exactly the "missed double-click" failure mode.
        bool justReleased = ( currentButtonState != 0 );
        if ( justReleased ) {
            releaseStartTime = now;
        }

        blockProbeButton = 0;
        blockProbeButtonTimer = 0;
        lastButtonState = currentButtonState;
        currentButtonState = 0;
        buttonChanged = true;
        lastStatus = ServiceStatus::BUSY;

        if ( justReleased && probe_button_trace ) {
            Serial.printf( "[%lu] PROBE  %d -> 0  sample #%lu  raw=%u\n",
                           (unsigned long)now, (int)lastButtonState,
                           (unsigned long)probeButtonPIOReadCount,
                           (unsigned int)probeButtonPIOLastResult );
        }

        // Drop any latched hold state on release - hold counters always
        // measure the current continuous press, not aggregate time.
        connectHeld = false;
        removeHeld = false;
        connectHoldTime = 0;
        removeHoldTime = 0;
        pressStartTime = 0;

        // Bounce filter: require the release to be sustained for
        // releaseDebounceMs before clearing the press-block AND before
        // marking the release "confirmed". A shorter float is treated as a
        // contact bounce / mid-press glitch inside an ongoing press, so it
        // doesn't clear the block early and doesn't confirm a release - which
        // is what lets the double-tap detector distinguish a genuine second
        // tap (preceded by a real release) from a mid-press glitch edge (see
        // the releaseConfirmed/allowDoubleTap gate in press detection). Note
        // isBlocked also clears unconditionally via the block timer in the
        // pressed branch, so a noisy release can never wedge it permanently.
        if ( releaseStartTime > 0 && ( now - releaseStartTime ) >= releaseDebounceMs ) {
            releaseConfirmed = true;
            if ( isBlocked ) {
                isBlocked = false;
                blockStartTime = 0;
                releaseStartTime = 0;
            }
        }

        return;
    } else {
        // We saw a pressed sample - any prior release was transient,
        // so reset the release timer. This makes releaseDebounceMs an
        // honest "released for N ms continuously" gate.
        releaseStartTime = 0;
    }

    // ========================================================================
    // BUTTON PRESSED - Handle blocking, press detection, and hold tracking
    // ========================================================================

    // Check if block timer has expired
    if ( isBlocked && ( now - blockStartTime >= blockDurationMs ) ) {
        isBlocked = false;
        blockStartTime = 0;
    }

    // Detect state changes
    bool stateChanged = ( newState != currentButtonState );

    if ( stateChanged ) {
        lastButtonState = currentButtonState;
        currentButtonState = newState;
        buttonChanged = true;
        lastStatus = ServiceStatus::BUSY;
        noteUserInput( );  // any button transition counts as user activity

        // Trace state transitions only (NOT every sample) - keeps the
        // print rate bounded even when processSample is called from the
        // PIO IRQ at ~1 kHz. Real human button activity is well under
        // 10 Hz so this is comfortable for Serial.
        if ( probe_button_trace ) {
            Serial.printf( "[%lu] PROBE  %d -> %d  sample #%lu  raw=%u\n",
                           (unsigned long)now,
                           (int)lastButtonState, newState,
                           (unsigned long)probeButtonPIOReadCount,
                           (unsigned int)probeButtonPIOLastResult );
        }
    } else {
        buttonChanged = false;
    }

    // ========================================================================
    // PRESS DETECTION - Register press event if not blocked
    // ========================================================================
    if ( !isBlocked ) {
        // Register a press event when:
        // 1. Transitioning from RELEASED (0) to any PRESSED (1 or 2)
        // 2. Switching between different button types (1↔2)
        if ( stateChanged && newState > 0 &&
             ( lastButtonState == 0 || ( lastButtonState > 0 && lastButtonState != newState ) ) ) {

            // -- Fast double-click undo/redo detection --------------------
            // Per-button rolling 2-press timestamp ring. Double fires when
            // the previous press is within DOUBLE_WINDOW_MS of this one.
            //
            // IMPORTANT: the double-tap (and only the double-tap) is gated on
            // releaseConfirmed - i.e. a debounce-confirmed sustained release
            // must have occurred between the two presses. A transient
            // mid-press float momentarily reads as released (newState==0)
            // then snaps back to the same button, creating a second press
            // edge a few hundred ms after the first; without this gate that
            // glitch pairs with the real press and fires a spurious undo
            // (the exact symptom we're fixing). We deliberately do NOT gate
            // basic press registration on releaseConfirmed - doing so would
            // wedge press detection on a noisy line that never delivers a
            // clean 50ms release window. Worst case on a noisy line we just
            // skip double-tap detection, never single-press registration.
            bool allowDoubleTap = releaseConfirmed;
            // Consume the confirmed-release token: the NEXT press won't count
            // as a double-tap until another sustained release is observed.
            releaseConfirmed = false;
            //
            // The actual undoUndo/undoRedo invocation runs in service()
            // main-loop context (not from IRQ) - we just stash the intent
            // in the pending-* flags + labels. service() drains them and
            // (the important bit) sets g_probeDoubleTapBail so probeMode
            // can detect "an undo just fired" and choose between:
            //   - fast-return if probeMode was JUST entered (entry window)
            //   - stay-alive if the user is already deep in probeMode and
            //     the double-tap is a real undo gesture
            // See probeMode's bail handling for the cutoff.
            //
            // Click 2's buttonPress is suppressed (dbl branch) so the second
            // tap doesn't trigger any in-probe action (mode switch, clear
            // nodesToConnect) - it's exclusively a double-tap gesture.
            static uint32_t connectClicks[ 2 ] = { 0, 0 };
            static uint32_t disconnectClicks[ 2 ] = { 0, 0 };
            constexpr uint32_t DOUBLE_WINDOW_MS = ProbingDoubleTap::kWindowMs;
            uint32_t* hist      = ( newState == 2 ) ? connectClicks    : disconnectClicks;
            uint32_t* otherHist = ( newState == 2 ) ? disconnectClicks : connectClicks;
            otherHist[ 0 ] = otherHist[ 1 ] = 0;

            bool dbl = false;
            if ( allowDoubleTap ) {
                hist[ 0 ] = hist[ 1 ];
                hist[ 1 ] = now;
                dbl = ( hist[ 0 ] != 0 ) && ( ( hist[ 1 ] - hist[ 0 ] ) <= DOUBLE_WINDOW_MS );
            } else {
                // No confirmed release since the last press - this edge is a
                // glitch/bounce, not a genuine second tap. Drop the history
                // so it can't later pair with a real press into a false double.
                hist[ 0 ] = hist[ 1 ] = 0;
            }

            if ( dbl ) {
                // Defer the heavy work (undoUndo/undoRedo + undoToast)
                // to main-loop context via the pending-* flags. Capture
                // the label NOW so it reflects the state at click time.
                if ( newState == 2 ) {
                    if ( undoCanRedo( ) ) {
                        g_pendingRedoLabel = undoPeekRedoLabel( );
                        g_pendingRedoSplit = undoLabelSplitAt( +1 );
                        g_pendingRedo = true;
                    }
                } else if ( newState == 1 ) {
                    if ( undoCanUndo( ) ) {
                        g_pendingUndoLabel = undoPeekUndoLabel( );
                        g_pendingUndoSplit = undoLabelSplitAt( 0 );
                        g_pendingUndo = true;
                    }
                }
                // Swallow click 2: don't propagate as a press event. The
                // first click already entered/triggered probeMode; we
                // don't want the second tap to also count as a press.
                buttonPress = 0;
                // Reset history to prevent a third click within the
                // window from triggering a stale match.
                hist[ 0 ] = hist[ 1 ] = 0;
            } else {
                // Single press - propagate to consumers. The press will sit
                // in buttonPress until either click 2 arrives (dbl branch
                // above clears it) or some consumer (handleProbeButtonActions
                // / readProbe::checkProbeButton) picks it up.
                buttonPress = newState;
            }

            // Start block period
            isBlocked = true;
            blockStartTime = now;
            pressStartTime = now;

            // Set old global blocking variables for backward compatibility
            blockProbeButton = blockDurationMs;
            blockProbeButtonTimer = now;

            lastStatus = ServiceStatus::BUSY;
        }
    }

    // ========================================================================
    // HOLD TRACKING - update continuous hold duration + latched threshold
    // flag. No code currently consumes these but the bookkeeping is cheap
    // and keeps the API surface ready for future gestures.
    // ========================================================================
    if ( currentButtonState > 0 && pressStartTime > 0 ) {
        unsigned long holdDuration = now - pressStartTime;
        if ( currentButtonState == 2 ) {
            connectHoldTime = holdDuration;
            if ( !connectHeld && holdDuration >= connectHoldThresholdMs ) {
                connectHeld = true;
                lastStatus = ServiceStatus::BUSY;
            }
        } else if ( currentButtonState == 1 ) {
            removeHoldTime = holdDuration;
            if ( !removeHeld && holdDuration >= removeHoldThresholdMs ) {
                removeHeld = true;
                lastStatus = ServiceStatus::BUSY;
            }
        }
    }
}

/**
 * @brief Get button press event (consumes the event if consume is true)
 */
int ProbeButton::getButtonPress( bool consume ) {
    int press = buttonPress;

    if ( press != 0 ) {
        blockProbeButton = 300;
        blockProbeButtonTimer = millis( );
    }

    if ( consume ) {
        buttonPress = 0; // Clear after reading
    }
    return press;
}

// ============================================================================
// PIO-based probe button reader (shares state machine with probe LED's WS2812)
//
// Hardware context: PROBE_LED_PIN (GPIO 2) is the WS2812 data line for the
// probe LED. The TRRS probe cable shorts GPIO 2 to BUTTON_PIN (GPIO 9) so
// they act as a single net; either pin reads/drives the shared wire. (A
// TRRRS cable would separate them - see .agents/memory.instruction.md.)
// The two probe buttons differ in which rail (+3V3 via PROBE_PIN, or GND)
// they pull the line toward through low-impedance switches.
//
// To discriminate connect / disconnect / no-button, we briefly drive the
// line to a known rail, release to HiZ, then sample: a closed switch snaps
// the line back to its tied rail in nanoseconds; a floating (no-press)
// line stays where parasitic capacitance left it. Repeat with the opposite
// drive direction to disambiguate, and the two samples decode as:
//   B1 B2 → meaning
//   1  1  → snapped HIGH both phases → connect-button (rev <4) / remove (rev >=4)
//   0  0  → snapped LOW  both phases → the other button
//   0  1  → no snap, line held by C  → floating, no press
//
// MODE OF OPERATION (this revision):
// The SM runs the button polling program continuously in the background
// at ~1 ms cadence. Each sample pushes to the RX FIFO and raises PIO
// IRQ flag 0. A CPU IRQ handler drains the FIFO, decodes, and runs the
// press/release/double-tap state machine directly - so button events
// are processed at the PIO's polling rate, NOT at the (variable, often
// 20-200 ms) main-loop service() rate that was missing fast clicks.
// When an LED show wants the line, probeButtonPausePolling() jumps the
// SM to the WS2812 program; probeButtonResumePolling() switches it back.
// The previous "trigger from CPU + wait for FIFO" mode is gone - this
// program produces a strict superset of its behaviour.
// ============================================================================

// Hand-encoded PIO instructions (pio_encode_* helpers are static inline,
// not constexpr, so they can't be used in a const array initializer).
// .side_set 1 means bit 12 is the side-set value (controls pin LATCH, not
// OE - we toggle OE via SET PINDIRS). Bits 11:8 are delay 0..15.
//
// NOTE on the shared-LED flicker (probe_led_on_button_pin): the ~875ns
// drive-high sample phase is a valid WS2812 data bit, and probeLEDhandler
// shows on every core-2 pass, so the sampler and the LED interact. Two
// fixes were tried and BOTH made the flicker worse on real hardware:
//   1. fast-clock sampler (divider 1, ~113ns sub-bit pulse, 12mA drive) -
//      hard edges ring on the unterminated probe cable into extra bits;
//   2. resume-entry latch guard (~386us delay after each show before the
//      first sample pulse, so it can't append to the un-latched frame).
// Reverted to the original timing below; revisit with a scope on the line.
//
// Program structure:
//   offset 0..8 : sample sequence (drive low / release / IN / drive high /
//                 release / IN / push / IRQ)
//   offset 9..13: ~1 ms delay loop using X and Y as nested counters
//   offset 14   : jump back to offset 0
static const uint16_t probe_button_pio_instructions[] = {
    0xe381u, // 0:  set    pindirs, 1  side 0 [3] ; OE=1, drive line LOW ~500ns
    0xe180u, // 1:  set    pindirs, 0  side 0 [1] ; OE=0 (HiZ), settle ~250ns
    0x4001u, // 2:  in     pins, 1     side 0     ; sample 1 (post drive-low)
    0xf381u, // 3:  set    pindirs, 1  side 1 [3] ; OE=1, drive line HIGH ~500ns
    0xf180u, // 4:  set    pindirs, 0  side 1 [1] ; OE=0 (HiZ), settle
    0x5001u, // 5:  in     pins, 1     side 1     ; sample 2 (post drive-high)
    0xe081u, // 6:  set    pindirs, 1  side 0     ; restore OE=1, latch pin LOW
    0x8000u, // 7:  push   noblock     side 0     ; push 2-bit result, clears ISR
    0xc000u, // 8:  irq    set 0       side 0     ; signal CPU
    0xe02fu, // 9:  set    x, 15       side 0     ; outer delay counter (16 iters)
    0xe05fu, // 10: set    y, 31       side 0     ; inner delay counter (32 iters)
    0xaf42u, // 11: nop                side 0 [15]; ~16 cycles per inner iter
    0x008bu, // 12: jmp    y--, 11     side 0     ; loop inner
    0x004au, // 13: jmp    x--, 10     side 0     ; loop outer (reload Y)
    0x0000u, // 14: jmp    0           side 0     ; restart sample sequence
};

static const pio_program_t probe_button_pio_program = {
    .instructions = probe_button_pio_instructions,
    .length       = sizeof( probe_button_pio_instructions ) / sizeof( probe_button_pio_instructions[ 0 ] ),
    .origin       = -1,
};

// Cached lazy-init state for the PIO probe button reader. We can't init
// at static-init time because probeLEDs.begin() runs later in setup() and
// has to claim its PIO/SM first. Instead we init on the first call that
// finds use_pio_probe_button enabled and probeLEDs ready.
//
// PIOProbeState is declared up at the top of the file so service() can
// see PIOProbeState::READY directly.
volatile PIOProbeState g_pioProbeState   = PIOProbeState::UNTRIED;
PIO                    g_pioProbePIO     = nullptr;
uint                   g_pioProbeSM      = 0;
uint                   g_pioProbeBtnPC   = 0;  // start of button polling program
uint                   g_pioProbeLedPC   = 0;  // start of WS2812 program
int                    g_pioProbeIrqNum  = -1; // NVIC IRQ number for the PIO IRQ line

// Concurrency model for the shared probe SM:
//
// The PIO IRQ is owned by core0 (initProbeButtonPIO runs from jOS.serviceAll
// on core0), while the SM-mode swap runs on core1 (probeLEDhandler ->
// core2stuff). The ORIGINAL bug was that probeButtonResumePolling re-enabled
// the IRQ via irq_set_enabled, which is per-core on RP2350 - so it leaked the
// IRQ enable onto core1 and let processSample() run on BOTH cores at once
// (the source of the phantom presses / spurious undos / extra LED flicker).
//
// The fix is simply to keep the IRQ owned by core0 ONLY: pause/resumePolling
// no longer touch the NVIC. We intentionally do NOT add a spinlock or a
// showingProbeLEDs gate to the handler: probeLEDhandler() spins in a tight
// loop on core1, so any "bail while a show owns the line" guard would bail on
// nearly every sample and starve button events. The brief (microsecond) SM
// swap can race a core0 FIFO read, but the handler's pio_sm_is_rx_fifo_empty()
// check makes a stray read a no-op, and during an actual WS2812 show the
// button program isn't running so flag 0 never fires.

// Latest decoded state pushed by the PIO IRQ handler. Read by legacy
// callers (checkProbeButtonHardware()) and the debug menu. Volatile +
// uint8_t = atomic single-byte reads/writes on the M33, no lock needed.
volatile uint8_t  probeBtnLatestState = 0;

// Counters / diagnostics for the debug menu and trace output. Updated
// from the IRQ handler so they're approximately monotonic.
volatile uint32_t probeButtonPIOReadCount    = 0; // total samples processed by IRQ
volatile uint32_t probeButtonCPUReadCount    = 0; // CPU bit-bang reads
volatile uint32_t probeButtonPIOTimeoutCount = 0; // unused in IRQ mode (kept for menu compat)
volatile uint32_t probeButtonPIOLastResult   = 0; // last raw 2-bit value from PIO
volatile uint32_t probeButtonPIOLastUs       = 0; // microseconds since boot of last IRQ
volatile uint32_t probeButtonCPULastUs       = 0; // wall-time of last CPU read, microseconds

// Verbose tracing toggled from the debug menu.
volatile int probe_button_trace = 0;

// Deferred-action flags: the IRQ handler can't safely run undoUndo/Redo
// (those touch a lot of state, may print to Serial, may refresh hardware
// connections), so it just records what to do and the main-loop service()
// drains these. File-scope (not static) so the forward declarations at
// the top can reference them.
volatile bool        g_pendingUndo       = false;
volatile bool        g_pendingRedo       = false;
volatile const char* g_pendingUndoLabel  = nullptr;
volatile const char* g_pendingRedoLabel  = nullptr;
volatile int         g_pendingUndoSplit  = -1;
volatile int         g_pendingRedoSplit  = -1;

// =====================================================================
// PIO IRQ handler: drains the RX FIFO, decodes each 2-bit sample into
// a button state, then runs the ProbeButton state machine directly.
// Marked __not_in_flash_func so it lives in RAM - the handler can fire
// at ~1 kHz and we don't want flash cache misses adding jitter.
// =====================================================================
static void __not_in_flash_func( probe_button_pio_irq_handler )( void ) {
    PIO pio = g_pioProbePIO;
    uint sm = g_pioProbeSM;

    if ( pio == nullptr ) {
        return; // race during teardown
    }

    // NOTE: we deliberately do NOT gate this handler on showingProbeLEDs or
    // a spinlock. probeLEDhandler() runs in a tight loop on core1 (the final
    // else-if of core2stuff between scheduler ticks), so "bail while an LED
    // show owns the line" would bail on essentially every sample and drop
    // all button events. The shared SM is safe to read here anyway: during a
    // WS2812 show the button program isn't running so flag 0 never fires, and
    // the only overlap is core1's microsecond SM-mode swap - against which the
    // pio_sm_is_rx_fifo_empty() guard below makes a stray read a no-op. The
    // real concurrency fix is upstream: the IRQ is owned by core0 ONLY (we no
    // longer let resumePolling re-enable it on core1), so processSample()
    // never runs on two cores at once.
    extern struct config jumperlessConfig;
    int probeRev = jumperlessConfig.hardware.probe_revision;
    ProbeButton& self = ProbeButton::getInstance( );

    while ( !pio_sm_is_rx_fifo_empty( pio, sm ) ) {
        uint32_t raw = pio_sm_get( pio, sm );
        int s1 = (int)( ( raw >> 30 ) & 1u );
        int s2 = (int)( ( raw >> 31 ) & 1u );

        uint8_t newState;
        if ( s1 == 1 && s2 == 1 )      newState = ( probeRev >= 4 ) ? 2 : 1;
        else if ( s1 == 0 && s2 == 0 ) newState = ( probeRev >= 4 ) ? 1 : 2;
        else                            newState = 0;

        // Update raw-state cache + diagnostics for legacy readers.
        probeBtnLatestState        = newState;
        probeButtonPIOReadCount++;
        probeButtonPIOLastResult   = ( (uint32_t)s2 << 1 ) | (uint32_t)s1;

        // Run the press/release/double-tap state machine on this sample.
        self.processSample( newState );
    }

    probeButtonPIOLastUs = time_us_32( );

    // Acknowledge the PIO IRQ flag so it can re-fire.
    pio_interrupt_clear( pio, 0 );
}

// Switch the SM to WS2812 mode for an LED show. Called by
// probeLEDhandler before showBlocking(). Idempotent if PIO mode
// isn't active.
void probeButtonPausePolling( void ) {
    if ( g_pioProbeState != PIOProbeState::READY ) return;
    PIO  pio = g_pioProbePIO;
    uint sm  = g_pioProbeSM;

    // NOTE: do NOT toggle the NVIC IRQ here. This function runs on core1
    // (probeLEDhandler -> core2stuff), but the PIO IRQ is owned by core0.
    // irq_set_enabled is per-core, so disabling here would not mask the
    // core0 handler anyway, and (the original bug) the matching enable in
    // resumePolling would LEAK the IRQ enable onto core1, letting both cores
    // run processSample concurrently. Keeping the IRQ owned solely by core0
    // is the actual fix. The brief SM-mode swap below can race a core0 IRQ
    // read, but the handler's pio_sm_is_rx_fifo_empty() guard makes that a
    // no-op, so no lock is needed.
    pio_sm_set_enabled( pio, sm, false );

    // Drain any pending RX samples the IRQ handler hasn't processed
    // (so we don't decode stale data after the LED show).
    while ( !pio_sm_is_rx_fifo_empty( pio, sm ) ) {
        (void)pio_sm_get( pio, sm );
    }
    pio_sm_clear_fifos( pio, sm );
    pio_sm_restart( pio, sm );

    // Jump to WS2812 wrap_target. SM will stall on autopull (TX FIFO
    // empty) until showBlocking() starts loading data.
    pio_sm_exec( pio, sm, pio_encode_jmp( g_pioProbeLedPC ) );
    pio_sm_set_enabled( pio, sm, true );
}

// Switch the SM back to continuous button polling after an LED show.
void probeButtonResumePolling( void ) {
    if ( g_pioProbeState != PIOProbeState::READY ) return;
    PIO  pio = g_pioProbePIO;
    uint sm  = g_pioProbeSM;

    // See probeButtonPausePolling: the IRQ stays owned by core0 only; we do
    // NOT re-enable it here (the original cross-core enable was the bug).
    pio_sm_set_enabled( pio, sm, false );
    pio_sm_clear_fifos( pio, sm );
    pio_sm_restart( pio, sm );
    pio_sm_exec( pio, sm, pio_encode_jmp( g_pioProbeBtnPC ) );
    pio_sm_set_enabled( pio, sm, true );

    pio_interrupt_clear( pio, 0 );
}

// =====================================================================
// Core0-safe pause/resume for the shared button/LED state machine.
//
// probeButtonPausePolling/ResumePolling are written for core1, which is
// the SM's owner: probeLEDhandler() runs pause -> showBlocking -> resume
// on essentially every core2stuff pass. If core0 calls the raw functions
// directly (self-test sampling windows), its resume can land while core1
// is inside showBlocking(): the SM jumps back to the button program mid
// WS2812 stream, the button program never drains the TX FIFO, and core1
// spins forever in showBlocking's drain wait - LEDs freeze and
// sendAllPathsCore2 is never processed again (hardware-confirmed wedge
// during the self-test crossbar phase).
//
// The safe pattern parks core1's handler FIRST using the gate it already
// honors: core2stuff only calls probeLEDhandler when checkingButton == 0,
// and the handler asserts showingProbeLEDs across its pause/show/resume
// critical section. So: raise checkingButton, wait out any in-flight
// show, and only then touch the SM. Callers MUST pair with
// probeButtonResumePollingFromCore0(), which releases the gate.
// =====================================================================
void probeButtonPausePollingFromCore0( void ) {
    checkingButton = 1; // core2stuff stops entering probeLEDhandler
    // Cover the gap where core1 passed the call-site check just before the
    // write above but hasn't asserted showingProbeLEDs yet (the switch body
    // between the two is a handful of RAM writes - microseconds).
    delay( 1 );
    unsigned long start = millis( );
    while ( showingProbeLEDs != 0 && millis( ) - start < 20 ) {
        delayMicroseconds( 50 ); // wait out an in-flight showBlocking
    }
    probeButtonPausePolling( );
}

void probeButtonResumePollingFromCore0( void ) {
    probeButtonResumePolling( );
    checkingButton = 0; // release core1's probeLEDhandler
}

// Lazily set up the shared-SM button program + IRQ. Returns true if the
// PIO path is usable. Safe to call repeatedly; only does work the first
// successful time.
static bool initProbeButtonPIO( void ) {
    if ( g_pioProbeState == PIOProbeState::READY )       return true;
    if ( g_pioProbeState == PIOProbeState::UNAVAILABLE ) return false;

    PIO pio = probeLEDs.getPIO( );
    int sm  = probeLEDs.getStateMachine( );
    if ( pio == nullptr || sm < 0 ) {
        // probeLEDs.begin() hasn't claimed yet - try again next call.
        return false;
    }

    if ( !pio_can_add_program( pio, &probe_button_pio_program ) ) {
        Serial.println( "\n\rprobe button PIO: no instruction memory, falling back to CPU path\n\r" );
        Serial.flush( );
        g_pioProbeState = PIOProbeState::UNAVAILABLE;
        return false;
    }

    g_pioProbePIO   = pio;
    g_pioProbeSM    = (uint)sm;
    g_pioProbeLedPC = probeLEDs.getProgramOffset( );
    g_pioProbeBtnPC = pio_add_program( pio, &probe_button_pio_program );

    // The probe LED SM was init'd with FIFO_JOIN_TX (giving it an 8-deep
    // TX FIFO with no RX FIFO). The button program needs to PUSH to the
    // RX FIFO, so we have to unjoin. The probe LED only has 1 pixel
    // (3 bytes), so a 4-deep TX FIFO is plenty.
    int pin = probeLEDs.getPin( );

    pio_sm_set_enabled( pio, (uint)sm, false );
    while ( !pio_sm_is_tx_fifo_empty( pio, (uint)sm ) ) {
        tight_loop_contents( );
    }
    pio_sm_clear_fifos( pio, (uint)sm );

    // SHIFTCTRL: clear FJOIN_TX / FJOIN_RX bits (unjoin to standard 4+4).
    hw_clear_bits( &pio->sm[ sm ].shiftctrl,
                   PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS | PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS );

    // PINCTRL: add SET_BASE=pin/SET_COUNT=1 and IN_BASE=pin. Preserve
    // existing SIDESET/OUT config that WS2812 init set up.
    uint32_t pinctrl = pio->sm[ sm ].pinctrl;
    pinctrl &= ~( PIO_SM0_PINCTRL_SET_BASE_BITS
                | PIO_SM0_PINCTRL_SET_COUNT_BITS
                | PIO_SM0_PINCTRL_IN_BASE_BITS );
    pinctrl |= ( (uint32_t)pin << PIO_SM0_PINCTRL_SET_BASE_LSB )
             | ( (uint32_t)1   << PIO_SM0_PINCTRL_SET_COUNT_LSB )
             | ( (uint32_t)pin << PIO_SM0_PINCTRL_IN_BASE_LSB );
    pio->sm[ sm ].pinctrl = pinctrl;

    pio_sm_set_consecutive_pindirs( pio, (uint)sm, (uint)pin, 1, true );

    // Install the PIO IRQ handler and wire PIO IRQ flag 0 → NVIC line 0.
    // This runs on core0 (initProbeButtonPIO is reached via jOS.serviceAll
    // in loop()), making core0 the sole owner of the button PIO IRQ. We never
    // enable this IRQ on core1 (resumePolling no longer touches it), so the
    // handler / processSample can only ever run on one core.
    g_pioProbeIrqNum = pio_get_irq_num( pio, 0 );
    irq_set_exclusive_handler( (uint)g_pioProbeIrqNum, probe_button_pio_irq_handler );
    pio_set_irq0_source_enabled( pio, pis_interrupt0, true );
    pio_interrupt_clear( pio, 0 );           // clear any stale flag
    irq_set_enabled( (uint)g_pioProbeIrqNum, true );

    // Start the SM in polling mode - PC at the start of the polling program.
    pio_sm_restart( pio, (uint)sm );
    pio_sm_exec( pio, (uint)sm, pio_encode_jmp( g_pioProbeBtnPC ) );
    pio_sm_set_enabled( pio, (uint)sm, true );

    g_pioProbeState = PIOProbeState::READY;
    return true;
}

/**
 * @brief Direct hardware button check - fast and non-blocking
 *
 * @return 0 = neither pressed, 1 = remove button, 2 = connect button
 *
 * NOTE: Blocking logic is now handled at the service() level
 */
int ProbeButton::checkProbeButtonHardware( void ) {
    extern struct config jumperlessConfig;

    // ============================================================
    // Multiplex coordination (PROBE_LED_PIN is the WS2812 data
    // line; BUTTON_PIN sits on the same external net). We MUST NOT
    // steal the pin function from PIO while:
    //   (a) probeLEDhandler is mid showBlocking() - the writer
    //       guards this with showingProbeLEDs != 0.
    //   (b) The WS2812 latch period (300us idle) is still in
    //       progress after a show - canShow() reports this.
    //
    // Old behaviour was to bail with return-0 the instant either
    // condition tripped. That dropped a whole ~11ms sample, and a
    // dropped release sample turns a fast double-click into one
    // sustained "held" event (rising edge is never re-seen). Now
    // we spin briefly so the read still happens in this tick.
    // 600us cap > worst-case latch (300us) + a healthy slop for
    // setPixelColor() + the new PIO-drain wait in showBlocking().
    // If anything actually pegs us for that long, the caller is
    // misbehaving and skipping the tick is the right thing.
    // ============================================================
    const uint32_t multiplexWaitDeadline = time_us_32( ) + 600;
    while ( showingProbeLEDs != 0 || !probeLEDs.canShow( ) ) {
        if ( (int32_t)( time_us_32( ) - multiplexWaitDeadline ) >= 0 ) {
            return 0; // give up - try next service tick
        }
        tight_loop_contents( );
    }

    // ============================================================
    // FAST PATH: PIO continuous polling.
    //
    // When the PIO IRQ handler is running, it has already decoded the
    // most recent sample into probeBtnLatestState AND already pushed
    // it through processSample() to update the press/release/double-
    // tap state machine. Callers of this function get the same cached
    // state that the IRQ saw - no extra hardware traffic, no SM mode
    // switch, no GPIO function flipping.
    //
    // Lazy init runs on the first call - by then probeLEDs.begin()
    // has already claimed its SM. If init fails (no PIO instruction
    // memory etc.) we permanently fall through to the CPU path.
    // ============================================================
    if ( jumperlessConfig.hardware.use_pio_probe_button && initProbeButtonPIO( ) ) {
        return (int)probeBtnLatestState;
    }

    // if ( switchPosition == 0 ) {
    //     showProbeLEDs = 3;
    // } 
    // Get pin references from Probing singleton
    Probing& p = Probing::getInstance( );

    // core1busy = true;
    checkingButton = 1;

    uint32_t cpu_start_us = time_us_32( );
    int buttonState = 0;
    int buttonState2 = 0;
    int buttonState3 = 0;
    int returnState = 0;

    // The WS2812 data pin and the button sense pin sit on the same external
    // net. When probeLEDs lives on BUTTON_PIN (probe_led_on_button_pin, the
    // default), the whole release/pull/sample/drive dance happens on that ONE
    // pin - sensing on GPIO2 would reintroduce the flaky-GPIO2-contact
    // dependence that mode exists to remove. gpio_get()/pulls work regardless
    // of funcsel, so the single-pin sequence is the two-pin one with both
    // roles on the data pin.
    const uint dataPin  = (uint)probeLEDs.getPin( );
    const uint sensePin = ( dataPin == (uint)BUTTON_PIN ) ? dataPin : (uint)BUTTON_PIN;

    gpio_function_t lastProbeButtonFunction = gpio_get_function( dataPin );

    // Button reading sequence with proper timing
    gpio_set_dir( sensePin, false );
    gpio_set_function( dataPin, GPIO_FUNC_SIO );
    gpio_disable_pulls( dataPin );
    //gpio_set_dir( dataPin, false );
    probeOeClr(dataPin);
    delayMicroseconds( BUTTON_SETTLE_US );

    gpio_set_dir( PROBE_PIN, true );
    gpio_put( PROBE_PIN, true );

    gpio_set_pulls( sensePin, false, true );
    gpio_set_input_enabled( sensePin, true );
    
    buttonState = gpio_get( sensePin );
    gpio_set_input_enabled( sensePin, false );

    delayMicroseconds( BUTTON_SETTLE_US );

    gpio_set_pulls( sensePin, true, false );
    gpio_set_input_enabled( sensePin, true );
    delayMicroseconds( BUTTON_SETTLE_US );

    buttonState2 = gpio_get( sensePin );
    gpio_set_input_enabled( sensePin, false );

    delayMicroseconds( BUTTON_SETTLE_US );

    gpio_set_pulls( sensePin, false, false );
    delayMicroseconds( BUTTON_SETTLE_US );

    gpio_set_dir( sensePin, true );
    gpio_put( sensePin, false );
    delayMicroseconds( BUTTON_SETTLE_SHORT_US );

    gpio_set_dir( sensePin, false );
    gpio_set_pulls( sensePin, false, true );
    gpio_set_input_enabled( sensePin, true );
    buttonState3 = gpio_get( sensePin );
    gpio_set_input_enabled( sensePin, false );

    gpio_set_pulls( sensePin, false, true);
    gpio_set_function( dataPin, lastProbeButtonFunction );
    probeOeSet(dataPin);
    // In single-pin mode the sequence above disabled the pad's input buffer;
    // the PIO button program's 'in pins' needs it back on.
    gpio_set_input_enabled( dataPin, true );

    delayMicroseconds( 20);

    checkingButton = 0;
    // core1busy = false;

    // Determine button state (handles probe revision)
    if ( buttonState == 1 && buttonState2 == 1 && buttonState3 == 1 ) {
        returnState = ( jumperlessConfig.hardware.probe_revision >= 4 ) ? 2 : 1;
    } else if ( buttonState == 0 && buttonState2 == 0 && buttonState3 == 0 ) {
        returnState = ( jumperlessConfig.hardware.probe_revision >= 4 ) ? 1 : 2;
    }

    probeButtonCPUReadCount++;
    probeButtonCPULastUs = time_us_32( ) - cpu_start_us;
    if ( probe_button_trace ) {
        Serial.printf( "[%lu] PROBE-CPU  raw=%d/%d/%d -> %d  (%luus)\n",
                       (unsigned long)millis( ),
                       buttonState, buttonState2, buttonState3,
                       returnState,
                       (unsigned long)probeButtonCPULastUs );
        Serial.flush( );
    }
    return returnState;
}

// Global reference for clean syntax
ProbeButton& probeButton = ProbeButton::getInstance( );

// ============================================================================
// Probing Class Implementation
// ============================================================================

// Static member initialization
Probing* Probing::instance = nullptr;

Probing& Probing::getInstance( ) {
    if ( instance == nullptr ) {
        instance = new Probing( );
    }
    return *instance;
}

Probing::Probing( ) {
    // Initialize probe row maps
    int probeRowMapInit[ 108 ] = {
        -1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, TOP_RAIL, GND,
        BOTTOM_RAIL, GND, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
        NANO_D1, NANO_D0, NANO_RESET_1, GND, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9, NANO_D10, NANO_D11, NANO_D12,
        NANO_D13, NANO_3V3, NANO_AREF, NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6, NANO_A7, NANO_5V, NANO_RESET_0, GND, NANO_VIN,
        LOGO_PAD_BOTTOM, LOGO_PAD_TOP, GPIO_PAD, DAC_PAD, ADC_PAD, BUILDING_PAD_TOP, BUILDING_PAD_BOTTOM, -1, -1, -1, -1 };

    int probeRowMapByPadInit[ 108 ] = {
        -1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, TOP_RAIL, TOP_RAIL_GND,
        BOTTOM_RAIL, BOTTOM_RAIL_GND, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
        NANO_D1, NANO_D0, NANO_RESET_1, NANO_GND_1, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9, NANO_D10, NANO_D11, NANO_D12,
        NANO_D13, NANO_3V3, NANO_AREF, NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6, NANO_A7, NANO_5V, NANO_RESET_0, NANO_GND_0, NANO_VIN,
        LOGO_PAD_BOTTOM, LOGO_PAD_TOP, GPIO_PAD, DAC_PAD, ADC_PAD, BUILDING_PAD_TOP, BUILDING_PAD_BOTTOM, -1, -1, -1, -1 };

    memcpy( probeRowMap, probeRowMapInit, sizeof( probeRowMap ) );
    memcpy( probeRowMapByPad, probeRowMapByPadInit, sizeof( probeRowMapByPad ) );

    // Deliberately NOT touching switchPosition here: it boots as -1
    // (unknown, the Probing.h initializer) and checkSwitchPosition()'s
    // absolute-classification branch names it from the measured current
    // within a check or two. This used to hard-code 1 (select) - an
    // ASSUMPTION, not a measurement - so a board booted with the switch at
    // measure spent its first second wrong, and any boot where sensing
    // couldn't run (feed not up, LED chip dark) kept the wrong guess until
    // the user flipped the switch hard enough to cross a transition
    // threshold.
    lastSwitchPositions[ 0 ] = 1;
    lastSwitchPositions[ 1 ] = 1;
    lastSwitchPositions[ 2 ] = 1;
}

/**
 * @brief Handle probe button actions and toggle logic
 *
 * This is called from service() and handles all the probe button
 * interactions, toggle logic, and probe mode triggering.
 *
 * Entry is IMMEDIATE on click 1 - feedback (LED, mode setup) happens
 * right away with no latency. If a second click arrives inside
 * ProbingDoubleTap::kWindowMs, probeMode's inner loop catches the
 * resulting undo/redo via g_probeDoubleTapBail and returns fast through
 * its normal exit path. The terminal banner is deferred inside probeMode
 * until the window has elapsed without a bail, so a fast-return never
 * leaves stale "connect nodes" / "clear nodes" lines on the screen.
 *
 * Past the entry window, a double-tap inside probeMode just fires
 * undo/redo on top of the running session - the session stays alive.
 * See the bail-handling inside probeMode() for the cutoff logic.
 */
void Probing::handleProbeButtonActions( ) {
    extern unsigned long startupTimers[];
    extern int probeToggle( int buttonState ); // Defined in Peripherals.cpp
    extern class ProbeButton& probeButton;     // High-frequency button service

    // Check if we're in a blocking period FIRST - don't consume events if blocked
    if ( blockProbeButton > 0 && ( millis( ) - blockProbeButtonTimer < blockProbeButton ) ) {
        return; // Still blocked, don't process or consume button events
    }

    // Now consume the button press event (only if not blocked)
    int buttonPress = probeButton.getButtonPress( );

    // Handle probe toggle when brightenedNet is active
    if ( brightenedNet > 0 ) {
        int probeToggleResult = probeToggle( buttonPress );
        if ( probeToggleResult >= 0 && brightenedNet > 0 ) {
            // Successfully toggled GPIO - block button and return to prevent probe mode
            blockProbeButton = gpioToggleFrequency;
            blockProbeButtonTimer = millis( );
            return; // Don't continue to probe mode logic
        } else if ( probeToggleResult == -4 ) {
            // GPIO output - remove button should just unhighlight
            highlighting.clearHighlighting( );
            blockProbeButton = 800;
            blockProbeButtonTimer = millis( );
            return; // Don't enter probe mode
        } else if ( probeToggleResult == -5 ) {
            // Regular net with no GPIO output - handle warning logic for disconnection
            if ( brightenedNode > 0 ) {
                // Check if we're already warning this net (second press)
                if ( warningNet == brightenedNet && warningTimeout > 0 ) {
                    // Second press - trigger probe clear mode
                    warningTimeout = 0;
                    connectOrClearProbe = 0;
                    showProbeLEDs = 2;
                    probingTimer = millis( );
                    startupTimers[ 0 ] = millis( );

                    // Run probe mode directly (it handles button state internally)
                    probeMode( 0, brightenedNode );
                    highlighting.clearHighlighting( );

                } else {
                    // First press - show warning animation
                    highlighting.warnNet( brightenedNode );
                    warningTimeout = 1500;
                    warningTimer = millis( );
                    // Use shorter block time to allow second press quickly
                    blockProbeButton = 400;
                    blockProbeButtonTimer = millis( );
                    return; // Don't enter probe mode on first press
                }
            }
            // Second press executed - use longer block after entering probe mode
            blockProbeButton = 800;
            blockProbeButtonTimer = millis( );
        } else if ( probeToggleResult == -3 || probeToggleResult == -2 ) {
            blockProbeButton = 800;
            blockProbeButtonTimer = millis( );
        }

    } else {
        firstConnection = -1;
    }

    // Use the button press event we got at the start of the function

    if ( buttonPress != 0 ) {
        // Button was pressed - stored state changed
        lastProbeButton = buttonPress;

        if ( buttonPress == 2 ) {
            // Connect button pressed - trigger probe connect mode
            connectOrClearProbe = 1;
            showProbeLEDs = 1;
            probingTimer = millis( );
            brightenedNet = 0;
            blockProbeButtonTimer = millis( );
            blockProbeButton = 1000;

            // Run probe mode directly (it handles button blocking/clearing internally)
            probeMode( 1, firstConnection );
            highlighting.clearHighlighting( );

        } else if ( buttonPress == 1 ) {
            // Remove button pressed - trigger probe clear mode
            startupTimers[ 0 ] = millis( );
            connectOrClearProbe = 0;
            showProbeLEDs = 2;
            probingTimer = millis( );
            brightenedNet = 0;
            blockProbeButtonTimer = millis( );
            blockProbeButton = 1000;

            // Run probe mode directly (it handles button blocking/clearing internally)
            probeMode( 0, firstConnection );
            highlighting.clearHighlighting( );
        }
    }
}

// ============================================================================
// ProbeSwitch Service Implementation - LOW priority
// ============================================================================

ProbeSwitch* ProbeSwitch::instance = nullptr;

ProbeSwitch& ProbeSwitch::getInstance( ) {
    if ( instance == nullptr ) {
        instance = new ProbeSwitch( );
    }
    return *instance;
}

/**
 * @brief Service method for probe switch checking
 * Checks the 3-position switch state.
 * LOW priority - not time-critical, can run infrequently.
 */
ServiceStatus ProbeSwitch::service( ) {
    lastStatus = ServiceStatus::IDLE;

    // Check switch position (cheap: digital read)
    Probing::getInstance( ).checkSwitchPosition( );

    return lastStatus;
}

// ============================================================================
// ProbePads Service Implementation - LOW priority
// ============================================================================

ProbePads* ProbePads::instance = nullptr;

ProbePads& ProbePads::getInstance( ) {
    if ( instance == nullptr ) {
        instance = new ProbePads( );
    }
    return *instance;
}

/**
 * @brief Service method for pad checking
 * Does expensive ADC pad reading.
 * LOW priority - expensive and not time-critical.
 * Rate-limited to 20Hz (every 50ms) to reduce overhead.
 */
ServiceStatus ProbePads::service( ) {
    lastStatus = ServiceStatus::IDLE;

    // Rate limit to 20Hz (every 50ms) - pad checking is expensive
    unsigned long now = millis( );
    if ( now - lastCheckTime < 50 ) {
        return lastStatus; // Skip this iteration
    }
    lastCheckTime = now;

    // Check pads (expensive: multiple ADC readings)
    Probing::getInstance( ).checkPads( );
    lastStatus = ServiceStatus::BUSY;

    return lastStatus;
}

// ============================================================================
// Probing Service Implementation - HIGH priority
// ============================================================================

/**
 * @brief Main service method for probing system
 *
 * This is called each loop iteration and handles:
 * - Reading probe position (HIGH priority - responsive)
 * - Probe button actions and toggle logic
 *
 * OPTIMIZATION: Expensive operations (checkPads, checkSwitchPosition) are now
 * handled by separate LOW priority services, keeping this service fast.
 */
void Probing::simulateProbeTap( int node, unsigned long holdMs ) {
    if ( node <= 0 ) {
        simTapUntil = 0;
        simTapNode = -1;
        return;
    }
    simTapNode = node;
    simTapUntil = millis( ) + ( holdMs == 0 ? 1 : holdMs );
}

ServiceStatus Probing::service( ) {
    // Update last status for base class tracking
    lastStatus = ServiceStatus::IDLE;

    // Check if probe menu is active (blocking)
    if ( sfProbeMenu != 0 || inPadMenu != 0 ) {
        lastStatus = ServiceStatus::BLOCKING;
        return lastStatus;
    }

    // Rate limiting for probe reading
    // Run probe check every ~10ms (100Hz) for responsive probing
    static unsigned long lastProbeCheckTime = 0;
    unsigned long now = millis( );
    bool shouldCheckProbe = ( now - lastProbeCheckTime >= 10 ); // 10ms = 100Hz

    int probeReading = lastProbeReading; // Use cached value by default

    if ( shouldCheckProbe ) {
        lastProbeCheckTime = now;

        // Read probe position (moderately expensive: ADC + mapping)
        probeReading = justReadProbe( true );
        // Simulated tap (simulateProbeTap / jl_probe_tap): stand in for the
        // ladder only when it read nothing, so a real tip always wins.
        if ( simTapUntil != 0 ) {
            if ( (long)( millis( ) - simTapUntil ) >= 0 ) {
                simTapUntil = 0;
                simTapNode = -1;
            } else if ( probeReading <= 0 ) {
                probeReading = simTapNode;
            }
        }
        lastProbeReading = probeReading;

        // If we did any work, mark as BUSY
        if ( probeReading > 0 ) {
            lastStatus = ServiceStatus::BUSY;
        }
    }

    // Handle probe button actions and toggle logic (always run - critical for UX)
    handleProbeButtonActions( );

    return lastStatus;
}

// Global service references
ProbeSwitch& probeSwitch = ProbeSwitch::getInstance( );
ProbePads& probePads = ProbePads::getInstance( );

// Backward compatibility - create references to singleton members
volatile int& sfProbeMenu = Probing::getInstance( ).sfProbeMenu;
unsigned long& probingTimer = Probing::getInstance( ).probingTimer;
int& probePin = Probing::getInstance( ).probePin;
int& buttonPin = Probing::getInstance( ).buttonPin;
volatile unsigned long& blockProbeButton = Probing::getInstance( ).blockProbeButton;
volatile unsigned long& blockProbeButtonTimer = Probing::getInstance( ).blockProbeButtonTimer;
volatile int& connectOrClearProbe = Probing::getInstance( ).connectOrClearProbe;
volatile int& node1or2 = Probing::getInstance( ).node1or2;
int& probeHighlight = Probing::getInstance( ).probeHighlight;
volatile int& removeFade = Probing::getInstance( ).removeFade;
int& debugProbing = Probing::getInstance( ).debugProbing;
volatile int& showingProbeLEDs = Probing::getInstance( ).showingProbeLEDs;
volatile int& switchPosition = Probing::getInstance( ).switchPosition;
int& probePowerDAC = Probing::getInstance( ).probePowerDAC;
int& showProbeCurrent = Probing::getInstance( ).showProbeCurrent;

// Additional references for global access
volatile int& inPadMenu = Probing::getInstance( ).inPadMenu;
volatile int& checkingButton = Probing::getInstance( ).checkingButton;
volatile int& lastProbeLEDs = Probing::getInstance( ).lastProbeLEDs;

// Export probe maps as references
int ( &probeRowMap )[ 108 ] = Probing::getInstance( ).probeRowMap;
int ( &probeRowMapByPad )[ 108 ] = Probing::getInstance( ).probeRowMapByPad;

// Export pad settings
int ( &logoTopSetting )[ 2 ] = Probing::getInstance( ).logoTopSetting;
int ( &logoBottomSetting )[ 2 ] = Probing::getInstance( ).logoBottomSetting;
int ( &buildingTopSetting )[ 2 ] = Probing::getInstance( ).buildingTopSetting;
int ( &buildingBottomSetting )[ 2 ] = Probing::getInstance( ).buildingBottomSetting;

static int resolveLogoPadAssignment( int configValue, int defaultNode ) {
    switch ( configValue ) {
    case -1:
        return -1;
    case 0:
        return RP_UART_TX;
    case 1:
        return RP_UART_RX;
    case 25:
        return ISENSE_PLUS;
    case 26:
        return ISENSE_MINUS;
    default:
        return defaultNode;
    }
}

static int nodeToLogoPadConfig( int node, int fallbackConfig ) {
    switch ( node ) {
    case RP_UART_TX:
        return 0;
    case RP_UART_RX:
        return 1;
    case ISENSE_PLUS:
        return 25;
    case ISENSE_MINUS:
        return 26;
    case -1:
        return -1;
    default:
        return fallbackConfig;
    }
}

/// @brief Wait for Core 2 to finish displaying blocking menu graphics
/// @param timeoutMs Maximum time to wait in milliseconds (default 100ms)
/// @return true if Core 2 finished, false if timeout
///
/// This function waits for Core 2 to complete a blocking LED display by polling
/// the showLEDsCore2 flag until it's cleared to 0. Core 2 sets showLEDsCore2=0
/// after completing the display.
///
/// CRITICAL: Core 1 must NEVER call leds.show*() directly - use this pattern:
///   1. Draw menu: b.print(...)
///   2. Signal Core 2: showLEDsCore2 = 12
///   3. Wait for completion: waitForBlockingDisplay()
///   4. Enter blocking loop (menu now guaranteed visible)
static bool waitForBlockingDisplay( uint32_t timeoutMs = 100 ) {
    extern volatile int showLEDsCore2;

    uint32_t waitStart = millis( );
    while ( showLEDsCore2 != 0 && ( millis( ) - waitStart ) < timeoutMs ) {
        tight_loop_contents( ); // Yield to Core 2, hint to CPU we're spinning
    }

    // Return true if Core 2 finished, false if timeout
    return ( showLEDsCore2 == 0 );
}

// ============================================================================
// Legacy Global Variables (not moved to class yet)
// ============================================================================

#define SPACE_FROM_LEFT 6

int lastReadRaw = 0;

int probeHalfPeriodus = 20;

unsigned long probeTimeout = 0;

int connectedRowsIndex = 0;
int connectedRows[ 32 ] = { -1 };

int nodesToConnect[ 2 ] = { -1, -1 };

unsigned long probeButtonTimer = 0;

int voltageSelection = TOP_RAIL;

int rainbowList[ 13 ][ 3 ] = {
    { 40, 50, 80 }, { 88, 33, 70 }, { 30, 15, 45 }, { 8, 27, 45 }, { 45, 18, 19 }, { 35, 42, 5 }, { 02, 45, 35 }, { 18, 25, 45 }, { 40, 12, 45 }, { 10, 32, 45 }, { 18, 5, 43 }, { 45, 28, 13 }, { 8, 12, 8 } };

int checkingPads = 0;

// Special function option colors - used in encoder cursor and probe menus
uint32_t sfOptionColors[ 12 ] = {
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

uint32_t deleteFade[ 13 ] = { 0x371f16, 0x28160b, 0x191307, 0x141005, 0x0f0901,
                              0x090300, 0x050200, 0x030100, 0x030000, 0x020000,
                              0x010000, 0x000000, 0x000000 };

int fadeIndex = 0;

// Global encoder cursor position for override in LED display functions
volatile int globalEncoderCursorNode = -1;             // -1 = cursor hidden
volatile int globalEncoderCursorInHeader = 0;          // 1 if in nano header
volatile uint32_t globalEncoderCursorColor = 0x4500e8; // Cursor color



unsigned long idleTime = millis( );
unsigned long idleSaveTime = 3000;


/**
 * @brief Handle encoder-based node selection in probe mode
 *
 * This function manages the encoder cursor navigation through breadboard, nano header,
 * and special function zones (rails, DAC, ADC, GPIO, UART, current sense).
 * It handles encoder movement, cursor display, button press for selection, and timeouts.
 *
 * The function modifies many parameters by reference to update the probe mode state.
 */
void Probing::handleEncoderCursorNavigation(
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

                ReadingDisplay::showName( displayName );
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
                    setDac0voltage( newValue, 1, 0, true );
                    globalState.power.dac0 = newValue;
                };
            } else {
                // DAC 1
                config.initialValue = globalState.power.dac1;
                config.label = "DAC 1";
                config.callback = []( float newValue, bool isLive, void* context ) {
                    setDac1voltage( newValue, 1, 0, true );
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

int Probing::probeMode( int setOrClear, int firstConnection, bool fromClickMenu ) {

    // Clear any stale double-tap bail flag from a previous session.
    // The flag is set inside the loop by service() when undo/redo fires
    // and cleared by our own bail check; this guards against the rare
    // case where it was set just as the previous probeMode session
    // exited through some other path (encoder hold, serial input,
    // timeout, etc.).
    g_probeDoubleTapBail = false;

    // clearColorOverrides(1, 1, 0);

    // Block button and clear any pre-existing state to prevent double-detection
    blockProbeButton = 3000;
    blockProbeButtonTimer = millis( );
    probeButton.clearButtonState( ); // Clear the button state that triggered entry

    // Enable text layer for special nodes display (UART, Current, etc.)

    /* clang-format off */

    int deleteMisses[ 20 ] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    /* clang-format on */

    int deleteMissesIndex = 0;

    int connectionsThisSession = 0; // Track total connections made this probe mode session

    routableBufferPower( 1, 1 );

    // probeMode owns the terminal from here: its banners and node names
    // scroll the rows the live reading pinned for itself, so drop that anchor
    // rather than let the next reading erase one of OUR lines. (No reading
    // can repaint while this loop runs - it services on jOS.serviceCritical(),
    // which dispatches only CRITICAL services, and MeasureMode is HIGH.)
    ReadingDisplay::resetLastShown( );

    // Banner deferral: on first entry of probeMode we don't yet know
    // whether this press is the first tap of a double-tap that will fire
    // undo/redo and bail out. Printing "connect nodes" / "clear nodes"
    // immediately would leave a stale entry in the terminal once the
    // bail kicks in. Defer the emit until the double-tap window has
    // passed without a bail (or until any user activity proves we're
    // staying). Toggle gotos (goto restartProbing inside the loop)
    // emit immediately - by then the user has committed to the session.
    bool bannerEmitted = false;
    bool firstEntry = true;
    // Set when a short wheel click (no encoder cursor showing, probe-button
    // entry) exits probing - the tail of this function then reopens the
    // click menu with that same click. Declared before the restartProbing
    // labels so a mode-toggle goto can't reset it.
    bool exitToClickMenu = false;
    // Outer-session entry timestamp. Set ONCE here (before the
    // restartProbing labels) so a goto-restart from a mode switch
    // inside the loop doesn't reset it. Used by the bail check below
    // to distinguish "user just opened probeMode and the second tap
    // means they didn't mean to" from "user has been in probeMode for
    // a while and the double-tap is a real undo gesture."
    //
    // (probeModeStartTime, declared near the loop start, DOES reset
    // on goto-restart - that's the right behaviour for the encoder
    // cursor logic that uses it, but NOT for the bail check.)
    const unsigned long outerProbeEntryTime = millis( );

    // In-probe deferred-press state. When readProbe() returns a button
    // code (-16 / -18), we DON'T process it immediately - we stash the
    // press here and wait kWindowMs for a possible second tap. If a
    // double-tap fires during the wait, service() drains the deferred
    // undo/redo and our bail handler (top of loop) clears
    // pendingInProbeButton - so click 1's mode switch / clear-in-
    // -progress effect never commits. If no second tap arrives, we
    // inject the press back into row[0] at the top of the loop and
    // the existing button handler processes it normally.
    //
    // Declared up here (before the restartProbing labels) so a goto-
    // restart from a mode switch can't reset it.
    int           pendingInProbeButton     = 0;     // 0, -16, or -18
    unsigned long pendingInProbeButtonTime = 0;
    // Tracks the oled hold state across loop iterations so we can detect
    // the held -> not-held transition (the moment an undo toast just
    // finished) and force the probeMode banner back onto the panel. Must
    // also sit above the restartProbing labels: a goto-restart in the
    // middle of a toast must NOT reset our tracker, otherwise we'd
    // re-arm and double-paint after the same toast. Initial value
    // captures whatever state we entered probeMode in - if a toast was
    // already in flight from the calling context, this avoids spuriously
    // detecting a false->true edge on the first iteration. Polling lives
    // inside probeMode because OLEDService is a low-priority service
    // that probeMode's tight loop doesn't run, so oledPeriodic() can't
    // do this work for us during a probeMode session.
    bool wasHeld = oled.oledIsHeld( );
    auto emitBanner = [&]() {
        if ( bannerEmitted ) return;
        bannerEmitted = true;
        if ( setOrClear == 1 ) {
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
    };

restartProbing:

    probeActive = 1;
    // A NetVoltageScan sense tap checks probeActive before starting but can
    // be mid-flight right now (it raises core2busy for the tap's ~1ms).
    // Wait it out so probe raw crosspoint sends never overlap a tap's PIO
    // handshake from the other core.
    waitCore2( );
    brightenNet( -1 );

    if ( switchPosition == 0 && globalState.hasConnection( probePowerDAC == 0 ? DAC0 : DAC1, ROUTABLE_BUFFER_IN ) ) {
       // changeTerminalColor( 197 );
       // Serial.println( "  Switch is in Measure mode!\n\r  Set switch to Select mode for best results\n\r" );
       // Serial.flush( );
    }

    // LED color hint is non-terminal state so it's safe to set immediately.
    rawOtherColors[ 1 ] = ( setOrClear == 1 ) ? 0x4500e8 : 0x6644A8;

    // First-entry banner is deferred until the main loop confirms the
    // double-tap window passed without bailing. Toggle gotos (which set
    // firstEntry=false before goto) emit immediately.
    if ( !firstEntry ) {
        emitBanner( );
    }

restartProbingNoPrint:

    if ( setOrClear == 1 && firstConnection == -1 ) {
        oled.clearPrintShow( "connect", 2, true, true, true );
    } else if ( setOrClear == 0 && firstConnection == -1 ) {
        oled.clearPrintShow( "clear", 2, true, true, true );
    }

    clearColorOverrides( 1, 1, 0 );

    probeHighlight = -1;

    int numberOfLocalChanges = 0;

    connectOrClearProbe = setOrClear;

    int row[ 16 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    row[ 1 ] = -2;
    row[ 0 ] = -2;

    probeTimeout = millis( );

    if ( setOrClear == 0 ) {
        probeButtonTimer = millis( );
    }

    if ( setOrClear == 1 ) {
        showProbeLEDs = 1;
    } else {
        showProbeLEDs = 2;
    }

    connectOrClearProbe = setOrClear;

    showLEDsCore2 = 1;
    unsigned long doubleSelectTimeout = millis( );
    int doubleSelectCountdown = 0;

    int lastProbedRows[ 4 ] = { 0, 0, 0, 0 };
    unsigned long fadeTimer = millis( );
    int fadeClear = -1;

    // Encoder support for node selection with special function zones
    EncoderAccelerator encoderAccel = EncoderAccelerator::Slow( ); // Use slow preset for precise node selection
    long lastEncoderPosition = encoderPosition;
    float encoderAccumulator = 0.0f; // Fractional position accumulator

    // Navigation zones: 0=Breadboard, 1=NanoHeader, 2=Rails, 3=DAC, 4=ADC, 5=GPIO, 6=UART
    enum CursorZone { ZONE_BREADBOARD = 0,
                      ZONE_NANO = 1,
                      ZONE_RAILS = 2,
                      ZONE_DAC = 3,
                      ZONE_ADC = 4,
                      ZONE_GPIO = 5,
                      ZONE_UART = 6,
                      ZONE_CURRENT = 7 };

    // Static variable to persist cursor position between selections within same probe mode session
    static int persistentEncoderCursorNode = -1;
    static int persistentCursorZone = ZONE_BREADBOARD;
    static int persistentSubIndex = 0;  // For multi-item zones (DAC 0/1, ADC 0-4, etc.)
    static bool firstProbeEntry = true; // Track first entry to reset position

    // Initialize cursor position on first entry or use persistent position
    int encoderCursorNode = firstProbeEntry ? 14 : persistentEncoderCursorNode; // Start at row 15 (0-indexed = 14)
    int cursorZone = firstProbeEntry ? ZONE_BREADBOARD : persistentCursorZone;
    int subIndex = firstProbeEntry ? 0 : persistentSubIndex; // Index within special function zone
    firstProbeEntry = false;                                 // After first use, persist position

    int lastEncoderCursorNode = -1; // Track last position to clear it
    int lastCursorZone = -1;
    int lastSubIndex = -1;
    unsigned long lastEncoderMovement = millis( ); // Time of last encoder movement
    unsigned long encoderHideTimeout = 5000;       // Hide cursor after 2 seconds of no movement
    bool encoderCursorVisible = false;             // Whether cursor is currently shown

    // Set rotary divider for good responsiveness during probing
    int savedRotaryDivider = rotaryDivider;
    rotaryDivider = 3;

    blockProbeButton = 1000;
    blockProbeButtonTimer = millis( );

    unsigned long probeModeStartTime = millis( );
    unsigned long lastLoopTime = millis( );

    //! this is the main loop for probing
    while ( Serial.available( ) == 0 && ( millis( ) - probeTimeout ) < 80000 ) {

        // Serial.println("Full loop took " + String(millis() - lastLoopTime) + "ms");
        // Serial.flush();
        // lastLoopTime = millis();

        delayMicroseconds( 20 ); // Reduced from 500 for faster encoder response

        // Keep critical services (like ProbeButton) running during blocking probeMode
        jOS.serviceCritical( );
        // ...and the switch position (ProbeSwitch is NORMAL, so it is not in
        // that set): detector-A-only tracking, agree mode only.
        checkSwitchPositionFast( );

        // Hold-end polling. serviceCritical above may have just kicked
        // off (or extended) an undo-toast hold via the double-tap drain.
        // We rely on the loop continuing to spin; on the iteration after
        // the hold's natural expiry, oledIsHeld() flips false and we
        // repaint the canonical probeMode banner. We don't paint in the
        // held->held or not-held->not-held cases - only the transition
        // edge - so this is at most one clearPrintShow per toast and
        // doesn't fight with normal probeMode rendering. firstConnection
        // gating mirrors the entry-banner block above for consistency.
        bool isHeld = oled.oledIsHeld( );
        if ( wasHeld && !isHeld && firstConnection == -1 ) {
            if ( setOrClear == 1 ) {
                oled.clearPrintShow( "connect", 2, true, true, true );
            } else {
                oled.clearPrintShow( "clear", 2, true, true, true );
            }
        }
        wasHeld = isHeld;

        // ============================================================
        // Double-tap fast-return / stay-alive cutoff.
        //
        // service() (called from serviceCritical above) sets
        // g_probeDoubleTapBail when it actually fires an undo/redo from
        // a double-tap. We branch on how far into the OUTER probeMode
        // session we are (outerProbeEntryTime, set once before the
        // restartProbing labels):
        //
        //   - Inside the entry window (< kWindowMs since outer entry):
        //     the user's gesture was a probe-button-DOUBLE-tap, not a
        //     deliberate probe-mode entry. Click 1 launched us here but
        //     click 2 cancelled the intent. Break out fast - since the
        //     banner is still deferred (see below), nothing gets
        //     printed and the only visible effect is a ~200 ms LED
        //     flash + the undo toast.
        //
        //   - Past the entry window: the user is deliberately in probe
        //     mode and the double-tap is a real undo gesture (e.g.
        //     "undo that connection I just made"). Just clear the flag
        //     and keep running - the undo fired via service()'s drain
        //     one line up.
        //
        // CRITICAL: uses outerProbeEntryTime, NOT probeModeStartTime.
        // probeModeStartTime gets reset on goto restartProbing (mode
        // switch inside probeMode), which would make the bail fire on
        // every "mode switch + undo" sequence and incorrectly exit
        // probeMode even when the user has been in it for ages.
        // ============================================================
        if ( g_probeDoubleTapBail ) {
            g_probeDoubleTapBail = false;
            // Cancel any in-flight pending press - the double-tap means
            // click 1 was the first half of an undo gesture, not a real
            // probe-mode action. Without this, a sequence like "in
            // connect mode, double-tap disconnect to undo" would defer
            // click 1's switch-to-clear, then commit it AFTER the bail
            // returned us to the loop top - leaving the user in clear
            // mode despite their double-tap.
            pendingInProbeButton = 0;
            if ( ( millis( ) - outerProbeEntryTime ) < ProbingDoubleTap::kWindowMs ) {
                blockProbeButton = ProbingDoubleTap::kWindowMs;
                blockProbeButtonTimer = millis( );
                break;
            }
            // else: past the entry window - stay in probeMode, undo
            // already fired via service()'s drain, just continue.
        }

        // pendingCommitting is set further down (after readProbe) when
        // the deferred-press window elapses and we inject the press
        // back into row[0]. Declared here so its scope covers the rest
        // of this iteration including the press handler.
        bool pendingCommitting = false;

        // First-entry banner: emit once the double-tap entry window has
        // passed without a fast-return. After that point we know this
        // is a real probe-mode session, not the first half of a
        // double-tap. Toggle-gotos (which set firstEntry=false before
        // jumping) emit immediately at the goto target.
        if ( firstEntry && !bannerEmitted &&
             ( millis( ) - outerProbeEntryTime ) >= ProbingDoubleTap::kWindowMs ) {
            emitBanner( );
        }

        // Service live crossbar display during probe mode for real-time updates
        liveCrossbarService.service( );
        
        // rotaryEncoderStuff();  // Update encoder state

        connectedRowsIndex = 0;

        // ======= ENCODER-BASED NODE SELECTION WITH SPECIAL FUNCTION ZONES =======
        handleEncoderCursorNavigation(
            setOrClear,
            node1or2,
            nodesToConnect,
            connectOrClearProbe,
            probeModeStartTime,
            lastEncoderPosition,
            encoderAccumulator,
            encoderCursorNode,
            cursorZone,
            subIndex,
            lastEncoderCursorNode,
            lastCursorZone,
            lastSubIndex,
            lastEncoderMovement,
            encoderCursorVisible,
            persistentEncoderCursorNode,
            persistentCursorZone,
            persistentSubIndex,
            row,
            connectedRows,
            connectedRowsIndex,
            encoderAccel,
            encoderHideTimeout,
            !fromClickMenu );

        // Check for encoder button HELD to exit probe mode. The function
        // above already did the visual cleanup (it leaves HELD set on
        // purpose so we can see it). We clear the encoder state HERE so
        // the next probeMode invocation (or other consumer) starts fresh.
        if ( encoderButtonState == HELD ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            break;
        }

        // Short wheel click with no encoder cursor showing, in a session
        // the probe button started: the user wants the menu, not probing.
        // Exit and reopen the click menu at the tail of this function.
        // A click while the cursor IS visible still selects the
        // highlighted node (handled inside the encoder function, which
        // consumes the event - seeing RELEASED here means no cursor).
        if ( !fromClickMenu && !encoderCursorVisible &&
             encoderButtonState == RELEASED ) {
            encoderButtonState = IDLE;
            lastButtonEncoderState = IDLE;
            exitToClickMenu = true;
            break;
        }
        // ======= END ENCODER SELECTION =======

        if ( firstConnection > 0 ) {
            row[ 0 ] = firstConnection;
            connectedRows[ 0 ] = row[ 0 ];
            connectedRowsIndex = 1;
            if ( setOrClear == 0 ) {
                firstConnection = -2;
            } else {
                firstConnection = -3;
            }

        } else if ( row[ 0 ] == -1 ) { // Only read physical probe if encoder didn't select
            row[ 0 ] = readProbe( );
        }



        // Handle encoder returns from readProbe() - these are handled by cursor logic above
        if ( row[ 0 ] == -19 || row[ 0 ] == -17 || row[ 0 ] == -10 ) {
            // Encoder movement/button - already handled by cursor logic
            // Clear encoder state so readProbe() can read physical probe next time
            encoderDirectionState = NONE;
            // Reset row[0] and continue loop
            row[ 0 ] = -1;
            continue;
        }

        // Resolve a deferred in-probe button press whose double-tap
        // window has elapsed without a cancel. Done here (AFTER
        // handleEncoderCursorNavigation + readProbe) because the
        // encoder function unconditionally writes row[0] = -1 when no
        // selection is active - earlier injection got clobbered. We
        // only inject when no fresher input (encoder selection,
        // probe-needle touch, or new button press from readProbe)
        // already claimed row[0] this iteration.
        if ( row[ 0 ] == -1 &&
             pendingInProbeButton != 0 &&
             ( millis( ) - pendingInProbeButtonTime ) >= ProbingDoubleTap::kWindowMs ) {
            row[ 0 ] = pendingInProbeButton;
            pendingInProbeButton = 0;
            pendingCommitting = true;
        }

        if ( row[ 0 ] == -1 ) {
            // tuiGlue.loop();
        } else {
            idleTime = millis( );
        }
        // Serial.println(row[0]);
        //! save local node file if idle for 3 seconds`
        if ( millis( ) - idleTime > idleSaveTime ) { // save local node file if idle for 3 seconds
            idleTime = millis( );
            // Only call service() if state is actually dirty to avoid unnecessary work
            // The service() will auto-save if dirty for >1 second
            if ( globalState.isDirty( ) && numberOfLocalChanges > 0 ) {
                SlotManager::getInstance( ).service();
                // Only reset numberOfLocalChanges if state was actually saved (no longer dirty)
                if ( !globalState.isDirty( ) ) {
                    numberOfLocalChanges = 0;
                }
            }
        }
        // probeButtonToggle = checkProbeButton();
        // if (isConnectable(row[0]) == false && row[0] != -1) {
        //   row[0] = -1;
        // //  continue;
        // }

        if ( setOrClear == 1 ) { //! remove fade animation
            deleteMissesIndex = 0;
            if ( millis( ) - fadeTimer > 500 ) {
                fadeTimer = millis( );
                if ( numberOfLocalChanges > 0 ) {

                    // saveStateToSlot( netSlot );
                    // saveLocalNodeFile( netSlot );
                    // Serial.print("\n\r");
                    // Serial.print("saving local node file\n\r");

                // refreshConnections();
                // numberOfLocalChanges = 0;
                }
            }
            // showLEDsCore2 = -1;

        } else {
            if ( millis( ) - fadeTimer > 12 ) {
                fadeTimer = millis( );

                if ( fadeIndex < 12 ) {
                    fadeIndex++;
                    if ( removeFade > 0 ) {
                        removeFade--;
                        showProbeLEDs = 2;
                    }
                } else {
                    deleteMissesIndex = 0;
                    for ( int i = 0; i < 20; i++ ) {
                        deleteMisses[ i ] = -1;
                    }
                }

                int fadeFloor = fadeIndex;
                if ( fadeFloor < 0 ) {
                    fadeFloor = 0;
                }

                for ( int i = deleteMissesIndex - 1; i >= 0; i-- ) {
                    int fadeOffset = map( i, 0, deleteMissesIndex, 0, 12 ) + fadeFloor;
                    if ( fadeOffset > 12 ) {
                        fadeOffset = 12;
                        // showLEDsCore2 = -1;
                    }
                    // clearLEDsExceptMiddle(deleteMisses[i], -1);

                    //  Serial.println(fadeOffset);
                    //  Serial.flush();
                    //  Serial.println("fadeOffset = " + String(fadeOffset));
                    //  Serial.flush();
                    // b.printRawRow(0b00001010, deleteMisses[i] - 1, deleteFadeSides[fadeOffset], 0xfffffe);
                    b.printRawRow( 0b00000100, deleteMisses[ i ] - 1, deleteFade[ fadeOffset ],
                                   0xfffffe );
                   // showLEDsCore2 = 2;
                }

                if ( deleteMissesIndex == 0 && fadeClear == 0 ) {
                    fadeClear = 1;
                    // Serial.println( "fadeClear = 1" );
                    // Serial.flush( );
                    // showLEDsCore2 = -1;
                    if ( numberOfLocalChanges > 0 ) {
                        // saveLocalNodeFile( netSlot );
                        // Serial.print("\n\r");
                        // Serial.print("saving local node file\n\r");
                        // saveStateToSlot( netSlot );
                        // refreshConnections();
                        // numberOfLocalChanges = 0;
                    }
                }
            }
        }

        if ( ( row[ 0 ] == -18 || row[ 0 ] == -16 ) ) { // ! Button press detected
                                                        // (millis() - probingTimer > 500)) { //&&

            // Defer fresh in-probe presses by kWindowMs so a double-tap
            // can cancel them BEFORE they take effect. Without this, a
            // sequence like "in connect mode, double-tap disconnect to
            // undo" processes click 1 immediately (switches to clear),
            // then click 2 fires undo - leaving the user in clear mode
            // despite their double-tap intent. With deferral, click 1
            // sits in pendingInProbeButton; if click 2 arrives within
            // the window, service() fires undo, the bail handler at
            // the top of the loop clears the pending press, and the
            // mode switch never commits.
            //
            // pendingCommitting is true only when the top-of-loop
            // resolver injected this press after the window elapsed -
            // that's our signal to actually process it.
            //
            // Cross-button case: if a DIFFERENT press arrives while
            // one is already pending (e.g. user presses disconnect
            // then connect within 300ms - not a double-tap because
            // the IRQ clears the opposite hist ring on button switch),
            // we don't want to silently drop click 1. Commit the old
            // pending immediately and defer the new press so both
            // mode switches happen sequentially.
            if ( !pendingCommitting ) {
                if ( pendingInProbeButton != 0 && pendingInProbeButton != row[ 0 ] ) {
                    int newPress = row[ 0 ];
                    row[ 0 ] = pendingInProbeButton;       // process old pending in this iter
                    pendingInProbeButton     = newPress;   // defer the new press
                    pendingInProbeButtonTime = millis( );
                    pendingCommitting = true;              // (informational - already past the early-out)
                    // fall through to handler with row[0] = old pending
                } else {
                    pendingInProbeButton     = row[ 0 ];
                    pendingInProbeButtonTime = millis( );
                    row[ 0 ] = -1;
                    continue;
                }
            }

            // NB: the old "exit on g_probeDoubleTapBail" branch is gone.
            // Click 2 of a double-tap doesn't set buttonPress anymore
            // (see ProbeButton::processSample dbl branch), so we never
            // see -16/-18 for the second click - readProbe just returns
            // -1. The undo/redo fires via the deferred flags drained
            // from serviceCritical() and probeMode stays alive.

            // Serial.println("row[0] = " + String(row[0]));
            // Serial.println("setOrClear = " + String(setOrClear));
            // Serial.println("node1or2 = " + String(node1or2));
            // Serial.println("connectionsThisSession = " + String(connectionsThisSession));
            // Serial.println("probingTimer = " + String(probingTimer));
            // Serial.println("probeButtonTimer = " + String(probeButtonTimer));
            // Serial.println("probeHighlight = " + String(probeHighlight));
            // Serial.println("showLEDsCore2 = " + String(showLEDsCore2));
            // Serial.println("connectedRowsIndex = " + String(connectedRowsIndex));
            // Serial.println("--------------------------------\n\r1\n\r2\n\r3\n\r4\n\r5\n\r");
            // Serial.flush( );
            // blockProbeButton = 8000;
            // blockProbeButtonTimer = millis( );
            if ( row[ 0 ] == -18 ) { // clear button
                // Serial.println("-18 clear button\n\r");

                if ( setOrClear == 0 ) { // already in clear mode
                    // Serial.println("-18 setOrClear == 0\n\r");
                    nodesToConnect[ 0 ] = -1;
                    nodesToConnect[ 1 ] = -1;
                    node1or2 = 0;
                    // clearLEDsExceptRails();
                    blockProbeButton = 8000;
                    blockProbeButtonTimer = millis( );
                    probeHighlight = -1;
                    showLEDsCore2 = -1;
                    // connectionsThisSession = 0;
                    Serial.print( "\x1b[2K\r" ); // Clear the line and return cursor to start

                    Serial.flush( );
                    // Serial.println("setOrClear == 0");
                } else { // switch to clear mode

                    // Serial.println("-18 setOrClear == 1\n\r");
                    setOrClear = 0;
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
                    //           lastProbedRows[0] = -1;
                    // lastProbedRows[1] = -1;
                    // clearLEDsExceptRails();
                    showLEDsCore2 = 1;
                    node1or2 = 0;

                    // Serial.println("-18 connectionsThisSession = " + String(connectionsThisSession) + "\n\n\n\n\n\r");
                    if ( connectionsThisSession == 0 && bannerEmitted ) {
                        // Only rewind if the banner was actually printed -
                        // \x1b[3A jumps the cursor up 3 rows to overwrite
                        // the deferred banner, which would otherwise eat
                        // 3 lines of unrelated output above us.
                        Serial.print( "\x1b[3A\x1b[0J" );
                        Serial.flush( );
                        connectionsThisSession = 0;
                    }

                    // showProbeLEDs = 1;
                    firstEntry = false;
                    bannerEmitted = false;  // re-print banner for the new mode
                    goto restartProbing;
                }
                // break;
            } else if ( row[ 0 ] == -16 ) { // connect button

                if ( setOrClear == 1 ) { // already in connect mode
                    // showProbeLEDs = 2;
                    //  delay(100);
                    if ( node1or2 == 1 ) {
                        connectedRowsIndex = 0;
                        nodesToConnect[ 0 ] = -1;
                        nodesToConnect[ 1 ] = -1;
                        node1or2 = 0;
                        // Serial.println(probeHighlight);

                        blockProbeButton = 8000;
                        blockProbeButtonTimer = millis( );

                        probeHighlight = -1;
                        // clearLEDsExceptRails();
                        showLEDsCore2 = -2;
                        // waitCore2();

                        // Serial.println("-16 setOrClear == 1\n\r");
                        // if ( connectionsThisSession == 0 ) {
                        Serial.print( "\x1b[2K\r" ); // Clear the line and return cursor to start

                        Serial.flush( );
                        // connectionsThisSession = 0;
                        // }
                        goto restartProbingNoPrint;

                    } else {

                        // Serial.println("-16 setOrClear == 0 && node1or2 == 0\n\r");
                        //  Serial.println("setOrClear == 1 && node1or2 ==
                        //  0");
                    }
                } else { // switch to connect mode
                    // Serial.println("-16 setOrClear == 0\n\r");
                    setOrClear = 1;
                    // showProbeLEDs = 2;

                    probingTimer = millis( );
                    blockProbeButton = 8000;
                    blockProbeButtonTimer = millis( );
                    probeButtonTimer = millis( );
                    // showNets();
                    // showLEDsCore2 = 1;
                    sfProbeMenu = 0;
                    connectedRowsIndex = 0;
                    connectedRows[ 0 ] = -1;
                    connectedRows[ 1 ] = -1;
                    nodesToConnect[ 0 ] = -1;
                    nodesToConnect[ 1 ] = -1;
                    node1or2 = 0;
                    // clearLEDsExceptRails();
                    //  lastProbedRows[0] = -1;
                    //  lastProbedRows[1] = -1;

                    for ( int i = deleteMissesIndex - 1; i >= 0; i-- ) {

                        // b.printRawRow( 0b00000100, deleteMisses[ i ] - 1, 0, 0xfffffe );
                        //   Serial.print(i);
                        //   Serial.print("   ");
                        //   Serial.print(deleteMisses[i]);
                        //   Serial.print("    ");
                        //  Serial.println(map(i, 0,deleteMissesIndex, 0, 19));
                    }
                    // showLEDsCore2 = 1;
                    if ( connectionsThisSession == 0 && bannerEmitted ) {
                        Serial.print( "\x1b[3A\x1b[0J" ); // rewind 3 banner lines
                        Serial.flush( );

                    } else {
                        Serial.print( "\x1b[2K\r" ); // Clear the line and return cursor to start
                        Serial.flush( );
                    }
                    connectionsThisSession = 0;

                    // Serial.println("setOrClear == 1");

                    firstEntry = false;
                    bannerEmitted = false;  // re-print banner for the new mode
                    goto restartProbing;
                }
                // break;
            }

            // Serial.print("\n\rCommitting paths!\n\r");
            row[ 1 ] = -2;
            probingTimer = millis( );

            connectedRowsIndex = 0;

            node1or2 = 0;
            nodesToConnect[ 0 ] = -1;
            nodesToConnect[ 1 ] = -1;
            probeHighlight = -1;
            // showLEDsCore2 = -1;
            break;
        } else {
            // probingTimer = millis();
        }

        if ( isConnectable( row[ 0 ] ) == false && row[ 0 ] != -1 ) {
            row[ 0 ] = -1;
        }

        if ( row[ 0 ] != -1 && row[ 0 ] != row[ 1 ] ) { // && row[0] != lastProbedRows[0] &&
            // row[0] != lastProbedRows[1]) {

            lastProbedRows[ 1 ] = lastProbedRows[ 0 ];
            lastProbedRows[ 0 ] = row[ 0 ];
            if ( connectedRowsIndex == 1 ) {
                nodesToConnect[ node1or2 ] = connectedRows[ 0 ];

                // Serial.print("nodesToConnect[");
                // Serial.print(node1or2);
                // Serial.print("] = ");
                // Serial.println(nodesToConnect[node1or2]);
                // Serial.flush();

                // oled.clear();
                // oled.print(definesToChar(nodesToConnect[0]));
                // oled.print(" - ");
                // oled.show();
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
                if ( setOrClear == 1 ) {
                    Serial.print( "  -  " );
                }
                // numChars = Serial.print(node2Name);
                Serial.flush( );

                probeHighlight = nodesToConnect[ node1or2 ];
                if ( setOrClear == 1 ) {
                    // probeConnectHighlight = nodesToConnect[node1or2];
                    brightenNet( probeHighlight, 5 );
                    oled.clearPrintShow( bothNames, 2, true, true, true );
                }

                // oled.clearPrintShow(bothNames, 2, 0, 5, true, true, true);

                // Serial.print("probing Highlight: ");
                // Serial.println(probeHighlight);
                // showProbeLEDs = 1;

                if ( nodesToConnect[ node1or2 ] > 0 &&
                     nodesToConnect[ node1or2 ] <= NANO_RESET_1 && setOrClear == 1 ) {

                    // probeConnectHighlight = nodesToConnect[node1or2];
                    //  Serial.print("probeConnectHighlight = ");
                    //  Serial.println(probeConnectHighlight);
                    // showLEDsCore2 = 2;
                        showProbeLEDs = 11;
                    // // b.clear();
                    b.printRawRow( 0b0010001, nodesToConnect[ node1or2 ] - 1, 0x000121e,
                                   0xfffffe );
                    showLEDsCore2 = 2;
                    delay( 40 );
                    b.printRawRow( 0b00001010, nodesToConnect[ node1or2 ] - 1, 0x0f0498,
                                   0xfffffe );
                    showLEDsCore2 = 2;
                    delay( 40 );

                    b.printRawRow( 0b00000100, nodesToConnect[ node1or2 ] - 1, 0x4000e8,
                                   0xfffffe );
                    showLEDsCore2 = 2;
                     delay( 60 );
                     showLEDsCore2 = 2;
                }

                node1or2++;
                probingTimer = millis( );
                //showLEDsCore2 = 1;
                doubleSelectTimeout = millis( );
                doubleSelectCountdown = 200;
                // delay(500);

                // delay(3);
            }

            if ( node1or2 >= 2 || ( setOrClear == 0 && node1or2 >= 1 ) ) {

                probeHighlight = -1;
                // Serial.print("connectedRowsIndex: ");
                // Serial.print(connectedRowsIndex);
                // Serial.print(" nodesToConnect[0]: ");
                // Serial.print(nodesToConnect[0]);
                // Serial.print(" nodesToConnect[1]: ");
                // Serial.println(nodesToConnect[1]);

                // Serial.print("fuck");

                if ( setOrClear == 1 && ( nodesToConnect[ 0 ] != nodesToConnect[ 1 ] ) &&
                     nodesToConnect[ 0 ] > 0 && nodesToConnect[ 1 ] > 0 ) {
                    // b.printRawRow( 0b00011111, nodesToConnect[ 0 ] - 1, 0x0, 0x00000000 );
                    // b.printRawRow( 0b00011111, nodesToConnect[ 1 ] - 1, 0x0, 0x00000000 );
                    Serial.print( "\r              \r" );

                    // Serial.println("fuck");
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
                        continue;
                    }

                    int numChars = strlen( node1Name );
                    for ( int i = 0; i < SPACE_FROM_LEFT - numChars; i++ ) {
                        Serial.print( " " );
                    }
                    Serial.print( node1Name );
                    Serial.print( "  -  " );
                    numChars = Serial.print( node2Name );
                    // for (int i = 0; i < 12 - numChars; i++) {
                    //   Serial.print(" ");
                    //   }

                    // Check if the second node is a GPIO and print its direction
                    if ( nodesToConnect[ 1 ] >= RP_GPIO_1 && nodesToConnect[ 1 ] <= RP_GPIO_8 ) {
                        // Find the GPIO index in gpioDef array
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

                    if ( firstConnection == -3 ) {
                        // Add to RAM state - DON'T save yet, let auto-save handle it
                        addBridgeToState( nodesToConnect[ 0 ], nodesToConnect[ 1 ], -1, true );
                        numberOfLocalChanges++;
                        // refreshConnections(1, 1, 0);
                        // showLEDsCore2 = -1;
                        connectionsThisSession++;
                        break;

                    } else {

                        // Add to RAM state (local changes accumulated in RAM)
                        addBridgeToState( nodesToConnect[ 0 ], nodesToConnect[ 1 ], -1, true );
                        showProbeLEDs = 1;
                        numberOfLocalChanges++;
                        connectionsThisSession++;
                    }
                    brightenNet( -1 );

                    oled.clearPrintShow( bothNames, 2, true, true, true );
                    // oled.clear();
                    // oled.print(node1Name);
                    // oled.print(" - ");
                    // oled.print(node2Name);
                    // oled.print(" connected");
                    // oled.show();
                    // Serial.println(numberOfLocalChanges);

                    // NOTE: refreshLocalConnections() is already called inside addBridgeToState()
                    // No need to call it again here - that was causing double refresh delay!
                    fadeTimer = millis( );
                    // if (numberOfLocalChanges > 5) {
                    //   saveLocalNodeFile(netSlot);
                    //   // refreshConnections();
                    //   numberOfLocalChanges = 0;

                    // } // else {
                    //  saveLocalNodeFile(netSlot);

                    //}

                    row[ 1 ] = -1;

                    // doubleSelectTimeout = millis();
                    for ( int i = 0; i < 12; i++ ) {
                        deleteMisses[ i ] = -1;
                    }

                    doubleSelectTimeout = millis( );
                    doubleSelectCountdown = 400;

                } else if ( setOrClear == 0 ) {

                    char node1Name[ 12 ];

                    char node2Name[ 12 ];
                    strcpy( node2Name, "   " );
                    char bothNames[ 25 ];
                    strcpy( bothNames, node1Name );
                    strcat( bothNames, " - " );
                    strcat( bothNames, node2Name );

                    // int numChars = strlen(node1Name);
                    // for (int i = 0; i < SPACE_FROM_LEFT - numChars; i++) {
                    //   Serial.print(" ");
                    //   }
                    // Serial.print(node1Name);
                    // Serial.print(" ");
                    // numChars = Serial.print(node2Name);

                    for ( int i = 12; i > 0; i-- ) {
                        deleteMisses[ i ] = deleteMisses[ i - 1 ];
                        // Serial.print(i);
                        // Serial.print("   ");
                        // Serial.println(deleteMisses[i]);
                    }
                    // Serial.print("\n\r");
                    deleteMisses[ 0 ] = nodesToConnect[ 0 ];

                    // deleteMisses[deleteMissesIndex] = nodesToConnect[0];
                    if ( deleteMissesIndex < 12 ) {
                        deleteMissesIndex++;
                    }
                    fadeIndex = -3;

                    //  Serial.println("\n\r");
                    //  Serial.print("deleteMissesIndex: ");
                    //   Serial.print(deleteMissesIndex);
                    //   Serial.print("\n\r");
                    for ( int i = deleteMissesIndex - 1; i >= 0; i-- ) {

                        b.printRawRow( 0b00000100, deleteMisses[ i ] - 1,
                                       deleteFade[ map( i, 0, deleteMissesIndex, 0, 12 ) ],
                                       0xfffffe );
                        //   Serial.print(i);
                        //   Serial.print("   ");
                        //   Serial.print(deleteMisses[i]);
                        //   Serial.print("    ");
                        //  Serial.println(map(i, 0,deleteMissesIndex, 0, 19));
                    }
                    clearHighlighting( 0 );
                    //  Serial.println();
                    // Remove from RAM state - let auto-save handle persistence
                    // This removes ALL connections containing nodesToConnect[0]
                    bool removed = removeBridgeFromState( nodesToConnect[ 0 ], -1, true );

                    // The number of removed connections is tracked in lastRemovedNodesIndex
                    int rowsRemoved = removed ? lastRemovedNodesIndex : 0;
                    if ( removed ) {
                        numberOfLocalChanges += rowsRemoved;
                    }
                    // if ( rowsRemoved > 0 ) {

                    // Serial.print("connectionsThisSession: ");
                    // Serial.print(connectionsThisSession);
                    // Serial.flush( );
                    // }

                    // waitCore2();
                    if ( rowsRemoved > 0 ) {
                        connectionsThisSession++;
                        // connectionsThisSession++;
                        removeFade = 10;

                        // goto restartProbing;

                        // Print the disconnected nodes using our helper function
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

                        fadeClear = 0;
                        fadeTimer = 0;
                    } else {
                        // oled.clear( );
                        oled.clearPrintShow( node1Name, 2, true, true, true );
                    }
                }

                if ( firstConnection == -3 ) {
                    //
                    // showLEDsCore2 = 1;
                    // firstConnection = -1;

                    break;
                }

                node1or2 = 0;
                nodesToConnect[ 0 ] = -1;
                nodesToConnect[ 1 ] = -1;
                // row[1] = -2;
                doubleSelectTimeout = millis( );
            }

            row[ 1 ] = row[ 0 ];
        }
        // Serial.print("\n\r");
        // Serial.print(" ");
        // Serial.println(row[0]);

        if ( millis( ) - doubleSelectTimeout > 700 ) {
            // Serial.println("doubleSelectCountdown");
            row[ 1 ] = -2;
            lastReadRaw = 0;
            lastProbedRows[ 0 ] = 0;
            lastProbedRows[ 1 ] = 0;
            doubleSelectTimeout = millis( );
            doubleSelectCountdown = 700;
        }

        // Serial.println(doubleSelectCountdown);

        if ( doubleSelectCountdown <= 0 ) {

            doubleSelectCountdown = 0;
        } else {
            doubleSelectCountdown =
                doubleSelectCountdown - ( millis( ) - doubleSelectTimeout );

            doubleSelectTimeout = millis( );
        }

        probeTimeout = millis( );

        if ( firstConnection == -2 ) {
            firstConnection = -1;
            break;
        }
    } //! end main probing loop


    // Serial.println("fuck you");
    //  digitalWrite(RESETPIN, LOW);
    node1or2 = 0;
    nodesToConnect[ 0 ] = -1;
    nodesToConnect[ 1 ] = -1;
    // row[1] = -2;
    connectedRowsIndex = 0;
    connectedRows[ 0 ] = -1;
    probeActive = false;
    probeHighlight = -1;
    //showProbeLEDs = 4;
    brightenNet( -1 );

    // (Previously: immediate fileCacheFlushNowAll("probe_exit") here.
    // Removed - flush during probe-exit was visibly stopping the UI
    // for ~700ms even though the user often wasn't done. Now we just
    // mark dirty and let the FileCacheFlushService idle gate catch it
    // ~750ms after the user actually stops touching things. The PSRAM
    // cache is the durability layer until then; the only loss window
    // is a cold-yank in that ~750ms post-input quiet period.)
    // showLEDsCore2 = 1;
    //  Serial.print("millis() - timer[0] = ");
    //  Serial.println(millis() - timer[0]);
    //  Serial.print("millis() - timer[1] = ");
    //  Serial.println(millis() - timer[1]);
    //  Serial.print("millis() - timer[2] = ");
    //  Serial.println(millis() - timer[2]);
    //  Serial.print("millis() - timer[3] = ");
    //  Serial.println(millis() - timer[3]);

    if ( connectionsThisSession == 0 && bannerEmitted ) {
        // Only rewind 3 lines if the banner was actually printed - if we
        // bailed out on a double-tap before the banner fired, those
        // lines belong to whatever was on screen before probeMode and
        // must NOT be erased.
        Serial.print( "\x1b[3A\x1b[0J" );
        Serial.flush( );
        connectionsThisSession = 0;
    }

    Serial.flush( );

    // showLEDsCore2 = -1;
    // refreshLocalConnections(-1);
    // delay(10);
    // Only trigger a save on exit if state is actually dirty (has unsaved changes)
    // If state was already saved during probing idle, skip the save
    if ( globalState.isDirty( ) ) {
        // State has unsaved changes - let auto-save scheduler handle it
        // The scheduler will save within 1 second after exiting probing
    }
    // Reset local change counter regardless - we've either saved or will auto-save
    numberOfLocalChanges = 0;
    // delay(10);
    //refreshConnections( 1, 1, 0 );
    row[ 0 ] = -1;
    row[ 1 ] = -2;
    // showLEDsCore2 = -1;
    // sprintf(oledBuffer, "        ");
    // drawchar();

    // rotaryEncoderMode = wasRotaryMode;
    // routableBufferPower(0);
    // delay(10);
    // showLEDsCore2 = -1;
    oled.showJogo32h( );

    // Restore rotary divider
    rotaryDivider = savedRotaryDivider;

    // Clear any visible cursor LED before exiting based on zone
    if ( cursorZone == ZONE_BREADBOARD && encoderCursorNode >= 0 ) {
        b.printRawRow( 0b00000100, encoderCursorNode, 0x000000, 0x000000 );
    } else if ( cursorZone == ZONE_NANO && encoderCursorNode >= 0 ) {
        int pixel = getNanoHeaderPixel( encoderCursorNode );
        if ( pixel >= 0 )
            leds.setPixelColor( pixel, 0x000000 );
    } else if ( cursorZone >= ZONE_RAILS ) {
        clearLEDsExceptRails( );
    }

    // Clear encoder cursor globals and highlighting
    globalEncoderCursorNode = -1;
    globalEncoderCursorInHeader = 0;
    Highlighting::getInstance( ).clearHighlighting( );

    // Clear all color overrides using helper function
    // clearColorOverrides( true, true, false );

    // Clear inPadMenu flag
    inPadMenu = 0;

    // Reset first entry flag so next entrance starts at row 15 in breadboard
    firstProbeEntry = true;

    // Force LED update to clear cursor
    showLEDsCore2 = 2;

    // Wait for button to be released before exiting
    // This prevents the press that triggered probeMode from being detected again
    // while (probeButton.getButtonState() != 0) {
    //     delay(10);  // Wait for user to release button
    // }

    // Clear any residual button state and block for safety
    probeButton.clearButtonState( );
    blockProbeButton = 1000; // Extra 100ms safety margin
    blockProbeButtonTimer = millis( );
    clearColorOverrides( 1, 1, 0 );

    // Disable text layer when exiting probe mode

// Serial.print("switchPosition = ");
// Serial.println(switchPosition);
// Serial.print("showProbeLEDs = ");
// Serial.println(showProbeLEDs);
// Serial.print("lastProbeLEDs = ");
// Serial.println(lastProbeLEDs);
// Serial.flush();
    if (switchPosition == 1) {
        showProbeLEDs = 4;
    } else {
        showProbeLEDs = 3;
    }

//     Serial.print("switchPosition = ");
// Serial.println(switchPosition);
// Serial.print("showProbeLEDs = ");
// Serial.println(showProbeLEDs);
// Serial.print("lastProbeLEDs = ");
// Serial.println(lastProbeLEDs);
// Serial.flush();

    if ( exitToClickMenu ) {
        // The wheel click that ended probing now opens the click menu.
        // Done here, AFTER the full exit cleanup, so the menu starts from
        // the same state a normal idle-click entry would. The real click
        // event is long gone (Core 1 auto-clears it ~15ms after the
        // physical release), so re-arm a synthetic one and run clickMenu()
        // directly - same trick as the Menu FX tuner in Debugs.cpp, minus
        // the auto-clear race, since synthesizeEncoderClick() refreshes
        // the event timestamp.
        synthesizeEncoderClick( );
        Menus::getInstance( ).clickMenu( );
    }

    return 1;
}

float Probing::measureMode( int updateSpeed ) {
    // NOTE: measureModeActive LED indicator is handled by MeasureMode service
    // Wait for button release (use state-based check, doesn't consume event)
    // while ( checkProbeButtonState( ) != 0 ) {
    //     delay( 1 );
    // }
    //   removeBridgeFromNodeFile(ROUTABLE_BUFFER_IN, -1, netSlot, 1);
    // removeBridgeFromNodeFile(ROUTABLE_BUFFER_OUT, -1, netSlot, 1);
    //   addBridgeToNodeFile(ROUTABLE_BUFFER_OUT, ADC3, netSlot, 1);

    // refreshLocalConnections();
    float measurement = 0.0;
    // Loop until button pressed (use state-based check, doesn't consume event)
    while ( checkProbeButtonState( ) == 0 ) {
        measurement = ( readAdc( 7, 16 ) * ( 16.0 / 4090 ) ) - 8.0;
        if ( measurement > -0.05 && measurement < 0.05 ) {
            measurement = 0.0;
            delay( 1 );
        }
        uint32_t measColor = scaleBrightness(
            logoColors8vSelect[ map( (long)( measurement * 10 ), -80, 80, 0, 59 ) ], -50 );
        // Serial.println(map((long)(measurement*10), -80, 80, 0, 59));
        char measChar[ 10 ] = "         ";
        // b.print("        ", (uint32_t)0x000000,(uint32_t)0xffffff);
        b.clear( 0 );
        sprintf( measChar, "% .1f V", measurement );

        b.print( measChar, (uint32_t)measColor, (uint32_t)0xfffffe );

        Serial.print( "                        \r" );
        Serial.print( measChar );

        delay( updateSpeed );
    }
    // removeBridgeFromNodeFile(ROUTABLE_BUFFER_OUT, -1, netSlot, 1);

    // addBridgeToNodeFile(ROUTABLE_BUFFER_IN, RP_GPIO_23, netSlot, 1);

    // refreshLocalConnections(-1);
    // delay(20);
    showProbeLEDs = 3;
    return measurement;
}

unsigned long blinkTimer = 0;

int Probing::selectSFprobeMenu( int function ) {
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

int Probing::attachPadsToSettings( int pad ) {
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

int Probing::delayWithButton( int delayTime ) {
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

int Probing::chooseDAC( int justPickOne ) {
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

int Probing::chooseIsense( void ) {
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

int Probing::chooseADC( void ) {
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

int Probing::chooseGPIOinputOutput( int gpioChosen ) {
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

int Probing::chooseGPIO( int skipInputOutput ) {
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

float Probing::voltageSelect( int fiveOrEight ) {
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
// Must be declared before checkSwitchPosition() which uses it.
// volatile: written by core 1's probeLEDhandler, read here on core 0.
volatile unsigned long lastProbeLEDUpdateTime = 0;
const unsigned long LED_SETTLE_TIME_MS = 35; // Wait 15ms after LED update before reading current

// Global timestamp for when INA219 probe current was last read
// Other code can check this to avoid interference with current sensing
volatile unsigned long lastProbeCurrentCheckTime = 0;

// Defined below with the GPIO-powered buffer experiment (debug.probe_power_gpio):
// returns the RP_GPIO_x node currently powering the buffer, or -1 for DAC power.
static int gpioBufferPowerNode( void );
// Re-asserts the claimed pin's output-high drive if some other subsystem
// stomped it (see definition banner). No-op when nothing is claimed.
static void reassertGpioBufferDrive( void );
// Estimates the buffer load current from ADC7 droop with continuous V0
// normalization (see banner at definition); false when no GPIO is claimed.
static bool gpioDroopCurrentEstimate( float* current_mA, float* adc7V );
// Detector A: tip-side digital sense on PROBE_PIN (see definition banner).
// Returns a POSITION in switchPosition's convention: 0 = MEASURE (pin read
// HIGH), 1 = SELECT (pin read LOW), -1 = skipped (button held / sampler busy).
static int probeSwitchTipSense( void );
// Detector B for a GPIO feed: feed-side blink, ADC7 held vs collapsed (see
// definition banner). Returns 0 = MEASURE (collapsed), 1 = SELECT (held),
// -1 = skipped (touch in progress / ADC busy / feed dark). *pctOut = the
// held percentage for the stats line.
static int probeSwitchFeedBlink( int* pctOut );
// debug.probe_switch_stats: one line per switch evaluation (source, current,
// droop anchor, thresholds, both detectors, shadow verdict, resulting position).
static void printProbeSwitchStats( const char* src, float mA, float adc7V,
                                   int detA, int detB, int blinkPct,
                                   int shadow, bool agreeMode,
                                   int pos, bool changed );

int Probing::checkSwitchPosition( ) { // 0 = measure, 1 = select

    // Pending-nudge retry + claim-pin viability re-check for InfraPaths.
    // Piggybacks on this service's ~500ms cadence, same as
    // reassertGpioBufferDrive below.
    infraServiceTick( );

    // Timing: only sample at a fixed interval.
    static unsigned long last_check_millis = 0;
    // SELECT->MEASURE needs two consecutive below-low readings (see below).
    static bool s_pendingMeasureFlip = false;

    if ( jumperlessConfig.dacs.auto_connect_probe <= 0 ) {
        switchPosition = 1;
        return switchPosition;
    }

    if ( checkingButton == 1 ) {
        // Serial.println( "checkingButton" );
        return switchPosition;
    }

    // CRITICAL: Also check if async DMA transfer is still in progress!
    // Reading probe current via INA219 while DMA is transferring to the LED pin
    // can cause flickers and interference. Wait for DMA to complete.
    // if ( probeLEDs.isDMABusy( ) ) {
    //     return switchPosition; // Try again next time - DMA still busy
    // }

    // CRITICAL: Also wait for LED current to stabilize after any LED update!
    // When LED color/brightness changes, current draw changes and INA219 needs time to settle.
    // This prevents spurious switch position changes from transient current readings.
    if ( ( millis( ) - lastProbeLEDUpdateTime ) < LED_SETTLE_TIME_MS ) {
        return switchPosition; // Try again next time - LED current still settling
    }

    // Timing gate: exit early if interval hasn't elapsed.
    unsigned long now_ms = millis( );
    if ( ( now_ms - last_check_millis ) < ProbeSwitch::getInstance( ).interval_ms ) {
        return switchPosition;
    }
    last_check_millis = now_ms;

    // LED keep-alive: the addressable LED in the probe is written ONCE per
    // mode change and never again, so a chip reset (rail glitch, cable
    // wiggle) leaves it dark indefinitely - and with it kills the current
    // signature this whole classifier depends on. Re-send the last static
    // idle pattern every few seconds so a reset chip heals itself. Animated
    // and transient modes re-send themselves; only the one-shot idle
    // signatures (measure / select idle / measure dim) need this. The send
    // consumes this check slot on purpose: the settle gate above needs to
    // see the LED write before the next current read.
    static unsigned long lastLedKeepaliveMs = 0;
    if ( showProbeLEDs == 0 &&
         ( lastProbeLEDs == 3 || lastProbeLEDs == 4 || lastProbeLEDs == 6 ) &&
         ( now_ms - lastLedKeepaliveMs ) > 5000 ) {
        lastLedKeepaliveMs = now_ms;
        showProbeLEDs = lastProbeLEDs;
        return switchPosition;
    }
    // Sensing strategy follows the LIVE feed source (InfraPaths), not a
    // config flag:
    //   DAC0    -> INA1 current (its 2-ohm shunt R57 is hardwired in DAC_0's
    //              output path - the only feed INA1 can see)
    //   GPIO    -> ADC7 droop estimate (INA1 is blind), DAC0-swap fallback
    //   no feed -> buffer unpowered (yielded to a user connection, or every
    //              candidate claimed): no current signature exists, so hold
    //              the last known position instead of guessing.
    if ( infraProbePowerSource( ) < 0 ) {
        return switchPosition;
    }
    // (An unconditional `checkingButton = 0` used to sit here - the button
    // sampler owns that flag; a switch check has no business clearing it.)

    // Update global timestamp BEFORE reading current so other code knows we're about to read
    lastProbeCurrentCheckTime = millis();

    // Self-heal the claim's pin drive. Several subsystems write gpioState /
    // pin registers for the routable GPIO bank (setGPIO on every refresh,
    // updateStateFromGPIOConfig from the probe GPIO menu, MicroPython, PWM
    // teardown); any of them flipping the claimed pin low kills the tip
    // while the bridge bookkeeping still says "connected", so the reclaim
    // above never fires. Seen on hardware: GPIO_1 OUTPUT-LOW with the claim
    // bridge intact - measure probing dead after the first tap. Cheap
    // idempotent register check every switch-check pass (~500ms).
    reassertGpioBufferDrive( );

    // Use the zero-corrected current (raw - probe_current_zero). The switch
    // thresholds are calibrated against this same corrected value, so existing
    // users keep their saved thresholds across firmware updates without having
    // to re-run calibration.
    //
    // Under GPIO buffer power INA1 sees nothing (its shunt lives in DAC_0's
    // output path), so sensing reads the load current as voltage droop on
    // ADC7 with a continuously-normalized unloaded peak (V0) - passive,
    // sub-ms, no tip glitch, no dependency on the unknown boot switch
    // position. (The old "swap the feed to DAC0 for one INA read" fallback
    // is gone: its trigger - the droop estimate failing - could only happen
    // with no GPIO claim, i.e. when this branch wasn't taken at all, so it
    // never ran; and it would have read the user's DAC0 load if DAC0 had
    // been claimed. Detector A in Phase 2 covers the "no usable current
    // signature" case instead.)
    float current_mA;
    float adc7V = 0.0f;
    const char* swSource = "ina";
    int gpioPowerNode = gpioBufferPowerNode( ); // >0 only while a GPIO claim is live
    if ( gpioPowerNode > 0 && gpioDroopCurrentEstimate( &current_mA, &adc7V ) ) {
        swSource = "droop";
        if ( showProbeCurrent == 1 ) {
            Serial.print( "                          \rSwitch-check current (ADC7 droop): " );
            Serial.print( current_mA );
            Serial.print( " mA at " );
            Serial.print( adc7V, 3 );
            Serial.print( " V" );
            Serial.flush( );
        }
    } else {
        current_mA = checkProbeCurrent( );
    }

    // ------------------------------------------------------------------
    // Two detectors, evaluated on every check and always logged:
    //
    //   A  tip-side digital sense on PROBE_PIN. In MEASURE the pin powers
    //      the probe's LED chain + its 100uF supply cap, so a 2us drive-low
    //      then release reads back HIGH; in SELECT the pin is the needle -
    //      floating, or on a pad ladder that drains it - and reads LOW.
    //      No ADC, no I2C, ~6us, and it does not care what the LEDs draw.
    //   B  the per-feed load signature. DAC0 feed: INA1 zero-corrected mA
    //      with the calibrated hysteresis pair plus a select_max ceiling
    //      (above it the buffer is LOADED by a tip touch, never SELECT).
    //      GPIO feed: a ~20us feed-side blink - ADC7 collapses in MEASURE
    //      (nothing else drives BUFFER_IN) and is held by the LED supply
    //      cap through the cable in SELECT.
    //
    // debug.probe_switch_agree selects who DECIDES: the legacy classifier
    // (B only, current thresholds, kept verbatim below) or the agreement
    // classifier (A == B sets, disagreement holds, four disagreements in a
    // row adopt A - A is right in every persistent case: dark LED, no
    // probe, weak GPIO droop, measure-position load; B is only right in
    // the transient select + hot-net touch). Until promoted, the agreement
    // verdict is computed as a SHADOW and printed next to the legacy one.
    // ------------------------------------------------------------------
    // All verdicts below use switchPosition's convention: 0 = MEASURE,
    // 1 = SELECT; -1 = no opinion; B may also say 2 = LOADED.
    const bool agreeMode = jumperlessConfig.debug.probe_switch_agree;
    const int detA = probeSwitchTipSense( );

    int detB = -1;
    int blinkPct = -1;
    if ( gpioPowerNode > 0 ) {
        detB = probeSwitchFeedBlink( &blinkPct );
    } else {
        float selMax = jumperlessConfig.calibration.probe_switch_select_max_ma;
        if ( selMax > 0.0f && current_mA > selMax ) {
            detB = 2; // LOADED: a touch, not the switch
        } else if ( current_mA > jumperlessConfig.calibration.probe_switch_threshold_high ) {
            detB = 1; // SELECT signature
        } else if ( current_mA < jumperlessConfig.calibration.probe_switch_threshold_low ) {
            detB = 0; // MEASURE signature
        }
    }
    // Touch veto for a B-driven claim of SELECT: while the tip sits on a
    // pad, the elevated load is the touch, not the LED chain. (Applies to
    // both feeds - a held pad in measure position loads either.)
    if ( detB == 1 ) {
        int padSense = readAdc( 5, 4 );
        if ( padSense >= jumperlessConfig.calibration.minimum_probe_reading ) {
            detB = -1;
        }
    }

    // Shadow / agreement verdict: 0 M, 1 S, -1 hold.
    static int s_disagreeRun = 0;
    int shadow = -1;
    if ( detA >= 0 && detB >= 0 && detB <= 1 && detA == detB ) {
        shadow = detA;
        s_disagreeRun = 0;
    } else if ( detA >= 0 || detB >= 0 ) {
        s_disagreeRun++;
        if ( s_disagreeRun >= 4 && detA >= 0 ) {
            shadow = detA;
        }
    }

    bool changed = false;
    if ( agreeMode ) {
        // -------- agreement classifier decides --------
        // Dark-LED heal: SELECT position, INA/blink says MEASURE, tip says
        // SELECT - the LED chip may have reset dark. Re-send the idle pattern
        // once per disagreement run so B can recover; the position holds.
        if ( switchPosition == 1 && detA == 1 && detB == 0 && s_disagreeRun == 1 &&
             showProbeLEDs == 0 ) {
            showProbeLEDs = ( lastProbeLEDs == 7 ) ? 7 : 4;
        }
        if ( shadow >= 0 && shadow != switchPosition ) {
            switchPosition = shadow;
            changed = true;
        }
        s_pendingMeasureFlip = false;
    } else {
        // -------- legacy classifier decides (B only) --------
        // HYSTERESIS LOGIC to prevent oscillation:
        //   MEASURE -> SELECT: only when current > HIGH threshold
        //   SELECT -> MEASURE: only when current < LOW threshold
        if ( switchPosition == -1 ) {
            // Boot / unknown: classify from the ABSOLUTE signatures - MEASURE
            // drives the high-impedance tip (~0 mA), SELECT powers the probe
            // LEDs (~1-1.5 mA). A low reading alone can't be trusted yet: if
            // the LED chip is dark, SELECT also reads ~0 mA. So on the first
            // low reading, light the select-idle pattern and read again next
            // check - current appearing means SELECT with a dark LED;
            // still-low means genuinely MEASURE.
            if ( current_mA > jumperlessConfig.calibration.probe_switch_threshold_high ) {
                int padSense = readAdc( 5, 4 ); // same touch veto as MEASURE->SELECT
                if ( padSense < jumperlessConfig.calibration.minimum_probe_reading ) {
                    switchPosition = 1;
                    changed = true;
                }
                s_pendingMeasureFlip = false;
            } else if ( current_mA < jumperlessConfig.calibration.probe_switch_threshold_low ) {
                if ( !s_pendingMeasureFlip ) {
                    s_pendingMeasureFlip = true;
                    if ( showProbeLEDs == 0 ) {
                        showProbeLEDs = 4;
                    }
                } else {
                    s_pendingMeasureFlip = false;
                    switchPosition = 0;
                    changed = true;
                }
            }
        } else if ( switchPosition == 0 ) {
            s_pendingMeasureFlip = false;  // only meaningful while in SELECT
            // Currently in MEASURE mode - only switch to SELECT if current exceeds
            // HIGH threshold. Touch veto: the buffer is POWERED from the
            // BUFFER_IN net, so a tip touch in measure position loads the same
            // rail the select-LED signature rides on - by magnitude alone it
            // looks exactly like the switch flipping to SELECT. The pad-sense
            // ADC knows the difference: while the tip is parked on a pad the
            // elevated current is touch load, not the LED, so hold MEASURE.
            if ( current_mA > jumperlessConfig.calibration.probe_switch_threshold_high ) {
                int padSense = readAdc( 5, 4 );
                if ( padSense >= jumperlessConfig.calibration.minimum_probe_reading ) {
                    // tip is on a pad - don't classify from a loaded rail
                } else {
                    switchPosition = 1;
                    changed = true;
                }
            }
        } else {
            // Currently in SELECT mode - only switch to MEASURE if current falls
            // below LOW threshold. A below-low reading here is ambiguous: a
            // genuine flip to MEASURE, or the probe's LED chip having reset
            // dark (a rail glitch when the buffer feed relocates, a cable
            // wiggle) - the select-idle LED's draw IS the signature this
            // classifier reads. So: re-send the LED pattern and require the
            // low reading to survive the NEXT check too.
            if ( current_mA < jumperlessConfig.calibration.probe_switch_threshold_low ) {
                if ( !s_pendingMeasureFlip ) {
                    s_pendingMeasureFlip = true;
                    if ( showProbeLEDs == 0 ) {
                        showProbeLEDs = ( lastProbeLEDs == 7 ) ? 7 : 4;
                    }
                } else {
                    s_pendingMeasureFlip = false;
                    switchPosition = 0;
                    changed = true;
                }
            } else {
                if ( s_pendingMeasureFlip && jumperlessConfig.debug.probe_switch_stats ) {
                    Serial.println( "[switch] measure flip discarded - LED re-lit (chip had reset)" );
                }
                s_pendingMeasureFlip = false;
            }
        }
    }

    if ( switchPosition == 0 && changed == true ) {
        showProbeLEDs = 3; // measure
    } else if ( switchPosition == 1 && changed == true ) {
        showProbeLEDs = 4; // select idle
    }

    if ( jumperlessConfig.debug.probe_switch_stats ) {
        printProbeSwitchStats( swSource, current_mA, adc7V, detA, detB, blinkPct,
                               shadow, agreeMode, switchPosition, changed );
        // (printProbeSwitchStats flushes; no unconditional flush on the hot
        // path - it stalled this service on the USB CDC buffer every check.)
    }

    return switchPosition;
}

// ABSOLUTE probe-path current (INA1) with NO probe_current_zero subtraction.
// This is the reference used for switch-position detection and threshold
// calibration: because the zero offset (measured with DAC0 disconnected, and
// re-measured every boot) drifts relative to operating conditions, subtracting
// it would put calibration and runtime in different frames. Switch detection
// only needs to tell SELECT (DAC0 sourcing LED current) from MEASURE (DAC0
// driving the high-impedance tip, ~0 mA), so the raw absolute value is what we
// compare on BOTH sides.
float Probing::checkProbeCurrentRaw( void ) {
#if defined(OG_JUMPERLESS)
    return 0.0f;
#else
    // Last known-good reading, served whenever a fresh one can't be taken.
    // The INA219 driver returns 0 from a failed I2C transaction, and 0mA is
    // indistinguishable from "switch in MEASURE" - returning the cached value
    // instead keeps one bus glitch from flipping the detected position.
    static float lastGoodCurrent_mA = 0.0f;

    // While the function generator runs, core1 streams DAC samples over this
    // same Wire bus and TwoWire has no cross-core lock - reading here would
    // interleave two masters' buffer state and corrupt both transactions.
    if ( wavegen.isRunning( ) ) {
        return lastGoodCurrent_mA;
    }

    // NOTE: no conversion-flag wait here. CNVR only clears when the POWER
    // register is read (which this path never does), so the old wait loops
    // fell straight through anyway. The current register is at most one
    // ~17ms averaging window stale, which is fine for switch sensing at a
    // 500ms check interval - and skipping the polls keeps this call fast
    // (<1ms worst case) and off the bus.
    INA1.getLastError( ); // flush stale error flag
    for ( int attempt = 0; attempt < 3; attempt++ ) {
        float current = INA1.getCurrent_mA( );
        if ( INA1.getLastError( ) == 0 ) {
            lastGoodCurrent_mA = current;
            return current;
        }
        delayMicroseconds( 200 );
    }
    return lastGoodCurrent_mA;
#endif
}

// Zero-corrected probe current for the user-facing "current at the tip" display.
// This is checkProbeCurrentRaw() minus the calibrated rest offset; do NOT use
// this for switch detection or threshold calibration (use the raw value).
float Probing::checkProbeCurrent( void ) {
    float current = checkProbeCurrentRaw( );

    // Serial.print("current (before zero) = ");
    // Serial.println(current);
    // Serial.flush();
    current = current - jumperlessConfig.calibration.probe_current_zero;

    // Serial.print("current (after zero) = ");
    // Serial.println(current);
    // Serial.flush();

    if ( showProbeCurrent == 1 ) {
        Serial.print( "                          \rProbe current: " );
        Serial.print( current );
        Serial.print( " mA" );
        Serial.flush( );

        // for (int i = 0; i < (int)(current*10.0); i++) {
        //   Serial.print("*");
        // }
        // Serial.println();
        // Serial.flush();
    }
    // Serial.println();
    // Serial.flush();
    // if (millis() % 1000 < 10) {
    //   Serial.print("current: ");
    //   Serial.print(current);
    //   Serial.println(" mA\n\r");
    //   Serial.flush();
    //  }

    // for (int i = 1; i < 4; i++) {
    //   Serial.print("timer[");
    //   Serial.print(i);
    //   Serial.print("]: ");

    //     Serial.println(timer[i] - timer[i - 1]);
    //     //Serial.print("\t");
    //   }

    // digitalWrite(10, HIGH);

    return current;
}

// Median of n genuinely independent INA1 samples in the zero-corrected frame
// (the same frame the switch thresholds are calibrated against). Each sample
// arms CNVR by reading the POWER register and waits for the NEXT completed
// conversion (~17ms with 16-sample averaging), so n samples cost ~n*20ms -
// calibration and self-test only, never the hot path. I2C failures are
// skipped; if every sample fails (or the wavegen owns the bus), falls back
// to checkProbeCurrent()'s cached last-good value.
float Probing::probeCurrentMedian( int n ) {
#if defined(OG_JUMPERLESS)
    return 0.0f;
#else
    extern float medianInPlace( float* vals, int n ); // Apps.cpp
    if ( n < 1 )
        n = 1;
    if ( n > 32 )
        n = 32; // ponytail: fixed stack buffer; 32x20ms is already absurdly long
    float samples[ 32 ];
    int good = 0;
    for ( int i = 0; i < n; i++ ) {
        if ( wavegen.isRunning( ) )
            break; // shared Wire bus, no cross-core lock (see checkProbeCurrentRaw)
        INA1.getLastError( );      // flush stale error flag
        (void)INA1.getPower_mW( ); // reading POWER clears CNVR
        unsigned long timeout_start = millis( );
        while ( !INA1.getConversionFlag( ) && ( millis( ) - timeout_start < 25 ) ) {
            delayMicroseconds( 100 );
        }
        float sample = INA1.getCurrent_mA( );
        if ( INA1.getLastError( ) == 0 ) {
            samples[ good++ ] = sample;
        }
    }
    if ( good == 0 ) {
        return checkProbeCurrent( );
    }
    return medianInPlace( samples, good ) - jumperlessConfig.calibration.probe_current_zero;
#endif
}

float Probing::checkProbeCurrentZero( void ) {
#if defined(OG_JUMPERLESS)
    // Only calibrate INA0 offset on OG, as INA1 doesn't exist.
    unsigned long timeout_start = millis( );
    while ( !INA0.getConversionFlag( ) && ( millis( ) - timeout_start < 20 ) ) {
        delayMicroseconds( 100 );
    }
    currentReadingOffset0_mA = INA0.getCurrent_mA( );
    jumperlessConfig.calibration.probe_current_zero = 0.0f;
    showProbeLEDs = 4;
    return 0.0f;
#else
    // return 0.0f;

    // Serial.println("\n\n\n\\n\n\n\n\n\n\n\n checkingProbeCurrentZero \n\n\n\n\n\n");
    // Serial.flush();
//delay(2000);

    showProbeLEDs = 10; // request probe LED off

    // showProbeLEDs is only a request flag consumed by probeLEDhandler() on
    // core1. Sampling INA1 while the probe LED is still lit bakes ~1.8mA of
    // LED current into the zero offset, which then poisons every corrected
    // reading (and the switch thresholds calibrated against them). Wait for
    // the handler to consume the request, then let the current settle.
    unsigned long ledOffWaitStart = millis( );
    while ( showProbeLEDs != 0 && ( millis( ) - ledOffWaitStart < 100 ) ) {
        delayMicroseconds( 100 );
    }
    delay( LED_SETTLE_TIME_MS + 5 );

    // Temporarily disconnect DAC0 from whatever it is connected to so
    // zero-calibration reads the true offset, then restore those exact links.
    // Park the infra probe feed first: the remove's flush-refresh runs
    // infraEvaluate, which would re-add the DAC0->BUF_IN feed from its
    // registry mid-sampling. (The feed itself adds ~nothing to the reading -
    // the buffer input is high-impedance - but the remove/restore below must
    // only ever see USER routes.)
    bool wasProbePowerOn = infraProbePowerWanted( );
    infraSetProbePowerEnabled( false );
    extern int lastRemovedNodes[20];
    extern int lastRemovedNodesIndex;

    int savedNodes[20];
    int savedCount = 0;

    bool hadConnections = removeBridgeFromState( DAC0, -1, true );
    if ( hadConnections && lastRemovedNodesIndex > 0 ) {
        for ( int i = 0; i < lastRemovedNodesIndex && i < 20; i++ ) {
            savedNodes[i] = lastRemovedNodes[i];
        }
        savedCount = lastRemovedNodesIndex;
    }

    // Always sync with core2 before sampling: the disconnect above (or a
    // refreshConnections() the caller just issued, as in first-start
    // calibration) may still be mid-apply, and sampling while DAC0 is still
    // routed reads load current instead of the true offset.
    waitCore2( );
    delayMicroseconds( 10000 );

    extern float medianInPlace( float* vals, int n ); // Apps.cpp
    const int div = 8; // several samples; a single reading is too noisy for a stored offset
    float current = 0.0;
    float zeroSamples[ div ];
    int goodSamples = 0;

    // Take genuinely independent samples: CNVR only clears when the POWER
    // register is read, so arm it before each sample and wait for the NEXT
    // completed conversion (up to ~17ms with 16-sample shunt+bus averaging).
    // Without the arm, the flag stays latched set and all 8 "samples" can be
    // re-reads of one averaging window. ~140ms worst case total - fine here,
    // this only runs at boot and inside calibration apps, never the hot path.
    // Samples that fail on the bus (driver returns 0 + error flag) are
    // skipped rather than counted as fake 0mA readings. Median, not mean:
    // one sample taken while something still settles (LED relatch, crossbar
    // apply) would drag a mean and poison every corrected reading after it.
    for ( int i = 0; i < div; i++ ) {
        INA1.getLastError( );        // flush stale error flag
        (void)INA1.getPower_mW( );   // reading POWER clears CNVR
        unsigned long timeout_start = millis( );
        while ( !INA1.getConversionFlag( ) && ( millis( ) - timeout_start < 25 ) ) {
            delayMicroseconds( 100 );
        }
        float sample = INA1.getCurrent_mA( );
        if ( INA1.getLastError( ) == 0 ) {
            zeroSamples[ goodSamples++ ] = sample;
        }
    }

    if ( goodSamples > 0 ) {
        current = medianInPlace( zeroSamples, goodSamples );
        jumperlessConfig.calibration.probe_current_zero = current;
    } else {
        // Every read failed - bus problem, not a measurement. Keep the
        // previous zero instead of storing garbage.
        Serial.println( "\n\rWARNING: probe current zero calibration failed (I2C errors), keeping previous value" );
        current = jumperlessConfig.calibration.probe_current_zero;
    }

    // Also refresh INA0's current offset here (this runs later than startup
    // and usually gives a cleaner zero for the live current-sense path).
    // Same guard: don't let a failed read store a fake 0 offset.
    INA0.getLastError( );
    float offset0 = INA0.getCurrent_mA( );
    if ( INA0.getLastError( ) == 0 ) {
        currentReadingOffset0_mA = offset0;
    }

    // Serial.print("Zero calibration current = ");
    // Serial.println(current);

    // saveConfig();

    //!!!!!!!!!!!!!!!

    if ( hadConnections && savedCount > 0 ) {
        for ( int i = 0; i < savedCount; i++ ) {
            int otherNode = savedNodes[i];
            // NEVER re-add ROUTABLE_BUFFER_IN here: the feed is
            // infra-managed and re-adds itself from its registry. A user
            // copy created here reads as a claim on the buffer, the feed
            // yields to it, and switch sensing goes dead (no live source).
            if ( otherNode != ROUTABLE_BUFFER_IN && otherNode > 0 ) {
                addBridgeToState( DAC0, otherNode, -1, true );
            }
        }
    }

    // Un-park the feed; the next rebuild re-adds it (nudge if the restore
    // loop above didn't already trigger one).
    infraSetProbePowerEnabled( wasProbePowerOn );
    if ( wasProbePowerOn ) {
        infraNudge( );
    }

    showProbeLEDs = 4;
    return current;
#endif
}

// ---------------------------------------------------------------------------
// GPIO buffer power: the routable buffer fed from an unused RP GPIO driven
// HIGH - the InfraPaths fallback when the user has claimed DAC0
// (debug.probe_power_gpio, which once opted into this, is now ignored).
// INA1 only sees current when DAC0 sources the buffer, so switch-position
// sensing reads the load current as ADC7 voltage droop instead (continuous
// unloaded-voltage normalization - see the droop banner below). The DAC
// bridge swap survives only as a fallback while no V0 exists yet.
// ---------------------------------------------------------------------------

// gpioDef index currently powering the buffer, -1 = DAC power (normal).
static int s_gpioPowerIdx = -1;
// The claimed pin's previous config, restored on release.
static int s_gpioPowerSavedDir = 1;
static uint8_t s_gpioPowerSavedState = 0;

// Droop-based current sensing: the GPIO pad's output impedance plus the
// crosspoint resistance between the pad and BUFFER_IN act as a free shunt -
// the buffer load current appears as a voltage droop on the net, the buffer
// replicates it at unity gain, and ADC7 is hardwired to the buffer output.
// So a passive ADC7 read IS a current measurement, and checkSwitchPosition()
// can keep using the SAME calibrated select/measure current thresholds
// without the periodic DAC bridge swap (and its tip glitch).
//
// Zero is continuous, not claim-time: we don't know the switch position at
// boot, so a one-shot (I,V) anchor taken while SELECT is loading the tip
// puts MEASURE at a large NEGATIVE current and SELECT near zero - and the
// detector latches the wrong side of the hysteresis forever. Instead track
// V0 = the unloaded tip voltage (seeded from persisted probe_droop_v0 on
// GPIO claim, fast attack to any higher ADC7 reading, slow track while the
// estimate looks unloaded) and map:
//   I(v) = max(0, (V0 - v) / R_droop)
// New peaks persist back into config; rail drift is absorbed by the slow
// track. Seen on hardware: measure 3.339V / select 3.196V -> ~4.8mA of real
// separation once V0 is the unloaded peak.
static float s_gpioDroopV0 = 0.0f; // continuously-normalized unloaded volts
static bool s_gpioDroopValid = false;
// The droop shunt resistance now comes from infraProbeDroopOhms():
// calibration.probe_droop_ohms when the INA-referenced calibration has run,
// else probe_pad_ohms + crosspoints-of-the-actual-routed-feed x
// crosspoint_resistance (exact - infra feed bridges are never duplicated).
// Slow-track gain while I looks unloaded (~0). At the ~500ms switch-check
// interval, 0.08 settles V0 onto a new rail level in a couple of seconds
// without pulling V0 down during SELECT (I is large then, so no track).
static const float kGpioDroopV0Track = 0.08f;
// Below this, treat the tip as unloaded and slow-track V0. Wide enough that
// a few mV of ADC noise / rail wander after a peak attack still re-zeros,
// but well under probe_switch_threshold_low so SELECT never decays V0.
static const float kGpioDroopUnloadedMax_mA = 0.50f;

static void seedGpioDroopV0FromConfig( void ) {
    float v0 = jumperlessConfig.calibration.probe_droop_v0;
    if ( v0 < 3.0f || v0 > 3.6f )
        v0 = 3.35f;
    s_gpioDroopV0 = v0;
    s_gpioDroopValid = true;
}

// (Candidate SELECTION - which GPIO to claim, whether a DAC is free - moved
// into routing/InfraPaths.cpp. Probing keeps only the PIN-LEVEL hardware
// work below, because it owns the droop model and the gpioState/direction
// bookkeeping that refreshConnections' setGPIO pass re-asserts.)

// Pin-level GPIO power claim, called from InfraPaths' GPIO candidate
// onActivate. The bridge itself is InfraPaths' business; this does the pin:
// save the previous config (fresh claims only - a re-assert of the same
// index must NOT capture our own forced output-high as the "previous"
// config), force output-high in the config model so refreshConnections'
// setGPIO pass re-asserts instead of stomping it, seed the droop model,
// and drive the pad. 12mA drive: stiffer feed -> less droop while the pad
// ladder / probe LED load the tip; setGPIO() never touches drive strength,
// so it survives refreshes.
void probeGpioPowerHwClaim( int idx ) {
    if ( idx < 0 || idx > 7 )
        return;
    int pin = gpioDef[ idx ][ 0 ];
    if ( s_gpioPowerIdx != idx ) {
        s_gpioPowerIdx = idx;
        s_gpioPowerSavedDir = globalState.config.gpioDirection[ idx ];
        s_gpioPowerSavedState = gpioState[ idx ];
        seedGpioDroopV0FromConfig( );
    }
    globalState.config.gpioDirection[ idx ] = 0;
    gpioState[ idx ] = 1;
    gpio_set_function( pin, GPIO_FUNC_SIO );
    gpio_set_drive_strength( pin, GPIO_DRIVE_STRENGTH_12MA );
    gpio_set_dir( pin, true );
    gpio_put( pin, true );
}

// Pin-level release, called from InfraPaths' GPIO candidate onDeactivate.
// If MicroPython took the pin, dir/pulls are Python's business now - only
// the 12mA pad drive was ours to undo.
void probeGpioPowerHwRelease( int idx ) {
    if ( idx < 0 || idx > 7 || s_gpioPowerIdx != idx )
        return;
    int pin = gpioDef[ idx ][ 0 ];
    if ( globalState.config.gpioPythonOwned[ idx ] ) {
        gpio_set_drive_strength( pin, GPIO_DRIVE_STRENGTH_4MA );
    } else {
        globalState.config.gpioDirection[ idx ] = s_gpioPowerSavedDir;
        gpioState[ idx ] = s_gpioPowerSavedState;
        gpio_set_dir( pin, false );
        gpio_set_drive_strength( pin, GPIO_DRIVE_STRENGTH_4MA ); // pad default
    }
    s_gpioPowerIdx = -1;
    s_gpioDroopValid = false;
}

// Exposed so checkSwitchPosition() can tell whether the buffer is currently
// GPIO-powered (and which node to swap during its current read).
static int gpioBufferPowerNode( void ) {
    return ( s_gpioPowerIdx >= 0 ) ? gpioDef[ s_gpioPowerIdx ][ 1 ] : -1;
}

// Exposed (via Probing.h) so GPIO-bank config appliers can skip the claim.
int probeGpioPowerClaimIdx( void ) {
    return s_gpioPowerIdx;
}

// The claim's pin must stay SIO / output / driving HIGH or the measure tip
// dies. gpioState[] doubles as the display state AND setGPIO()'s output
// value, and several writers can zero it (updateStateFromGPIOConfig's
// "output low" default, PWM teardown, MicroPython) - after which the next
// refreshConnections drives the pin low with the claim bridge still in
// place. Re-assert everything the claim needs; all writes are idempotent.
static void reassertGpioBufferDrive( void ) {
    if ( s_gpioPowerIdx < 0 )
        return;
    if ( globalState.config.gpioPythonOwned[ s_gpioPowerIdx ] )
        return; // Python owns it now; routableBufferPower(1) will re-claim
    int pin = gpioDef[ s_gpioPowerIdx ][ 0 ];
    bool stomped = gpio_get_function( pin ) != GPIO_FUNC_SIO ||
                   !gpio_get_dir( pin ) || gpio_get_out_level( pin ) == 0 ||
                   gpioState[ s_gpioPowerIdx ] != 1 ||
                   globalState.config.gpioDirection[ s_gpioPowerIdx ] != 0;
    if ( !stomped )
        return;
    globalState.config.gpioDirection[ s_gpioPowerIdx ] = 0;
    gpioState[ s_gpioPowerIdx ] = 1;
    gpio_set_function( pin, GPIO_FUNC_SIO );
    gpio_set_drive_strength( pin, GPIO_DRIVE_STRENGTH_12MA );
    gpio_set_dir( pin, true );
    gpio_put( pin, true );
}

static bool gpioDroopCurrentEstimate( float* current_mA, float* adc7V ) {
    if ( s_gpioPowerIdx < 0 )
        return false;
    // Source resistance for I = (V0 - v)/R: calibrated value or computed
    // from the actual routed feed path (see infraProbeDroopOhms).
    const float droopOhms = infraProbeDroopOhms( );
    float v = readAdcVoltage( 7, 16 );
    if ( adc7V )
        *adc7V = v;

    // Step re-anchor: a big jump between consecutive checks IS a load state
    // change (switch flip / button), and the higher-voltage side of the step
    // is by definition the unloaded level - anchor V0 there so the lower
    // reading becomes a true 0 mA. This is the only recovery from a
    // persisted V0 that's too HIGH for this board's actual rail (seen on
    // hardware: V0 3.350 vs real unloaded 3.297 = phantom 1.8 mA that sits
    // above the slow-track band forever and parks the detector in SELECT).
    static float s_lastDroopV = -1.0f;
    if ( s_lastDroopV >= 0.0f &&
         fabsf( v - s_lastDroopV ) * ( 1000.0f / droopOhms ) > 2.0f ) {
        float unloaded = ( v > s_lastDroopV ) ? v : s_lastDroopV;
        if ( unloaded > 3.6f )
            unloaded = 3.6f;
        if ( unloaded >= 3.0f ) {
            s_gpioDroopV0 = unloaded;
            s_gpioDroopValid = true;
            // V0 is RAM-only from here on (2026-08-16). It used to persist
            // back into calibration.probe_droop_v0 with a 20mV hysteresis,
            // and even that was a config-save source on the hot path (every
            // tap is a droop step). The seed comes from the calibration app /
            // self test, which are the only writers of probe_droop_v0 now.
        }
    }
    s_lastDroopV = v;

    if ( !s_gpioDroopValid || v > s_gpioDroopV0 ) {
        // New unloaded peak candidate (MEASURE, or a higher rail): zero
        // current is "the highest tip voltage we've seen". Confirm with a
        // median of 3 fresh reads - a single glitched-high sample would
        // latch an inflated V0 the slow track can never pull back down (the
        // resulting phantom current stays above the track band forever) -
        // and clamp to the plausible GPIO-high band.
        float m1 = readAdcVoltage( 7, 8 );
        float m2 = readAdcVoltage( 7, 8 );
        float lo = ( m1 < m2 ) ? m1 : m2;
        float hi = ( m1 < m2 ) ? m2 : m1;
        float peak = ( v < lo ) ? lo : ( ( v > hi ) ? hi : v ); // median of 3
        if ( peak > 3.6f )
            peak = 3.6f;
        if ( !s_gpioDroopValid || peak > s_gpioDroopV0 ) {
            s_gpioDroopV0 = peak;
            s_gpioDroopValid = true;
            // (No persistence: see the step re-anchor above.)
        }
    }
    float i = ( s_gpioDroopV0 - v ) * ( 1000.0f / droopOhms );
    if ( i < 0.0f )
        i = 0.0f;
    // Continuous re-zero while unloaded: pull V0 toward the live voltage so
    // a claim that started in SELECT (V0 seeded low) self-corrects the
    // moment MEASURE raises the tip, and so slow rail drift can't walk a
    // true MEASURE reading up into the SELECT threshold band. Skip the
    // track when loaded - that would collapse the SELECT signal into V0.
    if ( i < kGpioDroopUnloadedMax_mA ) {
        s_gpioDroopV0 += ( v - s_gpioDroopV0 ) * kGpioDroopV0Track;
        i = ( s_gpioDroopV0 - v ) * ( 1000.0f / droopOhms );
        if ( i < 0.0f )
            i = 0.0f;
    }
    *current_mA = i;
    return true;
}

static void printProbeSwitchStats( const char* src, float mA, float adc7V,
                                   int detA, int detB, int blinkPct,
                                   int shadow, bool agreeMode,
                                   int pos, bool changed ) {
    Serial.printf( "[switch] %-5s %6.2f mA", src, (double)mA );
    if ( strcmp( src, "droop" ) == 0 ) {
        Serial.printf( "  ADC7 %.3fV  V0 %.3fV", (double)adc7V, (double)s_gpioDroopV0 );
    }
    Serial.printf( "  thr lo/hi %.2f/%.2f",
                   (double)jumperlessConfig.calibration.probe_switch_threshold_low,
                   (double)jumperlessConfig.calibration.probe_switch_threshold_high );
    // A: tipSense H (pin held high = measure) / L (select) / - (skipped)
    // B: M / S / L(oaded) / - ; blink% for the GPIO feed
    // shadow: what the agreement classifier says (M/S/hold); "*" = it decides
    Serial.printf( "  A:%c B:%c", detA == 0 ? 'H' : ( detA == 1 ? 'L' : '-' ),
                   detB == 0 ? 'M' : ( detB == 1 ? 'S' : ( detB == 2 ? 'L' : '-' ) ) );
    if ( blinkPct >= 0 ) Serial.printf( "(%d%%)", blinkPct );
    Serial.printf( "  %s:%s", agreeMode ? "agree*" : "shadow",
                   shadow == 0 ? "M" : ( shadow == 1 ? "S" : "hold" ) );
    Serial.printf( "  -> %s%s\n\r",
                   pos == 1 ? "SELECT" : ( pos == 0 ? "MEASURE" : "UNKNOWN" ),
                   changed ? "  (CHANGED)" : "" );
    Serial.flush( );
}

// ---------------------------------------------------------------------------
// Detector A: tip-side digital sense.
//
// The probe's DPDT switch puts PROBE_PIN (GPIO10) on one of two nets:
//   MEASURE - the probe LED chain's supply, with its 100uF bulk cap C1
//   SELECT  - the needle (which drives the pad ladder when it touches one)
// So: set a soft drive, pull the pin LOW for ~2us (a 100uF cap barely
// notices; a floating needle is now at 0V), release the pin to a plain input
// - no pulls, RP2350-E9 says never trust a pull-down here - wait ~3us and
// read. HIGH means the cap held it: MEASURE. LOW means nothing did: SELECT
// (a needle on a ladder pad drains it too; only a needle on a driven-high
// row would read HIGH in SELECT, and that is a transient touch). Restore the
// pin to its always-on OUTPUT HIGH 8mA drive before returning. ~6us total,
// no ADC, no I2C, same core as readProbeRaw()/probeReadingIsPhantom() (which
// already blink PROBE_PIN for their own reasons).
// Skipped while a probe button is held / the button sampler is mid-read.
// ---------------------------------------------------------------------------
static int probeSwitchTipSense( void ) {
#if defined(OG_JUMPERLESS)
    return -1;
#else
    if ( checkingButton == 1 ) return -1;
    if ( ProbeButton::getInstance( ).getButtonState( ) != 0 ) return -1;
    gpio_disable_pulls( PROBE_PIN );
    gpio_set_drive_strength( PROBE_PIN, GPIO_DRIVE_STRENGTH_2MA );
    gpio_put( PROBE_PIN, false );
    delayMicroseconds( 2 );
    gpio_set_dir( PROBE_PIN, false );          // release: input, no pulls
    delayMicroseconds( 3 );
    int level = gpio_get( PROBE_PIN );
    gpio_put( PROBE_PIN, true );               // arm HIGH before re-enabling
    gpio_set_dir( PROBE_PIN, true );           // ...so the pad never re-drives LOW
    gpio_set_drive_strength( PROBE_PIN, GPIO_DRIVE_STRENGTH_8MA ); // main.cpp: OUTPUT_8MA
    return level ? 0 /* held high: MEASURE */ : 1 /* floating/drained: SELECT */;
#endif
}

// ---------------------------------------------------------------------------
// Detector B for a GPIO-sourced feed: feed-side blink.
//
// BUFFER_IN is fed from the claimed routable GPIO through ~170 ohm of
// crosspoints. Read ADC7 (buffer out = tip), drive the feed pin LOW for
// ~20us, read ADC7 again, restore. In MEASURE nothing else drives BUFFER_IN
// so the tip collapses toward 0; in SELECT the probe LED chain + C1 sit on
// BUFFER_IN through the cable and hold it at ~R_feed/(R_feed+R_cable) of the
// pre-blink level. The held percentage is compared with
// calibration.probe_switch_blink_hold_pct. Skipped while a touch is in
// progress (the tip is someone's signal), when the ADC is busy (never hold
// the tip dark waiting), or when the pre-read shows no feed at all.
// ---------------------------------------------------------------------------
static int probeSwitchFeedBlink( int* pctOut ) {
    if ( pctOut ) *pctOut = -1;
#if defined(OG_JUMPERLESS)
    return -1;
#else
    if ( s_gpioPowerIdx < 0 ) return -1;
    if ( lastReadRaw >= jumperlessConfig.calibration.minimum_probe_reading ) return -1;
    if ( !adcTryAcquire( ) ) return -1;
    int pin = gpioDef[ s_gpioPowerIdx ][ 0 ];
    // ADC7 sits behind the +/-8V scale/offset network (U7B): 0 V on the tip
    // reads ~mid-scale, not 0 counts. Work in VOLTS (same calibration as
    // readAdcVoltage) or a fully collapsed tip looks "73% held" - which is
    // exactly the mistake the first build of this detector made.
    const float toVolts = adcSpread[ 7 ] / 4095.0f;
    float preV = (float)readAdcHeld( 7, 2 ) * toVolts - adcZero[ 7 ];
    float midV = 0.0f;
    if ( preV >= 0.5f ) {   // below this the feed is dark, not a position
        gpio_put( pin, false );
        delayMicroseconds( 20 );   // hardware: the collapse is complete well inside 20us
        midV = (float)readAdcHeld( 7, 2 ) * toVolts - adcZero[ 7 ];
        gpio_put( pin, true );
    }
    adcRelease( );
    if ( preV < 0.5f ) return -1;
    if ( midV < 0.0f ) midV = 0.0f;
    int pct = (int)( midV * 100.0f / preV );
    if ( pctOut ) *pctOut = pct;
    // held (the LED supply cap on BUFFER_IN) = SELECT; collapsed = MEASURE.
    // Hardware (2026-08-16, GPIO_8 feed): SELECT holds at ~99%, MEASURE
    // collapses to ~0% - the 50% default sits in the middle of a wide gap.
    return ( pct >= jumperlessConfig.calibration.probe_switch_blink_hold_pct ) ? 1 : 0;
#endif
}

// ---------------------------------------------------------------------------
// Fast position tracking for probeMode. probeMode() is a blocking loop that
// services CRITICAL work only, and ProbeSwitch is NORMAL, so the full
// classifier never runs while a probe session is open (up to 80 s): flip
// the switch mid-session and nothing notices until it exits. Detector A
// alone (~6us, no ADC, no I2C) every 250 ms; two agreeing reads flip the
// position and its LED pattern. Only under debug.probe_switch_agree - A is
// unverified in SELECT until the hands-on matrix passes, so it must not
// steer sessions while the legacy classifier is in charge.
// ---------------------------------------------------------------------------
void Probing::checkSwitchPositionFast( void ) {
    if ( !jumperlessConfig.debug.probe_switch_agree ) return;
    if ( jumperlessConfig.dacs.auto_connect_probe <= 0 ) return;
    static unsigned long s_lastMs = 0;
    static int s_pending = -1;
    static int s_pendingCount = 0;
    unsigned long now = millis( );
    if ( now - s_lastMs < 250 ) return;
    s_lastMs = now;
    int a = probeSwitchTipSense( );
    if ( a < 0 ) return;
    if ( a == switchPosition ) {
        s_pending = -1;
        s_pendingCount = 0;
        return;
    }
    if ( s_pending == a ) {
        s_pendingCount++;
    } else {
        s_pending = a;
        s_pendingCount = 1;
    }
    if ( s_pendingCount >= 2 ) {
        switchPosition = a;
        s_pending = -1;
        s_pendingCount = 0;
        showProbeLEDs = ( a == 0 ) ? 3 : 4;
        if ( jumperlessConfig.debug.probe_switch_stats ) {
            Serial.printf( "[switch] fast A -> %s  (CHANGED, inside probeMode)\n\r",
                           a == 0 ? "MEASURE" : "SELECT" );
        }
    }
}

// (scrubStaleGpioBufferBridges is gone: infra bridges are never serialized
// into slots anymore, and stale claims in OLD slot files are dropped by
// infraScrubLoadedBridges() in the state load sanitizer.)

// Thin wrapper over the InfraPaths probe_power function. Kept because ~15
// call sites (boot, config appliers, self test, serial commands, probe
// entry) speak this signature. The candidate machinery (DAC0 -> GPIO8..1 ->
// DAC0, user-claim detection, bridge add/remove) all lives in
// routing/InfraPaths.cpp and runs at the head of every netlist rebuild.
void Probing::routableBufferPower( int offOn, int flash, int force ) {
#if defined(OG_JUMPERLESS)
    (void)offOn; (void)flash; (void)force;
    return;
#endif
    bool wantOn = ( offOn == 1 );
    bool changed = ( infraProbePowerWanted( ) != wantOn );
    infraSetProbePowerEnabled( wantOn );
    // Re-evaluate when the desired state changed, when the caller forces it,
    // or when power should be on but no source is active yet (e.g. every
    // candidate was claimed last time - resources may have freed up).
    // infraNudge() refreshes only outside an in-flight rebuild and leaves a
    // sticky retry flag otherwise, so this is safe from any core-0 context.
    (void)flash; // flash-vs-local no longer matters: state is the backing store
    if ( changed || force == 1 || ( wantOn && infraProbePowerSource( ) < 0 ) ) {
        infraNudge( );
    }
}

int probeADCmap[ 102 ];

int nothingTouchedReading = 15;
int mapFrom = 15;
// int calibrateProbe() {
//   /* clang-format off */

//   int probeRowMap[102] = {

//       0,	      1,	      2,	      3,	      4,
//       5,	      6,	      7,	      8, 9,	      10,
//       11,	      12,	      13,	      14,	      15,
//       16,	      17, 18,	      19,	      20,	      21,
//       22,	      23,	      24,	      25,	      26, 27,
//       28,	      29,	      30,	      TOP_RAIL, TOP_RAIL_GND,
//       BOTTOM_RAIL,	      BOTTOM_RAIL_GND, 31,	      32, 33, 34,
//       35,	      36,	      37,	      38,	      39, 40,
//       41,	      42,	      43,	      44,	      45,
//       46,	      47,	      48, 49,	      50,	      51,
//       52,	      53,	      54,	      55,	      56,
//       57, 58,	      59,	      60,	      NANO_D1, NANO_D0,
//       NANO_RESET_1,	      NANO_GND_1,	      NANO_D2,	      NANO_D3,
//       NANO_D4,	      NANO_D5,	      NANO_D6,	      NANO_D7, NANO_D8,
//       NANO_D9,	      NANO_D10,	      NANO_D11,	      NANO_D12,
//       NANO_D13,	      NANO_3V3,	      NANO_AREF,	      NANO_A0,
//       NANO_A1,	      NANO_A2,	      NANO_A3,	      NANO_A4, NANO_A5,
//       NANO_A6,	      NANO_A7,	      NANO_5V,	      NANO_RESET_0,
//       NANO_GND_0,	      NANO_VIN, LOGO_PAD_BOTTOM, LOGO_PAD_TOP,
//       GPIO_PAD, DAC_PAD,	      ADC_PAD,	      BUILDING_PAD_TOP,
//       BUILDING_PAD_BOTTOM,
//   };
//   /* clang-format on */

int nothingTouchedSamples[ 16 ] = { 0 };

int Probing::getNothingTouched( int samples ) {

    startProbe( );
    int rejects = 0;
    int loops = 0;

    for ( int i = 0; i < 16; i++ ) {

        nothingTouchedSamples[ i ] = 0;
    }
    do {

        // samples = 2;

        int sampleAverage = 0;
        rejects = 0;
        nothingTouchedReading = 0;
        for ( int i = 0; i < samples; i++ ) {
            // int reading = readProbeRaw(1);
            int readNoth = readAdc( 5, 8 );
            nothingTouchedSamples[ i ] = readNoth;
            //   delayMicroseconds(50);
            //   Serial.print("nothingTouchedSample ");
            //   Serial.print(i);
            //   Serial.print(": ");
            // Serial.println(readNoth);
        }
        loops++;

        for ( int i = 0; i < samples; i++ ) {

            if ( nothingTouchedSamples[ i ] < 100 ) {
                sampleAverage += nothingTouchedSamples[ i ];
            } else {
                rejects++;
            }
        }
        if ( samples - rejects <= 1 ) {
            Serial.println( "All nothing touched samples rejected, check sense pad "
                            "connections\n\r" );
            nothingTouchedReading = 36;
            return 0;
            break;
        }
        sampleAverage = sampleAverage / ( samples - rejects );
        rejects = 0;

        for ( int i = 0; i < samples; i++ ) {
            if ( abs( nothingTouchedSamples[ i ] - sampleAverage ) < 15 ) {
                nothingTouchedReading += nothingTouchedSamples[ i ];
                //       Serial.print("nothingTouchedSample ");
                //   Serial.print(i);
                //   Serial.print(": ");
                // Serial.println(nothingTouchedSamples[i]);
            } else {
                rejects++;
            }
        }

        nothingTouchedReading = nothingTouchedReading / ( samples - rejects );
        mapFrom = nothingTouchedReading;
        // Serial.print("mapFrom: ");
        // Serial.println(mapFrom);
        jumperlessConfig.calibration.probe_min = mapFrom;

        if ( loops > 5 ) {
            break;
        }

    } while ( ( nothingTouchedReading > 80 || rejects > samples / 2 ) && loops < 4 );
    //  Serial.print("nothingTouchedReading: ");
    //  Serial.println(nothingTouchedReading);
    return nothingTouchedReading;
}

// Pad-decode endpoints for the CURRENT switch position. The base
// probe_min/probe_max pair is calibrated with the SELECT feed (PROBE_PIN at
// ~3.3V) driving the tip. In MEASURE position the routable buffer drives the
// tip instead - at measure_mode_output_voltage from the DAC, or the drooped
// GPIO-high level when the feed fell through to a GPIO - and the pad ladder into the
// 10K sense divider is purely resistive, so every reading scales linearly
// with the tip voltage. The probe_min_measure/probe_max_measure pair holds
// the measure endpoints in the 3.3V frame (seeded from the base pair at
// load, individually adjustable); the decode multiplies them by the live
// tip voltage measured on ADC7 (hardwired to the buffer output = the tip).
// Non-static: probeCalibApp uses it so the calibration display decodes rows
// exactly the way the runtime does.
void probeMapRange( int* mapMin, int* mapMax ) {
#if defined(OG_JUMPERLESS)
    // OG has no measure buffer and no ADC7 (RP2040: ADC0-3 only), and its
    // checkProbeCurrentRaw() stub returns 0mA which parks switchPosition at
    // 0 - so without this guard the measure path below would read a
    // nonexistent ADC channel. OG always decodes with the base pair.
    *mapMin = jumperlessConfig.calibration.probe_min;
    *mapMax = jumperlessConfig.calibration.probe_max;
    return;
#endif
    static float cachedScale = 1.0f;
    static unsigned long lastScaleRead = 0;
    static bool inMeasure = false;
    if ( switchPosition != 0 ) { // select (or unknown): the calibrated frame
        inMeasure = false;
        *mapMin = jumperlessConfig.calibration.probe_min;
        *mapMax = jumperlessConfig.calibration.probe_max;
        return;
    }
    if ( !inMeasure ) {
        // Just flipped into measure: the tip net's loading changed with the
        // switch, so a scale cached in the other position is stale by more
        // than touch droop - force a fresh ADC7 read.
        inMeasure = true;
        lastScaleRead = 0;
    }
    int mMin = jumperlessConfig.calibration.probe_min_measure;
    int mMax = jumperlessConfig.calibration.probe_max_measure;
    if ( mMin <= 0 )
        mMin = jumperlessConfig.calibration.probe_min;
    if ( mMax <= 0 )
        mMax = jumperlessConfig.calibration.probe_max;
    // The scale must be read (nearly) simultaneously with the pad decode:
    // the decode is ratiometric (pad reading = tip voltage x ladder ratio,
    // scale = tip voltage / 3.3), so the tip voltage cancels EXACTLY - but
    // only if both reads see the same tip voltage. Hardware-measured: every
    // connect/disconnect rebuild re-applies the crosspoints and wanders the
    // tip ~70mV (more than a full row step at the ladder top) for a while
    // after, on ANY feed - DAC0 and GPIO measured identically, and LED-only
    // activity doesn't move it. A scale up to 25ms stale decoupled the two
    // reads across that wander and smeared taps onto adjacent rows. 2ms
    // still caches the several probeMapRange() calls within one sub-ms
    // decode pass, but re-ratios fast enough that feed wander cancels.
    if ( lastScaleRead == 0 || millis( ) - lastScaleRead > 2 ) {
        lastScaleRead = millis( );
        // 16 samples: the scale multiplies BOTH decode endpoints, so ADC7
        // noise moves every row boundary at once - worth the extra ~64us.
        float vTip = readAdcVoltage( 7, 16 );
        if ( vTip > 0.5f && vTip < 5.5f ) {
            cachedScale = vTip / 3.3f; // the pair's stored frame is 3.3V
        }
    }
    *mapMin = (int)( (float)mMin * cachedScale + 0.5f );
    *mapMax = (int)( (float)mMax * cachedScale + 0.5f );
}

unsigned long doubleTimeout = 0;

unsigned long padTimeout = 0;
int padTimeoutLength = 50;

int state = 0;
int lastPadTouched = 0;
unsigned long padNoTouch = 0;
int lastPadTouchedTime = 0;
int samePadCount = 0;

void Probing::checkPads( void ) {
    // startProbe();
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

    int mapMin, mapMax;
    probeMapRange( &mapMin, &mapMax );

    // lastReadRaw = -1;
    if ( probeReading <= mapMin ) {
        checkingPads = 0;
        return;
    }

    padNoTouch = 0;

    // probeReading = probeRowMap[map(probeReading, 30, 4050, 101, 0)];
    probeReading = probeRowMap[ map( probeReading, mapMin, mapMax, 101, 0 ) ];
    // stopProbe();

    if ( probeReading != lastPadTouched ) {
        lastPadTouchedTime = millis( );
        samePadCount = 0;
    } else {
        samePadCount++;
    }
    lastPadTouched = probeReading;
    // Serial.print("probeReading: ");
    // Serial.println(probeReading);
    // Serial.flush();
    // if (probeReading < LOGO_PAD_TOP || probeReading > BUILDING_PAD_BOTTOM) {
    //   padTimeout = millis();
    //   lastReadRaw = 0;
    //   checkingPads = 0;
    //   return;
    // }

    padTimeout = millis( );
    // Serial.print("probeReading: ");
    // Serial.println(probeReading);
    int foundGpio = 0;
    int foundAdc = 0;
    int foundDac = 0;
    // inPadMenu = 1;

    // (No line-clear here any more: every print this was clearing for - the
    // "Top guy" / "ADC pad" pad names, and the changedNetColors dumps behind
    // `&& false` - is commented out or dead, so all it did was wipe whatever
    // the user was typing, ~20x/sec for as long as a pad was touched.)

    switch ( probeReading ) {
    case LOGO_PAD_TOP:
        // Serial.print( "Top guy" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( LOGO_TOP, -2 );
        break;
    case LOGO_PAD_BOTTOM:
        // Serial.print( "Bottom guy" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( LOGO_BOTTOM, -2 );
        break;
    case ADC_PAD:
        // Serial.print( "ADC pad" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( ADC_0, -2 );
        setLogoOverride( ADC_1, -2 );
        break;
    case DAC_PAD:
        // Serial.print( "DAC pad" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( DAC_0, -2 );
        setLogoOverride( DAC_1, -2 );
        break;
    case GPIO_PAD:
        // Serial.print( "GPIO pad" );
        clearColorOverrides( 1, 1, 0 );
        setLogoOverride( GPIO_0, -2 );
        setLogoOverride( GPIO_1, -2 );
        break;
    case BUILDING_PAD_TOP:
        // Serial.print( "Building top" );
        clearColorOverrides( 1, 1, 0 ); //! highlighted net
        if ( brightenedNet != -1 && false) {
            hsvColor hsv = RgbToHsv( netColors[ brightenedNet ] );
            changedNetColors[ brightenedNet ].color = colorPicker( hsv.h, jumperlessConfig.display.led_brightness );
            changedNetColors[ brightenedNet ].node1 = brightenedNode;
            changedNetColors[ brightenedNet ].fromBridge = false; // User manually set this color
            netColors[ brightenedNet ] = unpackRgb( changedNetColors[ brightenedNet ].color );
            Serial.print( "changedNetColors[" );
            Serial.print( brightenedNet );
            Serial.print( "]: " );
            Serial.printf( "%06x\n", changedNetColors[ brightenedNet ].color );
            clearHighlighting( );
            checkChangedNetColors( -1 );
            // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
            showLEDsCore2 = -1; // Trigger LED update on core 2
            // saveChangedNetColorsToFile( netSlot, 0 ); // DEPRECATED: Colors now saved via YAML state

        } else {
            // colorPicker(45, jumperlessConfig.display.led_brightness);
        }
        break;
    case BUILDING_PAD_BOTTOM:
        // Serial.print( "Building bottom" );
        clearColorOverrides( 1, 1, 0 );
        if ( brightenedNet != -1 && false) {
            hsvColor hsv = RgbToHsv( netColors[ brightenedNet ] );
            changedNetColors[ brightenedNet ].color = colorPicker( hsv.h, jumperlessConfig.display.led_brightness );
            changedNetColors[ brightenedNet ].node1 = brightenedNode;
            changedNetColors[ brightenedNet ].fromBridge = false; // User manually set this color
            netColors[ brightenedNet ] = unpackRgb( changedNetColors[ brightenedNet ].color );
            Serial.print( "changedNetColors[" );
            Serial.print( brightenedNet );
            Serial.print( "]: " );
            Serial.printf( "%06x\n", changedNetColors[ brightenedNet ].color );
            clearHighlighting( );
            checkChangedNetColors( -1 );
            // Note: No need to call assignNetColors() here - core 2's showNets() recomputes colors every frame
            showLEDsCore2 = -1; // Trigger LED update on core 2
            // saveChangedNetColorsToFile( netSlot, 0 ); // DEPRECATED: Colors now saved via YAML state

        } else {
            // colorPicker(225, jumperlessConfig.display.led_brightness);
        }
        break;
    default:
        // clearColorOverrides(1, 1, 0);
        break;
    }

    // Serial.print("\t");
    // Serial.print(samePadCount);
    // Serial.print("\t");
    // Serial.print(millis() - lastPadTouchedTime);
    Serial.flush( );

    // Serial.println();

    checkingPads = 0;
}

float longRunningAverage = 0.0;
int longRunningAverageDamping = 10;

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

// Phantom-touch gate: the pad ladder is only ever energized by the tip feed
// (PROBE_PIN in select; in measure, the GPIO powering the buffer when the
// feed fell through to one). Blink that feed off for one short burst before
// accepting a changed reading - a real probe contact collapses to the dark
// floor, while a reading injected from anywhere else (a finger bridging a
// powered row onto a pad, body coupling from holding the board) persists,
// and gets rejected. ~115us per blink.
static bool probeReadingIsPhantom( int average, int mapMin ) {
    int pin = -1;
#if defined(OG_JUMPERLESS)
    pin = PROBE_PIN; // OG's tip feed is always PROBE_PIN (switchPosition parks at 0)
#else
    if ( switchPosition != 0 ) {
        pin = PROBE_PIN; // select: tip fed directly from PROBE_PIN
    } else if ( s_gpioPowerIdx >= 0 ) {
        pin = gpioDef[ s_gpioPowerIdx ][ 0 ]; // measure: GPIO-powered buffer
    }
#endif
    if ( pin < 0 ) {
        return false; // DAC-powered measure buffer: nothing we can blink
    }

    // ponytail: a steady contact re-blinks at most every 250ms (a >5 count
    // move re-checks immediately), so the tip feed isn't chopped on every
    // accepted read in the allowDuplicates flows. Ceiling: a phantom landing
    // within 5 counts of a just-verified reading rides the stale verdict for
    // up to 250ms. Upgrade path: invalidate the cache on no-contact gaps.
    static int lastCheckedValue = -1000;
    static unsigned long lastCheckedMs = 0;
    if ( millis( ) - lastCheckedMs < 250 && abs( average - lastCheckedValue ) <= 5 ) {
        return false;
    }

    gpio_put( pin, false );
    // 60us: the top of the pad ladder (rows ~45-60) is the far,
    // highest-resistance leg, so its sense node discharges much slower than
    // the "~1us" low rows. 25us left those rows only partway collapsed and
    // the tight floor below false-rejected them ("slow to register").
    delayMicroseconds( 60 );
    int dark = readAdc( 5, 8 );
    gpio_put( pin, true );
    delayMicroseconds( 30 );

    // A real contact collapses the pad toward the dark floor once its only
    // feed (the tip) is off; a phantom source (finger bridging a powered row,
    // body coupling) holds the pad near the lit value. Reject only when the
    // pad stays HIGH relative to this reading, not against a tight absolute
    // floor - so a slow-discharging high row that only fell partway still
    // passes. Threshold: 60% of the lit span, never below the old +10 floor.
    int darkFloor = mapMin + 10;
    int fracThresh = mapMin + ( ( average - mapMin ) * 6 ) / 10;
    int rejectAbove = ( fracThresh > darkFloor ) ? fracThresh : darkFloor;
    if ( dark > rejectAbove ) {
        lastCheckedMs = 0; // never cache a rejection
        if ( debugProbing == 1 ) {
            Serial.print( "phantom reading rejected: dark " );
            Serial.print( dark );
            Serial.print( " > " );
            Serial.print( rejectAbove );
            Serial.print( ", lit " );
            Serial.println( average );
        }
        return true;
    }
    lastCheckedValue = average;
    lastCheckedMs = millis( );
    return false;
}

int Probing::readProbeRaw( int readNothingTouched, bool allowDuplicates ) {
    // nothingTouchedReading = 165;
    // lastReadRaw = 0;

    // Keep the GPIO-powered measure tip alive on the hot path. checkSwitchPosition()
    // is the only other re-driver and it's gated to 500ms (and skipped during LED
    // settle / button checks), so a pin stomped low by a refresh/PWM/MicroPython
    // writer stayed dead - and every read here returned the dark floor - until a
    // button press forced a switch re-check. Re-asserting per read (self-guards to
    // a cheap register check when no GPIO is claimed) keeps the feed always on.
    reassertGpioBufferDrive( );

    int numberOfReads = 8;
    int lowReads = 0;
    int mapMin, mapMax;
    probeMapRange( &mapMin, &mapMax );

    int measurements[ 16 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // digitalWrite(PROBE_PIN, HIGH);

    // Serial.print("ConnectOrClearProbe: ");
    // Serial.println(connectOrClearProbe);
    // Serial.print("CheckingPads: ");
    // Serial.println(checkingPads);
    
    if ( connectOrClearProbe == 1 ) {

        for ( int i = 0; i < numberOfReads; i++ ) {
            measurements[ i ] = readAdc( 5, 16 );

            if ( measurements[ i ] < 300 && measurements[ i ] > ( mapMin + 10 ) && i < 4 ) {
                lowReads++;
            }
            if ( lowReads > 2 ) {
                 numberOfReads = 16;
            }
            delayMicroseconds( 5 );
        }
        //  Serial.print("connect: ");
    } else if ( checkingPads == 1 ) {
        for ( int i = 0; i < numberOfReads; i++ ) {
            measurements[ i ] = readAdc( 5, 16 );
            if ( measurements[ i ] < 300 && measurements[ i ] > ( mapMin + 10 ) && i < 4 ) {
                lowReads++;
            }
            if ( lowReads > 2 ) {
                 numberOfReads = 16;
            }
            delayMicroseconds( 5 );
        }
        // Serial.print("Pads: ");

    } else {
        // Measure position gets deeper bursts: its tip feed comes through
        // the crossbar (droopier, noisier than select's direct PROBE_PIN
        // feed), and this row-highlight path is measure mode's main
        // consumer. Select keeps a shorter burst but still more than before.
        int burstSamples = ( switchPosition == 0 ) ? 24 : 16;
        for ( int i = 0; i < numberOfReads; i++ ) {
            measurements[ i ] = readAdc( 5, burstSamples );
            if ( measurements[ i ] < 300 && measurements[ i ] > ( mapMin + 10 ) && i < 4 ) {
                lowReads++;
            }
            if ( lowReads > 2 ) {
                 numberOfReads = 16;
            }
            delayMicroseconds( 5 );
        }
        // Serial.print("s: ");
    }

    int maxVariance = 0;
    int variance = 0;

    // Serial.print("\r                                                                                    \rMeasurements: ");
    // for ( int i = 0; i < numberOfReads; i++ ) {
    //     Serial.print(measurements[ i ]);
    //     Serial.print(" ");
    // }
    // Serial.println();
    for ( int i = 0; i < numberOfReads - 1; i++ ) {
        variance = abs( measurements[ i ] - measurements[ i + 1 ] );
        if ( variance > maxVariance ) {
            maxVariance = variance;
        }
    }
    int average = medianProbeBursts( measurements, numberOfReads );
    // Serial.print("average ");
    // Serial.println(average);

#if USB_AUDIO_ENABLE
    // Tip is on a pad: tell the USB audio capture the probe is in use so it
    // yields the ADC to us (it resumes ~300 ms after the tip lifts). This is
    // the ONE place every probe path - idle service, pad check, probe mode,
    // measure mode - decodes a touch, which is why the hook lives here and not
    // in the callers.
    if ( average >= jumperlessConfig.calibration.minimum_probe_reading ) {
        usb_audio_probe_activity( );
    }
#endif

    // CONNECT-mode one-touch-one-node latch (measure position), part 1:
    // the RE-ARM. It must live OUT here because a released probe reads the
    // no-touch floor (~23 raw), which fails the accept branch's
    // minimum_probe_reading gate below and would never reach a re-arm
    // placed inside it (field-tested: the latch accepted the first tap and
    // then nothing, ever). Any burst at the no-touch level - or any read
    // outside measure position - re-arms.
    static bool measureTouchLatched = false;
    if ( switchPosition != 0 ||
         average < jumperlessConfig.calibration.minimum_probe_reading ) {
        measureTouchLatched = false;
    }

    int rowProbed = -1;
    // if (average < 90 && abs(average - nothingTouchedReading) > 10) {
    // Serial.print("var: ");
    // Serial.print(maxVariance);
    // Serial.print(" average: ");
    // Serial.println(average);
    // }

    // if (allowDuplicates){
    //   Serial.print("allowDuplicates: ");
    //   Serial.println(average);
    //   return average;
    //   //lastReadRaw = 4096;
    // }

    if ( maxVariance <= 6 && ( ( abs( average - lastReadRaw ) > 5 ) || checkingPads == 1 ) && ( average >= jumperlessConfig.calibration.minimum_probe_reading ) ) {

        // MEASURE position: contact engagement slews the pad reading over
        // several ms (the tip feeds the ladder through the crossbar - ~150
        // ohm + net capacitance - vs select's direct PROBE_PIN drive), so a
        // single sub-ms burst looks flat mid-slew and passes the variance
        // gate while decoding as a NEIGHBORING row (hardware-observed: taps
        // reading/connecting adjacent rows). Require two consecutive bursts
        // to agree before accepting a NEW reading; a real touch settles
        // within a burst or two, so this only adds ~1ms of latency.
        if ( switchPosition == 0 ) {
            static int prevBurstAvg = -1000;
            int prev = prevBurstAvg;
            prevBurstAvg = average;
            if ( abs( average - prev ) > 6 ) {
                return -1; // mid-slew: accept on the next agreeing burst
            }

            // CONNECT-mode one-touch-one-node latch, part 2 (re-arm is up
            // by the average computation): a HELD contact wanders with the
            // post-rebuild tip drift (~70mV measured), and once it moves >5
            // counts from the accepted value it re-enters this accept path
            // as a "new" reading - decoding as the NEIGHBORING row and
            // chaining the connection onto it (hardware-observed: taps in
            // connect mode with the switch in measure connect adjacent
            // rows). Latch on accept; only a burst back at the no-touch
            // floor (a real lift) re-arms. Highlight/measure flows are
            // untouched - they want continuous reads while dragging - and
            // select position never latches.
            if ( connectOrClearProbe == 1 ) {
                if ( measureTouchLatched ) {
                    return -1; // same touch drifting - not a new tap
                }
                measureTouchLatched = true; // consumes the release
            }
        }

        if ( probeReadingIsPhantom( average, mapMin ) ) {
            return -1;
        }
        // if (checkingPads != 1) {
        lastReadRaw = average;
        // }
        //      Serial.println("  ");

        // Serial.print("average: ");
        // Serial.print(average);
        // Serial.print(" numberOfReads: ");
        // Serial.println(numberOfReads);
        // Serial.flush();
        return average;

    } else {

        if ( allowDuplicates && ( average >= jumperlessConfig.calibration.minimum_probe_reading ) &&
             maxVariance <= 6 &&
             abs( average - lastReadRaw ) <= 10 ) {
            if ( probeReadingIsPhantom( average, mapMin ) ) {
                return -1;
            }
            lastReadRaw = average;
            return average;
        }

        if ( ( abs( average - lastReadRaw ) < 2 ) && allowDuplicates && ( average >= jumperlessConfig.calibration.minimum_probe_reading ) ) {
            if ( probeReadingIsPhantom( average, mapMin ) ) {
                return -1;
            }
            // Serial.print("allowDuplicates: ");
            // Serial.println(average);
            return average;
        }

        // Serial.print("nothingTouchedReading: ");
        // Serial.println(nothingTouchedReading);
        // longRunningAverageCount = 10;
        // longRunningAverage += ((float)average - longRunningAverage) / longRunningAverageCount;
        // Serial.print("longRunningAverage: ");
        // Serial.println(longRunningAverage);

        // Serial.print("average ");
        // Serial.println(average);
        return -1;
    }
}

int convertPadsToRows( int pad ) {
    int row = pad;
    if ( pad == LOGO_PAD_BOTTOM ) {
        row = RP_UART_TX;
    } else if ( pad == LOGO_PAD_TOP ) {
        row = RP_UART_RX;
    } else if ( pad == GPIO_PAD ) {
        row = GPIO_PAD;
    } else if ( pad == DAC_PAD ) {
        row = DAC_PAD;
    } else if ( pad == ADC_PAD ) {
        row = ADC_PAD;
    } else if ( pad == BUILDING_PAD_TOP ) {
        row = ISENSE_PLUS;
    } else if ( pad == BUILDING_PAD_BOTTOM ) {
        row = ISENSE_MINUS;
    }
    return row;
}
unsigned long lastProbeTime = millis( );
int lastProbeRead = 0;
int lastRowProbed = -1;

unsigned long lastDuplicateTime = millis( );
int lastDuplicateRead = 0;

int Probing::smoothProbeReading( int probeRead, bool reset ) {
    if ( reset ) {
        smoothedProbeRead = -1;
    }

    if ( probeRead <= 0 ) {
        return probeRead;
    }

    // Damp ADC jitter WITHOUT chasing a contact-settle ramp. The old
    // threshold (half a row step, ~20 counts) let the 3:1 blend follow a
    // finger touch settling in - the reading ramps 10-20 counts per cycle as
    // contact resistance drops, so the smoothed value crawled through the
    // NEIGHBORING row's band for ~250ms before landing on the touched row.
    // readProbeRaw already median-filters 8-16 ADC bursts; blend small steps
    // only and snap to anything larger.
    const int resetThreshold = 8;

    if ( smoothedProbeRead < 0 || abs( probeRead - smoothedProbeRead ) > resetThreshold ) {
        smoothedProbeRead = probeRead;
    } else {
        smoothedProbeRead = ( ( smoothedProbeRead * 5 ) + probeRead + 3 ) / 6;
    }

    return smoothedProbeRead;
}

int Probing::justReadProbe( bool allowDuplicates, int rawPad ) {

    // Call sequence number: lets the row-change confirmation below require
    // STRICTLY consecutive reads (any intervening no-read/blocked call
    // advances the sequence and voids a pending row change).
    static uint32_t callSeq = 0;
    callSeq++;

    // Check if probing is blocked and if the block timer has expired
    if ( blockProbing > 0 && ( millis( ) - blockProbingTimer < blockProbing ) ) {
        return -1; // Still blocked
    }
    // Block expired, clear it
    if ( blockProbing > 0 ) {
        blockProbing = 0;
    }

    int probeRead = readProbeRaw( 0, allowDuplicates );

    if ( probeRead <= 0 ) {

        return -1;
    }
    //   Serial.print("probeRead: ");
    // Serial.println(probeRead);

    // int rowProbed = map(probeRead, mapFrom, 4045, 101, 0);
    int stableProbeRead = smoothProbeReading( probeRead );
    int mapMin, mapMax;
    probeMapRange( &mapMin, &mapMax );
    int rowProbed = map( stableProbeRead, mapMin, mapMax, 101, 0 );
    // Serial.print("rowProbed: ");
    // Serial.println(rowProbed);

    if ( rowProbed <= 0 || rowProbed > sizeof( probeRowMap ) ) {
        if ( debugProbing == 1 ) {
            Serial.print( "out of bounds of probeRowMap[" );
            Serial.println( rowProbed );
        }
        return -1;
    }

    if ( allowDuplicates ) {
        // Check if the mapped row is the same, not just the raw reading
        if ( probeRowMapByPad[ rowProbed ] == lastRowProbed ) {
            // If the timer hasn't elapsed, reject the duplicate reading
            if ( millis( ) - lastDuplicateTime < 500 ) {
                if ( debugProbing == 1 ) {
                    // Once per second, not per read: a probe HELD on a row
                    // rejects ~100 duplicates/sec and flooded the terminal.
                    static unsigned long lastDupPrintMs = 0;
                    if ( millis( ) - lastDupPrintMs > 1000 ) {
                        lastDupPrintMs = millis( );
                        Serial.print( "Rejected duplicate row: " );
                        Serial.println( probeRowMap[ rowProbed ] );
                    }
                }
                return -1;
            } else {
                // Timer has elapsed, accept the reading and reset timer
                lastDuplicateTime = millis( );
                lastProbeRead = probeRead;
                // Return the same reading now that timer is complete
                if ( rawPad == 1 ) {
                    return probeRowMapByPad[ rowProbed ];
                } else {
                    return probeRowMap[ rowProbed ];
                }
            }
        } else {
            // A row DIFFERENT from the current one must be seen on two
            // CONSECUTIVE reads before it's reported. This branch used to
            // accept instantly while duplicates of the true row are
            // rate-limited to one per 500ms - so a single noisy decode into
            // a neighboring pad's band was preferentially reported, and
            // MeasureMode/Highlighting latched it from the 10ms service
            // cache. One confirming read at the 100Hz service cadence adds
            // ~10ms to a genuine row change.
            static int pendingNewRow = -1;
            static uint32_t pendingNewRowSeq = 0;
            if ( probeRowMapByPad[ rowProbed ] != pendingNewRow ||
                 callSeq != pendingNewRowSeq + 1 ) {
                pendingNewRow = probeRowMapByPad[ rowProbed ];
                pendingNewRowSeq = callSeq;
                return -1;
            }
            pendingNewRow = -1;
            // Confirmed: reset timer and update last values
            lastDuplicateTime = millis( );
            lastProbeRead = probeRead;
            lastRowProbed = probeRowMapByPad[ rowProbed ];
            if ( rawPad == 1 ) {
                return probeRowMapByPad[ rowProbed ];
            } else {
                return probeRowMap[ rowProbed ];
            }
        }
    }

    // For non-allowDuplicates case
    lastProbeRead = probeRead;
    lastRowProbed = probeRowMapByPad[ rowProbed ];
    if ( rawPad == 1 ) {
        return probeRowMapByPad[ rowProbed ];
    } else {
        return probeRowMap[ rowProbed ];
    }
}
/// @brief returns the row probed plus checks for button presses, or -1 if nothing
/// @return -16 connect, -18 remove, -19 encoder up, -17 encoder down, -10 encoder pressed
int Probing::readProbe( ) {
    int found = -1;
    // connectedRows[0] = -1;
    unsigned long buttonCheck = 0;
    // if (checkProbeButton() == 1) {
    //   return -18;
    // }
    // Check if probing is blocked and if the block timer has expired
    if ( blockProbing > 0 && ( millis( ) - blockProbingTimer < blockProbing ) ) {
        // Serial.println("Probing blocked");
        // Serial.flush();
        return -1; // Still blocked
    }
    // Block expired, clear it
    if ( blockProbing > 0 ) {
        blockProbing = 0;
    }

    int probeRead = -1; // readProbeRaw();
    // delay(100);
    // Serial.println(probeRead);
    // Serial.println(debugLEDs);
    while ( probeRead <= 0 ) {
        /// delay(50);
        // return -1;
        // Serial.println(debugLEDs);

        probeRead = readProbeRaw( );
        // rotaryEncoderStuff();

        // Return encoder state for handling in main probe loop
        if ( encoderDirectionState != NONE ) {
            if ( encoderDirectionState == UP ) {
                // Serial.println("encoder up");
                // Serial.flush();
                return -19;
            } else if ( encoderDirectionState == DOWN ) {
                // Serial.println("encoder down");
                // Serial.flush();
                return -17;
            }
        } else if ( encoderButtonState != IDLE ) {
            // Serial.println("encoder pressed");
            // Serial.flush();
            return -10;
        }


        // CRITICAL: actually service the button here. checkProbeButton()
        // only reads the cached buttonPress that ProbeButton::service
        // posts - it doesn't itself sample the hardware. probeMode's
        // outer loop only calls jOS.serviceCritical() once per iteration,
        // and the iteration time inflates to 20-50ms whenever an OLED
        // write or LED batch fires. Without this call, this inner loop
        // (up to 8 ms of ADC reads) is a sampling blackout window: any
        // press/release transitioning entirely inside it is lost, which
        // is the root cause of the "double-tap less reliable in probe
        // mode" symptom. service() has its own 4 ms rate limit so this
        // is cheap to call every iteration.
        probeButton.service( );

        // Check button state (blocking is handled by ProbeButton service)
        int buttonState = checkProbeButton( );
        // int buttonState = ProbeButton::getInstance().getButtonPress( true );
        if ( buttonState == 1 ) {
            return -18;
        } else if ( buttonState == 2 ) {
            return -16;
        }

        // delayMicroseconds(200);

        if ( millis( ) - doubleTimeout > 1000 ) {
            doubleTimeout = millis( );
            lastReadRaw = 0;
        }

        if ( millis( ) - lastProbeTime > 8 ) {
            lastProbeTime = millis( );
            // // Serial.println("probe timeout");
            // Serial.println("probe timeout");
            // Serial.flush();
            return -1;
        }
    }
    doubleTimeout = millis( );
    if ( debugProbing == 1 ) {
        // Serial.print("probeRead: ");
        // Serial.println(probeRead);
    }
    if ( probeRead == -1 ) {
        return -1;
    }

    // int padRead = 0;
    if ( probeRead < 300 ) { // take an average if it's a logo pad
        int probeReadings[ 8 ] = { 0 };
        for ( int i = 0; i < 8; i++ ) {
            probeReadings[ i ] = readProbeRaw( 0, 1 );
        }
        int probeReading = 0;
        int numberOfGoodReadings = 0;
        for ( int i = 0; i < 8; i++ ) {
            if ( probeReadings[ i ] > 0 ) {
                probeReading += probeReadings[ i ];
                numberOfGoodReadings++;
            }
        }
        // CRITICAL: don't divide by zero. All 4 reads can come back <= 0
        // if the probe is floating between pads or hit a transient noise
        // window. On Cortex-M33 with default CCR.DIV_0_TRP=0 the result
        // is silently 0 (which maps to an out-of-range row and returns
        // -1 below), but it's clearer + safer to bail explicitly.
        if ( numberOfGoodReadings == 0 ) {
            return -1;
        }
        probeRead = probeReading / numberOfGoodReadings;
        // padRead = 1;
        // Serial.print( "probeRead: " );
        // Serial.println( probeRead );
        // Serial.flush( );
    }

    int mapMin, mapMax;
    probeMapRange( &mapMin, &mapMax );
    int rowProbed = map( probeRead, mapMin, mapMax, 101, 0 );
    // Serial.print("\n\n\rprobeRead: ");
    // Serial.println(probeRead);
    // Serial.flush();
    // rowProbed = convertPadsToRows( rowProbed );

    if ( rowProbed <= 0 || rowProbed >= sizeof( probeRowMap ) ) {
        // if ( debugProbing == 1 ) {
        Serial.print( "out of bounds of probeRowMap[" );
        Serial.println( rowProbed );
        //}
        return -1;
    }
    if ( debugProbing == 1 ) {
        Serial.print( "probeRowMap[" );
        Serial.print( rowProbed );
        Serial.print( "]: " );
        Serial.println( probeRowMap[ rowProbed ] );
    }

    rowProbed = selectSFprobeMenu( probeRowMap[ rowProbed ] );
    if ( debugProbing == 1 ) {
        Serial.print( "rowProbed: " );
        Serial.println( rowProbed );
    }
    connectedRows[ 0 ] = rowProbed;
    connectedRowsIndex = 1;

    // Serial.print("maxVariance: ");
    // Serial.println(maxVariance);
    return rowProbed;
    // return probeRowMap[rowProbed];
}

int hsvProbe = 0;
int hsvProbe2 = 0;
unsigned long probeRainbowTimer = 0;

unsigned long lastProbeLEDsTime = 0;
unsigned long probeLEDsDelay = 20;

unsigned long lastButtonCheckTime = 0;

void Probing::probeLEDhandler( void ) {

    // core2busy = true;
    //  pinMode(2, OUTPUT);
    //  pinMode(9, INPUT);
    unsigned long currentTime = millis( );
    // if ( currentTime - lastProbeLEDsTime < probeLEDsDelay ) {
    //     return;
    // }
    lastProbeLEDsTime = currentTime;

    int waitedForButtonCheck = 0;

    while(checkingButton == 1) {
        waitedForButtonCheck++;
       tight_loop_contents();
       if (millis() - currentTime > 100) {
        // Serial.println("timeout");
        // Serial.flush();
        return;
       }
    }

    // if (waitedForButtonCheck > 0) {
    //     Serial.print("waitedForButtonCheck: ");
    //     Serial.println(waitedForButtonCheck);
    //     Serial.flush();
    //     delayMicroseconds(1000);
    // }

    // NOTE: showingProbeLEDs is no longer asserted across the switch
    // body. setPixelColor() is a pure RAM write and doesn't touch
    // PROBE_LED_PIN; the only thing the ProbeButton service has to
    // synchronize against is the actual PIO/DMA transfer (showBlocking
    // below). Holding the flag across setPixelColor + any incidental
    // logic above used to add tens of microseconds of "lock held but
    // doing nothing" per call, which compounded into missed button
    // edges. We lock only around the show.

    // Capture the requested mode BEFORE the switch: the cases below zero
    // showProbeLEDs as they consume it, and lastProbeLEDs used to be
    // overwritten right here - so the modeChanged computation after the
    // switch compared 0 against the already-clobbered tracker and was
    // ALWAYS false. lastProbeLEDUpdateTime never advanced, which left the
    // LED_SETTLE_TIME_MS gate in checkSwitchPosition() permanently open:
    // current got sampled while the WS2812's draw was still slewing after
    // a color change.
    int requestedMode = showProbeLEDs;

    switch ( showProbeLEDs ) {
        case 11:
            probeLEDs.setPixelColor( 0, 0x0f0fc6 ); // connect bright
            // Serial.println("connect bright");
            // Serial.flush();
            break;
    case 1:

            probeLEDs.setPixelColor( 0, 0x000032 ); // connect
        
        // probeLEDs[0].setColorCode(0x000011);
        //  Serial.println(showProbeLEDs);
        //   probeLEDs.show();
         showProbeLEDs = 0;
        break;
    case 2: {

        // Remove fade animation - red color that fades in over multiple frames
        // Array of colors from dim to bright red
        static const uint32_t removeFadeColors[] = {
            0x280000, // 0 - dimmest
            0x330101, // 1
            0x3c0202, // 2
            0x450303, // 3
            0x5e0404, // 4
            0x670505, // 5
            0x800707, // 6
            0xa90909, // 7
            0xb20a0a, // 8
            0xef1010, // 9
            0xff1313  // 10 - brightest
        };
        
        // Clamp removeFade to valid array bounds
        int fadeIndex = removeFade;
        if ( fadeIndex < 0 ) break;
        if ( fadeIndex > 10 ) break;
        
        probeLEDs.setPixelColor( 0, removeFadeColors[fadeIndex] );
        
        break;
    }
    case 3:
        probeLEDs.setPixelColor( 0, 0x003600 ); // measure
        // probeLEDs[0].setColorCode(0x001100);
        //  probeLEDs.show();
        //  Serial.println(showProbeLEDs);
        break;
    case 4:

        probeLEDs.setPixelColor( 0, 0x170c17 ); // select idle
        // NOTE: removed delay(20) here - it was being held while
        // showingProbeLEDs=1, which forced the ProbeButton service
        // to drop ~20ms worth of samples and miss the release edge
        // between two halves of a fast double-tap.
        break;
    case 5:
        probeLEDs.setPixelColor( 0, 0x111111 ); // all
        // probeLEDs[0].setColorCode(0x111111);
        //  Serial.println(showProbeLEDs);
        break;
    case 6:
        probeLEDs.setPixelColor( 0, 0x0c190c ); // measure dim
        break;
    case 7: {
        // hsvProbe++;
        if ( hsvProbe > 255 ) {
            hsvProbe = 0;
            hsvProbe2 -= 8;
            if ( hsvProbe2 < 15 ) {
                hsvProbe2 = 255;
            }
        }
        hsvColor probeColor;
        probeColor.h = hsvProbe;
        probeColor.s = hsvProbe2;
        probeColor.v = 25;

        uint32_t colorp = packRgb( HsvToRgb( probeColor ) );
        probeLEDs.setPixelColor( 0, colorp ); // select idle dim
        break;
    }
    case 8:
        // Was a spin-loop waiting for showProbeLEDs to be externally
        // changed away from 9 - but nothing in the tree ever set 9 or
        // observed the 8 -> 9 transition, so the loop would have
        // deadlocked core 2 forever AND held showingProbeLEDs=1 the
        // whole time (starving every button read). Reduced to a normal
        // single-shot case like everything else; the bottom of this
        // function shows it via showBlocking().
        probeLEDs.setPixelColor( 0, 0xffffff ); // max
        showProbeLEDs = 0;
        break;

    case 10:
        probeLEDs.setPixelColor( 0, 0x000000 ); // off
        showProbeLEDs = 0;
        break;
    default:
        break;
    }

    showProbeLEDs = 0;

    // Only update settle timer when LED MODE changes (not during fade brightness changes)
    // Case 2 is the remove fade - same mode, just brightness changes
    // We only care about mode changes that affect current draw significantly
    bool modeChanged = ( requestedMode != 0 && lastProbeLEDs != requestedMode );
    if ( requestedMode != 0 ) {
        lastProbeLEDs = requestedMode;
    }

    // CRITICAL: Probe LEDs always use blocking PIO transfers
    // This eliminates any DMA-related interference with INA219 current readings
    // and ensures immediate display for switch position detection.
    //
    // showingProbeLEDs is asserted only across the actual PIO transfer.
    // While it's set:
    //   - The continuous-polling SM (if active) gets switched to WS2812
    //     mode by probeButtonPausePolling() so showBlocking() can drive
    //     the line. We restore polling immediately after.
    //   - Any leftover CPU-path readers see the flag and back off.
    extern void probeButtonPausePolling( void );
    extern void probeButtonResumePolling( void );
    showingProbeLEDs = 1;
    probeButtonPausePolling( );
    probeLEDs.showBlocking( );
    probeButtonResumePolling( );
    showingProbeLEDs = 0;

    // Track when LED MODE was changed so we can wait for current to stabilize
    // Don't update for every fade step - that would block current reading continuously
    if ( modeChanged && requestedMode != 2 ) { // Don't count fade updates (case 2)
        lastProbeLEDUpdateTime = millis( );
    }
}

//! legacy functions from OG probing

int Probing::selectFromLastFound( void ) {

    rawOtherColors[ 1 ] = 0x0010ff;

    blinkTimer = 0;
    int selected = 0;
    int selectionConfirmed = 0;
    int selected2 = connectedRows[ selected ];
    Serial.print( "\n\r" );
    Serial.print( "      multiple nodes found\n\n\r" );
    Serial.print( "  short press = cycle through nodes\n\r" );
    Serial.print( "  long press  = select\n\r" );

    Serial.print( "\n\r " );
    for ( int i = 0; i < connectedRowsIndex; i++ ) {

        printNodeOrName( connectedRows[ i ] );
        if ( i < connectedRowsIndex - 1 ) {
            Serial.print( ", " );
        }
    }
    Serial.print( "\n\n\r" );
    delay( 10 );

    uint32_t previousColor[ 10 ];

    for ( int i = 0; i < connectedRowsIndex; i++ ) {
        previousColor[ i ] = leds.getPixelColor( nodesToPixelMap[ connectedRows[ i ] ] );
    }
    int lastSelected = -1;

    while ( selectionConfirmed == 0 ) {
        probeTimeout = millis( );
        // if (millis() - blinkTimer > 100)
        // {
        if ( lastSelected != selected && selectionConfirmed == 0 ) {
            for ( int i = 0; i < connectedRowsIndex; i++ ) {
                if ( i == selected ) {
                    leds.setPixelColor( nodesToPixelMap[ connectedRows[ i ] ],
                                        rainbowList[ 1 ][ 0 ], rainbowList[ 1 ][ 1 ],
                                        rainbowList[ 1 ][ 2 ] );
                } else {
                    // uint32_t previousColor =
                    // leds.getPixelColor(nodesToPixelMap[connectedRows[i]]);
                    if ( previousColor[ i ] != 0 ) {
                        int r = ( previousColor[ i ] >> 16 ) & 0xFF;
                        int g = ( previousColor[ i ] >> 8 ) & 0xFF;
                        int b = ( previousColor[ i ] >> 0 ) & 0xFF;
                        leds.setPixelColor( nodesToPixelMap[ connectedRows[ i ] ], ( r / 4 ) + 5,
                                            ( g / 4 ) + 5, ( b / 4 ) + 5 );
                    } else {

                        leds.setPixelColor( nodesToPixelMap[ connectedRows[ i ] ],
                                            rainbowList[ 1 ][ 0 ] / 8, rainbowList[ 1 ][ 1 ] / 8,
                                            rainbowList[ 1 ][ 2 ] / 8 );
                    }
                }
            }
            lastSelected = selected;

            Serial.print( " \r" );
            // Serial.print("");
            printNodeOrName( connectedRows[ selected ] );
            Serial.print( "  " );
        }
        // leds.show();
        showLEDsCore2 = 2;
        blinkTimer = millis( );
        //  }
        delay( 30 );
        int longShort = longShortPress( );
        delay( 5 );
        if ( longShort == 1 ) {
            selectionConfirmed = 1;
            // for (int i = 0; i < connectedRowsIndex; i++)
            // {
            //     if (i == selected)
            //     // if (0)
            //     {
            //         leds.setPixelColor(nodesToPixelMap[connectedRows[i]],
            //         rainbowList[rainbowIndex][0], rainbowList[rainbowIndex][1],
            //         rainbowList[rainbowIndex][2]);
            //     }
            //     else
            //     {
            //         leds.setPixelColor(nodesToPixelMap[connectedRows[i]], 0, 0, 0);
            //     }
            // }
            // showLEDsCore2 = 1;
            // selected = lastFound[node1or2][selected];
            //  clearLastFound();

            // delay(500);
            selected2 = connectedRows[ selected ];
            // return selected2;
            break;
        } else if ( longShort == 0 ) {

            selected++;
            blinkTimer = 0;

            if ( selected >= connectedRowsIndex ) {

                selected = 0;
            }
            // delay(100);
        }
        delay( 15 );
        //  }
        //}

        // showLEDsCore2 = 1;
    }
    selected2 = connectedRows[ selected ];

    for ( int i = 0; i < connectedRowsIndex; i++ ) {
        if ( i == selected ) {
            leds.setPixelColor( nodesToPixelMap[ connectedRows[ selected ] ],
                                rainbowList[ 0 ][ 0 ], rainbowList[ 0 ][ 1 ],
                                rainbowList[ 0 ][ 2 ] );
        } else if ( previousColor[ i ] != 0 ) {

            int r = ( previousColor[ i ] >> 16 ) & 0xFF;
            int g = ( previousColor[ i ] >> 8 ) & 0xFF;
            int b = ( previousColor[ i ] >> 0 ) & 0xFF;
            leds.setPixelColor( nodesToPixelMap[ connectedRows[ i ] ], r, g, b );
        } else {

            leds.setPixelColor( nodesToPixelMap[ connectedRows[ i ] ], 0, 0, 0 );
        }
    }

    // leds.setPixelColor(nodesToPixelMap[selected2], rainbowList[0][0],
    // rainbowList[0][1], rainbowList[0][2]); leds.show(); showLEDsCore2 = 1;
    probeButtonTimer = millis( );
    // connectedRowsIndex = 0;
    // justSelectedConnectedNodes = 1;
    return selected2;
}

int Probing::longShortPress( int pressLength ) {
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

int countLED = 0;
int lastProbeButtonState = 0;

/// @brief Checks for button press EVENTS (event-based, consumes event)
/// Use this when you want to detect a button press once and have it consumed.
/// For use in loops where you continuously check, use checkProbeButtonState() instead.
/// Now delegates to high-frequency ProbeButton service for instant response
/// @return 0 = neither pressed, 1 = remove button, 2 = connect button
int Probing::checkProbeButton( void ) {
    // Check for button press EVENTS, not current state
    // This prevents detecting the same press multiple times
    // Note: getButtonPress() consumes the event, so it only returns non-zero once per press
    int press = probeButton.getButtonPress( );

    // If we got a press event, return it
    if ( press != 0 ) {
        // Serial.println("\n\n\n\rcheckProbeButton press = " + String(press));
        return press;
    }

    // Otherwise check if we're in a blocking period (prevents rapid re-checking)
    if ( blockProbeButton > 0 && ( millis( ) - blockProbeButtonTimer < blockProbeButton ) ) {
        return 0; // Still blocked, no button state
    }

    // No press event and not blocked
    return 0;
}

/// @brief Checks CURRENT button state (state-based, doesn't consume events)
/// Use this in loops where you continuously check button state.
/// This won't consume button press events, allowing them to propagate to outer logic.
/// @return 0 = neither pressed, 1 = remove button, 2 = connect button
int Probing::checkProbeButtonState( void ) {
    // Simply return the current hardware state without consuming events
    // This allows the event to propagate to other parts of the code
    return probeButton.getButtonState( );
}

int Probing::readFloatingOrState( int pin, int rowBeingScanned ) { // this is the old probe reading code
    // return 0;
    enum measuredState state = unknownState;
    // enum measuredState state2 = floating;

    int readingPullup = 0;
    int readingPullup2 = 0;
    int readingPullup3 = 0;

    int readingPulldown = 0;
    int readingPulldown2 = 0;
    int readingPulldown3 = 0;

    // pinMode(pin, INPUT_PULLUP);

    if ( rowBeingScanned != -1 ) {

        analogWrite( PROBE_PIN, 128 );

        while ( 1 ) // this is the silliest way to align to the falling edge of the
                    // probe PWM signal
        {
            if ( gpio_get( PROBE_PIN ) != 0 ) {
                if ( gpio_get( PROBE_PIN ) == 0 ) {
                    break;
                }
            }
        }
    }

    delayMicroseconds( ( probeHalfPeriodus * 5 ) + ( probeHalfPeriodus / 2 ) );

    readingPullup = digitalRead( pin );
    delayMicroseconds( probeHalfPeriodus * 3 );
    readingPullup2 = digitalRead( pin );
    delayMicroseconds( probeHalfPeriodus * 1 );
    readingPullup3 = digitalRead( pin );

    // pinMode(pin, INPUT_PULLDOWN);

    if ( rowBeingScanned != -1 ) {
        while ( 1 ) // this is the silliest way to align to the falling edge of the
                    // probe PWM signal
        {
            if ( gpio_get( PROBE_PIN ) != 0 ) {
                if ( gpio_get( PROBE_PIN ) == 0 ) {
                    break;
                }
            }
        }
    }

    delayMicroseconds( ( probeHalfPeriodus * 5 ) + ( probeHalfPeriodus / 2 ) );

    readingPulldown = digitalRead( pin );
    delayMicroseconds( probeHalfPeriodus * 3 );
    readingPulldown2 = digitalRead( pin );
    delayMicroseconds( probeHalfPeriodus * 1 );
    readingPulldown3 = digitalRead( pin );

    // if (readingPullup == 0 && readingPullup2 == 1 && readingPullup3 == 0 &&
    // readingPulldown == 1 && readingPulldown2 == 0 && readingPulldown3 == 1)
    // {
    //     state = probe;
    // }

    if ( ( readingPullup != readingPullup2 || readingPullup2 != readingPullup3 ) &&
         ( readingPulldown != readingPulldown2 ||
           readingPulldown2 != readingPulldown3 ) &&
         rowBeingScanned != -1 ) {
        state = probe;

        // if (readingPulldown != readingPulldown2 || readingPulldown2 !=
        // readingPulldown3)
        // {
        //     state = probe;

        // } else
        // {
        //     Serial.print("!");
        // }
    } else {

        if ( readingPullup2 == 1 && readingPulldown2 == 0 ) {

            state = floating;
        } else if ( readingPullup2 == 1 && readingPulldown2 == 1 ) {
            //              Serial.print(readingPullup);
            // // Serial.print(readingPullup2);
            // // Serial.print(readingPullup3);
            // // //Serial.print(" ");
            //  Serial.print(readingPulldown);
            // // Serial.print(readingPulldown2);
            // // Serial.print(readingPulldown3);
            //  Serial.print("\n\r");

            state = high;
        } else if ( readingPullup2 == 0 && readingPulldown2 == 0 ) {
            //  Serial.print(readingPullup);
            // // Serial.print(readingPullup2);
            // // Serial.print(readingPullup3);
            // // //Serial.print(" ");
            //  Serial.print(readingPulldown);
            // // Serial.print(readingPulldown2);
            // // Serial.print(readingPulldown3);
            //  Serial.print("\n\r");
            state = low;
        } else if ( readingPullup == 0 && readingPulldown == 1 ) {
            // Serial.print("shorted");
        }
    }

    // Serial.print("\n");
    // showLEDsCore2 = 1;
    // leds.show();
    // delayMicroseconds(100);

    return state;
}

void Probing::startProbe( long probeSpeed ) {

    // pinMode(PROBE_PIN, OUTPUT_4MA);
    // // pinMode(BUTTON_PIN, INPUT_PULLDOWN);
    // // pinMode(ADC0_PIN, INPUT);
    // digitalWrite(PROBE_PIN, HIGH);
}

void Probing::stopProbe( ) {
    // pinMode(PROBE_PIN, INPUT);
    // pinMode(BUTTON_PIN, INPUT);
}

int Probing::checkLastFound( int found ) {
    int found2 = 0;
    return found2;
}

void Probing::clearLastFound( ) {}

// scanRows() deleted: dead code (no callers, immediate return 0), and its
// unreachable body carried a real bug (connected CHIP_L xMapRead but
// disconnected hardcoded x2). Row detection is the ADC5 resistor ladder now.

int Probing::readRails( int pin ) {
    int state = -1;

    // Serial.print("adc0 \t");
    // Serial.println(adcReadings[0]);
    // Serial.print("adc1 \t");
    // Serial.println(adcReadings[1]);
    // Serial.print("adc2 \t");
    // Serial.println(adcReadings[2]);
    // Serial.print("adc3 \t");
    // Serial.println(adcReadings[3]);

    return state;
}