// SPDX-License-Identifier: MIT
//
// Undo system.
//
// Compact delta ring (16k+ ops on PSRAM, ~512 SRAM-only) plus a small
// PSRAM-resident snapshot ring that backs non-symmetric ops (clear-all).
// Apply/revert is symmetric for connect/disconnect/DAC/GPIO; clear-all
// captures the full board state as YAML at record time. A bounded slice of
// the ring is persisted to /undo.hist on every slot save and restored at
// boot, so history now survives reboots (on every board - see persistence
// section below).

#include "Undo.h"
#include "Graphics.h"
#include "PsramArena.h"
#include "FilesystemStuff.h"
#include "States.h"
#include "Commands.h"  // refreshConnections() - push bridge list to CH446Q + LEDs
#include "Peripherals.h"  // setRailsAndDACs() - push DAC/rail voltages
#include "config.h"
#include "externVars.h"
#include "oled.h"
#include "OledGui.h"  // OledVars::setStr() - publish last undo/redo label to live screens
#include "NetManager.h"  // definesToChar() - friendly node names (GND/D2/...)
#include "JumperlessDefines.h"
#include "InfraPaths.h"  // infraIsBridge - the auto-managed authority
#include "RotaryEncoder.h"  // NUM_SLOTS, extern netSlot

extern volatile unsigned long undoActivityUntil;
extern int netSlot;

#include <Arduino.h>
#include <FatFS.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>
#include "hardware/structs/sio.h"

#if UNDO_ENABLED

// Per-area debug flag. psram_debug stays as a master that also enables this.
// Matches the linkage of the declaration in Undo.h (inside extern "C" block).
extern "C" {
    volatile int undo_debug = 0;
}

// NOTE: deliberately NO Serial.flush() here. These traces fire in hot paths
// (including under core_sync / fs_mutex on the save path), and a per-line
// blocking flush waits for the USB CDC TX FIFO to fully drain. On an SRAM-only
// board that path has little headroom, and a flood of flushed trace lines can
// stall the USB servicing long enough to wedge the device. Without flush the
// lines coalesce and drain in the background; we may lose the last few lines on
// a hard crash, which is an acceptable trade for not causing one.
#define UNDBG(fmt, ...) do { if (undo_debug || psram_debug) { Serial.printf("[%lu c%d] UNDO> " fmt "\n", (unsigned long)millis(), (int)(sio_hw->cpuid & 1), ##__VA_ARGS__); } } while(0)

extern struct config jumperlessConfig;
extern JumperlessState globalState;

// Backing-store + cache hooks used by the persistence path below. Declared at
// file scope (C++ linkage) on purpose: undoPersistHistory() lives inside the
// extern "C" lifecycle block, so a local `extern` there would bind C linkage
// and fail to resolve the C++ definitions in FilesystemStuff.cpp / FileCache.cpp.
extern bool safeFileWriteAllRaw(const char* path, const char* content,
                                size_t content_len, uint32_t timeout_ms);
extern bool fileCacheInvalidate(const char* path);

// Node-name <-> integer-id helpers (defined in States.cpp / NetManager.cpp).
// parseNodeName is the inverse of definesToChar and also parses raw integer
// ids, so the text format can store friendly node names that round-trip.
extern int parseNodeName(const String& nodeName);

// =============================================================================
// Op / txn record formats
// =============================================================================

namespace {

#pragma pack(push, 1)
struct UndoOp {
    uint8_t  type;           // UndoOpType
    uint8_t  flags;
    uint16_t txnIdx;         // back-pointer to enclosing txn (lower 16 bits)
    union {
        struct { int16_t n1, n2; uint32_t color; uint16_t _pad; } bridge;       // 12B body
        struct { uint8_t ch; uint8_t _pad[3]; float prev, next; } dac;          // 12B
        struct { uint8_t pin, prevVal, nextVal, prevDir; uint8_t _pad[8]; } gpio; // 12B
        struct { uint8_t prev, next; uint8_t _pad[10]; } slot;                  // 12B
        // snapTxnId is a leftover from the old persistent-history layout;
        // kept zero in this version (in-RAM blob refs are always live for
        // the life of the power cycle).
        struct { uint32_t blobOffset; uint32_t blobSize; uint32_t snapTxnId; } blob; // 12B
        uint8_t raw[12];
    };
};

struct UndoTxn {
    uint32_t startMs;
    uint32_t firstOp;        // global op index (monotonic, not ring offset)
    uint32_t globalId;       // monotonic txn ID at endTxn() time
    uint16_t opCount;
    uint8_t  flags;          // UNDO_TXN_* (waypoint, committed, user, obsolete)
    uint8_t  source;
    uint8_t  slot;           // owning slot - undo/redo only walks matching txns
    char     label[23];
};
#pragma pack(pop)

static_assert(sizeof(UndoOp) == 16, "UndoOp must be exactly 16 bytes");
static_assert(sizeof(UndoTxn) == 40, "UndoTxn must be exactly 40 bytes");

// Internal flag (not exposed in Undo.h - this is purely a ring bookkeeping
// concern). When the user records a new action on slot S after having
// undone some history on slot S, the txns past slot S's cursor that
// belong to slot S become unreachable. Rather than evicting them (which
// would also blow away other slots' txns recorded in the same time
// window), we mark them obsolete. The walk helpers skip obsolete entries
// in both directions; the storage gets reclaimed naturally when those
// ring slots reach the global tail and evict.
#define UNDO_TXN_OBSOLETE 0x08u

}  // namespace

// =============================================================================
// Shared ring + per-slot cursors
// =============================================================================
//
// Earlier iterations of this file kept a separate ring per slot, which
// either (a) wasted PSRAM by allocating identical-sized rings even for
// untouched slots, or (b) blew up at runtime when 16 KB allocations
// failed against a fragmented arena. Both modes ran into the same wall:
// rigid per-slot quotas don't match how people use the device.
//
// The current model is dead-simple: ONE big ring (ops + txns + blob
// arena), shared across all slots. Each transaction carries a 1-byte
// `slot` tag, and undo/redo for slot S walks the ring skipping txns
// that belong to other slots. Per-slot bookkeeping is just an 8-entry
// array of ring cursors (one size_t per slot) - 64 bytes total.
//
// The "split the space as we need" property falls out for free: a user
// who only ever touches slot 0 gets the whole ring for slot 0; a user
// who bounces between three slots shares the same ring proportionally
// to how much each slot mutated.
//
// Crucially, an undo on slot 1 still cannot replay ops recorded on
// slot 0 - the walk helpers refuse to step onto a foreign-slot txn.

namespace {

constexpr uint32_t TXN_AUTOCLOSE_MS = 200;

// A probe DAC "slide" fires many setDac() calls in quick succession as the user
// scrolls the voltage. Consecutive same-channel DAC sets made within this
// window collapse into a single undo step (the final value), so one undo
// reverts the whole slide instead of stepping back through every intermediate
// voltage. Wider than TXN_AUTOCLOSE_MS so a slow scroll still coalesces.
constexpr uint32_t DAC_SLIDE_COALESCE_MS = 1200;

// Forward decl for SnapshotEntry; defined later with the snapshot helpers.
struct SnapshotEntry;

}  // namespace

namespace {

// Sizes - selected at init based on PSRAM availability.
size_t g_opCap = 0;
size_t g_txnCap = 0;
size_t g_blobCap = 0;

UndoOp* g_ops = nullptr;
UndoTxn* g_txns = nullptr;
uint8_t* g_blobs = nullptr;

// Monotonic indices that wrap around the ring. We track a head (newest)
// and tail (oldest) in the ring. The cursor is now per-slot (below).
size_t g_opHead = 0;        // next free slot
size_t g_opTail = 0;        // oldest valid op
size_t g_txnHead = 0;       // next free txn slot
size_t g_txnTail = 0;       // oldest valid txn
size_t g_blobUsed = 0;
uint32_t g_globalTxnId = 0;

// Per-slot cursor: ring index into g_txns. Equal to g_txnHead means
// "live, no redo pending"; less than head (in ring sense) means the
// slot has been undone and can redo. Initialized to g_txnHead (== 0)
// for every slot at boot - no slot has any history yet.
//
// NUM_SLOTS+1 buckets: index NUM_SLOTS (8) is the SHARED FILE-CONTEXT bucket.
// Every path context (every project run file, every arbitrary slot YAML) uses
// that one bucket rather than a per-file ring - undo is a within-session tool,
// and per-file rings would multiply the ring's memory by the number of files
// on flash. Before this, an out-of-range netSlot was simply ignored by
// slotSwapIfNeeded, so a file context kept the PREVIOUS numbered slot's ring
// active and tagged its new records with that slot's number.
//
// SESSION-ONLY: bucket-8 transactions are never written to /undo.hist (see
// undoSerialize / undoHasPersistableTxns). The restore path clamps any
// slot >= NUM_SLOTS to 0, so persisted file-context txns would replay into
// slot 0's history on older firmware.
static constexpr int UNDO_FILE_BUCKET = NUM_SLOTS;
size_t g_slotCursor[NUM_SLOTS + 1] = {0};

// Active slot for the open transaction (and for the auto-detect path).
// -1 means undo isn't initialized; otherwise tracks netSlot.
int g_activeSlot = -1;

// Open transaction state - exactly one txn open at a time, for the
// active slot. If a different slot starts recording, the open txn is
// closed first.
bool g_inTxn = false;
size_t g_openTxnIdx = 0;
uint32_t g_openTxnStartMs = 0;
char g_openTxnLabel[24] = {0};
uint8_t g_openTxnSource = UNDO_SRC_UNKNOWN;
uint8_t g_openTxnSlot = 0;
uint16_t g_openTxnOpStart = 0;

uint32_t g_lastOpMs = 0;

// Ingest depth: when > 0, all record* calls become no-ops. Bracketed by
// undoBeginIngest()/undoEndIngest() around slot loads, boot-time state
// restores, and other "system is loading state" regions. Without this,
// every addConnection() that runs during a slot file load gets recorded
// as a fresh user action against the destination slot, polluting the
// new slot's history with phantom entries.
int g_ingestDepth = 0;

// Lazy waypoint recompute. Marked stale on every commit; recomputed once
// when undoNextWaypoint is asked for a fresh answer.
uint32_t g_waypointsValidAt = 0;
bool     g_waypointsDirty = true;

// Stat counters
uint32_t g_undoCount = 0;
uint32_t g_redoCount = 0;
uint32_t g_dropCount = 0;

// Set whenever the committed ring (txns/ops/cursors) changes in a way that
// would alter the persisted /undo.hist file. undoPersistHistory() early-outs
// when this is false so the frequent autosave path doesn't re-serialize +
// re-CRC unchanged history. Cleared inside undoPersistHistory().
bool g_historyDirty = false;

// Runtime persistence limits, chosen in undoInit() based on PSRAM. Persistence
// works on every board now (the user only needs a handful of states without
// PSRAM); these keep the no-PSRAM scratch buffer small. See UNDO_PERSIST_MAX_*
// for the compile-time ceilings the static scratch arrays are sized to.
size_t g_persistMaxTxns  = 0;   // most-recent txns to keep in the file
size_t g_persistMaxBytes = 0;   // body byte budget (drops oldest past this)
size_t g_persistBufBytes = 0;   // serialization scratch buffer size

// Persistence scratch buffer, reserved ONCE in undoInit() while the heap is at
// its emptiest and reused for the life of the power cycle. On SRAM-only boards
// the runtime heap routinely drops below g_persistBufBytes as connections
// accumulate, so the old per-save malloc() of this buffer would start failing
// and history would silently stop reaching flash (every reboot restored the
// same stale file). Owning it up front makes persistence independent of later
// heap pressure. nullptr if the boot reservation itself failed, in which case
// undoPersistHistory()/undoRestore() fall back to a per-call alloc (best
// effort, same as the old behavior).
uint8_t* g_persistBuf = nullptr;

// Snapshot ring (single, not per-slot - clear-all snapshots are tagged
// with the slot they were taken on via the SnapshotEntry.slot field).
SnapshotEntry* g_snapshots = nullptr;
size_t g_snapshotCap = 0;
size_t g_snapshotHead = 0;
uint32_t g_lastSnapshotTxnId = 0;
uint32_t g_lastSnapshotMs = 0;

// ============================================================================
// Blob arena - stores full-state captures for non-symmetric ops (clear_all)
// and for snapshots used as resync anchors.
//
// Each blob is the COMPLETE board state serialized as the same YAML a slot
// save writes (globalState.toYAML()), so a clear-all undo restores the entire
// board - bridges, colors, DAC/rail voltages, GPIO, paths - not just a subset.
// This is also what gets copied into /undo.hist, so persisted clear-all undo
// reconstructs everything.
//
// Format of each blob:
//   [u32 magic 'JLBS'][u32 size][u32 crc32][u32 reserved] -> 16-byte header
//   [char yaml[size]]                                       -> toYAML() output
//
// A typical board is ~0.5-2 KB of YAML.
// ============================================================================

constexpr uint32_t BLOB_MAGIC = 0x53424C4A;  // 'JLBS'

#pragma pack(push, 1)
struct BlobHeader {
    uint32_t magic;
    uint32_t size;     // body size only (not including header)
    uint32_t crc32;    // crc of body
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(BlobHeader) == 16, "BlobHeader sized wrong");

// Tiny CRC32 - matches the one in FileCache.cpp's journal code.
uint32_t blobCrc32(const uint8_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
    }
    return ~crc;
}

// Capture the full board state (as slot YAML) into a blob at *outOffset.
// Returns true on success and sets outOffset/outSize. Caller must hold no
// other locks.
bool blobAppendCurrentState(uint32_t* outOffset, uint32_t* outSize) {
    if (!g_blobs || g_blobCap == 0) return false;

    String yaml;
    if (!globalState.toYAML(yaml)) {
        UNDBG("blobAppendCurrentState: toYAML failed");
        return false;
    }
    size_t bodySize = yaml.length();
    size_t totalSize = sizeof(BlobHeader) + bodySize;
    size_t aligned = (totalSize + 3u) & ~size_t(3u);
    UNDBG("blobAppendCurrentState yaml=%u total=%u aligned=%u used=%u cap=%u",
          (unsigned)bodySize, (unsigned)totalSize, (unsigned)aligned,
          (unsigned)g_blobUsed, (unsigned)g_blobCap);

    if (aligned > g_blobCap) {
        UNDBG("  blob > arena cap, abort");
        return false;
    }
    if (g_blobUsed + aligned > g_blobCap) {
        UNDBG("  blob arena full - wrapping to 0");
        g_blobUsed = 0;
    }

    uint8_t* dst = g_blobs + g_blobUsed;
    BlobHeader* hdr = (BlobHeader*)dst;
    uint8_t* body = dst + sizeof(BlobHeader);
    memcpy(body, yaml.c_str(), bodySize);

    hdr->magic = BLOB_MAGIC;
    hdr->size = (uint32_t)bodySize;
    hdr->reserved = 0;
    hdr->crc32 = blobCrc32(body, bodySize);

    if (outOffset) *outOffset = (uint32_t)g_blobUsed;
    if (outSize) *outSize = (uint32_t)aligned;
    g_blobUsed += aligned;

    UNDBG("blob append OK off=%u size=%u (yaml %u bytes)",
          outOffset ? *outOffset : 0, outSize ? *outSize : 0, (unsigned)bodySize);
    return true;
}

