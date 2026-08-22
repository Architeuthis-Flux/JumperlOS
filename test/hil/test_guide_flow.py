#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guided-placement runtime: the guide end to end, plus the task-7 checks.

Drives the headless entry (`z <project>[ new|load|run=<N>]` on port 1,
MENU_DEBUG) with a 3-step project - note + place sip2 + connect - pushed to
/projects/hilguide/, and asserts the machine-parseable
`GUIDE step=<i>/<n> id=<id> state=<STATE>` lines in order while feeding the
guide's serial keys (n n n q).

WHAT CHANGED WITH RUN FILES (design-launcher §1, task 5). There is no
destination slot any more: every launch opens or creates
/projects/hilguide/hilguide_<N>.yaml and leaves it as the PERSISTENT active
context, so this suite's workbench is no longer slot 3 - it is whatever run
file the launch just announced on its `RUNFILE path=... action=new|load` line.
Every phase therefore CAPTURES that path instead of hardcoding one, because
the run counter is monotonic and depends on how many phases ran before it.
Fresh phases say `new`; resume phases say `load` (or nothing) with the
DIRECTORY name, since a wiring path combined with load is refused - the run
file's own runSource decides the variant, so the argument would be a lie.

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
  5. start-new is the new restart, and it is NON-DESTRUCTIVE: run N+1 is a
     fresh file and run N still exists on disk WITH its guideProgress
  6. the guided-ness gate on a run with no progress (runSource names a guided
     wiring -> fresh guide), then y-resume: quit mid-guide, `load`, resume at
     the saved step with the committed bridges intact ("rails at 0V")
  7. one `p` un-commit: bridge + RG_A name removed, guideProgress decremented
  8. power fixture: power_on commit applies topRail 2.5V (default rail_sane
     check passes as norows), resume past it RE-applies power and the exit
     tail stays silent about 0V
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
 12. vf tap starvation (/projects/hilvfnr): the bench `noroute` reproduced on
     a crowded 555-shaped fabric - asserts the per-node `noroute@22` split,
     the core-0 `TAP noroute ... bounceOk=0x00` diagnostic and the tap
     counters. See the long note at that phase: its verdict assertion is
     EXPECTED TO FLIP if invest-vf-noroute.md §8 Option 1 ever lands, and the
     flip is one line - `VFNR_EXPECT_REFUSAL = False`.
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
     agrees.
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
 17. rail restore (task 7 ruling 1): rails set to known values, a guided
     launch quit BEFORE power_on, and the exit tail names exactly those values
     coming back. The run file keeps 0 V (save=0), which is asserted too - it
     is a deliberate divergence, not an oversight.

BENCH-ONLY, NOT COVERED HERE. This suite never turns the wheel and cannot
touch a hole, so the ENCODER half of task 7 has no automated witness: the
260 ms confirm pend, the double-click that enters STEP_ADJUST, the wheel
slide inside it, and every probe-pad gesture. What IS covered is everything
those gestures funnel into - `>`/`<` are the wheel's browse twins and `m`/`c`
the pads' absolute twins, all running the same handlers.

Bench convention: snapshot board state + the active CONTEXT up front, restore
both in a finally (a prior round's incident: an uncaught exception stranded
slot 3 - the try/finally here is that lesson). The slot-3 fixture is gone with
the destination slots; nothing in this file writes a numbered slot any more.
TEARDOWN ORDER IS LOAD-BEARING: leave the run-file context FIRST, then delete
the run files, or the switch's dirty pre-save re-creates what was just removed
(test_parts_roundtrip learned that one). BOTH /projects/hilguide/ and
/projects/hilvfnr/ are removed afterwards, run files included:
test_projects.py's run_app('Projects') probe gates itself on the board holding
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
if not glob.glob("/dev/cu.*JLV5port1"):
    print("SKIP: board unavailable - no /dev/cu.*JLV5port1 device present")
    raise SystemExit(0)
if not glob.glob("/dev/cu.*JLV5port5"):
    print("SKIP: board unavailable - no /dev/cu.*JLV5port5 device present "
          "(port 1 exists; is the board half-enumerated?)")
    raise SystemExit(0)

import serial  # pyserial

from jl import (jl_exec, parse_kv, port1_command, port1_path, check, finish,
                board_state_capture, board_state_restore,
                active_context, restore_context)

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

_csi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]")


