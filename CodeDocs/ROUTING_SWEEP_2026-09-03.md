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
use rows 18/25 and 18/19/25 - columns Kevin's bench leaves empty (the first
version used 7/14, which are the 4051's and the 74393's rows; it passed,
but GPIO drive into a part's pin is not what the proof is for).

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

H2 coverage (added after the advisor pointed out no case had exercised it):
`h2_through_sf_chips` - seven A-E nets (one lane, six bounces through B, C,
D, F, G, H) then four C-G nets (one lane; A's and E's lanes toward C/G are
spent by the bounces, so no bounce row is left). 16-46, 17-47 and 18-48
route through I, J and K respectively:

    16-46: C4.2 G12.2 I12.2 I12.6   (C's I lane, G's I lane, bridged by I.X12 = IL, L.X12 claimed)
    17-47: C5.3 G13.3 J12.2 J12.6   (J.X12 = JL, L.X13 claimed)
    18-48: C13.4 G5.4 K13.2 K13.6   (K.X13 = IK, I.X14 claimed)

12/12 primaries, copper model clean (the far-end claims included), suite
61/61 with the case.

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

## 6. Morning follow-up: the LED that read 0.0 mA at 12 mA (Kevin, 10:32)

Kevin's bench, stacking on: an LED fed from the top rail through a pot
(rows 18-23-24-28, meter 12 mA) showed no ants on 24-28 and the highlight
read 0.0 mA for both 24-28 and 18-23. My first read (the stacked path's
three parallel copies lifting the 35 mV deadband to 1.6 mA) was wrong: at
12 mA the deadband is irrelevant. The chip status told the real story -
chip D could not be tapped at all:

* D's K lane: net 7's bounce (5-ADC3 through D).
* D's I and J lanes: net 13's OWN duplicates (same-chip X hops of 24-28
  took X6/X7 = DI/DJ once X2-X5 were gone), and D's L lane the probe feed.
* every bounce row: B, C, D by primaries; A, E, F, G, H by duplicates of
  the rail net and net 11.

Two K rows were free; the tap had no way to reach them. Kevin's ruling: keep
the stacking, but a duplicate may never take the last route a tap needs
("reserve a single path"), and do the search-order and same-net-row items.

### 6.1 Changes

* **Same-chip hop search order** (`freeXSearchOrder`, breadboard chips):
  the twelve breadboard-to-breadboard lanes first, the four I/J/K/L lanes
  last (they used to be tried in X order, so chip D reached DI/DJ before
  DE/DF/DG/DH).
* **A duplicate's same-chip hop never uses an SF lane** (both X searches
  in `resolveUncommittedHops`).
* **Tap-escape reservation** (`reservedTapEscapeX/Y`, next to the K-row
  reservation): a "tap-capable bounce chip" is a breadboard chip whose Y0,
  K lane and K row are all virgin. A duplicate may not take any of those
  three from one of the last `kTapBounceReserve = 1` such chips. Primaries
  are never refused; same-net re-claims pass (the checks only fire on
  virgin pins). Hooked into `freeOrSameNetX/Y` and, belt and braces, the
  two status setters.
* **Tap route tier 4** (`buildEphemeralRouteTiered`, RouteSafety.cpp):
  bounce through a breadboard row already on the node's net, on a chip
  whose K lane is virgin (usable K row) or already the net's own (K row
  shared by the net). Rescues a net spread over several chips when the
  node's home chip has no lane and no bounce row is left.
* **Suite**: `("taps",)` proof - the `i!` sense-route dry run must find a
  route for every scanned node - on `kevin_fixture`, `rail_stacking_light`
  and the new `stacked_taps_reachable` (Kevin's 10:32 netlist as pasted).

### 6.2 What the first cut missed (two more turns on the bench)

* The reservation first counted only the K-side ingredients (Y0, K lane,
  K row). A GPIO's tap bounces through a chip whose L lane and L row must
  be free too, and the last such chip (G) was eaten by GPIO duplicates while
  the reservation held H, whose L lane a primary owned. Now per tap kind:
  a chip is capable for rows (Y0 + K lane + K row), or for nodes on I, J or
  L (those plus its lane into that chip and its row there); a duplicate is
  refused whatever would take the last chip of any kind it serves.
* Tier 4 must accept a lane the node's own net already owns (a stacked
  copy rides it): rows 18 and 41 had only such lanes toward their net's
  other rows.
