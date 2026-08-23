# Type to Screen

An I2C OLED module pushed into the breadboard - **anywhere** on it - wired to
the Jumperless's I2C pins by the crossbar. You type a line in the terminal, it
appears on the panel. No jumper wires: the Jumperless makes every connection.

## Parts you need

| Part | Package | Rows |
|------|---------|------|
| DISP | SSD1306 / SH1106 I2C module, 4-pin header | 5 - 8 *(the default; any rows work)* |

The guided build places the header in rows 5-8 in the usual pin order - **GND
5, VCC 6, SCL 7, SDA 8** - because a guide needs somewhere concrete to point.
But those rows are only a **default**: `main.py` asks where each signal
actually is, and re-routes to match. *Check your silkscreen before powering
up* - a few boards swap VCC and GND, and that is the one mistake that kills
the panel.

Top rail is set to 3.3 V. Whichever rows you use, SCL is routed to `RP_GPIO_8`
(RP pin 27) and SDA to `RP_GPIO_7` (RP pin 26) - the same pair the built-in
I2C scanner uses. Those two are fixed by the hardware; only the breadboard row
is yours to choose.

> **Jumperless V5 only.** This project routes breadboard rows to
> `RP_GPIO_7` and `RP_GPIO_8`, and those nodes exist only on the V5. The original
> Jumperless has exactly three routable GPIO (`RP_GPIO_0` plus UART
> TX/RX), so the wiring loads there but cannot be routed.

## What it does

`main.py` runs four steps, in order.

**1. Tap-to-assign.** It asks where each of GND / VCC / SCL / SDA is. At every
one of those prompts you can **tap the hole with the probe** *or* **type the
row number** - both are live at the same time, and pressing enter alone keeps
the default. Every probe gesture here has a typed twin, which is what makes
the whole flow drivable from a terminal, from a script, and from the HIL:

    Where is the panel? Enter alone keeps the guide's header rows.
      GND  tap its hole with the probe, or type a row 1-60 (enter = 5, q = quit)
           GND = row 5 (default)
      VCC  tap its hole with the probe, or type a row 1-60 (enter = 6, q = quit)
           VCC = row 41 (tapped)
      ...
    assignment: GND 5  VCC 41  SCL 7  SDA 8

**2. Driver and size.** A three-item menu - enter picks the first:

    Panel type:
      1) SSD1306 128x32   (default)
      2) SSD1306 128x64
      3) SH1106  128x64

**3. The wiring beacon.** It routes what you assigned, then loops: scan the
bus, and the moment something answers, initialise it and start drawing an
animation. **You can wire the panel up while this is running** and watch it
spring to life the instant the last wire is right. The terminal shows a quiet
row of dots, not one error per attempt:

    waiting for the panel - wire it up now ('q' + enter to give up)
    ............
    panel up at 0x3c: 128x32, 4 rows of 16 chars

**4. Type to screen.** Each line you type is pushed onto a short history and
redrawn. An empty line clears, `q` quits, Ctrl-C works. If the panel falls off
the bus mid-session, the script drops back to the beacon instead of dying -
push the module back in and it comes straight back.

The driver is deliberately tiny: MicroPython's `framebuf` module supplies the
pixel buffer and the built-in 8x8 font, and the `Screen` class only adds the
command set. **The SH1106 shim is two small differences**, both in `Screen`:
its charge pump is `0xAD/0x8B` instead of `0x8D/0x14`, and it has no
memory-addressing-mode command at all - so `show()` sets the page pointer per
page and starts each row at **column 2**, because the SH1106's RAM is 132
columns wide with the glass wired to 2..129. Miss that offset and the whole
image sits two pixels left, wrapped.

### What it leaves behind: nothing of its own

On the way out - `q`, Ctrl-C, or any other exit - the script blanks the panel,
releases its GPIO claims, and **removes exactly the bridges it made**. It
records each route as it makes it, and a route that was already on the board
when it started is reported and left alone:

    GND row 5 -> GND was already routed - left alone
    SDA row 8 -> RP_GPIO_7

That is the deliberate reading of "clear the data lines on exit": *this script
cleans up after itself*, and a guided build's bridges are your saved circuit,
not its mess. Run it after a guided build and it will find all four routes
already there, make none, and remove none.

### Driving it without hands

Everything the probe can do here, typed input can do too, so the whole flow is
scriptable over the serial port: launch it (`z i2cscrn`, or the Files browser)
and send `5`, `6`, `41`, `42`, `2`, `q` as lines. That is exactly how
`test_projects.py` exercises it with no panel attached - through the real
launcher, the real prompts and the real stdin.

> **A note on script size.** Until wave 3 the launcher needed *two* full-size
> copies of the source in RAM to prepend `_jl_project`, against an Arduino heap
> that settles around 19 KB free - so anything past roughly 6 KB failed, and
> failed **silently**, printing `Running ...` and then `--- script finished ---`
> with nothing between. Fixed in `runCompanionScript`
> (`src/ProjectsApp.cpp`): **the source is never copied into RAM at all**. The
> runner hands MicroPython a short `execfile()` and the lexer streams the script
> off the filesystem, so there is no Arduino-side ceiling left, and every
> failure branch now prints.
>
> One bound survives, a level up: **MicroPython's compiler** needs heap
> proportional to the source. Around 12 KB compiles on a freshly booted board
> (~39 KB free) and raises `MemoryError` on the ~25 KB left deep into a long
> session. This script is about 12 KB. If you grow it, watch for that.

## Running it

- **Clickwheel:** `Guides` - it is a top-level menu row, before `Apps` - then
  **i2cscrn**, and walk the guided build. That is what wires the parts up. Run
  `main.py` when it offers at the end.
- **Headless:** `z i2cscrn` on the terminal drives the same flow. Add `new` to
  start over from the wiring, `load` to reopen the run you already have,
  `noscript` to stop at the wiring.
- **`main.py` on its own** is enough if you would rather not walk the guide at
  all: it does its own assignment and its own routing, so a module dropped in
  any four rows comes up from a bare board.

Launching a project does not borrow a slot any more. It opens
`/projects/i2cscrn/i2cscrn_run.yaml` - a **run file** - and makes it your active
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

- **The dots never stop** - nothing is acking. The beacon is the diagnostic:
  leave it running and re-seat one wire at a time. Check GND and VCC first (a
  panel with no power cannot ack anything), then that the rows you assigned are
  the rows the legs are actually in, then the SDA/SCL order.
- **Wrong rows assigned** - `q`, enter, and run it again. Nothing is
  remembered between runs, and the exit removed whatever it had routed.
- **A device answers, but at some other address** - the script tries `0x3C`
  and `0x3D`, which covers effectively every module. Anything else: add it to
  `ADDRS` at the top of `main.py`.
- **The panel lights up but the image is shifted two pixels and wrapped** -
  it is an SH1106, not an SSD1306. Pick option 3.
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
