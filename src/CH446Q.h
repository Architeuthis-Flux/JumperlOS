// SPDX-License-Identifier: MIT
#ifndef CH446Q_H
#define CH446Q_H

#include <stdint.h>  // For uint16_t
#include <Arduino.h>
#include "Jerial.h"


extern int netNumberC2;
extern int onOffC2;
extern int nodeC2;
extern int brightnessC2;
extern int hueShiftC2;
extern int lightUpNetCore2;

struct justXY {
    bool connected[16][8]; // 16 X values, 8 Y values, stores whether a connection exists
    };

// Bitfield version - 128 bits (16 bytes) per chip vs 128 bytes for bool array
// Memory efficient for storing chipXY state snapshots in fake GPIO configs
// MUCH faster and more memory-efficient: 16 bytes vs 128 bytes (8x smaller, fits better in cache)
struct chipXYBitfield {
    uint16_t connected[8]; // Each uint16_t stores 16 X values for one Y
    // Bit operations: connected[y] & (1 << x) to test
    // connected[y] |= (1 << x) to set
    // connected[y] &= ~(1 << x) to clear
};

// Use bitfield version for lastChipXY (global state tracking)
extern chipXYBitfield lastChipXY[12];

// Incremented by sendPaths() whenever crosspoints are (re)sent - lets cached
// row->ADC paths (measurement overlay) detect that routing may have changed.
extern volatile uint32_t crossbarPathGeneration;

// Bitfield helper functions
inline bool getConnectionBit(const chipXYBitfield& state, int x, int y) {
    if (x < 0 || x >= 16 || y < 0 || y >= 8) return false;
    return (state.connected[y] & (1 << x)) != 0;
}

inline void setConnectionBit(chipXYBitfield& state, int x, int y, bool value) {
    if (x < 0 || x >= 16 || y < 0 || y >= 8) return;
    if (value) {
        state.connected[y] |= (1 << x);
    } else {
        state.connected[y] &= ~(1 << x);
    }
}

void copyJustXYToBitfield(const struct justXY& src, chipXYBitfield& dst);
void copyBitfieldToJustXY(const chipXYBitfield& src, struct justXY& dst);

void sendPaths(int clean = 0);
void initCH446Q(void);
void sendXYraw(int chip, int x, int y, int setorclear);

void sendAllPaths(int clean = 0); // should we sort them by chip? for now, no

void sendPath(int path, int setOrClear = 1, int newOrLast = 0);
void findDifferentPaths(void);
void createXYarray(void);
void refreshPaths(void);
void sortPathsByChipXY(void);
void printChipStateArray(Stream *stream = &Jerial);
void printChipStateArrayColor(Stream *stream = &Jerial);  // Color-coded version showing net ownership
void printChipStateArrayColorCompact(int chipsPerRow = 6, char blankChar = ' ', Stream *stream = &Jerial);  // Compact color version
void updateChipStateArray(void);

// Live crossbar display - shows at top of terminal, updates on connection changes
// Use LiveCrossbarService for periodic refresh (via jOS.serviceAll())
extern bool liveCrossbarEnabled;
void setLiveCrossbarEnabled(bool enabled);
void updateLiveCrossbarDisplay(void);  // Called by LiveCrossbarService
void createChipOrderedIndex(void);
void printLastChipStateArray(void);

// ChipXY state management for fake GPIO
void applyChipXYState(const chipXYBitfield targetState[12]);
void captureCurrentChipXYState(chipXYBitfield snapshot[12]);

// Chip K-safe versions for INPUT FakeGPIO pins
// These exclude chip K (index 10) to prevent interference with OUTPUT pin voltage switching
void applyChipXYStateExcludeChipK(const chipXYBitfield targetState[12]);
void captureCurrentChipXYStateExcludeChipK(chipXYBitfield snapshot[12]);

#endif