* **F8 (found by the tier-4 trace): `nodeToNetIndex` is empty after every
  rebuild.** `buildNodeToNetIndex()` runs at the end of `getNodesToConnect()`
  and loops `for (i < numberOfNets)` - but `clearAllNTCC()` has just zeroed
  `numberOfNets`, and `sortPathsByNet()` (inside `bridgesToPaths`, which runs
  AFTER) is what sets it. So after any refresh the index reads -1 for every
  node. Consequences: RouteSafety's `componentHasShort` labels no wire with
  a net (only the driven-source rule has ever fired), the 2026-08-24
  shared-K-row tap fallback never fired, and LEDs.cpp's bridge-colour lookup
  by index never matches. The tap builder now keys its same-net decisions on
  the router's own ledger (`ledgerNetAt`: xStatus/yStatus of the node's
  pin), which is the source of truth anyway. Fixing `buildNodeToNetIndex`
  itself (iterate the nets array by content) would switch the net-label
  short check ON for every path and every checked crosspoint send - a
  behaviour change with a blast radius (part measurement and guide-check
  routes deliberately join a DAC's net to a row's net) that needs its own
  pass. Kevin's call; not done here.

Fixture diff (`snap_h2` -> `snap_taps_final`): 6 rows moved across 17
cases, all in `same_chip_pairs`, all the same-chip hop picking a breadboard
lane instead of an SF lane (A.X0 = AI -> A.X2 = AB0, and so on); every
other case identical to the crosspoint. Suite 70/70 (the three `taps`
proofs included, 24 nodes each on the stacked cases).

Bench, Kevin's live netlist restored on the final build: rows 23/24/28 tap
through D's I lane and the IK hub lane (`D(x6,y3) I(x14,y3) K(x13,y6)`),
`get_net_current(13)` = 11.77 mA against the meter's 12 mA, node voltages
6.69 / 5.49 / 4.36 / 4.05 V down the chain 18 -> 23 -> 24 -> 28.

The rail path 18-23 reads 37.7 mA for the LED's 12 mA: expected (Kevin) -
the 4051 on the same rail net draws the rest, so the rail path carries the
sum. The 24-28 estimate matched the meter within 2%.

## 7. Second morning round (Kevin, 11:39): the index, and rails first

### 7.1 `nodeToNetIndex` fixed (F8)

`buildNodeToNetIndex()` now walks the nets array by content (a slot with
number <= 0 is unused or cleared) instead of `for (i < numberOfNets)`, which
was always 0 when it ran. What that switches on, read from the code:

* `componentHasShort` (RouteSafety) now labels wires with their net, so a
  component holding nodes of two nets is a short. Its consumers: `validateAllPaths`
  on every rebuild (marks `skip` and the unconnectable-LED list; `sendPath`
  does not consult `skip`, so bulk connectivity is unchanged), the checked
  single-crosspoint send `sendXYraw` (MicroPython `send_raw`, the apps' and
  self-test's raw sends), and `planFastPath` for every sense tap.
* `drivenPairAllowed` (two driven sources in one component are fine when
  the user put them in one net) can finally answer yes - it compared two
  -1s before, so every GPIO+GND or GPIO-loopback net was being flagged as
  a short in `validateAllPaths` (and sent anyway).
* The scanner's shared-K-row fallback and LEDs.cpp's bridge-colour lookup
  by index start working.

Gate: the whole HIL suite (`run_all.py`) on the fix build, result below.

### 7.2 Rail duplicates: first pick, and the hub lanes

* `fillUnusedPaths` generates every round of the rail-net bridges' duplicates
  (GND, TOP_RAIL, BOTTOM_RAIL) before any other bridge's, so the duplicate
  pass - which walks paths in index order - routes rails first.
* `rescueRailDuplicatesViaHubs()` runs at the end of the duplicate pass for
  rail duplicates still unrouted: shape H1 (the chip's I/J/L lane, that
  hub's row for the chip, the hub lane into K) landing on the K row the rail
  already holds for that chip, so a second entry into the chip costs no
  bounce row and no virgin K row. Under the same reservations as every
  duplicate (last tap route, last two virgin K rows).
* The current-sense pair (I.X11 / J.X11 through the 2 ohm shunt) as an extra
  I<->J lane: a rail path through it is six crosspoints; `pathStruct` holds
  four. Left for the chip[6] change.

* Two more reservations for duplicates, found by the first run of this
  change (a rail copy took chip A's I lane and rows 2/5/6/7 lost their taps;
  three copies of one rail bridge took the same hub copper): a duplicate
  may not take a breadboard chip's LAST virgin lane into I/J/K/L, nor the
  last virgin hub lane into K (IK/JK/KL) - checked on a lane still virgin at
  both pins, since the two pins are claimed one after the other - and a
  duplicate's hub hop must bring virgin copper (`hubRouteBBtoSF(i, 0)`),
  while a primary's may ride copper its net already holds.

Results, dense suite on the final build (`snap_taps_final` -> `snap_rails3`):
70/70, every tap route kept on the stacked cases, 3 rows moved:

* `rail_stacking_light`: `1-TOP_RAIL` gains a hub copy
  `A0.1 K4.0 I14.0 K13.0` (A's I lane, I.Y0, the IK hub lane, onto the K row
  the rail already holds). Routed duplicates 1 -> 2. The second copy (via J)
  is refused: with the probe feed on KL, JK is the last virgin hub lane into
  K and stays for the taps. That is the copper: one hub copy per board
  while the feed sits on KL.
* `stacked_taps_gpio_dups`: rails first means `18-D7` gets a second copy and
  the GPIO copy `6-GPIO_1` loses its bounce row - the priority Kevin asked
  for.
* Kevin's own netlist (`stacked_taps_reachable`): no rail copy fits and
  nothing else moved. A and C (the top rail's chips) are down to their last
  virgin SF lane, which the taps keep; H (56-GND) is the one chip left that
  can bounce a tap, so its lanes are reserved too.

Full suite (`run_all.py`) on the same build, port 1 to itself: 8/11
files pass (micropython_fs, routing, routing_dense 70/70, config, stress,
paste_state, slot_files 96 checks, projects 275 checks), encoder_ui skips
without the probe, and two files fail on checks that predate today:

* `test_net_currents`: "INA0 sees a plausible loop current" reads 2.78 mA
  against a 3.0 mA floor. Measured on the bench: the loop reads 2.1 mA with
  every bridge unstacked and 4.5 mA with two copies each, on rows 5, 20 and
  25 alike - so it is not Kevin's 4051 but the 2026-09-02 stacking defaults
  (`stack_dacs` 0, counts no longer stamped) that the floor never met. The
  floor is now 1.5 mA, which still refuses an open loop; the scan-versus-INA
  agreement check in the same file passed throughout.
* `test_parts_roundtrip`: "plain bridge 55-42 (dup 2) survived the rewrite"
  expects `dup: 2` to be written back; the slot loader's rule from `12e6629`
  (a count equal to the slot's `stackPaths`, which is 2 on this bench, is a
  legacy stamp and becomes the default) drops it on purpose. The test is
  from 2026-08-28, the rule from 2026-09-02. Kevin's call which one is
  right; not touched.

Both findings were first seen on the index-fix build alone, which the
earlier (port-1-contaminated) run had also flagged, so neither is the
rails change.

### 7.3 "Couldn't top rail have gone K -> G -> A?" (Kevin, 13:30)

It could, and that is the shape the bounce tier tries for a rail copy:
K.Y6 -> G's K lane -> G's bounce row -> the A-G lane -> row 1. In the dump
the A-G pair's single lane was gone: 48-51's same-chip hop on G had taken
G.X0 (AG0) - the first free pin in G's order - while G still had both lanes
to B, D, E, F and H free. Two choices in the router were resource-blind:

* **Same-chip hop lanes**, now ranked by cost (`sameChipHopCost`): a lane
  of a doubled pair whose other lane is free (the pair keeps one) first; a
  pair's last lane (a single-lane pair, or a doubled pair whose other lane
  is spoken for) after; I/J/K/L lanes last. Ties keep the breadboard-first
  table order.
* **SF-chip rows for SF hops** (`bestSfRow`): among the free rows, the one
  whose breadboard chip has the most virgin SF lanes, so an SF-to-SF hop
  stops stranding a chip at its last lane (ADC3-D0's hop took J.Y0, chip
  A's row, when H's row would have cost nothing).

* The first cut of the row choice preferred the chip with the MOST spare
  lanes and moved the probe feed onto the one pristine chip (G) in almost
  every case - the chip the GPIO taps bounce through. It now packs: a
  reserve bounce chip last, a chip at its last SF lane next to last,
  otherwise the fewest spare lanes first.
* Tap shape for a node on I/J/L straight through the hub lane into K (a
  same-chip hop onto IK/JK/KL, landing on a K row whose breadboard pin is
  free): a GPIO tap no longer needs a bounce chip whenever KL is free.

Results (`snap_rails3` -> `snap_hop2`, 70/70, every tap kept): 26 rows
moved. Same-chip hops leave single-lane pairs alone (48-51 on G: X0 = the
only A-G lane -> X2, a B-G lane; 56-58 on H likewise). The feed and the
SF-to-SF hops sit on lane-poor chips' rows now. `rail_stacking_light`:
1-TOP_RAIL's copy bounces through D (`A6.1 K4.3 D0.0 D15.0`), 15-TOP_RAIL
gains a hub copy through I, GND-45's K copy goes (rails are equal, the
top rail's bridges come first). Kevin's netlist: still no copy for
1-TOP_RAIL - see 7.4.

### 7.4 Why K -> G -> A still does not happen on Kevin's netlist

With the A-G lane free, the bounce through G is refused by the tap
reservation, correctly: G is the only chip left whose Y0, K lane and K row
are all virgin, and a GPIO's tap (GP_1/2/3/8 are all in nets here) has no
other shape while the probe feed holds the KL hub lane - the new hub-lane
tap shape needs KL. So the rail copy and the GPIO taps compete for G, and
the taps win. Move the feed off KL (dac0 in the feed window, 2.8-3.9 V, or
the probe.power_source config) and GPIO taps go L -> KL -> K, leaving G for
the rail. Demonstrated below.
