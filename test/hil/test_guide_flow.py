#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guided-placement runtime: the guide end to end, plus the task-7 checks.

Drives the headless entry (`z <path> <slot>` on port 1, MENU_DEBUG) with a
3-step project - note + place sip2 + connect - pushed to /projects/hilguide/,
and asserts the machine-parseable `GUIDE step=<i>/<n> id=<id> state=<STATE>`
lines in order while feeding the guide's serial keys (n n n q).

Covers:
  1. fresh run into slot 3: WAIT/COMMIT per step in order, DONE, EXIT
  2. after the run (REPL): part bridge live, RG_A net name asserted,
     connect bridge live, guideProgress + placed: true in slot3.yaml
  3. relaunch -> resume offer (`GUIDE resume slot=3 step=3`) -> q = cancel,
     state untouched
  4. relaunch -> 'n' = restart -> progress cleared, fresh step 1, q ->
     bridges gone, placed: false, no guideProgress in slot3.yaml
  Task 7 (coverage debts + fixture-free checks):
  6. y-resume accept: quit mid-guide, relaunch, 'y' resumes at the saved
     step with the committed bridges intact (no power_on -> "rails at 0V")
  7. one `p` un-commit: bridge + RG_A name removed, guideProgress decremented
  8. power fixture: power_on commit applies topRail 2.5V (default rail_sane
     check passes as norows), y-resume past it RE-applies power and the exit
     tail stays silent about 0V
  9. no-power fixture: the powerApplied-asymmetry witness - resume past a
     power_on in a project with NO power: section keeps the "rails at 0V"
     exit note (and never claims a re-apply)
 10. voltage check: REPL drives DAC1 to 2.5V mid-guide (mpRemote is in the
     inner set), verify on the DAC1-routed row passes with the measured
     value; a band-miss fails on_fail: warn and 'n' advances past it;
     presence on a bare row (58 - ASSUMES nothing is plugged there) fails
     with val=float@58
 11. continuity refusal: a place step whose row is already routed reports
     `val=skip` + "check skipped (rows in use)" and warn-continues

Bench convention: snapshot board state + slot3.yaml + active slot up front,
restore all three in a finally (a prior round's incident: an uncaught
exception stranded slot 3 - the try/finally here is that lesson). The
/projects/hilguide/ tree is REMOVED afterwards: test_projects.py's
run_app('Projects') probe gates itself on the board holding exactly the two
projects IT authored, and a leftover hilguide would silently skip that phase.

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
                board_state_capture, board_state_restore)

SLOT = 3
SLOT_PATH = "/slots/slot3.yaml"
PROJ_DIR = "/projects/hilguide"
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
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            self.pump()
            m = rx.search(self.buf, self.pos)
            if m:
                self.pos = m.end()
                check(True, what)
                return True
        tail = self.buf[max(0, len(self.buf) - 600):]
        check(False, f"{what} (timed out; tail: {tail!r})")
        return False

    def close(self):
        try:
            self.pump()
        finally:
            self.ser.close()


# --- 0. Snapshot the bench --------------------------------------------------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

q = port1_command("Q", 1.5)
m = re.search(r"ACTIVE_SLOT:(\d+)", q)
check(m is not None, "queried active slot ('Q')")
orig_slot = int(m.group(1)) if m else 0

slot3_existed, slot3_before = read_device_file(SLOT_PATH)
print(f"  info: active slot {orig_slot}, slot3.yaml existed: {slot3_existed}")

