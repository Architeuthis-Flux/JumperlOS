# Guided Placement Subsystem — Design (2026-08-20, as-built through wave 2)

Companion to the approved plan (projects + guided placement branch). Sibling docs:
DESIGN_PROJECTS_SUBSYSTEM.md, DESIGN_SLOT_FILES.md, DESIGN_PART_ID_FOLLOWUP.md.
Where this doc and the approved plan disagree, the plan wins (it post-dates and
reconciles this design — notably: `connect:` is node-only, part-ID hook fields are
flat `type:`/`value:`/`part_id:`, and the starter catalog is 4 projects).

> **Wave 2 rewrote three things underneath this document**, and the sections that
> carry them are marked as-built:
> **(a)** placement is no longer one shape — a part is `expanded`, `compact` or
> `custom`, and the user can move it (§1.5, §3.3);
> **(b)** the guide session is a **browsable ring** with a DONE view, not a
> forward walk that exits off the end (§3.2, §3.3);
> **(c)** the guide no longer builds into a destination slot — it builds into the
> **run file** that is already the active context (§7, and
> DESIGN_PROJECTS_SUBSYSTEM.md §1b).
> The measurement machine was also rebuilt: **continuity reports ohms** (§5.3).

## 0. Summary of decisions

| Question | Decision |
|---|---|
| Language | **Hybrid-declarative**: `parts:` (TODO-component-placement.md format + extensions) auto-generates placement steps; optional `guide:` adds ordering, free text, power-on, verify overrides |
| Runtime shape | **Blocking app loop** pumping `jOS.serviceInner()`, internally `GuideSession` struct + `guideTick()` enum-state machine — exactly probeMode's T3.1 M1 shape (Probing.cpp:2643), liftable to a Service later |
| LED channel | **GraphicOverlays** (rendered last over live nets, main.cpp ~1809) in `requestLedShow(1)` netlist mode; never mode 3 (would evict live nets); no `b.print()` breadboard text in v1 (mode-2 menu buffer conflict) |
| Guide persistence | ~~Direct-into-slot (destination chosen up front)~~ → **wave 2: direct into the RUN FILE** the launcher already opened as the active context: half-built project persists; `guideProgress:` scalars round-trip; step text always re-read from the canonical project wiring, never from the run file |
| Placement shape | **wave 2**: per part, `expanded` (legs at the footprint rows + bridges) / `compact` (a leg sits IN its `connect:` hole) / `custom` (expanded geometry at a row the user moved it to) — §1.5 |
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

- **DIP mapping rule** (top rows = nodes 1-30, bottom = 31-60, node n±30 = same
  column across the ravine): `row:` is pin 1's ACTUAL hole, and for `dipN` it MUST
  be on the bottom half (31-60) — pin k ≤ N/2 → `row + (k-1)` (bottom, left→right);
  pin k > N/2 → `(row - 30) + (N-k)` (top, right→left). DIP-8 at row 35 → pins 1-4 =
  nodes 35,36,37,38; pins 5-8 = nodes 8,7,6,5. A top-anchored `row` (≤30) is no
  longer a legal DIP anchor at all — it fails placement/validation instead of
  silently mirroring. Nano-header nodes are legal `connect:` targets but not legal
  `row` bases in v1.
  > **Wave 2 correction (bench-found, photo committed —
  > `CodeDocs/PROJECTS_BENCH_CHECKLIST.md`, `CodeDocs/IMG_20260814_064104.jpg`):**
  > the mapping above is the FIXED version. The original v1 mapping anchored DIPs
  > on the TOP half and mirrored `row+30` downward for the far side — that put
  > pin 1 (the dot/notch) at the top-left, but real chips sit with pin 1 at the
  > BOTTOM-left. Wave 2 also added `axial2` (pin 1 = `row` on the top half,
  > pin 2 = `row+30`), the default footprint for 2-leg parts (resistors, diodes)
  > straddling the ravine; radial parts (caps, LEDs) keep `sip2` in adjacent rows.
- `connect:` absent → the leg occupies the hole but no bridge.
- Expansion to bridges via `JumperlessState::addConnection` (idempotent —
  `hasConnection` States.h:273 skips exact duplicates).
- Net names: `{NAME}_{PIN}` uppercased/sanitized, ≤31 chars (`NetNameEntry::name[32]`,
  States.h:172).

### 1.2 `guide:` section

One flow-map line per step, same tolerant style as `bridges:` — **`text:` is parsed by
quote-pair (like `name:` in `deserializeNets`, States.cpp:1854-1861) and must be the
last field** (field extraction splits on bare commas).

> **ONE STEP IS ONE `- {...}` FLOW MAP (wave 2, made explicit after a silent
> failure).** A flow map that merely **wraps** is fine — `guideParse` joins
> continuation lines until the braces balance (quote-aware, bounded at 4 lines /
> 512 chars, because the parser is streamed line-by-line precisely so a whole-file
> `String` cannot exhaust the heap again). What is **not** supported is true YAML
> **block style** (`- id: x` on one line, `  do: place` on the next): each field
> line falls through to the guide-level key arm, which ends the `steps:` list, so
> the step *after* it is dropped. That used to happen in total silence — two
> authored steps parsing as one, measured three ways on the board — and now
> prints `guide step spans lines - unsupported, next step may be lost`.
> The rule is also in `States.h`'s guide-format block; `test_guide_flow.py`
> phase 11b is the needle.

