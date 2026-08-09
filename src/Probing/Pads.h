// SPDX-License-Identifier: MIT
//
// Pads - probe engine v2: probe-mode logic (pad decode, probe-mode session,
// SF menus). Hardware access (button, switch, LEDs, buffer power) stays in
// the shared drivers in Probing.cpp, fronted by ProbeManager.
//
// Differences from the legacy Probing engine:
//  - Pad decode is calibration-free (PadDecode physics table), replacing
//    the probe_min/probe_max linear map.
//  - probeMode is not a blocking loop: a session is state advanced one
//    pipeline pass per service() call from the main loop (the MeasureMode
//    service pattern), so OLED/LED/terminal services never starve.
//
// Selected at runtime by jumperlessConfig.debug.probe_engine_v2 through the
// dispatch wrappers in Probing.h. Shared globals (switchPosition,
// connectOrClearProbe, showProbeLEDs, probeRowMap, ...) remain owned by the
// legacy singletons - this engine reads/writes them through the existing
// reference aliases so the two engines can never hold divergent state.
#ifndef PADS_H
#define PADS_H

#include "JumperlessDefines.h"
#include "JumperlOS.h"
#include "Probing.h" // shared aliases, ProbeButton, ProbingDoubleTap, wrappers
#include "RotaryEncoder.h" // EncoderAccelerator (session cursor state)

class Pads : public Service {
public:
    static Pads& getInstance();
    Pads(const Pads&) = delete;
    Pads& operator=(const Pads&) = delete;

    // Service interface. Registered through the ProbeEngineService
    // dispatcher (name "Probing") so jOS.forceServiceByName keeps working.
    ServiceStatus service() override;
    const char* getName() const override { return "Pads"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }

    // ---- probe-mode session (non-blocking; advanced by service()) ----
    enum class SessionEnd {
        ButtonCommit,   // same-mode probe button press = done
        EncoderHeld,    // wheel held = exit probing
        ExitToClickMenu,// short wheel click, probe-button entry = open menu
        DoubleTapBail,  // second tap of an entry double-tap cancelled us
        SerialInput,    // user typed = hand control back to the terminal
        Timeout,        // session timeout
        SingleShotDone, // preloaded firstConnection flow finished
        External        // torn down from outside (engine switch etc.)
    };
    // fromClickMenu: true when the click menu launched this session. When
    // false (probe-button entry), a short wheel click with no encoder
    // cursor showing exits probing and reopens the click menu.
    void beginSession(int setOrClear, int firstConnection = -1, bool fromClickMenu = false);
    void endSession(SessionEnd reason);
    bool sessionActive() const { return session.active; }

    // ---- readers (v2 decode) ----
    int readProbeRaw(int readNothingTouched = 0, bool allowDuplicates = false);
    int smoothProbeReading(int probeRead, bool reset = false);
    int justReadProbe(bool allowDuplicates = false, int rawPad = 0);
    int readProbe(); // single poll: row, or -16/-18 button, -19/-17/-10 encoder, -1 none
    void checkPads();
    int getNothingTouched(int samples = 8);
    int getLastProbeReading() const { return lastProbeReading; }

    // ---- kept probe-UI logic (copied from the legacy engine so its
    // internal probe reads use the v2 decode; still blocking while open,
    // same as legacy - ponytail: converting the choosers to session states
    // is the upgrade path) ----
    int selectSFprobeMenu(int function = 0);
    int attachPadsToSettings(int pad);
    int delayWithButton(int delayTime = 1000);
    int chooseGPIO(int skipInputOutput = 0);
    int chooseGPIOinputOutput(int gpioChosen);
    int chooseADC(void);
    int chooseDAC(int justPickOne = 0);
    int chooseIsense(void);
    float voltageSelect(int fiveOrEight = 8);
    int longShortPress(int pressLength = 500);
    int checkProbeButton(void);      // event-based (consumes)
    int checkProbeButtonState(void); // state-based

    // Read-block used by external code paths (parity with legacy members).
    volatile unsigned long blockProbing = 0;
    volatile unsigned long blockProbingTimer = 0;

private:
    Pads() = default;
    ~Pads() = default;
    static Pads* instance;