// Restore globalState from a YAML blob and apply it. Returns true if the blob
// is valid and state was restored. globalState.fromYAML() self-suppresses the
// undo-record hooks for its whole duration, so this won't recurse into the
// log even if the caller forgot to set g_undoApplying. The caller (undoUndo /
// undoRedo) pushes the result to hardware via undoCommitToHardware().
bool blobRestoreState(uint32_t offset, uint32_t size) {
    // size 0 = the op was recorded WITHOUT a blob (the append failed at
    // record time). Offset 0 is also a real arena slot, so validating
    // whatever header happens to sit there passed magic+CRC on a STALE
    // earlier blob and silently replaced the user's board with an older
    // one (review finding). No blob, no restore - fail loudly instead.
    if (size == 0) return false;
    if (!g_blobs || (size_t)offset + sizeof(BlobHeader) > g_blobCap) return false;
    BlobHeader* hdr = (BlobHeader*)(g_blobs + offset);
    if (hdr->magic != BLOB_MAGIC) {
        UNDBG("blob restore FAIL bad magic 0x%08X at off=%u",
              (unsigned)hdr->magic, (unsigned)offset);
        return false;
    }
    if ((size_t)offset + sizeof(BlobHeader) + hdr->size > g_blobCap) {
        UNDBG("blob restore FAIL size overflow at off=%u", (unsigned)offset);
        return false;
    }
    uint8_t* body = g_blobs + offset + sizeof(BlobHeader);
    if (blobCrc32(body, hdr->size) != hdr->crc32) {
        UNDBG("blob restore FAIL crc mismatch at off=%u", (unsigned)offset);
        return false;
    }

    // Rebuild the YAML string (the arena copy isn't NUL-terminated) and apply.
    char* tmp = (char*)malloc((size_t)hdr->size + 1);
    if (!tmp) {
        UNDBG("blob restore FAIL OOM for %u bytes", (unsigned)hdr->size);
        return false;
    }
    memcpy(tmp, body, hdr->size);
    tmp[hdr->size] = '\0';
    String yaml(tmp);
    free(tmp);

    String err;
    bool ok = globalState.fromYAML(yaml, err);
    UNDBG("blob restore (yaml %u bytes) ok=%d", (unsigned)hdr->size, (int)ok);
    return ok;
}

}  // namespace

volatile bool g_undoApplying = false;

// =============================================================================
// Allocation helpers - PSRAM if available, SRAM heap otherwise.
// =============================================================================

namespace {

void* undoAlloc(size_t bytes) {
    void* p = psram_alloc(bytes);
    if (!p) p = malloc(bytes);
    return p;
}

void undoFree(void* p) {
    if (!p) return;
    // Decide whether the block is in PSRAM (0x11000000-0x11800000) or SRAM.
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    if (addr >= 0x11000000u && addr < 0x12000000u) {
        psram_free(p);
    } else {
        free(p);
    }
}

inline size_t opRingNext(size_t i) { return (i + 1) % g_opCap; }
inline size_t opRingDelta(size_t a, size_t b) {
    // # of ops from a to b in forward direction
    return (b + g_opCap - a) % g_opCap;
}
inline size_t txnRingNext(size_t i) { return (i + 1) % g_txnCap; }
inline size_t txnRingPrev(size_t i) { return (i + g_txnCap - 1) % g_txnCap; }

inline bool ringEmpty(size_t head, size_t tail) { return head == tail; }
inline size_t ringCount(size_t head, size_t tail, size_t cap) {
    return (head + cap - tail) % cap;
}

// One-past-the-last-op slot for transaction `txnIdx`. Used both when
// trimming the redo tail (we're about to drop everything past it) and
// when evicting the oldest txn (the new op tail starts at the next op).
inline size_t opEndOfTxn(size_t txnIdx) {
    const UndoTxn& t = g_txns[txnIdx];
    return (t.firstOp + t.opCount) % g_opCap;
}

}  // namespace

// =============================================================================
// Internal: applying / reverting a single op
// =============================================================================

namespace {

// Forward declaration - the snapshot ring is defined further down. The
// in-RAM blob ref is always live (no cross-reboot recovery in this
// build), but we keep the lookup as a fallback for OP_CLEAR_ALL whose
// blob got clobbered by arena pressure since the op was recorded.
bool restoreFromSnapshotByTxnId(uint32_t snapTxnId);

void applyOp(const UndoOp& op) {
    UNDBG("applyOp type=%u", (unsigned)op.type);
    g_undoApplying = true;
    String err;
    switch (op.type) {
        case UNDO_OP_CONNECT: {
            globalState.addConnection(op.bridge.n1, op.bridge.n2, err);
            break;
        }
        case UNDO_OP_DISCONNECT: {
            globalState.removeConnection(op.bridge.n1, op.bridge.n2, err);
            break;
        }
        case UNDO_OP_DAC_SET: {
            // Channel encoding: 0=DAC0, 1=DAC1, 2=top rail, 3=bottom rail.
            if (op.dac.ch >= 2) {
                globalState.setRailVoltage(op.dac.ch == 2, op.dac.next);
            } else {
                globalState.setDacVoltage(op.dac.ch, op.dac.next);
            }
            break;
        }
        case UNDO_OP_GPIO_SET:
        case UNDO_OP_GPIO_DIR:
        case UNDO_OP_SLOT_SWITCH:
            // TODO: hook GPIO/slot mutation paths to record prev/next.
            break;
        case UNDO_OP_CLEAR_ALL:
            // Re-applying a clear means clear again. Connections are wiped
            // without re-recording (we're inside applyOp, g_undoApplying
            // is true, so the hook short-circuits).
            globalState.connections.clear();
            break;
        case UNDO_OP_BLOB_REPLACE:
            blobRestoreState(op.blob.blobOffset, op.blob.blobSize);
            break;
        default:
            break;
    }
    g_undoApplying = false;
}

void revertOp(const UndoOp& op) {
    UNDBG("revertOp type=%u", (unsigned)op.type);
    g_undoApplying = true;
    String err;
    switch (op.type) {
        case UNDO_OP_CONNECT: {
            globalState.removeConnection(op.bridge.n1, op.bridge.n2, err);
            break;
        }
        case UNDO_OP_DISCONNECT: {
            globalState.addConnection(op.bridge.n1, op.bridge.n2, err);
            break;
        }
        case UNDO_OP_DAC_SET: {
            if (op.dac.ch >= 2) {
                globalState.setRailVoltage(op.dac.ch == 2, op.dac.prev);
            } else {
                globalState.setDacVoltage(op.dac.ch, op.dac.prev);
            }
            break;
        }
        case UNDO_OP_CLEAR_ALL:
        case UNDO_OP_BLOB_REPLACE:
            // The in-arena blob ref is always live - no flash recovery.
            // If it failed validation the op is unrecoverable; leave
            // state alone so history walking past this point still works.
            if (!blobRestoreState(op.blob.blobOffset, op.blob.blobSize)) {
                UNDBG("revert CLEAR_ALL: blob restore failed at off=%u",
                      (unsigned)op.blob.blobOffset);
                // the user pressed undo and nothing came back - say so
                // (blobSize 0 = the board was too big to snapshot when the
                // clear-all was recorded)
                Serial.println(op.blob.blobSize == 0
                    ? "\r\n[Undo] clear-all can't be undone: the board was too"
                      " large to snapshot when it was cleared"
                    : "\r\n[Undo] clear-all restore failed (snapshot damaged)");
            }
            break;
        case UNDO_OP_GPIO_SET:
        case UNDO_OP_GPIO_DIR:
        case UNDO_OP_SLOT_SWITCH:
            break;
        default:
            break;
    }
    g_undoApplying = false;
}

// Re-apply or revert an entire transaction (range of ops) given direction.
// dir = +1 forward (redo), -1 backward (undo).
void applyTxn(const UndoTxn& txn, int dir) {
    UNDBG("applyTxn dir=%d firstOp=%u opCount=%u", dir, (unsigned)txn.firstOp, (unsigned)txn.opCount);
    if (txn.opCount == 0) return;
    if (dir > 0) {
        for (uint16_t i = 0; i < txn.opCount; i++) {
            size_t ringIdx = (txn.firstOp + i) % g_opCap;
            applyOp(g_ops[ringIdx]);
        }
    } else {
        for (int i = (int)txn.opCount - 1; i >= 0; i--) {
            size_t ringIdx = (txn.firstOp + i) % g_opCap;
            revertOp(g_ops[ringIdx]);
        }
    }
    UNDBG("applyTxn DONE");
}

}  // namespace

// =============================================================================
// Decimation - mark waypoints based on age tier
// =============================================================================

namespace {

void recomputeWaypoints() {
    UNDBG("recomputeWaypoints ENTER cap=%u head=%u tail=%u",
          (unsigned)g_txnCap, (unsigned)g_txnHead, (unsigned)g_txnTail);
    // Position 0 = head; bigger positive = older.
    bool small = (g_txnCap < 256);
    size_t tierBoundaries[8];
    size_t tierSpacing[8];
    int nTiers;
    if (small) {
        nTiers = 3;
        tierBoundaries[0] = 16;  tierSpacing[0] = 1;
        tierBoundaries[1] = 48;  tierSpacing[1] = 2;
        tierBoundaries[2] = (size_t)-1; tierSpacing[2] = 4;
    } else {
        nTiers = 6;
        tierBoundaries[0] = 32;   tierSpacing[0] = 1;
        tierBoundaries[1] = 96;   tierSpacing[1] = 2;
        tierBoundaries[2] = 224;  tierSpacing[2] = 4;
        tierBoundaries[3] = 480;  tierSpacing[3] = 8;
        tierBoundaries[4] = 992;  tierSpacing[4] = 16;
        tierBoundaries[5] = (size_t)-1; tierSpacing[5] = 32;
    }
    size_t txnCount = ringCount(g_txnHead, g_txnTail, g_txnCap);
    UNDBG("recomputeWaypoints txnCount=%u", (unsigned)txnCount);
    for (size_t back = 0; back < txnCount; back++) {
        size_t idx = (g_txnHead + g_txnCap - 1 - back) % g_txnCap;
        UndoTxn& t = g_txns[idx];
        size_t spacing = tierSpacing[nTiers - 1];
        for (int i = 0; i < nTiers; i++) {
            if (back < tierBoundaries[i]) { spacing = tierSpacing[i]; break; }
        }
        if (back % spacing == 0) t.flags |= UNDO_TXN_WAYPOINT;
        else                     t.flags &= ~UNDO_TXN_WAYPOINT;
    }
    UNDBG("recomputeWaypoints DONE");
}

}  // namespace

// =============================================================================
// Snapshot ring storage - declared here so undoInit can call into it. The
// triggering policy (undoMaybeTakeSnapshot) and public C-linkage helpers
// live further down.
// =============================================================================

namespace {

constexpr int SNAPSHOT_RING_DEFAULT = 16;     // PSRAM mode
constexpr int SNAPSHOT_RING_SRAM = 0;         // SRAM-only: no in-RAM ring
// (previously: periodic SNAPSHOT_INTERVAL_TXNS / _MS triggers - removed.
// undoMaybeTakeSnapshot now only fires on explicit / slot_switch /
// clear_all reasons to avoid churn during probe sessions.)

struct SnapshotEntry {
    uint32_t blobOffset;
    uint32_t blobSize;
    uint32_t txnIndex;
    uint32_t timestampMs;
    uint8_t  slot;       // owning slot - clear-all restore is per-slot
    bool     valid;
};

// Cross-reboot helper: find the snapshot matching `snapTxnId` in the
// ring and restore globalState from its blob. Returns true on success.
// Called by revertOp(OP_CLEAR_ALL) when the in-arena blob ref is stale.
bool restoreFromSnapshotByTxnId(uint32_t snapTxnId) {
    if (!g_snapshots || g_snapshotCap == 0) return false;
    for (size_t i = 0; i < g_snapshotCap; i++) {
        if (g_snapshots[i].valid && g_snapshots[i].txnIndex == snapTxnId) {
            return blobRestoreState(g_snapshots[i].blobOffset,
                                     g_snapshots[i].blobSize);
        }
    }
    return false;
}

void snapshotInit() {
    bool havePsram = psram_available();
    // 16 entries on PSRAM, 0 on SRAM. The ring is shared across slots -
    // each entry remembers which slot it captured so clear-all restore
    // doesn't get confused by snapshots from other slots.
    g_snapshotCap = havePsram ? 16 : 0;
    if (g_snapshotCap == 0) {
        g_snapshots = nullptr;
        return;
    }
    g_snapshots = (SnapshotEntry*)undoAlloc(g_snapshotCap * sizeof(SnapshotEntry));
    if (g_snapshots) memset(g_snapshots, 0, g_snapshotCap * sizeof(SnapshotEntry));
}

void takeSnapshot(const char* reason) {
    UNDBG("takeSnapshot ENTER reason=%s snapshots=%p cap=%u",
          reason, (void*)g_snapshots, (unsigned)g_snapshotCap);
    if (!g_snapshots || g_snapshotCap == 0) return;
    // Never mutate the blob arena while we're applying/reverting - blobRestore
    // is reading from it and a fresh append could clobber the source offset.
    if (g_undoApplying) return;
    uint32_t off = 0, size = 0;
    if (!blobAppendCurrentState(&off, &size)) {
        UNDBG("snapshot capture FAILED (%s)", reason);
        return;
    }
    UNDBG("takeSnapshot blob ok, populating entry slot=%u", (unsigned)g_snapshotHead);
    SnapshotEntry& s = g_snapshots[g_snapshotHead];
    s.blobOffset = off;
    s.blobSize = size;
    s.txnIndex = g_globalTxnId;
    s.timestampMs = millis();
    s.slot = (g_activeSlot >= 0) ? (uint8_t)g_activeSlot : 0;
    s.valid = true;
    g_snapshotHead = (g_snapshotHead + 1) % g_snapshotCap;
    g_lastSnapshotTxnId = g_globalTxnId;
    g_lastSnapshotMs = s.timestampMs;
    UNDBG("snapshot captured (%s) slot=%u txn=%u",
          reason, (unsigned)((g_snapshotHead + g_snapshotCap - 1) % g_snapshotCap),
          (unsigned)g_globalTxnId);
    UNDBG("takeSnapshot DONE");
}

}  // namespace

// =============================================================================
// Cross-reboot persistence - bounded undo-history ring on flash
// =============================================================================
//
// Now that the SPIFTL delta-journal makes flash writes cheap, the undo
// history survives reboots. On every slot save we serialize the most-recent
// slice of the shared ring into /undo_history.txt (through the write-back
// cache, so it's an instant PSRAM memcpy that the background flush coalesces
// to flash). On boot undoInit() reads it back so undo/redo continue across
// power cycles.
//
// The file is itself a *bounded* ring: we only persist the newest
// UNDO_PERSIST_MAX_TXNS transactions (capped further at UNDO_PERSIST_MAX_BYTES
// of body), dropping the oldest history so the file never grows without
// bound. Obsolete (orphaned redo-tail) and empty transactions are skipped.
//
// Format: a human- and machine-readable line-oriented text file. Each op is a
// terse mnemonic + args so a quick `cat` reveals the history, e.g.:
//
//   JLUNDO 3                         <- magic + format version
//   gid 42                           <- monotonic global txn id at write time
//   slots 8 5 0 0 0 0 0 0 0          <- per-slot cursor (linear kept-txn index)
//   t 0 p                            <- txn header: slot, source (p=probe)
//     c 4-8                          <-   connect node 4 to node 8
//   t 0 p
//     tr 0 3.3                       <-   top rail prev=0 -> next=3.3 V
//   t 0 p
//     x @0                           <-   clear-all, restores blob pool entry 0
//   @0 412                           <- blob pool: index 0, 412-byte YAML body
//   <412 bytes of board-state YAML>
//
// The txn's display label (e.g. "connect 4-8") is NOT stored - it's redundant
// with the op line(s) and is rebuilt from them on restore. Source tags are
// single chars: p=probe m=menu y=python k=mcp f=file i=internal ?=unknown.
//
// Op mnemonics: c/d = connect/disconnect (node names round-trip through
// definesToChar/parseNodeName, falling back to the raw integer when a name is
// ambiguous); dac0/dac1/tr/br = DAC0/DAC1/top-rail/bottom-rail set
// "<prev> <next>"; gs/gd = GPIO value/direction; ss = slot switch;
// x/r = clear-all / blob-replace, each referencing a "@<idx>" pool entry that
// holds the full board state as YAML. Blob bodies are length-prefixed so they
// can contain newlines safely. There is intentionally no CRC: the file is
// meant to be hand-readable (and hand-editable); a corrupt/garbled file is
// rejected structurally and the device just starts with empty history.
//
// We do NOT persist the PSRAM snapshot ring: snapshots are purely capture-side
// resync anchors and OP_CLEAR_ALL carries its own blob ref (a full board YAML,
// which we copy into the file), so clear-all undo still restores the entire
// board after a reboot. A fresh boot snapshot is taken when there's nothing to
// restore.
//
// Persistence works on every board. With PSRAM we keep up to 256 states; with
// only SRAM the scratch buffer comes from the small heap so we keep the most
// recent 32 (g_persistMaxTxns, set in undoInit).

