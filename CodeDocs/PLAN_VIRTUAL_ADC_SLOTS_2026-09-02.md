# Virtual ADC Slots Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `ADC_n` becomes a virtual, auto-assigned slot: any number of ADCs can be placed, the first few keep a dedicated physical channel and a static route exactly as today, and the rest are sampled round-robin through a fresh dynamically-routed tap per poll instead of a permanent chip-K row.

**Architecture:** A new `src/sensing/VirtualAdc.{h,cpp}` owns a 16-entry slot table. Every bridge add funnels through `JumperlessState::addConnection`, where physical ids (110-114) and the `ADC_AUTO` sentinel are resolved to slot node ids 200-215. At the head of every rebuild `vadcEvaluate()` assigns at most three physical channels (ADC0-3) to placed slots through the InfraPaths pool; the router expands a dedicated slot's node to its physical node (the FakeGPIO-input precedent) and marks a shared slot's path `SENSE`, which every routing stage skips. Shared slots are sampled on core 1 from `serviceNetVoltageScan` with the scan's `fastConnectPath` tap primitive (route built fresh per poll from live occupancy, claimed as `EPHEMERAL_PATH_NET`, torn down after the read). A dedicated slot whose path fails to route is demoted to shared and the routing portion of that rebuild runs once more.

**Tech Stack:** C++ (Arduino/RP2350 via PlatformIO), MicroPython bindings (`modules/jumperless/modjumperless.c`), HIL tests in `test/hil/*.py` over CDC port 5.

