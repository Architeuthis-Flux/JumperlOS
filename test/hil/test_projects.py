#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Projects subsystem, first slice: the /projects/555/ reference project.

Provisioning (projectFiles[] + initializeProjects) and the Python
load_project() binding land in later tasks, so this test pushes
scripts/projects/555/* to the board itself and then exercises the SAME
parser path the relaxed FileManager guard uses: wiring.yaml's content
copied into /slots/slot3.yaml and loaded with '<3'.

Asserts:
  - the three project files land under /projects/555/ byte-identical
  - the wiring loads: ADC0-7 and ADC1-37 bridges exist, topRail is 5 V
  - the custom `nets:` name resolves - the net holding node 37 is "TIMING"
    (the net NUMBER is discovered, not hardcoded: it is topology-dependent)
  - parts are in the state but NOT expanded (no `placed:` in the file ->
    default false): rows 5/35/12/16 have no part bridges, while a wholesale
    toYAML rewrite still carries all six parts with `placed: false`
  - meta:/guide: are swallowed on parse and deliberately NOT round-tripped

Phase 6 (task 5) adds a second, minimal project at /projects/hiltest/ and
covers the two contracts the encoder-driven launcher rests on - load_project()
on a meta:-first wiring, and the `_jl_project` preamble prepended to the
companion script. The launcher itself is NOT invoked from here; phase 6's
comment says why, and the clickwheel flow is a bench checklist instead.

Bench convention: snapshot board state + slot3.yaml + active slot up front,
restore all three at the end. The /projects/555/ and /projects/hiltest/ files
are left in place on purpose - the clickwheel Files-browser check is a
hands-on bench item, and two projects make the picker's navigation testable.

NOTE the loaded wiring sets topRail to 5.0 V for the duration of the test;
the board_state snapshot restores the bench rail voltage afterwards.
"""

import os
import re
import time

from jl import (jl_exec, parse_kv, port1_command, port1_path, check, finish,
                board_state_capture, board_state_restore)

SLOT_PATH = "/slots/slot3.yaml"
PROJ_DIR = "/projects/555"

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_DIR = os.path.join(REPO, "scripts", "projects", "555")
PROJECT_FILES = ("wiring.yaml", "main.py", "README.md")


def local_file(name):
    with open(os.path.join(SRC_DIR, name), "r") as f:
        return f.read()


def read_device_file(path):
    """Return (exists, content) for a device file via the REPL."""
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


WIRING = local_file("wiring.yaml")

# --- 0. Snapshot the bench: board state, slot3.yaml, active slot ----------
snapshot = board_state_capture()
check(snapshot is not None, "captured pre-test board state snapshot")

q = port1_command("Q", 1.5)
m = re.search(r"ACTIVE_SLOT:(\d+)", q)
check(m is not None, "queried active slot ('Q')")
orig_slot = int(m.group(1)) if m else 0

slot3_existed, slot3_before = read_device_file(SLOT_PATH)
print(f"  info: active slot {orig_slot}, slot3.yaml existed: {slot3_existed}")

# Move off slot 3 before touching its file - the idle auto-save of the
# ACTIVE slot would clobber the fs_write.
if orig_slot == 3:
    port1_command("<2", 4.0)
    time.sleep(1.5)

# --- 1. Push the project tree to the board --------------------------------
out = jl_exec(f"""
for d in ("/projects", {PROJ_DIR!r}):
    if not fs_exists(d):
        try:
            jfs.mkdir(d)
        except Exception as e:
            print("mkdirerr=", e)
print("projdir=", 1 if fs_exists({PROJ_DIR!r}) else 0)
""", timeout=25)
check(parse_kv(out).get("projdir") == 1, f"created {PROJ_DIR} on the board")

for name in PROJECT_FILES:
    content = local_file(name)
    path = f"{PROJ_DIR}/{name}"
    out = jl_exec(f"print('wrote=', 1 if fs_write({path!r}, {content!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, f"pushed {path}")
    exists, on_device = read_device_file(path)
    # fs_read round-trips through print(), which appends a newline; compare
    # on the stripped text so the trailing-newline difference isn't noise.
    check(exists and on_device.strip() == content.strip(),
          f"{path} content matches scripts/projects/555/{name}")

# --- 2. Load the wiring through the real slot-YAML parser ------------------
# Same content, same parser, same loadSlotFromPath-equivalent path that the
# relaxed FileManager guard now reaches for /projects/*.yaml.
out = jl_exec(f"print('wrote=', 1 if fs_write({SLOT_PATH!r}, {WIRING!r}) else 0)",
              timeout=30)
check(parse_kv(out).get("wrote") == 1, "copied wiring.yaml content to /slots/slot3.yaml")

resp = port1_command("<3", 4.0)
check("SLOT_CHANGED:3" in resp, "'<3' loaded the 555 wiring (SLOT_CHANGED:3 seen)")
time.sleep(2.0)  # let the load + refresh + reconcile settle

# --- 3. Bridges, rail, and the un-expanded parts ---------------------------
out = jl_exec("""
print("adc0=", 1 if is_connected("ADC0", 7) else 0)
print("adc1=", 1 if is_connected("ADC1", 37) else 0)
# parts are NOT placed (no `placed:` key -> default false), so none of the
# expansion bridges may exist:
print("u1gnd=", 1 if is_connected(5, "GND") else 0)
print("u1vcc=", 1 if is_connected(35, "TOP_RAIL") else 0)
print("r1a=", 1 if is_connected(12, "TOP_RAIL") else 0)
print("r2b=", 1 if is_connected(16, 37) else 0)
print("c1m=", 1 if is_connected(19, "GND") else 0)
print("nbridges=", get_num_bridges())
""")
vals = parse_kv(out)
check(vals.get("adc0") == 1, "bridge ADC0 <-> 7 exists after load")
check(vals.get("adc1") == 1, "bridge ADC1 <-> 37 exists after load")
check(vals.get("u1gnd") == 0, "part not expanded: U1 GND (row 5) has no GND bridge")
check(vals.get("u1vcc") == 0, "part not expanded: U1 VCC (row 35) has no TOP_RAIL bridge")
check(vals.get("r1a") == 0, "part not expanded: R1 A (row 12) has no TOP_RAIL bridge")
check(vals.get("r2b") == 0, "part not expanded: R2 B (row 16) has no 37 bridge")
check(vals.get("c1m") == 0, "part not expanded: C1 MINUS (row 19) has no GND bridge")

# --- 4. The custom nets: name --------------------------------------------
# Net numbers are topology-dependent (1-5 are the pre-created special nets:
# GND / Top Rail / Bottom Rail / DAC0 / DAC1), so DISCOVER the net holding
# node 37 rather than hardcoding it - and print it, because wiring.yaml's
# `nets:` entry has to name that number for deserializeNets to attach.
out = jl_exec("""
for i in range(1, 40):
    nodes = get_net_nodes(i)
    if not nodes:
        continue
    print("NET|%d|%s|%s" % (i, str(get_net_name(i)), str(nodes)))
""")
nets = {}
for line in out.splitlines():
    m = re.match(r"\s*NET\|(\d+)\|(.*)\|(.*?)\s*$", line)
    if m:
        nets[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())
for num in sorted(nets):
    print(f"  info: net {num}: {nets[num][0]!r} nodes {nets[num][1]}")

net37 = None
for num, (name, nodes) in nets.items():
    toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
    if "37" in toks:
        net37 = (num, name)
check(net37 is not None, "a net containing node 37 exists")
if net37:
    check(net37[1] == "TIMING",
          f"net holding node 37 resolves to the nets: name TIMING (net {net37[0]}, "
          f"name {net37[1]!r}; wiring.yaml declares num: 7)")
# The pre-created special nets (1=GND 2=Top Rail 3=Bottom Rail 4=DAC0 5=DAC1)
# must NOT have been renamed - naming net 1 is exactly the trap the reference
# YAML's original `num: 1` fell into.
for num, expected in ((1, "GND"), (2, "Top Rail")):
    if num in nets:
        check(nets[num][0] == expected,
              f"special net {num} still named {expected!r} (got {nets[num][0]!r})")

# --- 5. Parts survive a wholesale toYAML rewrite, still unplaced -----------
out = jl_exec("print('saved=', nodes_save(3))")
check(parse_kv(out).get("saved") == 3, "nodes_save(3) rewrote the slot via toYAML")
time.sleep(1.0)

_, rewritten = read_device_file(SLOT_PATH)
check("parts:" in rewritten, "parts: section survived the wholesale rewrite")
names = re.findall(r'- name: "([A-Za-z0-9_]+)"', rewritten)
check(names == ["U1", "R1", "R2", "C1", "LED1", "R3"],
      f"all six parts round-tripped in order (got {names})")
check(rewritten.count("placed: false") == 6 and "placed: true" not in rewritten,
      "every part is still placed: false after the rewrite")
for needle in ("footprint: dip8", "row: 5", 'value: "NE555"',
               "GND: {pin: 1, connect: GND, class: gnd}",
               "TRIG: {pin: 2, connect: 37, class: signal}",
               "CTRL: {pin: 5, class: nc}",
               "VCC: {pin: 8, connect: TOP_RAIL, class: power}",
               "footprint: sip2", 'value: "10k"', 'value: "47k"',
               'value: "10uF"', 'value: "330"', "type: led",
               "A: {pin: 1, connect: TOP_RAIL, class: signal}",
               "PLUS: {pin: 1, connect: 37, class: signal}",
               "MINUS: {pin: 2, connect: GND, class: signal}",
               "K: {pin: 2, connect: GND, class: signal}"):
    check(needle in rewritten, f"rewrite kept: {needle}")
# The inline one-line pins form (R1/R2/C1/LED1/R3 in wiring.yaml) parsed:
check("B: {pin: 2, connect: 36, class: signal}" in rewritten,
      "inline `pins: {A: {...}, B: {...}}` form parsed (R1 B -> node 36)")
check('name: "TIMING"' in rewritten, "the TIMING net name survived the rewrite")
check("topRail: 5.00" in rewritten, "power: topRail: 5.0 parsed and round-tripped")
# Documented as-built contract: meta:/guide: are swallowed, never re-emitted.
check("meta:" not in rewritten and "guide:" not in rewritten,
      "meta:/guide: were contained on parse and NOT round-tripped (as designed)")
check("guideProgress:" not in rewritten,
      "no guideProgress: emitted (guideSource empty - the guide runtime sets it)")

# --- 6. Launcher slice: a second project + the two contracts it rests on ----
# The launcher itself (src/ProjectsApp.cpp, task 5) is encoder-driven and is
# deliberately NOT invoked from here:
#   - its picker loop polls the encoder and jOS.serviceInner(), never
#     mp_hal_check_interrupt(), so run_app("Projects") from this REPL would
#     block the exec until somebody physically holds the clickwheel;
#   - before running a companion script it calls
#     setGlobalStreamWithInterrupt(&Serial), i.e. it moves the MicroPython
#     stream to port 1 out from under a port-5 caller.
# Nor can "did the app register?" be observed from here: jl_run_app()
# (JumperlessMicroPythonAPI.cpp:2416) returns 1 unconditionally and runApp()
# prints "App not found" to Serial (port 1), which this channel never sees.
# So this phase covers the two things the launcher DEPENDS on, through the
# same calls it makes, and the clickwheel flow itself stays a bench checklist
# in the task-5 report:
#   a) loadSlotFromPath() on a project wiring carrying meta: - via
#      load_project(), the task-4 binding that wraps that exact call;
#   b) the `_jl_project` preamble the launcher prepends to the script before
#      executePythonFileContent() - rebuilt here in the same shape and exec'd
#      on the device (mirroring the construction, not invoking the launcher).
HIL_DIR = "/projects/hiltest"  # 7 chars, the dir-name convention
HIL_WIRING = """version: 2
sourceOfTruth: bridges
meta:
  project: hiltest
  title: "HIL Test Project"
  variant: default
  summary: "one bridge, one marker script"
  script: main.py
bridges:
  - {n1: 20, n2: 21}
"""
HIL_MAIN = """# HIL marker script - see test/hil/test_projects.py phase 6.
_jl_project = globals().get("_jl_project", {})
print("hilmark=", 1)
print("hildir=", _jl_project.get("dir", "none"))
print("hilvariant=", _jl_project.get("variant", "none"))
print("hilwiring=", _jl_project.get("wiring", "none"))
"""

out = jl_exec(f"""
if not fs_exists({HIL_DIR!r}):
    try:
        jfs.mkdir({HIL_DIR!r})
    except Exception as e:
        print("mkdirerr=", e)
print("hildirmade=", 1 if fs_exists({HIL_DIR!r}) else 0)
print("w1=", 1 if fs_write({HIL_DIR + "/wiring.yaml"!r}, {HIL_WIRING!r}) else 0)
print("w2=", 1 if fs_write({HIL_DIR + "/main.py"!r}, {HIL_MAIN!r}) else 0)
""", timeout=30)
vals = parse_kv(out)
check(vals.get("hildirmade") == 1, f"created {HIL_DIR} on the board")
check(vals.get("w1") == 1 and vals.get("w2") == 1,
      f"pushed {HIL_DIR}/wiring.yaml + main.py (the launcher's listProjects target)")

# (a) The launcher's load step: load_project() -> loadSlotFromPath(). A
# wiring.yaml whose FIRST section is meta: is exactly the case the launcher
# hands the slot parser, so this also proves meta: doesn't derail the parse.
out = jl_exec("""
print("loaded=", 1 if load_project("hiltest") else 0)
print("br=", 1 if is_connected(20, 21) else 0)
print("stale555=", 1 if is_connected("ADC1", 37) else 0)
""", timeout=25)
vals = parse_kv(out)
check(vals.get("loaded") == 1, "load_project('hiltest') resolved the name to "
                               f"{HIL_DIR}/wiring.yaml and loaded it")
check(vals.get("br") == 1, "hiltest wiring's bridge 20-21 is live (meta: parsed past)")
check(vals.get("stale555") == 0, "the previous wiring's bridges are gone (fresh state)")

# (b) The companion-script contract: read main.py off the device, prepend the
# launcher's `_jl_project = {...}` line, exec it. Same shape ProjectsApp.cpp
# builds (dir / variant / wiring), same file the launcher would have run.
preamble = ('_jl_project = {"dir": "hiltest", "variant": "default", '
            f'"wiring": "{HIL_DIR}/wiring.yaml"}}\n')
out = jl_exec(f"""
src = fs_read({HIL_DIR + "/main.py"!r})
exec({preamble!r} + src)
""", timeout=25)
vals = parse_kv(out)
check(vals.get("hilmark") == 1, "the project's main.py ran and printed its marker")
check(vals.get("hildir") == "hiltest",
      f"_jl_project['dir'] reached the script (got {vals.get('hildir')!r})")
check(vals.get("hilvariant") == "default",
      f"_jl_project['variant'] reached the script (got {vals.get('hilvariant')!r})")
check(vals.get("hilwiring") == f"{HIL_DIR}/wiring.yaml",
      f"_jl_project['wiring'] reached the script (got {vals.get('hilwiring')!r})")

# (c) The apps[] row itself. runApp() resolves by name and tells the caller
# nothing (jl_run_app returns 1 unconditionally; "App not found" goes to port
# 1), so the observable signal is TIME - and the only launcher path that ends
# without an encoder is the empty-list exit, which holds its "No projects"
# message for 1.5 s and returns. So: take every /projects/*/wiring.yaml away
# (contents held here, written straight back), let the launcher take that
# exit, and compare its duration against an unregistered app name.
#
# The "are there any projects left?" guard runs ON THE DEVICE, in the same
# snippet as the run_app call: if a wiring.yaml were still there the launcher
# would open its picker and block this exec until somebody physically holds
# the clickwheel. If the deletes fail, the run_app simply doesn't happen.
out = jl_exec("""
dirs = []
for d in jfs.listdir("/projects"):
    d = d.rstrip("/")
    if fs_exists("/projects/" + d + "/wiring.yaml"):
        dirs.append(d)
print("PROJDIRS|" + ",".join(dirs))
""", timeout=25)
m = re.search(r"PROJDIRS\|(.*)", out)
device_projects = [d for d in (m.group(1).strip().split(",") if m else []) if d]
print(f"  info: /projects dirs with a wiring.yaml: {device_projects}")

