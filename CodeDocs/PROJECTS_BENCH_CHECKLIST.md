# Projects Branch — Consolidated Bench Checklist

One ordered bench-session script for the projects-guided-placement branch,
consolidated from the task 3-9 reports (with task 9's Files-browser correction
applied). Extracted verbatim from the branch workspace's task-10 report §5 so it
survives workspace cleanup. Run top to bottom; the forced-refresh step is LAST
by design.


Every outstanding hands-on item from the task 3–9 reports, de-duplicated,
ordered so nothing invalidates a later step, with task 9's §6.4 correction
**applied in place**. Tasks 1, 2, 4 and 10 produced no bench items.

## 0. Prelude — do these first or you will chase ghosts

- [ ] **Close the `jumperless` terminal client on port 1.** Any stray byte
      cancels a picker (documented `selectNodeAction` convention) and any byte
      that isn't `y`/`n` makes `yesNoMenu` return −1. **If a picker closes by
      itself, or the keep-prompt instantly self-declines, this is the first
      suspect — not the launcher.**
      *Narrowed:* one specific cause is now handled in firmware — both the
      resume prompt and the keep-prompt drain leftover `\r`/`\n` before calling
      `yesNoMenu`, so a bare line terminator (post-`input()` CRLF remnant, a
      stray Enter) no longer self-answers either offer. The drain swallows
      **line endings only**, so every *other* stray byte still returns −1 —
      which leaves the port-1 client as the remaining suspect for a
      self-declining prompt.
- [ ] **If you plan to run `test_encoder_ui`:** regenerate `jl_input.py`'s ADDR
      table from the *currently flashed* ELF (values for the current build are
      in §2 above). It is per-build and lives outside version control.
- [ ] Note the board's `top_oled.lock_connection` / `sda_row` / `scl_row`. If the
      OLED is on breadboard rows, the guide's `oscillates` GPIO picker and the
      `i2cscrn` project both care.

## 1. No parts needed — format, files, launcher

- [ ] **Overlay orientation eyeball** *(task 6)*. From the REPL:
      `overlay_set_pixel(1, 10, 0x200000)` and
      `overlay_set_pixel(6, 10, 0x000020)`. Red must be the **TOP outer-edge
      hole** of column 10; blue the hole just **below the gap**.
- [ ] **Files browser opens a project wiring** *(task 3, corrected by task 9)*.
      Files → `/projects/555/` → click `wiring.yaml`. Expect the status line
      `Loaded slot: wiring.yaml` (**not** eKilo), top rail 5 V, ADC0/ADC1
      bridges present.
      **⚠ Corrected wording — loading is NOT wiring.** For the parts-only
      projects (i2cscrn, nand00, eeprom) expect **zero breadboard connections**
      after a Files-browser load: their parts are `placed: false` and only a
      guide commit expands them. The 555 is the exception — it has two ADC taps
      in `bridges:`. Use this path to confirm the file *opens and parses*, then
      wire the board through the guided build before running any `main.py`.
- [ ] **The slot-clobber trap did not fire.** Immediately after that click, press
      `Q` (or check the slot indicator): **the active slot must be unchanged**,
      and `/slots/slot0.yaml` must be byte-identical.
- [ ] Click `README.md` in the same directory → opens in **eKilo**, not a slot
      load.
- [ ] **Run a `main.py` from the Files-browser click menu** — *after* a guided
      build has wired the board. This is the only path that exercises
      `File::readString()` on a ~5.7 KB script, which no HIL check can reach.
- [ ] **Launcher, full flow** *(task 5)*. Apps → Projects → the picker now lists
      **five** projects (555, eeprom, hiltest, i2cscrn, nand00). Confirm
      navigation and the 7-char OLED row rendering for `i2cscrn` (longest name,
      exactly at the limit). Title + summary on the OLED, mirrored in the
      terminal.