**Spec:** `CodeDocs/adc_design_reports/0_design_note_from_chat.md` (the design note from the 2026-09-02 ADC chat; Kevin's two rulings are saved as memory `adc-virtual-slots-ruling`, and the MicroPython compatibility rule as `micropython-api-backward-compat`). Supporting inventories from that session's design agents: `CodeDocs/adc_design_reports/1_tap_primitive_and_tdm.md` (the scan's tap primitive, TDM's fragilities, display consumers, the recommendation to route per poll), `2_path_lifecycle.md` (bridge → path → chip-K lane, what a failed path leaves behind), `3_adc_site_inventory.md` (every place that picks, displays or stores an ADC number). Status and context: `CodeDocs/VIRTUAL_ADC_HANDOFF_2026-09-02.md`.

## Global Constraints

- Build both targets with the Python-3.13 venv: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og` (system `pio` is broken under Python 3.14).
- OG (`NetsToChipConnections_OG.cpp`, excluded from V5 builds) is NOT changed by this plan; every new symbol the OG build sees must compile there (guard router-internal helpers with the V5 file, keep shared headers OG-safe).
- A single placed ADC must behave byte-identically to today: same physical channel class (ADC0-3), static path, full-rate ring reads, scope app and USB audio untouched.
- `ADC_7` (node 115) stays the probe tip and is never assigned as a slot. Physical ADC4 (node 114, chip L, 0-5 V) stops being a user node; a user request for it resolves to `ADC_AUTO`.
- Never take the last free physical channel for a dedicated slot: the scan, shared slots and guide one-shots need one channel per tap.
- No stored crosspoint state for shared slots: every crosspoint, lane claim and ADC-pool acquisition for a shared sample is per tap (the TDM stored-hop design is the documented bug class - see report 1 §B7).
- Never stack more than one tap in a core-2 pass (the 25 ms `waitCore2` contract, `NetVoltageScan.cpp` comment near `:1254`).
- Commit only after the task's build and its listed verification pass; stage files explicitly (`.pio/**/firmware.uf2` is tracked, never `git add -A`); never push.
- Bench work goes through port 5 (`test/hil/jl.py: jl_exec`) and port 7 (`test/hil/port7.py`); Kevin's client holds port 1. Flush saved state (`jumperless.nodes_save(-1)`) before any touch-flash.

---

## Part A - Virtual slots with dedicated channels (ships on its own)

After Part A: every surface still works, `ADC0..ADC3` names and `connect(5, ADC0)` resolve to slots, at most three slots get physical channels, a fourth placed ADC is refused with a clear message instead of a silent `-1` path, and nothing shorts. No UI is removed yet.

### Task A1: Node ids, names, validity

**Files:**
- Modify: `src/JumperlessDefines.h:211` (after `BOUNCE_NODE`)
- Modify: `src/routing/NetManager.cpp:98-115` (`specialDefines[]`)
- Modify: `src/remembering/FileParsing.cpp:2051-2067` (`isNodeValid`)
- Modify: `src/routing/States.cpp:2779` (`parseNodeName` numeric ceiling)
- Test: `test/hil/test_virtual_adc.py` (new; first check added here)

**Interfaces:**
- Produces: `#define VADC_0 200 … VADC_15 215`, `#define ADC_AUTO 216`, `#define VADC_COUNT 16`, `#define IS_VADC(n) ((n) >= VADC_0 && (n) <= VADC_15)`, `#define VADC_SLOT(n) ((n) - VADC_0)`, `#define VADC_PROBE_SLOT 7`, `#define VADC_MEASURE_SLOT 15`.
- Name table: `"ADC_0".."ADC_14"` (short) / `"ADC0".."ADC14"` (long) → `VADC_n`; `"ADC"` / `"ADC_AUTO"` → `ADC_AUTO`; physical entries renamed `"ADC0_HW".."ADC4_HW"` (short) / `"ADC_HW0".."ADC_HW4"` (long).

- [ ] **Step 1: Add the ids**

In `src/JumperlessDefines.h` right after `#define BOUNCE_NODE 199`:

```c
// Virtual ADC slots (CodeDocs/PLAN_VIRTUAL_ADC_SLOTS_2026-09-02.md). A user
// never names a physical channel: ADC_n is a slot, VirtualAdc.cpp maps it to
// ADC0-3 (dedicated) or to a shared time-multiplexed tap. 200-215 sit below
// the nodeToNetIndex[256] ceiling. Slot 7 is reserved so ADC_7 / adc_get(7)
// stay the probe tip; slot 15 is reserved for measure mode.
#define VADC_0   200
#define VADC_15  215
#define VADC_COUNT 16
#define ADC_AUTO 216   // "the next free user slot" - resolved when a bridge is added
#define IS_VADC(n)   ((n) >= VADC_0 && (n) <= VADC_15)
#define VADC_NODE(s) (VADC_0 + (s))
#define VADC_SLOT(n) ((n) - VADC_0)
#define VADC_PROBE_SLOT   7
#define VADC_MEASURE_SLOT 15
```

- [ ] **Step 2: Name table**

In `src/routing/NetManager.cpp` `specialDefines[]`, insert BEFORE the `{"TOP_R", ...}` entry (parseNodeName returns the first name match, so slots must precede the physical rows):

```c
    {"ADC",      "ADC_AUTO",    ADC_AUTO},      // 216 - auto slot
    {"ADC_0",    "ADC0",        VADC_0 + 0},    // 200
    {"ADC_1",    "ADC1",        VADC_0 + 1},
    {"ADC_2",    "ADC2",        VADC_0 + 2},
    {"ADC_3",    "ADC3",        VADC_0 + 3},
    {"ADC_4",    "ADC4",        VADC_0 + 4},
    {"ADC_5",    "ADC5",        VADC_0 + 5},
    {"ADC_6",    "ADC6",        VADC_0 + 6},
    {"ADC_8",    "ADC8",        VADC_0 + 8},
    {"ADC_9",    "ADC9",        VADC_0 + 9},
    {"ADC_10",   "ADC10",       VADC_0 + 10},
    {"ADC_11",   "ADC11",       VADC_0 + 11},
    {"ADC_12",   "ADC12",       VADC_0 + 12},
    {"ADC_13",   "ADC13",       VADC_0 + 13},
    {"ADC_14",   "ADC14",       VADC_0 + 14},
```

and rename the physical rows 110-114 (keep their values):

```c
    {"ADC0_HW",  "ADC_HW0",     ADC0},          // 110 - internal pool channel
    {"ADC1_HW",  "ADC_HW1",     ADC1},          // 111
    {"ADC2_HW",  "ADC_HW2",     ADC2},          // 112
    {"ADC3_HW",  "ADC_HW3",     ADC3},          // 113
    {"ADC4_HW",  "ADC_HW4",     ADC4},          // 114
```

`{"ADC_7", "ADC7", ADC7}` (115) stays exactly as it is.

- [ ] **Step 3: Validity and numeric parse ceiling**

`src/remembering/FileParsing.cpp` `isNodeValid`, add a range:

```c
  } else if (node >= VADC_0 && node <= ADC_AUTO) {
    withinRange = true;
```

`src/routing/States.cpp:2779` change `if (val >= 0 && val <= 200)` to `if (val >= 0 && val <= ADC_AUTO)`.

- [ ] **Step 4: Build both targets**

Run: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og`
Expected: SUCCESS for both. (`definesToChar(200)` now returns `"ADC0"` through `findDefineInfoByValue`; `definesToChar(110)` returns `"ADC_HW0"`.)

- [ ] **Step 5: First HIL check (slot node 200 is a valid node and prints as ADC_0)**

MicroPython exposes only integer node constants and `is_connected`; there is no name parser binding, so the tests use raw ids (`200` = `VADC_0`, `216` = `ADC_AUTO`) and read names back through port 7's `N` JSON. Create `test/hil/test_virtual_adc.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Virtual ADC slots: ids, auto-assignment, dedicated routing, readback."""
import json, time
from jl import jl_exec, parse_kv, check, finish
from port7 import port7_command

VADC_0 = 200
ADC_AUTO = 216

# --- 1. node 200 is valid, bridges, and is named ADC_0 in the netlist --------
out = jl_exec("""
import time
nodes_clear(); time.sleep(0.1)
connect(10, 200)
time.sleep(0.2)
print("connected=", 1 if is_connected(10, 200) else 0)
""")
vals = parse_kv(out)
check(vals.get("connected") == 1, "connect(10, 200) is accepted (isNodeValid covers the slot ids)")
time.sleep(0.5)
nets = json.loads(port7_command("N", 2).strip() or "{}").get("nets", [])
names = [n for net in nets for n in net.get("nodes", []) if "10" in net.get("nodes", [])]
check("ADC_0" in names, "row 10's net lists the slot as ADC_0 (name table)")
jl_exec("nodes_clear()")

finish("test_virtual_adc")
```

Run: `cd test/hil && python3 test_virtual_adc.py`
Expected: `test_virtual_adc: PASS (2 checks)`.

- [ ] **Step 6: Commit**

```bash
git add src/JumperlessDefines.h src/routing/NetManager.cpp src/remembering/FileParsing.cpp src/routing/States.cpp test/hil/test_virtual_adc.py
git commit -m "Virtual ADC slots: node ids 200-216, names, validity"
```

### Task A2: The slot table and the bridge-time resolver

**Files:**
- Create: `src/sensing/VirtualAdc.h`, `src/sensing/VirtualAdc.cpp`
- Modify: `src/routing/States.cpp:595` (`JumperlessState::addConnection`, before `isNodeValid`)
- Modify: `src/routing/InfraPaths.h:159-180` (new pool enumerators)
- Test: `test/hil/test_virtual_adc.py`

**Interfaces:**
- Produces (`VirtualAdc.h`):

```c
struct VadcSlot {
    bool placed;        // some bridge in bridges[] names VADC_NODE(slot)
    bool shared;        // sampled by the core-1 tap sampler (Part B); false = dedicated
    int8_t channel;     // physical ADC0-3 index when dedicated, -1 otherwise
    int netIndex;       // net the slot belongs to (0 = none), refreshed per rebuild
    int rowNode;        // first breadboard row (1-60) bridged to this slot, -1 if none
    float volts;        // shared: last tap; dedicated: last ring read (cache)
    uint32_t sampleMs;  // millis() of volts, 0 = never
    float drift;        // shared: early/late divergence of the last tap
    bool routeFailed;   // dedicated path came back unroutable this rebuild
    uint32_t color;     // LED colour of the last sample (0 = none)
};
extern VadcSlot vadcSlots[VADC_COUNT];
int  vadcResolveForBridge(int node, int otherNode);  // ADC_AUTO / 110-114 -> VADC_n; other nodes unchanged
int  vadcSlotOfNode(int node);                        // 0-15 or -1
int  vadcChannelOfNode(int node);                     // ADC0..ADC3 node id for a dedicated slot, -1 otherwise
bool vadcSlotIsShared(int slot);
void vadcEvaluate(void);                              // rebuild head (Task A3)
bool vadcAfterRouting(void);                          // after bridgesToPaths (Task A4): true = demoted, re-route once
int  vadcSlotForNet(int netIndex);                    // first placed slot on the net, -1
float vadcRead(int slot, bool* fresh);               // Task A5 / B3
void vadcPrintStatus(Stream* out);                    // 'v' terminal + port 7
```

- Pool enumerators added to `InfraAdcUser`: `INFRA_ADC_VADC_A, INFRA_ADC_VADC_B, INFRA_ADC_VADC_C` (one per dedicated slot, because `infraReleaseAdc(user)` frees everything an enumerator owns) and `INFRA_ADC_VADC_SHARED` (per-tap, Part B).

- [ ] **Step 1: Header**

`src/sensing/VirtualAdc.h`:

```c
// SPDX-License-Identifier: MIT
#ifndef VIRTUALADC_H
#define VIRTUALADC_H
#include <Arduino.h>
#include <stdint.h>
#include "JumperlessDefines.h"

// Virtual ADC slots - see CodeDocs/PLAN_VIRTUAL_ADC_SLOTS_2026-09-02.md.
// A slot is "placed" when any bridge names its node. vadcEvaluate() (rebuild
// head) hands physical ADC0-3 to placed slots in priority order through the
// InfraPaths pool; a slot that gets none is "shared" and is sampled by the
// core-1 tap sampler with a fresh route per poll (Part B).
struct VadcSlot {
    bool placed;
    bool shared;
    int8_t channel;
    int netIndex;
    int rowNode;
    float volts;
    uint32_t sampleMs;
    float drift;
    bool routeFailed;
    uint32_t color;
};

extern VadcSlot vadcSlots[VADC_COUNT];

// Bridge-time resolution (called by JumperlessState::addConnection):
//   ADC_AUTO           -> lowest free user slot (0-6, 8-14), or -1 if none
//   110-113 (ADC0-3)   -> VADC_0..3 (old slot files, connect(5, ADC0), undo)
//   114 (ADC4, chip L) -> ADC_AUTO resolution (no longer a user node)
//   anything else      -> unchanged
// A row that is already on a net holding a slot gets THAT slot back
// (never two slots on one net from the UI; explicit VADC_n still merges).
int vadcResolveForBridge(int node, int otherNode);
int vadcSlotOfNode(int node);
int vadcChannelOfNode(int node);
bool vadcSlotIsShared(int slot);
int vadcDedicatedCount(void);
void vadcEvaluate(void);
bool vadcAfterRouting(void);
int vadcSlotForNet(int netIndex);
float vadcRead(int slot, bool* fresh);
void vadcPrintStatus(Stream* out);
#endif
```

- [ ] **Step 2: Table + resolver**

`src/sensing/VirtualAdc.cpp` (first half; `vadcEvaluate`/`vadcAfterRouting`/`vadcRead` are filled in by A3/A4/A5 - add the empty bodies now so it links):

```cpp
// SPDX-License-Identifier: MIT
#include "VirtualAdc.h"
#include "JumperlOS.h"
#include "routing/States.h"
#include "routing/InfraPaths.h"
#include "routing/NetManager.h"
#include "Peripherals.h"

VadcSlot vadcSlots[VADC_COUNT];

static bool userSlot(int s) {
    return s >= 0 && s < VADC_COUNT && s != VADC_PROBE_SLOT && s != VADC_MEASURE_SLOT;
}

// A slot is placed if any persistent or ephemeral bridge names it.
static bool slotNamedInBridges(int slot) {
    ConnectionState& c = globalState.connections;
    int node = VADC_NODE(slot);
    for (int i = 0; i < c.numBridges; i++) {
        if (c.bridges[i][0] == node || c.bridges[i][1] == node) return true;
    }
    return false;
}

static int lowestFreeUserSlot(void) {
    for (int s = 0; s < VADC_COUNT; s++) {
        if (userSlot(s) && !slotNamedInBridges(s)) return s;
    }
    return -1;
}

int vadcSlotOfNode(int node) { return IS_VADC(node) ? VADC_SLOT(node) : -1; }

bool vadcSlotIsShared(int slot) {
    return slot >= 0 && slot < VADC_COUNT && vadcSlots[slot].placed && vadcSlots[slot].shared;
}

int vadcChannelOfNode(int node) {
    int s = vadcSlotOfNode(node);
    if (s < 0 || !vadcSlots[s].placed || vadcSlots[s].shared || vadcSlots[s].channel < 0) return -1;
    return ADC0 + vadcSlots[s].channel;
}

int vadcDedicatedCount(void) {
    int n = 0;
    for (int s = 0; s < VADC_COUNT; s++) {
        if (vadcSlots[s].placed && !vadcSlots[s].shared && vadcSlots[s].channel >= 0) n++;
    }
    return n;
}

int vadcSlotForNet(int netIndex) {
    if (netIndex <= 0) return -1;
    for (int s = 0; s < VADC_COUNT; s++) {
        if (vadcSlots[s].placed && vadcSlots[s].netIndex == netIndex) return s;
    }
    return -1;
}

// Slot already reachable from `row` through the CURRENT bridge set: the
// row itself, or any row on the row's net (nodeToNetIndex is from the last
// rebuild; vadcSlots[].netIndex is too, and for a row's first-ever bridge both
// are 0 - so scan bridges[], never the slot table, here).
static int slotOnRowsNet(int row) {
    ConnectionState& c = globalState.connections;
    int net = (row >= 1 && row <= 60) ? nodeToNetIndex[row] : 0;
    for (int i = 0; i < c.numBridges; i++) {
        int n1 = c.bridges[i][0], n2 = c.bridges[i][1];
        int slotNode = IS_VADC(n1) ? n1 : IS_VADC(n2) ? n2 : -1;
        if (slotNode < 0) continue;
        int other = (slotNode == n1) ? n2 : n1;
        if (other == row) return VADC_SLOT(slotNode);
        if (net > 0 && other >= 1 && other <= 60 && nodeToNetIndex[other] == net) return VADC_SLOT(slotNode);
    }
    return -1;
}

int vadcResolveForBridge(int node, int otherNode) {
    if (node != ADC_AUTO && !(node >= ADC0 && node <= ADC4)) return node;
    if (node >= ADC0 && node <= ADC3) return VADC_NODE(node - ADC0);   // explicit slot
    // ADC_AUTO (and the retired ADC4): a row that already sits on a net with
    // a slot keeps that slot - the UI never puts two slots on one net.
    int existing = slotOnRowsNet(otherNode);
    if (existing >= 0) return VADC_NODE(existing);
    int s = lowestFreeUserSlot();
    return (s >= 0) ? VADC_NODE(s) : -1;
}

void vadcEvaluate(void) {}                  // Task A3
bool vadcAfterRouting(void) { return false; } // Task A4
float vadcRead(int slot, bool* fresh) { if (fresh) *fresh = false; return 0.0f; } // Task A5
void vadcPrintStatus(Stream* out) {          // Task A5 fills the columns
    out->println("virtual ADC slots: (not evaluated yet)");
}
```

- [ ] **Step 3: Pool enumerators**

`src/routing/InfraPaths.h`, inside `enum InfraAdcUser` after `INFRA_ADC_GUIDE`:

```c
    INFRA_ADC_VADC_A,     // virtual ADC slot with a dedicated channel (one
    INFRA_ADC_VADC_B,     //  enumerator per dedicated slot: infraReleaseAdc
    INFRA_ADC_VADC_C,     //  frees EVERY channel an enumerator owns)
    INFRA_ADC_VADC_SHARED,// shared-slot tap sampler (core 1, per-tap acquire)
```

- [ ] **Step 4: Hook the funnel**

`src/routing/States.cpp` `JumperlessState::addConnection`, as the first statements:

```cpp
    // Virtual ADC resolution: ADC_AUTO and the physical ids 110-114 become
    // slot nodes here, the one place every user bridge, slot load and undo
    // replay passes through. Ephemeral bridges (measure mode) use
    // addEphemeralConnection and keep their physical ids.
    {
        int r1 = vadcResolveForBridge(node1, node2);
        int r2 = vadcResolveForBridge(node2, node1);
        if (r1 < 0 || r2 < 0) {
            errorMsg = "No free ADC slot (14 in use)";
            return false;
        }
        node1 = r1;
        node2 = r2;
    }
```

Add `#include "sensing/VirtualAdc.h"` at the top of States.cpp (every `src/` subfolder is on the include path).

- [ ] **Step 5: Build both targets**

Run: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og`
Expected: SUCCESS for both.

- [ ] **Step 6: HIL check - a connect with ADC0 lands as slot 0; ADC (auto) picks the next free**

Append to `test/hil/test_virtual_adc.py` before `finish(...)`:

```python
# --- 2. bridge-time resolution --------------------------------------------
out = jl_exec("""
import time
nodes_clear(); time.sleep(0.1)
connect(10, ADC0)      # physical constant 110 -> remapped to slot 0 (node 200)
connect(20, 216)       # ADC_AUTO -> lowest free user slot (1 -> node 201)
connect(20, 216)       # same row again -> keeps slot 1, no second slot
time.sleep(0.2)
print("has_10_200=", 1 if is_connected(10, 200) else 0)
print("has_20_201=", 1 if is_connected(20, 201) else 0)
print("no_20_202=", 0 if is_connected(20, 202) else 1)
print("no_10_110=", 0 if is_connected(10, 110) else 1)
nodes_clear()
""")
vals = parse_kv(out)
check(vals.get("has_10_200") == 1, "connect(10, ADC0) stored as row 10 - slot 0")
check(vals.get("has_20_201") == 1, "connect(20, ADC_AUTO) auto-assigned slot 1")
check(vals.get("no_20_202") == 1, "re-adding the same row to ADC_AUTO made no second slot")
check(vals.get("no_10_110") == 1, "no bridge carries the physical id 110 any more")
```

Run: `cd test/hil && python3 test_virtual_adc.py`
Expected: PASS (6 checks).

- [ ] **Step 7: Commit**

```bash
git add src/sensing/VirtualAdc.h src/sensing/VirtualAdc.cpp src/routing/States.cpp src/routing/InfraPaths.h test/hil/test_virtual_adc.py
git commit -m "Virtual ADC slots: slot table and bridge-time resolver"
```

### Task A3: The allocator at the rebuild head

**Files:**
- Modify: `src/sensing/VirtualAdc.cpp` (`vadcEvaluate`)
- Modify: `src/Commands.cpp:241`, `:426`, `:632` (each `infraEvaluate();` gets `vadcEvaluate();` right after it)
- Modify: `src/routing/InfraPaths.cpp:812-822` (`infraAdcUserClaimed`)

**Interfaces:**
- Consumes: `infraAcquireAdc(user, mask, allowSharedTdm)`, `infraReleaseAdc(user)`, `infraFreeAdcMask(mask)` (InfraPaths.h), `nodeToNetIndex[]`, `globalState.connections.bridges[]`.
- Produces: after `vadcEvaluate()`, `vadcSlots[s].placed/shared/channel/rowNode` are current for this rebuild; dedicated slots hold their channel under `INFRA_ADC_VADC_A/B/C`.

- [ ] **Step 1: Implement `vadcEvaluate`**

Replace the empty body in `VirtualAdc.cpp`:

```cpp
static const InfraAdcUser kDedicatedUsers[3] = {INFRA_ADC_VADC_A, INFRA_ADC_VADC_B, INFRA_ADC_VADC_C};
static int8_t s_userOfSlot[VADC_COUNT];   // index into kDedicatedUsers, -1 = none

static int firstRowOnSlot(int slot) {
    ConnectionState& c = globalState.connections;
    int node = VADC_NODE(slot);
    for (int i = 0; i < c.numBridges; i++) {
        int n1 = c.bridges[i][0], n2 = c.bridges[i][1];
        int other = (n1 == node) ? n2 : (n2 == node) ? n1 : -1;
        if (other >= 1 && other <= 60) return other;
    }
    return -1;
}

void vadcEvaluate(void) {
    static bool inited = false;
    if (!inited) {
        for (int s = 0; s < VADC_COUNT; s++) { s_userOfSlot[s] = -1; vadcSlots[s].channel = -1; }
        inited = true;
    }
    // 1. Which slots are placed, and which row they read.
    for (int s = 0; s < VADC_COUNT; s++) {
        VadcSlot& v = vadcSlots[s];
        v.placed = slotNamedInBridges(s);
        v.rowNode = v.placed ? firstRowOnSlot(s) : -1;
        v.netIndex = 0;   // nets are rebuilt after this; vadcAfterRouting fills it
        if (!v.placed) {
            if (s_userOfSlot[s] >= 0) { infraReleaseAdc(kDedicatedUsers[s_userOfSlot[s]]); s_userOfSlot[s] = -1; }
            v.channel = -1; v.shared = false; v.routeFailed = false;
            v.volts = 0; v.sampleMs = 0; v.color = 0;
        }
    }
    // 2. Dedicated budget: at most 3, and never the last free ADC0-3 channel
    //    (the scan, shared slots and guide one-shots need one per tap).
    //    infraFreeAdcMask() excludes channels with a pool owner, so the
    //    channels our own dedicated slots still hold from the previous rebuild
    //    are NOT in freeNow - add them back. Hand trace:
    //      two slots dedicated, nothing else owns:  freeNow=2, already=2 -> 3
    //      two dedicated + fake-GPIO TDM owns one:   freeNow=1, already=2 -> 2
    //      nothing placed, four free:                freeNow=4, already=0 -> 3
    //      TDM owns one, nothing placed:             freeNow=3, already=0 -> 2
    int freeNow = __builtin_popcount(infraFreeAdcMask(0x0F) & 0x0F);
    int alreadyDedicated = 0;
    for (int s = 0; s < VADC_COUNT; s++) if (vadcSlots[s].placed && !vadcSlots[s].shared && vadcSlots[s].channel >= 0) alreadyDedicated++;
    int budget = alreadyDedicated + freeNow - 1;
    if (budget > 3) budget = 3;
    if (budget < 0) budget = 0;
    // 3. Priority order: measure mode's slot, then user slots ascending.
    int order[VADC_COUNT]; int n = 0;
    order[n++] = VADC_MEASURE_SLOT;
    for (int s = 0; s < VADC_COUNT; s++) if (userSlot(s)) order[n++] = s;
    int dedicated = 0;
    for (int i = 0; i < n; i++) {
        int s = order[i];
        VadcSlot& v = vadcSlots[s];
        if (!v.placed) continue;
        if (v.shared) continue;          // demoted earlier; promotion only on a bridge-set change (step 4)
        if (v.channel >= 0 && s_userOfSlot[s] >= 0) {
            // keep-if-viable: the pool's own rule under this slot's enumerator
            int ch = infraAcquireAdc(kDedicatedUsers[s_userOfSlot[s]], 1u << v.channel, false);
            if (ch == v.channel && dedicated < budget) { dedicated++; continue; }
            infraReleaseAdc(kDedicatedUsers[s_userOfSlot[s]]); s_userOfSlot[s] = -1; v.channel = -1;
        }
        if (dedicated >= budget) { v.shared = true; v.channel = -1; continue; }
        int u = -1;
        for (int k = 0; k < 3; k++) {
            bool taken = false;
            for (int t = 0; t < VADC_COUNT; t++) if (s_userOfSlot[t] == k) taken = true;
            if (!taken) { u = k; break; }
        }
        if (u < 0) { v.shared = true; v.channel = -1; continue; }
        int ch = infraAcquireAdc(kDedicatedUsers[u], 0x0F, false);
        if (ch < 0) { v.shared = true; v.channel = -1; continue; }
        s_userOfSlot[s] = u; v.channel = ch; v.shared = false; dedicated++;
    }
}
```

- [ ] **Step 2: Promotion only on a bridge-set change**

Add a fingerprint so a demoted slot is retried only when bridges change (never flaps every rebuild). At the top of `vadcEvaluate()` after `inited`:

```cpp
    static uint32_t lastBridgeHash = 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < globalState.connections.numBridges; i++) {
        h = (h ^ (uint32_t)globalState.connections.bridges[i][0]) * 16777619u;
        h = (h ^ (uint32_t)globalState.connections.bridges[i][1]) * 16777619u;
    }
    bool bridgesChanged = (h != lastBridgeHash);
    lastBridgeHash = h;
    if (bridgesChanged) {
        for (int s = 0; s < VADC_COUNT; s++) if (vadcSlots[s].shared && !vadcSlots[s].routeFailed) vadcSlots[s].shared = false;
        for (int s = 0; s < VADC_COUNT; s++) vadcSlots[s].routeFailed = false;
    }
```

(`routeFailed` is set by `vadcAfterRouting` in Task A4; a demoted slot stays shared until the bridge set changes.)

- [ ] **Step 3: Pool blindness fix**

`infraAdcUserClaimed()` scans `bridges[]` for `ADC0+ch`; once user bridges carry slot ids it goes blind. In `src/routing/InfraPaths.cpp` replace the body's loop with:

```cpp
    for (int i = 0; i < c.numBridges; i++) {
        int n1 = c.bridges[i][0], n2 = c.bridges[i][1];
        int e1 = IS_VADC(n1) ? vadcChannelOfNode(n1) : n1;
        int e2 = IS_VADC(n2) ? vadcChannelOfNode(n2) : n2;
        if (e1 != adcNode && e2 != adcNode) continue;
        if (IS_FAKE_GP_IN(n1) || IS_FAKE_GP_IN(n2)) continue;
        if (globalState.isEphemeralConnection(n1, n2)) continue;
        return true;
    }
    return false;
```

with `#include "sensing/VirtualAdc.h"` added. (A dedicated slot's channel is owned through the pool AND visible as claimed - both consumers already treat "claimed" as occupied.)

- [ ] **Step 4: Call it from the three rebuild heads**

In `src/Commands.cpp` after each of the three `infraEvaluate();` lines (241, 426, 632) add `vadcEvaluate();` and `#include "sensing/VirtualAdc.h"` at the top.

- [ ] **Step 5: Build both targets**

Run: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/sensing/VirtualAdc.cpp src/routing/InfraPaths.cpp src/Commands.cpp
git commit -m "Virtual ADC slots: allocator hands ADC0-3 to placed slots at the rebuild head"
```

### Task A4: Router expansion, SENSE paths, demotion

**Files:**
- Modify: `src/routing/MatrixState.h:51` (`enum pathType` gains `SENSE`)
- Modify: `src/routing/NetsToChipConnections.cpp`: `findStartAndEndChips` (case block near `:6055-6082`), `assignPathType` (`:6232-6255`), overlap `expandNode` (`:5484-5500`), every `pathType == VIRTUAL` skip (`grep -n "pathType == VIRTUAL" src/routing/NetsToChipConnections.cpp`), `computeKNeedingNets`, `pathFullyRouted`, the rescue-pass `anyUnrouted` scan
- Modify: `src/routing/RouteSafety.cpp:392-402` (`validateAllPaths` skip)
- Modify: `src/sensing/VirtualAdc.cpp` (`vadcAfterRouting`)
- Modify: `src/Commands.cpp` (the three sites right after `bridgesToPaths();`)

**Interfaces:**
- Consumes: `vadcChannelOfNode(node)` (-1 = shared or unplaced), `vadcSlotOfNode`.
- Produces: a dedicated slot's path is routed as today with its node expanded to `ADC0+channel`; a shared slot's path has `pathType == SENSE`, all chip/x/y `-1`, and is skipped by every stage; `vadcAfterRouting()` returns true when a dedicated slot was demoted (caller re-routes once).

- [ ] **Step 1: enum**

`src/routing/MatrixState.h:51`: append `SENSE` to `enum pathType` (after `VIRTUAL`).

- [ ] **Step 2: expansion in `findStartAndEndChips`**

In the `switch (bothNodes[twice])` add a case next to the FakeGPIO input case (same shape, `:6055-6082`):

```cpp
    case VADC_0 ... VADC_15: {
      int expanded = vadcChannelOfNode(bothNodes[twice]);
      if (expanded < 0) {
        // shared slot: no fabric of its own - the core-1 sampler taps its row
        globalState.connections.paths[pathIdx].pathType = SENSE;
        globalState.connections.paths[pathIdx].chip[0] = -1;
        globalState.connections.paths[pathIdx].chip[1] = -1;
        startEndChip[twice] = -1;
        break;
      }
      bothNodes[twice] = expanded;
      if (twice == 0) globalState.connections.paths[pathIdx].node1 = expanded;
      else            globalState.connections.paths[pathIdx].node2 = expanded;
    }
    // fall through: the expanded node is a chip-K special-function pin
```

Place it immediately BEFORE the `case GND ... 141:` block (dev: `:6087`, inside `switch (bothNodes[twice])` at `:5994`) so the fall-through lands in the special-function scan.

Verify point: `mergeOverlappingCandidates(i)` and `assignPathType(i)` run after `findStartAndEndChips` on the same path (`bridgesToPaths` prep loop, `:1660-1700`). With both `chip[]` set to -1 and `candidates[][]` untouched (-1), `mergeOverlappingCandidates` must be a no-op for a SENSE path - read it (`:5969`) and add `if (globalState.connections.paths[i].pathType == SENSE) return;` at its top if it writes anything. `assignPathType` gets the early return in Step 3.

- [ ] **Step 3: `assignPathType` and the overlap validator**

In `assignPathType` (`:6232` area), before the FakeGPIO branches:

```cpp
  if (globalState.connections.paths[pathIndex].pathType == SENSE) return;   // set by findStartAndEndChips
  if (IS_VADC(node1)) { int e = vadcChannelOfNode(node1); if (e >= 0) node1 = e; }
  if (IS_VADC(node2)) { int e = vadcChannelOfNode(node2); if (e >= 0) node2 = e; }
```

In the overlap `expandNode` lambda (`:5484`): add `if (IS_VADC(node)) { int e = vadcChannelOfNode(node); return (e >= 0) ? e : node; }` before the FakeGPIO checks.

- [ ] **Step 4: skip SENSE everywhere VIRTUAL is skipped**

Run `grep -n "pathType == VIRTUAL" src/routing/NetsToChipConnections.cpp src/routing/RouteSafety.cpp` and change every hit to `(… == VIRTUAL || … == SENSE)`. Also:
- `computeKNeedingNets`: `if (p.pathType == VIRTUAL || p.pathType == SENSE || p.duplicate != 0) continue;`
- the rescue-pass `anyUnrouted` scan in `bridgesToPaths`: skip `SENSE` too (otherwise every rebuild with a shared slot runs the rescue pass).
- `pathFullyRouted`: return `true` for `SENSE` paths.
- `fillUnusedPaths`: no duplicate for a SENSE path (`if (globalState.connections.paths[pathIdx].pathType == SENSE) continue;` next to the VIRTUAL check).
- `couldntFindPath`: the existing VIRTUAL `continue` gains `|| SENSE`.
- `rebuildShownReadings` (Peripherals.cpp) needs nothing: a SENSE path names no physical node.

- [ ] **Step 5: demotion**

`VirtualAdc.cpp`:

```cpp
bool vadcAfterRouting(void) {
    bool demoted = false;
    for (int s = 0; s < VADC_COUNT; s++) {
        VadcSlot& v = vadcSlots[s];
        if (!v.placed) continue;
        v.netIndex = (v.rowNode >= 1 && v.rowNode <= 60) ? nodeToNetIndex[v.rowNode] : 0;
        if (v.shared || v.channel < 0) continue;
        int adcNode = ADC0 + v.channel;
        for (int i = 0; i < numberOfPaths; i++) {
            pathStruct& p = globalState.connections.paths[i];
            if (p.duplicate != 0) continue;
            if (p.node1 != adcNode && p.node2 != adcNode) continue;
            if (p.x[0] < 0 || p.y[0] < 0 || p.x[1] < 0 || p.y[1] < 0) {
                v.shared = true; v.routeFailed = true;
                if (s_userOfSlot[s] >= 0) { infraReleaseAdc(kDedicatedUsers[s_userOfSlot[s]]); s_userOfSlot[s] = -1; }
                v.channel = -1;
                demoted = true;
            }
            break;
        }
    }
    return demoted;
}
```

(`numberOfPaths` and `pathStruct` come from `routing/NetsToChipConnections.h` / `MatrixState.h`; include them.)

- [ ] **Step 6: re-route once on demotion**

In `src/Commands.cpp` at each of the three `bridgesToPaths();` sites, immediately after:

```cpp
  if (vadcAfterRouting()) {
    // A dedicated slot could not be routed: it is now shared (SENSE) - run the
    // routing portion once more so its chip-K row goes back to whoever needs it.
    // The second pass cannot demote another slot: the remaining dedicated
    // slots route exactly as they did in pass one, and chip K now has one row
    // MORE to give, so the second vadcAfterRouting() below only fills netIndex.
    clearAllNTCC();
    loadBridgesFromState();
    getNodesToConnect();
    globalState.display.reconcileAfterRebuild();
    partsReassertNetNames(globalState);
    rebuildChangedNetColorsFromBridges();
    bridgesToPaths();
    vadcAfterRouting();   // fills netIndex for the final routing; cannot demote again
  }
```

For the two siblings (`refreshLocalConnections` ~`:426`, `fastRefresh` ~`:632`) copy the same block but keep only the calls that sibling already makes between `clearAllNTCC()` and `bridgesToPaths()` (read each one; `fastRefresh` skips the display reconcile).

- [ ] **Step 7: Build both targets**

Run: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og`
Expected: SUCCESS. (OG compiles `VirtualAdc.cpp` and the `SENSE` enum but its router never sets it.)

- [ ] **Step 8: HIL - four ADCs, three route, the fourth is shared and nothing shorts**

Append to `test/hil/test_virtual_adc.py`:

```python
# --- 3. four placed ADCs: three dedicated, one shared, no short --------------
out = jl_exec("""
import time, jumperless
nodes_clear(); time.sleep(0.1)
dac_set(DAC0, 2.0); connect(DAC0, 10)
for r in (10, 20, 30, 40):
    connect(r, 216)             # ADC_AUTO
time.sleep(0.3)
st = jumperless.vadc_status()   # list of dicts: slot, placed, shared, channel, row (Task A5)
ded = [d for d in st if d["placed"] and not d["shared"]]
sh  = [d for d in st if d["placed"] and d["shared"]]
print("dedicated=", len(ded))
print("shared=", len(sh))
print("v_slot0=%.2f" % adc_get(0))
nodes_clear()
""")
vals = parse_kv(out)
check(vals.get("dedicated") == 3, "three slots got dedicated channels")
check(vals.get("shared") == 1, "the fourth slot is shared, not failed")
check(abs(vals.get("v_slot0", -9) - 2.0) < 0.3, "slot 0 on row 10 reads DAC0's 2.0 V")
```

`jumperless.vadc_status()` and the slot-aware `adc_get` are added in Task A5; run this check after A5 (it is listed here so the demotion logic is exercised by the same file).

- [ ] **Step 9: Commit**

```bash
git add src/routing/MatrixState.h src/routing/NetsToChipConnections.cpp src/routing/RouteSafety.cpp src/sensing/VirtualAdc.cpp src/Commands.cpp
git commit -m "Virtual ADC slots: router expands dedicated slots, skips SENSE paths, demotes on no-route"
```

### Task A5: Readback, status, MicroPython, JSON

**Files:**
- Modify: `src/sensing/VirtualAdc.cpp` (`vadcRead`, `vadcPrintStatus`)
- Modify: `src/JumperlessMicroPythonAPI.cpp:382` (`jl_adc_get`), new `jl_vadc_status_json`
- Modify: `modules/jumperless/modjumperless.c` (bind `vadc_status`)
- Modify: `src/routing/JsonState.cpp:200-212` (nets JSON "ADC" special)
- Modify: `src/SingleCharCommands.cpp` (register `v` if free, else extend the existing ADC status command - check with `grep -n "registerCommand( 'v'" src/SingleCharCommands.cpp`)
- Modify: `src/tubes/Ser3Backchannel.cpp` (`:vadc` verb next to `:json:power`)

**Interfaces:**
- Produces: `float vadcRead(int slot, bool* fresh)`; `adc_get(n)` semantics: `7` → probe tip (unchanged), placed slot `n` → its reading, unplaced `0-4` → legacy physical read; `jumperless.vadc_status()` → list of dicts `{slot, placed, shared, channel, row, net, volts, age_ms}`; terminal/port-7 status text.

- [ ] **Step 1: `vadcRead`**

```cpp
float vadcRead(int slot, bool* fresh) {
    if (slot < 0 || slot >= VADC_COUNT || !vadcSlots[slot].placed) { if (fresh) *fresh = false; return 0.0f; }
    VadcSlot& v = vadcSlots[slot];
    if (!v.shared && v.channel >= 0) {
        v.volts = readAdcVoltage(v.channel, 16);
        v.sampleMs = millis();
        if (fresh) *fresh = true;
        return v.volts;
    }
    if (fresh) *fresh = (v.sampleMs != 0 && millis() - v.sampleMs < 1000);
    return v.volts;   // shared: the sampler's cache (Part B keeps it warm)
}
```

- [ ] **Step 2: `adc_get` keeps every old script working**

`src/JumperlessMicroPythonAPI.cpp:382`:

```cpp
float jl_adc_get( int channel ) {
    if ( channel == 7 ) return readAdcVoltage( 7, 16 );          // probe tip
    if ( channel >= 0 && channel < VADC_COUNT && vadcSlots[ channel ].placed ) {
        bool fresh; return vadcRead( channel, &fresh );
    }
    if ( channel >= 0 && channel <= 4 ) return readAdcVoltage( channel, 16 );   // bare channel, nothing placed
    return 0.0f;
}
```

- [ ] **Step 3: status**

```cpp
void vadcPrintStatus(Stream* out) {
    out->println("slot placed mode  chan row  net  volts   age");
    for (int s = 0; s < VADC_COUNT; s++) {
        VadcSlot& v = vadcSlots[s];
        if (!v.placed) continue;
        bool fresh; float volts = vadcRead(s, &fresh);
        out->printf("%-4d %-6s %-5s %-4d %-4d %-4d %6.3f %5lums\r\n", s, "yes",
                    v.shared ? "shared" : "dedic", v.channel, v.rowNode, v.netIndex,
                    volts, v.sampleMs ? (unsigned long)(millis() - v.sampleMs) : 0UL);
    }
}
```

MicroPython: add `const char* jl_vadc_status_json(void)` in JumperlessMicroPythonAPI.cpp that writes `[{"slot":0,"placed":true,"shared":false,"channel":1,"row":10,"net":6,"volts":2.01,"age_ms":3},…]` into a static 1024-byte buffer, and bind it in `modjumperless.c` as `vadc_status` returning `json.loads`-able text (follow `get_all_paths` at `:3213` for the parsing pattern, or return the string and parse in Python with `ujson`). Register the terminal command and the `:vadc` port-7 verb by copying the shape of `:json:power`.

- [ ] **Step 4: JSON nets**

`src/routing/JsonState.cpp:207`: add before the physical check:

```cpp
             if (IS_VADC(node)) {
                 specialType = "ADC";
                 gpioOrAdcIndex = VADC_SLOT(node);
                 bool fresh; voltage = vadcRead(gpioOrAdcIndex, &fresh);
                 isSpecial = true;
             } else
```

- [ ] **Step 5: Build, run the HIL file**

Run: `"$SCRATCH/pio313/bin/pio" run -e jumperless_v5 -e jumperless_og` then flash (touch on port 5 + `picotool load -x`), then `cd test/hil && python3 test_virtual_adc.py` and `python3 test_routing.py`.
Expected: both PASS. The shared slot's volts read 0 until Part B (that is expected; check 3 reads slot 0, which is dedicated).

- [ ] **Step 6: Commit**

```bash
git add src/sensing/VirtualAdc.cpp src/JumperlessMicroPythonAPI.cpp modules/jumperless/modjumperless.c src/routing/JsonState.cpp src/SingleCharCommands.cpp src/tubes/Ser3Backchannel.cpp
git commit -m "Virtual ADC slots: readback, adc_get(n) by slot, status on terminal, port 7 and MicroPython"
```

### Task A6: Display plumbing for dedicated slots (colour, highlight readout, LED map)

**Files:**
- Modify: `src/hardwarestuff/RoutableGpio.cpp:1150` (`anyAdcConnected` - return the SLOT for the net)
- Modify: `src/Graphics.cpp:2991-2995` and `:3155-3165` (colour from `vadcSlots[slot].color`)
- Modify: `src/Peripherals.cpp:1215-1290` (`showLEDmeasurements` writes `vadcSlots[s].color` for dedicated slots from the channel's reading)
- Modify: `src/eyecandy/Highlighting.cpp:1744`, `:2083` (label "ADC n")
- Modify: `src/Graphics.cpp:3748` (`if (node == ADC_PAD)` → also `IS_VADC(node)` lights the ADC pad LED)

- [ ] **Step 1: `anyAdcConnected(net)` returns the slot, not the channel**

```cpp
int anyAdcConnected( int net ) {
    if ( net == -1 ) {
        for ( int s = 0; s < VADC_COUNT; s++ ) if ( vadcSlots[ s ].placed && vadcSlots[ s ].netIndex > 0 ) return s;
        return -1;
    }
    return vadcSlotForNet( net );
}
```

Every consumer of the return value now gets a slot index (0-15): update the two Graphics sites to read `vadcSlots[slot].color` instead of `adcReadingColors[channel]` and the `< 8` guard to `< VADC_COUNT`; update the Highlighting readout to print `"ADC %d"` with the slot and its `vadcRead` volts.

- [ ] **Step 2: `showLEDmeasurements` fills slot colours**

At the end of the existing per-channel loop, add:

```cpp
    for ( int s = 0; s < VADC_COUNT; s++ ) {
        VadcSlot& v = vadcSlots[ s ];
        if ( !v.placed || v.netIndex <= 0 || v.netIndex > numberOfNets ) continue;
        float volts = v.shared ? v.volts : adcReadings[ v.channel ];
        uint32_t color = measurementToColor( volts, -8.0f, 8.0f );
        int brightness = LEDbrightnessSpecial + (int)fabsf( volts * 2.0f );
        if ( brightness < 4 ) brightness = 4; else if ( brightness > 100 ) brightness = 100;
        if ( jumperlessConfig.display.lines_wires == 0 || numberOfShownNets > MAX_NETS_FOR_WIRES ) {
            lightUpNet( v.netIndex, -1, 1, brightness, 0, 0, color );
        }
        v.color = scaleBrightness( color, map( (int)fabsf( volts ), 0, 8, -30, 70 ) );
    }
```

and stop the old per-channel loop from painting a net twice: skip a channel whose net already belongs to a placed slot (`if (vadcSlotForNet(showADCreadings[i]) >= 0) continue;`).

- [ ] **Step 3: Build, flash, eyes on the bench**

Expected: a row bridged to `ADC` (auto) colours by voltage exactly as an `ADC0` row did before; the highlight readout says `ADC 0 · 2.00 V`; the ADC logo pad lights.

- [ ] **Step 4: Commit**

```bash
git add src/hardwarestuff/RoutableGpio.cpp src/Graphics.cpp src/Peripherals.cpp src/eyecandy/Highlighting.cpp
git commit -m "Virtual ADC slots: net colouring, highlight readout and pad LED read the slot table"
```

### Task A7: MicroPython compatibility contract (no script changes, ever)

Kevin's rule (memory `micropython-api-backward-compat`): every spelling and constant the module accepts today keeps working and now means the slot.

**Files:**
- Modify: `modules/jumperless/modjumperless.c:393-405` (name table: `"ADC0"/"ADC_0"/"ADC0_8V"` … `"ADC3"/"ADC_3"/"ADC3_8V"`, `"ADC4"`), `:1983-1995` (`jl_adc_get_func` argument handling), `:6530-6535` (exported node objects)
- Modify: `src/eyecandy/OledGui.cpp:142-146` (the `{adc:N}` placeholder resolver: `if ( strcmp( base, "adc" ) == 0 ) { … adcReadings[argN] … }`) so `{adc:N}` reads slot N through `vadcRead`
- Modify: `scripts/jumperless.pyi:114-126` (docstring: "channel or slot 0-14; 7 = probe tip"), `src/snakes/micropythonExamples.h:269` (comment only)
- Test: `test/hil/test_virtual_adc.py`

**Interfaces:**
- Consumes: `vadcResolveForBridge` (A2, through `addConnection`), `jl_adc_get` (A5).
- Produces: the contract below, and it is what Part C's `ADC` constant extends.

Contract (all of these behave identically, and all land on a slot):

| Script writes | Today | After |
|---|---|---|
| `connect(5, ADC1)` (node object, value 111) | physical ADC1 | slot 1 (funnel remap 111 → 201) |
| `connect(5, "ADC_1")`, `connect(5, "ADC1")`, `connect(5, "ADC1_8V")` | 111 via `find_node_value` | slot 1 (same table entries, same remap) |
| `connect(5, 111)` | physical | slot 1 |
| `connect(5, ADC4)` / `"ADC4"` / `114` | physical ADC4 (0-5 V) | next free slot (`ADC_AUTO`), documented |
| `adc_get(1)` / `get_adc(1)` | channel 1 | slot 1 if placed, else bare channel 1 |
| `adc_get(7)` | probe tip | probe tip (unchanged) |
| `is_connected(5, ADC1)` | bridge with 111 | true when the bridge holds slot 1 (see step 2) |
| OLED `{adc:1}` | channel 1 volts | slot 1 volts |

- [ ] **Step 1: keep the node objects and name table values at 110-114**

Do NOT renumber `node_adc0_obj..node_adc4_obj` or the table entries: scripts compare and print them, and the funnel in `addConnection` (Task A2) already turns 110-114 into slots. Add, next to them, the slot spellings so `str()`/name lookups of the new ids work: `{ "ADC_5", 205 } … { "ADC_14", 214 }`, and `{ "ADC", 216 }` (the constant object for `ADC` is Task C4).

- [ ] **Step 2: `is_connected` sees through the remap**

In `jl_nodes_is_connected` (`src/JumperlessMicroPythonAPI.cpp:1431`) resolve both arguments with `vadcResolveForBridge` before `globalState.hasConnection`, so `is_connected(5, ADC1)` is true when row 5 is bridged to slot 1:

```cpp
int jl_nodes_is_connected( int node1, int node2 ) {
    int r1 = vadcResolveForBridge( node1, node2 );
    int r2 = vadcResolveForBridge( node2, node1 );
    if ( r1 >= 0 ) node1 = r1;
    if ( r2 >= 0 ) node2 = r2;
    return globalState.hasConnection( node1, node2 ) ? 1 : 0;
}
```

(`ADC_AUTO` as an argument here resolves to "the slot already on that row's net", which is the question the script is asking.)

- [ ] **Step 3: `adc_get` argument range**

`jl_adc_get_func` (`modjumperless.c:1983`): widen the check to `0..14` and keep 7 as the tip; the slot/channel decision is already in `jl_adc_get` (A5).

- [ ] **Step 4: OLED placeholder**

In `OledGui.cpp:146` replace `adcReadings[argN]` with a slot-aware read (declare `argN` bounds 0-14 where the parser already clamps it):

```cpp
        bool fresh;
        float v = ( argN == 7 ) ? adcReadings[ 7 ]
                : ( argN >= 0 && argN < VADC_COUNT && vadcSlots[ argN ].placed ) ? vadcRead( argN, &fresh )
                : ( argN >= 0 && argN < 8 ) ? adcReadings[ argN ] : 0.0f;
        snprintf( out, outSize, "%.2f", (double)v );
```

`vadcRead` for a dedicated slot calls `readAdcVoltage`, which is core-0 safe with the ring live; the OLED resolver runs on core 0.

- [ ] **Step 5: HIL - every old spelling still works**

Append to `test/hil/test_virtual_adc.py`:

```python
# --- 5. MicroPython compatibility: old spellings all land on the slot -------
out = jl_exec("""
import time
nodes_clear(); time.sleep(0.1)
dac_set(DAC0, 1.0); connect(DAC0, 5)
connect(5, ADC1)                      # node object, value 111
time.sleep(0.2)
print("obj_ok=", 1 if is_connected(5, ADC1) else 0)
print("str1_ok=", 1 if is_connected(5, "ADC_1") else 0)
print("str2_ok=", 1 if is_connected(5, "ADC1_8V") else 0)
print("int_ok=", 1 if is_connected(5, 111) else 0)
print("slot_ok=", 1 if is_connected(5, 201) else 0)
print("v1=%.2f" % adc_get(1))
print("v1b=%.2f" % get_adc(1))
nodes_clear()
""")
vals = parse_kv(out)
for k in ("obj_ok", "str1_ok", "str2_ok", "int_ok", "slot_ok"):
    check(vals.get(k) == 1, f"{k}: old spelling resolves to slot 1")
check(abs(vals.get("v1", -9) - 1.0) < 0.3, "adc_get(1) reads slot 1's row (DAC0 1.0 V)")
check(abs(vals.get("v1b", -9) - 1.0) < 0.3, "get_adc(1) alias unchanged")
```

Run: `cd test/hil && python3 test_virtual_adc.py`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add modules/jumperless/modjumperless.c src/JumperlessMicroPythonAPI.cpp src/eyecandy/OledGui.cpp scripts/jumperless.pyi src/snakes/micropythonExamples.h test/hil/test_virtual_adc.py
git commit -m "Virtual ADC slots: every MicroPython ADC spelling keeps working and means the slot"
```

**Part A bench gate (Kevin):** place four ADCs with the probe (the old 0-4 picker still appears; any choice works), confirm rows colour, `b` shows three routed ADC paths and one with no coordinates but no "Couldn't find a path", scope app and USB audio unchanged with one ADC placed, and an existing project script (`src/snakes/projectFiles.h` 555 timer: `TAPS = (("ADC0", OUT_ROW), ("ADC1", CAP_ROW))`) runs unmodified.

---

## Part B - Shared slots sampled through fresh dynamic routes

After Part B: the fourth (and fifth, …) ADC reads a live voltage at 200/N Hz through a route built per poll, invalidated automatically by any rebuild, never holding a chip-K row.

### Task B1: Publish the tap primitive with drift returned

**Files:**
- Modify: `src/sensing/NetVoltageScan.cpp:268-360` (`senseNodeVoltage` split), `src/sensing/NetVoltageScan.h`

**Interfaces:**
- Produces: `bool nvScanTapNodeNow(int node, int adc, float* volts, float* drift)` - the existing tap (lock, `fastConnectPath`, 80 µs settle, 500 µs ring dwell, `fastDisconnectPath`) returning the late window and `|late - early|` WITHOUT the 50 mV floating rejection. `senseNodeVoltage` becomes a thin wrapper that keeps the rejection.

- [ ] **Step 1: split**

Rename the body of `senseNodeVoltage` to `static bool senseNodeVoltageRaw(int node, int adc, float* early, float* late)` ending right before the drift verdict (`if (fabsf(late - early) > kFloatingDriftVolts)`), then:

```cpp
static bool senseNodeVoltage(int node, int adc, float* volts) {
    float early, late;
    if (!senseNodeVoltageRaw(node, adc, &early, &late)) return false;
    nodeDrift[node] = fabsf(late - early);
    if (fabsf(late - early) > kFloatingDriftVolts) { tapFailDrift++; return false; }
    *volts = late;
    return true;
}
bool nvScanTapNodeNow(int node, int adc, float* volts, float* drift) {
    float early, late;
    if (!senseNodeVoltageRaw(node, adc, &early, &late)) return false;
    *volts = late;
    *drift = fabsf(late - early);
    return true;
}
```

Declare `nvScanTapNodeNow` in `NetVoltageScan.h` with the comment "core 1 only; caller decides what drift means".

- [ ] **Step 2: Build both; run `test_net_currents.py`**

Expected: SUCCESS; `test_net_currents: PASS` (the scan's behaviour is unchanged).

- [ ] **Step 3: Commit**

```bash
git add src/sensing/NetVoltageScan.cpp src/sensing/NetVoltageScan.h
git commit -m "Net-voltage scan: publish the tap primitive with drift returned"
```

### Task B2: The shared-slot sampler on core 1

**Files:**
- Modify: `src/sensing/VirtualAdc.cpp` (`vadcServiceSharedTap`), `VirtualAdc.h`
- Modify: `src/sensing/NetVoltageScan.cpp:1386-1392` (call it right after `serviceOneShotTap()`), and the zero-offset export (`scanZeroOffset[]` becomes readable: `float nvScanZeroOffset(int adc)`)

**Interfaces:**
- Produces: `void vadcServiceSharedTap(void)` - one tap per pass, round-robin over shared placed slots, hardware gates only (`core1FramesHeld`, `core1req::allIdle`, `core1busy || refreshInProgress`, `usbAudioOwnsAdc`, `probeActive`, `wavegen.isRunning()`), shares `kScanIntervalUs` with the scan, alternates passes with the scan when both are due, acquires `INFRA_ADC_VADC_SHARED` per tap (mask `0x0F`, `allowSharedTdm = true`) and releases after.

- [ ] **Step 1: implement**

The gate symbols (`core1FramesHeld`, `core1req::allIdle`, `core1busy`, `refreshInProgress`, `usbAudioOwnsAdc`, `probeActive`, `wavegen`, `core2busy`) live in different headers; `NetVoltageScan.cpp` already compiles against every one of them, so copy its `#include` block verbatim into `VirtualAdc.cpp` (drop what the compiler reports unused afterwards) rather than hunting them one by one.

```cpp
// core 1, from serviceNetVoltageScan(). One shared slot per pass.
void vadcServiceSharedTap(void) {
    static int rr = 0;
    static bool ourTurn = false;
    int shared = 0;
    for (int s = 0; s < VADC_COUNT; s++) if (vadcSlotIsShared(s) && vadcSlots[s].rowNode > 0) shared++;
    if (shared == 0) return;
    if (core1FramesHeld()) return;
    if (!core1req::allIdle()) return;
    if (core1busy || refreshInProgress) return;
#if USB_AUDIO_ENABLE
    if (usbAudioOwnsAdc) return;
#endif
    if (probeActive != 0) return;
    if (wavegen.isRunning()) return;
    if (!nvScanTapDue()) return;            // kScanIntervalUs since the last tap of either kind
    ourTurn = !ourTurn;                     // alternate with the scan when both want the pass
    if (!ourTurn && nvScanWantsPass()) return;
    for (int tries = 0; tries < VADC_COUNT; tries++) {
        rr = (rr + 1) % VADC_COUNT;
        if (vadcSlotIsShared(rr) && vadcSlots[rr].rowNode > 0) break;
    }
    VadcSlot& v = vadcSlots[rr];
    int adc = infraAcquireAdc(INFRA_ADC_VADC_SHARED, 0x0F, true);
    if (adc < 0) return;
    core2busy = true;
    nvScanMarkTap();                        // lastScanUs = micros()
    float volts, drift;
    if (nvScanTapNodeNow(v.rowNode, adc, &volts, &drift)) {
        v.volts = volts - nvScanZeroOffset(adc);
        v.drift = drift;
        v.sampleMs = millis();
    }
    infraReleaseAdc(INFRA_ADC_VADC_SHARED);
    core2busy = false;
}
```

Add to NetVoltageScan.cpp (and declare in its header): `bool nvScanTapDue(void) { return micros() - lastScanUs >= kScanIntervalUs; }`, `void nvScanMarkTap(void) { lastScanUs = micros(); }`, `bool nvScanWantsPass(void) { return jumperlessConfig.measurement.net_currents && scanNodeCount > 0; }`, `float nvScanZeroOffset(int adc) { return scanZeroOffset[adc]; }`.

Call site, `serviceNetVoltageScan()` first lines:

```cpp
    serviceOneShotTap();
    vadcServiceSharedTap();   // shared virtual ADC slots: user-requested work, hardware gates only
```

- [ ] **Step 2: Build both, flash, HIL**

Append to `test/hil/test_virtual_adc.py`:

```python
# --- 4. the shared slot reads a live voltage through a per-poll route ------
out = jl_exec("""
import time, jumperless
nodes_clear(); time.sleep(0.1)
dac_set(DAC1, 1.5); connect(DAC1, 40)
for r in (10, 20, 30, 40):
    connect(r, 216)             # ADC_AUTO
time.sleep(1.0)
st = jumperless.vadc_status()
sh = [d for d in st if d["placed"] and d["shared"]]
print("shared_slot=", sh[0]["slot"] if sh else -1)
print("shared_row=", sh[0]["row"] if sh else -1)
print("shared_v=%.2f" % (sh[0]["volts"] if sh else -9))
print("shared_age=", sh[0]["age_ms"] if sh else 99999)
nodes_clear()
""")
vals = parse_kv(out)
check(vals.get("shared_row") == 40, "row 40 (the fourth) is the shared slot")
check(abs(vals.get("shared_v", -9) - 1.5) < 0.3, "shared slot reads DAC1's 1.5 V through a per-poll tap")
check(vals.get("shared_age", 99999) < 500, "shared sample is fresh (< 500 ms)")
```

Expected: PASS. Then `b` on port 7: chip K has at most three ADC rows; the shared slot's path shows type SENSE with no coordinates.

- [ ] **Step 3: Commit**

```bash
git add src/sensing/VirtualAdc.cpp src/sensing/VirtualAdc.h src/sensing/NetVoltageScan.cpp src/sensing/NetVoltageScan.h
git commit -m "Virtual ADC slots: shared slots sampled on core 1 through fresh per-poll routes"
```

### Task B3: Shared-slot colour and ageing

**Files:**
- Modify: `src/Peripherals.cpp` (`showLEDmeasurements` loop from A6 already uses `v.volts` for shared slots - add ageing: a sample older than 2 s dims to the "unknown" colour)
- Modify: `src/eyecandy/Highlighting.cpp` readout: shared slot prints `ADC n · 1.50 V (shared)` and `ADC n · --` when stale

- [ ] **Step 1: implement the two visual rules (code in the A6 loop: `if (v.shared && (v.sampleMs == 0 || millis() - v.sampleMs > 2000)) { color = 0x101010; }`).**
- [ ] **Step 2: Build, flash, eyes: a shared row colours and updates visibly (200/N Hz); pulling the DAC bridge dims it within 2 s.**
- [ ] **Step 3: Commit** `git commit -m "Virtual ADC slots: shared slots colour and age on the board and in the readout"`

**Part B bench gate (Kevin):** four ADCs placed, one on a DAC row: the fourth reads live; turn the DAC, the row's colour follows; rewire something unrelated - no stale reading, no phantom crosspoint (`b` clean).

---

## Part C - Placement surfaces stop asking for a channel

### Task C1: Probe ADC pad and logo-pad bindings

**Files:** `src/Probing.cpp:4187` (`function = chooseADC()` → `function = ADC_AUTO`), `:4464` (`adcChosen = ADC_AUTO; settingOption` line deleted), `:1740-1800` (`resolveLogoPadAssignment`: values 2-7 → `ADC_AUTO`; `nodeToLogoPadConfig`: `IS_VADC(node) || node == ADC_AUTO` → 2), `Probing.h:432` (keep `chooseADC` declared but unused; delete in a later sweep).

- [ ] Steps: edit, build both, bench: tap the ADC pad then a row - no picker, the row becomes `ADC n` with the lowest free n; tap another row - next slot; tap a row already on a slot's net - OLED hint "already ADC n", no new slot. Commit `"Probe ADC pad places an auto slot"`.

### Task C2: Clickwheel ADC zone

**Files:** `src/Probing.cpp:2035-2040`, `:2078-2082` (`subIndex/maxSubIndex` for `ZONE_ADC` → 0), `:2226-2240` (`adcMap`/labels → single `"ADC"` entry: `actualNode = ADC_AUTO`), `:2519-2521` (`selectedNode = ADC_AUTO`).

- [ ] Steps: edit, build, bench with the wheel: one "ADC" entry commits an auto slot. Commit `"Clickwheel ADC zone is one entry"`.

### Task C3: Menu Show → Voltage

**Files:** `src/remembering/menuTree.h:104` (`"--*0**1**2**3**4*"` → `"--*ADC*"`), the matching action in `src/Menus.cpp` (find with `grep -n "optionVoltage" src/Menus.cpp`; the chosen digit became `ADC0 + option` - replace with `ADC_AUTO`).

- [ ] Steps: edit, build, bench: Show → Voltage → ADC → row: slot placed. Commit `"Menu Show > Voltage places an auto slot"`.

### Task C4: The `ADC` auto constant and docs strings

**Files:** `modules/jumperless/modjumperless.c:6530` (add `node_adc_auto_obj` = 216 exported as `ADC`; the name-table entry exists from A7), `src/selfreflection/HelpDocs.cpp:270` (text: "Python access: adc_get(n) reads slot n; connect(row, ADC) auto-assigns"), `scripts/jumperless.pyi:1555` and `scripts/jumperless_module.py:377` (add `ADC`).

- [ ] Steps: edit, build, `test_virtual_adc.py` gains `connect(15, ADC)` using the new constant and checks `is_connected(15, ADC)`. Commit `"MicroPython: ADC constant is the auto slot"`.

### Task C5: Handoff doc

- [ ] Write `CodeDocs/VIRTUAL_ADC_SLOTS_HANDOFF_<date>.md`: what landed, the two rulings, bench evidence per part, what is deliberately left (ADC4 dropped as a user node, OG router untouched, `chooseADC()` dead code), and the hands-on checklist for Kevin.

---

## Self-review notes

- Spec coverage: vocabulary (A1), ADC_AUTO at the bridge site (A2), allocator with pool enumerators and the last-free-channel rule (A3), demotion + single re-route + no flapping (A3 step 2 / A4), SENSE path skipped by every stage (A4), readback and status (A5), display plumbing (A6, B3), the priority sampler above the politeness gates with hardware gates kept (B2), persistence through names (A1 name table + the addConnection remap of numeric ids), placement surfaces (C1-C4). Not covered on purpose: measure mode keeps using the pool directly with `INFRA_ADC_MEASURE` (slot 15 is reserved but unused until someone needs the name); route-plan caching (an optimisation, not needed for correctness).
- Type consistency: `vadcResolveForBridge(int,int)`, `vadcChannelOfNode(int)`, `vadcSlotForNet(int)`, `vadcRead(int,bool*)`, `vadcAfterRouting(void)`, `vadcServiceSharedTap(void)`, `nvScanTapNodeNow(int,int,float*,float*)` are used with those exact signatures throughout.
- Known risk to verify in A4 step 2: `findStartAndEndChips` is a `switch` on `bothNodes[twice]` - confirm the `case GND ... 141` label exists on dev before relying on fall-through; if the special-function scan is a separate `if`, call it explicitly after the write-back.
