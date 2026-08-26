
// SPDX-License-Identifier: MIT
#ifndef PROBING_H
#define PROBING_H
#include "JumperlessDefines.h"
#include "JumperlOS.h"



// Forward declarations
class EncoderAccelerator;

// Window for the connect+disconnect double-tap that fires undo/redo.
// Shared by ProbeButton::service (which detects the gesture) and
// Probing::probeMode (which bails out so the second tap doesn't repaint
// the OLED with "connect"/"clear" on top of the undo toast).
namespace ProbingDoubleTap {
    constexpr uint32_t kWindowMs = 420;
}

// Set by ProbeButton::service the moment a double-tap fires undoUndo /
// undoRedo. probeMode checks (and clears) it on entry to short-circuit
// before any banner prints, then again inside the toggle branch as a
// safety net for cases where the second tap arrives slightly after
// probeMode has already started.
extern volatile bool g_probeDoubleTapBail;

// ----------------------------------------------------------------------------
// Probe button hardware-read diagnostics (driven from the debug menu).
// All defined in Probing.cpp.
//
// probe_button_trace: when nonzero, every checkProbeButtonHardware() call
//                     prints a one-line trace (path used, raw samples,
//                     decoded state, elapsed microseconds) to Serial.
// probeButtonPIOReadCount / probeButtonCPUReadCount:
//                     monotonic counters of which path served each read.
//                     Read these to verify the runtime use_pio_probe_button
//                     toggle is actually taking effect.
// probeButtonPIOTimeoutCount:
//                     PIO timeouts (would-be reads that fell back to 0).
//                     Nonzero indicates something is starving the PIO SM.
// probeButtonPIOLastResult:
//                     Last raw 2-bit sample from the PIO program. Bit 0 =
//                     drive-low/release sample, bit 1 = drive-high/release.
// probeButtonPIOLastUs / probeButtonCPULastUs:
//                     elapsed microseconds for the most recent read on each
//                     path. Useful for confirming PIO is the fast one.
// ----------------------------------------------------------------------------
// Diagnostics for the last probe_current_zero calibration (X prints them):
// the zero swings boot to boot (open item 2 of PROBE_REWORK_HANDOFF.md, and
// the 2.4 mA boot of 2026-08-17 that made the legacy classifier oscillate),
// so record what the calibration actually saw. Defined in Probing.cpp.
struct ProbeZeroDiag {
    float zero_mA;               // what was stored
    float sampleMin_mA, sampleMax_mA;
    int goodSamples;
    uint32_t ledOffAckMs;        // ms until core 1 consumed the LED-off request (100 = timed out)
    bool xbarIdleBeforeSampling; // the DAC0 disconnect had landed (mailbox idle) before sampling
    uint32_t atMs;               // millis() when it ran
    uint32_t runs;               // calibrations since boot
};
extern ProbeZeroDiag probeZeroDiag;

extern volatile int      probe_button_trace;
extern volatile uint32_t probeButtonPIOReadCount;
// 0 = PIO button reader not tried yet, 1 = in use, 2 = unavailable (CPU polling).
int probeButtonPioState( void );
extern volatile uint32_t probeLedShowCount;     // WS2811 frames sent to the probe LED
extern volatile uint32_t probeLedRequestCount;  // colour-change requests consumed
extern volatile uint32_t probeButtonCPUReadCount;
extern volatile uint32_t probeButtonPIOTimeoutCount;
extern volatile uint32_t probeButtonPIOLastResult;
extern volatile uint32_t probeButtonPIOLastUs;
extern volatile uint32_t probeButtonCPULastUs;

/**
 * @brief High-frequency probe button service (implemented in Probing.cpp)
 * 
 * Runs with CRITICAL priority to catch all button presses instantly.
 * Other code reads the cached state via probeButton.getButtonState()
 * 
 * New blocking behavior:
 * - When a press is detected, blocks subsequent presses for blockDurationMs (default 1 second)
 * - Block clears immediately if button is released OR timer expires
 * - Quick successive clicks register individually (each release clears block)
 * - Holding button down registers only once (block prevents re-triggering)
 * - Tracks continuous hold time and sets CONNECT_HELD/REMOVE_HELD flags
 */
