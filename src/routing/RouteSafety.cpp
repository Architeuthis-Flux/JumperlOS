// SPDX-License-Identifier: MIT
/*
 * Physical-wire graph short protection.
 *
 * Every pin on every CH446Q maps to a physical wire ID. Closing a crosspoint
 * unions the X-pin wire with the Y-pin wire. A connected component that holds
 * two driven sources, two distinct non-high-Z nets, or a doNotIntersect pair
 * is a short — refused before hardware is touched.
 */

#include "RouteSafety.h"
#include "CoreMailbox.h" // core1req::allIdle() (T2.2b) - src/coredination is on the include path
#include "CH446Q.h" // sendXYrawUnchecked

#ifndef OG_JUMPERLESS

#include "JumperlessDefines.h"
#include "MatrixState.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "States.h"
#include "externVars.h"
#include <string.h>

// ============================================================================
// Globals
// ============================================================================

volatile uint32_t routingGeneration = 1;
volatile uint16_t chipXYSuspectMask = 0;
volatile uint32_t sendxy_blocked_count = 0;
volatile bool sendXYrawCheckEnabled = true;

// ============================================================================
// Wire table
// ============================================================================

// Theoretical ceiling: 12 chips x 24 pins = 288 distinct wires if nothing
// were shared. The old 160 silently overflowed on the V5 fabric (allocWire
// returned kInvalidWire for later rows, making them invisible to the short
// checker); the self-check now fails loudly if the table ever fills again.
static const int kMaxWires = 288;
static const int kInvalidWire = -1;

// pinWire[chip][0..15] = X pins; pinWire[chip][16..23] = Y pins
static int16_t pinWire[12][24];
static int16_t wireNode[kMaxWires]; // node id if this wire IS a node, else -1
static uint8_t wireIsDriven[kMaxWires];
static uint8_t wireIsHighZ[kMaxWires];
static int numWires = 0;
static bool wireTableReady = false;

static bool isDrivenSourceNode(int node) {
    return node == GND || node == TOP_RAIL || node == BOTTOM_RAIL ||
           node == DAC0 || node == DAC1 ||
           node == ROUTABLE_BUFFER_OUT ||
           node == RP_UART_TX ||
           (node >= RP_GPIO_20 && node <= RP_GPIO_27);
}

static bool isHighZNode(int node) {
    return (node >= ADC0 && node <= ADC4) ||
           node == ROUTABLE_BUFFER_IN ||
           node == ISENSE_PLUS || node == ISENSE_MINUS ||
           node == RP_UART_RX ||
           node == NANO_AREF;
}

static int allocWire(int node) {
    if (numWires >= kMaxWires) return kInvalidWire;
    int id = numWires++;
    wireNode[id] = node;
    wireIsDriven[id] = (node > 0 && isDrivenSourceNode(node)) ? 1 : 0;
    wireIsHighZ[id] = (node > 0 && isHighZNode(node)) ? 1 : 0;
    return id;
}

// Find existing wire for a node, or allocate one.
static int wireForNode(int node) {
    for (int i = 0; i < numWires; i++) {
        if (wireNode[i] == node) return i;
    }
    return allocWire(node);
}

// Lane wire registry built during init
static int16_t laneWire[12][12][4]; // [lo][hi][laneIdx] -> wire id
static uint8_t laneCount[12][12];

static int laneWireFor(int fromChip, int toChip, int laneIdx) {
    int lo = fromChip < toChip ? fromChip : toChip;
    int hi = fromChip < toChip ? toChip : fromChip;
    if (laneIdx < 0 || laneIdx >= 4) return kInvalidWire;
    if (laneWire[lo][hi][laneIdx] < 0) {
        laneWire[lo][hi][laneIdx] = (int16_t)allocWire(-1);
        if (laneCount[lo][hi] <= laneIdx)
            laneCount[lo][hi] = (uint8_t)(laneIdx + 1);
    }
    return laneWire[lo][hi][laneIdx];
}

static int countPriorLanesOnChip(int chip, int peer, int xPin) {
    int count = 0;
    for (int x = 0; x < xPin; x++) {
        if (globalState.connections.chipStates[chip].xMap[x] == peer) count++;
    }
    return count;
}

void initRouteSafety(void) {
    numWires = 0;
    memset(pinWire, 0xFF, sizeof(pinWire)); // -1
    memset(wireNode, 0xFF, sizeof(wireNode));
    memset(wireIsDriven, 0, sizeof(wireIsDriven));
    memset(wireIsHighZ, 0, sizeof(wireIsHighZ));
    memset(laneWire, 0xFF, sizeof(laneWire));
    memset(laneCount, 0, sizeof(laneCount));

    // Pass 1: X pins
    for (int c = 0; c < 12; c++) {
        for (int x = 0; x < 16; x++) {
            int map = globalState.connections.chipStates[c].xMap[x];
            if (map >= CHIP_A && map <= CHIP_L) {
                int laneIdx = countPriorLanesOnChip(c, map, x);
                pinWire[c][x] = (int16_t)laneWireFor(c, map, laneIdx);
            } else if (map > 0) {
                pinWire[c][x] = (int16_t)wireForNode(map);
            }
        }
    }

    // Pass 2: Y pins
    for (int c = 0; c < 12; c++) {
        for (int y = 0; y < 8; y++) {
            int map = globalState.connections.chipStates[c].yMap[y];
            if (map == BOUNCE_NODE) {
                // Unique stub per breadboard chip
                pinWire[c][16 + y] = (int16_t)allocWire(-1);
            } else if (c >= CHIP_I && c <= CHIP_L && map >= CHIP_A && map <= CHIP_L) {
                // Chip references exist ONLY on the SF chips' Y pins. On
                // breadboard chips A-H, yMap 1-7 hold ROW NODES whose ids
                // 1-11 collide with the CHIP_B..CHIP_L sentinels - treating
                // them as chips aliased rows 1-11 onto lane wires and
                // corrupted the short graph for those rows.
                // SF chip Y → breadboard chip: same wire as that BB chip's X
                // pin that maps back to this SF chip.
                int peer = map;
                int found = kInvalidWire;
                for (int x = 0; x < 16; x++) {
                    if (globalState.connections.chipStates[peer].xMap[x] == c) {
                        found = pinWire[peer][x];
                        break;
                    }
                }
                if (found < 0) {
                    found = laneWireFor(c, peer, 0);
                }
                pinWire[c][16 + y] = (int16_t)found;
            } else if (map > 0) {
                pinWire[c][16 + y] = (int16_t)wireForNode(map);
            }
        }
    }

    wireTableReady = true;
}

static inline int wireOfX(int chip, int x) {
    if (chip < 0 || chip >= 12 || x < 0 || x >= 16) return kInvalidWire;
    return pinWire[chip][x];
}

static inline int wireOfY(int chip, int y) {
    if (chip < 0 || chip >= 12 || y < 0 || y >= 8) return kInvalidWire;
    return pinWire[chip][16 + y];
}

// ============================================================================
// Union-find
// ============================================================================

struct WireUF {
    int16_t parent[kMaxWires];
    int16_t sz[kMaxWires];

    void reset(int n) {
        for (int i = 0; i < n; i++) {
            parent[i] = (int16_t)i;
            sz[i] = 1;
        }
    }

    int find(int a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }

    void unite(int a, int b) {
        if (a < 0 || b < 0) return;
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (sz[a] < sz[b]) {
            int t = a;
            a = b;
            b = t;
        }
        parent[b] = (int16_t)a;
        sz[a] = (int16_t)(sz[a] + sz[b]);
    }
};

static void unionFromBitfield(WireUF& uf, const chipXYBitfield state[12]) {
    for (int c = 0; c < 12; c++) {
        for (int y = 0; y < 8; y++) {
            uint16_t bits = state[c].connected[y];
            if (bits == 0) continue;
            int yw = wireOfY(c, y);
            if (yw < 0) continue;
            for (int x = 0; x < 16; x++) {
                if (bits & (1u << x)) {
                    uf.unite(wireOfX(c, x), yw);
                }
            }
        }
    }
}

static void unionHops(WireUF& uf, const int8_t* hopChip, const int8_t* hopX,
                      const int8_t* hopY, int nHops) {
    for (int h = 0; h < nHops; h++) {
        int c = hopChip[h], x = hopX[h], y = hopY[h];
        if (c < 0 || x < 0 || y < 0) continue;
        uf.unite(wireOfX(c, x), wireOfY(c, y));
    }
}

static inline int netOfNode(int node) {
    if (node < 0 || node >= 256) return -1;
    return nodeToNetIndex[node];
}

// Two driven-source nodes may share a component ONLY when the user routed
// them into the same net (netlist creation already vetted that membership
// via connectionAllowed). GPIOs count as driven regardless of direction, so
// without this exemption legit nets like GPIO<->GND or a GPIO loopback would
// be refused.
static inline bool drivenPairAllowed(int nodeA, int nodeB) {
    int netA = netOfNode(nodeA);
    int netB = netOfNode(nodeB);
    return netA > 0 && netA == netB;
}

// Returns true if the UF currently contains a short. On short, optionally
// fills reason nets/sources into outNetA/outNetB.
static bool componentHasShort(WireUF& uf, int* outNetA = nullptr,
                              int* outNetB = nullptr) {
    // Per-root: first driven source node, first signal net.
    // Scratch is per-core static, not stack: ~3.4KB per call was a real
    // stack hazard once checked sendXYraw() started running this per
    // crosspoint. Callers on the same core are strictly sequential (no
    // recursion, encoder yield never routes), so one buffer per core is safe.
    static int16_t rootDrivenBuf[2][kMaxWires];
    static int16_t rootNetBuf[2][kMaxWires];
    static int16_t rootNodesBuf[2][kMaxWires][8];
    static uint8_t rootNodeCountBuf[2][kMaxWires];
    int core = get_core_num() & 1;
    int16_t* rootDriven = rootDrivenBuf[core];
    int16_t* rootNet = rootNetBuf[core];
    int16_t (*rootNodes)[8] = rootNodesBuf[core];
    uint8_t* rootNodeCount = rootNodeCountBuf[core];
    memset(rootDriven, 0xFF, sizeof(rootDrivenBuf[0]));
    memset(rootNet, 0xFF, sizeof(rootNetBuf[0]));
    memset(rootNodeCount, 0, sizeof(rootNodeCountBuf[0]));

    for (int w = 0; w < numWires; w++) {
        int node = wireNode[w];
        if (node <= 0) continue;
        int r = uf.find(w);

        if (wireIsDriven[w]) {
            if (rootDriven[r] >= 0 && rootDriven[r] != node &&
                !drivenPairAllowed(rootDriven[r], node)) {
                if (outNetA) *outNetA = rootDriven[r];
                if (outNetB) *outNetB = node;
                return true; // two driven sources not vetted as one net
            }
            if (rootDriven[r] < 0) rootDriven[r] = (int16_t)node;
        }

        if (!wireIsHighZ[w]) {
            int net = -1;
            if (node >= 0 && node < 256) net = nodeToNetIndex[node];
            // Ignore special reserved nets and empty
            if (net > 0 && net != FAKE_GPIO_TDM_NET && net != EPHEMERAL_PATH_NET) {
                if (rootNet[r] >= 0 && rootNet[r] != net) {
                    if (outNetA) *outNetA = rootNet[r];
                    if (outNetB) *outNetB = net;
                    return true; // two distinct nets
                }
                if (rootNet[r] < 0) rootNet[r] = (int16_t)net;
            }
        }

        if (rootNodeCount[r] < 8) {
            rootNodes[r][rootNodeCount[r]++] = (int16_t)node;
        }
    }

    // doNotIntersect / connectionAllowed on node pairs in each component
    for (int r = 0; r < numWires; r++) {
        if (rootNodeCount[r] < 2) continue;
        // Only check roots (parent[r]==r); others are empty
        if (uf.parent[r] != r) continue;
        for (int i = 0; i < rootNodeCount[r]; i++) {
            for (int j = i + 1; j < rootNodeCount[r]; j++) {
                if (!connectionAllowed(rootNodes[r][i], rootNodes[r][j])) {
                    if (outNetA) *outNetA = rootNodes[r][i];
                    if (outNetB) *outNetB = rootNodes[r][j];
                    return true;
                }
            }
        }
    }
    return false;
}

// ============================================================================
// Public checkers
// ============================================================================

bool wouldShort(const int8_t* hopChip, const int8_t* hopX, const int8_t* hopY,
                int nHops, int allowedNet) {
    if (!wireTableReady || nHops <= 0) return false;

    WireUF uf;
    uf.reset(numWires);
    unionFromBitfield(uf, lastChipXY);
    unionHops(uf, hopChip, hopX, hopY, nHops);

    // If allowedNet is set, temporarily clear nodeToNetIndex conflict by
    // treating nodes of allowedNet as unlabeled? Simpler: after building,
    // if the only "two nets" are allowedNet + something high-Z, fine.
    // componentHasShort uses nodeToNetIndex live — for fastConnect of a node
    // already in allowedNet to an ADC (high-Z), only one net appears. Good.
    (void)allowedNet;
    return componentHasShort(uf);
}

bool wouldShortCrosspoint(int chip, int x, int y) {
    int8_t hc = (int8_t)chip, hx = (int8_t)x, hy = (int8_t)y;
    // Simulate on a copy of lastChipXY that does NOT yet include this bit
    // (caller must not have set it). wouldShort unions lastChipXY + hop.
    return wouldShort(&hc, &hx, &hy, 1, -1);
}

bool wouldShortCrosspointMasked(int chip, int x, int y, int clearChip,
                                int clearY, uint16_t clearBits) {
    if (!wireTableReady) return false;
    // Same as wouldShortCrosspoint but with clearBits pretend-opened first,
    // so the chip-K source auto-disconnect can be SIMULATED before deciding -
    // no hardware mutation when the connect ends up refused.
    chipXYBitfield state[12];
    memcpy(state, lastChipXY, sizeof(state));
    if (clearBits && clearChip >= 0 && clearChip < 12 && clearY >= 0 &&
        clearY < 8) {
        state[clearChip].connected[clearY] &= (uint16_t)~clearBits;
    }
    WireUF uf;
    uf.reset(numWires);
    unionFromBitfield(uf, state);
    int8_t hc = (int8_t)chip, hx = (int8_t)x, hy = (int8_t)y;
    unionHops(uf, &hc, &hx, &hy, 1);
    return componentHasShort(uf);
}