**Step types**: `note`, `place` (commits that part's bridges+names), `connect`
(explicit bridge; `probe_confirm: true` waits for a probe tap on `n1`/`n2` via
`justReadProbe()` polling — **not** by nesting `probeMode(1,x)`, whose exit gestures
(Probing.cpp:3130-3147) would shadow the guide's controls), `power_on` (applies the
`power:` section; rails/DACs forced to 0V from guide start until this step), `verify`
(standalone check on `target` node or part pin), `run` (MicroPython script — escape
hatch).

**Per-step fields**: `do`, `part`, `n1/n2`, `target`, `check`
(`none|presence|continuity|vf|voltage|oscillates|i2c|rail_sane`), `min/max`
(**OHMS for continuity as of wave 2** — see §5.3; V for vf/voltage, Hz for
oscillates), `tol` (per-part, percent, on the part not the step), `on_fail` (`warn` [default: show ✗,
allow manual advance] | `retry` | `skip` | `block` [advance only via explicit skip
gesture]), `timeout_ms` (default 1500), `probe_confirm`, `text` (last).

### 1.3 Why hybrid holds up

`parts:` alone yields: footprint lighting, prompt, bridges, net names, default check
by part class. What it cannot express — and what `steps:` exists for: ordering across
parts vs interleaved notes, human context ("long leg", "the dot"), power-on placement
in sequence, verify-only steps, probe_confirm pedagogy, scripts. Authors with trivial
projects write zero steps (`auto: true`).

### 1.4 `connect:` steps use LITERAL rows and do not re-derive

A `do: connect` step's `n1:`/`n2:` are literal node names. When a part **moves**
(§1.5), its own wiring re-derives for free — bridges are never stored per part;
`expandOnePart` recomputes `partPinNode` against `pin.connect` every time it
applies — but a `connect:` step that named one of the rows the part just left is
now stale, and the guide **cannot fix it**. All it can do is say so:

```
  (note: step k targets row NN, which RG just left)
```

**Author rule:** route a part's own wiring through the parts' `pin.connect`
fields wherever you can, and keep `connect:` steps for board-to-board jumpers
that belong to no part.

### 1.5 Placement modes (wave 2)

`PartDefinition::placement` — `0 = expanded` (default), `1 = compact`,
`2 = custom` (`PART_PLACEMENT_*` in States.h). Serializer and parser landed in the
**same commit** per the round-trip rule, and the byte is emitted **only when
non-default** (the `verify:`/`color:` precedent), so every pre-wave-2 file is
byte-identical after a rewrite.

- **expanded** — legs sit at the footprint rows and a bridge runs from each leg to
  its `connect:` endpoint. The original v1 shape.
- **compact** — the breadboard-native shape from Kevin's photo: a leg whose
  `connect:` is a real hole **goes into that hole**, so there is no bridge for it
  at all. This is why `partPinNode()` is **the** geometry authority — every
  consumer (bridge expansion, removal, net naming, the `_GUIDE_FP_`/`_GUIDE_TGT_`
  overlays, check row resolution, `list_parts`) already went through it, so a mode
  change propagates by construction. **Bridge suppression is free**:
  `expandOnePart` already had `if (node == pin.connect) continue;`, and
  `removePartPlacement` guards on `hasConnection(node, pin.connect)` with
  `hasConnection(x, x) == false`, so the mirror case works too.
- **custom** — expanded geometry at a row the user moved the part to. `row:` *is*
  the custom row; there is no second field.

**Compact eligibility is PER PIN**, not per part: `pin.connect` must resolve to a
physical hole row — `1-60`, `TOP_RAIL` or `BOTTOM_RAIL` — the pin must not be
`class: nc`, and the part must not be a DIP (an IC's legs *are* its footprint).
`GND (100)` and every other fabric-only node (DAC/ADC/GPIO/ISENSE) is **not**
eligible: it has no holes.

| shape | expanded | compact | bridges left in compact |
|---|---|---|---|
| 2-leg, both connects are hole rows (the photo: chip-pin row → `TOP_RAIL`) | legs at `r`, `r+1`; two bridges | each leg in its endpoint hole | none |
| 2-leg, one hole + one fabric node (`connect: GND`) | legs `r`, `r+1`; two bridges | the hole leg moves; the GND leg **keeps its footprint row and its bridge** | one |
| 2-leg, one connect + one OPEN leg (LED: anode `connect: 22`, cathode absent) | legs `r`, `r+1`; one bridge | anode in 22; cathode follows into **23** (partner+1, same half) | none |
| …same, partner at a HALF boundary (30 or 60) | — | open leg falls back to **partner−1** (29 / 59) | none |
| …same, partner in a RAIL | — | open leg **keeps its footprint row** — a rail has no "adjacent row" | none |
| `class: nc` pin | footprint row, no bridge | **unchanged** | none |
| 3+ pins with an open leg and ≥2 eligible partners | footprint rows + bridges | open leg **keeps its footprint row** (no unambiguous "the other leg") | the eligible legs' bridges vanish; the rest stay |
| any DIP/IC | footprint | **refused**, and a hand-written `placement: compact` on a DIP is *normalized* to expanded at `commitPart` with a warning, so the serializer heals the file | unchanged |

Two readings the design text got wrong and the code fixes, both commented at the
site: **the open-leg fallback edge is the HALF boundary, not row 60** (nodes 30
and 31 are opposite corners of the board, so `partner+1` must stay in the same
half), and **a rail partner has no adjacent row** (`TOP_RAIL+1 == BOTTOM_RAIL`).

**The remove-before-mutate invariant** (stated at `guideMovePart`, the only
mutator, and at `removePartPlacement`'s declaration): never change a part's
`baseRow` or `placement` while `placed == true` other than as
**remove → mutate → reapply**. Both ends recompute their endpoints from
`partPinNode`, so removal MUST see the geometry the apply used; mutating first
strands every existing bridge on the fabric with nothing left that can find it
again. The compact snap **on a committed part** is the stress case, and HIL
phase 13 walks exactly it.

**Whole-part geometry is one predicate.** `partGeometryOk(p, reason, reasonLen)`
walks pin count, the even-DIP/axial rule, `row:` bounds, the per-footprint span,
**and every LISTED pin** — `offset:` forms included. `commitPart` (parse),
`jl_place_part` (API) and the guide's move check all call it, so an entry accepted
by one and rejected by another is structurally impossible instead of maintained by
comment. **Behaviour change to disclose:** a file whose out-of-bounds `offset:`
(or over-`pinCount` `pin:`) pin previously placed *partially* now has the **whole
part dropped**, with an unconditional print naming the pin. A partial part is
never right, and silence plus a wholesale-rewrite auto-save is how parts vanish.

`offset:` itself is **expanded-mode geometry only** (a compact-eligible pin ignores
it; a non-eligible one falls back to it), and `axial2` has made most historical
uses obsolete — the old way to straddle the ravine was a `sip2` with
`{offset: 0}`/`{offset: 30}`, which does not even parse.

**Two v1 limitations, accepted:** `custom` is lost by a compact round trip
(`m 44` → custom, `c` → compact, `c` → expanded leaves the part at row 44 marked
expanded — the *row*, the thing that matters, always survives; remembering the
pre-compact mode needs a second byte), and "moved back to the authored row" stays
`custom`, because no authored row is stored anywhere and capturing it would mean
parsing a second file mid-session.


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

**As built (wave 2), the machine gained a resting state and the ring:**

```
STEP_ADJUST     a MODAL move session on a place step (double-click / dblclick twin).
                Wheel SLIDES the part to the next legal row, click DROPS,
                dblclick cycles compact/expanded, hold/q CANCELS (restoring row
                AND mode). Emits `state=ADJUST`; leaving it re-emits the wait
                state and the landing annotation.
DONE            NOT a doorway - a browsable SUMMARY VIEW. Entered by committing
                the last unfinished step, by wheeling past either end of the
                ring, or by skipping the last step. Emits
                `GUIDE done committed=<c> skipped=<s> unfinished=<u>` after the
                state line. Left by browsing out, confirming, or quitting.
```

**The headline invariant: a wheel TURN can never reach EXIT.** Turns map to
`GuideKey::NEXT`/`PREV`, those reach exactly one function (`guideBrowse`), and
`guideBrowse` writes only `stepIdx`, `summaryShown` and a state of `STEP_ENTER`
or `DONE`. Browsing forward off the last step lands DONE; again wraps to step 1;
backward off step 1 wraps into DONE. Only a hold, `q`, or a **confirm at a clean
DONE** exits.

`COMPLETED` means **nothing unfinished**, not "the cursor reached the end" —
`guideFirstUnfinished(s) >= numSteps`. With a browse ring the cursor can sit at
DONE with the whole build untouched, and quitting from there must not claim a
finished build (offering the script, printing "Run saved"). A confirm at DONE
with work outstanding **jumps to the first unbuilt step** instead of exiting.

**The 260 ms confirm pend, and the race it created.** `guideReadInput` tests
`DOUBLECLICKED` first, then `HELD`, then RELEASED-off-PRESSED — which no longer
returns CONFIRM but arms `pendingConfirmMs` and returns NONE; a later tick past
260 ms returns CONFIRM. That is what makes the double-click distinguishable, and
it costs ~260 ms of latency on every wheel-click confirm (serial `n` and probe
CONNECT stay instant). The pend is cleared by any probe button, any serial byte,
`STEP_ENTER`, `STEP_RESULT`, **and DONE's arrival block** — the last one is
load-bearing: a click on the last step followed by a turn inside 260 ms, or an
impatient click in the final quarter-second of the 900 ms pass-hold, would
otherwise let the pend mature *at DONE*, where CONFIRM on an all-built guide
means EXIT. I.e. a wheel turn ending the guide. The clear must sit **inside**
`if (!summaryShown)`, not at the top of the case: the case body runs every tick
while resting in DONE, so an unconditional clear would kill the legitimate
wheel-click-at-DONE. **No HIL needle is constructible** — a pend can only be
armed by an encoder RELEASE, and every serial byte clears one. It is bench
item 3/4 in the checklist.

### 3.3 Controls — AS BUILT (wave 2)

Kevin's **control-surface principle**: the pads are ABSOLUTE (tap a row, the part
goes there), the wheel is RELATIVE (turn, it slides). Every gesture has a serial
twin so HIL can drive the logic even where it cannot drive the input.

| gesture / key | WAIT / PROBE_WAIT | ADJUST | DONE view | mid-check (VERIFY) | RESULT hold |
|---|---|---|---|---|---|
| wheel turn | **browse** prev/next, wraps through DONE | slide to the next LEGAL row (± , scans 30) | browse (wraps into the steps) | ignored | breaks the hold |
| wheel click | confirm — **pended 260 ms**; on a committed step it re-verifies | drop at the new row | finish when all built, else jump to the first unbuilt step | ignored (pend discarded at RESULT) | breaks the hold |
| wheel double-click | enter ADJUST (place steps; else "nothing to adjust on this step") | cycle compact/expanded, stay in ADJUST | ignored | ignored | breaks the hold |
| wheel hold | quit guide | **cancel** adjust | quit guide | abort check + quit | **quits** (not swallowed) |
| probe CONNECT | confirm (instant) | drop | finish/jump as click | ignored | breaks the hold |
| probe REMOVE | un-commit / `STEP_BACK` | cancel adjust | re-open the last step | abort + back | breaks the hold |
| probe tap | 1 probe_confirm → confirm; 2 own footprint → snap; 3 free hole → move pin 1; 4 else → identify | 2/3/4 (no probe_confirm mid-adjust) | ignored | ignored | breaks the hold |
| `n` / space | confirm (instant, no pend) | drop | finish/jump as click | ignored | breaks the hold |
| `p` | un-commit / back | cancel adjust | re-open the last step | abort + back | breaks the hold |
| `s` | skip-with-flag — **refused on a committed step** | ignored | ignored | abort + skip (same refusal) | breaks the hold |
| `v` | verify-only | ignored | ignored | ignored | breaks the hold |
| `q` | quit | **cancel adjust — NOT quit** (symmetric with hold) | quit | abort + quit | **quits** |
| `t <row>` | simulated tap (all four rules) | simulated tap (rules 2–4) | ignored | ignored | breaks the hold |
| `m <row>` | move pin 1 (place steps only, refused by name otherwise) | same | ignored | ignored | breaks the hold |
| `c` | cycle compact/expanded | same | ignored | ignored | breaks the hold |
| `>` / `<` | browse next / prev | **slide** — the wheel's twins slide where the wheel slides | browse | ignored | breaks the hold |

Banners, verbatim:

```
wheel=browse  click=confirm  dblclick=adjust  hold/q=quit  n/p/s/v  t/m <row>  c=snap  >/<=browse
  adjust RG: wheel=slide  click=drop  dblclick=snap  hold/q=cancel  (probe: tap=move, CONNECT=drop, REMOVE=cancel)
```

**Tap precedence** in `STEP_WAIT`/`STEP_PROBE_WAIT`, first match wins: (1) a
`probe_confirm` match, (2) the current PLACE part's own lit footprint → cycle
snap, (3) a **free** hole → move pin 1 there (a DIP tap on rows 1–30 maps to
`+30`, because pin 1 lives on the bottom half), (4) anything else → identify the
row. Non-place steps get 1 and 4 only. **"Free" is deliberately stricter than
`guideMoveLegal`**: a tap on a row that carries a net falls through to *identify*,
while `m <row>` onto that same row is a legal move with a `(row N joins net …)`
heads-up. Funnelling the tap into the move checker would silently eat identify on
every occupied row — bench-visible, HIL-invisible.

**`m <row>` deliberately does NOT apply the DIP `+30` mapping.** The pad cannot
type, so it gets the convenience; the typed key is literal, and `m 5` on a DIP
earns the honest `dip pin 1 (row:) must be on the bottom half` refusal — which is
also the only way that refusal is reachable at all.

**Refusal matrix** (every one prints `move refused: <reason>`, keeps the current
row **and** mode, and changes nothing on the fabric or on disk; first hit wins):

| # | condition | reason | source |
|---|---|---|---|
| 1 | footprint pin count outside 1-60 | `footprint pin count must be 1-60` | `partGeometryOk` |
| 2 | DIP/axial with an odd pin count | `a dip/axial footprint needs an even pin count` | `partGeometryOk` |
| 3 | target row outside 1-60 | `row: must be 1-60` | `partGeometryOk` |
| 4 | DIP anchored on the top half | `dip pin 1 (row:) must be on the bottom half (31-60)` | `partGeometryOk` |
| 5 | DIP that does not fit | `dipN at row R does not fit the board (far-side columns run past row 30)` | `partGeometryOk` |
| 6 | SIP run leaving its half | `sipN at row R does not fit the top/bottom half (runs past row 30/60)` | `partGeometryOk` |
| 7 | axial2 with ≠2 pins / bottom-anchored | `axial2 must have exactly 2 pins` / `axial2 pin 1 (row:) must be on the top half (1-30)` | `partGeometryOk` |
| 8 | a listed pin (incl. `offset:`) off-board on the footprint | `pin X (offset: N \| pin: N) does not land on the board from row R` | `partGeometryOk` |
| 9 | compact on a DIP | `ICs don't compact - their legs are the footprint` | `guideMoveLegal` |
| 10 | compact with no eligible pin | `no leg has a hole-row endpoint to sit in` | `guideMoveLegal` |
| 11 | compact same-row collapse | `legs A and B would both land on node N` | `guideMoveLegal` |
| 12 | a pin off-board in the MODE (not just the footprint) | `pin X lands off-board` | `guideMoveLegal` |
| 13 | another PLACED part's leg on the same row | `collides with R3 at row 44` | `guideMoveLegal` |
| — | non-place step | `(m <row> works on place steps only)` / `(c works on place steps only)` | `guidePlaceStepPart` |
| — | already there | `(RG is already compact at row 44)` | `guideMovePart` |

**Allowed with a heads-up, not refused:** a row that merely carries a net or a
bridge — joining an existing net by placement is ordinary breadboarding. Prints
`(row N joins net NETNAME)`, suppressed when the "net being joined" is one the
part's own legs already make (which otherwise fires on every compact snap of a
committed part). **Rails are exempt from rule 13** — a rail is a long bus and two
legs in it is what the user asked for; rails are never slide/move *targets*
either (`baseRow` moves within 1-60 only, and rails are reachable only as compact
endpoints).

**Two rulings worth keeping visible:**

- **`s` on a browsed, already-committed step is refused** —
  `(already committed - p removes it first)`. Clearing `committed[i]` while the
  bridges that commit built are still on the fabric is a session/fabric
  divergence: the flag would claim "never done" about hardware that is done. It
  only became reachable when browsing let the cursor park on a committed step.
  `guideDoSkip` returns a bool for this, because a refusal has to leave the
  caller somewhere — the **mid-check** skip path has already aborted the check,
  so a refusal that stayed in `STEP_VERIFY` would re-begin it forever.
- **`q` means two different things one state apart** — quit in WAIT, cancel in
  ADJUST. That is symmetric with hold, and it is the one place in the table where
  a key does not mean the same thing everywhere. Two presses still get you out.

**Probe-button polarity**: the `probe_revision>3` swap lives only in
`jl_probe_button_*` (JumperlessMicroPythonAPI.cpp:2144); raw
`probeButton.getButtonPress()` is unswapped. Shared helper:

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

> **AS BUILT — the scan-reuse clause above is DEAD. The checks always tap.**
> The `< ~250 ms old` shortcut was written and then dropped during task 7:
> the background scan smooths through an alpha-0.3 EMA *and* is input-paused
> exactly while the guide is being driven, so a **timestamp-fresh sample can
> still be one EMA step off a stale seed**. On the bench that served 0.77 V
> for a row a DAC was holding at 2.5 V. Timestamp-fresh is not value-fresh,
> and a one-shot tap costs ≤11 ms, so `GuideChecks.cpp`'s `VOLTAGE` (and
> every other tapping check) requests a tap unconditionally. The rationale is
> repeated at the `case GuideCheck::VOLTAGE` site; don't reintroduce the
> shortcut without solving the EMA-seed problem.
>
> One more piece of the tap API landed after this section was written:
> **`cancelOneShotTap()`**. A request that loses its owner — a check aborted
> while the scan core's hardware gates were closed — used to sit in the
> mailbox and fire later, unowned, closing real routes at a moment nothing
> was measuring. `guideCheckAbort()` now cancels it; the scan core consumes
> the request without touching hardware. See the contract in
> `NetVoltageScan.h`.

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
| `verify` oscillation | `oscillates` | Route target→free GPIO (ephemeral), count edges over a window of `max(timeout_ms, 500 ms)` while pumping; f in `min..max` Hz. **No free GPIO → tap fallback, no frequency** (§5.2a) | The 555 money shot |
| `verify` I2C | `i2c` | `i2cScan(sdaRow, sclRow, ..., leaveConnections=0)` (Apps.cpp:1771); expect ack at author-given address | Powered only |
| `run` | exit code | MicroPython script; nonzero → fail | Escape hatch |