namespace {

constexpr const char* UNDO_FILE_PATH  = "/undo_history.txt";
constexpr const char* UNDO_FILE_MAGIC = "JLUNDO";
constexpr int         UNDO_FILE_VERSION = 3;   // v3: human-readable text format

// Compile-time ceilings the static scratch arrays are sized to. The *actual*
// run-time limits are g_persistMaxTxns / g_persistMaxBytes (set in undoInit
// from PSRAM availability) and are always <= these. Whichever run-time limit
// hits first wins; the single newest transaction is always included even if it
// alone exceeds the byte budget (it still has to fit the scratch buffer or
// persistence is skipped for that save).
constexpr size_t UNDO_PERSIST_MAX_TXNS  = 256;
constexpr size_t UNDO_PERSIST_MAX_BYTES = 64u * 1024u;

// Text header allowance (magic + gid + slots/cursor lines). Small and fixed.
constexpr size_t UNDO_TEXT_HEADER_BYTES = 128u + (size_t)NUM_SLOTS * 12u;

// Per-blob framing overhead in the text pool: "@<idx> <len>\n" plus the
// trailing "\n" after the body. Counted once per unique blob during budgeting.
constexpr size_t UNDO_BLOB_FRAME_BYTES = 24u;

// Fallback serialization scratch buffer size used when g_persistBufBytes is 0:
// header + body budget + slack so a single oversized newest txn (or blob)
// still fits without overflow.
constexpr size_t UNDO_PERSIST_BUF_BYTES =
    UNDO_TEXT_HEADER_BYTES + UNDO_PERSIST_MAX_BYTES + 8192u;

// ---- Bounded text writer ---------------------------------------------------
// One emit path serves two roles: measuring (buf == nullptr, just counts the
// bytes that *would* be written) and writing (buf != nullptr, bounded by cap).
// That keeps the byte budget used during txn selection exactly in sync with
// what we later emit, so the scratch buffer never overflows in practice.
struct TextWriter {
    char*  buf;       // nullptr => measure only
    size_t cap;
    size_t len;
    bool   overflow;
};

void twPrintf(TextWriter& w, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!w.buf) {
        int n = vsnprintf(nullptr, 0, fmt, ap);
        if (n > 0) w.len += (size_t)n;
    } else {
        size_t avail = (w.len < w.cap) ? (w.cap - w.len) : 0;
        int n = vsnprintf(w.buf + w.len, avail, fmt, ap);
        if (n < 0)                       { w.overflow = true; }
        else if ((size_t)n >= avail)     { w.overflow = true; w.len = w.cap; }
        else                             { w.len += (size_t)n; }
    }
    va_end(ap);
}

void twPutBytes(TextWriter& w, const char* data, size_t n) {
    if (!w.buf) { w.len += n; return; }
    if (w.len + n > w.cap) {
        size_t room = (w.len < w.cap) ? (w.cap - w.len) : 0;
        if (room) memcpy(w.buf + w.len, data, room);
        w.len = w.cap;
        w.overflow = true;
        return;
    }
    memcpy(w.buf + w.len, data, n);
    w.len += n;
}

// ---- UndoSource <-> single-char tag ----------------------------------------
// p=probe m=menu y=python k=mcp f=file i=internal ?=unknown. The parser also
// accepts the older long forms ("probe"/"py"/...) so pre-existing files load.
const char* undoSrcTag(uint8_t src) {
    switch (src) {
        case UNDO_SRC_PROBE:    return "p";
        case UNDO_SRC_MENU:     return "m";
        case UNDO_SRC_PYTHON:   return "y";
        case UNDO_SRC_MCP:      return "k";
        case UNDO_SRC_FILE:     return "f";
        case UNDO_SRC_INTERNAL: return "i";
        default:                return "?";
    }
}
uint8_t undoParseSrcTag(const char* s) {
    if (!strcmp(s, "p") || !strcmp(s, "probe"))           return UNDO_SRC_PROBE;
    if (!strcmp(s, "m") || !strcmp(s, "menu"))            return UNDO_SRC_MENU;
    if (!strcmp(s, "y") || !strcmp(s, "py") || !strcmp(s, "python")) return UNDO_SRC_PYTHON;
    if (!strcmp(s, "k") || !strcmp(s, "mcp"))             return UNDO_SRC_MCP;
    if (!strcmp(s, "f") || !strcmp(s, "file"))            return UNDO_SRC_FILE;
    if (!strcmp(s, "i") || !strcmp(s, "int") || !strcmp(s, "internal")) return UNDO_SRC_INTERNAL;
    return UNDO_SRC_UNKNOWN;
}

// Render a node as its friendly short name (GND / ADC_0 / 12 / ...). We only
// emit the name when it round-trips cleanly back through parseNodeName; for the
// handful of ambiguous names (duplicate short names across the nano/special
// tables) we fall back to the raw integer, which parseNodeName always parses
// exactly. The result is copied out of definesToChar's shared static buffer so
// a second call in the same expression can't clobber it.
void undoNodeName(int node, char* out, size_t outSize) {
    const char* p = definesToChar(node, 0);
    if (p && p[0] && parseNodeName(String(p)) == node) {
        strncpy(out, p, outSize - 1);
        out[outSize - 1] = '\0';
    } else {
        snprintf(out, outSize, "%d", node);
    }
}

// Validate the blob at arena offset `off`; on success return its YAML body
// pointer and length. A stale / clobbered ref returns nullptr so the caller
// neutralizes the referencing op (matching the live revert's best effort).
const uint8_t* undoBlobBody(uint32_t off, uint32_t* outBodyLen) {
    if (!g_blobs) return nullptr;
    if ((size_t)off + sizeof(BlobHeader) > g_blobCap) return nullptr;
    const BlobHeader* h = (const BlobHeader*)(g_blobs + off);
    if (h->magic != BLOB_MAGIC) return nullptr;
    if ((size_t)off + sizeof(BlobHeader) + h->size > g_blobCap) return nullptr;
    const uint8_t* body = g_blobs + off + sizeof(BlobHeader);
    if (blobCrc32(body, h->size) != h->crc32) return nullptr;
    if (outBodyLen) *outBodyLen = h->size;
    return body;
}

inline bool undoOpIsBlob(const UndoOp& op) {
    return op.type == UNDO_OP_CLEAR_ALL || op.type == UNDO_OP_BLOB_REPLACE;
}

// Emit one op line. poolIdx >= 0 substitutes a blob-pool reference for
// CLEAR_ALL / BLOB_REPLACE; poolIdx < 0 emits the op with no blob (neutralized).
void undoEmitOp(TextWriter& w, const UndoOp& op, int poolIdx) {
    switch (op.type) {
        case UNDO_OP_CONNECT:
        case UNDO_OP_DISCONNECT: {
            char a[20], b[20];
            undoNodeName(op.bridge.n1, a, sizeof(a));
            undoNodeName(op.bridge.n2, b, sizeof(b));
            twPrintf(w, "  %s %s-%s\n",
                     op.type == UNDO_OP_CONNECT ? "c" : "d", a, b);
            break;
        }
        case UNDO_OP_DAC_SET: {
            const char* m = nullptr;
            switch (op.dac.ch) {
                case 0: m = "dac0"; break;
                case 1: m = "dac1"; break;
                case 2: m = "tr";   break;
                case 3: m = "br";   break;
                default: break;
            }
            if (m) twPrintf(w, "  %s %g %g\n", m,
                            (double)op.dac.prev, (double)op.dac.next);
            else   twPrintf(w, "  dac %u %g %g\n", (unsigned)op.dac.ch,
                            (double)op.dac.prev, (double)op.dac.next);
            break;
        }
        case UNDO_OP_GPIO_SET:
            twPrintf(w, "  gs %u %u %u\n", (unsigned)op.gpio.pin,
                     (unsigned)op.gpio.prevVal, (unsigned)op.gpio.nextVal);
            break;
        case UNDO_OP_GPIO_DIR:
            twPrintf(w, "  gd %u %u %u\n", (unsigned)op.gpio.pin,
                     (unsigned)op.gpio.prevDir, (unsigned)op.gpio.nextVal);
            break;
        case UNDO_OP_SLOT_SWITCH:
            twPrintf(w, "  ss %u %u\n", (unsigned)op.slot.prev,
                     (unsigned)op.slot.next);
            break;
        case UNDO_OP_CLEAR_ALL:
            if (poolIdx >= 0) twPrintf(w, "  x @%d\n", poolIdx);
            else              twPrintf(w, "  x\n");
            break;
        case UNDO_OP_BLOB_REPLACE:
            if (poolIdx >= 0) twPrintf(w, "  r @%d\n", poolIdx);
            else              twPrintf(w, "  r\n");
            break;
        default:
            twPrintf(w, "  ?\n");
            break;
    }
}

// The txn label (e.g. "connect 6-13") is intentionally NOT written - it's fully
// redundant with the op line(s) below it. We rebuild it on restore from the ops.
void undoEmitTxnHeader(TextWriter& w, const UndoTxn& t) {
    twPrintf(w, "t %u %s\n", (unsigned)t.slot, undoSrcTag(t.source));
}

// Single source of truth for a txn's display label, used by BOTH the live
// record path (recordOneAction) and the restore-time rebuild path
// (undoRebuildLabel) so the two formats can never drift apart - previously they
// did (e.g. record wrote "top 5.00V" while restore rebuilt "TOP RAIL 5.00V"),
// which broke the OLED toast's action/detail split.
//
// Structure: a txn label is an ACTION (the verb/source - "connect", "Top Rail",
// "GPIO 1", "Clear All", ...) plus an optional DETAIL (the value/target -
// "8-GP 2", "5.00V", "HIGH", ...). Either half may contain spaces, so the two
// can't be recovered by splitting on a space. The combined string is stored as
// "action detail" (or just "action" when there's no detail) and the ACTION
// LENGTH is returned so callers (the OLED toast) know exactly where the action
// ends without re-parsing - the split point is authored here, not guessed.
uint8_t undoFormatOpLabel(const UndoOp& op, char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    out[0] = '\0';

    char action[16];
    char detail[28];
    action[0] = '\0';
    detail[0] = '\0';

    switch (op.type) {
        case UNDO_OP_CONNECT:
        case UNDO_OP_DISCONNECT: {
            strncpy(action, op.type == UNDO_OP_CONNECT ? "connect" : "disconnect",
                    sizeof(action) - 1);
            action[sizeof(action) - 1] = '\0';
            // definesToChar returns a pointer into a shared static buffer for
            // numeric nodes, so copy the first name out before fetching the 2nd.
            char a[20], b[20];
            const char* p1 = definesToChar(op.bridge.n1, 0);
            strncpy(a, (p1 && p1[0]) ? p1 : "?", sizeof(a) - 1); a[sizeof(a) - 1] = '\0';
            const char* p2 = definesToChar(op.bridge.n2, 0);
            strncpy(b, (p2 && p2[0]) ? p2 : "?", sizeof(b) - 1); b[sizeof(b) - 1] = '\0';
            snprintf(detail, sizeof(detail), "%s-%s", a, b);
            break;
        }
        case UNDO_OP_DAC_SET: {
            const char* name;
            switch (op.dac.ch) {
                case 0:  name = "DAC 0";    break;
                case 1:  name = "DAC 1";    break;
                case 2:  name = "Top Rail"; break;
                case 3:  name = "Bot Rail"; break;
                default: name = "DAC";      break;
            }
            strncpy(action, name, sizeof(action) - 1);
            action[sizeof(action) - 1] = '\0';
            snprintf(detail, sizeof(detail), "%.2fV", (double)op.dac.next);
            break;
        }
        case UNDO_OP_GPIO_SET:
            snprintf(action, sizeof(action), "GPIO %u", (unsigned)op.gpio.pin);
            strncpy(detail, op.gpio.nextVal ? "HIGH" : "LOW", sizeof(detail) - 1);
            detail[sizeof(detail) - 1] = '\0';
            break;
        case UNDO_OP_GPIO_DIR:
            snprintf(action, sizeof(action), "GPIO %u", (unsigned)op.gpio.pin);
            strncpy(detail, op.gpio.nextVal ? "OUTPUT" : "INPUT", sizeof(detail) - 1);
            detail[sizeof(detail) - 1] = '\0';
            break;
        case UNDO_OP_SLOT_SWITCH:
            strncpy(action, "slot", sizeof(action) - 1);
            action[sizeof(action) - 1] = '\0';
            snprintf(detail, sizeof(detail), "%u->%u",
                     (unsigned)op.slot.prev, (unsigned)op.slot.next);
            break;
        case UNDO_OP_CLEAR_ALL:
            // Whole phrase lives in the action; no detail half.
            strncpy(action, "Clear All", sizeof(action) - 1);
            action[sizeof(action) - 1] = '\0';
            break;
        case UNDO_OP_BLOB_REPLACE:
            strncpy(action, "replace", sizeof(action) - 1);
            action[sizeof(action) - 1] = '\0';
            break;
        default:
            break;
    }

    // Compose "action detail" (or just "action") and report where the action
    // ends so the toast can split without re-parsing.
    if (detail[0]) snprintf(out, outSize, "%s %s", action, detail);
    else           { strncpy(out, action, outSize - 1); out[outSize - 1] = '\0'; }

    size_t alen = strlen(action);
    if (alen > outSize - 1) alen = outSize - 1;   // clamp if action got cut
    return (uint8_t)alen;
}

// Txn-level label, layered over undoFormatOpLabel. Most txns are a single op
// and just defer to it, but some user gestures bundle several ops under one
// undo step and deserve a combined label that the per-op formatter can't see:
//   - A "Rails" menu change moves the top AND bottom rail in one txn (two DAC
//     ops on channels 2 and 3). Label it "Rails <v>" instead of the first op's
//     "Top Rail <v>". Because this is reconstructed from the op pair, the live
//     label and the post-reboot rebuilt label match (the bug this fixes: live
//     showed the menu's "rails 3.3V" while restore rebuilt "Top Rail 3.3V").
// Returns the action length (split index), same contract as undoFormatOpLabel.
uint8_t undoFormatTxnLabel(const UndoTxn& t, char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    out[0] = '\0';
    if (t.opCount == 0) return 0;

    if (t.opCount >= 2) {
        const UndoOp& o0 = g_ops[t.firstOp % g_opCap];
        const UndoOp& o1 = g_ops[(t.firstOp + 1) % g_opCap];
        if (o0.type == UNDO_OP_DAC_SET && o1.type == UNDO_OP_DAC_SET &&
            ((o0.dac.ch == 2 && o1.dac.ch == 3) ||
             (o0.dac.ch == 3 && o1.dac.ch == 2))) {
            snprintf(out, outSize, "Rails %.2fV", (double)o0.dac.next);
            size_t alen = 5;   // "Rails"
            if (alen > outSize - 1) alen = outSize - 1;
            return (uint8_t)alen;
        }
    }
    return undoFormatOpLabel(g_ops[t.firstOp % g_opCap], out, outSize);
}

// Rebuild a txn's display label from its ops. Used on restore since the label
// is no longer stored in the file. Delegates to undoFormatTxnLabel so record +
// restore stay in lockstep.
void undoRebuildLabel(UndoTxn& t) {
    t.label[0] = '\0';
    if (t.opCount == 0) return;
    undoFormatTxnLabel(t, t.label, sizeof(t.label));
}

// Are there any committed, non-obsolete txns we'd want to persist? Used by
// the persist path to tell "nothing to save" (delete the stale file) apart
// from "serialize produced 0 bytes despite live history" (keep the old file).
// FILE-CONTEXT UNDO IS SESSION-ONLY. Transactions tagged with the shared file
// bucket are never written to /undo.hist: undoDeserialize clamps any
// slot >= NUM_SLOTS to 0, so a persisted file-context txn would replay into
// slot 0's history (on this firmware and, worse, on older firmware). This
// predicate has to agree with undoSerialize's selection loop below - if only
// one of them skipped the bucket you would get either persisted -2 txns or a
// permanent dirty-retry loop when bucket-8 history is all that exists.
static inline bool undoTxnIsPersistable(const UndoTxn& t) {
    if (t.opCount == 0) return false;
    if (t.flags & UNDO_TXN_OBSOLETE) return false;
    if (t.slot == (uint8_t)UNDO_FILE_BUCKET) return false;
    return true;
}

