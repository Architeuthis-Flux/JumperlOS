# EEPROM Dumper

A 24Cxx serial EEPROM in the breadboard, read over I2C and hex-dumped to the
terminal. The Jumperless makes every connection - including the one that
decides whether the chip can be written at all.

## Parts you need

| Part | Value | Package | Rows |
|------|-------|---------|------|
| U1   | 24C02 (or 24C16) | DIP-8 | pin 1 at row 35, across the middle gap |
| R1   | 4.7k  | axial   | 12 - 42, straddling the middle gap (SDA pull-up) |
| R2   | 4.7k  | axial   | 15 - 45, straddling the middle gap (SCL pull-up) |

Top rail is set to 3.3 V.

The 24Cxx pinout, and where each pin lands with pin 1 at row 35:

| Pin | Name | Row | Goes to |
|---|---|---|---|
| 1 | A0  | 35 | GND |
| 2 | A1  | 36 | GND |
| 3 | A2  | 37 | GND |
| 4 | GND | 38 | GND |
| 5 | SDA | 8  | `RP_GPIO_7` (RP pin 26) |
| 6 | SCL | 7  | `RP_GPIO_8` (RP pin 27) |
| 7 | WP  | 6  | top rail - **write protected** |
| 8 | VCC | 5  | top rail |

A0-A2 all grounded puts the chip at **0x50**.

> **Jumperless V5 only.** This project routes breadboard rows to
> `RP_GPIO_7` and `RP_GPIO_8`, and those nodes exist only on the V5. The original
> Jumperless has exactly three routable GPIO (`RP_GPIO_0` plus UART
> TX/RX), so the wiring loads there but cannot be routed.

### Why the two resistors are not optional

I2C is an open-drain bus: the chips only ever pull the lines *down*, so
something has to pull them back up. A 4-pin display module has pull-ups
soldered on it; a bare DIP EEPROM does not. The RP2350's internal pull-ups
are around 50-80k, and 60k into the crossbar-plus-breadboard capacitance
gives a rise time of a couple of microseconds - well past the 1 us the
100 kHz I2C spec allows. 4.7k brings that down to well under 500 ns.

### Write protect is a wire, not a jumper

Pin 7 (WP) high means "reads only". The wiring holds row 6 at the top
rail, so the chip is genuinely read-only from the moment it powers up -
not read-only by convention.

The optional write test in `main.py` is the only thing that changes that,
and it does so by re-routing, not by asking you to move a jumper:

    disconnect(6, TOP_RAIL)
    connect(6, GND)            # writes enabled for exactly one byte
    ...
    disconnect(6, GND)
    connect(6, TOP_RAIL)       # in a finally, so it always goes back

That re-route runs `refreshConnections()`, which re-asserts the slot's GPIO
configuration onto RP pins 26/27 - so the script rebuilds its `machine.I2C`
object afterwards. If you ever see the restore step report a failure,
re-load the wiring before trusting the chip again.

## What it does

    EEPROM Dumper eeprom
    i2c devices: ['0x50']
    0000  FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF  |................|
    ...
    00F0  FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF  |................|
    256 bytes read.  (all 0xFF - an erased chip)
    dump more - total size in bytes (blank = no):

16 bytes to a line with a printable-ASCII gutter, the same shape `hexdump -C`
uses. Then it offers to read further (for a 24C16, answer `2048`), and last
of all offers the write test, which defaults to **no**.

`ADDR_BITS = 8` at the top of `main.py` covers the 24C01 through 24C16. A
24C32 or larger uses a 16-bit word address - set it to 16 for those.

## Running it

- **Clickwheel:** Apps > Projects > eeprom, and walk the guided build. That is
  what actually wires the board. Then run `main.py` when it offers.
- **Headless:** `z /projects/eeprom/wiring.yaml <slot 0-7>` on the terminal
  drives the same flow, then run `/projects/eeprom/main.py`.

Opening `wiring.yaml` in the Files browser loads the file but wires **nothing**.
This project has no `bridges:` section at all - every connection lives in
`parts:`, and `expandPartsToBridges()` skips any part still marked
`placed: false`, which only a guide commit clears. Loaded that way the
breadboard stays completely unconnected and `main.py` reports an empty bus.

## Troubleshooting

- **`i2c devices: []`** - nothing acked. Most often the chip is in
  backwards: pin 1 is the end with the dot, at row 35, and the chip must
  straddle the middle gap. After that, check that both 4.7k resistors are
  really straddling the gap at rows 12/42 and 15/45.
- **A device answers, but not at 0x50** - one of A0/A1/A2 is not reaching
  GND, which shifts the address. 0x51 means A0 is high, 0x52 means A1, and
  so on. Change `ADDR` in `main.py` to match, or re-seat the chip.
- **The dump repeats every 256 bytes** - that is a 24C02, which really is
  256 bytes; the address wraps.
- **Reads work but the write test says the byte did not stick** - WP did not
  reach GND. Watch the terminal for the restore line; if the re-route
  failed, the chip stayed protected, which is the safe way to fail.
- **The Jumperless's own top OLED misbehaves while this runs** - in its
  default `connection_type 0` it lives on GPIO 7/8, the same pins, and it
  shares the i2c1 peripheral in every mode. Turn it off while you use this
  project.