def read_device_file(path):
    out = jl_exec(f"""
p = {path!r}
if fs_exists(p):
    print("EXISTS= 1")
    print("<<<FILE>>>")
    print(fs_read(p))
    print("<<<END>>>")
else:
    print("EXISTS= 0")
""", timeout=25)
    if "EXISTS= 1" not in out:
        return False, ""
    m = re.search(r"<<<FILE>>>\r?\n(.*)<<<END>>>", out, re.DOTALL)
    return True, (m.group(1) if m else "")


class GuideDriver:
    """One port-1 connection driving one guide session, with ORDERED
    status-line assertions (each expect searches only past the previous
    match, so out-of-order lines fail loudly)."""

    def __init__(self):
        self.ser = serial.Serial(port1_path(), 115200, timeout=0.05)
        self.buf = ""
        self.pos = 0
        # Prime the connection: the firmware's connection-init eats the first
        # byte(s) - same idiom as test_projects phase 6(d).
        self.ser.write(b"\r\n")
        self.ser.flush()
        quiet, overall = time.time(), time.time()
        while time.time() - overall < 4.0:
            if self.ser.read(4096):
                quiet = time.time()
            elif time.time() - quiet > 0.6:
                break
        self.ser.reset_input_buffer()

    def send(self, data):
        self.ser.write(data)
        self.ser.flush()

    def pump(self):
        chunk = self.ser.read(4096)
        if chunk:
            self.buf += _csi.sub("", chunk.decode(errors="replace"))

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
        check(run1 is not None and re.match(
                  r"^" + re.escape(PROJ_DIR) + r"/" + PROJ + r"_\d+\.yaml$", run1 or ""),
              f"the run file is named <dir>_<N>.yaml (got {run1!r})")
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
        d.expect(r"Run saved to " + PROJ + r"_\d+\.yaml \(now your active circuit\)",
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
    check(f'guideProgress: {{source: "{WIRING_PATH}", step: 3}}' in run1_yaml,
          "the RUN FILE round-trips guideProgress at step 3, bound to the "
          "CANONICAL wiring (never to itself)")
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

    # --- 5. Start-new IS the restart, and it is NON-DESTRUCTIVE -------------
    # The old destructive restart (un-place loop + guideSource wipe + re-save)
    # is deleted. Starting run N+1 leaves run N intact on disk, which is
    # strictly better than what it replaces - assert exactly that.
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} new\r\n".encode())
        guide_live = True
        run2 = d.expect_runfile("new", "start-new allocated the NEXT run file")
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

    prev_exists, prev_yaml = read_device_file(run1)
    check(prev_exists, f"NON-DESTRUCTIVE RESTART: {run1} still exists on disk")
    check(f'guideProgress: {{source: "{WIRING_PATH}", step: 3}}' in prev_yaml,
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
    check(f'guideProgress: {{source: "{WIRING_PATH}", step: 1}}' in run2_yaml,
          "guideProgress decremented to step 1 after p (in the run file)")
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
    # !!! THE VERDICT THIS PHASE ASSERTS IS EXPECTED TO FLIP. !!!
    # It encodes a real limitation, not a desired outcome. When the escalation
    # in invest-vf-noroute.md §8 Option 1 lands (the chain adds its sense legs
    # as ephemeral bridges so the FULL router plans stimulus + sense together,
    # instead of the tap builder's three tiers planning alone), this fixture
    # should ROUTE instead of refusing.
    #
    # THE FLIP IS ONE LINE: set VFNR_EXPECT_REFUSAL = False, just below.
    # Everything branch-dependent hangs off that flag, so nothing else in this
    # phase needs editing and no refusal-only assertion can be left behind to
    # fail a genuinely-fixed run. Do NOT delete the phase.
    #
    # The routed-branch lines are not a guess: they are what this same vf check
    # prints TODAY on a bare fabric, measured at val=2.83V while validating the
    # sequential-tap rewrite. So the tap machinery is known-good; only this
    # fixture's crowded fabric refuses it.
    #
    # Note what does NOT change across the flip: the check FAILS either way
    # (ok=0, on_fail=warn), because rows 22/23 are empty holes - routed, the
    # taps read the full stimulus across the gap, which lands outside the
    # 1.4-2.6 band as "no current". So the whole warn-advance tail below is
    # shared and stays put.
    #
    # Regression-protected on BOTH branches: the fixture builds the crowded
    # fabric, the vf check reaches a terminal verdict, and the warn-advance
    # tail works. Protected on the refusal branch specifically (i.e. today):
    # the per-node `noroute@<row>` split - an undifferentiated `noroute` is
    # what made three bench retries say nothing - plus the scan-core failure
    # latch surviving cross-core into a correct core-0 print and the
    # unconditional tap-counter dump. The scan core must never Serial-print,
    # so seeing those two lines at all is what proves the latch/print seam.
    # When the flip happens, that seam loses its coverage here; it would then
    # want a fixture that still starves a tap (a deliberately crowded chip).
    VFNR_EXPECT_REFUSAL = True
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
                     "vf routed and reported a measured voltage")
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

        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "quit the placed-move session")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

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
    check(f'guideProgress: {{source: "{WIRING_PATH}", step: 1}}' in pu_yaml,
          "the run file persisted the FIRST UNFINISHED step (1 = step 2), not "
          "the cursor and not stepIdx + 1")

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

    # --- 17. Rail restore: the values, named and given back -----------------
    #
    # RULING 1. Quit before power_on and the bench you had comes back. The
    # rails are set through the REPL with save=True so they are in
    # globalState.power, which is what the launcher captures.
    out = jl_exec("""
dac_set(2, 3.3, True)
dac_set(3, -1.5, True)
print("top=", dac_get(2))
print("bot=", dac_get(3))
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("top") is not None and abs(float(vals.get("top")) - 3.3) < 0.2 and
          vals.get("bot") is not None and abs(float(vals.get("bot")) + 1.5) < 0.2,
          f"bench rails set to 3.3 / -1.5 before the launch "
          f"(top={vals.get('top')}, bot={vals.get('bot')})")

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

    # The deliberate divergence (design §4.2): the restore is save=0, so the
    # RUN FILE - and globalState with it - keeps the safe 0 V that
    # guideForcePowerSafe wrote, while the hardware carries the user's bench.
    # A half-built project re-opened later must still come up unpowered. This
    # asserts the choice rather than tolerating it: flipping to save=1 would
    # write the user's rails into someone else's project file.
    out = jl_exec("""
print("top=", dac_get(2))
print("bot=", dac_get(3))
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("top") is not None and abs(float(vals.get("top"))) < 0.2 and
          vals.get("bot") is not None and abs(float(vals.get("bot"))) < 0.2,
          f"save=0: the run file's power stays at the safe 0 V "
          f"(state top={vals.get('top')}, bot={vals.get('bot')})")
    # And the hazard the save=0 rule exists to prevent, asserted directly: the
    # user's bench voltage must never end up written into someone else's
    # project file. (The 0.00 the guide put in globalState lands in this file
    # on the next idle auto-save; what matters here and now is that 3.3 does
    # not.)
    _, rr_yaml = read_device_file(run_rr)
    check("topRail: 3.3" not in rr_yaml,
          "save=0: the user's 3.3 V was NEVER written into the project's run "
          "file")

    out = jl_exec("""
dac_set(2, 0.0, True)
dac_set(3, 0.0, True)
print("zeroed= 1")
""", timeout=25)
    check(parse_kv(out).get("zeroed") == 1, "rails zeroed after the restore phase")