class ProbeButton : public Service {
public:
    static ProbeButton& getInstance();
    ProbeButton(const ProbeButton&) = delete;
    ProbeButton& operator=(const ProbeButton&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "ProbeButton"; }
    ServicePriority getPriority() const override { return ServicePriority::CRITICAL; }
    
    // BUTTON NUMBERING - read this before using the values below.
    //
    // These three return the RAW, PRE-SWAP button code, and which physical
    // button a raw 1 or 2 means DEPENDS ON hardware.probe_revision: the
    // sample decoders (checkProbeButtonHardware and the PIO handler in
    // Probing.cpp) map an all-high sample to 2 on revision >= 4 and to 1 on
    // revision <= 3, and the reverse for all-low. The rev-4 probe swapped
    // the two switches.
    //
    // The USER-FACING convention - what every caller outside this class
    // should speak - is fixed and revision-independent:
    //     0 = none, 1 = CONNECT (front), 2 = REMOVE (rear)
    // Callers reach it by applying the compensating swap when
    // probe_revision > 3. The two implementations of that swap are
    // jl_probe_button_* (JumperlessMicroPythonAPI.cpp) and guideProbeButton
    // (partsProbeButton, PartsApp.cpp); copy one of them, don't invent a third rule.
    //
    //@brief Get the current button state (raw, see the note above)
    //@return 0 = neither pressed, 1/2 = the two buttons, revision-dependent
    int getButtonState() const { return currentButtonState; }
    //@brief Get the current button press (raw, see the note above)
    //@return 0 = no press, 1/2 = the two buttons, revision-dependent
    int getButtonPress(bool consume = true);
    //@brief Check the probe button hardware (raw, see the note above)
    //@return 0 = neither pressed, 1/2 = the two buttons, revision-dependent
    int checkProbeButtonHardware(void);

    // Run the press/release/double-tap state machine against a freshly
    // decoded sample. Factored out of service() so the PIO IRQ handler
    // can drive it directly from the polling state machine, decoupling
    // event detection from the main loop's variable cadence.
    //
    // IRQ-safe: only updates ProbeButton fields and posts pending-undo/
    // pending-redo flags (consumed by service() in main context). Does
    // NOT call undoUndo/undoRedo/undoToast/Serial.printf directly.
    void processSample(uint8_t newState);
    
    // Clear the current button state (e.g., when entering probeMode)
    // NOTE: Does NOT clear isBlocked - the block must remain active to prevent re-triggering!
    // Hold tracking accessors. No consumer wires these up at the moment;
    // kept available so future gestures (history scrub, modifier-on-hold,
    // etc.) don't have to re-add the bookkeeping.
    bool isConnectHeld() const { return connectHeld; }
    bool isRemoveHeld() const { return removeHeld; }
    unsigned long getConnectHoldDuration() const { return connectHoldTime; }
    unsigned long getRemoveHoldDuration() const { return removeHoldTime; }

    // Zero the double-tap press history + any pending second-tap
    // confirmation (defined in Probing.cpp - the state lives at file scope
    // there). Kevin's rule: the double-click time gets CLEARED at every
    // explicit state clear, so a stale first-tap timestamp can never pair
    // a later single click into a phantom undo.
    void clearDoubleTapState(void);

    // Clear the press/event state but PRESERVE the double-tap pairing
    // history. This is the probe-mode ENTRY clear: at idle, tap 1 both
    // enters probe mode (consumed at block expiry, +200ms) and may be the
    // first tap of a double-tap-undo whose window is 420ms - probeMode's
    // banner deferral + entry-window bail are BUILT on tap 2 pairing with
    // tap 1 across the entry boundary. A full clearButtonState() here wiped
    // the stamp, so any double slower than ~200ms press-to-press silently
    // missed (measured: fired gaps <=193ms, missed 220-396ms - Kevin's
    // "inconsistent"). Stale-stamp phantoms are guarded separately now: the
    // 420ms window, the kDblConfirmSamples gate and the consumed-hold
    // suppression each stand on their own.
    void clearButtonStateKeepDoubleTap() {
        // If the physical button is still held when the clear lands, the
        // very next sample re-latches the held button as a fresh 0->N press
        // edge - a ghost second click from the SAME physical hold. (Seen
        // live: probe-mode entry consumes the press at block expiry, i.e.
        // 200ms into any click held longer than that; the entry clear then
        // ghost-registered the still-held button and the deferred press
        // exited the session ~0.5s later.) Swallow press registration until
        // a debounce-confirmed release - this hold has been consumed.
        if (currentButtonState > 0) suppressPressUntilRelease = true;
        currentButtonState = 0;
        buttonPress = 0;
        buttonChanged = false;
        connectHeld = false;
        removeHeld = false;
        connectHoldTime = 0;
        removeHoldTime = 0;
        pressStartTime = 0;
        // Treat an explicit clear as a confirmed release so the next genuine
        // press registers cleanly (the block, intentionally, still stands) -
        // except while the same physical hold is still on the button.
        releaseConfirmed = !suppressPressUntilRelease;
        // isBlocked and blockStartTime are NOT cleared - block must stay active!
    }