All checks are polled sub-states — nothing blocks cores; every wait pumps
`serviceInner()`. `checkFloating()` (2 full refreshes) is NOT used in the loop.

### 5.2a As-built deviations from the matrix

Recorded here so the table above can stay readable. Each is implemented and
commented at its site in `GuideChecks.cpp`.

- **`oscillates` — the fallback is keyed on GPIO AVAILABILITY, not on
  frequency.** The matrix says "sub-Hz falls back to `nodeVoltage[]` seeing
  both levels", which reads as a frequency-triggered second path. It isn't,
  and it can't be: you cannot know a signal is sub-Hz until after you have
  failed to measure it. What the code actually does is pick the measurement
  method up front, from whether a *free routable GPIO exists*. If one does
  (not MicroPython-owned, no PWM, not the top-OLED pins, no bridge, config
  direction INPUT — an output-configured pin would drive the net), the check
  routes the target to it with an ephemeral bridge and counts edges: real Hz,
  compared against `min..max`. If none is free, it falls back to repeated
  one-shot **taps** across the window (not raw `nodeVoltage[]`) and reports
  "both levels seen" as `osc` — a verdict with **no frequency at all**, so a
  `min/max` band cannot be honoured on that path. Say so to the author rather
  than inventing a number.
- **The window is author-sized via `timeout_ms`, and IS the schedule.**
  `oscWindowMs = max(timeout_ms, 500)`, and the step's effective timeout is
  then set to `window + 1000 ms`. So `timeout_ms` on an `oscillates` step is
  not "give up after this" — it is "measure for this long". A slow blinker
  needs a window long enough to contain several periods (the edge count is
  divided by 2 and by the window), which is the knob an author reaches for
  when the sub-Hz case matters. The GPIO path samples once per poll at the
  guide loop's multi-kHz cadence, so it is good to a few hundred Hz.
