#ifndef ROTARYENCODER_H
#define ROTARYENCODER_H

#include <Arduino.h>  // For millis() and abs()

#define NUM_SLOTS 8

extern volatile int rotaryEncoderMode;
extern int netSlot;
extern volatile int slotChanged;

extern volatile int slotPreview;
extern int rotState;
extern int encoderIsPressed;
extern int showingPreview;

extern int rotaryDivider;
extern int encoderRaw;
extern volatile int numberOfSteps;
extern volatile bool resetPosition;

extern volatile int encoderOverride;

extern volatile long encoderPosition;
extern long encoderPositionOffset;
extern bool resetEncoderPosition;
enum encoderDirectionStates { NONE,UP,DOWN };

/**
 * @brief Encoder button state machine
 * 
 * MULTI-CORE ARCHITECTURE:
 * - Core 2: Runs rotaryEncoderButtonStuff() continuously at high speed (~10μs)
 * - Core 1: Polls encoderButtonState in menu loops (~1000μs intervals)
 * 
 * TIMING PROTECTION (Critical for multi-core reliability):
 * - RELEASED and DOUBLECLICKED states persist for minimum 15ms
 * - lastButtonEncoderState is FROZEN during the 15ms hold period
 * - This preserves the transition that Core 1 checks: (RELEASED && PRESSED)
 * - Without freezing, Core 2 would update lastButtonEncoderState to RELEASED,
 *   breaking the detection pattern before Core 1 could read it
 * - 15ms is long enough for Core 1 critical services to catch events even when busy
 * 
 * DETECTION PATTERN:
 * Core 1 code checks: (encoderButtonState == RELEASED && lastButtonEncoderState == PRESSED)
 * Both values must remain stable for Core 1 to detect the button press.
 * 
 * USAGE PATTERN:
 * After detecting events, consuming code MUST set encoderButtonState = IDLE
 * to acknowledge. This provides immediate feedback; otherwise state auto-clears after 2ms.
 * 
 * State transitions:
 * - IDLE → PRESSED (on button press)
 * - PRESSED → HELD (if held long enough)
 * - PRESSED → RELEASED (on release, holds 15ms with lastButtonEncoderState frozen)
 * - RELEASED → IDLE (manual clear by Core 1, or auto after 15ms)
 * - IDLE → DOUBLECLICKED (on quick double press, holds 15ms with freeze)
 * - DOUBLECLICKED → IDLE (manual clear by Core 1, or auto after 15ms)
 */
enum encoderButtonStates { IDLE, PRESSED, HELD, RELEASED, DOUBLECLICKED, LONG_HELD, MEDIUM_HELD};

extern volatile encoderDirectionStates encoderDirectionState;
extern volatile encoderButtonStates encoderButtonState;
extern volatile encoderButtonStates lastButtonEncoderState;
extern volatile encoderDirectionStates lastDirectionState;
extern volatile bool encoderDirectionConsumed;

extern volatile bool buttonPressAnimActive;
extern volatile uint32_t pressAnimLogoColors[8];

// Press timing — buttonHoldStart is set to millis() at press start; the state
// machine flips PRESSED -> HELD once (millis()-buttonHoldStart) > buttonHoldLength.
extern unsigned long buttonHoldStart;
extern unsigned long buttonHoldLength;

void initRotaryEncoder(void);
void unInitRotaryEncoder(void);
void printRotaryEncoderHelp(void);
void rotaryEncoderStuff(void);
void rotaryEncoderButtonStuff(void);

// Core-1 yield contract: call this from ANY blocking wait/spin on core 1
// that can exceed ~100us (PIO handshakes, ADC-lock spins, settle delays) so
// the single-owner encoder poll never starves. It's a no-op on core 0 and
// internally throttled to 2kHz, so calling it every spin iteration is free.
// (Thin alias of rotaryEncoderStuff() - the name documents the intent.)
static inline void encoderServiceYield(void) { rotaryEncoderStuff(); }

// Re-arm a synthetic click event (RELEASED with lastButtonEncoderState =
// PRESSED and a fresh event timestamp) so a blocking UI loop can hand a
// click it detected to another poller, e.g. probeMode -> clickMenu().
void synthesizeEncoderClick(void);
bool isRotaryEncoderInitialized(void);
void printRotaryEncoderStatus(void);

