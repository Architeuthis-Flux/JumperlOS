# Projects branch — wave-2 bench session

One ordered script for the `projects-guided-placement` branch as it stands after
wave 2. Run it top to bottom: **each section can invalidate the ones after it**,
which is why the forced-refresh step is last and why §1 (no parts) comes before
§2 (parts in the holes).

Wave 2 changed enough that the wave-1 copy of this file is not worth diffing
against — it is preserved at commit `6dfaf7c` if you want the archaeology. The
short version of what moved:

| Was (wave 1) | Is now |
|---|---|
| Projects lived under **Apps** | **top-level** clickwheel row, before Apps |
| a project ran into a **destination slot**, then a keep-prompt | a project opens `/projects/<dir>/<dir>_<N>.yaml`, a **run file**, as your persistent context — no prompt, no slot |
| slots were eight numbered files | any YAML anywhere can be the active context; the board **boots the last active one** |
| DIP pin 1 anchored the **top** half | pin 1 (the dot) anchors the **bottom** half — **row 35** on every shipped project |
| 2-leg parts sat in two adjacent rows | resistors are `axial2` and **straddle the ravine** (row *r* and *r*+30) |
| continuity reported **milliamps** | continuity reports **ohms**, four-wire, on pass and on fail |
| `rail_sane` compared rows to the **setpoint** | it measures the rail, then compares rows to the **measurement** |
| the guide walked forward and exited off the end | the wheel **browses** a ring, wraps through a **DONE view**, and only a hold/`q` quits |

---

## 0. Prelude — do these first or you will chase ghosts

- [ ] **Close the** `jumperless` **terminal client on port 1.** Any stray byte
  cancels a picker (the `selectNodeAction` convention), and any byte that is not
  `y`/`n` makes `yesNoMenu` return −1 — i.e. **cancel**. Firmware drains leftover
  `\r`/`\n` before every offer, so a bare line terminator no longer self-answers
  one; every *other* stray byte still does. **If a picker closes by itself or a
  prompt instantly self-declines, this is the first suspect, not the launcher.**
- [ ] **Clear the breadboard.** §1 is the no-parts pass, and several of its steps
  (the tap rules, the collision refusal, the overlay eyeball) want empty holes.
- [ ] **If you plan to run** `test_encoder_ui`: regenerate `jl_input.py`'s ADDR
  table from the *currently flashed* ELF. It is per-build, it lives outside
  version control, and a stale one makes the suite SKIP (which `run_all` counts
  as a pass — so a SKIP here is expected, not a regression).
- [ ] **If you run the HIL suites with another USB dev board plugged in, check
  `~/.cursor/skills/jumperless-v5/.jumperless_port` first.** `jumperless.py`
  caches the REPL port and then trusts the cache blindly as long as that device
  still exists. If a re-detect ever picks a *different* board — an interrupted
  run is enough to trigger one — every later `jl_exec` runs on the wrong device,
  and the symptom is `NameError: name 'jfs' isn't defined` followed by raw-REPL
  timeouts. It reads as a firmware fault and is not one. Pin it to
  `/dev/cu.usbmodemJLV5port5`.
- [ ] **The HIL suites treat parts of the board as SCRATCH, and a killed run
  leaves the damage behind.** Before you run them, back up anything you care
  about in these two places — they are restored by a `finally`, and a run that
  is interrupted (Ctrl+C, a tool timeout, an unplugged cable) never reaches it:
  - **`/slots/slot3.yaml`** is overwritten by `test_projects` **and**
    `test_parts_roundtrip`. **Do not keep work in slot 3.** One killed run
    during wave 2 destroyed its contents permanently — numbered slots have a
    `/.bak` mirror only for the last *successful* load, which by then was
    already the test fixture.
  - **`/config.txt`** gets `test_config`'s **fail-safe droop sentinels**
    (`probe_droop_ohms = -55.5`, `probe_droop_v0 = 2.599`). They are chosen so a
    board that boots with them falls through to safe defaults rather than to a
    believable-but-wrong calibration — but they are not your values. `test_config`
    also toggles `[top_oled] show_in_terminal` twice and relies on reaching the
    second one.
  **After any interrupted run, check both before trusting the board**, and look
  for leftover `<dir>_<N>.yaml` run files and stray `/projects/hil*` fixture
  directories while you are there.
- [ ] Note the board's `top_oled.lock_connection` / `sda_row` / `scl_row`. In
  `connection_type 0` the top OLED lives on **GPIO 7/8** — the very pins the
  `i2cscrn` and `eeprom` projects use, and the pins the guide's `oscillates`
  picker may claim. Turn it off before those two.