- **`rail_sane` with an explicit `target:`** treats that row as VCC-class
  (rail ±0.2 V). See the `guide:` format notes in `States.h` for why, and for
  the `check: voltage` recipe that expresses the GND-class band instead.
- **`presence` on `type: ic`** returns SKIPPED/`ic_unverified` rather than
  running the stimulus. The carve-out matches the exact string `ic`; an
  author who types something else and writes an explicit `check: presence`
  gets the real (bounded, watchdog-capped) stimulus into the chip's ESD
  paths. Commented at the site.
- **`rail_sane`'s honest limit is a doc caveat, not a runtime message.**
  Hints print only on FAIL, and a rail_sane pass is the common case in every
  `power_on` step, so "voltage-sanity, not a rail ammeter" lives here and in
  the source comment instead of on the terminal and OLED after every
  successful power-up.

### 5.3 The measurement machine, AS BUILT (wave 2)

§5.2's continuity/vf rows describe the **old** top-side chain and an author-supplied
milliamp band. Both are gone. What follows is the machine.

**The topology is GROUND-SIDE, and the sense legs ride the chain.**

```
DAC0 ── rowA ── [part] ── rowB ── ISENSE_PLUS
                                    [2 Ω shunt R1, INA0]
                                  ISENSE_MINUS ── GND
rowA ── ADC(chA)        rowB ── ADC(chB)          (high-Z sense legs)
```