bool undoHasPersistableTxns(void) {
    if (!g_ops || !g_txns) return false;
    size_t pos = g_txnTail;
    while (pos != g_txnHead) {
        const UndoTxn& t = g_txns[pos];
        if (undoTxnIsPersistable(t)) return true;
        pos = (pos + 1) % g_txnCap;
    }
    return false;
}

// Serialize the live committed ring into `buf` (size `cap`) as the text format
// documented above. Returns the byte count written, or 0 if there's nothing to
// persist / the buffer overflowed.
size_t undoSerialize(uint8_t* buf, size_t cap) {
    if (!g_ops || !g_txns) return 0;
    if (cap < UNDO_TEXT_HEADER_BYTES) return 0;

    // Ring distance from the global tail (monotonic ordering helper).
    auto dist = [](size_t x) -> size_t {
        return (x + g_txnCap - g_txnTail) % g_txnCap;
    };

    // --- Select the newest reachable txns within the count + byte budget,
    //     walking backward from head. keptIdx[] holds them newest-first. The
    //     byte budget is measured in *text* bytes so it matches the file 1:1.
    static size_t keptIdx[UNDO_PERSIST_MAX_TXNS];
    static uint32_t budgetBlobOff[UNDO_PERSIST_MAX_TXNS];
    size_t nKept = 0;
    size_t nBudgetBlob = 0;
    size_t bytes = 0;
    size_t maxTxns  = g_persistMaxTxns  ? g_persistMaxTxns  : UNDO_PERSIST_MAX_TXNS;
    size_t maxBytes = g_persistMaxBytes ? g_persistMaxBytes : UNDO_PERSIST_MAX_BYTES;
    if (maxTxns > UNDO_PERSIST_MAX_TXNS) maxTxns = UNDO_PERSIST_MAX_TXNS;
    // [undo] max_saved_actions caps the persisted slice below the
    // PSRAM/SRAM budget picked in undoInit (the RAM ring is unaffected).
    {
        int cfgMax = jumperlessConfig.undo.max_saved_actions;
        if (cfgMax >= 1 && (size_t)cfgMax < maxTxns) maxTxns = (size_t)cfgMax;
    }
    {
        size_t pos = g_txnHead;
        while (pos != g_txnTail && nKept < maxTxns) {
            pos = (pos + g_txnCap - 1) % g_txnCap;
            const UndoTxn& t = g_txns[pos];
            if (undoTxnIsPersistable(t)) {
                // Measure this txn's text (header + op lines, no blob bodies).
                TextWriter mw{nullptr, 0, 0, false};
                undoEmitTxnHeader(mw, t);
                for (uint16_t i = 0; i < t.opCount; i++) {
                    const UndoOp& op = g_ops[(t.firstOp + i) % g_opCap];
                    undoEmitOp(mw, op, undoOpIsBlob(op) ? 0 : -1);
                }
                size_t add = mw.len;
                // Blob bodies new to this txn are counted once per unique offset.
                size_t blobAdd = 0;
                for (uint16_t i = 0; i < t.opCount; i++) {
                    const UndoOp& op = g_ops[(t.firstOp + i) % g_opCap];
                    if (!undoOpIsBlob(op) || op.blob.blobSize == 0) continue;
                    uint32_t off = op.blob.blobOffset;
                    bool seen = false;
                    for (size_t k = 0; k < nBudgetBlob; k++)
                        if (budgetBlobOff[k] == off) { seen = true; break; }
                    if (seen) continue;
                    if (nBudgetBlob < UNDO_PERSIST_MAX_TXNS)
                        budgetBlobOff[nBudgetBlob++] = off;
                    uint32_t bodyLen = 0;
                    if (undoBlobBody(off, &bodyLen))
                        blobAdd += UNDO_BLOB_FRAME_BYTES + bodyLen;
                }
                if (nKept > 0 && bytes + add + blobAdd > maxBytes) break;
                keptIdx[nKept++] = pos;
                bytes += add + blobAdd;
            }
            if (pos == g_txnTail) break;
        }
    }

    if (nKept == 0) return 0;  // nothing reachable to persist

    // --- Emit. Header first, then txns oldest-first, then the blob pool. ----
    TextWriter w{(char*)buf, cap, 0, false};
    twPrintf(w, "%s %d\n", UNDO_FILE_MAGIC, UNDO_FILE_VERSION);
    twPrintf(w, "gid %u\n", (unsigned)g_globalTxnId);

    // Per-slot cursors as linear txn indices (count of kept txns strictly older
    // than the cursor; cursor==head therefore maps to nKept, i.e. "live").
    twPrintf(w, "slots %d", NUM_SLOTS);
    for (int s = 0; s < NUM_SLOTS; s++) {
        size_t dC = dist(g_slotCursor[s]);
        size_t cnt = 0;
        for (size_t k = 0; k < nKept; k++)
            if (dist(keptIdx[k]) < dC) cnt++;
        twPrintf(w, " %u", (unsigned)cnt);
    }
    twPrintf(w, "\n");

    // Emit txns oldest-first, assigning blob-pool indices on first sight.
    static uint32_t poolOff[UNDO_PERSIST_MAX_TXNS];
    size_t nPool = 0;
    for (int k = (int)nKept - 1; k >= 0; k--) {
        const UndoTxn& t = g_txns[keptIdx[k]];
        undoEmitTxnHeader(w, t);
        for (uint16_t i = 0; i < t.opCount; i++) {
            const UndoOp& op = g_ops[(t.firstOp + i) % g_opCap];
            int poolIdx = -1;
            if (undoOpIsBlob(op) && op.blob.blobSize != 0 &&
                undoBlobBody(op.blob.blobOffset, nullptr)) {
                for (size_t pi = 0; pi < nPool; pi++)
                    if (poolOff[pi] == op.blob.blobOffset) { poolIdx = (int)pi; break; }
                if (poolIdx < 0 && nPool < UNDO_PERSIST_MAX_TXNS) {
                    poolOff[nPool] = op.blob.blobOffset;
                    poolIdx = (int)nPool++;
                }
            }
            undoEmitOp(w, op, poolIdx);
        }
    }

    // Emit the blob pool (idx order == assignment order). Each body is length-
    // prefixed so it can contain newlines without ambiguity.
    for (size_t pi = 0; pi < nPool; pi++) {
        uint32_t bodyLen = 0;
        const uint8_t* body = undoBlobBody(poolOff[pi], &bodyLen);
        if (!body) { twPrintf(w, "@%u 0\n\n", (unsigned)pi); continue; }
        twPrintf(w, "@%u %u\n", (unsigned)pi, (unsigned)bodyLen);
        twPutBytes(w, (const char*)body, bodyLen);
        twPrintf(w, "\n");
    }

    if (w.overflow) {
        // Buffer too small (e.g. a pathological newest txn). Don't truncate the
        // file; report 0 and let the persist path keep the previous good file.
        Serial.printf("[Undo] serialize OVERFLOW len=%u cap=%u - not saving\n",
                      (unsigned)w.len, (unsigned)cap);
        return 0;
    }

    UNDBG("serialize txns=%u pool=%u bytes=%u",
          (unsigned)nKept, (unsigned)nPool, (unsigned)w.len);
    return w.len;
}

// Rebuild the live ring from a serialized (text) image. Returns true if any
// history was restored (txnCount > 0). Parsing is deliberately tolerant: a
// malformed line is skipped rather than aborting the whole restore, and a
// missing blob just neutralizes its op (state left alone on that step).
bool undoDeserialize(const uint8_t* buf, size_t len) {
    if (!g_ops || !g_txns) return false;
    if (len < 8) return false;

    const char* p   = (const char*)buf;
    const char* end = p + len;

    // Copy the next line (sans newline / CR) into `out`; advance `p`. After a
    // header line `p` is left at the first byte of any length-prefixed body so
    // blob bodies can be read directly by byte count.
    auto nextLine = [&](char* out, size_t outCap) -> bool {
        if (p >= end) return false;
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        size_t lineLen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        size_t copy = (lineLen < outCap - 1) ? lineLen : (outCap - 1);
        memcpy(out, p, copy);
        out[copy] = '\0';
        if (copy > 0 && out[copy - 1] == '\r') out[copy - 1] = '\0';
        p = nl ? (nl + 1) : end;
        return true;
    };

    char line[192];

    // ---- header: "JLUNDO <version>" ----
    if (!nextLine(line, sizeof(line))) return false;
    {
        char magic[16] = {0};
        int ver = 0;
        if (sscanf(line, "%15s %d", magic, &ver) != 2 ||
            strcmp(magic, UNDO_FILE_MAGIC) != 0 || ver != UNDO_FILE_VERSION) {
            Serial.printf("[Undo] restore REJECT: bad header '%s' (want %s %d)\n",
                          line, UNDO_FILE_MAGIC, UNDO_FILE_VERSION);
            return false;
        }
    }

    // ---- "gid <n>" ----
    uint32_t gid = 0;
    if (!nextLine(line, sizeof(line)) || sscanf(line, "gid %u", &gid) != 1) {
        Serial.printf("[Undo] restore REJECT: expected gid line, got '%s'\n", line);
        return false;
    }

    // ---- "slots <N> <c0> <c1> ..." ----
    uint32_t fileCursors[NUM_SLOTS];
    for (int s = 0; s < NUM_SLOTS; s++) fileCursors[s] = 0;
    int fileNumSlots = 0;
    if (!nextLine(line, sizeof(line)) || strncmp(line, "slots", 5) != 0) {
        Serial.printf("[Undo] restore REJECT: expected slots line, got '%s'\n", line);
        return false;
    }
    {
        char* eptr = nullptr;
        fileNumSlots = (int)strtol(line + 5, &eptr, 10);
        for (int i = 0; i < fileNumSlots && i < NUM_SLOTS; i++) {
            char* s = eptr;
            long c = strtol(s, &eptr, 10);
            if (eptr == s) break;
            fileCursors[i] = (uint32_t)(c < 0 ? 0 : c);
        }
    }

    // ---- body ----
    size_t txnCount = 0, opCount = 0;
    g_blobUsed = 0;

    struct BlobRef { uint32_t opIdx; int poolIdx; };
    static BlobRef  refs[UNDO_PERSIST_MAX_TXNS];
    size_t nRefs = 0;
    static uint32_t poolArenaOff[UNDO_PERSIST_MAX_TXNS];
    static uint32_t poolArenaSize[UNDO_PERSIST_MAX_TXNS];
    static bool     poolSet[UNDO_PERSIST_MAX_TXNS];
    for (size_t i = 0; i < UNDO_PERSIST_MAX_TXNS; i++) poolSet[i] = false;

    bool haveTxn = false;  // a txn header has been seen but not yet finalized
    auto finalizeTxn = [&]() {
        if (haveTxn) {
            undoRebuildLabel(g_txns[txnCount]);  // label isn't stored; derive it
            txnCount++;
            haveTxn = false;
        }
    };

    while (nextLine(line, sizeof(line))) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') continue;  // blank line

        // ---- transaction header ----
        if (s[0] == 't' && s[1] == ' ') {
            finalizeTxn();
            if (txnCount >= g_txnCap - 1) break;  // ring full; keep what we have
            UndoTxn& t = g_txns[txnCount];
            memset(&t, 0, sizeof(t));
            t.firstOp = (uint32_t)opCount;
            // "t <slot> <src>"; any trailing tokens (e.g. a legacy quoted label)
            // are ignored - the label is rebuilt from the ops on finalize.
            unsigned slot = 0;
            char src[12] = {0};
            sscanf(s + 1, " %u %11s", &slot, src);
            t.slot   = (uint8_t)(slot < (unsigned)NUM_SLOTS ? slot : 0);
            t.source = undoParseSrcTag(src);
            t.flags  = UNDO_TXN_COMMITTED;
            if (t.source != UNDO_SRC_INTERNAL) t.flags |= UNDO_TXN_USER_INITIATED;
            haveTxn = true;
            continue;
        }

        // ---- blob pool entry: "@<idx> <len>" then <len> raw body bytes ----
        if (s[0] == '@') {
            finalizeTxn();
            unsigned idx = 0, blen = 0;
            if (sscanf(s + 1, "%u %u", &idx, &blen) != 2) continue;
            // Subtraction cannot wrap; `p + blen > end` did, so a corrupt or
            // hostile length (e.g. 4294967295) passed the check and turned
            // into a ~4GB memcpy at every boot (sweep finding, medium).
            if (blen > (size_t)(end - p)) break;  // truncated / bogus body
            const uint8_t* body = (const uint8_t*)p;
            p += blen;
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
            if (idx >= UNDO_PERSIST_MAX_TXNS) continue;
            if (blen == 0) {
                poolSet[idx] = true; poolArenaOff[idx] = 0; poolArenaSize[idx] = 0;
                continue;
            }
            size_t total   = sizeof(BlobHeader) + blen;
            size_t aligned  = (total + 3u) & ~size_t(3u);
            if (g_blobUsed + aligned > g_blobCap) continue;  // no room; leave unset
            uint8_t* dst = g_blobs + g_blobUsed;
            BlobHeader* h = (BlobHeader*)dst;
            h->magic = BLOB_MAGIC; h->size = blen; h->reserved = 0;
            memcpy(dst + sizeof(BlobHeader), body, blen);
            h->crc32 = blobCrc32(dst + sizeof(BlobHeader), blen);
            poolArenaOff[idx]  = (uint32_t)g_blobUsed;
            poolArenaSize[idx] = (uint32_t)aligned;
            poolSet[idx]       = true;
            g_blobUsed += aligned;
            continue;
        }

        // ---- op line (within the current txn) ----
        if (!haveTxn) continue;
        if (opCount >= g_opCap - 1) continue;  // op ring full; skip extra ops
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.txnIdx = (uint16_t)(txnCount & 0xFFFF);

        char mnem[8] = {0};
        const char* rest = s;
        int mi = 0;
        while (*rest && *rest != ' ' && mi < (int)sizeof(mnem) - 1) mnem[mi++] = *rest++;
        mnem[mi] = '\0';
        while (*rest == ' ') rest++;

        bool ok = true;
        if (!strcmp(mnem, "c") || !strcmp(mnem, "d")) {
            op.type = (mnem[0] == 'c') ? UNDO_OP_CONNECT : UNDO_OP_DISCONNECT;
            const char* dash = strchr(rest, '-');
            if (!dash) { ok = false; }
            else {
                char a[24], b[24];
                size_t al = (size_t)(dash - rest);
                if (al > sizeof(a) - 1) al = sizeof(a) - 1;
                memcpy(a, rest, al); a[al] = '\0';
                strncpy(b, dash + 1, sizeof(b) - 1); b[sizeof(b) - 1] = '\0';
                for (int bi = (int)strlen(b) - 1; bi >= 0 && b[bi] == ' '; bi--) b[bi] = '\0';
                op.bridge.n1 = (int16_t)parseNodeName(String(a));
                op.bridge.n2 = (int16_t)parseNodeName(String(b));
                op.bridge.color = 0;
            }
        } else if (!strcmp(mnem, "dac0") || !strcmp(mnem, "dac1") ||
                   !strcmp(mnem, "tr")   || !strcmp(mnem, "br")) {
            op.type = UNDO_OP_DAC_SET;
            op.dac.ch = !strcmp(mnem, "dac0") ? 0 :
                        !strcmp(mnem, "dac1") ? 1 :
                        !strcmp(mnem, "tr")   ? 2 : 3;
            float pv = 0, nv = 0;
            sscanf(rest, "%f %f", &pv, &nv);
            op.dac.prev = pv; op.dac.next = nv;
        } else if (!strcmp(mnem, "dac")) {
            op.type = UNDO_OP_DAC_SET;
            unsigned ch = 0; float pv = 0, nv = 0;
            sscanf(rest, "%u %f %f", &ch, &pv, &nv);
            op.dac.ch = (uint8_t)ch; op.dac.prev = pv; op.dac.next = nv;
        } else if (!strcmp(mnem, "gs")) {
            op.type = UNDO_OP_GPIO_SET;
            unsigned pin = 0, pv = 0, nv = 0;
            sscanf(rest, "%u %u %u", &pin, &pv, &nv);
            op.gpio.pin = (uint8_t)pin; op.gpio.prevVal = (uint8_t)pv; op.gpio.nextVal = (uint8_t)nv;
        } else if (!strcmp(mnem, "gd")) {
            op.type = UNDO_OP_GPIO_DIR;
            unsigned pin = 0, pd = 0, nd = 0;
            sscanf(rest, "%u %u %u", &pin, &pd, &nd);
            op.gpio.pin = (uint8_t)pin; op.gpio.prevDir = (uint8_t)pd; op.gpio.nextVal = (uint8_t)nd;
        } else if (!strcmp(mnem, "ss")) {
            op.type = UNDO_OP_SLOT_SWITCH;
            unsigned pv = 0, nv = 0;
            sscanf(rest, "%u %u", &pv, &nv);
            op.slot.prev = (uint8_t)pv; op.slot.next = (uint8_t)nv;
        } else if (!strcmp(mnem, "x") || !strcmp(mnem, "r")) {
            op.type = (mnem[0] == 'x') ? UNDO_OP_CLEAR_ALL : UNDO_OP_BLOB_REPLACE;
            const char* at = strchr(rest, '@');
            if (at && nRefs < UNDO_PERSIST_MAX_TXNS) {
                int pidx = atoi(at + 1);
                if (pidx >= 0 && pidx < (int)UNDO_PERSIST_MAX_TXNS) {
                    refs[nRefs].opIdx   = (uint32_t)opCount;
                    refs[nRefs].poolIdx = pidx;
                    nRefs++;
                }
            }
            // blobOffset/Size stay 0 until resolved (or remain neutralized)
        } else {
            ok = false;
        }

        if (!ok) continue;
        g_ops[opCount] = op;
        opCount++;
        g_txns[txnCount].opCount++;
    }
    finalizeTxn();

    // Resolve deferred blob references now that the pool is fully parsed.
    for (size_t i = 0; i < nRefs; i++) {
        int pidx = refs[i].poolIdx;
        uint32_t oi = refs[i].opIdx;
        if (oi >= opCount) continue;
        if (pidx >= 0 && pidx < (int)UNDO_PERSIST_MAX_TXNS && poolSet[pidx]) {
            g_ops[oi].blob.blobOffset = poolArenaOff[pidx];
            g_ops[oi].blob.blobSize   = poolArenaSize[pidx];
        }
        // else: missing blob -> op stays neutralized (offset/size 0)
    }

    if (txnCount >= g_txnCap) txnCount = g_txnCap - 1;
    g_txnTail = 0;
    g_txnHead = txnCount;   // < g_txnCap, never the full sentinel
    g_opTail = 0;
    g_opHead = opCount;
    g_globalTxnId = gid;

    // Reconstruct a plausible per-txn globalId sequence (newest == gid). These
    // are informational; the live counter (g_globalTxnId) drives new ids.
    for (size_t i = 0; i < txnCount; i++) {
        g_txns[i].startMs  = 0;
        g_txns[i].globalId = (gid >= txnCount) ? (uint32_t)(gid - txnCount + 1 + i)
                                               : (uint32_t)(i + 1);
    }

    // Cursors are linear txn indices; with tail==0 that's also the ring index.
    // The FILE bucket is not in the file (its txns are never persisted), so it
    // starts live at head - anything else would leave it pointing into another
    // slot's restored history.
    g_slotCursor[UNDO_FILE_BUCKET] = g_txnHead;
    for (int s = 0; s < NUM_SLOTS; s++) {
        if (s < fileNumSlots) {
            uint32_t v = fileCursors[s];
            if (v > (uint32_t)txnCount) v = (uint32_t)txnCount;
            g_slotCursor[s] = v;
        } else {
            g_slotCursor[s] = g_txnHead;
        }
    }

    g_waypointsDirty = true;
    g_historyDirty = false;
    UNDBG("restore OK txns=%u ops=%u blob=%u gid=%u",
          (unsigned)txnCount, (unsigned)opCount, (unsigned)g_blobUsed, (unsigned)gid);
    return txnCount > 0;
}

}  // namespace

