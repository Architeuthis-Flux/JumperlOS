// SPDX-License-Identifier: MIT
//
// Host check for the OG Jumperless router (src/routing/NetsToChipConnections_OG.cpp)
// and the RouteSafety validator it hands its paths to. No hardware, no PlatformIO:
//
//   cd JumperlOS && test/test_routing_og/run.sh
//
// It compiles the real router, the real RouteSafety.cpp and the real OG board
// descriptor against the tiny Arduino/firmware stubs in stubs/, builds netlists
// the way NetManager would, runs bridgesToPaths(), then unions every crosspoint
// the paths would send onto a physical wire model of the OG crossbar (derived
// from board_og.cpp: two lanes between every pair of breadboard chips, one lane
// from each breadboard chip to I/J/K, Y0 of every breadboard chip = chip L's Y
// for it, shared SF nodes on one wire) and reports:
//   SHORT        - two different nets end up on one piece of copper
//   STRAY-DRIVEN - a source (GND/3V3/5V/DAC/UART_TX/GPIO_0) in NO net is on a net
//   STRAY        - a row in no net is on a net (ADC / header-pin bounce lanes are
//                  the reference router's own practice and only get a note)
//   OPEN         - a bridge whose two nodes did not end up connected (functional;
//                  counted, never a failure - some netlists are unroutable)
//
// Modes:  fuzz_routing_og [iters seed maxBridges]        random netlists (default 2000 1 12)
//         fuzz_routing_og case GND-D6 3V3-D1 ...         one netlist, prints the routes
//         fuzz_routing_og regress                        the bench cases below, asserted
//   env: ONLY=SHORT (show only that class), SHOWDROPS=1, SHOWOPEN=1, ROWBIAS=75,
//        TRACE=1 (router debug prints, case mode), STACK=n (stack_paths/rails)
//
// 2026-09-22: on the shipped OG build this found GND-D6 + 3V3-D1 shorting GND to
// 3.3 V through chip H's hub line (Lchip never reset after clearAllNTCC's memset),
// 5V-D9 hopping onto the hub DAC0-ADC0 sat on, hop-chip lane indices written
// into chip L's X slot (row 60 / DAC0 / GPIO_0 closed onto rows), and y = 8
// leaking into a path (sendPath masks it to y0). The OG build had no validator
// between the router and the crossbar at all.
#include <Arduino.h>
#include <EEPROM.h>
#include "JumperlessDefines.h"
#include "config.h"
#include "MatrixState.h"
#include "States.h"
#include "NetManager.h"
#include "NetsToChipConnections.h"
#include "boards/board.h"
#include "FakeGpio.h"
#include "Peripherals.h"
#include "Graphics.h"
#include "InfraPaths.h"
#include "RouteSafety.h"
#include "CH446Q.h"

#include <vector>
#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <ctype.h>

bool g_quiet = true;
Stream Serial;
Stream Jerial;
EEPROMClass EEPROM;
JumperlessState globalState;
struct config jumperlessConfig;
int8_t nodeToNetIndex[256];
int newBridgeLength = 0;
int numberOfShownNets = 0;
int gpioNet[50];
uint8_t gpioReading[50];
uint32_t gpioReadingColors[50];
int showADCreadings[8];
const int gpioDef[10][3] = {{20, RP_GPIO_1, 0}, {21, RP_GPIO_2, 1}, {22, RP_GPIO_3, 2}, {23, RP_GPIO_4, 3},
                            {24, RP_GPIO_5, 4}, {25, RP_GPIO_6, 5}, {26, RP_GPIO_7, 6}, {27, RP_GPIO_8, 7},
                            {0, RP_UART_TX, 8}, {1, RP_UART_RX, 9}};
FakeGpioOutput fakeGpioOutputs[MAX_FAKE_GP_OUT];
FakeGpioInput fakeGpioInputs[MAX_FAKE_GP_IN];
int fakeGpioInputAdcChannel = -1;