All five legs are built in `chainBegin` and land in **one**
`refreshLocalConnections(0,0,0)` + `waitCore2()`. The ADC channels are acquired
**first** (`INFRA_ADC_GUIDE`, mask `0x0F`, `allowSharedTdm=false`), because the
user-claimed-ADC exclusion has to pick before the bridges are authored.

Three things this buys, each measured rather than argued:

1. **The CURR_SENSE− sink is structurally gone.** The old top-side chain drew
   **2.319 mA with no part at all** (≈1.42 kΩ to ground, the U12 DAC_1-repurpose
   path). Ground-side reads **0.000 mA** — below one LSB — while a bridged pair
   still draws 16.2 mA, which is the positive control that proves the loop is
   closed rather than merely absent. HIL phase 12c asserts all three.
2. **Four-wire.** The sense pair brackets the part, so the stimulus path drops out
   of the arithmetic: a crossbar "part" that 2-wire calls **236 Ω** measures
   **57 Ω** four-wire, stable to 2.2 % over ten reads.
3. **Option 1 dissolved the `noroute` class.** Routing the sense legs *with* the
   chain through the full router — instead of planning a lone tap in three tiers
   after the chain had already eaten the target chip's escape lanes — makes the
   exact fabric that starved on the bench **measure**. The sequential-same-ADC
   one-shot taps survive as the fallback.

