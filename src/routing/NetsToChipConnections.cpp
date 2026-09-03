// SPDX-License-Identifier: MIT

#include "NetsToChipConnections.h"
#include "boards/board.h"

#include <Arduino.h>
#include <EEPROM.h>

// Compatibility for clangd - these are provided by Arduino.h at compile time
#ifndef malloc
extern void *malloc(size_t size);
#endif
#ifndef free
extern void free(void *ptr);
#endif
#ifndef strlen
extern size_t strlen(const char *str);
#endif
#ifndef strcpy
extern char *strcpy(char *dest, const char *src);
#endif

#include "CH446Q.h"
#include "Commands.h"
#include "Graphics.h"
#include "JumperlessDefines.h"
#include "MatrixState.h"
#include "States.h"
#include "NetManager.h"
#include "Peripherals.h"
#include "Probing.h"
#include "FakeGpio.h"
#include "RouteSafety.h"
#include "InfraPaths.h"
//#include "SerialWrapper.h"

//#define Serial SerialWrap

// Compile-time debug flags - set to 1 to enable, 0 to disable
#ifndef DEBUG_NTCC1_ENABLED
#define DEBUG_NTCC1_ENABLED 0  // Basic path routing debug
#endif

#ifndef DEBUG_NTCC2_ENABLED
#define DEBUG_NTCC2_ENABLED 0  // Detailed routing and alt paths
#endif

#ifndef DEBUG_NTCC3_ENABLED
#define DEBUG_NTCC3_ENABLED 0  // Path conflicts and overlaps
#endif

#ifndef DEBUG_NTCC5_ENABLED
#define DEBUG_NTCC5_ENABLED 0  // Bridge-to-path conversion debug
#endif

#ifndef DEBUG_NTCC6_ENABLED
#define DEBUG_NTCC6_ENABLED 0  // Transaction state and conflict validation
#endif

// Debug macros - completely removed at compile time when disabled
#if DEBUG_NTCC1_ENABLED
  #define DEBUG_NTCC1_PRINT(x) do { if(debugNTCC) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC1_PRINTLN(x) do { if(debugNTCC) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC1_PRINTF(fmt, ...) do { if(debugNTCC) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC1_PRINT(x)
  #define DEBUG_NTCC1_PRINTLN(x)
  #define DEBUG_NTCC1_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC2_ENABLED
  #define DEBUG_NTCC2_PRINT(x) do { if(debugNTCC2) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC2_PRINTLN(x) do { if(debugNTCC2) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC2_PRINTF(fmt, ...) do { if(debugNTCC2) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC2_PRINT(x)
  #define DEBUG_NTCC2_PRINTLN(x)
  #define DEBUG_NTCC2_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC3_ENABLED
  #define DEBUG_NTCC3_PRINT(x) do { if(debugNTCC3) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC3_PRINTLN(x) do { if(debugNTCC3) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC3_PRINTF(fmt, ...) do { if(debugNTCC3) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC3_PRINT(x)
  #define DEBUG_NTCC3_PRINTLN(x)
  #define DEBUG_NTCC3_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC5_ENABLED
  #define DEBUG_NTCC5_PRINT(x) do { if(debugNTCC5) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC5_PRINTLN(x) do { if(debugNTCC5) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC5_PRINTF(fmt, ...) do { if(debugNTCC5) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC5_PRINT(x)
  #define DEBUG_NTCC5_PRINTLN(x)
  #define DEBUG_NTCC5_PRINTF(fmt, ...)
#endif

#if DEBUG_NTCC6_ENABLED
  #define DEBUG_NTCC6_PRINT(x) do { if(debugNTCC6) { Serial.print(x); } } while(0)
  #define DEBUG_NTCC6_PRINTLN(x) do { if(debugNTCC6) { Serial.println(x); } } while(0)
  #define DEBUG_NTCC6_PRINTF(fmt, ...) do { if(debugNTCC6) { Serial.printf(fmt, ##__VA_ARGS__); } } while(0)
#else
  #define DEBUG_NTCC6_PRINT(x)
  #define DEBUG_NTCC6_PRINTLN(x)
  #define DEBUG_NTCC6_PRINTF(fmt, ...)
#endif

// ============================================================================
// Fake GPIO Input TDM Net Merging
// ============================================================================
// Since fake GPIO inputs are time-domain multiplexed (only one connected at a 
// time via chip K Y switching), they can share paths. To allow this, we
// temporarily merge all FAKE_GP_IN paths into a single net during routing,
// then restore their original net numbers afterward for proper LED coloring.

// Forward declarations for variables defined later in this file
extern bool debugNTCC5;
extern volatile int numberOfPaths;

// Storage for original net numbers of fake GPIO input paths
static int fakeGpioInputOriginalNets[MAX_BRIDGES];
static int fakeGpioInputPathIndices[MAX_BRIDGES];
static int numFakeGpioInputPaths = 0;

// Check if a path is a FAKE_GP_IN path by checking its node endpoints directly.
// IMPORTANT: Only checks the path's own node1/node2 - does NOT do a bridge lookup.
// A bridge lookup would false-positive on paths like 49-52 where node 49 also
// appears in a separate FGPI bridge (49-FGPI). We only want to merge actual
// FGPI paths (where one endpoint IS a FAKE_GP_IN virtual node).
// This is safe because the merge runs right after sortPathsByNet() but before
// findStartAndEndChips(), so node values haven't been expanded yet.
static bool pathIsFakeGpioInput(int pathIdx) {
    int pathNode1 = globalState.connections.paths[pathIdx].node1;
    int pathNode2 = globalState.connections.paths[pathIdx].node2;
    
    return IS_FAKE_GP_IN(pathNode1) || IS_FAKE_GP_IN(pathNode2);
}

// Temporarily merge all fake GPIO input paths into FAKE_GPIO_TDM_NET for routing
// This allows TDM paths to share Y positions since same-net paths don't conflict
static void mergeFakeGpioInputNets(void) {
    numFakeGpioInputPaths = 0;
    
    // Find all fake GPIO input paths and collect their net numbers
    for (int i = 0; i < numberOfPaths; i++) {
        if (pathIsFakeGpioInput(i)) {
            // Store original net number
            fakeGpioInputOriginalNets[numFakeGpioInputPaths] = globalState.connections.paths[i].net;
            fakeGpioInputPathIndices[numFakeGpioInputPaths] = i;
            numFakeGpioInputPaths++;
            
            if (numFakeGpioInputPaths >= MAX_BRIDGES) break;
        }
    }
    
    // Merge all fake GPIO input paths to use the special TDM net
    if (numFakeGpioInputPaths > 1) {
        if (debugNTCC5) {
            Serial.print("TDM merge: merging ");
            Serial.print(numFakeGpioInputPaths);
            Serial.print(" fake GPIO input paths into TDM net ");
            Serial.println(FAKE_GPIO_TDM_NET);
        }
        
        for (int i = 0; i < numFakeGpioInputPaths; i++) {
            int pathIdx = fakeGpioInputPathIndices[i];
            globalState.connections.paths[pathIdx].net = FAKE_GPIO_TDM_NET;
        }
    }
}

// Restore original net numbers after routing is complete,
// and fix chip status entries that were written with the TDM net number
static void restoreFakeGpioInputNets(void) {
    if (numFakeGpioInputPaths <= 1) {
        return;  // Nothing to restore (0 or 1 FGPI paths don't need merging)
    }
    
    if (debugNTCC5) {
        Serial.print("TDM restore: restoring ");
        Serial.print(numFakeGpioInputPaths);
        Serial.println(" fake GPIO input paths to original nets");
    }
    
    // Phase 1: Restore original FGPI path net numbers
    for (int i = 0; i < numFakeGpioInputPaths; i++) {
        int pathIdx = fakeGpioInputPathIndices[i];
        globalState.connections.paths[pathIdx].net = fakeGpioInputOriginalNets[i];
        
        if (debugNTCC5) {
            Serial.print("  path ");
            Serial.print(pathIdx);
            Serial.print(" -> net ");
            Serial.println(fakeGpioInputOriginalNets[i]);
        }
    }
    
    // Phase 2: Fix any remaining paths with TDM net (duplicates created during routing)
    for (int i = 0; i < numberOfPaths; i++) {
        if (globalState.connections.paths[i].net == FAKE_GPIO_TDM_NET) {
            // Find the real net by matching node1/node2 against original FGPI paths
            int realNet = -1;
            for (int j = 0; j < numFakeGpioInputPaths; j++) {
                int origIdx = fakeGpioInputPathIndices[j];
                if (globalState.connections.paths[i].node1 == globalState.connections.paths[origIdx].node1 &&
                    globalState.connections.paths[i].node2 == globalState.connections.paths[origIdx].node2) {
                    realNet = fakeGpioInputOriginalNets[j];
                    break;
                }
            }
            if (realNet > 0) {
                globalState.connections.paths[i].net = realNet;
                if (debugNTCC5) {
                    Serial.print("  duplicate path ");
                    Serial.print(i);
                    Serial.print(" -> net ");
                    Serial.println(realNet);
                }
            }
        }
    }
    
    // Phase 3: Fix ALL chip status entries that still show FAKE_GPIO_TDM_NET
    // Routing may set xStatus and yStatus entries through intermediate hops, 
    // alt paths, etc. that aren't captured by the path's 4-hop arrays.
    // Brute-force scan all 12 chips' xStatus[16] and yStatus[8].
    for (int chip = 0; chip < 12; chip++) {
        for (int x = 0; x < 16; x++) {
            if (globalState.connections.chipStates[chip].xStatus[x] == FAKE_GPIO_TDM_NET) {
                // Find which restored path uses this chip+x position
                int realNet = -1;
                for (int p = 0; p < numberOfPaths; p++) {
                    if (globalState.connections.paths[p].net == FAKE_GPIO_TDM_NET) continue;
                    for (int hop = 0; hop < 4; hop++) {
                        if (globalState.connections.paths[p].chip[hop] == chip &&
                            globalState.connections.paths[p].x[hop] == x) {
                            realNet = globalState.connections.paths[p].net;
                            break;
                        }
                    }
                    if (realNet >= 0) break;
                }
                globalState.connections.chipStates[chip].xStatus[x] = realNet;
            }
        }
        for (int y = 0; y < 8; y++) {
            if (globalState.connections.chipStates[chip].yStatus[y] == FAKE_GPIO_TDM_NET) {
                // Find which restored path uses this chip+y position
                int realNet = -1;
                for (int p = 0; p < numberOfPaths; p++) {
                    if (globalState.connections.paths[p].net == FAKE_GPIO_TDM_NET) continue;
                    for (int hop = 0; hop < 4; hop++) {
                        if (globalState.connections.paths[p].chip[hop] == chip &&
                            globalState.connections.paths[p].y[hop] == y) {
                            realNet = globalState.connections.paths[p].net;
                            break;
                        }
                    }
                    if (realNet >= 0) break;
                }
                globalState.connections.chipStates[chip].yStatus[y] = realNet;
            }
        }
    }
    
    // Clear state
    numFakeGpioInputPaths = 0;
}

// Convenience macro for any NTCC debug output
#if DEBUG_NTCC1_ENABLED || DEBUG_NTCC2_ENABLED || DEBUG_NTCC3_ENABLED || DEBUG_NTCC5_ENABLED || DEBUG_NTCC6_ENABLED
  #define DEBUG_NTCC_ANY_ENABLED 1
#else
  #define DEBUG_NTCC_ANY_ENABLED 0
#endif

/*
 * USAGE EXAMPLES FOR CONVERTING DEBUG CODE:
 * 
 * OLD: if (debugNTCC2) { Serial.print("Value: "); Serial.println(value); }
 * NEW: DEBUG_NTCC2_PRINT("Value: "); DEBUG_NTCC2_PRINTLN(value);
 * 
 * OLD: if (debugNTCC6) { Serial.printf("Chip %c at %d,%d\n", chip, x, y); }
 * NEW: DEBUG_NTCC6_PRINTF("Chip %c at %d,%d\n", chip, x, y);
 * 
 * For expensive validation functions:
 * OLD: someExpensiveValidationFunction();
 * NEW: #if DEBUG_NTCC6_ENABLED
 *      someExpensiveValidationFunction();
 *      #endif
 * 
 * Debug level meanings:
 * NTCC1 - Basic path routing (replaces debugNTCC)
 * NTCC2 - Detailed routing and alt paths (replaces debugNTCC2) 
 * NTCC3 - Path conflicts and overlaps (replaces debugNTCC3)
 * NTCC5 - Bridge-to-path conversion (replaces debugNTCC5)
 * NTCC6 - Transaction state validation (replaces debugNTCC6)
 */

// don't try to understand this, it's still a mess
bool debugNTCC5 = false;
int startEndChip[2] = {-1, -1};
int bothNodes[2] = {-1, -1};
int endChip = -1;
int chipCandidates[2][4] = {
    {-1, -1, -1, -1},
    {-1, -1, -1,
     -1}}; // nano and sf nodes have multiple possible chips they could be
// connected to, so we need to store them all and check them all

int chipsLeastToMostCrowded[12] = {
    0, 1, 2, 3, 4,  5,
    6, 7, 8, 9, 10, 11}; // this will be sorted from most to least crowded, and
// will be used to determine which chip to use for
// each node
int sfChipsLeastToMostCrowded[4] = {
    8, 9, 10, 11}; // this will be sorted from most to least crowded, and will
// be used to determine which chip to use for each node

int numberOfUniqueNets = 0;
int numberOfNets = 0;
volatile int numberOfPaths = 0;

int pathsWithCandidates[MAX_BRIDGES] = {0};
int pathsWithCandidatesIndex = 0;

