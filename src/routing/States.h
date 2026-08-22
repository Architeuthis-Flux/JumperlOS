// SPDX-License-Identifier: MIT
#ifndef STATES_H
#define STATES_H

#include <Arduino.h>
#include <string.h>
#include <vector>
#include "JumperlessDefines.h"
#include "JumperlOS.h"
#include "MatrixState.h"
#include "LEDs.h"
#include "CH446Q.h"
#include "RotaryEncoder.h"   // NUM_SLOTS, SLOT_FILE_CONTEXT, extern netSlot
#include <YAMLDuino.h>


/*
This is the new target YAML format for the states system

everything is optional, it'll default to the config values

make sure it's easy to add new fields in the future

Example YAML format:
version: 2
sourceOfTruth: bridges  # or 'nets'

bridges:
  - {n1: 1, n2: 5, dup: 2}
  - {n1: 10, n2: 20, dup: 3}

nets:  # optional, for colors/names
  - {num: 6, name: "Signal A", color: 0xFF0000, nodes: [1, 5]}
  - {num: 7, name: "Signal B", nodes: [10, 20]}

power:
  topRail: 5.0
  bottomRail: 0.0
  dac0: 3.33
  dac1: 0.0

config:
  routing: {stackPaths: 2, stackRails: 3, stackDacs: 0, railPriority: 1}
  gpio:
    direction: [1,1,1,1,1,1,1,1,1,1]
    pulls: [0,0,0,0,0,0,0,0,0,0]
  uart: {txFunction: 0, rxFunction: 1}
  oled: {connected: false, lockConnection: false}

*/




// History buffer size (adjustable for undo/redo)
#ifndef STATE_HISTORY_SIZE
#define STATE_HISTORY_SIZE 0
#endif

// Parts layer capacity (guided placement / projects branch)
#if defined(OG_JUMPERLESS)
  #define MAX_PARTS 6
  #define MAX_PART_PINS 16
#else
  #define MAX_PARTS 16
  #define MAX_PART_PINS 24
#endif

// PartDefinition::placement - how a part's legs sit on the board (guide-UX
// design §2.1). EXPANDED is the default and the only value the pre-wave-2
// format could express, so it is the one the serializer omits.
#define PART_PLACEMENT_EXPANDED 0   // legs at the footprint rows + routed bridges
#define PART_PLACEMENT_COMPACT  1   // legs go straight into their connect: holes
#define PART_PLACEMENT_CUSTOM   2   // expanded-shaped, but the user moved row:

// Forward declarations
class JumperlessState;
class SlotManager;

// FakeGPIO restoration info (for loading from YAML)
struct FakeGpioRestorationInfo {
    int slot;
    int node;
    int mode;
    float v_high;
    float v_low;
    float threshold_high;
    float threshold_low;
    int high_voltage_node;  // For OUTPUT: TOP_RAIL, BOTTOM_RAIL, DAC0, DAC1, GND (-1 if not specified)
    int low_voltage_node;   // For OUTPUT: TOP_RAIL, BOTTOM_RAIL, DAC0, DAC1, GND (-1 if not specified)
};

// Source of truth for state reconciliation
enum SourceOfTruth {
    BRIDGES_PRIMARY,  // Bridges define connections, nets computed from bridges
    NETS_PRIMARY      // Nets define connections, bridges generated from nets
};

/**
 * @brief Ephemeral connection tracking
 * 
 * Ephemeral connections are temporary connections that:
 * - Are added to the bridges array for routing
 * - Are NEVER saved to YAML files  
 * - Do NOT trigger markDirty() (won't cause slot to save)
 * - Are tracked separately for cleanup
 * 
 * Use cases: Measure mode ADC connections, temporary probe connections
 */
struct EphemeralConnection {
    int node1;
    int node2;
    int bridgeIndex;  // Index in bridges array, -1 if not yet added
    
    EphemeralConnection() : node1(-1), node2(-1), bridgeIndex(-1) {}
    EphemeralConnection(int n1, int n2, int idx = -1) : node1(n1), node2(n2), bridgeIndex(idx) {}
};

/**
 * @brief Stores the connection topology: bridges, nets, and routing paths
 * This is the core of what makes a "slot" unique
 * 
 * WARNING: Contains large arrays (several KB). Avoid copying.
 */
struct ConnectionState {
    // STORED: Essential data (saved to YAML)
    int16_t bridges[MAX_BRIDGES][3];  // [bridge_index][node1, node2, duplicates]
    int16_t numBridges;
    
    netStruct nets[MAX_NETS];  // User-defined nets (optional, for colors/names/reference)
    int16_t numNets;  // Derived count
    
    // Bridge color hints (NOT saved to YAML directly, but used to color nets)
    // Indexed by bridge index, stores RGB color
    // Use 0xFFFFFFFF (all bits set) as sentinel for "no color specified"
    // This allows black (0x000000) to be a valid color
    uint32_t bridgeColors[MAX_BRIDGES];
    
    // COMPUTED: Runtime caches (NOT saved to YAML, computed from bridges/nets)
    pathStruct paths[MAX_BRIDGES];
    int16_t numPaths;
    bool pathsCacheValid;  // Set to false when connections change
    
    chipStatus chipStates[12];  // Derived from paths
    struct justXY chipXY[12];   // Crossbar switch states (reconstructed from paths)
    bool chipStatesCacheValid;
    
    ConnectionState();
    void clear();
    void invalidateCache(bool autoRefresh = true);  // Optional auto-refresh on invalidate
    void recomputePaths();  // Calls bridgesToPaths() to rebuild paths from current state
    void syncBridgesFromNets();  // Generate bridges from net node lists
    void syncNetsFromBridges();  // Generate nets from bridges
    
