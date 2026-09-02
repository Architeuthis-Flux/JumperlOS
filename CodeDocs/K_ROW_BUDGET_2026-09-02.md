# Chip-K y-row budget (2026-09-02) — "can't find a path from ADC_3 to D1"

## The report

Kevin's bench netlist (a placed 4051: rails on rows 1/11 and 37/28/22, DAC_1 on
54/58, GP_1-3 on 6-8, ADC_0 on 4, ADC_1+ADC_2 on rows 2+3, GP_6 on 20) plus
`ADC_3-D1` and the probe feed `BUF_IN-GP_8` left the last two with `-2` on every
hop: "we should be able to find a path here with some hops."

## Root cause (read from his `b` printout, then confirmed on the bench)

No hop count can help: ADC_3 lives only on chip K's x11, and every signal on K
leaves through one of K's **8 y-rows**. All 8 were already on other nets:

    K y: T T 9 B B 10 10 d1

Three of those were avoidable. The router hands out virgin K rows first-come
and tries **virgin-only first** (stackingAttempt 0), same-net second:

- `3-ADC_2` took a fresh row (K.y6 via G) although `3-ADC_1` had just put net
  10 on K.y5 via F — attempt 0 sees F.y0 as "taken" and G as clean.
- `11-TOP_R` and `28-BOT_R` each took a second direct K row for their rail.

The two paths that have **no alternative** to a K bounce (an ijkl path K<->L
or K<->I must close K.x <-> K.y <-> K.x12/13/14) are resolved last, in
resolveUncommittedHops, and found nothing.

## The fix (src/routing/NetsToChipConnections.cpp)

`kRowBudgetRefuses()` gates every virgin chip-K row in `freeOrSameNetY` and
the `setChipYStatusSafe` writer (same shape as the sense-tap reservation):

- A net with a K-resident node (from the chips resolveChipCandidates chose,
  `kNeedingNetMask`) always gets its **first** K row.
- A **second** row, or a row for a net merely hopping through K, is granted
  only while `virgin - 1 >= nets that need K and hold none yet`.
- A refused row falls to stackingAttempt 1, where the BB-to-SF alt search
  piggybacks on the net's existing K row through that row's breadboard chip.
- Duplicates are exempt (they run after all primaries; sense-tap rule stays).
- **Rescue pass**: if anything is still unrouted after the budgeted pass, the
  three phases run again with the budget off, skipping fully-routed paths
  (`pathFullyRouted`). The skip is mandatory: `ijklPaths` leaves
  `altPathNeeded` set and resets y to -2 on re-entry, and
  `resolveUncommittedHops(allowStacking=2)` is virgin-only, so a re-resolved
  path would refuse its own row.

With slack on K the routing is unchanged (verified: a synthetic netlist with
TOP_R on rows 1 and 11 still gets two direct K rows).

## Second finding, fixed alongside: phantom y6 crosspoints

`sendAllPaths` never looks at `skip`, `sendPath` filtered only `== -1`, and the
CH446Q address encoder masks y to 3 bits — so a failed primary with y == -2
(exactly those two paths) would have closed **y6** crosspoints: K.x2/x11/x12/x13
onto K.y6 (net 10) and L.x11/x14 onto L.y6 (GND), i.e. rows 2/3, GP_8, BUF_IN,
D1 all tied together and to GND. f86b243 wiped this for duplicates only.

- `src/CH446Q.cpp` sendPath now skips any coordinate `< 0` (last gate).
- `couldntFindPath` wipes a failed primary's coordinates like it already did
  for duplicates.

Whether the phantoms were live on Kevin's board is unproven: net 10 read
2.545 V (not GND) before the flash, so at least path 0's phantoms were not
closed; ADC_3 read 2.556 V then and 0.002 V after the fix, which is consistent
with (not proof of) K.x11 having sat on K.y6.

## Bench verification (board was running release 5.7.10.0, flashed this dev build)

- Kevin's netlist reloaded from slot0: **all 19 paths routed**, no `Couldn't
  find a path`, no negative coordinates. K rows: `T T 9 13 B 10 12 d1`
  (13 = BUF_IN feed via D, 12 = ADC_3 via G). Reshaped: `28-BOT_R` now goes
  D.x8 -> E.x6 <-> E.y0 <-> E.x1 -> K.y4 (4 crosspoints instead of 2);
  `3-ADC_2` / `2-ADC_2` reuse F/K.y5.
- Electrical: row 28 read 2.60 V through a temporary ADC_4 tap (BOT_R set to
  2.7 V) - the reshaped rail path carries the rail; D1 driven from a temporary
  GP_7 bridge read 3.27 V / -0.01 V on ADC_3. Both taps removed; the netlist
  and every path came back identical.
- `test/hil/test_routing.py`: PASS (5 checks).
- Both targets build. OG router (`NetsToChipConnections_OG.cpp`) NOT ported:
  it never got the sense-tap reservation either, and there is no OG board on
  the bench to verify a routing-behavior change. The sender-side `< 0` gate
  covers OG's phantom hazard.

## Left alone (noted)

- `resolveAltPaths` BBtoSF scans `bb < 8 - K.uncommittedHops` — a crude
  older reservation that excludes chips G/H as piggyback intermediates while
  ijkl paths are pending. Redundant under the budget and occasionally in the
  way (it is why the rescue pass exists). Old code; not changed.
- The BBtoSF alt commit marks `bb.x[chip0's lane index]` (a random pin on the
  hop chip) as claimed — pre-existing, harmless, visible in chip status.
- A failed ijkl primary keeps its x-lane claims (K.x12/L.x14) in chipStates.