# Gate: only run the probe when the board holds exactly the two projects this
# test authored, whose contents are right here to write back. Any other
# project (task 9 adds more) means content this test can't restore.
known_wirings = {"555": WIRING, "hiltest": HIL_WIRING}
if sorted(device_projects) == sorted(known_wirings):
    out = jl_exec("""
import time
for d in jfs.listdir("/projects"):
    d = d.rstrip("/")
    p = "/projects/" + d + "/wiring.yaml"
    if fs_exists(p):
        jfs.remove(p)
left = 0
for d in jfs.listdir("/projects"):
    d = d.rstrip("/")
    if fs_exists("/projects/" + d + "/wiring.yaml"):
        left += 1
print("left=", left)
if left == 0:
    t0 = time.ticks_ms()
    run_app("NoSuchAppXYZ")
    t1 = time.ticks_ms()
    run_app("Projects")
    t2 = time.ticks_ms()
    print("missing_ms=", time.ticks_diff(t1, t0))
    print("projects_ms=", time.ticks_diff(t2, t1))
""", timeout=40)
    vals = parse_kv(out)
    check(vals.get("left") == 0, "temporarily removed every /projects/*/wiring.yaml")
    check(vals.get("missing_ms") is not None and vals.get("missing_ms") < 300,
          f"an unregistered app name returns immediately ({vals.get('missing_ms')} ms)")
    check(vals.get("projects_ms") is not None and vals.get("projects_ms") >= 1400,
          "run_app('Projects') reached the registered launcher and took its "
          f"empty-list exit ({vals.get('projects_ms')} ms, the 1.5 s message hold)")

    # Put the wiring files straight back (from this file's own copies).
    for d, content in known_wirings.items():
        path = f"/projects/{d}/wiring.yaml"
        out = jl_exec(f"print('restored=', 1 if fs_write({path!r}, {content!r}) else 0)",
                      timeout=30)
        check(parse_kv(out).get("restored") == 1, f"restored {path}")