    // Note: Copying this struct copies all arrays (~several KB). Avoid when possible.
};

/**
 * @brief Power supply voltages for all DACs and rails
 */
struct PowerState {
    float topRail;      // Top breadboard rail voltage
    float bottomRail;   // Bottom breadboard rail voltage
    float dac0;         // DAC 0 voltage
    float dac1;         // DAC 1 voltage
    
    PowerState();
    void setDefaults();
    bool validate(String& errorMsg) const;
};

/**
 * @brief Display-related state (colors, brightness, etc.)
 */
struct DisplayState {
    // Net colors - only stored if manually changed from default
    // Tracks firstNode so we can reconcile after net rebuilds
    struct NetColorEntry {
        int netNumber;
        int firstNode;   // First node of net when color was set - for reconciliation
        rgbColor color;
        uint32_t rawColor;
        char colorName[32];
    };
    
    // Custom net names - tracked with firstNode so names follow nets during rebuilds
    struct NetNameEntry {
        int netNumber;
        int firstNode;   // First node of net when name was set - for reconciliation
        char name[32];
    };
    
    NetColorEntry customColors[MAX_NETS];
    int numCustomColors;
    
    NetNameEntry customNames[MAX_NETS];
    int numCustomNames;
    
    DisplayState();
    void clear();
    bool hasCustomColors() const { return numCustomColors > 0; }
    void setNetColor(int netNum, rgbColor color, uint32_t raw, const char* name);
    bool getNetColor(int netNum, rgbColor& color, uint32_t& raw, char* name) const;
    void removeNetColor(int netNum);
    
    // Custom net name functions
    void setNetName(int netNum, const char* name);
    const char* getNetName(int netNum) const;  // Returns nullptr if not set
    void removeNetName(int netNum);
    
    // Reconcile entries after nets are rebuilt - uses firstNode to find correct net numbers
    void reconcileAfterRebuild();
};

/**
 * @brief Configuration options that affect routing and behavior
 */
struct ConfigState {
    // State reconciliation mode
    SourceOfTruth sourceOfTruth;  // Which representation is authoritative
    
    // Routing preferences
    int stackPaths;     // How many paths to stack (0-3)
    int stackRails;     // How many rail connections to stack (0-3)
    int stackDacs;      // How many DAC connections to stack (0-3)
    int railPriority;   // Priority for rail connections
    
    // GPIO configuration (10 GPIOs: GP1-GP8, UART_TX, UART_RX)
    int gpioDirection[10];      // 0 = output, 1 = input
    int gpioPulls[10];          // 0 = pull down, 1 = pull up, 2 = none, 3 = bus keeper
    float gpioPwmFrequency[10]; // PWM frequency in Hz
    float gpioPwmDutyCycle[10]; // PWM duty cycle (0.0 to 1.0)
    bool gpioPwmEnabled[10];    // PWM enabled flag
    uint8_t gpioReadFloating[10]; // 1 = detect floating (tri-state), 0 = skip floating read
    volatile bool gpioPythonOwned[10];   // VOLATILE: True if MicroPython has exclusive control
                                         // Must be volatile since readGPIO() runs on Core 2 
                                         // and claiming happens on Core 1 (MicroPython)
    
    // UART configuration
    int uartTxFunction;  // 0 = tx, 1 = rx, 2 = gpio_in, 3 = gpio_out
    int uartRxFunction;  // 0 = tx, 1 = rx, 2 = gpio_in, 3 = gpio_out
    
    // OLED state
    bool oledConnected;
    bool oledLockConnection;
    
    // Auto-refresh behavior
    bool autoRefreshOnChange;  // Automatically call refreshConnections() when cache is invalidated
    
    ConfigState();
    void setDefaults();
};

