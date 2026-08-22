# Type to Screen

An SSD1306 OLED module pushed into the breadboard, wired to the Jumperless's
I2C pins by the crossbar. You type a line in the terminal, it appears on the
panel. No jumper wires - the Jumperless makes every connection.

## Parts you need

| Part | Package | Rows |
|------|---------|------|
| DISP | SSD1306 I2C module, 4-pin header | 5 - 8 |

The four pins go, in order: **GND at row 5, VCC at row 6, SCL at row 7,
SDA at row 8**. That is the pin order printed on nearly every cheap 0.96"
and 0.91" module, but *check your silkscreen before powering up* - a few
boards swap VCC and GND, and that is the one mistake that kills the panel.

Top rail is set to 3.3 V. Rows 7 and 8 are routed to `RP_GPIO_8` (RP pin 27,
SCL) and `RP_GPIO_7` (RP pin 26, SDA) - the same pair the built-in I2C
scanner uses.

> **Jumperless V5 only.** This project routes breadboard rows to
> `RP_GPIO_7` and `RP_GPIO_8`, and those nodes exist only on the V5. The original
> Jumperless has exactly three routable GPIO (`RP_GPIO_0` plus UART
> TX/RX), so the wiring loads there but cannot be routed.

## What it does

`main.py` opens `machine.I2C(1, scl=27, sda=26)`, scans the bus, and then
loops on `input()`. Each line you type is pushed onto a short history and
redrawn on the panel:

    Type to Screen
    i2c devices: ['0x3c']
    panel up: 128x32, 4 rows of 16 chars
    Type a line and press enter. Empty line clears. 'q' quits.
    > hello jumperless

An empty line clears the screen, `q` quits, and Ctrl-C works too.

The driver is deliberately tiny: MicroPython's `framebuf` module supplies
the pixel buffer and the built-in 8x8 font, and the `Screen` class in
`main.py` only adds the SSD1306 command set on top (init sequence, column
and page window, the `0x40` data prefix).

`HEIGHT = 32` at the top of `main.py` is the one thing to change for a
128x64 panel - set it to 64 and the driver picks the right multiplex ratio
and COM pin configuration by itself.

## Running it

- **Clickwheel:** `Projects` - it is a top-level menu row, before `Apps` - then
  **i2cscrn**, and walk the guided build. That is what wires the parts up. Run
  `main.py` when it offers at the end.
- **Headless:** `z i2cscrn` on the terminal drives the same flow. Add `new` to
  force a fresh run, `load` to reopen the latest one, `run=<N>` for a specific
  one, `noscript` to stop at the wiring.

Launching a project does not borrow a slot any more. It opens
`/projects/i2cscrn/i2cscrn_<N>.yaml` - a **run file** - and makes it your active
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

Tap a part's lit footprint with the probe to flip that one part between the
two. **Every part remembers its own setting.** A leg whose endpoint is not a
real hole - `GND` and the analog pins live in the crossbar, not on the board -
keeps its own row and its routed connection either way, so a compact part can
still have one bridge.

To **move** a part, tap any free hole and pin 1 jumps there. The wheel never
moves a part - it only browses the steps. On the terminal, `m <row>` and `c` do
the same two things the probe does. Chips (DIPs) sit with pin 1 - the dot or
the notch - on the **bottom half** of the board, rows 31-60. Axial parts (resistors, diodes) straddle the
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

- **`i2c devices: []`** - nothing acked. Check that the module is really in
  rows 5-8 with GND at 5, that the top rail is at 3.3 V, and that SCL/SDA
  are not swapped (row 7 is SCL, row 8 is SDA).
- **A device answers, but at some other address** - most 128x64 boards can
  be strapped to 0x3D. Change `ADDR` at the top of `main.py`.
- **Bare panel, no breakout board** - the 4-pin modules carry their own
  pull-up resistors. A raw panel does not; add 4.7k from row 7 and from
  row 8 up to the top rail (that is exactly what the `eeprom` project
  does for its bare chip).
- **The Jumperless's own top OLED behaves oddly while this runs** - in its
  default `connection_type 0` it lives on GPIO 7/8, the very pins this
  project uses, and it shares the i2c1 peripheral in every mode. Turn it
  off (or move it to the hardwired pins) while you play with this project.
- **The screen shows garbage after a while** - drop `I2C_HZ` to 50000.
  Long crossbar paths plus breadboard capacitance slow the edges down.
