#include "RotaryEncoder.h"
#include "externVars.h"  // noteUserInput()
#include "CH446Q.h"
#include "FileParsing.h"
#include "JumperlessDefines.h"
#include "LEDs.h"
#include "MatrixState.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "Peripherals.h"
#include "Probing.h"
#include "hardware/pwm.h"
#include "hardware/structs/sio.h" // sio_hw->cpuid for single-owner core check
#include "pico/mutex.h"
#include "pico/stdlib.h"
#include "quadrature.pio.h"

#include "Commands.h"
#include "Graphics.h"
#include "Menus.h"
#include <cstdint>

volatile int slotChanged = 0;
PIO pioEnc = nullptr; // Will be dynamically assigned

int smEnc = -1; // Will be dynamically assigned
uint offsetEnc = 0;
const uint PIN_AB = 12;

int netSlot = 0;

int newPositionEncoder = 0;
int lastPositionEncoder = 0;
int encoderRaw = 0;
int lastPosition = 0;
int position = 0;
volatile bool resetPosition = false;

int lastButtonState = 0;

volatile int rotaryEncoderMode = 0;

volatile int slotPreview = 0;

// volatile enum { IDLE, UP, DOWN } encoderDirectionState;
// volatile enum { IDLE, PRESSED, HELD, RELEASED } encoderButtonState;

void initRotaryEncoder( void ) {
    #if !defined(OG_JUMPERLESS)
    pinMode( BUTTON_ENC, INPUT );
    pinMode( QUADRATURE_A_PIN, INPUT_PULLUP );
    pinMode( QUADRATURE_B_PIN, INPUT_PULLUP );
    #endif

    // CRITICAL: Dynamically claim an unused PIO to avoid conflicts
    // Serial.println("◆ Initializing rotary encoder with dynamic PIO allocation...");

    // Try PIO instances in order. RP2350 has three PIO blocks; RP2040 has two.
#if defined(PICO_RP2350)
    PIO pio_instances[] = { pio0, pio1, pio2 };
#else
    PIO pio_instances[] = { pio0, pio1 };
#endif
    const int numPioInst = (int)( sizeof( pio_instances ) / sizeof( pio_instances[0] ) );
    bool pio_allocated = false;

    for ( int i = 0; i < numPioInst && !pio_allocated; i++ ) {
        PIO test_pio = pio_instances[ i ];
        // Serial.printf("◆ Trying PIO%d for rotary encoder...\n", pio_get_index(test_pio));

        // RP2350B: a block reaches 32 of the 48 pins (GPIOBASE 0 or 16). The
        // encoder reads PIN_AB / PIN_AB+1 (12/13) - a base-16 block (PIO2 is,
        // for the crossbar chip-select strobe, T3.2) cannot see them and the
        // claim would "succeed" into a dead SM. Skip it.
        if ( pio_get_gpio_base( test_pio ) != 0 ) {
            continue;
        }

        // Try to claim a state machine
        int test_sm = pio_claim_unused_sm( test_pio, false );
        if ( test_sm < 0 ) {
            //  Serial.printf("◆ PIO%d: No available state machines\n", pio_get_index(test_pio));
            continue;
        }

        // Check if we can add the program
        if ( !pio_can_add_program( test_pio, &quadrature_encoder_program ) ) {
            // Serial.printf("◆ PIO%d: Cannot add quadrature encoder program\n", pio_get_index(test_pio));
            pio_sm_unclaim( test_pio, test_sm );
            continue;
        }

        // Add the program
        uint test_offset = pio_add_program( test_pio, &quadrature_encoder_program );
        if ( test_offset < 0 ) {
            // Serial.printf("◆ PIO%d: Failed to add quadrature encoder program\n", pio_get_index(test_pio));
            pio_sm_unclaim( test_pio, test_sm );
            continue;
        }

        // Success! Assign the resources
        pioEnc = test_pio;
        smEnc = test_sm;
        offsetEnc = test_offset;

        // Serial.printf("◆ SUCCESS: Rotary encoder allocated PIO%d SM%d offset=%d\n",
        //             pio_get_index(pioEnc), smEnc, offsetEnc);

        pio_allocated = true;
    }

    if ( !pio_allocated ) {
        // Serial.println("◆ ERROR: Failed to allocate PIO resources for rotary encoder");
        // Serial.println("◆ Rotary encoder will not function");
        return;
    }

    // Initialize the quadrature encoder
    quadrature_encoder_program_init( pioEnc, smEnc, PIN_AB, 0 );
    // Serial.println("◆ Rotary encoder initialized successfully");
}

void unInitRotaryEncoder( void ) {
    if ( pioEnc && smEnc != (uint)-1 ) {
        // Serial.printf("◆ Cleaning up rotary encoder resources: PIO%d SM%d\n",
        //               pio_get_index(pioEnc), smEnc);

        // Remove the program and unclaim the state machine
        pio_remove_program( pioEnc, &quadrature_encoder_program, offsetEnc );
        pio_sm_unclaim( pioEnc, smEnc );

        // Reset the variables
        pioEnc = nullptr;
        smEnc = -1;
        offsetEnc = 0;

        // Serial.println("◆ Rotary encoder resources cleaned up");
    }
}

// const char rotaryEncoderHelp[] =
//     "\n\r" // this is for the og jumperless
//     "\t\t  Rotary Encoder Help\n\r"
//     "\t\t  -------------------\n\r"
//     "\n\r"
//     "            A  COM  B                                 \n\r"
//     "    _________________________________________________   \n\r"
//     "  /        D12 D11 D10                                \\ \n\r"
//     " / .        [,][,][,][][][][][][][][][][][][][]        \\  \n\r"
//     "|  ' `0    |  /```\\  |                                  | \n\r"
//     "|  '   `   | (  O  ) |                                  | \n\r"
//     "|  '     ` |  \\___/  |                                  | \n\r"
//     "|  '     '  ['][ ][']{}{}{}{}{}{}{}{}{}[][][][]         | \n\r"
//     "|  '     '  D13  AREF                                   | \n\r"
//     "|            button                                     | \n\r"
//     "\n\r"
//     "Stick a 5 pin rotary encoder into the board as shown above. \n\n\r"
//     "When rotary encoder mode is on, these pins will be connected\n\r"
//     "to the Jumperless [A-UART_Rx(16), B-UART_Tx(17), SW-GPIO_0] \n\n\r"

//     "The LEDs under A0-A7 {shown in curly braces} show which of the \n\r"
//     "8 slots are active/connected(pink) and previewing(blue/green)\n\n\r"
//     "Press the encoder button to made the previewed slot active\n\r"
//     "(going into probing mode will make the previewed slot active)\n\n\r"

//     "You can cycle through slots by entering z(next) or x(previous)\n\r"
//     "or by turning the rotary encoder and then pressing (obviously)\n\n\r"

//     "You can show the contents of all the slot files by entering s\n\r"
//     "(copy/paste the output into a text file on your computer) \n\n\r"
//     "Load files by entering o and paste the text into this terminal \n\n\r"

//     "Wokwi sketches will be loaded into whichever slot is active\n\n\r"

//     "This is a WIP, so let me know if something's broken or you want\n\r"
//     "something added. \n\n\r ";