// ============================================================================
// Parts layer (guided placement / projects branch)
// ============================================================================
// Slot-YAML `parts:` section (design: CodeDocs/DESIGN_GUIDED_PLACEMENT.md
// §1-§2/§6; serializer/parser live in routing/PartPlacement.cpp):
//
//   parts:
//     - name: "U1"          # required
//       type: ic            # optional: resistor|capacitor|diode|led|bjt|fet|ic
//       value: "NE555"      # optional
//       part_id: ""         # optional part-ID hook (future /partdb reference)
//       footprint: dip8     # dipN | sipN | axial2 (N = PHYSICAL pin count;
//                           # axial2 is always exactly 2 pins)
//       row: 35             # breadboard row of pin 1 - DIP requires 31-60
//                           # (bottom half only, pin 1's dot/notch), axial2
//                           # requires 1-30 (top half; pin 2 = row+30), SIP
//                           # is legal on either half
//       placed: false       # runtime flag; always serialized
//       placement: compact  # expanded (default, OMITTED) | compact | custom
//       pins:
//         GND:  {pin: 1, connect: GND, class: gnd}
//         TRIG: {pin: 2, connect: 7, class: signal}
//         A:    {offset: 0, connect: 45}
//
// PLACEMENT MODES (guide-UX design §2, PartPlacement.cpp partPinNode()):
// `expanded` (0, the default) puts every leg at its footprint row and lets
// expandOnePart() route a bridge from there to the pin's `connect:` node.
// `compact` (1) is how you would breadboard it by hand - a leg whose
// `connect:` is a PHYSICAL HOLE ROW (1-60, TOP_RAIL, BOTTOM_RAIL) goes
// straight into that hole and emits NO bridge, because the leg itself is the
// connection. Per-pin, not all-or-nothing: a leg whose endpoint is
// fabric-only (GND=100 has no holes; DAC/ADC/GPIO likewise), `class: nc`, or
// absent keeps its footprint row and its bridge. ICs never compact (their
// legs ARE the footprint) - a hand-written `placement: compact` on a DIP is
// normalized back to expanded on parse, with a warning. `custom` (2) is
// expanded geometry from a row: the user moved by hand; the marker exists so
// a future re-provision does not snap it back. Unknown values parse as
// expanded with a warning. Emitted only when non-default (the verify/color
// precedent), so pre-wave-2 files are byte-identical after a rewrite - and
// note the converse: an OLDER firmware opening a new file DROPS `placement:`
// silently (unknown keys are skipped by design), so the part comes back
// expanded.
//
// PIN FORMS PARSED: both the nested block form above (one `NAME: {...}` per
// line under `pins:`) and the inline one-line form
// `pins: {A: {pin: 1, connect: 7}, B: {pin: 2}}`. Part entries themselves
// parse in block form only (`- name: ...`). `pin:` is the 1-based physical
// pin number placed by the DIP/SIP footprint math; `offset:` places the pin
// at baseRow+offset on the same side and WINS over `pin:` when >= 0.
// `offset:` IS EXPANDED-MODE GEOMETRY ONLY - it names a footprint row, so a
// compact-eligible pin ignores it (the leg goes to `connect:` instead) and it
// is what a non-eligible pin falls back to. It also predates `axial2`, which
// makes most historical uses obsolete: the old way to straddle the ravine was
// a sip2 with `{offset: 0}` / `{offset: 30}` (which does not even parse -
// offsets may not cross the ravine), so a 2-leg part should now declare
// `footprint: axial2` and plain `pin: 1` / `pin: 2`. Offsets are still the
// right tool for a leg that sits at a fixed distance from pin 1 on a strip
// with gaps. An offset that lands off-board (or on the far half) now REJECTS
// THE WHOLE PART, exactly like an oversized DIP/SIP span - see partGeometryOk
// in PartPlacement.cpp; before wave 2 such a part was accepted and then
// placed partially, which is the same silent-partial-loss class.
// `connect:` is node-only - a row number 1-60 or any node name resolvable by
// parseNodeName(), the exact helper `bridges:` parsing uses. `class:` is
// signal|power|gnd|nc (default signal). Unknown keys inside a part entry are
// skipped without error; malformed part entries are skipped with a warning,
// like bridges. A pin name may not begin with '-' or '#': pin names are
// emitted UNQUOTED as `      <NAME>: {...}`, so "- X" would reload as a
// parts-LIST entry (every following pin misattributed to a phantom part) and
// "#X" as a comment. parsePinEntry refuses it on every path (YAML and
// place_part alike, with a parse warning); jl_place_part carries a matched
// second copy so the two predicates cannot drift.
// (Section-keyword names like `overlays` used to be dangerous too, because
// the overlays pass strstr-scanned the whole file; it now matches only an
// UN-INDENTED `overlays:` line and stops at the next un-indented header, so
// a part/pin/value carrying the word is harmless.)
//
// guideProgress round-trips as ONE top-level flow-map line - the only shape
// parsed and the only shape emitted (keep serializer and parser matched):
//   guideProgress: {source: "/projects/555/wiring.yaml", step: 3}
// Emitted only while guideSource is non-empty. `meta:` and `guide:` sections
// are swallowed on parse and NOT round-tripped - the launcher and the guide
// runtime re-read the project file themselves.
//
// `guide:` FORMAT NOTES (parsed by guiding/GuidedFlow.cpp, documented in
// CodeDocs/DESIGN_GUIDED_PLACEMENT.md §2/§5 - listed here because this is
// the file the section shares):
//   - a rail_sane step with an explicit `target:` is checked as VCC-class,
//     i.e. "within 0.2 V of the top rail". The format cannot say which class
//     a single named row is, so one band has to be the default and VCC is
//     the useful one; GND-class rows are what the classless form finds by
//     itself from the parts' own `class: gnd` pins. For an explicit "this
//     row should be at 0 V" check, use `check: voltage` with
//     min: -0.15 / max: 0.15.
//   - CONTINUITY `min:`/`max:` ARE OHMS (wave 2). They used to be milliamps,
//     because the check compared a raw INA current against an authored
//     current band; it now measures RESISTANCE four-wire (the sense legs ride
//     the stimulus chain, the shunt sits on the ground side) and divides, so
//     an authored bound names ohms. `value:` + the optional per-part `tol:`
//     (percent) derive the band on their own - tol_author (default 15) +
//     tol_meas (5 / 10 / 25 by decade) - and explicit min/max still WIN when
//     they are given. Two guards catch a file still carrying the old numbers:
//     a band that does not BRACKET the parsed `value:`, or (for a value-less
//     part) a `max:` under 5 ohms, logs "min/max look like legacy mA" and
//     falls back to the derived band. VF/VOLTAGE/OSCILLATES min/max are
//     unchanged (volts / volts / Hz).
//   - ONE STEP IS ONE `- {...}` FLOW MAP. Block style (`- id: x` on one line,
//     `  do: place` on the next) is NOT parsed: each field line falls through
//     to the guide-level key arm, which ends the steps: list, so the step
//     AFTER it is dropped. That used to happen in silence - two authored
//     steps parsing as one - and now prints "guide step spans lines -
//     unsupported, next step may be lost". A flow map that merely WRAPS
//     (`- {do: place, part: U1,` / `   text: "..."}`) is fine: guideParse
//     joins continuation lines until the braces balance, bounded at 4 lines /
//     512 chars so an unclosed brace cannot swallow the file. `text:` must
//     still be the LAST field of the joined line.
//   - a `do: connect` step's `n1:`/`n2:` are LITERAL rows and do NOT
//     re-derive when a part moves. A part's OWN wiring does re-derive for
//     free (bridges are never stored per part - expandOnePart recomputes
//     partPinNode against `pin.connect` every time it applies), so route
//     part wiring through the parts' `pin.connect` fields wherever you can
//     and keep `connect:` steps for board-to-board jumpers. When a move
//     vacates a row a later `connect:` step names, the guide prints
//     `(note: step k targets row NN, which <part> just left)` - it cannot
//     fix the step, only tell you the file has gone stale.
//
// fromYAML INDENT-HARDENING: top-level section headers are recognized on
// UN-indented lines only (the serializer never indents them), so a nested
// key like `config:` inside a contained section can't hijack the section
// state. Exception: `fakeGpio:` is also recognized while inside `config:` -
// the serializer nests it there ("  fakeGpio:"). An unrecognized un-indented
// `something:` header opens an ignored/contained section instead of
// corrupting the one before it.
//
// ROUND-TRIP IS LOAD-BEARING: toYAML is a wholesale rewrite; any section the
// serializer doesn't emit is silently destroyed by the SlotManager idle
// auto-save. serializeParts() must re-emit every field deserializeParts()
// accepts.

