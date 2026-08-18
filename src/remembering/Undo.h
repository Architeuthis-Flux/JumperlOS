// SPDX-License-Identifier: MIT
//
// Delta-based undo/redo system.
//
// Compact delta ring (16k+ ops on PSRAM, ~512 SRAM-only) plus a small
// PSRAM-resident snapshot ring that backs non-symmetric ops (clear-all).
//
// History is *persistent across reboots* on every board: a bounded slice of
// the ring is written to /undo.hist (through the write-back file cache) on
// every slot save, and restored at boot in undoInit(). PSRAM units keep a deep
// history (256 states); units without PSRAM keep the most recent 32. Clear-all
// stores the entire board as YAML so undoing it restores the full state.
//
// Hooks: undoBeginTxn(label, source) / undoRecord* / undoEndTxn (auto-
// closes after 200ms of quiescence).
// User-facing: undoUndo / undoRedo / undoScrubTo / undoCanUndo / undoCanRedo.

#ifndef UNDO_H
#define UNDO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Op types - 1 byte each. Mostly symmetric (apply / revert can be done
// without external context). OP_BLOB references the blob arena for
// non-symmetric ops like clear-all that need to remember the old state.
typedef enum {
    UNDO_OP_NONE = 0,
    UNDO_OP_CONNECT,        // bridge added
    UNDO_OP_DISCONNECT,     // bridge removed
    UNDO_OP_DAC_SET,        // DAC channel voltage changed
    UNDO_OP_GPIO_SET,       // GPIO output value changed
    UNDO_OP_GPIO_DIR,       // GPIO direction changed
    UNDO_OP_SLOT_SWITCH,    // active slot changed
    UNDO_OP_CLEAR_ALL,      // clearAllConnections (uses blob for old bridges)
    UNDO_OP_BLOB_REPLACE,   // generic "replace state" with blob backing
} UndoOpType;

// Source of a transaction - drives label colors and Python integration.
typedef enum {
    UNDO_SRC_UNKNOWN = 0,
    UNDO_SRC_PROBE,
    UNDO_SRC_MENU,
    UNDO_SRC_PYTHON,
    UNDO_SRC_MCP,
    UNDO_SRC_FILE,
    UNDO_SRC_INTERNAL,
} UndoSource;

// Transaction flags
#define UNDO_TXN_WAYPOINT       0x01u  // shown when scrubbing
#define UNDO_TXN_COMMITTED      0x02u  // transaction is closed
#define UNDO_TXN_USER_INITIATED 0x04u  // user pressed a button (vs internal)

// Public lifecycle
void undoInit(void);
void undoShutdown(void);

// Cross-reboot persistence.
//
// undoPersistHistory() serializes the most-recent bounded slice of the shared
// undo ring to /undo.hist via the write-back file cache. It's cheap and
// idempotent (early-outs when nothing changed since the last persist), so it's
// safe to call from the slot-save path on every save. undoRestore() reloads
// that file into the freshly-allocated ring and is called from undoInit();
// it's exposed for reset/format paths that want to re-seed history.
void undoPersistHistory(void);
bool undoRestore(void);

// Wipe all persisted + in-RAM undo/redo history. Deletes the history file(s)
// from flash, clears the shared ring (ops/txns/cursors), invalidates the
// snapshot ring, and resets the dirty flag so nothing is re-persisted. Safe to
// call before undoInit() (it still deletes the on-flash file). Used by the
// debug menu and the first-start calibration flow to start from clean history.
void undoWipeHistory(void);

// Notify the undo system that the active slot has changed. The system
// keeps a separate ring (lazily allocated) per slot, so undo/redo only
// ever walks the history of the currently-active slot - you can't redo
// an action from slot 0 while you're sitting on slot 1.
//
// Most call sites don't actually need to call this: every public API
// auto-detects slot changes by re-reading the global `netSlot`. But
// callers that just mutated `netSlot` themselves can call this for a
// tighter, race-free swap (the auto-detect only catches the change on
// the next API entry point).
void undoOnSlotSwitch(int newSlot);

// Bracket a "system is loading state" region. Inside the bracket, all
// record* calls become no-ops - the mutations are not user actions,
// they're the system bringing globalState into sync with a saved file.
// Nested calls are stacked: the outermost end pops the depth back to
// zero and resumes recording.
//
// Pair this around: slot file loads, boot-time state restores, Wokwi
// imports, and any other batch state-ingest path. Without it, every
// addConnection / setRailVoltage that runs during a slot switch
// pollutes the new slot's history with phantom "user actions".
void undoBeginIngest(void);
void undoEndIngest(void);
bool undoIsIngesting(void);