- [ ] **Scope item, do it while the board is still clean:** put a probe on the
  top rail, set the rails to something visible (say 3.3 V), then launch a guided
  project with a `power:` section — `z 555 new` is the cheapest. **The rail must
  never leave your pre-launch voltage on its way to 0 V.** A guided launch used
  to energize the project's declared rails for tens of milliseconds before the
  guide's INIT parked them; task 7 closed it by deferring the load's power apply
  until guided-ness is decided. HIL proves the *ordering*; only a scope proves
  the absence of a 20 ms pulse. Quit the guide with `q` when you have the trace.
  **Then put 555 back to having no runs**, or §1.2's first check below is
  invalidated before you reach it: `z 555 new` mints
  `/projects/555/555_1.yaml`, so delete that file (Files browser, or
  `jfs.remove("/projects/555/555_1.yaml")` from the REPL) and `<0` back to a
  slot before starting §1. If you would rather not, run the trace against a
  throwaway project instead — any directory under `/projects/` with a
  `wiring.yaml` carrying a `power:` section will do.

Machine grammar to watch on port 1 throughout (a companion to the human text,
and what a headless driver keys on):

```
PROJECTS n=<count>                       the project picker opened
VARIANTS n=<count>                       the variant picker opened (dormant)
RUNS n=<count> latest=<path>             before the load/new prompt
RUNFILE path=<path> action=new|load      the run file decision, every door
GUIDE resume file=<path> step=<k>        a real resume (not a fresh start)
GUIDE already complete (step k/n)        the completion clamp
GUIDE step=<i>/<n> id=<id> state=<S>     S = INIT|WAIT|PROBE_WAIT|ADJUST|
                                             VERIFY|RESULT|COMMIT|BACK|DONE|EXIT
GUIDE ... state=RESULT check=<c> val=<v> ok=<0|1>[ on_fail=<policy>]
GUIDE move part=<name> row=<r> placement=<expanded|compact|custom>
GUIDE done committed=<c> skipped=<s> unfinished=<u>
SCRIPT offer=<path>|none   /   SCRIPT action=run|skip
PROJECT error <reason>                   headless failure (z, load_project)
ACTIVE_SLOT:<n|-1>  +  ACTIVE_PATH:<path>     (`Q`; -1 means "a file")
```

`z <project>[ new|load|run=<N>][ noscript]` drives all of it headlessly.
Destination slots are gone: a stale `z 555 3` loud-fails with the usage line.

---

## 1. No parts needed — menu, files, run files, slots, the guide's hands

### 1.1 Projects is a top-level menu row

- [ ] Wheel through the clickwheel menu: **`Projects` appears before `Apps`**, not
  inside it. Click it.
- [ ] The picker lists the **four shipped projects** — `555`, `eeprom`,
  `i2cscrn`, `nand00` — and port 1 says `PROJECTS n=4`. If you see `hiltest`,
  a HIL run left its fixture behind (the suite removes it now; an older build's
  leftovers can be deleted from Files).
- [ ] `i2cscrn` is 7 glyphs, exactly the LED-matrix row limit — check it renders
  whole. Title + summary on the OLED, mirrored on the terminal.
- [ ] Hold to cancel: `  Cancelled.` and **your context is untouched** — press
  `Q` and confirm both the number and the path are what they were.

### 1.2 The run-file lifecycle

This is the wave's central change: launching a project no longer borrows a slot
and no longer asks where to keep it. It **opens a file and makes it yours**.

- [ ] Launch `555` on a project with no runs yet. There is **no prompt** — port 1
  says `RUNFILE path=/projects/555/555_1.yaml action=new`, and `Q` afterwards
  reports `ACTIVE_SLOT:-1` + `ACTIVE_PATH:/projects/555/555_1.yaml`.
- [ ] Quit the guide (`q`) and **relaunch `555`**. Now the prompt appears:
  `RUNS n=1 latest=…555_1.yaml`, the OLED reads `Load run 1?` / `No = new run`,
  and the terminal offers `y/click Yes = load latest (555_1.yaml), n = start new
  (555_2.yaml), other = cancel`.
  - [ ] **A bare CLICK answers Yes.** (`yesNoMenu` used to open on *No* while
    every prompt said "click Yes"; it now opens on the highlighted option the
    text names.) Confirm the highlight starts on Yes.
  - [ ] Answer **n** → `555_2.yaml action=new`, and **`555_1.yaml` still exists
    on disk with its own `guideProgress`.** Start-new is not destructive any
    more; run N+1 leaves run N alone.
  - [ ] Answer **y** on a third launch → `action=load`, and you resume at the
    step you quit on (`GUIDE resume file=… step=<k>`).
  - [ ] **Hold, or let it time out (20 s)** → `  Cancelled.`, nothing loaded,
    context untouched.
- [ ] **The pile-up hint**: get to 20 run files in one project (a loop of
  `z 555 new noscript` + `q` is the fast way) and confirm
  `(20 run files in /projects/555 - old runs can be deleted from Files)`.
  Then do exactly that — **delete them from the Files browser** — and confirm:
  - [ ] deleting a run file that is **not** active just removes it, and `Q` still
    reports the context you were on;
  - [ ] the allocator **does not reuse numbers**: after deleting `_1` while `_2`
    exists, the next new run is `_3` (HIL asserts this; the point here is that
    deleting from Files takes the same path);
  - [ ] **deleting the run file you are currently ON is the interesting one, and
    it is not asserted anywhere — write down what actually happens.** The
    manager still holds the path, so the next dirty auto-save will try to write
    it. Two plausible outcomes and both are acceptable behaviour: the file is
    silently re-created (a save to a path is a create), or the write fails and
    `service()` prints its failure **throttled to once per 10 s** rather than
    once per idle pass. What must NOT happen is a crash, a wedge, or a write
    landing somewhere else. `<0` recovers either way.