**The zero point is an OPERATING POINT, not a tare.** `DAC0` at "0 V" really sits
at about −73 mV, and through a low-resistance part that is a genuine −0.40 mA
which the part is conducting: `ΔV_ofs / I_ofs` reports the *same* resistance the
loaded point does. So the offset current is **signal**. Both the shunt and the
sense pair are read at the zero point, and R and Vf are taken from the
**difference between the two operating points** — a resistor is ohmic, so the
slope between two points IS its resistance, and the difference cancels both
channels' offsets and the INA's register zero exactly. (In `TAPS` mode the offset
read is skipped: both nodes ride one channel there, which cancels the channel
offset by construction.)

**Phases:** `CHAIN_BUILD` → `OFS_SETTLE` (30 ms) → `OFS_SAMPLE` (4 fresh shunt
samples + the sense pair + the leak gate) → `STIM_SETTLE` (60 ms) → `STIM_SAMPLE`
(8 fresh samples; `I_part = I_raw − I_ofs`) → `V_READ` → compute. **The INA poll
interval is 50 ms, not 10**, so 4+8 samples cost ~600 ms on their own and the
continuity/vf timeout floor is raised to 1400 ms.

**The fallback ladder — two different failures, kept distinct:**

| situation | behaviour |
|---|---|
| both free ADCs user-claimed → cannot acquire two | `SenseMode::TAPS` (the sequential same-ADC one-shots) |
| a **stimulus** leg in `unconnectablePaths[]` after the refresh | hard `FAIL "stimulus chain routing failed"`, `val=setup` |
| a **sense** leg in `unconnectablePaths[]` | one printed line, degrade to `TAPS` — refusing would make a check unrunnable where the old one ran |
| ring will not serve a fresh window, 8× | printed line, degrade to `TAPS` |
| `vSenseA < 0.25 V` under a ≥1 V stimulus | the rowA sense bridge is not on the row — degrade to `TAPS` |
| `\|I_part\| ≥ 2 mA && \|vB\| < 20 mV` | the rowB gate (current-conditional on purpose: at 47 k / 5 V the honest vB is ~14 mV) — degrade to `TAPS` |
| taps hard-fail (`-2`) on continuity **with** a parsed `value:` | `SenseMode::CURRENT` — a plausibility verdict, `val=~2.9mA` |
| taps hard-fail on vf, or on continuity with no value | honest `noroute@<node>` + the tap diagnostics |

Two independent routing-failure detectors are used on purpose: the router's own
`unconnectablePaths[]` (rebuilt at the end of every `bridgesToPaths()` pass, so it
is about *this* refresh) and the measured movement gates.

**Bands are OHMS.** `value:` + `tol` (per part, percent; default 15) +
`tol_meas` (5 / 10 / 25 by decade) derive the band; an explicit `min:`/`max:`
still wins. Two guards catch a file carrying pre-wave-2 milliamps — a band that
does not **bracket** the parsed `value:`, or a value-less part whose `max:` is
under 5 Ω — and log `min/max look like legacy mA (…) - ignoring, using the
value-derived ohm band`. Parts ≥20 kΩ are driven at **5 V** instead of 3.3.
Everything over **470 kΩ** returns `toohighR` ("placed unverified"): past that
there is nothing to measure, and saying so beats guessing.

