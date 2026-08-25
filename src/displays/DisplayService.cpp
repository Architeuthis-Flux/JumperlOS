// SPDX-License-Identifier: MIT
// Background display driver. Contract: DisplayService.h.

#include "DisplayService.h"

#include <new>
#include <string.h>

#include "Commands.h"          // refreshConnections
#include "DisplayBus.h"
#include "DisplayDrivers.h"
#include "PartPlacement.h"     // applyPartPlacement / removePartPlacement / partPinNode
#include "States.h"            // globalState
#include "boards/board.h"

// The Jumperless startup animation, already in flash (Images.h data via the
// StartupFrames accessor): 45 frames of 32x21 RGB, nearest-neighbor scaled
// and luma-thresholded onto the mono panel. This is what a detected display
// shows by default - the board saying hello.
extern const uint32_t* jl_get_startup_frame(int imageIndex);
static const int DISP_ANIM_FRAMES = 45;          // startupFrameLEN (Images.h)
static const uint32_t DISP_ANIM_STEP_MS = 120;
static const uint32_t DISP_PARTS_POLL_MS = 750;  // the oled checkConnection cadence
static const uint32_t DISP_BEACON_MS = 300;      // the i2cscrn beacon cadence

DisplayService& DisplayService::getInstance() {
    static DisplayService instance;
    return instance;
}
DisplayService& displayService = DisplayService::getInstance();

// Node for a routable RP pin (20-27 -> 131-138).
static inline int16_t nodeForRpPin(int pin) { return (int16_t)(131 + (pin - 20)); }

// ---------------------------------------------------------------------------
// Attach / detach
// ---------------------------------------------------------------------------

// Write the chosen bus nodes into the part's SDA/SCL pins' connect fields via
// the remove -> mutate -> reapply invariant (PartPlacement.h:25-30). Pin
// ROLES are the pin names - "SDA"/"SCL" - which both the seed DB and the
// custom-labeling flow use. VCC/GND stay untouched.
bool DisplayService::routeDataPins(int partIdx) {
    JumperlessState& st = globalState;
    PartDefinition& p = st.parts.parts[partIdx];

    int sdaIdx = -1, sclIdx = -1;
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        if (strcasecmp(p.pins[j].name, "SDA") == 0) sdaIdx = j;
        else if (strcasecmp(p.pins[j].name, "SCL") == 0) sclIdx = j;
    }
    if (sdaIdx < 0 || sclIdx < 0) return false;

    int16_t sdaNode = nodeForRpPin(inst.sdaPin);
    int16_t sclNode = nodeForRpPin(inst.sclPin);
    if (p.pins[sdaIdx].connect == sdaNode && p.pins[sclIdx].connect == sclNode) {
        return true;   // already routed (a reload restored our bridges)
    }

    String err;
    bool wasPlaced = p.placed;
    if (wasPlaced) {
        if (removePartPlacement(st, partIdx, err) < 0) return false;
    }
    p.pins[sdaIdx].connect = sdaNode;
    p.pins[sclIdx].connect = sclNode;
    err = "";
    int added = applyPartPlacement(st, partIdx, err);
    // Refusal detection (sweep finding, medium): applyPartPlacement returns
    // -1 only for a bad index - addConnection refusals (bridge table full)
    // come back as err TEXT with a non-negative count. Treating those as
    // success persisted connect: fields whose bridges never existed, and
    // the part then read as "already routed" forever.
    if (added < 0 || err.length() > 0) {
        // Put the pins back to unrouted and re-place so the part's labels
        // stay honest; the poll retries later.
        p.pins[sdaIdx].connect = -1;
        p.pins[sclIdx].connect = -1;
        String err2;
        applyPartPlacement(st, partIdx, err2);
        st.markDirty();   // the restore must persist too, or a save between
                          // attempts writes routed-looking pins with no bridges
        return false;
    }
    st.markDirty();
    refreshConnections(-1);
    return true;
}