else:
    print("  info: the board holds project dirs this test didn't author - "
          "skipped the run_app('Projects') probe (it must never leave a "
          "wiring.yaml behind for the picker to open on)")

# Put the live state back in sync with slot 3's FILE before the restore below
# rewrites it: load_project() deliberately leaves slot tracking alone
# (States.cpp:3080, the slot-clobber guard), so slot 3 is still "active" while
# holding hiltest's state - and a dirty active slot is what the idle auto-save
# writes out. switch_slot re-reads the file. (Port 5 on purpose: one fewer
# port-1 round trip when a terminal client is holding it.)
out = jl_exec("print('back=', switch_slot(3))", timeout=25)
time.sleep(1.5)

# (d) The HAPPY path: the picker actually opens, over real listed content, and
# comes back out without leaving the temp-slot latch set. 6(c) alone would
# still pass if listProjects always returned 0 (a broken path join, an inverted
# isDirectory test), so this drives the launcher WITH both projects present:
#   - port 5 calls run_app("Projects") in a worker thread;
#   - port 1 watches for the picker's `PROJECTS n=<count>` line - that count IS
#     listProjects' result, so seeing n>=2 is the happy-path assertion;
#   - port 1 then sends a byte, which the picker treats exactly like an encoder
#     hold (Menus.cpp:1992/:2646's "encoder hold, serial byte, or probe button"
#     convention), and the launcher takes its cancel-before-any-slot-call exit.
# The cancel byte is '\r' (what port1_command primes connections with, so a
# leftover copy is inert), re-sent every 0.5 s until the exec returns so a
# dropped byte can't leave the board sitting in the picker.
import threading

