# Fast Single-Path Send — Handoff Notes

**Goal for the next session:** replace the raw `sendXYraw()` crosspoint taps used by
`NetVoltageScan` (and eventually the other raw-XY users) with a proper routing-layer
function that can connect/disconnect ONE validated path quickly — no full
`sendAllPathsCore2` refresh — while guaranteeing it can never short two nets together.

## Why raw taps exist at all

`src/NetVoltageScan.cpp` measures the voltage of every node in every routed net by
momentarily tapping a free ADC (chip K x8–11) onto the node, ~200 taps/second. Per-path
current then falls out of Ohm's law on the crosspoint resistance
(`calibration.crosspoint_resistance`, ~40 ohms/crosspoint), displayed as marching ants
and probe-highlight mA readouts. A full refresh per tap is orders of magnitude too slow
(`sendPaths()` takes tens of ms), so taps are built ad-hoc from `lastChipXY` occupancy +
`chipStates[].xMap/yMap` fabric maps and sent with raw `sendXYraw()` calls
(`buildSenseRoute()` / `senseNodeVoltage()`, `src/NetVoltageScan.cpp` ~180–420).
`FakeGpio`'s `TimeDomainMultiplexer` does the same trick with hops extracted from routed
paths.

## Issues found with the raw approach

### 1. Bookkeeping vs hardware divergence (the core problem)

The CH446Q chips are **write-only** — `lastChipXY[12]` is the only record of crosspoint
state, and it can silently diverge from the hardware:

- `sendXYraw()` (`src/CH446Q.cpp` ~985–1076) updates `lastChipXY` BEFORE the word is
  queued to the PIO, then waits on the `chipSelect` handshake with a 1-second timeout.
  The timeout recovery resets the PIO state machine mid-stream — the strobe may or may
  not have physically latched. After that, bookkeeping lies.
- Once bookkeeping lies, every "is this lane free?" check in the scanner (and TDM) can
  approve a route that physically shorts a user's net into another net or source.
- There is no way to read back or verify actual crosspoint state; the only recovery is a
  full clear + resend.

### 2. Reproducible reading anomaly on multi-hop sense routes (unsolved)

With two independent zero-load nets routed ({TOP_RAIL@5V, row 15} and {DAC1@2V, row 45},
nothing plugged into the board):

- TOP_RAIL reads 4.99 V through its 1-hop chip-K route; row 15 reads **4.81 V** through
  a 3-hop route `C(x9) -> L(x14) -> K(x12) -> ADC, bounce on K y5` — same net, zero
  current, nodes must be equal.
- DAC1 reads 2.00 V; row 45 reads **2.15 V** via `G(x1) -> L(x14) -> K(x12)`, same K y5
  bounce. That's a ±4 mA phantom current on the ants.
- The error pulls readings toward ~3.4 V with ~10% coupling, is rock-stable within a tap
  (no drift between reads 250 us apart), and reproduces across reroutes and reboots.
- The same route shape bouncing on **K y4** (chip E stub) once read clean, and a
  properly-routed bridged ADC read the same node correctly at the same time. GND taps
  (1-hop, K x15) always read clean.

