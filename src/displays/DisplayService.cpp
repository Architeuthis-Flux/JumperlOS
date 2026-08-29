// SPDX-License-Identifier: MIT
// Background display driver. Contract: DisplayService.h.

#include "DisplayService.h"

#include <new>
#include <string.h>

#include "Commands.h"          // refreshConnections
#include "CoreMailbox.h"       // fabric-send fence for the soft-I2C chunks
#include "DisplayBus.h"
#include "DisplayDrivers.h"
#include "Peripherals.h"       // railTruth - the overvolt hint in the lost verdict
#include "PartPlacement.h"     // applyPartPlacement / removePartPlacement / partPinNode
#include "States.h"            // globalState
#include "Undo.h"              // UndoIngestGuard - route churn is not a user action
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

// Are this part's SDA/SCL pins still wired to OUR bus nodes? Name identity
// alone cannot see a slot LOAD: deserializeParts rebuilds the parts table and
// the bridge table wholesale without telling anyone, so a same-named panel
// from another slot can slide in with connect: -1 and no bridges behind it -
// and the service would keep flushing a bus that isn't there, because
// routeDataPins() is reachable only from attach(). Same predicate as
// routeDataPins' already-routed early return.
static bool dataPinsRouted(const PartDefinition& p, int sdaPin, int sclPin) {
    if (sdaPin < 20 || sclPin < 20) return true;   // not acquired yet
    int16_t sdaNode = nodeForRpPin(sdaPin);
    int16_t sclNode = nodeForRpPin(sclPin);
    bool sdaOk = false, sclOk = false;
    for (int j = 0; j < p.numPins && j < MAX_PART_PINS; j++) {
        if (strcasecmp(p.pins[j].name, "SDA") == 0)      sdaOk = (p.pins[j].connect == sdaNode);
        else if (strcasecmp(p.pins[j].name, "SCL") == 0) sclOk = (p.pins[j].connect == sclNode);
    }
    return sdaOk && sclOk;
}

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

    // Our remove/reapply churn is service plumbing, not user actions - without
    // this guard every route wrote phantom transactions into the undo history
    // and one undo press after placing a display disconnected it (sweep
    // finding, high; the routableBufferPower precedent).
    UndoIngestGuard undoGuard;
    int16_t priorSda = p.pins[sdaIdx].connect;
    int16_t priorScl = p.pins[sclIdx].connect;
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
        // Only dirty when the persisted fields actually changed - the 750 ms
        // retry loop was rewriting the slot file forever on a standing
        // refusal (sweep finding, medium).
        if (priorSda != -1 || priorScl != -1) st.markDirty();
        return false;
    }
    st.markDirty();
    refreshConnections(-1);
    return true;
}

void DisplayService::attach(int partIdx, const DisplayDriverDesc* desc) {
    inst.desc = desc;
    inst.partIdx = (int8_t)partIdx;
    // Identity for pollParts, stamped WITH the index it describes: the parts
    // table is keyed by name, and writing the name only after routeDataPins()
    // left a window where partIdx was published against an empty name.
    strncpy(attachedName, globalState.parts.parts[partIdx].name, sizeof(attachedName) - 1);
    attachedName[sizeof(attachedName) - 1] = '\0';
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

    // Allocate BEFORE routing (sweep finding, low): the retry used to rebuild
    // the whole fabric every 750 ms just to fail the alloc again.
    inst.fb = new uint8_t[desc->fbBytes];
    // pageFlip panels keep one shadow image per RAM half.
    uint16_t shadowBytes = desc->quirks.pageFlip ? (uint16_t)(desc->fbBytes * 2)
                                                 : desc->fbBytes;
    inst.shadow = new uint8_t[shadowBytes];

    if (!routeDataPins(partIdx)) {
        // No SDA/SCL pins or router refusal - stay EMPTY-ish; the poll
        // retries, backing off to 5 s after 5 straight refusals (a standing
        // refusal used to thrash remove/reapply every 750 ms forever).
        delete[] inst.fb;      inst.fb = nullptr;
        delete[] inst.shadow;  inst.shadow = nullptr;
        displayBusRelease(inst);
        inst.desc = nullptr;
        inst.partIdx = -1;
        inst.state = DispState::EMPTY;
        if (++routeFails == 5) {
            Serial.println("\r\nDISPLAY routing keeps failing - retrying slowly "
                           "(bridge table full?)");
        }
        if (routeFails >= 5) nextPartsPollMs = millis() + 5000;
        return;
    }
    routeFails = 0;
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
    initFails = 0;
    routeFails = 0;
    acquireWarned = false;
    attachedName[0] = '\0';
}