- [ ] **Cancel matrix** *(task 5)* — every one must land you back on your
      original slot **with your original connections**, and the Projects app must
      still work the *next* time (that's the `temporarySlotActive` latch):
      (a) hold on the project picker; (b) hold on the slot picker after
      answering Yes; (c) No at the keep-prompt; (d) let the keep-prompt time out
      (15 s).
- [ ] **Decline restores prior slot** *(task 5)*: set up a few bridges by hand
      first, run a project, decline — your bridges must be back **on the
      breadboard**, not just in the file.
- [ ] **Keep-flow + power cycle** *(task 5)*: run 555 → Yes → pick an empty slot
      → unplug/replug. The saved slot comes back with the project wiring and `Q`
      reports that slot active.
- [ ] **Mid-script clickwheel-hold interrupt** *(task 5)*: hold during the 555
      script. Expect `KeyboardInterrupt` → `--- script finished ---`, and the
      keep-prompt must **not** instantly self-cancel from that same hold
      (`waitForButtonRest`).
- [ ] **Variant picker** *(task 5)* — nothing ships two `wiring*.yaml` today.
      Drop a copy of `/projects/555/wiring.yaml` as `wiring.alt.yaml` (set its
      `meta.variant` to `alt`) and confirm the second picker appears with labels
      `default` / `alt`.
- [ ] **OLED-absent run** *(task 5)*: unplug the OLED — matrix + terminal alone
      must carry the whole flow.
- [ ] **Guided flow from the launcher** *(task 6)*: Apps → Projects → 555 →
      destination picker (defaults to first empty slot, `GSLOTS n=8` on the
      terminal) → wheel through a couple of steps → watch `_GUIDE_TGT_` pulse and
      the pin-1 marker → hold to quit → relaunch → **resume offer** appears.
- [ ] **Probe controls** *(task 6)*: CONNECT advances, REMOVE goes back (check
      the polarity on your probe revision). Tap an unrelated row mid-step → net
      name prints. Tap a target row on a `probe_confirm` connect step → confirms.
- [ ] **Probe tap decode while DAC0 is parked at 0 V** *(task 6, concern #1)* —
      guide active, before `power_on`.
- [ ] **OLED prompts** *(tasks 6, 7)* — hotplug poll runs during waits; check
      hints now also render on check failures.

> **If a picker shows nothing on the matrix** while OLED and terminal are fine:
> the pickers follow FileManager's unbracketed `b.clear()` / `b.print()` /
> `requestLedShow(2)` precedent rather than the menu-transition bracket — look at
> the gate in `main.cpp` (`menuTransitionCanShow()`).

## 2. Real parts — the verify matrix (task 7)

Machine grammar to watch on port 1:
`GUIDE step=<i>/<n> id=<id> state=VERIFY check=<name>` then
`… state=RESULT check=<name> val=<x> ok=<0|1>[ on_fail=<policy>]`.
`z /projects/<name>/wiring.yaml <slot>` drives the same flow headlessly.

- [ ] **continuity — real 1 kΩ** in two free rows: expect
      `check=continuity val=2.6-3.3mA ok=1` (derived band 1.77–4.63 mA). Pull the
      resistor, re-run with `v`: `val=0.0xmA ok=0` + "current below band".
- [ ] **vf — real LED**: forward → `check=vf val=1.7-2.2V ok=1` (red; green/blue
      higher). Reversed → `val=2.9-3.0V ok=0` + "no current - LED missing or
      reversed (flip it?)".
- [ ] **oscillates — the 555 kit**: after `power_on`, with `min/max` around the
      blink rate and `timeout_ms` ≥ 3–4 periods, expect
      `check=oscillates val=<f>Hz ok=1`. **Also do one run with all 8 GPIOs
      deliberately claimed** to see the tap fallback's honest `val=osc` +
      "frequency not measured" (this is the §5.2a availability-keyed path).
- [ ] **presence — ≥100 nF cap** (2-leg, both rows free): expect
      `check=presence val=hold2.5-3.3V@<rowA> ok=1`. Pull the cap →
      `val=(nocharge|decay)@<rowA> ok=0`.
- [ ] **presence on an IC** (any 555 place step): `val=ic_unverified ok=0` +
      "(check not run - continuing)". **This is BY DESIGN, not a bug.**
- [ ] **i2c — any powered device** (step with n1=SDA row, n2=SCL row, after
      `power_on`): `check=i2c val=1dev ok=1`.
- [ ] **rail_sane with class-tagged pins**: after a `power_on` confirm, expect
      the pass; then lift a gnd-class pin off the rail to see `ok=0` +
      "gnd-class row is off 0V".
- [ ] **The CURR_SENSE sink** *(task 7 finding #1 — worth knowing)*: bare board,
      `connect(DAC0, ISENSE_PLUS)`, `dac_set(DAC0, 3.3)` → `ina_get_current(0)`
      reads **~2.3 mA with nothing downstream** (~1.44 kΩ effective to ground,
      −0.06 mA at 0 V). Prime suspect is U12's DAC_1-repurpose path hanging on
      CURR_SENSE. The checks subtract a per-run dead-part baseline either way,
      but confirm the story — this false-passed an EMPTY row against a ~2 mA
      band before the baseline existed.

## 3. The three starter projects (task 9)

Common: **Apps → Projects → `<name>`**, pick a destination slot, walk the guide
(turn = prev/next, click = confirm, hold = exit), then run `main.py` when
offered. `z /projects/<name>/wiring.yaml 3` is the headless equivalent.

### 3.1 `i2cscrn` — needs an SSD1306 4-pin I²C module

**Turn the top OLED off first** if enabled: in `connection_type 0` it lives on
GPIO 7/8 — these very pins — and shares i2c1 in every mode.

- [ ] Placement: module header rows 5–8 — **GND 5, VCC 6, SCL 7, SDA 8**.
      *Check the silkscreen* — some boards swap VCC/GND, and that is the one
      mistake that kills the panel.
- [ ] Step 2 (place DISP): `check=presence val=ic_unverified ok=0 on_fail=warn`
      + "unpowered IC can't be electrically confirmed". **Correct behaviour.**
- [ ] Step 3 (power_on): `check=rail_sane val=3.3?V@6 ok=1`. `val=float@5` or
      `val=0.00V@6` → module not seated.
- [ ] Step 4 (verify i2c): `check=i2c val=1dev ok=1`, `3C` in the scanner grid.
- [ ] `main.py` first lines, exactly:
      `Type to Screen` / `project: i2cscrn` / `i2c devices: ['0x3c']` /
      `panel up: 128x32, 4 rows of 16 chars` / the type prompt.
- [ ] Type `hello` + enter → **`hello` on the panel**, top-left, 8 px font. Three
      more lines → four stacked, oldest scrolls off. Empty line → **clears**.
      `q` → clears, `bye`.
- [ ] 128×64 panel: set `HEIGHT = 64` → `panel up: 128x64, 8 rows of 16 chars`.
- [ ] Garbage after a while → drop `I2C_HZ` to `50000`.

### 3.2 `nand00` — needs a 74HC00 and an LED

- [ ] Placement: 74HC00 across the gap, **pin 1 (notch end) at row 5**. LED long
      leg row 18, short leg row 19. 330 Ω rows 15–16.
- [ ] Step 2 (place U1): `val=ic_unverified ok=0 on_fail=warn` — the carve-out.
- [ ] Step 3 (place LED1): `check=vf val=1.8?V ok=1` for red (band 1.4–2.6 V). A
      reversed LED fails and `on_fail: retry` re-offers the step — intended.
- [ ] Step 4 (place R1): `check=continuity val=7.?mA ok=1` (band 4.4–10.7 mA).
- [ ] Step 5 (power_on): `check=rail_sane val=3.3?V@35 ok=1`.
- [ ] Step 6 (verify): `check=voltage val=3.2?V ok=1` and **the LED visibly on**
      (both inputs pulled low → NAND out high). `val=float` → chip not seated.
- [ ] `main.py`: the four-row truth table, all `ok`, ending "All four rows match
      NAND. That is a working gate." **Watch the LED during the table — it must
      blink off exactly once, on the `1 1` row.**
- [ ] Live mode: `11` → `Y = LOW (expected 0)`, LED dark. `10` → `Y = HIGH`, lit.
- [ ] Failure shapes: all four `FLOATING` → chip unseated or rail off.
      `WRONG` on the last two rows only → an input leg (row 5 or 6) not making
      contact. `WRONG` on all four → not a 74HC00. A **74HC08** lights the LED on
      exactly **one** row (`1 1`) — AND is NAND's inverse. A **74HC02**'s pin 1
      is an *output* while this wiring drives pin 1 from `RP_GPIO_1` — two
      drivers on one node, pull it out rather than leave it powered.

### 3.3 `eeprom` — needs a 24C02/24C16 and two 4.7 kΩ

- [ ] Placement: 24Cxx across the gap, **pin 1 (the dot) at row 5**. 4.7 k in
      rows 12–13, another in 15–16.
- [ ] Step 2 (place U1): `val=ic_unverified ok=0 on_fail=warn`.
- [ ] Steps 3 and 4 (the pull-ups): `check=continuity val=0.6?mA ok=1` each
      (band ≈0.41–1.46 mA).
- [ ] Step 5 (power_on): `check=rail_sane val=3.3?V@35 ok=1`. The step text says
      WP is held high — reads only.
- [ ] Step 6 (verify i2c): `check=i2c val=1dev ok=1`, `50` in the grid. Note the
      guide **cannot** assert the address (`i2cScan` has no filter; `nDevices>0`
      is the whole pass) — the script owns the 0x50 assertion.
- [ ] `main.py` on an erased chip: a full `FF` dump ending
      `256 bytes read.  (all 0xFF - an erased chip)`. A used chip shows real
      bytes + the ASCII gutter and no such tail.
- [ ] Bigger part: answer `2048` for a 24C16 → note
      `note: ADDR_BITS is 8, so past 0xFF wraps.` and a dump that **repeats every
      256 bytes** (correct). Real 24C32+ → set `ADDR_BITS = 16`.
- [ ] **The write test** — answer `y`, expect
      `wrote 0xA5, read 0xA5 back - writes work` / `restored 0xFF at 0xFF.` /
      `write protect restored (row 36 on the rail).`
      **Then confirm the protection is real**: answer `n` on a second run and
      check with `Q` that **row 36 is bridged to `TOP_RAIL`, not GND**. If you
      ever see `COULD NOT RESTORE WRITE PROTECT`, re-load the wiring before
      trusting the chip.
- [ ] Wrong address: `0x51` → A0 not reaching GND, `0x52` → A1, etc.
      Nothing acks → most often the chip is in backwards; then check both
      pull-ups.

## 4. OG smoke (needs an OG board)

- [ ] Any guided wiring on OG *(task 6)*: terminal/OLED guidance, **no LED
      painting** from the guide, committed bridges lighting normally.
- [ ] Any guided project **with checks** *(task 7)*: every check reports
      `val=unsupported ok=0` + warn-continue; flow otherwise as on V5.
- [ ] Note the three task-9 projects are **V5-only** (the OG has no nodes for RP
      pins 26/27) — use the 555 for OG.

## 5. LAST — forced refresh (task 8). Do this after everything else.

It needs a **version-bumped flash plus a reboot**, which reflashes the board and
mutates `/projects` — so it must not precede any step above.

- [ ] Edit `/projects/555/wiring.yaml` **on the board**, then flash a build with
      a bumped `VERSION` so `checkAndHandleFirmwareUpdate()` fires. Confirm:
      (a) your edit **survives at the canonical path**; (b) the new default
      arrives as `/projects/555/wiring_original.yaml`; (c) the picker still
      offers **exactly one** variant for 555.
- [ ] Also confirm the **old-default-in-place** branch: flash an *unedited* 555
      twice across a version bump — it should be updated in place with no
      `_original` file.
- [ ] **Regression probe for the `_original`-in-variant-glob fix (in (c)):**
      the variant scan globs `wiring*.yaml`, which `wiring_original.yaml`
      matches — so it used to show the backup as a second "variant". That was
      **fixed at `406c2d4`**: `listVariantFiles` now skips any name containing
      `_original`. This step is what proves the fix on real hardware, since
      it is the only path that actually *creates* a `_original` file. Expect
      **exactly one** variant for 555. A spurious second entry means the
      exclusion regressed (or the backup is being written under a name that
      doesn't contain `_original`) — check `listVariantFiles` in
      `src/ProjectsApp.cpp` first.