- [ ] **Clicking `/projects/555/wiring.yaml` in the Files browser starts a run**
  — it no longer adopts the shipped template. The file manager closes *first*,
  then the prompt/guide comes up on a clean terminal.
- [ ] Click `README.md` in the same directory → opens in **eKilo**, not a load.
- [ ] Run a `main.py` from the Files-browser click menu, *after* a guided build
  has wired the board. This is the only path that exercises `File::readString()`
  on a ~5.7 KB script, which no HIL check can reach.

### 1.3 Slots are files now

- [ ] Open **any** YAML from Files (a slot file, a run file, a hand-written one)
  and confirm it becomes the active context: `SLOT_CHANGED:-1` + `ACTIVE_PATH:`,
  and `Q` agrees.
- [ ] **Boot-last-active across a power cycle.** With `[slots] boot_mode = 1`
  (the default), leave the board on a **run file**, unplug and replug. It must
  come back **in that run file's context with its wiring live** — not slot 0.
- [ ] **`boot_mode = 0` pins a slot.** Set `` `[slots] boot_mode = 0 `` and
  `` `[slots] boot_slot = 5 ``, reboot: it comes up on slot 5 with the canonical
  path. **Put `boot_mode` back to 1 and `boot_slot` back to 0 before you go on** —
  every later step assumes the default.
- [ ] The **Slots preview** (spinning the wheel through the Slots menu) **no
  longer applies rail power** to hardware. Wheel through a slot that has a 5 V
  rail saved and confirm the rails do not move while you spin; they apply when
  you actually load. (Task 4: the preview gate now also stops every detent from
  stamping a merely-glanced-at slot into `last_active.txt`.)

### 1.4 The guided build, with your hands and no parts

Launch `z 555 new` (or the menu). Every check will fail — the holes are empty —
which is exactly what you want while you exercise the input surface. The banner
you should see on INIT:

```
=== Guided build: 555 LED Flasher === (9 steps)
wheel=browse  click=confirm  dblclick=adjust  hold/q=quit  n/p/s/v  t/m <row>  c=snap  >/<=browse
rails + DACs held at 0V until the power_on step
```

- [ ] **THE DIP FLIP.** Step 2 reads *"555 across the middle gap, pin 1 (the dot)
  at **row 35**."* — bottom half, dot at the bottom-left, the way a real chip
  sits. The `_GUIDE_FP_` footprint overlay must light **rows 35–38 and 5–8**
  (pin 1 = 35, pin 8 = 5), and the `_GUIDE_TGT_` pulse must be on **35**.
  A top-half anchor here is the mirrored bug coming back.
- [ ] **Resistors straddle the ravine.** Step 3 says *"10k resistor: rows 10 and
  40"* — `axial2`, one leg each side. Same for the 47k (13/43) and the 330
  (16/46). The cap and the LED stay radial, two adjacent rows.
- [ ] **A wheel TURN can never leave the guide.** Turn past the last step: you
  land in the **DONE view** (`GUIDE done committed=… skipped=… unfinished=…`),
  turn again and you **wrap to step 1**. Turn backwards off step 1 and you wrap
  into DONE. `state=EXIT` must not appear anywhere in that walk.
- [ ] **Click confirms, with a 260 ms pend.** Wheel-click has a deliberate
  ~quarter-second delay before it commits (it is how the double-click is
  distinguished). Serial `n` and probe CONNECT are instant. If the guide feels
  sluggish, this is why — it is not a bug.
- [ ] **Double-click enters ADJUST** on a place step. Its own one-liner prints:
  `adjust <PART>: wheel=slide  click=drop  dblclick=snap  hold/q=cancel  (probe:
  tap=move, CONNECT=drop, REMOVE=cancel)`. Then:
  - [ ] **wheel = slide** the part to the next LEGAL row (it scans up to 30 rows
    and skips illegal ones); each detent prints `GUIDE move …` and `now: …`.
  - [ ] **click = drop**, **hold/`q` = cancel** and the part goes back where it
    was — *`q` in ADJUST cancels the adjust, it does not quit the guide.* Two
    presses get you out.
  - [ ] **double-click inside ADJUST cycles compact ↔ expanded.**
  - [ ] Slide into a wall (row 60 on a bottom-half part) → `(no legal row that
    way)`, once per detent. Noisy on a fast spin; harmless.
- [ ] **Tap gestures** (probe on the pads — the one input path HIL cannot reach):
  1. a `probe_confirm` target → confirms the step;
  2. **the part's own glowing footprint** → snap (cycles compact/expanded);
  3. a **free** hole → move pin 1 there (on a DIP, a tap on rows 1–30 maps to
     `+30`, because pin 1 lives on the bottom half);
  4. anything else → the row's net is identified and printed.
  Rule 3's "free" is stricter than the move checker on purpose: tapping a row
  that already carries a net falls through to *identify*, while typing
  `m <row>` onto that same row is a legal move with a `(row N joins net …)`
  heads-up.