    void clearButtonState() {
        clearDoubleTapState();
        clearButtonStateKeepDoubleTap();
    }

    // Adjustable timing parameters (milliseconds)
    //
    // Why 4ms (was 11ms)?  The probe button line is multiplexed with the
    // WS2812 data line, so each sample takes the line away from the PIO
    // for ~75us. With the old 11ms cadence and a 300ms double-tap window
    // we only got ~27 samples to catch one press, one release, and the
    // next press. If any of those three samples got eaten by a concurrent
    // LED show, the rising edge for the second click was missed and the
    // double-tap never fired. 4ms gives ~75 samples per window and still
    // keeps the average bus time below 2% of CPU.
    unsigned long checkIntervalMsSelect = 4;           // Rate limiting between hardware checks
    unsigned long checkIntervalMsMeasure = 4;          // Rate limiting between hardware checks
    unsigned long blockDurationMs = 200;        // Block duration after press detected
    unsigned long minimumBlockMs = 20;          // Minimum block time before release can clear (debounce)
    // Sustained-float release debounce. A release (newState==0) must persist
    // for this long before we (a) clear the press-block and (b) mark the
    // release "confirmed" so the NEXT press is allowed to register and feed
    // the double-tap history. This rejects the brief mid-press float glitches
    // that otherwise turn one physical press into two presses (and a stray
    // undo). 30ms is well under any human inter-tap gap, so genuine fast
    // double-taps (ProbingDoubleTap::kWindowMs apart) still register.
    unsigned long releaseDebounceMs = 30;
    unsigned long connectHoldThresholdMs = 800;        // Threshold to set connectHeld
    unsigned long removeHoldThresholdMs = 1000;        // Threshold to set removeHeld

    // Public state (for inline access)
    int currentButtonState = 0;   // 0=released, 1=remove, 2=connect
    int lastButtonState = 0;
    bool buttonChanged = false;
    int buttonPress = 0;          // Consumed by getButtonPress()

    // Hold state (latched flags + continuous duration counters). Updated
    // by ProbeButton::service every tick; cleared on release.
    bool connectHeld = false;
    bool removeHeld = false;
    unsigned long connectHoldTime = 0;
    unsigned long removeHoldTime = 0;
    
private:
    ProbeButton();
    ~ProbeButton() = default;
    unsigned long lastCheckTime = 0;
    bool isBlocked = false;
    unsigned long blockStartTime = 0;
    unsigned long pressStartTime = 0;
    // First time we saw newState==0 after a press transition. Used by
    // the release-bounce filter so it gates on sustained-released time
    // instead of time-since-press (which would block fast double-taps).
    unsigned long releaseStartTime = 0;
    // True once a release has been observed continuously for
    // releaseDebounceMs. Gates press registration + the double-tap history
    // so a transient mid-press float (which momentarily reads as released)
    // can't be (mis)counted as a fresh press. Starts true so the first
    // press after boot registers.
    bool releaseConfirmed = true;
    // Set by clearButtonState() when the clear lands while the button is
    // physically held: the next samples re-latch the held button as a fresh
    // press edge, and without this flag that ghost would REGISTER as a new
    // click (the entry clear also marks releaseConfirmed, making the ghost
    // double-tap-eligible). Cleared where releaseConfirmed is set - a
    // debounce-confirmed release ends the consumed hold.
    bool suppressPressUntilRelease = false;
};

enum probePressType {
  connectPress = 2,
  disconnectPress = 1,
  connectLongPress = 4,
  disconnectLongPress = 3,
  doubleClickConnect = 5,
  doubleClickDisconnect = 6,
  noPress = 0
};