static bool isFakeGpioInputPath(int pathIdx) {
    int pathNode1 = globalState.connections.paths[pathIdx].node1;
    int pathNode2 = globalState.connections.paths[pathIdx].node2;
    for (int b = 0; b < globalState.connections.numBridges; b++) {
        int b1 = globalState.connections.bridges[b][0];
        int b2 = globalState.connections.bridges[b][1];
        if ((b1 == pathNode1 || b1 == pathNode2) && IS_FAKE_GP_IN(b2)) return true;
        if ((b2 == pathNode1 || b2 == pathNode2) && IS_FAKE_GP_IN(b1)) return true;
    }
    return false;
}

int validateAllPaths(void) {
    if (!wireTableReady) return 0;

    chipXYBitfield accepted[12];
    memset(accepted, 0, sizeof(accepted));
    int found = 0;

    for (int i = 0; i < numberOfPaths; i++) {
        pathStruct& p = globalState.connections.paths[i];
        if (p.skip || p.net <= 0) continue;
        if (p.pathType == VIRTUAL) continue;
        // TDM manages these; simultaneous presence in paths[] is intentional.
        if (isFakeGpioInputPath(i)) continue;

        int8_t hc[4], hx[4], hy[4];
        int nHops = 0;
        for (int h = 0; h < 4; h++) {
            if (p.chip[h] < 0 || p.x[h] < 0 || p.y[h] < 0) continue;
            hc[nHops] = (int8_t)p.chip[h];
            hx[nHops] = (int8_t)p.x[h];
            hy[nHops] = (int8_t)p.y[h];
            nHops++;
        }
        if (nHops == 0) continue;

        WireUF uf;
        uf.reset(numWires);
        unionFromBitfield(uf, accepted);
        unionHops(uf, hc, hx, hy, nHops);

        int netA = -1, netB = -1;
        if (componentHasShort(uf, &netA, &netB)) {
            p.skip = true;
            if (numberOfUnconnectablePaths < 10) {
                unconnectablePaths[numberOfUnconnectablePaths][0] = p.node1;
                unconnectablePaths[numberOfUnconnectablePaths][1] = p.node2;
                numberOfUnconnectablePaths++;
            }
            if (debugNTCC) {
                Serial.print("RouteSafety: skipped path ");
                Serial.print(i);
                Serial.print(" net ");
                Serial.print(p.net);
                Serial.print(" (");
                Serial.print(p.node1);
                Serial.print("-");
                Serial.print(p.node2);
                Serial.print(") short ");
                Serial.print(netA);
                Serial.print("/");
                Serial.println(netB);
            }
            found++;
            continue;
        }

        // Accept hops into the running state
        for (int h = 0; h < nHops; h++) {
            int c = hc[h], x = hx[h], y = hy[h];
            if (c >= 0 && c < 12 && x >= 0 && x < 16 && y >= 0 && y < 8) {
                accepted[c].connected[y] |= (uint16_t)(1u << x);
            }
        }
    }
    return found;
}

// ============================================================================
// Suspect / generation
// ============================================================================

void markChipXYSuspect(int chip) {
    if (chip >= 0 && chip < 12) {
        chipXYSuspectMask |= (uint16_t)(1u << chip);
    }
}

void clearChipXYSuspect(void) { chipXYSuspectMask = 0; }

bool anyChipXYSuspect(void) { return chipXYSuspectMask != 0; }

void reassertOpenOnLanes(const int8_t* hopChip, const int8_t* hopX,
                         const int8_t* hopY, int nHops) {
    // For every hop lane (chip,x) and (chip,y bus), re-send open for any
    // crosspoint bookkeeping believes is open on that x column / involved y.
    //
    // Bail on the first handshake timeout: this can issue ~90 sends across a
    // 4-hop route, and when the PIO handshake is actually dead each one costs
    // its full timeout - that stacked up to ~180ms with readingADC held and
    // core2busy raised (a tap must stay well under waitCore2's 25ms). A sick
    // handshake means bookkeeping can't be trusted anyway; the clean refresh
    // that anyChipXYSuspect() forces is the real recovery, not more sends.
    for (int h = 0; h < nHops; h++) {
        int c = hopChip[h];
        int x = hopX[h];
        int yFocus = hopY[h];
        if (c < 0 || c >= 12) continue;
        for (int y = 0; y < 8; y++) {
            // Re-open every closed crosspoint on this chip that shares our
            // X column or our Y row — forces HW to match bookkeeping opens
            // before we trust the lane.
            uint16_t bits = lastChipXY[c].connected[y];
            for (int xi = 0; xi < 16; xi++) {
                if (bits & (1u << xi)) continue; // closed — leave alone
                // Only touch the X column or Y row we're about to use
                if (xi != x && y != yFocus) continue;
                // Bookkeeping says open; re-assert open
                int timeoutsBefore = ch446q_timeout_count;
                sendXYrawUnchecked(c, xi, y, 0, 500);
                if (ch446q_timeout_count != timeoutsBefore) return;
            }
        }
    }
}

// ============================================================================
// Fabric helpers for fastConnectPath (from NetVoltageScan)
// ============================================================================

static inline bool yRowFreeHW(int chip, int y) {
    return lastChipXY[chip].connected[y] == 0;
}

static bool xColumnFreeHW(int chip, int x) {
    for (int y = 0; y < 8; y++) {
        if (lastChipXY[chip].connected[y] & (1 << x)) return false;
    }
    return true;
}

static bool xStatusFree(int chip, int x) {
    int8_t s = globalState.connections.chipStates[chip].xStatus[x];
    return s == -1 || s == EPHEMERAL_PATH_NET;
}

static bool yStatusFree(int chip, int y) {
    int8_t s = globalState.connections.chipStates[chip].yStatus[y];
    return s == -1 || s == EPHEMERAL_PATH_NET;
}

// The two predicates every routing tier gates on, at file scope so the
// failure diagnostics can ask the SAME question the router asked.
// (buildEphemeralRoute keeps its local lambdas of the same name.)
static inline bool laneUsable(int chip, int x) {
    return xColumnFreeHW(chip, x) && xStatusFree(chip, x);
}
static inline bool yUsable(int chip, int y) {
    return yRowFreeHW(chip, y) && yStatusFree(chip, y);
}

static int laneToChip(int fromChip, int toChip) {
    for (int x = 0; x < 16; x++) {
        if (globalState.connections.chipStates[fromChip].xMap[x] == toChip)
            return x;
    }
    return -1;
}

// The net the ROUTER'S ledger says a fabric pin carries (-1 for none or a
// reserved sentinel). This, not nodeToNetIndex, is what the tap builder
// keys its same-net decisions on: nodeToNetIndex is rebuilt inside
// getNodesToConnect() while numberOfNets is still 0 from clearAllNTCC()
// (sortPathsByNet sets it later, in bridgesToPaths), so after every rebuild
// it reads -1 for every node - which is why the shared-K-row fallback below
// never fired on the bench (2026-09-03, the LED at 12 mA reading 0.0 mA).
static int ledgerNetAt(int chip, int pin, bool isX) {
    if (chip < 0 || chip >= 12 || pin < 0) return -1;
    int8_t st = isX ? globalState.connections.chipStates[chip].xStatus[pin]
                    : globalState.connections.chipStates[chip].yStatus[pin];
    if (st <= 0 || st == EPHEMERAL_PATH_NET || st == FAKE_GPIO_TDM_NET) return -1;
    return st;
}