struct PartPin {
    char    name[12];
    int8_t  pinNumber;   // 1-based physical pin; -1 = positioned by offset
    int8_t  offset;      // same-side offset from baseRow; -1 = use pinNumber + footprint
    int16_t connect;     // target node; -1 = none (leg occupies the hole, no bridge)
    uint8_t pinClass;    // 0=signal 1=power 2=gnd 3=nc
};

struct PartDefinition {
    char     name[16];
    char     typeStr[12];      // part-ID hook: resistor|capacitor|diode|led|bjt|fet|ic
    char     partId[16];       // part-ID hook: future /partdb reference
    int16_t  baseRow;          // breadboard row of pin 1 (1-60)
    uint8_t  footprint;        // 0=SIP 1=DIP 2=axial2
    uint8_t  pinCount;         // PHYSICAL pin count from the footprint (dip8 -> 8)
    uint8_t  numPins;          // entries used in pins[] (only listed pins are stored;
                               // pinCount is the footprint's N for the geometry math)
    uint8_t  defaultVerify;    // GuideCheck (raw uint8 until the guide runtime lands)
    uint32_t outlineColor;     // 0 = per-class defaults
    char     value[12];        // "10k" etc.
    uint8_t  tol;              // `tol:` percent for the derived continuity
                               // band (invest-measurement.md §2.2). 0 = unset
                               // -> the 15 % default; serialized only when set
    bool     placed;           // runtime: expansion applied (guide progress)
    uint8_t  placement;        // PART_PLACEMENT_* - serialized only when non-default
    PartPin  pins[MAX_PART_PINS];
    // Geometry for 1-based PHYSICAL pin k, `row:` = pin 1's ACTUAL hole
    // (bench verdict, wave 2 - photo-confirmed real chips sit dot/notch at
    // bottom-left; this flipped the original top-anchored mapping, which was
    // mirrored):
    //   SIP  -> baseRow+(k-1), legal on either half.
    //   DIP  -> baseRow MUST be on the bottom half (31-60) - a top-anchored
    //           baseRow is no longer a valid DIP anchor at all (returns -1
    //           for every pin, not just the far side). k<=N/2: baseRow+(k-1)
    //           (bottom, left->right); k>N/2: (baseRow-30)+(N-k) (top,
    //           right->left). Example: dip8 at row 35 -> pins 1-4 =
    //           35,36,37,38; pins 5-8 = 8,7,6,5.
    //   axial2 -> baseRow MUST be on the top half (1-30); pin 1 = baseRow,
    //           pin 2 = baseRow+30 (same column, straddling the ravine -
    //           the default footprint for 2-leg parts like resistors/diodes,
    //           convention only, not enforced).
    // Returns -1 when the pin would leave the board or the anchor is on the
    // wrong half. Per-pin `offset` overrides are applied by PartPlacement.cpp
    // (offset wins when >= 0). Full geometry + the wave-2 bench story:
    // CodeDocs/DESIGN_GUIDED_PLACEMENT.md.
    //
    // This is FOOTPRINT geometry only. The node a leg actually occupies is
    // partPinNode(p, pin) (PartPlacement.h) - the single exported geometry
    // authority, which consults `placement` first and falls through to here.
    // Every consumer (bridge expansion, removal, LED overlays, check row
    // resolution, net naming, list_parts) goes through partPinNode so compact
    // rows propagate by construction; nothing may re-derive geometry itself.
    int nodeForPin(int k) const;
};

struct PartsState {                 // member of JumperlessState
    PartDefinition parts[MAX_PARTS];
    int8_t numParts;
    // guideProgress scalars (round-tripped in slot YAML; step TEXT is always
    // re-read from guideSource by the guide runtime, never stored here)
    char    guideSource[96];
    int16_t guideStep;
    void clear();
    int findByName(const char* n) const;   // -1 when absent
};

/**
 * @brief Complete Jumperless state for a single slot
 * This is the main state container that gets saved/loaded
 *
 * WARNING: This class contains MASSIVE arrays (tens of KB) and must NEVER be copied!
 * Always use references (&) or pointers (*) when passing this object around.
 * Copy constructor and copy assignment operator are deleted to enforce this.
 */