// One poll: diff the parts table against the attached instance.
void DisplayService::pollParts(uint32_t now) {
    if ((int32_t)(now - nextPartsPollMs) < 0) return;
    // Attach/detach ROUTES (removePartPlacement/apply/refreshConnections) -
    // never do that under a modal UI loop that may itself be mid-routing.
    // Flushing chunks under modal load is fine; rebuilding the fabric is not.
    // isUiModal(), not inModalContext(): the latter is also true for a whole
    // script's lifetime (mp_hal_delay_ms pumps the inner set), which left a
    // panel placed mid-script unattached and dark until the script ended.
    if (jOS.isUiModal()) return;
    nextPartsPollMs = now + DISP_PARTS_POLL_MS;

    // Attached part still there, same identity?
    if (inst.partIdx >= 0) {
        // Identity is the NAME, not the index (Kevin's rule; both create paths
        // and now the load path keep names unique). The index is just where it
        // happened to sit: jl_remove_part's compaction slides a same-driver
        // part into it, so look the name up and RE-BIND rather than detach.
        int idx = globalState.parts.findByName(attachedName);
        bool gone = (idx < 0);
        if (!gone) {
            inst.partIdx = (int8_t)idx;
            const PartDefinition& p = globalState.parts.parts[idx];
            // placed:false is a part attach()'s own scan would never have
            // picked up, and a slot load can restore one that way with no
            // bridges behind it.
            gone = !p.placed ||
                   displayResolveForPart(p) != inst.desc ||
                   !dataPinsRouted(p, inst.sdaPin, inst.sclPin);
        }
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
    // V5 only: routeDataPins/nodeForRpPin encode the V5 node map (131-138)
    // and the soft-bus pins are V5's routable GPIO block - on OG the service
    // stays dormant (release finding: it registered unconditionally and
    // would have written bogus connect nodes for a placed display part).
    if (!board::currentBoard().caps.breadboardDisplays) return ServiceStatus::IDLE;
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
        // finding). The panel comes alive the moment the interaction ends -
        // and a sleeping script is not an interaction, hence isUiModal().
        if (jOS.isUiModal()) return ServiceStatus::IDLE;
        inst.nextBeaconMs = now + DISP_BEACON_MS;
        for (int a = 0; inst.desc->i2cAddrs[a] != 0 && a < 3; a++) {
            if (displayI2cPing(inst, inst.desc->i2cAddrs[a])) {
                // Pull-up honesty before celebrating: a bus whose SDA can't
                // rise ACKs everything, and that must not read as a panel.
                if (displayI2cCountGhosts(inst) > 8) {
                    if (!ghostWarned) {
                        ghostWarned = true;
                        Serial.println("\r\nDISPLAY every address answers - SDA "
                                       "held low (wedged device or short)");
                    }
                    displayBusUnstick(inst);   // give a wedged device its 9
                                               // clocks before the next beacon
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

    // FABRIC FENCE: the bus routes through crossbar lanes, and a pending
    // crosspoint send (measure mode fires two per row hop - remove + add of
    // the ephemeral ADC path) can re-assert or move those lanes mid-byte.
    // Bit-banging into a moving fabric was the measure-mode lost/alive
    // cycling: 8 corrupted chunks per row change, verdict "lost", beacon,
    // re-init, repeat. Defer the burst while a send is pending - the soft
    // bus revisits in ~4 ms. Bounded: legacy wavegen playback can park a
    // REQ_SEND pending indefinitely, so after 250 ms of continuous fencing
    // proceed anyway (the generation check below still forgives strikes).
    static unsigned long fenceSinceMs = 0;
    bool fabricBusy = !core1req::idle(core1req::REQ_SEND) ||
                      !core1req::idle(core1req::REQ_BYPASS);
    if (fabricBusy) {
        if (fenceSinceMs == 0) fenceSinceMs = now;
        if (now - fenceSinceMs < 250) return ServiceStatus::BUSY;
    } else {
        fenceSinceMs = 0;
    }
    uint32_t sendGenBefore, bypassGenBefore;
    core1req::snapshot(core1req::REQ_SEND, nullptr, nullptr, &sendGenBefore);
    core1req::snapshot(core1req::REQ_BYPASS, nullptr, nullptr, &bypassGenBefore);

    // Modal load (a human is waiting on the loop): halve the chunk and the
    // burst so the ambient animation never starves the menu/probe UI. A
    // running script is NOT that - halving there just made every panel
    // refresh twice as slow for the whole script.
    bool modal = jOS.isUiModal();
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
        // A fabric send completed under this burst: the error is the
        // fabric's doing, not the panel's - unstick and retry, no strike.
        uint32_t sendGenAfter, bypassGenAfter;
        core1req::snapshot(core1req::REQ_SEND, nullptr, nullptr, &sendGenAfter);
        core1req::snapshot(core1req::REQ_BYPASS, nullptr, nullptr, &bypassGenAfter);
        if (sendGenAfter != sendGenBefore || bypassGenAfter != bypassGenBefore) {
            displayBusUnstick(inst);
            return ServiceStatus::BUSY;
        }
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
        Serial.print(" (8 bus errors - re-beaconing)");
        // The bench signature of an overdriven panel is exactly this loss
        // loop: random-byte NACKs, clean lines, rails at 4V. Say so.
        // (railHwVolts seeds at -100 before the first write - fall back to
        // the state's setpoint, the PartLabels railTruth idiom.)
        float vTop = (railHwVolts[0] > -99.0f) ? railHwVolts[0] : globalState.power.topRail;
        float vBot = (railHwVolts[1] > -99.0f) ? railHwVolts[1] : globalState.power.bottomRail;
        float vHot = (vTop > vBot) ? vTop : vBot;
        if (vHot > 3.6f) {
            Serial.print(" - a rail is at ");
            Serial.print(vHot, 2);
            Serial.print("V; this panel wants 3.3V");
        }
        Serial.println();
        return ServiceStatus::BUSY;
    }
    flushFails = 0;
    if (r == 0) {
        inst.dirty = false;
        framesFlushed++;
    }
    return ServiceStatus::BUSY;
}

int DisplayService::aliveStateFor(const char* partName, uint32_t* frames) const {
    if (frames) *frames = 0;
    if (partName == nullptr || attachedName[0] == '\0' ||
        strcmp(attachedName, partName) != 0) {
        return -1;
    }
    if (frames) *frames = framesFlushed;
    return (inst.state == DispState::ALIVE) ? 1 : 0;
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
    // YIELDED counts too (sweep finding): the pins are then driven by the
    // USER's script - the scan must keep off that bus just the same.
    if (inst.partIdx < 0 || inst.sdaPin < 20) return 0;
    out[0] = nodeForRpPin(inst.sdaPin);
    out[1] = nodeForRpPin(inst.sclPin);
    return 2;
}