// void printRotaryEncoderHelp(void) {
//   Serial.print(rotaryEncoderHelp);
//   return;
// }

// Function to check if rotary encoder is properly initialized
bool isRotaryEncoderInitialized( void ) {
    return ( pioEnc != nullptr && smEnc != (uint)-1 );
}

// Bounded quadrature count read. The generated helper
// quadrature_encoder_get_count() reads fifo_level + 1 words, and that +1 is
// an UNBOUNDED pio_sm_get_blocking() waiting on a fresh push.
// encoderServiceYield() made this poll reachable from inside NetVoltageScan
// sense taps (readingADC held, crosspoints closed, core2busy raised) and
// from sendXYraw's PIO handshake wait - a quadrature SM that ever stops
// pushing would wedge core 1 with all of that claimed. Drain only what's
// already queued and keep the last value across a momentarily-empty FIFO:
// the program pushes continuously (~4 SM cycles/sample), so the newest
// queued word is at most microseconds stale - fresh enough for a clickwheel.
static long s_lastQuadCount = 0;
static long quadratureCountBounded( void ) {
    uint n = pio_sm_get_rx_fifo_level( pioEnc, smEnc );
    while ( n-- > 0 ) {
        s_lastQuadCount = (long)(int32_t)pio_sm_get( pioEnc, smEnc );
    }
    return s_lastQuadCount;
}

// Read the raw quadrature count straight from the PIO state machine. Safe to
// call from Core 1 only while Core 2's rotaryEncoderStuff() is suspended via
// encoderOverride (otherwise both cores drain the same RX FIFO and race).
long getEncoderRawCount( void ) {
    if ( !isRotaryEncoderInitialized( ) ) {
        return 0;
    }
    return quadratureCountBounded( );
}

// Function to get rotary encoder status
void printRotaryEncoderStatus( void ) {
    if ( isRotaryEncoderInitialized( ) ) {
        Serial.printf( "◆ Rotary encoder: PIO%d SM%d offset=%d\n",
                       pio_get_index( pioEnc ), smEnc, offsetEnc );
    } else {
        Serial.println( "◆ Rotary encoder: Not initialized" );
    }
}

unsigned long buttonHoldStart = 0;
unsigned long buttonHoldLength = 500;

unsigned long doubleClickTimer = 0;
unsigned long doubleClickLength = 250;

unsigned long buttonDebounceTimer = 0;
unsigned long buttonDebounceTimer2 = 0;
unsigned long debounceTime = 2000;

long positionOffset = 0;

int showingPreview = 0;
int rotState = 0;
int encoderWasPressed = 1;
int encoderIsPressed = 0;
int buttonState = 1; // digitalRead(BUTTON_ENC);

int encoderAstate = 0;
int encoderBstate = 0;
int justPressed = 1;
int lastEncoderBstate = 0;

int probeWasActive = 0;
int encoderReleased = 0;
int printSlotChanges = 0;

int debugEncoder = 0;

int encoderStepsToChangePosition = 2;

int lastRotaryDivider = 8;

/// number of steps to trigger a encoderDirectionState change
int rotaryDivider = 8;

volatile int encoderOverride = 0;

bool resetEncoderPosition = false;
long encoderPositionOffset = 0;
volatile long encoderPosition = 0;

volatile int numberOfSteps = 0;

volatile encoderDirectionStates encoderDirectionState = NONE;
volatile encoderButtonStates encoderButtonState = IDLE;

volatile encoderButtonStates lastButtonEncoderState = IDLE;

volatile encoderDirectionStates lastDirectionState = NONE;
volatile bool encoderDirectionConsumed = true;

// Timer to prevent Core 2 from clearing button events before Core 1 can read them
static unsigned long buttonEventTimestamp = 0;
const unsigned long BUTTON_EVENT_MIN_DURATION_US = 10000; // 10ms minimum hold time - ensures Core 1 catches events even when busy

// ── Hold animation state ──
// Managed by holdAnimationStuff(); drives the white→red LED sweep and
// transitions encoderButtonState from HELD → LONG_HELD when the animation ends.
static unsigned long holdAnimTimer = 0;
static int holdAnimStep = 0;
static bool holdAnimActive = false;
static bool holdAnimLongHeldFlashed = false;

// Encoder click/rotation interlock: pushing the button mechanically torques the
// shaft, so rotary pulses around the press moment are spurious and used to
// advance the value/menu on every click. We disambiguate click-vs-turn at the
// source, so a spurious step is never *committed* (and therefore never drawn) —
// no commit-then-revert, no single-frame flash on the OLED / breadboard.
//
// Two cooperating mechanisms:
//   - CONFIRM GATE: the first detent out of rest is held back (invisible) for
//                   STEP_CONFIRM_US. The raw motion still accumulates, but
//                   encoderPosition / encoderDirectionState stay frozen. If a
//                   press lands inside the window the pending motion is discarded
//                   (nothing was drawn). If the window elapses with no press, the
//                   motion commits in one go — the detent registers, just slightly
//                   late. Mid-spin steps are never gated, so an active turn stays
//                   fully responsive.
//   - AFTER window: once a press is detected, rotation is frozen for
//                   CLICK_SUPPRESS_AFTER_US so the click itself (and its release
//                   jiggle) cannot register as rotation; the baselines track the
//                   shaft so motion resumes without a jump once it expires.
// FOLLOW-UP (deep refactor): collapse the two consumer paths (encoderPosition for
// the numeric editors, encoderDirectionState/position for the menus) onto a single
// "committedRaw" value, so the AFTER window and the confirm gate become two simple
// policies governing how committedRaw follows the live quadrature count. Also unify
// the dual polling (Core 0 menu loops call rotaryEncoderStuff() directly while Core 2
// also polls it) so there is one owner of the encoder state.
const unsigned long CLICK_SUPPRESS_AFTER_US = 100000; // 100ms after the press
const unsigned long STEP_CONFIRM_US = 35000;          // first detent held this long before it shows
const unsigned long ENCODER_REARM_IDLE_US = 150000;   // shaft must rest this long to re-arm the gate
static unsigned long lastEncoderClickUs = 0;
static long heldEncoderPos = 0; // encoderPosition value held frozen during the AFTER window

// Confirm-gate state. heldConfirmPos / restMenuPosition snapshot the logical
// state at the moment the shaft leaves rest so a press can restore it; the gate
// is "armed" (motionConfirmed == false) until either it commits or a press
// cancels it.
static unsigned long lastRawChangeUs = 0;
static long lastRawForIdle = 0;
static unsigned long motionStartUs = 0;
static bool motionConfirmed = true; // true => no pending unconfirmed first-detent motion
static long heldConfirmPos = 0;     // encoderPosition held frozen during the confirm window
static long heldConfirmOffset = 0;  // encoderPositionOffset at arm time - restored on commit
static long restMenuPosition = 0;   // menu position counter captured at rest
static long prevEncoderPos = 0;     // encoderPosition from the previous poll (the at-rest value)
static bool encoderWasPressedGate = false; // press-edge detector local to rotaryEncoderStuff

