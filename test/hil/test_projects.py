#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Projects subsystem: the /projects/555/ reference project + provisioning.

The test pushes scripts/projects/555/* to the board itself and then exercises
the SAME parser path the relaxed FileManager guard uses: wiring.yaml's content
copied into /slots/slot3.yaml and loaded with '<3'.

Asserts:
  - the three project files land under /projects/555/ byte-identical
  - the wiring loads: ADC0-37 and ADC1-7 bridges exist, topRail is 5 V
  - the custom `nets:` name resolves - the net holding node 7 is "TIMING"
    (the net NUMBER is discovered, not hardcoded: it is topology-dependent)
  - parts are in the state but NOT expanded (no `placed:` in the file ->
    default false): rows 35/5/10/13 have no part bridges, while a wholesale
    toYAML rewrite still carries all six parts with `placed: false`
  - meta:/guide: are swallowed on parse and deliberately NOT round-tripped

Phase 6 (task 5) adds a second, minimal project at /projects/hiltest/. It
opens with the two contracts the encoder-driven launcher rests on -
load_project() on a meta:-first wiring, and the `_jl_project` preamble
prepended to the companion script - and then drives the launcher ITSELF:
6(c)/6(d) call run_app("Projects") for real from two ports at once (port 5
runs it in a worker thread, port 1 watches the picker and cancels it). What
stays out of reach from here is RUNNING a project's script, because the
launcher moves the MicroPython stream to port 1 before it does. See the
block at phase 6 for the full reasoning.

Phase 6(c) (task 8) is the PROVISIONING phase: delete built-in project files
off the board, drive initializeProjects() through the launcher's self-heal
call, and prove the files come back byte-exact (on-device FNV-1a == the hash
compiled into src/snakes/projectFiles.h) while /projects/hiltest - a project
the firmware has never heard of - is left completely alone.

  CONTRACT CHANGE vs the task-5 shape of this file: 6(c) used to TIME
  run_app("Projects") taking its empty-list exit (>= 1400 ms) to prove the
  apps[] row resolves. With the self-heal un-guarded that exit is no longer
  reachable - provisioning puts 555 back before listProjects() runs, so the
  picker opens and blocks on the encoder. The apps[]-resolution witness is now
  the picker's own "PROJECTS n=" line on port 1, which is strictly stronger
  (it carries listProjects' actual count). The `run_app("NoSuchAppXYZ")`
  immediate-return control is kept.

Phase 6(e) (task 9) covers the three starter projects that shipped after the
555 - i2cscrn, nand00 and eeprom. They are PROVISIONED, so nothing is pushed
from the host: the phase asserts they arrived byte-exact, then loads each
wiring through load_project(), checks every part leg against the node
list_parts() resolves from the DIP/SIP footprint math, round-trips the
power:/config: sections, compiles the companion script on the device, and
drives `z` far enough to prove the guide: section parses.

  The real parts (an SSD1306, a 74HC00, a 24Cxx) are NOT on this bench, so
  nothing here claims anything about what the circuits DO. That is a bench
  checklist in the task-9 report. What is asserted is everything that does
  not need the parts - which is the whole file format, both generators, the
  provisioning table and the guide parse.

Forced refresh (the firmware-update path, initializeProjects(true): untouched
old defaults updated in place, a user-edited file left alone with this build's
default parked beside it as wiring_original.yaml) has no serial trigger and is
NOT covered here - it stays a bench item.

Bench convention: snapshot board state + slot3.yaml + active slot up front,
restore all three in a finally (an uncaught exception must not strand the
bench - the same lesson test_guide_flow.py's header records). The
/projects/555/ and /projects/hiltest/ files
are left in place on purpose - the clickwheel Files-browser check is a
hands-on bench item, and two projects make the picker's navigation testable.

NOTE the loaded wiring sets topRail to 5.0 V for the duration of the test;
the board_state snapshot restores the bench rail voltage afterwards.
"""

import os
import re
import threading
import time

import serial  # pyserial

from jl import (jl_exec, parse_kv, port1_command, port1_path, check, finish,
                board_state_capture, board_state_restore,
                active_context, restore_context)

SLOT_PATH = "/slots/slot3.yaml"
PROJ_DIR = "/projects/555"

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_DIR = os.path.join(REPO, "scripts", "projects", "555")
PROJECT_FILES = ("wiring.yaml", "main.py", "README.md")

# The generated header is the firmware's side of the provisioning contract:
# whatever hash is compiled in there is what the board must end up holding.
GENERATED_HEADER = os.path.join(REPO, "src", "snakes", "projectFiles.h")

_csi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[78]")


def local_file(name):
    with open(os.path.join(SRC_DIR, name), "r") as f:
        return f.read()


def fnv1a32(data: bytes) -> int:
    """Same FNV-1a the generator and the firmware's fnv1a32_file() use."""
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def embedded_hash(var_name):
    """Pull PROJECT_555_<X>_HASHES[0] out of the generated header."""
    with open(GENERATED_HEADER, "r") as f:
        text = f.read()
    m = re.search(r"const uint32_t " + re.escape(var_name) +
                  r"_HASHES\[\d+\] = \{ (0x[0-9A-Fa-f]{8})", text)
    # Normalise to the same "0x%08X" spelling the device prints, so the
    # comparison is on the VALUE and not on how the hex was capitalised.
    return ("0x%08X" % int(m.group(1), 16)) if m else None


def device_hash(path):
    """(hash_hex, length) of a device file, hashed ON the device so nothing
    the serial transport does to the bytes can hide a mismatch.

    Reads through jfs in 256-byte chunks rather than fs_read(). fs_read()
    answers out of jl_fs_read_file()'s static 4096-byte buffer (1024 on OG)
    and TRUNCATES anything longer - which would hash a prefix and call it a
    match failure with no hint why. Chunking also keeps the peak allocation
    tiny, so this works no matter how fragmented the heap has become.
    """
    out = jl_exec(f"""
p = {path!r}
if fs_exists(p):
    f = jfs.open(p, "r")
    h = 0x811C9DC5
    n = 0
    while True:
        c = f.read(256)
        if not c:
            break
        b = c.encode('utf-8') if isinstance(c, str) else c
        for x in b:
            h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
        n += len(b)
    f.close()
    print("dhash=", "0x%08X" % h)
    print("dlen=", n)
else:
    print("dhash=", "MISSING")
    print("dlen=", -1)
""", timeout=60)
    v = parse_kv(out)
    return v.get("dhash"), v.get("dlen")


def run_projects_app(blind_cancel_after=12.0, deadline_s=35):
    """Drive run_app('Projects') from port 5 while port 1 watches the picker
    and cancels it. Shared by the provisioning phase 6(c) and the happy-path
    phase 6(d) - one copy of the threading/serial dance, not two.

    Returns (buf, n_listed, sends, worker, launch): the de-ANSI'd port-1 text,
    the count from the picker's "PROJECTS n=" line (None if never seen), how
    many cancel bytes went out, the worker thread, and the launch result dict.
    """
    launch = {}

    def _launch_projects():
        try:
            launch["out"] = jl_exec("run_app('Projects')\nprint('returned=', 1)",
                                    timeout=45)
        except SystemExit as e:                                  # jl_exec fails fast
            launch["err"] = str(e)
        except Exception as e:                                   # pragma: no cover
            launch["err"] = repr(e)

    buf = ""
    n_listed = None
    sends = 0
    worker = None

    ser = serial.Serial(port1_path(), 115200, timeout=0.05)
    try:
        # Prime the connection BEFORE the launcher opens (the firmware's
        # connection-init eats the first byte, and that byte would otherwise be
        # the one meant to cancel the picker).
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
        overall = time.time()

        last_send = 0.0
        deadline = time.time() + deadline_s
        while time.time() < deadline and worker.is_alive():
            chunk = ser.read(4096)
            if chunk:
                buf += _csi.sub("", chunk.decode(errors="replace"))
            if n_listed is None:
                mm = re.search(r"PROJECTS n=(\d+)", buf)
                if mm:
                    n_listed = int(mm.group(1))
            # Cancel once the picker announced itself; blind-cancel later so a
            # missed line can't wedge the board in the picker either. The byte
            # is '\r' (what port1_command primes with, so a leftover is inert),
            # re-sent every 0.5 s until the exec returns.
            if (n_listed is not None or time.time() - overall > blind_cancel_after) \
                    and time.time() - last_send > 0.5:
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

    return buf, n_listed, sends, worker, launch


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

# Shared helper, not a local regex: 'Q' now answers ACTIVE_SLOT:-1 +
# ACTIVE_PATH:<path> when a FILE context is active, and the old
# r"ACTIVE_SLOT:(\d+)" did not match -1 - this phase aborted outright whenever
# the bench happened to be sitting on a project run file.
orig_slot, orig_path = active_context(1.5)
check(orig_slot is not None, "queried active context ('Q')")
check(orig_path is not None, "'Q' reports ACTIVE_PATH")
if orig_slot is None:
    orig_slot = 0

slot3_existed, slot3_before = read_device_file(SLOT_PATH)
print(f"  info: active slot {orig_slot}, slot3.yaml existed: {slot3_existed}")

# Everything that MUTATES the board lives inside this try; phase 7's restore
# is its finally. Without it an uncaught exception anywhere below (a NameError
# in a new phase is how this was learned) walks out with slot 3 overwritten,
# the bench rails at the project's voltages and the active slot moved - a
# stranded bench that reads as a firmware bug the next time anyone looks.
# Same shape test_guide_flow.py uses. Phase 0 stays OUTSIDE: the finally
# needs snapshot/orig_slot/slot3_before bound before it can restore anything.
#
# The slot-3 bounce below is the FIRST mutating action, so it belongs inside
# the try, not above it - it moves the active slot, and a failure between it
# and the first phase would otherwise leave the bench on someone else's slot.
try:
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
print("adc0=", 1 if is_connected("ADC0", 37) else 0)
print("adc1=", 1 if is_connected("ADC1", 7) else 0)
# parts are NOT placed (no `placed:` key -> default false), so none of the
# expansion bridges may exist:
print("u1gnd=", 1 if is_connected(35, "GND") else 0)
print("u1vcc=", 1 if is_connected(5, "TOP_RAIL") else 0)
print("r1a=", 1 if is_connected(10, "TOP_RAIL") else 0)
print("r2b=", 1 if is_connected(43, 7) else 0)
print("c1m=", 1 if is_connected(19, "GND") else 0)
print("nbridges=", get_num_bridges())
""")
    vals = parse_kv(out)
    check(vals.get("adc0") == 1, "bridge ADC0 <-> 37 exists after load")
    check(vals.get("adc1") == 1, "bridge ADC1 <-> 7 exists after load")
    check(vals.get("u1gnd") == 0, "part not expanded: U1 GND (row 35) has no GND bridge")
    check(vals.get("u1vcc") == 0, "part not expanded: U1 VCC (row 5) has no TOP_RAIL bridge")
    check(vals.get("r1a") == 0, "part not expanded: R1 A (row 10, axial2) has no TOP_RAIL bridge")
    check(vals.get("r2b") == 0, "part not expanded: R2 B (row 43, axial2 row 13 + 30) has no 7 bridge")
    check(vals.get("c1m") == 0, "part not expanded: C1 MINUS (row 19) has no GND bridge")

    # --- 4. The custom nets: name --------------------------------------------
    # Net numbers are topology-dependent (1-5 are the pre-created special nets:
    # GND / Top Rail / Bottom Rail / DAC0 / DAC1), so DISCOVER the net holding
    # node 7 (the RC timing junction - THR/TRIG - now that wave 2 re-anchored
    # U1 to the bottom half) rather than hardcoding it - and print it, because
    # wiring.yaml's `nets:` entry has to name that number for deserializeNets
    # to attach.
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

    net7 = None
    for num, (name, nodes) in nets.items():
        toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
        if "7" in toks:
            net7 = (num, name)
    check(net7 is not None, "a net containing node 7 exists")
    if net7:
        check(net7[1] == "TIMING",
              f"net holding node 7 resolves to the nets: name TIMING (net {net7[0]}, "
              f"name {net7[1]!r}; wiring.yaml declares num: 7)")
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
    for needle in ("footprint: dip8", "row: 35", 'value: "NE555"',
                   "GND: {pin: 1, connect: GND, class: gnd}",
                   "TRIG: {pin: 2, connect: 7, class: signal}",
                   "CTRL: {pin: 5, class: nc}",
                   "VCC: {pin: 8, connect: TOP_RAIL, class: power}",
                   "footprint: sip2", "footprint: axial2",
                   'value: "10k"', 'value: "47k"',
                   'value: "10uF"', 'value: "330"', "type: led",
                   "A: {pin: 1, connect: TOP_RAIL, class: signal}",
                   "PLUS: {pin: 1, connect: 7, class: signal}",
                   "MINUS: {pin: 2, connect: GND, class: signal}",
                   "K: {pin: 2, connect: GND, class: signal}"):
        check(needle in rewritten, f"rewrite kept: {needle}")
    # The inline one-line pins form (R1/R2/C1/LED1/R3 in wiring.yaml) parsed:
    check("B: {pin: 2, connect: 6, class: signal}" in rewritten,
          "inline `pins: {A: {...}, B: {...}}` form parsed (R1 B -> DIS node 6)")
    check('name: "TIMING"' in rewritten, "the TIMING net name survived the rewrite")
    check("topRail: 5.00" in rewritten, "power: topRail: 5.0 parsed and round-tripped")
    # Documented as-built contract: meta:/guide: are swallowed, never re-emitted.
    check("meta:" not in rewritten and "guide:" not in rewritten,
          "meta:/guide: were contained on parse and NOT round-tripped (as designed)")
    check("guideProgress:" not in rewritten,
          "no guideProgress: emitted (guideSource empty - the guide runtime sets it)")

    # --- 6. Launcher slice: a second project + the two contracts it rests on ----
    # The launcher (src/ProjectsApp.cpp) is encoder-driven, so this phase opens
    # with the two things it DEPENDS on, reached through the same calls it makes:
    #   a) loadSlotFromPath() on a project wiring carrying meta: - via
    #      load_project(), the task-4 binding that wraps that exact call;
    #   b) the `_jl_project` preamble the launcher prepends to the script before
    #      executePythonFileContent() - rebuilt here in the same shape and exec'd
    #      on the device (mirroring the construction, not invoking the launcher).
    #
    # The launcher ITSELF is driven too - 6(c) and 6(d) below call
    # run_app("Projects") for real, from two ports at once: port 5 runs the
    # call in a worker thread while port 1 watches the picker and cancels it
    # (run_projects_app(), top of this file). That two-port dance is what makes
    # it reachable at all: the picker loop polls the encoder and
    # jOS.serviceInner(), never mp_hal_check_interrupt(), so a single-port
    # run_app() would block the exec until somebody physically held the
    # clickwheel. What is still out of reach from here is RUNNING a project's
    # script - before executing one the launcher calls
    # setGlobalStreamWithInterrupt(&Serial), moving the MicroPython stream to
    # port 1 out from under the port-5 caller. That, and the clickwheel
    # navigation itself, stay bench-checklist items.
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
print("stale555=", 1 if is_connected("ADC1", 7) else 0)
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

    # (c) PROVISIONING (task 8). projectFiles[] + initializeProjects() install the
    # built-in projects from firmware constants; the launcher calls the unforced
    # variant as a self-heal before it lists. There is no serial command that
    # reaches initializeProjects() and no reboot idiom anywhere in this suite, so
    # the launcher IS the trigger: delete files, run the app, watch them come back.
    #
    # What each assertion is for:
    #   - two of 555's three files are deleted, main.py is left alone: the restored
    #     pair proves "create missing", the untouched one proves the per-file
    #     existence check doesn't rewrite what's already right;
    #   - the restored bytes are hashed ON the device and compared to the hash
    #     compiled into src/snakes/projectFiles.h (which is itself cross-checked
    #     against a fresh hash of the repo source) - three-way, so a stale header
    #     or a mangled write both fail loudly;
    #   - /projects/hiltest is NOT in projectFiles[]. Its wiring.yaml is deleted
    #     too and must STAY deleted, and its directory + main.py must survive:
    #     provisioning creates missing files, it never enumerates /projects and
    #     never removes a directory it doesn't know about. That is the coexistence
    #     contract for hand-made projects on a user's board.
    for name, var in (("wiring.yaml", "PROJECT_555_WIRING_YAML"),
                      ("main.py", "PROJECT_555_MAIN_PY"),
                      ("README.md", "PROJECT_555_README_MD")):
        hdr = embedded_hash(var)
        src = "0x%08X" % fnv1a32(local_file(name).encode("utf-8"))
        check(hdr == src,
              f"{var}_HASHES[0] in projectFiles.h ({hdr}) == FNV of "
              f"scripts/projects/555/{name} ({src}) - generated header is current")

    pre_main_hash, pre_main_len = device_hash(f"{PROJ_DIR}/main.py")
    print(f"  info: {PROJ_DIR}/main.py before: {pre_main_hash} ({pre_main_len} bytes)")

    out = jl_exec(f"""
for p in ({PROJ_DIR + "/wiring.yaml"!r}, {PROJ_DIR + "/README.md"!r},
          {HIL_DIR + "/wiring.yaml"!r}):
    if fs_exists(p):
        jfs.remove(p)
print("w555=", 1 if fs_exists({PROJ_DIR + "/wiring.yaml"!r}) else 0)
print("r555=", 1 if fs_exists({PROJ_DIR + "/README.md"!r}) else 0)
print("m555=", 1 if fs_exists({PROJ_DIR + "/main.py"!r}) else 0)
print("whil=", 1 if fs_exists({HIL_DIR + "/wiring.yaml"!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("w555") == 0 and vals.get("r555") == 0 and vals.get("whil") == 0,
          "deleted 555/wiring.yaml, 555/README.md and hiltest/wiring.yaml")
    check(vals.get("m555") == 1, "left 555/main.py in place (the skip-what-exists case)")

    # The trigger. With hiltest's wiring gone and 555's about to be restored, the
    # picker should open over exactly one project.
    buf, n_listed, sends, worker, launch = run_projects_app()
    check(worker is not None and not worker.is_alive(),
          f"run_app('Projects') returned after the serial cancel ({sends} byte(s))")
    check(n_listed is not None and n_listed >= 1,
          f"the launcher's self-heal ran and the picker listed a project "
          f"(PROJECTS n={n_listed}) - initializeProjects() re-created what it needed")
    check("hiltest" not in buf,
          "hiltest was NOT listed - the firmware did not re-create a project it "
          "has no table entry for")
    if "err" in launch:
        print(f"  info: launch worker error: {launch['err'][:400]}")

    for name, var in (("wiring.yaml", "PROJECT_555_WIRING_YAML"),
                      ("README.md", "PROJECT_555_README_MD")):
        dh, dl = device_hash(f"{PROJ_DIR}/{name}")
        want = embedded_hash(var)
        check(dh == want,
              f"provisioning re-created {PROJ_DIR}/{name} byte-exact "
              f"(device FNV {dh}, firmware default {want}, {dl} bytes)")

    post_main_hash, post_main_len = device_hash(f"{PROJ_DIR}/main.py")
    check(post_main_hash == pre_main_hash and post_main_len == pre_main_len,
          f"555/main.py was left untouched by the unforced pass ({post_main_hash})")

    # Coexistence: hiltest's dir and its other file survived; its wiring did not
    # come back (nothing in projectFiles[] points there).
    out = jl_exec(f"""
print("hdir=", 1 if fs_exists({HIL_DIR!r}) else 0)
print("hmain=", 1 if fs_exists({HIL_DIR + "/main.py"!r}) else 0)
print("hwiring=", 1 if fs_exists({HIL_DIR + "/wiring.yaml"!r}) else 0)
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("hdir") == 1, f"{HIL_DIR} still exists - provisioning removed no "
                                 "directory it doesn't know about")
    check(vals.get("hmain") == 1, f"{HIL_DIR}/main.py survived provisioning untouched")
    check(vals.get("hwiring") == 0,
          f"{HIL_DIR}/wiring.yaml was NOT re-created (not a projectFiles[] entry)")

    # The control that survives the contract change: an unregistered app name still
    # returns immediately, so "the launcher took time" means the row resolved.
    out = jl_exec("""
import time
t0 = time.ticks_ms()
run_app("NoSuchAppXYZ")
t1 = time.ticks_ms()
print("missing_ms=", time.ticks_diff(t1, t0))
""", timeout=25)
    check(parse_kv(out).get("missing_ms") is not None and
          parse_kv(out).get("missing_ms") < 300,
          f"an unregistered app name returns immediately ({parse_kv(out).get('missing_ms')} ms)")

    # Put hiltest's wiring back (this file's own copy) so 6(d) has two projects.
    out = jl_exec(f"print('restored=', 1 if fs_write({HIL_DIR + '/wiring.yaml'!r}, {HIL_WIRING!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("restored") == 1, f"restored {HIL_DIR}/wiring.yaml")

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
    # dropped byte can't leave the board sitting in the picker. All of that lives
    # in run_projects_app() at the top of this file - 6(c) drives it too.
    q = port1_command("Q", 1.5)
    m = re.search(r"ACTIVE_SLOT:(\d+)", q)
    slot_before_launch = int(m.group(1)) if m else -1
    check(slot_before_launch == 3, f"active slot before the launch probe is 3 (got {slot_before_launch})")

    buf, n_listed, sends, worker, launch = run_projects_app()

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

    # --- 6(e). The task-9 starter projects -------------------------------------
    # i2cscrn / nand00 / eeprom are PROVISIONED projects, so unlike 555 above
    # nothing is pushed from the host here: the firmware's own projectFiles[]
    # put them on the board, and the first assertion in each block is that they
    # really did arrive byte-exact. Everything after that reads the DEVICE's
    # copy, which is the copy a user would run.
    #
    # Per project:
    #   (i)   repo FNV == projectFiles.h HASHES[0]  - the generated header is current
    #   (ii)  device FNV == HASHES[0]               - provisioning landed this build
    #   (iii) load_project() + list_parts()         - the wiring PARSES, and every
    #         leg resolves to the node the DIP/SIP math says it should. This is the
    #         assertion that catches a footprint or a row typo: list_parts()
    #         reports partPinNode()'s own answer, not the file's text.
    #   (iv)  the rails the wiring asks for actually land (3.3 V, measured)
    #   (v)   compile() of the companion script, ON the device, from the device's
    #         file. Never exec: all three scripts block on input().
    #   (vi)  the guide: section parses - `z` far enough to see `GUIDE step=1/<n>`,
    #         then quit. Nothing is placed, so no part bridges are created.
    #
    # The real parts (an SSD1306, a 74HC00, a 24Cxx) are NOT on this bench. What
    # the hardware does with them is a bench checklist in the task-9 report; what
    # is asserted here is everything that does not need them.


    class GuideDriver:
        """One port-1 connection driving one guide session, with ORDERED
    status-line assertions. A local copy of test_guide_flow.py's driver -
    jl.py is shared, this file is not, and duplicating the small serial
    helpers is already this suite's convention (read_device_file above)."""

        def __init__(self):
            self.ser = serial.Serial(port1_path(), 115200, timeout=0.05)
            self.buf = ""
            self.pos = 0
            # Prime the connection: the firmware's connection-init eats the first
            # byte(s) - the same idiom phase 6(d) uses.
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

        def expect(self, pattern, what, timeout=25):
            deadline = time.time() + timeout
            rx = re.compile(pattern)
            while time.time() < deadline:
                chunk = self.ser.read(4096)
                if chunk:
                    self.buf += _csi.sub("", chunk.decode(errors="replace"))
                m = rx.search(self.buf, self.pos)
                if m:
                    self.pos = m.end()
                    check(True, what)
                    return m
            tail = self.buf[max(0, len(self.buf) - 600):]
            check(False, f"{what} (timed out; tail: {tail!r})")
            return None

        def close(self):
            try:
                chunk = self.ser.read(4096)
                if chunk:
                    self.buf += _csi.sub("", chunk.decode(errors="replace"))
            finally:
                self.ser.close()


    # (part name, footprint, base row, [(pin name, expected node, expected connect)])
    # connect -1 = "the leg just occupies the hole". Node numbers are the ones
    # JumperlessDefines.h assigns: GND 100, TOP_RAIL 101, RP_GPIO_1..3 131/132/133,
    # RP_GPIO_7 137 (RP pin 26, SDA), RP_GPIO_8 138 (RP pin 27, SCL).
    TASK9_PROJECTS = (
        ("i2cscrn", 5, (
            ("DISP", "sip4", 5, (("GND", 5, 100), ("VCC", 6, 101),
                                 ("SCL", 7, 138), ("SDA", 8, 137))),
        )),
        ("nand00", 7, (
            ("U1", "dip14", 35, (("A1", 35, 131), ("B1", 36, 132), ("Y1", 37, 133),
                                 ("A2", 38, 100), ("GND", 41, 100), ("Y3", 11, -1),
                                 ("B4", 6, 100), ("VCC", 5, 101))),
            ("LED1", "sip2", 18, (("A", 18, -1), ("K", 19, 100))),
            ("R1", "axial2", 15, (("A", 15, 37), ("B", 45, 18))),
        )),
        ("eeprom", 7, (
            ("U1", "dip8", 35, (("A0", 35, 100), ("A2", 37, 100), ("GND", 38, 100),
                                ("SDA", 8, 137), ("SCL", 7, 138),
                                ("WP", 6, 101), ("VCC", 5, 101))),
            ("R1", "axial2", 12, (("A", 12, 101), ("B", 42, 8))),
            ("R2", "axial2", 15, (("A", 15, 101), ("B", 45, 7))),
        )),
    )

    # Host-side grammar check, first and unconditionally: py_compile costs nothing
    # and never depends on the board's heap, so a syntax error is caught here even
    # on a run where the device-side compile below has to bow out for memory.
    for _p in ("555",) + tuple(p[0] for p in TASK9_PROJECTS):
        _path = os.path.join(REPO, "scripts", "projects", _p, "main.py")
        try:
            compile(open(_path).read(), _path, "exec")
            check(True, f"scripts/projects/{_p}/main.py parses (host py_compile)")
        except SyntaxError as e:
            check(False, f"scripts/projects/{_p}/main.py: {e}")

    # The "a fresh load leaves the breadboard unconnected" assertion below
    # counts bridges touching rows 1-60. Infrastructure lives off-board on a
    # DEFAULT config, but not on every config: with top_oled.lock_connection
    # = 1 the oled_i2c infra path bridges sda_row<->gpio_sda and
    # scl_row<->gpio_scl (InfraPaths.cpp), and those rows are config values.
    # They default to NANO_D2/NANO_D3 = 72/73 (off the breadboard), but a
    # bench running the OLED on breadboard holes puts them in 1-60, where
    # they would false-fail a check that has nothing to do with the OLED.
    # So read the live config once and exclude exactly those two rows.
    # Streamed in 512-byte chunks and parsed on the DEVICE (test_config.py's
    # jfs.read idiom): /config.txt is several KB and this must not depend on a
    # contiguous MicroPython allocation deep into a long session. The last
    # chunk gets a synthetic "\n" so a file with no trailing newline still
    # flushes its final line through the same splitter.
    _cfg = jl_exec("""
f = jfs.open("/config.txt", "r")
sec = ""
buf = ""
done = False
while not done:
    chunk = jfs.read(f, 512)
    if not chunk:
        buf += "\\n"
        done = True
    else:
        buf += chunk
    while "\\n" in buf:
        ln, buf = buf.split("\\n", 1)
        s = ln.strip()
        if s.startswith("["):
            sec = s
        elif sec == "[top_oled]" and "=" in s:
            k, v = s.split("=", 1)
            k = k.strip()
            if k in ("lock_connection", "sda_row", "scl_row"):
                print("OLED|%s|%s" % (k, v.strip().rstrip(";").strip()))
jfs.close(f)
""", timeout=30)
    _oled = {}
    for _line in _cfg.splitlines():
        _m = re.match(r"\s*OLED\|(\w+)\|(-?\d+)\s*$", _line)
        if _m:
            _oled[_m.group(1)] = int(_m.group(2))
    if _oled.get("lock_connection") == 1:
        INFRA_ROWS = tuple(sorted({r for r in (_oled.get("sda_row"), _oled.get("scl_row"))
                                   if r is not None and 1 <= r <= 60}))
    else:
        INFRA_ROWS = ()
    if INFRA_ROWS:
        print(f"  info: top_oled.lock_connection=1 with breadboard rows "
              f"{INFRA_ROWS} - excluding the oled_i2c infra bridges from the "
              f"row-bridge count")
    else:
        print(f"  info: no OLED infra bridges expected on rows 1-60 "
              f"(lock_connection={_oled.get('lock_connection')}, "
              f"sda_row={_oled.get('sda_row')}, scl_row={_oled.get('scl_row')})")

    for proj, want_steps, want_parts in TASK9_PROJECTS:
        print(f"  --- 6(e) {proj} ---")
        pdir = f"/projects/{proj}"
        src_dir = os.path.join(REPO, "scripts", "projects", proj)

        # (i) + (ii) the three-way provisioning check, exactly phase 6(c)'s shape.
        for fname, var in ((f"{proj}/wiring.yaml", f"PROJECT_{proj.upper()}_WIRING_YAML"),
                           (f"{proj}/main.py", f"PROJECT_{proj.upper()}_MAIN_PY"),
                           (f"{proj}/README.md", f"PROJECT_{proj.upper()}_README_MD")):
            base = fname.split("/")[1]
            with open(os.path.join(src_dir, base), "r") as f:
                repo_hash = "0x%08X" % fnv1a32(f.read().encode("utf-8"))
            hdr = embedded_hash(var)
            check(hdr == repo_hash,
                  f"{var}_HASHES[0] ({hdr}) == FNV of scripts/projects/{fname} "
                  f"({repo_hash}) - generated header is current")
            dh, dl = device_hash(f"{pdir}/{base}")
            check(dh == hdr,
                  f"{pdir}/{base} on the board matches the firmware default "
                  f"(device FNV {dh}, {dl} bytes) - provisioning installed it")

        # (iii) the wiring parses, and every leg lands where the footprint math says
        out = jl_exec(f"""
print("loaded=", 1 if load_project({proj!r}) else 0)
ps = list_parts()
print("nparts=", len(ps))
for p in ps:
    print("PART|%s|%s|%d|%d" % (p['name'], p['footprint'], p['row'],
                                1 if p['placed'] else 0))
    for k in p['pins']:
        print("PIN|%s|%s|%d|%d" % (p['name'], k, p['pins'][k]['node'],
                                   p['pins'][k]['connect']))
n = get_num_bridges()
print("nbridges=", n)
rowb = 0
infra_rows = {INFRA_ROWS!r}
for i in range(n):
    b = get_bridge(i)
    if (1 <= b[0] <= 60) or (1 <= b[1] <= 60):
        print("ROWBRIDGE|%s" % str(b))
        if b[0] in infra_rows or b[1] in infra_rows:
            continue
        rowb += 1
print("rowbridges=", rowb)
""", timeout=40)
        vals = parse_kv(out)
        check(vals.get("loaded") == 1, f"load_project({proj!r}) loaded {pdir}/wiring.yaml")
        check(vals.get("nparts") == len(want_parts),
              f"{proj}: {vals.get('nparts')} parts parsed (expected {len(want_parts)})")
        # Counted as "bridges touching a breadboard row (1-60)", NOT as
        # get_num_bridges() == 0: the board carries INFRASTRUCTURE bridges that
        # have nothing to do with any project - the probe-power feed
        # (139, 106) = ROUTABLE_BUFFER_IN <-> DAC0 that InfraPaths owns is one,
        # and asserting a bare zero fails on a healthy board (bench-caught:
        # got 1). Most infra lives off the breadboard, but NOT all of it and
        # NOT on every config: infra_rows (computed before this loop) carries
        # the oled_i2c rows when top_oled.lock_connection puts the OLED on real
        # breadboard holes, and those are excluded here. Anything else touching
        # rows 1-60 is a real failure: these three projects carry their whole
        # circuit in parts: with no bridges: section, so every connection must
        # wait on a guide commit to set placed: true (expandPartsToBridges
        # skips the rest). That is the whole reason the READMEs send people
        # through the guided build instead of a bare Files-browser load, so it
        # gets asserted as a count and not just as the per-leg spot checks
        # below.
        check(vals.get("rowbridges") == 0,
              f"{proj}: a fresh load leaves the breadboard unconnected - 0 "
              f"non-infra bridges touch rows 1-60 (total bridges "
              f"{vals.get('nbridges')}; excluded infra rows {INFRA_ROWS})")

        got_parts, got_pins = {}, {}
        for line in out.splitlines():
            m = re.match(r"\s*PART\|([^|]+)\|([^|]+)\|(\d+)\|(\d+)\s*$", line)
            if m:
                got_parts[m.group(1)] = (m.group(2), int(m.group(3)), int(m.group(4)))
            m = re.match(r"\s*PIN\|([^|]+)\|([^|]+)\|(-?\d+)\|(-?\d+)\s*$", line)
            if m:
                got_pins[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))

        for pname, fp, row, pins in want_parts:
            got = got_parts.get(pname)
            check(got == (fp, row, 0),
                  f"{proj}.{pname}: footprint {fp} at row {row}, placed: false "
                  f"(got {got})")
            for pin, node, conn in pins:
                check(got_pins.get((pname, pin)) == (node, conn),
                      f"{proj}.{pname}.{pin} -> node {node}, connect {conn} "
                      f"(got {got_pins.get((pname, pin))})")

        # Nothing is placed, so the expansion bridges must not exist yet. Pick the
        # first pin of the first part that names a real target and prove its
        # bridge is absent.
        probe = next(((p[0], q[1], q[2]) for p in want_parts for q in p[3] if q[2] >= 0))
        out = jl_exec(f"print('early=', 1 if is_connected({probe[1]}, {probe[2]}) else 0)")
        check(parse_kv(out).get("early") == 0,
              f"{proj}: part not expanded - no {probe[1]}<->{probe[2]} bridge "
              f"before any place step")

        # (iv) the wiring's power: section parsed and round-trips. All three ask
        # for 3.3 V, not the 555's 5 V, because their signal rows land on real
        # RP2350 pins. Read back through toYAML the way phase 5 does, grepping on
        # the device so the whole slot file never crosses the wire.
        out = jl_exec("""
print("saved=", nodes_save(3))
for ln in fs_read("/slots/slot3.yaml").split("\\n"):
    s = ln.strip()
    if s.startswith("topRail:") or s.startswith("pulls:"):
        print("LINE|" + s)
""", timeout=35)
        check(parse_kv(out).get("saved") == 3, f"{proj}: nodes_save(3) rewrote the slot")
        lines = [l.split("|", 1)[1].strip() for l in out.splitlines()
                 if l.strip().startswith("LINE|")]
        check(any(l.startswith("topRail: 3.30") for l in lines),
              f"{proj}: power: topRail: 3.3 parsed and round-tripped (got {lines})")
        if proj in ("i2cscrn", "eeprom"):
            # The I2C projects also carry `config: gpio: pulls:` with index 6/7 set
            # to pull-up. Without it setGPIO() re-asserts the default PULLDOWN onto
            # RP pins 26/27 on every refreshConnections() - a pulled-down I2C bus.
            check(any(re.match(r"pulls:\s*\[0,0,0,0,0,0,1,1,0,0\]", l) for l in lines),
                  f"{proj}: gpio pulls index 6/7 = pull-up survived the round trip "
                  f"(got {lines})")

        # (v) the companion script compiles ON the device, from the device's file.
        # compile(), never exec(): every one of these scripts blocks on input().
        #
        # Read through jfs.open().read(size), NOT fs_read(): jl_fs_read_file()
        # answers out of a static 4096-byte buffer (1024 on OG), so fs_read
        # silently truncates anything larger and the compile then fails on a
        # half statement. The File object's read() has no such cap.
        #
        # This does need one contiguous MicroPython string the size of the
        # script, which is the one thing the REAL run path never needs -
        # executePythonFileContent() hands mp_embed_exec_str() a C pointer and
        # mp_lexer_new_from_str_len(..., free_len=0) lexes straight out of it.
        # A fresh board has ~39 KB free and allocates 12 KB contiguously without
        # complaint; deep into a long HIL session the same heap refuses 4 KB. So
        # a MemoryError HERE is a statement about this suite's heap, not about
        # the script, and is reported as exactly that instead of as a failure.
        out = jl_exec(f"""
import gc
gc.collect()
try:
    f = jfs.open({pdir + "/main.py"!r}, "r")
    src = f.read(f.size())
    f.close()
    print("srclen=", len(src))
    compile(src, {proj + "/main.py"!r}, "exec")
    print("compiled=", 1)
except MemoryError as e:
    print("nomem=", 1)
src = None
gc.collect()
""", timeout=45)
        vals = parse_kv(out)
        if vals.get("nomem") == 1:
            print(f"  info: {pdir}/main.py device-compile skipped - this session's "
                  f"MicroPython heap is too fragmented to hold the source. The "
                  f"launcher's run path does not make this allocation; running the "
                  f"script from the picker is a bench item.")
        else:
            check(vals.get("compiled") == 1,
                  f"{pdir}/main.py compiles under this MicroPython "
                  f"({vals.get('srclen')} bytes read through jfs, uncapped)")

        # (vi) the guide: section parses. Drive `z` far enough to see the first
        # step, then quit - no part is confirmed, so nothing is placed.
        guide_live = False
        d = GuideDriver()
        try:
            d.send(f"z {pdir}/wiring.yaml 3\r\n".encode())
            guide_live = True
            m = d.expect(r"GUIDE (?:resume slot=\d+ step=\d+|step=1/(?P<n>\d+) id=)",
                         f"{proj}: guide: section parsed and the runtime started")
            if m and m.group("n") is None:
                # Stale progress for this same wiring: restart, then look again.
                d.send(b"n")
                m = d.expect(r"GUIDE step=1/(?P<n>\d+) id=",
                             f"{proj}: restarted at step 1")
            if m:
                n_steps = int(m.group("n"))
                check(n_steps == want_steps,
                      f"{proj}: guide has {n_steps} steps (expected {want_steps})")
            d.expect(r"GUIDE step=1/\d+ id=\w+ state=WAIT",
                     f"{proj}: step 1 is waiting for input")
            d.send(b"q")
            d.expect(r"GUIDE .* state=EXIT", f"{proj}: 'q' quit the guide at step 1")
            guide_live = False
        finally:
            if guide_live:
                d.send(b"q")
                time.sleep(0.5)
            d.close()

        out = jl_exec("""
ps = list_parts()
print("anyplaced=", 1 if any(p['placed'] for p in ps) else 0)
print("progress=", guide_progress())
""", timeout=30)
        vals = parse_kv(out)
        check(vals.get("anyplaced") == 0,
              f"{proj}: quitting at step 1 placed nothing")
        check(vals.get("progress") in (0, -1),
              f"{proj}: no guide progress was committed (guide_progress()="
              f"{vals.get('progress')})")

finally:
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

    # Path-aware: a file context has no "<n" to go back to.
    restore_context(orig_slot, orig_path)
    time.sleep(1.5)

    if snapshot is not None:
        check(board_state_restore(snapshot), "board state restored to pre-test snapshot")

print("  info: /projects/555/ and /projects/hiltest/ left on the board - two "
      "entries so the clickwheel bench check can exercise picker navigation")
finish("test_projects")