// C7 verification rig (task #30): the CPU-decoded quadrature count runs in
// parallel with the legacy PIO counter; this flag picks which one feeds the
// session (debug menu "Encoder A/B", or poke it over SWD). X prints the
// drift between the two counts; X! resets the drift stats.
extern volatile int encoderUseCpuDecode;
bool encSamplerActive(void);
void encoderDriftReset(void);
void printEncoderC7Line(Stream& target);

/**
 * @brief Read the raw quadrature count directly from the PIO.
 *
 * Bypasses the encoderPosition/offset/hysteresis bookkeeping that Core 2
 * maintains in rotaryEncoderStuff(). Intended for screens that suspend the
 * normal encoder polling (encoderOverride) and want to track rotation
 * themselves while taking over the button pin. Returns 0 if the encoder PIO
 * is not initialized.
 */
long getEncoderRawCount(void);

/**
 * @brief Check if encoder button is physically pressed (hardware state)
 * @return true if button is currently pressed down, false otherwise
 * @note This reads the actual GPIO state, independent of the state machine
 */
bool isEncoderButtonPhysicallyPressed(void);

// ── Generalized click/hold classifier ──────────────────────────────────────
//
// The shared encoderButtonState machine is cross-core and gets re-armed by
// various consumers, which makes "did the user click or hold?" ambiguous for
// any one UI. EncoderClickTracker is a small per-UI classifier that watches
// the PHYSICAL pin and turns it into unambiguous one-shot events:
//
//   ENC_PRESS        button just went down. Fast UIs (editors, char pickers)
//                    fire their primary action here for instant feedback —
//                    but then they must treat a following HOLD/LONG_HOLD as
//                    a separate gesture.
//   ENC_CLICK        released BEFORE the hold threshold. Slow UIs (file
//                    manager, anything where a hold means "back/exit") fire
//                    here instead of on PRESS, so a hold never triggers the
//                    click action on its way to becoming a hold.
//   ENC_HOLD         hold threshold crossed, button still down (fires once).
//   ENC_LONG_HOLD    long-hold threshold crossed, still down (fires once).
//                    Convention: universal quit, like Ctrl-Q.
//   ENC_HOLD_RELEASE released after ENC_HOLD / ENC_LONG_HOLD fired (i.e.
//                    this release is NOT a click).
//
// Each UI owns its own instance (no shared static state to fight over) and
// calls poll() once per loop iteration. Call reset() on UI entry so a button
// already held during the transition isn't misread as a fresh press.
enum EncoderClickEvent {
    ENC_NONE = 0,
    ENC_PRESS,
    ENC_CLICK,
    ENC_HOLD,
    ENC_LONG_HOLD,
    ENC_HOLD_RELEASE,
};

struct EncoderClickTracker {
    unsigned long holdMs = 500;       // ENC_HOLD threshold
    unsigned long longHoldMs = 1500;  // ENC_LONG_HOLD threshold
    unsigned long debounceMs = 30;    // edge debounce

    void reset(void);                 // sync to current pin state, clear flags
    EncoderClickEvent poll(void);     // call every loop; returns one-shot events
    bool isDown(void) const { return wasDown; }
    unsigned long heldForMs(void) const; // 0 when not pressed

  private:
    bool wasDown = false;
    bool holdFired = false;
    bool longFired = false;
    unsigned long downStartMs = 0;
    unsigned long lastEdgeMs = 0;
};

/**
 * @brief Helper class for rotary encoder acceleration
 * 
 * Provides smooth acceleration for fast scrolling while maintaining precision
 * at slow speeds. Works for both integer and character selection.
 * 
 * Configurable parameters allow tuning the acceleration feel for different use cases.
 */
class EncoderAccelerator {
private:
    float accelerationMultiplier; // Current acceleration multiplier
    int lastDirection = 0;  // -1=down, 0=none, 1=up
    int consecutiveFastCount = 0;
    unsigned long lastChangeTime = 0;
    