Suspects, none proven: a phantom-closed crosspoint invisible to `lastChipXY` (see #1)
somewhere on the `L x14 / K x12 / K-y-stub` chain; or real leakage/loading on chip L,
whose x-pins are live circuits (x4–x11 are hard-wired to RP2350 GPIOs 20–27, x2 is the
buffer, x3 is ADC4). Either way: the new implementation must make route state
verifiable/re-assertable, and lane selection should be able to avoid or de-prioritize
suspect lanes.

### 3. No real short protection

`sendXYraw()`'s only guard is the chip-K voltage-source auto-disconnect
(`CHIP_K_VOLTAGE_SOURCES` = x4/x5/x6/x7/x15 on a shared y — see
`CodeDocs/CHIP_K_VOLTAGE_SAFETY.md`). Nothing checks:

- lane ownership by another net (`chipStates[].xStatus/yStatus` — the router's
  authoritative per-net records — are never consulted by raw callers; the scanner only
  reads the `lastChipXY` bitmaps, which carry no net identity),
- `doNotIntersect` rules (`connectionAllowed()`),
- shorts formed transitively across multi-hop lane chains on chips other than K,
- sources that live outside chip K: GND on L x15, RP GPIOs on L x4–x11, ADC4 on L x3,
  the routable buffer on K x2 / L x2.

### 4. Multiple uncoordinated writers

`sendPaths()` (full refresh), `NetVoltageScan` taps, `TimeDomainMultiplexer`
(`FakeGpio`), `Probing`, `Apps`, and `Debugs` all call `sendXYraw()` directly.
Serialization is purely by convention (everything happens to run on core 2's loop);
nothing enforces it, and the routing engine has no idea any temporary raw state exists —
a clear-first refresh landing between a tap's connect and disconnect would strand it
(currently avoided only because taps are atomic within one core-2 pass).

### 5. The router's own knowledge is bypassed

The routing engine already has everything needed to do this safely — per-net lane
ownership (`xStatus`/`yStatus`), Y-position budgeting (`yPositionLimits/Usage`,
`src/NetsToChipConnections.cpp` ~830–877), conflict-checked coordinate assignment
(`setPathX`/`setPathY`, ~760–820), and per-path crosspoint sending (`sendPath(i)`,
`src/CH446Q.cpp` ~955–982). But the only entry point that exercises it is the full
`bridgesToPaths()` pipeline (~1541+) with its global sort / fake-GPIO net merge /
duplicate generation — far too heavy per tap.

## What the new function should be

A routing-layer API, roughly:

```cpp
// Allocate + validate + physically close a single ephemeral path from nodeA to
// nodeB (nodeB typically an ADC). Returns a handle or fills a pathStruct.
// Fails cleanly if no conflict-free lanes exist RIGHT NOW.
int fastConnectPath(int nodeA, int nodeB, pathStruct* out /*, flags */);
void fastDisconnectPath(pathStruct* p);
```

Requirements:

- **Allocate through the router's state**, not raw bitmaps: respect
  `xStatus`/`yStatus` net ownership, `doNotIntersect`/`connectionAllowed()`, chip-K
  source rules, and treat L x4–x11 / L x15 / K x15 / K x2 / L x2 / L x3 as sources too.
  Refuse any lane owned by a different net; refuse connecting two distinct nets.
- **Register the ephemeral path in the bookkeeping** (`xStatus`/`yStatus` +
  `lastChipXY`, maybe a `pathStruct` slot flagged ephemeral) so refreshes, the scanner,
  TDM, and Probing all see it — and unregister on disconnect. Decide explicitly what a
  concurrent full refresh does with ephemeral paths (preserve or clear + notify owner).
- **Send only that path's crosspoints** (the `sendPath(i)` pattern), target well under
  ~100 us for a 4-crosspoint path.
- **Re-assert / verify option** against divergence (#1/#2): a mode that re-sends the
  "open" command for every crosspoint bookkeeping believes is open on the lanes being
  used, before trusting them (~20 us per crosspoint; only needed after timeout
  recoveries or on a generation-counter mismatch with `sendPaths`).
- **Single-writer discipline**: assert/require core 2 context, or add an explicit
  ownership token shared with `sendPaths`.

Callers to migrate once it exists:

1. `NetVoltageScan` — `buildSenseRoute()`/`senseNodeVoltage()` collapse into
   `fastConnectPath(node, ADCn)`; all the lane-picking fallback tiers (direct K lane,
   via L/I/J, neighbor-chip bounce with `connectionNamesX` pairing) move into the
   allocator where they belong.
2. `TimeDomainMultiplexer` / `FakeGpio` channel switching.
3. `Probing`'s buffer-power GPIO/DAC swaps and other raw pokes in `Apps`/`Debugs`.

## Current NetVoltageScan state (working, shipped behind config)

- `display.net_currents` (default on), `i` command toggles / `i!` one-shot report,
  `debug.net_voltage_scan` prints nodes+drift, tap counters, sense routes, occupancy,
  and per-path currents once a second.
- Validated on hardware: series-consistent currents within ~10% on a real
  DAC->ISENSE->row->GND loop; floating nets rejected (0.05 V early/late drift check +
  20 mV deadband read solid 0.00 mA); auto-zero via GND tap only while GND is in no net;
  rails/DACs scanned differentially with setpoint fallback (`fillSourceFallback`);
  ADC lock taken before any crosspoint closes and never stolen (stealing corrupts the
  ADC state machine and wedges both cores — already happened once); the rotary encoder
  is serviced inside every scanner wait so the clickwheel stays responsive.
- **Known limitation = the motivating bug:** the §2 anomaly makes some multi-hop routes
  read ~10% of (3.4 V − V_node) off, which fakes ±4 mA on zero-load rail/DAC nets.
  Root-causing/eliminating it via verified, validated routes is the point of the new
  function.

## Quick file map

| What | Where |
|---|---|
| Raw send + chip-K source guard + PIO timeout recovery | `src/CH446Q.cpp` ~985–1076 |
| Per-path send loop (model for fast send) | `src/CH446Q.cpp` ~955–982 |
| Scanner route builder + tap + service loop | `src/NetVoltageScan.cpp` |
| Router lane ownership / conflict helpers | `src/NetsToChipConnections.cpp` ~760–950 |
| Heavy full pipeline (what we're avoiding per-tap) | `src/NetsToChipConnections.cpp` `bridgesToPaths()` ~1541+ |
| TDM full-path switching (existing raw consumer) | `src/TimeDomainMultiplexer.cpp` |
| Fabric maps (xMap/yMap, lane names for paired double-lanes) | `src/MatrixState.cpp` `chipStatusInit`, `connectionNamesX` |