// The V5 nano table the firmware ships on both boards (MatrixState.cpp).
struct nanoStatus nano = {
    {" D0", " D1", " D2", " D3", " D4", " D5", " D6", " D7", " D8", " D9", "D10", "D11", "D12", "D13", "RST", "REF", " A0", " A1", " A2", " A3", " A4", " A5", " A6", " A7"},
    {NANO_D0, NANO_D1, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9, NANO_D10, NANO_D11, NANO_D12, NANO_D13, NANO_RESET, NANO_AREF, NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6, NANO_A7},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_I, CHIP_K, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J, CHIP_I, CHIP_J},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {-1, 1, -1, 3, -1, 5, -1, 7, -1, 9, -1, 8, -1, 10, 11, -1, 0, -1, 2, -1, 4, -1, 6, -1},
    {-1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1},
    {0, -1, 2, -1, 4, -1, 6, -1, 8, -1, 9, -1, 10, -1, -1, 11, -1, 1, -1, 3, -1, 5, -1, 7},
    {0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0},
    {-1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, -1, -1, -1, 0, 1, 2, 3, -1, -1, -1, -1},
    {-1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, 0, 0, 0, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1},
    {0},
};

// ---- firmware functions the router calls that live elsewhere -----------------
void updateLiveCrossbarDisplay(void) {}
chipXYBitfield lastChipXY[12];
int ch446q_timeout_count = 0;
void sendXYrawUnchecked(int, int, int, int, unsigned long) {}
extern int numberOfUnconnectablePaths;
void changeTerminalColor(int, bool, Stream*) {}
void assignTermColor(int) {}
bool infraIsBridge(int, int) { return false; }
bool connectionAllowed(int, int) { return true; }
const char* definesToChar(int d, int) {
    static char buf[16];
    snprintf(buf, sizeof buf, "%d", d);
    return buf;
}
int printNodeOrName(int node, int, int, Stream* s) { s->print(node); return 1; }
void printBridgeArray(Stream*) {}
void initChipStatus(void) {}

static const netStruct kSpecialNets[] = {
    {127, "Empty Net", {EMPTY_NET}, {{}}, EMPTY_NET, {EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET, EMPTY_NET}, 0},
    {1, "GND", {GND}, {{}}, GND, {BOTTOM_RAIL, TOP_RAIL, DAC1}, 1},
    {2, "Top Rail", {TOP_RAIL}, {{}}, TOP_RAIL, {GND, BOTTOM_RAIL, DAC0, DAC1}, 1},
    {3, "Bottom Rail", {BOTTOM_RAIL}, {{}}, BOTTOM_RAIL, {GND, TOP_RAIL, DAC0, DAC1}, 1},
    {4, "DAC 0", {DAC0}, {{}}, DAC0, {TOP_RAIL, BOTTOM_RAIL, DAC1}, 1},
    {5, "DAC 1", {DAC1}, {{}}, DAC1, {GND, TOP_RAIL, BOTTOM_RAIL, DAC0}, 1},
};

// Mirrors MatrixState.cpp initNets(): special nets, chip status from the board.
void initNets(void) {
    for (int i = 0; i < 6; i++) {
        globalState.connections.nets[i] = kSpecialNets[i];
        globalState.connections.nets[i].priority = 1;
    }
    const auto& b = board::currentBoard();
    for (int c = 0; c < 12; c++) {
        auto& cs = globalState.connections.chipStates[c];
        cs.chipNumber = c;
        cs.chipChar = 'A' + c;
        cs.uncommittedHops = 0;
        for (int x = 0; x < 16; x++) { cs.xStatus[x] = -1; cs.xMap[x] = b.xMap[c][x]; }
        for (int y = 0; y < 8; y++) { cs.yStatus[y] = -1; cs.yMap[y] = b.yMap[c][y]; }
    }
    initRouteSafety();
    for (int i = 6; i < MAX_NETS; i++) {
        globalState.connections.nets[i] = {0, " ", {}, {{}}, 0, {}, 0, false, {0, 0, 0}, 0, 0, false};
        globalState.connections.nets[i].priority = 1;
        globalState.connections.nets[i].termColor = 15;
    }
}

