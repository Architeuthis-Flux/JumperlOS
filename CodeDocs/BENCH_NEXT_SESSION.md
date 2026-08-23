# Bench session — the wave-3 firmware

One pass, top to bottom, ~45 minutes with parts (~15 without). Nothing here
invalidates a later step. The long reference is
[PROJECTS_BENCH_CHECKLIST.md](PROJECTS_BENCH_CHECKLIST.md) — this doc is the
session; that one is the detail when something looks wrong.

**On the desk:** the V5, a 555 kit (NE555, 10k, 47k, 10 µF, LED, 330), a
74HC00, a 24C02 + two 4.7k, an SSD1306 module, a DMM, and a 1k you trust.

---

## 0a. Today's two (5 min) — start here

Added 2026-08-23, after your two reports. Both are on the board already
(firmware **5.7.4.2**; the version bump is what re-provisions `/projects`).

- [ ] **`i2cscrn`, end to end.** The root cause was not the wiring and not your
  panel: `machine.I2C.scan()` probes every address with a **zero-length write**,
  the RP2 block cannot emit one, so it falls back to bit-banging — and the
  port's open-drain helpers were both polarity-inverted *and* never took the pad
  off the I2C peripheral. The bit-bang landed nowhere, the idle peripheral held
  SDA/SCL on their pull-ups, and every address NAKed. Exactly "the I2C lines are
  just held high". Proven fixed on hardware (a scan that took 5.6 s of timeouts
  now takes 16 ms), but **your panel answering is the confirmation I could not
  make myself** — the board had no I2C device routed while I was on it.
  - If it still fails: the script now says `every address answered - SDA is
    stuck low` when that is what is happening, which is the other way this
    looks. That one means a pull-up, not a fix.
- [ ] **The 555 with deliberately wrong parts.** Put a **22k where the 47k
  goes** (or anything 1k–100k). It must **pass**, print
  `R2: 22.1k measured (value: says 47.0k - not enforced, ...)`, and `main.py`
  must then predict a *slower* blink that matches what the LED actually does.
  This is the half of `enforce: false` I could not prove without a real
  resistor — the check runs *before* the part's bridges exist, so no fixture can
  fake conduction (the suite records that constraint at phase 12d).
  - [ ] **Pull the resistor out entirely** → must still **fail** with
    `open`, and the message must **not** quote a band any more.
  - [ ] **`measured` reaches Python**: after the build, `list_parts()` should
    show real ohms on R1/R2/R3. It is RAM-only — reboot and it is 0.0 again and
    the script says it fell back to the file.

---

## 0. Two ghost-chasers, 30 seconds

- [ ] **Close the `jumperless` terminal client on port 1.** A stray byte
  cancels pickers and answers prompts. It is the first suspect for anything
  that closes by itself.
- [ ] **Pull every part out of the board.** Part 1 assumes empty holes; several
  checks read "empty" as their pass.

If anything reboots during this session, the firmware now tells you why on the
next boot: look for `[crashlog]` and, new this wave, an `[abort]` line naming
the *caller*. Paste both lines — that is the whole diagnosis.

---

## 1. No parts — what wave 3 changed (15 min)

- [ ] **`Guides` renders whole.** Top-level menu, the row before `Apps`, six
  glyphs, no split at the S. (Machine output still says `PROJECTS n=4` on
  purpose — scripts and HIL depend on it.)
- [ ] **The 555 opens clean.** `z 555 new`, then `b`: **nothing on rows 1-60.**
  The two ADC taps that used to be pre-wired are gone; `main.py` connects its
  own now and removes them when it exits.
- [ ] **One run file, not twenty.** Launch `555` → `/projects/555/555_run.yaml`,
  no prompt. Quit mid-build and relaunch → **one** prompt, and it must say
  the fresh option **OVERWRITES**. Finish a build, relaunch → no prompt, it
  just reopens. Cancel at that prompt → your previous context is untouched.
  Keeping a build is `Slots` → `save to`.
