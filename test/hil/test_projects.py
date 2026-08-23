#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Projects subsystem: the /projects/555/ reference project + provisioning.

The test pushes scripts/projects/555/* to the board itself and then exercises
the SAME parser path the relaxed FileManager guard uses: wiring.yaml's content
copied into /slots/slot3.yaml and loaded with '<3'.

Asserts:
  - the three project files land under /projects/555/ byte-identical
  - the wiring loads with topRail 5 V and NOTHING on the breadboard: since
    wave 3 the 555 has no `bridges:` and no `nets:` section at all (its two
    pre-wired ADC taps, ADC0-37 and ADC1-7, moved into main.py, which makes
    them at start-up and removes them on exit), so a bare load must leave
    zero non-infra bridges touching rows 1-60 and no net over any row
  - the custom `nets:` NAME path moved with them: phase 6(a)'s hiltest
    fixture declares one over its own 20-21 bridge and asserts it resolves
    (the net NUMBER is discovered, not hardcoded: it is topology-dependent)
  - parts are in the state but NOT expanded (no `placed:` in the file ->
    default false): rows 35/5/10/13 have no part bridges, while a wholesale
    toYAML rewrite still carries all six parts with `placed: false`
  - meta:/guide: are swallowed on parse and deliberately NOT round-tripped

Phase 6 (task 5) adds a second, minimal project at /projects/hiltest/. It
opens with the two contracts the encoder-driven launcher rests on -
load_project() on a meta:-first wiring, and the `_jl_project` preamble
prepended to the companion script - and then drives the launcher ITSELF:
6(c)/6(d) call run_app("Guides") for real from two ports at once (port 5
runs it in a worker thread, port 1 watches the picker and cancels it).

RUN-FILE PROTECTION IS AN INVARIANT OF THIS FILE (W3-T3). The launcher's
default is now ONE run file per project, /projects/<dir>/<dir>_run.yaml, so
the suite's fixture run file IS the user's run file - under the old numbered
scheme a suite could mint <dir>_2.yaml and delete exactly that. The rules,
enforced throughout:

  * everything that can be fixtured lives in /projects/hiltest, a directory
    this suite creates and removes;
  * every phase that touches a REAL project's run file (6(c) parks one in
    /projects/555; 6(e) drives `z <proj>/wiring.yaml new`, which OVERWRITES
    <proj>_run.yaml) is covered by a run_file_capture() taken in phase 0 and
    a run_file_restore() in the teardown - bytes back, or absence back;
  * cleanup of a real project uses purge_numbered_runs() (digits only), never
    a startswith("<dir>_") sweep, because that matches <dir>_run.yaml.

The mode itself is PROBED with jl.project_run_mode(), not assumed: the
numbered scheme still compiles behind JL_PROJECT_RUN_HISTORY, and the phases
that are specific to one scheme say which and skip with a printed reason on
the other.

Phase 6(f) (wave 2) covers RUN FILES end to end, which is what replaced the
temp-slot + keep-prompt flow. A launch now opens or creates the project's run
file and leaves it as the persistent active context.
Running a project's script IS reachable now - not through the encoder path
(the launcher moves the MicroPython stream to port 1, out from under a port-5
caller) but through the headless `z` command driven FROM port 1, which is
where that stream lands anyway. 6(f) drives that: the exit table's rows A / D
/ E / G, the monotonic no-reuse allocator, the runSource: stamp, the proof
that run files are invisible to the template write-guard AND to provisioning,
and the deterministic no-active-context terminal state.

Phase 6(c) (task 8) is the PROVISIONING phase: delete built-in project files
off the board, drive initializeProjects() through the launcher's self-heal
call, and prove the files come back byte-exact (on-device FNV-1a == the hash
compiled into src/snakes/projectFiles.h) while /projects/hiltest - a project
the firmware has never heard of - is left completely alone.

  CONTRACT CHANGE vs the task-5 shape of this file: 6(c) used to TIME
  run_app("Guides") taking its empty-list exit (>= 1400 ms) to prove the
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
                active_context, restore_context, fault_scan,
                project_run_mode, project_run_path, purge_numbered_runs,
                run_file_capture, run_file_restore, reboot_board)

SLOT_PATH = "/slots/slot3.yaml"
PROJ_DIR = "/projects/555"
# Module scope so the teardown can remove it even when the phase that
# creates it never ran. 7 chars, the dir-name convention.
HIL_DIR = "/projects/hiltest"

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
    """Drive run_app('Guides') from port 5 while port 1 watches the picker
    and cancels it. Shared by the provisioning phase 6(c) and the happy-path
    phase 6(d) - one copy of the threading/serial dance, not two.

    Returns (buf, n_listed, sends, worker, launch): the de-ANSI'd port-1 text,
    the count from the picker's "PROJECTS n=" line (None if never seen), how
    many cancel bytes went out, the worker thread, and the launch result dict.
    """
    launch = {}

    def _launch_projects():
        try:
            launch["out"] = jl_exec("run_app('Guides')\nprint('returned=', 1)",
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
        #
        # THIS IS A RAW PORT-1 READER, so it owns its own fault witness: jl.py's
        # fault_scan only sees bytes jl.py itself read, and a suite that opens
        # its own connection bypasses it entirely. The connect banner matters
        # most - the firmware prints a post-fault [crashlog] exactly once, to
        # the FIRST terminal that attaches after the reboot, so whichever
        # reader gets there first is the only one that can ever see it.
        ser.write(b"\r\n")
        ser.flush()
        quiet, overall = time.time(), time.time()
        banner = b""
        while time.time() - overall < 4.0:
            b = ser.read(4096)
            if b:
                banner += b
                quiet = time.time()
            elif time.time() - quiet > 0.6:
                break
        fault_scan(_csi.sub("", banner.decode(errors="replace")),
                   "the run_app probe's connect banner")
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
        fault_scan(buf, "the run_app probe")
    finally:
        ser.close()

    return buf, n_listed, sends, worker, launch


def read_device_file(path):
    """Return (exists, content) for a device file via the REPL.

    Reads through jfs.open().read() in chunks, NOT fs_read(): fs_read()
    silently truncates at 4095 bytes (1023 on OG) - a pre-existing
    static-buffer cap - so a comparison against a source file larger than that
    fails on the truncation rather than on the content, which is exactly how it
    was found (the 555 README grew past the cap in wave 2 and this function
    reported a mismatch for a file that had just been written correctly).
    device_hash() below already reads this way; the two now match.
    """
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


def leave_context_to_slot3():
    """Leave whatever run file is active and land on slot 3. ALWAYS do this
    before deleting a run file: a run file that is still the ACTIVE CONTEXT
    gets re-created by the next switch's dirty pre-save (test_parts_roundtrip
    documents the same hazard)."""
    jl_exec("print('back=', switch_slot(3))", timeout=25)
    time.sleep(1.2)


def purge_fixture_runs(pdir, prefix):
    """Delete EVERY <prefix>*.yaml run file - numbered and <dir>_run.yaml -
    from a project directory THIS SUITE OWNS.

    Only ever call this on /projects/hiltest. On a shipped project the same
    sweep would delete <dir>_run.yaml, which is the user's circuit; those use
    purge_numbered_runs() plus the phase-0 snapshot instead. (This is the
    ledger's concern g, and with one well-known filename it is no longer a
    theoretical one.)"""
    out = jl_exec(f"""
n = 0
if fs_exists({pdir!r}):
    for nm in jfs.listdir({pdir!r}):
        if nm.startswith({prefix!r}) and nm.endswith(".yaml"):
            try:
                jfs.remove({pdir!r} + "/" + nm)
                n += 1
            except Exception as e:
                print("rmerr=", e)
print("purged=", n)
""", timeout=30)
    return parse_kv(out).get("purged")


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

# WHICH RUN-FILE SCHEME IS FLASHED? Probed, not assumed - see the header.
RUN_MODE = project_run_mode()
print(f"  info: firmware run-file mode: {RUN_MODE} "
      f"({'<dir>_run.yaml, one per project' if RUN_MODE == 'single' else '<dir>_<N>.yaml, JL_PROJECT_RUN_HISTORY'})")

# THE RUN-FILE INVARIANT (header). Snapshot every SHIPPED project's run file
# before anything runs: 6(c) parks one in /projects/555 and 6(e) drives
# `z <proj>/wiring.yaml new`, which in single-file mode overwrites the user's
# own <proj>_run.yaml. The snapshot is a device-side copy to <path>.hilbak and
# the teardown puts the bytes - or the absence - back. Taken OUTSIDE the try
# for the same reason the others are: the finally needs it bound.
REAL_PROJECT_DIRS = (PROJ_DIR, "/projects/i2cscrn", "/projects/nand00",
                     "/projects/eeprom")
real_run_snaps = [run_file_capture(project_run_path(d)) for d in REAL_PROJECT_DIRS]
_held = [s["path"] for s in real_run_snaps if s["existed"]]
print(f"  info: run-file snapshots taken for {len(real_run_snaps)} shipped "
      f"project(s); {len(_held)} exist(s) and will be restored byte-exact"
      + (f": {_held}" if _held else ""))

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

    # ------------------------------------------------------------------
    # INFRA_ROWS - computed ONCE here, used by every "a fresh load leaves the
    # breadboard unconnected" assertion in this file (phase 3's 555 and phase
    # 6(e)'s three).
    #
    # Those assertions count bridges touching rows 1-60. Infrastructure lives off-board on a
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

    # --- 3. Bridges, rail, and the un-expanded parts ---------------------------
    out = jl_exec(f"""
print("adc0=", 1 if is_connected("ADC0", 37) else 0)
print("adc1=", 1 if is_connected("ADC1", 7) else 0)
# parts are NOT placed (no `placed:` key -> default false), so none of the
# expansion bridges may exist:
print("u1gnd=", 1 if is_connected(35, "GND") else 0)
print("u1vcc=", 1 if is_connected(5, "TOP_RAIL") else 0)
print("r1a=", 1 if is_connected(10, "TOP_RAIL") else 0)
print("r2b=", 1 if is_connected(43, 7) else 0)
print("c1m=", 1 if is_connected(19, "GND") else 0)
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
""")
    vals = parse_kv(out)
    # THE TAPS ARE GONE (wave 3, Kevin's bench note: "for some reason the 555
    # always starts with 2 ADCs wired that aren't necessary"). wiring.yaml used
    # to ship ADC0-37 and ADC1-7 in its `bridges:` section; main.py makes them
    # itself now and removes them on the way out. Asserted as ABSENT, and the
    # row-bridge count below is what makes the absence mean something: a needle
    # that only says "not connected" would keep passing if the whole load
    # silently did nothing.
    check(vals.get("adc0") == 0,
          "NO pre-wired ADC0 <-> 37 tap after load (main.py owns that tap now)")
    check(vals.get("adc1") == 0,
          "NO pre-wired ADC1 <-> 7 tap after load (main.py owns that tap now)")
    # The 555 is now in the same class as the other three shipped projects
    # (phase 6(e) asserts this shape for i2cscrn / nand00 / eeprom): its whole
    # circuit lives in parts:, it has no bridges: section at all, so a bare
    # load must leave every breadboard row untouched. Counted as "non-infra
    # bridges touching rows 1-60", not as get_num_bridges() == 0 - see the
    # INFRA_ROWS block above phase 3 for why a bare zero fails on a healthy
    # board.
    check(vals.get("rowbridges") == 0,
          f"555: a fresh load leaves the breadboard unconnected - 0 non-infra "
          f"bridges touch rows 1-60 (total bridges {vals.get('nbridges')}; "
          f"excluded infra rows {INFRA_ROWS})")
    check(vals.get("u1gnd") == 0, "part not expanded: U1 GND (row 35) has no GND bridge")
    check(vals.get("u1vcc") == 0, "part not expanded: U1 VCC (row 5) has no TOP_RAIL bridge")
    check(vals.get("r1a") == 0, "part not expanded: R1 A (row 10, axial2) has no TOP_RAIL bridge")
    check(vals.get("r2b") == 0, "part not expanded: R2 B (row 43, axial2 row 13 + 30) has no 7 bridge")
    check(vals.get("c1m") == 0, "part not expanded: C1 MINUS (row 19) has no GND bridge")

    # --- 4. The net table after a parts-only load -----------------------------
    # This phase used to assert the custom `nets:` NAME: wiring.yaml declared
    # {num: 7, name: "TIMING"} and the net holding node 7 came back named
    # TIMING. That only ever worked because the ADC1-7 tap put node 7 in a net
    # AT LOAD TIME - the `nets:` entry went out with the taps in wave 3, since
    # a name that can never attach is worse than no name.
    #
    # So the assertion flips to the other side of the same coin: a parts-only
    # load must create NO net over any breadboard row. That is the net-table
    # view of phase 3's rowbridges == 0, and unlike the old needle it cannot go
    # vacuous - a load that silently did nothing fails phase 5's part checks.
    #
    # deserializeNets' name-attach path is NOT lost: phase 6(a)'s hiltest
    # fixture - this suite's own scratch project, so no shipped file has to
    # carry a bridge for the test's benefit - declares a `nets:` name over its
    # 20-21 bridge and asserts it resolves.
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

    row_nets = []
    for num, (name, nodes) in nets.items():
        toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
        rows = [t for t in toks if t.isdigit() and 1 <= int(t) <= 60
                and int(t) not in INFRA_ROWS]
        if rows:
            row_nets.append((num, name, rows))
    check(not row_nets,
          f"a parts-only load put no breadboard row in any net "
          f"(offending nets: {row_nets})")
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
    # Was: `name: "TIMING"` survived the rewrite. wiring.yaml no longer declares
    # a nets: entry (it went out with the ADC taps - phase 4), so the needle is
    # inverted: the dropped name must not come back from anywhere.
    check('name: "TIMING"' not in rewritten,
          "the dropped TIMING net name did not reappear in the rewrite")
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
    # run_app("Guides") for real, from two ports at once: port 5 runs the
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
nets:
  - {num: 6, name: "HILNET", color: 0x00ff88, nodes: [20, 21], user: true}
"""
    HIL_MAIN = """# HIL marker script - see test/hil/test_projects.py phase 6.
_jl_project = globals().get("_jl_project", {})
print("hilmark=", 1)
print("hildir=", _jl_project.get("dir", "none"))
print("hilvariant=", _jl_project.get("variant", "none"))
print("hilwiring=", _jl_project.get("wiring", "none"))
print("hilrun=", _jl_project.get("run", "none"))
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

    # (a) The launcher's load step, through the binding that now WRAPS it.
    # load_project("<name>") no longer adopts the shipped template: under the
    # run-file model "load project hiltest" means "open its run file, or create
    # it from the wiring" - which is exactly the destruction
    # path task 4 caught (load_project("eeprom") adopting the template and the
    # idle auto-save rewriting it without guide:/meta:). The LITERAL-path form
    # is deliberately still raw, and test_slot_files phase 6b is what covers it.
    #
    # A wiring.yaml whose FIRST section is meta: is exactly the case the
    # launcher hands the slot parser, so this also proves meta: doesn't derail
    # the parse.
    # ARM the staleness needle first. This used to read
    # `is_connected("ADC1", 7)` - one of the 555's pre-wired ADC taps - and
    # assert it was gone after the hiltest load. Wave 3 removed those taps, so
    # that needle would now pass on an empty board for the wrong reason. A
    # scratch 41-42 bridge, asserted PRESENT before the load and ABSENT after,
    # cannot go vacuous.
    out = jl_exec("""
connect(41, 42)
print("armed=", 1 if is_connected(41, 42) else 0)
""", timeout=25)
    check(parse_kv(out).get("armed") == 1,
          "armed the staleness needle: a scratch 41-42 bridge on the pre-load context")

    out = jl_exec("""
print("loaded=", 1 if load_project("hiltest") else 0)
print("br=", 1 if is_connected(20, 21) else 0)
print("stale=", 1 if is_connected(41, 42) else 0)
for i in range(1, 40):
    nodes = get_net_nodes(i)
    if not nodes:
        continue
    print("NET|%d|%s|%s" % (i, str(get_net_name(i)), str(nodes)))
""", timeout=25)
    vals = parse_kv(out)
    check(vals.get("loaded") == 1, "load_project('hiltest') began a run of the project")
    check(vals.get("br") == 1, "hiltest wiring's bridge 20-21 is live (meta: parsed past)")
    check(vals.get("stale") == 0,
          "the pre-load context's 41-42 bridge is gone (fresh state, not a merge)")

    # deserializeNets' NAME-ATTACH path, moved here from phase 4 when the 555
    # stopped shipping bridges to hang a net on. The fixture declares
    # {num: 6, name: "HILNET"} and the number is topology-dependent, so
    # DISCOVER the net holding node 20 rather than trusting the declaration.
    hnets = {}
    for line in out.splitlines():
        m = re.match(r"\s*NET\|(\d+)\|(.*)\|(.*?)\s*$", line)
        if m:
            hnets[int(m.group(1))] = (m.group(2).strip(), m.group(3).strip())
    for num in sorted(hnets):
        print(f"  info: net {num}: {hnets[num][0]!r} nodes {hnets[num][1]}")
    net20 = None
    for num, (name, nodes) in hnets.items():
        toks = [t.strip() for t in nodes.replace("[", "").replace("]", "").split(",")]
        if "20" in toks:
            net20 = (num, name)
    check(net20 is not None, "a net containing node 20 exists after the hiltest load")
    if net20:
        check(net20[1] == "HILNET",
              f"the fixture's `nets:` name attached: net holding node 20 is "
              f"HILNET (net {net20[0]}, name {net20[1]!r}; the file declares num: 6)")

    lp_slot, lp_path = active_context(1.5)
    RUN_NAME_RE = (r"^" + re.escape(HIL_DIR) + r"/hiltest_run\.yaml$"
                   if RUN_MODE == "single"
                   else r"^" + re.escape(HIL_DIR) + r"/hiltest_\d+\.yaml$")
    check(lp_slot == -1 and lp_path is not None and
          re.match(RUN_NAME_RE, lp_path or ""),
          f"load_project('<name>') adopted a RUN FILE, not the template "
          f"(got {lp_slot}, {lp_path!r})")
    check(lp_path != f"{HIL_DIR}/wiring.yaml",
          "the shipped template is NOT the active context after the name form")

    # (b) The companion-script contract: read main.py off the device, prepend the
    # launcher's `_jl_project = {...}` line, exec it. Same shape ProjectsApp.cpp
    # builds (dir / variant / wiring / run), same file the launcher would run.
    # "wiring" keeps its canonical-wiring-path meaning (provenance); "run" is
    # the state file the script's own mutations get saved into.
    preamble = ('_jl_project = {"dir": "hiltest", "variant": "default", '
                f'"wiring": "{HIL_DIR}/wiring.yaml", "run": "{lp_path}"}}\n')
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
    check(vals.get("hilrun") == lp_path,
          f"_jl_project['run'] reached the script (got {vals.get('hilrun')!r})")

    # (c) PROVISIONING (task 8). projectFiles[] + initializeProjects() install the
    # built-in projects from firmware constants; the launcher calls the unforced
    # variant as a self-heal before it lists. There is no serial command that
    # reaches initializeProjects() and nothing has rebooted the board at this
    # point in the suite, so the launcher IS the trigger: delete files, run the
    # app, watch them come back. (6(e) does reset once, LATER and for a
    # different reason - see the note there; this phase's argument is already
    # spent by then.)
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

    # RUN FILES ARE INVISIBLE TO PROVISIONING (design-launcher §3). Park one
    # inside /projects/555 across the provisioning pass below and prove it comes
    # out byte-identical: initializeProjects() iterates the compiled
    # projectFiles[] canonical paths only and never enumerates /projects, and
    # the forced-refresh backup namer produces wiring_original*.yaml - a
    # disjoint namespace from BOTH run-file spellings. This asserts it over the
    # UNFORCED pass (the launcher's self-heal), which is the only one with a
    # serial trigger; the forced refresh stays a bench item, as this file's
    # header already says.
    #
    # The parked file uses the REAL run-file name for the flashed mode, not a
    # made-up 555_777.yaml: in single-file mode <dir>_run.yaml is the only name
    # the launcher will ever produce, so it is the only one worth proving
    # provisioning cannot reach. Overwriting the user's own 555_run.yaml here
    # is safe because phase 0 snapshotted it and the teardown restores it.
    RUN_555 = (project_run_path(PROJ_DIR) if RUN_MODE == "single"
               else f"{PROJ_DIR}/555_777.yaml")
    RUN_555_BODY = ("version: 2\nsourceOfTruth: bridges\n"
                    'runSource: "/projects/555/wiring.yaml"\n'
                    "bridges:\n  - {n1: 31, n2: 32}\n")
    out = jl_exec(f"print('wrote=', 1 if fs_write({RUN_555!r}, {RUN_555_BODY!r}) else 0)",
                  timeout=30)
    check(parse_kv(out).get("wrote") == 1, f"parked a run file at {RUN_555}")
    pre_run_hash, pre_run_len = device_hash(RUN_555)

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
          f"run_app('Guides') returned after the serial cancel ({sends} byte(s))")
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

    post_run_hash, post_run_len = device_hash(RUN_555)
    check(post_run_hash == pre_run_hash and post_run_len == pre_run_len,
          f"the parked RUN FILE {os.path.basename(RUN_555)} survived provisioning "
          f"byte-identical ({post_run_hash}, {post_run_len} bytes) - "
          f"projectFiles[] cannot reach a run file")

    # And it is not a variant either: listVariantFiles requires a `wiring`
    # prefix, so 555 must still show exactly one wiring - proven through the
    # picker, which is the code path that would double-list it.
    out = jl_exec(f"""
names = [n for n in jfs.listdir({PROJ_DIR!r}) if n.endswith(".yaml")]
print("yamls=", len(names))
print("YAMLS|" + ",".join(sorted(names)))
""", timeout=25)
    check(os.path.basename(RUN_555) in out and "wiring.yaml" in out,
          f"the run file ({os.path.basename(RUN_555)}) and the wiring coexist "
          f"in /projects/555")

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

    # Leave the run-file context and go back to slot 3 before phase 6(d), so
    # the exit-A assertion below has a stable, nameable context to compare
    # against. switch_slot flushes the outgoing context to its own file first
    # (task 4's Q2 fix). Port 5 on purpose: one fewer port-1 round trip when a
    # terminal client is holding it.
    out = jl_exec("print('back=', switch_slot(3))", timeout=25)
    time.sleep(1.5)

    # (d) The HAPPY path: the picker actually opens, over real listed content, and
    # comes back out without leaving the temp-slot latch set. 6(c) alone would
    # still pass if listProjects always returned 0 (a broken path join, an inverted
    # isDirectory test), so this drives the launcher WITH both projects present:
    #   - port 5 calls run_app("Guides") in a worker thread;
    #   - port 1 watches for the picker's `PROJECTS n=<count>` line - that count IS
    #     listProjects' result, so seeing n>=2 is the happy-path assertion;
    #   - port 1 then sends a byte, which the picker treats exactly like an encoder
    #     hold (Menus.cpp:1992/:2646's "encoder hold, serial byte, or probe button"
    #     convention), and the launcher takes its cancel-before-any-slot-call exit.
    # The cancel byte is '\r' (what port1_command primes connections with, so a
    # leftover copy is inert), re-sent every 0.5 s until the exec returns so a
    # dropped byte can't leave the board sitting in the picker. All of that lives
    # in run_projects_app() at the top of this file - 6(c) drives it too.
    slot_before_launch, path_before_launch = active_context(1.5)
    check(slot_before_launch == 3,
          f"active slot before the launch probe is 3 (got {slot_before_launch})")

    buf, n_listed, sends, worker, launch = run_projects_app()

    check(n_listed is not None and n_listed >= 2,
          f"the picker opened over listProjects' real output (PROJECTS n={n_listed}) "
          "- listProjects found both projects")
    check("555" in buf and "hiltest" in buf,
          "the launcher listed both project dirs on the terminal")
    check(worker is not None and not worker.is_alive(),
          f"run_app('Guides') returned after the serial cancel "
          f"({sends} cancel byte(s) sent)")
    check("Cancelled" in buf, "the launcher took its cancel exit")
    if "err" in launch:
        print(f"  info: launch worker error: {launch['err'][:400]}")
    check(parse_kv(launch.get("out", "")).get("returned") == 1,
          "the REPL exec that launched the app completed normally")

    # EXIT A, the whole row: "cancel at the project picker -> previous context,
    # untouched; on disk untouched; plain return". This used to be the TEMP-SLOT
    # LATCH witness (assert ACTIVE_SLOT is 3 "and not 8"), because the launcher
    # borrowed slot 8 and only exitTemporarySlot put tracking back. The launcher
    # does not touch the temp slot at all any more, so the assertion is re-aimed
    # at what exit A actually promises now: the CONTEXT is identical, number and
    # path both. And the follow-up slot write proves the slot machinery still
    # works afterwards.
    slot_after, path_after = active_context(1.5)
    check(slot_after == slot_before_launch and path_after == path_before_launch,
          f"EXIT A: cancelling the picker left the context untouched "
          f"({slot_before_launch}, {path_before_launch!r} -> {slot_after}, {path_after!r})")
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
            self._scanned = 0
            # Prime the connection: the firmware's connection-init eats the first
            # byte(s) - the same idiom phase 6(d) uses.
            #
            # RAW PORT-1 READER: it carries its own fault witness (see the note
            # on the run_app probe above). The banner is scanned here because a
            # post-fault [crashlog] is printed once, to whichever terminal
            # attaches first.
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

        def _scan(self):
            # Scan only what is new, so one fault is reported once.
            if len(self.buf) > self._scanned:
                fault_scan(self.buf[self._scanned:], "the guide driver")
                self._scanned = len(self.buf)

        def expect(self, pattern, what, timeout=25):
            deadline = time.time() + timeout
            rx = re.compile(pattern)
            while time.time() < deadline:
                chunk = self.ser.read(4096)
                if chunk:
                    self.buf += _csi.sub("", chunk.decode(errors="replace"))
                    self._scan()
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
                    self._scan()
            finally:
                self.ser.close()


    # (part name, footprint, base row, [(pin name, expected node, expected connect)])
    # connect -1 = "the leg just occupies the hole". Node numbers are the ones
    # JumperlessDefines.h assigns: GND 100, TOP_RAIL 101, RP_GPIO_1..3 131/132/133,
    # RP_GPIO_7 137 (RP pin 26, SDA), RP_GPIO_8 138 (RP pin 27, SCL).
    #
    # ORDER MATTERS, for one measured reason: i2cscrn's sub-phase (v-b) below
    # compiles and RUNS a ~12 KB companion script, which leaves the
    # MicroPython heap fragmented enough that the NEXT project's step (v) -
    # which needs its whole source as one contiguous string - can take its
    # pre-existing "too fragmented" skip and quietly cost a check. Observed:
    # with i2cscrn first, eeprom's compile skipped. i2cscrn goes LAST so the
    # two cheap compiles happen on the cleaner heap and the heavy phase has
    # nothing after it. (The phases are otherwise independent; each loads its
    # own wiring and each tears down what it makes.)
    TASK9_PROJECTS = (
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
        ("i2cscrn", 5, (
            ("DISP", "sip4", 5, (("GND", 5, 100), ("VCC", 6, 101),
                                 ("SCL", 7, 138), ("SDA", 8, 137))),
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

    # INFRA_ROWS (the OLED's breadboard-row infra bridges, if the bench has
    # any) is computed once before phase 3 and reused here - the row-bridge
    # count below and the 555's in phase 3 exclude exactly the same rows.

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

        # (iii) the wiring parses, and every leg lands where the footprint math
        # says. LITERAL-path form on purpose: this phase is about the wiring
        # FILE's contents, and the name form would begin a run (allocating
        # <proj>_1.yaml and re-serializing it) before anything got read. The
        # literal form is the raw adopting loader, so what is asserted below is
        # the shipped template's own parse. Adopting a template is safe -
        # SlotManager refuses to WRITE one (task 4) - and (iv) moves the context
        # off it immediately.
        out = jl_exec(f"""
print("loaded=", 1 if load_project({pdir + "/wiring.yaml"!r}) else 0)
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
        check(vals.get("loaded") == 1, f"load_project() loaded {pdir}/wiring.yaml")
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
        #
        # i2cscrn is EXEMPT BY NAME, deterministically: its script is ~12 KB
        # since wave 3 and this route needs the whole source as one contiguous
        # MicroPython string, which does not fit mid-suite. It is not skipped
        # coverage - (v-b) below compiles AND RUNS it through the launcher's
        # own runner, which lexes from a C pointer and is the path users take.
        if proj == "i2cscrn":
            print(f"  info: {pdir}/main.py is compiled and RUN by (v-b) below "
                  f"through the launcher's runner - the whole-source REPL "
                  f"allocation this step makes does not fit a script that size")
            vals = {}
        else:
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
        if proj == "i2cscrn":
            pass
        elif vals.get("nomem") == 1:
            print(f"  info: {pdir}/main.py device-compile skipped - this session's "
                  f"MicroPython heap is too fragmented to hold the source. The "
                  f"launcher's run path does not make this allocation; running the "
                  f"script from the picker is a bench item.")
        else:
            check(vals.get("compiled") == 1,
                  f"{pdir}/main.py compiles under this MicroPython "
                  f"({vals.get('srclen')} bytes read through jfs, uncapped)")

        # (v-b) i2cscrn ONLY: drive the whole new flow through the REAL
        # runner and the REAL stdin (wave 3, Kevin's bench note: "have users
        # tap each signal and allow them to choose from a list of different
        # oled drivers and sizes. And when I exit the app, it clears the data
        # lines").
        #
        # Every probe gesture in that script has a typed equivalent at the same
        # prompt - the control-surface rule - and that is what makes this
        # testable at all. NO PANEL IS NEEDED: the beacon scans, finds nothing,
        # and a typed `q` takes the exit path, which is the half worth
        # asserting anyway.
        #
        # WHY A SCRATCH PROJECT AND NOT `exec(fs_read(...))` ON THE REPL:
        # the REPL route needs the whole source as one contiguous MicroPython
        # string and then compiles it out of the same heap - it MemoryErrors on
        # a script this size (measured: 25 KB free, largest block ~8-10 KB).
        # The launcher's runner lexes from a C pointer, which is both the path
        # users actually take and the one that fits. /projects/hili2c is this
        # suite's own fixture: a wiring with NO guide: section, so `z hili2c`
        # runs the script immediately, plus a byte-exact on-device copy of the
        # shipped i2cscrn main.py (hash-compared, so it cannot drift).
        #
        # The feed remaps SCL/SDA to rows 41/42 - proving the script re-routes
        # rather than assuming rows 5-8 - and picks driver 2. Row 5 -> GND is
        # ARMED BY HAND first, so the run also proves the don't-touch-what-
        # isn't-mine rule: reported, not counted, and still there at the end.
        if proj == "i2cscrn":
            I2C_DIR = "/projects/hili2c"
            # The 5 -> GND bridge is IN THE WIRING, not connected by hand:
            # `z hili2c` loads the run file and that load clears the board, so
            # anything armed beforehand is gone before the script starts. This
            # way the route is live for exactly the right reason - it is part
            # of the circuit the user already had - and the script has to
            # notice it, not re-make it, and not take it away.
            I2C_WIRING = ("version: 2\nsourceOfTruth: bridges\nmeta:\n"
                          "  project: hili2c\n  title: \"HIL i2cscrn drive\"\n"
                          "  variant: default\n"
                          "  summary: \"no guide - runs the script directly\"\n"
                          "  script: main.py\n"
                          "bridges:\n  - {n1: 5, n2: GND}\n")
            out = jl_exec(f"""
try:
    jfs.mkdir({I2C_DIR!r})
except Exception as e:
    pass
print("dir=", 1 if fs_exists({I2C_DIR!r}) else 0)
print("w=", 1 if fs_write({I2C_DIR + "/wiring.yaml"!r}, {I2C_WIRING!r}) else 0)
# Chunked copy: never a contiguous allocation the size of the script.
_s = jfs.open({pdir + "/main.py"!r}, "r")
_d = jfs.open({I2C_DIR + "/main.py"!r}, "w")
_n = 0
while True:
    _c = _s.read(512)
    if not _c:
        break
    jfs.write(_d, _c)
    _n += len(_c)
_s.close()
_d.close()
print("copied=", _n)
""", timeout=60)
            vals = parse_kv(out)
            check(vals.get("dir") == 1 and vals.get("w") == 1,
                  f"i2cscrn: built the {I2C_DIR} drive fixture (no guide: section)")

            # THE ONE REBOOT IN THIS SUITE, and it is here for a measured
            # reason: MicroPython compiles the companion script on the device,
            # and a ~11 KB source needs more heap than a long HIL session has
            # left. Measured on this board - 39 KB free after a reset compiles
            # it; ~25 KB mid-suite raises MemoryError inside execfile. A reset
            # makes this phase deterministic instead of a coin flip on how
            # fragmented the heap happens to be, and it costs ~10 s.
            #
            # SAFE HERE, and only here: phase 6(c)'s "the LAUNCHER is the only
            # provisioning trigger" argument is already spent (it ran and
            # passed above), the phases after this one are self-contained
            # jl_exec/GuideDriver sequences, and phase 7 restores the context,
            # slot 3 and the board state from host-side snapshots regardless.
            check(reboot_board(),
                  "i2cscrn: rebooted for a fresh MicroPython heap before the "
                  "script drive (the compile needs it; see the note above)")
            time.sleep(1.0)
            src_h, _sl = device_hash(f"{pdir}/main.py")
            cp_h, cp_l = device_hash(f"{I2C_DIR}/main.py")
            check(cp_h == src_h,
                  f"i2cscrn: the drive fixture is a BYTE-EXACT copy of the "
                  f"shipped main.py ({cp_h}, {cp_l} bytes) - the drive below "
                  f"cannot test a script that has drifted from the real one")

            d = GuideDriver()
            try:
                # \r only: a trailing \n survives the command reader and
                # would land on the script's first prompt as a bare Enter.
                # (main.py drains its input before prompting for exactly this
                # reason, but not sending it is the belt to that's braces.)
                d.send(b"z hili2c\r")
                # If the launcher cannot build the script buffer it now SAYS so
                # (ProjectsApp.cpp runCompanionScript) instead of printing
                # nothing - the wave-3 fix. Either way this expect names it.
                d.expect(r"SCRIPT action=run",
                         "i2cscrn: the launcher ran the companion script")
                d.expect(r"Type to Screen",
                         "i2cscrn: the script actually started (a companion "
                         "script that is too big for the heap prints NOTHING - "
                         "see runCompanionScript)")
                m = d.expect(r"Where is the panel\?",
                             "i2cscrn: tap-to-assign opened")
                # ONE ANSWER AT A TIME, each waited for before the next.
                # Not just tidiness: a whole burst pasted into a running
                # companion script comes back mangled on this board
                # (characters dropped and duplicated - see the report's
                # concerns), while answer-then-wait is reliable. It is also
                # what a human does, and it turns every step of the assignment
                # into its own assertion.
                #
                # Guarded on the prompt actually appearing: bytes sent when no
                # script is reading land on the single-char command handler
                # instead (a stray `q` starts the DMX app).
                for _sig, _row in (("GND", 5), ("VCC", 6),
                                   ("SCL", 41), ("SDA", 42)):
                    if m:
                        d.send(("%d\r" % _row).encode())
                        m = d.expect(r"%s = row %d \(typed\)" % (_sig, _row),
                                     f"i2cscrn: {_sig} took row {_row} from the "
                                     f"TYPED TWIN")
                d.expect(r"assignment: GND 5\s+VCC 6\s+SCL 41\s+SDA 42",
                         "i2cscrn: the assignment reads back what was typed - "
                         "two of the four remapped off the default header")
                m = d.expect(r"Panel type:", "i2cscrn: the driver menu opened")
                if m:
                    d.send(b"2\r")
                d.expect(r"driver: SSD1306 128x64",
                         "i2cscrn: the driver menu took '2' -> SSD1306 128x64")
                d.expect(r"GND row 5 -> GND was already routed - left alone",
                         "i2cscrn: the pre-existing route was reported, not re-made")
                d.expect(r"routed=3",
                         "i2cscrn: exactly 3 routes made (VCC/SCL/SDA)")
                m = d.expect(r"waiting for the panel",
                             "i2cscrn: the wiring beacon ran (it scans before "
                             "it will accept a quit, so this is a real scan)")
                if m:
                    d.send(b"q\r\n")
                d.expect(r"unrouted=3",
                         "i2cscrn: the exit removed exactly the 3 it made")
                d.expect(r"bye", "i2cscrn: the script's finally: ran")
                d.expect(r"--- script finished ---",
                         "i2cscrn: the launcher regained control")
            finally:
                d.close()
                time.sleep(1.0)

            # EXIT CLEARS THE DATA LINES - and the power route with them.
            out = jl_exec("""
print("scl=", 1 if is_connected(41, "RP_GPIO_8") else 0)
print("sda=", 1 if is_connected(42, "RP_GPIO_7") else 0)
print("vcc=", 1 if is_connected(6, "TOP_RAIL") else 0)
print("gnd=", 1 if is_connected(5, "GND") else 0)
""", timeout=30)
            vals = parse_kv(out)
            check(vals.get("scl") == 0 and vals.get("sda") == 0,
                  f"i2cscrn: exit cleared the DATA lines - 41->RP_GPIO_8 and "
                  f"42->RP_GPIO_7 are gone (got {vals.get('scl')}, {vals.get('sda')})")
            check(vals.get("vcc") == 0,
                  "i2cscrn: exit cleared the POWER route it made too (6 -> TOP_RAIL)")
            check(vals.get("gnd") == 1,
                  "i2cscrn: the route it did NOT make survived - the wiring's "
                  "own 5 -> GND is still there (it only cleans up after itself)")

            out = jl_exec(f"""
if fs_exists({I2C_DIR!r}):
    for nm in jfs.listdir({I2C_DIR!r}):
        try:
            jfs.remove({I2C_DIR!r} + "/" + nm)
        except Exception as e:
            print("rmerr=", e)
    try:
        jfs.rmdir({I2C_DIR!r})
    except Exception as e:
        print("rmdirerr=", e)
print("gone=", 0 if fs_exists({I2C_DIR!r}) else 1)
""", timeout=40)
            check(parse_kv(out).get("gone") == 1,
                  f"i2cscrn: removed the {I2C_DIR} drive fixture")
            leave_context_to_slot3()

        # (vi) the guide: section parses. Drive `z ... new` far enough to see
        # the first step, then quit - no part is confirmed, so nothing is
        # placed. `new` (not a bare launch) so the phase is deterministic
        # whatever the project's run file already held.
        #
        # `new` on a SHIPPED project OVERWRITES <proj>_run.yaml in single-file
        # mode - that is the whole point of "start fresh" - so this phase is
        # one of the two that phase 0's run_file_capture() exists for. The
        # teardown puts the user's bytes back.
        guide_live = False
        d = GuideDriver()
        try:
            d.send(f"z {pdir}/wiring.yaml new\r\n".encode())
            guide_live = True
            m = d.expect(r"RUNFILE path=(\S+) action=new",
                         f"{proj}: the launch wrote a run file")
            if RUN_MODE == "single":
                check(m is not None and m.group(1) == project_run_path(pdir),
                      f"{proj}: single-file mode wrote {proj}_run.yaml "
                      f"(got {m.group(1) if m else None!r})")
            m = d.expect(r"GUIDE step=1/(?P<n>\d+) id=",
                         f"{proj}: guide: section parsed and the runtime started")
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

    # --- 6(f). RUN FILES: the exit table, the allocator, the terminal state ---
    # Everything the temp-slot keep-flow used to do is gone; this is what
    # replaced it. Driven headless from PORT 1 on purpose: the launcher moves
    # the MicroPython stream to port 1 before it execs a companion script, so
    # port 1 is the only place a real script run is both drivable and readable.
    print("  --- 6(f) run files ---")

    leave_context_to_slot3()
    purged = purge_fixture_runs(HIL_DIR, "hiltest_")
    print(f"  info: purged {purged} pre-existing hiltest run file(s) for a "
          f"deterministic start (hiltest is THIS SUITE'S fixture directory - "
          f"the only one a blanket prefix sweep is allowed on)")

    # What the first launch of a run-less project must produce, per mode.
    RUN_FIRST = (project_run_path(HIL_DIR) if RUN_MODE == "single"
                 else f"{HIL_DIR}/hiltest_1.yaml")

    # (i) EXIT G: the non-guided happy path, end to end. hiltest ships no
    # guide:, so the launch runs its companion script UNCONDITIONALLY (that is
    # the whole point of a non-guided project - no offer), then saves the run
    # file and prints the one-liner.
    d = GuideDriver()
    try:
        d.send(b"z hiltest new\r\n")
        m = d.expect(r"RUNFILE path=(\S+) action=new", "z ... new wrote a run file")
        run1 = m.group(1) if m else None
        check(run1 == RUN_FIRST,
              f"first run of a project with no runs is "
              f"{os.path.basename(RUN_FIRST)} (got {run1!r})")
        d.expect(r"SCRIPT offer=" + re.escape(f"{HIL_DIR}/main.py"),
                 "the resolved companion script is announced")
        d.expect(r"SCRIPT action=run",
                 "a NON-guided launch runs its script unconditionally (no offer)")
        d.expect(r"hilmark= 1", "the project's main.py really ran")
        d.expect(r"hilrun= " + re.escape(run1 or "x"),
                 '_jl_project["run"] names the run file the script lives in')
        d.expect(r"--- script finished ---", "the script returned")
        d.expect(r"Run saved to " + re.escape(os.path.basename(RUN_FIRST)) +
                 r" \(now your active circuit\)",
                 "EXIT G: the launcher saved the run and said so")
    finally:
        d.close()
    time.sleep(1.0)

    ctx_slot, ctx_path = active_context(1.5)
    check(ctx_slot == -1 and ctx_path == run1,
          f"EXIT G: the run file is the ACTIVE CONTEXT afterwards "
          f"(got {ctx_slot}, {ctx_path!r})")

    exists, run1_yaml = read_device_file(run1)
    check(exists, f"{run1} exists and reads back")
    check(f'runSource: "{HIL_DIR}/wiring.yaml"' in run1_yaml,
          "the run file carries runSource: - the variant provenance the path "
          "cannot encode")
    check(re.search(r"n1:\s*20,\s*n2:\s*21", run1_yaml) is not None,
          "the run file carries the wiring's 20-21 bridge")
    check("meta:" not in run1_yaml,
          "the run file lost meta: on its first save (wholesale toYAML rewrite)")

    # (ii) CORRECTION-5 NEEDLE: a run file is NOT a template. SlotManager
    # refuses to write /projects/<dir>/wiring*.yaml, and <dir>_<N>.yaml
    # deliberately does not match that predicate - VERIFIED here rather than
    # assumed, because the whole run-file model rests on it. Dirty the state
    # and prove the idle auto-save lands IN THE RUN FILE while the shipped
    # template beside it is untouched.
    tpl_hash_before, _ = device_hash(f"{HIL_DIR}/wiring.yaml")
    jl_exec("connect(41, 42)", timeout=25)
    time.sleep(4.5)   # well past the idle flush gate
    _, run1_after = read_device_file(run1)
    check(re.search(r"n1:\s*41,\s*n2:\s*42", run1_after) is not None,
          "a run file IS writable: the auto-save landed the 41-42 edit in it "
          "(the template write-guard does not match <dir>_<N>.yaml)")
    tpl_hash_after, _ = device_hash(f"{HIL_DIR}/wiring.yaml")
    check(tpl_hash_after == tpl_hash_before,
          f"the shipped template beside it is byte-identical ({tpl_hash_after})")

    # (iii) Relaunch offers the LATEST run, and `noscript` skips the script.
    d = GuideDriver()
    try:
        d.send(b"z hiltest noscript\r\n")
        d.expect(r"RUNS n=1 latest=" + re.escape(run1),
                 "a relaunch reports the run count and the latest path")
        m = d.expect(r"RUNFILE path=(\S+) action=load",
                     "no mode arg = load latest when runs exist")
        check(m is not None and m.group(1) == run1,
              f"the relaunch opened {run1}")
        d.expect(r"SCRIPT action=skip", "`noscript` skipped the companion script")
        d.expect(r"Run saved to " + re.escape(os.path.basename(RUN_FIRST)),
                 "the run was saved anyway")
    finally:
        d.close()
    time.sleep(1.0)

    if RUN_MODE == "single":
        # (iv-single) ONE FILE, REUSED. `new` does not allocate anything - it
        # REWRITES the same <dir>_run.yaml from the wiring, which is exactly
        # what "start fresh" has to mean when there is only one name. The
        # 41-42 bridge (ii) left in the file is the witness: after `new` it is
        # gone, and the directory still holds exactly ONE run file.
        d = GuideDriver()
        try:
            d.send(b"z hiltest new noscript\r\n")
            m = d.expect(r"RUNFILE path=(\S+) action=new",
                         "a second `new` wrote a run file")
            run2 = m.group(1) if m else None
            check(run2 == run1,
                  f"SINGLE FILE: `new` reuses the SAME name, it does not "
                  f"allocate (got {run2!r}, expected {run1!r})")
        finally:
            d.close()
        time.sleep(1.0)

        _, after_new = read_device_file(run1)
        check(re.search(r"n1:\s*41,\s*n2:\s*42", after_new) is None,
              "OVERWRITE: `new` re-copied the template, so the 41-42 edit that "
              "was in the run file is gone")
        check(re.search(r"n1:\s*20,\s*n2:\s*21", after_new) is not None,
              "OVERWRITE: ...and the wiring's own 20-21 bridge is back")

        out = jl_exec(f"""
names = [n for n in jfs.listdir({HIL_DIR!r})
         if n.startswith("hiltest_") and n.endswith(".yaml")]
print("nrun=", len(names))
print("RUNS|" + ",".join(sorted(names)))
""", timeout=25)
        check(parse_kv(out).get("nrun") == 1,
              f"NO PILE-UP: three launches left exactly ONE run file in "
              f"{HIL_DIR} ({out.strip().splitlines()[-1] if out.strip() else out!r})")

        # `run=<N>` is the numbered scheme's grammar. A single-file build must
        # refuse it BY NAME rather than quietly opening <dir>_run.yaml - a
        # scripted driver asking for run 2 has a stale assumption and needs to
        # be told, not humoured.
        ctx_before_runN = active_context(1.5)
        d = GuideDriver()
        try:
            d.send(b"z hiltest run=2 noscript\r\n")
            d.expect(r"PROJECT error run=<N> needs a JL_PROJECT_RUN_HISTORY build",
                     "run=<N> is refused by NAME on a single-file build")
        finally:
            d.close()
        check(active_context(1.5) == ctx_before_runN,
              "the refused run=<N> changed nothing")

        run3 = run1   # the terminal-state needle below targets the one file
        print("  info: SKIPPED (JL_PROJECT_RUN_HISTORY only) - the monotonic "
              "allocator, the no-reuse-after-delete rule and positive run=<N>. "
              "There is one run file per project on this build, so there is no "
              "counter to advance; the numbered code still compiles behind the "
              "flag and those phases run on that build.")
    else:
        # (iv) ALLOCATOR: monotonic, and gaps are NEVER reused. Two launches give
        # _1 then _2; deleting _1 and launching again gives _3, not _1 - reusing a
        # gap would resurrect a stale transcript's idea of which file is which.
        d = GuideDriver()
        try:
            d.send(b"z hiltest new noscript\r\n")
            m = d.expect(r"RUNFILE path=(\S+) action=new", "second launch allocated a run file")
            run2 = m.group(1) if m else None
            check(run2 == f"{HIL_DIR}/hiltest_2.yaml",
                  f"the allocator went _1 -> _2 (got {run2!r})")
        finally:
            d.close()
        time.sleep(1.0)

        leave_context_to_slot3()
        out = jl_exec(f"""
if fs_exists({run1!r}):
    jfs.remove({run1!r})
print("gone=", 0 if fs_exists({run1!r}) else 1)
""", timeout=25)
        check(parse_kv(out).get("gone") == 1, f"deleted {run1} to open a gap at _1")

        d = GuideDriver()
        try:
            d.send(b"z hiltest new noscript\r\n")
            m = d.expect(r"RUNFILE path=(\S+) action=new", "third launch allocated a run file")
            run3 = m.group(1) if m else None
            check(run3 == f"{HIL_DIR}/hiltest_3.yaml",
                  f"NO REUSE: the gap left by _1 was skipped, next is _3 (got {run3!r})")
        finally:
            d.close()
        time.sleep(1.0)

        # run=<N> opens ONE specific run file - the grammar's determinism knob, so
        # a scripted driver can name the file it means instead of trusting whatever
        # "latest" happens to be. _2 is deliberately NOT the latest here (_3 is).
        d = GuideDriver()
        try:
            d.send(b"z hiltest run=2 noscript\r\n")
            m = d.expect(r"RUNFILE path=(\S+) action=load",
                         "run=<N> opened a run file")
            check(m is not None and m.group(1) == f"{HIL_DIR}/hiltest_2.yaml",
                  f"run=2 opened _2, not the latest _3 (got "
                  f"{m.group(1) if m else None!r})")
        finally:
            d.close()
        time.sleep(1.0)

        # ...and re-open _3 so the terminal-state needle below targets the file it
        # names (the needle needs the bad YAML written over the ACTIVE context).
        d = GuideDriver()
        try:
            d.send(b"z hiltest run=3 noscript\r\n")
            d.expect(r"RUNFILE path=" + re.escape(run3 or "x") + r" action=load",
                     "back on _3 for the terminal-state needle")
        finally:
            d.close()
        time.sleep(1.0)

    # (v) The `z` grammar. The old `z <path> <slot>` must break VISIBLY rather
    # than silently writing a slot, and an all-digit PROJECT NAME must keep
    # working - `z 555` is a project, not a destination slot.
    ctx_before_bad = active_context(1.5)
    d = GuideDriver()
    try:
        d.send(b"z hiltest 3\r\n")
        d.expect(r"destination slots are gone",
                 "the old `z <project> <slot>` grammar loud-fails with the usage line")
    finally:
        d.close()
    check(active_context(1.5) == ctx_before_bad,
          "the refused command changed nothing")

    d = GuideDriver()
    try:
        # An all-digit token BEFORE any mode word is the PROJECT, not the old
        # grammar's destination slot - a token-wise parse. Both modes prove it
        # from an error line that NAMES the resolved directory, and neither
        # starts 555's guide or writes anything: in single-file mode `run=<N>`
        # is refused by name (and the message carries `555_run.yaml`), in
        # numbered mode run=9999 cannot exist. Deliberately an error path -
        # `z 555` for real would overwrite the user's 555 run file.
        d.send(b"z 555 run=9999\r\n")
        if RUN_MODE == "single":
            d.expect(r"PROJECT error run=<N> needs a JL_PROJECT_RUN_HISTORY "
                     r"build - this build keeps ONE run file per project "
                     r"\(555_run\.yaml\)",
                     "an all-digit project name is a PROJECT, not a slot "
                     "(the refusal names 555_run.yaml)")
        else:
            d.expect(r"PROJECT error no such run file: /projects/555/555_9999\.yaml",
                     "an all-digit project name is a PROJECT, not a slot")
    finally:
        d.close()
    check(active_context(1.5) == ctx_before_bad,
          "the failed run=<N> changed nothing either")

    # (vi) EXIT E, the NO-ACTIVE-CONTEXT terminal state. For a FILE context
    # this is deterministic, not exotic (task 4 report, NEW-3): an invalid YAML
    # written over the ACTIVE run file makes loadSlotFromPath's parse-failure
    # RESTORE re-read the same bad file, and arbitrary paths have no /.bak
    # mirror. The manager then stops at activeSlotNumber == -1 with an EMPTY
    # path, a cleared state, and - the half that had zero coverage until now -
    # the HARDWARE cleared to match: nothing routed, nothing powered.
    #
    # The write and the load must be ONE snippet: split across two jl_exec
    # calls, the multi-second REPL gap lets the idle auto-save rewrite the run
    # file with valid content and the needle goes vacuous.
    #
    # topRail 99 V is what fails: fromYAML ends in validate() and
    # PowerState::validate rejects any rail/DAC outside +/-8 V.
    BAD_RUN = ("version: 2\nsourceOfTruth: bridges\n"
               "bridges:\n  - {n1: 51, n2: 52}\n"
               "power:\n  topRail: 99.00\n  bottomRail: 0.00\n"
               "  dac0: 0.00\n  dac1: 0.00\n")
    ctx_slot, ctx_path = active_context(1.5)
    check(ctx_slot == -1 and ctx_path == run3,
          f"the bad-YAML target IS the active context (got {ctx_slot}, {ctx_path!r})")
    out = jl_exec(f"""
print("wrote=", 1 if fs_write({run3!r}, {BAD_RUN!r}) else 0)
print("loaded=", 1 if load_project({run3!r}) else 0)
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("wrote") == 1, "wrote an invalid YAML over the ACTIVE run file")
    check(vals.get("loaded") == 0, "re-loading it fails (PowerState::validate rejects 99 V)")

    q = port1_command("Q", 2.0)
    check("ACTIVE_SLOT:-1" in q,
          f"TERMINAL STATE: ACTIVE_SLOT is -1 (Q said {q.strip()[-80:]!r})")
    # The discriminator is the EMPTY path: a normal FILE context also prints
    # ACTIVE_SLOT:-1, so only "ACTIVE_PATH:" with nothing after it separates the
    # terminal state from an ordinary run-file context.
    check(re.search(r"ACTIVE_PATH:[ \t]*\r?\n", q) is not None,
          "TERMINAL STATE: ACTIVE_PATH is EMPTY - not merely a file context")

    out = jl_exec(f"""
print("bad=", 1 if is_connected(51, 52) else 0)
n = get_num_bridges()
print("nbridges=", n)
rowb = 0
infra_rows = {INFRA_ROWS!r}
for i in range(n):
    b = get_bridge(i)
    if (1 <= b[0] <= 60) or (1 <= b[1] <= 60):
        if b[0] in infra_rows or b[1] in infra_rows:
            continue
        print("ROWBRIDGE|%s" % str(b))
        rowb += 1
print("rowbridges=", rowb)
print("top=", dac_get(2))
print("bot=", dac_get(3))
""", timeout=30)
    vals = parse_kv(out)
    check(vals.get("bad") == 0,
          "TERMINAL STATE: the failed file's own bridge was NOT applied")
    check(vals.get("rowbridges") == 0,
          f"TERMINAL STATE: NOTHING IS ROUTED - 0 non-infra bridges touch rows "
          f"1-60 (total {vals.get('nbridges')}, excluded infra rows {INFRA_ROWS})")
    # NOTHING POWERED BEYOND DEFAULTS: clear() resets the rails to 0 V (DAC0
    # keeps the probe feed voltage by design, which is why only the two rails
    # are asserted here).
    check(vals.get("top") is not None and abs(float(vals.get("top"))) < 0.5 and
          vals.get("bot") is not None and abs(float(vals.get("bot"))) < 0.5,
          f"TERMINAL STATE: NOTHING IS POWERED - both rails at 0 V "
          f"(top={vals.get('top')}, bottom={vals.get('bot')})")

    # Recover explicitly - the terminal state is a safe STOP, and leaving the
    # bench in it would strand every phase after this one.
    #
    # '<3' on port 1, NOT switch_slot(3): the MicroPython binding raises
    # ValueError("Invalid slot number") whenever jl_switch_slot returns -1, and
    # jl_switch_slot returns the PREVIOUS slot number on success - which is -1
    # exactly here. The switch works; the binding just cannot tell "came from
    # nowhere" from "bad argument". Recorded in the task-5 report as a
    # pre-existing return-value ambiguity that the terminal state made
    # reachable; it is task 4's surface, not the launcher's.
    port1_command("<3", 4.0)
    time.sleep(1.5)
    rec_slot, rec_path = active_context(1.5)
    check(rec_slot == 3,
          f"recovered from the terminal state with a plain slot switch "
          f"(got {rec_slot}, {rec_path!r})")

finally:
    # --- 7. Restore the bench --------------------------------------------------
    # RUN FILES FIRST, and only after leaving whichever one is active: a run
    # file that is still the context is re-created by the next switch's dirty
    # pre-save.
    #
    # TWO DIFFERENT JOBS, and conflating them is the hazard this file's header
    # names:
    #   * a SHIPPED project's <dir>_run.yaml is the USER'S circuit. It is put
    #     back byte-exact from phase 0's snapshot (or removed again if it did
    #     not exist). Numbered leftovers this suite minted go through
    #     purge_numbered_runs(), which matches digits and therefore cannot
    #     touch <dir>_run.yaml.
    #   * /projects/hiltest is this suite's own fixture directory and is
    #     deleted outright below.
    try:
        leave_context_to_slot3()
        total = 0
        for _pdir in REAL_PROJECT_DIRS:
            n = purge_numbered_runs(_pdir, _pdir.rsplit("/", 1)[-1])
            total += int(n) if n is not None else 0
        print(f"  info: removed {total} NUMBERED run file(s) from shipped "
              f"projects (digits-only sweep - <dir>_run.yaml is never in it)")
        restored = 0
        for _snap in real_run_snaps:
            ok = run_file_restore(_snap)
            check(ok, f"restored {_snap['path']} to its pre-test state "
                      f"({'byte-exact, ' + str(_snap['bytes']) + ' bytes' if _snap['existed'] else 'absent'})")
            restored += 1 if ok else 0
        print(f"  info: {restored}/{len(real_run_snaps)} shipped-project run "
              f"files restored to their pre-test state")
    except SystemExit:
        raise
    except Exception as e:  # pragma: no cover
        print(f"  info: run-file cleanup failed: {e!r}")

    # /projects/hiltest is THIS SUITE'S FIXTURE, not a shipped project - it is
    # written from the constants above at the top of phase 6 and provisioning
    # never re-creates it (that is one of the things phase 6 asserts). Leaving
    # it behind put a test artifact in the user's project picker and in every
    # later suite's view of the board. Phase 6 needs two projects; the BENCH
    # does not, and the four shipped ones give the clickwheel plenty to walk.
    try:
        out = jl_exec(f"""
if fs_exists({HIL_DIR!r}):
    for nm in jfs.listdir({HIL_DIR!r}):
        try:
            jfs.remove({HIL_DIR!r} + "/" + nm)
        except Exception as e:
            print("rmerr=", e)
    try:
        jfs.rmdir({HIL_DIR!r})
    except Exception as e:
        print("rmdirerr=", e)
print("hiltestgone=", 0 if fs_exists({HIL_DIR!r}) else 1)
""", timeout=30)
        check(parse_kv(out).get("hiltestgone") == 1,
              f"removed {HIL_DIR} - the suite's own fixture project is off the bench")
    except SystemExit:
        raise
    except Exception as e:  # pragma: no cover
        print(f"  info: hiltest cleanup failed: {e!r}")

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

print("  info: /projects/555/ left on the board (a shipped project; provisioning "
      "would restore it anyway). The suite's own /projects/hiltest fixture is gone.")
finish("test_projects")
