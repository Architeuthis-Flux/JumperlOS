# V6 crossbar matrix: where V5 chokes, and what to wire differently

Written 2026-09-03 from the r7 schematic (`test/hil/fabric_v5.py` is the
machine-readable version), the router as it stands on dev, and the dense
routing cases in `test/hil/test_routing_dense.py`. Companion to
`ROUTING_SWEEP_2026-09-03.md` (the firmware side of the same night).

Vocabulary used throughout:

* **row** - a breadboard row on a breadboard chip's Y pin (chips A-H, Y1-Y7).
* **lane** - a wire between two chips' pins. BB-BB lanes join two X pins;
  BB-SF lanes join a breadboard chip's X pin to an SF chip's Y pin; hub lanes
  join two SF chips' X pins (IJ, IK, IL, JK, JL, KL).
* **bounce row** - Y0 of a breadboard chip: no breadboard hole, so any lanes
  closed onto it are joined to each other. The router's only way to turn a
  corner inside the fabric without spending a row.
* **crosspoint** - one closed CH446Q switch. CH446Q on-resistance is at most
  65 ohm (10 ohm channel-to-channel spread) at a 12 V supply span per the
  datasheet; on the ±9 V board a two-crosspoint loop measured ~130 ohm
  (part-ID bench, 2026-08-26), so budget 50-65 ohm per crosspoint.

## 1. The V5 fabric in numbers

12 x CH446Q, 16 X by 8 Y, 128 crosspoints each, 1536 total.

| resource | count | notes |
|---|---|---|
| breadboard rows on chips | 56 | 7 per chip A-H; rows 29/30/59/60 are X pins of K/L (2 more crosspoints to reach anything) |
| bounce rows | 8 | one per breadboard chip |
| BB-BB lanes | 48 | 20 chip pairs with 2 lanes, 8 pairs with 1: A-E, A-G, B-F, B-H, C-E, C-G, D-F, D-H |
| BB-SF lanes | 32 | exactly one from each breadboard chip to each of I, J, K, L |
| SF rows | 32 | 8 per SF chip = one per breadboard chip, i.e. the far ends of the 32 BB-SF lanes |
| hub lanes | 6 | one each: IJ, IK, IL, JK, JL, KL |
| special nodes on K | 12 + GND | 29, 59, BUF_IN, AREF, TOP_RAIL, BOTTOM_RAIL, DAC1, DAC0, ADC0-3, GND |
| special nodes on L | 12 + GND | 30, 60, BUF_OUT, ADC4, GPIO 1-8, GND |
| nano pins on I / J | 11 / 11 | odd D + even A on I, even D + odd A on J; plus ISENSE +/- and UART RX/TX, one each |

Crosspoints per path shape (resistance scales with it):

| shape | crosspoints | example |
|---|---|---|
| two rows, same chip | 2 | 3-5: A.X(free lane) on Y3 and Y5 |
| two rows, different chips, direct lane | 2 | 1-8 |
| row to a special (rail, DAC, GPIO, nano pin), direct | 2 | 1-TOP_RAIL: A.X9 on Y1, K.X4 on Y0 |
| the same through a bounce | 4 | 2-ADC0 when A's K lane is taken: A -> C.Y0 -> C's K lane -> K.Y2 |
| special to special through a hub lane | 4 | DAC0-D0: K.X7 + K.X14 on a K row, J.X14 + J.X0 on a J row |
| special to special through a bounce | 4 | K row -> bb.Y0 -> L row |
| row to row through an SF chip (new tier, 2026-09-03) | 4 | c0 lane -> sf.Y[c0], sf.Y[c1] <- c1 lane, joined by a free hub lane |

## 2. Chokepoints, ranked by how often they bite

**C1. Chip K.** Rails, both DACs, ADC0-3, half of GND, BUF_IN, AREF, rows
29/59 all leave through one of K's eight rows, one row per breadboard chip,
and each breadboard chip has exactly one lane into K. Consequences seen on
the bench: Kevin's twelve-bridge netlist spends 8/8 K rows (rows 2-5 to
ADC0-3 each bounce through another chip's K lane); rail stacking cannot
happen on any real netlist (every rail duplicate needs a bounce row AND a
virgin K row, and the last two virgin rows are reserved for sense taps); the
probe feed BUF_IN<->GP_8 is the path that loses when K fills
(`k_rows_overflow_metric`). The K-row budget in the router (2026-09-02) is
firmware rationing of this one resource.