int numberOfUnconnectablePaths = 0;
int unconnectablePaths[10][2] = {
    {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
    {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
};
unsigned long timeToSort = 0;

bool debugNTCC = 0; // EEPROM.read(DEBUG_NETTOCHIPCONNECTIONSADDRESS);

bool debugNTCC2 = 1; // EEPROM.read(DEBUG_NETTOCHIPCONNECTIONSALTADDRESS);

bool debugNTCC3 = false;

bool debugNTCC4 = true; // Debug for forcing path updates

bool debugNTCC6 = false; // Debug for ijkl paths and direct connections

// Helper function to enable overlap debugging
void enableOverlapDebugging(bool enable) {
  debugNTCC3 = enable;
  if (enable) {
    Serial.println("Overlap debugging enabled - will show detailed conflict information");
  } else {
    Serial.println("Overlap debugging disabled");
  }
}

// State backup structures for transactional assignment
struct ChipStateBackup {
  int8_t xStatus[16];
  int8_t yStatus[8];
};

struct PathStateBackup {
  int net;
  int node1;
  int node2;
  int chip[4];
  int x[6];
  int y[6];
  bool altPathNeeded;
  bool sameChip;
  bool skip;
};




static ChipStateBackup chipBackup[12];
static PathStateBackup pathBackup;
static bool backupValid = false;

// Helper function to save current chip and path state
void saveRoutingState(int pathIndex) {
  // Save all chip states
  for (int i = 0; i < 12; i++) {
    for (int j = 0; j < 16; j++) {
      chipBackup[i].xStatus[j] = globalState.connections.chipStates[i].xStatus[j];
    }
    for (int j = 0; j < 8; j++) {
      chipBackup[i].yStatus[j] = globalState.connections.chipStates[i].yStatus[j];
    }
  }

  // Save path state
  pathBackup.net = globalState.connections.paths[pathIndex].net;
  pathBackup.node1 = globalState.connections.paths[pathIndex].node1;
  pathBackup.node2 = globalState.connections.paths[pathIndex].node2;
  pathBackup.altPathNeeded = globalState.connections.paths[pathIndex].altPathNeeded;
  pathBackup.sameChip = globalState.connections.paths[pathIndex].sameChip;
  pathBackup.skip = globalState.connections.paths[pathIndex].skip;

  for (int i = 0; i < 4; i++) {
    pathBackup.chip[i] = globalState.connections.paths[pathIndex].chip[i];
  }
  for (int i = 0; i < 6; i++) {
    pathBackup.x[i] = globalState.connections.paths[pathIndex].x[i];
    pathBackup.y[i] = globalState.connections.paths[pathIndex].y[i];
  }

  backupValid = true;

  DEBUG_NTCC6_PRINT("State saved for globalState.connections.paths[");
  DEBUG_NTCC6_PRINT(pathIndex);
  DEBUG_NTCC6_PRINTLN("]");
}

// Helper function to restore chip and path state from backup
void restoreRoutingState(int pathIndex) {
  if (!backupValid) {
    if (debugNTCC6) {
      Serial.println(
          "ERROR: Attempted to restore state but no backup available!");
    }
    return;
  }

  // Restore all chip states
  for (int i = 0; i < 12; i++) {
    for (int j = 0; j < 16; j++) {
      globalState.connections.chipStates[i].xStatus[j] = chipBackup[i].xStatus[j];
    }
    for (int j = 0; j < 8; j++) {
      globalState.connections.chipStates[i].yStatus[j] = chipBackup[i].yStatus[j];
    }
  }

  // Restore path state
  globalState.connections.paths[pathIndex].net = pathBackup.net;
  globalState.connections.paths[pathIndex].node1 = pathBackup.node1;
  globalState.connections.paths[pathIndex].node2 = pathBackup.node2;
  globalState.connections.paths[pathIndex].altPathNeeded = pathBackup.altPathNeeded;
  globalState.connections.paths[pathIndex].sameChip = pathBackup.sameChip;
  globalState.connections.paths[pathIndex].skip = pathBackup.skip;

  for (int i = 0; i < 4; i++) {
    globalState.connections.paths[pathIndex].chip[i] = pathBackup.chip[i];
  }
  for (int i = 0; i < 6; i++) {
    globalState.connections.paths[pathIndex].x[i] = pathBackup.x[i];
    globalState.connections.paths[pathIndex].y[i] = pathBackup.y[i];
  }

  DEBUG_NTCC6_PRINT("State restored for globalState.connections.paths[");
  DEBUG_NTCC6_PRINT(pathIndex);
  DEBUG_NTCC6_PRINTLN("]");
}

// Helper function to commit current state (clear backup)
void commitRoutingState(void) {
  backupValid = false;
  if (debugNTCC6) {
    Serial.println("State committed (backup cleared)");
  }
}

// Helper function to validate state consistency after transactions
void validateTransactionConsistency(void) {
#if DEBUG_NTCC6_ENABLED
  DEBUG_NTCC6_PRINTLN("\n=== TRANSACTION CONSISTENCY CHECK ===");

    // Check for paths that have assigned positions but skip flag is set
    for (int i = 0; i < numberOfPaths; i++) {
      if (globalState.connections.paths[i].skip) {
        bool hasAssignedPositions = false;
        for (int j = 0; j < 4; j++) {
          if (globalState.connections.paths[i].x[j] >= 0 || globalState.connections.paths[i].y[j] >= 0) {
            hasAssignedPositions = true;
            break;
          }
        }
        if (hasAssignedPositions) {
          Serial.print("INCONSISTENCY: Path[");
          Serial.print(i);
          Serial.println("] has skip=true but has assigned positions");
        }
      }
    }

    // Check for chip positions assigned to multiple nets
    for (int chip = 0; chip < 12; chip++) {
      for (int x = 0; x < 16; x++) {
        if (globalState.connections.chipStates[chip].xStatus[x] > 0) {
          int pathCount = 0;
          for (int p = 0; p < numberOfPaths; p++) {
            if (globalState.connections.paths[p].skip)
              continue;
            for (int seg = 0; seg < 4; seg++) {
              if (globalState.connections.paths[p].chip[seg] == chip && globalState.connections.paths[p].x[seg] == x) {
                pathCount++;
              }
            }
          }
          if (pathCount == 0) {
            Serial.print("INCONSISTENCY: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" X[");
            Serial.print(x);
            Serial.print("] assigned to net ");
            Serial.print(globalState.connections.chipStates[chip].xStatus[x]);
            Serial.println(" but no paths use it");
          }
        }
      }

      for (int y = 0; y < 8; y++) {
        if (globalState.connections.chipStates[chip].yStatus[y] > 0) {
          int pathCount = 0;
          for (int p = 0; p < numberOfPaths; p++) {
            if (globalState.connections.paths[p].skip)
              continue;
            for (int seg = 0; seg < 4; seg++) {
              if (globalState.connections.paths[p].chip[seg] == chip && globalState.connections.paths[p].y[seg] == y) {
                pathCount++;
              }
            }
          }
          if (pathCount == 0) {
            Serial.print("INCONSISTENCY: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" Y[");
            Serial.print(y);
            Serial.print("] assigned to net ");
            Serial.print(globalState.connections.chipStates[chip].yStatus[y]);
            Serial.println(" but no paths use it");
          }
        }
      }
    }

    DEBUG_NTCC6_PRINTLN("=== END CONSISTENCY CHECK ===\n");
#endif
}

// ============================================================================
// Chip-K y-row reservation for sense taps (bench 2026-08-24)
// ============================================================================
// Chip K's 8 y-rows are the only gateway from the crossbar fabric to ADC0-3.
// The net-voltage scan plans ephemeral sense-tap routes that must terminate on
// a free K y-row. A K-heavy netlist (rails, GND, DAC, rows 29/59) plus
// path-stacking DUPLICATES consumed all 8 K y-rows, so every tap failed
// "noroute" and every net displayed 0.0 mA (hardware diag showed K y4/y5 as
// bit-identical duplicates of y2/y1). DUPLICATE paths are a pure resistance
// optimization - never connectivity - so they must not consume one of the
// LAST TWO virgin K y-rows. Primary paths keep unconditional access
// (connectivity first). The other half of this fix is buildEphemeralRoute's
// same-net-row fallback in RouteSafety.cpp.
//
// routingDuplicatePathNow is set per-iteration by the three routing loops
// (commitPaths / resolveAltPaths / resolveUncommittedHops) from
// paths[i].duplicate, and cleared when each loop ends. It never leaks into
// ephemeral tap planning: RouteSafety.cpp does not call freeOrSameNetY() or
// setChipYStatusSafe().
static bool routingDuplicatePathNow = false;

static int virginChipKYRowCount(void) {
  int count = 0;
  for (int y = 0; y < 8; y++) {
    if (globalState.connections.chipStates[CHIP_K].yStatus[y] == -1) {
      count++;
    }
  }
  return count;
}

// True when a duplicate path may not take this row: the row is virgin on chip
// K and 2 or fewer virgin K rows remain (the candidate row included).
static bool reservedKRowForSenseTaps(int chip, int y) {
  return routingDuplicatePathNow && chip == CHIP_K &&
         globalState.connections.chipStates[CHIP_K].yStatus[y] == -1 &&
         virginChipKYRowCount() <= 2;
}

// ============================================================================
// Sense-tap escape reservation for duplicates (bench 2026-09-03)
// ============================================================================
// A sense tap from a breadboard row reaches the ADCs on chip K through the
// row's own chip (its K lane, or an I/J/L lane and a hub lane) or by
// bouncing through another breadboard chip: that chip's Y0, its K lane and
// its K row. Kevin's LED at 12 mA (meter) showed no ants and 0.0 mA because
// chip D had every lane spent - the K lane by a primary, the I/J lanes by
// the net's OWN stacked copies - and duplicates of the rail net had taken
// five of the eight bounce rows, so no tap could leave D although chip K
// still had two free rows.
//
// Ruling (Kevin, 2026-09-03): keep the stacking, duplicates may still
// bounce, but "reserve a single path": a duplicate may not take a resource
// that belongs to one of the last kTapBounceReserve TAP-CAPABLE bounce
// chips - a breadboard chip whose Y0, K lane and K row are all still
// virgin. Primaries are never refused (connectivity first), same-net
// re-claims are unaffected (the checks only fire on virgin pins), and the
// K-row reservation above keeps working alongside.
static const int kTapBounceReserve = 1;

static int kRowForBounceChip(int n) {
  for (int y = 0; y < 8; y++) {
    if (globalState.connections.chipStates[CHIP_K].yMap[y] == n) {
      return y;
    }
  }
  return -1;
}

// The SF chip's row wired to breadboard chip n (-1 if none).
static int sfRowForBounceChip(int sf, int n) {
  for (int y = 0; y < 8; y++) {
    if (globalState.connections.chipStates[sf].yMap[y] == n) {
      return y;
    }
  }
  return -1;
}

// A tap has four shapes by where its node lives: on a breadboard row
// (kind -1: the bounce needs the chip's Y0, K lane and K row) or on chip
// I, J or L (kind = that chip: the bounce additionally needs the chip's
// lane into it and its row there). A chip is tap-capable for a kind when
// every ingredient of that shape is still virgin.
static const int kTapKinds[4] = {-1, CHIP_I, CHIP_J, CHIP_L};

static bool tapCapableBounceChip(int n, int kind) {
  if (n < 0 || n >= 8) {
    return false;
  }
  int laneK = xMapForChipLane0(n, CHIP_K);
  int rowK = kRowForBounceChip(n);
  if (laneK < 0 || rowK < 0) {
    return false;
  }
  if (globalState.connections.chipStates[n].yStatus[0] != -1 ||
      globalState.connections.chipStates[n].xStatus[laneK] != -1 ||
      globalState.connections.chipStates[CHIP_K].yStatus[rowK] != -1) {
    return false;
  }
  if (kind >= 0) {
    int laneS = xMapForChipLane0(n, kind);
    int rowS = sfRowForBounceChip(kind, n);
    if (laneS < 0 || rowS < 0) {
      return false;
    }
    if (globalState.connections.chipStates[n].xStatus[laneS] != -1 ||
        globalState.connections.chipStates[kind].yStatus[rowS] != -1) {
      return false;
    }
  }
  return true;
}

static int tapCapableBounceChips(int kind) {
  int count = 0;
  for (int n = 0; n < 8; n++) {
    if (tapCapableBounceChip(n, kind)) {
      count++;
    }
  }
  return count;
}

// True when chip n is one of the last kTapBounceReserve chips that can
// bounce a tap of the given kind (onlyKind -2: any kind it serves).
static bool lastTapEscape(int n, int onlyKind) {
  for (int k = 0; k < 4; k++) {
    int kind = kTapKinds[k];
    if (onlyKind != -2 && kind != onlyKind) {
      continue;
    }
    if (tapCapableBounceChip(n, kind) &&
        tapCapableBounceChips(kind) <= kTapBounceReserve) {
      return true;
    }
  }
  return false;
}

// True when a duplicate may not take X pin x of a breadboard chip: the
// pin is that chip's lane into K (needed by every tap kind) or into I/J/L
// (needed by taps of nodes on that chip), and the chip is one of the last
// that can bounce such a tap.
static bool reservedTapEscapeX(int chip, int x) {
  if (!routingDuplicatePathNow || chip < 0 || chip >= 8 || x < 0 || x >= 16) {
    return false;
  }
  int to = globalState.connections.chipStates[chip].xMap[x];
  if (to == CHIP_K) {
    return lastTapEscape(chip, -2);
  }
  if (to == CHIP_I || to == CHIP_J || to == CHIP_L) {
    return lastTapEscape(chip, to);
  }
  return false;
}

// True when a duplicate may not take row y of chip: a breadboard chip's
// bounce row (Y0), or an SF chip's row wired to a breadboard chip that is
// one of the last tap escapes (K's row: every kind; I/J/L's row: taps of
// nodes on that chip).
static bool reservedTapEscapeY(int chip, int y) {
  if (!routingDuplicatePathNow || y < 0 || y >= 8) {
    return false;
  }
  if (chip >= 0 && chip < 8) {
    return y == 0 && lastTapEscape(chip, -2);
  }
  if (chip == CHIP_K || chip == CHIP_I || chip == CHIP_J || chip == CHIP_L) {
    int n = globalState.connections.chipStates[chip].yMap[y];
    if (n < 0 || n >= 8) {
      return false;
    }
    return lastTapEscape(n, chip == CHIP_K ? -2 : chip);
  }
  return false;
}

// ============================================================================
// Chip-K y-row budget for primary paths (bench 2026-09-02)
// ============================================================================
// Every K-resident signal (rails, DACs, ADC0-3, BUF_IN, AREF, rows 29/59, GND
// when it lands on K) leaves chip K through one of its 8 y-rows, so a net with
// a K node needs at least one. The router handed virgin K rows out first-come:
// a rail's second breadboard row, or an ADC net's second node (3-ADC_1, then
// 3-ADC_2 through a different hop chip) each took a fresh row, and the nets
// that have NO alternative to a K bounce - the probe feed BUF_IN<->GP_8 and
// ADC_3<->D1 - found all 8 spent and failed. Kevin's bench: "we should be able
// to find a path here with some hops."
//
// Rule: a net's FIRST K row is unconditional (connectivity first). A second
// row for a net that already holds one, or any row for a net that merely hops
// through K, is granted only while enough virgin rows remain for every
// K-needing net that holds none yet. A refused row falls through to the
// same-net retry (stackingAttempt 1), where the alt-path search piggybacks on
// the net's existing K row through that row's breadboard chip.
//
// The budget only bites when K would otherwise starve a net - with slack the
// routing is identical to before. If a refused path finds no piggyback, a
// rescue pass re-runs the three phases for the unrouted paths with the budget
// off, so nothing that routed before stops routing. The rescue must skip
// fully-routed paths: ijklPaths leaves altPathNeeded set and resets y to -2
// on re-entry, and resolveUncommittedHops(allowStacking=2) is virgin-only, so
// a re-resolved path would refuse its own row and break.
//
// Duplicates are exempt (they run after all primaries and carry the sense-tap
// reservation above). RouteSafety's ephemeral taps never come through here.
static bool kRowBudgetActive = false;
static bool kRowRescuePass = false;
static uint64_t kNeedingNetMask = 0; // nets with a node on chip K (bit = net)

static inline uint64_t netBit(int net) {
  return (net >= 0 && net < 64) ? (1ULL << net) : 0;
}

// Nets with a K-resident node, from the chips resolveChipCandidates chose.
static void computeKNeedingNets(void) {
  kNeedingNetMask = 0;
  for (int i = 0; i < numberOfPaths; i++) {
    struct pathStruct &p = globalState.connections.paths[i];
    if (p.pathType == VIRTUAL || p.duplicate != 0) {
      continue;
    }
    if (p.chip[0] == CHIP_K || p.chip[1] == CHIP_K) {
      kNeedingNetMask |= netBit(p.net);
    }
  }
}

static uint64_t kRowsHeldMask(void) {
  uint64_t held = 0;
  for (int y = 0; y < 8; y++) {
    held |= netBit(globalState.connections.chipStates[CHIP_K].yStatus[y]);
  }
  return held;
}

// True when net may not take the virgin chip-K row y right now.
static bool kRowBudgetRefuses(int chip, int y, int net) {
  if (!kRowBudgetActive || routingDuplicatePathNow || chip != CHIP_K) {
    return false;
  }
  if (globalState.connections.chipStates[CHIP_K].yStatus[y] != -1) {
    return false; // not virgin: the same-net / conflict rules decide
  }
  uint64_t held = kRowsHeldMask();
  uint64_t mine = netBit(net);
  bool needsK = (kNeedingNetMask & mine) != 0;
  bool holdsK = (held & mine) != 0;
  if (needsK && !holdsK) {
    return false; // first row: always
  }
  uint64_t waiting = kNeedingNetMask & ~held & ~mine;
  int waitingCount = __builtin_popcountll(waiting);
  return (virginChipKYRowCount() - 1) < waitingCount;
}

// Every position that has a chip also has both coordinates.
static bool pathFullyRouted(int i) {
  struct pathStruct &p = globalState.connections.paths[i];
  if (p.chip[0] == -1 || p.chip[1] == -1) {
    return false;
  }
  for (int j = 0; j < 4; j++) {
    if (p.chip[j] != -1 && (p.x[j] < 0 || p.y[j] < 0)) {
      return false;
    }
  }
  return true;
}

// Helper function to track direct Y status assignments
void setChipYStatus(int chip, int y, int net, const char *location) {
  if (debugNTCC6 && globalState.connections.chipStates[chip].yStatus[y] != -1 && globalState.connections.chipStates[chip].yStatus[y] != net) {
    DEBUG_NTCC6_PRINT("DIRECT Y ASSIGNMENT CONFLICT: ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" - Chip ");
    DEBUG_NTCC6_PRINT(chipNumToChar(chip));
    DEBUG_NTCC6_PRINT(" Y[");
    DEBUG_NTCC6_PRINT(y);
    DEBUG_NTCC6_PRINT("] occupied by net ");
    DEBUG_NTCC6_PRINT(globalState.connections.chipStates[chip].yStatus[y]);
    DEBUG_NTCC6_PRINT(", overwriting with net ");
    DEBUG_NTCC6_PRINTLN(net);
  }
  globalState.connections.chipStates[chip].yStatus[y] = net;
}

// Helper function to detect and report routing conflicts
void detectAndReportConflicts(void) {
#if DEBUG_NTCC6_ENABLED
  DEBUG_NTCC6_PRINTLN("\n=== CONFLICT DETECTION REPORT ===");

    // Check for X conflicts
    for (int chip = 0; chip < 12; chip++) {
      for (int x = 0; x < 16; x++) {
        if (globalState.connections.chipStates[chip].xStatus[x] > 0) {
          // Count how many paths use this X position
          int pathCount = 0;
          int nets[MAX_NETS];
          int netCount = 0;

          for (int pathIdx = 0; pathIdx < numberOfPaths; pathIdx++) {
            if (globalState.connections.paths[pathIdx].skip)
              continue;

            for (int seg = 0; seg < 4; seg++) {
              if (globalState.connections.paths[pathIdx].chip[seg] == chip &&
                  globalState.connections.paths[pathIdx].x[seg] == x) {
                pathCount++;

                // Track unique nets
                bool netExists = false;
                for (int n = 0; n < netCount; n++) {
                  if (nets[n] == globalState.connections.paths[pathIdx].net) {
                    netExists = true;
                    break;
                  }
                }
                if (!netExists && netCount < MAX_NETS) {
                  nets[netCount++] = globalState.connections.paths[pathIdx].net;
                }
              }
            }
          }

          if (netCount > 1) {
            Serial.print("X CONFLICT: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" X[");
            Serial.print(x);
            Serial.print("] used by nets: ");
            for (int n = 0; n < netCount; n++) {
              Serial.print(nets[n]);
              if (n < netCount - 1)
                Serial.print(", ");
            }
            Serial.println();
          }
        }
      }
    }

    // Check for Y conflicts
    for (int chip = 0; chip < 12; chip++) {
      for (int y = 0; y < 8; y++) {
        if (globalState.connections.chipStates[chip].yStatus[y] > 0) {
          // Count how many paths use this Y position
          int pathCount = 0;
          int nets[MAX_NETS];
          int netCount = 0;

          for (int pathIdx = 0; pathIdx < numberOfPaths; pathIdx++) {
            if (globalState.connections.paths[pathIdx].skip)
              continue;

            for (int seg = 0; seg < 4; seg++) {
              if (globalState.connections.paths[pathIdx].chip[seg] == chip &&
                  globalState.connections.paths[pathIdx].y[seg] == y) {
                pathCount++;

                // Track unique nets
                bool netExists = false;
                for (int n = 0; n < netCount; n++) {
                  if (nets[n] == globalState.connections.paths[pathIdx].net) {
                    netExists = true;
                    break;
                  }
                }
                if (!netExists && netCount < MAX_NETS) {
                  nets[netCount++] = globalState.connections.paths[pathIdx].net;
                }
              }
            }
          }

          if (netCount > 1) {
            Serial.print("Y CONFLICT: Chip ");
            Serial.print(chipNumToChar(chip));
            Serial.print(" Y[");
            Serial.print(y);
            Serial.print("] used by nets: ");
            for (int n = 0; n < netCount; n++) {
              Serial.print(nets[n]);
              if (n < netCount - 1)
                Serial.print(", ");
            }
            Serial.println();
          }
        }
      }
    }

    DEBUG_NTCC6_PRINTLN("=== END CONFLICT DETECTION ===\n");
#endif
}

// Helper function to safely set chip X status with validation
bool setChipXStatus(int chip, int x, int net, const char *location) {
  // Bounds checking to prevent crashes
  if (chip < 0 || chip >= 12 || x < 0 || x >= 16 || net < 0 ||
      net >= MAX_NETS) {
    DEBUG_NTCC6_PRINT("ERROR: Invalid parameters in setChipXStatus - ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" chip=");
    DEBUG_NTCC6_PRINT(chip);
    DEBUG_NTCC6_PRINT(" x=");
    DEBUG_NTCC6_PRINT(x);
    DEBUG_NTCC6_PRINT(" net=");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Invalid parameters, assignment failed
  }

  if (reservedTapEscapeX(chip, x)) {
    return false; // belt and braces: freeOrSameNetX vets this in the same iteration
  }

  // Check for overlap conflicts before assignment
  if (globalState.connections.chipStates[chip].xStatus[x] != -1 && globalState.connections.chipStates[chip].xStatus[x] != net) {
    DEBUG_NTCC6_PRINT("WARNING: X position conflict in ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" - Chip ");
    DEBUG_NTCC6_PRINT(chipNumToChar(chip));
    DEBUG_NTCC6_PRINT(" X[");
    DEBUG_NTCC6_PRINT(x);
    DEBUG_NTCC6_PRINT("] already occupied by net ");
    DEBUG_NTCC6_PRINT(globalState.connections.chipStates[chip].xStatus[x]);
    DEBUG_NTCC6_PRINT(", trying to assign net ");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Assignment failed due to conflict
  }

  // Assign the X position
  globalState.connections.chipStates[chip].xStatus[x] = net;

  DEBUG_NTCC6_PRINT("SUCCESS: ");
  DEBUG_NTCC6_PRINT(location);
  DEBUG_NTCC6_PRINT(" - Assigned X=");
  DEBUG_NTCC6_PRINT(x);
  DEBUG_NTCC6_PRINT(" on chip ");
  DEBUG_NTCC6_PRINT(chipNumToChar(chip));
  DEBUG_NTCC6_PRINT(" to net ");
  DEBUG_NTCC6_PRINTLN(net);

  return true; // Assignment successful
}

// Helper function to safely set chip Y status with validation (improved
// version)
bool setChipYStatusSafe(int chip, int y, int net, const char *location) {
  // Bounds checking to prevent crashes
  if (chip < 0 || chip >= 12 || y < 0 || y >= 8 || net < 0 || net >= MAX_NETS) {
    DEBUG_NTCC6_PRINT("ERROR: Invalid parameters in setChipYStatusSafe - ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" chip=");
    DEBUG_NTCC6_PRINT(chip);
    DEBUG_NTCC6_PRINT(" y=");
    DEBUG_NTCC6_PRINT(y);
    DEBUG_NTCC6_PRINT(" net=");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Invalid parameters, assignment failed
  }

  // Belt-and-braces for the sense-tap reservation: every K-capable write is
  // vetted by freeOrSameNetY() in the same iteration, but refuse here too so
  // no unvetted path can consume a reserved virgin K row.
  if (reservedKRowForSenseTaps(chip, y)) {
    return false;
  }
  if (kRowBudgetRefuses(chip, y, net)) {
    return false;
  }
  if (reservedTapEscapeY(chip, y)) {
    return false;
  }

  // Check for overlap conflicts before assignment
  if (globalState.connections.chipStates[chip].yStatus[y] != -1 && globalState.connections.chipStates[chip].yStatus[y] != net) {
    DEBUG_NTCC6_PRINT("WARNING: Y position conflict in ");
    DEBUG_NTCC6_PRINT(location);
    DEBUG_NTCC6_PRINT(" - Chip ");
    DEBUG_NTCC6_PRINT(chipNumToChar(chip));
    DEBUG_NTCC6_PRINT(" Y[");
    DEBUG_NTCC6_PRINT(y);
    DEBUG_NTCC6_PRINT("] already occupied by net ");
    DEBUG_NTCC6_PRINT(globalState.connections.chipStates[chip].yStatus[y]);
    DEBUG_NTCC6_PRINT(", trying to assign net ");
    DEBUG_NTCC6_PRINTLN(net);
    return false; // Assignment failed due to conflict
  }

  // Assign the Y position
  globalState.connections.chipStates[chip].yStatus[y] = net;

  DEBUG_NTCC6_PRINT("SUCCESS: ");
  DEBUG_NTCC6_PRINT(location);
  DEBUG_NTCC6_PRINT(" - Assigned Y=");
  DEBUG_NTCC6_PRINT(y);
  DEBUG_NTCC6_PRINT(" on chip ");
  DEBUG_NTCC6_PRINT(chipNumToChar(chip));
  DEBUG_NTCC6_PRINT(" to net ");
  DEBUG_NTCC6_PRINTLN(net);

  return true; // Assignment successful
}

// Helper function to safely set path X coordinates with validation
bool setPathX(int pathIndex, int xIndex, int xValue) {
  // Bounds checking to prevent crashes
  if (pathIndex < 0 || pathIndex >= MAX_BRIDGES || xIndex < 0 || xIndex >= 16) {
    return false; // Invalid parameters, assignment failed
  }

  // Allow special values (-2, -1) without conflict checking
  if (xValue < 0) {
    globalState.connections.paths[pathIndex].x[xIndex] = xValue;
    return true;
  }

  // Check for conflicts when overwriting existing real coordinates
  if (globalState.connections.paths[pathIndex].x[xIndex] >= 0 && globalState.connections.paths[pathIndex].x[xIndex] != xValue) {
    DEBUG_NTCC6_PRINT("WARNING: X coordinate conflict - globalState.connections.paths[");
    DEBUG_NTCC6_PRINT(pathIndex);
    DEBUG_NTCC6_PRINT("].x[");
    DEBUG_NTCC6_PRINT(xIndex);
    DEBUG_NTCC6_PRINT("] already set to ");
    DEBUG_NTCC6_PRINT(globalState.connections.paths[pathIndex].x[xIndex]);
    DEBUG_NTCC6_PRINT(", trying to assign ");
    DEBUG_NTCC6_PRINTLN(xValue);
    return false; // Conflict detected
  }

  // Assign the X coordinate
  globalState.connections.paths[pathIndex].x[xIndex] = xValue;
  return true; // Assignment successful
}

// Helper function to safely set path Y coordinates with validation
bool setPathY(int pathIndex, int yIndex, int yValue) {
  // Bounds checking to prevent crashes
  if (pathIndex < 0 || pathIndex >= MAX_BRIDGES || yIndex < 0 || yIndex >= 8) {
    return false; // Invalid parameters, assignment failed
  }

  // Allow special values (-2, -1) without conflict checking
  if (yValue < 0) {
    globalState.connections.paths[pathIndex].y[yIndex] = yValue;
    return true;
  }

  // Check for conflicts when overwriting existing real coordinates
  if (globalState.connections.paths[pathIndex].y[yIndex] >= 0 && globalState.connections.paths[pathIndex].y[yIndex] != yValue) {
    DEBUG_NTCC6_PRINT("WARNING: Y coordinate conflict - globalState.connections.paths[");
    DEBUG_NTCC6_PRINT(pathIndex);
    DEBUG_NTCC6_PRINT("].y[");
    DEBUG_NTCC6_PRINT(yIndex);
    DEBUG_NTCC6_PRINT("] already set to ");
    DEBUG_NTCC6_PRINT(globalState.connections.paths[pathIndex].y[yIndex]);
    DEBUG_NTCC6_PRINT(", trying to assign ");
    DEBUG_NTCC6_PRINTLN(yValue);
    return false; // Conflict detected
  }

  // Assign the Y coordinate
  globalState.connections.paths[pathIndex].y[yIndex] = yValue;
  return true; // Assignment successful
}

int pathIndex = 0;

// int powerDuplicates = 2;
// int dacDuplicates = 0;
// int pathDuplicates = 2;
// int powerPriority = 1;
// int dacPriority = 1;

// Y position limits to prevent high-priority nets from using all positions
int yPositionLimits[MAX_NETS] = {0}; // 0 = no limit
int yPositionUsage[MAX_NETS] = {0};  // Track current usage per net

// Or maybe a more useful way to default to: run a set number of connections
// (like 2-4) for power, then 2 for every regular jumper, then fill in the rest
// of with more power connections.

void initializeYPositionLimits(void) {
  // Clear usage counters
  for (int i = 0; i < MAX_NETS; i++) {
    yPositionUsage[i] = 0;
    yPositionLimits[i] = 0; // 0 = no limit
  }

  // Set limits for high-priority nets to prevent them from monopolizing Y
  // positions Reserve at least 2 Y positions per chip for inter-chip hops
  yPositionLimits[1] =
      3; // GND - reduced to leave more room for inter-chip hops
  yPositionLimits[2] = 2; // Top Rail - limit to 2 positions
  yPositionLimits[3] = 2; // Bottom Rail - limit to 2 positions
  yPositionLimits[4] = 2; // DAC0 - limit to 2 positions
  yPositionLimits[5] = 2; // DAC1 - limit to 2 positions

  DEBUG_NTCC2_PRINTLN(
      "Y position limits initialized (reserving space for inter-chip hops):");
  DEBUG_NTCC2_PRINTLN("  GND (net 1): max 3 Y positions");
  DEBUG_NTCC2_PRINTLN("  Power rails: max 2 Y positions each");
  DEBUG_NTCC2_PRINTLN("  DACs: max 2 Y positions each");
}

bool canNetUseMoreYPositions(int net) {
  // Bounds checking to prevent crashes
  if (net < 0 || net >= MAX_NETS) {
    return true; // Invalid net, allow to prevent blocking
  }

  if (yPositionLimits[net] == 0) {
    return true; // No limit set
  }
  return yPositionUsage[net] < yPositionLimits[net];
}

// Function prototypes for forward declarations
bool assignYPositionWithTracking(int chip, int yPos, int net);
void setChipYStatus(int chip, int y, int net, const char *location);
bool resolveYPositionConflicts(void);
void validateNoYPositionOverlaps(void);

// bool assignYPositionWithTracking(int chip, int yPos, int net) {
//   // Bounds checking to prevent crashes
//   if (chip < 0 || chip >= 12 || yPos < 0 || yPos >= 8 || net < 0 || net >=
//   MAX_NETS) {
//     return false; // Invalid parameters, assignment failed
//   }

//   // Check for overlap conflicts before assignment
//   if (globalState.connections.chipStates[chip].yStatus[yPos] != -1 && globalState.connections.chipStates[chip].yStatus[yPos] != net) {
//     if (debugNTCC6) {
//       Serial.print("WARNING: Y position overlap detected! Chip ");
//       Serial.print(chipNumToChar(chip));
//       Serial.print(" Y[");
//       Serial.print(yPos);
//       Serial.print("] already occupied by net ");
//       Serial.print(globalState.connections.chipStates[chip].yStatus[yPos]);
//       Serial.print(", trying to assign net ");
//       Serial.println(net);
//     }
//     // Don't overwrite - this could cause the overlap issue
//     return false; // Assignment failed due to conflict
//   }

//   // Only track new Y position usage if this Y position wasn't already used
//   by this net bool alreadyUsedByThisNet = false; if (globalState.connections.chipStates[chip].yStatus[yPos]
//   == net) {
//     alreadyUsedByThisNet = true;
//   }

//   // Assign the Y position
//   globalState.connections.chipStates[chip].yStatus[yPos] = net;

//   // Track usage if this is a new Y position for this net
//   if (!alreadyUsedByThisNet && yPositionLimits[net] > 0) {
//     // Count how many unique Y positions this net currently uses
//     int uniqueYPositions = 0;
//     for (int checkChip = 0; checkChip < 12; checkChip++) {
//       for (int checkY = 0; checkY < 8; checkY++) {
//         if (globalState.connections.chipStates[checkChip].yStatus[checkY] == net) {
//           // Check if we've already counted this Y position
//           bool alreadyCounted = false;
//           for (int prevChip = 0; prevChip < checkChip; prevChip++) {
//             if (globalState.connections.chipStates[prevChip].yStatus[checkY] == net) {
//               alreadyCounted = true;
//               break;
//             }
//           }
//           if (!alreadyCounted) {
//             uniqueYPositions++;
//           }
//         }
//       }
//     }
//     yPositionUsage[net] = uniqueYPositions;

//     if (debugNTCC2) {
//       Serial.print("  Assigned Y=");
//       Serial.print(yPos);
//       Serial.print(" on chip ");
//       Serial.print(chipNumToChar(chip));
//       Serial.print(" to net ");
//       Serial.print(net);
//       Serial.print(" (usage now: ");
//       Serial.print(yPositionUsage[net]);
//       Serial.print("/");
//       Serial.print(yPositionLimits[net]);
//       Serial.println(")");
//     }
//   }

//   return true; // Assignment successful
// }

void printYPositionUsageReport(void) {
  if (debugNTCC2) {
    Serial.println("\nY Position Usage Report:");
    Serial.println("Net\tName\t\tUsage/Limit");
    for (int net = 1; net < 6; net++) { // Check main power/special nets
      if (yPositionLimits[net] > 0) {
        Serial.print(net);
        Serial.print("\t");
        switch (net) {
        case 1:
          Serial.print("GND\t\t");
          break;
        case 2:
          Serial.print("Top Rail\t");
          break;
        case 3:
          Serial.print("Bottom Rail\t");
          break;
        case 4:
          Serial.print("DAC0\t\t");
          break;
        case 5:
          Serial.print("DAC1\t\t");
          break;
        default:
          Serial.print("Unknown\t\t");
          break;
        }
        Serial.print(yPositionUsage[net]);
        Serial.print("/");
        Serial.print(yPositionLimits[net]);
        if (yPositionUsage[net] >= yPositionLimits[net]) {
          Serial.print(" (LIMIT REACHED)");
        }
        Serial.println();
      }
    }
    Serial.println();
  }
}

bool resolveYPositionConflicts(void) {
  if (debugNTCC6) {
    Serial.println("\nResolving Y position conflicts (simple approach)...");
  }

  bool foundConflicts = false;

  // Simple approach: just look for paths that have overlapping Y positions
  for (int i = 0; i < numberOfPaths; i++) {
    if (globalState.connections.paths[i].skip)
      continue; // Skip already failed paths

    for (int j = i + 1; j < numberOfPaths; j++) {
      if (globalState.connections.paths[j].skip)
        continue; // Skip already failed paths
      if (globalState.connections.paths[i].net == globalState.connections.paths[j].net)
        continue; // Same net is OK

      // Check if these paths overlap on any chip
      for (int seg1 = 0; seg1 < 4; seg1++) {
        for (int seg2 = 0; seg2 < 4; seg2++) {
          if (globalState.connections.paths[i].chip[seg1] == globalState.connections.paths[j].chip[seg2] &&
              globalState.connections.paths[i].chip[seg1] != -1 && globalState.connections.paths[i].y[seg1] == globalState.connections.paths[j].y[seg2] &&
              globalState.connections.paths[i].y[seg1] > 0) {

            foundConflicts = true;
            if (debugNTCC6) {
              Serial.print("CONFLICT: Path ");
              Serial.print(i);
              Serial.print(" (net ");
              Serial.print(globalState.connections.paths[i].net);
              Serial.print(") and path ");
              Serial.print(j);
              Serial.print(" (net ");
              Serial.print(globalState.connections.paths[j].net);
              Serial.print(") both use chip ");
              Serial.print(chipNumToChar(globalState.connections.paths[i].chip[seg1]));
              Serial.print(" Y[");
              Serial.print(globalState.connections.paths[i].y[seg1]);
              Serial.println("]");
            }

            // Simple resolution: mark the higher-numbered path as unconnectable
            globalState.connections.paths[j].skip = true;
            if (debugNTCC6) {
              Serial.print("  Simple resolution: marked globalState.connections.paths[");
              Serial.print(j);
              Serial.println("] as unconnectable");
            }
            break;
          }
        }
        if (globalState.connections.paths[j].skip)
          break; // Don't check more if already marked
      }
    }
  }

  return foundConflicts;
}

void validateNoYPositionOverlaps(void) {
  if (debugNTCC6) {
    Serial.println("\nQuick Y position overlap check...");
    bool foundOverlaps = false;

    // Simple check: compare path pairs for overlaps
    for (int i = 0; i < numberOfPaths; i++) {
      if (globalState.connections.paths[i].skip)
        continue;

      for (int j = i + 1; j < numberOfPaths; j++) {
        if (globalState.connections.paths[j].skip)
          continue;
        if (globalState.connections.paths[i].net == globalState.connections.paths[j].net)
          continue; // Same net is OK

        // Quick check for any Y overlap
        for (int seg1 = 0; seg1 < 4; seg1++) {
          for (int seg2 = 0; seg2 < 4; seg2++) {
            if (globalState.connections.paths[i].chip[seg1] == globalState.connections.paths[j].chip[seg2] &&
                globalState.connections.paths[i].chip[seg1] != -1 &&
                globalState.connections.paths[i].y[seg1] == globalState.connections.paths[j].y[seg2] && globalState.connections.paths[i].y[seg1] > 0) {

              foundOverlaps = true;
              Serial.print("OVERLAP: Path ");
              Serial.print(i);
              Serial.print(" and ");
              Serial.print(j);
              Serial.print(" both use chip ");
              Serial.print(chipNumToChar(globalState.connections.paths[i].chip[seg1]));
              Serial.print(" Y[");
              Serial.print(globalState.connections.paths[i].y[seg1]);
              Serial.print("] nets ");
              Serial.print(globalState.connections.paths[i].net);
              Serial.print(" and ");
              Serial.println(globalState.connections.paths[j].net);
            }
          }
        }
      }
    }

    if (!foundOverlaps) {
      Serial.println("No Y position overlaps detected");
    }
  }
}

bool isGpioConnection(int pathIndex) {
  for (int i = 0; i < 10; i++) {
    if (globalState.connections.paths[pathIndex].node1 == gpioDef[i][0] ||
        globalState.connections.paths[pathIndex].node2 == gpioDef[i][1]) {
      return true;
    }
  }
  return false;
}

int getNetPriority(int netNumber) {
  // Bounds checking to prevent crashes
  if (netNumber < 0 || netNumber > MAX_NETS) {
    return 0;
  }

  return globalState.connections.nets[netNumber].priority;
}

void frontloadPriorityConnections(void) {
  DEBUG_NTCC2_PRINTLN("Frontloading connections by priority...");

  // Safety checks to prevent crashes
  if (numberOfPaths <= 0 || numberOfPaths > MAX_BRIDGES) {
    DEBUG_NTCC2_PRINT("Invalid numberOfPaths: ");
    DEBUG_NTCC2_PRINTLN(numberOfPaths);
    return;
  }

  // Use a simple in-place sorting approach to avoid large stack allocations
  // First, move all duplicates to the end
  int writeIndex = 0;

  // Pass 1: Copy non-duplicates to front, preserving order
  for (int i = 0; i < numberOfPaths; i++) {
    if (globalState.connections.paths[i].duplicate == 0) {
      if (writeIndex != i) {
        globalState.connections.paths[writeIndex] = globalState.connections.paths[i];
      }
      writeIndex++;
    }
  }

  // Store where duplicates should start
  int duplicateStartIndex = writeIndex;

  // Pass 2: Copy duplicates to end
  for (int i = 0; i < numberOfPaths; i++) {
    if (globalState.connections.paths[i].duplicate == 1) {
      globalState.connections.paths[writeIndex] = globalState.connections.paths[i];
      writeIndex++;
    }
  }

  // Now sort the non-duplicate section by priority
  // Simple insertion sort for priority paths (nets > 3 with priority > 1)
  for (int i = 1; i < duplicateStartIndex; i++) {
    pathStruct current = globalState.connections.paths[i];
    int currentPriority = (current.net > 3) ? getNetPriority(current.net) : 1;
    bool currentIsGpio =
        ((current.node1 >= NANO_D0 && current.node1 <= NANO_A7) ||
         (current.node2 >= NANO_D0 && current.node2 <= NANO_A7));

    int j = i - 1;

    // Move elements that should come after current path
    while (j >= 0) {
      int comparePriority = (globalState.connections.paths[j].net > 3) ? getNetPriority(globalState.connections.paths[j].net) : 1;
      bool compareIsGpio =
          ((globalState.connections.paths[j].node1 >= NANO_D0 && globalState.connections.paths[j].node1 <= NANO_A7) ||
           (globalState.connections.paths[j].node2 >= NANO_D0 && globalState.connections.paths[j].node2 <= NANO_A7));

      bool shouldMoveCurrentForward = false;

      // Higher priority should come first
      if (currentPriority > comparePriority) {
        shouldMoveCurrentForward = true;
      } else if (currentPriority == comparePriority) {
        // Same priority - GPIO connections get preference
        if (currentIsGpio && !compareIsGpio) {
          shouldMoveCurrentForward = true;
        }
      }

      if (!shouldMoveCurrentForward) {
        break;
      }

      globalState.connections.paths[j + 1] = globalState.connections.paths[j];
      j--;
    }

    globalState.connections.paths[j + 1] = current;
  }

  DEBUG_NTCC2_PRINT("Priority frontloading complete using in-place sort\n");

  // Count and show priority paths
#if DEBUG_NTCC2_ENABLED
  if (debugNTCC2) {
    int priorityCount = 0;
    for (int i = 0; i < duplicateStartIndex; i++) {
      if (globalState.connections.paths[i].net > 3 && getNetPriority(globalState.connections.paths[i].net) > 1) {
        priorityCount++;
      }
    }

    DEBUG_NTCC2_PRINT("Found ");
    DEBUG_NTCC2_PRINT(priorityCount);
    DEBUG_NTCC2_PRINT(" priority paths out of ");
    DEBUG_NTCC2_PRINT(duplicateStartIndex);
    DEBUG_NTCC2_PRINTLN(" non-duplicate paths");

    // Show the first few priority connections for verification
    if (priorityCount > 0) {
      DEBUG_NTCC2_PRINTLN("Top priority connections:");
      int shown = 0;
      for (int i = 0; i < duplicateStartIndex && shown < 5; i++) {
        if (globalState.connections.paths[i].net > 3 && getNetPriority(globalState.connections.paths[i].net) > 1) {
          DEBUG_NTCC2_PRINT("  [");
          DEBUG_NTCC2_PRINT(i);
          DEBUG_NTCC2_PRINT("] net ");
          DEBUG_NTCC2_PRINT(globalState.connections.paths[i].net);
          DEBUG_NTCC2_PRINT(" priority ");
          DEBUG_NTCC2_PRINT(getNetPriority(globalState.connections.paths[i].net));
          DEBUG_NTCC2_PRINT(": ");
          printNodeOrName(globalState.connections.paths[i].node1);
          DEBUG_NTCC2_PRINT("-");
          printNodeOrName(globalState.connections.paths[i].node2);
          if ((globalState.connections.paths[i].node1 >= NANO_D0 && globalState.connections.paths[i].node1 <= NANO_A7) ||
              (globalState.connections.paths[i].node2 >= NANO_D0 && globalState.connections.paths[i].node2 <= NANO_A7)) {
            DEBUG_NTCC2_PRINT(" [GPIO]");
          }
          DEBUG_NTCC2_PRINTLN();
          shown++;
        }
      }
    }
  }
#endif
}

void clearAllNTCC(void) {

  // digitalWrite(RESETPIN,HIGH);
  routingGeneration++;

  for (int i = 0; i < 12; i++) {
    chipsLeastToMostCrowded[i] = i;
  }
  for (int i = 0; i < 4; i++) {
    chipCandidates[0][i] = -1;
    chipCandidates[1][i] = -1;

    sfChipsLeastToMostCrowded[i] = i + 8;
  }
  
  // OPTIMIZATION: Only clear what we'll actually use
  // Estimate based on number of bridges (each bridge becomes ~1-2 paths typically)
  // Use actual bridge count instead of stale numberOfPaths
  int estimatedPaths = globalState.connections.numBridges * 2 + 12;  // 2x bridges + safety margin
  int pathsToClear = (estimatedPaths < MAX_BRIDGES) ? estimatedPaths : MAX_BRIDGES;

  pathsToClear = MAX_BRIDGES; // yeah fuck that
  
  // Fast bulk clear with memset
  memset(pathsWithCandidates, 0, pathsToClear * sizeof(int));
  memset(globalState.connections.paths, -1, pathsToClear * sizeof(pathStruct));
  for (int i = 0; i < pathsToClear; i++) {
    globalState.connections.paths[i].altPathNeeded = 0;
    globalState.connections.paths[i].skip = 0;

  }
  // //clang-format off
  // struct netStruct globalState.connections.nets[MAX_NETS] = { //these are the special function nets
  // that will always be made
  // //netNumber,       ,netName          ,memberNodes[] ,memberBridges[][2]
  // ,specialFunction        ,intsctNet[] ,doNotIntersectNodes[] ,priority
  // (unused)
  //     {     127      ,"Empty Net"      ,{EMPTY_NET}           ,{{}}
  //     ,EMPTY_NET              ,{}
  //     ,{EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET}
  //     , 0}, {     1        ,"GND"            ,{GND}                 ,{{}}
  //     ,GND                    ,{}          ,{SUPPLY_3V3,SUPPLY_5V,DAC0,DAC1}
  //     , 1}, {     2        ,"Top Rail"       ,{TOP_RAIL}            ,{{}}
  //     ,TOP_RAIL               ,{}          ,{GND} , 1}, {     3 ,"Bottom
  //     Rail"    ,{BOTTOM_RAIL}         ,{{}}                   ,BOTTOM_RAIL
  //     ,{}          ,{GND}                               , 1}, {     4 ,"DAC
  //     0"          ,{DAC0}                ,{{}}                   ,DAC0 ,{}
  //     ,{GND}                               , 1}, {     5        ,"DAC 1"
  //     ,{DAC1}                ,{{}}                   ,DAC1 ,{} ,{GND} , 1},
  //     {     6        ,"I Sense +"      ,{ISENSE_PLUS}         ,{{}}
  //     ,ISENSE_PLUS            ,{}          ,{ISENSE_MINUS} , 2}, {     7 ,"I
  //     Sense -"      ,{ISENSE_MINUS}        ,{{}} ,ISENSE_MINUS           ,{}
  //     ,{ISENSE_PLUS}                       , 2},
  // };
  globalState.connections.nets[0] = {     127      ,"Empty Net"      ,{EMPTY_NET}           ,{{}}                   ,EMPTY_NET                        ,{EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET} , 0};
  globalState.connections.nets[1] = {     1        ,"GND"            ,{GND}                 ,{{}}                   ,GND                              ,{BOTTOM_RAIL,TOP_RAIL,DAC1}    , 1};
       
  globalState.connections.nets[2] = {     2        ,"Top Rail"       ,{TOP_RAIL}            ,{{}}                   ,TOP_RAIL                         ,{GND, BOTTOM_RAIL, DAC0, DAC1}                               , 1};
  globalState.connections.nets[3] = {     3        ,"Bottom Rail"    ,{BOTTOM_RAIL}         ,{{}}                   ,BOTTOM_RAIL                      ,{GND, TOP_RAIL, DAC0, DAC1}                               , 1};
  globalState.connections.nets[4] = {     4        ,"DAC 0"          ,{DAC0}                ,{{}}                   ,DAC0                             ,{ TOP_RAIL, BOTTOM_RAIL, DAC1}                               , 1};
  globalState.connections.nets[5] =  {     5        ,"DAC 1"          ,{DAC1}                ,{{}}                   ,DAC1                            ,{GND, TOP_RAIL, BOTTOM_RAIL, DAC0}                               , 1};
  
  
  //clang-format on

  initNets();
  initializeYPositionLimits();

  for (int i = 0; i < 12; i++) {
    globalState.connections.chipStates[i].uncommittedHops = 0;
    for (int j = 0; j < 16; j++) {
      globalState.connections.chipStates[i].xStatus[j] = -1;
    }

    for (int j = 0; j < 8; j++) {
      globalState.connections.chipStates[i].yStatus[j] = -1;
    }
  }
  for (int i = 0; i < 10; i++) {
    unconnectablePaths[i][0] = -1;
    unconnectablePaths[i][1] = -1;
  }
  numberOfUnconnectablePaths = 0;
  // printPathsCompact();
  // printChipStatus();

  for (int i = 0; i < 8; i++) {
    if (gpioNet[i] != -2) {
      gpioNet[i] = -1;
    }
    showADCreadings[i] = -1;
    gpioReading[i] = 3;
    gpioReadingColors[i] = 0x010101;
  }
  if (gpioNet[8] != -2) {
    gpioNet[8] = -1;
  }
  if (gpioNet[9] != -2) {
    gpioNet[9] = -1;
  }
  gpioReading[8] = 3;
  gpioReading[9] = 3;
  gpioReadingColors[8] = 0x010101;
  gpioReadingColors[9] = 0x010101;

  startEndChip[0] = -1;
  startEndChip[1] = -1;
  bothNodes[0] = -1;

  bothNodes[1] = -1;

  numberOfUniqueNets = 0;
  numberOfNets = 0;
  numberOfPaths = 0;

  pathsWithCandidatesIndex = 0;
  pathIndex = 0;
  // findChangedNetColors();
  //  for (int i = 0; i<MAX_NETS; i++) {
  //   // changedNetColors[i] = 0;
  //  }
  // digitalWrite(RESETPIN,LOW);
}

void sortPathsByNet(
    void) // not actually sorting, just copying the bridges and nets back from
// netStruct so they're both in the same order
{
  if (debugNTCC) {
    Serial.println("sortPathsByNet()");
  }
  timeToSort = micros();
  numberOfPaths = 0;
  pathIndex = 0;

  if (debugNTCC) {
    printBridgeArray();
  }

  // OPTIMIZATION: Count nets efficiently
  numberOfNets = 1;
  for (int i = 1; i < MAX_NETS - 1; i++) {
    if (globalState.connections.nets[i].number == 0 || globalState.connections.nets[i].number == -1) {
      break;
    } else {
      numberOfNets++;
    }
  }

  // OPTIMIZATION: This loop is redundant - we'll count paths as we build them
  // Skipping it saves ~50-100us
  // for (int i = 0; i < MAX_BRIDGES; i++) {
  //   if ((globalState.connections.paths[i].node1 != 0 && globalState.connections.paths[i].node2 != 0) &&
  //       (globalState.connections.paths[i].node1 != -1 && globalState.connections.paths[i].node2 != -1)) {
  //     numberOfPaths++;
  //   } else if (globalState.connections.paths[i].node1 == 0 || globalState.connections.paths[i].node2 == 0) {
  //     break;
  //   }
  // }

  int routableBufferPowerFound = -1;

  int lastPowerPath = -1;
  numberOfUniqueNets = 0;
  numberOfShownNets = 0;

  // CRITICAL OPTIMIZATION: Use numberOfNets instead of MAX_NETS!
  // This avoids checking empty nets (saves 50-100us when we have few nets)
  for (int j = 1; j < numberOfNets; j++) {
    if (globalState.connections.nets[j].number == 0) {
      break;
    }

    for (int k = 0; k < MAX_NODES; k++) {
      if (globalState.connections.nets[j].bridges[k][0] == 0) {
        break;
        // continue;
      } else {
        // paths[] is [MAX_BRIDGES], but the per-net entries can total more than
        // that: combineNets lists one merged bridge under both surviving nets.
        if (pathIndex >= MAX_BRIDGES) {
          break;
        }
        int node1 = globalState.connections.nets[j].bridges[k][0];
        int node2 = globalState.connections.nets[j].bridges[k][1];
        
        globalState.connections.paths[pathIndex].net = globalState.connections.nets[j].number;
        globalState.connections.paths[pathIndex].node1 = node1;
        globalState.connections.paths[pathIndex].node2 = node2;
        globalState.connections.paths[pathIndex].duplicate = 0;

        if (globalState.connections.paths[pathIndex].net <= 5) {
          lastPowerPath = pathIndex;
        }

        // The probe power feed gets routed FIRST (shifted to path 0 below)
        // so its crosspoints land on predictable lanes. Match the ACTIVE
        // infra pair, whatever its source: the old DAC0/DAC1 check missed
        // the GPIO-powered variant entirely (its net is not <= 5, which is
        // also why the old net<=5 gate can't wrap this).
        {
          int src = infraProbePowerSource();
          if (src >= 0 &&
              ((node1 == ROUTABLE_BUFFER_IN && node2 == src) ||
               (node1 == src && node2 == ROUTABLE_BUFFER_IN))) {
            routableBufferPowerFound = pathIndex;
          }
        }

        // Track unique nets
        if (pathIndex == 0 || globalState.connections.paths[pathIndex].net != globalState.connections.paths[pathIndex - 1].net) {
          numberOfUniqueNets++;
          
          // OPTIMIZATION: Simplified visibility check using cached node values
          if (globalState.connections.paths[pathIndex].net >= 6) {
            bool node1Visible = (node1 <= 60) || 
                                (node1 >= NANO_D0 && node1 <= NANO_RESET_1) ||
                                (node1 >= FAKE_GPIO_1 && node1 <= FAKE_GPIO_32);
            bool node2Visible = (node2 <= 60) || 
                                (node2 >= NANO_D0 && node2 <= NANO_RESET_1) ||
                                (node2 >= FAKE_GPIO_1 && node2 <= FAKE_GPIO_32);
            
            if (node1Visible || node2Visible) {
              globalState.connections.nets[j].visible = 1;
              numberOfShownNets++;
              
              // Mark as virtual if it contains FAKE_GPIO nodes
              if ((node1 >= FAKE_GPIO_1 && node1 <= FAKE_GPIO_32) ||
                  (node2 >= FAKE_GPIO_1 && node2 <= FAKE_GPIO_32)) {
                globalState.connections.nets[j].virtual_net = true;
              }
            } else {
              globalState.connections.nets[j].visible = 0;
            }
          }
        }

        // if (debugNTCC) {
        // Serial.print("globalState.connections.paths[");
        // Serial.print(pathIndex);
        // Serial.print("] net: ");
        // Serial.println(globalState.connections.paths[pathIndex].net);
        //}
        pathIndex++;
      }
    }
  }

  // OPTIMIZATION: This shift operation is expensive (copies entire array)
  // Only do it if routableBufferPower is actually found (rare in fast operations)
  if (routableBufferPowerFound > 0) {
    pathStruct tempPath = globalState.connections.paths[routableBufferPowerFound];

    // OPTIMIZATION: Use memmove instead of loop (much faster for large shifts)
    // memmove handles overlapping memory correctly
    memmove(&globalState.connections.paths[1], 
            &globalState.connections.paths[0], 
            routableBufferPowerFound * sizeof(pathStruct));
    
    globalState.connections.paths[0] = tempPath;
  }
  // Serial.print("Routable Buffer Power Found: ");
  // Serial.println(routableBufferPowerFound);

  // Serial.print("Last Power Path: ");
  // Serial.println(lastPowerPath);

  newBridgeLength = numberOfPaths;
  numberOfPaths = pathIndex;
  globalState.connections.numPaths = numberOfPaths;  // Synchronize state struct

  // for (int i = 0; i < numberOfNets; i++) {

  //   }

  if (debugNTCC) {
    Serial.print("number unique of nets: ");
    Serial.println(numberOfUniqueNets);
    Serial.print("pathIndex: ");
    Serial.println(pathIndex);
    Serial.print("numberOfPaths: ");
    Serial.println(numberOfPaths);
  }
  // numberOfShownNets = numberOfUniqueNets;
  //  printPathArray();
  clearChipsOnPathToNegOne(); // clear chips and all trailing paths to -1{if
  // there are bridges that weren't made due to DNI
  // rules, there will be fewer paths now because
  // they were skipped}

  if (debugNTCC) {
    Serial.println("cleared trailing paths");
    // delay(10);
    printBridgeArray();
    // delay(10);
    Serial.println("\n\r");
    timeToSort = micros() - timeToSort;
    Serial.print("time to sort: ");
    Serial.print(timeToSort);
    Serial.println("us\n\r");
  }
}

// ============================================================================
// Hub rescue (2026-09-03): path shapes the tiers above never try
// ============================================================================
// After commitPaths / resolveAltPaths / resolveUncommittedHops (and the K-row
// rescue pass) a primary can still be unrouted while copper is left: every
// breadboard bounce row is spent but a special-function chip still has free
// rows, or the single lane from a breadboard chip into the SF chip its node
// lives on is taken. Two shapes reach that copper within the four crosspoints
// a pathStruct holds:
//
//   H1  BB -> SF through another SF chip (the "hub"):
//         c0.X[lane c0->hub] on the row y0      -> hub.Y[row wired to c0]
//         hub.X[hub lane hub<->sf]               -> sf.X[that lane] on a row ys
//         sf.X[node] on ys
//       chips: c0, sf, hub, sf. The breadboard chip at the far end of row
//       ys's wire has its lane into sf claimed too - that wire is on the net.
//
//   H2  BB -> BB through an SF chip:
//         c0.X[lane c0->sf] on y0  -> sf.Y[row wired to c0]
//         c1.X[lane c1->sf] on y1  -> sf.Y[row wired to c1]
//         sf.X[xb] closed on both rows, xb a hub lane whose far end (the
//         other SF chip's pin) is free and gets claimed.
//       chips: c0, c1, sf, sf. Only hub lanes may bridge: every other X pin
//       of an SF chip is a real node (a nano pin, a rail, an ADC ...).
//
// Runs only for primaries that are still not fully routed, after the whole
// existing pipeline, so a path that routed before routes exactly as before
// (test_routing_dense's snapshot diff is the gate). Every wire is claimed at
// both of its pins; a failed attempt rolls back through the routing
// transaction. Rows and lanes the net already holds are reused
// (allowStacking = 1): a row the net owns costs nothing.
//
// Topology queries only - nothing here knows a chip letter. TODO V6: the
// breadboard/SF split (chip < 8), the hub-lane X range (12..14) and the
// "SF row <-> breadboard chip" wiring should come from the board descriptor.

static const int HUB_LANE_X_FIRST = 12;
static const int HUB_LANE_X_LAST = 14;

static inline bool hubIsBreadboardChip(int c) { return c >= 0 && c < 8; }
static inline bool hubIsSfChip(int c) { return c >= 8 && c < 12; }

// The SF chip's row wired to breadboard chip bb (-1 if none).
static int hubSfRowFor(int sf, int bb) {
  for (int y = 0; y < 8; y++) {
    if (globalState.connections.chipStates[sf].yMap[y] == bb) {
      return y;
    }
  }
  return -1;
}

// A lane is free for net at BOTH its pins.
static bool hubLaneFree(int a, int xa, int b, int xb, int net) {
  return freeOrSameNetX(a, xa, net, 1) && freeOrSameNetX(b, xb, net, 1);
}

// An SF row is usable: the row itself and the breadboard pin at the far end
// of its wire.
static bool hubSfRowFree(int sf, int y, int net) {
  if (!freeOrSameNetY(sf, y, net, 1)) {
    return false;
  }
  int bb = globalState.connections.chipStates[sf].yMap[y];
  if (!hubIsBreadboardChip(bb)) {
    return false;
  }
  int xbb = xMapForChipLane0(bb, sf);
  return xbb >= 0 && freeOrSameNetX(bb, xbb, net, 1);
}

static bool hubClaimSfRow(int sf, int y, int net, const char *where) {
  int bb = globalState.connections.chipStates[sf].yMap[y];
  int xbb = xMapForChipLane0(bb, sf);
  return setChipYStatusSafe(sf, y, net, where) && setChipXStatus(bb, xbb, net, where);
}

// Wipe the coordinates a failed tier may have left (setPathX/Y refuse to
// overwrite a real coordinate with a different one). Under a transaction.
static void hubResetPath(int i) {
  struct pathStruct &p = globalState.connections.paths[i];
  p.chip[2] = -1;
  p.chip[3] = -1;
  for (int k = 0; k < 4; k++) {
    p.x[k] = -1;
    p.y[k] = -1;
  }
}

static bool hubRouteBBtoSF(int i) {
  struct pathStruct &p = globalState.connections.paths[i];
  int c0 = p.chip[0], sf = p.chip[1], net = p.net;
  if (!hubIsBreadboardChip(c0) || !hubIsSfChip(sf)) {
    return false;
  }
  int y0 = yMapForNode(p.node1, c0);
  int xNode = xMapForNode(p.node2, sf);
  if (y0 < 0 || xNode < 0) {
    return false;
  }
  if (!freeOrSameNetY(c0, y0, net, 1) || !freeOrSameNetX(sf, xNode, net, 1)) {
    return false;
  }
  for (int hub = 8; hub < 12; hub++) {
    if (hub == sf) {
      continue;
    }
    int xc0 = xMapForChipLane0(c0, hub); // c0's lane into the hub
    int yh = hubSfRowFor(hub, c0);       // the hub row on that lane
    int xh = xMapForChipLane0(hub, sf);  // hub lane, hub side
    int xs = xMapForChipLane0(sf, hub);  // hub lane, sf side
    if (xc0 < 0 || yh < 0 || xh < 0 || xs < 0) {
      continue;
    }
    if (!freeOrSameNetX(c0, xc0, net, 1) || !freeOrSameNetY(hub, yh, net, 1)) {
      continue;
    }
    if (!hubLaneFree(hub, xh, sf, xs, net)) {
      continue;
    }
    // a row on sf for the hop: one the net already holds first
    int ys = -1;
    for (int pass = 0; pass < 2 && ys < 0; pass++) {
      for (int y = 0; y < 8; y++) {
        bool held = globalState.connections.chipStates[sf].yStatus[y] == net;
        if ((pass == 0) != held) {
          continue;
        }
        if (hubSfRowFree(sf, y, net)) {
          ys = y;
          break;
        }
      }
    }
    if (ys < 0) {
      continue;
    }
    saveRoutingState(i);
    hubResetPath(i);
    p.chip[2] = hub;
    p.chip[3] = sf;
    bool ok = setPathX(i, 0, xc0) && setPathY(i, 0, y0) &&
              setPathX(i, 1, xNode) && setPathY(i, 1, ys) &&
              setPathX(i, 2, xh) && setPathY(i, 2, yh) &&
              setPathX(i, 3, xs) && setPathY(i, 3, ys) &&
              setChipXStatus(c0, xc0, net, "hub H1 c0 lane") &&
              setChipYStatusSafe(c0, y0, net, "hub H1 c0 row") &&
              setChipYStatusSafe(hub, yh, net, "hub H1 hub row") &&
              setChipXStatus(hub, xh, net, "hub H1 hub lane") &&
              setChipXStatus(sf, xs, net, "hub H1 sf lane") &&
              setChipXStatus(sf, xNode, net, "hub H1 sf node") &&
              hubClaimSfRow(sf, ys, net, "hub H1 sf row");
    if (!ok) {
      restoreRoutingState(i);
      continue;
    }
    commitRoutingState();
    p.altPathNeeded = false;
    p.sameChip = true;
    if (debugNTCC2) {
      Serial.print("hub H1: path ");
      Serial.print(i);
      Serial.print(" via ");
      Serial.print(chipNumToChar(hub));
      Serial.print(" onto ");
      Serial.print(chipNumToChar(sf));
      Serial.print(" row ");
      Serial.println(ys);
    }
    return true;
  }
  return false;
}

static bool hubRouteBBtoBB(int i) {
  struct pathStruct &p = globalState.connections.paths[i];
  int c0 = p.chip[0], c1 = p.chip[1], net = p.net;
  if (!hubIsBreadboardChip(c0) || !hubIsBreadboardChip(c1) || c0 == c1) {
    return false;
  }
  int y0 = yMapForNode(p.node1, c0);
  int y1 = yMapForNode(p.node2, c1);
  if (y0 < 0 || y1 < 0) {
    return false;
  }
  if (!freeOrSameNetY(c0, y0, net, 1) || !freeOrSameNetY(c1, y1, net, 1)) {
    return false;
  }
  for (int sf = 8; sf < 12; sf++) {
    int x0 = xMapForChipLane0(c0, sf);
    int x1 = xMapForChipLane0(c1, sf);
    int r0 = hubSfRowFor(sf, c0);
    int r1 = hubSfRowFor(sf, c1);
    if (x0 < 0 || x1 < 0 || r0 < 0 || r1 < 0) {
      continue;
    }
    if (!freeOrSameNetX(c0, x0, net, 1) || !freeOrSameNetX(c1, x1, net, 1)) {
      continue;
    }
    if (!freeOrSameNetY(sf, r0, net, 1) || !freeOrSameNetY(sf, r1, net, 1)) {
      continue;
    }
    // a hub lane on sf to bridge the two rows, its far end free
    int xb = -1, other = -1, xo = -1;
    for (int x = HUB_LANE_X_FIRST; x <= HUB_LANE_X_LAST && xb < 0; x++) {
      int o = globalState.connections.chipStates[sf].xMap[x];
      if (!hubIsSfChip(o)) {
        continue;
      }
      int xoo = xMapForChipLane0(o, sf);
      if (xoo >= 0 && hubLaneFree(sf, x, o, xoo, net)) {
        xb = x;
        other = o;
        xo = xoo;
      }
    }
    if (xb < 0) {
      continue;
    }
    saveRoutingState(i);
    hubResetPath(i);
    p.chip[2] = sf;
    p.chip[3] = sf;
    bool ok = setPathX(i, 0, x0) && setPathY(i, 0, y0) &&
              setPathX(i, 1, x1) && setPathY(i, 1, y1) &&
              setPathX(i, 2, xb) && setPathY(i, 2, r0) &&
              setPathX(i, 3, xb) && setPathY(i, 3, r1) &&
              setChipXStatus(c0, x0, net, "hub H2 c0 lane") &&
              setChipYStatusSafe(c0, y0, net, "hub H2 c0 row") &&
              setChipXStatus(c1, x1, net, "hub H2 c1 lane") &&
              setChipYStatusSafe(c1, y1, net, "hub H2 c1 row") &&
              setChipYStatusSafe(sf, r0, net, "hub H2 sf row0") &&
              setChipYStatusSafe(sf, r1, net, "hub H2 sf row1") &&
              setChipXStatus(sf, xb, net, "hub H2 bridge lane") &&
              setChipXStatus(other, xo, net, "hub H2 bridge far end");
    if (!ok) {
      restoreRoutingState(i);
      continue;
    }
    commitRoutingState();
    p.altPathNeeded = false;
    p.sameChip = true;
    if (debugNTCC2) {
      Serial.print("hub H2: path ");
      Serial.print(i);
      Serial.print(" via ");
      Serial.print(chipNumToChar(sf));
      Serial.print(" x");
      Serial.println(xb);
    }
    return true;
  }
  return false;
}

static void rescueViaHubs(int startIndex) {
  for (int i = startIndex; i < numberOfPaths; i++) {
    struct pathStruct &p = globalState.connections.paths[i];
    if (p.pathType == VIRTUAL || p.duplicate != 0 || p.skip) {
      continue;
    }
    if (p.net <= 0 || p.chip[0] < 0 || p.chip[1] < 0) {
      continue;
    }
    if (pathFullyRouted(i)) {
      continue;
    }
    switch (p.pathType) {
    case BBtoSF:
    case BBtoNANO:
      hubRouteBBtoSF(i);
      break;
    case BBtoBB:
      hubRouteBBtoBB(i);
      break;
    default:
      break;
    }
  }
}

void bridgesToPaths(
    int fillUnused,
    int allowStacking,
    int startIndex) { ///!this is the main function that gets called
  if (debugNTCC5) {
    Serial.print("bridgesToPaths(startIndex=");
    Serial.print(startIndex);
    Serial.println(")");
  }

  // Performance profiling (matches PROFILE_FAST_REFRESH in Commands.cpp)
  #define PROFILE_BRIDGES_TO_PATHS 0
  unsigned long btp_start = micros();
  unsigned long btp_step = btp_start;

  // Only clear pathsWithCandidates if starting from 0
  if (startIndex == 0) {
    for (int i = 0; i < MAX_BRIDGES; i++) {
      pathsWithCandidates[i] = 0;
    }
  }
  
  int duplicateStartIndex = 0;
  // allowStacking = 0;
  
  // Only sort if starting from beginning (sorting invalidates incremental approach)
  if (startIndex == 0) {
    sortPathsByNet();
    #if PROFILE_BRIDGES_TO_PATHS
    Serial.print("  sortPathsByNet: "); Serial.print(micros() - btp_step); Serial.println(" us");
    btp_step = micros();
    #endif
    
    // TDM OPTIMIZATION: Merge all fake GPIO input paths into a single net
    // Since they're time-domain multiplexed (only one connected at a time),
    // they can share paths. After routing, we restore their original nets.
    mergeFakeGpioInputNets();
  }

  // Frontload connections by priority routing
 //frontloadPriorityConnections();

  DEBUG_NTCC2_PRINT("After priority frontloading - total paths: ");
  DEBUG_NTCC2_PRINTLN(numberOfPaths);
  //   Serial.print("number of paths: ");
  // Serial.println(numberOfPaths);
  // Serial.print("number of shown paths: ");
  // Serial.println(numberOfShownNets);
  // printPathsCompact(2);

  // Serial.print("number of paths: ");
  // Serial.println(numberOfPaths);
  // Serial.print("number of shown paths: ");
  // Serial.println(numberOfShownNets);
  // sortPathsByNet();

  //   if (fillUnused == 1) {
  // fillUnusedPaths(jumperlessConfig.routing.stack_paths,
  // jumperlessConfig.routing.stack_rails, jumperlessConfig.routing.stack_dacs);
  // }

  // printPathsCompact(2);
  // printChipStatus();
  duplicateStartIndex = numberOfPaths;

  for (int i = startIndex; i < numberOfPaths; i++) {
    if (globalState.connections.paths[i].duplicate == 1) {
      continue;
    }
    
    // NOTE: FakeGPIO virtual nodes (FAKE_GP_OUT_x, FAKE_GP_IN_x) are expanded
    // to real voltage sources/ADCs in findStartAndEndChips() below.
    // The path's node1/node2 will be updated to the expanded values there.

    if (debugNTCC5) {
      delay(10);
      Serial.print("globalState.connections.paths[");
      Serial.print(i);
      Serial.print("]\n\rnodes [");
      Serial.print(globalState.connections.paths[i].node1);
      Serial.print("-");
      Serial.print(globalState.connections.paths[i].node2);
      Serial.println("]\n\r");
    }

    findStartAndEndChips(globalState.connections.paths[i].node1, globalState.connections.paths[i].node2, i);

    if (debugNTCC5) {
      delay(10);
      Serial.print("startEndChip[0]: ");
      Serial.print(startEndChip[0]);
      Serial.print("  startEndChip[1]: ");
      Serial.println(startEndChip[1]);
    }

    mergeOverlappingCandidates(i);
    if (debugNTCC5) {
      delay(10);
      Serial.println("mergeOverlappingCandidates done");
    }

    assignPathType(i);

    if (debugNTCC5) {
      delay(10);
      Serial.println("assignPathType done");
      Serial.println("\n\n\r");
    }
  }

  if (debugNTCC5) {
    Serial.println("paths with candidates:");
  }

  if (debugNTCC5) {
    delay(10);
    for (int i = 0; i < pathsWithCandidatesIndex; i++) {
      Serial.print(pathsWithCandidates[i]);
      Serial.print(",");
    }
    Serial.println("\n\r");
    // printPathArray();
  }

  #if PROFILE_BRIDGES_TO_PATHS
  Serial.print("  path analysis loop: "); Serial.print(micros() - btp_step); Serial.println(" us");
  btp_step = micros();
  #endif

  // Only resort if starting from beginning (sorting is global and breaks incremental)
  if (startIndex == 0) {
    sortAllChipsLeastToMostCrowded();
    #if PROFILE_BRIDGES_TO_PATHS
    Serial.print("  sortAllChipsLeastToMostCrowded: "); Serial.print(micros() - btp_step); Serial.println(" us");
    btp_step = micros();
    #endif
  }

  resolveChipCandidates(startIndex);
  #if PROFILE_BRIDGES_TO_PATHS
  Serial.print("  resolveChipCandidates: "); Serial.print(micros() - btp_step); Serial.println(" us");
  btp_step = micros();
  #endif

  // Primary pass under the chip-K y-row budget (see kRowBudgetRefuses)
  computeKNeedingNets();
  kRowBudgetActive = true;

  commitPaths(2, -1, 0, startIndex);
  #if PROFILE_BRIDGES_TO_PATHS
  Serial.print("  commitPaths: "); Serial.print(micros() - btp_step); Serial.println(" us");
  btp_step = micros();
  #endif

  resolveAltPaths(2, -1, 0, startIndex);
  #if PROFILE_BRIDGES_TO_PATHS
  Serial.print("  resolveAltPaths: "); Serial.print(micros() - btp_step); Serial.println(" us");
  btp_step = micros();
  #endif
  
  resolveUncommittedHops(2, -1, 0, startIndex);
  #if PROFILE_BRIDGES_TO_PATHS
  Serial.print("  resolveUncommittedHops: "); Serial.print(micros() - btp_step); Serial.println(" us");
  btp_step = micros();
  #endif

  kRowBudgetActive = false;

  // Rescue pass: whatever the budget left unrouted gets one more try with
  // unconditional K access, so a refused second row can never cost a path
  // that would have routed before the budget existed.
  {
    bool anyUnrouted = false;
    for (int i = startIndex; i < numberOfPaths; i++) {
      if (globalState.connections.paths[i].pathType == VIRTUAL ||
          globalState.connections.paths[i].duplicate != 0) {
        continue;
      }
      if (!pathFullyRouted(i)) {
        anyUnrouted = true;
        break;
      }
    }
    if (anyUnrouted) {
      kRowRescuePass = true;
      commitPaths(2, -1, 0, startIndex);
      resolveAltPaths(2, -1, 0, startIndex);
      resolveUncommittedHops(2, -1, 0, startIndex);
      kRowRescuePass = false;
      #if PROFILE_BRIDGES_TO_PATHS
      Serial.print("  K-row rescue pass: "); Serial.print(micros() - btp_step); Serial.println(" us");
      btp_step = micros();
      #endif
    }
  }

  // Hub rescue: shapes no tier above tries, only for primaries still
  // unrouted (see rescueViaHubs). Before the duplicates, so a primary always
  // outranks a stacked copy for the copper it needs.
  rescueViaHubs(startIndex);
  // printPathsCompact(2 );
  // printChipStatus();
  // Serial.println("no duplicates");

  // printPathsCompact(2);
  // printChipStatus();

  // Serial.println("fillUnused: ");
  // Serial.println(fillUnused);

  if (fillUnused == 1) {
    fillUnusedPaths(jumperlessConfig.routing.stack_paths,
                    jumperlessConfig.routing.stack_rails,
                    jumperlessConfig.routing.stack_dacs);

    // printPathsCompact(2);
    // printChipStatus();

    for (int i = duplicateStartIndex; i < numberOfPaths; i++) {
      if (globalState.connections.paths[i].duplicate == 0) {
        continue;
      }
      
      // Skip virtual paths
      if (globalState.connections.paths[i].pathType == VIRTUAL) {
        continue;
      }
      
      findStartAndEndChips(globalState.connections.paths[i].node1, globalState.connections.paths[i].node2, i);
      mergeOverlappingCandidates(i);
      assignPathType(i);
    }

    // Duplicates are generated after the initial resolve pass, so resolve chip
    // candidates for the duplicate section before committing routes.
    resolveChipCandidates(duplicateStartIndex);

    // printPathsCompact(2);
    // printChipStatus();
    commitPaths(0, -1, 1);
    resolveAltPaths(0, -1, 1);

    resolveUncommittedHops(0, -1, 1);
  }
  
  // TDM OPTIMIZATION: Restore original net numbers after routing completes
  // This ensures correct LED colors and net display while allowing path sharing
  restoreFakeGpioInputNets();
  
  couldntFindPath(1);
  // couldntFindPath();
  checkForOverlappingPaths();
  validateAllPaths();
  #if PROFILE_BRIDGES_TO_PATHS
  Serial.print("  validation: "); Serial.print(micros() - btp_step); Serial.println(" us");
  btp_step = micros();
  #endif

  //   printPathsCompact(2 );
  // printChipStatus();
#if DEBUG_NTCC2_ENABLED
  if (debugNTCC2) {
    // delay(10);
    printPathsCompact(2);
    // delay(10);
    printChipStatus();
    // delay(10);
    printYPositionUsageReport();
  }
#endif

  // Resolve any Y position conflicts before final validation
  // if (resolveYPositionConflicts()) {
  //   if (debugNTCC6) {
  //     Serial.println("Conflicts were found and resolved - re-validating...");
  //   }
  // }

  // Detect and report any remaining conflicts
#if DEBUG_NTCC6_ENABLED
  detectAndReportConflicts();
#endif

  // Validate transaction consistency
#if DEBUG_NTCC6_ENABLED
  // validateTransactionConsistency();
#endif

  #if PROFILE_BRIDGES_TO_PATHS
  unsigned long btp_total = micros() - btp_start;
  Serial.print("  bridgesToPaths TOTAL: "); Serial.print(btp_total); Serial.println(" us");
  #endif
  
  // Update live crossbar display if enabled
  updateLiveCrossbarDisplay();
}

void fillUnusedPaths(int duplicatePathsOverride, int duplicatePathsPower,
                     int duplicatePathsDac) {
  /// return;

  int duplicatePathIndex = 0;

  uint8_t nodeCount[MAX_NETS] = {0};
  uint8_t bridgeCount[MAX_NETS] = {0};

    //   Serial.print("numberOfNets: ");
    // Serial.println(numberOfNets);
    // Serial.print("globalState.connections.nets[");


  for (int n = 0; n < numberOfNets; n++) {

    // Serial.print(n);
    // Serial.print("] \n\rnumber: ");
    // Serial.println(globalState.connections.nets[n].number);

    for (int i = 0; i < MAX_NODES; i++) {
      if (globalState.connections.nets[n].nodes[i] == 0) {
        break;
      }
      nodeCount[n]++;
      // Serial.print(" \n\rnode: ");
      // Serial.println(globalState.connections.nets[n].nodes[i]);
    }

    for (int i = 0; i < MAX_NODES; i++) {
      if (globalState.connections.nets[n].bridges[i][0] == 0) {
        break;
      }
      bridgeCount[n]++;
      // Serial.print(" \n\rbridges: ");
      // Serial.print(globalState.connections.nets[n].bridges[i][0]);
      // Serial.print("-");
      // Serial.println(globalState.connections.nets[n].bridges[i][1]);
    }
    // Serial.println("\n\r");
  }

  // ============================================================================
  // NEW PER-BRIDGE DUPLICATE PATH GENERATION
  // ============================================================================
  // Instead of duplicating all bridges in a net uniformly, we now create
  // duplicate paths for each bridge individually based on its bridges[i][2] value.
  // This allows fine-grained control: j.connect(2,23,1,3) creates 3 duplicates
  // for that specific bridge, while other bridges in the same net can have different counts.
  
  int duplindex = 0;
  // Keep these off stack to avoid intermittent stack-overflow crashes while
  // routing (fillUnusedPaths can be called deep in the routing pipeline).
  static int bridgeDuplicateBudget[MAX_BRIDGES];
  static int bridgeNode1[MAX_BRIDGES];
  static int bridgeNode2[MAX_BRIDGES];
  static int bridgeNet[MAX_BRIDGES];
  int maxDuplicateRounds = 0;
  int bridgesToProcess = globalState.connections.numBridges;
  if (bridgesToProcess > MAX_BRIDGES) {
    bridgesToProcess = MAX_BRIDGES;
    if (debugNTCC) {
      Serial.println("Warning: numBridges exceeds MAX_BRIDGES, truncating duplicate generation");
    }
  }

  for (int i = 0; i < bridgesToProcess; i++) {
    bridgeDuplicateBudget[i] = 0;
    bridgeNode1[i] = 0;
    bridgeNode2[i] = 0;
    bridgeNet[i] = 0;
  }
  
  // Process each bridge and create duplicate paths based on its individual duplicate count
  for (int bridgeIdx = 0; bridgeIdx < bridgesToProcess; bridgeIdx++) {
    int node1 = globalState.connections.bridges[bridgeIdx][0];
    int node2 = globalState.connections.bridges[bridgeIdx][1];
    int bridgeDuplicates = globalState.connections.bridges[bridgeIdx][2];

    // Infra bridges (probe power feed, lock bridges) are NEVER duplicated,
    // regardless of the stored count: their resistance must stay a single
    // known crosspoint path (the droop-current model depends on it). This
    // guards even when the stored count isn't 0 - addConnection's
    // dedupe-into-existing INCREMENTS the count if a user re-adds the pair.
    if (infraIsBridge(node1, node2)) continue;

    // Find the path index for this bridge
    int pathIdx = -1;
    for (int p = 0; p < numberOfPaths; p++) {
      if ((globalState.connections.paths[p].node1 == node1 && globalState.connections.paths[p].node2 == node2) ||
          (globalState.connections.paths[p].node1 == node2 && globalState.connections.paths[p].node2 == node1)) {
        if (globalState.connections.paths[p].duplicate == 0) {  // Find the main path, not a duplicate
          pathIdx = p;
          break;
        }
      }
    }
    
    if (pathIdx < 0) continue;  // Bridge not yet converted to path
    
    int netNum = globalState.connections.paths[pathIdx].net;
    if (netNum <= 0) continue;  // Invalid net
    
    // Check if this bridge should skip duplicates based on special conditions
    bool shouldSkipDuplicates = false;
    
    // Don't duplicate power rails (nets 1-3) unless explicitly set
    if (netNum >= 1 && netNum <= 3) {
      // Use duplicatePathsPower parameter for power rails if not explicitly set
      if (bridgeDuplicates < 0) {
        bridgeDuplicates = duplicatePathsPower;
      }
    }
    // Don't duplicate DAC nets (nets 4-5) unless explicitly set
    else if (netNum == 4 || netNum == 5) {
      // Use duplicatePathsDac parameter for DAC nets if not explicitly set
      if (bridgeDuplicates < 0) {
        bridgeDuplicates = duplicatePathsDac;
      }
    }
    // Regular nets - classify THIS BRIDGE's nodes: GPIO and ADC bridges have
    // their own stacking counts, fake/virtual ones never stack
    else {
      // GPIO (and UART) bridges stack per stack_gpio (default 0): a GPIO has
      // exactly one lane per breadboard chip on chip L, so a duplicate costs
      // some other net its route - the user opts in
      bool isGpioBridge = (node1 >= RP_GPIO_1 && node1 <= RP_GPIO_8) || node1 == RP_UART_TX || node1 == RP_UART_RX ||
                          (node2 >= RP_GPIO_1 && node2 <= RP_GPIO_8) || node2 == RP_UART_TX || node2 == RP_UART_RX;
      
      // Don't duplicate bridges connecting to fake GPIO pins (FAKE_GPIO_1 through FAKE_GPIO_32)
      if (!shouldSkipDuplicates) {
        if ((node1 >= FAKE_GPIO_1 && node1 <= FAKE_GPIO_32) ||
            (node2 >= FAKE_GPIO_1 && node2 <= FAKE_GPIO_32)) {
          shouldSkipDuplicates = true;
        }
      }
      
      // Don't duplicate virtual paths (they use chipXY snapshots, not physical routing)
      if (!shouldSkipDuplicates && globalState.connections.paths[pathIdx].pathType == VIRTUAL) {
        shouldSkipDuplicates = true;
      }
      
      // ADC bridges stack per stack_adcs (default 0): a high-impedance input
      // gains nothing from a parallel path
      bool isAdcBridge = (node1 >= ADC0 && node1 <= ADC4) || node1 == ADC7 ||
                         (node2 >= ADC0 && node2 <= ADC4) || node2 == ADC7;
      
      // Default (-1, "the router decides") resolves per class. A stored count
      // >= 0 is an explicit per-connection request and is honoured as-is.
      if (bridgeDuplicates < 0) {
        if (shouldSkipDuplicates)  bridgeDuplicates = 0;
        else if (isGpioBridge)     bridgeDuplicates = jumperlessConfig.routing.stack_gpio;
        else if (isAdcBridge)      bridgeDuplicates = jumperlessConfig.routing.stack_adcs;
        else                       bridgeDuplicates = jumperlessConfig.routing.stack_paths;
      }
    }
     // Serial.print("globalState.connections.nets[");
    // Serial.print(globalState.connections.paths[i].net);
    // Serial.print("] numberOfDuplicates: ");
    // Serial.println(globalState.connections.nets[globalState.connections.paths[i].net].numberOfDuplicates);
  //}

  // get the nodes in the net and cycle them, so if the bridges are A-B, B-C,
  // the duplicate paths will start with A-C

  //  A-B, B-C                        -> A-C
  //  A-B, B-C, C-D                   -> A-C, A-D, B-D
  //  A-B, B-C, C-D, D-E              -> A-C, A-D, A-E, B-D, B-E,
  //  A-B, B-C, C-D, D-E, E-F         -> A-C, A-D, A-E, A-F, B-D, B-E, B-F, C-E,
  //  C-F, D-F A-B, B-C, C-D, D-E, E-F, F-G    -> A-C, A-D, A-E, A-F, A-G, B-D,
  //  B-E, B-F, B-G, C-E, C-F, C-G, D-F, D-G, E-G A-B, B-C, C-D, D-E, E-F, F-G,
  //  G-H -> A-C, A-D, A-E, A-F, A-G, A-H, B-D, B-E, B-F, B-G, B-H, C-E, C-F,
  //  C-G, C-H, D-F, D-G, D-H, E-G, E-H, F-H

  // int bridgeLUT[MAX_DUPLICATE] = {1, 1, 3, 5, 10, 15, 21, 28, 36, 45, 55, 66,
  // 78, 91, 105, 120, 136, 153, 171, 190, 210, 231, 253, 276, 300};

  // int16_t tempNodes[MAX_NETS][MAX_NODES] = {0};

  // for (int i = 1; i < numberOfNets; i++) {
  //   if (globalState.connections.nets[i].numberOfDuplicates == 0) {
  //     // Serial.print("globalState.connections.nets[");
  //     // Serial.print(i);
  //     // Serial.println("] numberOfDuplicates is 0");
  //     continue;
  //   }

  //   // int16_t tempNodes[MAX_NODES];
  //   //  Serial.print("globalState.connections.nets[");
  //   //  Serial.print(i);
  //   //  Serial.print("]  nodes[");

  //   for (int j = 0; j < nodeCount[i]; j++) {
  //     // tempNodes[j] = globalState.connections.nets[i].nodes[j];
  //     //   Serial.print(tempNodes[j]);
  //     // Serial.print(globalState.connections.nets[i].nodes[j]);
  //     // Serial.print(", ");
  //   }
  //   // Serial.println("]\t\t");

  //   int targetBridgeCount = globalState.connections.nets[i].numberOfDuplicates;
  //   int skip = 1;

  //   int unique = 0;

  //   int testCounter0 = 0;
  //   int testCounter1 = 1; // nodeCount[i] / 2;
  //   int testBridge[2] = {-1, -1};

  //   int bridge0 = 0;
  //   int bridge1 = 1;

  //   for (int j = 0; j < targetBridgeCount; j++) {
  //     if (nodeCount[i] >= 3) {
  //       for (int l = 0; l < MAX_DUPLICATE; l++) {
  //         if (unique == -1) {
  //           bridge1++;
  //           if (bridge1 >= nodeCount[i]) {
  //             bridge0++;
  //             if (bridge0 >= nodeCount[i]) {
  //               bridge0 = 0;
  //             }
  //             bridge1 = bridge0 + 1;
  //           }
  //           unique = 0;
  //         }
  //         if (globalState.connections.nets[i].nodes[bridge0] == 0 || globalState.connections.nets[i].nodes[bridge1] == 0) {
  //           break;
  //         }
  //         if (globalState.connections.nets[i].nodes[bridge0] == globalState.connections.nets[i].nodes[bridge1]) {
  //           unique = -1;
  //           continue;
  //         }
  //         if (globalState.connections.nets[i].nodes[bridge0] == globalState.connections.nets[i].bridges[l][0] &&
  //             globalState.connections.nets[i].nodes[bridge1] == globalState.connections.nets[i].bridges[l][1]) {
  //           unique = -1;
  //           continue;
  //         }
  //         if (globalState.connections.nets[i].nodes[bridge0] == globalState.connections.nets[i].bridges[l][1] &&
  //             globalState.connections.nets[i].nodes[bridge1] == globalState.connections.nets[i].bridges[l][0]) {
  //           unique = -1;
  //           continue;
  //         }
  //         unique = 1;
  //         // Serial.print(globalState.connections.nets[i].nodes[bridge0]);
  //         // Serial.print("-");
  //         // Serial.print(globalState.connections.nets[i].nodes[bridge1]);
  //         // Serial.println("]\t\t");
  //         // Serial.print("globalState.connections.nets[");

  //         break;
  //       }
  //     }
  //     newBridges[i][j][0] = globalState.connections.nets[i].nodes[bridge0];
  //     newBridges[i][j][1] = globalState.connections.nets[i].nodes[bridge1];

  //     globalState.connections.nets[i].bridges[j][0] = newBridges[i][j][0];
  //     globalState.connections.nets[i].bridges[j][1] = newBridges[i][j][1];

  //     bridge1++;

  //     if (bridge1 >= nodeCount[i]) {
  //       bridge0++;
  //       if (bridge0 >= nodeCount[i]) {
  //         bridge0 = 0;
  //       }
  //       bridge1 = bridge0 + 1;
  //     }

  //     if (newBridges[i][j][0] == newBridges[i][j][1] ||
  //         newBridges[i][j][0] == 0 || newBridges[i][j][1] == 0) {
  //       // Serial.print("skipping ");
  //       // Serial.println(j);
  //       j--;
  //       continue;
  //     } else {
  //       duplicatePathIndex++;
  //     }
  //   }
  // }
  // // int maxxed = 0;
  // int priorities[MAX_NETS] = {0};
  // int maxp = 0;

  // for (int j = 0; j < MAX_DUPLICATE; j++) {
  //   for (int i = 0; i < numberOfNets; i++) {
  //     priorities[i] = globalState.connections.nets[i].priority;
  //     if (i < 6 && globalState.connections.nets[i].priority > maxp) {
  //       maxp = globalState.connections.nets[i].priority;
  //     }
  //   }
  //   for (int k = 0; k < maxp; k++) {
  //     for (int i = 0; i < 6; i++) {
  //       // for (int p = 0; p < globalState.connections.nets[i].priority; p++) {

  //       if (globalState.connections.nets[i].numberOfDuplicates == 0) {
  //         continue;
  //       }

  //       // if (newBridges[i][j][0] >= 110 && newBridges[i][j][0] <= 115 ||
  //       //     newBridges[i][j][1] >= 110 && newBridges[i][j][1] <= 115) {
  //       //   continue;
  //       // }

  //       if (priorities[i] < 0) { ///!
  //         continue;
  //       }

  //       if (priorities[i] > 0) {
  //         priorities[i]--;
  //       }

  //       //! make it add the the priority so the connections are mixed
  //       if (probePowerDAC == 0) {
  //         if (newBridges[i][j][0] == ROUTABLE_BUFFER_IN &&
  //                 newBridges[i][j][1] == DAC0 ||
  //             newBridges[i][j][0] == DAC0 &&
  //                 newBridges[i][j][1] == ROUTABLE_BUFFER_IN) {
  //           continue;
  //         }
  //       } else if (probePowerDAC == 1) {
  //         if (newBridges[i][j][0] == ROUTABLE_BUFFER_IN &&
  //                 newBridges[i][j][1] == DAC1 ||
  //             newBridges[i][j][0] == DAC1 &&
  //                 newBridges[i][j][1] == ROUTABLE_BUFFER_IN) {
  //           continue;
  //         }
  //       }
  //       if (newBridges[i][j][0] != 0 || newBridges[i][j][1] != 0) {
  //         globalState.connections.paths[numberOfPaths].net = i;
  //         globalState.connections.paths[numberOfPaths].node1 = newBridges[i][j][0];
  //         globalState.connections.paths[numberOfPaths].node2 = newBridges[i][j][1];
  //         globalState.connections.paths[numberOfPaths].altPathNeeded = false;
  //         globalState.connections.paths[numberOfPaths].sameChip = false;
  //         globalState.connections.paths[numberOfPaths].skip = false;
  //         globalState.connections.paths[numberOfPaths].duplicate = 1;
  //         numberOfPaths++;
  //         if (numberOfPaths >= MAX_BRIDGES) {
  //           // maxxed = 1;
  //           return;
  //           break;
  //         }
  //       }
  //       // }
  //       // Serial.print("\n\r");
  //     }
  //   }

  //   // for (int i = 0; i < 6; i++) {
  //   //   priorities[i] = globalState.connections.nets[i].priority;
  //   // }
  //   for (int i = 5; i < numberOfNets; i++) {
  //     if (globalState.connections.nets[i].numberOfDuplicates == 0) {
  //       continue;
  //     }

  //     if (newBridges[i][j][0] >= 110 && newBridges[i][j][0] <= 115 ||
  //         newBridges[i][j][1] >= 110 && newBridges[i][j][1] <= 115) {
  //       continue;
  //     }

  //     if (priorities[i] <= 0) {
  //       continue;
  //     }
    // Override to 0 if this bridge type should never be duplicated
    if (shouldSkipDuplicates && bridgeDuplicates < 0) {
      bridgeDuplicates = 0;
    }
    
    if (bridgeDuplicates < 0) {
      bridgeDuplicates = 0;
    }

    // Stash per-bridge duplicate budgets. We apply them in round-robin order
    // below so early bridges do not starve later power rails.
    bridgeDuplicateBudget[bridgeIdx] = bridgeDuplicates;
    bridgeNode1[bridgeIdx] = node1;
    bridgeNode2[bridgeIdx] = node2;
    bridgeNet[bridgeIdx] = netNum;

    if (bridgeDuplicates > maxDuplicateRounds) {
      maxDuplicateRounds = bridgeDuplicates;
    }
  }

  // Round-robin duplicate creation:
  // pass 0 adds first duplicate for each eligible bridge,
  // pass 1 adds second duplicate, etc.
  // This keeps GND/TOP/BOT and regular duplicates interleaved.
  for (int round = 0; round < maxDuplicateRounds; round++) {
    for (int bridgeIdx = 0; bridgeIdx < bridgesToProcess; bridgeIdx++) {
      if (bridgeDuplicateBudget[bridgeIdx] <= round) {
        continue;
      }

      if (numberOfPaths >= MAX_BRIDGES) {
        Serial.println("Warning: MAX_BRIDGES reached during duplicate path generation");
        return;  // Can't add more paths
      }

      // Create a duplicate path with the same nodes and net
      globalState.connections.paths[numberOfPaths].net = bridgeNet[bridgeIdx];
      globalState.connections.paths[numberOfPaths].node1 = bridgeNode1[bridgeIdx];
      globalState.connections.paths[numberOfPaths].node2 = bridgeNode2[bridgeIdx];
      globalState.connections.paths[numberOfPaths].altPathNeeded = false;
      globalState.connections.paths[numberOfPaths].sameChip = false;
      globalState.connections.paths[numberOfPaths].skip = false;
      globalState.connections.paths[numberOfPaths].duplicate = 1;  // Mark as duplicate path
      numberOfPaths++;
      duplindex++;
    }
  }
  
  if (debugNTCC5) {
    Serial.print("Created ");
    Serial.print(duplindex);
    Serial.println(" duplicate paths from bridge duplicate counts");
  }

  // ============================================================================
  // OLD NET-BASED DUPLICATE PATH GENERATION (DISABLED)
  // ============================================================================
  // This legacy code created additional bridges between node pairs in multi-node nets
  // (e.g., A-B, B-C would create A-C as a redundant path).
  // This is now DISABLED in favor of the per-bridge duplicate system above.
  // The old system is preserved here (commented) for reference in case it's needed
  // for special cases like power rail redundancy.
  
  // If you need net-wide mesh topology duplicates (A-B, B-C -> A-C), uncomment this section
  // and set globalState.connections.nets[i].numberOfDuplicates to enable it per-net.
  
  // int priorities[MAX_NETS] = {0};
  // int maxp = 0;
  // 
  // for (int j = 0; j < MAX_DUPLICATE; j++) {
  //   ... (old combinatorial bridge generation logic)
  // }
  // Serial.print("done filling unused paths\n\r");
}

void commitPaths(int allowStacking, int powerOnly, int noOrOnlyDuplicates, int startIndex) {
  DEBUG_NTCC2_PRINTLN("commitPaths()\n\r");

  for (int i = startIndex; i < numberOfPaths; i++) {
    // Skip virtual paths
    if (globalState.connections.paths[i].pathType == VIRTUAL) {
      continue;
    }
    // K-row rescue pass: only the paths the budgeted pass left unrouted
    if (kRowRescuePass && pathFullyRouted(i)) {
      continue;
    }
    
    // duplicateSFnets();
    // Serial.print(i);
    // Serial.print(" \t");

    DEBUG_NTCC1_PRINT("\n\rpath[");
    DEBUG_NTCC1_PRINT(i);
    DEBUG_NTCC1_PRINT("] net: ");
    DEBUG_NTCC1_PRINT(globalState.connections.paths[i].net);
    DEBUG_NTCC1_PRINT("   \t ");

    if (debugNTCC) {
      printNodeOrName(globalState.connections.paths[i].node1);
    }
    DEBUG_NTCC1_PRINT(" to ");
    if (debugNTCC) {
      printNodeOrName(globalState.connections.paths[i].node2);
    }

    // Skip paths that are already successfully committed (x values set and no
    // altPath needed)
    if (globalState.connections.paths[i].x[0] >= 0 && globalState.connections.paths[i].x[1] >= 0 && !globalState.connections.paths[i].altPathNeeded) {
      continue; // Path already committed successfully
    }

    if (powerOnly == 1 && globalState.connections.paths[i].net > 5) {
      continue;
    }
    if (powerOnly == 1 && globalState.connections.paths[i].duplicate == 1) {
      continue;
    }

    if (noOrOnlyDuplicates == 1 && globalState.connections.paths[i].duplicate == 0) {
      continue;
    }

    if (noOrOnlyDuplicates == 0 && globalState.connections.paths[i].duplicate == 1) {
      continue;
    }

    // K y-row reservation: mark whether this iteration routes a duplicate
    routingDuplicatePathNow = (globalState.connections.paths[i].duplicate == 1);

    // Try path first without stacking, then with stacking if needed (only if
    // allowStacking is 2)
    bool pathCommitted = false;
    int maxStackingAttempts =
        (allowStacking == 2) ? 1 : 0; // 0-1 if allowStacking==2, 0-0 otherwise

    // Store original path state for retry attempts
    bool originalAltPathNeeded = globalState.connections.paths[i].altPathNeeded;

    for (int stackingAttempt = 0;
         stackingAttempt <= maxStackingAttempts && !pathCommitted;
         stackingAttempt++) {
      int currentAllowStacking =
          (allowStacking == 2) ? stackingAttempt : allowStacking;

      // Reset altPathNeeded for retry attempt
      if (stackingAttempt == 1) {
        globalState.connections.paths[i].altPathNeeded = originalAltPathNeeded; // Restore original state
      }

      switch (globalState.connections.paths[i].pathType) {
      case BBtoBB: {
        // Serial.print("BBtoBB\t");
        int freeLane = -1;
        int xMapL0c0 = xMapForChipLane0(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].chip[1]);
        int xMapL1c0 = xMapForChipLane1(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].chip[1]);

        int xMapL0c1 = xMapForChipLane0(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].chip[0]);
        int xMapL1c1 = xMapForChipLane1(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].chip[0]);

        if (globalState.connections.paths[i].chip[0] ==
            globalState.connections.paths[i].chip[1]) { // && (globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].yStatus[0] == -1)) {
          // if (globalState.connections.paths[i].sameChip == true ){
          //  Serial.print("same chip  ");
          setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
          setPathY(i, 1, yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[0]));
          setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                             "same-chip Y0");
          setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                             "same-chip Y1");
          setPathX(i, 0, -2);
          setPathX(i, 1, -2);

          if (debugNTCC == true) {
            Serial.print("path [");
            Serial.print(i);
            Serial.print("]  ");

            Serial.print(" \tchip[0]: ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));

            Serial.print("  x[0]: ");
            Serial.print(globalState.connections.paths[i].x[0]);

            Serial.print("  y[0]: ");
            Serial.print(globalState.connections.paths[i].y[0]);

            Serial.print("\t  chip[1]: ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));

            Serial.print("  x[1]: ");
            Serial.print(globalState.connections.paths[i].x[1]);

            Serial.print("  y[1]: ");
            Serial.print(globalState.connections.paths[i].y[1]);
          }

          pathCommitted = true;
          break;
        }

        DEBUG_NTCC2_PRINT("xMapL0c0: ");
        DEBUG_NTCC2_PRINTLN(xMapL0c0);
        DEBUG_NTCC2_PRINT("xMapL0c1: ");
        DEBUG_NTCC2_PRINTLN(xMapL0c1);
        DEBUG_NTCC2_PRINT("xMapL1c0: ");
        DEBUG_NTCC2_PRINTLN(xMapL1c0);
        DEBUG_NTCC2_PRINT("xMapL1c1: ");
        DEBUG_NTCC2_PRINTLN(xMapL1c1);

        // Check if we can commit this path without conflicts
        bool canCommitLane0 =
            freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapL0c0, globalState.connections.paths[i].net,
                           currentAllowStacking) &&
            freeOrSameNetX(globalState.connections.paths[i].chip[1], xMapL0c1, globalState.connections.paths[i].net,
                           currentAllowStacking);
        bool canCommitLane1 =
            (xMapL1c0 != -1) &&
            freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapL1c0, globalState.connections.paths[i].net,
                           currentAllowStacking) &&
            freeOrSameNetX(globalState.connections.paths[i].chip[1], xMapL1c1, globalState.connections.paths[i].net,
                           currentAllowStacking);

        if (canCommitLane0) {
          freeLane = 0;
        } else if (canCommitLane1) {
          freeLane = 1;
        } else {
          globalState.connections.paths[i].altPathNeeded = true;
          DEBUG_NTCC2_PRINT(
              "\tno free lanes for path, setting altPathNeeded flag");
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL0c0]);
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL1c0]);
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xStatus[xMapL0c1]);
          DEBUG_NTCC2_PRINT(" \t ");
          DEBUG_NTCC2_PRINT(globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xStatus[xMapL1c1]);
          DEBUG_NTCC2_PRINTLN(" \t ");
          break;
        }

        // Save state before making any assignments
        saveRoutingState(i);

        bool allAssignmentsSuccessful = false;

        // Only commit chip state after validating ALL connections
        if (freeLane == 0) {
          bool x0Success = setChipXStatus(globalState.connections.paths[i].chip[0], xMapL0c0,
                                          globalState.connections.paths[i].net, "BBtoBB lane0 chip0");
          bool x1Success = setChipXStatus(globalState.connections.paths[i].chip[1], xMapL0c1,
                                          globalState.connections.paths[i].net, "BBtoBB lane0 chip1");
          bool pathX0Success = setPathX(i, 0, xMapL0c0);
          bool pathX1Success = setPathX(i, 1, xMapL0c1);

          if (!x0Success || !x1Success || !pathX0Success || !pathX1Success) {
            if (debugNTCC6) {
              Serial.print("BBtoBB lane0 X assignment failed for globalState.connections.paths[");
              Serial.print(i);
              Serial.println("], will restore state");
            }
            allAssignmentsSuccessful = false;
          } else {
            allAssignmentsSuccessful = true;
          }
        } else if (freeLane == 1) {
          bool x0Success = setChipXStatus(globalState.connections.paths[i].chip[0], xMapL1c0,
                                          globalState.connections.paths[i].net, "BBtoBB lane1 chip0");
          bool x1Success = setChipXStatus(globalState.connections.paths[i].chip[1], xMapL1c1,
                                          globalState.connections.paths[i].net, "BBtoBB lane1 chip1");
          bool pathX0Success = setPathX(i, 0, xMapL1c0);
          bool pathX1Success = setPathX(i, 1, xMapL1c1);

          if (!x0Success || !x1Success || !pathX0Success || !pathX1Success) {
            if (debugNTCC6) {
              Serial.print("BBtoBB lane1 X assignment failed for globalState.connections.paths[");
              Serial.print(i);
              Serial.println("], will restore state");
            }
            allAssignmentsSuccessful = false;
          } else {
            allAssignmentsSuccessful = true;
          }
        }

        // Continue with Y assignments only if X assignments succeeded
        if (allAssignmentsSuccessful) {
          bool y0PathSuccess =
              setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
          bool y1PathSuccess =
              setPathY(i, 1, yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));

          // Check if Y position assignments succeed
          bool y0ChipSuccess = false;
          bool y1ChipSuccess = false;

          if (y0PathSuccess) {
            y0ChipSuccess =
                setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                   "BBtoBB Y0 assignment");
          }
          if (y1PathSuccess) {
            y1ChipSuccess =
                setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                   "BBtoBB Y1 assignment");
          }

          if (!y0PathSuccess || !y1PathSuccess || !y0ChipSuccess ||
              !y1ChipSuccess) {
            allAssignmentsSuccessful = false;
            if (debugNTCC6) {
              Serial.print("Path[");
              Serial.print(i);
              Serial.print("] net ");
              Serial.print(globalState.connections.paths[i].net);
              Serial.print(" Y assignment failed");
              if (!y0PathSuccess || !y0ChipSuccess) {
                Serial.print(" (Y0=");
                Serial.print(globalState.connections.paths[i].y[0]);
                Serial.print(" on chip ");
                Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));
                Serial.print(" failed)");
              }
              if (!y1PathSuccess || !y1ChipSuccess) {
                Serial.print(" (Y1=");
                Serial.print(globalState.connections.paths[i].y[1]);
                Serial.print(" on chip ");
                Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));
                Serial.print(" failed)");
              }
              Serial.println(", will restore state");
            }
          }
        }

        // Handle success or failure
        if (allAssignmentsSuccessful) {
          commitRoutingState();
          if (debugNTCC6) {
            Serial.print("BBtoBB globalState.connections.paths[");
            Serial.print(i);
            Serial.println("] assignments committed successfully");
          }
        } else {
          restoreRoutingState(i);
          globalState.connections.paths[i].altPathNeeded = true;
          if (debugNTCC6) {
            Serial.print("BBtoBB globalState.connections.paths[");
            Serial.print(i);
            Serial.println(
                "] assignments failed, state restored, marked for alt routing");
          }
          break;
        }

        if (debugNTCC2 == true) {
          Serial.print(" \tchip[0]: ");
          Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));

          Serial.print("  x[0]: ");
          Serial.print(globalState.connections.paths[i].x[0]);

          Serial.print("  y[0]: ");
          Serial.print(globalState.connections.paths[i].y[0]);

          Serial.print("\t  chip[1]: ");
          Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));

          Serial.print("  x[1]: ");
          Serial.print(globalState.connections.paths[i].x[1]);

          Serial.print("  y[1]: ");
          Serial.print(globalState.connections.paths[i].y[1]);

          Serial.print(" \t ");
          Serial.print(globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL0c0]);

          Serial.print(" \t ");
          Serial.print(globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xStatus[xMapL0c1]);
          Serial.print(" \t ");
        }
        pathCommitted = true;
        break;
      }

      case BBtoNANO:
      case BBtoSF: // nodes should always be in order of the enum, so node1 is
                   // BB and node2 is SF
      {
        int xMapBBc0 = xMapForChipLane0(
            globalState.connections.paths[i].chip[0], globalState.connections.paths[i].chip[1]); // find x connection to sf chip

        int xMapSFc1 = xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]);
        int yMapSFc1 = globalState.connections.paths[i].chip[0];

        // if ((globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapBBc0] == globalState.connections.paths[i].net ||
        //      globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapBBc0] == -1) &&
        //     (globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].yStatus[yMapSFc1] == globalState.connections.paths[i].net ||
        //      globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].yStatus[yMapSFc1] ==
        //          -1)) // how's that for a fuckin if statement
        // {

        if (freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapBBc0, globalState.connections.paths[i].net,
                           currentAllowStacking) == true &&
            freeOrSameNetY(globalState.connections.paths[i].chip[1], yMapSFc1, globalState.connections.paths[i].net,
                           currentAllowStacking) == true) {

          // Save state before making any assignments
          saveRoutingState(i);

          bool pathX0Success = setPathX(i, 0, xMapBBc0);
          bool pathX1Success = setPathX(i, 1, xMapSFc1);

          bool pathY0Success =
              setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));

          bool pathY1Success = setPathY(
              i, 1,
              globalState.connections.paths[i]
                  .chip[0]); // bb to sf connections are always in chip order,
          // so chip A is always connected to sf y 0

          bool chipX0Success = false;
          bool chipY0Success = false;
          bool chipX1Success = false;
          bool chipY1Success = false;

          if (pathX0Success) {
            chipX0Success = setChipXStatus(globalState.connections.paths[i].chip[0], xMapBBc0,
                                           globalState.connections.paths[i].net, "BBtoSF chip0");
          }
          if (pathY0Success) {
            chipY0Success = setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0],
                                               globalState.connections.paths[i].net, "BBtoSF Y0");
          }
          if (pathX1Success) {
            chipX1Success = setChipXStatus(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].x[1],
                                           globalState.connections.paths[i].net, "BBtoSF chip1");
          }
          if (pathY1Success) {
            chipY1Success = setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].chip[0],
                                               globalState.connections.paths[i].net, "BBtoSF Y1");
          }

          bool allAssignmentsSuccessful = pathX0Success && pathX1Success &&
                                          pathY0Success && pathY1Success &&
                                          chipX0Success && chipY0Success &&
                                          chipX1Success && chipY1Success;

          if (allAssignmentsSuccessful) {
            commitRoutingState();
            pathCommitted = true;
            if (debugNTCC6) {
              Serial.print("BBtoSF globalState.connections.paths[");
              Serial.print(i);
              Serial.println("] assignments committed successfully");
            }
          } else {
            restoreRoutingState(i);
            globalState.connections.paths[i].altPathNeeded = true;
            if (debugNTCC6) {
              Serial.print("BBtoSF assignment failed for globalState.connections.paths[");
              Serial.print(i);
              Serial.println("], state restored, marking for alt routing");
            }
            break;
          }

          if (debugNTCC2 == true) {
            // delay(10);

            Serial.print(" \t\n\rchip[0]: ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));

            Serial.print("  x[0]: ");
            Serial.print(globalState.connections.paths[i].x[0]);

            Serial.print("  y[0]: ");
            Serial.print(globalState.connections.paths[i].y[0]);

            Serial.print("\t  chip[1]: ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));

            Serial.print("  x[1]: ");
            Serial.print(globalState.connections.paths[i].x[1]);

            Serial.print("  y[1]: ");
            Serial.print(globalState.connections.paths[i].y[1]);

            Serial.print(" \t ");
            Serial.print(globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapBBc0]);

            Serial.print(" \t ");
            Serial.print(globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xStatus[xMapSFc1]);
            Serial.print(" \t ");

            Serial.println("  ");
          }
          pathCommitted = true;
        } else {
          globalState.connections.paths[i].altPathNeeded = true;

          if (debugNTCC2) {
            // delay(10);
            Serial.print(
                "\tno direct path, setting altPathNeeded flag (BBtoSF)");
          }
          break;
        }
        break;
      }
      case NANOtoSF:
      case NANOtoNANO: {
        // Serial.print(" NANOtoNANO  ");
        int xMapNANOC0 = xMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]);
        int xMapNANOC1 = xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]);

        if (debugNTCC6 && globalState.connections.paths[i].net == 23) {
          Serial.print("GP_8-D3 in commitPaths NANOtoSF globalState.connections.paths[");
          Serial.print(i);
          Serial.print("] - chip[0]=");
          Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));
          Serial.print(" chip[1]=");
          Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));
          Serial.print(" xMapNANOC0=");
          Serial.print(xMapNANOC0);
          Serial.print(" xMapNANOC1=");
          Serial.println(xMapNANOC1);
        }

        if (debugNTCC2) {
          Serial.print("NANOtoNANO globalState.connections.paths[");
          Serial.print(i);
          Serial.print("] - chip[0]=");
          Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));
          Serial.print(" chip[1]=");
          Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));
          Serial.print(" xMapNANOC0=");
          Serial.print(xMapNANOC0);
          Serial.print(" xMapNANOC1=");
          Serial.println(xMapNANOC1);
        }

        if (globalState.connections.paths[i].chip[0] == globalState.connections.paths[i].chip[1]) {
          if (debugNTCC2) {
            Serial.print("Same chip - checking X connections...");
          }
          bool x0Free = freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapNANOC0, globalState.connections.paths[i].net,
                                       currentAllowStacking);
          bool x1Free = freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapNANOC1, globalState.connections.paths[i].net,
                                       currentAllowStacking);
          if (debugNTCC2) {
            Serial.print(" x0Free=");
            Serial.print(x0Free);
            Serial.print(" x1Free=");
            Serial.println(x1Free);
          }

          // Check if this would violate Y position limits for high-traffic
          // chips
          bool respectsYLimits = true;
          if (globalState.connections.paths[i].chip[0] >= 8) { // Special function chips
            if (!canNetUseMoreYPositions(globalState.connections.paths[i].net)) {
              respectsYLimits = false;
              if (debugNTCC2) {
                Serial.print(" - rejected same-chip connection due to Y limits "
                             "for net ");
                Serial.println(globalState.connections.paths[i].net);
              }
            }
          }

          if (x0Free == true && respectsYLimits) {
            if (x1Free == true) {
              if (debugNTCC2) {
                Serial.print("COMMITTING NANOtoNANO same-chip globalState.connections.paths[");
                Serial.print(i);
                Serial.print("] - setting x[0]=");
                Serial.print(xMapNANOC0);
                Serial.print(" x[1]=");
                Serial.println(xMapNANOC1);
              }

              // Save state before making any assignments
              saveRoutingState(i);

              bool chipX0Success =
                  setChipXStatus(globalState.connections.paths[i].chip[0], xMapNANOC0, globalState.connections.paths[i].net,
                                 "NANOtoNANO same-chip x0");
              bool chipX1Success =
                  setChipXStatus(globalState.connections.paths[i].chip[1], xMapNANOC1, globalState.connections.paths[i].net,
                                 "NANOtoNANO same-chip x1");

              bool pathX0Success = setPathX(i, 0, xMapNANOC0);
              bool pathX1Success = setPathX(i, 1, xMapNANOC1);

              bool pathY0Success = setPathY(i, 0, -2);
              bool pathY1Success = setPathY(i, 1, -2);

              bool allAssignmentsSuccessful = chipX0Success && chipX1Success &&
                                              pathX0Success && pathX1Success &&
                                              pathY0Success && pathY1Success;

              if (allAssignmentsSuccessful) {
                commitRoutingState();
                globalState.connections.paths[i].sameChip = true;
                pathCommitted = true;
                if (debugNTCC6) {
                  Serial.print("NANOtoNANO same-chip globalState.connections.paths[");
                  Serial.print(i);
                  Serial.println("] assignments committed successfully");
                }
              } else {
                restoreRoutingState(i);
                globalState.connections.paths[i].altPathNeeded = true;
                if (debugNTCC6) {
                  Serial.print(
                      "NANOtoNANO same-chip assignment failed for globalState.connections.paths[");
                  Serial.print(i);
                  Serial.println("], state restored, marking for alt routing");
                }
              }
              // Serial.print(" ?????????");
              if (debugNTCC2) {
                Serial.print(" \t\t\tchip[0]: ");
                Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));

                Serial.print("  x[0]: ");
                Serial.print(globalState.connections.paths[i].x[0]);

                Serial.print("  y[0]: ");
                Serial.print(globalState.connections.paths[i].y[0]);

                Serial.print("\t  chip[1]: ");
                Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));

                Serial.print("  x[1]: ");
                Serial.print(globalState.connections.paths[i].x[1]);

                Serial.print("  y[1]: ");
                Serial.print(globalState.connections.paths[i].y[1]);
              }
              pathCommitted = true;
            } else {
              // Same chip but one X connection not available - need alt path
              globalState.connections.paths[i].altPathNeeded = true;
              if (debugNTCC2) {
                Serial.print("NANOtoNANO same chip: X1 connection conflict, "
                             "setting altPathNeeded for path ");
                Serial.println(i);
              }
            }
          } else {
            // Same chip but one X connection not available - need alt path
            globalState.connections.paths[i].altPathNeeded = true;
            if (debugNTCC2) {
              Serial.print("NANOtoNANO same chip: X0 connection conflict, "
                           "setting altPathNeeded for path ");
              Serial.println(i);
            }
          }
        } else {
          globalState.connections.paths[i].altPathNeeded = true;
          if (debugNTCC6 && globalState.connections.paths[i].net == 23) {
            Serial.print("GP_8-D3 setting altPathNeeded flag in commitPaths "
                         "(different chips: ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));
            Serial.print(" != ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));
            Serial.println(")");
          }
          if (debugNTCC2) {
            Serial.print(
                "\n\rno direct path, setting altPathNeeded flag (NANOtoNANO)");
            Serial.print(" \t ");
            Serial.println(i);
            printPathsCompact();
          }
        }
      }
      default:
        // Handle unhandled path types (BBtoBBL, NANOtoBBL, SFtoSF, etc.)
        break;
        // case BBtoNANO:
      }
      // if (debugNTCC2)
      // {
      //     Serial.println("\n\r");
      // }
    } // end stacking attempt loop
  }
  routingDuplicatePathNow = false;
  // duplicateSFnets();
  //    printPathsCompact();
  //     printChipStatus();

  // duplicateSFnets();
}


