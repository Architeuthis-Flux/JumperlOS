# 555 LED Flasher

An NE555 wired as an astable multivibrator, blinking an LED. The Jumperless
makes every connection between the parts; you only push the parts into the
breadboard.

## Parts you need

**The values below are what is in the drawer, not what the project requires.**
Nothing in the guide fails you for using something else: each resistor step
measures what you put in and reports it, and `main.py` works out the blink rate
*your* parts should give. Bring roughly the right order of magnitude and the
project tells you what you built.

| Part | Suggested | Package | Rows | Anything from |
|------|-----------|---------|------|---------------|
| U1   | NE555 | DIP-8   | pin 1 at row 35, across the middle gap | - |
| R1   | 10k   | axial   | 10 - 40, straddling the middle gap | 1k - 100k |
| R2   | 47k   | axial   | 13 - 43, straddling the middle gap | 1k - 100k |
| C1   | 10uF  | electrolytic | + at 18, - at 19 | 1uF - 100uF |
| LED1 | any   | 2-lead  | anode (long leg) 22, cathode 23 | any colour |
| R3   | 330   | axial   | 16 - 46, straddling the middle gap | 100R - 2k |

The steps still fail on the two things that are mistakes rather than choices:
a resistor reading **open** (a leg not seated in the hole) or **short** (both
legs the same side of the ravine), and an LED drawing **no current**, which is
what a backwards LED looks like electrically.

Top rail is set to 5 V. Nothing else is pre-wired: opening the project leaves
the breadboard completely bare, and every connection appears as you confirm
each part in the guide.

`main.py` needs two measurement taps - **ADC0 on row 37 (OUT)** and **ADC1 on
row 7 (the timing cap)** - so *it* makes them when it starts and removes them
when it exits. They used to ship in the wiring file, which put two wires on the
board before you had placed a single part. If you had already tapped one of
those rows yourself, the script notices and leaves your connection alone.

## What it does

C1 charges through R1+R2 and discharges through R2, so

    f    = 1.44 / ((R1 + 2*R2) * C1)
    duty = (R1 + R2) / (R1 + 2*R2)

With the suggested parts that is 1.44 / (104k * 10uF) = **~1.4 Hz** at a
**~55%** duty cycle - slightly longer on than off, because the charge path is
the longer one.

`main.py` does not assume any of that. It asks the board what is actually
placed, via `list_parts()`:

- **R1, R2** come back with a `measured` field - the ohms the guide's
  continuity check really read across that resistor, four-wire, during the
  build. That is the number the prediction uses.
- **C1** has no `measured` field, because capacitance is not something this
  hardware can measure. Its `value:` is taken on trust, and the script says so.

Then it counts rising edges on OUT and the fraction of samples above 2.5 V, and
prints both sides every 3 seconds:

    your parts: R1 9.83k  R2 47.2k  C1 10uF   (R measured)
    they predict: 1.41 Hz, 55% high
    measured: 1.33 Hz, 54% high   (predicted 1.41 Hz, 55%)   cap 2.91 V
      the blink implies C1 = 10.6uF

That last line solves the equation backwards for the one component nothing can
measure. If it lands far from the value on your capacitor, the capacitor is the
part that is not what the file thinks it is - electrolytics of this size are
routinely -20/+80% and this is how you see yours. Counting whole edges in a 3 s
window puts the measured frequency on a 1/3 Hz grid, so treat ±0.33 Hz as the
resolution, not as disagreement.

The frequency is mirrored to the OLED if one is attached. Hold the clickwheel
to stop it.

If you run `main.py` straight from the Files browser rather than after a guided
build, there are no measurements to read - `measured` is RAM-only and does not
survive a reboot - so it falls back to the file's values and tells you it did.

## Running it

- **Clickwheel:** `Guides` - it is a top-level menu row, before `Apps` - then
  **555**, and walk the guided build. That is what wires the parts up. Run
  `main.py` when it offers at the end.
- **Headless:** `z 555` on the terminal drives the same flow. Add `new` to
  start over from the wiring, `load` to reopen the run you already have,
  `noscript` to stop at the wiring.

Launching a project does not borrow a slot any more. It opens
`/projects/555/555_run.yaml` - a **run file** - and makes it your active
circuit, exactly as if you had clicked a YAML in the Files browser. There is
**one run file per project** and it is reused: your build is saved into it at
every step, the board comes back to it after a power cycle, and the next launch
reopens it silently - unless you quit a guided build part way through, in which
case you are asked once whether to resume or to start over. To keep a run,
`slots` > `save to` while it is active. The shipped `wiring.yaml` is a
read-only template and is never written to.

Clicking `wiring.yaml` in the Files browser starts a run the same way. Either
route, **only a guide commit wires a part**: `expandPartsToBridges()` skips any
part still marked `placed: false`. This project has no `bridges:` section at
all, so a run you have not walked through yet has **nothing** on the
breadboard.

## Moving parts around

**Compact vs expanded.** Every two-leg part can sit two ways. **Expanded** puts
it at its own rows and the Jumperless routes its connections through the
crossbar underneath - good while you are building, because each part is easy to
see and to test on its own. **Compact** is how you would breadboard it by hand:
the legs go straight into the rows they connect to - a resistor from a chip's
pin row to the rail goes leg-in-row-7, leg-in-the-rail - and the routed
connections for that part disappear, because the legs themselves make the
contact.

Tap a part's lit footprint with the probe to flip that one part between the
two. **Every part remembers its own setting.** A leg whose endpoint is not a
real hole - `GND` and the analog pins live in the crossbar, not on the board -
keeps its own row and its routed connection either way, so a compact part can
still have one bridge.

To **move** a part, tap any free hole and pin 1 jumps there. The wheel never
moves a part - it only browses the steps. On the terminal, `m <row>` and `c` do
the same two things the probe does. Chips (DIPs) anchor at pin 1 - the dot or
the notch - wherever it really sits: **bottom-left** (rows 31-60) or rotated
180 with pin 1 top-right (rows 1-30). Axial parts (resistors, diodes) straddle the
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
- Blinking at a rate you did not expect: read the script's own two lines before
  suspecting a fault. If `measured` and `predicted` agree, the circuit is
  working correctly and the parts are simply not the ones the file suggests. If
  they disagree and `the blink implies C1 = ...` is far from your cap's marking,
  believe the implied value.
- A resistor step says **open**: a leg is not in the hole, or it is in the wrong
  row. It is not complaining about the value - no value is enforced.
- LED dark but OUT reads ~2.5 V average: the LED is backwards. Long leg goes
  in row 22.
