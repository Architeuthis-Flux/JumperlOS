# Logic Gates 101

A 74HC00 quad NAND gate. The Jumperless drives both inputs of gate 1, reads
its output back, and lights an LED from the same output - so you can watch
the truth table and measure it at the same time.

## Parts you need

| Part | Value | Package | Rows |
|------|-------|---------|------|
| U1   | 74HC00 | DIP-14 | pin 1 at row 35, across the middle gap |
| LED1 | any   | 2-lead  | anode (long leg) 18, cathode 19 |
| R1   | 330   | axial   | 15 - 45, straddling the middle gap |

Top rail is set to **3.3 V**, not 5 V: rows 35, 36 and 37 land on real RP2350
pins, and those are 3.3 V parts. A 74HC00 is perfectly happy anywhere from
2 V to 6 V.

Pin map for gate 1:

| 74HC00 pin | Row | Goes to |
|---|---|---|
| 1 (1A) | 35 | `RP_GPIO_1` - the Jumperless drives it |
| 2 (1B) | 36 | `RP_GPIO_2` - the Jumperless drives it |
| 3 (1Y) | 37 | `RP_GPIO_3` for readback, and R1 -> LED1 -> GND |
| 7 (GND) | 41 | GND |
| 14 (VCC) | 5 | top rail |

> **Jumperless V5 only.** This project routes breadboard rows to
> `RP_GPIO_1`, `RP_GPIO_2` and `RP_GPIO_3`, and those nodes exist only on the V5. The original
> Jumperless has exactly three routable GPIO (`RP_GPIO_0` plus UART
> TX/RX), so the wiring loads there but cannot be routed.

The other three gates are not used, so their inputs (pins 4, 5, 9, 10, 12,
13) are tied to GND. That is not decoration: a floating CMOS input drifts
around its switching threshold, oscillates, and makes the chip draw far more
current than it should.

## What it does

      A  B  | Y  expected  LED
      ------+-----------------
      0  0  | 1  1         on    ok
      0  1  | 1  1         on    ok
      1  0  | 1  1         on    ok
      1  1  | 0  0         off   ok

    All four rows match NAND. That is a working gate.

NAND is "not and": the output is high unless **both** inputs are high. So
the LED is lit for three of the four input combinations and dark for the
fourth.

After the table, `main.py` drops into a live mode. Type `10` and the board
sets A high, B low, and reads the result back:

    A B> 10
       A=1 B=0 -> Y = HIGH (expected 1)

`q` quits, and Ctrl-C works too.

## Running it

- **Clickwheel:** `Projects` - it is a top-level menu row, before `Apps` - then
  **nand00**, and walk the guided build. That is what wires the parts up. Run
  `main.py` when it offers at the end.
- **Headless:** `z nand00` on the terminal drives the same flow. Add `new` to
  force a fresh run, `load` to reopen the latest one, `run=<N>` for a specific
  one, `noscript` to stop at the wiring.

Launching a project does not borrow a slot any more. It opens
`/projects/nand00/nand00_<N>.yaml` - a **run file** - and makes it your active
circuit, exactly as if you had clicked a YAML in the Files browser. Your build
is saved into it at every step, the board comes back to it after a power cycle,
and the next launch offers to reload it or to start run N+1 (the old one stays
on disk). The shipped `wiring.yaml` is a read-only template and is never
written to.

Clicking `wiring.yaml` in the Files browser starts a run the same way. Either
route, **only a guide commit wires a part**: `expandPartsToBridges()` skips any
part still marked `placed: false`, so a run you have not walked through yet has
nothing on the breadboard except whatever its `bridges:` section already held.

## Moving parts around

**Compact vs expanded.** Every two-leg part can sit two ways. **Expanded** puts
it at its own rows and the Jumperless routes its connections through the
crossbar underneath - good while you are building, because each part is easy to
see and to test on its own. **Compact** is how you would breadboard it by hand:
the legs go straight into the rows they connect to - a resistor from a chip's
pin row to the rail goes leg-in-row-7, leg-in-the-rail - and the routed
connections for that part disappear, because the legs themselves make the
contact.

Tap a part's lit footprint with the probe (or double-click the wheel, then
double-click again) to flip that one part between the two. **Every part
remembers its own setting.** A leg whose endpoint is not a real hole - `GND` and
the analog pins live in the crossbar, not on the board - keeps its own row and
its routed connection either way, so a compact part can still have one bridge.

To **move** a part, tap any free hole and pin 1 jumps there, or double-click the
wheel and turn to slide it. On the terminal, `m <row>` and `c` do the same two
things. Chips (DIPs) sit with pin 1 - the dot or the notch - on the **bottom
half** of the board, rows 31-60. Axial parts (resistors, diodes) straddle the
middle gap by default; radial parts (caps, LEDs) sit in two neighbouring rows.

**Browsing.** The wheel moves *between* steps rather than confirming them, and
the ring wraps: turn past the last step and you land on a summary of what is
built, skipped and still outstanding; turn again and you are back at step 1. A
turn never ends the build - hold the wheel, or press `q`, for that. You can skip
a part with `s` and come back to it later, and confirming at the summary jumps
you to the first thing you have not built yet.

> Measuring and *identifying* an unknown part (`identify_part(leftmost_row=...)`)
> is planned but is not in this release. What the guide shows today is the value
> each verify step measures - a resistance, a diode drop, a rail voltage.

## Troubleshooting

- **Every read says `FLOATING`** - the output pin is not driving. Check that
  the chip straddles the middle gap with pin 1 (next to the notch) at row 35,
  and that the top rail really is at 3.3 V.
- **The LED never lights but the readback is right** - the LED is backwards.
  The long leg goes in row 18.
- **The LED is on for all four rows** - the output is stuck high. Most often
  input pin 1 or 2 is not seated: a floating HC input drifts high, and two
  high inputs are the only case NAND pulls low.
- **The LED is on for exactly one row, `1 1`** - that is a 74HC**08** (AND).
  AND is NAND's exact inverse, so it lights the one row NAND darkens. Check
  the part number on the chip.
- **A 74HC02 (NOR) is worse than wrong here.** Its pin 1 is an *output*
  (1Y), not an input, and this wiring drives pin 1 from `RP_GPIO_1` - two
  drivers fighting over one node. Pull it out rather than leaving it
  powered, and use a 74HC00.
- **The last two rows disagree** - one of the two input legs (rows 35, 36) is
  not making contact.
