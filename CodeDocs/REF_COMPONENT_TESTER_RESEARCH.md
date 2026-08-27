# Automatic component identification — engineering brief for Jumperless V5

Primary sources are read at source level, not summarized from blogs. Where the Kübbeler
documentation and the C source disagree on a number, **the code wins** and both are given.

| What | Where |
|---|---|
| Kübbeler TransistorTester C source | https://github.com/kubi48/TransistorTester-source (`trunk/`) |
| Core 3-pin decision tree | `trunk/CheckPins.c` (1468 lines) |
| Resistance | `trunk/GetResistance.c` |
| Capacitance | `trunk/ReadCapacity.c`, `trunk/GetRLmultip.c` |
| Discharge / safety | `trunk/EntladePins.c`, `trunk/ChargePin10ms.c` |
| Reverse leakage | `trunk/GetIr.c` |
| Docs (LaTeX, per-section) | https://github.com/svn2github/transistortester/tree/master/Doku/trunk/pdftex/english — `50-measurement.tex`, `51-semicon.tex`, `52-resistors.tex`, `53-capacitors.tex` |
| Docs (PDF) | https://github.com/kubi48/TransistorTester-documentation/blob/main/TransistorTester_english.pdf |
| Older English manual | https://www.mikrocontroller.net/attachment/140311/TransistorTester_eng_094k.pdf |

---

## 0. The one structural difference that changes everything

The AVR tester is a **fixed-resistor ratio bridge**. It has exactly two source impedances
(680 Ω "R_L", 470 kΩ "R_H") and infers *all* currents from the voltage across them. Every
threshold in `CheckPins.c` is really a current threshold in disguise.

Jumperless is **a source-measure unit**. `ISENSE_PLUS` (node 108) and `ISENSE_MINUS` (109)
are *routable crossbar nodes* with INA0's 2 Ω shunt R1 between them
(`src/Peripherals.cpp:1809` — `INA0.setMaxCurrentShunt(1, 2.0)`), so you can insert a
calibrated ammeter **anywhere in the circuit**, and DAC0 is a programmable ±8 V source whose
own total output current is measured by INA1 through a second 2 Ω shunt R57
(`src/guiding/GuideChecks.cpp:1742`).

This means **you do not need a reference resistor at all.** The topology already proven on
this bench (`GuideChecks.cpp:801-839`) is a genuine 4-wire force/sense ohmmeter:

```
  DAC0 ──── rowA ──[ DUT ]── rowB ──── ISENSE_PLUS
                                        [2 Ω shunt, INA0]
                                       ISENSE_MINUS ──── GND
  rowA ──── ADC(chA)        rowB ──── ADC(chB)        (high-Z sense legs)
```

> "No current flows in the sense legs (the ADC path is a ~1M divider), so every ohm of
> stimulus-path resistance drops out of V_rowA − V_rowB. That is the whole 4-wire fix:
> R = (V_A − V_B) / I_part contains no crosspoint count, no crosspoint resistance, no DAC
> output impedance and no sink." — `GuideChecks.cpp:836`

So: **port the AVR's decision *tree*, throw away its resistor *arithmetic*.**

### Measured hardware constants you should build on

| Quantity | Value | Source |
|---|---|---|
| ADC0–3, ADC7 full scale | 18.28 V span, −8.00 V zero → **−8.0 … +10.28 V** | `Peripherals.cpp:301-302` (`adcSpread`, `adcZero`) |
| ADC LSB on those channels | 18.28/4095 = **4.46 mV** | `Peripherals.cpp:2760` |
| ADC4 | 5.0 V span, 0 zero → **1.22 mV LSB** | `Peripherals.cpp:2654` |
| ADC leg loading on a row | **~1 MΩ divider** (≈1.2 MΩ to the offset ref) | `GuideChecks.cpp:836`, ratio 3.3/18.28 = 5.54:1 |
| INA0 / INA1 shunt | **2 Ω**, INA219 shunt reg 10 µV/LSB → **5 µA/LSB** | `Peripherals.h:150`, TI INA219 datasheet https://www.ti.com/lit/ds/symlink/ina219.pdf |
| INA calibrated current reg | 30.5 µA/LSB — **don't use it**, read the shunt register | `Peripherals.h:143` |
| INA sample cadence | 50 ms poll (`CURRENT_SENSE_POLL_INTERVAL_MS`), 16 averages | `Peripherals.h:32`, `GuideChecks.cpp:1701` |
| RP2350 internal pull | **50–80 kΩ, ~60 kΩ nominal** | `src/snakes/projectFiles.h:538` |
| Bare-row parasitic C | 60 kΩ × ~2 µs rise ⇒ **≈33 pF** | `projectFiles.h:538-539` |
| Routable GPIO | RP_GPIO_1..8 = nodes 131–138 (pins 20–27), plus GPIO_0/16/17/18/19 | `JumperlessDefines.h:468-496` |
| RP2350 ADC | 12-bit, 48 MHz ADC clock ÷96 = 500 kSps ⇒ **2 µs/sample**, `adc_set_clkdiv(1.0)` | `Peripherals.cpp:497` |
| PIO timing resolution | 150 MHz sysclk ⇒ **6.67 ns** | `Peripherals.cpp:3146` |

---

## 1. The AVR TransistorTester: the actual algorithm

### 1.1 The port model and the six permutations

Each of the three test pins is three AVR port bits: a direct ADC-port bit (hard drive to
GND/VCC through ~19 Ω high-side / ~22 Ω low-side), an R_L bit (680 Ω), and an R_H bit
(470 kΩ). Any pin can be **direct-VCC, direct-GND, 680 Ω to either rail, 470 kΩ to either
rail, or high-Z input**, and can be ADC-read regardless (`50-measurement.tex:29-37`).

`main.c` calls `CheckPins(HighPin, LowPin, TristatePin)` six times — all 3! orderings
(`50-measurement.tex` Table "all combinations of measurement"):

```
1. (1=+, 2=−, 3=T)   2. (1=+, 3=−, 2=T)   3. (3=+, 2=−, 1=T)
4. (2=+, 3=−, 1=T)   5. (3=+, 1=−, 2=T)   6. (2=+, 1=−, 3=T)
```

Pin roles inside `CheckPins`: **HighPin** is the assumed emitter/source/anode-side,
**LowPin** the assumed collector/drain/cathode-side, **TristatePin** the assumed
base/gate. Results accumulate in globals `ntrans`/`ptrans` (`.b/.c/.e/.hfe/.uBE/.current/
.ice0/.ices/.gthvoltage/.count`) and `diodes[]`; `PartFound`/`PartMode` from `part_defs.h`.

Two early-outs that matter for porting:

```c
// CheckPins.c:288 — stop searching once the picture is complete
if ((ntrans.count + ptrans.count) > 1) {
    if (!((ntrans.count == 1) && (ntrans.b == ptrans.b))) goto checkDiode;
}
```
FETs and thyristors increment `count` by **two** deliberately ("count FET as two to
accelerate searching", `CheckPins.c:1204`) because a FET's two orientations are
indistinguishable, so finding one ends the search.

### 1.2 The decision tree, verbatim thresholds

All voltages below are **millivolts on a 5.0 V rail** as returned by `ReadADC()`.
`vcc_diff(x)` = Vcc − x. The "as fraction of Vcc" column is what you port.

#### Stage A — is there current with the gate/base doing nothing? (depletion devices)

`CheckPins.c:181-216` (the `EXACT_OTR` path, used on everything ≥ ATmega168):
drive HighPin hard to VCC, LowPin through 680 Ω to GND, and measure LowPin with the
TristatePin (a) pulled to VCC through 680 Ω, (b) pulled to GND, (c) left open. Then repeat
with the common-emitter roles swapped for the N-channel case. Record
`vCEs` (current with gate held), `lp_otr` (current with gate open), and the two swing
magnitudes `v_change_p`, `v_change_n`.

```c
if ((v_change_n < 288) && (v_change_p < 288)) goto checkDiode;   // gate does nothing
if ((adc.hp2 + v_change_p) < (adc.vCEs + v_change_n)) { /* N-channel orientation wins */ }
...
if ((adc.vCEs > 115) && ((adc.vCEs + adc.vCEs + 20) > adc.lp_otr))   // CheckPins.c:304
```

`vCEs > 115 mV` across 680 Ω = **169 µA** of channel current with the gate held → depletion
candidate. The second clause (`2·vCEs + 20 > lp_otr`) is the **germanium guard**: a Ge BJT
also leaks, but its leakage *rises a lot* when the base is open, so if `lp_otr` is more than
about twice `vCEs` it is a leaky BJT, not a JFET
(`51-semicon.tex:19-25` states this as "higher than 115 mV and not 100 mV lower than the
open-gate reading").

Then, still `CheckPins.c:312-341`:

```c
R_DDR = LoPinRL | TriPinRH;      // gate via 470k to GND
adc.lp1 = W10msReadADC(LowPin);  adc.tp1 = ReadADC(TristatePin);
R_PORT = TriPinRH;               // gate via 470k to VCC
adc.lp2 = W10msReadADC(LowPin);
if (adc.lp2 > (adc.lp1 + 599)) {          // gate modulates the channel by >599 mV
    ...
    if (adc.lp2 > 3911) → N-channel **depletion MOSFET**
    else                → N-channel **JFET**   (gate-source junction conducts)
}
```

**JFET vs depletion-MOSFET is decided by gate current alone**: hold the gate hard, read the
gate node — a MOSFET gate floats to the rail (>3911 mV = 0.78·Vcc), a JFET's gate-channel
junction clamps it. P-channel mirror: `adc.tp2 < 977` (0.195·Vcc) → P-D-MOS, else P-JFET
(`CheckPins.c:450`).

Depletion-device parameters (`CheckPins.c:357-359`):
```c
ntrans.uBE       = adc.lp1;                                   // source voltage
ntrans.gthvoltage= |adc.lp1 - adc.tp1|;                       // Vgs at that current
ntrans.current   = adc.lp1 * 10000 / RR680MI;                 // Id in 1 µA units
ntrans.ice0      = |V(LowPin) - V(Tri)| with BOTH via 470k;   // ≈ Vgs_off (Pieter-Tjerk)
```
`Idss` is then **extrapolated** through the square law by `expand_FET_quadratic()`
(`CheckPins.c:7-36`): given (Vgs_off = v0, Vgs = v1, Id = ii), it iteratively grows v by
0.4 % and i by 0.8 % per step until v reaches v0 — i.e. it integrates
`Id ∝ (1 − Vgs/Vgs_off)²` — and **bails returning 0 if the extrapolated current would exceed
40 mA**, the AVR pin limit. If the estimate is under 40 mA it re-measures with *no* source
resistor at all and re-extrapolates from the higher point (`51-semicon.tex:378-393`).

#### Stage B — PNP / P-channel (`CheckPins.c:536-828`)

Common-collector (emitter-follower) hFE is measured **first**, before the type is even known,
because a Darlington's CE hFE is misleadingly low. Collector hard to GND, emitter through
680 Ω to VCC, base via 470 kΩ (falling back to 680 Ω if the base current is too small):

```c
if (adc.rhp > (100 + adc.lp_otr))     // enough emitter current above the leakage floor
    c_hfe = rhp_corrected * (R_H_VAL*10000 / RR680PL) / adc.tp1;    // 470k base resistor
else
    c_hfe = (|tmp16 - adc.tp1| * 100) / adc.tp1;                    // 680Ω base resistor
```

Then common-emitter: emitter (HighPin) hard to VCC, collector through 680 Ω to GND, base
through 680 Ω to GND:

```c
if (adc.lp1 > 3422)  {          // 0.684·Vcc — collector pulled up ⇒ conducting
    R_DDR = LoPinRL | TriPinRH; // switch base to the 470k resistor
    adc.lp1 = W10msReadADC(LowPin);
    adc.tp2 = ReadADC(TristatePin);   // base voltage
    adc.hp2 = ReadADC(HighPin);       // emitter voltage
    if (adc.tp2 > 2000)  → **PNP**    // base dragged up toward the emitter ⇒ base current
        e_hfe = (lp1 - lp_otr) * (R_H_VAL*10000 / RR680MI) / adc.tp2;
        ptrans.uBE = |adc.hp2 - adc.tp2|;             // Vbe
        ptrans.current = adc.lp1*10000/RR680MI;       // Ic, 1 µA units
    else if ((adc.lp_otr < 97) && (adc.lp1 > 2000))  → **P-channel E-MOSFET**
        if (adc.hp2 > adc.lp1 + 250)  → **P-channel IGBT**   // Vds too big for a MOSFET
}
```

Doc (`51-semicon.tex:75`) states this discriminator as "if the base voltage is greater
0.97 V it must be a PNP"; the shipped code uses **2000 mV**. Use the code.

#### Stage C — NPN / N-channel / thyristor / triac (`CheckPins.c:830-1207`)

Common-collector hFE first (same structure, mirrored), then:

```c
ADC_DDR = LoADCm; ADC_PORT = TXD_VAL;      // emitter hard to GND
R_PORT = R_DDR = TriPinRL | HiPinRL;       // collector AND base both via 680Ω to VCC
adc.hp1 = W5msReadADC(HighPin);
if (adc.hp1 < 4400) {                      // 0.88·Vcc — collector pulled down ⇒ conducting
```
(the comment shows this limit was raised from 1600 → 4500 → 4400 specifically "for
opto-coupler with low hFE", `CheckPins.c:947-949`.)

**Thyristor test** — two questions, both required (`CheckPins.c:959-981`):
```c
R_PORT = HiPinRL;  adc.hp4 = W5msReadADC(HighPin);   // gate pulled to GND
R_DDR  = HiPinRL;  adc.hp3 = W5msReadADC(HighPin);   // gate released → still latched?
R_PORT = 0; wait5ms(); R_PORT = HiPinRL;             // interrupt anode current, restore
adc.hp2 = W5msReadADC(HighPin);                      // must NOT re-trigger
if ((adc.hp3 < 1600) && (adc.hp2 > 4400) && ((adc.hp1+150) > adc.hp4)) → THYRISTOR
```
i.e. *(a) it stays on with no gate drive; (b) it turns off when the holding current is
removed and does not restart.* The third clause rejects a triac triggered in the swapped
A1/gate mode. Only low-power devices work — "the holding current of the tester can reach
only 6 mA" (`51-semicon.tex:122`).

**Triac test** then reverses polarity and requires the whole sequence again with thresholds
244 / 977 / 733 / 733 / 244 mV (`CheckPins.c:1004-1051`).

**BJT vs E-MOSFET — the single cleanest discriminator in the whole program**
(`CheckPins.c:1063-1087`): drive the base/gate through the **470 kΩ** resistor to VCC with
the collector through 680 Ω to VCC and the emitter at GND, then look at the base node:

```c
adc.rtp = vcc_diff(ReadADC(TristatePin));   // drop across the 470k
if (adc.rtp > 2557)  → **NPN**              // ⇒ Ib > 2557mV/470k = 5.4 µA ⇒ a junction
else if ((adc.lp_otr < 97) && (adc.rhp > 3400)) → **N-channel E-MOSFET**
        if (adc.hp2 > 250 + adc.lp2) → **N-channel IGBT**
```
A BJT base clamps ~0.7 V and sinks µA through the 470 k; a MOS gate does not, so the base
node sits at Vcc and `rtp ≈ 0`. `adc.lp_otr < 97 mV` (143 µA) is the "it is genuinely OFF
when the gate is off" guard that keeps depletion FETs out of the enhancement branch.

**Gate threshold voltage** (`CheckPins.c:1188-1203`) — discharge the gate 10 ms through
680 Ω, then charge it slowly through 470 kΩ while polling the *digital* input on the drain,
and read the gate ADC the instant the drain flips. **Eleven repeats**, accumulated raw ADC,
then `V = (Σ + 1) · 4 / 9` mV. Comment records the measured real input trip points:
`0 is detected with input voltage of 2.12 V to 2.24 V` and
`1 is detected with more than 2.5 V (up to 2.57 V)` on mega168/328 — i.e. **the method's
accuracy is bounded by the Schmitt trigger's real thresholds, which they measured rather
than trusted.** `51-semicon.tex:363-365` notes a fast gate (small Cgs) reads slightly wrong:
BS250 moves 2.6 V → 2.5 V when 10 nF is added gate-source.

#### Stage D — diode (`CheckPins.c:1216-1407`)

Entry gate: `if (adc.lp_otr < 455) goto widmes;` — under 455 mV/680 Ω = **669 µA** there is
not enough forward current to be a diode; go measure resistance instead.

Pre-discharge, because a charged MOSFET gate fakes parts (`CheckPins.c:1237-1244`):
```c
R_DDR = HiPinRH;                       // 470k to ground during discharge, defeats leakage
for (ii=0; ii<200; ii++) {
   ADC_DDR = LoADCm | HiADCm; wait_about5ms();   // short both ends
   ADC_DDR = LoADCm; adc.hp1 = ReadADC(HighPin);
   if (adc.hp1 < (150/8)) break;       // ≈19 mV
}
```

Measure Vf at **both** currents, with the resistor first on the high side and then moved to
the low side, discharging the gate for P- then N-channel between (`CheckPins.c:1285-1337`) —
this is how the **body diode of a depletion MOSFET** is found: "We have to generate a
negative gate voltage to isolate the diode. For P-mode the resistors must reside on the VCC
side. For N-mode the resistors must be moved to the GND side."

Capacitor rejection (`CheckPins.c:1300-1316`): take the 680 Ω reading, then take it **again**
after the same wait. If the second reading rose by more than 20 mV, it is still charging →
capacitor, not diode.

The acceptance test (`CheckPins.c:1379-1384`) — `hp1` = Vf at 680 Ω, `hp2`/`hp3` = Vf at
470 kΩ:
```c
volt_dif = min(adc.hp3/8, 200);
tmp16    = (adc.hp1 < 1000) ? adc.hp1/100 : adc.hp1/16;
if ( adc.hp1 > 150 && adc.hp1 < 4640     // 0.03·Vcc … 0.928·Vcc
  && adc.hp2 < adc.hp1                   // low-current Vf must be LOWER (a cap would be ≥)
  && adc.hp1 > adc.hp3 + volt_dif        // ≥12.5 % higher at 690× the current
  && adc.hp3 > tmp16 )                   // but not 100× higher — that would be a resistor
    → DIODE, record Anode=HighPin, Cathode=LowPin, Voltage=hp1
```
The doc states the same rules as ratios (`51-semicon.tex:173-178`): Vf(680) must be
> 1.125 × Vf(470k) and 16 × Vf(470k) must be > Vf(680). The code comment explains the
`/100` variant: "for resistors the expected value is about `adc.hp1/670`, so `adc.hp1/100`
should also be OK to differ with resistor" — **a resistor's voltage scales with current, a
diode's does not. That single ratio test is the whole diode/resistor separation.**

Reverse leakage (`GetIr.c`): reverse-bias through 470 kΩ at 5 V,
`Ir[nA] = u_res·100000 / R_H_VAL` → **≈2 nA resolution**; if the drop exceeds 2500 mV
(5.3 µA) it re-measures with the 680 Ω and reports µA (`51-semicon.tex:183-186`).

#### Stage E — resistance (`GetResistance.c`)

Refuses to run if a transistor was found (`:72`) or if a diode was found on the same pin pair
(`:88-91`). Four measurements per direction, then the whole thing repeated in the opposite
direction:

```
type 1: 680Ω on the high side, read HighPin and LowPin
type 2: 680Ω on the low side,  read HighPin and LowPin
type 3: 470k on the high side, read HighPin
type 4: 470k on the low side,  read LowPin
```

**Settling loop** — the thing that keeps big caps and inductors from being read as resistors
(`GetResistance.c:99-108`):
```c
for (ii=1; ii<MAX_REPEAT; ii++) {          // MAX_REPEAT = 700/(5 + R_ANZ_MESS/8) ≈ 24
   adc.tp1 = W5msReadADC(LowPin);
   adc.hp1 = ReadADC(HighPin);
   if (|adc.hp1 - adc.hp2| < 3) break;     // 3 counts at U_SCALE=4 ⇒ 0.75 mV
   adc.hp2 = adc.hp1;
}
if (ii == MAX_REPEAT) goto testend;        // never settled ⇒ not a resistor
```

Range switch and formulas:
```c
if (adc.hp1 < 4400*U && adc.hp2 > 97*U)  goto testend;   // didn't break down ⇒ not a resistor
if (adc.hp2 >= 4972*U)                   goto testend;   // shorted
if (adc.lp1 < 169*U) {                                   // → use the 470k branch (>20 kΩ)
    if (adc.lp2 < 38*U) goto testend;                    // >60 MΩ ⇒ "no part"
    lirx1 = R_H_VAL * adc.hp2      / vcc_diff(adc.hp2);
    lirx2 = R_H_VAL * vcc_diff(adc.lp2) / adc.lp2;
    lrx1  = weighted_mean(lirx1, lirx2) * 100 + RH_OFFSET;   // RH_OFFSET = 3500 → +350 Ω
} else {                                                 // 680Ω branch
    lirx1 = RR680PL * (adc.hp1 - adc.tp1) / vcc_diff(adc.hp1);
    lirx2 = RR680MI * (adc.tp2 - adc.lp1) / adc.lp1;
    lrx1  = weighted_mean(lirx1, lirx2);
}
```
The weighting is 4:1 in favour of whichever reading is below `U_INT_LIMIT` (990 mV·U_SCALE),
because below 1 V the ADC switches to the 1.1 V internal reference and gets 4× the resolution
(`GetResistance.c:193-233`, `52-resistors.tex:60-64`).

Symmetry check (`GetResistance.c:240`): the two current directions must agree within
`|R1−R2|·10/(R1+R2+100) == 0` — i.e. **better than ~10 %**, otherwise it is not a resistor.

Claimed accuracy: **±1 % from 10 Ω to 20 MΩ after auto-calibration**, ±3 % before
(`52-resistors.tex:181-185`). Below 10 Ω "one resolution step results to a error of more
than 1 %" (`52-resistors.tex:143`).

#### Stage F — capacitance (`ReadCapacity.c`)

Two methods, chosen automatically. Both run *after* everything else, and only in three
pin combinations rather than six.

**Big caps (≳ 50 µF), `ReadCapacity.c:121-229`:** measure residual voltage first (§4.3),
then up to `MAX_LOAD_TIME = 500` pulses of exactly 10 ms through the 680 Ω, reading the
voltage **with no current flowing** after each:
```c
if (ovcnt16 > MAX_LOAD_TIME/4 && cap_voltage1 < MIN_VOLTAGE/4) break;  // 75 mV by 125 pulses
if (cap_voltage1 > MIN_VOLTAGE /*300 mV*/)                     break;  // enough
```
Then a self-discharge check: wait the *same* time it took to charge, re-read, and
```c
vloss = (cap_voltage2 * 1000) / cap_voltage1;    // "Vloss" in 0.1 %
if (cap_voltage2 > 200) goto keinC;              // lost >200 mV ⇒ leaky, not a capacitor
cap.cval = (ovcnt16+1) * GetRLmultip(cap_voltage1 + cap_voltage2);
```
`GetRLmultip()` interpolates a table of the closed-form charge equation on 25 mV spacing
from 300 to 1400 mV — the AVR has no cheap `log()`, so the transcendental is tabulated
(`GetRLmultip.c`). The **leakage correction is genuinely effective**: 68 µF alone reads
66.5 µF; with a 2.2 kΩ deliberately in parallel it reads 66.3 µF, where a Peaktech 3315 DMM
reads 192 µF (`53-capacitors.tex:98-104`).

**Small caps, `ReadCapacity.c:257-374`:** charge through 470 kΩ and time it with the 16-bit
Timer1 **input-capture triggered by the analog comparator** against the 1.1 V bandgap. The
whole point is that the capture register latches the counter in hardware, so the resolution
is one CPU clock (125 ns @ 8 MHz), not one polling loop. Overflow counting handles up to
12 s. Result `= (ovcnt16:tmpint) · RHmultip / (F_CPU/10000)`, minus a per-pin-pair zero
offset from EEPROM (`c_zero_tab[pin_combination]`), plus a slew-rate correction
`COMP_SLEW1/(cval+COMP_SLEW2)` for the smallest values. Floor: **50 pF @1 MHz, 25 pF @8 MHz**
(`ReadCapacity.c:432-445`).

Rejections: skip if a resistor was already found (`:84`); skip if a diode with
Vf < 1500 mV sits across the same pins (`:91-99`); refuse to call it a capacitor at the end
if any diode was found and it is not a FET (`:448`).

### 1.3 hFE, both circuits

The tester computes both and **keeps the larger** (`CheckPins.c:738`, `:752`, `:1126`,
`:1139`). Stated reason: "In order to prevent detecting the PNP in the inverse mode
(collector and emitter are swapped), the measurement with the higher current amplification
is taken as the right one" (`51-semicon.tex:84-87`).

**Common collector** (`51-semicon.tex:56-61`), PNP shown:
```
UB > 9 mV with the 680 Ω base resistor:   hFE = (UE − UB) / UB
UB < 10 mV → redo with the 470 kΩ:        hFE = (UE · 470000) / (UB · (680 + 22))
```
**Common emitter** (`51-semicon.tex:78`, `:130`), 470 kΩ base resistor always:
```
PNP:  hFE = ((UC − UC0) · 470000) / (UB · (680 + 19))
NPN:  hFE = ((VCC − UC − UC0) · 470000) / ((VCC − UB) · (680 + 22))
```
`UC0` is the collector voltage **with no base current** — the residual/leakage subtraction,
which is why germanium parts read sanely (`51-semicon.tex:320-325`). The effect is dramatic:

| Part | Markus F. original | Kübbeler CC | Kübbeler CE | Note |
|---|---|---|---|---|
| BC517 (NPN Darlington) | hFE 797, Vbe 1438 mV | **25 100**, 1.22 V | 764, 1.23 V | `51-semicon.tex:276, 299-305` |
| BC516 (PNP Darlington) | — | **76 200**, 1.20 V | 760, 1.23 V | |
| AC116-65 (Ge PNP) | 505, 378 mV | **72**, 149 mV | — | leakage-corrected |
| AD162 (Ge PNP) | 2127, 280 mV | **89**, 107 mV | — | |

**A Darlington reads ~30× higher in common-collector than common-emitter. If you only
implement one circuit, you will get Darlingtons wrong by a factor of 30.**

`ICE0` / `ICES` (`CheckPins.c:773-774`) are recorded from the two "no base drive" readings:
`ICE0 = lp_otr·10000/RR680MI` (base open), `ICES = vCEs·10000/RR680MI` (base shorted to
emitter), both in 1 µA units.

### 1.4 Complete threshold table (5 V rail → your rail)

| Code site | AVR value | As current or fraction | Meaning |
|---|---|---|---|
| `CheckPins.c:258` | 288 mV swing | 424 µA | gate/base drive "did something" |
| `CheckPins.c:304` | vCEs > 115 mV | 169 µA | depletion / leakage candidate |
| `CheckPins.c:304` | 2·vCEs+20 > lp_otr | — | germanium-BJT guard vs JFET |
| `CheckPins.c:329` | lp2 > lp1 + 599 mV | 0.12·Vcc | channel is gate-modulated |
| `CheckPins.c:339` | gate > 3911 mV | 0.78·Vcc | N-D-MOS (no gate current) vs N-JFET |
| `CheckPins.c:450` | gate < 977 mV | 0.195·Vcc | P-D-MOS vs P-JFET |
| `CheckPins.c:657` | lp1 > 3422 mV | 0.684·Vcc | P-side device is conducting |
| `CheckPins.c:674` | base > 2000 mV | 0.40·Vcc | PNP (base follows emitter) vs P-MOS |
| `CheckPins.c:780` | lp_otr < 97 mV | 143 µA | genuinely off ⇒ enhancement mode |
| `CheckPins.c:785` | Vds > Vgs-node + 250 mV | 0.05·Vcc | IGBT rather than MOSFET |
| `CheckPins.c:949` | hp1 < 4400 mV | 0.88·Vcc | N-side conducting (opto-friendly) |
| `CheckPins.c:978` | hp3 < 1600 && hp2 > 4400 | 0.32 / 0.88·Vcc | thyristor latch + turn-off |
| `CheckPins.c:1087` | rtp > 2557 mV | Ib > 5.4 µA | **BJT, not FET** |
| `CheckPins.c:1166` | rhp > 3400 mV | 5.0 mA | FET is fully on |
| `CheckPins.c:1221` | lp_otr < 455 mV | 669 µA | too little current for a diode |
| `CheckPins.c:1243` | hp1 < 19 mV | — | discharged enough to test |
| `CheckPins.c:1308` | rise > 20 mV on repeat | — | capacitor, not diode |
| `CheckPins.c:1384` | 150 < Vf < 4640 mV | 0.03–0.928·Vcc | plausible forward voltage |
| `GetResistance.c:169` | hp1<4400 && hp2>97 | — | no breakdown ⇒ not a resistor |
| `GetResistance.c:177` | hp2 ≥ 4972 mV | 0.994·Vcc | shorted |
| `GetResistance.c:180` | lp1 < 169 mV | ⇒ R > 20 kΩ | switch to the 470 kΩ branch |
| `GetResistance.c:181` | lp2 < 38 mV | ⇒ R > 60 MΩ | out of range |
| `GetResistance.c:240` | mismatch ≥ 10 % | — | asymmetric ⇒ not a resistor |
| `ReadCapacity.c:122,151` | 300 mV in ≤500×10 ms | — | big-cap charge target |
| `ReadCapacity.c:146` | <75 mV after 125 pulses | — | give up, >100 mF or shorted |
| `ReadCapacity.c:170` | >1300 mV in 1 pulse | 0.26·Vcc | too fast ⇒ small-cap method |
| `ReadCapacity.c:214` | Vloss > 200 mV | — | leaky ⇒ not a capacitor |
| `EntladePins.c:49,69` | <1000 / <1300 mV | 0.20 / 0.26·Vcc | safe to hard-short to GND |
| `EntladePins.c:13,82` | 10 s | — | give up → report "Cell!" (a battery) |

---

## 2. Porting the tree to Jumperless

### 2.1 Your three source impedances, and the order to use them

| # | Source | Impedance | Max current | Current readback | Use for |
|---|---|---|---|---|---|
| 0 | nothing (high-Z ADC read) | ~1.2 MΩ (the ADC leg) | — | — | **mandatory first touch** |
| 1 | internal pull, output disabled | ~60 kΩ + 130 Ω path | 3.3/60k = **55 µA** | ΔV across the pull, 74 nA/LSB | the AVR's 470 kΩ role: base/gate bias, leakage, high-R |
| 2 | DAC0 → ISENSE → GND | 132 Ω + DUT, **servo-able** | limited in software | **INA0 direct, 5 µA/LSB** | everything quantitative |
| 3 | GPIO push-pull | ~130 Ω | 3.3/130 = **25 mA** | none | fast edges, RC timing, hard clamps |

**25 mA exceeds your 20 mA junction budget.** Rule: never hard-drive (#3) a node until #1 or
#2 has established a **total loop resistance ≥ 165 Ω** (i.e. DUT ≥ ~35 Ω on top of the ~130 Ω
crossbar path). That is the single most important safety inversion versus the AVR, whose
680 Ω made hard-drive-into-anything intrinsically safe at 7.3 mA. The existing guide code
already carries a **50 mA INA watchdog** on its stimulus chain (`GuideChecks.cpp:1194`) —
reuse it, and tighten it to 20 mA for junction work.

### 2.2 The stimulus harness for a 3-terminal part

Seven ephemeral bridges, one refresh (you have 8 slots; `GuideChecks.cpp:833` already spends
5 on the 2-row case):

```
  DAC0 ──────────── rowH                       (force,  INA1 = total DAC0 current)
  rowL ──── ISENSE_PLUS [2Ω INA0] ISENSE_MINUS ──── GND   (return, INA0 = I_DUT)
  RP_GPIO_n ─────── rowT                       (gate/base drive: pull / push-pull / high-Z)
  rowH ──── ADCa    rowL ──── ADCb    rowT ──── ADCc      (sense, ~1.2 MΩ each)
```
Six permutations = six refreshes. Budget ≈ 50 ms per INA sample (`CURRENT_SENSE_POLL_INTERVAL_MS`,
and *do not* rewrite INA0's config — `Peripherals.h:148`), so use the ADCs for classification
and the INA only for the numbers you print. Realistic: ~2 s for a full 3-terminal identify.

### 2.3 Rescaled decision tree

`V_H`, `V_L`, `V_T` are the three sense-leg voltages. `I` = `inaShuntCurrent_mA()` =
`shuntVoltage_mV / 2.0`. `V_S` is the DAC0 setpoint. `R_pull` is the *self-calibrated*
pull for the GPIO in use (§2.5).

```
── 0. SAFETY ────────────────────────────────────────────────────────────────
  read rowH, rowL, rowT on ±8 V-capable ADC channels with NOTHING driven.
  if any |V| > 0.1 V  → staged discharge (§7). Never route a charged row to a 3.3 V pin.

── 1. TWO-TERMINAL PRESENCE (rowT high-Z) ───────────────────────────────────
  V_S = +1.0 V.  measure I.
  if I < 5 µA (1 LSB)     → open at 1 V. Go to 6 (high-R / capacitance).
  if I > 6 mA at 1.0 V    → R < 165 Ω. Clamp: drop V_S, never hard-drive this pair.

── 2. GATE/BASE MODULATION (the AVR's 288 mV test) ──────────────────────────
  V_S = +3.0 V (or the highest that keeps I ≤ 15 mA).
  I_open = I with rowT high-Z
  I_lo   = I with GPIO driving rowT low  through the pull-down
  I_hi   = I with GPIO driving rowT high through the pull-up
  ΔI = max(|I_hi − I_open|, |I_lo − I_open|)
  if ΔI < 0.6 mA          → not a 3-terminal device on this permutation → go to 5.
  (0.6 mA is the AVR's 288 mV/680 Ω = 424 µA scaled for your noisier 50 ms INA path.)

── 3. DEPLETION vs ENHANCEMENT ──────────────────────────────────────────────
  if I_open > 0.2 mA and I_open < 2 × I_(gate held off)   → DEPLETION candidate
        (the second clause is the germanium-BJT guard, CheckPins.c:304)
     hold rowT via the pull to each rail, read V_T:
        V_T within 0.15 V of the rail it is pulled to  → **D-MOSFET**  (no gate current)
        V_T clamped ≥ 0.3 V away from that rail        → **JFET**      (gate junction conducts)
     measure Vgs_off with the ±8 V DAC on the gate — you can pinch off parts the
     AVR cannot (its limit is 5 V; yours is 8 V).
     Idss: square-law extrapolation, `expand_FET_quadratic` (CheckPins.c:7), abort
     above your own current ceiling exactly as the AVR aborts above 40 mA.

── 4. BJT vs E-MOSFET vs IGBT ───────────────────────────────────────────────
  Bias rowT through the **pull only** (no push-pull) toward the rail that turns
  the device on. Read V_T.
     |V_T − V_drive| > 0.5 V  → base current is flowing  → **BJT**
                                Ib = (V_drive − V_T)/R_pull_eff    (§2.5)
                                Vbe = |V_T − V_emitter|
     |V_T − V_drive| < 0.15 V → no gate current          → **MOSFET / IGBT**
     N-type if it conducts with rowT pulled high, P-type if with rowT pulled low.
     IGBT: with the gate fully on, |V_H − V_L| > 0.25 V ⇒ IGBT, else MOSFET
           (CheckPins.c:785/1171; the 250 mV constant is a Vce(sat) floor and
            does NOT need rescaling — it is a device property, not a rail fraction).
     Darlington: Vbe > 1.0 V. Run the collector from the DAC at ≥5 V, not 3.3 V,
                 or a Darlington plus a shunt drop leaves no Vce headroom.

── 5. THYRISTOR / TRIAC ─────────────────────────────────────────────────────
  Only reachable when step 4 found conduction that persists. Three questions, all
  through the current-limited DAC:
     (a) pulse the gate, remove gate drive     → I stays        (latched)
     (b) set V_S = 0 briefly, restore          → I does NOT return (turned off)
     (c) gate held at the cathode rail         → I unchanged     (rejects a
                                                  mis-oriented triac)
  Your holding current is DAC-limited; the AVR manages only 6 mA, you can do
  more, but stay under 20 mA. Repeat with V_S negative for the triac test —
  the ±8 V DAC makes the reverse quadrant a first-class test instead of a
  polarity-swap hack.

── 6. DIODE ─────────────────────────────────────────────────────────────────
  Vf_hi = V_H − V_L servoed to I = 5 mA   (or the highest safe current)
  Vf_lo = V_H − V_L with the 60 kΩ pull only (I ≈ 40 µA)
  ACCEPT as a diode iff
     0.10 V < Vf_hi < 7.5 V                      (upper bound = your DAC headroom;
                                                  the AVR's 4.64 V ceiling was its rail)
     Vf_hi > 1.125 × Vf_lo                       (51-semicon.tex:174)
     16 × Vf_lo > Vf_hi                          (else it is a resistor)
     re-read Vf_hi after the same dwell: rise < 20 mV  (else it is a capacitor)
  A 125× current ratio (5 mA / 40 µA) predicts ΔVf = n·V_T·ln(125) ≈ 125 mV for
  silicon (n=1) and ~250 mV for an LED (n≈2). Anything with ΔVf < 30 mV over
  that ratio is a resistor; anything with ΔVf > 0.4 V is a series stack.

── 7. RESISTOR ──────────────────────────────────────────────────────────────
  4-wire: R = (V_H − V_L) / I.  Settle first (§3), then require the
  reverse-polarity measurement to agree within 10 % (GetResistance.c:240).

── 8. CAPACITOR ─────────────────────────────────────────────────────────────
  Only if 6 and 7 both refused. §4.
```

### 2.4 Formulas in your measurable quantities

Let `V_x` = `readAdcVoltage(ch)` in volts, `I` = `inaShuntCurrent_mA()` in mA,
`t` in µs from `time_us_64()` or a PIO cycle count at 6.67 ns.

```
Resistance (4-wire, the workhorse)
    R[Ω] = 1000 · (V_H − V_L) / I[mA]

Forward voltage at controlled current (servo DAC0)
    V_next = V_now + (I_target − I_now) · (132 + n·26/I_now[mA])       [volts, n=1 Si, 2 LED]
    Vf = V_H − V_L      ← report ALWAYS with the current it was taken at

Base current (below ~100 µA, where the INA has < 20 LSB)
    Ib[A] = (V_drive − V_T) / R_pull_eff
    R_pull_eff = 1 / (1/R_pull + 1/R_adcleg)          R_adcleg ≈ 1.2 MΩ  ← do not skip this,
                                                       it is a 5 % error on a 60 kΩ pull
Collector current
    Ic[A] = I / 1000                                   (INA0, 5 µA/LSB)

hFE, common emitter
    hFE = Ic / Ib = (I[mA]/1000) · R_pull_eff / (V_drive − V_T)
hFE, common collector (ISENSE moved into the emitter leg)
    hFE = Ie / Ib − 1
    → compute both, KEEP THE LARGER (CheckPins.c:738)

Vbe / Vgs(th)
    Vbe    = V_T − V_emitter                           (BJT, at the stated Ic)
    Vgs_th = V_T at the instant the drain node crosses a CALIBRATED digital threshold;
             repeat 11× and average (CheckPins.c:1190). Calibrate the threshold per
             pin by ramping DAC1 on the routed row and watching for the flip — the AVR
             team measured theirs at 2.12–2.24 V falling / 2.5–2.57 V rising rather
             than trusting the datasheet.

Reverse leakage
    Ir[A] = (V_drive − V_node) / R_pull_eff            → 74 nA/LSB with a 60 kΩ pull
    sub-100 nA: charge the row to +8 V, float everything, sample the ±8 V ADC
                intermittently:  Ir = C_row · dV/dt.  With C_row ≈ 33 pF, 1 nA gives
                30 mV/s. The ADC leg's own ~1.2 MΩ must be disconnected between
                samples or de-embedded — at 8 V it drains 6.7 µA and swamps everything.

Capacitance
    exponential fit (best):  V(t) = V∞(1 − e^(−t/τ));  regress ln(V∞ − V) on t
                             C = τ/R_series − C_row0        (C_row0 ≈ 33 pF, per row pair)
    threshold crossing:      C = t_th / (R · ln(V∞/(V∞ − V_th)))
    pulse counting (big C):  C = N·T_pulse / (R · ln(V∞/(V∞ − V_reached)))
```

### 2.5 Self-calibrating the internal pull (mandatory)

The RP2350 pull is 50–80 kΩ (`projectFiles.h:538`) — ±25 % out of the box, and every
formula in §2.4 that produces a base current, a leakage, or a high resistance is directly
proportional to it. Do this once at boot and store it:

1. **Absolute, all-parallel.** Enable pull-ups on all eight of RP_GPIO_1..8 (nodes 131–138),
   route all eight to one row, route that row through ISENSE to GND. Current is
   8 × 3.3/60 k ≈ **440 µA = 88 INA LSB → 1.1 % quantization**. Read V_row on an ADC.
   `R_parallel = (3.3 − V_row)/I`, so the mean pull is `8 · R_parallel`.
2. **Matching, pairwise.** Pull-up on pin A, pull-down on pin B, tie them together, read the
   node: `V = 3.3 · R_pd/(R_pu + R_pd)`. Same die, same corner → the *ratio* is good to a
   couple of percent even though the absolute value is not. Eight of these pin down every
   individual pull relative to the group mean.
3. Combine: each pull known to ≈ ±2 %, versus ±25 % from the datasheet.

Do **not** attempt to calibrate a single pull directly against the INA — one pull is 55 µA =
11 LSB and you get ±9 %.

### 2.6 RP2350 erratum E9 — this one bites you specifically

Per the RP2350 datasheet erratum, when a GPIO (0–47) is configured as an input with the
input buffer enabled and **the pad voltage sits between the logic levels**, leakage can reach
**120 µA at IOVDD = 3.3 V**, and the pin can latch at ~2.1–2.2 V; the recommended workaround
is an external pull-down of **8.2 kΩ or less**, or disabling the input buffer (IE) when not
actively reading.
https://hackaday.com/2024/09/20/raspberry-pi-rp2350-e9-erratum-redefined-as-input-mode-leakage-current/ ·
https://forums.raspberrypi.com/viewtopic.php?t=375631 ·
https://github.com/earlephilhower/arduino-pico/issues/2380

**Why it matters here:** a component tester deliberately parks nodes at intermediate
voltages. 120 µA is *more than twice* the 55 µA your internal pull sources, and it is **more
than the entire base current** you are trying to measure. Consequences:

- Never use a bare GPIO input as a high-Z sense point on an unknown node. Use an ADC channel.
- When a routable GPIO is acting as a 60 kΩ pull, its own input buffer must be **off**
  (`gpio_set_input_enabled(pin, false)`), or the erratum current adds to the pull current and
  your R_pull calibration is garbage.
- The digital-threshold trick for Vgs(th) and for RC timing *requires* the input buffer, so
  during those windows you are exposed. Keep those windows short, and prefer the ADC-based
  exponential fit over threshold crossing wherever the timescale allows.
- Check your silicon's stepping. A2 is affected; later steppings improved it —
  https://hackaday.com/2025/07/31/raspberry-pi-rp2350-a4-stepping-addresses-e9-current-leakage-bug/

---

## 3. Resistance without a reference resistor

**You have three methods; use them in this order.**

**(a) 4-wire force/sense — 0.1 Ω to ~200 kΩ.** `R = (V_H − V_L)/I`. The error budget, using
your real numbers (ADC LSB 4.46 mV ⇒ σ = LSB/√12 = 1.29 mV per channel, 1.82 mV on the
difference; shunt LSB 10 µV ⇒ σ = 2.9 µV ⇒ 1.44 µA):

| R | best safe V_S | I | V_H−V_L | δR/R (1 sample) | δR/R (100 avg) |
|---|---|---|---|---|---|
| 1 Ω | 2.0 V | 15 mA | 15 mV | 12 % | 1.2 % |
| 10 Ω | 2.7 V | 19 mA | 190 mV | 1.0 % | 0.1 % |
| 100 Ω | 3.0 V | 12.9 mA | 1.29 V | 0.14 % | — |
| 1 kΩ | 3.0 V | 2.65 mA | 2.65 V | 0.09 % | — |
| 10 kΩ | 8.0 V | 0.79 mA | 7.9 V | 0.19 % | — |
| 100 kΩ | 8.0 V | 80 µA | 7.99 V | 1.8 % | 0.18 % |
| 1 MΩ | 8.0 V | 8 µA (1.6 LSB) | — | **useless** | — |

Random error is not the limit — **systematic error is.** Above ~100 Ω you are limited by
the ADC gain calibration (`adcSpread`) and the 2 Ω shunt tolerance, so budget **±1 %** as
delivered accuracy, matching what the AVR achieves with 1 % reference resistors
(`52-resistors.tex:181`). The 4-wire topology is what buys you that without any reference
resistor: crosspoint resistance, DAC output impedance and the shunt's own drop all cancel.

**(b) Pull-divider — 20 kΩ to ~10 MΩ.** Enable pull-up on a routable GPIO into rowH, rowL to
GND, read `V_H`:
```
R_x = R_pull_eff · V_H / (3.3 − V_H)
```
Error sources, in order of size:
1. `R_pull` absolute tolerance — ±25 % raw, ±2 % after §2.5. **This dominates unless you calibrate.**
2. The ADC sense leg's ~1.2 MΩ in parallel with R_x. At R_x = 100 kΩ that is an **8 % error**;
   at 1 MΩ it is **45 %**. De-embed it: `1/R_x = 1/R_measured − 1/R_adcleg`, and calibrate
   `R_adcleg` by measuring one known resistor.
3. Erratum E9 leakage if the input buffer is left on — up to 120 µA against a 55 µA source.
   This can *invert* the answer. Turn IE off.
4. ADC quantization: worst at the ends of the divider. Keep `0.15 < V_H/3.3 < 0.85`, which
   is exactly the AVR's own `169 mV`→`4400 mV` window rescaled (`GetResistance.c:169,180`).

**(c) RC decay — 10 MΩ and up.** Charge the row, float it, and watch it bleed:
`R = τ/C` with `C` measured by §4 on the same node. This is the only method that reaches
the AVR's 60 MΩ ceiling, and it is the same primitive as the leakage measurement.

**Steal these two guards from `GetResistance.c` regardless of method:**
- **Settle before you believe it** (`:99-108`): loop until two consecutive readings differ by
  < 0.75 mV, up to ~24 iterations; if it never settles, it is a capacitor or an inductor, not
  a resistor. This one loop is why the AVR does not report electrolytics as resistors.
- **Reverse the polarity and require 10 % agreement** (`:240`). Free, and it catches diodes,
  semiconductor junctions and anything with an offset.

---

## 4. Capacitance

### 4.1 What is practical on this hardware

| Range | Method | R used | Timing | Resolution |
|---|---|---|---|---|
| 5 pF – 300 pF | PIO edge timing to a **calibrated** digital threshold | 60 kΩ pull | 6.67 ns/tick, τ = 2–18 µs → 300–2700 ticks | ~0.3 %, floor set by the 33 pF row offset |
| 100 pF – 10 µF | free-run ADC at 2 µs + exponential fit | 60 kΩ pull | τ = 6 µs – 0.6 s | < 1 % |
| 1 µF – 50 mF | free-run ADC + fit, or pulse counting | 132 Ω (DAC or GPIO) | τ = 132 µs – 6.6 s | < 1 % |
| > 50 mF | 10 ms pulse counting, AVR-style | 132 Ω | up to 500 pulses = 5 s | few % |

The **exponential fit is strictly better than threshold crossing on this hardware** and you
should default to it: the RP2350 has no user-accessible analog comparator peripheral (the
AVR's whole small-cap method is built on one), but it has a 12-bit 500 kSps ADC with DMA. Capture 200–500 samples of the
charge curve, then regress `ln(V∞ − V(t))` against `t`; the slope is `−1/τ` and the fit is
completely immune to threshold uncertainty, to the Schmitt hysteresis, and to E9's latch
voltage. Weight the fit toward `0.1 V∞ < V < 0.8 V∞` — beyond that the log blows up the
quantization noise.

### 4.2 Zero offset is not optional

A bare row-pair is **≈33 pF** (60 kΩ into the crossbar+breadboard giving "a couple of
microseconds", `projectFiles.h:538`) and it varies per row pair and per route. The AVR
handles this with `c_zero_tab[pin_combination]` in EEPROM, measured during selftest for all
six pin orderings and rejected if outside 10–190 counts (`AutoCheck.c:399-401`). Do the same:
a per-row-pair (or at least per-chip-pair) zero table, measured on empty rows, subtracted
before display, and a "capacitance below the offset ⇒ mark uncalibrated" path
(`ReadCapacity.c:396`).

### 4.3 Residual voltage, and why the ±8 V ADC makes this easy

The AVR cannot read a negative voltage, so it lifts the low side to ~132 mV through the
680 Ω and takes the *difference* between the two pins to recover a signed residual voltage
(`ReadCapacity.c:110-118`, `53-capacitors.tex:49-56`). **You do not need this trick** — ADC0–3
and ADC7 read −8.0 to +10.28 V natively (`Peripherals.cpp:301-302`). Just read both ends.

### 4.4 Leakage correction (Vloss) — port this

After charging, wait the *same* elapsed time it took to charge, re-read, and:
```
vloss = 1000 · ΔV / V_charged                (per mille)
if ΔV > 0.2 · V_charged  → not a capacitor, it is leaking
C_corrected uses (V_charged + ΔV) as the reached voltage
```
(`ReadCapacity.c:198-228`). Effectiveness, measured: 68 µF ∥ 2.2 kΩ reads 66.3 µF vs 66.5 µF
alone; a Peaktech 3315 DMM reads 192 µF on the same load (`53-capacitors.tex:98-104`).

### 4.5 Discharge safety

See the safety section — but note the specific hazard the AVR does not have: **a capacitor charged above
3.3 V, or below 0 V, must never reach a GPIO.** Read the row on a ±8 V ADC channel first,
always.

---

## 5. LED and diode Vf, and colour from Vf

### 5.1 Measuring at controlled current

Servo DAC0 until INA0 reads the target, then take `Vf = V_H − V_L` from the two sense legs
(this is exactly what the existing guide check does, and it already reports the current with
the value — `GuideChecks.cpp:1917-1932`). Newton step:
`V_next = V + (I_tgt − I)·(132 + n·26/I[mA])`, n≈1 silicon, n≈2 LED. Converges in 2–3
iterations, ≈150 ms at the 50 ms INA cadence.

**Two currents, always.** A single Vf cannot distinguish a diode from a resistor. Take Vf at
~5 mA and at ~40 µA (pull only) and apply the AVR's ratio tests (§2.3 step 6). The 125:1
current ratio gives ΔVf ≈ 125 mV for silicon, ≈250 mV for an LED, ≈0 for a resistor.

### 5.2 The 3.3 V problem, and the fix

**Blue, white, violet and UV LEDs cannot be lit from a 3.3 V GPIO through 130 Ω.** Their Vf
starts at 2.48–3.1 V and reaches 3.7–4.4 V. Use DAC0 at +5 to +8 V. Same argument applies to
Zeners: the AVR tops out around 4.5 V; your ±8 V DAC plus the −8…+10.28 V ADC reads Zeners to
~7.5 V directly, and the Kübbeler doc's own workaround for IGBTs — "the available 5 V gate
voltage of the tester is insufficient… a battery with about 3 V connected to the gate pin can
solve this problem" (`51-semicon.tex:369-374`) — is replaced by simply setting DAC1 to +8 V.

### 5.3 Vf bands by colour

Ranges from the Wikipedia LED article's material/wavelength/Vf table
(https://en.wikipedia.org/wiki/Light-emitting_diode), which is sourced per material system:

| Colour | λ (nm) | Vf range (V) | Material |
|---|---|---|---|
| Infrared | > 760 | **< 1.9** | GaAs, AlGaAs |
| Red | 610–760 | **1.63 – 2.03** | AlGaAs, GaAsP, AlGaInP, GaP |
| Orange | 590–610 | **2.03 – 2.10** | GaAsP, AlGaInP, GaP |
| Yellow | 570–590 | **2.10 – 2.18** | GaAsP, AlGaInP, GaP |
| Green | 500–570 | **1.9 – 4.0** | InGaN, GaN, GaP, AlGaInP, AlGaP |
| Blue | 450–500 | **2.48 – 3.7** | InGaN, SiC, ZnSe |
| Violet | 400–450 | **2.76 – 4.0** | InGaN |
| Ultraviolet | < 400 | **3.1 – 4.4** | AlGaN, AlGaInN, AlN, BN, diamond |
| White | broad | **2.7 – 3.5** | blue die + phosphor |

**Honest classification rules.** The bands overlap badly — green spans 1.9–4.0 V because
"green" is made from two entirely different material systems (old GaP green ≈ 2.1 V, modern
InGaN green ≈ 3.2 V). What you can report reliably at ~5 mA:

| Measured Vf @5 mA | Verdict you may state |
|---|---|
| 0.15 – 0.45 V | Schottky or germanium (AVR measures Ge AC128 B-E at 272 mV, SK14 Schottky at 263 mV — `51-semicon.tex:230,244`) |
| 0.55 – 0.80 V | ordinary silicon PN junction |
| 1.0 – 1.5 V | Darlington B-E, two series junctions, or an IR LED |
| 1.6 – 2.2 V | **red / orange / yellow LED, or old GaP green.** Cannot be split further by Vf |
| 2.4 – 2.9 V | ambiguous: high-Vf red-orange, or a low-Vf blue/green InGaN part |
| 2.9 – 3.6 V | **InGaN: blue, modern green, or white.** Cannot be split by Vf |
| 3.6 – 4.4 V | violet / UV |
| > 4.5 V | series stack, or a Zener in reverse |

Reference point: the Kübbeler tester measures a green LED at **1.95–1.96 V, 4–5 pF**
(`51-semicon.tex:220`), which is a GaP part, not InGaN — proof that "green ⇒ 3.2 V" is wrong
as often as it is right.

**Two ways to actually resolve colour, both available to you and not to the AVR:**
1. **Vf vs temperature / vs current slope.** Measure Vf at 100 µA, 1 mA, 5 mA and fit the
   ideality factor n from `ΔVf = n·V_T·ln(I₂/I₁)`. InGaN parts run n ≈ 1.8–3; AlGaInP red
   runs n ≈ 1.5–2; silicon n ≈ 1. It separates material systems better than Vf alone.
2. **The LED as its own photodiode.** Reverse-bias the LED, then float it and watch the row's
   voltage decay on the ±8 V ADC while illuminating it with the board's own RGB LEDs at a
   known wavelength. An LED only photo-responds to light at or above its own bandgap, so a
   red LED responds to nothing you can shine at it, a green one responds to blue, and a blue
   one responds to nothing. Photocurrent is tens of nA — right at the `C_row·dV/dt`
   sensitivity from §2.4. This is the only method that genuinely determines colour.

---

## 6. I2C device detection

You already have the data structure for this: `PartDbI2cIdent` in `src/partdb/PartDb.h:129-136`
carries `{numAddrs, addrs[4], whoAmIReg, whoAmIValue, whoAmIMask, flags}` and the header
comment already states the right policy — *"read-only probe — only ever read declared
registers."* `partdb_i2cIdents[]` currently holds **8 rows** (`PartDbData.h:1217-1226`):
0x3C range-2 (SSD1306), 0x68/reg 0x75/val 0x68/mask 0x7E (MPU6050), 0x76/0xD0/0x60 (BME280),
0x76/0xD0/0x58 (BMP280), 0x20×8, 0x48×4, 0x50×8, 0x68 bare (DS1307). Everything below is
rows to add.

### 6.1 Scan etiquette

**Scan 0x08–0x77 only (112 addresses).** Both reserved groups per NXP UM10204 §3.1.12 Table 4
(https://www.nxp.com/docs/en/user-guide/UM10204.pdf):

| 7-bit | Hex | Purpose |
|---|---|---|
| `0000 000` | 0x00 | general call (R/W=0) / START byte (R/W=1) |
| `0000 001` | 0x01 | CBUS address |
| `0000 010` | 0x02 | reserved, different bus format |
| `0000 011` | 0x03 | reserved, future |
| `0000 1XX` | 0x04–0x07 | Hs-mode master code |
| `1111 0XX` | 0x78–0x7B | 10-bit addressing prefix |
| `1111 1XX` | 0x7C–0x7F | device ID |

i2c-tools 4.2 changed its default lower bound from 0x03 to **0x08** for exactly this reason.
**General call is not inert:** `0x00` followed by data `0x06` is a bus-wide software reset;
`0x00`+`0x04` rewrites programmable slave addresses. Never touch 0x00.

**Default to a zero-data write probe.** `START | addr<<1|0 | (ACK?) | STOP` — no data byte is
ever clocked, so the master can never be left holding a slave-driven SDA. This is what
`Wire.beginTransmission(a); Wire.endTransmission();` emits with no `Wire.write()` between.
Return codes (https://github.com/arduino/reference-en/blob/master/Language/Functions/Communication/Wire/endTransmission.adoc):
`0` ACK, `1` buffer overflow, `2` **NACK on address = nothing there**, `3` NACK on data,
`4` other/bus error, `5` timeout. Only 0/2/4/5 are reachable for a zero-length probe.
**Treat 4 and 5 as "the bus is sick", not "no device"** — run a recovery (9 SCL pulses with
SDA released, then a manual STOP) before continuing, or every remaining address reports 4.

**A read probe obligates you.** `START | addr<<1|1 | ACK | 8 clocks | NAK | STOP` — once the
slave ACKs a read address it drives SDA, so you *must* clock the byte and answer NAK before
STOP is even physically formable. A zero-length read is a documented hard hang on the ESP32
I2C peripheral (https://github.com/espressif/arduino-esp32/pull/2301).

**Linux `i2cdetect`'s mode heuristic**, from `scan_i2c_bus()` in
https://github.com/mozilla-b2g/i2c-tools/blob/master/tools/i2cdetect.c:
```c
case MODE_AUTO:
    if ((i+j >= 0x30 && i+j <= 0x37) || (i+j >= 0x50 && i+j <= 0x5F))
         cmd = MODE_READ;      /* write-quick corrupts the Atmel AT24RF08 */
    else cmd = MODE_QUICK;     /* read-byte locks write-only clock chips at 0x69 */
```
Man page (https://manpages.debian.org/bookworm/i2c-tools/i2cdetect.8.en.html): `-q` *"is
known to corrupt the Atmel AT24RF08 EEPROM found on many IBM Thinkpad laptops"*; `-r` *"is
known to lock SMBus on various write-only chips (most notably clock chips at address
0x69)"*; and the standing warning *"This program can confuse your I2C bus, cause data loss
and worse!"*

**Recommendation for Jumperless:** zero-data write probe everywhere. It is strictly safer
than a read for the whole bus, and it does not corrupt ordinary 24Cxx parts (only the broken
AT24RF08, which is not a breadboard part). If you ever do read-probe, guarantee the
clock-8-then-NAK.

### 6.2 Address hazard list

| Address(es) | Hazard |
|---|---|
| 0x00 | general-call reset / address reassignment |
| 0x04–0x07 | Hs-mode master codes |
| **0x70–0x77** | **TCA9548A/PCA9548A mux has no register pointer — the first data byte of any write IS the channel bitmask.** A probe with a payload silently reconfigures the bus mid-scan. Zero-data only. (https://www.ti.com/lit/ds/symlink/tca9548a.pdf) |
| **0x50–0x57** | EEPROM. A probe carrying 1 byte moves the address pointer; 2 bytes on a 1-byte-addressed 24Cxx **begins a page write and destroys data.** Zero data, or pure read. |
| 0x54–0x57, 0x5C | AT24RF08 quick-write corruption (the historical reason for i2cdetect's read exception) |
| **0x69** | write-only PC clock chips ACK a read address and never drive data → bus lock. Also the AD0-high MPU/ICM address, so an identifier *wants* to talk here. Quick-write first; read only after WHO_AM_I confirms an IMU. |
| 0x24 (PN532) | frame protocol with an IRQ/ready handshake; a bare register read leaves it mid-frame |
| 0x5A (MLX90614) | SMBus with **mandatory PEC** and repeated-START; a plain I2C read returns garbage or stalls |

### 6.3 Address → likely part → disambiguation probe

`R[x]` = write register x, repeated-START, read one byte. `RW[x]` = same, 16-bit big-endian.
`Cmd` = a raw command word (Sensirion/Aosong style, no register pointer).

| Addr | Likely parts | Probe → expected |
|---|---|---|
| 0x0D | QMC5883L | `R[0x0D]` = 0xFF (ambiguous with a failed read — require a valid status at 0x06 too) |
| 0x0E | MAG3110 | `R[0x07]` = 0xC4 |
| 0x18/0x19 | **LIS3DH**, LIS2DH12, LSM303AGR-A, MCP9808 | `R[0x0F]` = **0x33** ⇒ LIS3DH-class; else `RW[0x06]`=0x0054 & `RW[0x07]`=0x0400 ⇒ MCP9808 |
| 0x1E | **HMC5883L**, LIS3MDL, LIS2MDL, LSM303-M, FXOS8700, MMA845x | `R[0x0A,0x0B,0x0C]` = 0x48,0x34,0x33 ("H43") ⇒ HMC5883L; `R[0x0F]`=0x3D ⇒ LIS3MDL; 0x40 ⇒ LIS2MDL; `R[0x0D]`=0xC7 ⇒ FXOS8700; 0x1A ⇒ MMA8451 |
| 0x1D/0x53 | **ADXL345/343** | `R[0x00]` = **0xE5** (DEVID) |
| **0x20–0x27** | **MCP23017/23008**, **PCF8574** | MCP23017: `R[0x00]`=0xFF (IODIRA POR) and `R[0x0A]`=0x00 (IOCON). **PCF8574 has no registers at all** — it ignores the pointer and returns the live port byte, so `R[0x00] == R[0x0A]`. That equality is the discriminator. |
| 0x23 / 0x5C | **BH1750** | no ID register. Write opcode `0x10`, wait 180 ms, read 2 bytes, expect a plausible non-0xFFFF lux word |
| 0x28/0x29 | **VL53L0X**, BNO055, TSL2561, TSL2591, TCS34725, VL6180X | **order matters:** `R[0x00]`=0xA0 ⇒ BNO055; `R[0xC0]`=**0xEE** ⇒ VL53L0X; `RW[0x0000]`=0xB4 ⇒ VL6180X; `R[0x12\|0xA0]`=0x50 ⇒ TSL2591; **then** `R[0x0A\|0x80]`=0x50 ⇒ TSL2561. Checking 0x0A before 0x12 mis-IDs a TSL2591 as a TSL2561 — a filed bug (https://github.com/meshtastic/firmware/issues/9124) |
| **0x38** | **AHT10/AHT20/DHT20**, FT6206 touch, NCP5623 | AHT: send `0x71`, read status — bit3 (CAL) set, bit7 (BUSY) clear. **AHT10 init cmd = 0xE1, AHT20 init cmd = 0xBE** — that is how you split them. FT6206: `R[0xA8]`=0x11 (vendor) + `R[0xA3]`=0x06 (FT6206) / 0x36 (FT6236) |
| 0x39 | **APDS-9960**, AS7341, TSL2561 | `R[0x92]` = **0xAB** ⇒ APDS9960; 0x24 ⇒ AS7341 |
| **0x3C/0x3D** | **SSD1306 / SH1106 / SSD1309 / SSD1327 OLED** | **No chip-ID register.** A read returns a status byte (D7 busy, D6 display on/off), and many cheap modules are effectively write-only. Confirm by *address + elimination* and report "OLED (SSD1306-class)", not a specific part. Stronger: write GDDRAM and read back — SSD1306 is 128 columns, SH1106 is 132 (https://github.com/olikraus/u8g2/discussions/2088) |
| **0x40–0x4F** | **INA219**, **PCA9685**, Si7021/HTU21D/SHT21, INA226/260, TMP006 | ordered: (1) `R[0xE7]`=0x3A ⇒ SHT2x family, then `Cmd 0xFA 0x0F` electronic ID: 0x15 ⇒ Si7021, 0x32 ⇒ HTU21D. (2) `R[0x00]`=0x11 (SLEEP+ALLCALL POR) ⇒ PCA9685. (3) `RW[0xFE]`=0x5449 & `RW[0xFF]`=0x2260 ⇒ INA226, 0x2270 ⇒ INA260. (4) **INA219 has no ID register** — infer from *absence* of the TI 0xFE/0xFF signature plus a sane 16-bit config at 0x00 |
| 0x44/0x45 | **SHT3x/SHT4x** | SHT3x: `Cmd 0xF32D`, read 3 bytes, validate CRC-8 (poly 0x31, init 0xFF). SHT4x: `Cmd 0x89` serial |
| **0x48–0x4B** | **ADS1115/ADS1015**, **TMP102**, **LM75**, TMP117, ADT7410 | `RW[0x0F]`=0x0117 ⇒ TMP117; `R[0x0B]`=0xCB ⇒ ADT7410. **ADS111x has no ID register**; `RW[0x01]`=0x8583 at POR. **ADS1115 vs ADS1015 is not distinguishable over I2C** — run a single-shot conversion; ADS1015 is 12-bit left-justified so the low 4 bits are always 0. TMP102/LM75: no ID; heuristics only |
| **0x50–0x57** | **AT24Cxx / 24LCxx EEPROM**, MB85RC FRAM, ADXL345 (0x53) | no ID register. Write address `0x0000`, repeated-START, read 2 bytes; re-issue the *same* addressed read and confirm identical bytes (pointer reset), whereas a bare current-address read walks forward. Never write ≥2 bytes here. |
| **0x57** | **MAX30102/30105** vs **AT24C32 on a DS3231 module** | `R[0xFF]`=0x15 (PART_ID) is **not sufficient** — an EEPROM happily returns whatever is at offset 0xFF. Read 0xFF **four times**: MAX3010x returns 0x15 every time, an EEPROM's pointer auto-increments. Module hint: an RTC at 0x68 present ⇒ 0x57 is the AT24C32 |
| 0x58/0x59 | SGP30 / SGP40 | `Cmd 0x3682` → 9-byte serial; `Cmd 0x202F` feature set, product type 0 ⇒ SGP30 |
| **0x5A/0x5B** | **CCS811**, **MLX90614**, MPR121, DRV2605 | `R[0x20]`=**0x81** ⇒ CCS811. MLX90614 needs SMBus+PEC; EEPROM 0x3C–0x3F are ID words, cmd `0x2E` returns its own address (0x5A). MPR121: `R[0x5D]`=0x10 |
| 0x5C | LPS22HB/25HB/33HW, DHT12, AM2320, BH1750 | `R[0x0F]`=0xB1/0xBD ⇒ LPS2x. DHT12: read 5 bytes at 0x00, byte[4] == sum(bytes[0..3]) |
| 0x5D / 0x14 | GT911 touch | 16-bit register addressing: read 4 bytes at `0x8140` = ASCII `"911\0"` |
| **0x60–0x67** | **MCP4725 DAC**, **Si5351A**, MCP4728, MPL3115A2, SCD30 (0x61) | MCP4725: no ID; a bare 5-byte read is `[status][DAC_hi][DAC_lo][EE_hi][EE_lo]`, status bits 4:3 must read 0. Si5351: no ID; `R[0x00]` bit7 SYS_INIT = 0 after boot, bits 1:0 REVID typically 3. MPL3115A2: `R[0x0C]`=0xC4 |
| **0x68** | **MPU6050**, **MPU9250**, **DS1307**, **DS3231**, **PCF8523**, ITG3200, AMG8833 | (1) `R[0x75]`: **0x68**⇒MPU6050/6000, 0x70⇒MPU6500, **0x71**⇒MPU9250, 0x73⇒MPU9255, 0xD3⇒ITG3200. (2) no match ⇒ RTC: read 0x00–0x06 and BCD-validate (sec≤0x59, min≤0x59, hr≤0x23, dow 1–7, date 1–31, mon 1–12). (3) split the RTC: **DS3231** has temperature at `R[0x11]/R[0x12]` (0x12's low 6 bits always 0) and OSF at `R[0x0F]` bit7; **DS1307** register 0x07 is control (SQWE/RS, upper bits read 0) and 0x08–0x3F are 56 bytes of writable NVRAM; **PCF8523/8563** have a *shifted* map — seconds start at **0x02**, so BCD validation of 0x00–0x06 fails while 0x02–0x08 succeeds |
| **0x69** | MPU (AD0 high), **ICM20948**, **and write-only PC clock chips** | ICM20948: `R[0x00]`=**0xEA**, but that is **bank-0 register 0x00, not 0x75** — write `R[0x7F]`=0x00 (REG_BANK_SEL) first. Never read-probe blind |
| **0x6A/0x6B** | **LSM6DS3/DSOX/DSL**, LSM9DS1-AG, ISM330DHCX, L3GD20H | `R[0x0F]`: 0x69⇒LSM6DS33/DS3, 0x6A⇒LSM6DS3TR-C/DSL, 0x6C⇒LSM6DSOX, 0x68⇒LSM9DS1-AG, 0x6B⇒ISM330DHCX, 0xD7⇒L3GD20H. LSM9DS1's magnetometer is a **separate address** (0x1C/0x1E) with `R[0x0F]`=0x3D |
| **0x70–0x77** | **TCA9548A mux**, HT16K33, SHTC3 (0x70), IS31FL3731 | TCA9548A: no ID, no pointer — a 1-byte read returns the live channel mask (0x00 at POR). SHTC3: `Cmd 0xEFC8`, `(id & 0x083F) == 0x0807` |
| **0x76/0x77** | **BMP280, BME280, BME680/688, BMP388/390, BMP180**, DPS310, MS5611, **+ TCA9548A + HT16K33** | (a) `R[0xD0]`: **0x58**⇒BMP280, **0x60**⇒BME280, **0x61**⇒BME680/688, **0x55**⇒BMP180/085. (b) nothing sensible at 0xD0 ⇒ `R[0x00]`: **0x50**⇒BMP388, **0x60**⇒BMP390 — **Bosch moved CHIP_ID to register 0x00 for the BMP3xx family.** DPS310: `R[0x0D]`=0x10. MS5611 is command-only |

**Compact WHO_AM_I register conventions** — this is what makes a generic identifier possible:

| Family | ID register | Values |
|---|---|---|
| InvenSense MPU | **0x75** | 0x68 MPU6050 · 0x70 MPU6500 · 0x71 MPU9250 · 0x73 MPU9255 |
| InvenSense ICM209xx | **0x00** (bank 0, select via 0x7F) | 0xEA ICM20948 · 0xE1 ICM20649 |
| **ST MEMS (LIS/LSM/L3G/LPS/HTS)** | **0x0F** | 0x33 LIS3DH/LIS2DH12 · 0x3D LIS3MDL/LSM9DS1-M · 0x40 LIS2MDL · 0x69 LSM6DS33 · 0x6A LSM6DSL · 0x6C LSM6DSOX · 0x68 LSM9DS1-AG · 0xB1 LPS22HB · 0xBD LPS25HB · 0xBC HTS221 |
| **Bosch BMP/BME legacy** | **0xD0** | 0x55 BMP180 · 0x58 BMP280 · 0x60 BME280 · 0x61 BME680 |
| **Bosch BMP3xx** | **0x00** | 0x50 BMP388 · 0x60 BMP390 |
| TI power monitors | **0xFE / 0xFF** | 0x5449/0x2260 INA226 · 0x5449/0x2270 INA260 · **INA219 none** |
| Microchip MCP98xx | 0x06 / 0x07 | 0x0054 / 0x0400 |
| Maxim MAX3010x | 0xFF | 0x15 |
| ScioSense CCS811 | 0x20 | 0x81 |
| Broadcom APDS9960 | 0x92 | 0xAB |
| ST VL53L0X | 0xC0 | 0xEE |
| ADI ADXL34x | 0x00 | 0xE5 |

**No ID register at all** — address plus behavioural probe plus elimination is the only route:
SSD1306/SH1106, PCF8574/A, TCA9548A, ADS101x/111x, MCP4725, Si5351, BH1750, LM75, TMP102,
AT24Cxx, HT16K33, **INA219**, PCF8591, DS1307.

### 6.4 Module-level correlation — the cheapest disambiguator you have

Breadboard parts arrive as modules, and the *set* of addresses is far more informative than
any one of them:

- `{0x68, 0x57}` ⇒ a **DS3231 RTC board** (RTC + its AT24C32). Resolves both 0x68 and 0x57.
- `{0x68, 0x1E, 0x77}` ⇒ **GY-87** (MPU6050 + HMC5883L + BMP180).
- `{0x76 or 0x77}` alone ⇒ bare Bosch pressure breakout, not a mux.
- `{0x3C}` alone ⇒ OLED.

Encode this as a post-pass over the ACK set, not as extra bus traffic. It costs nothing and
it fixes the two worst ambiguities on the list.

### 6.5 Jumperless-specific notes

- **Pull-ups are not optional and are not internal.** I2C is open-drain; the RP2350's 50–80 kΩ
  internal pull into the crossbar-plus-breadboard capacitance gives *microseconds* of rise
  time, well past the 1 µs the 100 kHz spec allows, so a bare DIP EEPROM on the breadboard
  needs a real 4.7 kΩ (`src/snakes/projectFiles.h:536-540`). Detect and *say so* rather than
  reporting "no devices found".
- **Scan before you power.** Run the §2 electrical identification pass on the rows *first*;
  an unpowered chip's ESD clamps make every pin look like a diode to the rails, which is
  itself a useful "there is an IC here" signal, and it tells you which rows are Vcc/GND
  before you energise anything.
- Extend `PartDbI2cIdent` with a second optional `(reg, value, mask)` pair and a
  `probeOrder` byte. Several rows above need two reads (BMP3xx's 0xD0-then-0x00, the
  SHT2x user-register-then-electronic-ID) and several need a strict ordering (0x29's
  TSL2591-before-TSL2561).

---

## 7. Safety and ordering rules

Distilled from `EntladePins.c`, `ReadCapacity.c`, `AutoCheck.c` and `51-semicon.tex`, with
the Jumperless-specific additions marked ★.

1. **★ Read before you drive.** Every unknown row gets read on a −8…+10.28 V ADC channel
   with nothing driven, before it is routed to any 3.3 V GPIO or ADC. A charged capacitor at
   8 V into a GPIO is your primary hardware-destruction mode and the AVR tester has no
   equivalent hazard.
2. **Discharge first, in stages.** `EntladePins()` runs before every measurement pass, not
   once at boot. Its staircase: all pins to GND through *both* resistors; while
   V > 1.3 V (0.26·Vcc) keep the high resistance in circuit; below 1.3 V switch to the low
   resistance; below 1.0 V (0.20·Vcc) hard-short the output pin to GND. Ported to your rails:
   bleed through the 60 kΩ pull above 0.86 V, through 132 Ω below that, hard-short below
   0.66 V. Ten seconds without success ⇒ **it is a battery, say so and stop**
   (`EntladePins.c:82-85`, `PART_CELL`). Then "for safety, discharge 5 % of discharge time"
   longer (`EntladePins.c:92-95`).
3. **Low current before high current.** Every AVR test that can run at 470 kΩ runs there
   first. Your equivalent: pull (55 µA) → DAC servo → GPIO push-pull. Never invert this.
4. **★ Never hard-drive an unknown node.** 3.3 V/130 Ω = 25 mA exceeds your 20 mA budget.
   Establish a total loop resistance ≥ 165 Ω with a current-limited source first, and keep
   the INA current watchdog armed (`GuideChecks.cpp:1194` already has a 50 mA one).
5. **Bound every stimulus in time.** The AVR's units are `wait5ms` / `wait10ms` / `W5msReadADC`
   and nothing is left energized across a decision. `ChargePin10ms()` exists solely so that
   a gate is driven for a fixed 10 ms and then returned to input with no pull-up
   (`ChargePin10ms.c`). Copy the discipline: every drive is `set → dwell → measure → release`,
   and the release is unconditional (the AVR's `clean_ports:` label is reached by every exit
   path in `CheckPins`, including every `goto`).
6. **Discharge the gate before every FET test.** `ChargePin10ms(TriPinRL, 0)` for N-channel,
   `(…, 1)` for P-channel, before diode tests and between threshold repeats. Comment at
   `CheckPins.c:1245`: "It is possible that wrong Parts are detected without discharging,
   because the gate of a MOSFET can be charged." A floating gate is the #1 source of
   irreproducible results.
7. **★ Gate voltage limits.** Default the gate drive to ±5 V and only extend toward ±8 V when
   nothing conducts. Logic-level FETs are commonly rated ±8 to ±12 V Vgs — ±8 V is at the
   edge for the worst of them and instantly fatal above.
8. **Abort extrapolations that predict over-current.** `expand_FET_quadratic()` returns 0
   rather than perform an Idss measurement that would exceed 40 mA (`CheckPins.c:29-33`),
   with the pleasant observation that the source-resistor feedback makes the real current
   self-limiting anyway. Do the same at your 20 mA.
9. **Disconnect immediately after a high-current sample.** `ADC_DDR = TXD_MSK;` on the very
   next line after the Idss read — "disconnect drain and source immediately after
   measurement, since quite a lot of current may flow" (`CheckPins.c:389`).
10. **★ Input buffers off** on any GPIO acting as a pull or parked at an intermediate voltage
    (erratum E9, §2.6).
11. **Self-test and zero-calibrate.** The AVR's `AutoCheck()` asks the user to short the
    probes, derives the pin resistances (`Calibrate_UR()`), measures ESR zero offsets per
    pin pair, measures capacitance zero offsets for all six pin orderings, and asks for a
    >100 nF capacitor to calibrate the comparator offset. You need the equivalent for:
    `R_pull` per GPIO (§2.5), `C_row0` per row pair (§4.2), the digital input threshold per
    pin, and `R_adcleg`.

---

## 8. Whole-board auto-scan

### 8.1 The cost problem

60 rows ⇒ 1770 unordered pairs. At the ~200 ms a real two-terminal identify costs (dominated
by the 50 ms INA cadence), a naive sweep is **6 minutes**. Three-terminal identification of
every triple is 34 220 triples — never do that. The whole design problem is getting from
O(N²) pair tests to O(N) cheap tests plus O(k) expensive ones, where k is the number of
components actually on the board.

### 8.2 Phase 0 — row census, O(N), well under a second

For each row independently: drive it from a routable GPIO through the internal pull, then
release it and time the decay with PIO (6.67 ns ticks) or the free-running ADC. You already
have both halves of this primitive — the guide's presence check charges a row through the
~122 Ω DAC0/ISENSE chain, strands the charge, and judges retention against the ~1 MΩ tap
dwell (`GuideChecks.cpp:1170-1178`), and `Debugs.cpp:1995-2030` has a working
drive/release/count-until-flip harness.

Classify each row into:

| τ (through 60 kΩ) | Effective C | Verdict |
|---|---|---|
| ~2 µs | ≈33 pF | **empty row** — exclude from everything below |
| 2–10 µs | 33–170 pF | one IC pin, or one lead of a small part |
| > 10 µs | > 170 pF | a capacitor, a long wire, or a populated net |
| never reaches threshold | — | **DC path to a rail** — flag, do not stimulate blindly |
| already at non-zero V | — | **driven or charged** — refuse, per rule 1 |

On a typical breadboard this drops 60 rows to 12–20 occupied ones, i.e. **1770 pairs → 70–190
pairs**, which is a 15–90 s sweep. That single O(N) pass is worth more than any clever
search on the full set.

### 8.3 Phase 1 — adjacency by group testing with guarding

Among the m occupied rows, find which pairs have anything between them. Two regimes:

**Run the whole adjacency phase below silicon turn-on.** Use a **0.1 V** DAC setpoint, exactly
as ICT does for its shorts test (100 Ω source, 0.1 V DC — §8.6). At 0.1 V no junction on the
board conducts, so an unpowered IC's ESD clamps and every diode in the mesh are invisible and
the network is momentarily *linear* — which is what makes both the group testing in this
section and the delta solve below valid. Raise the stimulus only in phase 2, per pair, once
you know what you are looking at. (CircuitSense does the same thing in the other direction:
start at 500 mV and adaptively raise to 2000 mV only for parts that will not turn on.)

**Sparse (m ≤ 20): just do the m(m−1)/2 pairs**, but do them *cheaply* — a 0.1 V DAC step and
one INA sample is enough to answer "is there a DC path", ~60 ms each. Only pairs that answer
"yes" get the full identify. Note that 0.1 V through 132 Ω is 760 µA into a dead short, so at
the shunt's 5 µA/LSB you still resolve up to ~1 kΩ from a single sample; step to 1.0 V for the
pairs that read open at 0.1 V but had a suspicious census entry.

**Dense (m > 20): group testing.** Drive row *i* from DAC0 (INA1 already measures DAC0's
total output current, `GuideChecks.cpp:1742` — no rerouting needed), tie a candidate group G
of rows to GND, and read INA1:
- no current ⇒ row *i* connects to **none** of G. One test eliminates |G| pairs.
- current ⇒ bisect G. `⌈log₂|G|⌉` more tests per neighbour found.

Cost to find all neighbours of one row: `1 + k·⌈log₂ m⌉`. Total `m(1 + k log₂ m)` versus
`m²/2`. At m = 60, k = 2: **~780 tests versus 1770** — worth it; at m = 15 it is a wash, so
gate the strategy on m.

**The correctness trap, and the fix.** A "hit" between row *i* and group G does **not** mean
row *i* touches a member of G directly: current can travel `i → X → (member of G)` through
any row X not in G and not grounded. This is precisely the problem bed-of-nails ICT solves
with **guarding** — and you can do it, because the crossbar can tie arbitrarily many rows to
one node:

> Hold every row that is not the driven row and not the single sense row at the **same
> potential as the sense row**. With no voltage across them, no current flows through them,
> and every parallel and series bypass path is nulled.

Concretely: driven row at V_S through DAC0; measured row to ISENSE→GND (0 V); **all other
occupied rows tied together and to GND**. A bypass path A→C→B now dumps into ground at C
instead of reaching the shunt.

**The residual error, exactly.** Guard row C is held near ground through the crossbar's
`R_guard` ≈ 130 Ω. Current arriving at C lifts it to `V_C = I_AC·R_guard`, and that voltage
drives `V_C/R_CB` onward into the sense node. Relative to the wanted `V_S/R_AB`:

```
error ≈ R_guard · R_AB / ( (R_AC + R_guard) · R_CB )   →   R_guard / R   when all three are R
```

So guarding is excellent where you need it most and weak where you need it least:
**130 Ω/10 kΩ = 1.3 % error at 10 kΩ, 13 % at 1 kΩ, useless at 130 Ω.**

Two ways to push `R_guard` down:
1. **Parallel paths.** Let the router place several crossbar routes from the guard set to
   GND; `R_guard` divides by the number of independent paths.
2. **Active guard — this is what real ICT does.** Instead of tying the guard node to ground
   through copper, drive it with DAC1 and *servo DAC1 until the guard node's own ADC reads
   0 V.* The guard node is then a virtual ground whose effective impedance is set by the
   servo's residual, not by the wiring: `R_guard,eff = V_residual/I_guard`, and at one ADC
   LSB (4.46 mV) with 1 mA of bypass current that is **4.5 Ω instead of 130 Ω** — a 30×
   improvement, taking the 1 kΩ case from 13 % to 0.45 %.

   The same theorem in its precision-metrology form, from the **Keithley *Low Level
   Measurements Handbook*, 7th ed. §2.2.1** (https://assets.testequity.com/te1/Documents/pdf/keithley/KeithleyLowLevelHandbook_7Ed.pdf):
   unguarded, a shunt leakage divides as `V_M = V_S·R_L/(R_S+R_L)`; guarded from a
   unity-gain buffer of open-loop gain `A_GUARD`, it becomes
   `V_M = V_S·(A_GUARD·R_L)/(R_S + A_GUARD·R_L)` — **the leakage resistance is multiplied by
   the guard amplifier's gain.** Their worked case (R_S = 10 GΩ, R_L = 100 GΩ) goes from
   **9 % error unguarded to < 0.001 % with A_GUARD = 10⁵**. Their definition is the one to
   memorise: *a guard is a low-impedance point at nearly the same potential as the
   high-impedance node being guarded.* And the part that matters for a firmware scanner
   chasing response time: **guarding bootstraps out the stray capacitance, so it is a
   latency optimisation too** — Keithley shows a measurement still unsettled after 12 s
   unguarded settling in ~2 s guarded (and elsewhere one still unsettled at 70 s).

**The 3-node case does not actually need guarding.** For a pure delta of *linear* elements,
three unguarded pairwise measurements are enough: each measurement `M_ab = R_ab ∥ (R_ac+R_bc)`
is exactly the equivalent-wye sum `R_a + R_b`, so

```
R_a = (M_ab + M_ca − M_bc)/2      R_b = (M_ab + M_bc − M_ca)/2      R_c = (M_bc + M_ca − M_ab)/2
S   = R_a·R_b + R_b·R_c + R_c·R_a
R_ab = S/R_c        R_bc = S/R_a        R_ca = S/R_b
```
Sanity check the wye first: any negative `R_a/R_b/R_c` means the network is **not** a
three-resistor delta (an extra node, or a nonlinear element) — fall back to guarding.

**Guarding is what you need when superposition fails or the network is bigger than three
nodes**: any diode or junction in the mesh, four or more connected rows, or when you want
each edge to be independently trustworthy without a global solve.

### 8.4 Phase 2 — orient and identify

For each adjacent pair, run §2.3 steps 5–8 (two-terminal). Then, for every row that turned
out to be adjacent to **two or more** other rows, try it as the tristate pin of a triple with
each pair of its neighbours — that is the AVR's six-permutation search restricted to triples
the adjacency graph says are plausible. On a real board this is a handful of triples, not
34 220.

Two orderings that save a lot of time, both taken straight from the AVR:
- **Stop early.** `if ((ntrans.count + ptrans.count) > 1) goto checkDiode;` — once you have
  found a complete picture, stop permuting (`CheckPins.c:288`). FETs and thyristors count as
  two, so finding one ends the search immediately (`CheckPins.c:1204`).
- **Don't look for a resistor where a diode was found**, and don't look for a capacitor where
  a resistor was found (`GetResistance.c:88-91`, `ReadCapacity.c:84,91`). The class hierarchy
  in `part_defs.h` is a priority order, not just an enum.

### 8.5 Throughput budget

| Phase | Cost | 60-row board, 15 occupied |
|---|---|---|
| 0 — census | ~150 µs/row (PIO) + one refresh each | **< 1 s** |
| 1 — adjacency, sparse | ~60 ms/pair × 105 | **~6 s** |
| 2 — 2-terminal identify | ~250 ms × (found edges, say 8) | **~2 s** |
| 2 — 3-terminal identify | ~2 s × (plausible triples, say 2) | **~4 s** |
| **total** | | **~13 s** |

Against 6+ minutes for the naive sweep. The census is the whole win; guarding is what makes
the rest *correct*.

### 8.6 Prior art, with the numbers worth copying

**In-circuit test (bed-of-nails).** The core instrument is not a voltmeter but a
**transimpedance amplifier**: a source drives one end of the DUT, the other end sits on an
op-amp summing junction held at virtual ground, and Z_X comes from the ratio of source
voltage to feedback voltage. With 2×10⁶ V/V open-loop gain and ±10 V swing the summing node
deviates **±5 µV** from ground — that is the whole justification for "virtual", and it is the
one architectural idea to steal
(https://www.electronicdesign.com/home/article/21202923/principles-of-analog-in-circuit-testing).
The published 3-wire error is `Rx_calc = R_X + R_S + R_M + (R_G·R_A)/(R_B+R_G)`, i.e. *guard
series impedance × source-side shunt ÷ measure-side shunt* — the same shape as §8.3's
`Z_g·R_AB/(R_AC·R_BC)`, derived independently. Adding a 4th wire (remote guard sense) lets
the amp servo out R_G; 6-wire adds remote sense on both legs. Measured error at
R_S = R_M = 0.8 Ω, R_G = 0.4 Ω, R_A = R_B = 50 Ω: **3-wire 1–2 %, 4-wire 0.5–1 %, 6-wire
< 0.5 %.**

Keysight i3070 Series 6 specs (https://docs.alltest.net/manual/Alltest-Agilent-Keysight-E9902G-Datasheet-29440-.pdf)
— useful as a calibration of what "good" means:

| Parameter | Value |
|---|---|
| Shorts test | **100 Ω source, 0.1 V DC**, threshold 2 Ω–1000 Ω, resolution 1.0 Ω |
| Shorts settling | default **50 µs**, programmable to 3.2768 s in 50 µs steps |
| R unguarded | 0.1–10 Ω 4-wire ±1.5 %; 300 Ω–10 kΩ 4-wire ±0.25 %; 1–10 MΩ 2-wire ±5 % |
| **R guarded** | 10 kΩ 6-wire, **guard ratio 1000:1 → ±2.5 %**; 100 kΩ, **guard ratio 10⁶:1 → ±1 %** |
| C | 10 pF–0.5 µF ±2 %; guarded 1 nF @ 1024 Hz, guard ratio 1000:1 → ±6 % |
| Vectorless opens | **200 mV, 8192 Hz, 0.5 fF – 750 pF**, 0.5–2 fF programming resolution |
| Diode | default **1 mA**, programmable to 100 mA, ±(1 % + 4 mV) |
| Beta | emitter bias 100 µA–100 mA, β 10–1000, **±15 %** |

Three transferable rules: **guard ratio is the figure of merit** (1000:1 buys 2.5 %, 10⁶:1
buys 1 %); **stimulus at 0.1 V is chosen specifically to stay below silicon turn-on** so
unpowered ICs on the board stay off; and **multiplex one digitizer between the DUT voltage
and the feedback voltage so gain errors cancel mathematically** — that last one is free on an
RP2350 with one round-robin ADC.

**Automatic guard selection** (US4774455, https://patents.google.com/patent/US4774455A/en) is
the actual ATE learn-mode algorithm: on a known-good board, apply a guard to each node
touching the component *in turn*, measure, **keep the ~4 that improve the reading most**,
then try them in combination — and **penalise each additional guard by 1 %** so the program
generator does not burn resources on marginal gain. Improvement saturates around four; that
is a search heuristic, not a hardware limit.

**Wire-harness continuity scan** — this is literally your problem, solved
(US20130162262A1, https://patents.google.com/patent/US20130162262A1/en): resistively pull
**every** pin high, then pull **one** pin low at a time and read **all** pins in parallel.
Each of N steps yields a full N-bit row of the connectivity matrix; row weight gives net size
directly. It is O(N) *only because there are N parallel receivers* — with k measure lines the
cost is `N·⌈(N−1)/k⌉`, so **N = 60, k = 4 → 900 configurations; k = 8 → 480**, versus 1770
naive. Since a Jumperless crossbar can short an arbitrary *set* of rows onto one measure
line, the read is a native Boolean-OR over the set, which is exactly a group test.

**Group testing bounds.** Hwang's Generalized Binary Splitting finds d defectives among n in
**≈ d·log₂(n/d) + O(d)** tests, degenerating to `⌊log₂ n⌋ + 1` for d = 1
(https://arxiv.org/pdf/2006.10268). Dorfman pooling gives expected tests per item
`1/s + 1 − (1−p)^s`, optimal `s ≈ 1/√p`, cost `≈ 2√p`
(https://pmc.ncbi.nlm.nih.gov/articles/PMC7641378/). For ~10 components on 60 rows,
p ≈ 10/1770 ≈ 0.006 ⇒ optimal pool ≈ 13 and **~0.15 tests per pair, a 6.6× win**.

**Threshold practice** from the harness industry
(https://cirris.com/guidelines-for-setting-resistance-test-thresholds/): continuity pass
`1.05·R + 0.5 Ω` stringent / `max(2 Ω, 1.10·R + 1 Ω)` good / `max(5 Ω, 1.20·R + 2 Ω)`
moderate; isolation 5 MΩ / 500 kΩ / 100 kΩ. IPC/WHMA-A-620 Rev A Class 3 default is
**2 Ω, or 1 Ω plus the actual resistance**. **Use two thresholds, not one** — a low one for
paths through components, a high one for pure isolation.

**IEEE 1149.4 mixed-signal test bus** — worth knowing, not worth copying. Each analog pin
gets an **ABM** (switches SB1/SB2 to internal buses AB1/AB2, SD to disconnect the core,
SH/SL/SG for digital drive), and a **TBIC** gates AB1/AB2 out to pins AT1/AT2
(https://grouper.ieee.org/groups/1149/4/dot4_itc2010_final.pdf,
https://www.edn.com/extensions-to-the-ieee-1149-1-boundary-scan-standard/). **AB1 forces
current, AB2 senses voltage — Kelvin sensing built into the standard**, and the reason there
are two buses rather than one. It did not take: accuracy is degraded by the ABMs' own R and
C, and a 2005 automotive demonstrator could not buy compliant silicon and emulated it with
STA400 muxes (https://arxiv.org/pdf/0710.4826). The standard went Inactive-Reserved in 2021
(https://standards.ieee.org/ieee/1149.4/4022/) and was re-approved as 1149.4-2024
(https://standards.ieee.org/ieee/1149.4/10427). What actually replaced it in board test is
**capacitive vectorless opens testing** (the 200 mV / 8192 Hz / 0.5 fF row above) plus
IEEE 1149.6 for AC-coupled nets, and IEEE 1687/P1687.2 for embedded analog instruments.

**CircuitSense (UIST '17)** — the closest existing prior art to what you are building
(https://teyenwu.com/publications/CircuitSense.pdf). Presence is sensed *mechanically*
(5 strain gauges per breadboard clip, 0.3–0.5 mV through an AD8228 at gain 100, scanned by a
two-stage 16:1 ADG1606 mux cascade) — a crossbar replaces all of that with electrical
presence detection. What matters is their **active probe**: a **50 Hz square wave**,
amplitude starting at **500 mV and adaptively raised to 2000 mV** for parts with a turn-on
threshold, chosen as the explicit tradeoff between latency and detectable R/C/L range. Values
come out in closed form: `V_out/V_in = (R_x+R_m)/(R_x+R_w+2R_m)` for R;
`log V(t) − log V₀ = −t/RC` fitted for C; `log V(t) − log(V₂−V₁) = −tR/L` for L — **the same
log-linearised RC fit recommended in §4.1**. Classification: 2000 samples at **75 kHz** per
wave, time- and frequency-domain moments **plus the cepstrum**, a separate Random Forest per
pin count. Result: **22 component types at 100 % accuracy** (R, C, L, 1N4001, LM317/337,
TIP31C/32C/120/122, and ten 8-pin ICs including NE555, LM358, MCP3002, 24LC256, MSGEQ7), with
**R 50–1000 Ω at ≤5 %, C 1–100 µF at ≤15 %, L 0.01–1 H** — every ceiling set by ADC
resolution and sample rate, both of which you beat. Their scaling shortcut is worth stealing
verbatim: probing all `C(8,2)=28` pairs of an 8-pin IC is too slow, so **they probe only the
four opposite-row pairs (1,8), (2,7), (3,6), (4,5)**.

**Toastboard (UIST '16)** (https://iot.stanford.edu/pubs/drew-toastboard-uist2016.pdf) wires
all 48 rows of a breadboard through a two-stage mux cascade to one ADC and colour-codes them.
Note the limit honestly: **it measures voltages only and does not extract topology** — the
user still draws the circuit and the software cross-checks it. Its requirements list is
nonetheless the right one: sense power/ground, sense when components are connected, measure
many nodes at once, visualise, lower the expertise barrier. Adjacent: CurrentViz (current
rather than voltage), Bifröst (code ↔ electrical correlation), VirtualComponent (software-
managed connections and values — the closest architectural cousin to a crossbar breadboard).

**Don't try to invert the network.** For circular planar resistor networks, recoverability
from the Dirichlet-to-Neumann map is fully characterised (https://arxiv.org/abs/1203.4045),
and joint topology+value recovery from limited boundary measurements has been formulated as a
polynomial system (https://arxiv.org/html/2412.02315) — but their worked example is **4
boundary + 2 interior nodes**, and the candidate-graph count grows nearly exponentially.
**Isolate with guarding and measure one component at a time.**

### 8.7 AC V-I signature analysis — the classifier the DC tree cannot replace

The DC tree in §2.3 identifies *semiconductors* well and *reactive* parts poorly. The
complementary technique is **Huntron-style V-I signature analysis**: apply a sine, plot
current against voltage, and classify the Lissajous figure. You have everything needed —
DAC0 with `refillTable()`/`dacTriangle()` (`Peripherals.h:226,248`) for the source and INA0
plus the ADCs for the two axes.

Huntron's whole source specification is three numbers — **V (peak), R (source resistance),
F** — from the ASA Training Course Workbook P/N 21-1376 Rev. A
(https://huntron.com/sales-support/manuals/ASA-Training-Course-Workbook-RevA-Board.pdf):

- **R:** 10, 50, 100, 1 k, 5 k, 10 k, 50 k, 100 kΩ
- **V:** 200 mV, 3, 5, 10, 15, 20 V peak
- **F:** 20 Hz – 2000 Hz (5 kHz on the higher models)
- **STAR interlock:** the legal V×R combinations are exactly those keeping available current
  **under 200 mA**. As a rule: `V_peak / R_source ≤ 200 mA`. (200 mV is legal on every R;
  20 V only on 50 k and 100 k.) **Your equivalent is `V_peak/132 Ω ≤ 20 mA` ⇒ V_peak ≤ 2.6 V
  on the bare crossbar path** — the ±8 V DAC only becomes usable at higher source impedance,
  i.e. through the 60 kΩ pull.

**Classify by invariance, not by shape.** This is the key idea and it is directly
implementable as three sweeps:

| Primitive | Signature | Behaviour under sweep |
|---|---|---|
| Short | pure vertical line | — |
| Open | pure horizontal line | — |
| **Resistor** | straight line, 0–90° | **invariant in F and V**; moves only with R_source |
| **Capacitor** | ellipse | **verticalises as F rises** (X_C falls) |
| **Inductor** | ellipse, tilted by winding DCR | **horizontalises as F rises** (X_L rises) |
| **Diode** | two linear segments meeting at ~90°, one direction | knee position = Vf |
| **Zener** | two knees, conduction both directions | reverse knee = V_Z |
| **BJT** | **C–B reads as a plain diode; B–E reads as a zener**; E–C reverse breakdown equals the B–E reverse breakdown | |

**Autorange to 45°.** The workbook's tuning rule: start at 100 Ω or 1 kΩ, step up a range if
the trace is near-horizontal, down if near-vertical, aim for 45° — because a 45° line means
`R_DUT ≈ R_source`. That is impedance autoranging in one sentence, and on your hardware
"stepping the range" means switching between the 132 Ω DAC path and the 60 kΩ pull.

**Quantitative readout.** The display is 4 divisions per side, so volts/division =
`V_range/4`. Their worked example: on the 3 V range, 0.75 V/div, and a silicon knee "slightly
before the first division" reads **0.6–0.7 V peak** — the same closed-form readout you get for
free from `V_H − V_L`.

**Learn a known-good envelope, not a golden trace.** Huntron Workstation stores per-range
Max Samples / Tolerance / Delay and a Compare Priority of Same / All / **Merge — a merged
min/max envelope built from several good boards**
(https://huntron.com/sales-support/download/workstation-tutorial.pdf). If you add a
"compare against last known good" mode to Jumperless, build the envelope from multiple
passes.

---

## 9. Pitfalls

**Darlington detection.** Common-emitter hFE reads ~30× low. BC517: 764 (CE) vs 25 100 (CC);
BC516: 760 vs 76 200 (`51-semicon.tex:276-278`). Run both circuits and keep the larger. Also
Vbe ≈ 1.2 V — on a 3.3 V collector supply minus a shunt drop there is barely any Vce left, so
**source the collector from the DAC at ≥5 V.**

**Depletion FETs read as enhancement FETs.** Both AVR enhancement branches are guarded by
`adc.lp_otr < 97 mV` — "if flow voltage in switched off mode low enough? (since D-Mode-FET
will be detected in error as E-Mode)" (`CheckPins.c:1166`, `:780`). Without that guard a
JFET is reported as a MOSFET with a bogus threshold.

**Germanium BJTs read as JFETs.** Both leak with the control terminal open. The separator is
that the BJT's leakage is *much larger with the base open than with it held*, hence
`2·vCEs + 20 > lp_otr` (`CheckPins.c:304`). Ge parts also drift with temperature: the doc
notes an AC128 that can only be measured when cooled (`51-semicon.tex:193-195`).

**Capacitors look like shorts, then like resistors, then like diodes.** Three separate
defences, all needed: the settle loop in `GetResistance.c:99-108`; the "read it twice and
require < 20 mV of rise" test in `CheckPins.c:1308`; and `hp2 < hp1` in the diode acceptance —
"hp2 ≥ hp1 is only possible with capacitor, not with a diode" (`CheckPins.c:1387`).

**Charged MOSFET gates fake parts.** See safety rule 6. The 200-iteration discharge loop
before the diode test exists purely for this (`CheckPins.c:1237-1244`).

**Body diodes and protection diodes.** A MOSFET's body diode means "no current in the
opposite direction" is not a usable diode test — "The identification of a diode by no current
flow in the opposite direction is not possible with a inverse parallel diode"
(`51-semicon.tex:180-181`). Finding a *depletion* MOSFET's body diode requires driving the
gate to a **negative** Vgs, which the AVR fakes by moving its resistors between the VCC and
GND sides (`CheckPins.c:1281-1337`). You can just set DAC1 to −3 V. Also: a BJT with an
integrated protection diode presents a **parasitic PNP with swapped base-collector**; the
tester finds both and picks the one with the higher base-emitter junction capacitance
(`51-semicon.tex:306-317`, BUL38D reported as NPNp + PNPn).

**Anti-parallel diode pairs read as resistors.** This is exactly why the low-current
measurement exists: "The additional measurement with the big resistor R_H is made, to differ
antiparallel diodes from resistors. A diode has a voltage that is nearly independent from the
current. The voltage of a resistor is proportional to the current" (`CheckPins.c:1246-1250`).

**Opto-couplers.** Very low hFE forced the "conducting" threshold from 1600 mV up to 4400 mV
(`CheckPins.c:947-949`) and forced 1 %-resolution hFE arithmetic (`LONG_HFE`). If you care
about optos, keep your equivalent threshold loose.

**Thyristor holding current.** The AVR can only source 6 mA and therefore only tests
low-power SCRs (`51-semicon.tex:121-122`). You can do better but not much — 20 mA still
misses most power thyristors, so report "no latch at 20 mA", not "not a thyristor".

**Symmetric parts have no pinout.** "Due to the symmetrical design of the JFET transistors,
the drain and source can not be distinguished" (`51-semicon.tex:394`). Same for a MOSFET's D/S
once you account for the body diode. Say "cannot determine" rather than guessing.

**★ Your ADC sense legs load the DUT.** ~1.2 MΩ per leg. Irrelevant at 1 kΩ, an 8 % error at
100 kΩ, and it wrecks nA-scale leakage entirely. Model it or disconnect it.

**★ The 50 ms INA cadence is your throughput limit.** Six permutations × several current
samples each ≈ 2 s. Classify with the ADCs (fast) and only reach for the INA for the numbers
you actually print.

**★ 2 Ω of shunt is in series with the DUT.** At 20 mA that is 40 mV — nine ADC LSBs. It
cancels in the 4-wire difference `V_H − V_L`, but not in anything you compute from `V_H`
alone against ground.