import serial  # pyserial

_csi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]")

q = port1_command("Q", 1.5)
m = re.search(r"ACTIVE_SLOT:(\d+)", q)
slot_before_launch = int(m.group(1)) if m else -1
check(slot_before_launch == 3, f"active slot before the launch probe is 3 (got {slot_before_launch})")

launch = {}


def _launch_projects():
    try:
        launch["out"] = jl_exec("run_app('Projects')\nprint('returned=', 1)", timeout=45)
    except SystemExit as e:                                  # jl_exec fails fast
        launch["err"] = str(e)
    except Exception as e:                                   # pragma: no cover
        launch["err"] = repr(e)


worker = None
buf = ""
n_listed = None
sends = 0

ser = serial.Serial(port1_path(), 115200, timeout=0.05)
try:
    # Prime the connection BEFORE the launcher opens (the firmware's
    # connection-init eats the first byte, and that byte would otherwise be the
    # one meant to cancel the picker).
    ser.write(b"\r\n")
    ser.flush()
    quiet, overall = time.time(), time.time()
    while time.time() - overall < 4.0:
        if ser.read(4096):
            quiet = time.time()
        elif time.time() - quiet > 0.6:
            break
    ser.reset_input_buffer()

    worker = threading.Thread(target=_launch_projects, daemon=True)
    worker.start()

    last_send = 0.0
    deadline = time.time() + 35
    while time.time() < deadline and worker.is_alive():
        chunk = ser.read(4096)
        if chunk:
            buf += _csi.sub("", chunk.decode(errors="replace"))
        if n_listed is None:
            mm = re.search(r"PROJECTS n=(\d+)", buf)
            if mm:
                n_listed = int(mm.group(1))
        # Cancel once the picker announced itself; blind-cancel after 12 s so a
        # missed line can't wedge the board in the picker either.
        if (n_listed is not None or time.time() - overall > 12) and \
                time.time() - last_send > 0.5:
            ser.write(b"\r")
            ser.flush()
            last_send = time.time()
            sends += 1
    worker.join(timeout=15)
    chunk = ser.read(4096)
    if chunk:
        buf += _csi.sub("", chunk.decode(errors="replace"))
