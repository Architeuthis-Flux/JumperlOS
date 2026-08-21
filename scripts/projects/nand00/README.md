# Logic Gates 101

A 74HC00 quad NAND gate. The Jumperless drives both inputs of gate 1, reads
its output back, and lights an LED from the same output - so you can watch
the truth table and measure it at the same time.

## Parts you need

| Part | Value | Package | Rows |
|------|-------|---------|------|
| U1   | 74HC00 | DIP-14 | pin 1 at row 5, across the middle gap |
| LED1 | any   | 2-lead  | anode (long leg) 18, cathode 19 |
| R1   | 330   | axial   | 15 - 16 |

Top rail is set to **3.3 V**, not 5 V: rows 5, 6 and 7 land on real RP2350
pins, and those are 3.3 V parts. A 74HC00 is perfectly happy anywhere from
2 V to 6 V.

Pin map for gate 1:

| 74HC00 pin | Row | Goes to |
|---|---|---|
| 1 (1A) | 5 | `RP_GPIO_1` - the Jumperless drives it |
| 2 (1B) | 6 | `RP_GPIO_2` - the Jumperless drives it |
| 3 (1Y) | 7 | `RP_GPIO_3` for readback, and R1 -> LED1 -> GND |
| 7 (GND) | 11 | GND |
| 14 (VCC) | 35 | top rail |

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

- **Clickwheel:** Apps > Projects > nand00.
- **Files browser:** open `/projects/nand00/wiring.yaml` to load the
  circuit, then run `/projects/nand00/main.py`.

## Troubleshooting

- **Every read says `FLOATING`** - the output pin is not driving. Check that
  the chip straddles the middle gap with pin 1 (next to the notch) at row 5,
  and that the top rail really is at 3.3 V.
- **The LED never lights but the readback is right** - the LED is backwards.
  The long leg goes in row 18.
- **The LED is on all four rows** - a 74HC**02** (NOR) or a 74HC**08** (AND)
  will not behave like this; check the part number on the chip. A 74HC00
  with input pin 1 or 2 not seated will also read high forever, because a
  floating HC input tends to drift high.
- **The last two rows disagree** - one of the two input legs (rows 5, 6) is
  not making contact.
