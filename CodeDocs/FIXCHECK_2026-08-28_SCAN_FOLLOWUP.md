# FIXCHECK 2026-08-28 (evening) — the Auto scan's three real bugs, from Kevin's session log

Kevin ran the scan on the standing rig (7447 + 2N3906 + 7-seg + copper demo
wiring) and it came back wrong in three ways. All three are fixed here, each
one measured on the bench BEFORE the fix rather than reasoned about. Plus the
wording pass he asked for.

| Symptom he saw | Root cause | Where |
|---|---|---|
| `identify fp=GGGGGG--GGGBGGGB tried=0 pass=none` | the rails the clamp map named were **the VCC pin and a signal pin**, so no record could orient | `partsResolveChipRails` (new), `PartsApp.cpp` |
| `rows 7-8: RESISTOR 2 (split from the span)` inside the chip | the 50 µA leg of the junction-vs-resistor ratio law false-trips in this fabric; the fallback then reported kΩ in an Ω field | `PartClassify.cpp`, span walk in `PartsApp.cpp` |
| `PARTID test part=C12 … status=-7` → "machinery is busy" | crossbar lane exhaustion, not busy: **every ADC/DAC/GND/rail is on chip K** and each breadboard chip has ONE direct lane to it | `PartMeasure.cpp`, `PartsApp.cpp` |
| interrogation-flavoured narration + `PARTSCAN` prefix | copy | `PartsApp.cpp`, `PartMeasure.cpp` |
| (found while reading his board) net names on the wrong nets | `findNodeInNet`'s GPIO fallback compares a **node** against **net numbers** | `PartPlacement.cpp` |

## 1. The rails, and why the fingerprint string can't referee them

Bench run on the placed 7447 (base 33, width 8, VCC row 40, GND row 3), three
fingerprints of the same chip with three different rail claims:

| rails claimed | fp string | per-pin Vf to the claimed "gnd" | match |
|---|---|---|---|
| gnd 3, vdd 40 (**true**) | `GGGGGGG-GGGBGGG-` | **0.67–0.69 V** | `7447:1,74595:13` |
| gnd 40, vdd 39 (the scan's guess) | `GGGGGG--GGGBGGGB` | 1.54–1.74 V | `7447:3` |
| gnd 40, vdd 3 (**swapped**) | `GGGGGGG-GGGBGGG-` | 1.54–1.73 V | `7447:1,74595:13` |

Two things fall out of row 3 of that table, and they are the whole fix:

- **The swap is invisible to the string.** `gnd=40/vdd=3` prints the byte-identical
  fingerprint and the identical match list. Worse, it also passes
  `partsChipOrientFromRails` (a 180° DIP rotation swaps pin 8 and pin 16, so
  the geometry fits perfectly) — the vector runner would have driven GND onto
  the chip's VCC pin and 5 V onto its GND pin.
- **The drop separates them cleanly.** One substrate diode is 0.67–0.69 V at
  1 mA; driving the real VCC row above a pin walks a junction CHAIN and reads
  1.54–1.74 V. A 2.5× separation on every one of the 14 signal pins.

So `partsIdentifyChip` no longer trusts what it was handed. It collects the
candidate `(gnd, vdd)` pairs the partdb's same-footprint records imply — each
record, both orientations — plus the scan's own guess, probes **two** signal
pins per pair with the existing `partScanClampFingerprint`, and keeps the pair
whose GND side reads like ONE junction (0.40–1.10 V). Nothing in band ⇒ no
identify: a generic dipN is honest, a reverse-fed chip is not. ~3 s for the
three candidates a dip16 generates.

Consequences, on this rig's numbers: `(40,39)` scores 0, `(40,3)` scores 0,
`(3,40)` scores 2 and wins, the fingerprint comes back `GGGGGGG-GGGBGGG-`
(1 mismatch vs the record), and the 7447's vector set runs in the rotated
orientation the rails prove — which the previous session already measured as
`7447(r):pass`.

Two follow-on changes fall out of having a resolver:

- the scan's confirm pass offers "identify?" even when the span's clamp map
  named no rails at all (it used to require `chipGnd > 0`),
- Parts > Test no longer dead-ends on "rails unclear" — the map's answer is
  a hint now, not a gate.

The `identify` line reports the rails it actually used:
`identify gnd=3 vdd=40 fp=… tried=2 pass=7447(r)`.

## 2. "RESISTOR 2" between two of the chip's own pins

Measured, chip unpowered (both rail feeds lifted):

```
part_identify(7, 8) -> LED 3.13V  screen=2.198V@1.045mA fwd / 5.473V@-0.015mA rev
part_identify(6, 7) -> LED 2.87V  screen=2.187V@1.075mA fwd / 5.494V@-0.015mA rev
```

One-way conduction, ~2.2 V at 1 mA. That is a junction chain
(substrate→collector→base→emitter), exactly what `PartMeasure.h` documents.
Note the verdict is **not stable** — the same pair that the scan called
`RESISTOR 2` reads `LED` here, because the verdict hangs on `vfLo`, the 0.05 mA
point of the ratio law:

- The FIXCHECK from earlier today already recorded that a 50 µA target "lives
  inside this fabric's transient floor" — fixture-build charge draining
  through the shunt trips the servo at the first DAC step. That is why
  `clampProbeDir` dropped the two-current law. `identifyTwoLead` still used it.
- A phantom trip reports a tiny `vfLo`; `vfHi > 8 × vfLo` then reads "scales
  like a resistor" and the fallback fires.
- That fallback set `r.value = vf / 1.0e-3f / 1000.0f` — **kΩ in a field every
  other producer and `formatOhms` fill in Ω**. A 1.7 V-at-1 mA chain printed
  as `2`.

Three changes, smallest first:

1. the ratio law only speaks when its low point is a real measurement
   (`vfLo > 0.25 V`, `clampProbeDir`'s own tie/junction boundary); below that
   the conservative verdict stands (junction-like).
2. the `!junctionLike` branch no longer claims RESISTOR at all. No
   two-terminal resistor conducts 1 mA one way and refuses the other, so the
   verdict is `UNKNOWN` + the drop, and the unit bug goes with it.
3. defence in depth: the span walk's chip-clamp referee
   (`partsDiodeIsChipClamp`, the star test) now also referees RESISTOR and
   SHORT splits, not just diode-family ones. A resistive verdict between two
   pins of a chip-wide span is in exactly the same trap, and this is the
   guard that stops the phantom stealing two of the chip's legs.

## 3. "measurement machinery is busy" for a board that will never get less busy

`PARTID test part=C12 … Couldn't find a path for 42 to ADC_1 / REFUSED 42-111
/ status=-7`. C12 is an `axial2` capacitor at base row 12, so its legs are
rows 12 and **42** (`nodeForPin`: axial2 straddles the ravine, pin 2 =
baseRow+30) — that part of the log is correct, not an off-by-30.

The refusal is topology. From `rev5plusXmap`: `ADC0-3`, `DAC0`, `DAC1`, `GND`,
`TOP_RAIL` and `BOTTOM_RAIL` **all live on chip K**, and chip F (rows 38-44)
has exactly one x-lane to chip K (x3), plus bounce lanes through I/J/L. The
7447's `VCC → BOTTOM_RAIL` bridge sits on row 40 — chip F — and rail paths are
stacked (`stackRails: 3`). Row 42's sense leg had nothing left.

Proof, on the live board:

```
part_identify(12, 42)                        -> status=-7          (as Kevin saw)
disconnect(40, BOTTOM_RAIL); disconnect(3, GND)
part_identify(12, 42)                        -> CAPACITOR status=0
```

An unrelated wire two rows away was the whole problem. Fixes:

- `partScanBegin`'s lift rule moved into `liftWiringInTheWay(s, rows, nRows,
  everything)`. On a refused sense leg the session lifts **everything it is
  allowed to lift** and asks once more, then restores it all through the
  existing `partScanEnd` contract. This is the same "briefly unwire it to
  test" ruling, applied to the rows the fixture needs rather than only the
  rows the part occupies.
- the `-7` message stops lying: *"the fabric has no measurement lane to those
  rows - clear a connection near them and try again"*. `-4` also had its own
  truth to tell (*"those rows read POWERED"*) and was landing in the same
  "busy" bucket.

Retrying with a different ADC channel was considered and rejected: all four
ADCs are on chip K, so the second channel has the same problem as the first.

## 4. Net names landing on nets that have nothing to do with the part

Not reported - found in his board state while chasing §3. Every net on the
board, as the firmware had named them:

| net | nodes | name it carried | should be |
|---|---|---|---|
| 6 | 57, GPIO_1 | `7447_RBI` | `7SEG52_S1` |
| 7 | 55, GPIO_2 | `7447_BI_RBO` | `7SEG52_S2` |
| 8 | 54, GPIO_3 | `7447_LT` | `7SEG52_S3` |
| 9 | 52, GPIO_4 | `7447_C` | `7SEG52_S4` |
| 10 | 28, GPIO_5 | `7447_B` | `7SEG52_S5` |
| 11 | 27, GPIO_6 | `7SEG52_S6` | correct |
| 12 | 23, GPIO_7 | `C12_K` | `7SEG52_S7` |
| 13 | 22, GPIO_8 | `7SEG52_S8` | correct |

The pattern is exact: **net N was named after whatever part pin sits on
NODE N.** The 7447's RBI leg is row 6, BI/RBO row 7, LT row 8, C row 9, B row
10; C12's K leg is row 12. Nets 11 and 13 are right precisely because no part
pin occupies node 11 or node 13.

`partsReassertNetNames` resolves a pin's net with `findNodeInNet(node)`. When
no net holds the node (an unconnected leg - all of the above), that function
falls through to

```c
for (int i = 0; i < 10; i++) if (gpioNet[i] == node) return gpioNet[i];
```

and `gpioNet[]` holds **net numbers** (every other reader in the tree compares
it against a net). So an unconnected leg on row 6 "matches" GPIO 1's net 6 and
comes back as net 6. The existing guard - `nets[netNum].number == netNum` -
cannot catch it, because the value returned IS a live net number.

Fixed at the parts-layer caller: it now asks the net whether it actually holds
the node (`netHoldsNode`, the same three lines `netHasNode` in
`Highlighting.cpp` and `netContainsNode` in `PartLabels.cpp` already carry).
The type confusion inside `findNodeInNet` is old routing code with several
other callers, so it is work-listed rather than changed here.

Bench-verified after the flash - the same board, every GPIO net named after
the segment that is actually on it:

```
{num: 6,  nodes: [GP_3, 54], name: "7SEG52_S3"}
{num: 7,  nodes: [GP_1, 57], name: "7SEG52_S1"}
{num: 8,  nodes: [GP_8, 22], name: "7SEG52_S8"}
{num: 9,  nodes: [GP_6, 27], name: "7SEG52_S6"}
{num: 10, nodes: [GP_7, 23], name: "7SEG52_S7"}
{num: 11, nodes: [GP_4, 52], name: "7SEG52_S4"}
{num: 12, nodes: [GP_5, 28], name: "7SEG52_S5"}
{num: 13, nodes: [GP_2, 55], name: "7SEG52_S2"}
```

## 5. Wording

`PARTSCAN` is gone from all 27 serial lines (21 `PartsApp.cpp`, 6
`PartMeasure.cpp`); `PARTID` / `PARTPICK` / `PARTDB` / `PARTSEL` stay. The
scan narrated like an interrogation ("interrogating rows 3-10", "asking
harder", "asking the dark rows", "row 34 answers", "walking everything that
shares row 58"); it now narrates like an instrument ("measuring rows 3-10",
"measuring each pair", "checking the unlit rows", "row 34 conducts",
"mapping everything that shares row 58"). Comments got the same pass, and the
part-replacement path's `victim` / `victimCopy` / `haveVictim` locals are now
`replaced` / `replacedCopy` / `haveReplaced`. No logic, no format specifiers,
no OLED string lengths changed.

## 6. The ground vote itself (Kevin's follow-up ask)

`partsFindClusterPower` built its junction-map triples all anchored on
`rows[0]` - `{0,1,2}`, `{0,3,4}`, `{0,5,1}` - which put that one row in SIX
pair-slots against two or three for every other row. `pick()` prefers the
row with the most `seen`, so **whatever the caller listed first won the
ground vote on evidence nobody else was allowed to gather**. The §1 case is
exactly that: the second look built its `conf[]` by walking the union
downward from row 40, so row 40 anchored everything and won.

Triples are now a ring - `{0,1,2}`, `{2,3,4}`, `{4,5,0}` - same session
count, every row in two pair-slots, no row in more.

Honest result on the bench: this does **not** make the map right about this
chip, it makes it tie. Both row 40 and row 3 come out unanimous anode with
`seen=3`, and the first-listed still breaks the tie. That is the map's real
ceiling, not a sampling artifact: on an unpowered bipolar chip VCC conducts
to every pin through the internal base resistors, so the junction map sees
*two* universal anodes and has no way to rank them - it reads presence and
direction, never the drop, and the drop is the only thing that separates
0.67 V (one substrate diode) from 1.54 V (a chain). So the print now says
"rails look like gnd 40 vdd 37" and `partsResolveChipRails` is the authority.
Re-driven on the bench after the change: the vdd guess moved 39 → 37, the
membership probe still found all six unlit pins, the union still sized dip16,
and the identify still landed `pass=7447(r)`.

## 7. Which rail a VCC pin reaches for (Kevin's follow-up ask)

It was `pin.connect = (node <= 30) ? TOP_RAIL : BOTTOM_RAIL` - the rail on
the pin's own half, geometry only. The 7447's VCC landed on row 40 and got
BOTTOM_RAIL, which was 5.00 V that evening and 3.37 V the same afternoon.

`partsRailForVcc(rec, node)` now asks the board: a rail under ~1 V is off and
proves nothing, and when the record declares a supply it prefers a rail
within 0.7 V of it. The only supply figure the partdb carries today is the
vector set's `supply` (the `5v` / `3v3` / `either` key authored per record) -
sparse but honest, so geometry stays the tie-break and a board with matching
rails places exactly as it used to. When a record wants a voltage neither
rail has, the placement says so instead of quietly bridging.

A record-level `supply:` field would cover the other ~180 records; that is a
schema change (header + generator + YAML) and is left for whoever wants it.

## 8. `findNodeInNet` fixed at the root (§4's cause, audited)

§4 guarded the one parts-layer caller and work-listed the function. The audit
of its other callers came back worse than expected, so the root is fixed too.

**Both fallback arrays hold NET NUMBERS**, verified against their writers:
`populateSpecialFunctions` does `gpioNet[0] = net` for `RP_GPIO_1` on through
`gpioNet[9] = net` for `RP_UART_RX` (`NetManager.cpp:817-876`), and
`rebuildShownReadings` does `showADCreadings[0] = pathNet` for ADC0 through
`[4]` for ADC4 (`Peripherals.cpp:1955-1969`; slots 5-7 are only ever zeroed).
Every other reader in the tree compares them against a net.

The old fallbacks compared them against the **node** being looked up, which
means:

- **the intended lookup could never fire.** `RP_GPIO_1` is node 131 and `ADC0`
  is 110, while `MAX_NETS` is 60 - `gpioNet[i] == node` was unreachable for
  every real peripheral node the fallback existed to serve.
- **what fired was the collision.** Ask about breadboard row 6 while
  `RP_GPIO_1` sits on net 6 and you got net 6. Rows 1-59 all overlap the net
  numbering, so this is common on any board that places parts with
  unconnected legs and routes a GPIO or ADC - not an edge case.

Both loops are keyed by node now, so the fallback answers the question it was
written for and stays silent otherwise. Callers whose behaviour improves, none
of which depended on the collision:

| Caller | Was |
|---|---|
| `PartsApp` `rowNetHas` (auto-routing power/gnd at placement) | an unconnected row could resolve to a stranger's net; if that net held GND or a rail the pin's power route was silently skipped |
| `PartLabels` `validNetForNode` (warnings, highlight masks, the unwired-pin column overlay, the `PARTPIN` line) | five unguarded uses - every unconnected part leg on a low row read as WIRED |
| `States.cpp` `reconcileAfterRebuild` (custom net names/colours) | a dead net's name migrated onto an unrelated live net |
| `PartPlacement` `partsReassertNetNames` | already belted by §4; unchanged |

The last one is worth a note: the migration does still happen on the pre-fix
firmware, it is just **invisible** on this bench, because
`partsReassertNetNames` had already registered its own `customNames` entry for
that net and `getNetName` returns the first match. Duplicate entries per net
are a latent smell of their own - not touched here.

`Graphics.cpp:2947` would have passed a net number as a node; it is commented
out. `PartLabels.cpp`'s comment claiming the fallbacks "return NODE values"
was wrong and is corrected.

## Verification

- Both targets (`jumperless_v5`, `jumperless_og`) build at every step.
- `test/hil/test_ic_identify.py` gained five checks, each aimed at a bug
  above. Run against the **pre-fix** firmware on the rig it now reports
  `FAIL (1/40 checks failed)`, and the one failure is exactly Kevin's:

  ```
  FAIL: rows 12/42 measurable beside the row-40 rail feed and a full
        GPIO bank (status=-7)
  ```

  The other 39 pass pre-fix, which is the honest reading: three of the new
  checks assert *physics* (`0.4-1.1 V` for the true ground row, a chain for
  its swap, identical fp strings either way) and physics does not change with
  firmware - they are there so a future edit cannot quietly break the
  resolver's premise.
- First draft of that lane check passed pre-fix and had to be strengthened
  twice, both times because the test was too kind:
  - it ran *after* `free_routable_gpios()`, and lifting the 7-seg's eight
    GPIO bridges frees the chip-L bounce lanes the conflict depends on. The
    refusal needs the direct F→K lane held by the row-40 rail feed **and**
    chip L full. It now runs first, on the fully-wired board.
  - the phantom-resistor check picked the first adjacent pin pair (rows 4/5),
    which reads EMPTY on this die - proving nothing. It now walks pairs until
    one actually conducts (rows 6/7 on this chip: one-way, 2.88 V).
- Board state captured before any bench work and byte-compared after: 11
  bridges, same nodes, same duplicate counts, same node order, 4 parts.

### Flashed and driven end to end

`pio run -e jumperless_v5 -t upload`, then the Auto Scan itself driven over
port 1 (its confirm prompts read `Serial`, so `y`/`n` work; the app was
launched with MicroPython `run_app('Auto Scan')` from port 5, and one stray
byte bails out of the picker so nothing is placed). The 7447 record was
removed first - the scan correctly reports placed parts as `rows 3-10: 7447
(placed)` and never re-identifies them - and re-placed afterwards.

```
identify? dip16 chip rows 33-40  powers it briefly  (y/n)
  rails? gnd 40 vdd 39: 1.54/1.54 V - not one junction
  rails? gnd 40 vdd 3: 1.54/1.54 V - not one junction
  rails? gnd 3 vdd 40: 0.67/0.67 V - one junction
  rails? gnd 40 vdd 33: 1.54/1.53 V - not one junction
  rails? gnd 3 vdd 10: 0.68/0.68 V - one junction
  vectors: 74595 ruled out by the clamp fingerprint
  vectors: trying 7447 (rotated 180)
  vectors: powered at 5V (rail), icc 9.0 mA
identify gnd=3 vdd=40 fp=GGGGGGG-GGGBGGG- tried=1 pass=7447(r)

PARTPICK level=chip n=2
  Which?: 7447
```

`tried=0 pass=none` became `tried=1 pass=7447(r)` with the 7447 preselected in
the picker. Both wrong pairs were rejected on the drop, the Tier-1 gate ruled
the 74595 out before powering anything, and the vector run drew 9.0 mA at 5 V.

### What is verified how

| Fix | Evidence |
|---|---|
| §1 rail resolver | **driven end to end on hardware** (above); all five candidate pairs measured |
| §2 phantom RESISTOR | measured (both pin pairs, both directions, with the drops); guard asserted by HIL |
| §3 blocked lane | measured before AND after removing the blocking wire; HIL check fails pre-fix, passes post-fix |
| §4 net names | root cause derived from the board's own net table; **fix confirmed on hardware** - all 8 GPIO nets now correct |
| §5 wording | compile + read + the driven scan's own output above |
| §6 ground vote | re-driven on the bench: sampling changed, outcome unchanged, identify still lands |
| §7 rail choice | **compile and reasoning only** - `partsCommitPlacement` is only reachable through the picker, which needs an encoder click |
| §8 findNodeInNet root | index→node mapping verified against both writers; both targets build; flashed and regression-checked (net names, HIL, board state). **Not directly observable over serial** - its consumers are the probe-tap `PARTPIN` line, LED/OLED warnings and the picker's auto-route, none of them scriptable |

Post-flash HIL after every change: `ic_identify: PASS (40 checks)`, 11 bridges
byte-identical, four parts, all eight GPIO net names correct.

Two ceilings the bench run exposed, neither fixed:

- **Five candidate pairs, and two of them tie.** `gnd 3 vdd 40` and
  `gnd 3 vdd 10` both score 2 - they share the true ground row, and the
  vdd side has no discriminator on this chip (nothing conducts pin-above-VCC,
  and two signal pins read open the same way). `(3,40)` won because the
  partdb lists the 7447 before whichever dip16 record puts power on pins 1/8.
  A wrong tie-break here is *safe* - the record's own orientation check
  rejects the pair and the identify reports `tried=0` without powering
  anything - but it would look exactly like the bug that started this, so it
  wants a real tie-break (pairs on opposite sides of the package, or trying
  each tied pair in turn).
- The scan skips placed parts entirely, so re-identifying a chip you have
  already named means removing the record first. That is defensible, but
  Parts > Test only offers re-assignment for *generic* `IC*` records, so
  there is no in-UI way to say "this named part is actually something else".

### Honest ceilings

- The phantom-RESISTOR HIL check is a **guard, not a reproducer**: the pre-fix
  verdict on a conducting pin pair is a coin flip (the scan called rows 7/8
  `RESISTOR 2`; the same pair read `LED` on three later runs) because it hangs
  on a servo trip that is itself intermittent. The assertion is right; its
  trigger is probabilistic.
- `findNodeInNet` is fixed at the root now (§8), but its consumers are all
  unscriptable, so the fix rests on the mapping verification plus regression
  checks rather than on watching a symptom disappear. The `PARTPIN row=N …
  net=` line is the cheapest human check: tap an unconnected part leg on a low
  row with the probe and it should read `net=-1`.
- Duplicate `customNames[]` entries per net (§8) let a migrated custom name
  hide behind the parts auto-name. Harmless today, confusing later.
- `MAX_NETS` is 60 and breadboard rows are 1-60, so node and net number spaces
  overlap almost completely. Anything else in the routing layer that takes a
  bare `int` for "a node or a net" is one typo from the same class of bug.
- `partsFindClusterPower` can only ever TIE between GND and VCC on an
  unpowered bipolar chip (§6). The sampling bias is gone; the physics ceiling
  is not, and it is the resolver's job now.
- `identifyTwoLead`'s `UNKNOWN` + drop verdict is honest but thin: a
  diode-plus-series-resistor leg (an LED module) now reports UNKNOWN where it
  used to report a fictional resistance. Naming that shape needs a real
  two-point fit, not a ratio test.