**C2. Chip L**, same shape for GPIO 1-8, BUF_OUT, ADC4, rows 30/60, GND.
Seven GPIO nets out of chip A route today, but through six bounces
(`gpio_seven_from_chip_a`): every GPIO after the first from one chip costs
another chip's bounce row.

**C3. One bounce row per breadboard chip.** Eight bounces board-wide, shared
by every overflow shape (second special from a chip, SF-to-SF, third net
between two chips). `nano_dense_metric` (six D pins on rows 1-6, six A pins
on rows 15-20) needs ten bounces and gets eight: two nets fail with rows to
spare on I and J.

**C4. One lane from a breadboard chip into each SF chip.** A breadboard chip
carries ONE net directly into I (or J, K, L); the second needs a bounce. The
Nano header alternates I/J pin by pin, so a Nano circuit with four D pins on
one chip's rows already needs two bounces.

**C5. Eight rows per SF chip.** At most eight nets touch I, eight touch J:
the 22 Nano signals can never all be in distinct nets at once (16 max, fewer
in practice because bounces burn rows too).

**C6. Single-lane breadboard pairs.** The eight "across the gap" pairs (A-E,
A-G, B-F, B-H, C-E, C-G, D-F, D-H) have one lane, so a second net between,
say, rows 1-7 and 31-37 bounces, and those nets can never be stacked.

**C7. Single hub lanes.** SF-to-SF has one direct path per pair; the probe
feed (BUF_IN on K to GP_8 on L) occupies KL, a K row and an L row on every
board that is not feeding from DAC0.

**C8. Rails and GND enter the fabric at one point each** (TOP_RAIL K.X4,
BOTTOM_RAIL K.X5, GND K.X15 and L.X15). There is no way to give a rail two
independent entries to a breadboard chip, so "parallel rail paths" on V5 is
a firmware wish the copper cannot grant.

**C9. Rows 29/30/59/60** are X pins on K/L: two extra crosspoints and a K or
L row to reach anything, and they take four X pins that could be lanes.

**C10. The probe feed rides the fabric.** A permanent K row + L row + KL
lane on every board, purely to power the probe's LEDs and tip.

## 3. Proposals that keep twelve chips

**P1. Rails and GND on an X pin of every breadboard chip (biggest win).**
Give each of A-H three X pins hardwired to TOP_RAIL, BOTTOM_RAIL and GND.
A rail connection becomes ONE crosspoint (~60 ohm instead of ~120), never
touches K, never needs a lane, and stacking is free: two rows on the same
rail are already in parallel through the rail copper, and a second rail
crosspoint on the same row halves the per-row resistance at zero fabric
cost. K loses three of its twelve special pins and, more importantly, every
rail/GND row it used to spend. Cost: three X pins per breadboard chip; take
them from three of the chip's five doubled lanes (A keeps two lanes to B and
C, drops to one for D, F, H) - after P1 those lanes are needed far less,
because rail and GND nets no longer bounce through them. Router impact: the
rail node's candidate chip becomes "the breadboard chip of the row", i.e.
the same-chip BB-to-SF shape that already exists for K-resident nodes.

**P2. Take the probe feed off the fabric.** A small analog switch (or a
pair of MOSFETs) selecting DAC0 / a GPIO onto BUF_IN outside the crossbar
returns a K row, an L row and the KL lane on every board (C7, C10). The
InfraPaths arbitration keeps its logic; it stops routing.

**P3. Move rows 29/30/59/60 off K/L.** Either accept 56 rows, or give the
four corner rows to the breadboard chips' Y0 of two chips (losing two bounce
rows - a bad trade, see C3), or - best - a ninth breadboard chip (section
4). If they stay on K/L, at least pair them (29 and 59 on one chip's
adjacent pins) so a same-chip X-hop joins them.

**P4. ADC inputs on the row side.** With virtual ADC slots the sense taps
are ephemeral routes that must land on a free K row. Put ADC0-3 on L as
well as K (four X pins on L freed by moving 30/60 and ADC4), so a tap can
use whichever of K/L has a row.