**Value strings a reader will meet:** `open`, `short`, `<R>` (e.g. `9.87k`),
`~<I>mA` (a leading tilde = *R was not measured*, this is a plausibility verdict
on the current — never read it as a resistance), `toohighR`, `leakX.XXmA` (the
isense path is carrying current with the stimulus at 0 V), `srcNNmA` (INA1 saw
>50 mA *leaving DAC0*, i.e. fault current bypassing the ground-side shunt),
`noroute@<node>`, `setup`, `skip`, `timeout`, `ic_unverified`, `unsupported`.

**Two watchdogs, not one.** The primary trips at |I| > 50 mA through the shunt.
The ground-side move made it blind to current that leaves DAC0 into rowA and
reaches ground **without** passing rowB — a mis-seated leg landing in a grounded
row is the realistic case — so **INA1** (`0x41`, whose 2 Ω R57 sits in DAC0's
*output* path) trips the same teardown, rate-limited to one I²C transaction per
50 ms (the cadence the chip converts at, so it cannot miss a fault for longer
than the primary's own sample period).

**`rail_sane` changed what it compares against.** Two gates: the rail itself
within 0.25 V + 5 % of its setpoint, then each **power-class** row within
max(0.15 V, 3 %) of the **measured** rail, on the same ADC. `val=` names the
worst power-class row. This board's rails run ~220 mV low, so `4.77V@8` on a
5.00 V rail is now the **PASS** it always should have been.

**Diagnostics, printed, never a verdict:** an INA1 crosscheck (gated on
`I_part ≥ 0.5 mA`, because ungated it announced a leak under every honest `open`)
and an ADC-scan-current crosscheck (log-only at I ≥ 2 mA, >25 % disagreement
names **task #32** as open — the scan estimate is diagnostic-only in guide checks
until that task closes).

**Disclosed limits.** The RING path cancels channel **offset** but not channel
**gain** (~1–2 % of each node's voltage — under 2 % of R at a real 330 Ω part,
inside `tol_meas`; the known fix is a second ring dwell with the channels
swapped). Capacitance stays presence-only, and that check keeps the OLD top-side
chain deliberately, because it charges rather than measures current. Four rungs of
the ladder above are code-reviewed but never HIL-exercised, because every one of
them needs the router or the ring to fail in a way no authored fixture produces.

Two off-bench helpers, both exact-token `z` subcommands: `z band <value> [type]
[tol]` (the parsed value and the derived band, no hardware) and `z shunt [n]`
(n fresh INA0 shunt-register samples and their spread in LSBs).

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

## 7. Interruption & resume — AS BUILT (wave 2)

~~Destination slot chosen up front~~ — **the guide builds into the RUN FILE** the
launcher already opened as the active context (DESIGN_PROJECTS_SUBSYSTEM.md §1b).
Every STEP_COMMIT still does `saveActiveSlot(err, /*skipValidation=*/true)` —
bailing at step 6 leaves a working circuit with 6 steps' bridges, names, colors,
parts (`placed: true`) and `guideProgress: {source, step}`; the board is usable
immediately. Restart is no longer destructive: "start new" allocates run N+1 and
leaves run N intact on disk, so the old un-place loop + `guideSource` wipe is
deleted.

**`guideProgress.step` is the FIRST UNFINISHED step, not `stepIdx + 1`.** With a
browse ring the cursor can be anywhere, and persisting the cursor would let a
resume skip a step that was never built. `guideFirstUnfinished(s)` is what gets
written, and INIT marks every step *before* the resume index as committed.

> **Limitation, accepted for v1 and worth knowing before you debug it:**
> `guideProgress` is **one scalar**, so commits made *beyond* the first unfinished
> step are forgotten on the next launch — they come back flagged unfinished even
> though they are physically built. Re-confirming them is **safe**
> (`expandOnePart` is guarded by `hasConnection`, and CONNECT's commit is too, so
> no bridge is duplicated), but the user re-walks work that is already done. The
> real fix is a `committed[]` bitmap in the YAML.

A saved step at or past the end — including a source edited down to *fewer* steps —
returns `ALREADY_COMPLETE` **without starting a session**, rather than dropping the
user into a DONE summary. `guideRun` returns a result
(`PARSE_FAILED / NOTHING_TO_DO / ALREADY_COMPLETE / QUIT / COMPLETED`) because the
launcher cannot tell exit F from exit H otherwise, and reparsing a 7.5 KB
`GuideScript` launcher-side just to learn `numSteps` is not affordable.

### 7.1 The rails across a guide session

**The rule:** launching a guided project must not disturb the user's bench. The
rails they had set stay live until the guide's INIT parks everything at 0 V, and
they come back on the way out **unless the project's own `power_on` took
ownership**.

`captureUserPower()` runs at the top of both launcher entries, reading live
`globalState.power` — the last moment the user's pre-project values exist, because
the very next thing either entry does is load a run file over them. `guideRun`
takes a `GuideRunPower*` in/out: the guide only **names** the values at the exit
tail, and the **caller performs** the restore, with `save=0`.

| Path | `guideRun` returns | Session ran? | Rails afterwards |
|---|---|---|---|
| quit from WAIT / PROBE_WAIT / ADJUST, before `power_on` | `QUIT` | yes | **captured user values restored** |
| quit mid-check (VERIFY abort), before `power_on` | `QUIT` | yes | captured values restored |
| quit from the DONE view with steps unfinished | `QUIT` | yes | captured values restored |
| quit after a committed `power_on` **with** a `power:` section | `QUIT` | yes | **the project's power stands** |
| quit after a committed `power_on` with **no** `power:` section | `QUIT` | yes | captured values restored (nothing was applied) |
| every step committed/skipped, exit from DONE | `COMPLETED` | yes | project's power if `powerApplied`, else captured values |
| guide source unreadable / nothing to build / already complete | `PARSE_FAILED` / `NOTHING_TO_DO` / `ALREADY_COMPLETE` | **no** | **the run file's own power is applied** |

The boundary in one sentence: **the rails rule governs the exits where a guide
session actually ran; everything else is a plain context load.** A guided launch
where no session ever ran (a finished build re-opened, say) must still come up
powered, or its companion script runs on a dead board.

`save=0` is deliberate: the run file keeps the safe 0 V `guideForcePowerSafe(save=1)`
wrote, so a half-built project re-opened later still comes up unpowered. Live rails
and the file diverge **on purpose**.

**That divergence is why the rails got a hardware twin.** `railHwVolts[2]`
(Peripherals.cpp) is written by `setTopRail`/`setBotRail` on **every** write
regardless of `save` — the exact twin of `s_dacHwVolts`, which has worked that way
for the DACs all along. `getDacHardwareVoltage()` answers channels 2 and 3 from it,
falling back to `globalState.power` until the first rail write of the session.
Pointed at it: `dac_get(0..3)`, the rail net reading (OLED + terminal), the rail
LED voltage dots (which read `railHwVolts[]` **directly** — that renderer is
`__not_in_flash_func` and must keep drawing while flash is busy, so it cannot call
the flash-resident accessor), and the **rail adjuster's initial value** (an
explicit adjust-confirm IS a deliberate write, so seeding from what the rail is
physically doing is right, and the divergence heals as a result).

