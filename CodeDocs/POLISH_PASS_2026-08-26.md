# Polish pass — 2026-08-26 (overnight, toward 5.7.7.0)

The first full pass of the "Snow Leopard" cycle: no new functionality, only
verification, latency, and coherence. Everything below ran between 00:53 and
~04:00 with the bench board on the debug probe.

## What ran

1. **48 audit fixes regression-checked** against the current tree, one
  adversarial checker each → `FIXCHECK_2026-08-26.md`. 16 clean; the rest
   had open sibling paths or residuals, now largely closed (below).
2. **53 deferred findings adversarially verified** (the audit doc's "next
  pass work-list") → `PENDING_VERDICTS_2026-08-26.md`. 40 confirmed, 9
   refuted, 4 mixed.
3. **12 interaction paths latency-traced with arithmetic** + 3 unification
  lenses → `LATENCY_AUDIT_2026-08-26.md`. This is the map for the rest of
   the 5.7.7 cycle and the V6 paradigm.
4. **Full HIL baseline on 5.7.6** exposed three real failure clusters, each
  root-caused on hardware (see below).
5. **~90 verified fixes applied** in 12 batch commits (`cc4d80d..5fefeee`),
  both targets building green, then flashed over SWD and re-HIL'd.



## The three HIL clusters, root-caused

- **Projects launcher dead** (7 failures): the "Parts menu is born" commit
deleted the `apps[]` row that was `projectsAppLauncher`'s only caller.
Restored as name-dispatch only; the menu-level Guides retirement stands.
- **A plain slot load rewrote the slot file** (the no-write canary):
`parkDacAtMeasureTarget` persisted the calibrated 3.32 V over fixtures'
3.33 V — 10 mV > the 5 mV epsilon → `markDirty()` → idle auto-save. The
park now writes state without dirtying. Latent since "Probe power feeds
from DAC0"; self-limiting on the bench, which is why it was green before.
- **Abandoned interactive script wedged the raw REPL until reboot**: the
interrupt check peeks only the head byte of a FIFO nothing drains
mid-script, and the standard client opener leads with CR — one stray 0x0D
masked every Ctrl-C behind it. Fixed with a tightly-scoped CR/LF skip.
(The "broken q quit" was the harness sending bare `q` with no CR —
readline semantics, not a firmware bug. The firmware's script stdin path
was byte-level verified clean on the bench.)



## Corrections to earlier ledgers

- Audit finding #51 ("FS wipe suppresses re-provisioning") is **projects-
irrelevant**: `files_provisioned` gates only `images/*.bin`. May still
apply to images; unverified.
- `test_guide_flow.py` no longer exists at HEAD (deleted with the blocking
guide runner); `run_all.py`'s TESTS list still names it, and
`test_projects.py` still drives the retired `GUIDE step=` machine in dead
phases. **Harness salvage needed** (deliberately not done tonight — test
assertions are not mine to rewrite unreviewed).
- Pending #0 (PARTPICK `\r\n`), #8/#12 (rotaryDivider), #10 (menu-cancel
render drop), #15 (fs_mutex), #37 (I2C scan), #48 (lastChipXY), #52
(show_in_terminal) — **refuted**, do not re-chase.



## Deferred work-list (verified real, not fixed tonight)

**Needs-bench (confirmed, waiting for hardware iteration):**

- #16 GPIO pin claims never released after file-run scripts / soft reboot
- #17 mphalport cooked-stdout stub wins the link over the real impl
  - we should check the mp docs and what I'm doing to make sure, I think this is intentional but make sure what the tradeoffs are
- #19 USB audio mic streams silence (PCM ring consumer never reinstated)
- #23 PWM slice sharing (GPIO pairs clobber each other's frequency)
- #28 `usb_cdc.ignore_dtr=1` unread-CDC core-0 spin
  - yes fix this
- #38 OLED reinit vs core-1 wavegen cross-core bus collision
  - this may be inevitable, wavegen kinda takes up the whole i2c bus, maybe add some mitigations
- #43 `new (std::nothrow)` reportedly aborts on OOM (verifier disassembly
says the fallbacks are dead code — needs a deliberate allocator decision)
  - remove dead code wherever possible