void DisplayService::attach(int partIdx, const DisplayDriverDesc* desc) {
    inst.desc = desc;
    inst.partIdx = (int8_t)partIdx;
    inst.i2cAddr = desc->i2cAddrs[0];

    const char* reason = nullptr;
    if (!displayBusAcquire(inst, &reason)) {
        // Stay EMPTY (not YIELDED): with the pins never acquired,
        // displayBusUserClaimed() reads them as -1 and the YIELDED resume
        // path would detach/re-attach - and re-print this - every 750 ms
        // poll (sweep finding). One line, then the poll retries quietly.
        if (!acquireWarned) {
            acquireWarned = true;
            Serial.print("\r\nDISPLAY paused: ");
            Serial.println(reason ? reason : "bus unavailable");
        }
        inst.desc = nullptr;
        inst.partIdx = -1;
        inst.state = DispState::EMPTY;
        return;
    }
    acquireWarned = false;
    if (!routeDataPins(partIdx)) {
        // No SDA/SCL pins or router refusal - stay EMPTY-ish and retry at
        // poll cadence rather than half-attaching.
        displayBusRelease(inst);
        inst.desc = nullptr;
        inst.partIdx = -1;
        inst.state = DispState::EMPTY;
        return;
    }

    inst.fb = new (std::nothrow) uint8_t[desc->fbBytes];
    // pageFlip panels keep one shadow image per RAM half.
    uint16_t shadowBytes = desc->quirks.pageFlip ? (uint16_t)(desc->fbBytes * 2)
                                                 : desc->fbBytes;
    inst.shadow = new (std::nothrow) uint8_t[shadowBytes];
    if (inst.fb == nullptr || inst.shadow == nullptr) {
        delete[] inst.fb;      inst.fb = nullptr;
        delete[] inst.shadow;  inst.shadow = nullptr;
        displayBusRelease(inst);
        inst.desc = nullptr;
        inst.partIdx = -1;
        inst.state = DispState::EMPTY;
        return;
    }
    memset(inst.fb, 0, desc->fbBytes);
    inst.shadowValidMask = 0;
    // Soft chunk 16: the 7-byte position header is fixed, so 8 data bytes
    // spent >half the bus time on overhead; 16 lands ~1 ms per chunk at the
    // 250 kHz half-bit and cuts a 512-byte frame to 32 transactions.
    inst.chunkBytes = (inst.sdaPin == 26) ? 32 : 16;  // hw vs soft budget
    inst.state = DispState::ROUTED;
    inst.nextBeaconMs = 0;
    ghostWarned = false;
    Serial.print("\r\nDISPLAY routed id=");
    Serial.print(desc->id);
    Serial.print(" part=");
    Serial.print(globalState.parts.parts[partIdx].name);
    Serial.print(" sda=GP");
    Serial.print(inst.sdaPin);
    Serial.print(" scl=GP");
    Serial.print(inst.sclPin);
    Serial.println(" (power rides the rails - beacon waiting)");
    Serial.flush();
}

void DisplayService::detach() {
    if (inst.fb != nullptr) {
        delete[] inst.fb;
        inst.fb = nullptr;
    }
    if (inst.shadow != nullptr) {
        delete[] inst.shadow;
        inst.shadow = nullptr;
    }
    inst.shadowValidMask = 0;
    inst.backHalf = 0;
    displayBusRelease(inst);
    // The part's bridges (if it still exists) are its own data now; a part
    // that left the table already took them down via removePartPlacement.
    inst.desc = nullptr;
    inst.partIdx = -1;
    inst.state = DispState::EMPTY;
    inst.midFrame = false;
    inst.dirty = false;
    inst.userOwnsContent = false;
    yieldNoted = false;
    flushFails = 0;
}