- [ ] **The collision refusal**: `m <row>` onto a row already holding another
  **placed** part's leg → `move refused: collides with <NAME> at row <N>`, and
  nothing changes on the fabric or on disk. Rails are exempt — two legs in a
  rail is what a rail is for.
- [ ] **Compact makes the overlay sparser.** Snap a part whose endpoint is a
  rail (`c` on the 555's R1, whose A leg goes to `TOP_RAIL`): the leg moves into
  the rail hole and **paints no LED**, because the overlay only draws nodes
  1–60. One hole lights where two did. Correct, but worth seeing once.
- [ ] **Skip and return.** `s` on step 3 → `(skipped)` and the cursor advances.
  Commit the rest; at DONE the summary counts it (`skipped=1`) and a **confirm
  at DONE jumps back to the skipped step** rather than exiting. Commit it and
  DONE goes clean.
- [ ] **`s` on a step you already committed is refused**: `(already committed -
  p removes it first)`. `p` (or probe REMOVE) un-commits *and pulls the
  hardware*; the flag and the fabric never disagree.
- [ ] **The script offer.** Walk a build to a clean DONE and confirm at DONE:
  `SCRIPT offer=/projects/555/main.py`, the OLED asks `Run main.py?`, and
  declining (or the 15 s timeout) prints `SCRIPT action=skip` — leaving the run
  file active and the rails where the guide left them.
- [ ] **A build where you skipped everything offers nothing**: `s` through all
  the steps → `GUIDE done committed=0 skipped=N unfinished=0`, `q`, and
  `  (nothing was built - no script offer)` with **no `SCRIPT` lines at all**.
  The run file is still saved — persistence is unconditional.
- [ ] **Rails on the way out.** Set the rails to something you will recognise
  (3.3 / −1.5), launch guided, quit before `power_on`. The exit tail names the
  values coming back: `rails + DACs restored (top=3.30V bot=-1.50V dac0=… dac1=…)
  - the project never powered up`, and the rails are physically there. Quit
  *after* a committed `power_on` instead and the **project's** power stands.
- [ ] **The readouts agree with the hardware, not the file.** In that same
  window (after a restore, before the next context switch) check that
  `dac_get(2)` / `dac_get(3)`, the OLED rail reading and the rail LED dots all
  show the **physical** 3.3 / −1.5. `Y` deliberately still prints the *saved*
  `power:` — it is the paste format — and now says so above it:
  `(rails are physically at top=3.30V bot=-1.50V - the power: below is this
  context's SAVED state, which is what S pastes back)`.
- [ ] **The rail adjuster opens on the physical value** in that same window
  (click a rail net, adjust). Wave 2 pointed it at hardware truth: before, it
  opened at the saved 0 V and — because it live-updates as you turn — the first
  detent yanked a live 3.3 V rail toward zero.

---

## 2. Real parts — the measurement pass

**Continuity reports OHMS now**, on pass and on fail, on the OLED and in the
terminal. Every milliamp expectation from wave 1 is stale. The bands come from
`value:` plus tolerance (author, default 15 % + a per-decade measurement
allowance), and an authored `min:`/`max:` still wins when it is given.

Two off-bench helpers worth a line each — neither touches hardware:

- `z band <value> [type] [tol]` → prints the parsed value and the derived band.
- `z shunt [n]` → n fresh INA0 **shunt-register** samples, their spread in LSBs
  (10 µV each, 5 µA across the 2 Ω R1) and the mean current.

### 2.1 The shipped band table

| project | part | value | stimulus | expected `val=` | band |
|---|---|---|---|---|---|
| 555 | R1 | 10k | 3.3 V | ~`9.?k` | **8.00k–12.0k** |
| 555 | R2 | 47k | **5.0 V** (the ≥20 kΩ escalation) | ~`4?.?k` | **35.2k–58.8k** |
| 555 | R3 | 330 | 3.3 V | ~`3??` | **264–396** |
| nand00 | R1 | 330 | 3.3 V | ~`3??` | **264–396** |
| eeprom | R1, R2 | 4.7k | 3.3 V | ~`4.?k` | **3.76k–5.64k** |
| i2cscrn | — | — | — | no continuity steps | — |

(`35.2k` and not `35.3k` is real: `%.1fk` rounds 35250 Ω half-to-even.)

### 2.2 continuity — a real 1 kΩ in two free rows

- [ ] Straddle the ravine with it and confirm: `val=0.9?k`–`1.1k`, `ok=1`, with
  the detail line `R1: 1.02k (band 800-1.20k, 3.2mA @ 3.3V)`.
- [ ] **Pull it, re-run with `v`** → `val=open ok=0`, hint *"no conduction -
  part missing, or a leg not seated?"*, detail `R1: open (1uA at 3.3V across
  the rows; band 800-1.20k)`. This is the new shape — not a small current.
- [ ] **Both legs on the same side of the ravine** (so the two rows short) →
  `val=short ok=0`, hint *"reads as a short - are the legs bridging the
  ravine?"*. Worth doing once: it is the single most common miswiring on an
  `axial2` step, and it used to look like a pass.
- [ ] **The wrong value** — a 10 k in a 1 k step → `val=10.1k ok=0` and
  *"reads 10.1k, expected 1.00k +/-20% - wrong part?"*.
- [ ] **A resistor over 470 kΩ** (author one if you have a 1 M) →
  `val=toohighR`, *"resistance too large to measure - placed unverified"*.
  There is nothing to measure past that, so the check says so instead of
  guessing. (This replaced the old 0.15 mA-floor refusal.)

### 2.3 vf — a real LED

- [ ] Forward, red → `val=1.8?V ok=1`, band 1.40–2.60 V, detail
  `LED1 vf 1.87V @ 4.2mA (band 1.40-2.60V)`. **The current is new** — the value
  token itself is still just the voltage (no spaces in a machine token).
  Green/blue sit higher and may fall outside the shipped band; that is the band
  being honest, not a fault.
- [ ] **Reversed** → `val=2.9?V ok=0` and *"no current - LED missing or reversed
  (flip it?)"*. Missing and reversed are electrically identical from here, and
  the hint says both. `on_fail: retry` re-offers the step — flip it and confirm.

### 2.4 presence — a ≥100 nF cap, and the IC carve-out

- [ ] 2-leg cap, both rows free → `val=hold2.??V@<rowA> ok=1`. Pull it →
  `val=nocharge@<rowA>` or `decay@<rowA>`, `ok=0`.
- [ ] **presence on an IC** (any 555/eeprom/nand00 place step) →
  `val=ic_unverified ok=0 on_fail=warn` + *"unpowered IC can't be electrically
  confirmed - real verification happens at power_on"* + *"(check not run -
  continuing)"*. **This is by design.** Capacitance stays presence-only: that
  check keeps the OLD top-side chain deliberately, because it charges rather
  than measures current.

### 2.5 rail_sane — it changed meaning, and the change is the point

The old rule ("a power row within 0.2 V of the **setpoint**") is gone. Two gates
now: the **rail itself** within 0.25 V + 5 % of its setpoint, then each
power-class row within max(0.15 V, 3 %) of the **measured** rail, read on the
same ADC.

- [ ] Power up the 555 build. **`val=4.7?V@5 ok=1` is a PASS** on a 5.00 V rail —
  this board's rails genuinely run ~220 mV low, and the check finally knows it.
  (Wave 1 recorded `4.74V@8 ok=0` as a failure; it was the check that was wrong.)
  Detail: `rail: meas 4.77V (set 5.00V); worst row delta 0.01V @8`. The `@`
  names the worst **power-class** row — for the 555 that is VCC at row 5 or
  RESET at row 38, for eeprom/nand00 row 5, for i2cscrn row 6.
- [ ] **Lift a power-class pin off the rail** → `ok=0` with the row's own reading
  in the detail. Wire one to GND instead → it fails by volts.
- [ ] **Two new refusal strings** you may meet, both fail-safe:
  `leakX.XXmA` (the isense path is carrying current with the stimulus at 0 V —
  something is already connected to the rows under test), and `srcNNmA`
  (INA1 saw >50 mA *leaving DAC0*, i.e. fault current that bypasses the
  ground-side shunt — the hint names a leg in a grounded row). Neither should
  appear on a correctly built circuit; if one does, stop and look at the wiring.
- [ ] **A degraded shape worth recognising:** `val=~2.9mA`, with a **leading
  tilde**, means *"R was not measured — no sense route — and here is whether the
  current is plausible for the nominal value."* It is a current, not a
  resistance, and must not be read as one.
- [ ] **On your own older files** you may see `min/max look like legacy mA (…) -
  ignoring, using the value-derived ohm band`. That is the guard working, not a
  bug: a band that does not bracket `value:`, or a value-less part whose `max:`
  is under 5 Ω, is assumed to be pre-wave-2 milliamps.

### 2.6 §1.8 accuracy spot-checks, against a DMM

Nothing on the bench so far substantiates the accuracy claims — HIL can only
measure crossbar bridges. Measure each part on a DMM first, then read the
guide's number:

- [ ] 330 Ω and 4.7 k → within **±3 %**
- [ ] 10 k → **±4 %**  ·  47 k → **±7 %** (at 5 V)  ·  100 k → **±12 %**

All four sit comfortably inside their ±20–25 % bands — the bands verify the
*part*, not the meter. Note also the disclosed residual: the primary (ring) read
path cancels channel **offset** but not channel **gain**, worth ~1–2 % of each
node's voltage (≈38 mV on a 330 Ω part's 2.14 V drop, under 2 % of R). If the
bench finds it matters, the known fix is a second ring dwell with the channels
swapped.

### 2.7 The three starter projects, end to end

Common shape: **Projects → `<name>`** (or `z <name> new`), walk the guide, run
`main.py` when it is offered.

#### 2.7a `555` — NE555, 10k, 47k, 10 µF, LED, 330

- [ ] Placement, as the steps now say: 555 **pin 1 (dot) at row 35** (so pin 8 =
  VCC is row 5, pin 3 = OUT is row 37); 10k across **10/40**; 47k across
  **13/43**; 330 across **16/46**; 10 µF **+ row 18, − row 19**; LED **anode 22,
  cathode 23**.
- [ ] The three continuity steps and the vf step per §2.1–2.3.
- [ ] `power_on` → §2.5. Then the final step: `check=oscillates` on row 37 with
  `min: 0.3 max: 30` → `val=1.?Hz ok=1`, and the LED blinking at ~1.4 Hz.
- [ ] **Do one oscillates run with all 8 GPIOs deliberately claimed** to see the
  tap fallback's honest `val=osc ok=1` + *"tap fallback: both levels seen;
  frequency not measured (no free GPIO)"*.

#### 2.7b `nand00` — 74HC00 (DIP-14), LED, 330

- [ ] Placement: 74HC00 **pin 1 (notch end) at row 35** → A1=35, B1=36, Y1=37,
  GND=41, VCC=row 5. LED **anode 18, cathode 19**. 330 across **15/45**.
- [ ] Step 2 → `ic_unverified` (the carve-out). Step 3 (LED) → §2.3. Step 4
  (330) → `val=3?? ok=1`, band 264–396.
- [ ] `power_on` → `val=3.3?V@5 ok=1`. Then `check=voltage` on row 37 →
  `val=3.2?V ok=1` **and the LED visibly on** (both inputs pulled low → NAND out
  high). `val=float` means the chip is not seated.
- [ ] `main.py`: the four-row truth table, all `ok`, ending *"All four rows match
  NAND. That is a working gate."* **Watch the LED during the table — it must go
  dark exactly once, on the `1 1` row.**
- [ ] Live mode: `11` → `Y = LOW (expected 0)`, LED dark. `10` → `Y = HIGH`, lit.
- [ ] Failure shapes: all four `FLOATING` → chip unseated or rail off. `WRONG` on
  the last two rows only → an input leg (row 35 or 36) not making contact.
  `WRONG` on all four → not a 74HC00. A **74HC08** lights the LED on exactly one
  row (`1 1`) — AND is NAND's inverse. A **74HC02**'s pin 1 is an *output* while
  this wiring drives pin 1 from `RP_GPIO_1`: two drivers on one node — pull it
  out rather than leave it powered.

#### 2.7c `eeprom` — 24C02/24C16 (DIP-8), two 4.7 kΩ

- [ ] Placement: 24Cxx **pin 1 (the dot) at row 35** → A0/A1/A2 = 35/36/37,
  GND = 38, SDA = row 8, SCL = row 7, **WP = row 6**, VCC = row 5. Pull-ups
  across **12/42** (SDA) and **15/45** (SCL).
- [ ] Both pull-up steps → `val=4.?k ok=1`, band 3.76k–5.64k.
- [ ] `power_on` → `val=3.3?V@5 ok=1`. The step text says WP is held high —
  reads only.
- [ ] Verify i2c → `val=1dev ok=1`, `50` in the scanner grid. The guide **cannot**
  assert the address (`i2cScan` has no filter; `nDevices>0` is the whole pass) —
  the script owns the 0x50 assertion.
- [ ] `main.py` on an erased chip: a full `FF` dump ending `256 bytes read.
  (all 0xFF - an erased chip)`. A used chip shows real bytes + the ASCII gutter
  and no such tail.
- [ ] A 24C16: answer `2048` → the note `ADDR_BITS is 8, so past 0xFF wraps.` and
  a dump that **repeats every 256 bytes** (correct). A real 24C32+ needs
  `ADDR_BITS = 16`.
- [ ] **The write test** — answer `y`: `wrote 0xA5, read 0xA5 back - writes work`
  / `restored 0xFF at 0xFF.` / `write protect restored (row 6 on the rail).`
  Then prove the protection is real: answer `n` on a second run and check with
  `Q` that **row 6 is bridged to `TOP_RAIL`, not GND**. If you ever see
  `COULD NOT RESTORE WRITE PROTECT`, re-load the wiring before trusting the chip.
- [ ] Wrong address: `0x51` → A0 not reaching GND, `0x52` → A1, and so on.
  Nothing acks → most often the chip is in backwards; then check both pull-ups.

#### 2.7d `i2cscrn` — an SSD1306 4-pin I²C module

**Turn the top OLED off first** if it is enabled: in `connection_type 0` it
lives on GPIO 7/8 — these very pins — and shares i2c1 in every mode.

- [ ] Placement: module header **rows 5–8 — GND 5, VCC 6, SCL 7, SDA 8**.
  *Check the silkscreen* — some boards swap VCC/GND, and that is the one mistake
  that kills the panel.
- [ ] Step 2 → `ic_unverified` (correct). Step 3 (`power_on`) →
  `val=3.3?V@6 ok=1`; `val=float@6` or `val=0.00V@6` → module not seated.
- [ ] Step 4 (i2c) → `val=1dev ok=1`, `3C` in the grid.
- [ ] `main.py` first lines, exactly: `Type to Screen` / `project: i2cscrn` /
  `i2c devices: ['0x3c']` / `panel up: 128x32, 4 rows of 16 chars` / the prompt.
- [ ] Type `hello` + enter → `hello` **on the panel**, top-left, 8 px font. Three
  more lines → four stacked, oldest scrolls off. Empty line → **clears**. `q` →
  clears, `bye`.
- [ ] 128×64 panel: set `HEIGHT = 64` → `panel up: 128x64, 8 rows of 16 chars`.
- [ ] Garbage after a while → drop `I2C_HZ` to `50000`.

### 2.8 rail_sane replay

- [ ] With any project powered, deliberately set the top rail 1 V away from what
  the project asked for (`` `[power] `` or the rail adjuster) and re-run the
  `power_on` check with `v`. It must fail on the **rail-vs-setpoint** gate
  (`rail: meas 4.00V, set 5.00V - outside …`), not on the row gate — the two
  failures say different things and the detail line distinguishes them.