- #45 stale REQ_BYPASS/REQ_SEND momentarily disconnects the crossbar
  - oh yes fix this, this can interfere with the user's circuit 
- OLED pin-change **teardown ordering** (the refusal half landed; ending the
right Wire while shared with DAC/INAs is the bench half)
  - the oled on the i2c0 bus shouldn't really need to change pins at runtime
- Double-tap `longEnough` measures elapsed-since-arm, not contact (#32
residual) — probe-timing sensitive
- JeoPixel FIFO-stall wedge (SM stops consuming → canShow false forever) —
hardware-fault case, needs a deadline decision

**Design-shaped (documented, for the cycle not a night):**

- Parts tap acceptance should key on a fresh row *transition*, not a timed
deadtime (fixcheck #2: 400 ms loses to the 500 ms re-emission period —
the parked probe still self-accepts, just later; and fast sub-400 ms taps
are swallowed). Owns the `justReadProbe` contract.
  - we need varying colors for the different pins, even if they're undefined, just default to a rainbow
  - 
- Placement-created power conflicts (#9), display-part identity across slot
loads (#6), per-pin vs per-instance bus claims (#7/#3), `inModalContext`
over-breadth during MicroPython scripts (#8 residual).
  - fix this, maybe ask me with options
- The encoder feel guards are **deliberate old-code behavior** — documented
with numbers in the latency audit (35 ms first-detent confirm re-arming
below 6.7 detents/s; 100 ms rotation deafness after any press; 200 ms
recoil disbelief after fast spins). Changing them is a Kevin decision.
  - that's necessary to stop ghost clicks
- The three unification proposals (input events, output invalidation,
state/ownership) in `LATENCY_AUDIT_2026-08-26.md` — the V6 backbone.
- Behavior-changing hints on old surfaces, worth a deliberate pass: `f` has
no receipt, empty-history double-tap is silent, undo/filesystem share the
same yellow, refusals play the accept animation (Probing.cpp:3640).

**Small residuals surfaced by tonight's own fixes:**

- `populateSpecialFunctions` runs before the new net-full refusal
(NetManager.cpp:929) — refused GPIO/UART nodes still publish `gpioNet[]`.
- `addBridgeToNet`'s refusal print is still unconditional (its sibling got
debugNM-gated).
- Stale `gpioState==6` marks are never cleared on two paths (oled.cpp  
hardwired-pin case; initI2C's own marks) — a pin can stay excluded from  
`setGPIO`.
- `updateConfigValue` returns void — refusals can't be signaled to the
config UI.
- Apps.cpp `runPythonScriptFromPath` (the `/` door) lacks the same RX drain
the `z` door got.
- `core1req::post` is not `__not_in_flash_func` (take/complete are).



## State of the bench (morning of 2026-08-26)

- **The board runs tonight's dev build** (flashed over SWD, verify OK).
The version string still reads 5.7.6 — nothing bumped it; 5.7.7.0
numbering is Kevin's call.
- The release 5.7.6 image is recoverable: `git show cc4d80d~1:.pio/build/jumperless_v5/firmware.uf2`
(or reflash after `git stash` + rebuild at `486eb28`).
- Context: slot 3, main terminal. HIL post-fix run result: see the
addendum line at the bottom of this file.
- **Uncommitted, deliberately:** `lib/Jadafruit_NeoPixel/`* (the DMA-latch
rewrite from the previous bench session + tonight's bounded rollover
clamp). It is hardware-visual — Kevin's eyes verify it, then it should be
committed as its own change.
  - the LEDs look good



## Needs human eyes (serial can't see these)

1. LED strip integrity with the JeoPixel latch rewrite: fast animations, no
  one-pixel shift flicker, no garbling after long idle.
  1. this looks good, I haven't seen any garbling
2. Audit bench gates 1 and 3 (parts label blooms; probe part-pin → OLED
  `<part> <pin>`, GND still says GND).
3. Menu feel: navigation LED response should now lead the OLED, held-button
  behavior unchanged, menu exits clean on all three paths.
4. Snake: user overlays now stay visible during the game and survive it.
  1. it does not survive after we leave the overlay test menu, this is the correct behavior



## Addendum — post-fix HIL verdict (03:20)

`HIL suite: FAIL (7/11 passed, 1 SKIPPED)` — but the delta vs the 5.7.6
baseline is unambiguously positive, and every remaining red is harness- or
bench-side, not firmware:

- **Fixed and bench-verified:** all 7 projects-launcher failures (picker,
provisioning, self-heal, cancel — green); the canary's two NEGATIVE
CONTROL checks (the spurious slot rewrite — green, and re-proven by hand:
slot3.yaml byte-identical through load + 10 s idle); the REPL-wedge fix
demonstrated live (a ^C reached a blocked script mid-`input()` — masked
forever on 5.7.6).
- **Still red, harness-side:** every guide `q`/`n` gesture (the tests send
bare chars with no CR at MicroPython `input()` prompts — readline
correctly waits; the retired GUIDE-machine grammar needs salvage); the
canary *plant* step (writes the marker but never verifies it landed).
- **Still red, bench-side:** eeprom/nand00 guides bail early because no
physical chip is seated tonight ("4 of 4 reads floated").
  - we need to  work on the parts thing, I have a 2N3906 on 17-18-19, adding it in the parts menu doesn't seem to do anything. We should ask the user to tap any 3 pins on a transistor and our part detection should figure it out. And we need some feedback. I also have a 7400 nand on row 10-16, 40-46
- **New finding from the hardened runner's own honesty check:**
`board_state_capture`'s fingerprint covers the `parts:` section but
`board_state_restore`'s S-paste does not reinstate it — slot 3 lost its
parts during the suite (reconstructed from `/projects/nand00/wiring.yaml`,
verified back). Harness work-list, high priority: **restore must
round-trip parts**.
- test_encoder_ui SKIPped (needs the jl_input SWD skill wired up).

**Decision: the dev build stays on the board.** Every suite that passed on
5.7.6 still passes; three real firmware bug clusters are fixed; the
remaining failures reproduce identically or worse on 5.7.6.

(Precision note: the `FAIL test_guide_flow.py` row is a phantom — the file
was deleted with the blocking guide runner and the runner still lists it, so
its "failure" is `No such file or directory`. Every real guide phase lives
inside test_projects.py. True post-fix score: 7 of 10 existing files pass.)
## Day-2 addendum — the deferred round + the measure-mode display hunt (16:30)

**Deferred tasks landed** (per Kevin's annotations in this doc): #28 CDC
fail-fast, #45 stale-bypass cancel + generation-idle waits, the dead-code
sweep (-1058 lines; nothrow fallbacks, orphaned oscilloscope,
Probing::measureMode, colorPicker's unreachable body, hideNets, needsRender),
wavegen/OLED bus-window mitigations + shared-bus pin-change refusal,
mphalport restored (the stub won the link by object order under a blanket
--allow-multiple-definition; UART <p> responses, mid-print Ctrl-C, and
script OLED mirroring return; that linker flag deserves its own audit),
and the design cluster per rulings: #6 name identity (load dedupe +
compaction re-bind), #7/#3 last-writer-wins pin ownership, #8 UI-modal split
(scripts no longer count as modal), #9 closed as no-change (RouteSafety
already refuses the dangerous short).

**The measure-mode display drops were POWER, not software.** Rails at 4V
overdrove the 3.3V panel; random-byte NACKs with clean lines cycled it
lost/alive, worst under measure-mode activity. Chased through five
falsified mechanisms (fabric mailbox, raw crosspoint strobes, INA current
check, ephemeral-route proximity, IRQ preemption) with a taxonomy-
instrumented strike line; the byte-index/line-state/dRaw/dtCur columns
ruled each out in one bench pass. Setting the rails to 3.3V ends it.
Kept from the chase (correct regardless): the fabric-send fence + strike
forgiveness on the chunk path, and the same-net measure guard (no route
rebuild when hopping rows within one net). Added the hints the bug earned:
PartLabels `power_overvolt` warning for display parts on a >3.6V supply,
and the lost message names the rail voltage.

**Verified on the tap path:** measure-mode ephemeral connects post
bypass-only sends (no clean escalation exists in refreshLocalConnections)
with non-clearing LED options - existing connections cannot blink off from
a measure tap at the send level.

**Also in this round:** Kevin's single-site X-macro config system + TUI
(config.h/configManager/Tui), VERSION -> 5.8.0.
