# Guided Placement Subsystem — Design (2026-08-20)

Companion to the approved plan (projects + guided placement branch). Sibling docs:
DESIGN_PROJECTS_SUBSYSTEM.md, DESIGN_PART_ID_FOLLOWUP.md. Where this doc and the
approved plan disagree, the plan wins (it post-dates and reconciles this design —
notably: `connect:` is node-only, part-ID hook fields are flat `type:`/`value:`/
`part_id:`, and the starter catalog is 4 projects).

## 0. Summary of decisions

| Question | Decision |
|---|---|
| Language | **Hybrid-declarative**: `parts:` (TODO-component-placement.md format + extensions) auto-generates placement steps; optional `guide:` adds ordering, free text, power-on, verify overrides |
| Runtime shape | **Blocking app loop** pumping `jOS.serviceInner()`, internally `GuideSession` struct + `guideTick()` enum-state machine — exactly probeMode's T3.1 M1 shape (Probing.cpp:2643), liftable to a Service later |
| LED channel | **GraphicOverlays** (rendered last over live nets, main.cpp ~1809) in `requestLedShow(1)` netlist mode; never mode 3 (would evict live nets); no `b.print()` breadboard text in v1 (mode-2 menu buffer conflict) |
| Guide persistence | **Direct-into-slot** (destination slot chosen up front — Kevin's decision): half-built project persists; `guideProgress:` scalars round-trip in slot YAML; step text always re-read from the project file |
| Net naming | Named **as each step commits**, but authoritative source is the parts table, **re-asserted after every rebuild** (fixes name loss on net merges) |
| Verification | Per-step-type matrix (§5); honest about ICs (unpowered = presence-hint only); one-shot tap **request API serviced on core 2** (`senseNodeVoltage`/`pairSenseTap` are `static` and core-2-affine — NetVoltageScan.h:23) |

## 1. The language

### 1.1 `parts:` base layer (implements CodeDocs/TODO-component-placement.md)

Parsed by a dedicated section scanner in the style of `deserializeOverlaysFromYAML`
(GraphicOverlays.cpp:410 — `strstr` the section, then line-walk; the main `fromYAML`
loop only routes flow-map lines, so multi-line list items need their own pass, which
overlays already prove out). See the plan's Format spec for the final field set
(`type:`/`value:`/`part_id:` hooks, `footprint:`, `pin:`/`offset:`, node-only
`connect:`, `class:`).

- **DIP mapping rule** (verified geometry: WokwiParser.cpp:120-190 — top rows = nodes
  1-30, bottom = 31-60, node n+30 = same column below the ravine): for `dipN` with
  pin 1 at `row` on the top half, pin k ≤ N/2 → `row + (k-1)`; pin k > N/2 →
  `row + 30 + (N-k)`. DIP-8 at row 5 → pins 1-4 = nodes 5,6,7,8; pins 5-8 = nodes
  38,37,36,35. `row > 30` mirrors. Nano-header nodes are legal `connect:` targets but
  not legal `row` bases in v1.
- `connect:` absent → the leg occupies the hole but no bridge.
- Expansion to bridges via `JumperlessState::addConnection` (idempotent —
  `hasConnection` States.h:273 skips exact duplicates).
- Net names: `{NAME}_{PIN}` uppercased/sanitized, ≤31 chars (`NetNameEntry::name[32]`,
  States.h:172).

### 1.2 `guide:` section

One flow-map line per step, same tolerant style as `bridges:` — **`text:` is parsed by
quote-pair (like `name:` in `deserializeNets`, States.cpp:1854-1861) and must be the
last field** (field extraction splits on bare commas).

**Step types**: `note`, `place` (commits that part's bridges+names), `connect`
(explicit bridge; `probe_confirm: true` waits for a probe tap on `n1`/`n2` via
`justReadProbe()` polling — **not** by nesting `probeMode(1,x)`, whose exit gestures
(Probing.cpp:3130-3147) would shadow the guide's controls), `power_on` (applies the
`power:` section; rails/DACs forced to 0V from guide start until this step), `verify`
(standalone check on `target` node or part pin), `run` (MicroPython script — escape
hatch).

**Per-step fields**: `do`, `part`, `n1/n2`, `target`, `check`
(`none|presence|continuity|vf|voltage|oscillates|i2c|rail_sane`), `min/max` (mA for
continuity, V for vf/voltage, Hz for oscillates), `on_fail` (`warn` [default: show ✗,
allow manual advance] | `retry` | `skip` | `block` [advance only via explicit skip
gesture]), `timeout_ms` (default 1500), `probe_confirm`, `text` (last).

### 1.3 Why hybrid holds up

`parts:` alone yields: footprint lighting, prompt, bridges, net names, default check
by part class. What it cannot express — and what `steps:` exists for: ordering across
parts vs interleaved notes, human context ("long leg", "the dot"), power-on placement
in sequence, verify-only steps, probe_confirm pedagogy, scripts. Authors with trivial
projects write zero steps (`auto: true`).

## 2. Data model

**`src/routing/States.h`** — add:

```cpp
#if defined(OG_JUMPERLESS)
  #define MAX_PARTS 6
  #define MAX_PART_PINS 16
#else
  #define MAX_PARTS 16
  #define MAX_PART_PINS 24
#endif

struct PartPin {
    char    name[12];
    int8_t  pinNumber;   // 1-based physical pin; -1 = positioned by offset
    int8_t  offset;      // same-side offset; -1 = use pinNumber + footprint
    int16_t connect;     // target node; -1 = none
    uint8_t pinClass;    // 0=signal 1=power 2=gnd 3=nc
};

struct PartDefinition {
    char     name[16];
    char     typeStr[12];      // part-ID hook: resistor|capacitor|diode|led|bjt|fet|ic
    char     partId[16];       // part-ID hook: future /partdb reference
    int16_t  baseRow;
    uint8_t  footprint;        // 0=SIP 1=DIP
    uint8_t  pinCount;
    uint8_t  defaultVerify;    // GuideCheck
    uint32_t outlineColor;     // 0 = per-class defaults
    char     value[12];        // "10k" etc.
    bool     placed;           // runtime: expansion applied (guide progress)
    PartPin  pins[MAX_PART_PINS];
    int nodeForPin(int k) const;   // the DIP/SIP math from §1.1
};

struct PartsState {                 // new member of JumperlessState
    PartDefinition parts[MAX_PARTS];
    int8_t numParts;
    void clear();
    int findByName(const char* n) const;
};
```

(~8 KB on V5, ~2 KB on OG; the copy-deletion at States.h:267 protects us.)

**New `src/routing/PartPlacement.h/.cpp`** (called from States.cpp):

```cpp
void serializeParts(const JumperlessState& st, String& out);               // from toYAML
bool deserializeParts(JumperlessState& st, const char* yaml, String& err); // own section scan
int  expandPartsToBridges(JumperlessState& st, String& err);               // load: placed==true parts
int  applyPartPlacement(JumperlessState& st, int partIdx, String& err);    // one part (guide commit)
int  removePartPlacement(JumperlessState& st, int partIdx, String& err);   // back/undo
void partsReassertNetNames(JumperlessState& st);                           // §6
void makePinNetName(const PartDefinition& p, const PartPin& pin, char out[32]);
```

**`src/routing/States.cpp`**: `fromYAML` gains `parts:` routed to a post-loop
single-pass `deserializeParts` (next to the overlays pass, States.cpp:~1384); `toYAML`
calls `serializeParts` — **this is the auto-save blocker fix**: `toYAML` is a wholesale
rewrite, and any section it doesn't emit is destroyed by the first idle auto-save
(SlotManager service, States.h:404-411). Also emit/parse top-level
`guideProgress: {source: "/projects/...", step: N}` scalars. The `guide:` steps are
**deliberately not round-tripped** — the runner re-reads them from
`guideProgress.source`. Loading a slot with `placed` parts re-expands via
`expandPartsToBridges`. Serialize `placed` per part.

**New `src/guiding/GuidedFlow.h/.cpp`**:

```cpp
enum class GuideStepType : uint8_t { NOTE, PLACE, CONNECT, POWER_ON, VERIFY, RUN_SCRIPT };
enum class GuideCheck    : uint8_t { NONE, PRESENCE, CONTINUITY, VF, VOLTAGE, OSCILLATES, I2C_ACK, RAIL_SANE };
enum class GuideOnFail   : uint8_t { WARN, RETRY, SKIP, BLOCK };

struct GuideStep {
    GuideStepType type;  int8_t partIdx;  int16_t n1, n2, target;
    GuideCheck check;    float min, max;  GuideOnFail onFail;
    uint16_t timeoutMs;  bool probeConfirm;
    char text[96];  char script[40];
};
#define MAX_GUIDE_STEPS 48
struct GuideScript {
    char title[32]; char sourcePath[96];
    GuideStep steps[MAX_GUIDE_STEPS]; int numSteps; bool autoFromParts;
};   // ~7 KB -> single static instance

bool guideParse(const char* yamlPath, GuideScript& out, String& err);
void guideRun(const char* projectYamlPath, int resumeStep = -1);   // BLOCKING app entry
struct GuideSession;                                                // ProbeSession-style
void guideTick(GuideSession& s);
```

**New `src/guiding/GuideChecks.cpp`** — the verify implementations (§5).

## 3. Runtime: state machine, controls, composition

### 3.1 Why blocking

Every wizard exemplar is a blocking loop (probeCalibApp Apps.cpp:674, self test,
VoltageAdjuster); a guide is inherently modal. Decisively: inside `serviceInner()` only
the **inner set** runs (CRITICAL priority, JumperlOS.h:172), and Highlighting/
MeasureMode are HIGH — the modal loop *automatically* silences the services that would
fight over LEDs/OLED/encoder. SlotManager (HIGH) also pauses, so auto-save can't race
mid-step — the guide calls `saveActiveSlot` explicitly at each commit. Structure as
`GuideSession` + `guideTick()` so a later Service-ification is the same mechanical
lift probeMode went through.

```cpp
void guideRun(const char* path, int resumeStep) {
    GuideSession s;  guideSessionBegin(s, path, resumeStep);
    while (!s.done) { guideTick(s); jOS.serviceInner(); delayMicroseconds(50); }
    guideExitTail(s);   // save progress, clear _GUIDE_ overlays, restore terminal
}
```

### 3.2 States

```
GUIDE_INIT      load/parse, force rails+DACs to 0V, clear _GUIDE_ overlays, banner
STEP_ENTER      render prompt (OLED+serial) + footprint overlay + target pulse
STEP_WAIT       pump input; passive presence pre-check every ~500ms on place steps
                (advance-on-detect is a hint; confirm still required in v1)
STEP_PROBE_WAIT (probe_confirm) justReadProbe() until tap on n1/n2; other rows -> show net name
STEP_VERIFY     launch check (§5), poll while pumping; timeoutMs budget
STEP_RESULT     pass -> STEP_COMMIT; fail -> on_fail policy
STEP_COMMIT     applyPartPlacement / addConnection, set net names, refreshConnections(1),
                saveActiveSlot(skipValidation), progress++, -> STEP_ENTER
STEP_BACK       removePartPlacement / removeConnection, progress--, -> STEP_ENTER
GUIDE_DONE      summary, requestLedShow(1); -> exit
GUIDE_EXIT      (hold or 'q') yesNoMenu("Save progress?") -> exit tail
```

### 3.3 Controls (all always available — Kevin's decision)

| Input | Action |
|---|---|
| Wheel turn | prev/next step (back past a committed step = STEP_BACK un-commit; forward past current = skip-with-flag) |
| Wheel click (RELEASED-off-PRESSED edge, as probeCalibApp Apps.cpp:858) | confirm / advance / re-run verify |
| Wheel hold ≥1.5 s | exit (yesNoMenu) |
| Probe CONNECT button | confirm/advance |
| Probe REMOVE button | back |
| Probe tap | probe_confirm ack; otherwise identify row (prints net name) |
| Serial (port 1) | `n`/space=next, `p`=back, `s`=skip, `v`=verify, `q`=quit, `t <row>`=probe-tap override — the HIL path |

**Probe-button polarity**: the `probe_revision>3` swap lives only in `jl_probe_button_*`
(JumperlessMicroPythonAPI.cpp:2144); raw `probeButton.getButtonPress()` is unswapped.
Shared helper:

```cpp
static int guideProbeButton() {           // 0 none, 1 CONNECT/confirm, 2 REMOVE/back — post-swap
    int b = probeButton.getButtonPress(true);
    if (jumperlessConfig.hardware.probe_revision > 3) { if (b==1) b=2; else if (b==2) b=1; }
    return b;
}
```

Encoder is serviced by core 2; read `encoderPosition`/`encoderButtonState` globals
exactly as probeCalibApp does (Apps.cpp:722, 858).

## 4. LED / OLED / terminal rendering

### 4.1 Addressing

The guided renderer speaks **GraphicOverlay 10×30 coordinates** exclusively.
Node n → `col = ((n-1) % 30) + 1`; top half (n ≤ 30) rows 1-5, bottom rows 6-10
(`coordToLedIndex`, GraphicOverlays.cpp:46 — goes through `screenMap`). **Hardware
check required before footprint math freezes**: GraphicOverlays.h:9-21 is itself
unsure of sub-row orientation ("A or E?").

Two overlays with reserved names (excluded from YAML persistence next to the
`_SELFTEST_` exclusion at GraphicOverlays.cpp:346):
- `_GUIDE_FP_`: full 10×30, transparent except placed-part outlines — dim class colors
  (power `0x1A0000`, gnd `0x001A02`, signal `0x000818`, nc `0x040404`, pin-1 marker
  `0x180800`). Persistent for the whole guide.
- `_GUIDE_TGT_`: current step's target holes, pulsed ~2 Hz by rewriting colors from
  `guideTick` + `requestLedShow(1)`. Mode 1 keeps live nets rendering underneath;
  overlays paint last (main.cpp:1809).

`connect` steps: light all 5 holes of both rows (n1 static, n2 pulsing).

### 4.2 OG fallback (1 LED/row)

Overlays and glyphs no-op on OG (GraphicOverlays.cpp:297, Graphics.cpp:3701).
Fallback: **commit the step's bridges up front in dim class colors** (`bridgeColors[]`)
— target rows light through the ordinary net render. Electrically safe: rails/DACs
held at 0V until `power_on`. Pulse = two-color alternation per tick.

### 4.3 OLED + terminal

- OLED: prompts via `oled.showMultiLineSmallText(text, true, true)` (probeCalibApp's
  3-line block, Apps.cpp:705) with the OLED-hotplug poll pattern; results via
  `ReadingDisplay::show(name, rowNode, value, value2, hint)` — dedupes, safe to call
  every tick; `ReadingDisplay::resetLastShown()` on entry (as runApp does).
- Terminal mirrors everything (works with no OLED): step text with
  `cycleTerminalColor`, rows via `printNodeOrName(node,1)`, and one machine-parseable
  line per transition: `GUIDE step=3/10 id=place_R1 state=WAIT` /
  `... state=PASS check=continuity val=0.33mA`.

## 5. Verification

### 5.1 New primitive: cross-core one-shot tap API

`senseNodeVoltage()` (NetVoltageScan.cpp:230) and `pairSenseTap()` (:330) are `static`
**and core-2-affine** (taps share core 2's loop with `sendAllPathsCore2` so they can't
race a crossbar refresh — NetVoltageScan.h:23). The background scan only visits
*routed* nets, so an unconnected part row has no fresh `nodeVoltage[]`. Add a
request/poll pair serviced inside `serviceNetVoltageScan()`:

```cpp
bool requestNodeTap(int node);                    // false if a request is in flight
int  nodeTapResult(float* volts, float* drift);   // 0 pending, 1 ok, -1 floating, -2 route/adc fail
bool requestPairTap(int n1, int n2);
int  pairTapResult(float* v1, float* v2);
```

Cost ≤11 ms worst case on core 2 (budget documented at NetVoltageScan.cpp:250); the
guide polls while pumping. Where a step's rows are already routed and powered, reuse
`nodeVoltage[]`/`nodeVoltageMs[]` when < ~250 ms old.

### 5.2 Matrix

| Step / part class | Check | Sequence | Honest limits |
|---|---|---|---|
| `note` | none | — | — |
| `place` wire / 2-lead R | `continuity` | `DAC0→ISENSE_PLUS`, `ISENSE_MINUS→rowA`, `rowB→GND` via `addEphemeralConnection` (States.h:286, LED option 0); DAC0 = 3.3 V; read INA0 current; expect `min..max` mA (author-supplied from `value:`, incl. ~100-150 Ω path R); tear down + DAC to 0 | Needs rows otherwise-unconnected; path R adds ~2-5% at 10k |
| `place` LED / diode | `vf` | Same chain into anode row, cathode→GND, DAC0 = 3.0 V; pair-tap A,B: `V(A)−V(B)` in band AND I > 0.5 mA → present+oriented; I≈0 & V(A)≈stimulus → missing/reversed ("flip it?") | Keep stimulus ≤3.3 V |
| `place` capacitor | `presence` | Tap each leg row: charge/drift signature differs from open row (`nodeDrift[]`); polarity not checkable | Weak → default `on_fail: warn` |
| `place` IC (unpowered) | `presence` | Tap each pin row; inserted legs alter drift/settling via die ESD paths | **Presence-hint only — never hard-fail.** Real verification at `power_on`/`verify` |
| `connect` | by construction | Routed bridge; `probe_confirm` adds human ack | — |
| `power_on` | `rail_sane` | Apply `power:`; wait 100 ms; VCC-class pin rows ≈ topRail ±0.2 V, GND-class ≈ 0 ±0.15 V | Voltage-sanity, not a rail ammeter — say so |
| `verify` voltage | `voltage` | Tap target, compare band | — |
| `verify` oscillation | `oscillates` | Route target→free GPIO (ephemeral), count edges over 500 ms–timeoutMs while pumping; f in `min..max` Hz; sub-Hz falls back to `nodeVoltage[]` seeing both levels | The 555 money shot |
| `verify` I2C | `i2c` | `i2cScan(sdaRow, sclRow, ..., leaveConnections=0)` (Apps.cpp:1771); expect ack at author-given address | Powered only |
| `run` | exit code | MicroPython script; nonzero → fail | Escape hatch |

All checks are polled sub-states — nothing blocks cores; every wait pumps
`serviceInner()`. `checkFloating()` (2 full refreshes) is NOT used in the loop.

## 6. Net naming

- **When**: at STEP_COMMIT — the name lands the moment the connection lands (terminal:
  `row 6 → U1_TRIG`; OLED via ReadingDisplay). Custom name already wins in
  `netDisplayName` (Highlighting.cpp:830-845).
- **Durability**: `setCustomNetName` + firstNode-keyed `reconcileAfterRebuild`
  (States.h:195) survives ordinary rebuilds, but **merges lose names**. Fix: the parts
  table is the source of truth — `partsReassertNetNames(globalState)` re-applies names
  after every rebuild (call site: end of `syncNetsFromBridges`/the reconcile hook).
  **Collision rule**: lowest part index wins per net; an explicit `nets:` `name:`
  outranks all auto names.
- **Budget**: ~50 usable nets; a guide project uses ~10-15. Warn once near cap.

## 7. Interruption & resume

Destination slot chosen up front (Kevin's decision; default: first empty). Every
STEP_COMMIT does `saveActiveSlot(err, /*skipValidation=*/true)` — bailing at step 6
leaves a working slot with 6 steps' bridges, names, colors, parts (`placed: true`),
and `guideProgress: {source, step}`; the board is usable immediately. Re-opening the
project sees `guideProgress.step > 0` → `yesNoMenu` "Resume at step N / restart?".
Restart = `removePartPlacement` for all placed parts, clear progress.

## 8. Python API

`jl_place_part(name, row, pins_json)`, `jl_remove_part(name)`, `jl_list_parts()`,
`jl_guide_progress()`. Guide *control* from Python is intentionally not offered
(both modal on core 0); HIL drives the guide over port-1 serial keys.

## 9. Testing

HIL (`test/hil/`, jl.py: REPL port 5 + terminal port 1; SWD encoder injection where
available):
- `test_parts_roundtrip.py`: file-driven parts roundtrip (see plan/ledger ruling),
  extended with place_part API once bindings land; the **auto-save rewrite survival**
  assertion is the regression that matters.
- `test_guide_flow.py`: 3-step project written via REPL, launched via port-1 command,
  driven `n n n q`, `GUIDE ...` status lines asserted; kill mid-guide → resume state;
  `t <row>` drives probe_confirm. `simulateProbeTap` can't run during a modal guide
  and can't fake buttons (Probing.h:381-388) — hence the serial override.
- Bench: real 555 kit; reversed LED and missing resistor must produce the specced
  on_fail behavior; overlay orientation check; OG smoke test.

## 10. Risks

1. **Serializer round-trip is the load-bearing wall** — land the HIL test WITH the
   serializer.
2. Overlay coordinate orientation unverified — hardware check first.
3. Electrical checks are heuristics — bands author-owned, default warn, never block
   on physics we can't promise.
4. RAM: +8 KB parts state (V5) + 7 KB GuideScript static; OG loses overlays (fallback
   is bridges-as-colors).
5. Modal loop starves HIGH/NORMAL services by design — measure mode unavailable
   mid-guide; explicit saves cover persistence.
6. MAX_BRIDGES (128 V5 / 72 OG) and ~50-net budget bound project size — parser warns.