// ---- netlist builder (what NetManager would produce) ---------------------------
struct Bridge { int a, b; };

static bool isSupplyLike(int n) {
    return n == GND || n == SUPPLY_5V || n == SUPPLY_3V3 || n == DAC0 || n == DAC1;
}

// Returns false if the bridge set is not a legal netlist (two supplies in one net).
static bool buildNetlist(const std::vector<Bridge>& bridges) {
    std::map<int, int> parent;
    auto find = [&](int n) { while (parent.count(n) && parent[n] != n) n = parent[n]; return n; };
    for (auto& br : bridges) {
        if (!parent.count(br.a)) parent[br.a] = br.a;
        if (!parent.count(br.b)) parent[br.b] = br.b;
        int ra = find(br.a), rb = find(br.b);
        if (ra != rb) parent[ra] = rb;
    }
    // group nodes by root
    std::map<int, std::vector<int>> groups;
    for (auto& kv : parent) groups[find(kv.first)].push_back(kv.first);
    // assign net numbers
    std::map<int, int> rootNet;
    int nextNet = 6;
    for (auto& g : groups) {
        int supplies = 0, net = -1;
        for (int n : g.second) {
            if (isSupplyLike(n)) supplies++;
            if (n == GND) net = 1;
            else if (n == DAC0) net = 4;
            else if (n == DAC1) net = 5;
        }
        if (supplies > 1) return false;
        if (net < 0) net = nextNet++;
        if (net >= MAX_NETS - 2) return false;
        rootNet[g.first] = net;
    }
    memset(nodeToNetIndex, -1, sizeof nodeToNetIndex);
    for (auto& g : groups) {
        int net = rootNet[g.first];
        netStruct& ns = globalState.connections.nets[net];
        ns.number = net;
        ns.name = "net";
        int ni = 0;
        // special nets already carry their anchor node
        if (net >= 6) { for (int k = 0; k < MAX_NODES; k++) ns.nodes[k] = 0; }
        else { ni = 1; }
        for (int n : g.second) {
            bool dup = false;
            for (int k = 0; k < ni; k++) if (ns.nodes[k] == n) dup = true;
            if (!dup && ni < MAX_NODES) ns.nodes[ni++] = n;
            nodeToNetIndex[n] = net;
        }
        int bi = 0;
        for (int k = 0; k < MAX_NODES; k++) { ns.bridges[k][0] = 0; ns.bridges[k][1] = 0; }
        for (auto& br : bridges) {
            if (find(br.a) != g.first) continue;
            if (bi < MAX_NODES) { ns.bridges[bi][0] = br.a; ns.bridges[bi][1] = br.b; bi++; }
        }
    }
    globalState.connections.numBridges = 0;
    for (auto& br : bridges) {
        int i = globalState.connections.numBridges++;
        globalState.connections.bridges[i][0] = br.a;
        globalState.connections.bridges[i][1] = br.b;
        globalState.connections.bridges[i][2] = -1;
    }
    return true;
}

// ---- physical wire model of the OG crossbar ----------------------------------------
// wire id per (chip, x) and (chip, y); nodes get their own wire; lanes are shared.
static int wireX[12][16], wireY[12][8];
static std::map<int, int> nodeWire;          // node -> wire
static std::map<int, int> wireNodeOf;        // wire -> node (only real nodes)
static int numWires = 0;