    int lastProbeReading = 0;
    int smoothedProbeRead = -1;

    // v2 button handling (replaces legacy handleProbeButtonActions when
    // this engine is active): starts sessions instead of blocking calls.
    void handleProbeButtonActions();

    // ---- session state ----
    struct ProbeSession {
        bool active = false;
        int setOrClear = 1;
        int firstConnection = -1; // >0 preloaded node; -2/-3 single-shot markers
        bool fromClickMenu = false;
        // lifecycle
        unsigned long outerEntryTime = 0;
        bool bannerEmitted = false;
        bool firstEntry = true;
        bool exitToClickMenu = false;
        bool wasHeld = false;
        // deferred in-probe button press (double-tap cancel window)
        int pendingInProbeButton = 0;
        unsigned long pendingInProbeButtonTime = 0;
        // pairing / feedback
        int row0 = -2;
        int row1 = -2;
        int lastProbedRows[4] = { 0, 0, 0, 0 };
        int deleteMisses[20];
        int deleteMissesIndex = 0;
        unsigned long fadeTimer = 0;
        int fadeClear = -1;
        unsigned long doubleSelectTimeout = 0;
        int doubleSelectCountdown = 0;
        int numberOfLocalChanges = 0;
        int connectionsThisSession = 0;
        unsigned long probeModeStartTime = 0;
        // encoder cursor navigation state (session-lifetime)
        EncoderAccelerator encoderAccel = EncoderAccelerator::Slow();
        long lastEncoderPosition = 0;
        float encoderAccumulator = 0.0f;
        int encoderCursorNode = 14;
        int cursorZone = 0;
        int subIndex = 0;
        int lastEncoderCursorNode = -1;
        int lastCursorZone = -1;
        int lastSubIndex = -1;
        unsigned long lastEncoderMovement = 0;
        bool encoderCursorVisible = false;
        int savedRotaryDivider = 8;
    };
    ProbeSession session;

    // pipeline stages (one sessionPass per service() call)
    void sessionPass();
    void sessionRestart();               // (re)enter rendering/init for current mode
    void emitBanner();
    bool sessionHandleButton(bool pendingCommitting); // true = pass consumed / session ended
    void sessionHandleRow();
    void sessionFadeAnimation();
    void sessionIdleSave();

    // encoder cursor navigation (copied from the legacy engine)
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
        bool clickExitsToMenu);
};

extern Pads& pads;

// ----------------------------------------------------------------------------
// jOS service dispatchers - registered in main.cpp INSTEAD of the legacy
// probing/probeSwitch/probePads singletons. They keep the legacy service
// names so jOS.forceServiceByName() callers keep working, route to the
// engine selected by debug.probe_engine_v2, and enforce the probeActive
// gate that the legacy blocking loop provided implicitly (these services
// never ran during a blocking probeMode; a v2 session must suppress them
// the same way).
// ----------------------------------------------------------------------------

class ProbeEngineService : public Service {
public:
    static ProbeEngineService& getInstance();
    ServiceStatus service() override;
    const char* getName() const override { return "Probing"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
private:
    ProbeEngineService() = default;
    static ProbeEngineService* instance;
};

class ProbeSwitchGate : public Service {
public:
    static ProbeSwitchGate& getInstance();
    ServiceStatus service() override;
    const char* getName() const override { return "ProbeSwitch"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
private:
    ProbeSwitchGate() = default;
    static ProbeSwitchGate* instance;
};

class ProbePadsGate : public Service {
public:
    static ProbePadsGate& getInstance();
    ServiceStatus service() override;
    const char* getName() const override { return "ProbePads"; }
    ServicePriority getPriority() const override { return ServicePriority::LOW; }
private:
    ProbePadsGate() = default;
    static ProbePadsGate* instance;
    unsigned long lastCheckTime = 0;
};

extern ProbeEngineService& probeEngineService;
extern ProbeSwitchGate& probeSwitchGate;
extern ProbePadsGate& probePadsGate;

#endif // PADS_H