class JumperlessState {
public:
    ConnectionState connections;
    PowerState power;
    DisplayState display;
    ConfigState config;
    PartsState parts;      // guided-placement parts table (+ guideProgress scalars)

    // Where this state's circuit came from, when it is a per-run project file.
    // Round-trips as ONE un-indented top-level flow-scalar line, emitted only
    // while non-empty:
    //     runSource: "/projects/555/wiring.yaml"
    // The launcher writes it when it allocates a run file; nothing else reads
    // it yet. It lives here rather than in PartsState because it describes the
    // whole context, not the parts table.
    //
    // ROUND-TRIP IS LOAD-BEARING (same rule as parts/guideProgress): toYAML is
    // a wholesale rewrite, so a scalar that is PARSED but not EMITTED is
    // destroyed by the very next SlotManager idle auto-save. Serializer and
    // parser must land in the same commit, always.
    char runSource[96];

    // Metadata
    int version;  // State format version for future compatibility
    
    JumperlessState();
    
    // CRITICAL: This object is HUGE (~33-50KB). Copying it exhausts the RP2040's
    // tiny stack/heap (the OG has only ~50KB TOTAL free RAM) and is wasteful on
    // V5. Copying is therefore DELETED - the compiler will reject any copy. Work
    // in place on globalState / a JumperlessState& reference, or serialize via
    // toYAML/fromYAML to move state between slots without a struct copy.
    //
    // SAFE:   JumperlessState& state = globalState;           (reference, no copy)
    // SAFE:   void func(JumperlessState& state);              (pass by reference)
    // COMPILE ERROR: JumperlessState state = globalState;     (copy - deleted)
    // COMPILE ERROR: void func(JumperlessState state);        (pass by value - deleted)
    JumperlessState(const JumperlessState&) = delete;
    JumperlessState& operator=(const JumperlessState&) = delete;
    
    // Connection management
    bool addConnection(int node1, int node2, String& errorMsg, int duplicates = -1);  // -1 = use default from config
    bool removeConnection(int node1, int node2, String& errorMsg);
    bool hasConnection(int node1, int node2) const;
    int getConnectionDuplicates(int node1, int node2) const;  // Get number of parallel copies
    bool setConnectionDuplicates(int node1, int node2, int duplicates, String& errorMsg);
    void clearAllConnections();
    
    // Ephemeral connection management (temporary connections that are NEVER saved)
    // Use for measure mode, temporary probing, etc.
    // Parameters:
    //   node1, node2: The nodes to connect
    //   errorMsg: Output error message on failure
    //   applyRouting: If true, immediately route and send to hardware (calls refreshLocalConnections)
    //   ledShowOption: LED display option (0=none, 1=show, -1=default). Only used if applyRouting=true
    //   color: Optional bridge color (0xFFFFFFFF = use default). Useful for visual feedback during measurements
    bool addEphemeralConnection(int node1, int node2, String& errorMsg, 
                                bool applyRouting = false, int ledShowOption = 0, 
                                uint32_t color = 0xFFFFFFFF);
    bool removeEphemeralConnection(int node1, int node2, String& errorMsg,
                                   bool applyRouting = false, int ledShowOption = -1);
    void clearAllEphemeralConnections(bool applyRouting = false, int ledShowOption = -1);
    bool isEphemeralConnection(int node1, int node2) const;
    int getEphemeralConnectionCount() const;
    // InfraPaths' raw bridge removal shifts the bridge array under the
    // ephemeral side-list; this applies the same bridgeIndex fixup that
    // removeEphemeralConnection does internally.
    void adjustEphemeralIndicesAfterBridgeRemoval(int removedBridgeIdx);
    
    // Power management
    void setDacVoltage(int dacNum, float voltage);
    float getDacVoltage(int dacNum) const;
    void setRailVoltage(bool isTopRail, float voltage);
    float getRailVoltage(bool isTopRail) const;
    
    // Configuration
    void setPathStacking(int paths, int rails, int dacs);
    void getPathStacking(int& paths, int& rails, int& dacs) const;
    void setAutoRefresh(bool enabled) { config.autoRefreshOnChange = enabled; }
    bool getAutoRefresh() const { return config.autoRefreshOnChange; }
    void setSourceOfTruth(SourceOfTruth source) { config.sourceOfTruth = source; markDirty(); }
    SourceOfTruth getSourceOfTruth() const { return config.sourceOfTruth; }
    
    // GPIO
    void setGpioDirection(int gpio, int direction);
    int getGpioDirection(int gpio) const;
    void setGpioPull(int gpio, int pull);
    int getGpioPull(int gpio) const;
    void setGpioPwmFrequency(int gpio, float frequency);
    float getGpioPwmFrequency(int gpio) const;
    void setGpioPwmDutyCycle(int gpio, float dutyCycle);
    float getGpioPwmDutyCycle(int gpio) const;
    void setGpioPwmEnabled(int gpio, bool enabled);
    bool getGpioPwmEnabled(int gpio) const;
    
    // UART
    void setUartTxFunction(int function);
    int getUartTxFunction() const;
    void setUartRxFunction(int function);
    int getUartRxFunction() const;
    
    // Display
    void setNetColor(int netNum, rgbColor color, uint32_t raw, const char* name);
    bool getNetColor(int netNum, rgbColor& color, uint32_t& raw, char* name) const;
    