int ijklPaths(int pathNumber, int currentAllowStacking) {
  // return 0;
  int chip0 = globalState.connections.paths[pathNumber].chip[0];
  int chip1 = globalState.connections.paths[pathNumber].chip[1];
  // int chip2 = globalState.connections.paths[pathNumber].chip[2];
  // int chip3 = globalState.connections.paths[pathNumber].chip[3];

  if (debugNTCC6) {
    Serial.print("ijklPaths() called for globalState.connections.paths[");
    Serial.print(pathNumber);
    Serial.print("] net=");
    Serial.print(globalState.connections.paths[pathNumber].net);
    Serial.print(" chips=[");
    Serial.print(chipNumToChar(chip0));
    Serial.print(",");
    Serial.print(chipNumToChar(chip1));
    Serial.print("]");
  }

  if (chip0 == chip1) {
    if (debugNTCC6) {
      Serial.println(" - same chip, skipping");
    }
    return 0;
  }
  if (chip0 < 8 || chip1 < 8) { // allow it to find a hop here
    if (debugNTCC6) {
      Serial.println(" - involves breadboard chip, skipping");
    }
    return 0;
  }

  // Check if this path is already committed (prevent duplicates)
  if (globalState.connections.paths[pathNumber].x[0] != -1 && globalState.connections.paths[pathNumber].x[1] != -1 &&
      !globalState.connections.paths[pathNumber].altPathNeeded) {
    if (debugNTCC) {
      Serial.println("ijklPaths: path already committed, skipping");
    }
    return 1; // Path already exists and is committed
  }

  int x0 = -1;
  int x1 = -1;
  //  printPathsCompact();
  //  printChipStatus();
  for (int i = 12; i < 15; i++) {
    if (globalState.connections.chipStates[chip0].xMap[i] == chip1) {
      x0 = i;
    }
    if (globalState.connections.chipStates[chip1].xMap[i] == chip0) {
      x1 = i;
    }
  }
  // if ((globalState.connections.chipStates[chip0].xStatus[x0] == -1 ||
  //      globalState.connections.chipStates[chip0].xStatus[x0] == globalState.connections.paths[pathNumber].net) &&
  //     (globalState.connections.chipStates[chip1].xStatus[x1] == -1 ||
  //      globalState.connections.chipStates[chip1].xStatus[x1] == globalState.connections.paths[pathNumber].net)) {

  if (freeOrSameNetX(chip0, x0, globalState.connections.paths[pathNumber].net, currentAllowStacking) ==
          true &&
      freeOrSameNetX(chip1, x1, globalState.connections.paths[pathNumber].net, currentAllowStacking) ==
          true) {

    if (debugNTCC6) {
      Serial.print(" - SUCCESS! Direct ijkl connection established: ");
      Serial.print(chipNumToChar(chip0));
      Serial.print(".X[");
      Serial.print(x0);
      Serial.print("] <-> ");
      Serial.print(chipNumToChar(chip1));
      Serial.print(".X[");
      Serial.print(x1);
      Serial.println("]");
    }

    // Save state before making any assignments
    saveRoutingState(pathNumber);

    bool interChipX0Success = setChipXStatus(chip0, x0, globalState.connections.paths[pathNumber].net,
                                             "ijklPaths inter-chip x0");
    bool interChipX1Success = setChipXStatus(chip1, x1, globalState.connections.paths[pathNumber].net,
                                             "ijklPaths inter-chip x1");
    bool pathX0Success =
        setPathX(pathNumber, 0, xMapForNode(globalState.connections.paths[pathNumber].node1, chip0));
    bool pathX1Success =
        setPathX(pathNumber, 1, xMapForNode(globalState.connections.paths[pathNumber].node2, chip1));

    bool nodeX0Success = true;
    bool nodeX1Success = true;
    if (globalState.connections.paths[pathNumber].x[0] != -1 && globalState.connections.paths[pathNumber].x[1] != -1) {
      nodeX0Success = setChipXStatus(chip0, globalState.connections.paths[pathNumber].x[0],
                                     globalState.connections.paths[pathNumber].net, "ijklPaths node x0");
      nodeX1Success = setChipXStatus(chip1, globalState.connections.paths[pathNumber].x[1],
                                     globalState.connections.paths[pathNumber].net, "ijklPaths node x1");
    }

    bool allAssignmentsSuccessful = interChipX0Success && interChipX1Success &&
                                    pathX0Success && pathX1Success &&
                                    nodeX0Success && nodeX1Success;

    if (!allAssignmentsSuccessful) {
      restoreRoutingState(pathNumber);
      if (debugNTCC6) {
        Serial.print("ijklPaths assignment failed for globalState.connections.paths[");
        Serial.print(pathNumber);
        Serial.println("], state restored, connection not established");
      }
      return 0; // Failed to establish connection
    }

    // If we get here, all assignments succeeded
    commitRoutingState();

    globalState.connections.chipStates[globalState.connections.paths[pathNumber].chip[0]].uncommittedHops++;
    globalState.connections.chipStates[globalState.connections.paths[pathNumber].chip[1]].uncommittedHops++;

    globalState.connections.paths[pathNumber].sameChip = true;
    // globalState.connections.paths[pathNumber].altPathNeeded = false;

    globalState.connections.paths[pathNumber].chip[2] = chip0;
    globalState.connections.paths[pathNumber].chip[3] = chip1;
    setPathX(pathNumber, 2, x0);
    setPathX(pathNumber, 3, x1);

    // For direct ijkl connections between special function chips, Y values
    // still need resolving Set them to -2 so resolveUncommittedHops can resolve
    // them
    setPathY(pathNumber, 0, -2);
    setPathY(pathNumber, 1, -2);
    setPathY(pathNumber, 2, -2);
    setPathY(pathNumber, 3, -2);
    //  printPathsCompact();
    //  printChipStatus();
    // Serial.print("pathNumber: ");
    // Serial.println(pathNumber);
    return 1;
  } else {
    if (debugNTCC6) {
      Serial.print(" - FAILED: ijkl connections not available (");
      Serial.print(chipNumToChar(chip0));
      Serial.print(".X[");
      Serial.print(x0);
      Serial.print("]=");
      Serial.print(globalState.connections.chipStates[chip0].xStatus[x0]);
      Serial.print(", ");
      Serial.print(chipNumToChar(chip1));
      Serial.print(".X[");
      Serial.print(x1);
      Serial.print("]=");
      Serial.print(globalState.connections.chipStates[chip1].xStatus[x1]);
      Serial.println(")");
    }
    return 0;
  }
}