finally:
    ser.close()

check(n_listed is not None and n_listed >= 2,
      f"the picker opened over listProjects' real output (PROJECTS n={n_listed}) "
      "- listProjects found both projects")
check("555" in buf and "hiltest" in buf,
      "the launcher listed both project dirs on the terminal")
check(worker is not None and not worker.is_alive(),
      f"run_app('Projects') returned after the serial cancel "
      f"({sends} cancel byte(s) sent)")
check("Cancelled" in buf, "the launcher took its cancel exit")
if "err" in launch:
    print(f"  info: launch worker error: {launch['err'][:400]}")
check(parse_kv(launch.get("out", "")).get("returned") == 1,
      "the REPL exec that launched the app completed normally")

# The latch witness. enterTemporarySlot(8) sets activeSlotNumber = 8 and only
# exitTemporarySlot puts it back (States.cpp:3235/:3252), so a cancel that
# wrongly entered - or entered and failed to unwind - shows up as ACTIVE_SLOT:8
# here. And the follow-up slot write proves the slot machinery still works.
q = port1_command("Q", 1.5)
m = re.search(r"ACTIVE_SLOT:(\d+)", q)
slot_after = int(m.group(1)) if m else -1
check(slot_after == 3,
      f"cancelling the picker left no temp slot behind (ACTIVE_SLOT:{slot_after}, not 8)")
out = jl_exec("print('saved=', nodes_save(3))", timeout=25)
check(parse_kv(out).get("saved") == 3,
      "a slot operation still succeeds after the cancelled launch")

# --- 7. Restore the bench --------------------------------------------------
# Restore the FILE first, switch slots second (same hazard as phase 0).
if slot3_existed:
    out = jl_exec(f"print('restored=', 1 if fs_write({SLOT_PATH!r}, {slot3_before!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("restored") == 1, "restored slot3.yaml prior content")
else:
    out = jl_exec(f"""
if fs_exists({SLOT_PATH!r}):
    jfs.remove({SLOT_PATH!r})
print("removed=", 0 if fs_exists({SLOT_PATH!r}) else 1)
""")
    check(parse_kv(out).get("removed") == 1,
          "removed the test's slot3.yaml (did not exist before)")

port1_command(f"<{orig_slot}", 4.0)
time.sleep(1.5)

if snapshot is not None:
    check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

print("  info: /projects/555/ and /projects/hiltest/ left on the board - two "
      "entries so the clickwheel bench check can exercise picker navigation")
finish("test_projects")