    // Dirty flag management (for lazy writes)
    void markDirty();
    bool isDirty() const { return dirty; }
    void clearDirty();
    unsigned long getLastModifiedTime() const { return lastModifiedTime; }
    
    // Validation
    bool validate(String& errorMsg) const;
    
    // Serialization (YAML format)
    // showANSI: 0=plain, 1=colored hex, 2=colored blocks only (for terminal preview)
    bool toYAML(String& output, int showANSI = 0) const;
    bool fromYAML(const String& input, String& errorMsg);
    
    // Legacy format support
    bool fromLegacyNodeFile(const String& nodeFileContent, String& errorMsg);
    
    // State comparison
    bool operator==(const JumperlessState& other) const;
    bool operator!=(const JumperlessState& other) const { return !(*this == other); }
    
    // Utility
    void clear();
    size_t estimateRAMUsage() const;
    
private:
    // Dirty flag infrastructure
    bool dirty;
    unsigned long lastModifiedTime;
    
    // Ephemeral connections tracking (never saved to YAML)
    static constexpr int MAX_EPHEMERAL_CONNECTIONS = 8;
    EphemeralConnection ephemeralConnections[MAX_EPHEMERAL_CONNECTIONS];
    int numEphemeralConnections;
    
    // Helper for validation
    bool isNodeValid(int node) const;
    bool isConnectionAllowed(int node1, int node2, String& errorMsg) const;
    
    // YAML serialization helpers
    void serializeBridges(String& output) const;
    bool deserializeBridges(const char* yamlContent, String& errorMsg);
    void serializeNets(String& output) const;
    bool deserializeNets(const char* yamlContent, String& errorMsg);
    void serializePower(String& output) const;
    bool deserializePower(const char* yamlContent, String& errorMsg);
    void serializeConfig(String& output) const;
    bool deserializeConfig(const char* yamlContent, String& errorMsg);
    void serializeFakeGpio(String& output) const;
    bool deserializeFakeGpio(const char* yamlContent, String& errorMsg);
};

// Global list of pending FakeGPIO restorations (populated during YAML load, applied after bridges are restored)
extern std::vector<FakeGpioRestorationInfo> pendingFakeGpioRestorations;

/**
 * @brief Manages slot files and active state
 * Singleton pattern - use SlotManager::getInstance()
 */
class SlotManager : public Service {
public:
    // Get singleton instance
    static SlotManager& getInstance();
    
    // Prevent copying
    SlotManager(const SlotManager&) = delete;
    SlotManager& operator=(const SlotManager&) = delete;
    
    // Service interface
    ServiceStatus service() override;
    const char* getName() const override { return "States"; }
    ServicePriority getPriority() const override { return ServicePriority::HIGH; }
    // Not an existing gate: the auto-save is idle-gated (systemIdleForFlush,
    // >= 750 ms quiet) and the editor/preview transitions are edge checks, so
    // 50 ms is a new, consequence-free upper bound on how often the dirty /
    // editor / preview state is looked at.
    uint32_t periodUs() const override { return 50000; }

    // Active state access
    JumperlessState& getActiveState();
    const JumperlessState& getActiveState() const;
    int getActiveSlot() const { return activeSlotNumber; }

    // ---- Path-based active context (design-slots.md §0) --------------------
    // The active context's identity is ALWAYS a path. Numbered slots 0-7 (and
    // Python 99) carry their canonical /slots/slotN.yaml here AND keep their
    // number; anything else sets activeSlotNumber == netSlot ==
    // SLOT_FILE_CONTEXT and this path is the only identity.
    /** Full path of the active context. Never null; "" only before the first load. */
    const char* getActiveSlotPath() const { return activeSlotPath; }
    /** True when the active context is a file, not a numbered slot. */
    bool isPathContext() const { return activeSlotNumber == SLOT_FILE_CONTEXT; }
    /** Display name for the active context, <= 7 chars (OLED row / LED matrix). */
    String activeContextLabel7() const;
    /**
     * Strict canonical-slot-file matcher. Returns N only for an EXACT
     * "/slots/slot<N>.yaml" (N in 0..NUM_SLOTS-1, digits only, round-trip
     * checked) or 99 for "/slots/slotPython.yaml"; -1 for everything else.
     * Deliberately full-path, not basename: a user's /projects/foo/slot3.yaml
     * is a file context, not slot 3. This retires the "slot_555.yaml".toInt()
     * == 0 trap that let a project file adopt slot 0 and get clobbered.
     */
    static int slotNumberForCanonicalPath(const String& path);
    /**
     * True for a shipped project TEMPLATE: /projects/<dir>/wiring*.yaml.
     * Such a path is READ-ONLY as an active context - saveActiveSlot refuses
     * to write it and it can never become the boot context. Per-run project
     * files are <dir>_<N>.yaml and deliberately do not match.
     */
    static bool isTemplatePath(const char* path);

