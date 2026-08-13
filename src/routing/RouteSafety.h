// SPDX-License-Identifier: MIT
#ifndef ROUTESAFETY_H
#define ROUTESAFETY_H

#include <Arduino.h>
#include <stdint.h>
#include "MatrixState.h"

// Reserved net id for ephemeral fastConnectPath lane claims (int8_t-safe).
// NOTE: like FAKE_GPIO_TDM_NET (58), this sentinel aliases a real net index
// on a board with 58+ user nets - accepted, matches the existing pattern.
#define EPHEMERAL_PATH_NET (MAX_NETS - 1) // 59

// Bumped by clearAllNTCC() / sendPaths() so stale ephemeral handles become
// no-ops. Incremented from both cores without atomics on purpose: a lost
// increment still CHANGES the value, which is all the staleness check needs.
extern volatile uint32_t routingGeneration;

// Per-chip bits set when sendXYraw PIO handshake times out (bookkeeping may lie).
extern volatile uint16_t chipXYSuspectMask;

// sendXYraw short-check refusals (readable from Core 0; no USB I/O on Core 1).
extern volatile uint32_t sendxy_blocked_count;

// Default ON. Debug command can disable if a legitimate pattern is refused.
extern volatile bool sendXYrawCheckEnabled;

struct FastPathHandle {
    pathStruct path;
    uint32_t generation = 0;
    bool active = false;
};

void initRouteSafety(void);

// Gate bridgesToPaths() output: skip paths that would short distinct nets /
// driven sources. Returns number of paths skipped.
int validateAllPaths(void);

// True if adding these hops on top of believed hardware state (lastChipXY)
// would create a short. allowedNet >= 0 means that net is permitted in the
// merged component (fastConnectPath of a node already in that net).
bool wouldShort(const int8_t* hopChip, const int8_t* hopX, const int8_t* hopY,
                int nHops, int allowedNet = -1);

// Single closing crosspoint vs lastChipXY (used by checked sendXYraw).
bool wouldShortCrosspoint(int chip, int x, int y);

// Same, but with clearBits on (clearChip, clearY) simulated open first, so
// the chip-K source auto-disconnect can be evaluated without touching
// hardware until the connect is approved. clearBits == 0 is a plain check.
bool wouldShortCrosspointMasked(int chip, int x, int y, int clearChip,
                                int clearY, uint16_t clearBits);

// Dry-run: fill a pathStruct with a conflict-free route without claiming or
// sending. Returns true if a route exists right now.
bool planFastPath(int nodeA, int nodeB, pathStruct* out);

// Allocate + claim + close a conflict-free ephemeral path. Fills *out.
// Returns 0 on success, negative on failure.
int fastConnectPath(int nodeA, int nodeB, FastPathHandle* out,
                    unsigned long hopTimeoutUs = 2000);

// Open the path's crosspoints and release lane claims. No-op if generation
// is stale (a full refresh already wiped the hardware state).
void fastDisconnectPath(FastPathHandle* p);

// Re-send "open" for every crosspoint bookkeeping believes is open on the
// lanes about to be used (divergence recovery / §2 experiment).
void reassertOpenOnLanes(const int8_t* hopChip, const int8_t* hopX,
                         const int8_t* hopY, int nHops);

void markChipXYSuspect(int chip);
void clearChipXYSuspect(void);
bool anyChipXYSuspect(void);

// Assert-based synthetic cases + lastChipXY audit. Returns 0 if all pass.
int routeSafetySelfCheck(Stream* out = &Serial);
void auditLastChipXY(Stream* out = &Serial);

#endif // ROUTESAFETY_H