// Paced direction-event emission (see the "Paced event emission" block in
// rotaryEncoderStuffLocked): timestamp of the last emitted UP/DOWN event and
// how long an unconsumed event may sit before it's considered abandoned.
static unsigned long dirEventEmitUs = 0;
const unsigned long DIR_EVENT_ABANDON_US = 250000; // 250ms

// Direction of the last committed detent step (+1 = raw increasing, -1 =
// decreasing, 0 = none yet). Drives the backlash margin: a step against this
// direction needs an extra half-divider of travel before it counts.
static int lastStepDir = 0;

// Recoil guard: when a fast flick ends, the shaft bounces off the detent
// spring by 1-2 REAL detents (+4..+7 raw counts against the travel
// direction - verified on hardware via the [enc] diagnostics), which the
// static backlash margin can't absorb. While steps are committing rapidly
// (faster than RECOIL_ARM_GAP_US apart), reversals are disbelieved outright
// until the shaft has been step-quiet for RECOIL_GUARD_US; the recoil travel
// slides the baseline instead of accumulating, so resumed forward motion
// registers without catching up through the bounce distance. A deliberate
// direction change just waits out the guard (~200ms after the last fast
// step) - imperceptible in practice.
static unsigned long lastCommitUs = 0;
static unsigned long recoilGuardUntilUs = 0;
const unsigned long RECOIL_ARM_GAP_US = 100000; // commits closer than this = "fast"
const unsigned long RECOIL_GUARD_US = 200000;   // reversal quiet window after fast steps

// Hysteresis baseline for menu navigation: the raw encoder count at the last
// committed detent step. A new step is only emitted once the shaft has moved a
// full rotaryDivider counts from here, so the trip points are always a full
// divider away from where the shaft currently rests. This makes the menu step
// boundary phase-independent: when you settle into a detent the count can chatter
// by a count or two without ever crossing a trip point. rotaryDivider sets the
// sensitivity (counts per step), not the absolute bin phase.
static long lastDetentRaw = 0;

// ── Press animation state ──
// White flash on press, rainbow transition during second half of buttonHoldLength.
static bool pressAnimActive = false;
static unsigned long pressAnimUpdateTimer = 0;
volatile bool buttonPressAnimActive = false;
volatile uint32_t pressAnimLogoColors[ 8 ] = { 0 };

// Body of the button-state poll. ONLY call with encoderPollMutex held (via
// the rotaryEncoderButtonStuff() wrapper or from rotaryEncoderStuffLocked).
static void rotaryEncoderButtonStuffLocked( void ) {
    // CRITICAL: Don't update lastButtonEncoderState while we're holding an event for Core 1
    // This preserves the PRESSED->RELEASED transition that Core 1 checks for
    // Only update when state actually changes or when not in a held event state
    bool isHoldingEventForCore1 = false;
    if ( encoderButtonState == RELEASED || encoderButtonState == DOUBLECLICKED ) {
        // Check if we're still within the minimum hold time
        if ( micros( ) - buttonEventTimestamp < BUTTON_EVENT_MIN_DURATION_US ) {
            isHoldingEventForCore1 = true;
        }
    }

    // Only update lastButtonEncoderState if we're not holding an event
    // EXCEPTION: If Core 1 cleared state to IDLE, always update (event was consumed)
    if ( !isHoldingEventForCore1 || encoderButtonState == IDLE ) {
        lastButtonEncoderState = encoderButtonState;
    }

    buttonState = gpio_get( BUTTON_ENC );
    // Serial.print("buttonState: ");
    // Serial.println(buttonState);

    if ( buttonState == 0 ) {
        encoderIsPressed = 1;
        //     Serial.print("pressed: ");
        // Serial.println(encoderIsPressed);
    } else {
        encoderIsPressed = 0;
        // Serial.print("pressed: ");
        // Serial.println(encoderIsPressed);
    }

    // State machine logic - only transitions on actual changes
    // RELEASED and DOUBLECLICKED states persist until explicitly cleared by consuming code
    // This prevents missing button events in polling loops

    // Multi-core protection: Ensure event states persist long enough for Core 1 to detect
    // Core 2 runs this function much faster than Core 1 can poll, so we need a minimum hold time

    // Only transition to IDLE if we're not in a persistent event state
    if ( encoderIsPressed == 0 && encoderWasPressed == 0 ) {
        // Don't auto-clear RELEASED or DOUBLECLICKED states immediately
        // Wait for minimum duration OR explicit clear by consuming code
        if ( encoderButtonState == RELEASED || encoderButtonState == DOUBLECLICKED ) {
            // Check if enough time has passed since the event was set
            if ( micros( ) - buttonEventTimestamp >= BUTTON_EVENT_MIN_DURATION_US ) {
                // Time expired - now safe to auto-clear if still not consumed
                lastButtonEncoderState = encoderButtonState; // Update before clearing
                encoderButtonState = IDLE;
            }
        } else if ( encoderButtonState != RELEASED && encoderButtonState != DOUBLECLICKED ) {
            // For PRESSED or HELD states, clear immediately when button released
            encoderButtonState = IDLE;
        }
        // lastButtonEncoderState = IDLE;
    }

    if ( encoderIsPressed == 0 && encoderWasPressed == 1 ) {
        // CRITICAL: Set lastButtonEncoderState BEFORE changing to RELEASED
        // This ensures the PRESSED state is captured for Core 1's detection pattern
        lastButtonEncoderState = encoderButtonState; // Should be PRESSED or HELD
        encoderButtonState = RELEASED;
        buttonEventTimestamp = micros( ); // Mark event time for minimum hold duration
        encoderReleased = 1;
        doubleClickTimer = millis( );
        buttonDebounceTimer2 = micros( );

        showLEDsCore2 = 1;
        encoderWasPressed = encoderIsPressed;
    }

    if ( encoderIsPressed == 1 && encoderWasPressed == 1 ) {
        if ( millis( ) - buttonHoldStart > buttonHoldLength && encoderButtonState != MEDIUM_HELD && encoderButtonState != LONG_HELD ) {
            // HELD is set here; LONG_HELD is driven by holdAnimationStuff()
            // when the animation finishes its cycles.
            encoderButtonState = HELD;
            // if (triggerFlash == -1) {
            //     triggerFlash = 1;
            //     flashCountdown = 0;
            // }
        }
    }

    if ( encoderIsPressed == 1 && encoderWasPressed == 0 ) {
        buttonHoldStart = millis( );
        // NOTE: do NOT pio_sm_restart() here (removed). The quadrature
        // program keeps its previous-pin-state in the OSR; restart clears
        // the OSR, so if the pins aren't at 00 the program sees a phantom
        // transition and adds a spurious +/-1 count. Restarting on every
        // click (plus every divider change) made the raw count drift by
        // THOUSANDS over a session - caught via the [enc] reversal
        // diagnostics. The SM is initialized once and never restarted.
        // encoderRaw = quadrature_encoder_get_count(pioEnc, smEnc);
        // lastPositionEncoder = encoderRaw;

        // Click/rotation interlock: mark the press moment so rotaryEncoderStuff can
        // suppress the click jiggle (see CLICK_SUPPRESS_AFTER_US / confirm-gate notes).
        lastEncoderClickUs = micros( );

        if ( millis( ) - doubleClickTimer < doubleClickLength ) {
            encoderButtonState = DOUBLECLICKED;
            buttonEventTimestamp = micros( ); // Mark event time for minimum hold duration

        } else {
            encoderButtonState = PRESSED;
        }
        doubleClickTimer = millis( );
        // buttonHoldStart = millis();

        encoderWasPressed = encoderIsPressed;
    }

    lastButtonState = buttonState;
    encoderWasPressed = encoderIsPressed;
}