    // Slot management
    bool loadSlot(int slotNum, String& errorMsg);
    /**
     * Load slot state from an arbitrary YAML file path and ADOPT it as the
     * active context: activeSlotNumber/netSlot become
     * slotNumberForCanonicalPath(path), falling back to SLOT_FILE_CONTEXT,
     * and activeSlotPath becomes `path`.
     *
     * ATOMIC ON FAILURE (contract the launcher's exit table leans on):
     * tracking is adopted ONLY on a successful open AND parse. Callers may
     * treat a false return as "nothing happened to the context".
     *
     *   - Open failure: nothing happened; every tracker untouched.
     *   - Parse/validate failure: fromYAML has already replaced globalState
     *     by the time it returns false, so the prior context is RE-LOADED
     *     from its own file. Trackers end where they started.
     *     (fromYAML CAN return false: it ends in `return validate()`, and
     *     PowerState::validate rejects any rail/DAC outside +/-8 V.)
     *   - EXCEPTION, double failure: if that restoring re-load also fails,
     *     the manager enters the NO-ACTIVE-CONTEXT state - activeSlotNumber
     *     == netSlot == -1, empty path, cleared state, board cleared to match
     *     (nothing routed, nothing powered beyond defaults) - where nothing
     *     can be auto-saved. For a FILE context this is DETERMINISTIC when
     *     the caller passed the active path itself (USBfs's MSC-eject reload
     *     does), because the restore re-reads the same bad file; numbered
     *     contexts get loadSlot's /.bak mirror rescue, arbitrary paths do
     *     not. See the full note on the definition.
     */
    bool loadSlotFromPath(const String& path, String& errorMsg);
    bool saveSlot(int slotNum, String& errorMsg, bool skipValidation = false);  // skipValidation for faster auto-saves
    /** Path-aware save of the active context. THE dispatch point (design §3). */
    bool saveActiveSlot(String& errorMsg, bool skipValidation = false);
    /** Serialize the active state to an arbitrary path (generalized writeSlotFile). */
    bool writeStateToPath(const char* path, String& errorMsg, bool skipValidation = false);
    bool slotExists(int slotNum) const;
    bool deleteSlot(int slotNum, String& errorMsg);
    void clearActiveSlot();
    /**
     * Remember the active context as the boot context (/slots/last_active.txt).
     * Self-gating: a no-op while temp-slot or preview mode is active, and for
     * slot 99 / temp slot 8 - a calibration app or an isolated MicroPython
     * session must never become what the board boots into.
     */
    void updateLastActive();
    /**
     * Point tracking at a path WITHOUT loading it - the boot seed's one use.
     * main.cpp's loadfile: does the actual load on the first pass, so there is
     * exactly one load path.
     */
    void adoptBootPath(const String& path);
    
    // Preview mode - loads slot into globalState without applying to hardware
    // Just tracks which slot we should return to when done
    bool enterPreviewMode(int slotToPreview, String& errorMsg);
    bool isPreviewMode() const { return previewModeActive; }
    int getPreviewedSlotNumber() const { return previewSlotNumber; }
    int getOriginalSlotNumber() const { return originalSlotNumber; }
    void clearPreviewMode();  // Clear preview flag without loading anything
    bool exitPreview(bool applyPreview, String& errorMsg);  // Exit preview, optionally applying or reverting
    
    // Slot tracking synchronization
    void setActiveSlot(int slotNum);  // Sets activeSlotNumber and syncs with global netSlot
    void syncFromGlobalNetSlot();     // Updates activeSlotNumber from netSlot (for external changes)
    
    // Temporary slot mode - for apps that need a working slot and want to restore when done
    // This is different from preview mode: temp mode clears the slot and applies to hardware
    bool enterTemporarySlot(int tempSlot, bool saveCurrentFirst = true);  // Enter temp slot, returns false on error
    bool exitTemporarySlot(bool refreshHardware = true);                   // Return to original slot
    bool isTemporarySlotMode() const { return temporarySlotActive; }
    int getTemporarySlotOriginal() const { return temporarySlotOriginal; }
    
    // Create slot files on-demand (not pre-created)
    bool ensureSlotExists(int slotNum);
    
    // History management (undo/redo)
    void pushHistory();
    bool canUndo() const;
    bool canRedo() const;
    bool undo(String& errorMsg);
    bool redo(String& errorMsg);
    void clearHistory();
    int getHistorySize() const { return STATE_HISTORY_SIZE; }
    int getHistoryDepth() const;  // How many undo steps are available
    
    // Legacy support
    bool migrateOldSlotFile(int slotNum, String& errorMsg);
    
    // Utility
    void printSlotInfo(int slotNum);
    void listSlots();
    size_t getActiveStateRAMUsage() const;
    
    // File I/O helpers (public for direct slot file manipulation)
    bool readSlotFile(int slotNum, String& content, String& errorMsg);
    bool writeSlotFile(int slotNum, const String& content, String& errorMsg);
    
private:
    SlotManager();
    ~SlotManager() = default;
    
    // State storage - reference to globalState (no duplication!)
    JumperlessState& activeState;
    int activeSlotNumber;
    // Path identity of the active context. Fixed array, NOT String: the
    // auto-save path runs from service() on every idle flush and must not
    // churn the heap. INVARIANT: every `activeSlotNumber = ...` in this class
    // is paired with an activeSlotPath assignment - a number/path disagreement
    // is the silent-clobber vector this design exists to close.
    char activeSlotPath[128];

    // Preview mode state
    bool previewModeActive;
    int previewSlotNumber;       // Which slot we're previewing
    int originalSlotNumber;      // Which slot to return to when done
    char previewOriginalPath[128];  // ...and its path (originalSlotNumber may be -2)
    float originalRailVoltages[2]; // Save rail voltages (topRail, bottomRail) during preview

    // Temporary slot mode state (for apps using a working slot)
    bool temporarySlotActive;
    int temporarySlotOriginal;   // Which slot to return to when exiting temp mode
    char temporarySlotOriginalPath[128];  // ...and its path (may be a run file)

    // Depth-1 re-entrancy guard for loadSlotFromPath's parse-failure restore.
    // The restore re-loads the PRIOR context through the public loaders (so it
    // inherits parts expansion, fakeGpio, refresh and the power re-assert
    // instead of duplicating them); this flag is what stops a restore whose
    // own file is also bad from re-entering the restore logic forever.
    bool restoringContext;

