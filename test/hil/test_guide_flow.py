#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guided-placement runtime: the guide end to end, plus the task-7 checks.

Drives the headless entry (`z <project>[ new|load|run=<N>]` on port 1,
MENU_DEBUG) with a 3-step project - note + place sip2 + connect - pushed to
/projects/hilguide/, and asserts the machine-parseable
`GUIDE step=<i>/<n> id=<id> state=<STATE>` lines in order while feeding the
guide's serial keys (n n n q).

WHAT CHANGED WITH RUN FILES (design-launcher §1, task 5). There is no
destination slot any more: every launch opens or creates a run file in
/projects/hilguide/ and leaves it as the PERSISTENT active context, so this
suite's workbench is no longer slot 3 - it is whatever run file the launch
just announced on its `RUNFILE path=... action=new|load` line. Every phase
therefore CAPTURES that path instead of hardcoding one. Fresh phases say
`new`; resume phases say `load` (or nothing) with the DIRECTORY name, since a
wiring path combined with load is refused - the run file's own runSource
decides the variant, so the argument would be a lie.

WHICH FILE, AND WHY IT BARELY MATTERS HERE (W3-T3). The default is now ONE run
file per project, /projects/<dir>/<dir>_run.yaml, silently reused; the wave-2
numbered <dir>_<N>.yaml allocator lives behind JL_PROJECT_RUN_HISTORY. The
mode is PROBED (jl.project_run_mode) rather than assumed, and because every
phase reads its path off the RUNFILE line, only ONE thing genuinely inverts:
phase 5's restart. Numbered: run N+1 is a new file and run N survives.
Single: "start fresh" REWRITES the one file, which is precisely why the
interactive launcher puts a prompt in front of it when the build in there is
unfinished. Both halves are asserted, branched on the probe.

RUN-FILE PROTECTION. Everything this file touches lives in ALL FIVE of its own
fixture directories - /projects/hilguide, /projects/hilvfnr, /projects/hilstv,
/projects/hilrail and /projects/hilleg - which the suite creates and which the
teardown sweeps and removes wholesale (that list and the teardown's must stay
in step; the sweep deletes every .yaml, run files included, and is only legal
because every directory in it is suite-owned). It never launches a SHIPPED
project, so it never writes a run file the user owns. Keep it that way: with
one well-known filename, a `z 555 new` anywhere in here would overwrite the
user's 555 circuit.

The guide runtime now also stamps the step TOTAL into the progress line
(`guideProgress: {source: ..., step: k, of: n}`) - the launcher's mid-flight
gate reads `step < of` from the file without loading it. guide_progress_needle()
below is the one place that shape is spelled out.