static int laneWire(int c, int d, int lane) {
    static std::map<std::tuple<int, int, int>, int> m;
    auto key = std::make_tuple(std::min(c, d), std::max(c, d), lane);
    if (!m.count(key)) m[key] = numWires++;
    return m[key];
}
static int hubWire(int bbChip) {  // BB chip Y0 <-> L Y[bbChip]
    static std::map<int, int> m;
    if (!m.count(bbChip)) m[bbChip] = numWires++;
    return m[bbChip];
}
static int wireForNode(int node) {
    if (!nodeWire.count(node)) { nodeWire[node] = numWires; wireNodeOf[numWires] = node; numWires++; }
    return nodeWire[node];
}
static void buildWires() {
    const auto& b = board::currentBoard();
    for (int c = 0; c < 8; c++) {
        for (int x = 0; x < 16; x++) {
            int d = b.xMap[c][x];
            if (d < 8) {
                int lane = 0;
                for (int px = 0; px < x; px++) if (b.xMap[c][px] == d) lane++;
                wireX[c][x] = laneWire(c, d, lane);
            } else {
                wireX[c][x] = laneWire(c, d, 0);  // BB x lane to SF chip d == d's Y[c]
            }
        }
        wireY[c][0] = hubWire(c);
        for (int y = 1; y < 8; y++) wireY[c][y] = wireForNode(b.yMap[c][y]);
    }
    for (int c = 8; c < 12; c++) {
        for (int x = 0; x < 16; x++) wireX[c][x] = wireForNode(b.xMap[c][x]);
        for (int y = 0; y < 8; y++) wireY[c][y] = (c == CHIP_L) ? hubWire(y) : laneWire(y, c, 0);
    }
}

static bool isDriven(int n) {
    return n == GND || n == SUPPLY_5V || n == SUPPLY_3V3 || n == DAC0 || n == DAC1 || n == RP_UART_TX || n == RP_GPIO_0;
}
static bool isHighZ(int n) { return (n >= ADC0 && n <= ADC3) || n == ISENSE_PLUS || n == ISENSE_MINUS || n == RP_UART_RX || n == NANO_AREF; }
static bool isNano(int n) { return n >= NANO_D0 && n <= NANO_A7; }

static std::string nodeName(int n) {
    char buf[32];
    switch (n) {
    case GND: return "GND"; case SUPPLY_5V: return "5V"; case SUPPLY_3V3: return "3V3";
    case DAC0: return "DAC0"; case DAC1: return "DAC1"; case ADC0: return "ADC0"; case ADC1: return "ADC1";
    case ADC2: return "ADC2"; case ADC3: return "ADC3"; case ISENSE_PLUS: return "ISENSE+"; case ISENSE_MINUS: return "ISENSE-";
    case RP_GPIO_0: return "GPIO_0"; case RP_UART_TX: return "UART_TX"; case RP_UART_RX: return "UART_RX";
    case NANO_RESET: return "RST"; case NANO_AREF: return "AREF";
    }
    if (n >= NANO_D0 && n <= NANO_D13) { snprintf(buf, sizeof buf, "D%d", n - NANO_D0); return buf; }
    if (n >= NANO_A0 && n <= NANO_A7) { snprintf(buf, sizeof buf, "A%d", n - NANO_A0); return buf; }
    snprintf(buf, sizeof buf, "%d", n);
    return buf;
}

struct UF {
    std::vector<int> p;
    UF(int n) : p(n) { for (int i = 0; i < n; i++) p[i] = i; }
    int find(int a) { while (p[a] != a) { p[a] = p[p[a]]; a = p[a]; } return a; }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[a] = b; }
};

struct XP { int chip, x, y, path, net; };
static long g_open = 0, g_bridges = 0, g_skipped = 0; static int g_openShown = 0;

static void printPaths() {
    for (int i = 0; i < numberOfPaths; i++) {
        auto& p = globalState.connections.paths[i];
        printf("  path[%2d] net %2d %s-%s%s%s :", i, p.net, nodeName(p.node1).c_str(), nodeName(p.node2).c_str(),
               p.skip ? " SKIP" : "", p.duplicate ? " dup" : "");
        for (int h = 0; h < 4; h++) {
            if (p.chip[h] < 0) continue;
            printf(" %c[x%d,y%d]", 'A' + p.chip[h], p.x[h], p.y[h]);
        }
        printf("\n");
    }
}