// =============================================================================
// Public lifecycle
// =============================================================================

// =============================================================================
// Slot-aware ring helpers
// =============================================================================
// The ring is shared, so undo/redo for slot S must skip txns that belong
// to other slots and txns that we marked obsolete (via the redo-trim
// in undoBeginTxn). These walks are O(ringSize) worst case, but the
// constant is tiny and they only run on user-pressed undo / redo.

namespace {

// Is this txn slot a candidate for slot S's history?
inline bool txnBelongsToSlot(const UndoTxn& t, int slot) {
    if (t.opCount == 0) return false;
    if (t.flags & UNDO_TXN_OBSOLETE) return false;
    return t.slot == (uint8_t)slot;
}

// Walk backward from cursor (exclusive) toward tail; return ring index of
// the first txn matching slot S, or SIZE_MAX if none.
size_t findPrevSlotTxn(int slot, size_t cursor) {
    if (g_txnHead == g_txnTail) return SIZE_MAX;  // empty
    if (cursor == g_txnTail) return SIZE_MAX;     // already at oldest
    size_t pos = cursor;
    while (pos != g_txnTail) {
        pos = (pos + g_txnCap - 1) % g_txnCap;
        if (txnBelongsToSlot(g_txns[pos], slot)) return pos;
        if (pos == g_txnTail) break;
    }
    return SIZE_MAX;
}

// Walk forward from cursor (inclusive) toward head; return ring index of
// the first txn matching slot S, or SIZE_MAX if none.
size_t findNextSlotTxn(int slot, size_t cursor) {
    if (g_txnHead == g_txnTail) return SIZE_MAX;
    size_t pos = cursor;
    while (pos != g_txnHead) {
        if (txnBelongsToSlot(g_txns[pos], slot)) return pos;
        pos = (pos + 1) % g_txnCap;
    }
    return SIZE_MAX;
}

// Snap a slot's cursor back into [tail, head] if eviction has dragged
// the global tail past it. If the cursor was pointing at an evicted txn
// it loses meaning; treat that as "you're now at the oldest end of your
// (now-shrunken) history." Cheap O(1) check via ringCount.
void clampSlotCursor(int slot) {
    size_t& c = g_slotCursor[slot];
    if (g_txnHead == g_txnTail) { c = g_txnHead; return; }
    // c is valid if the forward distance from tail to c is <= forward
    // distance from tail to head. Otherwise it's behind the new tail.
    size_t fromTailToCursor = (c + g_txnCap - g_txnTail) % g_txnCap;
    size_t fromTailToHead   = (g_txnHead + g_txnCap - g_txnTail) % g_txnCap;
    if (fromTailToCursor > fromTailToHead) c = g_txnTail;
}

}  // namespace

// =============================================================================
// Active-slot swap (lightweight - no allocation)
// =============================================================================

namespace {

// Make `slot` the active slot for new recordings. If a txn is open for a
// different slot, close it first so its ops land in the right history.
// No allocation - the ring is shared.
//
// We call undoEndTxn() directly here. It's a C-linkage public API but
// that's fine - the open txn is keyed off g_openTxnSlot, which was set
// when the txn opened, so closing here lands the ops under the correct
// (old) slot tag before we flip g_activeSlot to the new one.
void slotSwap(int slot) {
    if (slot < 0 || slot > UNDO_FILE_BUCKET) return;
    if (g_activeSlot == slot) return;
    if (g_inTxn) undoEndTxn();
    g_activeSlot = slot;
}

// Map a global netSlot value to a cursor bucket. ONLY SLOT_FILE_CONTEXT maps
// to the shared file bucket; netSlot == 8 (the apps' temporary slot) keeps its
// existing "ignored, out of range" behavior, and the defcon sentinels
// (-1 / NUM_SLOTS) are ignored as before.
inline int undoBucketForNetSlot(int slot) {
    if (slot == SLOT_FILE_CONTEXT) return UNDO_FILE_BUCKET;
    if (slot < 0 || slot >= NUM_SLOTS) return -1;
    return slot;
}

// Auto-detect: if `netSlot` differs from g_activeSlot, swap. Called at
// the top of every public API entry point.
inline void slotSwapIfNeeded() {
    if (g_activeSlot < 0) return;  // not initialized
    int bucket = undoBucketForNetSlot(netSlot);
    if (bucket < 0) return;
    if (bucket == g_activeSlot) return;
    slotSwap(bucket);
}

}  // namespace

// =============================================================================
// Public lifecycle
// =============================================================================