enum measuredState
{
  floating = 2,
  high = 1,
  low = 0,
  probe = 3,
  unknownState = 4 
};

/**
 * @brief Probing switch service - handles probe switch position (NORMAL priority)
 * 
 * Checks the probe switch position (self-gated to interval_ms inside
 * checkSwitchPosition()) and runs infraServiceTick() on every call.
 */
class ProbeSwitch : public Service {
public:
    static ProbeSwitch& getInstance();
    ProbeSwitch(const ProbeSwitch&) = delete;
    ProbeSwitch& operator=(const ProbeSwitch&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "ProbeSwitch"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    // 10 ms, NOT interval_ms: checkSwitchPosition() has early returns before
    // its 500 ms gate (checkingButton, LED settle) that expect to retry on the
    // next pass, and infraServiceTick() runs on every call with no gate of
    // its own (its comment says "~500 ms"; the calls were every 3rd pass).
    uint32_t periodUs() const override { return 10000; }
    unsigned long interval_ms = 500; // switchPositionCheckInterval;
    
private:
    ProbeSwitch() = default;
    ~ProbeSwitch() = default;
};

/**
 * @brief Probe pads service - handles expensive ADC pad reading (LOW priority)
 * 
 * Reads probe pads via multiple ADC samples.
 * This is EXPENSIVE (multiple ADC reads) and not time-critical.
 */
class ProbePads : public Service {
public:
    static ProbePads& getInstance();
    ProbePads(const ProbePads&) = delete;
    ProbePads& operator=(const ProbePads&) = delete;
    
    ServiceStatus service() override;
    const char* getName() const override { return "ProbePads"; }
    ServicePriority getPriority() const override { return ServicePriority::LOW; }
    // Encodes service()'s own 50 ms (20 Hz) gate, which stays in place.
    uint32_t periodUs() const override { return 50000; }

private:
    ProbePads() = default;
    ~ProbePads() = default;
    unsigned long lastCheckTime = 0;
};

/**
 * @brief Probing system service - handles probe reading and actions
 * 
 * Reads probe position and handles probe button actions.
 * Runs at HIGH priority for responsive probe interaction.
 */
class Probing : public Service {
public:
    // Get singleton instance
    static Probing& getInstance();
    
    // Prevent copying
    Probing(const Probing&) = delete;
    Probing& operator=(const Probing&) = delete;
    