static bool findNodeOnFabric(int node, int* chipOut, int* pinOut, bool* isXOut) {
    for (int c = CHIP_A; c <= CHIP_H; c++) {
        for (int y = 1; y < 8; y++) {
            if (globalState.connections.chipStates[c].yMap[y] == node) {
                *chipOut = c;
                *pinOut = y;
                *isXOut = false;
                return true;
            }
        }
    }
    if (node <= CHIP_L) return false;
    for (int c = CHIP_I; c <= CHIP_L; c++) {
        for (int x = 0; x < 16; x++) {
            if (globalState.connections.chipStates[c].xMap[x] == node) {
                *chipOut = c;
                *pinOut = x;
                *isXOut = true;
                return true;
            }
        }
    }
    return false;
}

// Build a raw route from nodeA to nodeB (typically an ADC). Same fallback
// tiers as the old NetVoltageScan builder, but gated on xStatus/yStatus too.
//
// allowSharedKRow (the 0.0 mA starvation fix, bench 2026-08-24): chip K's 8
// y-rows are the ONLY gateway to ADC0-3, and a K-heavy netlist + stacking
// duplicates can consume all of them - every tier below terminates on a K
// row, so saturation = every tap noroute = every net displays 0.0 mA. The
// fallback pass may land on a K row whose every closed crosspoint already
// belongs to nodeA's OWN net (the row IS that net electrically, so joining
// the high-Z ADC lane to it is lawful - the same insight GuideChecks'
// chainBegin uses). Virgin rows are always preferred (the caller runs a
// virgin-only pass first): a shared-row tap reads the net at K's junction
// rather than at the node's hole, which flattens pair-tap deltas.
// NetsToChipConnections' duplicate reservation is the complementary half
// (it keeps rows virgin in the first place).
// Why the last route build failed, for the i! dry run only (the scan never
// prints). Tier 4 writes one code per chip it rejected: k = K lane busy
// and not the net's, r = no row of the net on that chip, l = no lane
// (virgin or the net's) between the chips.
char routeTrace[64];