---

## 3. The accumulated bench-only items

Everything the HIL suites structurally cannot reach, each with where it came
from and what to do. **Board state is noted per item** so you can batch them.

| # | Item | Board |
|---|---|---|
| 1 | Ctrl+C vs wheel-hold discriminator | clear |
| 2 | wheel-hold inside a blocking call | clear |
| 3 | the pend race (click + flick) | clear |
| 4 | the pass-hold pend race | clear |
| 5 | MSC host-edit round-trip | clear |
| 6 | corrupt-run-file terminal state | clear |
| 7 | the interactive prompts | clear |
| 8 | probe buttons on the prompts | clear |
| 9 | the variant picker | clear (needs a file you add) |
| 10 | headless guided-complete **with** a script | parts optional |
| 11 | OLED-absent run | any (unplug the OLED) |
| 12 | the matrix-picker gate | any (diagnostic, only if a picker looks blank) |

- [ ] **1 — Ctrl+C vs the wheel hold** *(task 1)*. Run any long script and stop it
  **both ways**: `Ctrl+C` on port 1, and a 3-second clickwheel hold. Both must
  give `KeyboardInterrupt` → `--- script finished ---`, exactly once, and the
  prompt that follows must **not** self-cancel from that same hold
  (`waitForButtonRest`). The wave-1 note "it doesn't show that but it does exit"
  is the thing to re-check: the interrupt is now delivered via the flag alone,
  single-fire, and both delivery paths were rewritten.