**P5. Second lane for the single-lane pairs** only if X pins come free
(P1 spends them, P2 does not). Lower priority than P1: the 2026-09-03 hub
tier already turns "no lane and no bounce" into "through an SF chip's rows".

## 4. If chips are added (a separate decision)

The router today already handles a node that lives on two chips (GND is on
K and L; `resolveChipCandidates` picks). Every proposal here uses that
mechanism so the algorithm does not change shape; only the tables do.

**A1. A second specials chip, "M" = mirror of K.** M.Y0-7 wired to A-H like
K's rows (one new lane per breadboard chip, one X pin each), M.X carrying a
second copy of TOP_RAIL, BOTTOM_RAIL, GND, DAC0/1 and ADC0-3 (bus them - they
are the same nets). Every breadboard chip gains a second row into the
specials, rail stacking becomes a real parallel path (two lanes, two rows,
two K/M crosspoints), and K's row pressure halves. Router: candidates for
TOP_RAIL are {K, M}; the existing GND alternator / `moreAvailableChip`
logic applies unchanged. This is the smallest change with the largest
routability gain. Combine with P1 and the rails vanish from K/M entirely,
leaving them for DAC/ADC/BUF and doubling THOSE.

**A2. A bounce hub, "N".** N.X0-15 = two lanes to every breadboard chip,
N.Y0-7 = eight NC rows. Any two breadboard chips can now be joined by
closing their lanes onto one N row: four crosspoints, no breadboard bounce
row spent, eight more bounces board-wide (C3), and every pair gains two
lanes (C6). Router: this is the existing "bounce through a breadboard chip's
Y0" tier with a chip that has eight bounce rows instead of one - generalise
`for (bb = 0; bb < 8) ... Y0` to "for every bounce row on every chip that
has any", which the descriptor should expose (section 7).

**A3. Both (A1 + A2, fourteen chips)** is the configuration where nothing in
`test_routing_dense.py`'s metric cases would fail: the nano-dense case needs
bounces (A2), the K-overflow case needs specials rows (A1).

**A4. A larger part instead of more parts.** If the family offers a 16x16
(or 8x32) crosspoint, use it for K and L: 16 rows per specials chip is two
rows per breadboard chip, the same effect as A1 without a new lane per
breadboard chip - but the second row still enters through the same single
BB-K lane unless a second lane is added, so A1's extra lane is what buys the
parallel path.

Cost model for a new chip on the breadboard side: every SF-style chip costs
each breadboard chip one X pin per lane into it. A-H have 16 X pins: 12
BB-BB lanes + 4 SF lanes today. A1 and A2 each take one (A2 two if it is to
give every pair two lanes); P1 takes three. That is the budget to trade in.

## 5. Other directions (resistance, availability, how people use it)

**R1. Power taps with low-R switches.** The rails and GND carry the current;
signals do not. Instead of (or besides) P1, give one row per breadboard chip
(rows 1, 8, 15, 22, 31, 38, 45, 52 - the first row of each chip) a dedicated
low on-resistance analog switch (a few ohm, e.g. a 16:2 or 8:1 mux part) to
each rail and GND. The crossbar then only has to reach the tap row (one lane,
two crosspoints, high-impedance sense of the rail is unaffected) and the
tap row carries the current through the low-R switch. Users already tend to
land power on the outer rows; the LEDs can hint which rows are taps.

**R2. Per-row rail crosspoints (P1) are the analog-friendly version** of R1:
every row reaches a rail in one 60 ohm switch, two rows in parallel 30 ohm.
For audio and op-amp circuits (rails + GND on many rows, signals chained
row to row) this removes almost every bounce the current V5 needs.

**R3. Keep every crosspoint path short for signals, not for power.** A
4-crosspoint bounce is ~250 ohm; for a logic line, an ADC sense or an audio
signal that is fine; for a 100 mA rail it is a volt. Design the fabric so
power never bounces (P1/A1/R1) and let signals keep the flexible, longer
paths.