- [ ] **The click is instant again.** No quarter-second lag, no ADJUST mode,
  no double-click gesture — all of it is gone.
- [ ] **Two fast clicks are two clicks now.** In any menu, click-click fast:
  **two** actions. The old firmware ate the second one — that was the driver
  calling it a "double-click" and no menu knowing what that meant. Then click a
  few sloppy singles: **exactly one** action each. A doubled action on a single
  click is the thing to report — it would mean the button is bouncing, which
  the board's RC on the encoder pin is supposed to prevent.
- [ ] **The drag trail is gone.** In a guide, move a part with the probe: tap a
  free hole, then another, then another. Old footprints must go dark as it
  moves. *(This is the one wave-3 headline with no automated witness.)*
- [ ] **A wheel turn still cannot leave a guide.** Spin past the last step →
  DONE view → keep spinning → it wraps to step 1. Only hold/`q` exits.
- [ ] **Boot comes back where you left it.** Leave the board on a run file,
  unplug, replug: same context, wiring live.

---

## 2. The encoder rail menu — four checks I could not automate (2 min)

Its loop reads the encoder and probe; serial only cancels it. Everything about
it in wave 3 is verified by reading code, not by running it.

- [ ] **Does a rail set from the menu survive?** Set a rail, switch slots, come
  back. Still there? *(If not, the menu's edit is not being marked dirty.)*
- [ ] **Does a no-touch visit stay quiet?** Open the slider, short-press without
  turning, back out. Nothing should be written.
- [ ] **Does a cancelled node pick stay quiet?** OUTPUT → Voltage → cancel the
  node selection. Same.
- [ ] **Cancel out of the slider — does the rail move?** This one may be a real
  bug, and it predates wave 3: the cancel path never sets the action's voltage,
  so the rail may snap to a stale value *and persist it*. **If it moves, tell
  me and I'll fix it.**

> Checks 2 and 3 are invisible by nature (a spurious rewrite is byte-identical),
> so mark the file first:
> `fs_write('/slots/slot2.yaml', fs_read('/slots/slot2.yaml') + '\n# probe\n')`,
> do the menu action, wait 5 s, then
> `print('# probe' in fs_read('/slots/slot2.yaml'))` → **True means nothing was
> rewritten, which is the pass.**

---

## 3. Companion scripts — the silent-failure find (3 min)

Until this wave a companion script over ~6 KB **ran nothing and said nothing**.
The shipped `eeprom` script is 6441 bytes, which is why its end-to-end row was
never ticked.

- [ ] **`eeprom`'s `main.py`, end to end.** It must print `EEPROM Dumper` and a
  scan result — not `Running …` straight to `--- script finished ---`.
- [ ] **`555`'s and `i2cscrn`'s too**, same reason.
- [ ] If one ever goes quiet again it now *says why* — look for
  `Out of memory launching …` and send me the size and free heap it names.

---

## 4. Real parts — the measurement pass (15 min)

This is the wave-2 payoff you have not seen yet: **continuity reports ohms**,
four-wire, so the crossbar's own resistance drops out of the number.

- [ ] **A 1 kΩ straddling the ravine** → `val=0.9?k`–`1.1k`, `ok=1`, and a
  detail line naming the band and the current.
- [ ] **Pull it, press `v`** → `val=open`, "no conduction — part missing, or a
  leg not seated?" (not a tiny current any more).
- [ ] **Both legs on the same side** (so the rows short) → `val=short`, and the
  hint asks whether the legs bridge the ravine. This was a silent pass before.
- [ ] **A 10 k in a 1 k step** → `reads 10.1k, expected 1.00k ±20% — wrong
  part?` **Use `nand00` or `eeprom` for this one, not the 555** — the 555's
  resistor steps are `enforce: false` as of 2026-08-23 and will report the
  10 k and pass. That is the point of §0a; this bullet is the *enforcing*
  behaviour, which every other project still has.
- [ ] **The 555 kit, guided, end to end.** Its 47k should now read as a
  resistance instead of the 0.03 mA quantization floor that failed last time,
  and the LED's vf step should route — the fabric that refused three times on
  your last pass now measures. Watch for `val=noroute`; if you see it, that is
  the one regression I most want to hear about. The final step is a **note**
  now, not an `oscillates` verify: that check drives a 3.3 V GPIO onto the
  target and refuses a node that can reach this project's 5 V rail, so it could
  only ever report `unmeasured`. `main.py` times the blink instead, and can
  compare it to the parts.
- [ ] **Spot-check three values against the DMM** (330, 10k, 47k). Claimed:
  ±3 % up to 10 k, ±7 % at 47 k. Tell me if reality is worse — the bands are
  author-owned and easy to widen.
- [ ] **`power_on` and `rail_sane`**: your 4.74 V on a 5 V rail should now
  **pass** — rows are compared against the measured rail, not the setpoint.
- [ ] **`nand00`** and **`eeprom`** end to end (both are DIP-flip re-authored:
  pin 1, the dot, sits at **row 35** on the bottom half now — the way a real
  chip actually sits).

---

## 5. `i2cscrn` — the rebuilt flow (8 min, needs the module)

Do it on a **bare board**; that is the case it was rewritten for.

- [ ] **Tap each signal** (GND/VCC/SCL/SDA) with the probe. Then run it again
  and **type** the rows instead — both work at the same prompt.
- [ ] **Put the module somewhere other than rows 5-8** and confirm the readback
  names the rows you actually chose.
- [ ] **Start it with the module not yet placed** and watch the beacon bring
  the panel to life the moment the last wire lands.
- [ ] **Pick the wrong panel size on purpose**, confirm the driver list works.
- [ ] **`q` on exit** — the routes it made are gone.

---

## 6. Three decisions I need from you

1. **Cancel-out-of-the-slider (§2 check 4).** Pre-existing. If it moves the
   rail, do you want it fixed, or is that the intended "cancel keeps what you
   dialed" feel?
2. **What "clears the data lines" should mean.** The i2cscrn script currently
   removes only the routes *it* made — so after a guided build placed them, it
   removes nothing. Should it instead clear the signal rows regardless of who
   wired them?
3. **The crash latcher stays?** ~150 lines, zero cost unless the board aborts,
   and it is what will name the cause if your delete-crash ever returns. Keep
   it permanently, or make it debug-only?

---

## 7. What I could not prove, and what is still open

**Bench-only by nature** (no automated witness exists): the drag trail, the
probe gestures themselves, the encoder menu, the i2cscrn probe/animation path,
and all real-part accuracy.

**Known open, deferred deliberately:**
- Net colours are still assigned on core 2; a colour can be one render stale
  after a net-set change (self-correcting, cosmetic — the destructive
  "everything black" version is fixed).
- Resume tracks one step index, so committing out of order makes you re-walk
  the later steps.
- `switch_slot()` from a no-context state succeeds then raises; the agreed
  shape is recorded but not built.
- **Provisioning is version-gated**: user boards keep the old READMEs, the old
  555 wiring *with* its taps, and the old i2cscrn script until `VERSION` bumps.
  Bump it at release or none of section 1 reaches anyone but you.

**Your old notes, answered:** the delete-a-run-file reboot could not be
reproduced (five faithful attempts; the flow is sound) — the latcher exists to
catch it if it returns; the too-many-files complaint is the single run file in
§1; "this whole thing is confusing" is the ADJUST mode being gone.

**Housekeeping:** `/projects/555/555_run.yaml` on your board right now is a
103-byte test leftover — safe to delete. And `/slots/slot2.yaml` holds HIL
fixture wiring (11-12, 13-14, 41-42, 53-54, top rail 1.80 V), not yours: the
overnight suite uses slot 2 as its scratch pad, and a run that was killed
part-way never put your version back. Reload slot 2 if it held something you
wanted — the harness fix that stops the next one from happening landed the same
night.