finally:
    # --- 6. Restore the bench ----------------------------------------------
    # Unwedge a possibly-still-waiting guide FIRST (its loop owns port 1's
    # bytes; everything after this needs the main menu back). Only when we
    # know a session may be live - see guide_live above.
    if guide_live:
        try:
            ser = serial.Serial(port1_path(), 115200, timeout=0.05)
            try:
                for _ in range(3):
                    ser.write(b"q")
                    ser.flush()
                    time.sleep(0.5)
                    ser.read(4096)
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
    # a directory that still holds a <dir>_<N>.yaml).
    out = jl_exec(f"""
for p in ({WIRING_PATH!r}, {POWER_PATH!r}, {NOPOWER_PATH!r}, {CHECKS_PATH!r},
          {REFUSAL_PATH!r}, {VFNR_PATH!r}):
    if fs_exists(p):
        jfs.remove(p)
runs = 0
for dd in ({PROJ_DIR!r}, {VFNR_DIR!r}):
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
for dd in ({PROJ_DIR!r}, {VFNR_DIR!r}):
    try:
        jfs.rmdir(dd)
    except Exception:
        pass
print("runsremoved=", runs)
print("gone=", 0 if fs_exists({PROJ_DIR!r}) else 1)
print("gonevfnr=", 0 if fs_exists({VFNR_DIR!r}) else 1)
""", timeout=30)
    vals = parse_kv(out)
    print(f"  info: removed {vals.get('runsremoved')} leftover run file(s)")
    check(vals.get("gone") == 1, f"removed {PROJ_DIR} from the board")
    check(vals.get("gonevfnr") == 1, f"removed {VFNR_DIR} from the board")

    if snapshot is not None:
        check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

finish("test_guide_flow")