static bool buildEphemeralRouteTiered(int nodeA, int nodeB, pathStruct* out,
                                      bool allowSharedKRow) {
    routeTrace[0] = 0;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 4; i++) {
        out->chip[i] = -1;
        out->x[i] = -1;
        out->y[i] = -1;
    }
    out->node1 = nodeA;
    out->node2 = nodeB;
    out->net = EPHEMERAL_PATH_NET;

    int chipA, pinA, chipB, pinB;
    bool isXA, isXB;
    if (!findNodeOnFabric(nodeA, &chipA, &pinA, &isXA)) return false;
    if (!findNodeOnFabric(nodeB, &chipB, &pinB, &isXB)) return false;

    int nodeANet = ledgerNetAt(chipA, pinA, isXA);

    // A K row is sharable when the router's own ledger says everything on
    // it - the row claim and every closed lane - is nodeA's net.
    auto kRowSharedByNet = [&](int y) -> bool {
        if (!allowSharedKRow || nodeANet <= 0) return false;
        if (globalState.connections.chipStates[CHIP_K].yStatus[y] != nodeANet)
            return false;
        uint16_t bits = lastChipXY[CHIP_K].connected[y];
        if (bits == 0) return false;   // virgin rows go through yOk
        for (int x = 0; x < 16; x++) {
            if (!(bits & (1u << x))) continue;
            if (globalState.connections.chipStates[CHIP_K].xStatus[x] != nodeANet)
                return false;
        }
        return true;
    };

    // Prefer: nodeB is ADC on chip K (the common sense-tap case)
    int adcChip = -1, adcX = -1;
    int otherChip = -1, otherPin = -1;
    bool otherIsX = false;
    if (isXB && chipB == CHIP_K && pinB >= 8 && pinB <= 11) {
        adcChip = chipB;
        adcX = pinB;
        otherChip = chipA;
        otherPin = pinA;
        otherIsX = isXA;
    } else if (isXA && chipA == CHIP_K && pinA >= 8 && pinA <= 11) {
        adcChip = chipA;
        adcX = pinA;
        otherChip = chipB;
        otherPin = pinB;
        otherIsX = isXB;
    }

    auto laneOk = [](int chip, int x) -> bool {
        return xColumnFreeHW(chip, x) && xStatusFree(chip, x);
    };
    auto yOk = [](int chip, int y) -> bool {
        return yRowFreeHW(chip, y) && yStatusFree(chip, y);
    };
    // Every tier terminates on a chip-K row through THIS gate: virgin, or
    // (fallback pass only) wholly owned by nodeA's net.
    auto kRowOk = [&](int y) -> bool {
        return yOk(CHIP_K, y) || kRowSharedByNet(y);
    };

    int nHops = 0;
    int chipKY = -1;

    if (adcChip == CHIP_K && !laneOk(CHIP_K, adcX)) return false;

    if (adcChip == CHIP_K && !otherIsX) {
        // Breadboard row on A-H
        int chip = otherChip;
        int pin = otherPin;
        int laneK = laneToChip(chip, CHIP_K);
        if (laneK >= 0 && laneOk(chip, laneK) && kRowOk(chip)) {
            out->chip[0] = chip;
            out->x[0] = laneK;
            out->y[0] = pin;
            nHops = 1;
            chipKY = chip;
        } else {
            static const int viaChips[3] = {CHIP_L, CHIP_I, CHIP_J};
            for (int v = 0; v < 3 && chipKY < 0; v++) {
                int via = viaChips[v];
                int laneVia = laneToChip(chip, via);
                int laneViaK = laneToChip(via, CHIP_K);
                int laneKVia = laneToChip(CHIP_K, via);
                if (laneVia < 0 || laneViaK < 0 || laneKVia < 0) continue;
                if (!laneOk(chip, laneVia) || !yOk(via, chip) ||
                    !laneOk(via, laneViaK) || !laneOk(CHIP_K, laneKVia))
                    continue;
                for (int d = 0; d < 8; d++) {
                    int dLaneK = laneToChip(d, CHIP_K);
                    if (dLaneK >= 0 && kRowOk(d) && laneOk(d, dLaneK)) {
                        out->chip[0] = chip;
                        out->x[0] = laneVia;
                        out->y[0] = pin;
                        out->chip[1] = via;
                        out->x[1] = laneViaK;
                        out->y[1] = chip;
                        out->chip[2] = CHIP_K;
                        out->x[2] = laneKVia;
                        out->y[2] = d;
                        nHops = 3;
                        chipKY = d;
                        break;
                    }
                }
            }
            for (int n = CHIP_A; n <= CHIP_H && chipKY < 0; n++) {
                if (n == chip) continue;
                int nLaneK = laneToChip(n, CHIP_K);
                if (nLaneK < 0 || !yOk(n, 0) || !laneOk(n, nLaneK) ||
                    !kRowOk(n))
                    continue;
                for (int xc = 0; xc < 16 && chipKY < 0; xc++) {
                    if (globalState.connections.chipStates[chip].xMap[xc] != n)
                        continue;
                    if (!laneOk(chip, xc)) continue;
                    for (int xn = 0; xn < 16; xn++) {
                        if (globalState.connections.chipStates[n].xMap[xn] != chip)
                            continue;
                        if (strcmp(connectionNamesX[chip][xc],
                                   connectionNamesX[n][xn]) != 0)
                            continue;
                        if (!laneOk(n, xn)) break;
                        out->chip[0] = chip;
                        out->x[0] = xc;
                        out->y[0] = pin;
                        out->chip[1] = n;
                        out->x[1] = xn;
                        out->y[1] = 0;
                        out->chip[2] = n;
                        out->x[2] = nLaneK;
                        out->y[2] = 0;
                        nHops = 3;
                        chipKY = n;
                        break;
                    }
                }
            }
        }
            // Tier 4 (2026-09-03, Kevin's LED at 12 mA reading 0.0 mA):
            // bounce through a breadboard ROW that is already on nodeA's
            // net, on a chip whose K lane is virgin (with a usable K row) or
            // already the net's own (K row shared by the net). Joining our
            // lane to that row touches only copper the net owns - the same
            // insight as the shared-K-row pass. Rescues a net spread over
            // several chips (a rail feeding a few rows) when the node's home
            // chip has no lane left and every bounce row is spent.
            auto trace = [&](int n, char why) {
                size_t len = strlen(routeTrace);
                if (len + 3 < sizeof(routeTrace)) {
                    routeTrace[len] = (char)('A' + n);
                    routeTrace[len + 1] = why;
                    routeTrace[len + 2] = ' ';
                    routeTrace[len + 3] = 0;
                }
            };
            if (nodeANet <= 0) trace(chip, 'n');
            for (int n = CHIP_A; n <= CHIP_H && chipKY < 0 && nodeANet > 0; n++) {
                if (n == chip) continue;
                int nLaneK = laneToChip(n, CHIP_K);
                if (nLaneK < 0) continue;
                bool laneVirgin = laneOk(n, nLaneK) && kRowOk(n);
                bool laneOwned =
                    globalState.connections.chipStates[n].xStatus[nLaneK] == nodeANet &&
                    kRowSharedByNet(n);
                if (!laneVirgin && !laneOwned) { trace(n, 'k'); continue; }
                // A breadboard row first; failing that the bounce row
                // itself when a stacked copy of the net rides it (y0 is
                // then the net's copper too - the case where the copies
                // spent every bounce lane the node's chip had).
                int row = -1;
                for (int r = 1; r < 8 && row < 0; r++) {
                    if (globalState.connections.chipStates[n].yStatus[r] == nodeANet) row = r;
                }
                if (row < 0 && globalState.connections.chipStates[n].yStatus[0] == nodeANet) row = 0;
                if (row < 0) { trace(n, 'r'); continue; }
                // The lane between the two chips may be virgin or already
                // the net's own (a stacked copy of the net rides it): both
                // pins then carry nodeA's net and closing onto them joins
                // net copper to net copper. A pre-closed crosspoint is
                // dropped by the filter below.
                auto laneOkOrOwned = [&](int c, int x) -> bool {
                    return laneOk(c, x) ||
                           globalState.connections.chipStates[c].xStatus[x] == nodeANet;
                };
                for (int xc = 0; xc < 16 && chipKY < 0; xc++) {
                    if (globalState.connections.chipStates[chip].xMap[xc] != n)
                        continue;
                    if (!laneOkOrOwned(chip, xc)) continue;
                    for (int xn = 0; xn < 16; xn++) {
                        if (globalState.connections.chipStates[n].xMap[xn] != chip)
                            continue;
                        if (strcmp(connectionNamesX[chip][xc],
                                   connectionNamesX[n][xn]) != 0)
                            continue;
                        if (!laneOkOrOwned(n, xn)) break;
                        out->chip[0] = chip;
                        out->x[0] = xc;
                        out->y[0] = pin;
                        out->chip[1] = n;
                        out->x[1] = xn;
                        out->y[1] = row;
                        out->chip[2] = n;
                        out->x[2] = nLaneK;
                        out->y[2] = row;
                        nHops = 3;
                        chipKY = n;
                        break;
                    }
                }
                if (chipKY < 0) trace(n, 'l');
            }
        if (chipKY < 0) return false;
        out->chip[nHops] = CHIP_K;
        out->x[nHops] = adcX;
        out->y[nHops] = chipKY;
        return true;
    }

    if (adcChip == CHIP_K && otherIsX && otherChip == CHIP_K) {
        for (int d = 0; d < 8; d++) {
            int dLaneK = laneToChip(d, CHIP_K);
            if (dLaneK >= 0 && kRowOk(d) && laneOk(d, dLaneK)) {
                out->chip[0] = CHIP_K;
                out->x[0] = otherPin;
                out->y[0] = d;
                out->chip[1] = CHIP_K;
                out->x[1] = adcX;
                out->y[1] = d;
                return true;
            }
        }
        return false;
    }

    if (adcChip == CHIP_K && otherIsX) {
        // Node on I/J/L: first the hub lane straight into K - a same-chip
        // hop on the node's chip onto its hub lane (IK/JK/KL), landing on a
        // K row whose breadboard pin is free (2026-09-03: GPIO taps needed
        // a bounce chip while the probe feed held KL; with KL free, or a
        // node on I/J, this costs no bounce row at all).
        {
            int laneHubK = laneToChip(otherChip, CHIP_K);
            int laneKHub = laneToChip(CHIP_K, otherChip);
            if (laneHubK >= 0 && laneKHub >= 0 && laneOk(otherChip, laneHubK) &&
                laneOk(CHIP_K, laneKHub)) {
                int hubRow = -1;
                for (int y = 0; y < 8 && hubRow < 0; y++) {
                    int bb = globalState.connections.chipStates[otherChip].yMap[y];
                    int bbX = (bb >= CHIP_A && bb <= CHIP_H) ? laneToChip(bb, otherChip) : -1;
                    if (yOk(otherChip, y) && bbX >= 0 && laneOk(bb, bbX)) hubRow = y;
                }
                for (int d = 0; d < 8 && hubRow >= 0; d++) {
                    int dLaneK = laneToChip(d, CHIP_K);
                    if (dLaneK < 0 || !kRowOk(d) || !laneOk(d, dLaneK)) continue;
                    out->chip[0] = otherChip;
                    out->x[0] = otherPin;
                    out->y[0] = hubRow;
                    out->chip[1] = otherChip;
                    out->x[1] = laneHubK;
                    out->y[1] = hubRow;
                    out->chip[2] = CHIP_K;
                    out->x[2] = laneKHub;
                    out->y[2] = d;
                    out->chip[3] = CHIP_K;
                    out->x[3] = adcX;
                    out->y[3] = d;
                    return true;
                }
            }
        }
        // ... else bounce through a breadboard chip's y0 to K
        for (int c = CHIP_A; c <= CHIP_H; c++) {
            int laneSrc = laneToChip(c, otherChip);
            int laneK = laneToChip(c, CHIP_K);
            if (laneSrc >= 0 && laneK >= 0 && yOk(otherChip, c) &&
                laneOk(c, laneSrc) && yOk(c, 0) && laneOk(c, laneK) &&
                kRowOk(c)) {
                out->chip[0] = otherChip;
                out->x[0] = otherPin;
                out->y[0] = c;
                out->chip[1] = c;
                out->x[1] = laneSrc;
                out->y[1] = 0;
                out->chip[2] = c;
                out->x[2] = laneK;
                out->y[2] = 0;
                out->chip[3] = CHIP_K;
                out->x[3] = adcX;
                out->y[3] = c;
                return true;
            }
        }
        // Fallback pass only: the node's net already holds a K row (its
        // own hub hop - DAC0 -> I_POS over IK, the probe feed over KL) and
        // every bounce row is spent. That row IS the net; the ADC lane
        // joins it in one hop, the way a breadboard row's shared-K-row
        // pass does (2026-09-03 evening, the current loop with a rail on
        // row 2: ISENSE_PLUS and GPIO 8 had no route while their nets sat
        // on K rows y2 and y1).
        for (int y = 0; y < 8; y++) {
            if (kRowSharedByNet(y)) {
                out->chip[0] = CHIP_K;
                out->x[0] = adcX;
                out->y[0] = y;
                return true;
            }
        }
        return false;
    }

    // Generic same-chip or refuse — non-ADC pairs not needed yet for scanner
    return false;
}