Covers:
  1b. exit D: an unopenable source (the project DIRECTORY as the wiring path)
      fails the copy and leaves the previous context untouched, number AND path
  2. fresh run: WAIT/COMMIT per step in order, DONE, EXIT, then the script
     offer resolving to `SCRIPT offer=none` (hilguide ships no main.py)
  3. after the run (REPL): part bridge live, RG_A net name asserted, connect
     bridge live, guideProgress + placed: true + runSource: in the RUN FILE
  4. load-latest of a COMPLETED run does NOT relaunch the guide - the
     completion clamp reports `GUIDE already complete (step 3/3)` and the
     built state is untouched
  5. start-new is the new restart. Numbered mode: NON-DESTRUCTIVE - run N+1
     is a fresh file and run N still exists on disk WITH its guideProgress.
     Single-file mode: the one run file is REWRITTEN from the template and
     exactly one run file is left in the directory
  6. the guided-ness gate on a run with no progress (runSource names a guided
     wiring -> fresh guide), then y-resume: quit mid-guide, `load`, resume at
     the saved step with the committed bridges intact ("rails at 0V")
  7. one `p` un-commit: bridge + RG_A name removed, guideProgress decremented
  8. power fixture: power_on commit applies topRail 2.5V (default rail_sane
     check passes as norows), resume past it RE-applies power and the exit
     tail stays silent about 0V
 8b. the mirror of 8, and the H3 item-1 needle: a SKIPPED power_on must stay
     skipped across a resume. The run file carries the skip SET (`skipped:
     0x4`), the resumed INIT says "power_on was skipped - rails stay at 0V"
     instead of energizing them under the 0V banner, the DONE summary reads
     committed=2 skipped=1 (not committed=3), and the exit tail restores the
     user's own rails because nothing was ever energized
  9. no-power fixture: the powerApplied-asymmetry witness - resume past a
     power_on in a project with NO power: section keeps the "rails at 0V"
     exit note (and never claims a re-apply)
 10. voltage check: REPL drives DAC1 to 2.5V mid-guide (mpRemote is in the
     inner set), verify on the DAC1-routed row passes with the measured
     value; a band-miss fails on_fail: warn and 'n' advances past it;
     presence on a bare row (58 - ASSUMES nothing is plugged there) fails
     with val=(nocharge|decay)@58 - the charge-retention check's two
     bare-row verdicts (drained before the tap / caught mid-drain)
 11. continuity refusal: a place step whose row is already routed reports
     `val=skip` + "check skipped (rows in use)" and warn-continues
 12. vf on the starving fabric (/projects/hilvfnr): the crowded 555-shaped
     build that made the bench - and this fixture - refuse with `noroute`.
     Option 1 landed in task 8, so it now MEASURES: `val=2.9xV`. That flip is
     the measurement wave's marquee regression and `VFNR_EXPECT_REFUSAL` is
     the one line holding it.
 12b. the diagnostic seam the flip uncovered (/projects/hilstv): every ADC
     user-claimed, so the taps starve on the POOL instead of on routing - the
     scan-core latch still reaches core 0, now reading `node=-1 rc=0`, which
     is invest-vf-noroute.md §6's other discriminator.
 12c. the ground-side topology, measured: old chain ~2.3 mA with no part, new
     chain < 0.3 mA, and a bridged positive control drawing real current so
     "no current" cannot mean "the leg never routed". Plus INA0-vs-INA1.
 12d. four-wire R against a known crossbar 'part': the five-leg chain by hand,
     both sense channels reading, R stable over 10 repeats - and 2-wire
     reporting several times more for the same part, which is the whole
     reason the rework exists. (The guide-level version is not constructible
     without real parts - see the task-8 report §1.7.)
 12e. the shunt register's quantization floor via `z shunt`.
 12f. the band table off-bench via `z band` - §2.5's numbers plus the reject
     table, with no hardware in the path.
 12g. rail_sane's three verdicts (/projects/hilrail): a row on a 5.00V-set
     rail PASSES on the measured rail (the bench failure, replayed), a row on
     GND fails, an unconnected row fails.
 12h. the ohm flip's two legacy-mA guards (/projects/hilleg): a band that does
     not bracket `value:`, and a value-less band under 5 ohm, both warn and
     fall back to the derived band - while a real ohm band is honoured.
 12i. oscillates on a 5V project (/projects/hilrail, a real 4 Hz slow PWM on
     the target row): the GPIO edge-counting route is refused because the
     rails exceed the pin's 3.3V domain, and the high-Z tap fallback that
     catches it must NOT report an authored band as verified - banded reports
     SKIPPED val=unmeasured and advances on ONE confirm, unbanded still
     PASSES val=osc.
 13. move + snap (task 6): `m <row>` and `c` - the headless twins of the probe
     pads - refused BY NAME off a place step, an off-board move refused with
     its reason, then a move to row 44 (placement=custom, in the machine line
     AND in the run file) and a compact snap whose compact-eligible leg loses
     its bridge while the GND leg keeps one. `c` again round-trips to
     expanded, and expanded being the DEFAULT means the run file ends with no
     `placement:` line at all.
 13b. the PLACED move (task 6 review): every move above happens with the part
     un-placed, so guideMovePart's remove -> mutate -> reapply branch - the
     invariant this whole layer rests on - had zero coverage. Commit the part
     FIRST, then move and snap it while it is live on the fabric, and assert
     the old rows' bridges are gone, the new rows' are live, and the run file
     agrees. It ends with the W3-T2 needle: TWO CONSECUTIVE `m <row>` moves,
     then the `_GUIDE_FP_` overlay read back at the DONE view (where it is the
     only overlay registered) - exactly ten cells lit, at the two columns the
     part now occupies and nowhere else, so the footprint overlay is proven to
     be rebuilt whole rather than accumulated. The run file must already carry
     the SECOND move, because moves persist as they happen now that the wheel
     slide's batching is gone.
 13c. TWO parts, the F1 regression (final whole-branch review): every other
     fixture here has one part, so the inter-part collision rule never fired
     and nobody noticed it refused Kevin's photo gesture. Commit a dip8, then
     snap a resistor whose `connect:` names one of the chip's pin rows - the
     snap must SUCCEED with the leg IN that row and its bridge suppressed,
     while a share nobody declared (moving the footprint onto another of the
     chip's rows) is still refused.
 14. browse + wrap (task 7): `>` past the last step lands in the DONE view
     (`GUIDE done committed= skipped= unfinished=`), `>` again WRAPS to step 1
     with NO EXIT emitted anywhere in between - the wheel and its serial twins
     can never leave the guide - and `q` at DONE is what exits.
 15. skip-and-return: `s` at step 2, commit the rest, DONE reports skipped=1,
     confirm JUMPS to the skipped step instead of exiting, commit it, DONE is
     clean and the next confirm exits.
 16. persisted first-unfinished + the post-commit wrap: commit step 1, browse
     to step 3, commit it OUT OF ORDER - the cursor wraps to step 2, the only
     unfinished step - and what the run file persists is step 1 (the first
     unfinished), NOT the cursor. Relaunch resumes at step 2. Under the old
     rule this file would have said step 3 and step 2 would never be built.
 17. all-skipped is not built: skip every step, reach DONE with
     `committed=0 skipped=3 unfinished=0`, quit - and the companion script is
     NOT offered ("nothing was built"), with no SCRIPT lines at all, while the
     run file is still saved and announced.
 18. rail restore (task 7 ruling 1): rails and DAC1 set to known values, a
     guided launch quit BEFORE power_on, and both halves asserted - the exit
     tail names the values, and dac_get reads them back off the PINS (DAC1 is
     the independent witness that the restore physically ran: INIT drove it to
     0 and nothing else touches it). The run file still holds 0 V, because the
     restore is save=0 on purpose.

BENCH-ONLY, NOT COVERED HERE. This suite never turns the wheel and cannot
touch a hole, so the physical input surface has no automated witness: the
wheel click, the wheel turn, and every probe-pad gesture. What IS covered is
everything those gestures funnel into - `>`/`<` are the wheel's browse twins
and `m`/`c` the pads' absolute twins, all running the same handlers.

(W3-T2 pruned what used to stand here: the 260 ms confirm pend, the
double-click into STEP_ADJUST, the wheel slide inside it, and the pend race
the final fix wave closed. All four are gone from the firmware - Kevin's
"this whole thing is confusing, we need to streamline this interface" ruling
- so they are not untested, they do not exist. A wheel click now confirms on
its own RELEASED edge, which is why no pend can mature in a state the user
already left.)

Also bench-only, and NOT fakeable from here: the LED drag trail itself. The
`_GUIDE_FP_` overlay is readable through overlay_serialize() and phase 13b
asserts it, but the trail Kevin saw was in the `leds` buffer underneath -
overlay pixels that stopped being lit were never repainted black because
nothing posted a LED_CLEAR. No REPL surface reads that buffer back, so the
clear-first fix is verified by eye on the bench (checklist SS1.4).

Bench convention: snapshot board state + the active CONTEXT up front, restore
both in a finally (a prior round's incident: an uncaught exception stranded
slot 3 - the try/finally here is that lesson). The slot-3 fixture is gone with
the destination slots; nothing in this file writes a numbered slot any more.
TEARDOWN ORDER IS LOAD-BEARING: leave the run-file context FIRST, then delete
the run files, or the switch's dirty pre-save re-creates what was just removed
(test_parts_roundtrip learned that one). BOTH /projects/hilguide/ and
/projects/hilvfnr/ are removed afterwards, run files included:
test_projects.py's run_app('Guides') probe gates itself on the board holding
exactly the two projects IT authored, and either leftover would silently skip
that phase.

Port discipline: the guide loop reads its keys straight off port 1, so this
test holds ONE port-1 connection per drive and sends keys only after the
status line that proves the guide is waiting for them. If the board is
absent, skip cleanly; if port 1 is held by a client, fail fast (project rule).
"""

import glob
import re
import time

# Auto-skip BEFORE jl.py helpers run (they sys.exit(FAIL) when the board is
# missing; an absent board should be a clean SKIP for this suite).
# Exit 77, not 0: jl.SKIP_EXIT, so run_all prints SKIP rather than PASS for a
# file that asserted nothing. Hardcoded because jl is deliberately not imported
# until after these gates (importing it is what sys.exits on a missing board).
if not glob.glob("/dev/cu.*JLV5port1"):
    print("SKIP: board unavailable - no /dev/cu.*JLV5port1 device present")
    raise SystemExit(77)
if not glob.glob("/dev/cu.*JLV5port5"):
    print("SKIP: board unavailable - no /dev/cu.*JLV5port5 device present "
          "(port 1 exists; is the board half-enumerated?)")
    raise SystemExit(77)

import serial  # pyserial

from jl import (jl_exec, parse_kv, port1_command, port1_path, check, finish,
                board_state_capture, board_state_restore,
                active_context, restore_context, fault_scan,
                project_run_mode, project_run_path)

PROJ = "hilguide"
PROJ_DIR = "/projects/" + PROJ
WIRING_PATH = PROJ_DIR + "/wiring.yaml"

# 3 steps: note + place (sip2 resistor at rows 45/46, pin A bridges to 50)
# + connect 52-53. Rows chosen away from the 555 kit's 5-26 so a bench build
# doesn't visually collide; everything is restored regardless. The place step
# carries `check: none` EXPLICITLY: this fixture tests the flow machine, and
# the class default (continuity) would now really run - and really fail - on
# a phantom part with nothing in the holes. The check phases below have their
# own fixtures.
#
# RG's TWO PIN CLASSES ARE THE POINT (phase 13). Pin A's endpoint is row 50 -
# a physical hole - so compact placement puts the leg IN row 50 and emits no
# bridge. Pin B's endpoint is GND, which lives in the crossbar and has no
# holes at all, so that leg keeps its footprint row AND its bridge in either
# mode. One part therefore witnesses both halves of the per-pin compact rule;
# a part wired only to holes would never prove the fallback exists.
WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL Guide"
  summary: "3-step guided-flow HIL fixture"
parts:
  - name: "RG"
    type: resistor
    value: "1k"
    footprint: sip2
    row: 45
    pins: {A: {pin: 1, connect: 50}, B: {pin: 2, connect: GND, class: gnd}}
guide:
  title: "HIL Guide"
  steps:
    - {do: note, text: "hil guide intro"}
    - {do: place, part: RG, check: none, text: "place RG at rows 45-46"}
    - {do: connect, n1: 52, n2: 53, text: "bridge 52 to 53"}
"""

# Power fixture (phase 8): witnesses power_on commit + resume-past-power_on
# re-apply, and the default rail_sane check's honest "norows" pass.
POWER_PATH = PROJ_DIR + "/power.yaml"
POWER_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL Power"
power:
  topRail: 2.5
guide:
  title: "HIL Power"
  steps:
    - {do: note, text: "power fixture"}
    - {do: connect, n1: 52, n2: 53, text: "bridge 52-53"}
    - {do: power_on, text: "power up"}
    - {do: note, text: "after power"}
"""

# No-power fixture (phase 9): the powerApplied-asymmetry witness. Same steps,
# no power: section - resume past the committed power_on must NOT claim a
# re-apply, and the exit tail must keep its "rails at 0V" note.
NOPOWER_PATH = PROJ_DIR + "/nopower.yaml"
NOPOWER_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL NoPower"
guide:
  title: "HIL NoPower"
  steps:
    - {do: note, text: "nopower fixture"}
    - {do: connect, n1: 52, n2: 53, text: "bridge 52-53"}
    - {do: power_on, text: "power up (nothing to apply)"}
    - {do: note, text: "after power"}
"""

# Checks fixture (phase 10): row 45 is wired to DAC1 in the loaded bridges;
# the REPL drives DAC1 to 2.5V mid-guide (hardware-only write - the guide's
# INIT zeroed the state) and the voltage check taps the row. Row 58 is the
# presence-check floating row - the ONE bench assumption in this file.
CHECKS_PATH = PROJ_DIR + "/checks.yaml"
CHECKS_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL Checks"
bridges:
  - {n1: 45, n2: DAC1}
guide:
  title: "HIL Checks"
  steps:
    - {do: note, text: "checks fixture"}
    - {do: verify, target: 45, check: voltage, min: 2.2, max: 2.8, timeout_ms: 4000, text: "row 45 near 2.5V"}
    - {do: verify, target: 45, check: voltage, min: 4.5, max: 4.9, timeout_ms: 4000, text: "band miss"}
    - {do: verify, target: 58, check: presence, timeout_ms: 4000, text: "floating row"}
"""

# Refusal fixture (phase 11): the place step's row 45 is pre-routed by the
# wiring's own bridge, so the continuity stimulus must refuse to run.
REFUSAL_PATH = PROJ_DIR + "/refusal.yaml"
REFUSAL_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL Refusal"
parts:
  - name: "RG"
    type: resistor
    value: "1k"
    footprint: sip2
    row: 45
    pins: {A: {pin: 1}, B: {pin: 2}}
bridges:
  - {n1: 45, n2: 50}
guide:
  title: "HIL Refusal"
  steps:
    - {do: place, part: RG, check: continuity, timeout_ms: 4000, text: "rows in use refusal"}
"""

# TWO-part compact fixture (phase 13c) - the F1 regression, and the only
# fixture in this file with more than one part. Everything else here places a
# single part, which is exactly why nine reviews and every green suite missed
# that the inter-part collision rule refused Kevin's photo gesture: a compact
# leg lands ON its `connect:` row, and in any real project that row holds the
# leg of the chip it is wired to.
#
# Geometry (binding, wave 2): UC is a dip8 at row 35, so pin 1 -> node 35 and
# pin 3 -> node 37. RC is a sip2 at row 45 (legs 45/46) whose pin A DECLARES
# `connect: 37` - the chip's OUT-side leg row - and whose pin B goes to the
# rail. Snap RC compact and leg A must land IN row 37, sharing the row with
# UC's own leg. That share is authored; refusing it is the bug.
COMPACT_PATH = PROJ_DIR + "/compact.yaml"
COMPACT_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL Compact"
parts:
  - name: "UC"
    type: ic
    footprint: dip8
    row: 35
    pins:
      P1: {pin: 1, connect: GND, class: gnd}
      P3: {pin: 3, connect: 50, class: signal}
  - name: "RC"
    type: resistor
    value: "10k"
    footprint: sip2
    row: 45
    pins: {A: {pin: 1, connect: 37}, B: {pin: 2, connect: TOP_RAIL, class: power}}
guide:
  title: "HIL Compact"
  steps:
    - {do: place, part: UC, check: none, text: "place the chip first"}
    - {do: place, part: RC, check: none, text: "then snap the resistor onto its pin row"}
"""

# Continuation-line fixture (phase 11b, task 9 §A item 11). A `guide:` step
# whose flow map wraps onto a second line used to SWALLOW the step after it:
# the wrapped tail fell through guideParse's key:value arm, which cleared
# inSteps, and the next `- ` item was then dropped in silence - two authored
# steps parsed as ONE. Task 8 found it while writing fixtures (its report
# §7.3 measured it three ways). Both halves of the fix are witnessed here:
#   - step 1 WRAPS, and both its text and step 2 must survive the join;
#   - step 3 carries an APOSTROPHE outside any double-quoted run (a `script:`
#     path; `part: O'Brien` is the same shape). The first cut of the detector
#     honoured `'` as a string delimiter - which nothing else in this parser
#     does - so that balanced line reported UNCLOSED, the join ate step 4, and
#     a file that parsed clean before the join existed silently lost a step.
#     Step 4 existing is the assertion;
#   - the trailing `stray:` line is the residual BLOCK-style shape, which the
#     parser still cannot support and must now warn about instead of eating
#     the next step in silence.
# `do: run` parses and counts but never executes, so this fixture still
# touches no hardware.
CONT_PATH = PROJ_DIR + "/contline.yaml"
CONT_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilguide
  title: "HIL Continuation"
guide:
  title: "HIL Continuation"
  steps:
    - {do: note,
       text: "step one wrapped onto two lines"}
    - {do: note, text: "step two must survive"}
    - {do: run, script: /python_scripts/o'brien.py, text: "an apostrophe OUTSIDE double quotes"}
    - {do: note, text: "step four survives the apostrophe"}
    stray: notastep
"""

# vf tap-starvation fixture (phase 12) - lives in its OWN project dir because
# it must be a self-contained 555-shaped build. Verbatim from
# .superpowers/sdd/projects-wave-2-bench-notes/invest-vf-noroute.md §9: it
# rebuilds the exact committed fabric of Kevin's bench session (10 connect
# steps + two static ADC bridges applied at load) with rows 22/23 left clean,
# then runs the identical vf check that failed there 3/3.
VFNR_PROJ = "hilvfnr"
VFNR_DIR = "/projects/" + VFNR_PROJ
VFNR_PATH = VFNR_DIR + "/wiring.yaml"
VFNR_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilvfnr
  title: "HIL VF NoRoute"
parts:
  - name: "LEDX"
    type: led
    footprint: sip2
    row: 22
    pins: {A: {pin: 1}, K: {pin: 2, connect: GND}}
bridges:
  - {n1: ADC0, n2: 7}
  - {n1: ADC1, n2: 37}
guide:
  title: "HIL VF NoRoute"
  steps:
    - {do: connect, n1: 5,  n2: GND,      text: "u1 gnd"}
    - {do: connect, n1: 6,  n2: 37,       text: "u1 trig"}
    - {do: connect, n1: 8,  n2: TOP_RAIL, text: "u1 reset"}
    - {do: connect, n1: 35, n2: TOP_RAIL, text: "u1 vcc"}
    - {do: connect, n1: 12, n2: TOP_RAIL, text: "r1 a"}
    - {do: connect, n1: 13, n2: 36,       text: "r1 b"}
    - {do: connect, n1: 15, n2: 36,       text: "r2 a"}
    - {do: connect, n1: 16, n2: 37,       text: "r2 b"}
    - {do: connect, n1: 18, n2: 37,       text: "c1 +"}
    - {do: connect, n1: 19, n2: GND,      text: "c1 -"}
    - {do: place, part: LEDX, check: vf, min: 1.4, max: 2.6,
       on_fail: warn, timeout_ms: 4000, text: "vf tap under 555 state"}
"""
VFNR_CONNECT_IDS = ["connect_5", "connect_6", "connect_8", "connect_35",
                    "connect_12", "connect_13", "connect_15", "connect_16",
                    "connect_18", "connect_19"]

# ---------------------------------------------------------------------------
# Task-8 fixtures: the measurement rework
# ---------------------------------------------------------------------------

# Phase 12b - the diagnostic seam, starved by ADC exhaustion instead of route
# starvation. Same crowded 555-shaped fabric as hilvfnr, but ALL FOUR pool
# channels carry a user bridge, so infraAdcUserClaimed excludes every one of
# them: Option 1 cannot acquire its two sense channels, the tap fallback
# cannot acquire one, and the refusal is structural. Rows 41/42 for ADC2/ADC3
# sit on chip F, away from the rows under test (22/23 are chip D), so the only
# thing they change is the ADC ledger.
STARVE_PROJ = "hilstv"
STARVE_DIR = "/projects/" + STARVE_PROJ
STARVE_PATH = STARVE_DIR + "/wiring.yaml"
STARVE_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilstv
  title: "HIL ADC Starve"
parts:
  - name: "LEDY"
    type: led
    footprint: sip2
    row: 22
    pins: {A: {pin: 1}, K: {pin: 2, connect: GND}}
bridges:
  - {n1: ADC0, n2: 7}
  - {n1: ADC1, n2: 37}
  - {n1: ADC2, n2: 41}
  - {n1: ADC3, n2: 42}
guide:
  title: "HIL ADC Starve"
  steps:
    - {do: place, part: LEDY, check: vf, min: 1.4, max: 2.6, on_fail: warn, timeout_ms: 6000, text: "vf with every ADC user-claimed"}
"""

# Phase 12g - rail_sane's three verdicts. Each fixture places ONE part whose
# pin classes are what collectClassRows() collects, then powers up: the part
# must be COMMITTED first, because rail_sane reads placed parts' pins.
# topRail 5.0 on purpose - the bench failure was a 5 V rail measuring ~4.78.
RAIL_DIR = "/projects/hilrail"
RAIL_OK_PATH = RAIL_DIR + "/ok.yaml"
RAIL_GND_PATH = RAIL_DIR + "/gnd.yaml"
RAIL_FLOAT_PATH = RAIL_DIR + "/float.yaml"


def _rail_wiring(title, a_pin):
    return """version: 2
sourceOfTruth: bridges
meta:
  project: hilrail
  title: "%s"
parts:
  - name: "PWR"
    type: resistor
    value: "1k"
    footprint: sip2
    row: 8
    pins: {A: {pin: 1, %sclass: power}, B: {pin: 2, connect: GND, class: gnd}}
power:
  topRail: 5.0
guide:
  title: "%s"
  steps:
    - {do: place, part: PWR, check: none, text: "the power-class part"}
    - {do: power_on, check: rail_sane, timeout_ms: 6000, text: "power up 5V"}
""" % (title, a_pin, title)


RAIL_OK_WIRING = _rail_wiring("HIL Rail OK", "connect: TOP_RAIL, ")
RAIL_GND_WIRING = _rail_wiring("HIL Rail GND", "connect: GND, ")
RAIL_FLOAT_WIRING = _rail_wiring("HIL Rail Float", "")

# Phase 12i - oscillates on a 5 V project: the refusal must not waive the band.
# These live in RAIL_DIR on purpose: it is already created above, already swept
# by the teardown (which deletes every .yaml named in the list at the bottom),
# and it already ships `power: topRail: 5.0` - which IS the supply ceiling the
# oscillates GPIO gate refuses on. No new fixture directory to keep in step.
#
# Row 30 is bridged to RP_GPIO_1 (node 131) in the wiring itself, so a slow PWM
# started on GPIO 1 before the launch reaches the target the moment the project
# loads. setupPWM routes anything under 10 Hz to setupSlowPWM
# (Peripherals.cpp:3086, range 0.001-10 Hz), so 4 Hz is a genuine square wave -
# the shipped 555's own regime - with nothing on the host racing the guide's
# poll loop.
RAIL_OSC_BAND_PATH = RAIL_DIR + "/oscband.yaml"
RAIL_OSC_NOBAND_PATH = RAIL_DIR + "/oscnoband.yaml"
OSC_TARGET_ROW = 30
OSC_PWM_PIN = 1
OSC_PWM_HZ = 4.0


def _osc_wiring(title, band):
    return """version: 2
sourceOfTruth: bridges
meta:
  project: hilrail
  title: "%s"
bridges:
  - {n1: %d, n2: RP_GPIO_%d}
power:
  topRail: 5.0
guide:
  title: "%s"
  steps:
    - {do: power_on, check: rail_sane, timeout_ms: 6000, text: "power up 5V"}
    - {do: verify, target: %d, check: oscillates, %stimeout_ms: 4000, on_fail: warn, text: "osc on a 5V-railed project"}
""" % (title, OSC_TARGET_ROW, OSC_PWM_PIN, title, OSC_TARGET_ROW, band)


RAIL_OSC_BAND_WIRING = _osc_wiring("HIL Osc Banded", "min: 0.3, max: 30, ")
RAIL_OSC_NOBAND_WIRING = _osc_wiring("HIL Osc Unbanded", "")

# Phase 12h - the ohm flip's two legacy-mA guards, plus the honoured-ohm case.
LEGACY_DIR = "/projects/hilleg"
LEGACY_PATH = LEGACY_DIR + "/wiring.yaml"
LEGACY_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hilleg
  title: "HIL Legacy Bands"
parts:
  - name: "RLEG"
    type: resistor
    value: "10k"
    footprint: sip2
    row: 50
    pins: {A: {pin: 1}, B: {pin: 2}}
  - name: "RBARE"
    type: resistor
    footprint: sip2
    row: 54
    pins: {A: {pin: 1}, B: {pin: 2}}
  - name: "ROHM"
    type: resistor
    value: "100"
    footprint: sip2
    row: 57
    pins: {A: {pin: 1}, B: {pin: 2}}
guide:
  title: "HIL Legacy Bands"
  steps:
    - {do: place, part: RLEG, check: continuity, min: 0.1, max: 0.8, on_fail: warn, timeout_ms: 6000, text: "value parses, band does not bracket it"}
    - {do: place, part: RBARE, check: continuity, min: 0.04, max: 0.3, on_fail: warn, timeout_ms: 6000, text: "no value, max under 5 ohm"}
    - {do: place, part: ROHM, check: continuity, min: 20, max: 900, on_fail: warn, timeout_ms: 6000, text: "a real ohm band"}
"""


_csi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]")


def read_device_file(path):
    """Chunked through jfs.open(), NOT fs_read(): fs_read() silently truncates
    at 4095 bytes (a pre-existing static-buffer cap), which turns any
    comparison against a larger file into a false mismatch. Matched with
    test_projects.py's copy."""
    out = jl_exec(f"""
p = {path!r}
if fs_exists(p):
    print("EXISTS= 1")
    print("<<<FILE>>>")
    f = jfs.open(p, "r")
    while True:
        c = f.read(256)
        if not c:
            break
        print(c, end="")
    f.close()
    print()
    print("<<<END>>>")
else:
    print("EXISTS= 0")
""", timeout=40)
    if "EXISTS= 1" not in out:
        return False, ""
    m = re.search(r"<<<FILE>>>\r?\n(.*)<<<END>>>", out, re.DOTALL)
    return True, (m.group(1) if m else "")


def guide_progress_needle(source, step, total, skipped=0):
    """The exact one-line flow map toYAML emits for guideProgress.

        guideProgress: {source: "<path>", step: <k>, of: <n>, skipped: 0x<m>}

    `of:` - the step TOTAL as of the last persist - arrived with W3-T3 and is
    written by guidePersistProgress, the only code that both knows numSteps
    and saves the file. The single-run-file launcher needs `step < of`
    answerable from the FILE ALONE (it decides whether to prompt BEFORE it is
    allowed to load anything, because the prompt can still be cancelled), and
    numSteps cannot be recomputed launcher-side: guideParse resolves `part:`
    names, and synthesizes auto steps, against the LIVE parts table.

    Asserting the total here is therefore not cosmetic - it is the input to
    the mid-flight gate. step == of is a FINISHED build (silent reopen);
    step < of is MID-FLIGHT (the one prompt). `of:` is emitted only when
    non-zero, so a hand-written fixture with no total round-trips unchanged -
    test_parts_roundtrip's exact-shape needle depends on that.

    `skipped:` (H3 item 1) is the SKIP SET as a hex bitmask, bit i = step i was
    deliberately skipped. It has to be persisted because `step:` above is
    guideFirstUnfinished, which counts a skip as FINISHED - so a skipped step
    always sits strictly BELOW the resume cursor, and the INIT resume loop used
    to promote every index below the cursor to `committed`. For a `power_on`
    step that promotion RE-ENERGIZED THE RAILS on the next launch, two lines
    under the banner that promises they are held at 0 V.

    A live guide session always knows its own skip set, so every runtime
    persist emits this key - INCLUDING `skipped: 0x0`, which is why the
    no-skips call sites still assert it. The key is never written
    speculatively: a hand-written flow map, or one from firmware predating the
    key, carries no `skipped:` at all and reads back as "unknown" rather than
    as a false "nothing was skipped" - and only the unknown case makes resume
    refuse to re-energize a rail. test_parts_roundtrip's exact-shape needle
    (no `of:`, so no `skipped:`) depends on that omission too."""
    return (f'guideProgress: {{source: "{source}", step: {step}, of: {total}, '
            f'skipped: {hex(skipped)}}}')


class GuideDriver:
    """One port-1 connection driving one guide session, with ORDERED
    status-line assertions (each expect searches only past the previous
    match, so out-of-order lines fail loudly)."""

    def __init__(self):
        self.ser = serial.Serial(port1_path(), 115200, timeout=0.05)
        self.buf = ""
        self.pos = 0
        self._scanned = 0
        # Prime the connection: the firmware's connection-init eats the first
        # byte(s) - same idiom as test_projects phase 6(d).
        #
        # THIS IS A RAW PORT-1 READER, so it carries its own fault witness:
        # jl.py's fault_scan only sees the bytes jl.py itself read, and a
        # driver that opens its own connection bypasses it entirely. The
        # connect banner is scanned because the firmware prints a post-fault
        # [crashlog] exactly ONCE, to the first terminal that attaches after
        # the reboot - whoever gets there first is the only one who can see it.
        self.ser.write(b"\r\n")
        self.ser.flush()
        quiet, overall = time.time(), time.time()
        banner = b""
        while time.time() - overall < 4.0:
            b = self.ser.read(4096)
            if b:
                banner += b
                quiet = time.time()
            elif time.time() - quiet > 0.6:
                break
        fault_scan(_csi.sub("", banner.decode(errors="replace")),
                   "the guide driver's connect banner")
        self.ser.reset_input_buffer()

    def send(self, data):
        self.ser.write(data)
        self.ser.flush()

    def pump(self):
        chunk = self.ser.read(4096)
        if chunk:
            self.buf += _csi.sub("", chunk.decode(errors="replace"))
            # Scan only what is new, so one fault is reported once.
            if len(self.buf) > self._scanned:
                fault_scan(self.buf[self._scanned:], "the guide driver")
                self._scanned = len(self.buf)

    def expect(self, pattern, what, timeout=25):
        """Returns the re.Match on success (truthy, so `if d.expect(...)`
        still reads the same) and None on timeout - callers that need a
        captured group take it off the return value."""
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            self.pump()
            m = rx.search(self.buf, self.pos)
            if m:
                self.pos = m.end()
                check(True, what)
                return m
        tail = self.buf[max(0, len(self.buf) - 600):]
        check(False, f"{what} (timed out; tail: {tail!r})")
        return None

    def expect_runfile(self, action, what, timeout=25):
        """Capture the run file the launch just opened. Never hardcode the
        number: N is monotonic across the whole suite and depends on how many
        phases ran before this one."""
        m = self.expect(r"RUNFILE path=(\S+) action=" + action, what, timeout)
        return m.group(1) if m else None

    def close(self):
        try:
            self.pump()
        finally:
            self.ser.close()


# --- 0. Snapshot the bench --------------------------------------------------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

# Shared helper, not a local regex: 'Q' now answers ACTIVE_SLOT:-1 +
# ACTIVE_PATH:<path> when a FILE context is active, and the old
# r"ACTIVE_SLOT:(\d+)" did not match -1 - this phase aborted outright whenever
# the bench happened to be sitting on a project run file.
orig_slot, orig_path = active_context(1.5)
check(orig_slot is not None, "queried active context ('Q')")
check(orig_path is not None, "'Q' reports ACTIVE_PATH")
if orig_slot is None:
    orig_slot = 0

print(f"  info: entry context slot={orig_slot} path={orig_path!r}")

# WHICH RUN-FILE SCHEME IS FLASHED? Probed, not assumed (jl.project_run_mode).
# "single" = ONE /projects/<dir>/<dir>_run.yaml per project, reused; "history"
# = the wave-2 <dir>_<N>.yaml allocator behind JL_PROJECT_RUN_HISTORY. Every
# phase below captures the path off the launch's own RUNFILE line, so almost
# nothing here cares - the exceptions are phase 5's restart semantics, which
# genuinely invert, and they say so.
RUN_MODE = project_run_mode()
print(f"  info: firmware run-file mode: {RUN_MODE}")

# The run-file NAME, per mode - used by the handful of assertions that check
# the spelling rather than just capturing it off the RUNFILE line.
if RUN_MODE == "single":
    RUN_BASENAME_RE = PROJ + r"_run\.yaml"
    RUN_NAME_HUMAN = "<dir>_run.yaml"
else:
    RUN_BASENAME_RE = PROJ + r"_\d+\.yaml"
    RUN_NAME_HUMAN = "<dir>_<N>.yaml"
RUN_NAME_RE = r"^" + re.escape(PROJ_DIR) + r"/" + RUN_BASENAME_RE + r"$"

# guide_live: True while a guide session may be blocking on port-1 keys -
# the finally sends its 'q' ONLY then (a blind 'q' at the main menu would
# launch the DMX app and strand the bench the other way).
guide_live = False

try:
    # --- 1. Push the project ------------------------------------------------
    out = jl_exec(f"""
for d in ("/projects", {PROJ_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({PROJ_DIR!r}) else 0)
print("wrote=", 1 if fs_write({WIRING_PATH!r}, {WIRING!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("projdir") == 1, f"created {PROJ_DIR} on the board")
    check(vals.get("wrote") == 1, f"pushed {WIRING_PATH}")

    # --- 1b. EXIT D witness: the run-file COPY fails ------------------------
    # RE-AIMED for run files. This used to prove loadSlotFromPath's
    # atomic-on-open-failure contract by handing `z` the project DIRECTORY as a
    # wiring path. Under the run-file model the copy runs FIRST, so the same
    # input now exercises exit D instead: copyFileRaw cannot open the source,
    # nothing is created (the destination is only opened after the source
    # succeeds), the retry fails the same way, and the previous context is
    # untouched - number AND path, which is why both are compared. No guide
    # session can start on this path, so no unwedge byte is armed.
    slot_before_bad, path_before_bad = active_context(1.5)
    d = GuideDriver()
    try:
        d.send(f"z {PROJ_DIR}\r\n".encode())
        d.expect(r"PROJECT error cannot read " + re.escape(PROJ_DIR),
                 "EXIT D: an unopenable wiring source fails the run-file copy")
    finally:
        d.close()
    slot_after_bad, path_after_bad = active_context(1.5)
    check(slot_after_bad == slot_before_bad,
          f"EXIT D left ACTIVE_SLOT unchanged "
          f"({slot_before_bad} -> {slot_after_bad})")
    check(path_after_bad == path_before_bad,
          f"EXIT D left ACTIVE_PATH unchanged "
          f"({path_before_bad!r} -> {path_after_bad!r})")

    # --- 2. Fresh guided run: z ... new -> n n n q -------------------------
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        run1 = d.expect_runfile("new", "start-new allocated a run file")
        check(run1 is not None and re.match(RUN_NAME_RE, run1 or ""),
              f"the run file is named {RUN_NAME_HUMAN} (got {run1!r})")
        d.expect(r"GUIDE step=1/3 id=note_1 state=INIT", "INIT status line")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "step 1 (note) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=1/3 id=note_1 state=RESULT check=none val=pass",
                 "step 1 RESULT carries check=none val=pass")
        d.expect(r"GUIDE step=1/3 id=note_1 state=COMMIT", "step 1 COMMIT")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "step 2 (place RG) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=RESULT check=none val=pass",
                 "step 2 RESULT (check: none - the flow fixture opts out of checks)")
        d.expect(r"GUIDE step=2/3 id=place_RG state=COMMIT", "step 2 COMMIT")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "step 3 (connect) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=COMMIT", "step 3 COMMIT")
        d.expect(r"GUIDE .* state=DONE", "DONE after the last commit")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "EXIT on q at the DONE ack")
        guide_live = False
        # EXIT H: a completed guide falls through to the script step. hilguide
        # ships no main.py, so the offer resolves to `none` and is skipped -
        # the same two machine lines a real offer prints, which is what makes
        # the grammar greppable on every path.
        d.expect(r"SCRIPT offer=none", "completed guide reaches the script step")
        d.expect(r"SCRIPT action=skip", "no companion script -> action=skip")
        d.expect(r"Run saved to " + RUN_BASENAME_RE + r" \(now your active circuit\)",
                 "the launcher's closing one-liner names the run file")
    finally:
        d.close()
    time.sleep(1.0)   # let the final save settle

    ctx_slot, ctx_path = active_context(1.5)
    check(ctx_slot == -1 and ctx_path == run1,
          f"the run file is the ACTIVE CONTEXT after the launcher returned "
          f"(got {ctx_slot}, {ctx_path!r}, expected -1, {run1!r})")

    # --- 3. State after the run (REPL + the RUN FILE) -----------------------
    out = jl_exec("""
print("rg=", 1 if is_connected(45, 50) else 0)
print("conn=", 1 if is_connected(52, 53) else 0)
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("rg") == 1, "place commit: RG pin A bridge 45-50 is live")
    check(vals.get("conn") == 1, "connect commit: bridge 52-53 is live")

    # Net name: the net holding node 45 must carry the parts-layer auto name
    # RG_A (asserted at commit + re-asserted by partsReassertNetNames).
    # get_net_nodes returns a comma-separated STRING - tokenize exactly like
    # test_projects.py does (a substring test would match 45 inside 145).
    out = jl_exec("""
name = "none"
for i in range(1, 40):
    nodes = str(get_net_nodes(i))
    if not nodes:
        continue
    toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
    if "45" in toks:
        name = str(get_net_name(i))
        break
print("NETNAME|" + name)
""", timeout=25)
    m = re.search(r"NETNAME\|(.+?)\s*$", out, re.MULTILINE)
    net_name = m.group(1).strip() if m else "none"
    check(net_name == "RG_A",
          f"net holding node 45 is named RG_A (got {net_name!r})")

    run1_exists, run1_yaml = read_device_file(run1)
    check(run1_exists, f"the run file {run1} exists on the board")
    check(guide_progress_needle(WIRING_PATH, 3, 3) in run1_yaml,
          "the RUN FILE round-trips guideProgress at step 3 OF 3, bound to the "
          "CANONICAL wiring (never to itself) - step == of, i.e. FINISHED, "
          "which is what makes the launcher reopen this file silently")
    check(f'runSource: "{WIRING_PATH}"' in run1_yaml,
          "the run file carries runSource: - the variant the path cannot encode")
    check("placed: true" in run1_yaml, "the run file has RG placed: true")
    check("{n1: 45, n2: 50}" in run1_yaml.replace("  ", " ") or
          re.search(r"n1:\s*45,\s*n2:\s*50", run1_yaml) is not None,
          "the run file carries the 45-50 expansion bridge")
    check(re.search(r"n1:\s*52,\s*n2:\s*53", run1_yaml) is not None,
          "the run file carries the 52-53 connect bridge")
    # First-save normalization (design §1.2): the run file started as a
    # verbatim wiring copy carrying meta:/guide:, and the first rewrite drops
    # both (they are swallowed on parse and never re-emitted). That is exactly
    # WHY the guide must be parsed from the canonical wiring, so it is asserted
    # rather than assumed.
    check("guide:" not in run1_yaml and "meta:" not in run1_yaml,
          "the run file lost meta:/guide: on its first save (as designed)")

    # --- 4. Load-latest of a COMPLETED run does not relaunch the guide ------
    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"RUNS n=\d+ latest=" + re.escape(run1), "RUNS names the latest run")
        loaded = d.expect_runfile("load", "load-latest re-opened the run file")
        check(loaded == run1, f"load-latest opened {run1} (got {loaded!r})")
        d.expect(r"GUIDE resume file=" + re.escape(run1) + r" step=3",
                 "resume line names the FILE and the step (never a slot)")
        d.expect(r"GUIDE already complete \(step 3/3\)",
                 "the completion clamp refuses to relaunch a finished build")
        d.expect(r"SCRIPT offer=none", "a finished build falls through to the script step")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    out = jl_exec("print('rg=', 1 if is_connected(45, 50) else 0)", timeout=25)
    check(parse_kv(out).get("rg") == 1,
          "re-opening the completed run left the built state untouched")

    # --- 5. Start-new IS the restart --------------------------------------
    # The old destructive restart (un-place loop + guideSource wipe + re-save)
    # is deleted either way. What "start new" costs DEPENDS ON THE MODE, and
    # that is the whole point of W3-T3, so both halves are asserted:
    #
    #   numbered (JL_PROJECT_RUN_HISTORY): run N+1 is a NEW file and run N
    #     survives on disk WITH its guideProgress - strictly better than the
    #     destructive restart it replaced;
    #   single-file (default): there is one name, so "start fresh" REWRITES
    #     it from the template. The old progress is gone BY DESIGN - which is
    #     exactly why this is the one path the launcher puts a prompt in front
    #     of when the build in the file is unfinished. Here it is reached
    #     through an EXPLICIT `new`, and headless never prompts.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        run2 = d.expect_runfile("new", "start-new wrote a run file")
        if RUN_MODE == "single":
            check(run2 == run1,
                  f"SINGLE FILE: start-fresh reuses the one name "
                  f"({run1!r} -> {run2!r})")
        else:
            check(run2 is not None and run2 != run1,
                  f"start-new is a different file from the previous run "
                  f"({run1!r} -> {run2!r})")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT",
                 "the new run enters at step 1 with no progress", timeout=40)
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "q quits the new run at step 1")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    out = jl_exec("""
print("rg=", 1 if is_connected(45, 50) else 0)
print("conn=", 1 if is_connected(52, 53) else 0)
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("rg") == 0, "the new run has no RG placement bridge 45-50")
    check(vals.get("conn") == 0, "the new run has no connect bridge 52-53")

    if RUN_MODE == "single":
        out = jl_exec(f"""
names = [n for n in jfs.listdir({PROJ_DIR!r})
         if n.startswith({PROJ + "_"!r}) and n.endswith(".yaml")]
print("nrun=", len(names))
print("RUNS|" + ",".join(sorted(names)))
""", timeout=25)
        check(parse_kv(out).get("nrun") == 1,
              f"SINGLE FILE: start-fresh left exactly ONE run file in "
              f"{PROJ_DIR} - it overwrote, it did not accumulate")
        print("  info: SKIPPED (JL_PROJECT_RUN_HISTORY only) - the "
              "non-destructive-restart assertions (run N survives run N+1). "
              "On a single-file build the overwrite IS the semantics; the "
              "run_2_yaml checks below are the same assertions read the other "
              "way round.")
    else:
        prev_exists, prev_yaml = read_device_file(run1)
        check(prev_exists, f"NON-DESTRUCTIVE RESTART: {run1} still exists on disk")
        check(guide_progress_needle(WIRING_PATH, 3, 3) in prev_yaml,
              "NON-DESTRUCTIVE RESTART: the previous run still holds its "
              "guideProgress - starting a new run did not clear it")

    _, run2_yaml = read_device_file(run2)
    check("guideProgress:" not in run2_yaml,
          "the new run has no guideProgress (quit at step 1, nothing committed)")
    check("placed: true" not in run2_yaml, "no part is placed: true in the new run")

    # =====================================================================
    # Task 7 extensions. Coverage debts first (task 6 review), then the
    # power-resume pair (the powerApplied-asymmetry witness), then the
    # fixture-free electrical checks.
    # =====================================================================

    # --- 6. The guided-ness gate with no progress, then resume --------------
    # Bare `z <dir>` = load latest, and the latest run (phase 5's) carries NO
    # guideProgress. The run file lost its own guide: section on the first
    # save, so the gate falls back to runSource - which names a wiring that IS
    # guided - and starts the guide fresh on this run file. Without that arm a
    # run quit before its first commit would be permanently non-guided and
    # would run the companion script against a circuit nobody built.
    d = GuideDriver()
    try:
        d.send(f"z {PROJ}\r\n".encode())
        guide_live = True
        loaded = d.expect_runfile("load", "bare `z <dir>` loads the latest run")
        check(loaded == run2, f"bare z loaded the latest run {run2} (got {loaded!r})")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT",
                 "no guideProgress + a guided runSource -> fresh guide at step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "step 2 WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "step 3 WAIT (2 commits in)")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit at step 3")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume file=" + re.escape(run2) + r" step=2",
                 "resume at the saved step 2, named by FILE")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=INIT",
                 "resume re-enters at step 3 (INIT)", timeout=40)
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "resumed step 3 WAIT")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the resumed session")
        # TASK 7 ruling 1, and the resume-then-quit variant of it: no power_on
        # ran behind this resume, so the guide gives the bench back instead of
        # leaving the rails parked at 0 V. Phase 17 pins the actual VALUES.
        d.expect(r"rails \+ DACs restored \(top=",
                 "no power_on behind the resume -> the user's rails are restored")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    out = jl_exec("print('rg=', 1 if is_connected(45, 50) else 0)", timeout=25)
    check(parse_kv(out).get("rg") == 1,
          "resume kept the committed place bridge 45-50")

    # --- 7. p un-commit: back out the place step, assert removal ------------
    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume file=" + re.escape(run2) + r" step=2",
                 "resume again (step 2)")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT",
                 "resumed at step 3 for the un-commit", timeout=40)
        d.send(b"p")
        d.expect(r"GUIDE step=2/3 id=place_RG state=BACK", "p un-commits the place step")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "re-entered step 2 WAIT")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit after the un-commit")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    out = jl_exec("""
print("rg=", 1 if is_connected(45, 50) else 0)
name = "none"
for i in range(1, 40):
    nodes = str(get_net_nodes(i))
    if not nodes:
        continue
    toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
    if "45" in toks:
        name = str(get_net_name(i))
        break
print("NETNAME|" + name)
""", timeout=25)
    check(parse_kv(out).get("rg") == 0, "p removed the place bridge 45-50")
    m = re.search(r"NETNAME\|(.+?)\s*$", out, re.MULTILINE)
    net_name = m.group(1).strip() if m else "none"
    check(net_name != "RG_A",
          f"RG_A net name is gone after the un-commit (got {net_name!r})")

    _, run2_yaml = read_device_file(run2)
    check(guide_progress_needle(WIRING_PATH, 1, 3) in run2_yaml,
          "guideProgress decremented to step 1 of 3 after p (in the run file) "
          "- step < of, i.e. MID-FLIGHT, which is the only state that prompts")
    check("placed: true" not in run2_yaml, "RG placed: false after the un-commit")

    # --- 8. Power fixture: power_on commit + resume re-applies rails --------
    out = jl_exec(f'print("wrote=", 1 if fs_write({POWER_PATH!r}, {POWER_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the power fixture")

    d = GuideDriver()
    try:
        d.send(f"z {POWER_PATH} new\r\n".encode())
        guide_live = True
        run_pw = d.expect_runfile("new", "power fixture allocated its own run file")
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT", "power fixture step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/4 id=connect_52 state=WAIT", "step 2 WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/4 id=power_on state=WAIT", "step 3 (power_on) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/4 id=power_on state=VERIFY check=rail_sane",
                 "power_on launches the default rail_sane check")
        d.expect(r"GUIDE step=3/4 id=power_on state=RESULT check=rail_sane val=norows ok=1",
                 "rail_sane passes as norows (no class-tagged pins)")
        d.expect(r"power applied: topRail=2\.50", "power_on commit applies topRail 2.5V")
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "step 4 WAIT (power live)")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit at step 4")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    _, pw_yaml = read_device_file(run_pw)
    check("topRail: 2.5" in pw_yaml, "the run file persisted topRail 2.5 after power_on")
    check(f'runSource: "{POWER_PATH}"' in pw_yaml,
          "runSource names the power fixture, not the default wiring")

    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume file=" + re.escape(run_pw) + r" step=3",
                 "resume past the committed power_on, from the power run file")
        d.expect(r"resume past power_on: rails re-applied",
                 "resume re-applies the file's power", timeout=40)
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "resumed at step 4")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the power-resumed session")
        time.sleep(0.5)
        d.pump()  # catch the exit tail before the negative assert below
        guide_live = False
    finally:
        d.close()
    check("rails + DACs restored" not in d.buf and
          "rails + DACs left at 0V" not in d.buf,
          "power was re-applied on resume -> the guide restores NOTHING and "
          "says nothing about the rails (the project's power stands)")
    time.sleep(1.0)

    # --- 8b. A SKIPPED power_on must stay skipped across a resume ------------
    #
    # H3 item 1, and the reason `skipped:` exists in the flow map at all.
    #
    # THE BUG THIS PINS. `guideStep` in the flow map is guideFirstUnfinished,
    # whose own comment says a SKIPPED step counts as FINISHED - so a skipped
    # step always lands strictly BELOW the persisted resume cursor. INIT's
    # resume loop then walked every index below the cursor doing
    # `committed[i] = true`, with nothing persisted that could tell a commit
    # from a skip. For a `power_on` step that promotion called setTopRail /
    # setBotRail / setDac*: THE RAILS CAME UP AT THE PROJECT'S VOLTAGE, two
    # lines under the banner that had just printed "rails + DACs held at 0V
    # until the power_on step". The user skipped power on purpose - the bench
    # supply was not ready - and the next launch energized it for them.
    #
    # Two knock-ons ride along and are asserted here too: the promotion set
    # powerApplied, which suppressed the exit tail's restore of the user's own
    # bench rails; and the promoted skips were counted as built work by the
    # DONE summary and by the "nothing was built, no script offer" gate.
    #
    # Phase 8 above is the mirror image (power_on COMMITTED -> resume DOES
    # re-apply and restores nothing). The pair is the point: the fix must not
    # be "never re-apply", it must be "re-apply exactly what was committed".
    d = GuideDriver()
    try:
        d.send(f"z {POWER_PATH} new\r\n".encode())
        guide_live = True
        run_sk = d.expect_runfile("new", "skipped-power phase allocated its own run file")
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT", "skipped-power phase at step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/4 id=connect_52 state=WAIT", "step 2 WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/4 id=power_on state=WAIT", "step 3 (power_on) WAIT")
        d.send(b"s")
        d.expect(r"\(skipped\)", "s skips the power_on instead of committing it")
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "step 4 WAIT, rails never came up")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit at step 4 with power_on skipped")
        guide_live = False
    finally:
        d.close()
    check("power applied: topRail" not in d.buf,
          "a skipped power_on applies no power in the first session")
    time.sleep(1.5)

    _, sk_yaml = read_device_file(run_sk)
    # THE FORMAT ASSERTION. Steps 0 and 1 committed, step 2 (power_on) skipped,
    # step 3 unfinished -> first-unfinished is 3, and the skip set is bit 2 =
    # 0x4. Without the mask this line would be indistinguishable from "three
    # steps committed", which is precisely how the rails got energized.
    check(guide_progress_needle(POWER_PATH, 3, 4, skipped=0x4) in sk_yaml,
          "the run file persists the SKIP SET (bit 2 = the power_on) beside "
          "the first-unfinished step, not just the scalar")
    check("topRail: 2.5" not in sk_yaml,
          "a skipped power_on leaves the run file's rails at 0V")

    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume file=" + re.escape(run_sk) + r" step=3",
                 "resume past the SKIPPED power_on, from its own run file")
        # THE HEADLINE ASSERTION - the one line that is the whole finding.
        d.expect(r"resume: power_on was skipped - rails stay at 0V",
                 "the resume HONOURS the skip: the rails are not energized",
                 timeout=40)
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "resumed at step 4")
        # The skip survived as a SKIP, not as a commit: 2 built, 1 skipped,
        # 1 unfinished. Under the bug this read committed=3.
        d.send(b">")
        d.expect(r"GUIDE done committed=2 skipped=1 unfinished=1",
                 "the resumed DONE summary counts the skip as a SKIP, and the "
                 "promoted-skip inflation of the built count is gone")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the skip-resumed session")
        # Knock-on (a): powerApplied stayed false, so the guide gives the
        # user's own bench rails back instead of silently keeping the
        # project's. Phase 8 asserts the exact opposite for a COMMITTED
        # power_on - that contrast is the regression guard.
        d.expect(r"rails \+ DACs restored \(top=",
                 "nothing was energized, so the user's rails are restored")
        guide_live = False
    finally:
        d.close()
    check("rails re-applied" not in d.buf,
          "NO re-apply claim behind a skipped power_on")
    time.sleep(1.0)

    _, sk2_yaml = read_device_file(run_sk)
    check("topRail: 2.5" not in sk2_yaml,
          "the resumed session never wrote the project's rail voltage - the "
          "physical witness that setTopRail was never called")

    # --- 9. No-power fixture: the powerApplied-asymmetry witness ------------
    out = jl_exec(f'print("wrote=", 1 if fs_write({NOPOWER_PATH!r}, {NOPOWER_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the no-power fixture")

    d = GuideDriver()
    try:
        d.send(f"z {NOPOWER_PATH} new\r\n".encode())
        guide_live = True
        run_np = d.expect_runfile("new", "no-power fixture allocated its own run file")
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT", "no-power fixture step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/4 id=connect_52 state=WAIT", "step 2 WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/4 id=power_on state=WAIT", "step 3 (power_on) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/4 id=power_on state=RESULT check=rail_sane val=norows ok=1",
                 "rail_sane still norows-passes without a power: section")
        d.expect(r"no power: section in the project - rails stay at 0V",
                 "power_on commit says nothing was applied")
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "step 4 WAIT")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit at step 4")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume file=" + re.escape(run_np) + r" step=3",
                 "resume past the no-power power_on, from its own run file")
        d.expect(r"resume past power_on: project has no power: section",
                 "resume notes there is nothing to re-apply (asymmetry fix)", timeout=40)
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "resumed at step 4")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the no-power resumed session")
        # The asymmetry witness survives task 7's rewording: a committed
        # power_on with NOTHING to apply still counts as "the project never
        # powered up", so the exit tail restores rather than staying silent.
        d.expect(r"rails \+ DACs restored \(top=",
                 "a power_on with nothing to apply still restores the user's "
                 "rails (the asymmetry fix, re-aimed at the new line)")
        guide_live = False
    finally:
        d.close()
    check("rails re-applied" not in d.buf,
          "no re-apply claim for a project with no power: section")
    time.sleep(1.0)

    # --- 10. Voltage + presence checks (REPL drives DAC1 mid-guide) ---------
    out = jl_exec(f'print("wrote=", 1 if fs_write({CHECKS_PATH!r}, {CHECKS_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the checks fixture")

    d = GuideDriver()
    try:
        d.send(f"z {CHECKS_PATH} new\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT", "checks fixture step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/4 id=verify_45 state=WAIT", "step 2 (voltage) WAIT")
        # Drive DAC1 mid-guide over the REPL (mpRemote is CRITICAL/inner-set,
        # so it pumps while the guide blocks). Hardware-only write: the
        # guide's INIT zeroed the state and the slot must keep that.
        out = jl_exec("dac_set(DAC1, 2.5)\nprint('dacset= 1')", timeout=25)
        check(parse_kv(out).get("dacset") == 1, "REPL drove DAC1 to 2.5V mid-guide")
        # Settle margin between the REPL's DAC write and the check. NOT about
        # scan freshness any more: the voltage check ALWAYS takes a one-shot
        # tap (GuideChecks.cpp deliberately dropped DESIGN §5.1's "<250 ms
        # old nodeVoltage[] is good enough" clause - timestamp-fresh EMA data
        # served 0.77 V for a row the DAC held at 2.5 V). What the pause still
        # buys is the DAC output and its route reaching the row before
        # anything measures it, plus the port-5 exec fully returning before
        # port 1 sends the next key.
        time.sleep(2.0)
        d.send(b"n")
        d.expect(r"GUIDE step=2/4 id=verify_45 state=VERIFY check=voltage",
                 "voltage check launches")
        d.expect(r"GUIDE step=2/4 id=verify_45 state=RESULT check=voltage val=2\.\d+V ok=1",
                 "voltage check passes with the measured ~2.5V")
        d.expect(r"GUIDE step=3/4 id=verify_45 state=WAIT", "step 3 (band miss) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=3/4 id=verify_45 state=RESULT check=voltage val=2\.\d+V ok=0 on_fail=warn",
                 "band-miss fails with the measured value and on_fail=warn")
        d.expect(r"check failed - n advances anyway", "warn offers the manual advance")
        d.send(b"n")
        d.expect(r"advancing past the failed check", "second n takes the warn advance")
        d.expect(r"GUIDE step=4/4 id=verify_58 state=WAIT", "step 4 (presence) WAIT")
        d.send(b"n")
        # Charge-retention presence: the check charges the row itself and
        # taps for retained charge. A bare row drains through the tap's 1M
        # front end at sub-ms tau - caught mid-drain it reads "decay",
        # fully drained "nocharge"; either is the required fail.
        d.expect(r"GUIDE step=4/4 id=verify_58 state=RESULT check=presence val=(nocharge|decay)@58 ok=0 on_fail=warn",
                 "presence on a bare row fails (nocharge/decay)")
        d.send(b"n")
        d.expect(r"GUIDE .* state=DONE", "warn advance past presence reaches DONE")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "EXIT from DONE")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)
    out = jl_exec("dac_set(DAC1, 0.0)\nprint('daczero= 1')", timeout=25)
    check(parse_kv(out).get("daczero") == 1, "DAC1 back to 0V after the check phase")

    # --- 11. Continuity refusal: pre-routed row -> skip + warn-continue -----
    out = jl_exec(f'print("wrote=", 1 if fs_write({REFUSAL_PATH!r}, {REFUSAL_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the refusal fixture")

    d = GuideDriver()
    try:
        d.send(f"z {REFUSAL_PATH} new\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE step=1/1 id=place_RG state=WAIT", "refusal fixture step 1")
        # Free ride on this fixture: ITS RG has no `connect:` on either pin,
        # so there is no hole for a leg to jump into and compact has nothing
        # to compact TO. The refusal must name that and change nothing - the
        # continuity phase below then runs exactly as it always did.
        d.send(b"c")
        d.expect(r"move refused: no leg has a hole-row endpoint to sit in",
                 "compact is refused on a part with no hole-row endpoint")
        d.send(b"n")
        d.expect(r"GUIDE step=1/1 id=place_RG state=VERIFY check=continuity",
                 "continuity check launches")
        d.expect(r"GUIDE step=1/1 id=place_RG state=RESULT check=continuity val=skip ok=0 on_fail=warn",
                 "stimulus refused: val=skip on the routed row")
        d.expect(r"check skipped \(rows in use\)", "the human refusal line prints")
        d.expect(r"check not run - continuing", "warn-class skip proceeds with the confirm")
        d.expect(r"GUIDE .* state=DONE", "the single step commits to DONE")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "EXIT from DONE")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 11b. a wrapped flow-map step no longer eats the next one -----------
    #
    # See CONT_WIRING. THE COUNT IS THE ASSERTION: before the fix this file
    # parsed as ONE step, so "(2 steps)" and a reachable `step=2/2` are both
    # things that could not have been printed. The prompt text is the second
    # half - it lives entirely on the continuation line, so seeing it proves
    # the tail was joined rather than merely tolerated.
    out = jl_exec(f'print("wrote=", 1 if fs_write({CONT_PATH!r}, {CONT_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the continuation-line fixture")

    d = GuideDriver()
    try:
        d.send(f"z {CONT_PATH} new\r\n".encode())
        guide_live = True
        # The residual, block-style shape: loud now, silent before.
        d.expect(r"guide step spans lines - unsupported, next step may be lost",
                 "a block-style line inside steps: warns instead of eating the "
                 "next step in silence", timeout=40)
        d.expect(r"=== Guided build: HIL Continuation === \(4 steps\)",
                 "the wrapped step, the one after it AND the apostrophe-bearing "
                 "step's successor all parsed (4 steps)")
        # STEP_ENTER prints the prompt BEFORE its status line, so expect the
        # text first (the suite's standing ordering trap).
        d.expect(r"step one wrapped onto two lines",
                 "the continuation line's own text: reached the step")
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT", "wrapped step 1 waits")
        d.send(b"n")
        d.expect(r"step two must survive",
                 "the step AFTER the wrapped one was not swallowed")
        d.expect(r"GUIDE step=2/4 id=note_2 state=WAIT", "step 2 waits")
        d.send(b"n")
        # The apostrophe is the point: a balanced line must NOT be treated as
        # unclosed. The commit echoes script:, so this also proves the field
        # parsed intact rather than merely surviving.
        d.expect(r"GUIDE step=3/4 id=run_3 state=WAIT",
                 "the apostrophe-bearing step is step 3 of 4, not a join victim")
        d.send(b"n")
        d.expect(r"run step '/python_scripts/o'brien\.py'",
                 "script: kept its apostrophe through the parse")
        d.expect(r"step four survives the apostrophe",
                 "THE NEEDLE: the step after the apostrophe was not swallowed")
        d.expect(r"GUIDE step=4/4 id=note_4 state=WAIT", "step 4 waits")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "EXIT from the continuation fixture")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 12. vf tap starvation: the bench `noroute`, reproduced + diagnosed --
    #
    # WHAT THIS PHASE LOCKS IN, AND WHAT IT DELIBERATELY DOES NOT.
    #
    # The suite had ZERO vf coverage; its other tap checks are single taps on a
    # near-empty board, where the direct tier routes trivially - which is why
    # HIL never hit the bench's `noroute`. This fixture rebuilds the crowded
    # 555 fabric that starves the sense tap, and asserts the HONEST current
    # behaviour: rows 22/23 are on chip D, the stimulus chain terminates on
    # chip D too and eats D's lanes to K, I, J and L, and the last-resort
    # bounce tier finds no candidate chip at all - so the tap is genuinely
    # refused. bounceOk=0x00 in the diagnostic below IS that proof.
    #
    # !!! THE FLIP HAPPENED (task 8). !!!
    # This phase asserted a refusal because the tap builder planned its sense
    # route ALONE, in three tiers, after the stimulus chain had already eaten
    # chip D's escapes - task 2 measured that with bounceOk=0x00, no candidate
    # chip left in the whole fabric. invest-vf-noroute.md §8 Option 1 has now
    # landed: the chain adds rowA->ADCx and rowB->ADCy as ephemeral bridges in
    # the SAME refreshLocalConnections as the stimulus, so the full router -
    # which has far more route shapes and may lawfully share the net's own
    # lanes - plans all five legs together, and the sampler reads the ring
    # channels with no fastConnectPath anywhere.
    #
    # So the exact fabric that starved on Kevin's bench AND in this fixture
    # now MEASURES. That is the wave's marquee regression and this line is
    # what holds it: VFNR_EXPECT_REFUSAL stays False.
    #
    # The routed-branch value is measured, not predicted: this same vf check
    # reads ~2.9 V on empty rows (the full stimulus across the gap, less the
    # chain's own drops, minus the two-point offset correction).
    #
    # What does NOT change across the flip: the check FAILS either way (ok=0,
    # on_fail=warn), because rows 22/23 are empty holes - routed, the sense
    # legs read the full stimulus across the gap, which lands outside the
    # 1.4-2.6 band as "no current". So the warn-advance tail is shared.
    #
    # THE SEAM THE FLIP UNCOVERED. The refusal branch was the only exercise of
    # the scan-core latch -> core-0 print path (the scan core must never
    # Serial-print, so seeing `TAP noroute ...` + the counters at all is what
    # proves the latch survives cross-core). Phase 12b below is its
    # replacement: a fixture that starves the taps a DIFFERENT way - by
    # user-claiming every ADC the pool could hand out - so the diagnostic seam
    # keeps its coverage. Do NOT delete either phase.
    VFNR_EXPECT_REFUSAL = False
    out = jl_exec(f"""
for d in ("/projects", {VFNR_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({VFNR_DIR!r}) else 0)
print("wrote=", 1 if fs_write({VFNR_PATH!r}, {VFNR_WIRING!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("projdir") == 1, f"created {VFNR_DIR} on the board")
    check(vals.get("wrote") == 1, f"pushed {VFNR_PATH}")

    d = GuideDriver()
    try:
        d.send(f"z {VFNR_PATH} new\r\n".encode())
        guide_live = True
        for i, sid in enumerate(VFNR_CONNECT_IDS, start=1):
            d.expect(rf"GUIDE step={i}/11 id={sid} state=WAIT",
                     f"vfnr step {i}/11 ({sid}) WAIT", timeout=40)
            d.send(b"n")
            d.expect(rf"GUIDE step={i}/11 id={sid} state=COMMIT",
                     f"vfnr step {i}/11 COMMIT", timeout=40)
        d.expect(r"GUIDE step=11/11 id=place_LEDX state=WAIT",
                 "vfnr step 11 (vf place) WAIT", timeout=40)
        d.send(b"n")
        d.expect(r"GUIDE step=11/11 id=place_LEDX state=VERIFY check=vf",
                 "vf check launches on the crowded fabric")
        if VFNR_EXPECT_REFUSAL:
            # Refusal-only artifacts: neither of these prints on a successful
            # tap, so they live entirely inside this branch.
            # rc=-3 is buildEphemeralRoute refusing; bounceOk=0x00 means the
            # last tier had no candidate chip - starvation, not a stale ring.
            d.expect(r"TAP noroute node=2[23]->ADC\d rc=-3 Kfree=0x[0-9A-Fa-f]+ "
                     r"Kxbusy=0x[0-9A-Fa-f]+ Dxbusy=0x[0-9A-Fa-f]+ "
                     r"bounceOk=0x[0-9A-Fa-f]+",
                     "tap-failure diagnostic names the node, rc and the masks")
            d.expect(r"\[nvscan\] taps ok:\d+ .*noroute:\d+ .*ringstale:\d+",
                     "tap counters print unconditionally on the hard failure")
            d.expect(r"GUIDE step=11/11 id=place_LEDX state=RESULT check=vf "
                     r"val=noroute@2[23] ok=0 on_fail=warn",
                     "vf reports noroute PER NODE (see the flip note above)")
            d.expect(r"no sense route to the rows", "the human refusal line prints")
        else:
            # Post-Option-1: the taps route. Still ok=0 - empty holes read the
            # full stimulus across the gap, outside the 1.4-2.6 band.
            d.expect(r"GUIDE step=11/11 id=place_LEDX state=RESULT check=vf "
                     r"val=[23]\.\d+V ok=0 on_fail=warn",
                     "vf ROUTED on the starving fabric and reported a measured "
                     "voltage (Option 1: the marquee regression)")
            # The §4 detail line, on a FAIL - the measurement prints either way
            # now, and vf carries the current it was measured at (§1.10).
            d.expect(r"LEDX vf [23]\.\d+V @ .*\(band 1\.40-2\.60V\)",
                     "vf detail carries the value AND the current, on a fail")
            d.expect(r"no current - LED missing or reversed",
                     "empty rows 22/23 report no current, not a refusal")
        # Shared tail: warn-class failure either way.
        d.expect(r"check failed - n advances anyway", "warn offers the advance")
        d.send(b"n")
        d.expect(r"advancing past the failed check", "second n takes the advance")
        d.expect(r"GUIDE .* state=DONE", "warn advance reaches DONE")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "EXIT from DONE")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 12b. the diagnostic seam, starved a DIFFERENT way ------------------
    #
    # Phase 12's flip took the scan-core-latch -> core-0-print path out of the
    # suite: a routed tap prints neither the `TAP noroute` line nor the
    # counters, and that seam is load-bearing (the scan core must NEVER
    # Serial-print, so it latches into volatiles and core 0 formats them).
    #
    # This fixture starves the taps by exhausting the OTHER resource. Every
    # channel the pool can hand out - ADC0-3 - is claimed by a user bridge, so
    # `infraAdcUserClaimed` excludes all four: chainBegin cannot acquire the
    # two it needs for Option 1's sense legs and falls back to task 2's
    # one-shot taps, and those cannot acquire one either. The refusal is
    # therefore honest and structural, not manufactured - a user really can
    # bridge all four ADCs, and the 555 project already bridges two.
    #
    # The DIAGNOSTIC SHAPE DIFFERS from phase 12's old one, deliberately:
    # nothing called fastConnectPath, so the latch carries node=-1 rc=0 with
    # empty masks. That is exactly the discriminator invest-vf-noroute.md §6
    # asked for - "node=-1 rc=0 means -2 came from ADC exhaustion or a stale
    # ring, not from route starvation" - and asserting it here keeps BOTH
    # readings of the line under test.
    #
    # vf, not continuity: a continuity check with a parsed value degrades to
    # the §1.9 current-only verdict instead of refusing, which is correct
    # behaviour and the wrong thing to assert here.
    out = jl_exec(f"""
for d in ("/projects", {STARVE_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({STARVE_DIR!r}) else 0)
print("wrote=", 1 if fs_write({STARVE_PATH!r}, {STARVE_WIRING!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("projdir") == 1, f"created {STARVE_DIR} on the board")
    check(vals.get("wrote") == 1, f"pushed {STARVE_PATH}")

    d = GuideDriver()
    try:
        d.send(f"z {STARVE_PATH} new\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE step=1/1 id=place_LEDY state=WAIT",
                 "adc-starved fixture step 1 WAIT", timeout=40)
        d.send(b"n")
        d.expect(r"GUIDE step=1/1 id=place_LEDY state=VERIFY check=vf",
                 "vf launches with every ADC user-claimed")
        # node=-1 rc=0: no fastConnectPath was refused - the pool had nothing
        # to grant. Masks are zero for the same reason.
        d.expect(r"TAP noroute node=-1->ADC-1 rc=0 Kfree=0x[0-9A-Fa-f]+ "
                 r"Kxbusy=0x[0-9A-Fa-f]+ \?xbusy=0x[0-9A-Fa-f]+ "
                 r"bounceOk=0x[0-9A-Fa-f]+",
                 "the scan-core latch reaches core 0 and names ADC exhaustion")
        d.expect(r"\[nvscan\] taps ok:\d+ .*noroute:\d+ .*ringstale:\d+",
                 "tap counters print unconditionally on the hard failure")
        d.expect(r"GUIDE step=1/1 id=place_LEDY state=RESULT check=vf "
                 r"val=noroute@2[23] ok=0 on_fail=warn",
                 "vf refuses PER NODE when no ADC can be had")
        d.expect(r"no sense route to the rows", "the human refusal line prints")
        d.expect(r"check failed - n advances anyway", "warn offers the advance")
        d.send(b"n")
        d.expect(r"GUIDE .* state=DONE", "warn advance reaches DONE")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "EXIT from DONE")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 12c. the ground-side topology, measured -----------------------------
    #
    # invest-measurement.md §5 item 2 - the go/no-go that chose §1.2 over §1.3,
    # re-run as a regression. Three readings, and the THIRD is what makes the
    # second mean anything:
    #
    #   A  old top-side chain, no part  -> the CURR_SENSE- sink, ~2.3 mA
    #   B  new ground-side chain, no part -> must be < 0.3 mA
    #   C  new ground-side chain + a rowA<->rowB bridge -> real loop current
    #
    # Without C, B reads the same whether the topology works or the
    # ISENSE_MINUS->GND leg silently failed to route: no loop, no current
    # either way. C proves the loop is closed.
    out = jl_exec("""
import time
nodes_clear()
time.sleep(0.2)
dac_set(DAC0, 0.0)
time.sleep(0.1)

def loop_mA():
    v = []
    for _ in range(8):
        v.append(ina_get_current(0) * 1000.0)
        time.sleep(0.05)
    v.sort()
    return v[len(v) // 2]

# A: the OLD chain - DAC0 -> shunt -> row, with the sink downstream of nothing
connect(DAC0, ISENSE_PLUS)
connect(ISENSE_MINUS, 34)
connect(44, GND)
time.sleep(0.3)
dac_set(DAC0, 3.3)
time.sleep(0.4)
print("old_mA=", loop_mA())
dac_set(DAC0, 0.0)
nodes_clear()
time.sleep(0.2)

# B: the NEW chain - the shunt sits between the far row and ground
connect(DAC0, 34)
connect(44, ISENSE_PLUS)
connect(ISENSE_MINUS, GND)
time.sleep(0.3)
dac_set(DAC0, 3.3)
time.sleep(0.4)
print("new_mA=", loop_mA())

# C: positive control - close the loop with a crossbar 'part'
connect(34, 44)
time.sleep(0.3)
print("closed_mA=", loop_mA())
print("ina1_mA=", ina_get_current(1) * 1000.0)
dac_set(DAC0, 0.0)
time.sleep(0.1)
nodes_clear()
""", timeout=90)
    vals = parse_kv(out)
    old_mA = abs(vals.get("old_mA", 0.0))
    new_mA = abs(vals.get("new_mA", 99.0))
    closed_mA = abs(vals.get("closed_mA", 0.0))
    ina1_mA = abs(vals.get("ina1_mA", 0.0))
    check(old_mA > 1.0,
          f"the OLD top-side chain still shows the CURR_SENSE- sink "
          f"({old_mA:.3f} mA with no part) - the control that makes B mean something")
    check(new_mA < 0.3,
          f"GROUND-SIDE: no part, no current through the shunt "
          f"({new_mA:.3f} mA < 0.3) - the sink is structurally gone")
    check(closed_mA > 2.0,
          f"and the loop really is closed: bridging the rows draws "
          f"{closed_mA:.2f} mA, so ISENSE_MINUS->GND routed")
    # §1.7's INA1 crosscheck, on a loop where both should see the same current.
    check(abs(ina1_mA - closed_mA) <= (0.1 * closed_mA + 0.2),
          f"INA0 ({closed_mA:.2f} mA) and INA1 ({ina1_mA:.2f} mA) agree within "
          f"10% + 0.2 mA - no current leaking outside the part path")

    # --- 12d. four-wire R against a known crossbar 'part' --------------------
    #
    # §5 item 3, adapted - see the report's §1.7. The guide-level version
    # (author a continuity step over two bridged rows) is NOT constructible:
    # the stimulus refusal rule refuses to energize a row that already carries
    # a user bridge, and a bridge is the only way to make two rows conduct
    # without a real part. So this drives the SAME five-leg topology
    # chainBegin builds - three stimulus legs plus Option 1's two sense ADC
    # bridges - by hand, and asserts the arithmetic the check performs on it.
    #
    # The assertion that matters is the LAST one: 4-wire and 2-wire disagree
    # by the whole stimulus path, which is the entire reason the rework exists.
    out = jl_exec("""
import time
nodes_clear()
time.sleep(0.2)
dac_set(DAC0, 0.0)
time.sleep(0.1)
connect(DAC0, 34)
connect(44, ISENSE_PLUS)
connect(ISENSE_MINUS, GND)
connect(34, ADC2)
connect(44, ADC3)
connect(34, 44)
time.sleep(0.4)

i = []
for _ in range(4):
    i.append(ina_get_current(0) * 1000.0)
    time.sleep(0.06)
i.sort()
i_ofs = i[len(i) // 2]
va_ofs = adc_get(2)
vb_ofs = adc_get(3)
print("i_ofs_mA=", i_ofs)

dac_set(DAC0, 3.3)
time.sleep(0.3)
i = []
for _ in range(8):
    i.append(ina_get_current(0) * 1000.0)
    time.sleep(0.06)
i.sort()
i_med = i[len(i) // 2]
print("i_mA=", i_med)
print("vA=", adc_get(2))
print("vB=", adc_get(3))

rs = []
for _ in range(10):
    dv = (adc_get(2) - adc_get(3)) - (va_ofs - vb_ofs)
    rs.append(dv / ((i_med - i_ofs) / 1000.0))
    time.sleep(0.03)
rs.sort()
print("r_med=", rs[len(rs) // 2])
print("r_min=", rs[0])
print("r_max=", rs[-1])
print("r_2wire=", 3.3 / (i_med / 1000.0))
# Q1's margin datum, MEASURED rather than argued: INA1 (0x41, R57 in DAC0's
# OUTPUT path) is the watchdog that trips at 50 mA on fault current bypassing
# the ground-side shunt. This is what it reads on a healthy low-resistance
# loop, i.e. how much headroom the trip actually has over the harshest
# legitimate operating point the suite produces. Reported, never asserted -
# a threshold assertion here would just re-encode the constant.
print("ina1_mA=", ina_get_current(1) * 1000.0)
# the loop is deliberately LEFT LIVE - the scale assertion below reads the
# shunt register through `z shunt` at this same ~14 mA operating point.
""", timeout=90)
    vals = parse_kv(out)
    vA = vals.get("vA", 0.0)
    vB = vals.get("vB", 0.0)
    r_med = vals.get("r_med", 0.0)
    r_2wire = vals.get("r_2wire", 0.0)
    spread = abs(vals.get("r_max", 0.0) - vals.get("r_min", 0.0))
    check(vA > 1.0,
          f"the rowA sense bridge routed and reads the stimulus ({vA:.3f} V) - "
          f"Option 1's sense leg is on the row")
    check(0.05 < vB < vA,
          f"the rowB sense bridge routed and reads the drop above the shunt "
          f"({vB:.3f} V), below rowA as the current direction demands")
    # A crossbar bridge is 1-3 crosspoints at a measured mean 41.8 ohm each.
    check(10.0 < r_med < 250.0,
          f"4-wire R across the crossbar 'part' is crosspoint-scale "
          f"({r_med:.1f} ohm)")
    check(spread < 0.2 * abs(r_med),
          f"and it is stable: {spread:.1f} ohm spread over 10 reads "
          f"(< 20% of {r_med:.1f})")
    check(r_2wire > 2.0 * r_med,
          f"THE POINT: 2-wire would have reported {r_2wire:.0f} ohm for the same "
          f"part that 4-wire measures at {r_med:.0f} - the difference is the "
          f"stimulus path, and it is why bands could never be honest before")
    # Datum only (no assertion): the INA1 source watchdog's headroom at the
    # harshest healthy operating point this suite builds.
    print(f"  info: INA1 sees {abs(vals.get('ina1_mA', 0.0)):.2f} mA leaving DAC0 "
          f"on this ~{abs(vals.get('i_mA', 0.0)):.1f} mA loop - the source "
          f"watchdog trips at 50 mA")

    # THE SHUNT REGISTER'S SCALE, under load. Everything else in the suite
    # reads INA0's CURRENT register (12c, 12d above) or reads the shunt
    # register UNLOADED (12e, where the answer is ~0 either way), and every
    # guide-level phase lands on `open` / `no current` where the scale is
    # irrelevant - so a 2x error in inaShuntCurrent_mA()'s `/2.0f`, or reading
    # getShuntVoltage() in volts instead of millivolts, would pass the entire
    # suite while corrupting every resistance the rework computes. The loop is
    # still live at ~14 mA, so the two registers must now agree.
    i_reg_mA = abs(vals.get("i_mA", 0.0))
    out = port1_command("z shunt 16", collect_seconds=6.0)
    m = re.search(r"SHUNT n=(\d+) .*mean_mA=(-?[\d.]+)", out)
    check(m is not None,
          f"'z shunt' answers under load (got: {out[-160:]!r})")
    if m:
        i_shunt_mA = abs(float(m.group(2)))
        check(abs(i_shunt_mA - i_reg_mA) <= max(0.1 * i_reg_mA, 0.3),
              f"SCALE: the shunt register reads {i_shunt_mA:.2f} mA where the "
              f"current register reads {i_reg_mA:.2f} mA (within 10%) - the "
              f"/2.0f across the 2 ohm R1, and mV-not-volts, are both right")
    jl_exec("""
import time
dac_set(DAC0, 0.0)
time.sleep(0.1)
nodes_clear()
time.sleep(0.2)
""", timeout=30)

    # --- 12e. the shunt register's quantization floor ------------------------
    #
    # §5 item 7. The whole measurement now rides on INA0's shunt-voltage
    # register instead of its current register, because the latter is
    # calibrated to 30.5 uA/bit and a 47k part at 3.3 V is two counts. The
    # shunt register's LSB is a hardware constant 10 uV - 5 uA across R1 - and
    # this asserts the noise floor is actually that good.
    # `active` needs BOTH isense nodes routed - the poll skips entirely
    # otherwise - so route the unloaded chain first and hold DAC0 at 0.
    jl_exec("""
import time
nodes_clear()
time.sleep(0.2)
dac_set(DAC0, 0.0)
connect(DAC0, 34)
connect(44, ISENSE_PLUS)
connect(ISENSE_MINUS, GND)
time.sleep(0.4)
""", timeout=30)
    out = port1_command("z shunt 32", collect_seconds=8.0)
    jl_exec("import time\nnodes_clear()\ntime.sleep(0.2)\n", timeout=25)
    m = re.search(r"SHUNT n=(\d+) mean_mV=(-?[\d.]+) min_mV=(-?[\d.]+) "
                  r"max_mV=(-?[\d.]+) spread_lsb=([\d.]+)", out)
    check(m is not None, f"'z shunt 32' reports the register (got: {out[-160:]!r})")
    if m:
        check(int(m.group(1)) >= 16,
              f"collected {m.group(1)} fresh shunt samples")
        check(float(m.group(5)) <= 4.0,
              f"unloaded shunt-register spread is {m.group(5)} LSB (<= 4, i.e. "
              f"+/-2) - 5 uA/LSB is real resolution, not a rounding artifact")

    # --- 12f. the band table, off-bench --------------------------------------
    #
    # §5 item 1. `z band` runs the same parsePartValue / guideResistorBand the
    # check runs, with no hardware in the path at all, so the shipped band
    # table is regression-tested without a part in a hole. The rows here ARE
    # invest-measurement.md §2.5's table.
    BAND_CASES = [
        # (command args, expected regex, what)
        ("10k resistor",   r"kind=ohms .*band=8\.00k-12\.0k stim=3\.3",
         "10k -> 8.00k-12.0k at 3.3V (555 R1, eeprom-scale)"),
        ("47k resistor",   r"kind=ohms .*band=35\.2k-58\.8k stim=5\.0",
         "47k -> 35.2k-58.8k and the >=20k rule drives it at 5V (555 R2)"),
        ("330 resistor",   r"kind=ohms .*band=264-396 stim=3\.3",
         "330 -> 264-396 (555 R3, nand00 R1)"),
        ("4.7k resistor",  r"kind=ohms .*band=3\.76k-5\.64k stim=3\.3",
         "4.7k -> 3.76k-5.64k (eeprom R1/R2)"),
        ("10k resistor 5", r"kind=ohms .*tol=10 .*band=9\.00k-11\.0k",
         "an author tol: 5 tightens 10k to +/-10% total"),
        ("1M resistor",    r"kind=ohms ohms=1e\+06 ",
         "'M' on a resistor is mega"),
        ("1m resistor",    r"kind=ohms ohms=1e\+06 ",
         "and so is 'm' - the shipped convention, milliohms are a non-goal"),
        ("470R resistor",  r"kind=ohms ohms=470 ",
         "a trailing R is an ohm unit, not a multiplier"),
        ("10uF capacitor", r"kind=farads v=1e-05 band=none",
         "10uF parses as farads and gets no resistor band"),
        ("100nF capacitor", r"kind=farads v=1e-07 band=none", "100nF parses"),
        # the reject table (§2.1)
        ("10uF resistor",  r"kind=none v=-1", "unit/type contradiction is rejected"),
        ("1M capacitor",   r"kind=none v=-1", "megafarads are rejected"),
        ("2k2 resistor",   r"kind=none v=-1", "'2k2' infix is rejected, not misparsed"),
        ("NE555 ic",       r"kind=none v=-1", "a part number with no digits leading"),
        ("24C02 ic",       r"kind=none v=-1", "a part number with digits leading"),
        ("0 resistor",     r"kind=none v=-1", "zero is not a part value"),
        ("-10k resistor",  r"kind=none v=-1", "negative is not a part value"),
    ]
    for args, rx, what in BAND_CASES:
        out = port1_command(f"z band {args}", collect_seconds=1.2)
        line = ""
        for ln in out.splitlines():
            if "GUIDEBAND" in ln:
                line = ln.strip()
                break
        check(re.search(rx, line) is not None, f"z band {args}: {what}")
        time.sleep(0.1)

    # --- 12g. rail_sane: the bench failure, replayed ------------------------
    #
    # §3 + §5 item 6. The bench read 4.74 V on row 8 of a rail SET to 5.00 and
    # the check failed it, because it compared the row against the setpoint -
    # while the net-scan work had already proved this board's rails run ~220 mV
    # low. The rule is now two-step: gate the rail against its setpoint
    # generously (the board check), then compare rows against the MEASURED
    # rail tightly and on the same ADC channel (the wiring check). A healthy
    # board passes; a miswired row still fails by volts.
    #
    # Three fixtures, one per verdict, each a placed part whose pin classes
    # are what collectClassRows() finds.
    for path, wiring, expect_rx, expect_what, detail_rx in (
        (RAIL_OK_PATH, RAIL_OK_WIRING,
         r"state=RESULT check=rail_sane val=[45]\.\d+V@8 ok=1",
         "THE REGRESSION: a row bridged to a 5.00V-set rail PASSES on its "
         "measured value (this exact reading failed on the bench)",
         r"rail: meas [45]\.\d+V \(set 5\.00V\); worst row delta 0\.\d+V @8"),
        (RAIL_GND_PATH, RAIL_GND_WIRING,
         r"state=RESULT check=rail_sane val=-?\d\.\d+V@8 ok=0",
         "a power-class row wired to GND still fails, by volts of margin",
         r"rail: meas [45]\.\d+V \(set 5\.00V\); row 8 reads -?\d\.\d+V"),
        (RAIL_FLOAT_PATH, RAIL_FLOAT_WIRING,
         r"state=RESULT check=rail_sane val=(float@8|-?\d\.\d+V@8) ok=0",
         "an unconnected power-class row fails (floating, or off the rail)",
         None),
    ):
        jl_exec(f"""
for d in ("/projects", {RAIL_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("wrote=", 1 if fs_write({{p!r}}, {{w!r}}) else 0)
""".format(p=path, w=wiring), timeout=30)
        d = GuideDriver()
        try:
            d.send(f"z {path} new\r\n".encode())
            guide_live = True
            d.expect(r"GUIDE step=1/2 id=place_PWR state=WAIT",
                     f"{path}: the power-class part step", timeout=40)
            d.send(b"n")
            d.expect(r"GUIDE step=1/2 id=place_PWR state=COMMIT",
                     f"{path}: part committed, its bridges live")
            d.expect(r"GUIDE step=2/2 id=power_on state=WAIT",
                     f"{path}: power_on step")
            d.send(b"n")
            d.expect(r"GUIDE step=2/2 id=power_on state=VERIFY check=rail_sane",
                     f"{path}: rail_sane launches")
            d.expect(expect_rx, expect_what, timeout=30)
            if detail_rx:
                d.expect(detail_rx,
                         "the detail line names the MEASURED rail and the setpoint")
            if "ok=0" in expect_rx:
                d.expect(r"check failed - n advances anyway", "warn offers the advance")
                d.send(b"n")
            d.expect(r"GUIDE .* state=DONE", f"{path}: reaches DONE")
            d.send(b"q")
            d.expect(r"GUIDE .* state=EXIT", f"{path}: EXIT")
            guide_live = False
        finally:
            d.close()
        # Rails are live after a committed power_on - put them back before the
        # next fixture measures anything.
        jl_exec("import time\ndac_set(TOP_RAIL, 0.0)\ndac_set(BOTTOM_RAIL, 0.0)\n"
                "nodes_clear()\ntime.sleep(0.2)\n", timeout=25)
        time.sleep(1.0)

    # --- 12i. oscillates on 5 V: the refusal must not waive the band --------
    #
    # H1 review F1. The oscillates check counts edges on a 3.3 V RP2350 GPIO,
    # so it now refuses to close that route when the project's rails could
    # swing the target above the pin's domain, and falls back to high-Z taps.
    # The taps can see THAT a node is swinging but not HOW FAST - and the
    # fallback's success verdict used to be an unconditional GUIDE_CHECK_PASS
    # that never read st.min/st.max. So the refusal silently converted "1.4 Hz,
    # inside 0.3-30" into "both levels seen, PASS": the shipped 555 (5.0 V
    # rail, min: 0.3, max: 30) would have passed while blinking at 500 Hz.
    #
    # A guide that cannot measure something must say so; it may never call it
    # good. The verdict is keyed on whether a BAND was authored, so:
    #
    #   banded   -> GUIDE_CHECK_SKIPPED, val=unmeasured, ok=0 ("unverified")
    #   unbanded -> GUIDE_CHECK_PASS,    val=osc,        ok=1
    #
    # The unbanded half is not decoration: without it "always refuse" would
    # also pass this phase, and the author who only asked "is it oscillating?"
    # deserves the answer the taps really can give.
    #
    # NOTE the SINGLE 'n' on the banded case. That is the behavioural proof of
    # SKIPPED over FAIL, and it cannot be read off `ok=` alone: SKIPPED never
    # measured anything, so GuidedFlow's warn branch prints "(check not run -
    # continuing)" and goes straight to COMMIT on the confirm that launched the
    # check, while a measured FAIL sits at WAIT until a SECOND confirm. If the
    # verdict ever regressed to FAIL, the COMMIT expect below would time out.
    print("  --- 12i: oscillates, the 5V refusal and the authored band ---")

    # WHEN THE PWM IS STARTED IS LOAD-BEARING, and it cost a red run to learn.
    # A project load re-applies the file's `config:` (and the refresh's setGPIO
    # pass re-asserts GPIO direction/PWM from it), so a PWM started BEFORE
    # `z ... new` is dead by the time the guide is running - measured: row 30
    # swings 0<->3.28 V before the load and reads a flat 0.03 V span after it.
    # The power_on step's own COMMIT refresh kills it a second time. So it is
    # started HERE, after that commit, from the REPL - which is reachable
    # mid-guide because MpRemoteService is in the guide loop's inner set, the
    # same door phase 10 uses to drive DAC1. Nothing after this point does a
    # full refreshConnections: the check is refused at gate 1 before any GPIO
    # route, and the tap fallback uses fastConnectPath, not a refresh.
    try:
        for path, wiring, want_val, want_ok, label in (
            (RAIL_OSC_BAND_PATH, RAIL_OSC_BAND_WIRING, "unmeasured", "0",
             "a BANDED oscillates whose frequency could not be measured"),
            (RAIL_OSC_NOBAND_PATH, RAIL_OSC_NOBAND_WIRING, "osc", "1",
             "an UNBANDED oscillates - 'is it oscillating' was answered"),
        ):
            jl_exec("""
print("wrote=", 1 if fs_write({p!r}, {w!r}) else 0)
""".format(p=path, w=wiring), timeout=30)
            d = GuideDriver()
            guide_live = False
            try:
                d.send(f"z {path} new\r\n".encode())
                guide_live = True
                d.expect(r"GUIDE step=1/2 \S+ state=WAIT",
                         f"12i: {label}: the power_on step", timeout=40)
                d.send(b"n")
                d.expect(r"GUIDE step=2/2 \S+ state=WAIT",
                         "12i: power_on committed (5V live), at the oscillates step",
                         timeout=45)
                # jl_pwm_func returns None and RAISES on a bad setup
                # (modjumperless.c:2380-2386), so "it did not raise" is the
                # whole success signal.
                out = jl_exec(f"""
try:
    pwm({OSC_PWM_PIN}, {OSC_PWM_HZ}, 0.5)
    print("pwmok=", 1)
except Exception as e:
    print("pwmok=", 0)
    print("pwmerr=", e)
""", timeout=30)
                check(parse_kv(out).get("pwmok") == 1,
                      f"12i: a {OSC_PWM_HZ} Hz slow PWM is running on GPIO "
                      f"{OSC_PWM_PIN} -> row {OSC_TARGET_ROW} - the oscillating "
                      f"source this phase measures against")
                d.send(b"n")
                d.expect(r"GUIDE step=2/2 \S+ state=VERIFY check=oscillates",
                         "12i: the oscillates check launched")
                # The ceiling refusal happens before any hardware moves, and it
                # must SAY so - the brief's ruling is that a refusal explains
                # itself in the check's own voice.
                d.expect(r"osc: this project drives its rails at 5\.00 V",
                         "12i: the refusal named the rail voltage and the reason",
                         timeout=30)
                m = d.expect(r"GUIDE step=2/2 \S+ state=RESULT check=oscillates "
                             r"val=(\S+) ok=(\d)",
                             f"12i: {label}: reported a verdict", timeout=45)
                got_val = m.group(1) if m else "??"
                got_ok = m.group(2) if m else "?"
                check(got_val == want_val and got_ok == want_ok,
                      f"12i: {label} -> val={want_val} ok={want_ok} "
                      f"(got val={got_val} ok={got_ok})")
                if want_val == "unmeasured":
                    d.expect(r"\(check not run - continuing\)",
                             "12i: reported as NOT RUN rather than measured-and-failed",
                             timeout=25)
                    d.expect(r"GUIDE step=2/2 \S+ state=COMMIT",
                             "12i: ...and a SINGLE confirm advanced it - the "
                             "behavioural proof it is SKIPPED, not FAIL",
                             timeout=25)
                else:
                    d.expect(r"GUIDE step=2/2 \S+ state=COMMIT",
                             "12i: the unbanded PASS committed")
                d.expect(r"GUIDE .* state=DONE", f"12i: {label}: reaches DONE")
                d.send(b"q")
                d.expect(r"GUIDE .* state=EXIT", f"12i: {label}: EXIT")
                guide_live = False
            finally:
                if guide_live:
                    d.send(b"q")
                    time.sleep(0.5)
                d.close()
            jl_exec(f"import time\npwm_stop({OSC_PWM_PIN})\n"
                    "dac_set(TOP_RAIL, 0.0)\ndac_set(BOTTOM_RAIL, 0.0)\n"
                    "nodes_clear()\ntime.sleep(0.2)\n", timeout=25)
            time.sleep(1.0)
    finally:
        # Belt and braces: the per-iteration stop above is the normal path, but
        # an exception mid-drive must not leave a GPIO toggling on the bench.
        jl_exec(f"pwm_stop({OSC_PWM_PIN})", timeout=25)

    # --- 12h. the ohm flip's two legacy guards ------------------------------
    #
    # §2.3 + §5 item 8. Continuity min/max used to be MILLIAMPS. They are ohms
    # now, so a file still carrying the old numbers would read "0.1-0.8 ohms"
    # and fail everything. Two guards, and this fixture trips both:
    #   RLEG  value 10k + min 0.1/max 0.8 -> the band does not BRACKET 10k
    #   RBARE no value  + max 0.3         -> no real resistor band is under 5 ohm
    # Both must log the warning and fall back to the derived band; the third
    # step proves an ohm band that IS meaningful is honoured as authored.
    jl_exec(f"""
for d in ("/projects", {LEGACY_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("wrote=", 1 if fs_write({LEGACY_PATH!r}, {LEGACY_WIRING!r}) else 0)
""", timeout=30)
    d = GuideDriver()
    try:
        d.send(f"z {LEGACY_PATH} new\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE step=1/3 id=place_RLEG state=WAIT", "legacy fixture step 1",
                 timeout=40)
        d.send(b"n")
        d.expect(r"min/max look like legacy mA \(0\.1-0\.8\) - ignoring, using "
                 r"the value-derived ohm band",
                 "GUARD 1: an authored band that does not bracket value: is legacy mA")
        d.expect(r"state=RESULT check=continuity val=open ok=0",
                 "and the check runs on the derived band, reporting the honest open")
        d.expect(r"RLEG: open \(.*band 8\.00k-12\.0k\)",
                 "the detail line proves the DERIVED band was used, not 0.1-0.8")
        d.expect(r"check failed - n advances anyway", "warn advance offered")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RBARE state=WAIT", "legacy fixture step 2")
        d.send(b"n")
        d.expect(r"min/max look like legacy mA \(0\.04-0\.3\) - ignoring",
                 "GUARD 2: a value-less part whose max is under 5 ohm is legacy mA")
        d.expect(r"state=RESULT check=continuity val=open ok=0",
                 "value-less legacy step falls back to 'something conducts'")
        d.expect(r"check failed - n advances anyway", "warn advance offered")
        d.send(b"n")
        d.expect(r"GUIDE step=3/3 id=place_ROHM state=WAIT", "legacy fixture step 3")
        d.send(b"n")
        d.expect(r"state=RESULT check=continuity val=open ok=0",
                 "a real ohm band runs without any legacy warning")
        d.expect(r"ROHM: open \(.*band 20-900\)",
                 "an authored band that DOES bracket the value is honoured as ohms")
        d.expect(r"check failed - n advances anyway", "warn advance offered")
        d.send(b"n")
        d.expect(r"GUIDE .* state=DONE", "legacy fixture reaches DONE")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "legacy fixture EXIT")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 13. Move + snap: the pads' headless twins --------------------------
    #
    # Kevin's control-surface principle says the probe pads are ABSOLUTE - tap
    # a hole and the part acts THERE - and the wheel is relative. This suite
    # can do neither: it never turns the wheel and it cannot touch a hole. So
    # what it drives is the absolute pair's SERIAL twins, `m <row>` (move pin 1
    # to this row) and `c` (flip this part compact/expanded), which run through
    # exactly the same handlers the pads do. The tap rules and the wheel are
    # bench items.
    #
    # RG is wired one leg to a hole (row 50) and one leg to GND (crossbar, no
    # holes), so ONE part witnesses both halves of the per-pin compact rule -
    # see the fixture note at the top.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        run_mv = d.expect_runfile("new", "move/snap phase allocated its own run file")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "move phase step 1 (note) WAIT")
        # (a) Both keys are refused BY NAME off a place step. A note step has
        # no part, so there is nothing to move and nothing to snap.
        d.send(b"m 44\r")
        d.expect(r"\(m <row> works on place steps only\)",
                 "m <row> is refused on a non-place step")
        d.send(b"c")
        d.expect(r"\(c works on place steps only\)", "c is refused on a non-place step")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "step 2 (place RG) WAIT")

        # (b) An illegal move is refused WITH ITS REASON and changes nothing.
        # sip2 at row 60 would put pin 2 at row 61 - the same shared predicate
        # (partGeometryOk) the parser and place_part() apply, so the guide can
        # never move a part into a shape the next load would drop.
        d.send(b"m 60\r")
        d.expect(r"move refused: sip2 at row 60 does not fit the bottom half",
                 "an off-board move is refused and names its reason")

        # (c) The move itself: pin 1 to row 44. The mode becomes CUSTOM - the
        # marker that says the user put it here deliberately - and a move never
        # silently flips compact/expanded (that is c's job).
        d.send(b"m 44\r")
        d.expect(r"GUIDE move part=RG row=44 placement=custom",
                 "m 44 moves pin 1 and marks the placement custom")
        time.sleep(1.5)   # guidePersistProgress writes the run file immediately
        _, mv_yaml = read_device_file(run_mv)
        check("row: 44" in mv_yaml, "the RUN FILE carries the moved row: 44")
        check("placement: custom" in mv_yaml,
              "the RUN FILE carries placement: custom (serializer + parser "
              "landed together, so it survives the next auto-save)")

        # (d) The snap. Compact is per-pin, so this is one line for a whole
        # geometry change: pin A's leg goes INTO row 50, pin B stays on its
        # footprint row because GND has no holes.
        d.send(b"c")
        d.expect(r"GUIDE move part=RG row=44 placement=compact",
                 "c snaps RG to compact")
        time.sleep(1.5)
        _, mv_yaml = read_device_file(run_mv)
        check("placement: compact" in mv_yaml, "the RUN FILE now says placement: compact")
        check("row: 44" in mv_yaml, "the snap kept the moved row (row: is still 44)")

        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=COMMIT", "the compact RG commits")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "step 3 WAIT")
        # THE compact assertion, taken mid-guide over the REPL (mpRemote is in
        # the inner set): the compact-eligible leg's bridge is GONE, because
        # the leg IS the connection now, while the fabric-endpoint leg still
        # has its routed bridge. One part, both behaviours.
        out = jl_exec("""
print("abridge=", 1 if is_connected(44, 50) else 0)
print("bbridge=", 1 if is_connected(45, "GND") else 0)
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("abridge") == 0,
              "COMPACT: pin A's 44->50 bridge is ABSENT (the leg sits in row 50)")
        check(vals.get("bbridge") == 1,
              "COMPACT: pin B keeps its footprint row AND its GND bridge "
              "(fabric endpoints have no holes to sit in)")

        # (e) The round trip. p un-commits, c flips back, and the bridge the
        # compact leg replaced comes back - remove -> mutate -> reapply is the
        # whole re-derivation, nothing is stored per part.
        d.send(b"p")
        d.expect(r"GUIDE step=2/3 id=place_RG state=BACK", "p un-commits the compact RG")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "back at step 2")
        d.send(b"c")
        d.expect(r"GUIDE move part=RG row=44 placement=expanded",
                 "c again returns RG to expanded")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=COMMIT", "the expanded RG commits")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "step 3 WAIT after the round trip")
        out = jl_exec("""
print("abridge=", 1 if is_connected(44, 50) else 0)
print("bbridge=", 1 if is_connected(45, "GND") else 0)
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("abridge") == 1,
              "EXPANDED again: pin A's 44->50 bridge is back")
        check(vals.get("bbridge") == 1, "EXPANDED again: pin B's GND bridge is still there")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the move/snap session")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.5)

    _, mv_yaml = read_device_file(run_mv)
    check("row: 44" in mv_yaml, "the run file kept the moved row after the round trip")
    check("placement:" not in mv_yaml,
          "expanded is the DEFAULT and is NOT emitted - a full round trip "
          "leaves no placement: line behind, so pre-wave-2 files are "
          "byte-identical after a rewrite")

    # --- 13b. The PLACED move: remove -> mutate -> reapply -------------------
    #
    # THE COVERAGE DEBT (task 6 review). Every move in phase 13 happens with
    # RG un-placed, so guideMovePart's `wasPlaced == true` branch - remove the
    # part's bridges against the geometry that APPLIED them, mutate, reapply -
    # had no witness at all, and it is the branch the whole placement layer
    # rests on. Mutating first strands every old bridge on the fabric with
    # nothing left that knows how to find it, and that failure is invisible
    # until someone looks at the crossbar.
    #
    # So: commit the part FIRST, then move it and snap it while it is live.
    # RG is a sip2 at row 45 - pin A (connect 50) at 45, pin B (connect GND)
    # at 46 - so the authored placement is 45->50 and 46->GND.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        run_pm = d.expect_runfile("new", "placed-move phase allocated its own run file")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "placed-move step 1 WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "step 2 (place RG) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=COMMIT", "RG is COMMITTED - now it is live")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "step 3 WAIT")

        out = jl_exec("""
print("a45=", 1 if is_connected(45, 50) else 0)
print("b46=", 1 if is_connected(46, "GND") else 0)
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("a45") == 1 and vals.get("b46") == 1,
              "PLACED: the authored bridges 45->50 and 46->GND are live "
              f"(got a45={vals.get('a45')}, b46={vals.get('b46')})")

        # Browse back to the place step - `<` is the wheel's serial twin, and
        # a committed step re-entered this way is exactly the browse landing
        # the annotation exists for.
        # ORDER: STEP_ENTER renders the prompt and its history annotation
        # FIRST and emits the status line last, so the expects run in that
        # order too (this driver only ever searches forward).
        d.send(b"<")
        d.expect(r"\(already committed - click re-verifies\)",
                 "a committed browse landing says so")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT",
                 "`<` browses back to the committed place step")

        # RULING (fix round 1): `s` cannot skip a step that is already built.
        # Clearing the flag while the commit's bridges are still on the fabric
        # would make the session and the hardware disagree; `p` is the gesture
        # that un-commits, and it removes the bridges too. Only reachable at
        # all because browsing can park the cursor on a committed step.
        pos_before_skip = d.pos
        d.send(b"s")
        d.expect(r"\(already committed - p removes it first\)",
                 "s is REFUSED on an already-committed step")
        check("state=" not in d.buf[pos_before_skip:d.pos],
              "the refused skip changed no state and advanced no cursor")

        # The move, WHILE PLACED. Old rows must be vacated on the fabric and
        # new rows live - that is the remove->mutate->reapply invariant.
        d.send(b"m 44\r")
        d.expect(r"GUIDE move part=RG row=44 placement=custom",
                 "m 44 moves the LIVE part")
        out = jl_exec("""
print("old_a=", 1 if is_connected(45, 50) else 0)
print("old_b=", 1 if is_connected(46, "GND") else 0)
print("new_a=", 1 if is_connected(44, 50) else 0)
print("new_b=", 1 if is_connected(45, "GND") else 0)
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("old_a") == 0,
              "PLACED MOVE: the old 45->50 bridge is GONE (remove ran against "
              "the geometry that applied it)")
        check(vals.get("old_b") == 0, "PLACED MOVE: the old 46->GND bridge is GONE")
        check(vals.get("new_a") == 1, "PLACED MOVE: the new 44->50 bridge is live")
        check(vals.get("new_b") == 1, "PLACED MOVE: the new 45->GND bridge is live")

        time.sleep(1.5)   # guidePersistProgress writes the run file immediately
        _, pm_yaml = read_device_file(run_pm)
        check("row: 44" in pm_yaml and "placement: custom" in pm_yaml,
              "PLACED MOVE: the run file agrees (row: 44 + placement: custom)")
        check("placed: true" in pm_yaml,
              "PLACED MOVE: the part is still placed: true after the move")

        # And the snap, also while placed: pin A's leg goes INTO row 50 and
        # its bridge disappears; pin B's fabric endpoint keeps both.
        d.send(b"c")
        d.expect(r"GUIDE move part=RG row=44 placement=compact",
                 "c snaps the LIVE part to compact")
        out = jl_exec("""
print("a=", 1 if is_connected(44, 50) else 0)
print("b=", 1 if is_connected(45, "GND") else 0)
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("a") == 0,
              "PLACED SNAP: compact removed pin A's bridge (the leg is the "
              "connection now)")
        check(vals.get("b") == 1, "PLACED SNAP: pin B's GND bridge survives")
        time.sleep(1.5)
        _, pm_yaml = read_device_file(run_pm)
        check("placement: compact" in pm_yaml,
              "PLACED SNAP: the run file says placement: compact")

        # --- W3-T2 needle: TWO consecutive moves, then the overlay ----------
        #
        # Kevin: ">>> dragging a part doesn't clear the LEDs behind it,
        # filling up the board". Two things are being nailed down here.
        #
        # 1. THE OVERLAY IS REBUILT WHOLE, not appended to. `_GUIDE_FP_` is a
        #    full 30x10 grid re-registered on every applied move, so after two
        #    moves in a row it must describe the CURRENT footprint and nothing
        #    else - not one cell of rows 44/45/46 or 40/41 may still be lit.
        #    This is the layer the fix's investigation cleared of suspicion,
        #    and it is the layer that would silently regress if someone made
        #    guideRenderFootprints() incremental.
        # 2. Every move PERSISTS as it happens now that the wheel slide's
        #    batching (`persist=false` + adjustDirty + guideAdjustFlush) is
        #    gone with the ADJUST mode - the run file must already say row 36
        #    without anything having "landed" it.
        #
        # WHAT THIS DOES NOT COVER, stated so nobody reads it as more than it
        # is: the trail Kevin SAW lives in the `leds` buffer, one layer below
        # the overlay - renderGraphicOverlays() paints only non-transparent
        # cells and showNets() never clears, so a cell that stops being lit
        # keeps its last colour until a LED_CLEAR request runs
        # clearLEDsExceptRails(). There is no REPL surface that reads that
        # buffer back, so the clear-first fix itself is a BENCH item
        # (checklist SS1.4). This needle guards the layer that IS readable.
        #
        # Read at the DONE view on purpose: `_GUIDE_TGT_` is removed there, so
        # `_GUIDE_FP_` is the only overlay in the JSON and its 300-cell colors
        # array cannot be pushed past jl_overlay_serialize()'s 4096-byte
        # static buffer by a second overlay. ncells= is the witness that the
        # array arrived whole.
        #
        # Geometry: RG is COMPACT, so pin A's leg sits in its `connect:` row
        # 50 whatever baseRow says, and only pin B (endpoint GND, no hole)
        # travels - baseRow + 1. Node -> overlay cell is guidePaintNode's:
        # col = ((n - 1) % 30) + 1, and a node > 30 fills overlay rows 6-10.
        # So row 36 compact must light exactly col 7 (node 37) and col 20
        # (node 50), five sub-rows each = 10 cells.
        d.send(b"m 40\r")
        d.expect(r"GUIDE move part=RG row=40 placement=compact",
                 "consecutive move 1 of 2: m 40 (compact stays compact)")
        d.send(b"m 36\r")
        d.expect(r"GUIDE move part=RG row=36 placement=compact",
                 "consecutive move 2 of 2: m 36")

        # Into the DONE view, where the target overlay is gone.
        d.send(b">")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT",
                 "browsed off the place step toward DONE")
        d.send(b">")
        d.expect(r"GUIDE done committed=2 skipped=0 unfinished=1",
                 "reached the DONE view (only _GUIDE_FP_ is registered here)")

        # The del + gc.collect() tail is not tidiness: a bare `s = ...` in the
        # REPL leaves the ~2.8 KB JSON string AND the 300-element `cells` list
        # bound in device globals for the rest of the session, and this suite
        # runs inside a longer sequence that has been seen to hit a
        # MicroPython MemoryError on a later allocation. Free them here.
        out = jl_exec("""
import gc
s = overlay_serialize()
i = s.find('_GUIDE_FP_')
if i < 0:
    print("found= 0")
else:
    print("found= 1")
    j = s.find('"colors":[', i)
    k = s.find(']', j)
    cells = s[j + 10:k].split(',')
    print("ncells=", len(cells))
    lit = []
    n = 0
    for c in cells:
        if c.replace('"', '') != '000000':
            lit.append(n)
        n = n + 1
    print("nlit=", len(lit))
    print("cols=", "-".join([str((x % 30) + 1) for x in lit]))
    print("rows=", "-".join([str((x // 30) + 1) for x in lit]))
    del cells, lit
del s
gc.collect()
""", timeout=30)
        vals = parse_kv(out)
        check(vals.get("found") == 1,
              "the _GUIDE_FP_ footprint overlay is registered after two moves")
        check(vals.get("ncells") == 300,
              "the overlay's colors array arrived WHOLE (300 cells, not "
              f"truncated) - got {vals.get('ncells')}")
        lit_cols = sorted(set(str(vals.get("cols", "")).split("-")))
        lit_rows = sorted(set(str(vals.get("rows", "")).split("-")))
        check(vals.get("nlit") == 10,
              "exactly 10 overlay cells are lit - two nodes x five sub-rows, "
              f"no drag trail (got {vals.get('nlit')})")
        check(lit_cols == ["20", "7"],
              "the lit columns are EXACTLY node 37 (col 7) and node 50 "
              f"(col 20) - rows 40/41/44/45/46 left nothing behind (got {lit_cols})")
        check(lit_rows == ["10", "6", "7", "8", "9"],
              f"and they sit in the bottom half, overlay rows 6-10 (got {lit_rows})")

        time.sleep(1.5)
        _, pm_yaml = read_device_file(run_pm)
        check("row: 36" in pm_yaml,
              "the SECOND consecutive move persisted on its own - no batching "
              "left to flush now that the wheel slide is gone")

        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the placed-move session")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 13c. TWO parts: the compact leg lands on the chip's pin row --------
    #
    # THE F1 REGRESSION (final whole-branch review). Compact puts an eligible
    # leg AT its `pin.connect`, and in any real project that row is the leg of
    # the part it is wired to - Kevin's photo is literally "resistors directly
    # between 6 and 7, and 7 to top rail" on a committed 555. The inter-part
    # collision rule had no exemption for that, so once the chip was committed
    # EVERY compact-eligible part in the shipped 555/eeprom/nand00 came back
    # `collides with U1 at row N`: the flagship gesture, dead in its flagship
    # scenario.
    #
    # Nothing caught it because every other fixture in this file has ONE part.
    # This one has two, in the natural build order (chip first), and it pins
    # both halves of the rule:
    #   - the AUTHORED share (RC's pin A declares connect: 37, and 37 is UC's
    #     pin-3 leg row) must SUCCEED, with the bridge suppressed and the leg
    #     reported in row 37 by list_parts;
    #   - a share NOBODY declared (moving RC's footprint onto UC's pin-1 row)
    #     must still be REFUSED, or the fix would have traded one bug for the
    #     silent net merge the rule exists to prevent.
    out = jl_exec(f'print("wrote=", 1 if fs_write({COMPACT_PATH!r}, {COMPACT_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the two-part compact fixture")

    d = GuideDriver()
    try:
        d.send(f"z {COMPACT_PATH} new\r\n".encode())
        guide_live = True
        run_cx = d.expect_runfile("new", "two-part compact fixture allocated its run file")
        d.expect(r"GUIDE step=1/2 id=place_UC state=WAIT", "step 1 (place the chip) WAIT")
        d.send(b"n")
        d.expect(r"GUIDE step=1/2 id=place_UC state=COMMIT",
                 "the chip COMMITS - its legs are now live on rows 35 and 37")
        d.expect(r"GUIDE step=2/2 id=place_RC state=WAIT", "step 2 (place RC) WAIT")

        out = jl_exec("""
print("ucp3=", 1 if is_connected(37, 50) else 0)
print("ucp1=", 1 if is_connected(35, "GND") else 0)
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("ucp3") == 1 and vals.get("ucp1") == 1,
              "the chip's own bridges are live (37->50, 35->GND) - so row 37 "
              "really is occupied by a placed part's leg")

        # (a) THE CONTROL, first, while RC is still expanded: row 35 is UC's
        # pin-1 leg and RC declares no connection to it. Still a collision.
        d.send(b"m 35\r")
        d.expect(r"move refused: collides with UC at row 35",
                 "an UNDECLARED share of another part's row is still refused")

        # (b) THE FIX: pin A's endpoint IS row 37, so landing there is the
        # gesture, not a collision.
        d.send(b"c")
        d.expect(r"GUIDE move part=RC row=45 placement=compact",
                 "compact SUCCEEDS onto the chip's pin row (F1: this came back "
                 "'collides with UC at row 37' before the fix)")
        d.send(b"n")
        d.expect(r"GUIDE step=2/2 id=place_RC state=COMMIT", "the compact RC commits")

        out = jl_exec("""
print("expbridge=", 1 if is_connected(45, 37) else 0)
print("railbridge=", 1 if is_connected(46, "TOP_RAIL") else 0)
print("ucp3=", 1 if is_connected(37, 50) else 0)
by = {}
for p in list_parts():
    by[p['name']] = p
rc = by.get('RC', {'pins': {}})
print("rcanode=", rc['pins']['A']['node'])
print("rcbnode=", rc['pins']['B']['node'])
""", timeout=25)
        vals = parse_kv(out)
        check(vals.get("rcanode") == 37,
              f"LEG IN THE ROW: RC pin A resolves to node 37 - the chip's own "
              f"pin row (got {vals.get('rcanode')})")
        check(vals.get("rcbnode") == 101,
              f"RC pin B resolves to the rail hole row TOP_RAIL=101 "
              f"(got {vals.get('rcbnode')})")
        check(vals.get("expbridge") == 0,
              "BRIDGE SUPPRESSED: no 45->37 routed bridge - the leg IS the "
              "connection")
        check(vals.get("railbridge") == 0,
              "BRIDGE SUPPRESSED: no 46->TOP_RAIL bridge either (both legs are "
              "compact-eligible, so this part routes nothing at all)")
        check(vals.get("ucp3") == 1,
              "the chip's 37->50 bridge is untouched by the snap")

        d.expect(r"GUIDE .* state=DONE", "both steps committed -> DONE")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the two-part session")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.5)

    _, cx_yaml = read_device_file(run_cx)
    check("placement: compact" in cx_yaml,
          "the run file persisted RC's compact placement")
    check(cx_yaml.count("placement:") == 1,
          "and only RC's - the chip stays expanded (ICs never compact)")

    # --- 14. Browse + wrap: THE WHEEL CAN NEVER EXIT THE GUIDE --------------
    #
    # Kevin: "when we spin the clickwheel around to get to the end, it should
    # just loop until we hold to exit - you'd want to see the whole thing
    # first." `>` and `<` are the wheel's serial twins and run the same
    # handler, so this is the one automated witness that browsing wraps
    # through the DONE view instead of walking out of the guide.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        d.expect_runfile("new", "browse phase allocated its own run file")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "browse phase at step 1")
        d.send(b">")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT",
                 "`>` browses forward WITHOUT skipping or committing")
        d.send(b">")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "`>` again -> step 3")
        d.send(b">")
        d.expect(r"GUIDE done committed=0 skipped=0 unfinished=3",
                 "past the last step is the DONE VIEW, and it counts honestly "
                 "(browsing committed nothing)")
        d.send(b">")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT",
                 "`>` at DONE WRAPS to step 1 - the ring closes")
        # THE assertion of this phase: no EXIT anywhere in that whole lap.
        check("state=EXIT" not in d.buf[:d.pos],
              "NOTHING the wheel did emitted EXIT - a full lap through DONE "
              "never leaves the guide")
        d.send(b"<")
        d.expect(r"GUIDE done committed=0 skipped=0 unfinished=3",
                 "`<` from step 1 wraps BACKWARD into the DONE view")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "q at DONE is what exits")
        guide_live = False
        time.sleep(0.7)
        d.pump()
    finally:
        d.close()
    # Quitting with everything unfinished is exit F, not a finished build:
    # no script offer, no "Run saved" - the launcher returns immediately.
    check("SCRIPT offer" not in d.buf,
          "quitting from DONE with unfinished steps is exit F (no script "
          "offer) - COMPLETED means nothing is left unfinished, not 'the "
          "cursor reached the end'")
    time.sleep(1.0)

    # --- 15. Skip and return ------------------------------------------------
    # A skipped step is not abandoned: the DONE view counts it, and confirm
    # there jumps back to it instead of leaving.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        d.expect_runfile("new", "skip-and-return phase allocated its own run file")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "skip phase at step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "step 2 WAIT")
        d.send(b"s")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "s skips step 2")
        d.send(b"n")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=COMMIT", "step 3 commits")
        d.expect(r"GUIDE done committed=2 skipped=1 unfinished=0",
                 "DONE reports the skip")
        d.send(b"n")
        d.expect(r"\(skipped\)", "the landing says the step was skipped")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT",
                 "confirm at DONE JUMPS to the skipped step instead of exiting")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=COMMIT", "the skipped step commits")
        d.expect(r"GUIDE done committed=3 skipped=0 unfinished=0",
                 "DONE is clean - the skip flag cleared on commit")
        d.send(b"n")
        d.expect(r"GUIDE .* state=EXIT", "confirm at a clean DONE finishes")
        guide_live = False
        d.expect(r"SCRIPT offer=none", "an all-committed build reaches the script step")
    finally:
        d.close()
    time.sleep(1.0)

    # --- 16. Persisted first-unfinished + the post-commit wrap ---------------
    #
    # THE RULE (design §3.2): browse position is EPHEMERAL, so what the run
    # file stores is the first genuinely unfinished step - never the cursor.
    # Committing OUT OF ORDER is what makes the two differ: under the old
    # `stepIdx + 1` rule this file would have said step 3 and step 2 would
    # have been silently skipped forever by every resume.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        run_pu = d.expect_runfile("new", "persistence phase allocated its own run file")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "persistence phase at step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "step 1 committed, cursor at step 2")
        d.send(b">")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "browsed PAST step 2 to step 3")
        d.send(b"n")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=COMMIT", "step 3 commits OUT OF ORDER")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT",
                 "the post-commit cursor WRAPS to step 2 - the only unfinished "
                 "step - instead of walking off the end")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit from step 2")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.5)

    _, pu_yaml = read_device_file(run_pu)
    check(guide_progress_needle(WIRING_PATH, 1, 3) in pu_yaml,
          "the run file persisted the FIRST UNFINISHED step (1 = step 2) of 3, "
          "not the cursor and not stepIdx + 1")

    d = GuideDriver()
    try:
        d.send(f"z {PROJ} load\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume file=" + re.escape(run_pu) + r" step=1",
                 "resume reads the first-unfinished index back")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT",
                 "resume lands on the step that was never built", timeout=40)
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the resumed persistence session")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # --- 17. An all-skipped build is not a built circuit ---------------------
    #
    # RULING (fix round 1). Skipping every step reaches DONE and returns
    # COMPLETED - "nothing left unfinished" is literally true - but no bridge
    # was ever placed, and running the companion script against a circuit
    # nobody assembled is worse than not running it. The run file is still
    # saved; only the script is suppressed, and NO SCRIPT lines are printed so
    # a headless driver can tell this apart from `offer=none`.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        d.expect_runfile("new", "all-skipped phase allocated its own run file")
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "all-skipped phase at step 1")
        d.send(b"s")
        d.expect(r"GUIDE step=2/3 id=place_RG state=WAIT", "skipped step 1")
        d.send(b"s")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=WAIT", "skipped step 2")
        d.send(b"s")
        d.expect(r"GUIDE done committed=0 skipped=3 unfinished=0",
                 "all three skipped: DONE has nothing unfinished AND nothing built")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "q exits the all-skipped build")
        guide_live = False
        d.expect(r"\(nothing was built - no script offer\)",
                 "COMPLETED with zero commits does not offer the companion script")
        d.expect(r"Run saved to " + RUN_BASENAME_RE,
                 "the run file is still saved and announced - only the script is "
                 "suppressed")
        time.sleep(0.7)
        d.pump()
    finally:
        d.close()
    check("SCRIPT offer" not in d.buf,
          "no SCRIPT lines at all for a circuit nobody built")
    time.sleep(1.0)

    # --- 18. Rail restore: the values, named AND actually on the pins -------
    #
    # RULING 1. Quit before power_on and the bench you had comes back. The
    # rails and DAC1 are set through the REPL with save=True so they are in
    # globalState.power, which is what the launcher captures.
    #
    # DAC1 is here on purpose: it is the one channel that proves the RESTORE
    # RAN, rather than proving the exit tail printed a string. The guide's INIT
    # drove it to 0 V, nothing else touches it (DAC0 is the probe feed and
    # re-parks itself), so 1.25 V can only be back on that pin because
    # restoreUserPower() physically put it there.
    out = jl_exec("""
dac_set(2, 3.3, True)
dac_set(3, -1.5, True)
dac_set(1, 1.25, True)
print("top=", dac_get(2))
print("bot=", dac_get(3))
print("dac1=", dac_get(1))
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("top") is not None and abs(float(vals.get("top")) - 3.3) < 0.2 and
          vals.get("bot") is not None and abs(float(vals.get("bot")) + 1.5) < 0.2,
          f"bench rails set to 3.3 / -1.5 before the launch "
          f"(top={vals.get('top')}, bot={vals.get('bot')})")
    check(vals.get("dac1") is not None and abs(float(vals.get("dac1")) - 1.25) < 0.15,
          f"DAC1 set to 1.25 before the launch (got {vals.get('dac1')})")

    d = GuideDriver()
    try:
        d.send(f"z {POWER_PATH} new\r\n".encode())
        guide_live = True
        run_rr = d.expect_runfile("new", "rail-restore phase allocated its own run file")
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT",
                 "rail-restore phase at step 1 (power_on is step 3, never reached)")
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit BEFORE power_on")
        d.expect(r"rails \+ DACs restored \(top=3\.30V bot=-1\.50V",
                 "the exit tail names the EXACT values coming back")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    # THE HARDWARE HALF. dac_get now answers channels 2/3 from railHwVolts -
    # the voltage last written to the pin, save or not - exactly as it has
    # always answered 0/1 from s_dacHwVolts. So this reads what is physically
    # on the rails, not what the run file remembers, and DAC1 is the
    # independent witness that restoreUserPower() ran at all.
    out = jl_exec("""
print("top=", dac_get(2))
print("bot=", dac_get(3))
print("dac1=", dac_get(1))
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("top") is not None and abs(float(vals.get("top")) - 3.3) < 0.2,
          f"RESTORED: the top rail is physically back at 3.3 V "
          f"(got {vals.get('top')})")
    check(vals.get("bot") is not None and abs(float(vals.get("bot")) + 1.5) < 0.2,
          f"RESTORED: the bottom rail is physically back at -1.5 V "
          f"(got {vals.get('bot')})")
    check(vals.get("dac1") is not None and abs(float(vals.get("dac1")) - 1.25) < 0.15,
          f"RESTORED: DAC1 is back at 1.25 V - INIT drove it to 0 and nothing "
          f"but the restore could have returned it (got {vals.get('dac1')})")
    # THE BINDING save=0 CHECK, and the hazard the rule exists to prevent: the
    # user's bench voltage must never end up written into someone else's
    # project file. Live rails and the file diverge ON PURPOSE here - a
    # half-built project re-opened later must still come up unpowered - and
    # this is the assertion that would catch a flip to save=1. (The 0.00 the
    # guide put in globalState lands in this file on the next idle auto-save;
    # what matters here and now is that 3.3 does not.)
    _, rr_yaml = read_device_file(run_rr)
    check("topRail: 3.3" not in rr_yaml,
          "save=0: the user's 3.3 V was NEVER written into the project's run "
          "file")

    out = jl_exec("""
dac_set(2, 0.0, True)
dac_set(3, 0.0, True)
dac_set(1, 0.0, True)
print("zeroed= 1")
""", timeout=25)
    check(parse_kv(out).get("zeroed") == 1,
          "rails and DAC1 zeroed after the restore phase")

finally:
    # --- 6. Restore the bench ----------------------------------------------
    # Unwedge a possibly-still-waiting guide FIRST (its loop owns port 1's
    # bytes; everything after this needs the main menu back). Only when we
    # know a session may be live - see guide_live above.
    if guide_live:
        try:
            ser = serial.Serial(port1_path(), 115200, timeout=0.05)
            try:
                # Raw reader, own fault witness (see GuideDriver.__init__).
                unwedge = b""
                for _ in range(3):
                    ser.write(b"q")
                    ser.flush()
                    time.sleep(0.5)
                    unwedge += ser.read(4096) or b""
                fault_scan(_csi.sub("", unwedge.decode(errors="replace")),
                           "the guide unwedge drain")
            finally:
                ser.close()
            print("  info: sent unwedge 'q' to a possibly-live guide session")
        except Exception as e:  # pragma: no cover
            print(f"  info: unwedge attempt failed: {e!r}")

    # ORDER: leave the run-file context FIRST. Every launch above made a run
    # file the ACTIVE context, so its dirty state auto-saves back to ITSELF -
    # delete it while it is still active and the next switch's dirty pre-save
    # simply re-creates it (or fails noisily against a removed directory).
    # test_parts_roundtrip documents the same hazard.
    restore_context(orig_slot, orig_path)
    time.sleep(1.5)

    # Project tree OFF the board, RUN FILES INCLUDED (test_projects' run_app
    # probe gates on the board holding only ITS two projects, and rmdir refuses
    # a directory that still holds a run file). The blanket "every .yaml"
    # sweep below is only safe because every directory in the list is one THIS
    # SUITE created - never do it to a shipped project, where <dir>_run.yaml is
    # the user's circuit.
    out = jl_exec(f"""
for p in ({WIRING_PATH!r}, {POWER_PATH!r}, {NOPOWER_PATH!r}, {CHECKS_PATH!r},
          {REFUSAL_PATH!r}, {CONT_PATH!r}, {COMPACT_PATH!r},
          {VFNR_PATH!r}, {STARVE_PATH!r}, {LEGACY_PATH!r},
          {RAIL_OK_PATH!r}, {RAIL_GND_PATH!r}, {RAIL_FLOAT_PATH!r},
          {RAIL_OSC_BAND_PATH!r}, {RAIL_OSC_NOBAND_PATH!r}):
    if fs_exists(p):
        jfs.remove(p)
runs = 0
for dd in ({PROJ_DIR!r}, {VFNR_DIR!r}, {STARVE_DIR!r}, {RAIL_DIR!r}, {LEGACY_DIR!r}):
    if not fs_exists(dd):
        continue
    try:
        names = jfs.listdir(dd)
    except Exception:
        names = []
    for nm in names:
        if nm.endswith(".yaml"):
            try:
                jfs.remove(dd + "/" + nm)
                runs += 1
            except Exception as e:
                print("rmerr=", e)
for dd in ({PROJ_DIR!r}, {VFNR_DIR!r}, {STARVE_DIR!r}, {RAIL_DIR!r}, {LEGACY_DIR!r}):
    try:
        jfs.rmdir(dd)
    except Exception:
        pass
print("runsremoved=", runs)
print("gone=", 0 if fs_exists({PROJ_DIR!r}) else 1)
print("gonevfnr=", 0 if fs_exists({VFNR_DIR!r}) else 1)
print("gonet8=", 0 if (fs_exists({STARVE_DIR!r}) or fs_exists({RAIL_DIR!r})
                       or fs_exists({LEGACY_DIR!r})) else 1)
""", timeout=30)
    vals = parse_kv(out)
    print(f"  info: removed {vals.get('runsremoved')} leftover run file(s)")
    check(vals.get("gone") == 1, f"removed {PROJ_DIR} from the board")
    check(vals.get("gonevfnr") == 1, f"removed {VFNR_DIR} from the board")
    check(vals.get("gonet8") == 1,
          "removed the task-8 fixture dirs (hilstv / hilrail / hilleg)")

    if snapshot is not None:
        check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

finish("test_guide_flow")