bool isEncoderButtonPhysicallyPressed( void ) {
    // Button is active LOW - returns true when physically pressed (GPIO reads 0)
    return gpio_get( BUTTON_ENC ) == 0;
}

// ── EncoderClickTracker ─────────────────────────────────────────────────────
// Per-UI click/hold classifier on the physical pin (see RotaryEncoder.h).

void EncoderClickTracker::reset( void ) {
    wasDown = isEncoderButtonPhysicallyPressed( );
    // If the button is already down on entry (e.g. the click that launched
    // this UI is still held), pretend the hold already fired so the eventual
    // release is classified as ENC_HOLD_RELEASE, not a spurious ENC_CLICK.
    holdFired = wasDown;
    longFired = wasDown;
    downStartMs = millis( );
    lastEdgeMs = millis( );
}

unsigned long EncoderClickTracker::heldForMs( void ) const {
    return wasDown ? ( millis( ) - downStartMs ) : 0;
}

EncoderClickEvent EncoderClickTracker::poll( void ) {
    unsigned long now = millis( );
    bool down = isEncoderButtonPhysicallyPressed( );

    if ( down != wasDown ) {
        if ( now - lastEdgeMs < debounceMs ) {
            return ENC_NONE; // bounce - ignore the edge for now
        }
        lastEdgeMs = now;
        wasDown = down;
        if ( down ) {
            downStartMs = now;
            holdFired = false;
            longFired = false;
            return ENC_PRESS;
        }
        return holdFired ? ENC_HOLD_RELEASE : ENC_CLICK;
    }

    if ( down ) {
        unsigned long held = now - downStartMs;
        if ( !longFired && held >= longHoldMs ) {
            longFired = true;
            holdFired = true;
            return ENC_LONG_HOLD;
        }
        if ( !holdFired && held >= holdMs ) {
            holdFired = true;
            return ENC_HOLD;
        }
    }
    return ENC_NONE;
}