void resolveAltPaths(int allowStacking, int powerOnly, int noOrOnlyDuplicates, int startIndex) {
  // if (debugNTCC5) {
  //  Serial.println("\n\rresolveAltPaths()");
  //  Serial.println(" ");
  //  Serial.print("numberOfPaths: ");
  //  Serial.println(numberOfPaths);
  //}
  // return;
  int couldFindPath = -1;

  for (int i = startIndex; i < numberOfPaths; i++) {
    // Skip virtual paths
    if (globalState.connections.paths[i].pathType == VIRTUAL) {
      continue;
    }
    // K-row rescue pass: a fully-routed ijkl path still carries altPathNeeded,
    // and re-entering ijklPaths would reset its y-rows to -2
    if (kRowRescuePass && pathFullyRouted(i)) {
      continue;
    }
    
    if (powerOnly == 1 && globalState.connections.paths[i].net > 5) {
      continue;
    }
    if (powerOnly == 1 && globalState.connections.paths[i].duplicate == 1) {
      continue;
    }

    if (noOrOnlyDuplicates == 1 && globalState.connections.paths[i].duplicate == 0) {
      continue;
    }

    if (noOrOnlyDuplicates == 0 && globalState.connections.paths[i].duplicate == 1) {
      continue;
    }

    // K y-row reservation: mark whether this iteration routes a duplicate
    routingDuplicatePathNow = (globalState.connections.paths[i].duplicate == 1);

    // Debug output removed for production

    if (globalState.connections.paths[i].altPathNeeded == true) {
      //   Serial.print("\n\n\rPATH: ");
      //   Serial.println(i);
      // Serial.print("globalState.connections.paths[i].altPathNeeded = ");
      // Serial.println(globalState.connections.paths[i].altPathNeeded);
      // Serial.print("numberOfPaths = ");
      // Serial.println(numberOfPaths);

      // Try alt path first without stacking, then with stacking if needed (only
      // if allowStacking is 2)
      bool altPathResolved = false;
      int maxStackingAttempts =
          (allowStacking == 2) ? 1
                               : 0; // 0-1 if allowStacking==2, 0-0 otherwise

      for (int stackingAttempt = 0;
           stackingAttempt <= maxStackingAttempts && !altPathResolved;
           stackingAttempt++) {
        int currentAllowStacking =
            (allowStacking == 2) ? stackingAttempt : allowStacking;

        // Reset altPathNeeded for retry attempt
        if (stackingAttempt == 1) {
          globalState.connections.paths[i].altPathNeeded = true;
        }

        // Try ijklPaths first with current stacking setting
        if (debugNTCC6 && globalState.connections.paths[i].net == 23) {
          Serial.print("Trying ijklPaths for GP_8-D3 connection (globalState.connections.paths[");
          Serial.print(i);
          Serial.println("])");
        }

        if (ijklPaths(i, currentAllowStacking) == 1) {
          if (debugNTCC6 && globalState.connections.paths[i].net == 23) {
            Serial.println("ijklPaths SUCCESS for GP_8-D3!");
          }
          altPathResolved = true;
          continue;
        }

        if (debugNTCC6 && globalState.connections.paths[i].net == 23) {
          Serial.println(
              "ijklPaths failed for GP_8-D3, trying other alt paths...");
        }

        switch (globalState.connections.paths[i].pathType) {
        case BBtoBB: {
          int foundPath = 0;
          if (debugNTCC2) {
            Serial.println("BBtoBB");
          }

          int yNode1 = yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]);
          int yNode2 = yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]);

          setChipYStatusSafe(globalState.connections.paths[i].chip[0], yNode1, globalState.connections.paths[i].net,
                             "BBtoBB alt Y0");
          setChipYStatusSafe(globalState.connections.paths[i].chip[1], yNode2, globalState.connections.paths[i].net,
                             "BBtoBB alt Y1");

          if (foundPath == 1) {
            couldFindPath = i;
            break;
          }
          int giveUpOnL = 0;
          int swapped = 0;

          int chipsWithFreeY0[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
          int numberOfChipsWithFreeY0 = 0;

          for (int chipFreeY = 0; chipFreeY < 8; chipFreeY++) {
            // if (globalState.connections.chipStates[chipFreeY].yStatus[0] == -1 ||
            //     globalState.connections.chipStates[chipFreeY].yStatus[0] == globalState.connections.paths[i].net) {

            if (freeOrSameNetY(chipFreeY, 0, globalState.connections.paths[i].net,
                               currentAllowStacking) == true) {
              chipsWithFreeY0[chipFreeY] = chipFreeY;
              numberOfChipsWithFreeY0++;
            }
          }

          for (int chipFreeY = 0; chipFreeY < 8; chipFreeY++) {
            if (debugNTCC2) {
              Serial.print("\n\r");
              Serial.print("path: ");
              Serial.print(i);
              Serial.print("\tindex: ");
              Serial.print(chipFreeY);
              Serial.print("  chip: ");
              printChipNumToChar(chipsWithFreeY0[chipFreeY]);
              Serial.print("\n\r");
            }
          }

          for (int bb = 0; bb < 8; bb++) // this is a long winded way to do this
                                         // but it's at least slightly readable
          {
            if (chipsWithFreeY0[bb] == -1) {
              // Serial.print("chipsWithFreeY0[");
              // Serial.print(bb);
              // Serial.print("] == -1\n\r");
              continue;
            }

            int xMapL0c0 = xMapForChipLane0(bb, globalState.connections.paths[i].chip[0]);
            int xMapL0c1 = xMapForChipLane0(bb, globalState.connections.paths[i].chip[1]);

            int xMapL1c0 = xMapForChipLane1(bb, globalState.connections.paths[i].chip[0]);
            int xMapL1c1 = xMapForChipLane1(bb, globalState.connections.paths[i].chip[1]);

            // if (bb == 7 && giveUpOnL == 0 && swapped == 0) {
            //   bb = 0;
            //   giveUpOnL = 0;
            //   swapped = 1;
            //   // Serial.println("\t\t\tt\t\t\tt\t\tswapped");
            //   // swapDuplicateNode(i);
            // } else if (bb == 7 && giveUpOnL == 0 && swapped == 1) {
            //   bb = 0;
            //   giveUpOnL = 1;
            // }

            //   if ((globalState.connections.chipStates[CHIP_L].yStatus[bb] != -1 &&
            //        globalState.connections.chipStates[CHIP_L].yStatus[bb] != globalState.connections.paths[i].net) &&
            //       giveUpOnL == 0) {

            //     continue;
            //   }

            if (globalState.connections.paths[i].chip[0] == bb || globalState.connections.paths[i].chip[1] == bb) {
              continue;
            }

            // if (globalState.connections.paths[i].chip[0] == globalState.connections.paths[i].chip[1]) {
            //   continue;
            // }

            // if (globalState.connections.chipStates[bb].xStatus[] == globalState.connections.paths[i].net ||
            //     globalState.connections.chipStates[bb].xStatus[xMapL0c0] == -1)

            if (freeOrSameNetX(bb, xMapL0c0, globalState.connections.paths[i].net,
                               currentAllowStacking) == true) {
              // were going through each bb chip to see if it has a
              // connection to both chips free

              // if (globalState.connections.chipStates[bb].xStatus[xMapL0c1] == globalState.connections.paths[i].net ||
              //     globalState.connections.chipStates[bb].xStatus[xMapL0c1] == -1) // lanes 0 0
              if (freeOrSameNetX(bb, xMapL0c1, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true) {
                // if (globalState.connections.chipStates[bb].yStatus[0] == -1) {
                setChipXStatus(bb, xMapL0c0, globalState.connections.paths[i].net,
                               "BBtoBB alt lane0 hop chip x0");
                setChipXStatus(bb, xMapL0c1, globalState.connections.paths[i].net,
                               "BBtoBB alt lane0 hop chip x1");

                //   if (giveUpOnL == 0) {
                //     globalState.connections.chipStates[CHIP_L].yStatus[bb] = globalState.connections.paths[i].net;
                //     globalState.connections.chipStates[bb].yStatus[0] = globalState.connections.paths[i].net;
                //     globalState.connections.paths[i].y[2] = 0;
                //     globalState.connections.paths[i].y[3] = 0;
                //   } else {
                // if (debugNTCC2) {
                //     Serial.println("Gave up on L  ");
                // Serial.println(bb);
                //                  // }

                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net, "BBtoBB hop Y=0");
                //}

                globalState.connections.paths[i].sameChip = true;

                globalState.connections.paths[i].chip[2] = bb;
                globalState.connections.paths[i].chip[3] = bb;
                setPathX(i, 2, xMapL0c0);
                setPathX(i, 3, xMapL0c1);

                setChipXStatus(bb, xMapL0c0, globalState.connections.paths[i].net,
                               "BBtoBB alt lane0 hop chip x0 dup");
                setChipXStatus(bb, xMapL0c1, globalState.connections.paths[i].net,
                               "BBtoBB alt lane0 hop chip x1 dup");

                // Serial.print("!!!!3 bb: ");
                // Serial.println(bb);
                // Serial.print("chip[3]: ");
                // Serial.println(globalState.connections.paths[i].chip[3]);

                globalState.connections.paths[i].altPathNeeded = false;
                altPathResolved = true;

                setPathX(i, 0, xMapForChipLane0(globalState.connections.paths[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane0(globalState.connections.paths[i].chip[1], bb));

                setChipXStatus(globalState.connections.paths[i].chip[0],
                               xMapForChipLane0(globalState.connections.paths[i].chip[0], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt lane0 chip0");
                setChipXStatus(globalState.connections.paths[i].chip[1],
                               xMapForChipLane0(globalState.connections.paths[i].chip[1], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt lane0 chip1");

                setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                setPathY(i, 1, yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));

                setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                   "BBtoBB alt lane0 y0");
                setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                   "BBtoBB alt lane0 y1");

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print("  chip[2]: ");
                  Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(globalState.connections.paths[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(globalState.connections.paths[i].x[3]);

                  Serial.print(" \n\r");
                }
                // printPathsCompact();
                //}
                // continue;
                break;
              }
            }
            if (freeOrSameNetX(bb, xMapL1c0, globalState.connections.paths[i].net,
                               currentAllowStacking) == true) {
              if (freeOrSameNetX(bb, xMapL1c1, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true) {
                setChipXStatus(bb, xMapL1c0, globalState.connections.paths[i].net,
                               "BBtoBB alt lane1 hop x0");
                setChipXStatus(bb, xMapL1c1, globalState.connections.paths[i].net,
                               "BBtoBB alt lane1 hop x1");

                //   if (giveUpOnL == 0) {
                //     // Serial.print("Give up on L?");
                //     globalState.connections.chipStates[CHIP_L].yStatus[bb] = globalState.connections.paths[i].net;
                //     globalState.connections.chipStates[bb].yStatus[0] = globalState.connections.paths[i].net;
                //     globalState.connections.paths[i].y[2] = 0;
                //     globalState.connections.paths[i].y[3] = 0;
                //   } else {
                // if (debugNTCC2) {
                // Serial.println("Gave up on L");
                //}
                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                   "BBtoBB alt lane1 hop y0");
                /// }

                globalState.connections.paths[i].chip[2] = bb;
                globalState.connections.paths[i].chip[3] = bb;
                setPathX(i, 2, xMapL1c0);
                setPathX(i, 3, xMapL1c1);
                globalState.connections.paths[i].sameChip = true;
                globalState.connections.paths[i].altPathNeeded = false;
                altPathResolved = true;

                setChipXStatus(bb, xMapL1c0, globalState.connections.paths[i].net,
                               "BBtoBB alt lane1 hop x0 dup");
                setChipXStatus(bb, xMapL1c1, globalState.connections.paths[i].net,
                               "BBtoBB alt lane1 hop x1 dup");

                setPathX(i, 0, xMapForChipLane1(globalState.connections.paths[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane1(globalState.connections.paths[i].chip[1], bb));

                setChipXStatus(globalState.connections.paths[i].chip[0],
                               xMapForChipLane1(globalState.connections.paths[i].chip[0], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt lane1 chip0");
                setChipXStatus(globalState.connections.paths[i].chip[1],
                               xMapForChipLane1(globalState.connections.paths[i].chip[1], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt lane1 chip1");

                setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                setPathY(i, 1, yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));

                setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                   "BBtoBB alt lane1 y0");
                setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                   "BBtoBB alt lane1 y1");

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print("  chip[2]: ");
                  Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(globalState.connections.paths[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(globalState.connections.paths[i].x[3]);

                  Serial.print(" \n\r");
                }
                // continue;
                break;
              }
            }
            if (freeOrSameNetX(bb, xMapL0c0, globalState.connections.paths[i].net,
                               currentAllowStacking) == true &&
                false) {
              if (freeOrSameNetX(bb, xMapL1c1, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true) {
                //   if (giveUpOnL == 0) {
                //     globalState.connections.chipStates[CHIP_L].yStatus[bb] = globalState.connections.paths[i].net;
                //     globalState.connections.chipStates[bb].yStatus[0] = globalState.connections.paths[i].net;
                //     globalState.connections.paths[i].y[2] = 0;
                //     globalState.connections.paths[i].y[3] = 0;
                //   } else {
                if (debugNTCC2) {
                  Serial.println("Gave up on L");
                }
                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                   "BBtoBB alt mixed hop y0");
                //}

                setChipXStatus(bb, xMapL0c0, globalState.connections.paths[i].net,
                               "BBtoBB alt mixed hop x0");
                setChipXStatus(bb, xMapL1c1, globalState.connections.paths[i].net,
                               "BBtoBB alt mixed hop x1");

                globalState.connections.paths[i].chip[2] = bb;
                globalState.connections.paths[i].chip[3] = bb;
                setPathX(i, 2, xMapL0c0);
                setPathX(i, 3, xMapL1c1);

                globalState.connections.paths[i].altPathNeeded = false;
                altPathResolved = true;

                setPathX(i, 0, xMapForChipLane0(globalState.connections.paths[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane1(globalState.connections.paths[i].chip[1], bb));

                setChipXStatus(globalState.connections.paths[i].chip[0],
                               xMapForChipLane0(globalState.connections.paths[i].chip[0], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt mixed chip0");
                setChipXStatus(globalState.connections.paths[i].chip[1],
                               xMapForChipLane1(globalState.connections.paths[i].chip[1], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt mixed chip1");

                setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                setPathY(i, 1, yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));

                setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                   "BBtoBB alt mixed y0");
                setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                   "BBtoBB alt mixed y1");

                globalState.connections.paths[i].sameChip = true;
                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print("  chip[2]: ");
                  Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(globalState.connections.paths[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(globalState.connections.paths[i].x[3]);

                  Serial.print(" \n\r");
                }
                // printPathsCompact();
                //}
                // continue;
                break;
              }
            }
            if (freeOrSameNetX(bb, xMapL1c0, globalState.connections.paths[i].net,
                               currentAllowStacking) == true &&
                false) {
              if (freeOrSameNetX(bb, xMapL0c1, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true) {
                //   if (giveUpOnL == 0) {
                //     globalState.connections.chipStates[CHIP_L].yStatus[bb] = globalState.connections.paths[i].net;
                //     globalState.connections.chipStates[bb].yStatus[0] = globalState.connections.paths[i].net;
                //     globalState.connections.paths[i].y[2] = 0;
                //     globalState.connections.paths[i].y[3] = 0;
                //   } else {
                //     if (debugNTCC2) {
                //       Serial.println("Gave up on L");
                //     }
                setPathY(i, 2, 0);
                setPathY(i, 3, 0);
                setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                   "BBtoBB alt mixed2 hop y0");
                //}

                setChipXStatus(bb, xMapL0c1, globalState.connections.paths[i].net,
                               "BBtoBB alt mixed2 hop x0");
                setChipXStatus(bb, xMapL1c0, globalState.connections.paths[i].net,
                               "BBtoBB alt mixed2 hop x1");

                globalState.connections.paths[i].chip[2] = bb;
                globalState.connections.paths[i].chip[3] = bb;
                setPathX(i, 2, xMapL0c1);
                setPathX(i, 3, xMapL1c0);
                // globalState.connections.paths[i].y[2] = -2;
                // globalState.connections.paths[i].y[3] = -2;
                globalState.connections.paths[i].altPathNeeded = false;
                altPathResolved = true;
                globalState.connections.paths[i].sameChip = true;
                setPathX(i, 0, xMapForChipLane1(globalState.connections.paths[i].chip[0], bb));
                setPathX(i, 1, xMapForChipLane0(globalState.connections.paths[i].chip[1], bb));

                setChipXStatus(globalState.connections.paths[i].chip[0],
                               xMapForChipLane1(globalState.connections.paths[i].chip[0], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt mixed2 chip0");
                setChipXStatus(globalState.connections.paths[i].chip[1],
                               xMapForChipLane0(globalState.connections.paths[i].chip[1], bb),
                               globalState.connections.paths[i].net, "BBtoBB alt mixed2 chip1");

                setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                setPathY(i, 1, yMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));

                setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                   "BBtoBB alt mixed2 y0");
                setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                   "BBtoBB alt mixed2 y1");

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print(i);
                  Serial.print(" chip[2]: ");
                  Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));

                  Serial.print("  x[2]: ");
                  Serial.print(globalState.connections.paths[i].x[2]);

                  Serial.print("  x[3]: ");
                  Serial.print(globalState.connections.paths[i].x[3]);

                  Serial.print(" \n\r");
                }
                couldFindPath = i;
                // continue;
                break;
              }

              if (debugNTCC2) {
                Serial.print("\n\r");
                Serial.print(i);
                Serial.print("  chip[2]: ");
                Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));

                Serial.print("  x[2]: ");
                Serial.print(globalState.connections.paths[i].x[2]);

                Serial.print("  x[3]: ");
                Serial.print(globalState.connections.paths[i].x[3]);

                Serial.print(" \n\r");
              }
            }
            //}
          }
          // continue;
          break;
        }

        case BBtoNANO: {
          if (debugNTCC2) {
            Serial.println("   BBtoNANO");
          }
          
          // Debug output removed for production
          
                     // Add detailed debug for failed BBtoNANO paths
          if (debugNTCC2 && (globalState.connections.paths[i].node1 == 5 || globalState.connections.paths[i].node1 == 6 || globalState.connections.paths[i].node1 == 33)) {
            Serial.print("   Debugging BBtoNANO globalState.connections.paths[");
            Serial.print(i);
            Serial.print("]: ");
            printNodeOrName(globalState.connections.paths[i].node1);
            Serial.print(" (chip ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));
            Serial.print(") -> ");
            printNodeOrName(globalState.connections.paths[i].node2);
            Serial.print(" (chip ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));
            Serial.print(") - altPath=");
            Serial.println(globalState.connections.paths[i].altPathNeeded);
          }
          
          int foundHop = 0;
          
          // For BB to NANO connections, we need to route through an intermediate breadboard chip
          // We have: breadboard pin -> intermediate BB chip -> nano pin
          
                     // Try breadboard chips first (0-7), then special function chips (8-11)
           for (int intermediateChip = 0; intermediateChip < 12; intermediateChip++) {
             // Skip if this is the same as our source or target chip
             if (intermediateChip == globalState.connections.paths[i].chip[0] || intermediateChip == globalState.connections.paths[i].chip[1]) {
               continue;
             }
             
             // Debug output removed for production
             
             int xMapSourceToIntermediate = -1;
             int xMapIntermediateToSource = -1;
             int xMapIntermediateToNano = -1;
             int xMapSourceFromIntermediate = -1;  // Return connection from intermediate back to source
             int intermediateY = -1;  // Y position for SF intermediate chips
             
             if (intermediateChip < 8) {
               // Breadboard chip as intermediate
               // Check if this intermediate chip has free Y0 and connections to both endpoints
               if (!freeOrSameNetY(intermediateChip, 0, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue;
               }
               
               // Check connection from source breadboard chip to intermediate chip
               xMapSourceToIntermediate = xMapForChipLane0(globalState.connections.paths[i].chip[0], intermediateChip);
               if (xMapSourceToIntermediate == -1 || !freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapSourceToIntermediate, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue;
               }
               
               // Check connection from intermediate chip back to source
               xMapIntermediateToSource = xMapForChipLane0(intermediateChip, globalState.connections.paths[i].chip[0]);
               if (xMapIntermediateToSource == -1 || !freeOrSameNetX(intermediateChip, xMapIntermediateToSource, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue;
               }
               
               // CRITICAL: Also check if source chip has free X position for return connection
               if (intermediateChip < 8) {
                 // For breadboard intermediate chips, this is direct connection
                 xMapSourceFromIntermediate = xMapForChipLane0(globalState.connections.paths[i].chip[0], intermediateChip);
               } else {
                 // For SF intermediate chips, need to find the connection from source BB chip to the intermediate Y position
                 xMapSourceFromIntermediate = xMapForChipLane0(globalState.connections.paths[i].chip[0], intermediateY);
               }
               
               if (xMapSourceFromIntermediate == -1 || !freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapSourceFromIntermediate, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue;
               }
               
               // Check connection from intermediate chip to nano chip
               xMapIntermediateToNano = xMapForNode(globalState.connections.paths[i].chip[1], intermediateChip);
               if (xMapIntermediateToNano == -1 || !freeOrSameNetX(intermediateChip, xMapIntermediateToNano, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue;
               }
             } else {
               // Special function chip as intermediate (chips I, J, K, L)
               // Find Y position for intermediate SF chip
               intermediateY = -1;
               for (int testY = 0; testY < 8; testY++) {
                 if (freeOrSameNetY(intermediateChip, testY, globalState.connections.paths[i].net, currentAllowStacking)) {
                   // For SF chips, also check if the corresponding BB chip X is free
                   int bbChipX = xMapForChipLane0(testY, intermediateChip);
                   if (bbChipX != -1 && freeOrSameNetX(testY, bbChipX, globalState.connections.paths[i].net, currentAllowStacking)) {
                     intermediateY = testY;
                     break;
                   }
                 }
               }
               
               if (intermediateY == -1) {
                 continue; // No free Y position on intermediate SF chip
               }
               
               // Check connection from source breadboard chip to intermediate SF chip
               xMapSourceToIntermediate = xMapForChipLane0(globalState.connections.paths[i].chip[0], intermediateY);
               if (xMapSourceToIntermediate == -1 || !freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapSourceToIntermediate, globalState.connections.paths[i].net, currentAllowStacking)) {
                 continue;
               }
               
               // For SF chips, the return connection goes back to the BB chip at intermediateY position
               // Find the X position on the SF chip that connects to that BB chip
               xMapIntermediateToSource = xMapForChipLane0(intermediateChip, intermediateY);
               if (xMapIntermediateToSource == -1 || !freeOrSameNetX(intermediateChip, xMapIntermediateToSource, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue;
               }
               
               // Check ijkl connection from intermediate SF chip to target nano chip
               for (int ijklX = 12; ijklX <= 14; ijklX++) {
                 if (globalState.connections.chipStates[intermediateChip].xMap[ijklX] == globalState.connections.paths[i].chip[1]) {
                   xMapIntermediateToNano = ijklX;
                   break;
                 }
               }
               
               if (xMapIntermediateToNano == -1 || !freeOrSameNetX(intermediateChip, xMapIntermediateToNano, globalState.connections.paths[i].net, currentAllowStacking)) {
                 continue;
               }
               
               // Check the corresponding ijkl connection on the target nano chip
               int targetIjklX = -1;
               for (int ijklX = 12; ijklX <= 14; ijklX++) {
                 if (globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xMap[ijklX] == intermediateChip) {
                   targetIjklX = ijklX;
                   break;
                 }
               }
               
               if (targetIjklX == -1 || !freeOrSameNetX(globalState.connections.paths[i].chip[1], targetIjklX, globalState.connections.paths[i].net, currentAllowStacking)) {
                 continue;
               }
               
               // Store the intermediate Y for later use - don't reuse xMapIntermediateToSource
               // intermediateY is already stored in the variable above
             }
            
                         // For BBtoNANO routing, the nano Y position must correspond to the intermediate chip
             int nanoY = -1;
             if (intermediateChip < 8) {
               // For breadboard intermediate chips, nano Y must be the intermediate chip number
               nanoY = intermediateChip;
               if (!freeOrSameNetY(globalState.connections.paths[i].chip[1], nanoY, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue; // Required Y position not available
               }
             } else {
               // For SF intermediate chips, nano Y corresponds to the BB chip at intermediateY
               nanoY = intermediateY;
               if (!freeOrSameNetY(globalState.connections.paths[i].chip[1], nanoY, globalState.connections.paths[i].net, currentAllowStacking)) {
                 // Debug output removed for production
                 continue; // Required Y position not available
               }
             }
             
             // We found a valid intermediate chip - set up the routing
             // Debug output removed for production
             if (debugNTCC2) {
               Serial.print("Found BB to NANO hop through chip ");
               Serial.print(chipNumToChar(intermediateChip));
               Serial.print(" for path ");
               Serial.print(i);
               Serial.print(" using correct nano Y=");
               Serial.print(nanoY);
               Serial.print(" (corresponds to chip ");
               Serial.print(chipNumToChar(intermediateChip < 8 ? intermediateChip : intermediateY));
               Serial.println(")");
             }
             
             // Save state before making any assignments
             saveRoutingState(i);
             
             // Set up the 4-segment path: source BB -> intermediate chip -> intermediate chip -> nano
             globalState.connections.paths[i].chip[2] = intermediateChip;
             globalState.connections.paths[i].chip[3] = intermediateChip;
             
             // X coordinates
             bool pathX0Success = setPathX(i, 0, xMapSourceToIntermediate);
             bool pathX1Success = setPathX(i, 1, xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));
             bool pathX2Success = setPathX(i, 2, xMapIntermediateToSource);
             bool pathX3Success = setPathX(i, 3, xMapIntermediateToNano);
             
             // Y coordinates - different for BB vs SF intermediate chips
             bool pathY0Success = setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
             bool pathY1Success = setPathY(i, 1, nanoY);  // Use the found free Y position
             bool pathY2Success = false;
             bool pathY3Success = false;
             if (intermediateChip < 8) {
               // Breadboard intermediate chip
               pathY2Success = setPathY(i, 2, 0);   // Intermediate BB chip Y=0
               pathY3Success = setPathY(i, 3, 0);   // Intermediate BB chip Y=0
             } else {
               // Special function intermediate chip
               pathY2Success = setPathY(i, 2, intermediateY);  // SF chip Y position
               pathY3Success = setPathY(i, 3, intermediateY);  // SF chip Y position
             }
             
             // Update chip status - validate all assignments first
             bool chipX0Success = setChipXStatus(globalState.connections.paths[i].chip[0], xMapSourceToIntermediate, globalState.connections.paths[i].net, "BBtoNANO alt source->intermediate");
             bool chipX1Success = setChipXStatus(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].x[1], globalState.connections.paths[i].net, "BBtoNANO alt nano x");
             bool chipX2Success = setChipXStatus(intermediateChip, xMapIntermediateToSource, globalState.connections.paths[i].net, "BBtoNANO alt intermediate->source");
             bool chipX3Success = setChipXStatus(intermediateChip, xMapIntermediateToNano, globalState.connections.paths[i].net, "BBtoNANO alt intermediate->nano");
             
             // CRITICAL: Also assign the return connection on source chip
             bool chipX0ReturnSuccess = setChipXStatus(globalState.connections.paths[i].chip[0], xMapSourceFromIntermediate, globalState.connections.paths[i].net, "BBtoNANO alt source return X");
             
             bool chipY0Success = setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net, "BBtoNANO alt source y");
             bool chipY1Success = setChipYStatusSafe(globalState.connections.paths[i].chip[1], nanoY, globalState.connections.paths[i].net, "BBtoNANO alt nano y");
             bool chipY2Success = false;
             bool chipBBXSuccess = true;  // For SF intermediate chips
             
             if (intermediateChip < 8) {
               chipY2Success = setChipYStatusSafe(intermediateChip, 0, globalState.connections.paths[i].net, "BBtoNANO alt hop y");
             } else {
               chipY2Success = setChipYStatusSafe(intermediateChip, intermediateY, globalState.connections.paths[i].net, "BBtoNANO alt SF hop y");
               // Also update the corresponding breadboard chip X status for SF chips
               int bbChipX = xMapForChipLane0(intermediateY, intermediateChip);
               if (bbChipX != -1) {
                 chipBBXSuccess = setChipXStatus(intermediateY, bbChipX, globalState.connections.paths[i].net, "BBtoNANO alt SF BB connection");
               }
             }
             
             // Check if ALL assignments succeeded
             bool allAssignmentsSuccessful = pathX0Success && pathX1Success && pathX2Success && pathX3Success &&
                                           pathY0Success && pathY1Success && pathY2Success && pathY3Success &&
                                           chipX0Success && chipX1Success && chipX2Success && chipX3Success &&
                                           chipX0ReturnSuccess && chipY0Success && chipY1Success && chipY2Success && chipBBXSuccess;
             
             if (allAssignmentsSuccessful) {
               commitRoutingState();
               globalState.connections.paths[i].altPathNeeded = false;
               altPathResolved = true;
               foundHop = 1;
               if (debugNTCC6) {
                 Serial.print("BBtoNANO alt globalState.connections.paths[");
                 Serial.print(i);
                 Serial.println("] assignments committed successfully");
               }
             } else {
               restoreRoutingState(i);
               // Debug output removed for production
               if (debugNTCC6) {
                 Serial.print("BBtoNANO alt assignment failed for globalState.connections.paths[");
                 Serial.print(i);
                 Serial.println("], state restored, trying next intermediate chip");
               }
               continue;  // Try next intermediate chip
             }
            
                         if (debugNTCC2) {
               Serial.print("BBtoNANO alt path resolved: ");
               printNodeOrName(globalState.connections.paths[i].node1);
               Serial.print(" -> chip ");
               Serial.print(chipNumToChar(intermediateChip));
               Serial.print(" -> ");
               printNodeOrName(globalState.connections.paths[i].node2);
               Serial.print(" (nano Y=");
               Serial.print(nanoY);
               Serial.print(" CORRECTED for chip ");
               Serial.print(chipNumToChar(intermediateChip < 8 ? intermediateChip : intermediateY));
               Serial.println(")");
             }
            
            break;
          }
          
          // If we get here, no intermediate chip worked
          // Debug output removed for production
          break;
        }

        case NANOtoSF:
        case NANOtoNANO: {
          if (debugNTCC2) {
            Serial.println("   NANOtoNANO");
          }
          int foundHop = 0;
          int giveUpOnL = 0;
          int swapped = 0;
          // duplicateSFnets();
          ////printPathsCompact();
          // printChipStatus();
          giveUpOnL = 0;

          // Serial.print(i);
          // Serial.println("   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

          for (int bb = 0; bb < 8; bb++) // this is a long winded way to do this
                                         // but it's at least slightly readable
          {
            int sfChip1 = globalState.connections.paths[i].chip[0];
            int sfChip2 = globalState.connections.paths[i].chip[1];

            int chip1Lane = xMapForNode(sfChip1, bb);
            int chip2Lane = xMapForNode(sfChip2, bb);

            if (bb == 7 && giveUpOnL == 0 && swapped == 0) {
              bb = 0;
              giveUpOnL = 0;
              swapped = 1;
              // swapDuplicateNode(i);
            } else if (bb == 7 && giveUpOnL == 0 && swapped == 1) {
              // bb = 0;
              giveUpOnL = 1;
            }

            // if ((globalState.connections.chipStates[CHIP_L].yStatus[bb] != -1 &&
            //      globalState.connections.chipStates[CHIP_L].yStatus[bb] != globalState.connections.paths[i].net) &&
            //     giveUpOnL == 0) {

            //   continue;
            // }
            // if ((globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].yStatus[bb] != -1 &&
            //      globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].yStatus[bb] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].yStatus[bb] != -1 &&
            //      globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].yStatus[bb] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[bb].xStatus[chip1Lane] != -1 &&
            //      globalState.connections.chipStates[bb].xStatus[chip1Lane] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[bb].xStatus[chip2Lane] != -1 &&
            //      globalState.connections.chipStates[bb].xStatus[chip2Lane] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[bb].yStatus[0] != -1 && globalState.connections.chipStates[bb].yStatus[0] !=
            //     globalState.connections.paths[i].net))
            //     {

            if (freeOrSameNetY(globalState.connections.paths[i].chip[0], bb, globalState.connections.paths[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetY(globalState.connections.paths[i].chip[1], bb, globalState.connections.paths[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetX(bb, chip1Lane, globalState.connections.paths[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetX(bb, chip2Lane, globalState.connections.paths[i].net,
                               currentAllowStacking) == false ||
                freeOrSameNetY(bb, 0, globalState.connections.paths[i].net, currentAllowStacking) ==
                    false) {
              //                 Serial.print("bb:\t");
              // Serial.println(bb);
              // Serial.print("xStatus:\t");
              // Serial.println(globalState.connections.chipStates[bb].xStatus[chip1Lane]);
              // Serial.print("xStatus:\t");
              // Serial.println(globalState.connections.chipStates[bb].xStatus[chip2Lane]);
              // Serial.println(" ");
              // Serial.print("path: ");
              // Serial.println(i);
              //    printPathsCompact();
              //   printChipStatus();
              //   Serial.print("!!!!!!!!!!!!!!!!!!!!!!!!\n\r");
              // continue;
            } else {
              // if (globalState.connections.chipStates[bb].xStatus[chip1Lane] == globalState.connections.paths[i].net ||
              //     globalState.connections.chipStates[bb].xStatus[chip1Lane] == -1) {
              if (freeOrSameNetX(bb, chip1Lane, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true) {
                // if (globalState.connections.chipStates[bb].xStatus[chip2Lane] == globalState.connections.paths[i].net ||
                //     globalState.connections.chipStates[bb].xStatus[chip2Lane] == -1) {
                if (freeOrSameNetX(bb, chip2Lane, globalState.connections.paths[i].net,
                                   currentAllowStacking) == true) {
                  // printPathsCompact();
                  // printChipStatus();

                  if (giveUpOnL == 1) {
                    if (debugNTCC2) {
                      // Serial.println("Gave up on L");
                      // Serial.print("path :");
                      // Serial.println(i);
                    }
                    continue;
                    // break;
                  }

                  globalState.connections.paths[i].sameChip = true;

                  setChipXStatus(bb, chip1Lane, globalState.connections.paths[i].net,
                                 "NANOtoNANO alt hop x0");
                  setChipXStatus(bb, chip2Lane, globalState.connections.paths[i].net,
                                 "NANOtoNANO alt hop x1");

                  if (globalState.connections.paths[i].chip[0] != globalState.connections.paths[i].chip[1]) {
                    //                 Serial.println("VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV");
                    //                 int pathAddress = (int)&globalState.connections.paths[i];
                    // Serial.print(pathAddress);
                    // Serial.print("\tpath: ");
                    // Serial.println(i);
                    // Serial.print("bb:\t");
                    // Serial.print(bb);

                    globalState.connections.paths[i].chip[2] = bb;
                    globalState.connections.paths[i].chip[3] = bb;
                    setPathY(i, 2, 0);
                    setPathY(i, 3, 0);
                    setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                       "NANOtoNANO alt hop y0");

                    setPathX(i, 2, chip1Lane);
                    setPathX(i, 3, chip2Lane);
                  }

                  globalState.connections.paths[i].altPathNeeded = false;
                  altPathResolved = true;

                  setPathX(i, 0, xMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                  setPathX(i, 1, xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));
                  setChipXStatus(globalState.connections.paths[i].chip[0],
                                 xMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]),
                                 globalState.connections.paths[i].net, "NANOtoNANO alt chip0");
                  setChipXStatus(globalState.connections.paths[i].chip[1],
                                 xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]),
                                 globalState.connections.paths[i].net, "NANOtoNANO alt chip1");

                  setPathY(i, 0, bb);
                  setPathY(i, 1, bb);

                  //            Serial.print(">>>> path ");
                  // Serial.println(i);

                  setChipYStatusSafe(globalState.connections.paths[i].chip[0], bb, globalState.connections.paths[i].net,
                                     "NANOtoNANO alt y0");
                  setChipYStatusSafe(globalState.connections.paths[i].chip[1], bb, globalState.connections.paths[i].net,
                                     "NANOtoNANO alt y1");

                  if (debugNTCC2) {
                    Serial.println("\n\r");
                    Serial.print(i);
                    Serial.print("  chip[2]: ");
                    Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));

                    Serial.print("  y[2]: ");
                    Serial.print(globalState.connections.paths[i].y[2]);

                    Serial.print("  y[3]: ");
                    Serial.print(globalState.connections.paths[i].y[3]);

                    Serial.println(" \n\r");
                  }

                  foundHop = 1;
                  couldFindPath = i;
                  // printPathsCompact();
                  // printChipStatus();
                  // Serial.println("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
                  //  printPathsCompact();
                  //  printChipStatus();

                  // Serial.print("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
                  // continue;
                  break;
                }
              }
            }
          }

          for (int bb = 0; bb < 8;
               bb++) // this will connect to a random breadboard row, add a
          // test to make sure nothing is connected
          {
            int sfChip1 = globalState.connections.paths[i].chip[0];
            int sfChip2 = globalState.connections.paths[i].chip[1];

            int chip1Lane = xMapForNode(sfChip1, bb);
            int chip2Lane = xMapForNode(sfChip2, bb);

            // Serial.print("bb:\t");
            // Serial.println(bb);
            // Serial.print("xStatus:\t");
            // Serial.println(globalState.connections.chipStates[bb].xStatus[chip1Lane]);
            // Serial.print("xStatus:\t");
            // Serial.println(globalState.connections.chipStates[bb].xStatus[chip2Lane]);
            // Serial.println(" ");
            // Serial.print("path: ");
            // Serial.println(i);
            // if ((globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].yStatus[bb] != -1 &&
            //      globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].yStatus[bb] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].yStatus[bb] != -1 &&
            //      globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].yStatus[bb] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[bb].xStatus[chip1Lane] != -1 &&
            //      globalState.connections.chipStates[bb].xStatus[chip1Lane] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[bb].xStatus[chip2Lane] != -1 &&
            //      globalState.connections.chipStates[bb].xStatus[chip2Lane] != globalState.connections.paths[i].net) ||
            //     (globalState.connections.chipStates[bb].yStatus[0] != -1 && globalState.connections.chipStates[bb].yStatus[0] !=
            //     globalState.connections.paths[i].net))
            //     {

            if (freeOrSameNetY(globalState.connections.paths[i].chip[0], bb, globalState.connections.paths[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetY(globalState.connections.paths[i].chip[1], bb, globalState.connections.paths[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetX(bb, chip1Lane, globalState.connections.paths[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetX(bb, chip2Lane, globalState.connections.paths[i].net,
                               currentAllowStacking) == true ||
                freeOrSameNetY(bb, 0, globalState.connections.paths[i].net, currentAllowStacking) ==
                    true) {
              //                 Serial.print("bb:\t");
              // Serial.println(bb);
              // Serial.print("xStatus:\t");
              // Serial.println(globalState.connections.chipStates[bb].xStatus[chip1Lane]);
              // Serial.print("xStatus:\t");
              // Serial.println(globalState.connections.chipStates[bb].xStatus[chip2Lane]);
              // Serial.println(" ");
              // Serial.print("path: ");
              // Serial.println(i);
              //    printPathsCompact();
              //   printChipStatus();
              //   Serial.print("?????????????????????\n\r");
              //   // continue;
            } else {
              // Serial.print("?????????????????????\n\r");
              // if ((globalState.connections.chipStates[bb].xStatus[chip1Lane] == globalState.connections.paths[i].net ||
              //      globalState.connections.chipStates[bb].xStatus[chip1Lane] == -1) &&
              //     foundHop == 0) {
              if (freeOrSameNetX(bb, chip1Lane, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true &&
                  foundHop == 0) {
                if (freeOrSameNetX(bb, chip2Lane, globalState.connections.paths[i].net,
                                   currentAllowStacking) == true) {
                  // Serial.print("path :");
                  // Serial.println(i);
                  //  printPathsCompact();
                  setChipXStatus(bb, chip1Lane, globalState.connections.paths[i].net,
                                 "NANOtoNANO random bb hop x0");
                  setChipXStatus(bb, chip2Lane, globalState.connections.paths[i].net,
                                 "NANOtoNANO random bb hop x1");
                  setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                     "NANOtoNANO random bb hop y0");
                  if (globalState.connections.paths[i].chip[0] !=
                      globalState.connections.paths[i].chip[1]) // this makes it not try to find a third
                  // chip if it doesn't need to
                  {
                    globalState.connections.paths[i].chip[2] = bb;
                    setPathX(i, 2, chip1Lane);
                    setPathX(i, 3, chip2Lane);

                    setPathY(i, 2, 0);
                    setPathY(i, 3, 0);
                  }

                  globalState.connections.paths[i].sameChip = true;
                  globalState.connections.paths[i].altPathNeeded = false;
                  altPathResolved = true;

                  setPathX(i, 0, xMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                  setPathX(i, 1, xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]));
                  setChipXStatus(globalState.connections.paths[i].chip[0],
                                 xMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]),
                                 globalState.connections.paths[i].net, "NANOtoNANO random bb chip0");
                  setChipXStatus(globalState.connections.paths[i].chip[1],
                                 xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]),
                                 globalState.connections.paths[i].net, "NANOtoNANO random bb chip1");
                  // Serial.print(">>>> path ");
                  // Serial.println(i);

                  setPathY(i, 0, bb);
                  setPathY(i, 1, bb);
                  setChipYStatusSafe(globalState.connections.paths[i].chip[0], bb, globalState.connections.paths[i].net,
                                     "NANOtoNANO random bb y0");
                  setChipYStatusSafe(globalState.connections.paths[i].chip[1], bb, globalState.connections.paths[i].net,
                                     "NANOtoNANO random bb y1");

                  if (debugNTCC2) {
                    Serial.print("\n\r");
                    Serial.print(i);
                    Serial.print("  chip[2]: ");
                    Serial.print(chipNumToChar(globalState.connections.paths[i].chip[2]));
                    // Serial.print("  y[2]: ");

                    Serial.print("  y[2]: ");
                    Serial.print(globalState.connections.paths[i].y[2]);

                    Serial.print("  y[3]: ");
                    Serial.print(globalState.connections.paths[i].y[3]);

                    Serial.print(" \n\r");
                  }
                  foundHop = 1;
                  couldFindPath = i;
                  // printPathsCompact();
                  // printChipStatus();
                  // continue;
                  // printPathsCompact();
                  // printChipStatus();
                  break;
                }
              }
            }
          }
          // couldntFindPath(i);
        }
        case BBtoSF: {
          // Serial.print("globalState.connections.paths[");
          // Serial.print(i);
          // Serial.println("] ");

          // duplicateSFnets();
          if (globalState.connections.paths[i].pathType == BBtoSF ||
              globalState.connections.paths[i].pathType == BBtoNANO) // do bb to sf first because these
          // are hardest to find
          {
            int foundPath = 0;

            if (debugNTCC2) {
              Serial.print("\n\rBBtoSF\tpath: ");
              Serial.println(i);
            }

            // Every breadboard chip is a candidate bounce (its NC Y0 row joins
            // the lane from chip[0] to its lane into the SF chip). The upper
            // bound used to be 8 - chipStates[chip[1]].uncommittedHops, which
            // counts SF-to-SF paths that borrowed a Y row on the SF chip: with
            // the probe feed (BUF_IN -> GP_8) sitting on L that skipped chip H
            // outright, and H was the only chip whose L row (L.Y7) was still
            // free, so 7 -> GP_2 failed with A's own L row taken by GP_1
            // (2026-09-02). The row the SF-to-SF path actually took is already
            // refused by freeOrSameNetY(sfChip, bb, ...) below; the OG router
            // has always looped to 8.
            for (int bb = 0; bb < 8;
                 bb++) // check if any other chips have free paths to both the
            // sf chip and target chip
            {
              // tryAfterSwap:

              if (foundPath == 1) {
                if (debugNTCC2) {
                  Serial.print("Found Path!\n\r");
                }
                couldFindPath = i;

                break;
              }

              int sfChip = globalState.connections.paths[i].chip[1];

              // The bounce chip's OWN lane into the SF chip: the pin that
              // closes on bb.Y0 and lands on sfChip.Y[bb]. This used to be
              // xMapForChipLane0(chip[0], bb) - chip[0]'s pin toward bb, used
              // as an index on bb - so the availability check below tested an
              // unrelated pin of bb (its I lane, mostly), the claim marked
              // that unrelated pin busy, and the lane the path really used
              // (x[3], recomputed correctly further down) was never claimed.
              // No hardware short came of it only because sfChip.Y[bb], the
              // other end of the same wire, was claimed. Found by
              // test_routing_dense (2026-09-03): UNCLAIMED WIRE / CLAIM
              // WITHOUT COPPER on every bounced BB-to-SF path.
              int xMapBB = xMapForChipLane0(bb, sfChip);
              if (xMapBB == -1 || xMapForChipLane0(globalState.connections.paths[i].chip[0], bb) == -1) {
                continue; // no such lane pair: bb cannot bounce for this path
              }
              // if (xMapForChipLane1(globalState.connections.paths[i].chip[0], bb) == -1)
              // {
              //     //Serial.print("xMapForChipLane1(globalState.connections.paths[i].chip[0], bb) !=
              //     -1");

              //     continue; // don't bother checking if there's no connection
              // }
              // Serial.print("           fuck         ");
              int yMapSF = bb; // always

              // not chip L
              if (debugNTCC2) {
                Serial.print("\n\r");
                Serial.print("      bb: ");
                printChipNumToChar(bb);
                Serial.print("  \t  sfChip: ");
                printChipNumToChar(sfChip);
                Serial.print("  \t  xMapBB: ");
                Serial.print(xMapBB);
                Serial.print("  \t  yMapSF: ");
                Serial.print(yMapSF);
                Serial.print("  \t  xStatus: ");
                Serial.print(globalState.connections.chipStates[bb].xStatus[xMapBB]);
                Serial.print("  \n\r");
              }

              if (freeOrSameNetX(bb, xMapBB, globalState.connections.paths[i].net,
                                 currentAllowStacking) == true &&
                  freeOrSameNetY(bb, 0, globalState.connections.paths[i].net, currentAllowStacking) ==
                      true) {
                // were going through each bb chip to see if it has a
                // connection to both chips free

                int xMapL0c0 = xMapForChipLane0(globalState.connections.paths[i].chip[0], bb);
                int xMapL1c0 = xMapForChipLane1(globalState.connections.paths[i].chip[0], bb);

                int xMapL0c1 = xMapForChipLane0(bb, globalState.connections.paths[i].chip[0]);
                int xMapL1c1 = xMapForChipLane1(bb, globalState.connections.paths[i].chip[0]);

                if (debugNTCC2) {
                  Serial.print("\n\r");
                  Serial.print("      bb: ");
                  printChipNumToChar(bb);
                  Serial.print("  \t  sfChip: ");
                  printChipNumToChar(sfChip);
                  Serial.print("  \t  xMapBB: ");
                  Serial.print(xMapBB);
                  Serial.print("  \t  yMapSF: ");
                  Serial.print(yMapSF);
                  Serial.print("  \t  xStatus: ");
                  Serial.print(globalState.connections.chipStates[bb].xStatus[xMapBB]);
                  Serial.print("  \n\r");

                  Serial.print("xMapL0c0: ");
                  Serial.print(xMapL0c0);
                  Serial.print("  \txMapL1c0: ");

                  Serial.print(xMapL0c1);
                  Serial.print("  \txMapL1c1: ");

                  Serial.print(xMapL1c0);
                  Serial.print("  \txMapL0c1: ");
                  Serial.print(xMapL1c1);
                  Serial.print("\n\n\r");
                }
                int freeLane = -1;
                // Serial.print("\t");
                // Serial.print(bb);

                // if ((xMapL1c0 != -1) &&
                //     globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL1c0] ==
                //         globalState.connections.paths[i].net) // check if lane 1 shares a net first so
                //         it
                //                      // should prefer sharing lanes
                // {
                //   freeLane = 1;
                //} else

                // if ((globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL0c0] == -1) ||
                //            globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL0c0] ==
                //                globalState.connections.paths[i].net) // lanes will alway be taken
                //                together,
                //                             // so only check chip 1
                // {
                // a lane is free only when BOTH its pins are (they are
                // always claimed together, but a check on one end only
                // trusts that every claimant got it right)
                if (freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapL0c0, globalState.connections.paths[i].net,
                                   currentAllowStacking) == true &&
                    freeOrSameNetX(bb, xMapL0c1, globalState.connections.paths[i].net,
                                   currentAllowStacking) == true) {
                  freeLane = 0;
                } else if (freeOrSameNetX(globalState.connections.paths[i].chip[0], xMapL1c0,
                                          globalState.connections.paths[i].net,
                                          currentAllowStacking) == true &&
                           freeOrSameNetX(bb, xMapL1c1, globalState.connections.paths[i].net,
                                          currentAllowStacking) == true) {
                  freeLane = 1;
                } else {
                  continue;
                }

                // if (globalState.connections.chipStates[sfChip].yStatus[yMapSF] != -1 &&
                //     globalState.connections.chipStates[sfChip].yStatus[yMapSF] != globalState.connections.paths[i].net) {
                if (freeOrSameNetY(sfChip, yMapSF, globalState.connections.paths[i].net,
                                   currentAllowStacking) == false) {
                  continue;
                }

                globalState.connections.paths[i].altPathNeeded = false;
                altPathResolved = true;

                int SFnode = xMapForNode(globalState.connections.paths[i].node2, globalState.connections.paths[i].chip[1]);
                // Serial.print("\n\r\t\t\t\tSFnode: ");
                // Serial.println(SFnode);
                // Serial.print("\n\r\t\t\t\tFree Lane: ");
                // Serial.println(freeLane);

                if (freeLane == 0) {
                  globalState.connections.paths[i].chip[2] = bb;
                  globalState.connections.paths[i].chip[3] = bb;
                  setChipXStatus(globalState.connections.paths[i].chip[0], xMapL0c0, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane0 chip0");
                  setChipXStatus(globalState.connections.paths[i].chip[1], SFnode, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane0 chip1");

                  setChipXStatus(globalState.connections.paths[i].chip[2], xMapL0c1, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane0 hop x0");
                  setChipXStatus(globalState.connections.paths[i].chip[2], xMapBB, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane0 hop x1");

                  setPathX(i, 0, xMapL0c0);
                  setPathX(i, 1, SFnode);

                  setPathX(i, 2, xMapL0c1);
                  globalState.connections.paths[i].chip[3] = globalState.connections.paths[i].chip[2];
                  setPathX(i, 3, xMapBB);

                  setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                  setPathY(i, 1, yMapSF);
                  setPathY(i, 2, 0);
                  setPathY(i, 3, 0);
                  setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                     "BBtoSF alt lane0 bb y0");

                  setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                     "BBtoSF alt lane0 chip0 y0");

                  setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                     "BBtoSF alt lane0 chip1 y1");
                  setChipYStatusSafe(globalState.connections.paths[i].chip[2], 0, globalState.connections.paths[i].net,
                                     "BBtoSF alt lane0 hop y0");

                  globalState.connections.paths[i].sameChip = true;
                } else if (freeLane == 1) {
                  globalState.connections.paths[i].chip[2] = bb;
                  globalState.connections.paths[i].chip[3] = bb;
                  setChipXStatus(globalState.connections.paths[i].chip[0], xMapL1c0, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane1 chip0");
                  setChipXStatus(globalState.connections.paths[i].chip[1], SFnode, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane1 chip1");

                  setChipXStatus(globalState.connections.paths[i].chip[2], xMapL1c1, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane1 hop x0");
                  setChipXStatus(globalState.connections.paths[i].chip[2], xMapBB, globalState.connections.paths[i].net,
                                 "BBtoSF alt lane1 hop x1");

                  setPathX(i, 0, xMapL1c0);
                  setPathX(i, 1, SFnode);

                  setPathX(i, 2, xMapL1c1);
                  setPathX(i, 3, xMapBB);

                  globalState.connections.paths[i].chip[3] = globalState.connections.paths[i].chip[2];

                  setPathY(i, 0, yMapForNode(globalState.connections.paths[i].node1, globalState.connections.paths[i].chip[0]));
                  setPathY(i, 1, yMapSF);
                  setPathY(i, 2, 0);
                  setPathY(i, 3, 0);
                  setChipYStatusSafe(bb, 0, globalState.connections.paths[i].net,
                                     "BBtoSF alt lane1 bb y0");

                  setChipYStatusSafe(globalState.connections.paths[i].chip[0], globalState.connections.paths[i].y[0], globalState.connections.paths[i].net,
                                     "BBtoSF alt lane1 chip0 y0");

                  setChipYStatusSafe(globalState.connections.paths[i].chip[1], globalState.connections.paths[i].y[1], globalState.connections.paths[i].net,
                                     "BBtoSF alt lane1 chip1 y1");
                  setChipYStatusSafe(globalState.connections.paths[i].chip[2], 0, globalState.connections.paths[i].net,
                                     "BBtoSF alt lane1 hop y0");
                }

                foundPath = 1;
                couldFindPath = i;

                if (debugNTCC2 == true) {
                  Serial.print("\n\rFreelane = ");
                  Serial.print(freeLane);
                  Serial.print("\t path: ");
                  Serial.print(i);
                  Serial.print(" \n\rchip[0]: ");
                  Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));

                  Serial.print("  x[0]: ");
                  Serial.print(globalState.connections.paths[i].x[0]);

                  Serial.print("  y[0]: ");
                  Serial.print(globalState.connections.paths[i].y[0]);

                  Serial.print("\t  chip[1]: ");
                  Serial.print(chipNumToChar(globalState.connections.paths[i].chip[1]));

                  Serial.print("  x[1]: ");
                  Serial.print(globalState.connections.paths[i].x[1]);

                  Serial.print("  y[1]: ");
                  Serial.print(globalState.connections.paths[i].y[1]);

                  Serial.print("   globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[");
                  Serial.print(xMapL0c0);
                  Serial.print("]: ");
                  Serial.print(globalState.connections.chipStates[globalState.connections.paths[i].chip[0]].xStatus[xMapL0c0]);

                  Serial.print("   globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xStatus[ ");
                  Serial.print(xMapL0c1);
                  Serial.print("]: ");
                  Serial.print(globalState.connections.chipStates[globalState.connections.paths[i].chip[1]].xStatus[xMapL0c1]);
                  Serial.print(" \t\n\r");
                  // printChipStatus();
                }
                // break;
              }

              // if (foundPath == 0 && swapped == 0 && bb == 7) {
              //   //swapped = 1;
              //   // if (debugNTCC2 == true)
              //   //  Serial.print("\n\rtrying again with swapped nodes\n\r");

              //   // globalState.connections.paths[i].x[0] = xMapForNode(globalState.connections.paths[i].node2,
              //   globalState.connections.paths[i].chip[0]);
              //   // swapDuplicateNode(i);
              //   bb = 0;
              //   // goto tryAfterSwap;
              // }
            }
          }
          break;
        }

        default:
          // Handle unhandled path types (BBtoBBL, NANOtoBBL, SFtoSF, etc.)
          break;
          // break;
        } // end stacking attempt loop
      }
    }
  }
  routingDuplicatePathNow = false;
}

bool freeOrSameNetX(int chip, int x, int net, int allowStacking) {
  if (chip < 0 || chip >= 12 || x < 0 || x >= 16) return false;   // a -1 lane from xMapForChipLane1 indexed the status array at -1
  if (reservedTapEscapeX(chip, x)) {
    return false;
  }
  // Serial.print("freeOrSameNetX: ");
  // Serial.print(chip);
  // Serial.print(", ");
  // Serial.print(x);
  // Serial.print(", ");
  // Serial.print(net);
  // Serial.print(", ");
  // Serial.print(allowStacking);
  // Serial.print(" = ");
  if (globalState.connections.chipStates[chip].xStatus[x] == -1 ||
      (globalState.connections.chipStates[chip].xStatus[x] == net && allowStacking == 1)) {
    // Serial.println("true");
    return true;
  } else {
    // Serial.println("false");
    return false;
  }
}

bool freeOrSameNetY(int chip, int y, int net, int allowStacking) {
  if (chip < 0 || chip >= 12 || y < 0 || y >= 8) return false;   // a -1 lane from xMapForChipLane1 indexed the status array at -1
  // Serial.print("freeOrSameNetY: ");
  // Serial.print(chip);
  // Serial.print(", ");
  // Serial.print(y);
  // Serial.print(", ");
  // Serial.print(net);
  // Serial.print(", ");
  // Serial.print(allowStacking);
  // Serial.print(" = ");
  // Duplicates may not take one of the last two virgin chip-K y-rows - those
  // are reserved for ephemeral sense-tap routes (see reservedKRowForSenseTaps).
  // Stacking onto an already-same-net row is unaffected.
  if (reservedKRowForSenseTaps(chip, y)) {
    return false;
  }
  if (kRowBudgetRefuses(chip, y, net)) {
    return false;
  }
  if (reservedTapEscapeY(chip, y)) {
    return false;
  }
  if (globalState.connections.chipStates[chip].yStatus[y] == -1 ||
      (globalState.connections.chipStates[chip].yStatus[y] == net && allowStacking == 1)) {
    // Serial.println("true");
    return true;
  } else {
    // Serial.println("false");
    return false;
  }
}

bool frontEnd(int chip, int y, int x) { // is this an externally facing node
  if (chip < 8) {
    if (y == 0) // bounce node
    {
      return false;
    } else {
      return true;
    }
  }
  if (chip >= 8) {
    if (x >= 12 && x <= 14) // ijkl
      return false;
  } else {
    return true;
  }

  return false;
}

// Serial.print("path");
// Serial.print(i);

// resolveUncommittedHops();

// printPathsCompact();
// printChipStatus();

void couldntFindPath(int forcePrint) {
  if (debugNTCC2 == true || forcePrint == 1 || debugNTCC5 == true) {
    // Serial.print("\n\r");
  }
  numberOfUnconnectablePaths = 0;
  for (int i = 0; i < numberOfPaths; i++) {
    // Skip virtual paths - they don't need physical routing
    if (globalState.connections.paths[i].pathType == VIRTUAL) {
      continue;
    }
    
    if (debugNTCC5) {
      Serial.print("path ");
      Serial.println(i);
    }
    int foundNegative = 0;
    for (int j = 0; j < 3; j++) {
      if (globalState.connections.paths[i].chip[j] == -1 && j >= 2) {
        continue;
      }

      if (globalState.connections.paths[i].x[j] < 0 || globalState.connections.paths[i].y[j] < 0) {
        foundNegative = 1;
      }
    }

    if (foundNegative == 1 && globalState.connections.paths[i].duplicate == 0) {

      if (debugNTCC2 == true || forcePrint == 1 ) {
        if (globalState.connections.paths[i].node1 == -1 || globalState.connections.paths[i].node2 == -1) {
          continue;
        }
        if (globalState.connections.paths[i].pathType == VIRTUAL) {
          continue;
        }
        // Serial.print("currentSenseOverlayState.virtualWireNode1 = ");
        // Serial.println(currentSenseOverlayState.virtualWireNode1);
        // Serial.print("currentSenseOverlayState.virtualWireNode2 = ");
        // Serial.println(currentSenseOverlayState.virtualWireNode2);
        // Serial.print("globalState.connections.paths[i].node1 = ");
        // Serial.println(globalState.connections.paths[i].node1);
        // Serial.print("globalState.connections.paths[i].node2 = ");
        // Serial.println(globalState.connections.paths[i].node2);

        if (  currentSenseOverlayState.virtualWireNode1 == globalState.connections.paths[i].node1 && 
              currentSenseOverlayState.virtualWireNode2 == globalState.connections.paths[i].node2) {
          continue;
        }
        if (  currentSenseOverlayState.virtualWireNode1 == globalState.connections.paths[i].node2 && 
              currentSenseOverlayState.virtualWireNode2 == globalState.connections.paths[i].node1) {
          continue;
        }

        Serial.print("\n\rCouldn't find a path for ");
        printNodeOrName(globalState.connections.paths[i].node1);
        Serial.print(" to ");
        printNodeOrName(globalState.connections.paths[i].node2);
        Serial.println("\n\r");
      }

      // The table is [10][2] - validateAllPaths guards this write and this
      // copy did not, so an 11th unroutable path wrote past it (sweep).
      if (numberOfUnconnectablePaths < 10) {
        unconnectablePaths[numberOfUnconnectablePaths][0] = globalState.connections.paths[i].node1;
        unconnectablePaths[numberOfUnconnectablePaths][1] = globalState.connections.paths[i].node2;
      }
      numberOfUnconnectablePaths++;
      globalState.connections.paths[i].skip = true;
      // Same hazard as the duplicate branch below, one layer up: a failed
      // ijkl primary keeps its x lanes with y == -2, sendAllPaths never looks
      // at skip, and sendPath encoded y=-2 as a phantom y6 crosspoint (bench
      // 2026-09-02: BUF_IN/K.x12 and GP_8/L.x14 landed on K.y6/L.y6 - an
      // ADC net and GND). Wipe every coordinate of a failed primary.
      for (int j = 0; j < 4; j++) {
        globalState.connections.paths[i].x[j] = -1;
        globalState.connections.paths[i].y[j] = -1;
      }
    } else if (foundNegative == 1 && globalState.connections.paths[i].duplicate == 1) {
      // A duplicate that couldn't finish routing (e.g. refused a reserved
      // chip-K y-row for sense taps) is silently dropped. Wipe its
      // coordinates: a half-committed hop keeps x set with y == -2, and
      // sendPath() only filters -1, so an unwiped -2 would be encoded as a
      // phantom crosspoint (y=-2 -> y6) and could short its net onto an
      // occupied K row. altPathNeeded is cleared so incremental passes don't
      // retry a path we intentionally dropped.
      for (int j = 0; j < 4; j++) {
        globalState.connections.paths[i].x[j] = -1;
        globalState.connections.paths[i].y[j] = -1;
      }
      globalState.connections.paths[i].altPathNeeded = false;
    }
  }
  if (debugNTCC2 == true || forcePrint == 1 || debugNTCC5 == true) {
    // Serial.print("\n\r");
  }
}

void resolveUncommittedHops2(void) {}


  // Same-chip hop X search order. Breadboard chips try their twelve
  // breadboard-to-breadboard lanes before their four I/J/K/L lanes
  // (2026-09-03): an SF lane spent on a same-chip hop is one of the few
  // ways a sense tap can leave the chip. SF chips keep only the hub lanes
  // (every other X pin is a real node).
  const int freeXSearchOrder[12][16] = {
      {2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 14, 15, 0, 1, 9, 13},   // a: breadboard lanes first, then AI AJ AK AL
      {0, 1, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 2, 3, 11, 15},   // b: breadboard lanes first, then BI BJ BK BL
      {0, 1, 2, 3, 6, 7, 8, 10, 11, 12, 14, 15, 4, 5, 9, 13},   // c: breadboard lanes first, then CI CJ CL CK
      {0, 1, 2, 3, 4, 5, 8, 9, 10, 12, 13, 14, 6, 7, 11, 15},   // d: breadboard lanes first, then DI DJ DL DK
      {0, 2, 3, 4, 6, 7, 10, 11, 12, 13, 14, 15, 1, 5, 8, 9},   // e: breadboard lanes first, then EK EL EI EJ
      {0, 1, 2, 4, 5, 6, 8, 9, 12, 13, 14, 15, 3, 7, 10, 11},   // f: breadboard lanes first, then FK FL FI FJ
      {0, 2, 3, 4, 6, 7, 8, 9, 10, 11, 14, 15, 1, 5, 12, 13},   // g: breadboard lanes first, then GL GK GI GJ
      {0, 1, 2, 4, 5, 6, 8, 9, 10, 11, 12, 13, 3, 7, 14, 15},   // h: breadboard lanes first, then HL HK HI HJ
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 11, 12, 13, 14, -1}, // i
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 14, -1}, // j
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 14, -1}, // k
      {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 14, -1}, // l
  };

// A row on an SF chip the net already holds, reusable for another hop of
// the same net (2026-09-03): the row is on the net already, so closing one
// more of the net's X pins on it changes nothing electrically, and it spares
// a virgin row - on chip K the scarcest thing on the board (a rail duplicate
// and every sense tap need one). The row's far-end breadboard pin must be
// free or the net's own. Primaries only: the duplicate pass (allowStacking
// 0) keeps looking for virgin rows so a stacked copy is a real second path.
// Before this an SF-to-SF hop or a same-chip X-X hop always took a virgin
// row, which is how one net ended up holding two K rows (kevin_fixture:
// net 9 on K.Y5 and K.Y7).
static int sameNetSfRow(int chip, int net, int allowStacking) {
  if (allowStacking == 0 || chip < 8 || chip >= 12) {
    return -1;
  }
  for (int y = 0; y < 8; y++) {
    if (globalState.connections.chipStates[chip].yStatus[y] != net) {
      continue;
    }
    int bb = globalState.connections.chipStates[chip].yMap[y];
    int bbX = (bb >= 0 && bb < 8) ? xMapForChipLane0(bb, chip) : -1;
    if (bbX != -1 && !freeOrSameNetX(bb, bbX, net, 1)) {
      continue;
    }
    return y;
  }
  return -1;
}

void resolveUncommittedHops(int allowStacking, int powerOnly,
                            int noOrOnlyDuplicates, int startIndex) {
  if (debugNTCC2) {
    Serial.println("\nresolveUncommittedHops()");
  }

  for (int i = startIndex; i < numberOfPaths; i++) {
    // Skip virtual paths
    if (globalState.connections.paths[i].pathType == VIRTUAL) {
      continue;
    }
    // K-row rescue pass: only the paths the budgeted pass left unrouted
    if (kRowRescuePass && pathFullyRouted(i)) {
      continue;
    }
    
    // Filter paths based on powerOnly and duplicates flags
    if (powerOnly == 1 && (globalState.connections.paths[i].net > 5 || globalState.connections.paths[i].duplicate == 1)) {
      continue;
    }
    if (noOrOnlyDuplicates == 1 && globalState.connections.paths[i].duplicate == 0) {
      continue;
    }
    if (noOrOnlyDuplicates == 0 && globalState.connections.paths[i].duplicate == 1) {
      continue;
    }

    // K y-row reservation: mark whether this iteration routes a duplicate
    routingDuplicatePathNow = (globalState.connections.paths[i].duplicate == 1);

    // Check if this path needs resolution
    bool hasUnresolvedY = false;
    bool hasUnresolvedX = false;

    for (int checkY = 0; checkY < 4; checkY++) {
      if (globalState.connections.paths[i].y[checkY] == -2) {
        hasUnresolvedY = true;
        break;
      }
    }

    for (int checkX = 0; checkX < 4; checkX++) {
      if (globalState.connections.paths[i].x[checkX] == -2) {
        hasUnresolvedX = true;
        break;
      }
    }

    // Skip paths that don't need resolution
    if (!hasUnresolvedY && !hasUnresolvedX) {
      continue;
    }

    if (debugNTCC2) {
      Serial.print("Path[");
      Serial.print(i);
      Serial.print("] net=");
      Serial.print(globalState.connections.paths[i].net);
      Serial.print(" has unresolved positions: ");
      if (hasUnresolvedY) Serial.print("Y ");
      if (hasUnresolvedX) Serial.print("X ");
      Serial.println();
    }

    // Save state before making changes
    saveRoutingState(i);
    bool allAssignmentsSucceeded = true;

    // Handle X position assignments
    if (hasUnresolvedX) {
      // For same-chip connections, find one X position that works for both nodes
      if (globalState.connections.paths[i].sameChip && globalState.connections.paths[i].chip[0] == globalState.connections.paths[i].chip[1] && 
          globalState.connections.paths[i].x[0] == -2 && globalState.connections.paths[i].x[1] == -2) {
        
        int targetChip = globalState.connections.paths[i].chip[0];
        int sharedX = -1;
        
        // Find a free X position that can be used for both nodes on the same chip
        for (int searchIdx = 0; searchIdx < 16; searchIdx++) {
          if (freeXSearchOrder[targetChip][searchIdx] == -1) {
            continue;
          }
          
          int testX = freeXSearchOrder[targetChip][searchIdx];
          // A duplicate's same-chip hop stays off the chip's SF lanes: they
          // are how a sense tap leaves the chip (Kevin, 2026-09-03)
          if (globalState.connections.paths[i].duplicate != 0 && targetChip < 8 &&
              globalState.connections.chipStates[targetChip].xMap[testX] >= 8) {
            continue;
          }
          if (!freeOrSameNetX(targetChip, testX, globalState.connections.paths[i].net, allowStacking)) {
            continue;
          }
          
          // For breadboard chips, check that inter-chip connected X positions are also free
          if (targetChip < 8) {
            int connectedChip = globalState.connections.chipStates[targetChip].xMap[testX];
            if (connectedChip < 8) {
              // This X connects to another breadboard chip - check reciprocal X
              int reciprocalX = xMapForChipLane0(connectedChip, targetChip);
              if (reciprocalX != -1) {
                if (!freeOrSameNetX(connectedChip, reciprocalX, globalState.connections.paths[i].net, allowStacking)) {
                  continue; // Connected chip's X is occupied by different net
                }
              }
            } else if (connectedChip >= 8) {
              // This X connects to an SF chip - check the SF chip's Y position
              // SF chip Y[bbChipIndex] connects to BB chip[bbChipIndex]
              int sfYPosition = targetChip; // The BB chip index determines which Y on the SF chip
              if (!freeOrSameNetY(connectedChip, sfYPosition, globalState.connections.paths[i].net, allowStacking)) {
                continue; // SF chip's Y is occupied by different net
              }
            }
          }
          
          sharedX = testX;
          break;
        }
        
        if (sharedX != -1) {
          // Assign the same X position to both positions for same-chip connection
          bool success = setPathX(i, 0, sharedX) && setPathX(i, 1, sharedX);
          bool chipSuccess = false;
          if (success) {
            chipSuccess = setChipXStatus(targetChip, sharedX, globalState.connections.paths[i].net, "resolveUncommittedHops same-chip X");
            
            // For breadboard chips, also mark the connected chip's reciprocal X or SF chip's Y
            if (chipSuccess && targetChip < 8) {
              int connectedChip = globalState.connections.chipStates[targetChip].xMap[sharedX];
              if (connectedChip < 8) {
                int reciprocalX = xMapForChipLane0(connectedChip, targetChip);
                if (reciprocalX != -1) {
                  bool reciprocalSuccess = setChipXStatus(connectedChip, reciprocalX, globalState.connections.paths[i].net, "resolveUncommittedHops same-chip X reciprocal");
                  if (!reciprocalSuccess) {
                    chipSuccess = false;
                  }
                }
              } else if (connectedChip >= 8) {
                // Mark the SF chip's Y position as used
                int sfYPosition = targetChip; // The BB chip index determines which Y on the SF chip
                bool sfYSuccess = setChipYStatusSafe(connectedChip, sfYPosition, globalState.connections.paths[i].net, "resolveUncommittedHops same-chip X->SF Y");
                if (!sfYSuccess) {
                  chipSuccess = false;
                }
              }
            }
          }
          
          if (!success || !chipSuccess) {
            allAssignmentsSucceeded = false;
          } else {
            if (debugNTCC2) {
              Serial.print("  Assigned shared X=");
              Serial.print(sharedX);
              Serial.print(" to same-chip globalState.connections.paths[");
              Serial.print(i);
              Serial.print("] on chip ");
              Serial.println(chipNumToChar(targetChip));
            }
          }
        } else {
          allAssignmentsSucceeded = false;
          if (debugNTCC2) {
            Serial.print("ERROR: No free shared X found for same-chip globalState.connections.paths[");
            Serial.print(i);
            Serial.println("]");
          }
        }
      } else {
        // Regular X position assignment for positions that still need resolution
        for (int pos = 0; pos < 4; pos++) {
          if (globalState.connections.paths[i].chip[pos] != -1 && globalState.connections.paths[i].x[pos] == -2) {
            int freeX = -1;
            
            // Find free X position using search order
            for (int searchIdx = 0; searchIdx < 16; searchIdx++) {
              if (freeXSearchOrder[globalState.connections.paths[i].chip[pos]][searchIdx] == -1) {
                continue;
              }
              
              int testX = freeXSearchOrder[globalState.connections.paths[i].chip[pos]][searchIdx];
              if (globalState.connections.paths[i].duplicate != 0 && globalState.connections.paths[i].chip[pos] < 8 &&
                  globalState.connections.chipStates[globalState.connections.paths[i].chip[pos]].xMap[testX] >= 8) {
                continue; // same rule as the shared-X search above
              }
              if (!freeOrSameNetX(globalState.connections.paths[i].chip[pos], testX, globalState.connections.paths[i].net, allowStacking)) {
                continue;
              }
              
              // For breadboard chips, check that inter-chip connected X positions are also free
              if (globalState.connections.paths[i].chip[pos] < 8) {
                int connectedChip = globalState.connections.chipStates[globalState.connections.paths[i].chip[pos]].xMap[testX];
                if (connectedChip < 8) {
                  // This X connects to another breadboard chip - check reciprocal X
                  int reciprocalX = xMapForChipLane0(connectedChip, globalState.connections.paths[i].chip[pos]);
                  if (reciprocalX != -1) {
                    if (!freeOrSameNetX(connectedChip, reciprocalX, globalState.connections.paths[i].net, allowStacking)) {
                      continue; // Connected chip's X is occupied by different net
                    }
                  }
                } else if (connectedChip >= 8) {
                  // This X connects to an SF chip - check the SF chip's Y position
                  // SF chip Y[bbChipIndex] connects to BB chip[bbChipIndex]
                  int sfYPosition = globalState.connections.paths[i].chip[pos]; // The BB chip index determines which Y on the SF chip
                  if (!freeOrSameNetY(connectedChip, sfYPosition, globalState.connections.paths[i].net, allowStacking)) {
                    continue; // SF chip's Y is occupied by different net
                  }
                }
              }
              
              freeX = testX;
              break;
            }

            if (freeX != -1) {
              bool pathXSuccess = setPathX(i, pos, freeX);
              bool chipXSuccess = false;
              if (pathXSuccess) {
                chipXSuccess = setChipXStatus(globalState.connections.paths[i].chip[pos], freeX, globalState.connections.paths[i].net, "resolveUncommittedHops X");
                
                // For breadboard chips, also mark the connected chip's reciprocal X or SF chip's Y
                if (chipXSuccess && globalState.connections.paths[i].chip[pos] < 8) {
                  int connectedChip = globalState.connections.chipStates[globalState.connections.paths[i].chip[pos]].xMap[freeX];
                  if (connectedChip < 8) {
                    int reciprocalX = xMapForChipLane0(connectedChip, globalState.connections.paths[i].chip[pos]);
                    if (reciprocalX != -1) {
                      bool reciprocalSuccess = setChipXStatus(connectedChip, reciprocalX, globalState.connections.paths[i].net, "resolveUncommittedHops X reciprocal");
                      if (!reciprocalSuccess) {
                        chipXSuccess = false;
                      }
                    }
                  } else if (connectedChip >= 8) {
                    // Mark the SF chip's Y position as used
                    int sfYPosition = globalState.connections.paths[i].chip[pos]; // The BB chip index determines which Y on the SF chip
                    bool sfYSuccess = setChipYStatusSafe(connectedChip, sfYPosition, globalState.connections.paths[i].net, "resolveUncommittedHops X->SF Y");
                    if (!sfYSuccess) {
                      chipXSuccess = false;
                    }
                  }
                }
              }

              if (!pathXSuccess || !chipXSuccess) {
                allAssignmentsSucceeded = false;
                break;
              }

              if (debugNTCC2) {
                Serial.print("  Assigned X=");
                Serial.print(freeX);
                Serial.print(" to position ");
                Serial.print(pos);
                Serial.print(" chip ");
                Serial.println(chipNumToChar(globalState.connections.paths[i].chip[pos]));
              }
            } else {
              allAssignmentsSucceeded = false;
              if (debugNTCC2) {
                Serial.print("ERROR: No free X found for position ");
                Serial.print(pos);
                Serial.println();
              }
              break;
            }
          }
        }
      }
    }

    // Handle Y position assignments
    if (allAssignmentsSucceeded && hasUnresolvedY) {
      // For same-chip connections, find one Y position that works for both nodes
      if (globalState.connections.paths[i].sameChip && globalState.connections.paths[i].chip[0] == globalState.connections.paths[i].chip[1] && 
          globalState.connections.paths[i].y[0] == -2 && globalState.connections.paths[i].y[1] == -2) {
        
        int targetChip = globalState.connections.paths[i].chip[0];
        int sharedY = sameNetSfRow(targetChip, globalState.connections.paths[i].net, allowStacking);
        
        // Find a free Y position that can be used for both nodes on the same chip
        for (int testY = 0; sharedY < 0 && testY < 8; testY++) {
          // For breadboard chips, only allow Y=0
          if (targetChip < 8 && testY != 0) {
            continue;
          }
          
          if (freeOrSameNetY(targetChip, testY, globalState.connections.paths[i].net, allowStacking)) {
            // For special function chips, check breadboard chip X connection
            if (targetChip >= 8) {
              int bbChipX = xMapForChipLane0(testY, targetChip);
              if (bbChipX != -1) {
                if (!freeOrSameNetX(testY, bbChipX, globalState.connections.paths[i].net, allowStacking)) {
                  continue;
                }
              }
            }
            sharedY = testY;
            break;
          }
        }
        
        if (sharedY != -1) {
          // Assign the same Y position to both positions for same-chip connection
          bool success = setPathY(i, 0, sharedY) && setPathY(i, 1, sharedY);
          bool chipSuccess = false;
          if (success) {
            chipSuccess = setChipYStatusSafe(targetChip, sharedY, globalState.connections.paths[i].net, "resolveUncommittedHops same-chip Y");
          }
          
          // Update breadboard chip X status for special function chips
          if (success && chipSuccess && targetChip >= 8) {
            int bbChipX = xMapForChipLane0(sharedY, targetChip);
            if (bbChipX != -1) {
              bool xSuccess = setChipXStatus(sharedY, bbChipX, globalState.connections.paths[i].net, "resolveUncommittedHops same-chip BB X");
              if (!xSuccess) {
                allAssignmentsSucceeded = false;
              }
            }
          }
          
          if (!success || !chipSuccess) {
            allAssignmentsSucceeded = false;
          } else {
            if (debugNTCC2) {
              Serial.print("  Assigned shared Y=");
              Serial.print(sharedY);
              Serial.print(" to same-chip globalState.connections.paths[");
              Serial.print(i);
              Serial.print("] on chip ");
              Serial.println(chipNumToChar(targetChip));
            }
          }
        } else {
          allAssignmentsSucceeded = false;
          if (debugNTCC2) {
            Serial.print("ERROR: No free shared Y found for same-chip globalState.connections.paths[");
            Serial.print(i);
            Serial.println("]");
          }
        }
      } else {
        // Check if this is an ijkl inter-chip path
        bool isIjklPath = false;
      if (globalState.connections.paths[i].chip[0] != globalState.connections.paths[i].chip[1] && globalState.connections.paths[i].chip[2] != -1 && globalState.connections.paths[i].chip[3] != -1) {
        // Check if positions 0&2 are same chip and positions 1&3 are same chip
        if (globalState.connections.paths[i].chip[0] == globalState.connections.paths[i].chip[2] && globalState.connections.paths[i].chip[1] == globalState.connections.paths[i].chip[3]) {
          isIjklPath = true;
          if (debugNTCC2) {
            Serial.print("Detected ijkl inter-chip globalState.connections.paths[");
            Serial.print(i);
            Serial.print("] - positions 0&2 on chip ");
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[0]));
            Serial.print(", positions 1&3 on chip ");
            Serial.println(chipNumToChar(globalState.connections.paths[i].chip[1]));
          }
        }
      }

      if (isIjklPath) {
        // Handle ijkl paths: assign same Y to chip pairs (0&2, 1&3)
        int chipPairs[2][2] = {{0, 2}, {1, 3}}; // {pos0, pos2} and {pos1, pos3}
        
        for (int pairIdx = 0; pairIdx < 2; pairIdx++) {
          int pos1 = chipPairs[pairIdx][0];
          int pos2 = chipPairs[pairIdx][1];
          
          // Check if this chip pair needs Y resolution
          bool needsY = (globalState.connections.paths[i].chip[pos1] != -1 && globalState.connections.paths[i].y[pos1] == -2) ||
                        (globalState.connections.paths[i].chip[pos2] != -1 && globalState.connections.paths[i].y[pos2] == -2);
          
          if (needsY) {
            int targetChip = globalState.connections.paths[i].chip[pos1]; // Both positions should be same chip
            int freeY = sameNetSfRow(targetChip, globalState.connections.paths[i].net, allowStacking);
            
            for (int testY = 0; freeY < 0 && testY < 8; testY++) {
              // For breadboard chips, only allow Y=0
              if (targetChip < 8 && testY != 0) {
                continue;
              }

              // Check if this Y position is free
              if (!freeOrSameNetY(targetChip, testY, globalState.connections.paths[i].net, allowStacking)) {
                continue;
              }

              // For special function chips, check breadboard chip X connection
              if (targetChip >= 8) {
                int bbChipX = xMapForChipLane0(testY, targetChip);
                if (bbChipX != -1) {
                  if (!freeOrSameNetX(testY, bbChipX, globalState.connections.paths[i].net, allowStacking)) {
                    continue;
                  }
                }
              }

              freeY = testY;
              break;
            }

            if (freeY != -1) {
              // Assign same Y to both positions in the pair
              bool success = true;
              
              if (globalState.connections.paths[i].y[pos1] == -2) {
                success &= setPathY(i, pos1, freeY);
              }
              if (globalState.connections.paths[i].y[pos2] == -2) {
                success &= setPathY(i, pos2, freeY);
              }
              
              bool chipYSuccess = setChipYStatusSafe(targetChip, freeY, globalState.connections.paths[i].net, "resolveUncommittedHops ijkl Y");
              
              if (!success || !chipYSuccess) {
                allAssignmentsSucceeded = false;
                if (debugNTCC2) {
                  Serial.print("ERROR: Failed ijkl Y assignment for chip ");
                  Serial.println(chipNumToChar(targetChip));
                }
                break;
              }

              // Update breadboard chip X status for special function chips
              if (targetChip >= 8) {
                int bbChipX = xMapForChipLane0(freeY, targetChip);
                if (bbChipX != -1) {
                  bool xSuccess = setChipXStatus(freeY, bbChipX, globalState.connections.paths[i].net, "resolveUncommittedHops ijkl BB X");
                  if (!xSuccess) {
                    allAssignmentsSucceeded = false;
                    if (debugNTCC2) {
                      Serial.print("ERROR: Failed ijkl BB X assignment for chip ");
                      Serial.println(chipNumToChar(freeY));
                    }
                    break;
                  }
                }
              }

              if (debugNTCC2) {
                Serial.print("  Assigned Y=");
                Serial.print(freeY);
                Serial.print(" to ijkl chip pair: positions ");
                Serial.print(pos1);
                Serial.print("&");
                Serial.print(pos2);
                Serial.print(" on chip ");
                Serial.println(chipNumToChar(targetChip));
              }
            } else {
              allAssignmentsSucceeded = false;
              if (debugNTCC2) {
                Serial.print("ERROR: No free Y found for ijkl chip ");
                Serial.println(chipNumToChar(targetChip));
              }
              break;
            }
          }
        }
      } else {
        // Handle regular paths: assign Y to each position independently
        for (int pos = 0; pos < 4; pos++) {
          if (globalState.connections.paths[i].chip[pos] != -1 && globalState.connections.paths[i].y[pos] == -2) {
            int freeY = sameNetSfRow(globalState.connections.paths[i].chip[pos], globalState.connections.paths[i].net, allowStacking);
            
            for (int testY = 0; freeY < 0 && testY < 8; testY++) {
              // For breadboard chips, only allow Y=0
              if (globalState.connections.paths[i].chip[pos] < 8 && testY != 0) {
                continue;
              }

              // Check if this Y position is free
              if (!freeOrSameNetY(globalState.connections.paths[i].chip[pos], testY, globalState.connections.paths[i].net, allowStacking)) {
                continue;
              }

              // For special function chips, check breadboard chip X connection
              if (globalState.connections.paths[i].chip[pos] >= 8) {
                int bbChipX = xMapForChipLane0(testY, globalState.connections.paths[i].chip[pos]);
                if (bbChipX != -1) {
                  if (!freeOrSameNetX(testY, bbChipX, globalState.connections.paths[i].net, allowStacking)) {
                    continue;
                  }
                }
              }

              freeY = testY;
              break;
            }

            if (freeY != -1) {
              bool pathYSuccess = setPathY(i, pos, freeY);
              bool chipYSuccess = false;
              if (pathYSuccess) {
                chipYSuccess = setChipYStatusSafe(globalState.connections.paths[i].chip[pos], freeY, globalState.connections.paths[i].net, "resolveUncommittedHops Y");
              }

              if (!pathYSuccess || !chipYSuccess) {
                allAssignmentsSucceeded = false;
                break;
              }

              // Update breadboard chip X status for special function chips
              if (globalState.connections.paths[i].chip[pos] >= 8) {
                int bbChipX = xMapForChipLane0(freeY, globalState.connections.paths[i].chip[pos]);
                if (bbChipX != -1) {
                  bool xSuccess = setChipXStatus(freeY, bbChipX, globalState.connections.paths[i].net, "resolveUncommittedHops BB X for SF");
                  if (!xSuccess) {
                    allAssignmentsSucceeded = false;
                    break;
                  }
                }
              }

              if (debugNTCC2) {
                Serial.print("  Assigned Y=");
                Serial.print(freeY);
                Serial.print(" to position ");
                Serial.print(pos);
                Serial.print(" chip ");
                Serial.println(chipNumToChar(globalState.connections.paths[i].chip[pos]));
              }
            } else {
              allAssignmentsSucceeded = false;
              if (debugNTCC2) {
                Serial.print("ERROR: No free Y found for position ");
                Serial.print(pos);
                Serial.println();
              }
              break;
            }
          }
        }
      }
      }
    }

    // Commit or restore based on success
    if (allAssignmentsSucceeded) {
      commitRoutingState();
      if (debugNTCC2) {
        Serial.print("SUCCESS: All positions resolved for globalState.connections.paths[");
        Serial.print(i);
        Serial.println("]");
      }
    } else {
      restoreRoutingState(i);
      globalState.connections.paths[i].altPathNeeded = true;
      if (debugNTCC2) {
        Serial.print("FAILED: Position assignments failed for globalState.connections.paths[");
        Serial.print(i);
        Serial.println("], state restored");
      }
    }
  }
  routingDuplicatePathNow = false;

  // Debug: Show final path states
  if (debugNTCC2) {
    Serial.println("Final path Y values after resolveUncommittedHops:");
    int unresolvedCount = 0;
    for (int i = 0; i < numberOfPaths; i++) {
      bool hasNegTwo = false;
      for (int j = 0; j < 4; j++) {
        if (globalState.connections.paths[i].y[j] == -2) {
          hasNegTwo = true;
          break;
        }
      }
      if (hasNegTwo) {
        unresolvedCount++;
        Serial.print("  globalState.connections.paths[");
        Serial.print(i);
        Serial.print("] net=");
        Serial.print(globalState.connections.paths[i].net);
        Serial.print(" (");
        printNodeOrName(globalState.connections.paths[i].node1);
        Serial.print("-");
        printNodeOrName(globalState.connections.paths[i].node2);
        Serial.print(") still has -2: ");
        Serial.print("chips=[");
        for (int j = 0; j < 4; j++) {
          if (globalState.connections.paths[i].chip[j] != -1) {
            Serial.print(chipNumToChar(globalState.connections.paths[i].chip[j]));
            if (j < 3 && globalState.connections.paths[i].chip[j + 1] != -1)
              Serial.print(",");
          }
        }
        Serial.print("] y=[");
        for (int j = 0; j < 4; j++) {
          Serial.print(globalState.connections.paths[i].y[j]);
          if (j < 3)
            Serial.print(",");
        }
        Serial.print("] altPathNeeded=");
        Serial.println(globalState.connections.paths[i].altPathNeeded);
      }
    }
    if (unresolvedCount == 0) {
      Serial.println("  All Y positions successfully resolved!");
    } else {
      Serial.print("  WARNING: ");
      Serial.print(unresolvedCount);
      Serial.println(" paths still have unresolved Y positions (-2)");
    }
  }
}



int checkForOverlappingPaths() {
  int found = 0;

  if (debugNTCC3) {
    Serial.println("\n=== CHECKING FOR OVERLAPPING PATHS ===");
  }

  // printPathsCompact(2);
  // printChipStatus();

  for (int i = 0; i < numberOfPaths; i++) {
    int fchip[4] = {globalState.connections.paths[i].chip[0], globalState.connections.paths[i].chip[1], globalState.connections.paths[i].chip[2],
                    globalState.connections.paths[i].chip[3]};

    for (int j = 0; j < numberOfPaths; j++) {
      if (i == j) {
        continue;
      }
      if (globalState.connections.paths[i].net == globalState.connections.paths[j].net) {
        continue;
      }
      
      //! 
      // Check if both paths are fake GPIO input paths (they share ADC via time-multiplexing)
      // This check is done once per i,j pair (outside f,s loops) for efficiency
      // Path nodes have been expanded from FAKE_GP_IN_x to ADCx, so we check bridges
      auto isFakeGpioInputPath = [](int pathIdx) -> bool {
        int pathNode1 = globalState.connections.paths[pathIdx].node1;
        // Search bridges for one that matches this path's node1 and has a FAKE_GP_IN
        for (int b = 0; b < globalState.connections.numBridges; b++) {
          int bridgeNode1 = globalState.connections.bridges[b][0];
          int bridgeNode2 = globalState.connections.bridges[b][1];
          if (bridgeNode1 == pathNode1 && IS_FAKE_GP_IN(bridgeNode2)) {
            return true;
          }
          if (bridgeNode2 == pathNode1 && IS_FAKE_GP_IN(bridgeNode1)) {
            return true;
          }
        }
        return false;
      };
      
      if (isFakeGpioInputPath(i) && isFakeGpioInputPath(j)) {
        // Both paths are fake GPIO inputs sharing the same ADC - skip overlap check
        continue;
      }
      
      int schip[4] = {globalState.connections.paths[j].chip[0], globalState.connections.paths[j].chip[1], globalState.connections.paths[j].chip[2],
                      globalState.connections.paths[j].chip[3]};

      for (int f = 0; f < 4; f++) {
        for (int s = 0; s < 4; s++) {
          if (fchip[f] == schip[s] && fchip[f] != -1) {
            if (globalState.connections.paths[i].x[f] <= 0 || globalState.connections.paths[j].x[s] <= 0) {
              continue;
            }
            if (globalState.connections.paths[i].y[f] <= 0 || globalState.connections.paths[j].y[s] <= 0) {
              continue;
            }
            // Skip overlap check for power rails - multiple nets can share power rails
            // Check both node1 and node2 of each path for power rail nodes
            // Also expand FakeGPIO virtual nodes to their actual voltage sources
            int node1_i = globalState.connections.paths[i].node1;
            int node2_i = globalState.connections.paths[i].node2;
            int node1_j = globalState.connections.paths[j].node1;
            int node2_j = globalState.connections.paths[j].node2;
            
            // Expand FakeGPIO virtual nodes to actual voltage sources for power rail check
            auto expandNode = [](int node) -> int {
              if (IS_FAKE_GP_OUT(node)) {
                int slot = FAKE_GP_OUT_SLOT(node);
                if (slot >= 0 && slot < MAX_FAKE_GP_OUT && fakeGpioOutputs[slot].active) {
                  return (fakeGpioOutputs[slot].currentState == 1) 
                      ? fakeGpioOutputs[slot].highVoltageNode 
                      : fakeGpioOutputs[slot].lowVoltageNode;
                }
              }
              if (IS_FAKE_GP_IN(node)) {
                int slot = FAKE_GP_IN_SLOT(node);
                if (slot >= 0 && slot < MAX_FAKE_GP_IN && fakeGpioInputs[slot].active) {
                  // All inputs share a single ADC (dynamically selected)
                  return (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : ADC0;
                }
              }
              return node;
            };
            
            node1_i = expandNode(node1_i);
            node2_i = expandNode(node2_i);
            node1_j = expandNode(node1_j);
            node2_j = expandNode(node2_j);
            
            auto isPowerRail = [](int node) {
              return (node == TOP_RAIL || node == BOTTOM_RAIL || node == GND || 
                      node == TOP_RAIL_GND || node == BOTTOM_RAIL_GND);
            };
            
            // Check if both paths connect to the same power rail
            bool sharesPowerRail = false;
            if (isPowerRail(node2_i) && isPowerRail(node2_j) && node2_i == node2_j) {
              sharesPowerRail = true;  // Both paths have same power rail as node2
            }
            if (isPowerRail(node1_i) && isPowerRail(node1_j) && node1_i == node1_j) {
              sharesPowerRail = true;  // Both paths have same power rail as node1
            }
            if (isPowerRail(node2_i) && isPowerRail(node1_j) && node2_i == node1_j) {
              sharesPowerRail = true;
            }
            if (isPowerRail(node1_i) && isPowerRail(node2_j) && node1_i == node2_j) {
              sharesPowerRail = true;
            }
            
            if (sharesPowerRail) {
              // Both paths connect to the same power rail - this is allowed
              continue;
            }
            
            if (globalState.connections.paths[i].x[f] == globalState.connections.paths[j].x[s] && globalState.connections.paths[i].skip <= 0 &&
                globalState.connections.paths[j].skip <= 0) {
              // if (debugNTCC3) {
              if (globalState.connections.paths[i].duplicate > 0 || globalState.connections.paths[j].duplicate > 0) {
              if (globalState.connections.paths[i].duplicate > 0) {
                globalState.connections.paths[i].net = -1;
                globalState.connections.paths[i].duplicate = 0;
                globalState.connections.paths[i].chip[0] = -1;
                globalState.connections.paths[i].chip[1] = -1;
                globalState.connections.paths[i].chip[2] = -1;
                globalState.connections.paths[i].chip[3] = -1;
                globalState.connections.paths[i].x[0] = -1;
                globalState.connections.paths[i].x[1] = -1;
                globalState.connections.paths[i].x[2] = -1;
                globalState.connections.paths[i].x[3] = -1;
                globalState.connections.paths[i].y[0] = -1;
                globalState.connections.paths[i].y[1] = -1;
                globalState.connections.paths[i].y[2] = -1;
                globalState.connections.paths[i].y[3] = -1;
                globalState.connections.paths[i].altPathNeeded = false;
                globalState.connections.paths[i].sameChip = false;
                globalState.connections.paths[i].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at x ");
                // Serial.print(globalState.connections.paths[i].x[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(globalState.connections.paths[i].net);
                // Serial.print(" and ");
                // Serial.print(globalState.connections.paths[j].net);
                // Serial.println("   skipping");
                
              }
              if (globalState.connections.paths[j].duplicate > 0) {
                globalState.connections.paths[j].net = -1;
                globalState.connections.paths[j].duplicate = 0;
                globalState.connections.paths[j].chip[0] = -1;
                globalState.connections.paths[j].chip[1] = -1;
                globalState.connections.paths[j].chip[2] = -1;
                globalState.connections.paths[j].chip[3] = -1;
                globalState.connections.paths[j].x[0] = -1;
                globalState.connections.paths[j].x[1] = -1;
                globalState.connections.paths[j].x[2] = -1;
                globalState.connections.paths[j].x[3] = -1;
                globalState.connections.paths[j].y[0] = -1;
                globalState.connections.paths[j].y[1] = -1;
                globalState.connections.paths[j].y[2] = -1;
                globalState.connections.paths[j].y[3] = -1;
                globalState.connections.paths[j].altPathNeeded = false;
                globalState.connections.paths[j].sameChip = false;
                globalState.connections.paths[j].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at x ");
                // Serial.print(globalState.connections.paths[i].x[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(globalState.connections.paths[i].net);
                // Serial.print(" and ");
                // Serial.print(globalState.connections.paths[j].net);
                // Serial.println("   skipping");
               
              }
              continue;
              }

              Serial.print("OVERLAP DETECTED: Path ");
              Serial.print(i);
              Serial.print(" (");
              printNodeOrName(globalState.connections.paths[i].node1);
              Serial.print("-");
              printNodeOrName(globalState.connections.paths[i].node2);
              Serial.print(") and Path ");
              Serial.print(j);
              Serial.print(" (");
              printNodeOrName(globalState.connections.paths[j].node1);
              Serial.print("-");
              printNodeOrName(globalState.connections.paths[j].node2);
              Serial.print(") overlap at X=");
              Serial.print(globalState.connections.paths[i].x[f]);
              Serial.print(" on chip ");
              Serial.print(chipNumToChar(fchip[f]));
              Serial.print(", nets ");
              Serial.print(globalState.connections.paths[i].net);
              Serial.print(" and ");
              Serial.println(globalState.connections.paths[j].net);
              globalState.connections.paths[i].skip = true;

              // Add to unconnectable paths for LED animation
              if (numberOfUnconnectablePaths < 10) {
                unconnectablePaths[numberOfUnconnectablePaths][0] = globalState.connections.paths[i].node1;
                unconnectablePaths[numberOfUnconnectablePaths][1] = globalState.connections.paths[i].node2;
                numberOfUnconnectablePaths++;
                if (debugNTCC3) {
                  Serial.print("Added to unconnectable paths: ");
                  printNodeOrName(globalState.connections.paths[i].node1);
                  Serial.print("-");
                  printNodeOrName(globalState.connections.paths[i].node2);
                  Serial.println();
                }
              }

              // printPathsCompact();
              // printChipStatus();
              // }
              // return 1;
              found++;
            } else if (globalState.connections.paths[i].y[f] == globalState.connections.paths[j].y[s] && globalState.connections.paths[i].skip <= 0 &&
                       globalState.connections.paths[j].skip <= 0) {
              // if (debugNTCC3) {
              if (globalState.connections.paths[i].duplicate > 0 || globalState.connections.paths[j].duplicate > 0) {
              if (globalState.connections.paths[i].duplicate > 0) {
                globalState.connections.paths[i].net = -1;
                globalState.connections.paths[i].duplicate = 0;
                globalState.connections.paths[i].chip[0] = -1;
                globalState.connections.paths[i].chip[1] = -1;
                globalState.connections.paths[i].chip[2] = -1;
                globalState.connections.paths[i].chip[3] = -1;
                globalState.connections.paths[i].x[0] = -1;
                globalState.connections.paths[i].x[1] = -1;
                globalState.connections.paths[i].x[2] = -1;
                globalState.connections.paths[i].x[3] = -1;
                globalState.connections.paths[i].y[0] = -1;
                globalState.connections.paths[i].y[1] = -1;
                globalState.connections.paths[i].y[2] = -1;
                globalState.connections.paths[i].y[3] = -1;
                globalState.connections.paths[i].altPathNeeded = false;
                globalState.connections.paths[i].sameChip = false;
                globalState.connections.paths[i].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at y ");
                // Serial.print(globalState.connections.paths[i].y[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(globalState.connections.paths[i].net);
                // Serial.print(" and ");
                // Serial.println(globalState.connections.paths[j].net);
                // Serial.println("   skipping");
              }
              if (globalState.connections.paths[j].duplicate > 0) {
                globalState.connections.paths[j].net = -1;
                globalState.connections.paths[j].duplicate = 0;
                globalState.connections.paths[j].chip[0] = -1;
                globalState.connections.paths[j].chip[1] = -1;
                globalState.connections.paths[j].chip[2] = -1;
                globalState.connections.paths[j].chip[3] = -1;
                globalState.connections.paths[j].x[0] = -1;
                globalState.connections.paths[j].x[1] = -1;
                globalState.connections.paths[j].x[2] = -1;
                globalState.connections.paths[j].x[3] = -1;
                globalState.connections.paths[j].y[0] = -1;
                globalState.connections.paths[j].y[1] = -1;
                globalState.connections.paths[j].y[2] = -1;
                globalState.connections.paths[j].y[3] = -1;
                globalState.connections.paths[j].altPathNeeded = false;
                globalState.connections.paths[j].sameChip = false;
                globalState.connections.paths[j].skip = 0;
                // Serial.print("Duplicate path ");
                // Serial.print(i);
                // Serial.print(" and ");
                // Serial.print(j);
                // Serial.print(" overlap at y ");
                // Serial.print(globalState.connections.paths[i].y[f]);
                // Serial.print(" on chip ");
                // Serial.print(chipNumToChar(fchip[f]));
                // Serial.print("   nets ");
                // Serial.print(globalState.connections.paths[i].net);
                // Serial.print(" and ");
                // Serial.println(globalState.connections.paths[j].net);
                // Serial.println("   skipping");
              }
              continue;
              }

              Serial.print("OVERLAP DETECTED: Path ");
              Serial.print(i);
              Serial.print(" (");
              printNodeOrName(globalState.connections.paths[i].node1);
              Serial.print("-");
              printNodeOrName(globalState.connections.paths[i].node2);
              Serial.print(") and Path ");
              Serial.print(j);
              Serial.print(" (");
              printNodeOrName(globalState.connections.paths[j].node1);
              Serial.print("-");
              printNodeOrName(globalState.connections.paths[j].node2);
              Serial.print(") overlap at Y=");
              Serial.print(globalState.connections.paths[i].y[f]);
              Serial.print(" on chip ");
              Serial.print(chipNumToChar(fchip[f]));
              Serial.print(", nets ");
              Serial.print(globalState.connections.paths[i].net);
              Serial.print(" and ");
              Serial.println(globalState.connections.paths[j].net);
              globalState.connections.paths[i].skip = true;
              
              // Add to unconnectable paths for LED animation
              if (numberOfUnconnectablePaths < 10) {
                unconnectablePaths[numberOfUnconnectablePaths][0] = globalState.connections.paths[i].node1;
                unconnectablePaths[numberOfUnconnectablePaths][1] = globalState.connections.paths[i].node2;
                numberOfUnconnectablePaths++;
                if (debugNTCC3) {
                  Serial.print("Added to unconnectable paths: ");
                  printNodeOrName(globalState.connections.paths[i].node1);
                  Serial.print("-");
                  printNodeOrName(globalState.connections.paths[i].node2);
                  Serial.println();
                }
              }
              
              // printPathsCompact();
              // printChipStatus();
              // }
              // return 1;
              found++;
            }
          }
        }
      }
    }
  }
  
  if (debugNTCC3 || found > 0) {
    Serial.print("=== OVERLAP CHECK COMPLETE: ");
    Serial.print(found);
    Serial.print(" overlaps found, ");
    Serial.print(numberOfUnconnectablePaths);
    Serial.println(" total unconnectable paths ===");
  }
  
  return found;
}

int printNetOrNumber(int net, Stream* target) {
  int spaces = 0;
  switch (net) {
  case 0:
    spaces = target->print("E");
    break;
  case 1:
    spaces = target->print("Gn");
    break;
  case 2:
    spaces = target->print("T");
    break;
  case 3:
    spaces = target->print("B");
    break;
  case 4:
    spaces = target->print("d0");
    break;
  case 5:
    spaces = target->print("d1");
    break;
  default:
    spaces = target->print(net);
    break;
  }
  return spaces;
}

/// @brief print paths in a compact format
/// @param showCullDupes 0 = show all paths, 1 = show routed duplicates, 2 =
/// show all duplicates
void printPathsCompact(int showCullDupes, Stream* target) {

  target->print("numberOfPaths: ");
  target->println(numberOfPaths);
  target->print("numberOfNets: ");
  target->println(numberOfNets);
  assignTermColor();
  int lastDuplicate = 0;
  int duplicateSection = 0;

  int skipLine = 0;
  target->println(
      "\n\rpath\tnet\tnode1\tchip0\tx0\ty0\tnode2\tchip1\tx1\ty1\ta"
      "ltPath\tsameChp\tdup\tpathType\tchip2\tx2\ty2");

  for (int i = 0; i < numberOfPaths; i++) {
    skipLine = 0;
    switch (showCullDupes) {
    case 0:
      if (globalState.connections.paths[i].duplicate > 0) {
        skipLine = 1;
        // continue;
      }
      break;
    case 1:

      if (globalState.connections.paths[i].duplicate > 0 && globalState.connections.paths[i].x[0] < 0 && globalState.connections.paths[i].x[1] < 0) {
        skipLine = 1;
        // continue;
      }
      break;
    }

    if (globalState.connections.paths[i].duplicate > 0 && duplicateSection == 0) {
      // Serial.println("\n\rduplicates");
      // duplicateSection = 1;
      skipLine = 1;
      // continue;
    }
    if (globalState.connections.paths[i].duplicate == 0 && duplicateSection == 1) {
      skipLine = 1;
      // continue;
    }

    if (skipLine == 0) {
      lastDuplicate = globalState.connections.paths[i].duplicate;
      {
        int pn = globalState.connections.paths[i].net;   // a culled duplicate carries net -1
        changeTerminalColor(globalState.connections.nets[(pn >= 0 && pn < MAX_NETS) ? pn : 0].termColor, false, target);
      }
      target->print(i);
      target->print("\t");

      printNetOrNumber(globalState.connections.paths[i].net, target);
      target->print("\t");
      printNodeOrName(globalState.connections.paths[i].node1, 0, globalState.connections.paths[i].net, target);
      target->print("\t");
      target->print(chipNumToChar(globalState.connections.paths[i].chip[0]));
      target->print("\t");
      target->print(globalState.connections.paths[i].x[0]);
      target->print("\t");
      target->print(globalState.connections.paths[i].y[0]);
      target->print("\t");
      printNodeOrName(globalState.connections.paths[i].node2, 0, globalState.connections.paths[i].net, target);
      target->print("\t");
      target->print(chipNumToChar(globalState.connections.paths[i].chip[1]));
      target->print("\t");
      target->print(globalState.connections.paths[i].x[1]);
      target->print("\t");
      target->print(globalState.connections.paths[i].y[1]);
      target->print("\t");
      target->print(globalState.connections.paths[i].altPathNeeded);
      target->print("\t");
      target->print(globalState.connections.paths[i].sameChip);
      target->print("\t");
      target->print(globalState.connections.paths[i].duplicate);
      target->print("\t");
      printPathType(i, target);

      if (globalState.connections.paths[i].chip[2] != -1) {
        target->print(" \t");
        target->print(chipNumToChar(globalState.connections.paths[i].chip[2]));
        target->print(" \t");
        target->print(globalState.connections.paths[i].x[2]);
        target->print(" \t");
        target->print(globalState.connections.paths[i].y[2]);
        target->print(" \t");
        target->print(globalState.connections.paths[i].x[3]);
        target->print(" \t");
        target->print(globalState.connections.paths[i].y[3]);
      }
      if (1) {
        if (globalState.connections.paths[i].chip[3] != -1) {
          target->print(" \t");
          target->print(chipNumToChar(globalState.connections.paths[i].chip[3]));
          target->print(" \t");
        }
      }

      target->println(" ");
    }

    if (showCullDupes > 0 && duplicateSection == 0 && i >= numberOfPaths - 1) {
      duplicateSection = 1;
      changeTerminalColor(-1, false, target);
      target->println("\n\rduplicates");
      i = 0;
    }
    changeTerminalColor(-1, false, target);
  }
  target->flush();
}

void printChipStatus(Stream* target) {
  target->println(
      "\n\rchip\t0    1    2    3    4    5    6    7    8    9    10   "
      "11   "
      "12   13   14   15\t\t0    1    2    3    4    5    6    7");
  for (int i = 0; i < 12; i++) {
    int spaces = 0;
    target->print(chipNumToChar(i));
    target->print("\t");
    for (int j = 0; j < 16; j++) {
      if (globalState.connections.chipStates[i].xStatus[j] == -1) {
        spaces += target->print(".");
      } else {
        changeTerminalColor(globalState.connections.nets[globalState.connections.chipStates[i].xStatus[j]].termColor, false, target);
        spaces += printNetOrNumber(globalState.connections.chipStates[i].xStatus[j], target);
        changeTerminalColor(-1, false, target);
      }
      for (int k = 0; k < 4 - spaces; k++) {
        target->print(" ");
      }
      target->print(" ");
      spaces = 0;
    }
    target->print("\t");
    for (int j = 0; j < 8; j++) {
      if (globalState.connections.chipStates[i].yStatus[j] == -1) {
        spaces += target->print(".");
      } else {
        changeTerminalColor(globalState.connections.nets[globalState.connections.chipStates[i].yStatus[j]].termColor, false, target);
        spaces += printNetOrNumber(globalState.connections.chipStates[i].yStatus[j], target);
        changeTerminalColor(-1, false, target);
      }

      for (int k = 0; k < 4 - spaces; k++) {
        target->print(" ");
      }
      target->print(" ");
      spaces = 0;
    }
    if (i == 7) {
      target->print("\n\n\rchip\t0    1    2    3    4    5    6    7    "
                   "8    9    10   "
                   "11   12   13   14   15\t\t0    1    2    3    4    5 "
                   "   6    7");
    }
    target->println(" ");
  }
  target->flush();
}

void findStartAndEndChips(int node1, int node2, int pathIdx) {
  if (debugNTCC2) {
    Serial.print("findStartAndEndChips()\n\r");
  }
  bothNodes[0] = node1;
  bothNodes[1] = node2;
  startEndChip[0] = -1;
  startEndChip[1] = -1;

  if (debugNTCC5) {
    Serial.print("finding chips for nodes: ");
    Serial.print(definesToChar(node1));
    Serial.print("-");
    Serial.println(definesToChar(node2));
  }

  for (int twice = 0; twice < 2; twice++) // first run gets node1 and start
                                          // chip, second is node2 and end
  {
    if (debugNTCC5) {
      Serial.print("node: ");
      Serial.println(twice + 1);
      Serial.println(" ");
    }
    int candidatesFound = 0;

    switch (bothNodes[twice]) {
    case 1 ... 60: // on the breadboard
    {
      globalState.connections.paths[pathIdx].chip[twice] = board::currentBoard().bbNodesToChip[bothNodes[twice]];
      if (debugNTCC5) {
        Serial.print("chip: ");
        Serial.println(chipNumToChar(globalState.connections.paths[pathIdx].chip[twice]));
      }
      break;
    }
    case NANO_D0 ... NANO_A7: // on the nano
    {
      int nanoIndex = defToNano(bothNodes[twice]);

      if (nano.numConns[nanoIndex] == 1) {
        globalState.connections.paths[pathIdx].chip[twice] = nano.mapIJ[nanoIndex];
        if (debugNTCC5) {
          Serial.print("nano chip: ");
          Serial.println(chipNumToChar(globalState.connections.paths[pathIdx].chip[twice]));
        }
      } else {
        if (debugNTCC5) {
          Serial.print("nano candidate chips: ");
        }
        chipCandidates[twice][0] = nano.mapIJ[nanoIndex];
        globalState.connections.paths[pathIdx].candidates[twice][0] = chipCandidates[twice][0];
        // Serial.print(candidatesFound);
        if (debugNTCC5) {
          Serial.print(chipNumToChar(globalState.connections.paths[pathIdx].candidates[twice][0]));
        }
        candidatesFound++;
        chipCandidates[twice][1] = nano.mapKL[nanoIndex];
        globalState.connections.paths[pathIdx].candidates[twice][1] = chipCandidates[twice][1];
        candidatesFound++;
        if (debugNTCC5) {
          Serial.print(" ");
          Serial.println(chipNumToChar(globalState.connections.paths[pathIdx].candidates[twice][1]));
        }
      }
      break;
    }
    // Virtual node expansion for FakeGPIO outputs
    // Expands FAKE_GP_OUT_x to actual voltage source based on currentState
    // NOTE: We update path.node1/node2 so routing can find x/y coordinates
    // Display functions will convert back to virtual names for display
    case FAKE_GP_OUT_0 ... FAKE_GP_OUT_7: {
      int slot = FAKE_GP_OUT_SLOT(bothNodes[twice]);
      if (slot >= 0 && slot < MAX_FAKE_GP_OUT && fakeGpioOutputs[slot].active) {
        int expandedNode = (fakeGpioOutputs[slot].currentState == 1) 
            ? fakeGpioOutputs[slot].highVoltageNode 
            : fakeGpioOutputs[slot].lowVoltageNode;
        if (debugNTCC5) {
          Serial.print("FakeGPIO OUT slot ");
          Serial.print(slot);
          Serial.print(" expanded to node ");
          Serial.println(expandedNode);
        }
        bothNodes[twice] = expandedNode;
        // Update path node so routing can find xMap coordinates
        if (twice == 0) {
          globalState.connections.paths[pathIdx].node1 = expandedNode;
        } else {
          globalState.connections.paths[pathIdx].node2 = expandedNode;
        }
      }
      // Fall through to handle expanded node as special function
    }
    // Virtual node expansion for FakeGPIO inputs
    // All inputs expand to ADC0 - only one is connected at a time via chip K switching
    case FAKE_GP_IN_0 ... FAKE_GP_IN_31: {
      // Check if this is actually a FakeGPIO input (not fallthrough from output)
      if (IS_FAKE_GP_IN(bothNodes[twice])) {
        int slot = FAKE_GP_IN_SLOT(bothNodes[twice]);
        if (slot >= 0 && slot < MAX_FAKE_GP_IN && fakeGpioInputs[slot].active) {
          // All inputs share a single ADC (dynamically selected)
          int expandedNode = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : ADC0;
          if (debugNTCC5) {
            Serial.print("FakeGPIO IN slot ");
            Serial.print(slot);
            Serial.print(" expanded to ADC");
            Serial.println(fakeGpioInputAdcChannel);
          }
          bothNodes[twice] = expandedNode;
          // Update path node so routing can find xMap coordinates
          if (twice == 0) {
            globalState.connections.paths[pathIdx].node1 = expandedNode;
          } else {
            globalState.connections.paths[pathIdx].node2 = expandedNode;
          }
        }
      }
      // Fall through to handle expanded node as special function
    }
    case GND ... 141: {
      if (debugNTCC5) {
        Serial.print("special function candidate chips: ");
      }
      for (int i = 8; i < 12; i++) {
        for (int j = 0; j < 16; j++) {
          if (globalState.connections.chipStates[i].xMap[j] == bothNodes[twice]) {
            chipCandidates[twice][candidatesFound] = i;
            globalState.connections.paths[pathIdx].candidates[twice][candidatesFound] =
                chipCandidates[twice][candidatesFound];
            candidatesFound++;
            if (debugNTCC5) {
              Serial.print(chipNumToChar(i));
              Serial.print(" ");
            }
          }
        }
      }

      if (candidatesFound == 1) {
        globalState.connections.paths[pathIdx].chip[twice] = chipCandidates[twice][0];

        globalState.connections.paths[pathIdx].candidates[twice][0] = -1;
        if (debugNTCC5) {
          Serial.print("chip: ");
          Serial.println(chipNumToChar(globalState.connections.paths[pathIdx].chip[twice]));
        }
      }
      if (debugNTCC5) {
        Serial.println(" ");
      }
      break;
    }
    }
  }
}

void mergeOverlappingCandidates(
    int pathIndex) // also sets altPathNeeded flag if theyre on different
// sf chips (there are no direct connections between
// them)
{
  // Serial.print("\t 0 \t");
  int foundOverlap = 0;

  if ((globalState.connections.paths[pathIndex].candidates[0][0] != -1 &&
       globalState.connections.paths[pathIndex].candidates[1][0] != -1)) // if both nodes have candidates
  {
    /// Serial.print("\t1");
    for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 3; j++) {
        if (globalState.connections.paths[pathIndex].candidates[0][i] ==
            globalState.connections.paths[pathIndex].candidates[1][j]) {
          // Serial.print("! \t");
          globalState.connections.paths[pathIndex].chip[0] = globalState.connections.paths[pathIndex].candidates[0][i];
          globalState.connections.paths[pathIndex].chip[1] = globalState.connections.paths[pathIndex].candidates[0][i];
          foundOverlap = 1;
          break;
        }
      }
    }
    if (foundOverlap == 0) {
      if (pathsWithCandidatesIndex < MAX_BRIDGES) {
        pathsWithCandidates[pathsWithCandidatesIndex] = pathIndex;
        pathsWithCandidatesIndex++;
      }
    }
  } else if (globalState.connections.paths[pathIndex].candidates[0][0] != -1) // if node 1 has candidates
  {
    // Serial.print("\t2");

    for (int j = 0; j < 3; j++) {
      if (globalState.connections.paths[pathIndex].chip[1] == globalState.connections.paths[pathIndex].candidates[0][j]) {
        // Serial.print("! \t");
        globalState.connections.paths[pathIndex].chip[0] = globalState.connections.paths[pathIndex].candidates[0][j];

        foundOverlap = 1;

        break;
      }
    }
    if (foundOverlap == 0) {
      if (pathsWithCandidatesIndex < MAX_BRIDGES) {
        pathsWithCandidates[pathsWithCandidatesIndex] = pathIndex;
        pathsWithCandidatesIndex++;
      }
    }

    // globalState.connections.paths[pathIndex].altPathNeeded = 1;
  } else if (globalState.connections.paths[pathIndex].candidates[1][0] != -1) // if node 2 has candidates
  {
    // Serial.print(" \t3");

    for (int j = 0; j < 3; j++) {
      if (globalState.connections.paths[pathIndex].chip[0] == globalState.connections.paths[pathIndex].candidates[1][j]) {
        // Serial.print("! \t");

        globalState.connections.paths[pathIndex].chip[1] = globalState.connections.paths[pathIndex].candidates[1][j];
        foundOverlap = 1;
        break;
      }
    }
    if (foundOverlap == 0) {
      if (pathsWithCandidatesIndex < MAX_BRIDGES) {
        pathsWithCandidates[pathsWithCandidatesIndex] = pathIndex;
        pathsWithCandidatesIndex++;
      }
    }

    // globalState.connections.paths[pathIndex].altPathNeeded = 1;
  }

  if (foundOverlap == 1) {
    globalState.connections.paths[pathIndex].candidates[0][0] = -1;
    globalState.connections.paths[pathIndex].candidates[0][1] = -1;
    globalState.connections.paths[pathIndex].candidates[0][2] = -1;
    globalState.connections.paths[pathIndex].candidates[1][0] = -1;
    globalState.connections.paths[pathIndex].candidates[1][1] = -1;
    globalState.connections.paths[pathIndex].candidates[1][2] = -1;
  } else {
  }

  //   if (globalState.connections.paths[pathIndex].chip[0] >= CHIP_I && globalState.connections.paths[pathIndex].chip[1] >=
  //   CHIP_I) {
  //     if (globalState.connections.paths[pathIndex].chip[0] != globalState.connections.paths[pathIndex].chip[1]) {

  //       globalState.connections.paths[pathIndex].altPathNeeded = 1;
  //     }
  //   }
}

void assignPathType(int pathIndex) {
  // Get nodes - expand FakeGPIO virtual nodes to actual voltage sources/ADCs for path type determination
  int node1 = globalState.connections.paths[pathIndex].node1;
  int node2 = globalState.connections.paths[pathIndex].node2;
  
  // Expand virtual nodes for path type determination
  // (path.node1/node2 still contain virtual nodes for display)
  if (IS_FAKE_GP_OUT(node1)) {
    int slot = FAKE_GP_OUT_SLOT(node1);
    if (slot >= 0 && slot < MAX_FAKE_GP_OUT && fakeGpioOutputs[slot].active) {
      node1 = (fakeGpioOutputs[slot].currentState == 1) 
          ? fakeGpioOutputs[slot].highVoltageNode 
          : fakeGpioOutputs[slot].lowVoltageNode;
    }
  } else if (IS_FAKE_GP_IN(node1)) {
    int slot = FAKE_GP_IN_SLOT(node1);
    if (slot >= 0 && slot < MAX_FAKE_GP_IN && fakeGpioInputs[slot].active) {
      // All inputs share a single ADC (dynamically selected)
      node1 = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : ADC0;
    }
  }
  
  if (IS_FAKE_GP_OUT(node2)) {
    int slot = FAKE_GP_OUT_SLOT(node2);
    if (slot >= 0 && slot < MAX_FAKE_GP_OUT && fakeGpioOutputs[slot].active) {
      node2 = (fakeGpioOutputs[slot].currentState == 1) 
          ? fakeGpioOutputs[slot].highVoltageNode 
          : fakeGpioOutputs[slot].lowVoltageNode;
    }
  } else if (IS_FAKE_GP_IN(node2)) {
    int slot = FAKE_GP_IN_SLOT(node2);
    if (slot >= 0 && slot < MAX_FAKE_GP_IN && fakeGpioInputs[slot].active) {
      // All inputs share a single ADC (dynamically selected)
      node2 = (fakeGpioInputAdcChannel >= 0) ? (ADC0 + fakeGpioInputAdcChannel) : ADC0;
    }
  }
  
  if (globalState.connections.paths[pathIndex].chip[0] == globalState.connections.paths[pathIndex].chip[1]) {
    globalState.connections.paths[pathIndex].sameChip = true;
  } else {
    globalState.connections.paths[pathIndex].sameChip = false;
  }

  if ((globalState.connections.paths[pathIndex].node1 == 29 || globalState.connections.paths[pathIndex].node1 == 59 ||
       globalState.connections.paths[pathIndex].node1 == 30 || globalState.connections.paths[pathIndex].node1 == 60) ||
      globalState.connections.paths[pathIndex].node1 == 114 || globalState.connections.paths[pathIndex].node1 == 116 ||
      globalState.connections.paths[pathIndex].node1 == 117) {
    // Serial.print("\n\n\rthis should be a bb to sf connection\n\n\n\r
    // ");
    //  globalState.connections.paths[pathIndex].altPathNeeded = true;
    // Serial.print("path ");
    // Serial.print(pathIndex);
    // Serial.print(" is a bb to sf connection, swapping\n\r");
    // Serial.print("node1: ");
    // Serial.print(globalState.connections.paths[pathIndex].node1);
    // Serial.print("\tnode2: ");
    // Serial.print(globalState.connections.paths[pathIndex].node2);
    // Serial.println("\n\r");
    swapNodes(pathIndex);
    //     Serial.print("path ");
    // Serial.print(pathIndex);
    // Serial.print(" is a bb to sf connection, swapping\n\r");
    // Serial.print("node1: ");
    // Serial.print(globalState.connections.paths[pathIndex].node1);
    // Serial.print("\tnode2: ");
    // Serial.print(globalState.connections.paths[pathIndex].node2);
    // Serial.println("\n\r");
    // globalState.connections.paths[pathIndex].Lchip = true;

    globalState.connections.paths[pathIndex].nodeType[0] = SF; // maybe have a separate type for ChipL
    // connected nodes, but not now
  }

  if ((globalState.connections.paths[pathIndex].node1 >= 1 && globalState.connections.paths[pathIndex].node1 <= 28) ||
      (globalState.connections.paths[pathIndex].node1 >= 31 && globalState.connections.paths[pathIndex].node1 <= 58)) {
    globalState.connections.paths[pathIndex].nodeType[0] = BB;
  } else if (globalState.connections.paths[pathIndex].node1 >= NANO_D0 &&
             globalState.connections.paths[pathIndex].node1 <= NANO_A7) {
    globalState.connections.paths[pathIndex].nodeType[0] = NANO;
  } else if (globalState.connections.paths[pathIndex].node1 >= GND && globalState.connections.paths[pathIndex].node1 <= 141) {
    globalState.connections.paths[pathIndex].nodeType[0] = SF;
  }

  if ((globalState.connections.paths[pathIndex].node2 == 29 || globalState.connections.paths[pathIndex].node2 == 59 ||
       globalState.connections.paths[pathIndex].node2 == 30 || globalState.connections.paths[pathIndex].node2 == 60) ||
      globalState.connections.paths[pathIndex].node2 == 114 || globalState.connections.paths[pathIndex].node2 == 116 ||
      globalState.connections.paths[pathIndex].node2 == 117 || globalState.connections.paths[pathIndex].chip[1] == CHIP_K) {
    // Serial.print("\n\n\rthis should be a bb to sf connection 2\n\n\n\r
    // "); globalState.connections.paths[pathIndex].altPathNeeded = true; globalState.connections.paths[pathIndex].Lchip =
    // true;
    globalState.connections.paths[pathIndex].nodeType[1] = SF;
  } else if ((globalState.connections.paths[pathIndex].node2 >= 1 && globalState.connections.paths[pathIndex].node2 <= 28) ||
             (globalState.connections.paths[pathIndex].node2 >= 31 && globalState.connections.paths[pathIndex].node2 <= 58)) {
    globalState.connections.paths[pathIndex].nodeType[1] = BB;
  } else if (globalState.connections.paths[pathIndex].node2 >= NANO_D0 &&
             globalState.connections.paths[pathIndex].node2 <= NANO_A7) {
    globalState.connections.paths[pathIndex].nodeType[1] = NANO;
  } else if (globalState.connections.paths[pathIndex].node2 >= GND && globalState.connections.paths[pathIndex].node2 <= 141) {
    globalState.connections.paths[pathIndex].nodeType[1] = SF;
  }

  if ((globalState.connections.paths[pathIndex].nodeType[0] == NANO &&
       globalState.connections.paths[pathIndex].nodeType[1] == SF)) {
    globalState.connections.paths[pathIndex].pathType = NANOtoSF;
    if (globalState.connections.paths[pathIndex].chip[0] != globalState.connections.paths[pathIndex].chip[1]) {
      globalState.connections.paths[pathIndex].altPathNeeded = true;
    }
  } else if ((globalState.connections.paths[pathIndex].nodeType[0] == SF &&
              globalState.connections.paths[pathIndex].nodeType[1] == SF)) {
    globalState.connections.paths[pathIndex].pathType =
        NANOtoSF; // SFtoSF is dealt with the same as NANOtoSF
    // Serial.print("pathIndex: ");
    // Serial.println(pathIndex);
    // Serial.print("globalState.connections.paths[pathIndex].pathType: ");
    // Serial.println(globalState.connections.paths[pathIndex].pathType);
    globalState.connections.paths[pathIndex].altPathNeeded = true;
  } else if ((globalState.connections.paths[pathIndex].nodeType[0] == SF &&
              globalState.connections.paths[pathIndex].nodeType[1] == NANO)) {
    // swapNodes(pathIndex);
    globalState.connections.paths[pathIndex].pathType = NANOtoSF;
    if (globalState.connections.paths[pathIndex].chip[0] != globalState.connections.paths[pathIndex].chip[1]) {
      globalState.connections.paths[pathIndex].altPathNeeded = true;
    }

    // globalState.connections.paths[pathIndex].altPathNeeded = true;
  } else if ((globalState.connections.paths[pathIndex].nodeType[0] == BB &&
              globalState.connections.paths[pathIndex].nodeType[1] == SF)) {
    globalState.connections.paths[pathIndex].pathType = BBtoSF;
  } else if ((globalState.connections.paths[pathIndex].nodeType[0] == SF &&
              globalState.connections.paths[pathIndex].nodeType[1] == BB)) {
    swapNodes(pathIndex);
    globalState.connections.paths[pathIndex].pathType = BBtoSF;
  } else if ((globalState.connections.paths[pathIndex].nodeType[0] == BB &&
              globalState.connections.paths[pathIndex].nodeType[1] == NANO)) {
    globalState.connections.paths[pathIndex].pathType = BBtoNANO;
  } else if (globalState.connections.paths[pathIndex].nodeType[0] == NANO &&
             globalState.connections.paths[pathIndex].nodeType[1] ==
                 BB) // swtich node order so BB always comes first
  {
    swapNodes(pathIndex);
    globalState.connections.paths[pathIndex].pathType = BBtoNANO;
  } else if (globalState.connections.paths[pathIndex].nodeType[0] == BB &&
             globalState.connections.paths[pathIndex].nodeType[1] == BB) {
    globalState.connections.paths[pathIndex].pathType = BBtoBB;
  } else if (globalState.connections.paths[pathIndex].nodeType[0] == NANO &&
             globalState.connections.paths[pathIndex].nodeType[1] == NANO) {
    globalState.connections.paths[pathIndex].pathType = NANOtoNANO;
  }
  if (debugNTCC) {
    Serial.print("Path ");
    Serial.print(pathIndex);
    Serial.print(" type: ");
    printPathType(pathIndex);
    Serial.print("\n\r");

    Serial.print("  Node 1: ");
    Serial.print(globalState.connections.paths[pathIndex].node1);
    Serial.print("\tNode 2: ");
    Serial.print(globalState.connections.paths[pathIndex].node2);
    Serial.print("\n\r");

    Serial.print("  Chip 1: ");
    Serial.print(globalState.connections.paths[pathIndex].chip[0]);
    Serial.print("\tChip 2: ");
    Serial.print(globalState.connections.paths[pathIndex].chip[1]);
    Serial.print("\n\r");
  }
}

void swapNodes(int pathIndex) {
  int temp = 0;
  temp = globalState.connections.paths[pathIndex].node1;
  globalState.connections.paths[pathIndex].node1 = globalState.connections.paths[pathIndex].node2;
  globalState.connections.paths[pathIndex].node2 = temp;

  temp = globalState.connections.paths[pathIndex].chip[0];
  globalState.connections.paths[pathIndex].chip[0] = globalState.connections.paths[pathIndex].chip[1];
  globalState.connections.paths[pathIndex].chip[1] = temp;

  temp = globalState.connections.paths[pathIndex].candidates[0][0];
  globalState.connections.paths[pathIndex].candidates[0][0] = globalState.connections.paths[pathIndex].candidates[1][0];
  globalState.connections.paths[pathIndex].candidates[1][0] = temp;

  temp = globalState.connections.paths[pathIndex].candidates[0][1];
  globalState.connections.paths[pathIndex].candidates[0][1] = globalState.connections.paths[pathIndex].candidates[1][1];
  globalState.connections.paths[pathIndex].candidates[1][1] = temp;

  temp = globalState.connections.paths[pathIndex].candidates[0][2];
  globalState.connections.paths[pathIndex].candidates[0][2] = globalState.connections.paths[pathIndex].candidates[1][2];
  globalState.connections.paths[pathIndex].candidates[1][2] = temp;

  enum nodeType tempNT = globalState.connections.paths[pathIndex].nodeType[0];
  globalState.connections.paths[pathIndex].nodeType[0] = globalState.connections.paths[pathIndex].nodeType[1];
  globalState.connections.paths[pathIndex].nodeType[1] = tempNT;

  temp = globalState.connections.paths[pathIndex].x[0];
  globalState.connections.paths[pathIndex].x[0] = globalState.connections.paths[pathIndex].x[1];
  globalState.connections.paths[pathIndex].x[1] = temp;

  temp = globalState.connections.paths[pathIndex].y[0];
  globalState.connections.paths[pathIndex].y[0] = globalState.connections.paths[pathIndex].y[1];
  globalState.connections.paths[pathIndex].y[1] = temp;
}

int xMapForNode(int node, int chip) {
  int nodeFound = -1;
  for (int i = 0; i < 16; i++) {
    if (globalState.connections.chipStates[chip].xMap[i] == node) {
      nodeFound = i;
      break;
    }
  }
  if (nodeFound == -1) {
    if (debugNTCC) {
      Serial.print("xMapForNode: \n\rnode ");
      Serial.print(node);
      Serial.print(" not found on chip ");
      Serial.println(chipNumToChar(chip));
    }
  }

  return nodeFound;
}

int yMapForNode(int node, int chip) {
  int nodeFound = -1;
  for (int i = 1; i < 8; i++) {
    if (globalState.connections.chipStates[chip].yMap[i] == node) {
      nodeFound = i;
      break;
    }
  }
  return nodeFound;
}

int xMapForChipLane0(int chip1, int chip2) {
  int nodeFound = -1;
  for (int i = 0; i < 16; i++) {
    if (globalState.connections.chipStates[chip1].xMap[i] == chip2) {
      nodeFound = i;
      break;
    }
  }
  return nodeFound;
}
int xMapForChipLane1(int chip1, int chip2) {
  int nodeFound = -1;
  for (int i = 0; i < 15; i++) {   // i+1 below: 15 is the last valid pair start
    if (globalState.connections.chipStates[chip1].xMap[i] == chip2) {
      if (globalState.connections.chipStates[chip1].xMap[i + 1] == chip2) {
        nodeFound = i + 1;
        break;
      }
    }
  }

  if (nodeFound == -1) {
    if (debugNTCC) {
      Serial.print("nodeNotFound lane 1: ");
      Serial.print(chipNumToChar(chip1));
      Serial.print(" ");
      Serial.println(chipNumToChar(chip2));
    }
  }

  return nodeFound;
}

int gndChipAlternator = 0; // Static counter for alternating GND chips

void resolveChipCandidates(int startIndex) {
  int nodesToResolve[2] = {
      0, 0}; // {node1,node2} 0 = already found, 1 = needs resolving

  

  for (int pathIndex = startIndex; pathIndex < numberOfPaths; pathIndex++) {
    // Skip virtual paths
    if (globalState.connections.paths[pathIndex].pathType == VIRTUAL) {
      continue;
    }
    
    // For duplicate path handling with stack_rails > 0, only process GND paths
    // Return early if not GND net since it's the only one with multiple routes
    if (globalState.connections.paths[pathIndex].duplicate == 1 && jumperlessConfig.routing.stack_rails > 0) {
      if (globalState.connections.paths[pathIndex].net != 1) {
        continue; // Skip non-GND nets for duplicate path processing when stacking
      }
      
      if (debugNTCC) {
        Serial.print("Processing GND duplicate globalState.connections.paths[");
        Serial.print(pathIndex);
        Serial.println("] with stacking enabled");
      }
    }

    nodesToResolve[0] = 0;
    nodesToResolve[1] = 0;

    if (globalState.connections.paths[pathIndex].chip[0] == -1) {
      nodesToResolve[0] = 1;
    } else {
      nodesToResolve[0] = 0;
    }

    if (globalState.connections.paths[pathIndex].chip[1] == -1) {
      nodesToResolve[1] = 1;
    } else {
      nodesToResolve[1] = 0;
    }

    for (int nodeOneOrTwo = 0; nodeOneOrTwo < 2; nodeOneOrTwo++) {
      if (nodesToResolve[nodeOneOrTwo] == 1) {
        // Check if this is a GND path (net 1)
        bool isGndPath = (globalState.connections.paths[pathIndex].net == 1);
        bool isGndDuplicateWithStacking = (isGndPath && globalState.connections.paths[pathIndex].duplicate == 1 && jumperlessConfig.routing.stack_rails > 0);
        int selectedChip = -1;

        if (isGndPath) {
          // Special handling for GND paths to balance between chips K and L
          int chipK = -1, chipL = -1;
          
          // Find K and L in the candidates
          for (int candIdx = 0; candIdx < 3; candIdx++) {
            if (globalState.connections.paths[pathIndex].candidates[nodeOneOrTwo][candIdx] == CHIP_K) {
              chipK = CHIP_K;
            } else if (globalState.connections.paths[pathIndex].candidates[nodeOneOrTwo][candIdx] == CHIP_L) {
              chipL = CHIP_L;
            }
          }

          if (chipK != -1 && chipL != -1) {
            // Both K and L are candidates
            if (isGndDuplicateWithStacking) {
              // For GND duplicate paths with stacking enabled, ensure we use both chips
              // Use the opposite chip from what was chosen for the primary path
              bool primaryUsesK = false, primaryUsesL = false;
              
              // Check what chips are already used by non-duplicate GND paths
              for (int checkPath = 0; checkPath < pathIndex; checkPath++) {
                if (globalState.connections.paths[checkPath].net == 1 && globalState.connections.paths[checkPath].duplicate == 0) {
                  if (globalState.connections.paths[checkPath].chip[0] == CHIP_K || globalState.connections.paths[checkPath].chip[1] == CHIP_K) {
                    primaryUsesK = true;
                  }
                  if (globalState.connections.paths[checkPath].chip[0] == CHIP_L || globalState.connections.paths[checkPath].chip[1] == CHIP_L) {
                    primaryUsesL = true;
                  }
                }
              }
              
              // For duplicates, prefer the chip that's less used, or alternate if both are used
              if (primaryUsesK && !primaryUsesL) {
                selectedChip = chipL;
              } else if (primaryUsesL && !primaryUsesK) {
                selectedChip = chipK;
              } else {
                // Both or neither used, alternate
                selectedChip = (gndChipAlternator % 2 == 0) ? chipK : chipL;
                gndChipAlternator++;
              }
              
              if (debugNTCC) {
                Serial.print("GND duplicate globalState.connections.paths[");
                Serial.print(pathIndex);
                Serial.print("] stacking enabled, selected chip ");
                Serial.print(chipNumToChar(selectedChip));
                Serial.print(" (primaryUsesK=");
                Serial.print(primaryUsesK);
                Serial.print(", primaryUsesL=");
                Serial.print(primaryUsesL);
                Serial.println(")");
              }
            } else if (jumperlessConfig.routing.stack_rails > 0) {
              // When stacking is enabled, prefer the less crowded chip but allow both
              selectedChip = moreAvailableChip(chipK, chipL);
              
              if (debugNTCC) {
                Serial.print("GND globalState.connections.paths[");
                Serial.print(pathIndex);
                Serial.print("] stacking enabled, selected chip ");
                Serial.print(chipNumToChar(selectedChip));
                Serial.println(" (less crowded)");
              }
            } else {
              // Alternate between K and L when not stacking
              selectedChip = (gndChipAlternator % 2 == 0) ? chipK : chipL;
              gndChipAlternator++;
              
              if (debugNTCC) {
                Serial.print("GND globalState.connections.paths[");
                Serial.print(pathIndex);
                Serial.print("] alternating to chip ");
                Serial.print(chipNumToChar(selectedChip));
                Serial.print(" (alternator: ");
                Serial.print(gndChipAlternator - 1);
                Serial.println(")");
              }
            }
          } else if (chipK != -1) {
            selectedChip = chipK;
          } else if (chipL != -1) {
            selectedChip = chipL;
          } else {
            // Fall back to standard selection if neither K nor L found
            selectedChip = moreAvailableChip(globalState.connections.paths[pathIndex].candidates[nodeOneOrTwo][0],
                                           globalState.connections.paths[pathIndex].candidates[nodeOneOrTwo][1]);
          }
        } else {
          // Standard chip selection for non-GND paths
          selectedChip = moreAvailableChip(globalState.connections.paths[pathIndex].candidates[nodeOneOrTwo][0],
                                         globalState.connections.paths[pathIndex].candidates[nodeOneOrTwo][1]);
        }

        globalState.connections.paths[pathIndex].chip[nodeOneOrTwo] = selectedChip;
        
        if (debugNTCC && !isGndPath) {
          Serial.print("globalState.connections.paths[");
          Serial.print(pathIndex);
          Serial.print("] chip from ");
          Serial.print(
              chipNumToChar(globalState.connections.paths[pathIndex].chip[(1 + nodeOneOrTwo) % 2]));
          Serial.print(" to chip ");
          Serial.print(chipNumToChar(globalState.connections.paths[pathIndex].chip[nodeOneOrTwo]));
          Serial.print(" chosen\n\n\r");
        }
      }
    }
  }
}

int moreAvailableChip(int chip1, int chip2) {
  int chipChosen = -1;
  sortSFchipsLeastToMostCrowded();
  sortAllChipsLeastToMostCrowded();

  for (int i = 0; i < 12; i++) {
    if (chipsLeastToMostCrowded[i] == chip1 ||
        chipsLeastToMostCrowded[i] == chip2) {
      chipChosen = chipsLeastToMostCrowded[i];
      break;
    }
  }
  return chipChosen;
}

void sortSFchipsLeastToMostCrowded(void) {
  bool tempDebug = debugNTCC;
  // debugNTCC = false;
  int numberOfConnectionsPerSFchip[4] = {0, 0, 0, 0};

  for (int i = 0; i < numberOfPaths; i++) {
    for (int j = 0; j < 2; j++) {
      if (globalState.connections.paths[i].chip[j] > 7) {
        numberOfConnectionsPerSFchip[globalState.connections.paths[i].chip[j] - 8]++;
      }
    }
  }

  if (debugNTCC) {
    for (int i = 0; i < 4; i++) {
      Serial.print("sf connections: ");
      Serial.print(chipNumToChar(i + 8));
      Serial.print(numberOfConnectionsPerSFchip[i]);
      Serial.print("\n\r");
    }
  }
  // debugNTCC = tempDebug;
}

void sortAllChipsLeastToMostCrowded(void) {
  // bool tempDebug = debugNTCC;
  // debugNTCC = false;

  int numberOfConnectionsPerChip[12] = {
      0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0}; // this will be used to determine which chip is most
  // crowded

  for (int i = 0; i < 12; i++) {
    chipsLeastToMostCrowded[i] = i;
  }

  if (debugNTCC) {
    // Serial.println("\n\r");
  }
  for (int i = 0; i < numberOfPaths; i++) {
    for (int j = 0; j < 2; j++) {
      if (globalState.connections.paths[i].chip[j] != -1) {
        numberOfConnectionsPerChip[globalState.connections.paths[i].chip[j]]++;
      }
    }
  }

  // debugNTCC = false;
  if (debugNTCC5) {
    for (int i = 0; i < 12; i++) {
      Serial.print(chipNumToChar(i));
      Serial.print(": ");
      Serial.println(numberOfConnectionsPerChip[i]);
    }

    Serial.println("\n\r");
  }

  int temp = 0;

  for (int i = 0; i < 12; i++) {
    for (int j = 0; j < 11; j++) {
      if (numberOfConnectionsPerChip[j] > numberOfConnectionsPerChip[j + 1]) {
        temp = numberOfConnectionsPerChip[j];
        // chipsLeastToMostCrowded[j] = chipsLeastToMostCrowded[j + 1];
        numberOfConnectionsPerChip[j] = numberOfConnectionsPerChip[j + 1];
        numberOfConnectionsPerChip[j + 1] = temp;

        temp = chipsLeastToMostCrowded[j];
        chipsLeastToMostCrowded[j] = chipsLeastToMostCrowded[j + 1];
        chipsLeastToMostCrowded[j + 1] = temp;
      }
    }
  }

  for (int i = 0; i < 12; i++) {
    if (debugNTCC5) {
      Serial.print(chipNumToChar(chipsLeastToMostCrowded[i]));
      Serial.print(": ");
      Serial.println(numberOfConnectionsPerChip[i]);
    }
  }

  /*
      if (debugNTCC == true)
      {
          for (int i = 0; i < 4; i++)
          {
              Serial.print("\n\r");
              Serial.print(chipNumToChar(sfChipsLeastToMostCrowded[i]));
              Serial.print(": ");

              Serial.print("\n\r");
          }
      }
  */
  // debugNTCC = tempDebug;
  //  bbToSfConnections();
}

void printPathArray(void) // this also prints candidates and x y
{
  // Serial.print("\n\n\r");
  // Serial.print("newBridgeIndex = ");
  // Serial.println(newBridgeIndex);
  Serial.print("\n\r");
  int tabs = 0;
  int lineCount = 0;
  for (int i = 0; i < numberOfPaths; i++) {
    Serial.print("\n\r");
    tabs += Serial.print(i);
    Serial.print("  ");
    if (i < 10) {
      tabs += Serial.print(" ");
    }
    if (i < 100) {
      tabs += Serial.print(" ");
    }
    tabs += Serial.print("[");
    tabs += printNodeOrName(globalState.connections.paths[i].node1);
    tabs += Serial.print("-");
    tabs += printNodeOrName(globalState.connections.paths[i].node2);
    tabs += Serial.print("]\tNet ");
    tabs += printNodeOrName(globalState.connections.paths[i].net);
    tabs += Serial.println(" ");
    tabs += Serial.print("\n\rnode1 chip:  ");
    tabs += printChipNumToChar(globalState.connections.paths[i].chip[0]);
    tabs += Serial.print("\n\rnode2 chip:  ");
    tabs += printChipNumToChar(globalState.connections.paths[i].chip[1]);
    // tabs += Serial.print("\n\n\rnode1 candidates: ");
    // for (int j = 0; j < 3; j++) {
    //   printChipNumToChar(globalState.connections.paths[i].candidates[0][j]);
    //   tabs += Serial.print(" ");
    // }
    // tabs += Serial.print("\n\rnode2 candidates: ");
    // for (int j = 0; j < 3; j++) {
    //   printChipNumToChar(globalState.connections.paths[i].candidates[1][j]);
    //   tabs += Serial.print(" ");
    // }
    tabs += Serial.print("\n\rpath type: ");
    tabs += printPathType(i);

    if (globalState.connections.paths[i].altPathNeeded == true) {
      tabs += Serial.print("\n\ralt path needed");
    } else {
    }
    tabs += Serial.println("\n\n\r");

    /// Serial.print(tabs);
    for (int i = 0; i < 24 - (tabs); i++) {
      Serial.print(" ");
    }
    tabs = 0;
  }
}

int printPathType(int pathIndex, Stream* target) {
  switch (globalState.connections.paths[pathIndex].pathType) {
  case 0:
    return target->print("BB to BB");
    break;
  case 1:
    return target->print("BB to NANO");
    break;
  case 2:
    return target->print("NANO to NANO");
    break;
  case 3:
    return target->print("BB to SF");
    break;
  case 4:
    return target->print("NANO to SF");
    break;
  case 10:
    return target->print("VIRTUAL");
    break;
  default:
    return target->print("Not Assigned");
    break;
  }
}

int defToNano(int nanoIndex) { return nanoIndex - NANO_D0; }

char chipNumToChar(int chipNumber) { return chipNumber + 'A'; }

int printChipNumToChar(int chipNumber) {
  return Serial.print(chipNumber);
  if (chipNumber == -1) {
    return Serial.print("-1");
  } else {
    return Serial.print((char)(chipNumber + 'A'));
  }
}

void clearChipsOnPathToNegOne(void) {
  // OPTIMIZATION: Only clear paths we actually have + small buffer
  // Don't iterate through all MAX_BRIDGES (192)!
  int pathsToClear = numberOfPaths + 8;  // Small safety margin
  if (pathsToClear > MAX_BRIDGES - 1) pathsToClear = MAX_BRIDGES - 1;
  
  for (int i = 0; i < pathsToClear; i++) {
    if (i >= numberOfPaths) {
      globalState.connections.paths[i].node1 = 0;
      globalState.connections.paths[i].node2 = 0;
      globalState.connections.paths[i].net = 0;
    }
    for (int c = 0; c < 4; c++) {
      globalState.connections.paths[i].chip[c] = -1;
    }

    for (int c = 0; c < 6; c++) {
      globalState.connections.paths[i].x[c] = -1;
      globalState.connections.paths[i].y[c] = -1;
    }

    for (int c = 0; c < 3; c++) {
      globalState.connections.paths[i].candidates[c][0] = -1;
      globalState.connections.paths[i].candidates[c][1] = -1;
      globalState.connections.paths[i].candidates[c][2] = -1;
    }
  }
}




/*
So the nets are all made, now we need to figure out which chip connections
need to be made to make that phycially happen

start with the special function nets, they're the highest priority

maybe its simpler to make an array of every possible connection


start at net 1 and go up

find start and end chip

bb chips
sf chips
nano chips


things that store x and y valuse for paths
chipStatus.xStatus[]
chipStatus.yStatus[]
nanoStatus.xStatusIJKL[]


struct nanoStatus {  //there's only one of these so ill declare and initalize
together unlike above

//all these arrays should line up (both by index and visually) so one index
will give you all this data

//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const char *pinNames[24]=  {
" D0",   " D1",   " D2",   " D3",   " D4",   " D5",   " D6",   " D7",   " D8",
" D9",    "D10",    "D11",     "D12",    "D13",      "RST",     "REF",   "
A0", " A1",   " A2",   " A3",   " A4",   " A5",   " A6",   " A7"};// String
with readable name //padded to 3 chars (space comes before chars)
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t pinMap[24] =  {
NANO_D0, NANO_D1, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7,
NANO_D8, NANO_D9, NANO_D10, NANO_D11,  NANO_D12, NANO_D13, NANO_RESET,
NANO_AREF, NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6,
NANO_A7};//Array index to internal arbitrary #defined number
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t numConns[24]= {
1 , 1      , 2      , 2      , 2      , 2      , 2      , 2      , 2      , 2
, 2 , 2       ,  2       , 2       , 1         , 1        , 2      , 2      ,
2 , 2 , 2      , 2      , 1      , 1      };//Whether this pin has 1 or 2
connections to special function chips    (OR maybe have it be a map like i = 2
j = 3  k = 4 l = 5 if there's 2 it's the product of them ij = 6  ik = 8  il =
10 jk = 12 jl = 15 kl = 20 we're trading minuscule amounts of CPU for RAM)
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t  mapIJ[24] =  {
CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J
, CHIP_I , CHIP_J  , CHIP_I  ,  CHIP_J  , CHIP_I  , CHIP_I    ,  CHIP_J  ,
CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J , CHIP_I , CHIP_J
};//Since there's no overlapping connections between Chip I and J, this holds
which of those 2 chips has a connection at that index, if numConns is 1, you
only need to check this one const int8_t  mapKL[24] =  { -1     , -1     ,
CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K , CHIP_K
, CHIP_K  ,  CHIP_K  , -1 , -1        , -1       , CHIP_K , CHIP_K , CHIP_K ,
CHIP_K , CHIP_L , CHIP_L , -1     , -1     };//Since there's no overlapping
connections between Chip K and L, this holds which of those 2 chips has a
connection at that index, -1 for no connection
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapI[24]  =  {
-1 , 1      , -1     , 3      , -1     , 5      , -1     , 7      , -1     , 9
, -1 , 8       ,  -1      , 10      , 11        , -1       , 0      , -1     ,
2 , -1 , 4      , -1     , 6      , -1     };//holds which X pin is connected
to the index on Chip I, -1 for none int8_t xStatusI[24]  =  { -1     , 0 , -1
, 0 , -1     , 0      , -1     , 0      , -1     , 0      , -1      , 0 ,  -1
, 0       , 0         , -1       , 0      , -1     , 0      , -1     , 0 , -1
, 0      , -1     };//-1 for not connected to that chip, 0 for available, >0
means it's connected and the netNumber is stored here
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapJ[24]  =  {
0 , -1     , 2      , -1     , 4      , -1     , 6      , -1     , 8      , -1
, 9       , -1      ,  10      , -1      , -1        , 11       , -1     , 1 ,
-1 , 3      , -1     , 5      , -1     , 7      };//holds which X pin is
connected to the index on Chip J, -1 for none int8_t xStatusJ[24]  =  { 0 , -1
, 0      , -1     , 0      , -1     , 0      , -1     , 0      , -1     , 0 ,
-1 , 0        , 0       , -1        , 0        , -1     , 0      , -1     , 0
, -1 , 0      , -1     , 0      };//-1 for not connected to that chip, 0 for
available, >0 means it's connected and the netNumber is stored here
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapK[24]  =  {
-1 , -1     , 4      , 5      , 6      , 7      , 8      , 9      , 10     ,
11 , 12      , 13      ,  14      , -1      , -1        , -1       , 0      ,
1 , 2 , 3      , -1     , -1     , -1     , -1     };//holds which X pin is
connected to the index on Chip K, -1 for none int8_t xStatusK[24]  =  { -1 ,
-1     , 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0 , 0
, 0 , 0       , -1      , -1        , -1       , 0      , 0      , 0      , 0
, -1     , -1     , -1     , -1     };//-1 for not connected to that chip, 0
for available, >0 means it's connected and the netNumber is stored here
//                         |        |        |        |        |        | | |
| |        |         |         |          |         |           |          | |
| |        |        |        |        |        | const int8_t xMapL[24]  =  {
-1 , -1     , -1     , -1     , -1     , -1     , -1     , -1     , -1     ,
-1 , -1      , -1      ,  -1      , -1      , -1        , -1       , -1     ,
-1 , -1 , -1     , 12     , 13     , -1     , -1     };//holds which X pin is
connected to the index on Chip L, -1 for none int8_t xStatusL[24]  =  { -1 ,
-1     , -1     , -1     , -1     , -1     , -1     , -1     , -1     , -1 ,
-1 , -1 ,  -1      , -1      , -1        , -1       , -1     , -1     , -1 ,
-1 , 0 , 0      , -1     , -1     };//-1 for not connected to that chip, 0 for
available, >0 means it's connected and the netNumber is stored here

// mapIJKL[]     will tell you whethrer there's a connection from that nano
pin to the corresponding special function chip
// xMapIJKL[]    will tell you the X pin that it's connected to on that sf
chip
// xStatusIJKL[] says whether that x pin is being used (this should be the
same as mt[8-10].xMap[] if theyre all stacked on top of each other)
//              I haven't decided whether to make this just a flag, or store
that signal's destination const int8_t reversePinMap[110] = {NANO_D0, NANO_D1,
NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9,
NANO_D10, NANO_D11, NANO_D12, NANO_D13, NANO_RESET, NANO_AREF, NANO_A0,
NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6,
NANO_A7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,GND,101,102,SUPPLY_3V3,104,SUPPLY_5V,DAC0,DAC1_8V,ISENSE_PLUS,ISENSE_MINUS};

};

struct netStruct globalState.connections.nets[MAX_NETS] = { //these are the special function nets that
will always be made
//netNumber,       ,netName          ,memberNodes[] ,memberBridges[][2]
,specialFunction        ,intsctNet[] ,doNotIntersectNodes[] ,priority { 127
,"Empty Net"      ,{EMPTY_NET}           ,{{}}                   ,EMPTY_NET
,{}
,{EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET,EMPTY_NET} , 0},
    {     1        ,"GND\t"          ,{GND}                 ,{{}} ,GND ,{}
,{SUPPLY_3V3,SUPPLY_5V,DAC0,DAC1_8V}    , 1}, {     2        ,"+5V\t"
,{SUPPLY_5V}           ,{{}}                   ,SUPPLY_5V              ,{}
,{GND,SUPPLY_3V3,DAC0,DAC1_8V}          , 1}, {     3        ,"+3.3V\t"
,{SUPPLY_3V3}          ,{{}}                   ,SUPPLY_3V3             ,{}
,{GND,SUPPLY_5V,DAC0,DAC1_8V}           , 1}, {     4        ,"DAC 0\t"
,{DAC0}
,{{}}                   ,DAC0                ,{}
,{GND,SUPPLY_5V,SUPPLY_3V3,DAC1_8V}        , 1}, {     5        ,"DAC 1\t"
,{DAC1_8V}             ,{{}}                   ,DAC1_8V                ,{}
,{GND,SUPPLY_5V,SUPPLY_3V3,DAC0}        , 1}, {     6        ,"I Sense +"
,{ISENSE_PLUS}  ,{{}}                   ,ISENSE_PLUS     ,{} ,{ISENSE_MINUS} ,
2}, {     7        ,"I Sense -"      ,{ISENSE_MINUS} ,{{}} ,ISENSE_MINUS ,{}
,{ISENSE_PLUS}                      , 2},
};



Index   Name            Number          Nodes                   Bridges Do Not
Intersects 0       Empty Net       127             EMPTY_NET {0-0} EMPTY_NET 1
GND             1               GND,1,2,D0,3,4 {1-GND,1-2,D0-1,2-3,3-4}
3V3,5V,DAC_0,DAC_1 2       +5V             2 5V,11,12,10,9
{11-5V,11-12,10-11,9-10}        GND,3V3,DAC_0,DAC_1 3 +3.3V           3
3V3,D10,D11,D12 {D10-3V3,D10-D11,D11-D12} GND,5V,DAC_0,DAC_1 4       DAC 0 4
DAC_0 {0-0} GND,5V,3V3,DAC_1 5       DAC 1           5               DAC_1
{0-0} GND,5V,3V3,DAC_0 6       I Sense +       6 I_POS,6,5,A1,AREF
{6-I_POS,5-6,A1-5,AREF-A1}      I_NEG 7       I Sense -       7 I_NEG {0-0}
I_POS

Index   Name            Number          Nodes                   Bridges Do Not
Intersects 8       Net 8           8               7,8 {7-8} 0 9       Net 9
9               D13,D1,A7 {D13-D1,D13-A7} 0




struct chipStatus{

int chipNumber;
char chipChar;
int8_t xStatus[16]; //store the bb row or nano conn this is eventually
connected to so they can be stacked if conns are redundant int8_t yStatus[8];
//store the row/nano it's connected to const int8_t xMap[16]; const int8_t
yMap[8];

};



struct chipStatus globalState.connections.chipStates[12] = {
  {0,'A',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_I, CHIP_J, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E,
CHIP_K, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H},//X MAP constant
  {CHIP_L,  t2,t3, t4, t5, t6, t7, t8}},  // Y MAP constant

  {1,'B',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_I, CHIP_J, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_F, CHIP_K, CHIP_G, CHIP_G, CHIP_H, CHIP_H}, {CHIP_L,
t9,t10,t11,t12,t13,t14,t15}},

  {2,'C',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_I, CHIP_J, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_K, CHIP_H, CHIP_H}, {CHIP_L,
t16,t17,t18,t19,t20,t21,t22}},

  {3,'D',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_I, CHIP_J, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_K}, {CHIP_L,
t23,t24,t25,t26,t27,t28,t29}},

  {4,'E',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_K, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_I,
CHIP_J, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_H, CHIP_H}, {CHIP_L,   b2, b3,
b4, b5, b6, b7, b8}},

  {5,'F',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_K, CHIP_C, CHIP_C, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_I, CHIP_J, CHIP_G, CHIP_G, CHIP_H, CHIP_H}, {CHIP_L,  b9,
b10,b11,b12,b13,b14,b15}},

  {6,'G',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_K, CHIP_D, CHIP_D, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_I, CHIP_J, CHIP_H, CHIP_H}, {CHIP_L,
b16,b17,b18,b19,b20,b21,b22}},

  {7,'H',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {CHIP_A, CHIP_A, CHIP_B, CHIP_B, CHIP_C, CHIP_C, CHIP_D, CHIP_K, CHIP_E,
CHIP_E, CHIP_F, CHIP_F, CHIP_G, CHIP_G, CHIP_I, CHIP_J}, {CHIP_L,
b23,b24,b25,b26,b27,b28,b29}},

  {8,'I',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {NANO_A0, NANO_D1, NANO_A2, NANO_D3, NANO_A4, NANO_D5, NANO_A6, NANO_D7,
NANO_D11, NANO_D9, NANO_D13, NANO_RESET, DAC0, ADC0, SUPPLY_3V3, GND},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}},

  {9,'J',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {NANO_D0, NANO_A1, NANO_D2, NANO_A3, NANO_D4, NANO_A5, NANO_D6, NANO_A7,
NANO_D8, NANO_D10, NANO_D12, NANO_AREF, DAC1_8V, ADC1_5V, SUPPLY_5V, GND},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}},

  {10,'K',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6, NANO_A7,
NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}},

  {11,'L',
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}, // x status
  {-1,-1,-1,-1,-1,-1,-1,-1}, //y status
  {ISENSE_MINUS, ISENSE_PLUS, ADC0, ADC1_5V, ADC2_5V, ADC3_8V, DAC1_8V, DAC0,
t1, t30, b1, b30, NANO_A4, NANO_A5, SUPPLY_5V, GND},
  {CHIP_A,CHIP_B,CHIP_C,CHIP_D,CHIP_E,CHIP_F,CHIP_G,CHIP_H}}
  };

enum nanoPinsToIndex       {     NANO_PIN_D0 ,     NANO_PIN_D1 , NANO_PIN_D2
,     NANO_PIN_D3 ,     NANO_PIN_D4 ,     NANO_PIN_D5 ,     NANO_PIN_D6 ,
NANO_PIN_D7 ,     NANO_PIN_D8 ,     NANO_PIN_D9 ,     NANO_PIN_D10 ,
NANO_PIN_D11 ,      NANO_PIN_D12 ,     NANO_PIN_D13 ,       NANO_PIN_RST ,
NANO_PIN_REF ,     NANO_PIN_A0 ,     NANO_PIN_A1 ,     NANO_PIN_A2 ,
NANO_PIN_A3 ,     NANO_PIN_A4 ,     NANO_PIN_A5 ,     NANO_PIN_A6 ,
NANO_PIN_A7 };

extern struct nanoStatus nano;


struct pathStruct{

  int node1; //these are the rows or nano header pins to connect
  int node2;
  int net;

  int chip[3];
  int x[3];
  int y[3];
  int candidates[3][3];

};
*/

