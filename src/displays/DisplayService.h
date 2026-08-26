// SPDX-License-Identifier: MIT
#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include <Arduino.h>
#include "JumperlOS.h"
#include "DisplayTypes.h"

// The background display driver (Guides-Simplification workstream C).
//
// Lifecycle, all from one parts-table poll (covers place_part, slot loads,
// board clear and part removal uniformly - no routing-layer hooks):
//   placed part resolves to a driver -> route its DATA pins (SDA/SCL by pin
//   name, connect-field write + reapply; VCC/GND never touched - the user
//   wires power) -> BEACON pings until the panel answers (i.e. until it IS
//   powered) -> init -> ALIVE: the startup-frame animation plays and the
//   framebuffer flushes in bounded chunks, one bus transaction per tick.
//   Flush errors return to BEACON (self-healing); a user pin claim YIELDS
//   (release + one line + retry - warn, never block); the part leaving the
//   table detaches everything.
//
// inInnerSet = true is the point: the animation keeps running inside menus
// and probe mode. The chunk budget halves while something BLOCKING holds the
// loop so ambient never starves the modal UI.
class DisplayService : public Service {
public:
    static DisplayService& getInstance();

    DisplayService(const DisplayService&) = delete;
    DisplayService& operator=(const DisplayService&) = delete;

    ServiceStatus service() override;
    const char* getName() const override { return "Displays"; }
    ServicePriority getPriority() const override { return ServicePriority::NORMAL; }
    uint32_t periodUs() const override { return 2000; }
    bool inInnerSet() const override { return true; }

    // The one instance slot (the 8 routable GPIOs realistically bound N=2;
    // slice 1 proves the framework with 1).
    const DisplayInstance& instance() const { return inst; }

    // The routed bus nodes (131-138) while this service owns a live bus, for
    // core 1's net-voltage scan to EXCLUDE: a sense tap closes crosspoints on
    // the net mid-I2C-transaction, and one corrupted bit costs a re-init.
    // Cross-core read of two int8 fields - a torn read costs one scan round.
    int activeDataNodes(int16_t out[2]) const;

    // The [disp] line in the i! dump. The chunk-retry fix MASKS bus glitches
    // by design - this counter is what tells a healthy bus from one that is
    // quietly retrying its way through every frame.
    void printStats(Stream* out) const;

private:
    DisplayService() = default;
    ~DisplayService() = default;

    DisplayInstance inst;
    uint32_t nextPartsPollMs = 0;
    bool ghostWarned = false;
    bool yieldNoted = false;
    bool acquireWarned = false;   // one line per acquire-failure episode
    uint8_t flushFails = 0;       // consecutive chunk errors (8 = panel lost)
    uint8_t initFails = 0;        // consecutive init failures (5 = slow beacon)
    uint8_t routeFails = 0;       // consecutive routing refusals (5 = slow poll)
    bool allocWarned = false;     // one line per framebuffer-OOM episode
    char attachedName[16] = {0};  // part identity at attach (index can slide)
    uint32_t flushRetryTotal = 0; // cumulative chunk retries (glitch health)
    uint32_t lostTotal = 0;       // beacon bounces after 8-in-a-row failures
    uint32_t framesFlushed = 0;   // completed frames since boot

    void pollParts(uint32_t now);
    void attach(int partIdx, const DisplayDriverDesc* desc);
    void detach();
    bool routeDataPins(int partIdx);
    void animate(uint32_t now);
};

extern DisplayService& displayService;

#endif // DISPLAY_SERVICE_H