static bool buildEphemeralRoute(int nodeA, int nodeB, pathStruct* out) {
    // Pass 1: virgin K rows only (the historical behavior - taps land on
    // their own copper, pair deltas stay honest). Pass 2 only when the
    // fabric is saturated: share a K row wholly owned by nodeA's net.
    bool ok = buildEphemeralRouteTiered(nodeA, nodeB, out, false);
    if (!ok) ok = buildEphemeralRouteTiered(nodeA, nodeB, out, true);
    if (!ok) return false;

    // A shared-row route may include a crosspoint the net's REAL wiring
    // already closed (e.g. joining onto K row y via the node's own lane).
    // Drop those hops: they need no send, and - load-bearing - teardown
    // (fastDisconnectPath / the error unwind) must NEVER open a crosspoint
    // the user's circuit owns. Pass-1 routes only use virgin lanes, so the
    // filter is inert there. The ADC hop can never be pre-closed (its lane
    // is required column-free), so at least one hop always survives.
    int chipA = -1, pinA = -1;
    bool isXA = false;
    int nodeANet = findNodeOnFabric(nodeA, &chipA, &pinA, &isXA)
                       ? ledgerNetAt(chipA, pinA, isXA) : -1;
    int w = 0;
    for (int h = 0; h < 4; h++) {
        int c = out->chip[h], x = out->x[h], y = out->y[h];
        if (c < 0 || x < 0 || y < 0) continue;
        bool preClosed = (lastChipXY[c].connected[y] & (1u << x)) != 0 &&
                         nodeANet > 0 &&
                         globalState.connections.chipStates[c].xStatus[x] == nodeANet;
        if (preClosed) continue;
        out->chip[w] = (int8_t)c;
        out->x[w] = (int8_t)x;
        out->y[w] = (int8_t)y;
        w++;
    }
    for (; w < 4; w++) {
        out->chip[w] = -1;
        out->x[w] = -1;
        out->y[w] = -1;
    }
    return true;
}

static void claimPathLanes(const pathStruct& p, int net) {
    for (int h = 0; h < 4; h++) {
        int c = p.chip[h], x = p.x[h], y = p.y[h];
        if (c < 0 || x < 0 || y < 0) continue;
        if (globalState.connections.chipStates[c].xStatus[x] == -1)
            globalState.connections.chipStates[c].xStatus[x] = (int8_t)net;
        if (globalState.connections.chipStates[c].yStatus[y] == -1)
            globalState.connections.chipStates[c].yStatus[y] = (int8_t)net;
    }
}

static void releasePathLanes(const pathStruct& p, int net) {
    for (int h = 0; h < 4; h++) {
        int c = p.chip[h], x = p.x[h], y = p.y[h];
        if (c < 0 || x < 0 || y < 0) continue;
        if (globalState.connections.chipStates[c].xStatus[x] == net)
            globalState.connections.chipStates[c].xStatus[x] = -1;
        if (globalState.connections.chipStates[c].yStatus[y] == net)
            globalState.connections.chipStates[c].yStatus[y] = -1;
    }
}

bool planFastPath(int nodeA, int nodeB, pathStruct* out) {
    if (!out || !wireTableReady) return false;
    if (!buildEphemeralRoute(nodeA, nodeB, out)) return false;
    int8_t hc[4], hx[4], hy[4];
    int nHops = 0;
    for (int h = 0; h < 4; h++) {
        if (out->chip[h] < 0 || out->x[h] < 0 || out->y[h] < 0) continue;
        hc[nHops] = (int8_t)out->chip[h];
        hx[nHops] = (int8_t)out->x[h];
        hy[nHops] = (int8_t)out->y[h];
        nHops++;
    }
    if (nHops == 0) return false;
    int allowedNet = -1;
    if (nodeA >= 0 && nodeA < 256 && nodeToNetIndex[nodeA] > 0)
        allowedNet = nodeToNetIndex[nodeA];
    return !wouldShort(hc, hx, hy, nHops, allowedNet);
}

int fastConnectPath(int nodeA, int nodeB, FastPathHandle* out,
                    unsigned long hopTimeoutUs) {
    if (!out || !wireTableReady) return -1;
    if (core1FramesHeld() || !core1req::allIdle()) return -2;  // a path send pending / in flight

    out->active = false;
    pathStruct route;
    if (!buildEphemeralRoute(nodeA, nodeB, &route)) return -3;

    int8_t hc[4], hx[4], hy[4];
    int nHops = 0;
    for (int h = 0; h < 4; h++) {
        if (route.chip[h] < 0 || route.x[h] < 0 || route.y[h] < 0) continue;
        hc[nHops] = (int8_t)route.chip[h];
        hx[nHops] = (int8_t)route.x[h];
        hy[nHops] = (int8_t)route.y[h];
        nHops++;
    }
    if (nHops == 0) return -3;

    int allowedNet = -1;
    if (nodeA >= 0 && nodeA < 256 && nodeToNetIndex[nodeA] > 0)
        allowedNet = nodeToNetIndex[nodeA];
    else if (nodeB >= 0 && nodeB < 256 && nodeToNetIndex[nodeB] > 0)
        allowedNet = nodeToNetIndex[nodeB];

    if (wouldShort(hc, hx, hy, nHops, allowedNet)) return -4;

    if (anyChipXYSuspect()) {
        reassertOpenOnLanes(hc, hx, hy, nHops);
    }

    claimPathLanes(route, EPHEMERAL_PATH_NET);
    out->generation = routingGeneration;
    out->path = route;

    // Send via unchecked — whole route already validated
    int sent = 0;
    for (int h = 0; h < nHops; h++) {
        int timeoutsBefore = ch446q_timeout_count;
        sendXYrawUnchecked(hc[h], hx[h], hy[h], 1, hopTimeoutUs);
        sent++;
        if (ch446q_timeout_count != timeoutsBefore) {
            for (int u = sent - 1; u >= 0; u--) {
                sendXYrawUnchecked(hc[u], hx[u], hy[u], 0, hopTimeoutUs);
            }
            releasePathLanes(route, EPHEMERAL_PATH_NET);
            return -5;
        }
    }

    out->active = true;
    return 0;
}