    // Internal: write the active state to activeSlotPath (saveActiveSlot's
    // SLOT_FILE_CONTEXT branch).
    bool saveStateToActivePath(String& errorMsg, bool skipValidation);
    // Internal: set activeSlotPath from a slot number's canonical filename.
    void setActivePathFromSlot(int slotNum);

    // History buffer (circular buffer for undo/redo)
    JumperlessState* historyBuffer;
    int historySize;
    int historyHead;       // Points to next write position
    int historyCount;      // Number of valid history entries
    int historyPosition;   // Current position in history (for redo)
    
    // File name helpers
    String getSlotFilename(int slotNum) const;  // Returns .yaml filename
    String getLegacySlotFilename(int slotNum) const;  // Returns old .txt filename
    String getJSONSlotFilename(int slotNum) const;  // Returns old .json filename
    
    // History helpers
    void initHistory();
    void cleanupHistory();
    int historyIndex(int offset) const;  // Helper for circular buffer indexing
};

// Global singleton declaration - THE single source of truth for all Jumperless state
// This replaces the old global arrays (net[], path[], ch[]) with a unified state object
extern JumperlessState globalState;

// Global helpers for easy access
namespace StateHelpers {
    // Quick access to common operations
    inline JumperlessState& getState() { return SlotManager::getInstance().getActiveState(); }
    inline bool addConnection(int n1, int n2, int dup = -1) { String err; return getState().addConnection(n1, n2, err, dup); }
    inline bool removeConnection(int n1, int n2) { String err; return getState().removeConnection(n1, n2, err); }
    inline void setDac(int dac, float voltage) { getState().setDacVoltage(dac, voltage); }
    inline void setRail(bool top, float voltage) { getState().setRailVoltage(top, voltage); }
    inline int getDuplicates(int n1, int n2) { return getState().getConnectionDuplicates(n1, n2); }
    inline bool setDuplicates(int n1, int n2, int dup) { String err; return getState().setConnectionDuplicates(n1, n2, dup, err); }
    
    // Undo/redo shortcuts
    inline bool undo() { String err; return SlotManager::getInstance().undo(err); }
    inline bool redo() { String err; return SlotManager::getInstance().redo(err); }
    
    // Slot operations
    inline bool saveSlot(int slot) { String err; return SlotManager::getInstance().saveSlot(slot, err); }
    inline bool loadSlot(int slot) { String err; return SlotManager::getInstance().loadSlot(slot, err); }
}

// ============================================================================
// Custom Net Name Functions  
// ============================================================================
// Custom names are stored in DisplayState by NET NUMBER (not array index)
// This means names persist correctly when nets are added/deleted/reordered

void setCustomNetName(int netNum, const char* name);  // Set custom name (nullptr/empty to clear)
bool hasCustomNetName(int netNum);                     // Check if net has custom name
void clearAllCustomNetNames(void);                     // Reset all names to defaults

// ============================================================================
// Hardware Application Function
// ============================================================================

// Apply globalState settings to hardware (DACs, GPIO, etc.). `skipPower`
// applies the GPIO half ONLY - see slotLoadDeferPowerApply below.
void applyStateToHardware(bool skipPower = false);

// Apply ONLY the active state's power (rails + both DACs) to hardware. The
// completion half of a deferred load: whoever set slotLoadDeferPowerApply owes
// the context either this call or a deliberate substitute.
void applyStatePowerToHardware(void);

// GUIDED-LAUNCH TRANSIENT CLOSE (guide-UX design §4, task 7). Set it TRUE
// immediately before a loadSlotFromPath() whose power must not reach the rails
// yet - the guided launcher's run-file load, where the project's `power:`
// would otherwise energize the board for tens of ms before the guide's INIT
// forces 0 V. loadSlotFromPath LATCHES AND CLEARS it on entry, so:
//   - it is consumed by exactly one load and can never leak to the next one;
//   - the parse-failure restore recursion and the terminal-state clear inside
//     that same call still apply power normally (they re-enter with the flag
//     already false), which is what keeps "nothing powered beyond defaults"
//     and "a restored context asserts its power" true;
//   - a retry loop must re-arm it before EACH attempt.
// Every other context switch - boot, Files clicks, slot cycling, non-guided
// project runs - never touches it and keeps task 4's apply-power-on-load
// guarantee unchanged.
extern bool slotLoadDeferPowerApply;

// Decide what context the board boots into (config [slots].boot_mode +
// /slots/last_active.txt). Call once after configLoaded and BEFORE the first
// `loadfile:` pass - it only seeds netSlot / activeSlotPath; loadfile: loads.
void seedBootContext(void);
extern const char* LAST_ACTIVE_PATH;   // "/slots/last_active.txt"

// ============================================================================
// State Backup/Restore Functions (for MicroPython entry/exit, undo, etc.)
// Uses compressed YAML format - only stores actual connections, not empty array slots
// ============================================================================

void storeStateBackup(void);                    // Store compressed YAML snapshot of globalState
void restoreStateBackup(bool autoSave = false); // Restore from backup (optionally save to slot)
void restoreAndSaveStateBackup(void);           // Restore and immediately save to current slot
void clearStateBackup(void);                    // Clear the backup
bool hasStateBackup(void);                      // Check if backup exists
bool hasStateChanges(void);                     // Compare current state with backup
size_t getStateBackupSize(void);                // Get backup size in bytes (for diagnostics)
void printStateBackupInfo(void);                // Print detailed memory usage statistics

#endif // STATES_H

