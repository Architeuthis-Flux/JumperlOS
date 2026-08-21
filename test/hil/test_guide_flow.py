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
    pins: {A: {pin: 1, connect: 50}, B: {pin: 2}}
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
        d.expect(r"rails \+ DACs left at 0V",
                 "no power_on behind the resume -> exit notes rails at 0V")
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
    check("rails + DACs left at 0V" not in d.buf,
          "no 'rails at 0V' note when power was re-applied on resume")
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
        d.expect(r"rails \+ DACs left at 0V \(safe state",
                 "exit tail keeps the 'rails at 0V' note (the asymmetry fix)")
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