- [ ] **2 — the wheel hold while a script is BLOCKED** *(task 1, the reason the
  fix was not the obvious one)*. Two shapes:
  (i) a script that catches `KeyboardInterrupt` and continues — the hold must
  interrupt it again, not just once;
  (ii) a script parked in `wait_touch()` / `probe_read()` /
  `probe_button_blocking()` — these C loops poll the interrupt flag *directly*
  and never see a scheduled exception, so a hold must break them out. This is
  the case a direct `mp_sched_keyboard_interrupt()` could never reach.
- [ ] **3 — the pend race, forward flick** *(task 7, blocking fix, no possible HIL
  needle)*. On the **last step of a completed build**: click the wheel, then
  **turn forward inside a quarter-second**. You must land in the DONE view **and
  stay there**. Before the fix, the pend matured after the turn had already
  landed you at DONE, and a CONFIRM at DONE on an all-built guide means EXIT —
  i.e. a wheel turn ended the guide.
- [ ] **4 — the pend race, impatient click** *(same fix, other half)*. On a
  passing check on the **final** step, click during the 900 ms pass-hold, in its
  last quarter-second. Same requirement: DONE, and stay. Then check the
  legitimate gesture still works — a plain click at DONE finishes (or jumps to
  the first unbuilt step).
- [ ] **5 — MSC host-edit round-trip** *(task 4, the harness cannot mount)*. Load
  a run file, mount the board as USB MSC, **edit the run file from the host**,
  eject. The edit must be **live** — this path used to discard host edits to a
  file context silently on eject.
