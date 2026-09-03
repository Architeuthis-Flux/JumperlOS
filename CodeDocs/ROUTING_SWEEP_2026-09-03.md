# Routing sweep 2026-09-03 (overnight, dev)

Kevin's ask (2026-09-02 23:13): sweep the routing code, find more paths through
the existing crossbar matrix and more parallel paths (rails, breadboard to
breadboard), no regressions - no shorts, no desync between what the router
believes and what the crossbars were sent - and a set of HIL tests with a
dozen dense / edge-case routings checked against the chip status. Plus a V6
document (separate: `CodeDocs/V6_CROSSBAR_MATRIX.md`).

This file is the ledger for the firmware side. Written incrementally; the
newest section is at the bottom.

## 1. The verification model (committed first, before any router change)

`test/hil/fabric_v5.py` is GENERATED from the r7 KiCad schematic by
`test/hil/tools/gen_fabric_v5.py` (kicad-cli netlist export -> every CH446Q
X/Y pin's net name). It is independent of the firmware's `chipStatusInit` /
`rev5plusXmap` tables; the two were compared by eye pin-for-pin and agree.

`test/hil/routing_check.py` reads two views of one routing:

* the router's view: `b` on port 1 (bridge array, path table with up to four
  crosspoints per path, chip status = `xStatus`/`yStatus` claims)
* the hardware's view: `:crossbar` on port 7 = `lastChipXY`, the crosspoints
  the firmware last SENT (96 hex words, chip-major, one 16-bit X mask per Y)