// Returns number of hard failures. Prints details when verbose.
static int checkRouting(const std::vector<Bridge>& bridges, bool verbose, std::string& tag) {
    std::vector<XP> xps;
    for (int i = 0; i < numberOfPaths; i++) {
        auto& p = globalState.connections.paths[i];
        if (p.skip || p.net <= 0 || p.pathType == VIRTUAL) continue;
        for (int h = 0; h < 4; h++) {
            if (p.chip[h] < 0 || p.x[h] < 0 || p.y[h] < 0) continue;
            if (p.chip[h] >= 12 || p.x[h] >= 16 || p.y[h] >= 8) { if (verbose) printf("  OOB crosspoint path %d\n", i); continue; }
            xps.push_back({p.chip[h], p.x[h], p.y[h], i, p.net});
        }
    }
    UF uf(numWires);
    for (auto& c : xps) uf.unite(wireX[c.chip][c.x], wireY[c.chip][c.y]);

    int fails = 0;
    // per component: nets seen, nodes seen
    std::map<int, std::set<int>> compNets, compNodes;
    for (auto& kv : wireNodeOf) {
        int w = kv.first, node = kv.second;
        int r = uf.find(w);
        compNodes[r].insert(node);
        int net = nodeToNetIndex[node];
        if (net > 0) compNets[r].insert(net);
    }
    for (auto& kv : compNets) {
        int r = kv.first;
        auto& nets = kv.second;
        auto& nodes = compNodes[r];
        if (nets.size() > 1) {
            fails++;
            tag = "SHORT";
            if (verbose) {
                printf("  SHORT: one copper component carries nets");
                for (int n : nets) printf(" %d", n);
                printf(" nodes:");
                for (int n : nodes) printf(" %s(net %d)", nodeName(n).c_str(), nodeToNetIndex[n]);
                printf("\n");
            }
        } else if (nets.size() == 1) {
            int net = *nets.begin();
            for (int n : nodes) {
                if (nodeToNetIndex[n] == net) continue;
                // a node from no net riding on this net's copper
                if (isDriven(n)) {
                    fails++;
                    tag = "STRAY-DRIVEN";
                    if (verbose) printf("  STRAY: driven node %s (no net) is on net %d's copper\n", nodeName(n).c_str(), net);
                } else if (!isHighZ(n) && !isNano(n)) {
                    fails++;
                    tag = "STRAY";
                    if (verbose) printf("  STRAY: node %s (no net) is on net %d's copper\n", nodeName(n).c_str(), net);
                } else if (verbose) {
                    printf("  note: %s (no net) used as bounce lane by net %d\n", nodeName(n).c_str(), net);
                }
            }
        }
    }
    // connectivity
    for (auto& br : bridges) {
        if (!nodeWire.count(br.a) || !nodeWire.count(br.b)) continue;
        if (uf.find(nodeWire[br.a]) != uf.find(nodeWire[br.b])) {
            g_open++;
            if (verbose) printf("  OPEN: %s-%s not connected\n", nodeName(br.a).c_str(), nodeName(br.b).c_str());
            if (getenv("SHOWOPEN") && !verbose && g_openShown < 8) { g_openShown++; printf("\n=== OPEN %s-%s in:", nodeName(br.a).c_str(), nodeName(br.b).c_str()); for (auto& b2 : bridges) printf(" %s-%s", nodeName(b2.a).c_str(), nodeName(b2.b).c_str()); printf("\n"); printPaths(); }
        }
    }
    g_bridges += bridges.size();
    return fails;
}

static int routeAndCheck(const std::vector<Bridge>& bridges, bool verbose, std::string& tag) {
    clearAllNTCC();
    if (!buildNetlist(bridges)) return -1;
    bridgesToPaths();
    return checkRouting(bridges, verbose, tag);
}