# Move off slot 3 before touching its file (idle auto-save of the ACTIVE slot
# would clobber the fs_write / the guide's own saves are the only writes we
# want attributed to it).
if orig_slot == SLOT:
    port1_command("<2", 4.0)
    time.sleep(1.5)

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

    # --- 1b. Load-failure witness (review IMPORTANT 1): a failed load must
    # leave slot tracking where it was - a dangling activeSlotNumber would
    # let the next auto-save write partial state into the destination slot.
    # fromYAML is unconditionally tolerant (it never returns false), so the
    # only headless trigger is an open failure: the project DIRECTORY passes
    # the z command's safeFileExists guard but safeFileOpen(dir, "r") fails
    # inside loadSlotFromPath (bench-verified). No guide session can start
    # on this path, so no unwedge byte is armed.
    q = port1_command("Q", 1.5)
    m = re.search(r"ACTIVE_SLOT:(\d+)", q)
    slot_before_bad = int(m.group(1)) if m else -1
    d = GuideDriver()
    try:
        d.send(f"z {PROJ_DIR} {SLOT}\r\n".encode())
        d.expect(r"GUIDE error load failed", "unopenable wiring path fails the load")
    finally:
        d.close()
    q = port1_command("Q", 1.5)
    m = re.search(r"ACTIVE_SLOT:(\d+)", q)
    slot_after_bad = int(m.group(1)) if m else -2
    check(slot_after_bad == slot_before_bad,
          f"failed load left ACTIVE_SLOT unchanged "
          f"({slot_before_bad} -> {slot_after_bad}, not {SLOT})")

    # --- 2. Fresh guided run: z -> n n n q ---------------------------------
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} {SLOT}\r\n".encode())
        guide_live = True
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
    finally:
        d.close()
    time.sleep(1.0)   # let the final save settle

    # --- 3. State after the run (REPL + slot file) --------------------------
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

    _, slot_yaml = read_device_file(SLOT_PATH)
    check(f'guideProgress: {{source: "{WIRING_PATH}", step: 3}}' in slot_yaml,
          "slot3.yaml round-trips guideProgress at step 3")
    check("placed: true" in slot_yaml, "slot3.yaml has RG placed: true")
    check("{n1: 45, n2: 50}" in slot_yaml.replace("  ", " ") or
          re.search(r"n1:\s*45,\s*n2:\s*50", slot_yaml) is not None,
          "slot3.yaml carries the 45-50 expansion bridge")
    check(re.search(r"n1:\s*52,\s*n2:\s*53", slot_yaml) is not None,
          "slot3.yaml carries the 52-53 connect bridge")

    # --- 4. Relaunch -> resume offer -> q cancels ---------------------------
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume slot=3 step=3", "resume offer announced")
        d.expect(r"other = cancel", "resume prompt rendered")
        time.sleep(0.5)          # yesNoMenu drains line endings, then polls
        d.send(b"q")             # not y/n -> cancel
        d.expect(r"GUIDE cancelled", "q at the resume offer cancels")
        guide_live = False
    finally:
        d.close()

    out = jl_exec("print('rg=', 1 if is_connected(45, 50) else 0)", timeout=25)
    check(parse_kv(out).get("rg") == 1,
          "cancel at the resume offer left the built state untouched")

    # --- 5. Relaunch -> restart ('n') -> fresh step 1 -> q ------------------
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume slot=3 step=3", "resume offer on second relaunch")
        d.expect(r"other = cancel", "resume prompt rendered again")
        time.sleep(0.5)
        d.send(b"n")             # yesNoMenu: No = restart
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT",
                 "restart cleared progress and re-entered at step 1", timeout=40)
        d.send(b"q")
        d.expect(r"GUIDE .* state=EXIT", "q quits the restarted guide at step 1")
        guide_live = False
    finally:
        d.close()
    time.sleep(1.0)

    out = jl_exec("""
print("rg=", 1 if is_connected(45, 50) else 0)
print("conn=", 1 if is_connected(52, 53) else 0)
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("rg") == 0, "restart removed the RG placement bridge 45-50")
    check(vals.get("conn") == 0, "restart removed the connect bridge 52-53")

    _, slot_yaml = read_device_file(SLOT_PATH)
    check("guideProgress:" not in slot_yaml,
          "restart + quit at step 1 leaves no guideProgress in slot3.yaml")
    check("placed: true" not in slot_yaml,
          "no part is placed: true after the restart")

    # =====================================================================
    # Task 7 extensions. Coverage debts first (task 6 review), then the
    # power-resume pair (the powerApplied-asymmetry witness), then the
    # fixture-free electrical checks.
    # =====================================================================

    # --- 6. y-resume accept: quit mid-guide, relaunch, 'y' resumes ----------
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} {SLOT}\r\n".encode())
        guide_live = True
        # Phase 5 quit at step 1 with no commits -> no guideProgress -> no offer
        d.expect(r"GUIDE step=1/3 id=note_1 state=WAIT", "fresh step 1 (no stale resume offer)")
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
        d.send(f"z {WIRING_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume slot=3 step=2", "resume offer at the saved step 2")
        d.expect(r"other = cancel", "resume prompt rendered")
        time.sleep(0.5)
        d.send(b"y")
        d.expect(r"GUIDE step=3/3 id=connect_52 state=INIT",
                 "y-resume re-enters at step 3 (INIT)", timeout=40)
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
          "y-resume kept the committed place bridge 45-50")

    # --- 7. p un-commit: back out the place step, assert removal ------------
    d = GuideDriver()
    try:
        d.send(f"z {WIRING_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume slot=3 step=2", "resume offer again (step 2)")
        d.expect(r"other = cancel", "prompt rendered")
        time.sleep(0.5)
        d.send(b"y")
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

    _, slot_yaml = read_device_file(SLOT_PATH)
    check(f'guideProgress: {{source: "{WIRING_PATH}", step: 1}}' in slot_yaml,
          "guideProgress decremented to step 1 after p")
    check("placed: true" not in slot_yaml, "RG placed: false after the un-commit")

    # --- 8. Power fixture: power_on commit + y-resume re-applies rails ------
    out = jl_exec(f'print("wrote=", 1 if fs_write({POWER_PATH!r}, {POWER_WIRING!r}) else 0)',
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, "pushed the power fixture")

    d = GuideDriver()
    try:
        d.send(f"z {POWER_PATH} {SLOT}\r\n".encode())
        guide_live = True
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

    _, slot_yaml = read_device_file(SLOT_PATH)
    check("topRail: 2.5" in slot_yaml, "slot3.yaml persisted topRail 2.5 after power_on")

    d = GuideDriver()
    try:
        d.send(f"z {POWER_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume slot=3 step=3", "resume offer past the committed power_on")
        d.expect(r"other = cancel", "prompt rendered")
        time.sleep(0.5)
        d.send(b"y")
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
        d.send(f"z {NOPOWER_PATH} {SLOT}\r\n".encode())
        guide_live = True
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
        d.send(f"z {NOPOWER_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE resume slot=3 step=3", "resume offer past the no-power power_on")
        d.expect(r"other = cancel", "prompt rendered")
        time.sleep(0.5)
        d.send(b"y")
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
        d.send(f"z {CHECKS_PATH} {SLOT}\r\n".encode())
        guide_live = True
        d.expect(r"GUIDE step=1/4 id=note_1 state=WAIT", "checks fixture step 1")
        d.send(b"n")
        d.expect(r"GUIDE step=2/4 id=verify_45 state=WAIT", "step 2 (voltage) WAIT")
        # Drive DAC1 mid-guide over the REPL (mpRemote is CRITICAL/inner-set,
        # so it pumps while the guide blocks). Hardware-only write: the
        # guide's INIT zeroed the state and the slot must keep that.
        out = jl_exec("dac_set(DAC1, 2.5)\nprint('dacset= 1')", timeout=25)
        check(parse_kv(out).get("dacset") == 1, "REPL drove DAC1 to 2.5V mid-guide")
        # Let the background scan (if net_currents is on) converge its EMA on
        # row 45 - the check may reuse <250ms-fresh scan data, and a
        # half-converged EMA right after the DAC step would read low.
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
        d.send(f"z {REFUSAL_PATH} {SLOT}\r\n".encode())
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

    # Project tree OFF the board (test_projects' run_app probe gates on the
    # board holding only ITS two projects).
    out = jl_exec(f"""
for p in ({WIRING_PATH!r}, {POWER_PATH!r}, {NOPOWER_PATH!r}, {CHECKS_PATH!r}, {REFUSAL_PATH!r}):
    if fs_exists(p):
        jfs.remove(p)
try:
    jfs.rmdir({PROJ_DIR!r})
except Exception:
    pass
print("gone=", 0 if fs_exists({PROJ_DIR!r}) else 1)
""", timeout=25)
    check(parse_kv(out).get("gone") == 1, f"removed {PROJ_DIR} from the board")

    # slot3.yaml back to its pre-test content (file first, slot switch after -
    # the same hazard ordering test_projects documents).
    if slot3_existed:
        out = jl_exec(f"print('restored=', 1 if fs_write({SLOT_PATH!r}, {slot3_before!r}) else 0)",
                      timeout=30)
        check(parse_kv(out).get("restored") == 1, "restored slot3.yaml prior content")
    else:
        out = jl_exec(f"""
if fs_exists({SLOT_PATH!r}):
    jfs.remove({SLOT_PATH!r})
print("removed=", 0 if fs_exists({SLOT_PATH!r}) else 1)
""", timeout=25)
        check(parse_kv(out).get("removed") == 1,
              "removed the test's slot3.yaml (did not exist before)")

    port1_command(f"<{orig_slot}", 4.0)
    time.sleep(1.5)

    if snapshot is not None:
        check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

finish("test_guide_flow")