void fastDisconnectPath(FastPathHandle* p) {
    if (!p || !p->active) return;
    if (p->generation != routingGeneration) {
        // Refresh already wiped hardware + xStatus; just drop the handle.
        p->active = false;
        return;
    }
    for (int h = 3; h >= 0; h--) {
        int c = p->path.chip[h], x = p->path.x[h], y = p->path.y[h];
        if (c < 0 || x < 0 || y < 0) continue;
        // 1000us/hop, and keep trying every hop even after a timeout (a
        // stranded closed crosspoint is a short risk, so best-effort all
        // four): 4ms worst case, part of the tap's <25ms total budget.
        sendXYrawUnchecked(c, x, y, 0, 1000);
    }
    releasePathLanes(p->path, EPHEMERAL_PATH_NET);
    p->active = false;
}

// ============================================================================
// Tap route-failure diagnostics
// ============================================================================
//
// invest-vf-noroute.md §6: when a sense tap is refused, the two numbers that
// tell #1 (route starvation) from #2 (ring stale) apart are chip K's free-y
// mask and the target row's own chip x-occupancy - every tap route in every
// tier of buildEphemeralRoute terminates by closing (K, adcX, y) on a
// completely free K y row, reached over one free x lane on the row's chip.
// Kfree with <=1 bit set (or bits whose d-side lane is busy) IS starvation.
// Read-only and print-free so the scan core can snapshot at the failure.
void fastPathFailSnapshot(int node, uint16_t* kFreeYMask, uint16_t* kXBusyMask,
                          int* chip, uint16_t* xBusyMask, uint8_t* bounceOkMask) {
    // The masks answer with the SAME predicates buildEphemeralRoute gates on
    // (laneOk = xColumnFreeHW && xStatusFree, yOk = yRowFreeHW &&
    // yStatusFree), not the raw crosspoint bitfield - a lane claimed by a
    // live net's routing bookkeeping is just as unusable as a closed one, and
    // reading only the hardware bits sends you hunting the wrong thing.
    if (kFreeYMask) {
        uint16_t m = 0;
        for (int y = 0; y < 8; y++) {
            if (yUsable(CHIP_K, y))
                m |= (uint16_t)(1u << y);
        }
        *kFreeYMask = m;
    }
    if (kXBusyMask) {
        // Includes the ADC columns x8-x11: buildEphemeralRoute's very first
        // gate is laneOk(CHIP_K, adcX), so a busy ADC column refuses every
        // tier before any lane search happens.
        uint16_t m = 0;
        for (int x = 0; x < 16; x++) {
            if (!laneUsable(CHIP_K, x))
                m |= (uint16_t)(1u << x);
        }
        *kXBusyMask = m;
    }
    if (bounceOkMask) {
        // The LAST tier: when a row's own chip has no free lane to K, I, J or
        // L, the route must bounce off a neighbour chip's y0 and leave over
        // that chip's K lane. These are exactly the tier-2 outer guards, so
        // bounceOk == 0 means the bounce tier was never even entered - the
        // difference between "no lane pair matched" and "no candidate chip
        // existed", which is otherwise invisible from the other masks.
        uint8_t m = 0;
        for (int n = CHIP_A; n <= CHIP_H; n++) {
            int nLaneK = laneToChip(n, CHIP_K);
            if (nLaneK < 0) continue;
            if (!yUsable(n, 0) || !laneUsable(n, nLaneK) || !yUsable(CHIP_K, n))
                continue;
            m |= (uint8_t)(1u << n);
        }
        *bounceOkMask = m;
    }
    if (chip) *chip = -1;
    if (xBusyMask) *xBusyMask = 0;
    int c = -1, pin = -1;
    bool isX = false;
    if (node < 0 || !wireTableReady) return;
    if (!findNodeOnFabric(node, &c, &pin, &isX)) return;
    if (c < 0 || c >= 12) return;
    if (chip) *chip = c;
    if (xBusyMask) {
        uint16_t m = 0;
        for (int x = 0; x < 16; x++) {
            if (!laneUsable(c, x))
                m |= (uint16_t)(1u << x);
        }
        *xBusyMask = m;
    }
}

// ============================================================================
// Self-check + audit
// ============================================================================

void auditLastChipXY(Stream* out) {
    if (!out) out = &Serial;
    if (!wireTableReady) {
        out->println("RouteSafety: wire table not ready");
        return;
    }
    WireUF uf;
    uf.reset(numWires);
    unionFromBitfield(uf, lastChipXY);
    int netA = -1, netB = -1;
    bool shorted = componentHasShort(uf, &netA, &netB);
    out->printf("RouteSafety audit: wires=%d suspect=0x%03x blocked=%lu gen=%lu %s",
                numWires, (unsigned)chipXYSuspectMask,
                (unsigned long)sendxy_blocked_count,
                (unsigned long)routingGeneration,
                shorted ? "SHORT" : "ok");
    if (shorted) {
        out->printf(" (%d/%d)", netA, netB);
    }
    out->println();
}