static const int kNodePool[] = {
    GND, SUPPLY_5V, SUPPLY_3V3, DAC0, DAC1, ADC0, ADC1, ADC2, ADC3, RP_GPIO_0, RP_UART_TX, RP_UART_RX,
    NANO_D0, NANO_D1, NANO_D2, NANO_D3, NANO_D4, NANO_D5, NANO_D6, NANO_D7, NANO_D8, NANO_D9, NANO_D10, NANO_D11, NANO_D12, NANO_D13,
    NANO_A0, NANO_A1, NANO_A2, NANO_A3, NANO_A4, NANO_A5, NANO_A6, NANO_A7, NANO_RESET, NANO_AREF,
};

static int randNode(int rowBias) {
    if (rand() % 100 < rowBias) return 1 + rand() % 60;
    return kNodePool[rand() % (sizeof kNodePool / sizeof kNodePool[0])];
}

static int parseNode(const char* t) {
    struct { const char* n; int v; } tab[] = {{"GND",GND},{"5V",SUPPLY_5V},{"3V3",SUPPLY_3V3},{"DAC0",DAC0},{"DAC1",DAC1},
        {"ADC0",ADC0},{"ADC1",ADC1},{"ADC2",ADC2},{"ADC3",ADC3},{"ISENSE+",ISENSE_PLUS},{"ISENSE-",ISENSE_MINUS},
        {"GPIO_0",RP_GPIO_0},{"UART_TX",RP_UART_TX},{"UART_RX",RP_UART_RX},{"RST",NANO_RESET},{"AREF",NANO_AREF}};
    for (auto& e : tab) if (!strcmp(t, e.n)) return e.v;
    if (t[0] == 'D' && isdigit(t[1])) return NANO_D0 + atoi(t + 1);
    if (t[0] == 'A' && isdigit(t[1])) return NANO_A0 + atoi(t + 1);
    return atoi(t);
}
extern bool debugNTCC, debugNTCC2, debugNTCC3, debugNTCC5, debugNTCC6;