// Transaction control. beginTxn opens a new transaction - subsequent
// undoRecord* calls attach to it. endTxn closes it. If you call recordX
// without an open transaction, an implicit one is opened with a default
// label.
void undoBeginTxn(const char* label, UndoSource source);
void undoEndTxn(void);
bool undoTxnInProgress(void);

// Record op helpers. Called from the mutation hooks in States.cpp,
// Commands.cpp, Probing.cpp, JumperlessMicroPythonAPI.cpp.
void undoRecordConnect(int node1, int node2, uint32_t color);
void undoRecordDisconnect(int node1, int node2, uint32_t color);
void undoRecordClearAll(void);
void undoRecordDacSet(int channel, float prevVolts, float nextVolts);
void undoRecordGpioSet(int pin, int prevVal, int nextVal);
void undoRecordGpioDir(int pin, int prevDir, int nextDir);
void undoRecordSlotSwitch(int prevSlot, int nextSlot);

// User-facing controls
bool undoCanUndo(void);
bool undoCanRedo(void);
bool undoUndo(void);   // step back one transaction
bool undoRedo(void);   // step forward one transaction

// Cursor model:
//  position = 0 means head of history (live state == latest commit)
//  position = -N means N transactions behind the head
//  position = +K means K transactions ahead (after Undo, into redo land)
int  undoPosition(void);
int  undoTotalTxns(void);   // total txns in the ring (excluding redo tail)
const char* undoLabelAt(int relativeOffset);  // 0 = current, -1 = previous, etc.
// Action length (split index) of the label at `relativeOffset` - the byte
// offset where the detail begins (action = label[0..N), detail = label[N+1..]).
// -1 if there's no txn. Used by undoToast() to separate the small "action" from
// the large "detail" since the action itself may contain spaces ("Top Rail").
int  undoLabelSplitAt(int relativeOffset);

// History menu / scrubbing helpers - move to a specific waypoint position.
// Walks deltas as needed. Returns true on success.
bool undoScrubTo(int targetPosition);

// Returns the position of the next/prev waypoint in scrubbing direction.
// dir > 0 = older (undo), dir < 0 = newer (redo).
int  undoNextWaypoint(int fromPosition, int dir);

// Snapshot helpers (used internally + by debug commands).
// Snapshots back OP_CLEAR_ALL. They live in PSRAM only - no flash copy.
void undoMaybeTakeSnapshot(const char* reason);
bool undoForceSnapshot(const char* reason);
int  undoSnapshotCount(void);

void undoDumpStatus(void);

// Per-area debug flag. When non-zero, undo records / apply / revert /
// snapshot operations emit per-step Serial traces. psram_debug (defined
// in PsramArena.h) acts as a master that also enables this.
extern volatile int undo_debug;

// OLED toast helpers - paint a 2-line "undo|redo\n<label>" on the OLED.
// undoPeekUndoLabel() / undoPeekRedoLabel() return the label of the txn
// that the NEXT undoUndo() / undoRedo() would consume, so callers can
// capture the label before invoking the action.
// `actionLen` is the label's action/detail split index (see undoLabelSplitAt);
// pass -1 if unknown and the toast will fall back to splitting on the first
// space. Capture it alongside the peeked label, before stepping undo/redo.
void        undoToast(bool isRedo, const char* label, int actionLen);
const char* undoPeekUndoLabel(void);
const char* undoPeekRedoLabel(void);

// Light the logo LED yellow for `durationMs` (bumps the existing window
// out if a longer one is already active). Called by undoToast() for
// each step and by ProbeButton when the hold-scroll gesture arms.
void        undoFlashLogo(uint32_t durationMs);

// Scrub guard - while true, mutation hooks short-circuit (we're applying
// deltas internally, not recording new ones).
extern volatile bool g_undoApplying;

#ifdef __cplusplus
}

// RAII helper: suppress undo/redo recording for the lifetime of the guard.
// Wraps undoBeginIngest()/undoEndIngest() so a function with multiple return
// paths (e.g. routableBufferPower) can suppress recording of its internal,
// system-managed state changes without threading undoEndIngest() through every
// exit. Nesting-safe - ingest depth is a counter, so guards can stack.
struct UndoIngestGuard {
    UndoIngestGuard()  { undoBeginIngest(); }
    ~UndoIngestGuard() { undoEndIngest(); }
    UndoIngestGuard(const UndoIngestGuard&) = delete;
    UndoIngestGuard& operator=(const UndoIngestGuard&) = delete;
};
#endif

#endif  // UNDO_H