void holdAnimationStuff( void ) {
    // Hold animation: fill logo LED pairs from bottom → top in rainbow.
    // 3 pairs (GPIO, DAC, ADC) light up in sequence and STAY on per sweep.
    // 2 full sweeps with increasing speed, then LONG_HELD fires.
    // Colours cycle through a rainbow hue based on the overall animation progress.

    static const logoOverrideNames holdLogoPairs[][ 2 ] = {
        { GPIO_0, GPIO_1 },
        { DAC_0, DAC_1 },
        { ADC_0, ADC_1 },
    };
    static const logoOverrideNames holdLogos[] = {
        GPIO_0, GPIO_1,
        DAC_0, DAC_1,
        ADC_0, ADC_1 };
    static const int PAIR_COUNT = 3;
    static const int LOGO_COUNT = 6;
    static const int HOLD_ANIM_CYCLES = 2;            // number of full sweeps before LONG_HELD
    static const unsigned long HOLD_STEP_MS = 450;    // ms per pair step (slow/deliberate)
    static const unsigned long HOLD_STEP_IN_PAD = 50; // ms per pair step (slow/deliberate)

    static const uint32_t PRESS_WHITE = 0x151633; // visible but not blinding

    static int triggerFlash = -1;
    static unsigned long flashDuration = 150;
    static unsigned long flashStartTime = 0;
    static encoderButtonStates lastEncoderButtonState = IDLE;
    static int rebootFlag = -1;

    uint32_t flashColors[ 4 ] = {
        0x101053, // PRESSED
        0x441022, // HELD
        0x663305, // MEDIUM_HELD
        0x990000, // LONG_HELD
    };

    if ( rebootFlag > 0 && encoderIsPressed == 1) {
        // if ( encoderButtonState != IDLE) {
            rebootFlag++;
            if ( rebootFlag >= 100 ) {
                rebootFlag = -1;
                triggerFlash = -1;
                rp2040.reboot( );
            }
            triggerFlash = 3;

        // } else {
            // rebootFlag = -1;
        // }
    } else {
        rebootFlag = -1;
    }
    // Trigger flash whenever encoderButtonState changes (map state → flash color index)
    if ( encoderButtonState != lastEncoderButtonState ) {
        lastEncoderButtonState = encoderButtonState;
        if ( encoderButtonState == PRESSED ) {
            triggerFlash = 0;
        } else if ( encoderButtonState == HELD ) {
            triggerFlash = 1;
        } else if ( encoderButtonState == MEDIUM_HELD ) {
            triggerFlash = 2;
        } else if ( encoderButtonState == LONG_HELD ) {
            triggerFlash = 3;
        } else {
            triggerFlash = -1; // IDLE, RELEASED, DOUBLECLICKED — no flash
        }
        if ( triggerFlash >= 0 ) {
            flashStartTime = millis( );
        }
    }

    // ── Press animation: white flash → rainbow transition ──
    // Activates on PRESSED, runs until HELD or release.

    if ( encoderButtonState == PRESSED && !pressAnimActive ) {
        // Immediate white flash on press
        // Write colors BEFORE setting the cross-core flag so logoSwirl()
        // on Core 1 never sees the flag with stale/zero color data.
        pressAnimActive = true;
        for ( int i = 0; i < 8; i++ ) {
            pressAnimLogoColors[ i ] = flashColors[ 0 ];
        }
        pressAnimUpdateTimer = millis( );
        buttonPressAnimActive = true; // set AFTER colors are ready
        showLEDsCore2 = 2;
    }
    unsigned long elapsed = millis( ) - buttonHoldStart;

    if ( pressAnimActive || holdAnimActive ) {

        // first press, hold trigger, medium hold, long hold

        // for ( int i = 0; i < 4; i++ ) {
        //     if ( millis( ) - buttonHoldStart >= flashCountdown && millis( ) - buttonHoldStart <= flashCountdown + flashDuration ) {
        //         // setLogoOverride( LOGO_TOP, flashColors[ i ] );
        //         // setLogoOverride( LOGO_BOTTOM, flashColors[ i ] );
        //         // showLEDsCore2 = 2;
        //         inFlashRange = i;
        //         break;
        //     }
        // }

        // Throttle updates to ~20ms
        // Skip rainbow overwrite while state-change flash is active so flash colors aren't clobbered
        bool flashActive = ( triggerFlash >= 0 && ( millis( ) - flashStartTime < flashDuration ) );
        if ( millis( ) - pressAnimUpdateTimer >= 10 && rebootFlag == -1 ) {
            pressAnimUpdateTimer = millis( );

            unsigned long halfPoint = 1;
            // Serial.print("elapsed: ");
            // Serial.println(elapsed);

            // Serial.flush();

            if ( elapsed >= halfPoint && !flashActive ) {
                // Second half: transition outer LEDs from white to rainbow
                float progress = 1.0f - (float)( elapsed - ( halfPoint ) ) / (float)( HOLD_ANIM_CYCLES * ( HOLD_STEP_MS + HOLD_STEP_IN_PAD ) * LOGO_COUNT );
                // Serial.println(progress);
                if ( progress > 1.0f ) {
                    progress -= 1.0f;
                } else if ( progress < 0.0 ) {
                    progress += 1.0f;
                }

                uint8_t sat = 255 - (uint8_t)( progress * 90.0f ); // 0 (white) → 230 (vivid)

                for ( int i = 0; i < 6; i++ ) {
                    uint8_t hue = ( (uint8_t)( ( i * 255 ) / 6 ) + (uint8_t)( progress * 2000.0f ) ) % 255; // evenly spaced rainbow

                    uint8_t val = (uint8_t)( ( 1.0f - progress ) * 80.0 ) + 35;

                    hsvColor hsv = { hue, sat, val };

                    pressAnimLogoColors[ i ] = HsvToRaw( hsv );
                }
                // Center LEDs stay white
                // pressAnimLogoColors[ 6 ] = PRESS_WHITE;
                pressAnimLogoColors[ 7 ] = PRESS_WHITE;
                showLEDsCore2 = 2;
            }
        }
    }

    // Cancel press animation on double-click
    if ( pressAnimActive && encoderButtonState == DOUBLECLICKED ) {
        pressAnimActive = false;
        buttonPressAnimActive = false;
        showLEDsCore2 = 2;
    }

    // ── Start animation on entering HELD ──
    if ( ( encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ) && !holdAnimActive ) {
        // Keep press animation (logo rainbow) running — it stays visible
        // while the connector LED hold animation plays alongside it.

        holdAnimStep = 0;
        holdAnimTimer = millis( );
        holdAnimActive = true;
        holdAnimLongHeldFlashed = false;
        for ( int p = 0; p < PAIR_COUNT; p++ ) {
            setLogoOverride( holdLogoPairs[ p ][ 0 ], -3 );
            setLogoOverride( holdLogoPairs[ p ][ 1 ], -3 );
        }
    }

    // ── Step animation while HELD ──
    if ( ( encoderButtonState == HELD || encoderButtonState == MEDIUM_HELD ) && holdAnimActive ) {
        unsigned long stepDuration = holdAnimStep % 2 == 0 ? HOLD_STEP_MS : HOLD_STEP_IN_PAD; // Alternate between main step and pad
        if ( millis( ) - holdAnimTimer >= stepDuration ) {
            holdAnimTimer = millis( );

            int cycleNum = holdAnimStep / LOGO_COUNT; // which sweep (0‥N-1)
            int pairIdx = holdAnimStep % LOGO_COUNT;  // which pair  (0‥2)

            if ( cycleNum < HOLD_ANIM_CYCLES ) {
                // Rainbow: hue progresses through 0°→360° across all steps
                int totalSteps = HOLD_ANIM_CYCLES * LOGO_COUNT;
                uint8_t hue = ( 255 - ( (int)( (float)holdAnimStep / (float)( totalSteps - 4 ) * 255.0f ) ) ) % 256; // 0‥255

                // // HSV→RGB (S=1, V=1) — full saturation, full brightness
                // float h60 = hue / 60.0f;
                // int   sector = (int)h60 % 6;
                // float frac = h60 - (int)h60;
                // uint8_t q = (uint8_t)(255 * (1.0f - frac));
                // uint8_t t = (uint8_t)(255 * frac);
                // uint8_t r, g, bv;
                // switch (sector) {
                //   case 0: r = 255; g = t;   bv = 0;   break;
                //   case 1: r = q;   g = 255; bv = 0;   break;
                //   case 2: r = 0;   g = 255; bv = t;   break;
                //   case 3: r = 0;   g = q;   bv = 255; break;
                //   case 4: r = t;   g = 0;   bv = 255; break;
                //   default:r = 255; g = 0;   bv = q;   break;
                // }
                // uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bv;
                hsvColor hsv = { hue, 180, 190 };
                uint32_t color = HsvToRaw( hsv );
                // At the start of each new sweep, clear all pairs to begin fresh fill
                if ( pairIdx == 0 ) {
                    for ( int p = 0; p < PAIR_COUNT; p++ ) {
                        setLogoOverride( holdLogoPairs[ p ][ 0 ], -3 );
                        setLogoOverride( holdLogoPairs[ p ][ 1 ], -3 );
                    }
                }

                // Light this pair (it stays on until next sweep clears)
                setLogoOverride( holdLogos[ pairIdx ], color );
                // setLogoOverride(holdLogoPairs[pairIdx][0], color);
                // setLogoOverride(holdLogoPairs[pairIdx][1], color);
                showLEDsCore2 = 2;

                holdAnimStep++;
                // After first 3 logo pairs (one full sweep of 6 steps) are lit, fire MEDIUM_HELD and flash
                if ( holdAnimStep == LOGO_COUNT ) {
                    encoderButtonState = MEDIUM_HELD;
                    lastEncoderButtonState = MEDIUM_HELD;
                    triggerFlash = 2;
                    flashStartTime = millis( );
                }
                // After second full sweep (12 steps), fire LONG_HELD and flash
                if ( holdAnimStep == HOLD_ANIM_CYCLES * LOGO_COUNT ) {
                    encoderButtonState = LONG_HELD;
                    lastEncoderButtonState = LONG_HELD;
                    triggerFlash = 3;
                    flashStartTime = millis( );
                    buttonEventTimestamp = micros( );
                }
            } else {
                // Animation finished → fire LONG_HELD
            }
        }
    }

    // ── Flash all red on LONG_HELD (once) ──
    if ( encoderButtonState == LONG_HELD && holdAnimActive && !holdAnimLongHeldFlashed ) {
        for ( int p = 0; p < PAIR_COUNT; p++ ) {
            setLogoOverride( holdLogoPairs[ p ][ 0 ], 0xaaaaaa );
            setLogoOverride( holdLogoPairs[ p ][ 1 ], 0xaaaaaa );
        }

        for ( int i = 0; i < 6; i++ ) {
            pressAnimLogoColors[ i ] = flashColors[ triggerFlash ];
        }
        pressAnimLogoColors[ 7 ] = PRESS_WHITE;

        showLEDsCore2 = 2;
        holdAnimLongHeldFlashed = true;
        if ( rebootFlag == -1 ) {
            rebootFlag = 1;
        }
        // rp2040.reboot( );
    }

    // ── Clean up when button is released ──
    if ( holdAnimActive && encoderIsPressed == 0 ) {
        for ( int p = 0; p < PAIR_COUNT; p++ ) {
            setLogoOverride( holdLogoPairs[ p ][ 0 ], -3 );
            setLogoOverride( holdLogoPairs[ p ][ 1 ], -3 );
        }
        showLEDsCore2 = 2;
        holdAnimActive = false;
        holdAnimLongHeldFlashed = false;
        holdAnimStep = 0;
        // Also clear the logo rainbow that was kept alive during HELD
        buttonPressAnimActive = false;
        pressAnimActive = false;
    }

    // Clean up press animation on actual release (RELEASED, IDLE, DOUBLECLICKED).
    // Don't clear during HELD/LONG_HELD — the logo rainbow stays visible there.
    if ( pressAnimActive && encoderButtonState != PRESSED && encoderButtonState != HELD && encoderButtonState != MEDIUM_HELD && encoderButtonState != LONG_HELD ) {
        buttonPressAnimActive = false;
        pressAnimActive = false;
        rebootFlag = -1;
    }

    // ── Show state-change flash last: set logo colors then showLEDsCore2 so nothing overwrites before Core 1 reads ──
    if ( triggerFlash >= 0 ) {
        for ( int i = 0; i < 6; i++ ) {
            pressAnimLogoColors[ i ] = flashColors[ triggerFlash ];
        }
        pressAnimLogoColors[ 7 ] = PRESS_WHITE;
        if ( millis( ) - flashStartTime >= flashDuration ) {
            triggerFlash = -1;
        }
        showLEDsCore2 = 2;
    }
}