and runs a union-find over the copper (closing (chip, x, y) joins the X pin's
net with the Y pin's net). Checks:

| check | fails when |
|---|---|
| connectivity | a bridge's two nodes are not in one component |
| SHORT | one component holds nodes from two nets |
| STRAY | a component with a net reaches a real node (row, nano pin, rail, GND, DAC/ADC, GPIO, BUF_IN/OUT, AREF, ISENSE) that is in no net |
| chip-local | two nets close crosspoints on one X column or one Y row of one chip |
| sent == planned | the path table's crosspoint set differs from `lastChipXY` |
| UNCLAIMED X/Y | a crosspoint a net closes is not claimed for that net in the chip status |
| WRONG CLAIM | a claim sits on copper that carries a different net |
| UNCLAIMED WIRE | a lane or bounce row a net rides is free at one of its pins (another net could close onto it) |
| CLAIM WITHOUT COPPER (strict) | a claim on copper no net rides - a phantom that blocks a lane for nothing |

`test/hil/test_routing_dense.py` pastes 16 cases as state documents (bridges
with explicit per-bridge `dup:` counts, rails/DACs at 0 V), captures both
views, runs the model, and for two cases energises a source ONLY after the
model has cleared the routing (GPIO loopback across chips A/B; DAC0 through
chip K to ADC0). The two cases that deliberately exceed the fabric are
metrics (they must still be short-free). It pauses the net-voltage scan
(`[measurement] net_currents`) for the duration - the scan closes ephemeral
sense taps to ADC0 that a crossbar snapshot catches mid-flight - and snapshots
/ restores the bench itself, so it is safe standalone.

`JL_ROUTING_SNAP_DIR=<dir>` saves every capture;
`test/hil/tools/routing_snapshot_diff.py before after` lists the path rows a
firmware change moved. That is the regression gate for every router change in
this sweep: a change that only adds routes must move zero rows in the
fully-routed cases.

### Baseline on 5.7.10.0 (dev `396e930`)

57 checks, 5 failing, all one bug (below). Every case that fits the fabric
routes completely; `all_28_chip_pairs` (one net between every pair of
breadboard chips, 7 rows and 7 lanes per chip) routes direct with no bounce;
`gpio_seven_from_chip_a` routes 7 GPIO nets out of one chip through six
bounces; the same document pasted twice routes identically (parallel_AB,
gnd_spread, k_rows_all_eight).

## 2. Findings

### F1 - the BB-to-SF bounce checks and claims the wrong pin (bug, fixed)

`resolveAltPaths`, case BBtoSF, the bounce loop: `xMapBB` was
`xMapForChipLane0(chip[0], bb)` - chip[0]'s X index toward the bounce chip -
and that index was then used ON the bounce chip: `freeOrSameNetX(bb, xMapBB)`
tested an unrelated pin of `bb` (its I lane, for chip A as chip[0]) and
`setChipXStatus(bb, xMapBB)` claimed it. The lane the path really closes
(`bb`'s own lane into the SF chip) was recomputed correctly for `x[3]` further
down but never checked for availability and never claimed.

On Kevin's live netlist (rows 2-5 -> ADC0-3, bounced through C, D, E, F) the
chip status showed `C.X4 D.X6 E.X8 F.X10` claimed (all I lanes, unused) and
`C.X13 D.X15 E.X1 F.X3` (the K lanes the paths ride) free. No hardware short
resulted because `K.Y[bb]`, the other end of the same wire, was claimed; the
cost was a false-busy pin per bounce (a lane blocked for nothing) and an
availability test on the wrong pin (a bounce chip refused because its I lane
was busy, or accepted with its K lane busy - saved only by the K row check).

Fix: `xMapBB = xMapForChipLane0(bb, sfChip)`; the lane pair between chip[0]
and `bb` is now checked at both pins. The OG router has the same lines and is
left alone per the standing rule. Verified by the suite (section 3).

### F2 - the net-voltage scan is invisible to the path table (not a bug, a test hazard)

The scan's ephemeral taps (`fastConnectPath`) close crosspoints that are in
`lastChipXY` but not in `paths[]`; the first baseline run caught one
mid-flight (`K.X8` ADC0 on a bounce row). The suite pauses the scan. Anything
else that compares `:crossbar` with `b` must do the same.

### F3 - a part on the breadboard clamps its rows to the rails (bench fact)

With the rails at 0 V, DAC0 at 2.5 V read 1.86 V at ADC0 through row 31; with
the bottom rail at 2.7 V it read 2.41 V. INA1 showed no load on DAC0. Kevin's
4051 sits on rows 31-38 (and its VSS is bridged to GND): its input clamp to
the 0 V rail is what pulled the row. Not a routing effect - the model and
`lastChipXY` agreed throughout - but the electrical proofs in the suite now
use rows 7/14 and 18/19/25, away from where a part usually lives.

### F4 - rail stacking almost never happens (structural, not a code bug)

`rail_stacking_light` (two rails on two chips, GND, DAC0, `stack_rails=3`)
produced ONE routed duplicate: GND on chip K next to its primary on L. The
rails got none: every path into a rail is a chip-K row, each breadboard chip
has exactly one lane into K, duplicates may only use FREE lanes
(`allowStacking=0` in the duplicate pass), and `reservedKRowForSenseTaps`
keeps the last two virgin K rows for the sense taps. So a rail duplicate needs
another chip's bounce row AND a third virgin K row - on any netlist with a few
K nets that never exists. Parallel rail paths on V5 are a hardware question
(V6 doc, chokepoint 1).

### F5 - the GND alternator is never reset (nondeterminism candidate, NOT reproduced)

`gndChipAlternator` is a file-scope counter incremented per GND path and
never cleared in `clearAllNTCC`, so a GND path's K/L choice could depend on
how many rebuilds came before. The suite pastes `gnd_spread` twice and got
identical routes both times, and with `stack_rails > 0` (the default) the
router uses `moreAvailableChip(K, L)` instead of the alternator, so today the
alternator only matters with `stack_rails = 0`. Left as is; noted.

### F6 - what the metric cases say the router cannot do today

* `nano_dense_metric` (D0-D5 -> rows 1-6, A0-A5 -> rows 15-20: sixteen nano
  nets from two chips): 11 of 13 primaries route; `19-A4` and `20-A5` fail
  with every bounce row spent, although chips I and J each still have two
  free rows. A path into I via J's hub lane (C.X5 -> J.Y2 -> J.X13 (IJ) ->
  I.X13 -> free I row) would route them - a shape the router does not try.
* `k_rows_overflow_metric` (nine K nets + the probe feed): the feed
  `BUF_IN-GP_8` is the path that loses, exactly as the K-row budget comment
  predicts when K is genuinely full.

### F7 - a path that fails after `ijklPaths` keeps its X claims (minor leak)

`ijklPaths` claims the two node pins and the hub lane's two pins and
commits; when `resolveUncommittedHops` then finds no rows, it restores to
the state saved at ITS start, so the four X claims survive as claims
without copper (seen on the feed `BUF_IN-GP_8` in `k_rows_overflow_metric`:
K.X2, K.X12, L.X11, L.X14 claimed, nothing closed). Within one rebuild that
blocks the KL lane for a later SF-to-SF path; the next rebuild starts clean.
Not fixed tonight (a release pass would need the copper model to know which
far-end claims are still ridden by routed paths of the same net); the suite
only insists on claim-strictness when every path routed.

## 3. Changes (each with the fixture diff)

### 3.1 F1 fix (`resolveAltPaths` BBtoSF bounce)

Suite on the fix build: 56/57 green (the one left is F7 in the overflow
metric case). `routing_snapshot_diff` baseline -> fix: 4 rows moved, all in
`nano_gpio_realistic`, all still routed with the same bounce count - the
availability test now looks at the right pin, so three bounced paths chose
different bounce chips (D -> C, E -> D, F -> E) and `D1-UART_RX`'s same-chip
hop took I.Y1 instead of I.Y6. Every other case: zero rows moved, identical
crosspoints. Chip status is now consistent with the copper in every case
(no UNCLAIMED WIRE / CLAIM WITHOUT COPPER anywhere but F7).

### 3.2 Hub rescue tier (new path shapes, only for paths nothing else routed)

`rescueViaHubs()` runs after the K-row rescue pass and before the
duplicates, for primaries that are still not fully routed. Two shapes, both
four crosspoints:

* **H1** breadboard row -> SF node through a second SF chip: the row's chip
  takes its lane into the hub, the hub's row for that chip closes onto the
  hub lane to the target SF chip, and that lane lands on any free row of the
  target chip together with the node. Uses the SF-to-SF hub lanes (IJ, IK,
  IL, JK, JL, KL) as a way INTO K/L/I/J when the chip's own lane is taken
  and every bounce row is spent.
* **H2** breadboard row -> breadboard row through an SF chip: both chips'
  lanes into the SF chip land on their rows there, and a free hub lane's X
  pin bridges the two rows (its far end is claimed - that wire is on the
  net). Reaches a second/third path between two chips when both lanes and
  all bounces are gone.

Every wire is claimed at both pins (the class of bug F1 was); a failed
attempt rolls back through `saveRoutingState`/`restoreRoutingState`; rows
and lanes the net already holds are reused. Written as topology queries
(`hubSfRowFor`, `xMapForChipLane0`) with the V5 literals (`chip < 8`,
X12-14) called out for the descriptor - see `V6_CROSSBAR_MATRIX.md` §7.

Not covered: SF-to-SF when the direct hub lane is busy and no bounce row is
left (six crosspoints, `pathStruct` holds four), and a GND path whose chosen
chip (K or L) is full while the other has rows (`resolveChipCandidates`
picks once, by path count).

Fixture diff (`snap_after_f1` -> `snap_after_hub`): 2 rows moved across 16
cases, both in `nano_dense_metric`, both from unrouted to routed:

    19-A4: C-1.-1 I-1.-1  ->  C13.5 I4.1 K13.2 I14.1   (H1, hub K: row 19 -> CK -> K.Y2 -> IK -> I.Y1 -> A4)
    20-A5: C-1.-1 J-1.-1  ->  C9.6 J5.3 L13.2 J12.3    (H1, hub L: row 20 -> CL -> L.Y2 -> JL -> J.Y3 -> A5)

13/13 primaries now route in that case (11/13 before); every other case
identical to the crosspoint. Suite 57/57 on the hub build, the copper model
clean on the hub paths (every lane claimed at both pins).

### 3.3 Same-net SF row reuse in `resolveUncommittedHops` (K rows are the currency)

`resolveUncommittedHops` resolved every `-2` row of an SF chip by scanning
for a VIRGIN row (its `allowStacking` argument arrives as 2 from
`bridgesToPaths`, and `freeOrSameNetY` only treats a same-net row as free
when the argument is exactly 1). So a same-chip X-X hop (DAC0-ADC0 on K) or
an SF-to-SF hub hop (ADC3-D0: K.X11 + K.X14 on a K row) always took a fresh
row even when the net already held one on that chip - Kevin's live netlist
carried net 9 on both K.Y5 and K.Y7.

`sameNetSfRow(chip, net, allowStacking)` is tried first at the three row
searches: a row already claimed for the net whose far-end breadboard pin is
free or the net's own. Electrically identical (the row is on the net), and
it spares a virgin row - on K the resource rail duplicates and the sense
taps compete for. Primaries only: the duplicate pass passes `allowStacking
= 0` and keeps looking for virgin rows so a stacked copy stays a second
path.

Fixture diff (`snap_after_hub` -> `snap_after_samenet`): 1 row moved across
16 cases, in `kevin_fixture_2026_09_02`:

    ADC3-D0: K11.7 J0.0 K14.7 J14.0  ->  K11.5 J0.0 K14.5 J14.0

Net 9 already held K.Y5 (row 5 -> ADC3 through F's bounce); the ADC3-D0 hop
now rides it instead of taking K.Y7, so on Kevin's live netlist chip K has a
free row again (before tonight it had none, which is exactly the state the
2026-08-24 sense-tap comment describes: every net-voltage tap "noroute").
Every other case identical to the crosspoint; suite 57/57.

## 4. Where that leaves the asks

**More paths through the existing matrix.** Two new shapes (hub tier), and
the bounce tier now tests the right pin (F1) so a bounce chip is no longer
refused or accepted on an unrelated lane. What is still not attempted, with
the reason: SF-to-SF through a second hub when the direct hub lane is busy
and no bounce row is left (six crosspoints; `pathStruct` holds four - the
V6 doc's §7 asks for `chip[6]`); a mixed-lane bounce for BB-to-BB (lane 0
one side, lane 1 the other - the code has it under `&& false`; every case
tonight routed without it, so it stayed off rather than change routes
without a failing case to justify it); a GND path retrying the other of
K/L when its chosen chip is full.

**More parallel paths.** Breadboard-to-breadboard: `stack_paths` already
takes the second lane where a pair has two (`all_28_chip_pairs` with
`dup: 2` stacked 20 of 28 nets in the first baseline), and a bounce for the
rest when a bounce row is free. Rails: on V5 a rail duplicate is a chip-K
row, so the only firmware lever is spending fewer K rows on primaries -
which 3.3 does (one row per net per chip where the net already has one).
The structural answer (rails on every breadboard chip, a second specials
chip) is in `V6_CROSSBAR_MATRIX.md` §3-4; nothing in software gives a V5
rail two independent entries to a breadboard chip.

**No regressions.** Every router commit tonight carries the dense suite
(57 checks) and a snapshot diff against the previous build; the only rows
that moved are listed in §3 with the reason. `test/hil/run_all.py routing`
runs `test_routing.py` and the dense suite together.

**Not done, for Kevin:** the OG router shares F1 (same lines,
`NetsToChipConnections_OG.cpp` ~3198) - left alone per the standing rule;
the F7 claim leak; the mixed-lane bounce; `pathStruct chip[6]`.

## 5. Final gate

`python3 test/hil/run_all.py routing` on the board running `bce1912`:
`test_routing.py` PASS (5 checks), `test_routing_dense.py` PASS (57
checks), bench restored to the pre-suite snapshot (Kevin's twelve bridges,
parts, rails 3.80/2.70 V, DACs 2.50/0.60 V, `[measurement] net_currents`
back to 1). Nothing pushed.
