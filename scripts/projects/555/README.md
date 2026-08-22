# 555 LED Flasher

An NE555 wired as an astable multivibrator, blinking an LED at about 1.4 Hz.
The Jumperless makes every connection between the parts; you only push the
parts into the breadboard.

## Parts you need

| Part | Value | Package | Rows |
|------|-------|---------|------|
| U1   | NE555 | DIP-8   | pin 1 at row 35, across the middle gap |
| R1   | 10k   | axial   | 10 - 40, straddling the middle gap |
| R2   | 47k   | axial   | 13 - 43, straddling the middle gap |
| C1   | 10uF  | electrolytic | + at 18, - at 19 |
| LED1 | any   | 2-lead  | anode (long leg) 22, cathode 23 |
| R3   | 330   | axial   | 16 - 46, straddling the middle gap |

Top rail is set to 5 V. ADC0 watches OUT (row 37), ADC1 watches the timing
cap (row 7).

## What it does

R1 charges C1 through R1+R2 and discharges it through R2, so

    f = 1.44 / ((R1 + 2*R2) * C1) = 1.44 / (104k * 10uF) = ~1.4 Hz

The LED blinks at that rate, slightly longer on than off: OUT is high for
0.693*(R1+R2)*C1 = 0.395 s and low for 0.693*R2*C1 = 0.326 s, so the duty
cycle is ~54.8%.

`main.py` counts the edges on OUT and prints a line every 3 seconds
(counting whole edges in a 3 s window means the printed frequency lands on
a 1/3 Hz grid - ±0.33 Hz resolution):

    freq: 1.38 Hz   cap: 2.91 V

and mirrors the frequency to the OLED if one is attached. Hold the
clickwheel to stop it.

## Running it

- **Clickwheel:** `Projects` - it is a top-level menu row, before `Apps` - then
  **555**, and walk the guided build. That is what wires the parts up. Run
  `main.py` when it offers at the end.
- **Headless:** `z 555` on the terminal drives the same flow. Add `new` to
  force a fresh run, `load` to reopen the latest one, `run=<N>` for a specific
  one, `noscript` to stop at the wiring.

Launching a project does not borrow a slot any more. It opens
`/projects/555/555_<N>.yaml` - a **run file** - and makes it your active
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

- No blink at all: check the 555's orientation - pin 1 (next to the dot) at
  row 35, and the chip must straddle the middle gap.
- Blinking way too fast or slow: R1/R2 swapped, or the cap is not 10uF.
- LED dark but OUT reads ~2.5 V average: the LED is backwards. Long leg goes
  in row 22.
