# FIXCHECK 2026-08-29 (after midnight) — cap values in the Auto scan, and the file that nearly vanished

Kevin's ask: "We should also be testing part values like capacitors and
resistors in auto scan, and let's fix any issues left." Resistors already
carried ohms end to end; capacitors carried nothing (`CAPACITOR conf=0.00`,
a parts list that just said "capacitor"). Both are fixed and bench-driven.
But the session opened with something worse than any of that.

## 0. PartsApp.cpp lost its back 1,600 lines - recovered, byte-verified

At 23:43 - between the evening FIXCHECK session and this one - something
rewrote `src/PartsApp.cpp` as its FIRST 3,429 lines only. Everything from
`partsIdentifyChip`'s definition on was gone: `partsResolveChipRails`,
`partsConfirmOne`, the whole `partsAutoLauncher`. The tree did not link.
`PartMeasure.cpp` / `PartClassify.cpp` were rewritten in the same minute but
survived intact.

Cause, per Kevin: a branch switch (the same event left `src/oled.h`'s
OLED-mirror position tweak in the tree). The recovery below stands either
way; a safety copy of the final file sits at
`/tmp/PartsApp_final_backup.cpp`.

Recovery, since the state existed nowhere on disk: the evening session's
transcript holds every edit it made. All 75 operations (15 main-session
edits, 59 wording-pass edits from its subagent, one `sed`) were replayed
onto `HEAD:src/PartsApp.cpp` in their original order - zero failures - and
the rebuilt file's first 3,429 lines byte-matched the truncated survivor
exactly, which is the proof the replay IS the lost file. Every other file
the evening session touched was replay-verified the same way: all MATCH.

## 1. Capacitance is now measured, not shrugged at

`partScanCapMeasure` (PartMeasure.cpp), two fixtures, picked by what the
part does:

- **Pull-down decay** (~5nF-20uF): charge the row from the roving GPIO
  (~80R, 40ms lifts anything under ~500uF past 2V), release into the pin's
  own pulldown, and time the row's ADC through the 2.0V and 0.9V crossings
  (two crossings cancel the release instant; the measured end voltage takes
  the ADC lane bias out of the asymptote). C = tau / R_pull, with R_pull
  measured in-session by the existing `partScanCalibratePull` (its
  3-row-only gate relaxed - the fixture never needed the third row).
- **Hard-loop charge integration** (bigger, or no GPIO free): step DAC0 to
  2V through the drive fixture and count the charge in - the first 30ms
  from the SINK row's voltage (the pair sweep's Ohm's-law trick, ~25us a
  sample), the rest from the INA's true mA at the fast-poll cadence.
  C = Q / V(end). Presence additionally requires the current to DECAY -
  steady conduction is a diode or resistor, not storage.

  The sink-path resistance is MEASURED, not assumed: one non-blocking INA
  poll per millisecond rides the tight ADC loop, and each fresh conversion
  pairs the shunt's true mA with the sink row's volts at that instant -
  the median ratio IS the live route. The first draft used a 67R "one
  hop + the shunt" constant, and Kevin's known-100uF electrolytic read
  260uF on it: the route the router actually picked was a two-hop bounce
  (~170R), and no constant covers both shapes. With the ratio measured
  in-run the same part reads 115-117uF across five runs (+/-0.6%) -
  inside the part's own electrolytic tolerance. 67R survives only as the
  fallback for transients too fast to give both meters the same instant,
  and those caps' values come from stage 1 anyway.

`identifyTwoLead` uses it in both capacitor branches: the no-conduct branch
(where it also reaches far below the old 0.10mA-at-25ms INA watch - a
100nF used to read EMPTY) and the fake-out branch that produced Kevin's
`conf=0.00`.

## 2. The two bugs the bench found while proving it

Both were caught by driving the real flow, neither was visible in reasoning:

- **The blind 5ms discharge.** The first HIL run measured C12 fine (204uF)
  and then read the very same part as `DIODE conf=0.90` seconds later: a
  204uF cap through two routed GND legs is tau ~27ms, so `partScanDischarge`'s
  fixed 5ms left it near 2V, and the next screen met a charged source.
  The discharge now WATCHES the rows (a grounded plate still reads its
  share of the cap voltage while charge remains) and drains until they are
  flat, 600ms ceiling; a clean session still pays ~5ms.