int routeSafetySelfCheck(Stream* out) {
    if (!out) out = &Serial;
    if (!wireTableReady) {
        out->println("RouteSafety self-check: FAIL (no wire table)");
        return -1;
    }
    int fails = 0;

    // 0. Wire table must not have overflowed (a full table means some pins
    // got kInvalidWire and their nets are invisible to the short checker)
    if (numWires >= kMaxWires) {
        out->printf("FAIL: wire table full (%d/%d) - pins truncated\n",
                    numWires, kMaxWires);
        fails++;
    } else {
        out->printf("ok: wire table %d/%d\n", numWires, kMaxWires);
    }

    // 1. Two driven sources on chip K same Y must short
    {
        chipXYBitfield saved[12];
        memcpy(saved, lastChipXY, sizeof(saved));
        memset(lastChipXY, 0, sizeof(lastChipXY));
        // Pretend TOP_RAIL (K x4) already on y0
        lastChipXY[CHIP_K].connected[0] |= (1 << 4);
        bool s = wouldShortCrosspoint(CHIP_K, 7, 0); // DAC0 on y0
        if (!s) {
            out->println("FAIL: DAC0+TOP_RAIL on K.y0 not detected");
            fails++;
        } else {
            out->println("ok: two chip-K sources on same Y");
        }
        memcpy(lastChipXY, saved, sizeof(saved));
    }

    // 2. Paired-lane identity: A.x2 and B.x0 should be the same wire (AB0)
    {
        int wa = wireOfX(CHIP_A, 2);
        int wb = wireOfX(CHIP_B, 0);
        // Find first X on A→B and B→A
        int ax = -1, bx = -1;
        for (int x = 0; x < 16; x++) {
            if (ax < 0 && globalState.connections.chipStates[CHIP_A].xMap[x] == CHIP_B)
                ax = x;
            if (bx < 0 && globalState.connections.chipStates[CHIP_B].xMap[x] == CHIP_A)
                bx = x;
        }
        if (ax >= 0 && bx >= 0) {
            wa = wireOfX(CHIP_A, ax);
            wb = wireOfX(CHIP_B, bx);
            if (wa < 0 || wa != wb) {
                out->printf("FAIL: paired lane A.x%d / B.x%d wires %d/%d\n", ax, bx, wa, wb);
                fails++;
            } else {
                out->printf("ok: paired lane A.x%d == B.x%d (wire %d)\n", ax, bx, wa);
            }
        }
    }

    // 3. Bounce stub uniqueness
    {
        int b0 = wireOfY(CHIP_A, 0);
        int b1 = wireOfY(CHIP_B, 0);
        if (b0 < 0 || b0 == b1) {
            out->println("FAIL: bounce stubs not unique");
            fails++;
        } else {
            out->println("ok: bounce stubs unique per chip");
        }
    }

    // 4. GND on K and L share a wire
    {
        int gk = -1, gl = -1;
        for (int x = 0; x < 16; x++) {
            if (globalState.connections.chipStates[CHIP_K].xMap[x] == GND)
                gk = wireOfX(CHIP_K, x);
            if (globalState.connections.chipStates[CHIP_L].xMap[x] == GND)
                gl = wireOfX(CHIP_L, x);
        }
        if (gk < 0 || gk != gl) {
            out->printf("FAIL: GND K/L wires %d/%d\n", gk, gl);
            fails++;
        } else {
            out->println("ok: GND shared across K and L");
        }
    }

    // 5-7. Driven-pair rule: GPIOs count as driven regardless of direction,
    // so GND+GPIO joined on a chip-L lane must be ALLOWED when the user
    // routed them into one net (loopbacks, GPIO-to-GND reads), and refused
    // when they belong to different nets or to none (raw pokes).
    {
        int gndX = -1, gpioX = -1;
        for (int x = 0; x < 16; x++) {
            if (globalState.connections.chipStates[CHIP_L].xMap[x] == GND)
                gndX = x;
            if (globalState.connections.chipStates[CHIP_L].xMap[x] == RP_GPIO_20)
                gpioX = x;
        }
        if (gndX < 0 || gpioX < 0) {
            out->println("FAIL: GND/RP_GPIO_20 not found on chip L xMap");
            fails++;
        } else {
            chipXYBitfield saved[12];
            memcpy(saved, lastChipXY, sizeof(saved));
            int8_t savedGndNet = nodeToNetIndex[GND];
            int8_t savedGpioNet = nodeToNetIndex[RP_GPIO_20];

            memset(lastChipXY, 0, sizeof(lastChipXY));
            lastChipXY[CHIP_L].connected[0] |= (uint16_t)(1u << gndX);

            // 5. Same net -> allowed
            nodeToNetIndex[GND] = 7;
            nodeToNetIndex[RP_GPIO_20] = 7;
            if (wouldShortCrosspoint(CHIP_L, gpioX, 0)) {
                out->println("FAIL: GND+GPIO in ONE net refused (loopback regression)");
                fails++;
            } else {
                out->println("ok: GND+GPIO same net allowed");
            }

            // 6. Different nets -> short
            nodeToNetIndex[RP_GPIO_20] = 8;
            if (!wouldShortCrosspoint(CHIP_L, gpioX, 0)) {
                out->println("FAIL: GND+GPIO in different nets not detected");
                fails++;
            } else {
                out->println("ok: GND+GPIO cross-net shorts");
            }

            // 7. Unrouted pair -> short (raw pokes must stay refused)
            nodeToNetIndex[GND] = 0;
            nodeToNetIndex[RP_GPIO_20] = 0;
            if (!wouldShortCrosspoint(CHIP_L, gpioX, 0)) {
                out->println("FAIL: unrouted GND+GPIO pair not detected");
                fails++;
            } else {
                out->println("ok: unrouted driven pair shorts");
            }

            nodeToNetIndex[GND] = savedGndNet;
            nodeToNetIndex[RP_GPIO_20] = savedGpioNet;
            memcpy(lastChipXY, saved, sizeof(saved));
        }
    }

    // 8. Two plain nets joined through one breadboard-chip lane must short
    {
        // Pick a REAL fabric lane on chip A (first x that maps to another
        // chip) - hardcoding x0 fails on fabrics where that pin is unmapped.
        int laneX = -1;
        for (int x = 0; x < 16; x++) {
            int map = globalState.connections.chipStates[CHIP_A].xMap[x];
            if (map >= CHIP_A && map <= CHIP_L) {
                laneX = x;
                break;
            }
        }
        int nodeR1 = globalState.connections.chipStates[CHIP_A].yMap[1];
        int nodeR2 = globalState.connections.chipStates[CHIP_A].yMap[2];
        if (laneX >= 0 && nodeR1 > 0 && nodeR1 < 256 && nodeR2 > 0 && nodeR2 < 256) {
            chipXYBitfield saved[12];
            memcpy(saved, lastChipXY, sizeof(saved));
            int8_t savedN1 = nodeToNetIndex[nodeR1];
            int8_t savedN2 = nodeToNetIndex[nodeR2];

            memset(lastChipXY, 0, sizeof(lastChipXY));
            lastChipXY[CHIP_A].connected[1] |= (uint16_t)(1u << laneX);
            nodeToNetIndex[nodeR1] = 7;
            nodeToNetIndex[nodeR2] = 8;
            if (!wouldShortCrosspoint(CHIP_A, laneX, 2)) {
                out->printf("FAIL: two nets via shared A.x%d lane not detected\n", laneX);
                out->printf("  dbg laneW=%d y1W=%d y2W=%d n1=%d n2=%d net1=%d net2=%d\n",
                            wireOfX(CHIP_A, laneX), wireOfY(CHIP_A, 1),
                            wireOfY(CHIP_A, 2), nodeR1, nodeR2,
                            nodeToNetIndex[nodeR1], nodeToNetIndex[nodeR2]);
                fails++;
            } else {
                out->println("ok: two nets via shared lane shorts");
            }

            nodeToNetIndex[nodeR1] = savedN1;
            nodeToNetIndex[nodeR2] = savedN2;
            memcpy(lastChipXY, saved, sizeof(saved));
        } else {
            out->println("FAIL: no fabric lane found on chip A for test 8");
            fails++;
        }
    }

    out->printf("RouteSafety self-check: %s (%d fails), %d wires\n",
                fails ? "FAIL" : "PASS", fails, numWires);
    auditLastChipXY(out);
    return fails;
}

#else // OG_JUMPERLESS

#include "RouteSafety.h"

volatile uint32_t routingGeneration = 1;
volatile uint16_t chipXYSuspectMask = 0;
volatile uint32_t sendxy_blocked_count = 0;
volatile bool sendXYrawCheckEnabled = true;

void initRouteSafety(void) {}
int validateAllPaths(void) { return 0; }
bool wouldShort(const int8_t*, const int8_t*, const int8_t*, int, int) {
    return false;
}
bool wouldShortCrosspoint(int, int, int) { return false; }
bool wouldShortCrosspointMasked(int, int, int, int, int, uint16_t) {
    return false;
}
bool planFastPath(int, int, pathStruct*) { return false; }
int fastConnectPath(int, int, FastPathHandle*, unsigned long) { return -1; }
void fastDisconnectPath(FastPathHandle* p) {
    if (p) p->active = false;
}
void reassertOpenOnLanes(const int8_t*, const int8_t*, const int8_t*, int) {}
void markChipXYSuspect(int) {}
void clearChipXYSuspect(void) {}
bool anyChipXYSuspect(void) { return false; }
int routeSafetySelfCheck(Stream* out) {
    if (out) out->println("RouteSafety: V5 only");
    return 0;
}
void auditLastChipXY(Stream* out) {
    if (out) out->println("RouteSafety: V5 only");
}
#endif // OG_JUMPERLESS