extern "C" {

void undoInit(void) {
    if (g_ops) return;  // already initialized

#if defined(OG_JUMPERLESS)
    // Undo is disabled on the OG (RP2040): its no-PSRAM ring + persist scratch
    // (~21 KB) is unaffordable on the ~71 KB heap shared with FatFS/MicroPython,
    // and it fragments the heap enough that later file opens / printfs abort.
    // Leaving g_ops/g_blobs null makes all the undo record hooks no-op (they
    // null-check), exactly like the "allocation failed" path below. Undo is a
    // nice-to-have, not required for the LLM/MicroPython control goal.
    return;
#endif

    bool havePsram = psram_available();
    if (havePsram) {
        // Default sizes: ops 16384, txns 4096, blobs 256KB. The ring is
        // shared across all 8 slots - each entry carries a 1-byte slot
        // tag so undo/redo only walks matching txns. No per-slot
        // allocation, no failure modes when slots come online.
        g_opCap = 16384;
        g_txnCap = 4096;
        g_blobCap = 256 * 1024;
    } else {
        // Ops/txns halved in the RAM reclamation pass (was 512/128): history
        // is shallower, and every ring is a power of two as the wrap math
        // requires. Blobs are 8 KB, NOT the pass's 2 KB: a clear-all blob is
        // a full toYAML capture, and with parts placed that is ~4.7 KB on
        // the reference bench (8 parts) - at 2 KB the append aborted every
        // time and undo-of-clear-all silently restored nothing. 8 KB holds
        // one parts-bearing board comfortably; a bridge-maxed 16-part board
        // can still exceed it, and then the clear-all op records no blob
        // and its undo refuses loudly (blobRestoreState size==0).
        g_opCap = 256;
        g_txnCap = 64;
        g_blobCap = 8 * 1024;
    }

    // Persistence limits. With PSRAM we keep a deep history; without it the
    // serialization scratch buffer + /undo.hist body live in SRAM. SRAM-only
    // builds run with the heap nearly full (often <15 KB free), so the old
    // ~12 KB scratch buffer routinely failed to allocate and the save was
    // silently dropped (you'd just keep restoring the previous file). Keep the
    // no-PSRAM budget intentionally small so the scratch buffer reliably fits;
    // a handful of undo steps is all that's expected without PSRAM anyway.
    if (havePsram) {
        g_persistMaxTxns  = 256;
        g_persistMaxBytes = 64u * 1024u;
    } else {
        g_persistMaxTxns  = 24;
        g_persistMaxBytes = 3u * 1024u;
    }
    // Slack covers one oversized newest txn/blob beyond the body budget. A
    // clear-all blob is a full-board YAML capture (<2 KB), so 2 KB is plenty on
    // the no-PSRAM path while keeping the total scratch buffer ~5 KB.
    size_t persistSlack = havePsram ? 4096u : 2048u;
    g_persistBufBytes = UNDO_TEXT_HEADER_BYTES + g_persistMaxBytes + persistSlack;

    auto isPow2 = [](size_t n) { return n != 0 && (n & (n - 1)) == 0; };
    if (!isPow2(g_opCap) || !isPow2(g_txnCap)) {
        Serial.println("[Undo] FATAL: ring caps must be powers of two");
        g_opCap = g_txnCap = g_blobCap = 0;
        return;
    }

    g_ops = (UndoOp*)undoAlloc(g_opCap * sizeof(UndoOp));
    g_txns = (UndoTxn*)undoAlloc(g_txnCap * sizeof(UndoTxn));
    g_blobs = (uint8_t*)undoAlloc(g_blobCap);

    if (!g_ops || !g_txns || !g_blobs) {
        undoFree(g_ops); undoFree(g_txns); undoFree(g_blobs);
        g_ops = nullptr; g_txns = nullptr; g_blobs = nullptr;
        g_opCap = g_txnCap = g_blobCap = 0;
        Serial.println("[Undo] allocation failed - undo disabled");
        return;
    }

    memset(g_ops, 0, g_opCap * sizeof(UndoOp));
    memset(g_txns, 0, g_txnCap * sizeof(UndoTxn));
    memset(g_blobs, 0, g_blobCap);

    // Reserve the persistence scratch buffer now, while the heap is emptiest.
    // See g_persistBuf's declaration: a per-save malloc of this ~5 KB on an
    // SRAM-only board fails once enough connections have eaten the heap, and a
    // failed persist silently leaves the old file on flash (the "always
    // restores the same state" bug). Grabbing it up front makes saves
    // heap-pressure-proof; if even this fails we leave it null and fall back to
    // a per-call alloc below.
    g_persistBuf = (uint8_t*)undoAlloc(g_persistBufBytes);
    if (!g_persistBuf)
        Serial.printf("[Undo] WARN: persist scratch reserve of %u bytes failed "
                      "- will retry per-save (history may not persist under "
                      "heap pressure)\n", (unsigned)g_persistBufBytes);

    g_opHead = g_opTail = 0;
    g_txnHead = g_txnTail = 0;
    for (int i = 0; i <= UNDO_FILE_BUCKET; i++) g_slotCursor[i] = 0;
    g_blobUsed = 0;
    g_globalTxnId = 0;
    g_inTxn = false;

    {
        // Boot may already be sitting in a file context (boot-last-active).
        int bucket = undoBucketForNetSlot(netSlot);
        g_activeSlot = (bucket >= 0) ? bucket : 0;
    }

    Serial.printf("[Undo] init: %u ops + %u txns + %u KB blob shared across %d slots (%s, active=%d)\n",
        (unsigned)g_opCap, (unsigned)g_txnCap, (unsigned)(g_blobCap / 1024),
        NUM_SLOTS, havePsram ? "PSRAM" : "SRAM-only", g_activeSlot);
    Serial.printf("[Undo] persist budget: %u txns / %u B body, %u B scratch %s\n",
        (unsigned)g_persistMaxTxns, (unsigned)g_persistMaxBytes,
        (unsigned)g_persistBufBytes,
        g_persistBuf ? "reserved" : "RESERVE FAILED (per-save alloc)");

    snapshotInit();
    if (g_snapshots)
        Serial.printf("[Undo] snapshot ring: %u entries\n", (unsigned)g_snapshotCap);

    // Restore persisted history (every board). If there's nothing to restore,
    // seed the snapshot ring with a fresh boot capture as before.
    bool restored = undoRestore();

    // Boot-time restore summary (always on - this runs once on Core 0 before
    // Core 1 is spamming Serial, so it's safe even when undo_debug would not
    // be). For each slot that has reachable history it reports the cursor, the
    // total reachable txns, and how many are undoable (reachable + strictly
    // older than the cursor). If a slot shows tot>0 but undoable=0 the cursor
    // was restored at the wrong end; if restore=0 the file was rejected.
    {
        size_t totalReachable = 0;
        char summary[200];
        size_t off = 0;
        summary[0] = '\0';
        for (int s = 0; s <= UNDO_FILE_BUCKET; s++) {
            size_t reach = 0, undoable = 0;
            bool pastCursor = (g_slotCursor[s] == g_txnTail);
            size_t pos = g_txnTail;
            while (pos != g_txnHead) {
                if (pos == g_slotCursor[s]) pastCursor = true;
                const UndoTxn& t = g_txns[pos];
                if (t.opCount && !(t.flags & UNDO_TXN_OBSOLETE) &&
                    t.slot == (uint8_t)s) {
                    reach++;
                    if (!pastCursor) undoable++;
                }
                pos = (pos + 1) % g_txnCap;
            }
            totalReachable += reach;
            if (reach > 0 && off + 40 < sizeof(summary)) {
                off += (size_t)snprintf(summary + off, sizeof(summary) - off,
                    " s%d[cur=%u tot=%u undoable=%u]", s,
                    (unsigned)g_slotCursor[s], (unsigned)reach, (unsigned)undoable);
            }
        }
        Serial.printf("[Undo] restore=%d head=%u tail=%u gid=%u active=%d "
                      "netSlot=%d reachable=%u%s\n",
            (int)restored, (unsigned)g_txnHead, (unsigned)g_txnTail,
            (unsigned)g_globalTxnId, g_activeSlot, netSlot,
            (unsigned)totalReachable, summary);
    }

    if (g_snapshots && !restored)
        takeSnapshot("boot");

    // One-shot cleanup of artifacts from the old persistent-history
    // implementation (replaced by /undo_history.txt). Best-effort.
    FatFS.remove("/undo.log");
    FatFS.remove("/undo.snap");
}

void undoShutdown(void) {
    if (g_inTxn) undoEndTxn();
    // Capture history one last time before the lights go out, then drain the
    // write-back cache so /undo_history.txt actually reaches flash.
    undoPersistHistory();
    extern void fileCacheFlushNowAll(const char* reason);
    fileCacheFlushNowAll("shutdown");
}

// Serialize the live history to /undo.hist via the normal file system.
// Cheap and idempotent: early-outs when nothing changed since the last persist,
// and the write-back cache dedups byte-identical writes. Runs on every board -
// SRAM-only units just keep a smaller bounded slice (see the g_persistMaxTxns /
// g_persistMaxBytes budgets chosen in undoInit). Durability (the eventual flush
// to flash) is handled by the cache's flush service / shutdown drain, same as
// every other slot/config save.
void undoPersistHistory(void) {
    if (!g_ops || !g_txns) return;
    if (!g_historyDirty) return;        // nothing new to write
    // [undo] persist = false: keep the RAM ring but never touch flash, and
    // drop any stale file so the next boot doesn't restore ghost history.
    if (!jumperlessConfig.undo.persist) {
        if (safeFileExists(UNDO_FILE_PATH)) safeFileDelete(UNDO_FILE_PATH);
        g_historyDirty = false;
        return;
    }
    size_t bufBytes = g_persistBufBytes ? g_persistBufBytes : UNDO_PERSIST_BUF_BYTES;

    // Prefer the buffer reserved at boot (g_persistBuf) so a save never depends
    // on a large runtime malloc that fails under SRAM pressure. Only fall back
    // to a per-call alloc if the boot reservation itself failed.
    uint8_t* buf = g_persistBuf;
    bool ownBuf = false;
    if (!buf) {
        buf = (uint8_t*)undoAlloc(bufBytes);
        ownBuf = true;
    }
    if (!buf) {
        // Real durability failure (NOT debug-gated): on an SRAM-only board this
        // ~5 KB scratch comes from the small heap, and if it can't be had the
        // history never reaches flash - the previous (stale) file is what the
        // next boot restores. Leave g_historyDirty set so the next save retries.
        Serial.printf("[Undo] persist FAIL: scratch alloc of %u bytes failed "
                      "- history NOT saved\n", (unsigned)bufBytes);
        return;
    }

    size_t total = undoSerialize(buf, bufBytes);
    if (total == 0) {
        if (ownBuf) undoFree(buf);
        if (!undoHasPersistableTxns()) {
            // Genuinely nothing reachable: drop any stale file so a future boot
            // doesn't restore history that no longer exists.
            if (safeFileExists(UNDO_FILE_PATH)) safeFileDelete(UNDO_FILE_PATH);
            g_historyDirty = false;
        } else {
            // Serialize failed (e.g. scratch overflow on a pathological txn) but
            // live history exists - keep the previous good file and leave the
            // dirty flag set so the next save retries instead of nuking history.
            Serial.printf("[Undo] persist: serialize produced 0 bytes with live "
                          "history present - keeping previous file\n");
        }
        return;
    }

    // Write straight to flash at this natural save point, identically on every
    // board (PSRAM and SRAM alike). The write-back file cache is compiled out
    // (USE_FILE_CACHE=0) because its PSRAM path diverged from SRAM in subtle
    // ways - undo history could come back with zeroed ops only on PSRAM. A
    // direct raw write keeps flash the single source of truth, and there's no
    // deferred-memcpy win to chase here anyway. fileCacheInvalidate() is a
    // no-op in pass-through mode but is kept so re-enabling the cache later
    // still drops any stale cached copy that would shadow flash.
    fileCacheInvalidate(UNDO_FILE_PATH);
    bool ok = safeFileWriteAllRaw(UNDO_FILE_PATH, (const char*)buf, total, 3000);
    if (ownBuf) undoFree(buf);
    if (ok) {
        g_historyDirty = false;
    } else {
        Serial.printf("[Undo] persist FAIL: write of %u bytes to %s failed\n",
                      (unsigned)total, UNDO_FILE_PATH);
    }
    UNDBG("persist %s -> %u bytes (%s)", UNDO_FILE_PATH,
          (unsigned)total, ok ? "ok" : "FAILED");
}

// Wipe all undo/redo history - on flash AND in RAM. Deletes the history
// file(s), empties the shared ring, invalidates the snapshot ring, and clears
// the dirty flag so the next save doesn't immediately re-persist anything.
// Safe to call before undoInit() (the ring globals are zero-initialized, so the
// resets are no-ops and we just delete the on-flash file).
void undoWipeHistory(void) {
    // Drop any open transaction WITHOUT recording it (don't call undoEndTxn -
    // that would commit a stray txn into the ring we're about to clear).
    g_inTxn = false;

    // Empty the shared ring + per-slot cursors.
    g_opHead = g_opTail = 0;
    g_txnHead = g_txnTail = 0;
    for (int s = 0; s <= UNDO_FILE_BUCKET; s++) g_slotCursor[s] = 0;
    g_globalTxnId = 0;
    g_blobUsed = 0;
    g_waypointsDirty = true;
    g_historyDirty = false;  // ring is empty - nothing to persist

    // Invalidate the snapshot ring (its blob offsets point into the arena we
    // just reset to 0; leaving them "valid" would let a clear-all restore read
    // stale bytes).
    if (g_snapshots && g_snapshotCap > 0) {
        memset(g_snapshots, 0, g_snapshotCap * sizeof(SnapshotEntry));
    }
    g_snapshotHead = 0;
    g_lastSnapshotTxnId = 0;
    g_lastSnapshotMs = 0;

    // Delete the persisted file(s). UNDO_FILE_PATH is the live name; /undo.hist
    // is a legacy name from earlier persistent-history builds - nuke it too so a
    // stale copy can't shadow a clean start. Invalidate any cached entry first
    // (no-op in pass-through mode, but correct if the cache is ever re-enabled).
    fileCacheInvalidate(UNDO_FILE_PATH);
    if (safeFileExists(UNDO_FILE_PATH)) safeFileDelete(UNDO_FILE_PATH);
    fileCacheInvalidate("/undo.hist");
    if (safeFileExists("/undo.hist")) safeFileDelete("/undo.hist");

    Serial.println("[Undo] history wiped (flash + RAM)");
}

// Load /undo.hist (if present + valid) into the freshly-allocated ring.
// Called from undoInit() after the rings exist. Returns true if any history
// was restored.
bool undoRestore(void) {
    if (!g_ops || !g_txns) return false;
    if (!safeFileExists(UNDO_FILE_PATH)) {
        Serial.printf("[Undo] restore: no %s on flash (first boot or never "
                      "persisted)\n", UNDO_FILE_PATH);
        return false;
    }
    size_t bufBytes = g_persistBufBytes ? g_persistBufBytes : UNDO_PERSIST_BUF_BYTES;

    int32_t sz = safeFileSize(UNDO_FILE_PATH);
    if (sz < 8 || (size_t)sz > bufBytes) {
        Serial.printf("[Undo] restore REJECT: size %ld out of range (max=%u)\n",
                      (long)sz, (unsigned)bufBytes);
        return false;
    }

    // fileCacheReadInto (under safeFileReadAll) reserves one byte for a NUL
    // terminator, so size the buffer at file size + 1. Reuse the boot-reserved
    // scratch when it's big enough (it almost always is - the writer caps the
    // file at g_persistBufBytes) so restore doesn't need its own heap alloc.
    uint8_t* buf = (g_persistBuf && (size_t)sz + 1 <= bufBytes) ? g_persistBuf : nullptr;
    bool ownBuf = false;
    if (!buf) {
        buf = (uint8_t*)undoAlloc((size_t)sz + 1);
        ownBuf = true;
    }
    if (!buf) return false;

    size_t readBytes = 0;
    bool ok = safeFileReadAll(UNDO_FILE_PATH, (char*)buf, (size_t)sz + 1, &readBytes);
    bool restored = false;
    if (ok && readBytes >= 8) {
        restored = undoDeserialize(buf, readBytes);
    }
    if (ownBuf) undoFree(buf);

    if (restored) {
        Serial.printf("[Undo] restored history from %s (%u bytes)\n",
                      UNDO_FILE_PATH, (unsigned)readBytes);
    }
    return restored;
}

void undoOnSlotSwitch(int newSlot) {
    if (g_activeSlot < 0) return;
    // Same mapping the auto-detect uses: SLOT_FILE_CONTEXT -> the shared file
    // bucket, 0..NUM_SLOTS-1 -> themselves, everything else ignored.
    int bucket = undoBucketForNetSlot(newSlot);
    if (bucket < 0) return;
    slotSwap(bucket);
}

void undoBeginIngest(void) {
    g_ingestDepth++;
}

void undoEndIngest(void) {
    if (g_ingestDepth > 0) {
        g_ingestDepth--;
        // When the outermost ingest region closes, also reset the
        // last-op timestamp so a stale auto-close from before the
        // ingest doesn't fire on the next user action.
        if (g_ingestDepth == 0) g_lastOpMs = millis();
    }
}

bool undoIsIngesting(void) { return g_ingestDepth > 0; }

bool undoTxnInProgress(void) {
    if (!g_ops) return false;
    slotSwapIfNeeded();
    return g_inTxn;
}

void undoBeginTxn(const char* label, UndoSource source) {
    if (!g_ops) return;
    if (g_undoApplying) return;
    slotSwapIfNeeded();
    if (g_inTxn) undoEndTxn();

    // Mark slot's redo tail obsolete. Once you take a new action on slot
    // S after having undone some history on slot S, the txns belonging
    // to slot S between cursor[S] and head are no longer reachable. Other
    // slots' txns in that ring range stay valid - they're independent
    // histories that shouldn't be touched by slot S's actions.
    clampSlotCursor(g_activeSlot);
    size_t cur = g_slotCursor[g_activeSlot];
    if (cur != g_txnHead) {
        size_t pos = cur;
        size_t marked = 0;
        while (pos != g_txnHead) {
            UndoTxn& t = g_txns[pos];
            if (t.opCount > 0 && t.slot == (uint8_t)g_activeSlot &&
                !(t.flags & UNDO_TXN_OBSOLETE)) {
                t.flags |= UNDO_TXN_OBSOLETE;
                marked++;
            }
            pos = (pos + 1) % g_txnCap;
        }
        g_dropCount += (uint32_t)marked;
        g_waypointsDirty = true;
    }

    // If we're full, evict the oldest txn (and bump any per-slot cursor
    // that was pointing at the evicted entry to the new tail).
    if (txnRingNext(g_txnHead) == g_txnTail) {
        size_t evicting = g_txnTail;
        g_opTail = opEndOfTxn(g_txnTail);
        g_txnTail = txnRingNext(g_txnTail);
        for (int s = 0; s <= UNDO_FILE_BUCKET; s++) {
            if (g_slotCursor[s] == evicting) g_slotCursor[s] = g_txnTail;
        }
        g_waypointsDirty = true;
    }

    g_openTxnIdx = g_txnHead;
    g_openTxnStartMs = millis();
    g_openTxnOpStart = (uint16_t)g_opHead;
    g_openTxnSource = (uint8_t)source;
    g_openTxnSlot = (uint8_t)((g_activeSlot >= 0) ? g_activeSlot : 0);
    if (label) {
        strncpy(g_openTxnLabel, label, sizeof(g_openTxnLabel) - 1);
        g_openTxnLabel[sizeof(g_openTxnLabel) - 1] = '\0';
    } else {
        g_openTxnLabel[0] = '\0';
    }
    g_inTxn = true;
    g_lastOpMs = g_openTxnStartMs;
    UNDBG("beginTxn idx=%u slot=%u src=%u label='%s'",
          (unsigned)g_openTxnIdx, (unsigned)g_openTxnSlot,
          (unsigned)source, label ? label : "");
}

void undoEndTxn(void) {
    if (!g_ops || !g_inTxn) return;
    UNDBG("endTxn ENTER idx=%u opHead=%u opStart=%u txnHead=%u txnTail=%u",
          (unsigned)g_openTxnIdx, (unsigned)g_opHead, (unsigned)g_openTxnOpStart,
          (unsigned)g_txnHead, (unsigned)g_txnTail);
    UndoTxn& t = g_txns[g_openTxnIdx];
    t.startMs = g_openTxnStartMs;
    t.firstOp = (uint32_t)g_openTxnOpStart;
    size_t added = (g_opHead >= g_openTxnOpStart)
                       ? (g_opHead - g_openTxnOpStart)
                       : (g_opHead + g_opCap - g_openTxnOpStart);
    t.opCount = (uint16_t)added;
    t.flags = UNDO_TXN_COMMITTED;
    if (g_openTxnSource != UNDO_SRC_INTERNAL) t.flags |= UNDO_TXN_USER_INITIATED;
    t.source = g_openTxnSource;
    t.slot = g_openTxnSlot;
    // Derive the committed label from the ops so the live label matches what
    // restore rebuilds (also op-driven). Otherwise a caller-supplied label like
    // the rails menu's "rails 3.3V" shows in RAM but is replaced by the
    // op-rebuilt "Rails 3.30V" after a reboot. Scripted bundles (Python/MCP)
    // keep their explicit label - it carries intent that can't be reconstructed
    // from the raw ops (e.g. "import wokwi sketch").
    bool scripted = (g_openTxnSource == UNDO_SRC_PYTHON ||
                     g_openTxnSource == UNDO_SRC_MCP);
    if (t.opCount > 0 && !scripted) {
        undoFormatTxnLabel(t, t.label, sizeof(t.label));
    } else {
        strncpy(t.label, g_openTxnLabel, sizeof(t.label) - 1);
        t.label[sizeof(t.label) - 1] = '\0';
    }

    UNDBG("endTxn opCount=%u", (unsigned)t.opCount);
    if (t.opCount > 0) {
        g_globalTxnId++;
        t.globalId = g_globalTxnId;
        g_txnHead = txnRingNext(g_txnHead);
        // Cursor for the slot we just committed to advances to the new
        // head (txn was appended at slot[g_openTxnSlot]'s "live" end).
        g_slotCursor[g_openTxnSlot] = g_txnHead;
        g_waypointsDirty = true;
        g_historyDirty = true;
        undoMaybeTakeSnapshot("periodic");
        UNDBG("endTxn DONE");
    }
    g_inTxn = false;
}

static void maybeAutoClose() {
    if (g_inTxn && (millis() - g_lastOpMs) > TXN_AUTOCLOSE_MS) {
        undoEndTxn();
    }
}

static void appendOp(const UndoOp& op) {
    if (!g_ops) return;
    if (g_undoApplying) return;
    UNDBG("appendOp type=%u inTxn=%d", (unsigned)op.type, (int)g_inTxn);
    if (!g_inTxn) {
        undoBeginTxn(nullptr, UNDO_SRC_UNKNOWN);
    }
    // If full, evict oldest txn (which advances op tail and bumps any
    // per-slot cursor that was pointing at the evicted txn).
    if (opRingNext(g_opHead) == g_opTail) {
        if (!ringEmpty(g_txnHead, g_txnTail)) {
            size_t evicting = g_txnTail;
            g_opTail = opEndOfTxn(g_txnTail);
            g_txnTail = txnRingNext(g_txnTail);
            for (int s = 0; s <= UNDO_FILE_BUCKET; s++) {
                if (g_slotCursor[s] == evicting) g_slotCursor[s] = g_txnTail;
            }
            g_waypointsDirty = true;
        } else {
            g_opTail = opRingNext(g_opTail);
        }
    }
    g_ops[g_opHead] = op;
    g_ops[g_opHead].txnIdx = (uint16_t)(g_openTxnIdx & 0xFFFF);
    g_opHead = opRingNext(g_opHead);
    g_lastOpMs = millis();
    g_historyDirty = true;
}

// Each public record function opens a fresh transaction with a label that
// matches the user's intent, appends one op, and immediately closes it.
// This gives PER-ACTION undo granularity: each connect/disconnect/DAC
// change is its own undo step, even when they happen in rapid succession
// during a probe mode session.
//
// If a caller needs to bundle multiple ops as a single undo unit (e.g. a
// Python script doing 5 connects atomically), it can call undoBeginTxn /
// undoEndTxn explicitly around the operations - the per-record helpers
// detect an already-open external transaction and skip the close so they
// just contribute their op to the bundle.
//
// Note: we still call appendOp first (which auto-opens an implicit txn if
// none is open) and only close it if WE were the ones who opened it.
//
// We also slotSwapIfNeeded() here so that any record* call lands on the
// correct slot's history (the active slot is always == netSlot at the
// moment of recording).
//
// Ingest gate: if a slot file is loading, all the addConnection /
// setRailVoltage calls that run as part of that load are NOT user
// actions and must not enter the history. The slot loader brackets
// its region with undoBeginIngest()/undoEndIngest(); we short-circuit
// here while that's active.
static void recordOneAction(const UndoOp& op) {
    if (!g_ops) return;
    if (g_ingestDepth > 0) return;
    slotSwapIfNeeded();
    bool weOpened = !g_inTxn;
    if (weOpened) {
        // Derive the label straight from the op - one source of truth, so we
        // never write a string just to parse it back. External (multi-op)
        // transactions keep the label their opener passed to undoBeginTxn.
        char lbl[sizeof(g_openTxnLabel)];
        undoFormatOpLabel(op, lbl, sizeof(lbl));
        undoBeginTxn(lbl, UNDO_SRC_PROBE);
    }
    appendOp(op);
    if (weOpened) undoEndTxn();
}

// Connections that are auto-managed by the system should NOT pollute the
// user-visible undo history - the user didn't make them and can't
// meaningfully reason about undoing them. The InfraPaths registry is the
// authority (the old node-pair heuristic only knew BUFFER_IN<->DAC and let
// the GPIO power claim and OLED/UART lock bridges leak into history).
// BUFFER_IN pairs are covered even when transiently unregistered (self
// test / calibration manipulate them under the system allowance).
static inline bool isAutoManagedBridge(int n1, int n2) {
    return infraIsBridge(n1, n2) ||
           n1 == ROUTABLE_BUFFER_IN || n2 == ROUTABLE_BUFFER_IN;
}

void undoRecordConnect(int node1, int node2, uint32_t color) {
    UNDBG("recordConnect %d-%d color=0x%08X", node1, node2, (unsigned)color);
    if (isAutoManagedBridge(node1, node2)) {
        UNDBG("recordConnect skipped (auto-managed DAC<->BUF)");
        return;
    }
    UndoOp op = {};
    op.type = UNDO_OP_CONNECT;
    op.bridge.n1 = (int16_t)node1;
    op.bridge.n2 = (int16_t)node2;
    op.bridge.color = color;
    recordOneAction(op);
}

void undoRecordDisconnect(int node1, int node2, uint32_t color) {
    UNDBG("recordDisconnect %d-%d color=0x%08X", node1, node2, (unsigned)color);
    if (isAutoManagedBridge(node1, node2)) {
        UNDBG("recordDisconnect skipped (auto-managed DAC<->BUF)");
        return;
    }
    UndoOp op = {};
    op.type = UNDO_OP_DISCONNECT;
    op.bridge.n1 = (int16_t)node1;
    op.bridge.n2 = (int16_t)node2;
    op.bridge.color = color;
    recordOneAction(op);
}

void undoRecordClearAll(void) {
    UNDBG("recordClearAll ENTER bridges=%d", globalState.connections.numBridges);
    // Capture the PRE-clear state into the snapshot ring so we can revert
    // this clear-all later. The OP_CLEAR_ALL record reuses that blob's
    // arena offset as its restore source - no double allocation.
    undoForceSnapshot("clear_all");

    UndoOp op = {};
    op.type = UNDO_OP_CLEAR_ALL;
    // Latest snapshot owns the blob we just captured.
    SnapshotEntry* sn = nullptr;
    if (g_snapshots && g_snapshotCap > 0) {
        size_t lastIdx = (g_snapshotHead + g_snapshotCap - 1) % g_snapshotCap;
        if (g_snapshots[lastIdx].valid) sn = &g_snapshots[lastIdx];
    }
    if (sn) {
        op.blob.blobOffset = sn->blobOffset;
        op.blob.blobSize = sn->blobSize;
        op.blob.snapTxnId = sn->txnIndex;
        UNDBG("recordClearAll using snapshot off=%u sz=%u tid=%u",
              (unsigned)sn->blobOffset, (unsigned)sn->blobSize,
              (unsigned)sn->txnIndex);
    } else {
        // No snapshot available (very small SRAM build or alloc fail).
        // Fall back to a fresh blob copy so in-RAM undo still works.
        uint32_t blobOff = 0, blobSize = 0;
        if (blobAppendCurrentState(&blobOff, &blobSize)) {
            op.blob.blobOffset = blobOff;
            op.blob.blobSize = blobSize;
            op.blob.snapTxnId = 0;
        } else {
            UNDBG("recordClearAll: no snapshot AND blob alloc failed");
        }
    }
    recordOneAction(op);
}

void undoRecordDacSet(int channel, float prevVolts, float nextVolts) {
    if (!g_ops) return;
    if (g_ingestDepth > 0) return;
    if (g_undoApplying) return;
    slotSwapIfNeeded();

    UndoOp op = {};
    op.type = UNDO_OP_DAC_SET;
    op.dac.ch = (uint8_t)channel;
    op.dac.prev = prevVolts;
    op.dac.next = nextVolts;

    // Slide coalescing: if the most-recent committed txn on this slot is a DAC
    // set on the SAME channel made moments ago (and there's no redo pending),
    // retarget its voltage in place instead of recording another undo step.
    // The op keeps its ORIGINAL `prev` (the value before the slide began), so a
    // single undo reverts the whole slide rather than one voltage at a time.
    if (!g_inTxn && !ringEmpty(g_txnHead, g_txnTail) &&
        (millis() - g_lastOpMs) <= DAC_SLIDE_COALESCE_MS) {
        uint8_t slot = (uint8_t)((g_activeSlot >= 0) ? g_activeSlot : 0);
        size_t prevIdx = txnRingPrev(g_txnHead);
        UndoTxn& pt = g_txns[prevIdx];
        if ((pt.flags & UNDO_TXN_COMMITTED) && !(pt.flags & UNDO_TXN_OBSOLETE) &&
            pt.slot == slot && pt.opCount == 1 &&
            g_slotCursor[slot] == g_txnHead) {
            UndoOp& po = g_ops[(size_t)pt.firstOp % g_opCap];
            if (po.type == UNDO_OP_DAC_SET && po.dac.ch == (uint8_t)channel) {
                po.dac.next = nextVolts;     // keep the pre-slide `prev`
                undoRebuildLabel(pt);        // refresh "<name> X.XXV"
                pt.startMs = millis();
                g_lastOpMs = millis();
                g_historyDirty = true;
                UNDBG("recordDacSet COALESCE ch=%d -> %.2fV",
                      channel, (double)nextVolts);
                return;
            }
        }
    }

    recordOneAction(op);
}

void undoRecordGpioSet(int pin, int prevVal, int nextVal) {
    UndoOp op = {};
    op.type = UNDO_OP_GPIO_SET;
    op.gpio.pin = (uint8_t)pin;
    op.gpio.prevVal = (uint8_t)prevVal;
    op.gpio.nextVal = (uint8_t)nextVal;
    recordOneAction(op);
}

void undoRecordGpioDir(int pin, int prevDir, int nextDir) {
    UndoOp op = {};
    op.type = UNDO_OP_GPIO_DIR;
    op.gpio.pin = (uint8_t)pin;
    op.gpio.prevDir = (uint8_t)prevDir;
    op.gpio.nextVal = (uint8_t)nextDir;
    recordOneAction(op);
}

void undoRecordSlotSwitch(int prevSlot, int nextSlot) {
    UndoOp op = {};
    op.type = UNDO_OP_SLOT_SWITCH;
    op.slot.prev = (uint8_t)prevSlot;
    op.slot.next = (uint8_t)nextSlot;
    recordOneAction(op);
}

// =============================================================================
// User-facing controls
// =============================================================================

// Eagerly close any open transaction if its 200ms quiescence window has
// elapsed. Called from canUndo/canRedo so the user-visible state
// reflects mutations that happened a moment ago without requiring a
// follow-up mutation to trigger maybeAutoClose.
static void undoFinalizePendingIfStale(void) {
    if (g_ops && g_inTxn && (millis() - g_lastOpMs) > TXN_AUTOCLOSE_MS) {
        undoEndTxn();
    }
}

bool undoCanUndo(void) {
    if (!g_ops) return false;
    slotSwapIfNeeded();
    undoFinalizePendingIfStale();
    if (g_activeSlot < 0) return false;
    clampSlotCursor(g_activeSlot);
    // Are there any non-obsolete txns belonging to this slot strictly
    // before the slot's cursor? (findPrevSlotTxn does this exact walk.)
    return findPrevSlotTxn(g_activeSlot, g_slotCursor[g_activeSlot]) != SIZE_MAX;
}

bool undoCanRedo(void) {
    if (!g_ops) return false;
    slotSwapIfNeeded();
    undoFinalizePendingIfStale();
    if (g_activeSlot < 0) return false;
    clampSlotCursor(g_activeSlot);
    return findNextSlotTxn(g_activeSlot, g_slotCursor[g_activeSlot]) != SIZE_MAX;
}

// Push reverted state to hardware: bridges -> crosspoints + LEDs, plus
// rails / DACs (cheap: 4 small I2C writes). In-memory mutation alone
// doesn't reroute switches, refresh LEDs, or re-issue DAC voltages.
static void undoCommitToHardware(void) {
    // Everything here just pushes the ALREADY-decided state to hardware
    // (re-route, LED repaint, re-issue rails/DACs). None of it is a new user
    // action, so keep g_undoApplying asserted for the whole commit. Otherwise a
    // DAC/rail nudge inside refreshConnections() or setRailsAndDACs() - e.g. the
    // probe buffer power DAC snapping back to measure_mode_output_voltage after
    // a DAC undo - would call undoRecordDacSet(), open a fresh transaction, and
    // mark the redo tail obsolete: traversing a DAC op would silently wipe redo.
    bool wasApplying = g_undoApplying;
    g_undoApplying = true;
    // clean=1 forces a full re-route + LED repaint. Needed because a clear-all
    // undo now replaces the entire board state (via YAML restore), not just a
    // single bridge, so a partial commit could leave stale crosspoints/LEDs.
    refreshConnections(-1, 1, 1);
    setRailsAndDACs(0);
    g_undoApplying = wasApplying;
}

bool undoUndo(void) {
    if (!undoCanUndo()) return false;
    UNDBG("UNDO start cursor=%u (slot %d)",
          (unsigned)g_slotCursor[g_activeSlot], g_activeSlot);
    if (g_inTxn) undoEndTxn();
    size_t prev = findPrevSlotTxn(g_activeSlot, g_slotCursor[g_activeSlot]);
    if (prev == SIZE_MAX) return false;
    UndoTxn& t = g_txns[prev];
    applyTxn(t, -1);
    g_slotCursor[g_activeSlot] = prev;
    g_undoCount++;
    g_historyDirty = true;
    undoCommitToHardware();
    UNDBG("UNDO done cursor=%u", (unsigned)g_slotCursor[g_activeSlot]);
    return true;
}

bool undoRedo(void) {
    if (!undoCanRedo()) return false;
    UNDBG("REDO start cursor=%u (slot %d)",
          (unsigned)g_slotCursor[g_activeSlot], g_activeSlot);
    size_t next = findNextSlotTxn(g_activeSlot, g_slotCursor[g_activeSlot]);
    if (next == SIZE_MAX) return false;
    UndoTxn& t = g_txns[next];
    applyTxn(t, +1);
    g_slotCursor[g_activeSlot] = txnRingNext(next);
    g_redoCount++;
    g_historyDirty = true;
    undoCommitToHardware();
    UNDBG("REDO done cursor=%u", (unsigned)g_slotCursor[g_activeSlot]);
    return true;
}

namespace {
// Count slot's reachable txns in [tail, head). Used by undoTotalTxns and
// position helpers. This is O(ring size) but only runs on user-visible
// queries, not during recording.
int countSlotTxns(int slot) {
    if (!g_ops) return 0;
    if (g_txnHead == g_txnTail) return 0;
    int n = 0;
    size_t pos = g_txnTail;
    while (pos != g_txnHead) {
        if (txnBelongsToSlot(g_txns[pos], slot)) n++;
        pos = (pos + 1) % g_txnCap;
    }
    return n;
}

// How many txns has slot S undone? Counts non-obsolete same-slot txns
// in [cursor, head). Position is reported as -that count.
int countSlotUndone(int slot, size_t cursor) {
    if (!g_ops) return 0;
    if (g_txnHead == g_txnTail) return 0;
    int n = 0;
    size_t pos = cursor;
    while (pos != g_txnHead) {
        if (txnBelongsToSlot(g_txns[pos], slot)) n++;
        pos = (pos + 1) % g_txnCap;
    }
    return n;
}
}  // namespace

int undoPosition(void) {
    if (!g_ops || g_activeSlot < 0) return 0;
    clampSlotCursor(g_activeSlot);
    return -countSlotUndone(g_activeSlot, g_slotCursor[g_activeSlot]);
}

int undoTotalTxns(void) {
    if (!g_ops || g_activeSlot < 0) return 0;
    return countSlotTxns(g_activeSlot);
}

// Resolve a cursor-relative offset to a same-slot txn ring index, or SIZE_MAX.
// relativeOffset: 0 = the txn the next undo would consume (most recent same-slot
// txn before the cursor); -1 = one before that; +1 = the txn the next redo would
// apply (== same-slot txn at the cursor, if any). Shared by undoLabelAt and
// undoLabelSplitAt so the label and its split index always come from one txn.
static size_t undoResolveRelTxn(int relativeOffset) {
    if (!g_ops || g_activeSlot < 0) return SIZE_MAX;
    clampSlotCursor(g_activeSlot);
    size_t cursor = g_slotCursor[g_activeSlot];
    if (relativeOffset >= 1) {
        size_t pos = cursor;
        for (int i = 0; i < relativeOffset; i++) {
            size_t found = findNextSlotTxn(g_activeSlot, pos);
            if (found == SIZE_MAX) return SIZE_MAX;
            pos = (found + 1) % g_txnCap;
            if (i == relativeOffset - 1) return found;
        }
        return SIZE_MAX;
    }
    int steps = 1 - relativeOffset;  // relativeOffset=0 -> 1 step back
    size_t pos = cursor;
    for (int i = 0; i < steps; i++) {
        size_t found = findPrevSlotTxn(g_activeSlot, pos);
        if (found == SIZE_MAX) return SIZE_MAX;
        pos = found;
        if (i == steps - 1) return found;
    }
    return SIZE_MAX;
}

const char* undoLabelAt(int relativeOffset) {
    size_t idx = undoResolveRelTxn(relativeOffset);
    if (idx == SIZE_MAX) return "";
    return g_txns[idx].label[0] ? g_txns[idx].label : "(unlabeled)";
}

// Action length (split index) of the label at `relativeOffset`, recomputed from
// the txn's first op via undoFormatOpLabel so it matches exactly what's stored.
// Falls back to the first space for custom (externally-labelled) txns, or -1 if
// there's no txn. The toast uses this to separate the small action from the
// large detail without guessing where the boundary is.
int undoLabelSplitAt(int relativeOffset) {
    size_t idx = undoResolveRelTxn(relativeOffset);
    if (idx == SIZE_MAX) return -1;
    UndoTxn& t = g_txns[idx];
    if (t.opCount > 0) {
        char scratch[sizeof(t.label)];
        uint8_t split = undoFormatTxnLabel(t, scratch, sizeof(scratch));
        if (strcmp(scratch, t.label) == 0) return (int)split;
    }
    const char* sp = strchr(t.label, ' ');
    return sp ? (int)(sp - t.label) : -1;
}

bool undoScrubTo(int targetPosition) {
    if (!g_ops || g_activeSlot < 0) return false;
    int cur = undoPosition();
    while (cur != targetPosition) {
        if (cur > targetPosition) {
            if (!undoUndo()) return false;
        } else {
            if (!undoRedo()) return false;
        }
        cur = undoPosition();
    }
    return true;
}

int undoNextWaypoint(int fromPosition, int dir) {
    if (!g_ops || g_activeSlot < 0 || dir == 0) return fromPosition;
    if (g_waypointsDirty) {
        recomputeWaypoints();
        g_waypointsDirty = false;
        g_waypointsValidAt = g_globalTxnId;
    }
    // Walk same-slot waypoints. fromPosition is negative (== -back, where
    // back is how many same-slot txns have been undone). We step through
    // same-slot txns in the requested direction, returning when we hit one
    // that is flagged as a waypoint.
    int back = -fromPosition;
    int total = undoTotalTxns();
    if (total <= 0) return fromPosition;
    if (dir > 0) {
        // older: walk backward through same-slot txns
        clampSlotCursor(g_activeSlot);
        size_t pos = g_slotCursor[g_activeSlot];
        // first move to "current position" - back same-slot txns from head
        for (int i = 0; i < back; i++) {
            size_t f = findPrevSlotTxn(g_activeSlot, pos);
            if (f == SIZE_MAX) return -back;
            pos = f;
        }
        while (back < total - 1) {
            back++;
            size_t f = findPrevSlotTxn(g_activeSlot, pos);
            if (f == SIZE_MAX) return -(total - 1);
            pos = f;
            if (g_txns[f].flags & UNDO_TXN_WAYPOINT) return -back;
        }
        return -(total - 1);
    } else {
        // newer: walk forward through same-slot txns
        clampSlotCursor(g_activeSlot);
        size_t pos = g_slotCursor[g_activeSlot];
        for (int i = 0; i < back; i++) {
            size_t f = findPrevSlotTxn(g_activeSlot, pos);
            if (f == SIZE_MAX) break;
            pos = f;
        }
        while (back > 0) {
            size_t f = findNextSlotTxn(g_activeSlot, pos);
            if (f == SIZE_MAX) return 0;
            pos = (f + 1) % g_txnCap;
            back--;
            if (g_txns[f].flags & UNDO_TXN_WAYPOINT) return -back;
        }
        return 0;
    }
}

// =============================================================================
// Snapshot policy / public helpers (storage + capture defined above)
// =============================================================================

bool undoForceSnapshot(const char* reason) {
    if (!g_ops || !g_snapshots) return false;
    takeSnapshot(reason ? reason : "forced");
    return true;
}

int undoSnapshotCount(void) {
    if (!g_snapshots) return 0;
    int valid = 0;
    for (size_t i = 0; i < g_snapshotCap; i++) if (g_snapshots[i].valid) valid++;
    return valid;
}

void undoMaybeTakeSnapshot(const char* reason) {
    if (!g_ops || !g_snapshots) return;

    // Snapshots back the non-symmetric ops (currently just OP_CLEAR_ALL)
    // and provide resync anchors for scrubbing. We used to also snapshot
    // every N transactions / every M ms as cheap "safety nets" - but
    // those triggers fired during normal probe work and added churn.
    // Now we only snapshot when an explicit caller asks for it:
    //   * undoRecordClearAll() calls undoForceSnapshot("clear_all")
    //   * Slot switches call this with reason == "slot_switch"
    //   * Anyone passing reason == "explicit" forces one
    if (!reason) return;
    bool shouldSnapshot = (strcmp(reason, "explicit") == 0 ||
                           strcmp(reason, "slot_switch") == 0 ||
                           strcmp(reason, "clear_all") == 0);
    if (shouldSnapshot) {
        takeSnapshot(reason);
    }
}

// --- OLED toast helpers ------------------------------------------------------
// Format: "undo\n<label>" or "redo\n<label>" at text size 1, centered.
// Falls back to short tag if no label / OLED missing. Cheap call - the
// OLED auto-recovers when Highlighting::service() next repaints.
const char* undoPeekUndoLabel(void) {
    // The next undo will consume the txn immediately behind the cursor:
    //   undoLabelAt(0) returns the txn at cursor-1 (per existing impl).
    return undoLabelAt(0);
}

const char* undoPeekRedoLabel(void) {
    // The next redo will reapply the txn at the cursor itself.
    return undoLabelAt(+1);
}

void undoFlashLogo(uint32_t durationMs) {
    uint32_t target = millis() + durationMs;
    // Only extend the window - never shorten it. A long-running hold should
    // not be cut short by a single-shot flash from undoToast().
    if (target > undoActivityUntil) undoActivityUntil = target;
}

// --- OLED undo/redo toast layout ---------------------------------------------
// The toast is built from up to three logical pieces:
//   tag    = "undo" / "redo"
//   action = "connect" / "Top Rail" / "GPIO 1" / "Clear All" / ... (verb/source)
//   nodes  = "8-GP 2" / "5.00V" / "2->3" / ...                     (detail/value)
// The layout below is hardcoded to match the design exported from the OLED
// layout editor (device/screens/layout.json) - see undoBuildToastScreen.

// Split a label into action + detail using the authored split index
// `actionLen` (from undoLabelSplitAt / undoFormatOpLabel). Because the action
// can contain spaces ("Top Rail", "GPIO 1", "Clear All"), the boundary cannot
// be found by scanning for a space - it is carried explicitly:
//   "Top Rail 5.00V" (len 8) -> "Top Rail" / "5.00V"
//   "GPIO 1 HIGH"    (len 6) -> "GPIO 1"   / "HIGH"
//   "Clear All"      (len 9) -> "Clear All"/ ""
//   "connect 8-GP 2" (len 7) -> "connect"  / "8-GP 2"
// actionLen < 0 (unknown / custom label) falls back to the first space.
static void undoSplitLabel(const char* label, int actionLen,
                           char* action, size_t actionSize,
                           char* nodes, size_t nodesSize) {
    action[0] = '\0';
    nodes[0] = '\0';
    if (!label || !label[0]) return;

    size_t len = strlen(label);
    if (actionLen < 0 || (size_t)actionLen > len) {
        const char* sp = strchr(label, ' ');
        actionLen = sp ? (int)(sp - label) : (int)len;
    }

    size_t a = (size_t)actionLen;
    if (a >= actionSize) a = actionSize - 1;
    memcpy(action, label, a);
    action[a] = '\0';

    const char* d = label + actionLen;
    if (*d == ' ') d++;   // skip the single separator between action and detail
    strncpy(nodes, d, nodesSize - 1);
    nodes[nodesSize - 1] = '\0';
}

// Build the toast as a retained OledScreen of anchored Text elements.
// Hardcoded to reproduce the design exported from the OLED layout editor
// (device/screens/layout.json), i.e. exactly what oledGuiLoadScreen() would
// produce from that file:
//   tag    ("undo"/"redo") : New Science 13pt, anchored top-left  (x/y ignored)
//   action ("connect"/...) : Pragmatism  8pt, absolute (7, 18)    (FREE/FREE)
//   nodes  ("8-GP 2"/...)  : Pragmatism 10pt, right-anchored at y=11 (RIGHT/FREE)
// Any piece that is empty is simply skipped.
static void undoBuildToastScreen(OledScreen& screen, const char* tag,
                                 const char* action, const char* nodes) {

                                    // return;
    screen.clearElements();
    screen.w = 128;
    screen.h = 32;

    if (tag && tag[0]) {
        int idx = screen.addText(tag, 3, 1, "New Science", 13);
        if (idx >= 0) screen.setAnchor(idx, OLED_H_LEFT, OLED_V_TOP);
    }
    if (action && action[0]) {
        int idx = screen.addText(action, 5, 18, "Pragmatism", 7);
        if (idx >= 0) screen.setAnchor(idx, OLED_H_LEFT, OLED_V_FREE);
    }
    if (nodes && nodes[0]) {
        int idx = screen.addText(nodes, 79, 11, "Pragmatism", 8);
        if (idx >= 0) screen.setAnchor(idx, OLED_H_RIGHT, OLED_V_FREE);
    }
}

void undoToast(bool isRedo, const char* label, int actionLen) {
    const char* tag = isRedo ? "redo" : "undo";

// return;

    // Match the probeMode entry banner style: blank line above and
    // below, tab-indented body. Standalone block so probe-mode prints
    // and prompts before/after don't smear into the toast line.

    // Publish the last undo/redo action to the OLED variable registry so any
    // live screen bound to {undo} updates automatically (no display required).
    {
        char undoVar[64];
        if (label && label[0]) snprintf(undoVar, sizeof(undoVar), "%s %s", tag, label);
        else                   snprintf(undoVar, sizeof(undoVar), "%s", tag);
        OledVars::setStr("undo", undoVar);
    }

    if (oled.isConnected()) {
    // Split the label into action + nodes at the authored boundary;
    // undoBuildToastScreen lays them out (with the tag) per the hardcoded
    // layout-editor design, e.g.:
    //   undo
    //   connect              8-GP 2
    char action[16];
    char nodes[48];
    undoSplitLabel(label, actionLen, action, sizeof(action), nodes, sizeof(nodes));

    // Build the toast as a retained oledgui scene graph (anchored Text
    // elements) and render it into the framebuffer. A function-static
    // OledScreen is reused across toasts so a hot undo/redo path pays no
    // per-toast allocation; it is never registered with OledGui (we render it
    // manually here), so the background render service never touches it.
    static OledScreen toastScreen;
    undoBuildToastScreen(toastScreen, tag, action, nodes);

    // Snapshot-aware hold: capture the pre-toast framebuffer FIRST, then
    // paint + priority-flush the toast. oledHoldBegin saves the current live
    // framebuffer (the background that was visible before this toast) into a
    // shadow buffer and arms the post-flush latch. render() repaints the
    // framebuffer with the toast scene graph; priorityFlushHeldFrame() pushes
    // it to the panel and restores the shadow back into the live framebuffer.
    // Any probe-mode / clearHighlighting / status writes during the hold
    // window therefore accumulate against the pre-toast background rather than
    // smearing on top of toast pixels. When the hold expires, oledPeriodic()
    // flushes the live framebuffer once - the panel transitions cleanly from
    // the toast to the up-to-date underlying content with no toast residue.
    //
    // A persistent idle oledgui page steps aside while we own the panel.
    OledGui::getInstance().notePanelTakenByOther();
    oled.oledHoldBegin(1800);
    toastScreen.resolveBindings();
    toastScreen.render(oled);
    oled.priorityFlushHeldFrame();
}

    const int disColor = 202;
    const int conColor = 45;
    int actionColor = -1;
    if (label && label[0]) {
        if (strstr(label, "disconnect") != NULL) {
            actionColor = disColor;
        } else if (strstr(label, "connect") != NULL) {
            actionColor = conColor;
        }
    }


    if (isRedo) {
        changeTerminalColor(122, false, &Serial);    
        Serial.print("\r\t[REDO]");
        changeTerminalColor(actionColor, false, &Serial);
        Serial.printf(" %s\033[0K", (label && label[0]) ? label : "<unlabeled>");
        changeTerminalColor(-1, true, &Serial);
    } else {
        changeTerminalColor(209, false, &Serial);
        Serial.print("\r\t[UNDO]");
        changeTerminalColor(actionColor, false, &Serial);
        Serial.printf(" %s\033[0K", (label && label[0]) ? label : "<unlabeled>");
        changeTerminalColor(-1, true, &Serial);
    }
    Serial.flush();    
    // Visual cue: logo turns yellow for ~600 ms (long enough to read the
    // OLED label and notice the color change without lingering).
    undoFlashLogo(500);

}

void undoDumpStatus(void) {
    if (!g_ops) { Serial.println("[Undo] disabled"); return; }
    slotSwapIfNeeded();
    Serial.printf("[Undo] active slot=%d  ops %u/%u  txns %u/%u  blob %u/%u\n",
        g_activeSlot,
        (unsigned)ringCount(g_opHead, g_opTail, g_opCap), (unsigned)g_opCap,
        (unsigned)ringCount(g_txnHead, g_txnTail, g_txnCap), (unsigned)g_txnCap,
        (unsigned)g_blobUsed, (unsigned)g_blobCap);
    Serial.printf("        active cursor %d (head=0)  total %d  undo=%u redo=%u drop=%u\n",
        undoPosition(), undoTotalTxns(),
        (unsigned)g_undoCount, (unsigned)g_redoCount, (unsigned)g_dropCount);
    if (g_snapshots) {
        int valid = 0;
        for (size_t i = 0; i < g_snapshotCap; i++) if (g_snapshots[i].valid) valid++;
        Serial.printf("        snapshots %d/%u  last txn=%u age=%lus\n",
            valid, (unsigned)g_snapshotCap,
            (unsigned)g_lastSnapshotTxnId,
            g_lastSnapshotMs ? (unsigned long)((millis() - g_lastSnapshotMs) / 1000) : 0UL);
    }
    // Per-slot summary - shared ring, just counts how many txns each
    // slot owns and where its cursor sits.
    Serial.println("        per-slot summary:");
    for (int i = 0; i <= UNDO_FILE_BUCKET; i++) {
        int slotTotal = countSlotTxns(i);
        if (slotTotal == 0 && i != g_activeSlot) continue;
        int slotUndone = countSlotUndone(i, g_slotCursor[i]);
        Serial.printf("          %-7s%s  txns %d  cursor %d\n",
            (i == UNDO_FILE_BUCKET) ? "file" : (String("slot ") + String(i)).c_str(),
            (i == g_activeSlot) ? " *" : "  ",
            slotTotal, -slotUndone);
    }
}

}  // extern "C"

