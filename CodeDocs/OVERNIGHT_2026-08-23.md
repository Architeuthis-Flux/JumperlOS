# Overnight run — 2026-08-22 into 08-23

You asked me to run all night, find bugs and weird UI interactions, and fix
them. This is what happened. Sections marked **PENDING** were still moving when
you woke up; the ledger at
`.superpowers/sdd/projects-wave-2-bench-notes/progress.md` has the live state,
and every finding lives in `HUNT_FINDINGS.md` beside it.

---

## The short version

Wave 3 finished and closed (six tasks, each reviewed). Then a twelve-lens hunt
over the whole branch produced **55 findings that survived two independent
skeptics**, of which the sharpest are below. Separately, a defect that had been
dismissed three times as "transient, a reboot cleared it" turned out to be real,
attributable to the byte, and to have a **user-facing half nobody had noticed**.

Two things I want you to read even if you read nothing else:

1. **The i2c check destroys the wiring you just built.** It tears down the
   project's own SDA/SCL bridges every time it runs, which means the shipped
   `eeprom` and `i2cscrn` guides break at their verify step. (Fix: batch H1.)
2. **Your board's script memory never resets.** MicroPython initialises once per
   boot and is never torn down, so every script a user runs permanently consumes
   part of one shared heap. Fresh, the largest allocatable block is ~29 KB; the
   shipped `i2cscrn` script is 12.3 KB. **A user is two or three medium scripts
   from a cliff** — and at that cliff the board prints `--- script finished ---`
   right after a bare `MemoryError`, so it reads as "my script is too big" or
   "my board is broken" when the truth is "reboot and it works."

---

## What the night changed on the board

### Your double-click rule, implemented — and it was hiding a real bug

You said the encoder's double-click should not be a thing. It is gone
(`7a6023f`). What made it worth more than a policy change: the tree's click
idiom is `RELEASED && last == PRESSED`, and the driver was substituting a
`DOUBLECLICKED` state for the second fast press — which **most consumers do not
match, so the second click was silently dropped.** The driver even carried a
diagnostic comment about the missed-click question. Removing it should make fast
clicks land. The probe's double-tap is untouched (separate feature, out of
scope), and the rule is saved so future work inherits it.

**One consequence to feel at the bench:** a fast double press in a guide now
confirms twice. That is two clicks doing two things, which is the rule's honest
meaning, and it is recoverable (browse back, `p` un-commits). If you hate it,
the fix belongs in the guide, not back in the driver — say the word.

### The MemoryError, solved and measured

- The crash itself was a string accumulator: five places built data with
  `data += chunk`, and MicroPython requests `len+1` bytes for a string — so a
  2048-char buffer asks for **2049** and a 3114-char one asks for **3115**,
  which are exactly the two numbers from the old crash reports.
- Underneath it, the test harness never reset the device's global namespace, so
  55 names accumulated across a session; `test_config` alone permanently cost
  **11 KB of contiguous heap per run**. After the fixes, it costs **14 bytes**.
- The falsifier passed: **two full `run_all` cycles back-to-back with no reboot
  between, 11/11 both times, zero MemoryErrors** — the exact condition all three
  original crashes happened under. The heap handed between cycles went from 6123
  bytes of largest block to 25162.
- Worth knowing: a microbenchmark told that agent its own fix was a net loss.
  The real-suite A/B showed the opposite — without it `test_config` costs 27 KB
  of contiguity, *worse than the original bug*. It measured in the real sequence
  instead of trusting the cheap number.

### A test that could lie, caught in the act

`test_projects` had a `nomem` branch that printed an informational line and
never recorded a check — so on a fragmented heap the suite quietly asserted less
while still reporting PASS. It is now a real check, and on its first honest run
it **passed**, meaning both shipped scripts genuinely compiled. Every suite is
being swept for that shape.

---

## The findings, ranked (55 survivors + 8 adjudicated disputes)

Full detail — scenario, mechanism, proposed fix, and both skeptics' reasoning —
is in `.superpowers/sdd/projects-wave-2-bench-notes/HUNT_FINDINGS.md`. Batched
into six fix tasks, briefed with my rulings attached:

| batch | what it fixes | status |
|---|---|---|
| **H1** | the critical i2c teardown, i2c never routing its pins, stimulus into driven rails, `rail_sane` vs the bottom rail, an orphaned tap after a timeout, and a voltage pre-check before the frequency probe touches a GPIO | **DONE**, reviewed twice |
| **H2** | the harness: a teardown that **deletes your run files**, a control that cannot fail, a restore that reports success while restoring nothing, `board_state_restore` checking a message instead of state | **DONE**, device-verified |
| **H3** | the guide: **resume promotes skipped steps to committed**, so a skipped `power_on` silently re-energizes your rails at 5 V | **item 1 DONE**, reviewed; its other items queued |
| **H4** | data corruption: parts truncated then erased, `remove_part` re-indexing under a live guide, unescaped net names in YAML, an unterminated quote deleting a whole part | briefed, queued |
| **H5** | context failure paths: a truncated file adopted as your context, a corrupt run file dead-ending a project, `deleteSlot` re-opening the slot-0 clobber | briefed, queued |
| **H6** | interaction: an out-of-bounds colour-table read, modal re-entrancy, latched probe presses replaying later, and **the handshake byte that has been cancelling your pickers all along** | briefed, queued |