**R4. Usage-aware pin placement on the SF chips.** Things used together
should sit on chips that reach each other in one hop: (a) GPIO and UART
(L, and TX/RX on I/J) - a UART-to-row is fine but UART-to-GPIO loopback is
a hub-lane path; (b) DAC0 and the ADCs (all on K - good; K-to-K same-chip
hops cost one K row); (c) the expansion port lines and GPIO - see section 6.
If the port replaces the Nano header, I and J lose the odd/even split and
can host 16 port lines + 4 GPIO each, making "port line <-> GPIO" a
same-chip hop instead of a hub-lane path.

**R5. Sense without a K row.** Virtual ADC taps need a free K row per tap.
A dedicated sense chip (an SF chip whose X pins are the 4 ADC inputs plus 12
hub lanes and whose 8 rows are the breadboard chips) turns "tap row N" into
a two-crosspoint route that never competes with rails or DACs. It is A1
specialised for sensing.

## 6. The expansion port (the Nano header's successor)

The Nano header today is 22 signals on I/J X pins plus AREF on K, RESET on
GPIO 18/19, and power pads switched by jumpers. As a port:

* Keep the port lines on I/J exactly as nano pins are today (the router's
  BB-to-NANO shape, `nano.mapIJ`, `xMapI/J`): a plug-in Nano-header board
  is then wiring only. Fewer lines (16) frees X pins on I/J for a second
  hub lane each (IJ x2, IK x2 ...) - the SF-to-SF chokepoint (C7).
* Route the rails, GND, ±9 V and 5 V to the port outside the fabric. A
  second Jumperless on the port sees the lines as its own SF nodes and its
  own router handles its side; nothing new is needed on this side either.
* Reserve two port lines hardwired to L's GPIO pins (digital handshake with
  an application board) and one to BUF_OUT (a buffered analog out).
* Give the port a presence pin the firmware can read, so the port's lines
  show up as nodes only when something is plugged in - the same "capability,
  not board name" gate `BoardCaps` uses.

## 7. What the router needs from the board descriptor to survive any of this

The V5 router is table-driven for lanes (`xMapForChipLane0/1` search
`xMap`), but these assumptions are still literals in
`src/routing/NetsToChipConnections.cpp`:

| assumption | where | descriptor query it should become |
|---|---|---|
| chips 0-7 are breadboard chips, 8-11 special | `chip < 8` / `>= 8` throughout; `frontEnd()`; `sortSFchipsLeastToMostCrowded` | `isBreadboardChip(c)`, `isSfChip(c)` |
| a breadboard chip's bounce row is Y0, and only Y0 | every bounce tier (`setPathY(i, 2, 0)`), `resolveUncommittedHops` ("only allow Y=0") | `bounceRows(c)` (A2 gives some chips eight) |
| an SF chip's row index == the breadboard chip index | `yMapSF = bb`, `nanoY = intermediateChip`, `sfYPosition = targetChip` | `sfRowFor(sf, bb)` (the hub tier already searches `yMap`) |
| hub lanes are X12-X14 of an SF chip | `for (i = 12; i < 15)` in `ijklPaths` and the BBtoNANO alt | `hubLanes(sf)` |
| K is the specials chip: row budget, sense-tap reservation, GND K/L alternation | `kRowBudgetRefuses`, `reservedKRowForSenseTaps`, `gndChipAlternator`, `CHIP_K` literals | a per-chip `rowsAreScarce` / `senseTapReserve` field; candidates from `xMap` (already) |
| the Nano pins live on I/J only, one chip each | `nano.mapIJ`, `mapKL`, `numConns` | derived from `xMap` at init (`findStartAndEndChips` already searches `xMap` for specials - do the same for nano pins) |
| a path has at most four crosspoints | `pathStruct chip[4]`, `sendPath`, every tier | `chip[6]` (x/y already have six slots) - needed for SF-to-SF through a hub when the direct hub lane is busy |
| rows 1..60 map to chips through `bbNodesToChip` | `board::currentBoard().bbNodesToChip` (already descriptor) | keep |

The tiers themselves are a fixed list of shapes tried in order (direct,
same-chip hop, bounce, hub lane, hub-row hop). Expressed over those queries
the list does not change when chips are added; only the descriptor does,
which is what `src/boards/board.h` promises ("adding a new board is
mechanical"). The hub tier added on 2026-09-03 is written that way as a
worked example; the rest is a refactor with `test_routing_dense.py`'s
snapshot diff as the gate (zero moved rows on V5).