#else  // !UNDO_ENABLED

// ===========================================================================
// Undo subsystem compiled out (e.g. OG_JUMPERLESS).
// ===========================================================================
// One flag (UNDO_ENABLED, see JumperlessDefines.h) removes the entire undo
// implementation - the delta ring, persistence, AND the ~3.2 KB static
// OledScreen undo-toast buffer - in a single place. These no-op stubs satisfy
// the full Undo.h API + the two exported globals so every caller (States,
// Commands, Probing, the MicroPython API, menus) and the UndoIngestGuard RAII
// helper compile and link unchanged, with no per-call-site #ifdef. The
// behavior matches the old runtime disable (undoInit early-returned and every
// record hook null-checked into a no-op) but now the code and its statics are
// dropped by the linker rather than carried dead.
extern "C" {

volatile int  undo_debug     = 0;
volatile bool g_undoApplying = false;

void        undoInit(void) {}
void        undoShutdown(void) {}
void        undoPersistHistory(void) {}
bool        undoRestore(void) { return false; }
void        undoWipeHistory(void) {}
void        undoOnSlotSwitch(int) {}
void        undoBeginIngest(void) {}
void        undoEndIngest(void) {}
bool        undoIsIngesting(void) { return false; }
void        undoBeginTxn(const char*, UndoSource) {}
void        undoEndTxn(void) {}
bool        undoTxnInProgress(void) { return false; }
void        undoRecordConnect(int, int, uint32_t) {}
void        undoRecordDisconnect(int, int, uint32_t) {}
void        undoRecordClearAll(void) {}
void        undoRecordDacSet(int, float, float) {}
void        undoRecordGpioSet(int, int, int) {}
void        undoRecordGpioDir(int, int, int) {}
void        undoRecordSlotSwitch(int, int) {}
bool        undoCanUndo(void) { return false; }
bool        undoCanRedo(void) { return false; }
bool        undoUndo(void) { return false; }
bool        undoRedo(void) { return false; }
int         undoPosition(void) { return 0; }
int         undoTotalTxns(void) { return 0; }
const char* undoLabelAt(int) { return nullptr; }
int         undoLabelSplitAt(int) { return -1; }
bool        undoScrubTo(int) { return false; }
int         undoNextWaypoint(int, int) { return 0; }
void        undoMaybeTakeSnapshot(const char*) {}
bool        undoForceSnapshot(const char*) { return false; }
int         undoSnapshotCount(void) { return 0; }
void        undoDumpStatus(void) {}
void        undoToast(bool, const char*, int) {}
const char* undoPeekUndoLabel(void) { return nullptr; }
const char* undoPeekRedoLabel(void) { return nullptr; }
void        undoFlashLogo(uint32_t) {}

}  // extern "C"

#endif  // UNDO_ENABLED