int main(int argc, char** argv) {
    buildWires();
    jumperlessConfig.routing.stack_paths = 2;
    jumperlessConfig.routing.stack_rails = 3;
    for (int i = 0; i < 50; i++) gpioNet[i] = -1;
    if (argc > 1 && !strcmp(argv[1], "regress")) {
        // Each line: a netlist that shorted or mis-routed on the 2026-09-22 bench
        // build. Must route with no SHORT/STRAY and every bridge connected.
        // Every one of these failed on the 2026-09-22 bench build (8/8). The lane-1
        // slot bug, the paired -2 bounce and the hop -2 need a busier board to
        // show and are left to the fuzz mode.
        const char* cases[][8] = {
            {"GND-D6", "3V3-D1", 0},                 // GND<->3V3 through chip H's hub
            {"D6-A0", 0},                             // I<->J hop: chip[3] was never set
            {"5V-D9", "DAC0-ADC0", 0},               // 5V onto DAC0's hub line
            {"53-UART_TX", "60-AREF", 0},             // hop-chip lane index in chip L's X slot -> GPIO_0
            {"23-1", "A5-DAC0", 0},                   // row 1 (node 1 == CHIP_B) read as a lane by the validator
            {"D5-1", 0},                              // NANO->L routed as breadboard->L: y = 8
            {"D7-5V", "A7-48", "29-10", "55-40", "4-8", "13-GND", "49-35", "53-27"}, // Lchip decided before chips
            {"AREF-A6", "AREF-60", 0},                // AREF on the V5 nano table (chip K)
        };
        int failures = 0;
        for (auto& c : cases) {
            std::vector<Bridge> bridges;
            for (int i = 0; i < 8 && c[i]; i++) {
                char buf[64]; strncpy(buf, c[i], 63); buf[63] = 0;
                char* dash = strchr(buf, '-'); *dash = 0;
                bridges.push_back({parseNode(buf), parseNode(dash + 1)});
            }
            long openBefore = g_open;
            std::string tag;
            int r = routeAndCheck(bridges, true, tag);
            bool ok = (r == 0) && (g_open == openBefore);
            printf("%s:", ok ? "ok  " : "FAIL");
            for (auto& br : bridges) printf(" %s-%s", nodeName(br.a).c_str(), nodeName(br.b).c_str());
            printf("\n");
            if (!ok) { printPaths(); failures++; }
        }
        printf("regress: %d failing\n", failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "case")) {
        std::vector<Bridge> bridges;
        for (int i = 2; i < argc; i++) {
            char buf[64]; strncpy(buf, argv[i], 63); buf[63] = 0;
            char* dash = strchr(buf, '-');
            if (!dash) continue;
            *dash = 0;
            bridges.push_back({parseNode(buf), parseNode(dash + 1)});
        }
        if (getenv("STACK")) { jumperlessConfig.routing.stack_paths = atoi(getenv("STACK")); jumperlessConfig.routing.stack_rails = atoi(getenv("STACK")); }
        if (getenv("TRACE")) { g_quiet = false; debugNTCC = debugNTCC2 = debugNTCC3 = debugNTCC6 = true; }
        std::string tag;
        int r = routeAndCheck(bridges, true, tag);
        g_quiet = true;
        printPaths();
        printf("result: %d (%s)\n", r, tag.c_str());
        return r > 0 ? 1 : 0;
    }
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
    int maxBridges = argc > 3 ? atoi(argv[3]) : 12;
    srand(seed);
    printf("OG fabric: %d wires, board %s\n", numWires, board::currentBoard().name);

    std::map<std::string, int> tally;
    int legal = 0, shown = 0;
    for (int it = 0; it < iters; it++) {
        int n = 1 + rand() % maxBridges;
        std::vector<Bridge> bridges;
        for (int i = 0; i < n; i++) {
            int rb = getenv("ROWBIAS") ? atoi(getenv("ROWBIAS")) : 60; int a = randNode(rb), b = randNode(rb);
            if (a == b) continue;
            bridges.push_back({a, b});
        }
        std::string tag;
        int r = routeAndCheck(bridges, false, tag);
        if (r < 0) continue;
        legal++;
        int dropped = 0;
        for (int i = 0; i < numberOfPaths; i++) if (globalState.connections.paths[i].skip && !globalState.connections.paths[i].duplicate) dropped++;
        g_skipped += dropped;
        if (dropped && r == 0 && getenv("SHOWDROPS") && shown < 4) {
            shown++;
            printf("\n=== DROP iter %d, bridges:", it);
            for (auto& br : bridges) printf(" %s-%s", nodeName(br.a).c_str(), nodeName(br.b).c_str());
            printf("\n");
            extern bool debugNTCC; g_quiet = false; debugNTCC = true;
            std::string t2; routeAndCheck(bridges, true, t2);
            g_quiet = true; debugNTCC = false;
            printPaths();
        }
        if (r > 0) {
            tally[tag]++;
            const char* want = getenv("ONLY");
            if (shown < 6 && (!want || tag == want)) {
                shown++;
                printf("\n=== FAIL #%d (%s) seed %u iter %d, bridges:", shown, tag.c_str(), seed, it);
                for (auto& br : bridges) printf(" %s-%s", nodeName(br.a).c_str(), nodeName(br.b).c_str());
                printf("\n");
                std::string t2;
                routeAndCheck(bridges, true, t2);
                printPaths();
            }
        }
    }
    printf("\n%d legal netlists routed;", legal);
    int total = 0;
    for (auto& kv : tally) { printf(" %s=%d", kv.first.c_str(), kv.second); total += kv.second; }
    printf(" (%d failing)\n", total);
    printf("bridges %ld, open (not connected) %ld (%.2f%%), primary paths dropped %ld\n", g_bridges, g_open, 100.0 * g_open / (g_bridges ? g_bridges : 1), g_skipped);
    return total ? 1 : 0;
}