- [ ] **6 — the corrupt-run-file terminal state** *(task 4 NEW-3; deterministic,
  not exotic)*. Over MSC, write **invalid** YAML (a `topRail: 99.0` is enough —
  `PowerState::validate` rejects anything past ±8 V) over the **active** run
  file and eject. The reload fails, the restore re-reads the same bad file and
  fails again, and the board enters the **no-active-context terminal state**:
  `Q` says `ACTIVE_SLOT:-1` with an **empty** `ACTIVE_PATH:`, **nothing is
  routed**, both rails at 0 V. That is a safe stop, not data loss. `<0` or a
  reboot recovers. Confirm the save-failure print is **throttled** (once per
  10 s), not once per idle pass.
- [ ] **7 — the interactive prompts** *(task 5; headless never prompts, by
  design)*. Cover the wording and the defaults on all three: the load/new prompt
  (§1.2), the script offer (§1.4), and the variant picker (#9). For each: a bare
  **click answers Yes**, a **hold cancels**, a **timeout cancels**, and the OLED
  text matches the terminal text.
- [ ] **8 — probe buttons on the new prompts** *(task 5)*. The control-surface
  principle says wheel + probe + serial everywhere. `yesNoMenu` is the shared
  idiom and nothing bespoke was invented, but the probe path was never
  exercised: CONNECT = yes, REMOVE = no, on every prompt above.
- [ ] **9 — the variant picker** *(task 5, dormant)*. Nothing bundled ships two
  `wiring*.yaml`. Drop a copy of `/projects/555/wiring.yaml` as
  `/projects/555/wiring.alt.yaml` with `meta.variant: alt`, then launch and
  answer **n** (start new): `VARIANTS n=2` with labels `default` / `alt`.
  Confirm it appears **only** on the start-new path — load-latest resolves its
  variant from the run file's own `runSource:` and never asks.
  Then, with runs present, try `z /projects/555/wiring.alt.yaml` with **no mode
  arg**: it resolves to LOAD and now says so —
  `(variant taken from runSource; the path argument was not used)`.
  Delete `wiring.alt.yaml` afterwards, or §5's variant count will be wrong.
- [ ] **10 — headless guided-complete WITH a companion script** *(task 5; both
  halves are tested separately, their junction is not)*. `z 555 new`, walk the
  guide to a clean DONE, and confirm `SCRIPT offer=/projects/555/main.py` →
  `SCRIPT action=run` → **the script actually runs**. Headless does not prompt;
  it runs unless you passed `noscript`.
- [ ] **11 — OLED-absent run** *(task 5)*. Unplug the OLED. Matrix + terminal
  alone must carry the whole flow, prompts included.
- [ ] **12 — if a picker shows nothing on the matrix** while OLED and terminal are
  fine: the pickers follow FileManager's unbracketed `b.clear()` / `b.print()` /
  `requestLedShow(2)` precedent rather than the menu-transition bracket — look at
  `menuTransitionCanShow()` in `main.cpp`.

---

## 4. OG smoke (needs an OG board)

Unchanged scope from wave 1.

- [ ] Any guided wiring on OG: terminal/OLED guidance, **no LED painting** from
  the guide, committed bridges lighting normally.
- [ ] Any guided project **with checks**: every check reports `val=unsupported
  ok=0` and warn-continues; the flow is otherwise as on V5.
- [ ] The three parts projects (`eeprom`, `i2cscrn`, `nand00`) are **V5-only** —
  the OG has no nodes for RP pins 26/27. Use the **555** for OG.

---

## 5. LAST — forced refresh. Do this after everything else.

It needs a **version-bumped flash plus a reboot**, which reflashes the board and
mutates `/projects` — so it must not precede any step above. Delete any
`wiring.alt.yaml` from §3 #9 first.

- [ ] Edit `/projects/555/wiring.yaml` **on the board**, then flash a build with a
  bumped `VERSION` so `checkAndHandleFirmwareUpdate()` fires. Confirm:
  (a) your edit **survives at the canonical path**; (b) the new default arrives
  as `/projects/555/wiring_original.yaml`; (c) the picker still offers **exactly
  one** variant for 555.
- [ ] Also confirm the **old-default-in-place** branch: flash an *unedited* 555
  twice across a version bump — it is updated in place, with no `_original`.
- [ ] **Regression probe for the `_original`-in-variant-glob fix (part (c)).** The
  variant scan globs `wiring*.yaml`, which `wiring_original.yaml` matches, so it
  used to show the backup as a second "variant". Fixed at `406c2d4`:
  `listVariantFiles` skips any name containing `_original`. This step is what
  proves it on real hardware, because it is the only path that actually
  *creates* a `_original` file. Expect **exactly one** variant. A spurious second
  entry means the exclusion regressed (or the backup is being written under a
  name that does not contain `_original`) — check `listVariantFiles` in
  `src/ProjectsApp.cpp` first.
- [ ] **Run files are not touched by provisioning.** Park a `555_1.yaml` in
  `/projects/555` across the refresh and confirm it comes back byte-identical.
  - **But an IN-PROGRESS run's resume bookkeeping is only advisory afterwards.**
    Compose the two items above: the run file survives byte-identical, carrying
    `guideProgress: {source: /projects/555/wiring.yaml, step: k}` — and the
    in-place branch just rewrote that wiring. Step *text* is always re-read from
    `guideSource` (States.h), so `k` and the INIT `committed[]` backfill now
    index a step list that may have changed length or order. Degradation is
    graceful by construction (the resume clamp reports `already complete` on a
    shrunk list, and `hasConnection`/`expandOnePart` guards mean no bridge is
    ever duplicated), but the *labels* can lie about what was actually built.
    After an in-place template update, either `p`/`v` re-walk anything doubtful
    or — the clean path — start a new run.
- [ ] Finally, put the bench back: active context on **slot 0**,
  `[slots] boot_mode = 1`, `boot_slot = 0`, rails and DACs at 0 V, and no
  leftover run files in the four project directories.