- **A junction that isn't.** The Auto scan itself then produced
  `rows 12-42: DIODE -0.41V` - census poke plus pair sweep had charged the
  cap, and the one-way screen read the discharge current as forward
  conduction. No junction passes 1mA under 0.25V, and a negative drop is a
  charged capacitor sourcing current - the diode branch now refuses drops
  under 0.25V and hands the case to the cap machinery (whose decay gate
  keeps a low-drop schottky from riding along; it lands in UNKNOWN with
  the drop instead).

## 3. Where the value now shows up

- scan narration: `rows 12-42: CAPACITOR 115uF`
- the confirm: `add? CAPACITOR rows 12-42  115uF  (y/n)`
- the placed record: `value: '115uF'` (round-trips through
  `parsePartValue`; the part card's type line shows it for free; verified
  to survive a reboot via the slot auto-save)
- Parts > Test: the OLED card reads "115uF / measured", the serial line
  carries `value=0.000115` (`%.4g` now - `%.3f` printed every real
  capacitance as 0.000)
- the part-highlight card: `115uF measured` (new CAPACITOR arm in
  `partTestSummary`)
- the four scan report sites share one `partsResultDetail` helper instead
  of four hand-rolled ohms/volts blocks

`formatFarads` lives beside `formatOhms` (GuideChecks.cpp): pF/nF/uF/mF,
plain ASCII 'u'.

## Verification

- Both targets build at every step.
- `test/hil/test_ic_identify.py` gained a cap-value check (type, plausible
  farads, conf >= 0.5) against whatever capacitor record the bench holds.
  Final run on the final firmware: **PASS (45 checks)**, bridge set
  byte-identical around every call.
- C12 (the demo rig's electrolytic, rows 12/42, Kevin says nominal
  100uF): 116.7 / 115.3 / 116.6 / 115.2uF across four calibrated runs
  (+/-0.6% spread, ~15% above nominal - inside an electrolytic's own
  band). Precision was never the problem: the pre-calibration runs
  repeated at 260uF just as tightly, all agreeing on the same wrong
  R_sink constant. Kevin's ground truth is what exposed it.
- The Auto Scan driven end to end over port 1, twice (record removed
  first, `y` at the confirm): census -> `CAPACITOR 115uF` -> added C12
  with the value -> `board restored (11 wires back)`, 12 bridges before
  and after, record intact across a reboot.

## 4. "Does the board fully clear before testing?" (Kevin's follow-up)

Two different surfaces, two different answers, both verified tonight:

- **The Auto Scan** lifts ALL user wiring up front since the evening
  session ("board cleared for the scan (11 wires lifted - they go back
  when it's done)") - the strongest form of clearing, watched twice
  tonight with the 12-bridge set byte-identical after.
- **Parts > Test / `part_identify`** deliberately does NOT clear the
  whole board first: it lifts wiring on the part's own rows and the
  measurement path, and only when the fabric then refuses a plain sense
  leg does it lift everything liftable and ask once more (the evening
  session's -7 fix). That retry is what turned the old dead-end
  ("machinery is busy") into tonight's `status=0 lifted=11`, seven
  sessions in a row on the fully-wired board. What REMAINED was
  cosmetic: the router printed its raw "Couldn't find a path for 42 to
  ADC_1" lines for the failed first attempt and nothing explained them,
  so the retry read like a malfunction. The session now narrates it:
  `no measurement lane to row 42 - briefly unwiring the rest of the
  board and asking again...`

## Honest ceilings

- The hard loop's fast phase falls back to the 67R sink-path constant
  only when fewer than two INA conversions land inside the transient
  (sub-~3ms, i.e. sub-~20uF through the loop) - and those parts' values
  come from stage 1's calibrated pull-down decay instead. The INA phase
  is shunt-grade, so big electrolytics get the good meter either way.
- A leaky electrolytic rides the 2s integration ceiling and reads high; a
  leak-slope fit is the named upgrade path.
- Stage 1's floor is ~5nF (ADC sample cadence vs a 50k decay); below that
  a part is indistinguishable from an empty row's ~100pF.
- `partScanCapMeasure` can prove presence and still refuse the value (pull
  calibration refused, tiny tau): the verdict is then detect-only -
  CAPACITOR, `value=0`, degraded - and every consumer already prints it as
  a bare "capacitor".
- The scan leaves electrolytic POLARITY unknown (roles from a charge
  transient are noise, so they are reset to plain leads).