    // Configurable acceleration parameters
    float baseSpeed;        // Starting multiplier (e.g., 0.1 for very slow)
    float maxSpeed;         // Maximum multiplier (e.g., 50.0 for fast)
    float rampRate;         // How fast to accelerate (e.g., 12.0)
    float decayRate;        // Decay when slowing down (e.g., 0.9)
    int fastThreshold;      // Delta magnitude for "fast" rotation (e.g., 4)
    int timeoutMs;          // Reset timeout in milliseconds (e.g., 120)
    
public:
    /**
     * @brief Construct accelerator with custom parameters
     * 
     * @param baseSpeed Starting multiplier (default 0.1 = very slow/precise)
     * @param maxSpeed Maximum multiplier (default 50.0 = very fast)
     * @param rampRate Acceleration rate (default 12.0 = aggressive)
     * @param decayRate Deceleration rate (default 0.9 = slow decay)
     * @param fastThreshold Delta for "fast" rotation (default 4)
     * @param timeoutMs Reset timeout in ms (default 120)
     */
    EncoderAccelerator(
        float baseSpeed = 0.1f,
        float maxSpeed = 50.0f,
        float rampRate = 12.0f,
        float decayRate = 0.9f,
        int fastThreshold = 4,
        int timeoutMs = 120
    ) : baseSpeed(baseSpeed),
        maxSpeed(maxSpeed),
        rampRate(rampRate),
        decayRate(decayRate),
        fastThreshold(fastThreshold),
        timeoutMs(timeoutMs)
    {
        reset();
    }
    
    void reset() {
        accelerationMultiplier = baseSpeed;
        lastDirection = 0;
        consecutiveFastCount = 0;
        lastChangeTime = millis();
    }
    
    /**
     * @brief Calculate accelerated delta value
     * 
     * @param encoderDelta Raw encoder change
     * @return Accelerated delta (fractional for smooth movement)
     */
    float getAcceleratedDelta(long encoderDelta) {
        if (encoderDelta == 0) {
            return 0.0f;
        }
        
        // Determine current direction
        int currentDirection = (encoderDelta > 0) ? 1 : ((encoderDelta < 0) ? -1 : 0);
        
        // Reset acceleration if direction changed
        if (currentDirection != 0 && currentDirection != lastDirection) {
            accelerationMultiplier = baseSpeed;
            consecutiveFastCount = 0;
            lastDirection = currentDirection;
        }
        
        // Calculate acceleration based on delta magnitude and timing
        unsigned long currentTime = millis();
        unsigned long timeSinceLastChange = currentTime - lastChangeTime;
        int deltaMagnitude = abs(encoderDelta);
        
        // Fast rotation = large delta between polls
        bool isFastRotation = (deltaMagnitude >= fastThreshold);
        
        if (isFastRotation) {
            consecutiveFastCount++;
            
            // Accelerate aggressively for fast movements
            if (consecutiveFastCount >= 0) {
                accelerationMultiplier += rampRate;
                if (accelerationMultiplier > maxSpeed) {
                    accelerationMultiplier = maxSpeed;
                }
            }
        } else if (timeSinceLastChange > timeoutMs) {
            // Slow/stopped - reset to base speed
            accelerationMultiplier = baseSpeed;
            consecutiveFastCount = 0;
            lastDirection = 0;
        } else {
            // Medium speed - maintain fractional acceleration
            consecutiveFastCount = 0;
            if (accelerationMultiplier > 1.0f) {
                accelerationMultiplier *= decayRate;
            }
        }
        
        lastChangeTime = currentTime;
        
        // Calculate fractional change - allows sub-unit movements
        return encoderDelta * accelerationMultiplier;
    }
    
    /**
     * @brief Get current acceleration multiplier (for debugging/display)
     */
    float getMultiplier() const {
        return accelerationMultiplier;
    }
    
    // Preset configurations for common use cases
    
    /**
     * @brief Fast preset - for large ranges (e.g., 0-2048)
     * Very aggressive acceleration for quick scrolling
     */
    static EncoderAccelerator Fast() {
        return EncoderAccelerator(0.1f, 50.0f, 12.0f, 0.9f, 4, 120);
    }
    
    /**
     * @brief Medium preset - for moderate ranges or character selection
     * Balanced acceleration with good precision
     */
    static EncoderAccelerator Medium() {
        return EncoderAccelerator(0.1f, 0.3f, 0.1f, 10.85f, 4, 50);
    }
    
    /**
     * @brief Slow preset - for precise selection (e.g., 0-100)
     * Gentle acceleration with maximum precision
     *
     * baseSpeed 0.15 ~= one step per physical detent (8 raw counts per
     * detent) with a touch of extra travel on multi-detent turns; maxSpeed
     * 0.65 lets fast spins cover ~5 nodes per detent. Careful single
     * detents still move exactly one step.
     */
    static EncoderAccelerator Slow() {
        return EncoderAccelerator(0.15f, 0.65f, 0.15f, 10.8f, 5, 170);
    }

    /**
     * @brief Subtle preset - for subtle acceleration for menu selection
     * 
     */
    static EncoderAccelerator Subtle() {
        return EncoderAccelerator(0.01f, 1.0f, 0.3f, 10.8f, 6, 170);
    }   
};









#endif