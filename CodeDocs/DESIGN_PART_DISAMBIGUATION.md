# Part disambiguation - what separates the parts the vectors cannot

Companion to `DESIGN_IC_IDENTIFICATION.md` (the tiers) - written 2026-08-30
alongside the full partdb identification-vector fill-out. Two methods LANDED
that session; two are DESIGNED here and wait for a bench session.

## 1. What landed 2026-08-30 (context)

- **Every logic record now carries `fingerprint:` + `vec:`** (33 vector
  sets), authored from TI/Nexperia/ON datasheets fetched fresh - plus the
  74393 and GMT177 records. Pin-map errata found and fixed: the 7410's
  pins 12/13 were swapped; the 74107's VERIFY flag resolved as correct.
- **GPIO budget law**: the runner claims `numIn + 1` GPIOs from a pool of
  at most 8 (`partsFreeGpios`, gi < 8), so authored sets stay at <= 6
  driven inputs (7 absolute max; 2 fewer when a non-I2C0 OLED holds two
  GPIOs). The original quad-gate seeds drove 8 inputs and could never run
  - rewritten to 3-of-4 gates. Widening the pool means touching the UART
  GPIOs (gi 8/9) - Kevin's call, not taken by default.
- **VEE strap-as-input** (landed): the generator now lets a power-class
  pin with role NONE (the 4051/52/53's VEE) be DRIVEN as a vec input - a
  GPIO held low IS the datasheet's single-supply VEE=VSS strap. Zero
  firmware change. If the user already wired that row the runner refuses
  (honest refusal, not a misfire).
- **Candidate cap fix** (landed): `partsVectorIdentify` used to stop at
  `maxOut` (8) candidates TRIED - fatal once the database grew past 8
  sets per DIP width (the 74393 and the whole 4000 family sat past the
  cap). It now tries every candidate; the result array caps reported
  failures and a pass evicts a fail rather than being dropped.
- **Icc band** (landed): optional `icc: LO-HI` (mA) per record, checked
  against the INA feed reading the runner already takes at power-up;
  outside the band = verdict 0 with `failStep = -2` (`fail@icc` in the MP
  surface, measured mA appended to every candidate). Requires a fixed
  `vec_supply` - the band is supply-dependent. NOTE: the icc read happens
  BEFORE the input GPIOs are claimed, so chip inputs float during it -
  meaningless for logic (floating CMOS shoot-through), fine for op-amps
  (high-Z analog inputs). Author bands only on analog records.

## 2. The remaining tie classes

| Tie | Why vectors tie | Separator |
|---|---|---|
| 7404 vs 7414, 4011 vs 4093 | identical pins AND truth table | hysteresis sweep (§3) |
| LM358 vs TL072 vs NE5532 | identical pins, comparator vec | icc band (landed) |
| SPI displays (GMT177 et al) | no vectors possible on a SIP module | RDDID probe (§4) |
| 74HC00 vs 74HCT00 (alias-level) | one record covers the family | not a goal - report the record, 5.4 |

Everything else in the database now separates by pin map + truth table +
fingerprint (e.g. 74373 vs 74374: the D-change-while-pin-11-high step;
74273 vs both: pin 1 clears instead of tri-stating; 7447 vs 74595: the
all-G open-collector map).

## 3. DESIGN - hysteresis sweep (Schmitt vs plain)

When >= 2 vector survivors differ only in Schmitt-ness, sweep ONE input
with a DAC while watching one output, instead of GPIO levels:

1. Reuse the survivor's vec set: apply step 0's input levels, pick the
   first (in, out) pair - for the 7414 that is pin 1 -> pin 2; for the
   4093 hold the gate's other input high first.
2. Route DAC1 (not DAC0 - that is the 3.3V supply path in CMOS mode) to
   the input row. Step 0 -> 3.3V in ~50mV steps; record V_rise where the
   output flips. Step back down; record V_fall. Hysteresis = V_rise - V_fall.
3. Verdict: > 0.4V = Schmitt; < 0.15V = plain; between = tie stands
   (report both, 5.4).

Datasheet anchors (agent-verified 2026-08-30): 74HC14 at VCC=4.5V:
VT+ 1.7-3.15V, VT- 0.9-2.0V, hysteresis 0.4-1.4V (Nexperia); 74LS14 at
5V: VT+ ~1.6V, VT- ~0.8V, hyst 0.4 min / 0.8 typ. A plain 74HC04
switches near VCC/2 both directions with < 0.1V split. The 0.4V/0.15V
bounds clear every family's spec corners at either supply.

Cost: one new measurement primitive (DAC ramp + ADC watch on the existing
session fixture), a per-record flag or a derived pair from the vec set.
No schema growth if the sweep pins are derived per rule 1.

## 4. DESIGN - SPI display ID probe (Tier 2 for the SPI world)

I2C modules identify by WHO_AM_I; SPI displays can too. All
datasheet-verified (Sitronix ST7735S V1.1, ST7789V, Ilitek ILI9341 V1.11):

- **The one-wire trick**: ST77xx SDA is BIDIRECTIONAL in 4-line serial
  mode ("serial data input/output"). After clocking the read command out,
  tri-state the MOSI GPIO and keep clocking: the panel drives the same
  line. Modules without a MISO pin (the GMT177) are still readable.
- **The reads** (24-bit reads take ONE dummy clock after the command;
  8-bit reads take none):
  - `D3h` (RDID4) -> `00 93 41`: **ILI9341**, a hard silicon default
    (survives SW+HW reset, not module OTP). D3h does not exist on any
    ST77xx - a real ST77xx returns garbage/zeros, so this read is the
    first ask.
  - `04h` (RDDID) -> `85 85 52`: **ST7789V** power-on default.
  - `04h` -> `7C 89 F0`-class: **ST7735S** (ID1 default 0x7C; plain
    ST7735 is 0x5C; ID2/ID3 are panel-maker OTP - match ID1, log the rest).
  - ILI9341 reads return on SDO where the board wires one (Interface II);
    the bidirectional-SDA path is Interface I.
- **Safety shape**: same as the I2C probe - power the module
  current-limited (displays: expect 10-40mA with backlight OFF - do not
  drive BL), CS low only around each read, all lines released on exit.
  9 clocks of all-ones + CS toggle as the pre-read bus flush.
- **Where it hangs**: a SIP cluster whose rails were named and whose pin
  count/roles match an SPI-display record (CS+DC+SCK+MOSI present) - the
  same gate `partsProbeClusterI2C` uses, SPI flavored. New partdb field
  when implemented: `spi_id: D3=009341` / `spi_id: 04=858552&FFFFFF` -
  mirror the whoami grammar.

## 5. Bench verdicts (2026-08-30, Kevin's V5, this branch flashed)

- **CD4051 NAMED**: `part_vectors(31,8,38,1)` -> `4051:pass(2.3mA)`, all
  other candidates fail (`74595:fail@1`, the rest `fail@0`); the VEE
  strap-as-input ran live. Measured Tier-1 `fp=BBBBBBV-BBBBBBB-`
  (VEE reads V - one junction up to VDD, none down - with 0.62V Vf):
  zero mismatches against the authored `CCCCCB?-BBBCCCC-`.
- **74HC393 NAMED**: `part_vectors(41,7,47,11)` -> `74393:pass(3.7mA)`;
  the 7474 survived to step 2 before its edge test failed it. Measured
  `fp=BBBBBB-BBBBBB-` == the authored C-map.
- The 2.3mA "quiescent" on every CMOS candidate is floating-input
  shoot-through (icc reads before the input GPIOs claim) - re-confirms
  the §1 rule: icc bands belong on analog records only.
- Board state diffed CLEAN after both runs (capture/restore discipline).

## 5a. The held-state authoring law (scan run, 2026-08-30 15:05)

Kevin's Auto Scan failed the 74393 at its held-2QA check while the same
vec passed via bare `part_vectors` on the same board. Scan context (LED
traffic, read-fixture moves over the row NEXT to the async 2CLR pin) can
glitch stored async state between steps. The law, applied to the 393 and
binding for every future sequential vec:

- **Assert a stored HIGH only in the step whose edge created it** - the
  read happens immediately, before any other fixture traffic.
- **Every later check of that register expects LOW after a clear** - a
  glitch that corrupts the state still ends LOW, so the check is immune.
- Exposure audit of the other counters: 7490/74107/74161/4013/4017/4020/
  4024/4040/4060/4094 already read each count immediately after its edge
  and end in clear-to-LOW; their residual held-HIGH checks (4013 step 5,
  4017 steps 6-7, 4094 step 7) have small windows and no async pin
  adjacent to a read row - left standing until a bench run says otherwise.

Same session's other scan-context finds: the census roving GPIO was
never OWNED (core 2 could unmake the drive mid-census - the intermittent
every-LED-lit scans), and the second-look rails sampler was bottom-half
blind (voted vdd=INH on the 4051). Both fixed - see the commit.

## 5b. Bench debts carried forward
- 74HC90/74HC107 aliases assume the LS pin maps hold for HC (they do per
  Nexperia, but no HC90 exists at TI - the alias is thin).
- The 4046 and the monostable 74123 got static-only/no vectors on
  purpose: oscillators and pulse widths are invisible to static steps.
  The 74123's CLR must NEVER be released with A=L/B=H mid-run - the
  rising CLR itself triggers a pulse (TI SDLS043).
- The GMT177 record is the 8-pin retail variant; GoldenMorning's own
  7-pin GMT177-02 drops BLK - confirm the bench unit's pin count.