// One poll: diff the parts table against the attached instance.
void DisplayService::pollParts(uint32_t now) {
    if ((int32_t)(now - nextPartsPollMs) < 0) return;
    // Attach/detach ROUTES (removePartPlacement/apply/refreshConnections) -
    // never do that under a modal loop that may itself be mid-routing.
    // Flushing chunks under modal load is fine; rebuilding the fabric is not.
    if (jOS.getBlockingService() != nullptr) return;
    nextPartsPollMs = now + DISP_PARTS_POLL_MS;

    // Attached part still there, same identity?
    if (inst.partIdx >= 0) {
        bool gone = inst.partIdx >= globalState.parts.numParts ||
                    displayResolveForPart(globalState.parts.parts[inst.partIdx]) != inst.desc;
        if (gone) detach();
        else if (inst.state == DispState::YIELDED && !displayBusUserClaimed(inst)) {
            // The user's script released our pins: resume via a fresh attach.
            int idx = inst.partIdx;
            const DisplayDriverDesc* desc = inst.desc;
            detach();
            attach(idx, desc);
        }
        return;
    }

    // Not attached: first placed part that resolves to a driver wins.
    for (int i = 0; i < globalState.parts.numParts && i < MAX_PARTS; i++) {
        const PartDefinition& p = globalState.parts.parts[i];
        if (!p.placed) continue;
        const DisplayDriverDesc* desc = displayResolveForPart(p);
        if (desc != nullptr) {
            attach(i, desc);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Animation (paused the moment MP content lands)
// ---------------------------------------------------------------------------

void DisplayService::animate(uint32_t now) {
    if (inst.userOwnsContent || inst.midFrame) return;
    if ((int32_t)(now - inst.animNextMs) < 0) return;
    inst.animNextMs = now + DISP_ANIM_STEP_MS;

    const uint32_t* src = jl_get_startup_frame(inst.animFrame);
    inst.animFrame += inst.animDir;
    if (inst.animFrame >= DISP_ANIM_FRAMES - 1 || inst.animFrame <= 0) inst.animDir = -inst.animDir;
    if (src == nullptr || inst.fb == nullptr) return;

    const int srcW = 32, srcH = 21;
    const int w = inst.desc->w, h = inst.desc->h;
    memset(inst.fb, 0, inst.desc->fbBytes);
    for (int dy = 0; dy < h; dy++) {
        int sy = (dy * srcH) / h;
        for (int dx = 0; dx < w; dx++) {
            int sx = (dx * srcW) / w;
            uint32_t c = src[sy * srcW + sx];
            uint32_t luma = ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
            if (luma > 96) {
                inst.fb[(dy / 8) * w + dx] |= (uint8_t)(1u << (dy & 7));
            }
        }
    }
    inst.dirty = true;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

ServiceStatus DisplayService::service() {
    uint32_t now = millis();
    pollParts(now);
    if (inst.state == DispState::EMPTY) return ServiceStatus::IDLE;

    // A user script claiming our pins pauses everything - warn, never block.
    if (inst.state != DispState::YIELDED && displayBusUserClaimed(inst)) {
        inst.state = DispState::YIELDED;
        inst.midFrame = false;
        if (!yieldNoted) {
            yieldNoted = true;
            Serial.print("\r\nDISPLAY paused: GP");
            Serial.print(inst.sdaPin);
            Serial.println(" claimed by your script - release it to resume");
        }
        return ServiceStatus::IDLE;
    }
    if (inst.state == DispState::YIELDED) return ServiceStatus::IDLE;

    if (inst.state == DispState::ROUTED) {
        if ((int32_t)(now - inst.nextBeaconMs) < 0) return ServiceStatus::IDLE;
        // Bring-up waits out modal interactions: init's full-RAM clear is a
        // ~45 ms burst in ONE service call - fine ambient, hostile inside a
        // menu/probe loop that pumps inner-set services (verify-workflow
        // finding). The panel comes alive the moment the interaction ends.
        if (jOS.getBlockingService() != nullptr) return ServiceStatus::IDLE;
        inst.nextBeaconMs = now + DISP_BEACON_MS;
        for (int a = 0; inst.desc->i2cAddrs[a] != 0 && a < 3; a++) {
            if (displayI2cPing(inst, inst.desc->i2cAddrs[a])) {
                // Pull-up honesty before celebrating: a bus whose SDA can't
                // rise ACKs everything, and that must not read as a panel.
                if (displayI2cCountGhosts(inst) > 8) {
                    if (!ghostWarned) {
                        ghostWarned = true;
                        Serial.println("\r\nDISPLAY every address answers - SDA "
                                       "is stuck low (add ~4.7k pull-ups)");
                    }
                    return ServiceStatus::BUSY;
                }
                inst.i2cAddr = inst.desc->i2cAddrs[a];
                if (inst.desc->ops->init(inst)) {
                    initFails = 0;
                    inst.state = DispState::ALIVE;
                    inst.animNextMs = 0;
                    inst.userOwnsContent = false;
                    Serial.print("\r\nDISPLAY alive id=");
                    Serial.print(inst.desc->id);
                    Serial.print(" addr=0x");
                    Serial.println(inst.i2cAddr, HEX);
                    Serial.flush();
                } else {
                    // A panel that pings but won't init: unstick the bus (a
                    // failed clear can wedge it too), and after 5 strikes say
                    // so once and back the beacon off to 2 s - the old path
                    // retried the full ~45 ms init every 300 ms forever,
                    // silently (verify-workflow finding).
                    displayBusUnstick(inst);
                    if (++initFails == 5) {
                        Serial.print("\r\nDISPLAY init failing id=");
                        Serial.print(inst.desc->id);
                        Serial.println(" - retrying slowly (check power/wiring)");
                    }
                    if (initFails >= 5) inst.nextBeaconMs = now + 2000;
                }
                return ServiceStatus::BUSY;
            }
        }
        return ServiceStatus::IDLE;
    }

    // ALIVE: advance the animation, then push at most ONE bus chunk.
    animate(now);
    if (!inst.dirty && !inst.midFrame) return ServiceStatus::IDLE;

    // Soft-bus pacing: visits every ~4 ms, and each visit BURSTS chunks
    // back-to-back under a time budget. One-chunk-per-8 ms was the real
    // frame-time bottleneck (bus busy ~1 ms of every window) - the tearing
    // Kevin still saw after delta flush was pacing, not wire speed. A delta
    // frame now lands inside a single visit; worst-case duty stays bounded
    // (~1.5 ms per 4 ms mid-frame, half that under a modal load).
    bool softBus = (inst.sdaPin != 26);
    if (softBus) {
        if ((int32_t)(now - inst.nextChunkMs) < 0) return ServiceStatus::IDLE;
        inst.nextChunkMs = now + 4;
    }

    // Modal load (something BLOCKING owns the loop): halve the chunk and the
    // burst so the ambient animation never starves the menu/probe UI.
    bool modal = jOS.getBlockingService() != nullptr;
    uint8_t budget = inst.chunkBytes;
    if (modal && budget > 4) budget /= 2;
    uint8_t saved = inst.chunkBytes;
    inst.chunkBytes = budget;
    uint32_t burstUs = modal ? 700 : 1500;
    uint32_t burstStart = micros();
    int r;
    do {
        r = inst.desc->ops->flushChunk(inst);
    } while (r == 1 && (uint32_t)(micros() - burstStart) < burstUs);
    inst.chunkBytes = saved;

    if (r < 0) {
        // ONE bus error is a glitch, not a lost panel - a probe tip on the
        // bus rows, a scan tap, a marginal edge. The cursor didn't advance
        // and every chunk re-positions (page + column), so just retrying
        // the same chunk next tick is safe. Bouncing to beacon on the first
        // NACK re-ran init per glitch: the repeated "DISPLAY alive" spam,
        // the garbage frames, and most of the slowness on the first bench.
        flushRetryTotal++;
        // Clear the wedge BEFORE the retry: an interrupted byte can leave
        // the panel driving SDA low, where every subsequent START fails too
        // - that was the lost->alive cycling (8 straight errors, "fixed" by
        // the beacon's pings accidentally clocking the slave free, one black
        // re-init later). Nine clocks + STOP makes the very next retry real.
        displayBusUnstick(inst);
        if (++flushFails < 8) return ServiceStatus::BUSY;
        flushFails = 0;
        lostTotal++;
        inst.state = DispState::ROUTED;
        inst.midFrame = false;
        inst.nextBeaconMs = now + DISP_BEACON_MS;
        Serial.print("\r\nDISPLAY lost id=");
        Serial.print(inst.desc->id);
        Serial.println(" (8 bus errors - re-beaconing)");
        return ServiceStatus::BUSY;
    }
    flushFails = 0;
    if (r == 0) {
        inst.dirty = false;
        framesFlushed++;
    }
    return ServiceStatus::BUSY;
}

void DisplayService::printStats(Stream* out) const {
    if (inst.partIdx < 0) return;
    static const char* stateNames[] = {"EMPTY", "ROUTED", "ALIVE", "YIELDED"};
    out->printf("[disp] id=%s state=%s frames:%lu retries:%lu lost:%lu\n",
                inst.desc ? inst.desc->id : "?",
                stateNames[(int)inst.state & 3],
                (unsigned long)framesFlushed,
                (unsigned long)flushRetryTotal,
                (unsigned long)lostTotal);
}

int DisplayService::activeDataNodes(int16_t out[2]) const {
    if (inst.partIdx < 0 || inst.sdaPin < 20) return 0;
    if (inst.state != DispState::ROUTED && inst.state != DispState::ALIVE) return 0;
    out[0] = nodeForRpPin(inst.sdaPin);
    out[1] = nodeForRpPin(inst.sclPin);
    return 2;
}