**`Y` deliberately did not move.** Its body *is* the save format — `S` pastes it
back and slot files are written by the same `toYAML()` — so changing its numbers
would change what gets saved and what a paste restores. `cmd_printYAML` (a readout
call site, not the serializer) prints one advisory line instead, above `version:`
so `board_state_capture`'s slice is unaffected:

```
  (rails are physically at top=3.30V bot=-1.50V - the power: below is this context's SAVED
   state, which is what S pastes back)
```

### 7.2 The script offer

After a **completed** guided build the companion script is **optional**:
`SCRIPT offer=<path>`, an encoder/terminal prompt (`y`/click = run, 15 s timeout =
done), then `SCRIPT action=run|skip`. Declining leaves the run file active and the
rails where the guide left them — a completed 555 keeps blinking on hardware alone.

**A build whose every step was skipped offers nothing.** It reaches DONE and
returns `COMPLETED` ("nothing left unfinished" is literally true) but nothing was
assembled, so `guideRun` also reports how many steps the session committed and zero
commits prints `  (nothing was built - no script offer)` with **no `SCRIPT` lines
at all** — distinguishable from `offer=none`. The run file is still saved;
persistence is unconditional, only the script is suppressed. The three no-session
results are unaffected (they are plain context loads, §7.1).

### 7.3 The measured value on the panel

`STEP_RESULT` renders through `ReadingDisplay::show(headName, primaryRow, ck.val,
nullptr, "ok"|"FAIL")` on pass **and** fail. `headName` is `"<part> <value>"` for a
place step (`RG 1k`), else the check name. `primaryRow` is `st.target` when > 0,
else the first resolved pin node of the step's part, else `st.n1`, else −1
(name-only layout). It is **gated on the check having measured something** —
`check: none` synthesises a PASS that never touched the hardware, and
SKIPPED/UNSUPPORTED refused to run; without the gate every note step would show a
fake reading *and* stall. On a pass there is a **900 ms interruptible hold**
(pumping `serviceInner()`, broken by any guide key, honouring hold/`q` as a quit)
because COMMIT → STEP_ENTER repaints the panel within ~1 ms. On a fail nothing
repaints, so no hold; the long hint prints **after** the value so the panel ends on
the actionable line.

## 8. Python API

`jl_place_part(name, row, pins_json)`, `jl_remove_part(name)`, `jl_list_parts()`,
`jl_guide_progress()`. Guide *control* from Python is intentionally not offered
(both modal on core 0); HIL drives the guide over port-1 serial keys.

`list_parts()` returns
`{name, type, value, row, footprint, placed, placement, pins: {PIN: {node, connect,
class}}}`. **`placement` is not decoration**: it is the mode `partPinNode()`
resolved every `node` through, and without it a compact leg sitting in its endpoint
hole is indistinguishable from an expanded leg that happens to have landed on the
same row.

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
- `test_slot_files.py`: the path-context layer underneath (DESIGN_SLOT_FILES.md §10).
- Bench: **CodeDocs/PROJECTS_BENCH_CHECKLIST.md** is the ordered session script.
  What is structurally unreachable from a suite, and therefore lives only there:
  every **encoder** gesture (the 260 ms pend and its two race shapes, the
  double-click into ADJUST, the wheel slide) — a pend can only be armed by an
  encoder RELEASE and every serial byte clears one; every **probe pad** gesture
  (the tap precedence, the DIP `+30` mapping, tap-to-snap) — this suite has no
  probe; the **interactive prompts** (headless never prompts, by design); the
  **no-blip** scope trace; **real-part accuracy** against a DMM; and the LED
  eyeballs (overlay orientation, the sparser compact overlay). `>`/`<` and `m`/`c`
  are the wheel's and the pads' serial twins running the same handlers, so the
  *logic* is covered — the input routing is not.

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