// ── Single-core ownership ───────────────────────────────────────────────────
// ALL encoder state (PIO FIFO drain, hysteresis, interlock gates, button
// state machine) is mutated by Core 1 ONLY. The earlier scheme - Core 0
// menu/editor loops and Core 1's core2stuff() both calling the poll, first
// bare, then mutex+throttle serialized - still had two alternating writers
// over ~20 pieces of interlock/hysteresis/pacing state, and the alternation
// itself produced count glitches (skips, phantom reverse steps).
//
// Single-writer (per pico_sync guidance: prefer ownership over locking for
// hot paths): calls from Core 0 return immediately and the poll body needs
// no mutex at all. Consumers on either core keep reading the volatile
// event flags (encoderDirectionState / encoderButtonState) exactly as
// before - one-word reads/writes, handshake semantics unchanged.
//
// Cadence is guaranteed by an unconditional owner poll at the top of
// core2stuff() (the old in-branch poll sites starved in probe mode, which
// is why Core 0 used to poll directly). Internally throttled - the PIO
// counts in hardware, so sampling faster than 2kHz buys nothing.
static volatile uint32_t lastEncoderPollUs = 0;
static const uint32_t ENCODER_POLL_INTERVAL_US = 500; // 2kHz effective poll

static void rotaryEncoderStuffLocked( void );
static void rotaryEncoderButtonStuffLocked( void );

void rotaryEncoderStuff( void ) {

#if defined(OG_JUMPERLESS)
    // OG has no rotary encoder. The quadrature PIO program may also have failed
    // to load (the RP2040's 2 PIO blocks are oversubscribed - boot logs "no
    // instruction memory"), so reading pioEnc/smEnc here on core1 is both useless
    // and a fault risk. No-op on OG.
    return;
#endif

    // Single-owner: only Core 1 runs the poll body. (Core 0 call sites in
    // menus/probing/apps are left in place as no-ops - cheaper than
    // hunting them all down, and they document where polling used to be.)
    if ( ( sio_hw->cpuid & 1 ) == 0 ) {
        return;
    }

    // encoderServiceYield() made this reachable from any core-1 blocking wait
    // (sendXYraw's PIO handshake included), which can run before setup1's
    // initRotaryEncoder() or after a failed PIO allocation - don't touch the
    // quadrature SM until it exists.
    if ( !isRotaryEncoderInitialized( ) ) {
        return;
    }

    uint32_t now = micros( );
    if ( (uint32_t)( now - lastEncoderPollUs ) < ENCODER_POLL_INTERVAL_US ) {
        return;
    }
    lastEncoderPollUs = now;

    // Decrement AFTER the 2kHz throttle so encoderOverride counts throttled
    // polls (~500us each), the unit its users assume: Debugs' 100000 means
    // ~50s and MicroPython's 10 means ~5ms. Before this, every
    // encoderServiceYield() call decremented it - and the yield now runs from
    // kHz-rate spin loops (sendXYraw handshake, NetVoltageScan waits), which
    // drained a 100000-count suspension in well under a second.
    if ( encoderOverride > 0 ) {
        encoderOverride--;
        return;
    }

    rotaryEncoderStuffLocked( );
}

// Menu loops on Core 0 call this every iteration for click responsiveness.
// Under single-ownership it's a no-op there - Core 1's steady poll runs the
// button state machine, and the 10ms BUTTON_EVENT_MIN_DURATION_US hold is
// what guarantees Core 0 pollers still catch the PRESSED/RELEASED edges.
void rotaryEncoderButtonStuff( void ) {
    if ( ( sio_hw->cpuid & 1 ) == 0 ) {
        return;
    }
    rotaryEncoderButtonStuffLocked( );
}

// Re-arm a synthetic click event: the RELEASED + lastButtonEncoderState ==
// PRESSED edge every menu poller checks for. Lives here (not at the call
// site) because buttonEventTimestamp is static to this file - refreshing it
// gives the consumer the full BUTTON_EVENT_MIN_DURATION_US window instead of
// racing Core 1's auto-clear against a stale timestamp. Write order matters
// for the lock-free handoff to Core 1's poll: timestamp first (arms the
// hold window), then RELEASED (poll now freezes lastButtonEncoderState and
// won't auto-clear), then PRESSED into the frozen slot.
//
// Used to hand a click from one blocking Core 0 UI loop to another, e.g.
// probeMode handing its exit click to clickMenu(). The Menu FX tuner in
// Debugs.cpp predates this helper and pokes the two states directly.
void synthesizeEncoderClick( void ) {
    buttonEventTimestamp = micros( );
    encoderButtonState = RELEASED;
    lastButtonEncoderState = PRESSED;
}