That last one deserves a note: the "close your terminal client or it cancels
pickers" workaround in the bench checklist was never the client misbehaving —
the host's ENQ/DC4 handshake byte is being treated as user input.

---

---

## Two incidents from the night that outrank the fixes

### Your board HardFaulted on a clean bench

An `operator new` allocation abort, ~21 minutes into a run, mid-`test_config`;
clean after a reboot, and the evidence says it was not the batch's own code
(the changed code does not execute in that suite, static RAM was byte-identical
between runs, both suites pass standalone). Two things make it worth your time
anyway. **The crash latcher built earlier the same night caught and symbolized
it** — the instrument that exists because your delete-crash could not be
reproduced earned itself within hours, on the same abort family. And it fits
the heap finding above: one shared, never-torn-down heap plus a long session is
exactly the pressure case.

### A green test suite laundered corruption — and the clamp helped it

The aborted run died *after* planting its calibration sentinels. Because
`test_config` restores to its **own** phase-0 snapshot, the two later runs
re-baked the corruption and **passed green**. Calibration is repaired
(`probe_droop_ohms` back to 0.0, `probe_droop_v0` to 3.160).

The detail that makes this a real finding rather than an anecdote: the board was
left at `probe_droop_v0 = 3.350`, **not** the planted `2.599` — configManager's
`[3.0, 3.6]` clamp had already rewritten the deliberately-implausible sentinel
into a perfectly believable value. Half the poison was laundered before a human
could see it, and 3.350 would have been snapshotted and believed forever. So
"add a plausibility check" is not a sufficient fix; the write-up recommends a
phase-0 breadcrumb ledger, with the plausibility gate as the cheap half that can
land first. Full mechanism in `task-h2-report.md`.

**This also cost you data.** `/slots/slot2.yaml` now holds HIL fixture wiring —
proving one of the harness bugs was real required running the pre-fix teardown,
which by construction skips its slot restore. That is the second loss tonight
(a killed run took `/slots/slot3.yaml` earlier). Both are the exact class H2
fixed. Reload slot 2 if it mattered.

---

## The board you woke up to

A full `run_all` from a fresh boot, run after everything above landed:
**PASS, 10 of 11 suites green + 1 SKIP, exit 0**, no crashlog on the boot banner,
bench left at slot 0 / `boot_mode 1` / ports free. Per-suite: micropython_fs 5,
routing 5, net_currents 8, config 31, stress 5, paste_state 13, slot_files 96,
parts_roundtrip 179, projects 280, guide_flow 425; `test_encoder_ui` skips on a
stale SWD ADDR table, which is pre-existing and expected. The suites grew from
1082 checks to 1247 overnight, and several of those new ones exist because a
control that could not fail was replaced with one that can.

The firmware on your board is committed as the tracked `firmware.uf2`.

## Where the next session starts

Three things are queued and specified, in this order:

1. **`States.cpp:1347`** — a torn or garbage `skipped:` value reads as an
   authoritative zero, which defeats the legacy refusal that stops an old file
   from energizing your rails. Same class as the bug just fixed, reachable
   through file corruption. One-line fix (use the parse's endptr), needs a flash.
2. **`GuidedFlow.cpp:2079`** — `p` un-commits the *wrong* step, and this got
   more dangerous overnight: now that skips persist, pressing it on a browsed
   step can erase a skip decision from a previous session and write that
   erasure to disk.
3. **Batches H4, H5, H6** — briefed with rulings attached, covering data
   corruption in the parts table, the context layer's failure paths, and the
   interaction defects including the handshake byte that has been cancelling
   your pickers.

## What needs you

1. **The three decisions** in `BENCH_NEXT_SESSION.md` §6 still stand (the
   cancel-out-of-slider rail behaviour, what "clears the data lines" should
   mean, whether the crash latcher stays).
2. **The double-confirm question** above.
3. **Bump `VERSION` at release.** Provisioning is version-gated, so none of the
   project content fixes reach a user's board until it does.
4. ~~**One deliberate loss to accept or overturn:** the shipped 555's final
   "is it blinking?" check now reports `unmeasured` instead of passing...~~
   **ANSWERED 2026-08-23** — you asked for the 555 to stop enforcing values and
   just compute the expected frequency and duty cycle from what was placed, so
   this one resolved itself. The `oscillates` step is gone (a step that can only
   ever report `unmeasured` is worse than no step); `main.py` times the blink
   off ADC0 and prints it beside what the *measured* resistors predict. The
   sampled-tap frequency estimator that would have restored the old check is
   therefore **dead scope** — do not build it. Details in the 555's README and
   `BENCH_NEXT_SESSION.md` §0a.

## What I deliberately did not do

- No firmware feature was invented at 4 a.m. Where a fix needed a design
  decision with a real tradeoff (giving each script a fresh interpreter, which
  would reset a REPL session out from under someone), it is written up and
  handed to you rather than guessed at.
- Batches H5 and H6 are briefed but not run — the board is one resource, every
  fix needs a build, a flash and a suite run, and I would rather four batches
  landed verified than six landed hopeful.