    // Service interface
    ServiceStatus service() override;
    const char* getName() const override { return "Probing"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    
    // Member variables (previously globals)
    volatile int sfProbeMenu = 0;
    unsigned long probingTimer = 0;
    
    int probePin = 10;
    int buttonPin = 9;
    
    volatile unsigned long blockProbing = 0;
    volatile unsigned long blockProbingTimer = 0;
    
    volatile unsigned long blockProbeButton = 0;
    volatile unsigned long blockProbeButtonTimer = 0;
    
    volatile int connectOrClearProbe = 0;
    volatile int node1or2 = 0;
    int probeHighlight = 0;
    int logoTopSetting[2] = {0, 0};
    int logoBottomSetting[2] = {0, 0};
    int buildingTopSetting[2] = {0, 0};
    int buildingBottomSetting[2] = {0, 0};
    int showProbeCurrent = 0;
    
    int probePowerDAC = 0;

    volatile int removeFade = 0;
    
    int debugProbing = 0;
    
    volatile int showingProbeLEDs = 0;
    // volatile: written by the switch classifier on core 0, read by core 1's
    // probeLEDhandler / LED code and by every probe path (2026-08-16).
    volatile int switchPosition = -1; // -1 = unknown, 0 = measure, 1 = select
    int lastSwitchPositions[3] = {0, 0, 0};
    
    int probeRowMapByPad[108];
    int probeRowMap[108];
    
    volatile int lastProbeLEDs = 0;   // written by core 1's probeLEDhandler, read on core 0
    int lastProbeButton = 0;
    
    volatile int inPadMenu = 0;
    volatile int checkingButton = 0;
    
    // Track last probe reading for other services
    int getLastProbeReading() const { return lastProbeReading; }

    // Simulated probe tap (the MicroPython jl_probe_tap hook): while the hold
    // lasts, the CACHED reading above reports `node` whenever the real ladder
    // reads nothing, so MeasureMode and the probe highlighter latch exactly as
    // they would for a held tip. Only the cache is faked - readProbeRaw() and
    // probeMode() are untouched, so it cannot fake button presses or connect
    // mode, and a genuine tap always wins over the simulation.
    // node <= 0 cancels an active hold.
    void simulateProbeTap(int node, unsigned long holdMs);
    
    // Check if we need to trigger a goto (for backward compatibility during transition)
    bool needsProbeConnect() const { return triggerProbeConnect; }
    bool needsProbeClear() const { return triggerProbeClear; }
    void clearTriggers() { triggerProbeConnect = false; triggerProbeClear = false; }
    char getPendingInput() const { return pendingInput; }
    void clearPendingInput() { pendingInput = '\0'; }
    
    // Public methods (existing function interfaces)
    // fromClickMenu: true when the click menu launched this session. When
    // false (probe-button entry), a short wheel click with no encoder
    // cursor showing exits probing and opens the click menu.
    int probeMode(int setOrClear = 1, int firstConnection = -1, bool fromClickMenu = false);

    // T3.1 M1 (B5): probeMode() runs as a tick-based state machine. One
    // ProbeSession (defined in Probing.cpp) holds a session's loop-carried
    // state; probeTick() is exactly one pass of the old while body;
    // probeExitTail() is the old post-loop cleanup. probeMode() is now a
    // thin wrapper: begin -> tick until done -> exit tail. The pump
    // (serviceInner) still lives inside the PROBE_RUN tick - M3 moves it out.
    struct ProbeSession;
    void probeSessionBegin(ProbeSession& s, int setOrClear, int firstConnection, bool fromClickMenu);
    void probeTick(ProbeSession& s);
    int probeExitTail(ProbeSession& s);
    void probeEmitBanner(ProbeSession& s);
    void checkPads(void);
    int delayWithButton(int delayTime = 1000);
    
    int chooseGPIO(int skipInputOutput = 0);
    int chooseGPIOinputOutput(int gpioChosen);
    int chooseADC(void);
    int chooseDAC(int justPickOne = 0);
    int chooseIsense(void);
    int attachPadsToSettings(int pad);
    
    float voltageSelect(int fiveOrEight = 8);
    int longShortPress(int pressLength = 500);
    
    int selectFromLastFound(void);
    int checkLastFound(int);
    void clearLastFound(void);
    
    int checkProbeButton(void);  // Event-based: consumes button press event (use for one-shot detection)
    int checkProbeButtonState(void);  // State-based: reads current hardware state (use in loops)
    int checkProbeDoubleClick(unsigned long timeout, int waitForRelease = 0);
    int readFloatingOrState(int pin = 0, int row = 0);
    
    // infraServiceTick() + classifySwitchPosition(): what the ProbeSwitch
    // service (and the calibration app / MicroPython) call.
    int checkSwitchPosition(void);
    // The classifier alone (500 ms self-gated): also called from probeMode()'s
    // loop with inSession = true, where the infra tick must not run and the
    // position may only change on agreement of both detectors (see the
    // definition for why).
    int classifySwitchPosition(bool inSession = false);
    // Detector-A-only position tracking for probeMode's blocking loop
    // (agree mode only; see the definition).
    void checkSwitchPositionFast(void);
    float checkProbeCurrentRaw(void);   // ABSOLUTE INA1 current, NO probe_current_zero subtraction (internal building block / diagnostics)
    float checkProbeCurrent(void);      // zero-corrected current (raw - probe_current_zero); used for switch detection, calibration, and display
    float checkProbeCurrentZero(void);
    float probeCurrentMedian(int n);    // median of n independent CNVR-armed samples, zero-corrected frame (~17ms each; calibration/self-test only)
    
    void routableBufferPower(int offOn, int flash = 0, int force = 0);
    
    void startProbe(long probeSpeed = 25000);
    void stopProbe();
    
    int selectSFprobeMenu(int function = 0);
    
    int getNothingTouched(int samples = 8);
    
    int readRails(int pin = 0);
    int justReadProbe(bool allowDuplicates = false, int rawPad = 0);
    int readProbe(void);
    
    int readProbeRaw(int readNothingTouched = 0, bool allowDuplicates = false); 
    int smoothProbeReading(int probeRead, bool reset = false);
    int calibrateProbe(void);
    void calibrateDac0(float target = 3.3);
    
    void probeLEDhandler(void);
    int probeToggle(int buttonState = -1);  // Note: Currently unused, global function used instead
    
private:
    Probing();
    ~Probing() = default;
    int lastProbeReading = 0;
    volatile int simTapNode = -1;            // simulateProbeTap()
    volatile unsigned long simTapUntil = 0;  // millis() deadline; 0 = inactive
    int smoothedProbeRead = -1;
    unsigned long lastButtonCheckTime = 0;
    unsigned long waitTimer = 0;
    
    // Flags for triggering probe actions (used during transition from goto-based code)
    bool triggerProbeConnect = false;
    bool triggerProbeClear = false;
    char pendingInput = '\0';
    
    // Handle probe button actions and toggle logic
    void handleProbeButtonActions();
    
    // Handle encoder-based node selection in probe mode
    void handleEncoderCursorNavigation(
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
        bool clickExitsToMenu
    );
};

// Global references for clean syntax
extern ProbeButton& probeButton;
extern ProbeSwitch& probeSwitch;
extern ProbePads& probePads;

// Backward compatibility - global references to singleton members
extern volatile int& sfProbeMenu;
extern unsigned long& probingTimer;
extern int& probePin;
extern int& buttonPin;
extern volatile unsigned long& blockProbeButton;
extern volatile unsigned long& blockProbeButtonTimer;
extern volatile int& connectOrClearProbe;
extern volatile int& node1or2;
extern int& probeHighlight;
extern volatile int& removeFade;
extern int& debugProbing;
extern volatile int& showingProbeLEDs;
extern volatile int& switchPosition;

// Encoder cursor override for LED display functions
extern volatile int globalEncoderCursorNode;      // -1 = hidden, else breadboard node
extern volatile int globalEncoderCursorInHeader;  // 1 if in nano header
extern volatile uint32_t globalEncoderCursorColor; // Cursor color
extern int& probePowerDAC;
extern int& showProbeCurrent;


// Pad-decode endpoints for the CURRENT switch position (defined in
// Probing.cpp): the base probe_min/max pair in SELECT, the
// probe_min_measure/max_measure pair scaled by the live ADC7 tip voltage in
// MEASURE. Use this instead of reading the calibration values directly so
// displays/calibration match what the runtime decode actually does.
void probeMapRange(int* mapMin, int* mapMax);

// Evict stale routable-GPIO -> BUFFER_IN power-claim bridges (persisted into
// slots by debug.probe_power_gpio) that don't belong to the live claim.
// Called by routableBufferPower(1) and by the slot-load path in main.cpp.
// Pin-level hardware for the GPIO probe-power claim; called ONLY from
// routing/InfraPaths.cpp's probe_power candidate callbacks. Claim saves the
// pin's previous config and drives it SIO/12mA/output-high; release
// restores (unless MicroPython owns the pin now) and invalidates the droop
// model. Candidate selection lives in InfraPaths.
void probeGpioPowerHwClaim(int gpioDefIdx);
void probeGpioPowerHwRelease(int gpioDefIdx);

// The two switch-position detectors, exposed for the calibration app and the
// self test (both live in Probing.cpp with the classifier). Return a POSITION:
// 0 = MEASURE, 1 = SELECT, -1 = no opinion.
//   probeSwitchTipSenseNow(): detector A (PROBE_PIN drive-low/release/read).
//   probeSwitchFeedBlinkNow(pct): detector B for a live GPIO feed (-1 when the
//     feed is DAC0 / no feed / ADC busy / touch); *pct = held %.
int probeSwitchTipSenseNow(void);
int probeSwitchFeedBlinkNow(int* pct);

// Additional references for backward compatibility
extern volatile int& inPadMenu;
extern volatile int& checkingButton;
extern volatile int& lastProbeLEDs;

// Export probe maps for Apps.cpp
extern int (&probeRowMap)[108];
extern int (&probeRowMapByPad)[108];

// Export pad settings
extern int (&logoTopSetting)[2];
extern int (&logoBottomSetting)[2];
extern int (&buildingTopSetting)[2];
extern int (&buildingBottomSetting)[2];

// Global state flags from main.cpp
extern volatile int probeActive;

extern volatile bool core1busy;
extern volatile bool core2busy;
extern volatile int loadingFile;

// Timestamp for last INA219 probe current check (for measure mode timing coordination)
extern volatile unsigned long lastProbeCurrentCheckTime;
  

#endif