static void rotaryEncoderStuffLocked( void ) {

#if !defined( OG_JUMPERLESS )
    // RP2350-E9 pad-latch mitigation. The quadrature pins rely on the
    // internal ~50k pull-ups; per erratum E9 a pulled-up input released from
    // ground can latch around ~2.1V (input-buffer leakage holds the pad
    // mid-rail and the weak pull-up never recovers it), so quadrature edges
    // silently vanish for hundreds of ms - unit-dependent silicon lottery.
    // Dropping the pad's input-enable for a couple of us removes the leakage
    // path so the pull-up wins, then re-enables. One pin at a time: a
    // single-pin blip decodes as one step out + one step back (net zero),
    // and the 4-8 count step hysteresis absorbs the +/-1 wiggle.
    // ponytail: the real fix is external pull-ups <=8.2k on GPIO 12/13
    // (board rev); this is the firmware bandaid.
    {
        static unsigned long lastUnlatchUs = 0;
        unsigned long nowUnlatchUs = micros( );
        if ( nowUnlatchUs - lastUnlatchUs > 5000 ) { // 200Hz; latch windows are 100ms+
            lastUnlatchUs = nowUnlatchUs;
            hw_clear_bits( &pads_bank0_hw->io[ QUADRATURE_A_PIN ], PADS_BANK0_GPIO0_IE_BITS );
            busy_wait_us_32( 2 );
            hw_set_bits( &pads_bank0_hw->io[ QUADRATURE_A_PIN ], PADS_BANK0_GPIO0_IE_BITS );
            busy_wait_us_32( 2 );
            hw_clear_bits( &pads_bank0_hw->io[ QUADRATURE_B_PIN ], PADS_BANK0_GPIO0_IE_BITS );
            busy_wait_us_32( 2 );
            hw_set_bits( &pads_bank0_hw->io[ QUADRATURE_B_PIN ], PADS_BANK0_GPIO0_IE_BITS );
        }
    }
#endif

    // Handle button state checking and updates
    rotaryEncoderButtonStuffLocked( );

    // Drive the hold animation + LONG_HELD transition
    holdAnimationStuff( );

    // if (resetPosition == true) {
    //  // quadrature_encoder_program_init(pioEnc, smEnc, PIN_AB, 0);
    //   //pio_sm_restart(pioEnc, smEnc);
    //   //pio_sm_clear_fifos(pioEnc, smEnc);
    //   //pio_sm_drain_tx_fifo(pioEnc, smEnc);

    //   positionOffset = quadrature_encoder_get_count(pioEnc, smEnc);
    //   positionOffset = positionOffset / rotaryDivider;
    //   Serial.print("\n\n\rencoderRaw: ");
    //   Serial.println(encoderRaw);
    //   Serial.print("positionOffset: ");
    //   Serial.println(positionOffset);

    //   // encoderRaw -= positionOffset;
    //   // encoderRaw = encoderRaw / rotaryDivider;
    //   //lastPositionEncoder = positionOffset/rotaryDivider;
    //   resetPosition = false;
    // }

    long rawCount = quadratureCountBounded( );

    encoderPosition = rawCount - encoderPositionOffset;
    if ( resetEncoderPosition == true ) {
        encoderPositionOffset = rawCount;
        encoderPosition = 0;
        resetEncoderPosition = false;
    }

    if ( lastRotaryDivider != rotaryDivider ) {
        // NOTE: no pio_sm_restart() here (removed) - the divider is purely
        // software bookkeeping (raw counts per logical step); the PIO just
        // counts and must never be restarted. Restart clears the program's
        // previous-pin-state (OSR), adding a phantom +/-1 whenever the
        // pins aren't at 00 - and since menus/probe/highlighting all set
        // different dividers, the constant restarts drifted the raw count
        // by thousands per session ("hops backwards while scrolling").
        lastRotaryDivider = rotaryDivider;
        // Changing the divider only changes sensitivity; reseed the hysteresis
        // baseline to the current raw count so the logical position doesn't jump.
        lastDetentRaw = rawCount;
        lastStepDir = 0; // direction continuity broken with the baseline
    }

    // ---- Click/rotation interlock --------------------------------------------
    unsigned long nowClickUs = micros( );

    // Rest / motion tracking for the first-detent confirm gate. When the shaft
    // first leaves a long rest, snapshot the logical state and arm the gate.
    if ( rawCount != lastRawForIdle ) {
        if ( ( nowClickUs - lastRawChangeUs ) >= ENCODER_REARM_IDLE_US && motionConfirmed ) {
            // Shaft was at rest: this is the first motion out of rest. prevEncoderPos
            // holds the at-rest encoderPosition (before this raw change applied).
            motionStartUs = nowClickUs;
            motionConfirmed = false;
            heldConfirmPos = prevEncoderPos;
            heldConfirmOffset = encoderPositionOffset;
            restMenuPosition = (long)position;
        }
        lastRawForIdle = rawCount;
        lastRawChangeUs = nowClickUs;
    }

    // Press edge: resolve the gate. If motion was still unconfirmed, the press is a
    // click on the wheel, not a turn — discard the pending (never-drawn) motion by
    // restoring the at-rest snapshot. Otherwise hold at the current committed value.
    bool pressedNow = isEncoderButtonPhysicallyPressed( );
    if ( pressedNow && !encoderWasPressedGate ) {
        if ( !motionConfirmed ) {
            heldEncoderPos = heldConfirmPos;
            position = (int)restMenuPosition;
        } else {
            heldEncoderPos = (long)encoderPosition;
        }
        encoderDirectionState = NONE;
        encoderDirectionConsumed = true;
        motionConfirmed = true; // gate resolved; nothing left pending
        // A click cancels any queued-but-unemitted detents too - replaying
        // them after the press would move a UI the user just clicked on.
        lastPositionEncoder = encoderRaw;
    }
    encoderWasPressedGate = pressedNow;

    // While the button is physically held, suppress all rotation outright. Keep the
    // AFTER window anchored to "now" through the hold so suppression also continues
    // for CLICK_SUPPRESS_AFTER_US past the release (covers release jiggle on long holds).
    if ( pressedNow ) {
        lastEncoderClickUs = nowClickUs;
    }

    // AFTER window: freeze both consumer paths so the click itself cannot register
    // as rotation. Baselines track the shaft so motion resumes without a jump.
    if ( ( nowClickUs - lastEncoderClickUs ) < CLICK_SUPPRESS_AFTER_US ) {
        encoderPositionOffset = rawCount - heldEncoderPos;
        encoderPosition = heldEncoderPos;
        lastDetentRaw = rawCount; // keep the hysteresis baseline under the shaft, no catch-up
        lastStepDir = 0;          // baseline moved - direction continuity broken
        if ( encoderDirectionConsumed ) encoderDirectionState = NONE;
        numberOfSteps = 0;
        prevEncoderPos = (long)encoderPosition;
        return;
    }

    // CONFIRM GATE: hold the first detent out of rest invisible until the window
    // elapses. The raw count keeps accumulating (we leave lastDetentRaw alone), but
    // encoderPosition / direction stay frozen so consumers draw nothing. On expiry
    // the accumulated delta flushes through the detent block below in one commit.
    if ( !motionConfirmed ) {
        if ( ( nowClickUs - motionStartUs ) < STEP_CONFIRM_US ) {
            encoderPositionOffset = rawCount - heldConfirmPos;
            encoderPosition = heldConfirmPos;
            if ( encoderDirectionConsumed ) encoderDirectionState = NONE;
            numberOfSteps = 0;
            prevEncoderPos = (long)encoderPosition;
            return;
        }
        motionConfirmed = true; // window elapsed with no click — commit the motion
        // Restore the arm-time offset so the motion accumulated during the
        // freeze flushes into encoderPosition in one go. The freeze loop
        // above rebases the offset every poll to pin encoderPosition; if the
        // rebase were left standing, the entire freeze window's travel would
        // be permanently swallowed from the encoderPosition stream. At
        // deliberate turning speeds (>150ms between detents) the gate
        // re-arms for EVERY detent, so each detent's whole travel got eaten
        // - the probe-mode connect cursor (encoderPosition consumer, unlike
        // the menus' detent-event path) skipped constantly.
        encoderPositionOffset = heldConfirmOffset;
        encoderPosition = rawCount - encoderPositionOffset;
    }
    // ---- end click/rotation interlock ----------------------------------------

    prevEncoderPos = (long)encoderPosition; // at-rest baseline for the next motion-start check

    // Hysteresis: only step once the shaft has moved a full rotaryDivider of raw
    // counts past the last committed detent. Each crossing is exactly one logical
    // step, so the divider sets sensitivity (counts per step) without placing any
    // fixed bin boundary the resting detent could chatter across.
    long detentDelta = rawCount - lastDetentRaw;

    // Backlash margin: the detent spring settling into a notch (or quadrature
    // chatter at certain speeds) can re-cross the trip point AGAINST the
    // direction of travel - the residual after a committed step can leave the
    // reverse trip point only one detent's worth of counts away. The old code
    // accidentally masked these phantom reversals by collapsing steps; the
    // paced emission below faithfully reports every step, so reversals must
    // clear an extra half-divider before they count ("hops a step backwards
    // while scrolling at particular speeds").
    long stepThreshold = (long)rotaryDivider;
    bool reverseDelta = ( lastStepDir > 0 && detentDelta < 0 ) ||
                        ( lastStepDir < 0 && detentDelta > 0 );
    if ( reverseDelta ) {
        stepThreshold += rotaryDivider / 2;
    }

    // Recoil guard (see banner above): during/just after a fast run of
    // steps, reverse travel is treated as detent-spring bounce no matter
    // how far it goes. Slide the baseline so the bounce never accumulates
    // more than a sub-step residual - resumed travel in the original
    // direction then registers after at most one detent, and a genuine
    // direction change commits as soon as the guard expires.
    if ( reverseDelta && (long)( recoilGuardUntilUs - nowClickUs ) > 0 ) {
        long cap = stepThreshold - 1;
        if ( detentDelta > cap ) {
            lastDetentRaw = rawCount - cap;
        } else if ( detentDelta < -cap ) {
            lastDetentRaw = rawCount + cap;
        }
        detentDelta = 0; // nothing commits this poll
    }

    int steps = 0;
    if ( detentDelta >= stepThreshold || detentDelta <= -stepThreshold ) {
        steps = (int)( detentDelta / rotaryDivider ); // truncates toward zero; |steps| >= 1 here

        // Glitch-storm clamp: more than 4 detents landing inside ONE 500us
        // poll window is not human rotation (a hard flick peaks well under 1
        // detent per poll) - it's a failing encoder or a noise burst on the
        // quadrature lines delivering a storm of ordered edges (seen on a
        // unit with dying encoder contacts: dead for 300ms, then 24 detents
        // in a few polls). Commit a single step in the storm's direction and
        // swallow the rest so the UI can't be raced through a dozen items.
        constexpr int kMaxStepsPerPoll = 4;
        if ( steps > kMaxStepsPerPoll || steps < -kMaxStepsPerPoll ) {
            steps = ( steps > 0 ) ? 1 : -1;
            lastDetentRaw = rawCount; // rebase under the shaft, no catch-up
        } else {
            lastDetentRaw += (long)steps * rotaryDivider;
        }

        int stepDir = ( steps > 0 ) ? 1 : -1;
        if ( lastStepDir != 0 && stepDir != lastStepDir ) {
            // Genuine reversal: any queued events from the old direction are
            // stale - drop the backlog so the first thing the UI sees is the
            // new direction, not leftover old-direction motion.
            lastPositionEncoder = encoderRaw;
        }
        lastStepDir = stepDir;
        encoderRaw += steps; // emission below drains one event per consumer ack

        // Arm the recoil guard only when steps are committing rapidly -
        // slow deliberate stepping (including quick one-detent jogs) never
        // arms it, so normal back-and-forth navigation is unaffected.
        if ( (unsigned long)( nowClickUs - lastCommitUs ) < RECOIL_ARM_GAP_US ) {
            recoilGuardUntilUs = nowClickUs + RECOIL_GUARD_US;
        }
        lastCommitUs = nowClickUs;
    }

    numberOfSteps = abs( steps );

    // ── Paced event emission ──
    // Emit ONE detent per direction event and hold the rest back until the
    // consumer acknowledges (sets encoderDirectionState back to NONE). The
    // old code snapped lastPositionEncoder all the way to encoderRaw on
    // every emission, so a multi-detent twist - or detents that accumulated
    // while the consumer was busy with an OLED/TUI redraw - collapsed into a
    // single step: the "encoder skips pulses / feels choppy" complaint.
    // Backlog now drains one event per consumer ack (the 500µs cross-core
    // poll throttle sets the max drain rate). If nothing consumes the event
    // (no UI listening, e.g. the main screen), it expires after
    // DIR_EVENT_ABANDON_US and the backlog is dropped with it so a later
    // consumer doesn't replay stale motion.
    if ( encoderDirectionConsumed && encoderDirectionState != NONE ) {
        encoderDirectionState = NONE; // consumer ack'd via the consumed flag
    }
    if ( encoderDirectionState == NONE ) {
        if ( lastPositionEncoder > encoderRaw ) {
            position++;
            encoderDirectionState = UP;
            encoderDirectionConsumed = false; // Mark as unconsumed
            noteUserInput( );  // gate background flash writes - user is turning
            lastPositionEncoder--;
            dirEventEmitUs = nowClickUs;
        } else if ( lastPositionEncoder < encoderRaw ) {
            position--;
            encoderDirectionState = DOWN;
            encoderDirectionConsumed = false; // Mark as unconsumed
            noteUserInput( );
            lastPositionEncoder++;
            dirEventEmitUs = nowClickUs;
        }
    } else if ( (unsigned long)( nowClickUs - dirEventEmitUs ) > DIR_EVENT_ABANDON_US ) {
        // Unconsumed for too long: no active consumer. Drop event + backlog.
        encoderDirectionState = NONE;
        encoderDirectionConsumed = true;
        lastPositionEncoder = encoderRaw;
    }

    // slotManager();

    // buttonState = digitalRead(BUTTON_ENC);
    //  if (millis() - buttonHoldStart > buttonHoldLength && buttonState == 0 )
    //  {
    //      //Serial.println("held\n\r");
    //      //refreshSavedColors();

    //     slotChanged = 1;
    //     netSlot = slotPreview;
    // }

    if ( debugEncoder == 1 ) {
        if ( encoderButtonState != lastButtonEncoderState ) {
            switch ( encoderButtonState ) {
            case IDLE:
                // Serial.print("IDLE");
                break;
            case PRESSED:
                Serial.print( lastButtonEncoderState );
                Serial.println( " PRESSED" );
                // delay(150);
                break;
            case HELD:

                Serial.println( "HELD" );
                // delay(150);
                break;
            case LONG_HELD:

                Serial.println( "LONG_HELD" );
                // delay(150);
                break;
            case RELEASED:

                Serial.println( "RELEASED" );
                // delay(150);
                break;

            case DOUBLECLICKED:
                Serial.println( "DOUBLECLICKED" );
                /// delay(150);
                break;

            default:
                break;
            }
        }
        if ( encoderDirectionState != NONE || encoderButtonState != IDLE ) {
            switch ( encoderDirectionState ) {
            case NONE:

                // Serial.print("NONE");
                break;
            case UP:
                Serial.print( position );
                Serial.println( "  UP" );

                // delay(150);
                break;
            case DOWN:
                Serial.print( position );
                Serial.println( "  DOWN" );
                // delay(150);
                break;
            default:
                break;
            }
        }
    }
}
