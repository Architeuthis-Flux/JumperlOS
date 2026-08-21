# 555 LED Flasher

An NE555 wired as an astable multivibrator, blinking an LED at about 1.4 Hz.
The Jumperless makes every connection between the parts; you only push the
parts into the breadboard.

## Parts you need

| Part | Value | Package | Rows |
|------|-------|---------|------|
| U1   | NE555 | DIP-8   | pin 1 at row 5, across the middle gap |
| R1   | 10k   | axial   | 12 - 13 |
| R2   | 47k   | axial   | 15 - 16 |
| C1   | 10uF  | electrolytic | + at 18, - at 19 |
| LED1 | any   | 2-lead  | anode (long leg) 22, cathode 23 |
| R3   | 330   | axial   | 25 - 26 |

Top rail is set to 5 V. ADC0 watches OUT (row 7), ADC1 watches the timing
cap (row 37).

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

- **Clickwheel:** Apps > Projects > 555, and walk the guided build - that is
  what wires the parts up. Then run `main.py` when it offers.
- **Headless:** `z /projects/555/wiring.yaml <slot 0-7>` on the terminal
  drives the same flow, then run `/projects/555/main.py`.

Opening `wiring.yaml` in the Files browser loads the file and makes the two
ADC tap connections from its `bridges:` section, but it does **not** wire the
circuit. Parts are bridged only as the guided build confirms each one -
`expandPartsToBridges()` skips any part still marked `placed: false`, and only
a guide commit sets that flag. Loaded that way, the LED will not blink.

## Troubleshooting

- No blink at all: check the 555's orientation - pin 1 (next to the dot) at
  row 5, and the chip must straddle the middle gap.
- Blinking way too fast or slow: R1/R2 swapped, or the cap is not 10uF.
- LED dark but OUT reads ~2.5 V average: the LED is backwards. Long leg goes
  in row 22.
