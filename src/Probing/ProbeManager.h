// SPDX-License-Identifier: MIT
//
// ProbeManager - the probe HARDWARE surface for the v2 engine: button
// presses, switch position, probe LEDs, tip/buffer power, and tip current.
//
// The actual drivers (ProbeButton's PIO sampler + IRQ state machine, the
// switch-position droop/INA sensing, the core-1 LED handler, the buffer
// power GPIO claim) are stateful hardware singletons that must exist
// exactly once, so they stay in Probing.cpp and BOTH engines share them.
// This class is the organized front door: new code (Pads' session logic,
// apps, future subsystems) should talk to the probe hardware through here
// rather than reaching for the legacy singletons directly. When the legacy
// engine is eventually retired, the driver implementations migrate into
// ProbeManager.cpp behind this same API.
#ifndef PROBE_MANAGER_H
#define PROBE_MANAGER_H

#include "Probing.h" // shared drivers + aliases

class ProbeManager {
public:
    static ProbeManager& getInstance();
    ProbeManager(const ProbeManager&) = delete;
    ProbeManager& operator=(const ProbeManager&) = delete;

    // ---- probe button (shared ProbeButton CRITICAL service) ----
    // Consume the pending press event: 0 none, 1 remove, 2 connect.
    int buttonPressEvent(bool consume = true);
    // Live state without consuming: 0 released, 1 remove, 2 connect.
    int buttonState();
    void clearButtonState();
    // Suppress button events for ms (0 = clear the block).
    void blockButton(unsigned long ms);
    bool buttonBlocked();

    // ---- connect/measure switch ----
    int switchPos();           // cached: -1 unknown, 0 measure, 1 select
    int checkSwitchPosition(); // active check (may bounce the tip feed)

    // ---- probe LEDs (core-1 probeLEDhandler modes) ----
    // 1 connect, 2 clear, 3 measure-idle, 4 select-idle, 11 node flash...
    void setProbeLEDs(int mode);

    // ---- tip / buffer power ----
    void bufferPower(int offOn, int flash = 0, int force = 0);
    float probeCurrent(); // zero-corrected INA1 current (mA)

private:
    ProbeManager() = default;
    ~ProbeManager() = default;
    static ProbeManager* instance;
};

extern ProbeManager& probeManager;

#endif // PROBE_MANAGER_H
// ProbeManager - the probe HARDWARE surface for the v2 engine: button
// presses, switch position, probe LEDs, tip/buffer power, and tip current.
//
// The actual drivers (ProbeButton's PIO sampler + IRQ state machine, the
// switch-position droop/INA sensing, the core-1 LED handler, the buffer
// power GPIO claim) are stateful hardware singletons that must exist
// exactly once, so they stay in Probing.cpp and BOTH engines share them.
// This class is the organized front door: new code (Pads' session logic,
// apps, future subsystems) should talk to the probe hardware through here
// rather than reaching for the legacy singletons directly. When the legacy
// engine is eventually retired, the driver implementations migrate into
// ProbeManager.cpp behind this same API.
#ifndef PROBE_MANAGER_H
#define PROBE_MANAGER_H

#include "Probing.h" // shared drivers + aliases

class ProbeManager {
public:
    static ProbeManager& getInstance();
    ProbeManager(const ProbeManager&) = delete;
    ProbeManager& operator=(const ProbeManager&) = delete;

    // ---- probe button (shared ProbeButton CRITICAL service) ----
    // Consume the pending press event: 0 none, 1 remove, 2 connect.
    int buttonPressEvent(bool consume = true);
    // Live state without consuming: 0 released, 1 remove, 2 connect.
    int buttonState();
    void clearButtonState();
    // Suppress button events for ms (0 = clear the block).
    void blockButton(unsigned long ms);
    bool buttonBlocked();

    // ---- connect/measure switch ----
    int switchPos();          // cached: -1 unknown, 0 measure, 1 select
    int checkSwitchPosition();// active check (may bounce the tip feed)

    // ---- probe LEDs (core-1 probeLEDhandler modes) ----
    // 1 connect, 2 clear, 3 measure-idle, 4 select-idle, 11 node flash...
    void setProbeLEDs(int mode);

    // ---- tip / buffer power ----
    void bufferPower(int offOn, int flash = 0, int force = 0);
    float probeCurrent();     // zero-corrected INA1 current (mA)

private:
    ProbeManager() = default;
    ~ProbeManager() = default;
    static ProbeManager* instance;
};

extern ProbeManager& probeManager;

#endif // PROBE_MANAGER_H
